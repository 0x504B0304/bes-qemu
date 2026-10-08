/* BEST1700 Bluetooth modem configuration latches.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_btc_modem.h"

#define MODEM_MIX_START (1u << 30)
#define MODEM_MIX_CONFIG_MASK 0xde800168u
#define MODEM_ENERGY_MASK 0x003fffffu

/* Only established configuration fields are implemented. In particular,
 * energy measurements, calibration commands and RF events are not storage.
 * The supported fields have zero reset values. No signal processing or
 * successful calibration is implied by accepting a configuration write. */
static const struct {
    hwaddr offset;
    uint32_t mask;
} modem_fields[] = {
    { 0x004, 0x000000f0 }, /* AHI tracking */
    { 0x00c, 0x10003c7d }, /* Demodulator selection and lock thresholds */
    { 0x018, 0x00000007 }, /* Power estimator coefficient */
    { 0x028, 0xffffffff }, /* One-tone diagnostic gain */
    { 0x02c, 0x0000000f }, /* One-tone diagnostic frequency */
    { 0x040, 0x00000001 }, /* IQ compensation enable */
    { 0x05c, 0x000e0000 }, /* IQ compensation gain */
    { 0x0c0, 0x00000008 }, /* PSD filter bypass */
    { 0x0c4, 0x3fffffff }, /* BLE TX power coefficients 0..2 */
    { 0x0c8, 0x3fffffff }, /* BLE TX power coefficients 3..5 */
    { 0x0cc, 0x3fffffff }, /* BLE TX power coefficients 6..7 */
    { 0x0d0, 0xffffffff }, /* Rate converter IF */
    { 0x0d4, 0x07ff07ff }, /* Filter timing */
    { 0x0e0, 0x00000018 }, /* AGC power lock */
    { 0x0e4, 0xfffff000 },
    { 0x0e8, 0xff000000 },
    { 0x0ec, 0xfffff000 },
    { 0x0f0, 0xff000000 },
    { 0x0f4, 0xfffff000 },
    { 0x0f8, 0xff000000 },
    { 0x104, 0x1fffffff }, /* AGC acquisition thresholds */
    { 0x108, 0x1fffffff },
    { 0x120, 0xffffffff }, /* IQ compensation timing */
    { 0x134, 0xffffffff }, /* AGC lock count */
    { 0x140, 0x00000003 }, /* Oversampling selection */
    { 0x148, 0xffffffff }, /* IF selection */
    { 0x14c, 0x01ffffff }, /* Rate converter step */
    { 0x150, 0x01ffffff },
    { 0x154, 0x000fffff }, /* Channel step */
    { 0x158, 0x000fffff },
    { 0x15c, 0x00000003 }, /* ADC clock rate */
    { 0x160, 0xfffff000 }, /* AGC coefficients */
    { 0x164, 0xff000000 },
    { 0x168, 0xfffff000 },
    { 0x16c, 0xff000000 },
    { 0x170, 0xffffffff },
    { 0x174, 0xffffffff },
    { 0x178, 0xfffff000 },
    { 0x17c, 0xff000000 },
    { 0x1a8, 0xffffffff },
    { 0x1ac, 0x00000fff },
    { 0x1b0, 0xffffffff },
    { 0x1b4, 0xffffffff }, /* Synchronization thresholds */
    { 0x1bc, 0x3fff8000 },
    { 0x1c0, 0xe0f041ff },
    { 0x1c4, 0xffff8000 },
    { 0x1c8, 0xffffffff },
    { 0x1cc, 0x1ff00000 },
    { 0x1d0, 0x03fc0000 },
    { 0x1e8, 0xffc0001d }, /* DPSK demodulator control */
    { 0x1f0, 0x0000ff00 }, /* DPSK gain */
    { 0x20c, 0xffffffff }, /* Modem delay */
    { 0x210, 0x000000ff }, /* Transmit startup delay */
    { 0x214, 0x000d0000 }, /* Transmit IQ swap and signed data */
    { 0x228, 0x7fffffff }, /* Fast-ack waveform and correlation thresholds */
    { 0x230, 0xffff0000 }, /* Receive power thresholds */
    { 0x234, 0xffff0000 },
    { 0x238, 0xffff0000 },
    { 0x23c, 0x000fffff }, /* Receive rate converter */
    { 0x240, 0xffff8404 },
    { 0x248, 0x81ffffff }, /* Rate conversion and MDLL clock selection */
    { 0x250, 0xefffffff }, /* IQ mismatch/DC calibration control */
    { 0x254, 0x00000018 }, /* Transmit ramp selection */
    { 0x25c, 0x0000000e }, /* Input filter enable */
    { 0x2c0, 0x00100000 }, /* AGC RRC gain selection */
    { 0x2c4, 0xffe007ff }, /* Filter timing */
    { 0x300, 0x0000000f }, /* GFSK sample conversion */
    { 0x308, 0x000fffff },
    { 0x324, 0x00000009 }, /* GFSK configuration */
    { 0x334, MODEM_MIX_START }, /* one-tone mixer command */
    { 0x338, MODEM_ENERGY_MASK }, /* one-tone I energy readback */
    { 0x33c, MODEM_ENERGY_MASK }, /* one-tone Q energy readback */
    { 0x340, 0x0000000f }, /* DPSK sample conversion */
    { 0x344, 0x000003ff },
    { 0x370, 0x00ffffff }, /* EDR sample conversion */
    { 0x374, 0x3fff000f }, /* AHI tracking */
    { 0x378, 0x3fff0fff },
    { 0x400, 0x00000001 }, /* Receive timing enable */
    { 0x414, 0x00ffffff }, /* Synchronization error threshold */
    { 0x438, 0x0000ffff }, /* Receive startup delays */
    { 0x440, 0x00000070 }, /* Oversampling position */
};

static uint32_t modem_mask(hwaddr offset)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(modem_fields); i++) {
        if (modem_fields[i].offset == offset) {
            return modem_fields[i].mask;
        }
    }
    return 0;
}

static MemTxResult modem_unsupported(hwaddr offset, bool write, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 BTC modem unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 "\n", write ? "write" : "read", offset, value);
    return MEMTX_ERROR;
}

static MemTxResult modem_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    Best1700BtcModem *s = opaque;

    *value = 0;
    if (offset == 0x334) {
        /* START_MIX is a command pulse.  The real block does not expose a
         * sticky completion bit at this address, so reads observe the idle
         * value after the pulse has been accepted. */
        return MEMTX_OK;
    }
    if (offset == 0x338) {
        *value = s->energy_i;
        return MEMTX_OK;
    }
    if (offset == 0x33c) {
        *value = s->energy_q;
        return MEMTX_OK;
    }
    if (!modem_mask(offset)) {
        return modem_unsupported(offset, false, 0);
    }
    *value = s->config[offset / sizeof(uint32_t)];
    return MEMTX_OK;
}

static MemTxResult modem_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    Best1700BtcModem *s = opaque;
    uint32_t mask = modem_mask(offset);

    if (offset == 0x334) {
        /* MIX_MODE_CONFIG is driven by the firmware as a two-phase command.  The
         * generated register header documents START_MIX (bit 30), while the
         * production driver supplies mixer setup fields in the same write:
         * 0x9e000168/0x9e800168 followed by 0xde000168/0xde800168.  Reserved
         * bits outside that observed hardware word remain fail-closed. */
        if (value & ~MODEM_MIX_CONFIG_MASK) {
            return modem_unsupported(offset, true, value);
        }
        s->mix_config = value & ~MODEM_MIX_START;
        if (value == MODEM_MIX_START) {
            /* There is no RF source attached to the emulator.  Accept the
             * command and publish the hardware reset/no-signal measurement
             * instead of inventing a successful energy result. */
            s->energy_i = 0;
            s->energy_q = 0;
        } else if (value & MODEM_MIX_START) {
            /* START_MIX is a pulse.  The analog source is intentionally not
             * synthesized; a valid command still reaches the read-only
             * measurement registers with the no-signal value. */
            s->energy_i = 0;
            s->energy_q = 0;
        }
        return MEMTX_OK;
    }
    if (offset == 0x338 || offset == 0x33c) {
        /* These are measurement readbacks.  They are intentionally not
         * writable even though the generic register header contains generic
         * field setters for every register. */
        return modem_unsupported(offset, true, value);
    }

    /* Unsupported fields are an incomplete-model diagnostic, not a claim
     * about the electrical behavior of reserved silicon registers. */
    if (!mask || (value & ~mask)) {
        return modem_unsupported(offset, true, value);
    }
    s->config[offset / sizeof(uint32_t)] = value;
    return MEMTX_OK;
}

static const MemoryRegionOps modem_ops = {
    .read_with_attrs = modem_read,
    .write_with_attrs = modem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_btc_modem_init(Best1700BtcModem *s, Object *owner)
{
    memory_region_init_io(&s->iomem, owner, &modem_ops, s,
                          "best1700.btc-modem", BEST1700_BTC_MODEM_WINDOW);
    best1700_btc_modem_reset(s);
}

void best1700_btc_modem_reset(Best1700BtcModem *s)
{
    memset(s->config, 0, sizeof(s->config));
    s->mix_config = 0;
    s->energy_i = 0;
    s->energy_q = 0;
}
