/* BEST1700 DMA controller register and bus-transfer model.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "best1700_dma.h"

#define DMA_CHANNEL_BASE 0x100u
#define DMA_CHANNEL_STRIDE 0x20u
#define DMA_ENABLE 1u
#define DMA_CROSS_1KB (1u << 3)
#define DMA_CHANNEL_CLOCKS (0xffu << 12)
#define DMA_MANUAL_CLOCK (1u << 31)
#define DMA_GLOBAL_FIELDS (DMA_ENABLE | DMA_CROSS_1KB | \
                           DMA_CHANNEL_CLOCKS | DMA_MANUAL_CLOCK)
#define DMA_ERROR_MASK (1u << 14)
#define DMA_TC_MASK (1u << 15)
#define DMA_ACTIVE (1u << 17)
#define DMA_HALT (1u << 18)
#define DMA_TC_REQUEST (1u << 31)
#define DMA_SRC_INC (1u << 26)
#define DMA_DST_INC (1u << 27)
#define DMA_COUNT_MASK 0xfffu
#define DMA_TICK_NS 1000

static uint32_t best1700_dma_masked_status(Best1700Dma *s, bool error)
{
    uint32_t status = 0;
    uint32_t raw = error ? s->raw_error : s->raw_tc;
    uint32_t mask = error ? DMA_ERROR_MASK : DMA_TC_MASK;

    for (unsigned i = 0; i < s->channel_count; i++) {
        if (s->channel[i].config & mask) {
            status |= raw & (1u << i);
        }
    }
    return status;
}

static void best1700_dma_update_irq(Best1700Dma *s)
{
    if (s->irq) {
        qemu_set_irq(s->irq, (best1700_dma_masked_status(s, false) |
                              best1700_dma_masked_status(s, true)) != 0);
    }
}

static bool best1700_dma_runnable(Best1700Dma *s, unsigned index)
{
    uint32_t config = s->channel[index].config;
    unsigned flow = (config >> 11) & 7;
    bool clock_ready = !(s->config & DMA_MANUAL_CLOCK) ||
                       (s->config & (1u << (12 + index)));

    /* Peripheral sources and peripheral-controlled termination require
     * request lines which are not connected yet. Leave them pending without
     * synthesizing input or repeatedly scheduling an empty receive FIFO.
     * Flow 1 supports an immediately ready synchronous sink; TX request-line
     * backpressure and burst timing are not implemented. */
    return (s->config & DMA_ENABLE) && clock_ready && (config & DMA_ENABLE) &&
           !(config & DMA_HALT) && (flow == 0 || flow == 1);
}

static void best1700_dma_schedule(Best1700Dma *s)
{
    for (unsigned i = 0; i < s->channel_count; i++) {
        if (best1700_dma_runnable(s, i)) {
            if (!timer_pending(s->timer)) {
                timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                     DMA_TICK_NS);
            }
            return;
        }
    }
    timer_del(s->timer);
}

static bool best1700_dma_transfer(Best1700Dma *s, unsigned index)
{
    Best1700DmaChannel *channel = &s->channel[index];
    uint32_t control = channel->control;
    unsigned count = control & DMA_COUNT_MASK;
    unsigned src_width = 1u << ((control >> 18) & 7);
    unsigned dst_width = 1u << ((control >> 21) & 7);

    /* Unequal widths need FIFO packing, not zero padding or byte loss. */
    if (src_width > 4 || dst_width > 4 || src_width != dst_width ||
        (channel->source & (src_width - 1)) ||
        (channel->destination & (dst_width - 1))) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "best1700 %s DMA ch%u unsupported widths/alignment %u/%u\n",
            s->name, index, src_width, dst_width);
        return false;
    }
    /* Each aligned beat is at most four bytes, so no individual bus request
     * crosses a 1 KB boundary. CROSS_1KB permits a burst to span that boundary;
     * this model already splits bursts into beats in either mode. */
    for (unsigned i = 0; i < count; i++) {
        uint8_t payload[4];

        if (address_space_read(s->address_space, channel->source,
                MEMTXATTRS_UNSPECIFIED, payload, src_width) != MEMTX_OK ||
            address_space_write(s->address_space, channel->destination,
                MEMTXATTRS_UNSPECIFIED, payload, dst_width) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                "best1700 %s DMA ch%u bus error src=0x%08x dst=0x%08x\n",
                s->name, index, channel->source, channel->destination);
            return false;
        }
        if (control & DMA_SRC_INC) {
            channel->source += src_width;
        }
        if (control & DMA_DST_INC) {
            channel->destination += dst_width;
        }
        channel->control = (control & ~DMA_COUNT_MASK) | (count - i - 1);
    }
    return true;
}

static bool best1700_dma_load_lli(Best1700Dma *s, unsigned index)
{
    Best1700DmaChannel *channel = &s->channel[index];
    uint8_t descriptor[16];
    /* Bit 0 selects the descriptor bus master. Both current master ports
     * share the instance's explicitly supplied address space. */
    uint32_t address = channel->lli & ~1u;

    if ((address & 3) || address > UINT32_MAX - (sizeof(descriptor) - 1) ||
        address_space_read(s->address_space, address, MEMTXATTRS_UNSPECIFIED,
                            descriptor, sizeof(descriptor)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "best1700 %s DMA ch%u invalid LLI=0x%08x\n",
            s->name, index, channel->lli);
        return false;
    }
    channel->source = ldl_le_p(descriptor);
    channel->destination = ldl_le_p(descriptor + 4);
    channel->lli = ldl_le_p(descriptor + 8);
    channel->control = ldl_le_p(descriptor + 12);
    return true;
}

static void best1700_dma_tick(void *opaque)
{
    Best1700Dma *s = opaque;

    for (unsigned i = 0; i < s->channel_count; i++) {
        Best1700DmaChannel *channel = &s->channel[i];
        uint32_t bit = 1u << i;
        bool success;

        if (!best1700_dma_runnable(s, i)) {
            continue;
        }
        success = best1700_dma_transfer(s, i);
        if (success) {
            if (channel->control & DMA_TC_REQUEST) {
                s->raw_tc |= bit;
                if (s->terminal_callback) {
                    /* The callback observes a completed terminal count,
                     * after the final beat has reached the destination. */
                    s->terminal_callback(s->terminal_opaque, i);
                }
            }
            if (channel->lli & ~1u) {
                success = best1700_dma_load_lli(s, i);
                if (success) {
                    /* One descriptor per tick bounds circular chains. */
                    continue;
                }
            } else {
                channel->lli = 0;
            }
        }
        if (!success) {
            s->raw_error |= bit;
        }
        channel->config &= ~DMA_ENABLE;
    }
    best1700_dma_update_irq(s);
    best1700_dma_schedule(s);
}

static MemTxResult best1700_dma_unsupported(Best1700Dma *s, hwaddr offset,
                                            bool write, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 %s DMA unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 "\n",
        s->name, write ? "write" : "read", offset, value);
    return MEMTX_ERROR;
}

static uint32_t *best1700_dma_channel_register(Best1700Dma *s, hwaddr offset)
{
    if (offset >= DMA_CHANNEL_BASE &&
        offset < DMA_CHANNEL_BASE +
                 BEST1700_DMA_MAX_CHANNELS * DMA_CHANNEL_STRIDE) {
        unsigned index = (offset - DMA_CHANNEL_BASE) / DMA_CHANNEL_STRIDE;
        Best1700DmaChannel *channel = &s->channel[index];

        switch (offset & (DMA_CHANNEL_STRIDE - 1)) {
        case 0x00: return &channel->source;
        case 0x04: return &channel->destination;
        case 0x08: return &channel->lli;
        case 0x0c: return &channel->control;
        case 0x10: return &channel->config;
        }
    }
    return NULL;
}

static MemTxResult best1700_dma_read(void *opaque, hwaddr offset,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    Best1700Dma *s = opaque;
    uint32_t *reg = best1700_dma_channel_register(s, offset);

    *value = 0;
    if (reg) {
        *value = *reg;
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x00:
        *value = best1700_dma_masked_status(s, false) |
                 best1700_dma_masked_status(s, true);
        break;
    case 0x04: *value = best1700_dma_masked_status(s, false); break;
    case 0x0c: *value = best1700_dma_masked_status(s, true); break;
    case 0x08:
    case 0x10: break;
    case 0x14: *value = s->raw_tc; break;
    case 0x18: *value = s->raw_error; break;
    case 0x1c:
        for (unsigned i = 0; i < s->channel_count; i++) {
            if (s->channel[i].config & DMA_ENABLE) {
                *value |= 1u << i;
            }
        }
        break;
    case 0x30: *value = s->config; break;
    default: return best1700_dma_unsupported(s, offset, false, 0);
    }
    return MEMTX_OK;
}

static MemTxResult best1700_dma_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    Best1700Dma *s = opaque;
    uint32_t *reg = best1700_dma_channel_register(s, offset);

    if (reg) {
        unsigned index = (offset - DMA_CHANNEL_BASE) / DMA_CHANNEL_STRIDE;

        /* Initialization probes every physical register bank, including banks
         * outside this instance's implemented transfer/capture channels.
         * Preserve their disabled configuration without inventing DMA work. */
        if (index >= s->channel_count &&
            (offset & (DMA_CHANNEL_STRIDE - 1)) == 0x10 &&
            (value & DMA_ENABLE)) {
            return best1700_dma_unsupported(s, offset, true, value);
        }
        /* ACTIVE is a read-only indication of beats currently in flight.
         * Transfers complete atomically during the timer callback. */
        *reg = (offset & (DMA_CHANNEL_STRIDE - 1)) == 0x10 ?
               value & ~DMA_ACTIVE : value;
        best1700_dma_update_irq(s);
        best1700_dma_schedule(s);
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x08: s->raw_tc &= ~value; break;
    case 0x10: s->raw_error &= ~value; break;
    case 0x00:
    case 0x04:
    case 0x0c:
    case 0x14:
    case 0x18:
    case 0x1c: return MEMTX_OK; /* Read-only status. */
    case 0x30:
        if (value & ~DMA_GLOBAL_FIELDS) {
            return best1700_dma_unsupported(s, offset, true, value);
        }
        s->config = value;
        break;
    default: return best1700_dma_unsupported(s, offset, true, value);
    }
    best1700_dma_update_irq(s);
    best1700_dma_schedule(s);
    return MEMTX_OK;
}

static const MemoryRegionOps best1700_dma_ops = {
    .read_with_attrs = best1700_dma_read,
    .write_with_attrs = best1700_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_dma_init(Best1700Dma *s, Object *owner, const char *name,
                       unsigned channels, AddressSpace *address_space)
{
    g_assert(channels > 0 && channels <= BEST1700_DMA_MAX_CHANNELS);
    g_assert(address_space);
    s->channel_count = channels;
    s->address_space = address_space;
    s->irq = NULL;
    s->name = g_strdup(name);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, best1700_dma_tick, s);
    memory_region_init_io(&s->iomem, owner, &best1700_dma_ops, s, name,
                          BEST1700_DMA_WINDOW);
    best1700_dma_reset(s);
}

void best1700_dma_set_irq(Best1700Dma *s, qemu_irq irq)
{
    s->irq = irq;
    best1700_dma_update_irq(s);
}

void best1700_dma_connect_terminal(Best1700Dma *s, void *opaque,
                                   Best1700DmaTerminalCallback callback)
{
    s->terminal_opaque = opaque;
    s->terminal_callback = callback;
}

void best1700_dma_reset(Best1700Dma *s)
{
    timer_del(s->timer);
    s->config = 0;
    s->raw_tc = 0;
    s->raw_error = 0;
    memset(s->channel, 0, sizeof(s->channel));
    best1700_dma_update_irq(s);
}

void best1700_dma_destroy(Best1700Dma *s)
{
    timer_free(s->timer);
    s->timer = NULL;
    g_free(s->name);
    s->name = NULL;
}
