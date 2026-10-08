/* BEST1700 Bluetooth-controller automatic power gating.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_btc_power.h"

#define BTC_PSC_WRITE_KEY 0xcafe0000u
#define BTC_PSC_AUTO_ENABLE (1u << 0)
#define BTC_PSC_HW_ENABLE (1u << 1)
#define BTC_PSC_AUTO_MASK (BTC_PSC_AUTO_ENABLE | BTC_PSC_HW_ENABLE)
#define BTC_PSC_SLEEP_NO_WFI (1u << 2)
#define BTC_PSC_WAKE_AON (1u << 15)
#define BTC_PSC_POWERDN_START 1u
#define BTC_PSC_POWERUP_START 1u
#define BTC_PSC_MANUAL_MASK 0x00000fffu
#define BTC_PSC_MANUAL_CLK_STOP (1u << 0)
#define BTC_PSC_MANUAL_ISO_EN (1u << 1)
#define BTC_PSC_MANUAL_RESETN_ASSERT (1u << 2)
#define BTC_PSC_MANUAL_PSW_EN (1u << 3)
#define BTC_PSC_MANUAL_MEM_PSW_EN (1u << 8)

static MemTxResult best1700_btc_power_unsupported(hwaddr offset,
                                                bool write, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 BTC PSC unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 "\n", write ? "write" : "read", offset, value);
    return MEMTX_ERROR;
}

static bool best1700_btc_power_wake_pending(const Best1700BtcPower *s)
{
    return s->wake_level && (s->wakeup_mask & BTC_PSC_WAKE_AON);
}

static void best1700_btc_power_set_off(Best1700BtcPower *s, bool off)
{
    if (s->off == off) {
        return;
    }
    s->off = off;
    if (s->set_power) {
        s->set_power(s->power_opaque, off);
    }
}

static void best1700_btc_power_update_wake(Best1700BtcPower *s)
{
    if (s->off && best1700_btc_power_wake_pending(s)) {
        best1700_btc_power_set_off(s, false);
    }
}

static MemTxResult best1700_btc_power_read(void *opaque, hwaddr offset,
                                          uint64_t *value, unsigned size,
                                          MemTxAttrs attrs)
{
    Best1700BtcPowerRegister *reg = opaque;
    Best1700BtcPower *s = reg->power;

    switch (reg->offset) {
    case 0x020:
        *value = s->auto_control;
        break;
    case 0x024:
        /* Only NO_WFI is modeled. The analog sequencer's state encoding
         * and acknowledgement timing are not synthesized here. */
        *value = s->sleep_control;
        break;
    case 0x028:
    case 0x02c:
    case 0x030:
        *value = s->manual_control;
        break;
    case 0x114:
    case 0x120:
        *value = 0;
        break;
    case 0x118:
        *value = s->wakeup_mask;
        break;
    case 0x124:
        /* Live masked wake level: neither read-clear nor a latched ACK. */
        *value = best1700_btc_power_wake_pending(s) ? BTC_PSC_WAKE_AON : 0;
        break;
    default:
        *value = 0;
        return best1700_btc_power_unsupported(reg->offset, false, 0);
    }
    return MEMTX_OK;
}

static MemTxResult best1700_btc_power_write(void *opaque, hwaddr offset,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    Best1700BtcPowerRegister *reg = opaque;
    Best1700BtcPower *s = reg->power;

    switch (reg->offset) {
    case 0x020:
        /* Validate the entire keyed write before changing any state. */
        if ((value & ~BTC_PSC_AUTO_MASK) != BTC_PSC_WRITE_KEY) {
            break;
        }
        s->auto_control = value & BTC_PSC_AUTO_MASK;
        return MEMTX_OK;
    case 0x024:
        if (value & ~BTC_PSC_SLEEP_NO_WFI) {
            break;
        }
        s->sleep_control = value;
        return MEMTX_OK;
    case 0x030: {
        uint32_t control;

        /* PSC control writes carry a fixed write-enable key. The lower
         * fields are hardware latches; each step in the power sequence
         * changes only the requested controls and never invents an ACK. */
        if ((value & 0xffff0000u) != BTC_PSC_WRITE_KEY ||
            (value & ~(BTC_PSC_WRITE_KEY | BTC_PSC_MANUAL_MASK))) {
            break;
        }
        control = value & BTC_PSC_MANUAL_MASK;
        s->manual_control = control;
        /* The final disable step asserts PSW_EN (and normally MEM_PSW_EN);
         * clearing PSW_EN is the corresponding enable step. */
        if (control & BTC_PSC_MANUAL_PSW_EN) {
            best1700_btc_power_set_off(s, true);
        } else {
            best1700_btc_power_set_off(s, false);
        }
        return MEMTX_OK;
    }
    case 0x028:
        if (value == 0) {
            return MEMTX_OK;
        }
        if (value == BTC_PSC_POWERDN_START) {
            best1700_btc_power_set_off(s, true);
            return MEMTX_OK;
        }
        break;
    case 0x02c:
        if (value == 0) {
            return MEMTX_OK;
        }
        if (value == BTC_PSC_POWERUP_START) {
            best1700_btc_power_set_off(s, false);
            return MEMTX_OK;
        }
        break;
    case 0x114:
        /* Inactive explicit requests and GPIO mask only. */
        if (value == 0) {
            return MEMTX_OK;
        }
        break;
    case 0x118:
        if (value & ~BTC_PSC_WAKE_AON) {
            break;
        }
        s->wakeup_mask = value;
        best1700_btc_power_update_wake(s);
        return MEMTX_OK;
    default:
        break;
    }
    return best1700_btc_power_unsupported(reg->offset, true, value);
}

static const MemoryRegionOps best1700_btc_power_ops = {
    .read_with_attrs = best1700_btc_power_read,
    .write_with_attrs = best1700_btc_power_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_btc_power_init(Best1700BtcPower *s, Object *owner)
{
    static const hwaddr offsets[BEST1700_BTC_POWER_REGISTERS] = {
        0x020, 0x024, 0x028, 0x02c, 0x030, 0x114, 0x118, 0x120, 0x124,
    };
    unsigned i;

    memory_region_init(&s->iomem, owner, "best1700.btc-power",
                       BEST1700_BTC_POWER_WINDOW);
    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        Best1700BtcPowerRegister *reg = &s->registers[i];
        g_autofree char *name = g_strdup_printf("best1700.btc-power.%03x",
                                               (unsigned)offsets[i]);

        reg->power = s;
        reg->offset = offsets[i];
        memory_region_init_io(&reg->iomem, owner, &best1700_btc_power_ops,
                              reg, name, 4);
        memory_region_add_subregion(&s->iomem, offsets[i], &reg->iomem);
    }
    best1700_btc_power_reset(s);
}

void best1700_btc_power_reset(Best1700BtcPower *s)
{
    /* Zero inactive configuration is model policy, not a silicon reset
     * measurement. The machine owns CPU/NVIC reset ordering. */
    s->auto_control = 0;
    s->sleep_control = 0;
    s->manual_control = 0;
    s->wakeup_mask = 0;
    s->wake_level = false;
    s->off = false;
}

void best1700_btc_power_connect(Best1700BtcPower *s, void *opaque,
                              void (*power)(void *opaque, bool off))
{
    s->power_opaque = opaque;
    s->set_power = power;
}

bool best1700_btc_power_request_sleep(Best1700BtcPower *s)
{
    if (s->off) {
        return true;
    }
    if (s->auto_control != BTC_PSC_AUTO_MASK ||
        !(s->sleep_control & BTC_PSC_SLEEP_NO_WFI)) {
        return false;
    }

    s->off = true;
    if (s->set_power) {
        s->set_power(s->power_opaque, true);
    }
    /* A request can arrive before the CPU commits its automatic power
     * transition. Still complete that transition and release through the
     * retained reset vector when the wake level is already high. Refusing
     * to gate leaves firmware in its IRQ-masked WFI sleep entry with no
     * warm restore, so a retained IPC request can never be acknowledged. */
    best1700_btc_power_update_wake(s);
    return s->off;
}

void best1700_btc_power_set_wake(Best1700BtcPower *s, bool level)
{
    s->wake_level = level;
    best1700_btc_power_update_wake(s);
}

bool best1700_btc_power_is_off(const Best1700BtcPower *s)
{
    return s->off;
}
