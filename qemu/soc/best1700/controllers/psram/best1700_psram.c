/*
 * BEST1700 PSRAM manager and digital PHY control.
 *
 * The power handshake and manager transfers use virtual time. Analog delay
 * calibration and memory-array transfers are not modeled by this device;
 * array storage and mode-register identity belong to the package endpoint.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "best1700_psram.h"

#define MC_COMMAND       0x004
#define MC_ADDRESS       0x008
#define MC_LENGTH        0x00c
#define MC_TX            0x014
#define MC_RX            0x018
#define MC_CLEAR         0x01c
#define MC_POWER         0x024
#define MC_WRITE_BUFFER  0x044
#define MC_FREQUENCY     0x200
#define MC_INIT          0x400
#define MC_STATUS        0x404

#define PHY_CONTROL     0x040
#define PHY_STATUS      0x044
#define PHY_DRIVE        0x050
#define PHY_DLL_STATUS   0x054
#define PHY_ANALOG       0x058
#define PHY_AUTO_LP_MASK ((1u << 11) | (1u << 12))

/* Transaction-level timing, rather than an analog timing specification. */
#define COMPLETION_NS 1000

static void best1700_psram_dll_update(Best1700PsramState *s)
{
    uint32_t drive = s->phy[PHY_DRIVE / 4];
    uint32_t analog = s->phy[PHY_ANALOG / 4];
    bool clock_ready = !(drive & (1u << 29)) || (analog & (1u << 16));
    bool reset_released = !(drive & (1u << 28)) || (analog & (1u << 19));
    bool enabled = s->phy_lp_state == 3 && clock_ready && reset_released;

    if (enabled != s->dll_enabled) {
        timer_del(s->dll_timer);
        s->dll_enabled = enabled;
        s->dll_locked = false;
        if (enabled) {
            timer_mod(s->dll_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + COMPLETION_NS);
        }
    }
}

static void best1700_psram_dll_complete(void *opaque)
{
    Best1700PsramState *s = opaque;

    s->dll_locked = s->dll_enabled;
}

static uint32_t best1700_psram_dll_status(Best1700PsramState *s)
{
    if (!s->dll_locked) {
        return 0;
    }
    return s->dll_delay_taps | (1u << 6) |
           (s->dll_delay_taps == 0 ? 1u << 7 : 0) |
           (s->dll_delay_taps == 63 ? 1u << 8 : 0);
}

static void best1700_psram_set_analog_control(Best1700PsramState *s,
                                             uint32_t value)
{
    uint32_t previous = s->phy[PHY_ANALOG / 4];

    s->phy[PHY_ANALOG / 4] = value;
    /* A changed delay/range restarts acquisition. Clock and reset overrides
     * are resolved by the same path used for the MMIO control registers. */
    if ((previous ^ value) & 0x6ffff) {
        timer_del(s->dll_timer);
        s->dll_enabled = s->dll_locked = false;
    }
    best1700_psram_dll_update(s);
}

bool best1700_psram_ispi_read(void *opaque, unsigned page, unsigned reg,
                             uint16_t *value)
{
    Best1700PsramState *s = opaque;

    if (page || reg >= ARRAY_SIZE(s->analog)) {
        return false;
    }
    switch (reg) {
    case 0x50:
        *value = s->phy[PHY_ANALOG / 4] & 0xffff;
        break;
    case 0x51:
        *value = best1700_psram_dll_status(s);
        break;
    case 0x52:
        *value = (s->analog[reg] & ~(3u << 11)) |
                 (((s->phy[PHY_ANALOG / 4] >> 17) & 3) << 11);
        break;
    default:
        *value = s->analog[reg];
        break;
    }
    return true;
}

bool best1700_psram_ispi_write(void *opaque, unsigned page, unsigned reg,
                              uint16_t value)
{
    Best1700PsramState *s = opaque;

    if (page || reg >= ARRAY_SIZE(s->analog)) {
        return false;
    }
    if (reg == 0x51) {
        return true; /* Read-only lock and delay result, also exposed by MMIO. */
    }
    s->analog[reg] = value;
    if (reg == 0x50) {
        best1700_psram_set_analog_control(s,
            (s->phy[PHY_ANALOG / 4] & ~0xffffu) | value);
    } else if (reg == 0x52) {
        best1700_psram_set_analog_control(s,
            (s->phy[PHY_ANALOG / 4] & ~(3u << 17)) |
            (((value >> 11) & 3) << 17));
    }
    return true;
}

static void best1700_psram_phy_complete(void *opaque)
{
    Best1700PsramState *s = opaque;

    if ((s->phy[PHY_CONTROL / 4] & PHY_AUTO_LP_MASK) == PHY_AUTO_LP_MASK) {
        /* Digital power-up handshake state used by this PHY revision. */
        s->phy_lp_state = 3;
    }
    best1700_psram_dll_update(s);
}

static void best1700_psram_complete(void *opaque)
{
    Best1700PsramState *s = opaque;
    unsigned count = s->command_words;

    switch (s->command) {
    case 0x05: /* Mode-register set. */
        if (count > s->tx_count) {
            return; /* The guest may still supply the remaining FIFO words. */
        }
        for (unsigned i = 0; i < count; i++) {
            if (s->endpoint.write_mode) {
                s->endpoint.write_mode(s->endpoint.opaque, s->command_channels,
                                       s->command_address + i, s->tx[i]);
            }
        }
        memmove(s->tx, s->tx + count,
                (s->tx_count - count) * sizeof(s->tx[0]));
        s->tx_count -= count;
        break;
    case 0x06: /* Mode-register read. */
        s->rx_head = 0;
        s->rx_count = count;
        for (unsigned i = 0; i < count; i++) {
            s->rx[i] = s->endpoint.read_mode ?
                s->endpoint.read_mode(s->endpoint.opaque, s->command_channels,
                                      s->command_address + i) : UINT32_MAX;
        }
        if (!s->endpoint.read_mode) {
            qemu_log_mask(LOG_UNIMP, "best1700 psram: mode read without package\n");
        }
        break;
    case 0x09: /* Self refresh. */
    case 0x10: /* Power down. */
        s->sleeping = true;
        break;
    case 0x20: /* NOP exits a managed low-power state. */
    case 0x40: /* Start clock. */
        s->sleeping = false;
        break;
    case 0xff: /* Package reset does not reset controller configuration. */
        s->tx_count = s->rx_count = s->rx_head = 0;
        s->sleeping = false;
        if (s->endpoint.reset) {
            s->endpoint.reset(s->endpoint.opaque, s->command_channels);
        }
        break;
    case 0x00: /* No command. */
    case 0x08: /* Refresh. */
    case 0x25: /* Impedance calibration reset. */
    case 0x45: /* Short impedance calibration. */
    case 0x80: /* Stop clock. */
    case 0x85: /* Long impedance calibration. */
        break;
    default:
        g_assert_not_reached(); /* Rejected at submission. */
    }
    s->pending = false;
}

static void best1700_psram_schedule(Best1700PsramState *s)
{
    if (!s->pending || s->rejected || timer_pending(s->command_timer) ||
        (s->command == 0x05 && s->tx_count < s->command_words)) {
        return;
    }
    timer_mod(s->command_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + COMPLETION_NS);
}

static bool best1700_psram_command_supported(unsigned command)
{
    switch (command) {
    case 0x00:
    case 0x05:
    case 0x06:
    case 0x08:
    case 0x09:
    case 0x10:
    case 0x20:
    case 0x25:
    case 0x40:
    case 0x45:
    case 0x80:
    case 0x85:
    case 0xff:
        return true;
    default:
        return false;
    }
}

static uint32_t best1700_psram_status(Best1700PsramState *s)
{
    uint32_t value = (1u << 24) | (1u << 25); /* Empty array read/write buffers. */

    value |= s->pending ? 1 : 0;
    value |= s->rx_count ? s->rx_count << 2 : 1u << 1;
    value |= s->tx_count == BEST1700_PSRAM_FIFO_WORDS ? 1u << 6 : 0;
    value |= (BEST1700_PSRAM_FIFO_WORDS - s->tx_count) << 7;
    if (s->regs[MC_INIT / 4] & 1) {
        value |= (s->sleeping ? 2u : 4u) << 16;
    }
    return value;
}

static uint64_t best1700_psram_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700PsramState *s = opaque;

    if (offset & 3) {
        return 0;
    }
    if (offset >= BEST1700_PSRAM_PHY_OFFSET &&
        offset < BEST1700_PSRAM_PHY_OFFSET + sizeof(s->phy)) {
        offset -= BEST1700_PSRAM_PHY_OFFSET;
        if (offset == PHY_STATUS) {
            return (s->phy_lp_state << 25) | (1u << 5) | (1u << 15);
        }
        if (offset == PHY_DLL_STATUS) {
            return best1700_psram_dll_status(s);
        }
        return s->phy[offset / 4];
    }
    if (offset >= sizeof(s->regs)) {
        return 0;
    }
    switch (offset) {
    case MC_STATUS:
        return best1700_psram_status(s);
    case MC_RX:
        if (s->rx_count) {
            s->rx_count--;
            return s->rx[s->rx_head++];
        }
        return 0;
    case MC_CLEAR:
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static void best1700_psram_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    Best1700PsramState *s = opaque;

    if (offset & 3) {
        return;
    }
    if (offset >= BEST1700_PSRAM_PHY_OFFSET &&
        offset < BEST1700_PSRAM_PHY_OFFSET + sizeof(s->phy)) {
        offset -= BEST1700_PSRAM_PHY_OFFSET;
        if (offset == PHY_STATUS || offset == PHY_DLL_STATUS) {
            return;
        }
        if (offset == PHY_ANALOG) {
            best1700_psram_set_analog_control(s, value);
            return;
        }
        if (offset == PHY_CONTROL) {
            uint32_t previous = s->phy[offset / 4] & PHY_AUTO_LP_MASK;
            uint32_t next = value & PHY_AUTO_LP_MASK;

            if (next != previous) {
                timer_del(s->phy_timer);
                s->phy_lp_state = 0;
                if (next == PHY_AUTO_LP_MASK) {
                    timer_mod(s->phy_timer,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + COMPLETION_NS);
                }
            }
        }
        s->phy[offset / 4] = value;
        if (offset == PHY_CONTROL || offset == PHY_DRIVE) {
            best1700_psram_dll_update(s);
        }
        return;
    }
    if (offset >= sizeof(s->regs)) {
        return;
    }
    switch (offset) {
    case MC_STATUS:
    case MC_RX:
        return;
    case MC_COMMAND:
        if (s->pending && (value & 0xff) != 0xff) {
            qemu_log_mask(LOG_GUEST_ERROR, "best1700 psram: command while busy\n");
            return;
        }
        s->command = value;
        s->command_address = s->regs[MC_ADDRESS / 4];
        s->command_channels = (s->regs[MC_LENGTH / 4] >> 13) & 3;
        s->command_words = s->regs[MC_LENGTH / 4] & 0xff;
        s->pending = true;
        s->rejected = !best1700_psram_command_supported(s->command) ||
            ((s->command == 0x05 || s->command == 0x06) &&
             (!s->command_words || s->command_words > BEST1700_PSRAM_FIFO_WORDS)) ||
            (s->command == 0x06 && s->rx_count);
        timer_del(s->command_timer);
        if (s->rejected) {
            /* No fabricated hardware error bit. Hold busy until FIFO clear,
             * package reset or controller reset, and preserve queued data. */
            qemu_log_mask(LOG_UNIMP,
                          "best1700 psram: rejected manager command 0x%02x"
                          " words=%u rx-pending=%u\n", s->command,
                          s->command_words, s->rx_count);
        } else {
            best1700_psram_schedule(s);
        }
        break;
    case MC_TX:
        if (s->tx_count < BEST1700_PSRAM_FIFO_WORDS) {
            s->tx[s->tx_count++] = value;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "best1700 psram: transmit FIFO full\n");
        }
        best1700_psram_schedule(s);
        return;
    case MC_CLEAR:
        if (value & 1) {
            s->tx_count = 0;
        }
        if (value & 2) {
            s->rx_count = s->rx_head = 0;
        }
        if (value & 3) {
            if (s->rejected) {
                s->pending = s->rejected = false;
            } else if ((value & 1) && s->pending && s->command == 0x05) {
                timer_del(s->command_timer);
                best1700_psram_schedule(s);
            }
        }
        return;
    case MC_FREQUENCY:
        /* The frequency status is read-only and follows the selected point. */
        value = (value & ~(1u << 9)) | ((value & 1) << 9);
        break;
    case MC_POWER:
        s->sleeping = (value & 3) != 0;
        break;
    case MC_WRITE_BUFFER:
        value &= ~3u; /* No deferred array writes: drain/invalidate self-clear. */
        break;
    default:
        break;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps best1700_psram_ops = {
    .read = best1700_psram_read,
    .write = best1700_psram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void best1700_psram_connect_endpoint(Best1700PsramState *s,
                                    const Best1700PsramEndpoint *endpoint)
{
    s->endpoint = endpoint ? *endpoint : (Best1700PsramEndpoint) {0};
}

void best1700_psram_reset(Best1700PsramState *s)
{
    timer_del(s->command_timer);
    timer_del(s->phy_timer);
    timer_del(s->dll_timer);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->phy, 0, sizeof(s->phy));
    memset(s->analog, 0, sizeof(s->analog));
    memset(s->tx, 0, sizeof(s->tx));
    memset(s->rx, 0, sizeof(s->rx));
    s->tx_count = s->rx_count = s->rx_head = 0;
    s->command_address = 0;
    s->command = s->command_channels = s->command_words = 0;
    s->phy_lp_state = 0;
    s->pending = s->rejected = s->sleeping = false;
    s->dll_enabled = s->dll_locked = false;
}

static void best1700_psram_device_reset(DeviceState *dev)
{
    best1700_psram_reset(BEST1700_PSRAM(dev));
}

static void best1700_psram_init(Object *obj)
{
    Best1700PsramState *s = BEST1700_PSRAM(obj);

    memory_region_init_io(&s->iomem, obj, &best1700_psram_ops, s,
                          TYPE_BEST1700_PSRAM, BEST1700_PSRAM_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->command_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    best1700_psram_complete, s);
    s->phy_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                best1700_psram_phy_complete, s);
    s->dll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                best1700_psram_dll_complete, s);
}

static void best1700_psram_finalize(Object *obj)
{
    Best1700PsramState *s = BEST1700_PSRAM(obj);

    timer_free(s->command_timer);
    timer_free(s->phy_timer);
    timer_free(s->dll_timer);
}

static void best1700_psram_realize(DeviceState *dev, Error **errp)
{
    Best1700PsramState *s = BEST1700_PSRAM(dev);

    if (s->dll_delay_taps > 63) {
        error_setg(errp, "BEST1700 PSRAM DLL delay must be in 0..63");
    }
}

static Property best1700_psram_properties[] = {
    /* Ideal-clock compatibility parameter, not a measured silicon value. */
    DEFINE_PROP_UINT32("dll-delay-taps", Best1700PsramState, dll_delay_taps, 16),
    DEFINE_PROP_END_OF_LIST(),
};

static void best1700_psram_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BEST1700 PSRAM manager and digital PHY";
    dc->realize = best1700_psram_realize;
    dc->reset = best1700_psram_device_reset;
    device_class_set_props(dc, best1700_psram_properties);
}

static const TypeInfo best1700_psram_info = {
    .name = TYPE_BEST1700_PSRAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700PsramState),
    .instance_init = best1700_psram_init,
    .instance_finalize = best1700_psram_finalize,
    .class_init = best1700_psram_class_init,
};

static void best1700_psram_register_types(void)
{
    type_register_static(&best1700_psram_info);
}

type_init(best1700_psram_register_types)
