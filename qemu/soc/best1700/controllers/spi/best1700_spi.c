/* BEST1700 digital SPI/SSP controller, ordinary polled master mode.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_spi.h"

#define SPI_ENABLE (1u << 1)
#define SPI_TX_NOT_FULL (1u << 1)
#define SPI_RX_NOT_EMPTY (1u << 2)
#define SPI_BUSY (1u << 4)
#define SPI_CR0_MASK ((0x1fu << 16) | (0xffu << 8) | (3u << 6))
#define SPI_CR1_MASK (SPI_ENABLE | (0xfu << 7) | (1u << 11) | (3u << 12))

static unsigned best1700_spi_bits(const Best1700Spi *s)
{
    /* BEST1700 extends DSS to CR0[20:16], unlike standard PL022. */
    return ((s->cr0 >> 16) & 0x1f) + 1;
}

static uint32_t best1700_spi_frame_mask(unsigned bits)
{
    return UINT32_MAX >> (32 - bits);
}

static MemTxResult best1700_spi_unsupported(Best1700Spi *s, hwaddr offset,
                                            bool write, uint64_t value)
{
    if (s->diagnostics++ < 32) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 %s SPI unsupported %s offset=0x%" HWADDR_PRIx
            " value=0x%" PRIx64 "\n",
            s->name, write ? "write" : "read", offset, value);
    }
    return MEMTX_ERROR;
}

static void best1700_spi_schedule(Best1700Spi *s)
{
    uint64_t clocks, ns;

    if (!(s->cr1 & SPI_ENABLE) || !s->input_hz) {
        timer_del(s->timer);
        return;
    }
    if (!s->active) {
        if (!s->tx_count || !s->prescale || best1700_spi_bits(s) < 4) {
            return;
        }
        /* The current bus has no endpoint. Dequeue a real transmitted word
         * but never echo it or infer a sensor identity from its contents. */
        s->tx_head = (s->tx_head + 1) % BEST1700_SPI_MODEL_FIFO_DEPTH;
        s->tx_count--;
        s->active_bits = best1700_spi_bits(s);
        s->active_divider = s->prescale * (((s->cr0 >> 8) & 0xff) + 1);
        s->active = true;
    }
    if (timer_pending(s->timer)) {
        return;
    }
    clocks = (uint64_t)s->active_bits * s->active_divider;
    ns = (clocks * NANOSECONDS_PER_SECOND + s->input_hz - 1) / s->input_hz;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MAX(ns, 1));
}

static void best1700_spi_complete(void *opaque)
{
    Best1700Spi *s = opaque;

    if (!s->active || !(s->cr1 & SPI_ENABLE) || !s->input_hz) {
        return;
    }
    if (s->rx_count < BEST1700_SPI_MODEL_FIFO_DEPTH) {
        unsigned tail = (s->rx_head + s->rx_count) %
                        BEST1700_SPI_MODEL_FIFO_DEPTH;

        s->rx[tail] = best1700_spi_frame_mask(s->active_bits);
        s->rx_count++;
    } else if (s->diagnostics++ < 32) {
        /* No unverified overrun interrupt or status bit is fabricated. */
        qemu_log_mask(LOG_GUEST_ERROR,
            "best1700 %s SPI receive model FIFO overflow\n", s->name);
    }
    s->active = false;
    best1700_spi_schedule(s);
}

static MemTxResult best1700_spi_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    Best1700Spi *s = opaque;

    *value = 0;
    switch (offset) {
    case 0x00: *value = s->cr0; break;
    case 0x04: *value = s->cr1; break;
    case 0x08:
        if (s->rx_count) {
            *value = s->rx[s->rx_head];
            s->rx_head = (s->rx_head + 1) % BEST1700_SPI_MODEL_FIFO_DEPTH;
            s->rx_count--;
        }
        break;
    case 0x0c:
        /* Only status bits established for this controller are exposed. */
        *value = (s->tx_count < BEST1700_SPI_MODEL_FIFO_DEPTH ?
                  SPI_TX_NOT_FULL : 0) |
                 (s->rx_count ? SPI_RX_NOT_EMPTY : 0) |
                 (s->active && (s->cr1 & SPI_ENABLE) ? SPI_BUSY : 0);
        break;
    case 0x10: *value = s->prescale; break;
    case 0x14: /* Interrupts and DMA requests are not implemented. */
    case 0x18:
    case 0x1c:
    case 0x24:
    case 0x88: break; /* Ordinary full-duplex mode. */
    case 0x28: *value = s->sample_max; break;
    case 0x2c: *value = s->sample_delay; break;
    default: return best1700_spi_unsupported(s, offset, false, 0);
    }
    return MEMTX_OK;
}

static MemTxResult best1700_spi_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    Best1700Spi *s = opaque;

    switch (offset) {
    case 0x00:
        if (value & ~SPI_CR0_MASK) {
            return best1700_spi_unsupported(s, offset, true, value);
        }
        s->cr0 = value;
        break;
    case 0x04:
        /* Slave and unverified loopback modes must not silently succeed. */
        if (value & ~SPI_CR1_MASK) {
            return best1700_spi_unsupported(s, offset, true, value);
        }
        s->cr1 = value;
        break;
    case 0x08: {
        unsigned tail;

        if (s->tx_count == BEST1700_SPI_MODEL_FIFO_DEPTH ||
            best1700_spi_bits(s) < 4) {
            return best1700_spi_unsupported(s, offset, true, value);
        }
        tail = (s->tx_head + s->tx_count) % BEST1700_SPI_MODEL_FIFO_DEPTH;
        s->tx[tail] = value & best1700_spi_frame_mask(best1700_spi_bits(s));
        s->tx_count++;
        break;
    }
    case 0x0c:
    case 0x18:
    case 0x1c: return MEMTX_OK; /* Read-only status. */
    case 0x10:
        if (value > 0xfe || (value & 1)) {
            return best1700_spi_unsupported(s, offset, true, value);
        }
        s->prescale = value;
        break;
    case 0x14:
    case 0x24:
    case 0x88:
        /* In particular +0x88=0 still receives every transmitted frame.
         * Extended asymmetric frame modes need a separate implementation. */
        if (value) {
            return best1700_spi_unsupported(s, offset, true, value);
        }
        break;
    case 0x20: return MEMTX_OK; /* No implemented interrupt source to clear. */
    case 0x28:
        if (value > 0xff) {
            return best1700_spi_unsupported(s, offset, true, value);
        }
        s->sample_max = value;
        break;
    case 0x2c:
        if (value & ~((1u << 18) | 0xffu)) {
            return best1700_spi_unsupported(s, offset, true, value);
        }
        /* Sample phase does not change a constantly high disconnected pin. */
        s->sample_delay = value;
        break;
    default: return best1700_spi_unsupported(s, offset, true, value);
    }
    best1700_spi_schedule(s);
    return MEMTX_OK;
}

static const MemoryRegionOps best1700_spi_ops = {
    .read_with_attrs = best1700_spi_read,
    .write_with_attrs = best1700_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_spi_init(Best1700Spi *s, Object *owner, const char *name,
                       uint32_t input_hz)
{
    s->input_hz = input_hz;
    s->name = g_strdup(name);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, best1700_spi_complete, s);
    memory_region_init_io(&s->iomem, owner, &best1700_spi_ops, s, name,
                          BEST1700_SPI_WINDOW);
    best1700_spi_reset(s);
}

void best1700_spi_set_clock_hz(Best1700Spi *s, uint32_t input_hz)
{
    if (input_hz != s->input_hz) {
        /* Mid-frame changes restart one frame's timing; no bit-level clock
         * phase is modeled, and no extra RX word is created. */
        timer_del(s->timer);
        s->input_hz = input_hz;
        best1700_spi_schedule(s);
    }
}

void best1700_spi_reset(Best1700Spi *s)
{
    timer_del(s->timer);
    s->cr0 = s->cr1 = s->prescale = 0;
    s->sample_max = s->sample_delay = 0;
    s->tx_head = s->tx_count = s->rx_head = s->rx_count = 0;
    s->diagnostics = 0;
    s->active = false;
    s->active_bits = 0;
    s->active_divider = 0;
    memset(s->tx, 0, sizeof(s->tx));
    memset(s->rx, 0, sizeof(s->rx));
}

void best1700_spi_destroy(Best1700Spi *s)
{
    timer_free(s->timer);
    s->timer = NULL;
    g_free(s->name);
    s->name = NULL;
}
