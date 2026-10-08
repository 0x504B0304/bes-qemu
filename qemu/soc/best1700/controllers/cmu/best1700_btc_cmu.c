/* BEST1700 Bluetooth-controller clock and reset registers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_btc_cmu.h"

#define BTC_SUBSYS_RESET (1u << 3)
#define BTC_CPU_RESET (1u << 7)
#define BTC_AON_RESET_MASK (BTC_SUBSYS_RESET | BTC_CPU_RESET)
#define BTC_VTOR_MASK 0xffffff80u
#define BTC_CPU_CFG_CONTROL_MASK 0x3f8u
#define BTC_DEEPSLEEP_START (1u << 0)
#define BTC_DEEPSLEEP_GATE (1u << 1)
#define BTC_CAL_LPO_INT_EN (1u << 2)
#define BTC_HCLK_MCU_ENABLE (1u << 3)
#define BTC_DEEPSLEEP_EN (1u << 4)
#define BTC_DEEPSLEEP_ROMRAM_EN (1u << 5)
#define BTC_OSC_EN_BT_EN (1u << 6)
#define BTC_UART_WAKE_EN (1u << 7)
#define BTC_TIMER_WT26_MASK (0xffu << 8)
#define BTC_CPU_IDLE_GATE_EN (1u << 16)
#define BTC_MANUAL_RAM_RETN (1u << 17)
#define BTC_CPU_IDLE_GATE_DIS (1u << 18)
#define BTC_DEEPSLEEP_MODE (1u << 24)
#define BTC_PU_OSC (1u << 25)
#define BTC_UART_WAKE (1u << 27)
#define BTC_SLEEP_ALLOW (1u << 29)
#define BTC_DEEPSLEEP_ALLOWED (1u << 30)
#define BTC_SLEEP_SYNC (1u << 31)
#define BTC_SLEEP_CFG_CONTROL_MASK (BTC_DEEPSLEEP_GATE | BTC_CAL_LPO_INT_EN | \
                                    BTC_HCLK_MCU_ENABLE | BTC_DEEPSLEEP_EN | \
                                    BTC_DEEPSLEEP_ROMRAM_EN | BTC_OSC_EN_BT_EN | \
                                    BTC_UART_WAKE_EN | BTC_TIMER_WT26_MASK | \
                                    BTC_CPU_IDLE_GATE_EN | BTC_MANUAL_RAM_RETN | \
                                    BTC_CPU_IDLE_GATE_DIS | BTC_DEEPSLEEP_MODE | \
                                    BTC_PU_OSC | BTC_UART_WAKE | BTC_SLEEP_ALLOW | \
                                    BTC_DEEPSLEEP_ALLOWED)
#define BTC_RF_MEM_TIMING_MASK 0x0003ffffu
#define BTC_RAM_MEM_TIMING_MASK 0x0007ffffu
#define BTC_WRITE_UNLOCK (1u << 0)
#define BTC_WRITE_UNLOCK_STATUS (1u << 1)
#define BTC_WAKEUP_MASK_EXT 0x0000ffffu
#define BTC_CAL_LPO_DONE (1u << 31)
#define BTC_PVT_RO_EN (1u << 0)
#define BTC_PVT_COUNT_START_EN (1u << 1)
#define BTC_PVT_OSC_CLK_EN (1u << 2)
#define BTC_PVT_RSTN (1u << 3)
#define BTC_PVT_TIMESET_MASK (0x3ffu << 4)
#define BTC_PVT_CFG_MASK (BTC_PVT_RO_EN | BTC_PVT_COUNT_START_EN | \
                          BTC_PVT_OSC_CLK_EN | BTC_PVT_RSTN | \
                          BTC_PVT_TIMESET_MASK)
#define BTC_PVT_COUNT_MASK 0x3ffffu
#define BTC_PVT_COUNT_DONE (1u << 18)

static MemTxResult best1700_btc_cmu_unsupported(hwaddr offset,
                                               bool write, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 BTC CMU unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 "\n", write ? "write" : "read", offset, value);
    /* Keep missing devices visible. In particular, an IPC command must not
     * turn into a successful TX completion or a fabricated ready flag. */
    return MEMTX_ERROR;
}

static void best1700_btc_cmu_update_irq(Best1700BtcCmu *s)
{
    qemu_set_irq(s->calibration_irq,
                 s->calibration_pending &&
                 (s->sleep_config & BTC_CAL_LPO_INT_EN));
}

static void best1700_btc_cmu_calibration_complete(void *opaque)
{
    Best1700BtcCmu *s = opaque;

    s->calibration_busy = false;
    s->calibration_result = BTC_CAL_LPO_DONE | s->calibration_count;
    s->calibration_pending = true;
    best1700_btc_cmu_update_irq(s);
}

static void best1700_btc_cmu_pvt_complete(void *opaque)
{
    Best1700BtcCmu *s = opaque;
    uint64_t window_us = (s->pvt_cfg & BTC_PVT_TIMESET_MASK) >> 4;
    uint64_t count;

    /* The firmware starts the PVT counter with all four control bits asserted and
     * polls COUNT_DONE.  Use the profile reference clock as the deterministic
     * virtual ring-oscillator source; the window and saturation behavior stay
     * architectural and are independent of host wall-clock speed. */
    count = ((uint64_t)s->reference_hz * window_us + 999999) / 1000000;
    count = MIN(count, (uint64_t)BTC_PVT_COUNT_MASK);
    s->pvt_count = (uint32_t)count | BTC_PVT_COUNT_DONE;
    s->pvt_busy = false;
}

static MemTxResult best1700_btc_cmu_pvt_start(Best1700BtcCmu *s)
{
    uint64_t window_us = (s->pvt_cfg & BTC_PVT_TIMESET_MASK) >> 4;
    int64_t now;

    if (s->pvt_busy || !s->pvt_timer ||
        !(s->pvt_cfg & BTC_PVT_RO_EN) ||
        !(s->pvt_cfg & BTC_PVT_COUNT_START_EN) ||
        !(s->pvt_cfg & BTC_PVT_OSC_CLK_EN) ||
        !(s->pvt_cfg & BTC_PVT_RSTN)) {
        return best1700_btc_cmu_unsupported(0xcc, true, s->pvt_cfg);
    }
    if (!window_us) {
        window_us = 1;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now > INT64_MAX - (int64_t)(window_us * 1000)) {
        return best1700_btc_cmu_unsupported(0xcc, true, s->pvt_cfg);
    }
    s->pvt_count = 0;
    s->pvt_busy = true;
    timer_mod_ns(s->pvt_timer, now + (int64_t)(window_us * 1000));
    return MEMTX_OK;
}

static MemTxResult best1700_btc_cmu_calibration_start(Best1700BtcCmu *s,
                                                     uint64_t value)
{
    uint64_t cycles = (s->clk_out & 0xff) + 1;
    uint64_t count, duration_ns;
    int64_t now;

    /* Busy-start rejection is model policy. Validate before replacing any
     * prior result, pending interrupt, or in-flight measurement. */
    if (value != 1 || s->calibration_busy || !s->calibration_timer ||
        !s->reference_hz || !s->lpo_hz) {
        return best1700_btc_cmu_unsupported(0x48, true, value);
    }
    count = cycles * s->reference_hz / s->lpo_hz;
    duration_ns = (cycles * NANOSECONDS_PER_SECOND + s->lpo_hz - 1) /
                  s->lpo_hz;
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!count || count > (BTC_CAL_LPO_DONE - 1) ||
        now > INT64_MAX - (int64_t)duration_ns) {
        return best1700_btc_cmu_unsupported(0x48, true, value);
    }

    /* Capture the count and deadline now: later clock-output writes only
     * affect the next measurement. Frequencies are startup-only inputs. */
    s->calibration_count = count;
    s->calibration_result = 0;
    s->calibration_pending = false;
    s->calibration_busy = true;
    best1700_btc_cmu_update_irq(s);
    timer_mod_ns(s->calibration_timer, now + (int64_t)duration_ns);
    return MEMTX_OK;
}

static MemTxResult best1700_btc_cmu_read(void *opaque, hwaddr offset,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    Best1700BtcCmu *s = opaque;

    switch (offset) {
    case 0x00:
    case 0x04:
        *value = s->clocks;
        break;
    case 0x08:
        *value = s->clock_mode;
        break;
    case 0x20:
        *value = s->mode_mclk;
        break;
    case 0x28:
        *value = s->target_addr;
        break;
    case 0x30:
        *value = s->ram_timing;
        break;
    case 0x0c:
        *value = s->div_timer;
        break;
    case 0x10:
    case 0x14:
        *value = s->reset_n;
        break;
    case 0x18:
        *value = s->div_wdt;
        break;
    case 0x24:
        *value = s->sleep_config | (s->mac_sleep_ready ? BTC_SLEEP_SYNC : 0);
        break;
    case 0x2c:
        *value = s->cpu_cfg;
        break;
    case 0x34:
        *value = s->rf_mem_timing;
        break;
    case 0x38:
        *value = s->clock_select;
        break;
    case 0x3c:
        *value = s->wakeup_mask;
        break;
    case 0x40:
        *value = s->write_unlocked ? BTC_WRITE_UNLOCK_STATUS : 0;
        break;
    case 0x44:
        *value = s->clk_out;
        break;
    case 0x48:
        *value = s->calibration_result;
        /* Read-ack is model policy: the original handler reads this result
         * without an explicit ACK, but pulse vs read-clear is unconfirmed.
         * DONE/count remain readable after the interrupt is acknowledged. */
        s->calibration_pending = false;
        best1700_btc_cmu_update_irq(s);
        break;
    case 0x4c:
        /* Request readback is model policy, not actual CPU halt status. */
        *value = s->idle_request;
        break;
    case 0x58:
        *value = s->mresetn_mode;
        break;
    case 0x5c:
        *value = s->mresetn_rxtx;
        break;
    case 0x60:
    case 0x64:
        *value = s->mresetn_state;
        break;
    case 0x68:
    case 0x6c:
        *value = s->mclk_enable;
        break;
    case 0x70:
        *value = s->mresetn_mode2;
        break;
    case 0x74:
        *value = s->mresetn_rxtx2;
        break;
    case 0x78:
    case 0x7c:
        *value = s->mresetn_state2;
        break;
    case 0x80:
    case 0x84:
        *value = s->mclk_enable2;
        break;
    case 0x88:
        *value = s->mclk_mode2;
        break;
    case 0xcc:
        *value = s->pvt_cfg;
        break;
    case 0xd0:
        *value = s->pvt_count;
        break;
    case 0x8c:
        *value = s->wakeup_mask_ext;
        break;
    case 0x90:
        *value = s->irq_masked_ext;
        break;
    case 0xbc:
        /* The SDK exposes this as the controller crash-dump PC. It is a
         * live diagnostic view of the BTC CPU, rather than a writable latch. */
        *value = s->cpu_pc ? *s->cpu_pc : s->bt_pc;
        break;
    case 0xc0:
        *value = s->wifi_irq_mask;
        break;
    case 0xc8:
        *value = s->irq_masked;
        break;
    case 0xd4:
    case 0xd8:
        *value = s->ext_clocks;
        break;
    case 0xdc:
        *value = s->ext_clock_mode;
        break;
    case 0xe0:
    case 0xe4:
        *value = s->ext_reset_n;
        break;
    case 0xa8:
        *value = s->sel_sys_gt;
        break;
    default:
        *value = 0;
        return best1700_btc_cmu_unsupported(offset, false, 0);
    }
    return MEMTX_OK;
}

static MemTxResult best1700_btc_cmu_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    Best1700BtcCmu *s = opaque;

    switch (offset) {
    case 0x00:
        s->clocks |= value;
        break;
    case 0x04:
        s->clocks &= ~value;
        break;
    case 0x08:
        s->clock_mode = value;
        break;
    case 0x20:
        s->mode_mclk = value;
        break;
    case 0x28:
        s->target_addr = value;
        break;
    case 0x30:
        s->ram_timing = value & BTC_RAM_MEM_TIMING_MASK;
        break;
    case 0x0c:
        s->div_timer = value;
        break;
    case 0x10:
        /* The command asserts reset; status reads the active-low wire. */
        s->reset_n &= ~value;
        break;
    case 0x14:
        s->reset_n |= value;
        break;
    case 0x18:
        s->div_wdt = value;
        break;
    case 0x1c:
        /* RESET_PULSE is a command.  Only the two CPU/subsystem reset lines
         * have a modeled downstream consumer; accepting other bits would
         * falsely claim that unmodeled peripheral domains were reset. */
        if (value & ~BTC_AON_RESET_MASK) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        if (value && s->set_reset) {
            s->set_reset(s->reset_opaque, true, s->vtor);
            s->set_reset(s->reset_opaque, false, s->vtor);
        }
        break;
    case 0x24:
        /* The MAC supplies the handshake. PSC may independently power-gate
         * the processor; CMU's local CPU/RAM power controls remain absent.
         * Accept read-only sync carried by an ordinary guest RMW. */
        if ((value & ~(BTC_SLEEP_CFG_CONTROL_MASK | BTC_DEEPSLEEP_START |
                       BTC_SLEEP_SYNC)) ||
            ((value & BTC_DEEPSLEEP_START) &&
             (!(value & BTC_DEEPSLEEP_GATE) || !s->mac_sleep_ready ||
              s->wakeup_mask || s->wakeup_mask_ext))) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        /* START self-clear is model policy. The guest WFI performs the
         * architectural wait; the command never resets or patches the CPU. */
        s->sleep_config = value & BTC_SLEEP_CFG_CONTROL_MASK;
        best1700_btc_cmu_update_irq(s);
        if ((value & BTC_DEEPSLEEP_START) && s->request_sleep) {
            s->request_sleep(s->sleep_opaque);
        }
        break;
    case 0x2c:
        /* Bus-monitor IRQ and target-operation bits need a bus monitor.
         * They are not plain storage or completion flags. */
        if (value & 7) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        s->cpu_cfg = value & BTC_CPU_CFG_CONTROL_MASK;
        break;
    case 0x34:
        /* RF memory timing only. Sleep/deep-sleep/shutdown bits 8..13
         * need power-state semantics and must not become passive latches. */
        if (value & ~BTC_RF_MEM_TIMING_MASK) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        s->rf_mem_timing = value;
        break;
    case 0x38:
        s->clock_select = value & 0x1fffffffu;
        break;
    case 0x3c:
        s->wakeup_mask = value;
        break;
    case 0x40:
        if (value & ~BTC_WRITE_UNLOCK) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        s->write_unlocked = !!(value & BTC_WRITE_UNLOCK);
        break;
    case 0x44:
        s->clk_out = value;
        break;
    case 0x58:
        s->mresetn_mode = value;
        break;
    case 0x5c:
        s->mresetn_rxtx = value;
        break;
    case 0x60:
        s->mresetn_state |= value;
        break;
    case 0x64:
        s->mresetn_state &= ~value;
        break;
    case 0x68:
        s->mclk_enable |= value;
        break;
    case 0x6c:
        s->mclk_enable &= ~value;
        break;
    case 0x70:
        s->mresetn_mode2 = value;
        break;
    case 0x74:
        s->mresetn_rxtx2 = value;
        break;
    case 0x78:
        s->mresetn_state2 |= value;
        break;
    case 0x7c:
        s->mresetn_state2 &= ~value;
        break;
    case 0x80:
        s->mclk_enable2 |= value;
        break;
    case 0x84:
        s->mclk_enable2 &= ~value;
        break;
    case 0x88:
        s->mclk_mode2 = value;
        break;
    case 0x48:
        return best1700_btc_cmu_calibration_start(s, value);
    case 0x4c:
        if (value != 1) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        /* CPU_IDLE_FLAG precedes the guest WFI instruction. Retain only
         * the established request, without changing CPU or IRQ state. */
        s->idle_request = 1;
        break;
    case 0xcc:
        if (value & ~BTC_PVT_CFG_MASK) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        s->pvt_cfg = value;
        if ((value & (BTC_PVT_RO_EN | BTC_PVT_COUNT_START_EN |
                      BTC_PVT_OSC_CLK_EN | BTC_PVT_RSTN)) ==
            (BTC_PVT_RO_EN | BTC_PVT_COUNT_START_EN |
             BTC_PVT_OSC_CLK_EN | BTC_PVT_RSTN)) {
            return best1700_btc_cmu_pvt_start(s);
        }
        if (!(value & BTC_PVT_RSTN)) {
            timer_del(s->pvt_timer);
            s->pvt_busy = false;
            s->pvt_count = 0;
        }
        break;
    case 0xd0:
        /* PVT_COUNT is a hardware status register.  Firmware only reads it;
         * reject writes instead of creating a guest-controlled DONE flag. */
        return best1700_btc_cmu_unsupported(offset, true, value);
    case 0x90:
    case 0xbc:
    case 0xc8:
        return best1700_btc_cmu_unsupported(offset, true, value);
    case 0xc0:
        if (value & ~0x3ffu) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        s->wifi_irq_mask |= value;
        break;
    case 0xd4:
        s->ext_clocks |= value;
        break;
    case 0xd8:
        s->ext_clocks &= ~value;
        break;
    case 0xdc:
        s->ext_clock_mode = value;
        break;
    case 0xe0:
        s->ext_reset_n |= value;
        break;
    case 0xe4:
        s->ext_reset_n &= ~value;
        break;
    case 0xe8:
        return best1700_btc_cmu_unsupported(offset, true, value);
    case 0x8c:
        if (value & ~BTC_WAKEUP_MASK_EXT) {
            return best1700_btc_cmu_unsupported(offset, true, value);
        }
        s->wakeup_mask_ext = value;
        break;
    case 0xa8:
        s->sel_sys_gt = value & 7;
        break;
    default:
        /* This includes local RESET_PULSE: its downstream reset domains
         * are not established. AON CPU/subsystem pulses are handled below. */
        return best1700_btc_cmu_unsupported(offset, true, value);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps best1700_btc_cmu_ops = {
    .read_with_attrs = best1700_btc_cmu_read,
    .write_with_attrs = best1700_btc_cmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

bool best1700_btc_cmu_cpu_held(const Best1700BtcCmu *s)
{
    return (s->aon_reset_asserted & BTC_AON_RESET_MASK) != 0;
}

static void best1700_btc_cmu_notify_reset(Best1700BtcCmu *s)
{
    if (s->set_reset) {
        s->set_reset(s->reset_opaque, best1700_btc_cmu_cpu_held(s), s->vtor);
    }
}

static uint32_t best1700_btc_cmu_aon_read(void *opaque, hwaddr offset,
                                        uint32_t *value)
{
    Best1700BtcCmu *s = opaque;

    switch (offset) {
    case 0x168:
        *value = s->vtor;
        return BTC_VTOR_MASK;
    case 0xc4:
        *value = 0;
        return BTC_AON_RESET_MASK;
    case 0xc8:
    case 0xcc:
        *value = ~s->aon_reset_asserted;
        return BTC_AON_RESET_MASK;
    default:
        return 0;
    }
}

static uint32_t best1700_btc_cmu_aon_write(void *opaque, hwaddr offset,
                                         uint32_t value, uint32_t lane_mask)
{
    Best1700BtcCmu *s = opaque;
    bool was_held = best1700_btc_cmu_cpu_held(s);
    uint32_t mask;

    switch (offset) {
    case 0x168:
        mask = lane_mask & BTC_VTOR_MASK;
        s->vtor = (s->vtor & ~mask) | (value & mask);
        return BTC_VTOR_MASK;
    case 0xc4:
        if ((value & lane_mask & BTC_AON_RESET_MASK) && s->set_reset) {
            /* Consumers that defer vCPU work must remember the assertion
             * even when the release notification follows immediately. */
            s->set_reset(s->reset_opaque, true, s->vtor);
            best1700_btc_cmu_notify_reset(s);
        }
        return BTC_AON_RESET_MASK;
    case 0xc8:
        s->aon_reset_asserted |= value & lane_mask & BTC_AON_RESET_MASK;
        break;
    case 0xcc:
        s->aon_reset_asserted &= ~(value & lane_mask & BTC_AON_RESET_MASK);
        break;
    default:
        return 0;
    }
    if (was_held != best1700_btc_cmu_cpu_held(s)) {
        best1700_btc_cmu_notify_reset(s);
    }
    return BTC_AON_RESET_MASK;
}

Best1700AonEndpoint best1700_btc_cmu_endpoint(Best1700BtcCmu *s)
{
    return (Best1700AonEndpoint) {
        .opaque = s,
        .read = best1700_btc_cmu_aon_read,
        .write = best1700_btc_cmu_aon_write,
    };
}

void best1700_btc_cmu_init(Best1700BtcCmu *s, Object *owner,
                         uint32_t reference_hz, uint32_t lpo_hz)
{
    s->reference_hz = reference_hz;
    s->lpo_hz = lpo_hz;
    s->calibration_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       best1700_btc_cmu_calibration_complete, s);
    s->pvt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                best1700_btc_cmu_pvt_complete, s);
    memory_region_init_io(&s->iomem, owner, &best1700_btc_cmu_ops, s,
                          "best1700.btc-cmu", BEST1700_BTC_CMU_WINDOW);
    best1700_btc_cmu_reset(s);
}

void best1700_btc_cmu_set_irq(Best1700BtcCmu *s, qemu_irq irq)
{
    s->calibration_irq = irq;
    best1700_btc_cmu_update_irq(s);
}

void best1700_btc_cmu_destroy(Best1700BtcCmu *s)
{
    if (s->calibration_timer) {
        timer_del(s->calibration_timer);
        timer_free(s->calibration_timer);
        s->calibration_timer = NULL;
    }
    if (s->pvt_timer) {
        timer_del(s->pvt_timer);
        timer_free(s->pvt_timer);
        s->pvt_timer = NULL;
    }
}

void best1700_btc_cmu_reset(Best1700BtcCmu *s)
{
    /* Conservative model defaults, not measured silicon power-on values.
     * Peripheral gate bit counts are not established: retain all command
     * bits without assigning unidentified gates to invented devices. */
    s->clocks = 0;
    s->reset_n = 0;
    s->clock_mode = 0;
    s->mode_mclk = 0;
    s->target_addr = 0;
    s->ram_timing = 0;
    s->write_unlocked = false;
    s->div_timer = 0;
    s->div_wdt = 0;
    s->sleep_config = 0;
    s->mac_sleep_ready = false;
    s->cpu_cfg = 0;
    s->rf_mem_timing = 0;
    s->clock_select = 0;
    s->wakeup_mask = 0;
    s->wakeup_mask_ext = 0;
    s->idle_request = 0;
    s->clk_out = 0;
    s->sel_sys_gt = 0;
    s->mresetn_mode = 0;
    s->mresetn_rxtx = 0;
    s->mresetn_state = 0;
    s->mclk_enable = 0;
    s->mresetn_mode2 = 0;
    s->mresetn_rxtx2 = 0;
    s->mresetn_state2 = 0;
    s->mclk_enable2 = 0;
    s->mclk_mode2 = 0;
    s->irq_masked_ext = 0;
    s->bt_pc = 0;
    s->wifi_irq_mask = 0;
    s->irq_masked = 0;
    s->ext_clocks = 0;
    s->ext_clock_mode = 0;
    s->ext_reset_n = 0;
    s->pvt_cfg = 0;
    s->pvt_count = 0;
    s->pvt_busy = false;
    if (s->calibration_timer) {
        timer_del(s->calibration_timer);
    }
    s->calibration_count = 0;
    s->calibration_result = 0;
    s->calibration_busy = false;
    s->calibration_pending = false;
    best1700_btc_cmu_update_irq(s);
    s->aon_reset_asserted = BTC_AON_RESET_MASK;
    s->vtor = 0;
    best1700_btc_cmu_notify_reset(s);
}

void best1700_btc_cmu_connect_reset(Best1700BtcCmu *s, void *opaque,
    void (*set_reset)(void *opaque, bool asserted, uint32_t vtor))
{
    s->reset_opaque = opaque;
    s->set_reset = set_reset;
}

void best1700_btc_cmu_set_mac_sleep(void *opaque, bool ready)
{
    Best1700BtcCmu *s = opaque;

    s->mac_sleep_ready = ready;
}

void best1700_btc_cmu_connect_sleep(Best1700BtcCmu *s, void *opaque,
                                   void (*request)(void *opaque))
{
    s->sleep_opaque = opaque;
    s->request_sleep = request;
}

void best1700_btc_cmu_set_pc(Best1700BtcCmu *s, const uint32_t *pc)
{
    s->cpu_pc = pc;
}
