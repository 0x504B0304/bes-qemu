/*
 * BEST1700 Bluetooth baseband configuration, clocks and target-3 timer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "best1700_btc_baseband.h"

#define CORE_RESET (1u << 31)
#define CORE_SOFTWARE_REQUEST (1u << 27)
#define CORE_SOFTWARE_IRQ (1u << 3)
#define CORE_IRQ_FIELDS 0x000080ffu
#define BT_CORE_CONFIG_FIELDS 0x00080f3fu
#define BT_CORE_VERSION 0x0b000600u
#define BLE_CORE_VERSION 0x0b001200u
#define BT_IRQ_ENABLE_FIELDS 0x01df0017u
#define BLE_CORE_CONFIG_FIELDS 0x0010070fu
#define BLE_ADVERT_ABORT (1u << 25)
#define BLE_SCAN_ABORT (1u << 24)
#define BLE_IDLE_ABORT_COMMANDS (BLE_ADVERT_ABORT | BLE_SCAN_ABORT)
#define EVENT_TABLE_ENTRIES 16
#define EVENT_TABLE_STRIDE 16
#define EVENT_STATUS_MASK 0x38u
#define EVENT_STATUS_IDLE 0x18u
#define BLE_IRQ_ENABLE_FIELDS 0x000101ffu
#define BLE_SEED_INIT (1u << 31)
#define BLE_SEED_VALUE 0x003fffffu
#define CLOCK_UPDATE (1u << 30)
#define CLOCK_SAMPLE (1u << 31)
#define ISO_UPDATE (1u << 30)
#define ISO_SAMPLE (1u << 31)
#define ISO_MODE_MASK 0x00000007u
#define SLOT_MASK 0x0fffffffu
#define SPI_CONFIG_FIELDS 0x3fff00b0u
#define SPI_COMPLETE (1u << 1)
#define HALF_US_NS 500
#define HALF_SLOT_HUS 625
#define CLOCK_PERIOD_HUS (((uint64_t)SLOT_MASK + 1) * HALF_SLOT_HUS)
#define ISO_PERIOD_HUS (1ULL << 33)
#define SLEEP_CONFIG 0x80000003u
#define EXTERNAL_WAKE_DISABLED (1u << 31)
#define SLEEP_START (1u << 2)
#define CORRECTION_START (1u << 3)
#define SLEEP_STATUS (1u << 15)
#define CORRECTION_DELTA (1u << 31)
#define SLEEP_IRQ (1u << 1)
#define CORRECTION_IRQ (1u << 0)
#define CNTL1_ROUTE0_CLEAR (1u << 4)
#define CNTL1_ROUTE1_CLEAR (1u << 8)
#define CNTL1_ROUTE2_CLEAR (1u << 9)
#define CNTL1_ROUTE3_CLEAR (1u << 10)
#define CNTL1_ROUTE4_CLEAR (1u << 18)
#define CNTL1_ROUTE5_CLEAR (1u << 19)
#define CNTL1_SETTRIG (1u << 7)
#define CNTL1_AFH_MONITOR_START (1u << 11)
#define CNTL1_TIME_IRQ_CLEAR (1u << 12)
#define CNTL1_SYNC_COUNT_CLEAR (1u << 13)
#define CNTL1_FA_TIMEOUT_CLEAR (1u << 1)
#define CNTL1_BTCLK_SET (1u << 2)
#define CNTL1_RX_FAOK_CLEAR (1u << 3)
#define CNTL1_SWAGC_BT_VALID (1u << 5)
#define CNTL1_SWAGC_BLE_VALID (1u << 6)
#define C7C_ROUTE0_ENABLE (1u << 0)
#define C7C_ACL_TRIGGER (1u << 3)
#define C7C_SCO_TRIGGER (1u << 4)
#define C7C_TRIP_DELAY_MASK (0x1fu << 8)
#define C7C_TRIGGER_MODE_MASK (0x3u << 13)
#define C7C_RXBIT_SELECT (1u << 15)
#define C7C_LINK_ID_MASK (0xfu << 16)
#define C7C_KNOWN_MASK (C7C_ROUTE0_ENABLE | C7C_ACL_TRIGGER | C7C_SCO_TRIGGER | C7C_TRIP_DELAY_MASK | C7C_TRIGGER_MODE_MASK | C7C_RXBIT_SELECT | C7C_LINK_ID_MASK | 0x00300000u | 0x3f000000u)
#define TRIGGER_STATUS_MASK 0x00001f80u

static uint64_t baseband_elapsed_hus(Best1700BtcBaseband *s)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->clock_epoch_ns;

    return s->sleep_state == BEST1700_BTC_SLEEPING ?
           0 : MAX(elapsed, 0) / HALF_US_NS;
}

static void baseband_sleep_sync(Best1700BtcBaseband *s)
{
    if (s->sleep_sync) {
        s->sleep_sync(s->sleep_sync_opaque,
                      s->sleep_state == BEST1700_BTC_SLEEPING);
    }
}

static void baseband_update_irq(Best1700BtcBaseband *s)
{
    if (s->irq) {
        qemu_set_irq(s->irq, (s->irq_status & s->irq_enable) != 0);
    }
}

static void baseband_refresh_trigger_status(Best1700BtcBaseband *s)
{
    uint32_t flags = 0;

    for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
        if (s->sw_targets[i].enabled && s->sw_targets[i].written == 3 &&
            s->sleep_state == BEST1700_BTC_AWAKE) {
            flags |= 1u << (7 + i);
        }
    }
    s->config[0xe60 / sizeof(uint32_t)] =
        (s->config[0xe60 / sizeof(uint32_t)] & ~TRIGGER_STATUS_MASK) | flags;
}

static void baseband_clear_sw_trigger(Best1700BtcBaseband *s, unsigned index)
{
    if (index >= G_N_ELEMENTS(s->sw_targets)) {
        return;
    }
    timer_del(s->sw_targets[index].timer);
    s->sw_targets[index].enabled = false;
    if (index == 0) {
        s->config[0xc7c / sizeof(uint32_t)] &= ~C7C_ROUTE0_ENABLE;
        s->config[0xc98 / sizeof(uint32_t)] &= ~CORE_RESET;
    }
    baseband_refresh_trigger_status(s);
}

static void baseband_mac_context_restore(Best1700BtcBaseband *s)
{
    /* The warm restore path replays the retained register image, enables the
     * BT core and then pulses +d00 before restoring interrupt enables. Treat
     * the pulse as a MAC transient-state boundary: stale status, aborts and
     * scheduled trigger work belong to the previous context and must not
     * leak into the restored one. Configuration, clocks, seeds and guest
     * exchange memory are retained. The LPO wake and clock-correction
     * events belong to the retained sleep sequencer: clearing them here
     * strands the original warm restore before its two IRQ stages. Preserve
     * only pending events; never recreate an event already ACKed by the guest. */
    s->irq_status &= SLEEP_IRQ | CORRECTION_IRQ;
    s->bt_irq_status = 0;
    s->bt_error = 0;
    s->ble_irq_status = 0;
    s->ble_error = 0;
    timer_del(s->ble_abort_timer);
    s->ble_abort_pending = 0;
    for (unsigned i = 0; i < G_N_ELEMENTS(s->targets); i++) {
        timer_del(s->targets[i].timer);
        s->targets[i].written = 0;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
        timer_del(s->sw_targets[i].timer);
        s->sw_targets[i].written = 0;
        s->sw_targets[i].enabled = false;
    }
    baseband_refresh_trigger_status(s);
    s->mac_restore_count++;
    baseband_update_irq(s);
}

static bool baseband_event_table_idle(Best1700BtcBaseband *s)
{
    uint64_t offset = (uint64_t)s->config[0x02c / 4] * 2;
    uint64_t length = EVENT_TABLE_ENTRIES * EVENT_TABLE_STRIDE;
    const uint8_t *table;

    if (!s->exchange || offset + length > memory_region_size(s->exchange)) {
        return false;
    }
    table = (const uint8_t *)memory_region_get_ram_ptr(s->exchange) + offset;
    for (unsigned i = 0; i < EVENT_TABLE_ENTRIES; i++) {
        /* The original scheduler initializes all entries to status 3.
         * Any other state requires the radio activity engine, including
         * uninitialized entries. Do not infer idle from the enable bit. */
        if ((lduw_le_p(table + i * EVENT_TABLE_STRIDE) & EVENT_STATUS_MASK) !=
            EVENT_STATUS_IDLE) {
            return false;
        }
    }
    return true;
}

static void baseband_idle_abort_complete(void *opaque)
{
    Best1700BtcBaseband *s = opaque;

    /* Recheck shared descriptors at execution, not just at command issue.
     * If a peer queued an activity, leave the request pending instead of
     * inventing an end event for a radio engine we do not yet implement. */
    if (!baseband_event_table_idle(s)) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 BTC BLE abort stopped at non-idle event table\n");
        return;
    }
    s->ble_abort_pending = 0;
    /* An empty event table has no current activity, descriptor to retire,
     * or end-of-activity interrupt. Command completion only self-clears
     * the consumed request; common timers and RAL records are untouched. */
}

static void baseband_target_expired(void *opaque)
{
    Best1700BtcTimestampTarget *target = opaque;
    Best1700BtcBaseband *s = target->owner;

    /* A real virtual-time deadline produces pending independently of the
     * interrupt mask. ACK and mask writes must never rearm this event. */
    s->irq_status |= 1u << (5 + target->index);
    baseband_update_irq(s);
}

static void baseband_program_target(Best1700BtcBaseband *s, unsigned index)
{
    Best1700BtcTimestampTarget *t = &s->targets[index];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = MAX(now - s->clock_epoch_ns, 0);
    uint64_t current = s->native_base_hus + elapsed / HALF_US_NS;
    uint64_t target = (uint64_t)t->slot * HALF_SLOT_HUS +
                      HALF_SLOT_HUS - 1 - t->fine;
    uint64_t distance;

    timer_del(t->timer);
    /* Power-on target defaults and partially initialized pairs are not
     * established. Only explicitly programmed pairs can produce an event. */
    if (t->written != 3 || s->sleep_state != BEST1700_BTC_AWAKE) {
        return;
    }
    distance = (target + CLOCK_PERIOD_HUS - current % CLOCK_PERIOD_HUS) %
               CLOCK_PERIOD_HUS;
    if (distance >= CLOCK_PERIOD_HUS / 2) {
        /* The verified driver uses future targets, at most 900 seconds out.
         * Past/ambiguous targets remain readable but do not invent a missed
         * event or a match one full wrap away. A later field write retries. */
        qemu_log_mask(LOG_UNIMP,
            "best1700 BTC target%u past/ambiguous target is not armed "
            "slot=0x%08x fine=%u\n", index + 1, t->slot, t->fine);
        return;
    }
    /* Align to the clock epoch, not to the programming instruction's phase.
     * An exact current counter match is due now; a passed fine count is not. */
    timer_mod(t->timer,
              now + (distance ? distance * HALF_US_NS -
                                elapsed % HALF_US_NS : 0));
}

static void baseband_program_sw_target(Best1700BtcBaseband *s, unsigned index)
{
    Best1700BtcTimestampTarget *t = &s->sw_targets[index];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = MAX(now - s->clock_epoch_ns, 0);
    uint64_t current = s->native_base_hus + elapsed / HALF_US_NS;
    uint64_t target = (uint64_t)t->slot * HALF_SLOT_HUS +
                      HALF_SLOT_HUS - 1 - t->fine;
    uint64_t distance;

    timer_del(t->timer);
    if (t->written != 3 || !t->enabled ||
        s->sleep_state != BEST1700_BTC_AWAKE) {
        baseband_refresh_trigger_status(s);
        return;
    }
    distance = (target + CLOCK_PERIOD_HUS - current % CLOCK_PERIOD_HUS) %
               CLOCK_PERIOD_HUS;
    if (distance >= CLOCK_PERIOD_HUS / 2) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 BTC software target%u past/ambiguous target is not "
            "armed slot=0x%08x fine=%u\n", index, t->slot, t->fine);
        baseband_refresh_trigger_status(s);
        return;
    }
    timer_mod(t->timer,
              now + (distance ? distance * HALF_US_NS -
                                elapsed % HALF_US_NS : 0));
    baseband_refresh_trigger_status(s);
}

static void baseband_sample_iso(Best1700BtcBaseband *s)
{
    uint64_t elapsed = baseband_elapsed_hus(s);

    s->sampled_iso = (uint32_t)((s->iso_base_hus + elapsed) / 2);
}

static void baseband_rearm_targets(Best1700BtcBaseband *s)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(s->targets); i++) {
        baseband_program_target(s, i);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
        baseband_program_sw_target(s, i);
    }
}

static bool baseband_update_slot_clock(Best1700BtcBaseband *s,
                                       uint32_t requested)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed;
    uint64_t half_us;
    uint64_t iso_hus;

    /* SLOTCLK is a live counter. CLKN_UPD replaces only the slot portion;
     * the fine phase and the independent ISO counter keep running. */
    if (s->sleep_state != BEST1700_BTC_AWAKE) {
        return false;
    }
    elapsed = baseband_elapsed_hus(s);
    half_us = s->native_base_hus + elapsed;
    iso_hus = s->iso_base_hus + elapsed;
    s->native_base_hus = ((uint64_t)(requested & SLOT_MASK) * HALF_SLOT_HUS +
                          half_us % HALF_SLOT_HUS) % CLOCK_PERIOD_HUS;
    s->iso_base_hus = iso_hus % ISO_PERIOD_HUS;
    s->clock_epoch_ns = now;
    baseband_rearm_targets(s);
    return true;
}

static bool baseband_update_iso_clock(Best1700BtcBaseband *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed;
    uint64_t delta;

    /* ISOCNTCORR is expressed in microseconds and ISOINTCORR_HUS supplies
     * the optional half-microsecond remainder. Apply the correction at the
     * ISO_UPD command boundary while preserving the BT slot clock. */
    if (s->sleep_state != BEST1700_BTC_AWAKE) {
        return false;
    }
    elapsed = baseband_elapsed_hus(s);
    delta = (uint64_t)s->correction_iso_us * 2 +
            (s->correction_iso_hus & 1u);
    s->native_base_hus = (s->native_base_hus + elapsed) % CLOCK_PERIOD_HUS;
    s->iso_base_hus = (s->iso_base_hus + elapsed + delta) % ISO_PERIOD_HUS;
    s->clock_epoch_ns = now;
    baseband_rearm_targets(s);
    return true;
}

static void baseband_sample_clock(Best1700BtcBaseband *s)
{
    uint64_t elapsed = baseband_elapsed_hus(s);
    uint64_t half_us = s->native_base_hus + elapsed;

    /* One half-slot is 625 half-microseconds. All three registers latch
     * together; subsequent reads must not sample time independently. */
    s->sampled_slot = (half_us / 625) & SLOT_MASK;
    s->sampled_fine = 624 - half_us % 625;
    baseband_sample_iso(s);
}

static void baseband_capture_now(Best1700BtcBaseband *s, unsigned index)
{
    uint64_t half_us = s->native_base_hus + baseband_elapsed_hus(s);

    s->capture_clkn[index] = (half_us / HALF_SLOT_HUS) & SLOT_MASK;
    s->capture_fine[index] = 624 - half_us % HALF_SLOT_HUS;
}

static void baseband_finish_sleep(Best1700BtcBaseband *s, uint32_t cycles)
{
    timer_del(s->sleep_timer);
    s->sleep_elapsed = cycles;
    /* Resume the reference clock at wake by model policy. The later delta
     * correction adds only the missing LPO interval, not ISR execution time. */
    s->clock_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->sleep_state = BEST1700_BTC_NEEDS_CORRECTION;
    baseband_refresh_trigger_status(s);
    baseband_sleep_sync(s);
    s->irq_status |= SLEEP_IRQ;
    baseband_update_irq(s);
}

static void baseband_sleep_expired(void *opaque)
{
    Best1700BtcBaseband *s = opaque;

    baseband_finish_sleep(s, s->sleep_cycles);
}

void best1700_btc_baseband_set_external_wake(Best1700BtcBaseband *s, bool level)
{
    s->external_wake = level;
    if (level && s->sleep_state == BEST1700_BTC_SLEEPING &&
        !(s->sleep_control & EXTERNAL_WAKE_DISABLED)) {
        int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                          s->sleep_epoch_ns;
        uint64_t cycles = muldiv64(MAX(elapsed, 0), s->lpo_hz,
                                   NANOSECONDS_PER_SECOND);

        /* An early wake counts only completed LPO ticks. The guest's
         * programmed deadline and its IPC request remain untouched. */
        baseband_finish_sleep(s, MIN(cycles, s->sleep_cycles));
    }
}

static void baseband_correction_expired(void *opaque)
{
    Best1700BtcBaseband *s = opaque;
    uint64_t elapsed = baseband_elapsed_hus(s);
    uint64_t delta = (uint64_t)(s->correction_slot & SLOT_MASK) * HALF_SLOT_HUS +
                     HALF_SLOT_HUS - 1 - s->correction_fine;

    s->native_base_hus = (s->native_base_hus + elapsed + delta) % CLOCK_PERIOD_HUS;
    s->iso_base_hus = (s->iso_base_hus + elapsed) % ISO_PERIOD_HUS;
    if (s->iso_control & 1) {
        delta = (uint64_t)s->correction_iso_us * 2 + s->correction_iso_hus;
        s->iso_base_hus = (s->iso_base_hus + delta) % ISO_PERIOD_HUS;
    }
    /* Apply an integer counter delta without moving its running tick phase. */
    s->clock_epoch_ns += elapsed * HALF_US_NS;
    s->sleep_state = BEST1700_BTC_AWAKE;
    s->correction_written = 0;
    baseband_sleep_sync(s);
    for (unsigned i = 0; i < G_N_ELEMENTS(s->targets); i++) {
        baseband_program_target(s, i);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
        baseband_program_sw_target(s, i);
    }
    s->irq_status |= CORRECTION_IRQ;
    qemu_log_mask(CPU_LOG_INT, "best1700 BTC correction done ns=%" PRId64
                  " irq=0x%x mask=0x%x\n",
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                  s->irq_status, s->irq_enable);
    baseband_update_irq(s);
}

/* These are configuration fields, never a fallback for unknown MMIO.
 * The event-write address/data registers only configure future radio events;
 * programming them does not issue a transaction to the configured target. */
/* Index only the immutable register metadata. Zero-mask holes remain
 * unsupported; dynamic register behavior stays in the MMIO handlers. */
static const struct {
    uint32_t mask;
    uint32_t reset;
} baseband_config[BEST1700_BTC_BASEBAND_SIZE / sizeof(uint32_t)] = {
    [0x00c / sizeof(uint32_t)] = { 0x00010000, 0 }, /* Error IRQ mask; no error source modeled */
    [0x02c / sizeof(uint32_t)] = { 0x00003fff, 0 }, /* Event-table pointer; no scheduler attached */
    [0x03c / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* External/oscillator/radio wake-up presets */
    [0x050 / sizeof(uint32_t)] = { 0xbfbfbfbf, 0 }, /* Four diagnostic selectors and output enables */
    [0x064 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Software profiling: four byte groups */
    [0x070 / sizeof(uint32_t)] = { SPI_CONFIG_FIELDS, 0 }, /* SPI pointer, format and clock only */
    [0x074 / sizeof(uint32_t)] = { 0xfffff3ff, 0 }, /* AGC/IQ, RF polarity and interface selection */
    [0x0e0 / sizeof(uint32_t)] = { 0x03ff01ff, 0 }, /* Event prefetch-abort/prefetch lead times */
    /* Core enable stores the requested state. There is no radio scheduler
     * attached, so enabling does not create ready, packet or FIFO events. */
    [0x400 / sizeof(uint32_t)] = { BT_CORE_CONFIG_FIELDS, 0 }, /* Window, enable, coexistence, LMP flow */
    [0x40c / sizeof(uint32_t)] = { BT_IRQ_ENABLE_FIELDS, 0 }, /* Start/end/skip, RX, extension IRQ masks */
    [0x428 / sizeof(uint32_t)] = { 0x00003fff, 0 }, /* Current BT RX descriptor pointer only */
    [0x450 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Four 7-bit diagnostic selectors + enables */
    [0x478 / sizeof(uint32_t)] = { 0x000f3fff, 0 }, /* BT sync-error allowance and frequency pointer */
    [0x47c / sizeof(uint32_t)] = { 0x3f003f00, 0 }, /* BT TX/RX rate selection */
    [0x48c / sizeof(uint32_t)] = { 0x00ff7fff, 0 }, /* BT RX/TX power-up and TX power-down delays */
    [0x490 / sizeof(uint32_t)] = { 0xff007f7f, 0 }, /* BT sync position and TX/RX path delays */
    [0x534 / sizeof(uint32_t)] = { 0x0000003f, 0 }, /* EDR RX guard timeout only */
    [0x540 / sizeof(uint32_t)] = { 0x07ff00f0, 0 }, /* PCA target offset/step; starts unsupported */
    [0x544 / sizeof(uint32_t)] = { 0x00ff07ff, 0 }, /* PCA interval/shift operand; trigger 12 absent */
    [0x550 / sizeof(uint32_t)] = { 0x00000004, 0 }, /* MWS coexistence requested enable only */
    [0x564 / sizeof(uint32_t)] = { 0x000f0f00, 0 }, /* BTM10 and BTM12 priority nibbles */
    [0x568 / sizeof(uint32_t)] = { 0x00000ff0, 0 }, /* BTM17 and BTM18 priority nibbles */
    [0x800 / sizeof(uint32_t)] = { BLE_CORE_CONFIG_FIELDS, 0 }, /* Window, enable, filters, MD_DSB */
    [0x80c / sizeof(uint32_t)] = { BLE_IRQ_ENABLE_FIELDS, 0 }, /* Event/error enables; bit 8 unnamed */
    [0x828 / sizeof(uint32_t)] = { 0x00003fff, 0 }, /* Current BLE RX descriptor pointer only */
    [0x850 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Four 7-bit diagnostic selectors + enables */
    [0x878 / sizeof(uint32_t)] = { 0x1fc03fff, 0 }, /* BLE frequency pointer, PHY mask, LR flush */
    [0x87c / sizeof(uint32_t)] = { 0xff77ff03, 0xe440e400 }, /* BLE RADIOCNTL3 rate/valid/RSSI fields */
    [0x880 / sizeof(uint32_t)] = { 0xffff7fff, 0 }, /* BLE PHY 0 power timing and sync position */
    [0x884 / sizeof(uint32_t)] = { 0xffff7fff, 0 }, /* BLE PHY 1 power timing and sync position */
    [0x888 / sizeof(uint32_t)] = { 0xffff7fff, 0 }, /* BLE PHY 2 power timing and sync position */
    [0x88c / sizeof(uint32_t)] = { 0x00007fff, 0 }, /* BLE PHY 3 TX power-up/down timing */
    [0x890 / sizeof(uint32_t)] = { 0x007f7f7f, 0 }, /* BLE PHY 0 TX/RX path delays */
    [0x894 / sizeof(uint32_t)] = { 0x007f7f7f, 0 }, /* BLE PHY 1 TX/RX path delays */
    [0x898 / sizeof(uint32_t)] = { 0xffffff7f, 0 }, /* BLE PHY 2 path and RX flush timing */
    [0x89c / sizeof(uint32_t)] = { 0xff7f007f, 0 }, /* BLE PHY 3 path and RX flush timing */
    [0x930 / sizeof(uint32_t)] = { 0xffff3fff, 0 }, /* Advertising interval and AUXPTR thresholds */
    [0x940 / sizeof(uint32_t)] = { 0x00ff3fff, 0 }, /* Address-list entry count and base pointer */
    [0x944 / sizeof(uint32_t)] = { 0x00003fff, 0 }, /* Current list pointer: low 14-bit subset */
    [0x948 / sizeof(uint32_t)] = { 0x0000003f, 0 }, /* List-search timeout setting, no search */
    [0x970 / sizeof(uint32_t)] = { 0x00ff3fff, 0 }, /* Resolving-list count + low 14-bit base subset */
    [0x974 / sizeof(uint32_t)] = { 0x00003fff, 0 }, /* Current resolving pointer: low 14-bit subset */
    [0x980 / sizeof(uint32_t)] = { 0xffff00ff, 0 }, /* DFCNTL0_1US: RX sample/switch, TX switch */
    [0x984 / sizeof(uint32_t)] = { 0xffff00ff, 0 }, /* DFCNTL0_2US: RX sample/switch, TX switch */
    [0x988 / sizeof(uint32_t)] = { 0xffff00ff, 0 }, /* DFCNTL1_1US: RX sample/switch, TX switch */
    [0x98c / sizeof(uint32_t)] = { 0xffff00ff, 0 }, /* DFCNTL1_2US: RX sample/switch, TX switch */
    [0x994 / sizeof(uint32_t)] = { 0x0000ffff, 0 }, /* DF primary TX/RX antenna IDs and count enables */
    [0x9d0 / sizeof(uint32_t)] = { 0x000000ff, 0 }, /* ISO IRQ masks; no ISO compare engine */
    [0x9e0 / sizeof(uint32_t)] = { 0x000000ff, 0 }, /* ISO GPIO setting only; no pin/timer effects */
    [0xc04 / sizeof(uint32_t)] = { 0xffffff3f, 0 }, /* Event enables, radio gain/power selection */
    [0xc0c / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Fast-ack radio timing */
    [0xc10 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Fast-ack gain, power and ECC timing */
    [0xc14 / sizeof(uint32_t)] = { 0xffffe07f, 0 }, /* SBC selection and ramp-down count */
    [0xc18 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Fast-ack, PCM and hardware SPI selection */
    [0xc2c / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* AGC guard and time-interrupt configuration */
    [0xc38 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Ten AGC mask triplets + BT/WiFi coexistence */
    [0xc3c / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* BLE AGC receive indices */
    [0xc74 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Unnamed retained word; width, not silicon mask */
    [0xc7c / sizeof(uint32_t)] = { C7C_KNOWN_MASK, 0 }, /* Playback trigger and retained high-mode fields */
    [0xc80 / sizeof(uint32_t)] = { 0xffffcfff, 1 }, /* Trigger/FA mode configuration; bit 0 resets set */
    [0xc84 / sizeof(uint32_t)] = { 0x0fffffff, 0 }, /* TWS synchronization clock, low 28 bits */
    [0xc88 / sizeof(uint32_t)] = { 0xffffffff, 0x00003b8f }, /* PCM enhanced mode/control */
    [0xc8c / sizeof(uint32_t)] = { 0xffffff3f, 0x57fc0134 }, /* Fast-ack mode, guard and ECC */
    [0xc90 / sizeof(uint32_t)] = { 0xffffffff, 0x1a1a0000 }, /* Fast-ack receive window and ECC */
    [0xc9c / sizeof(uint32_t)] = { 0x00ffffff, 0 }, /* PCM private-data format flags */
    [0xca0 / sizeof(uint32_t)] = { 0x000000ff, 0 }, /* PCM private-data format extension */
    [0xca4 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* TX-event write target */
    [0xca8 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* RX-event write target */
    [0xcac / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Positive TX-event write data */
    [0xcb4 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Positive RX-event write data */
    [0xcbc / sizeof(uint32_t)] = { 0xbf80ffff, 0x0000000a }, /* Enhanced PCM and AFH receive gain */
    [0xcc0 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Unnamed retained word; width, not silicon mask */
    [0xcc4 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Fast-ack software register 0 */
    [0xcc8 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Fast-ack software register 1 */
    [0xccc / sizeof(uint32_t)] = { 0x00ff7fff, 0 }, /* Unnamed retained fields, no operation modeled */
    [0xd04 / sizeof(uint32_t)] = { 0x7fcffcff, 0 }, /* relay and RF SPI access counters */
    [0xd54 / sizeof(uint32_t)] = { 0x3f7f7f0f, 0 }, /* AFH window count, spacing, size, priority */
    [0xd78 / sizeof(uint32_t)] = { 0x57ff007f, 0 }, /* BLE timing and reset/trigger IRQ enables */
    [0xd90 / sizeof(uint32_t)] = { 0x1fffffff, 0 }, /* FEC threshold and coexistence configuration */
    [0xe68 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* Four BESTRX byte configuration fields */
    [0xe6c / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* BESTRX opaque word; width, not silicon mask */
    [0xe74 / sizeof(uint32_t)] = { 0x00130000, 0 }, /* Relay TX/RX count and 4M enable settings */
    [0x614 / sizeof(uint32_t)] = { 0x0080ffff, 0 }, /* eSCO mute pattern and control */
    [0x618 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* eSCO current TX pointer */
    [0x624 / sizeof(uint32_t)] = { 0x0000ffff, 0 }, /* eSCO length and trigger control */
    [0x634 / sizeof(uint32_t)] = { 0x0080ffff, 0 }, /* eSCO mute pattern and control */
    [0x670 / sizeof(uint32_t)] = { 0x00000080, 0 }, /* audio path software-CVSD control */
    [0x674 / sizeof(uint32_t)] = { 0x00000080, 0 }, /* audio path software-CVSD control */
    [0x688 / sizeof(uint32_t)] = { 0x80ff01ff, 0 }, /* PCM physical clock value/limit/invert */
    [0xc00 / sizeof(uint32_t)] = { 0xffffffff, 0 }, /* BTC test-mode latch */
    [0xc4c / sizeof(uint32_t)] = { 0x7f7f7f7f, 0 }, /* four GPIO terminal-count selectors */
    [0xc50 / sizeof(uint32_t)] = { 0x9f1f1f1f, 0 }, /* DMA terminal-count selectors + enable */
    [0xc60 / sizeof(uint32_t)] = { 0x00ffff7f, 0 }, /* 4M TX path timing */
    [0xc64 / sizeof(uint32_t)] = { 0x00ffff7f, 0 }, /* 4M RX path timing */
    [0xc68 / sizeof(uint32_t)] = { 0x007fff7f, 0 }, /* 2M TX path timing */
    [0xc6c / sizeof(uint32_t)] = { 0x00ffff7f, 0 }, /* 2M RX path timing */
    [0xd08 / sizeof(uint32_t)] = { 0x0fffffff, 0 }, /* software trigger route 1 clock */
    [0xd0c / sizeof(uint32_t)] = { 0x800003ff, 0 }, /* software trigger route 1 fine/enable */
    [0xd10 / sizeof(uint32_t)] = { 0x0fffffff, 0 }, /* software trigger route 2 clock */
    [0xd14 / sizeof(uint32_t)] = { 0x800003ff, 0 }, /* software trigger route 2 fine/enable */
    [0xd18 / sizeof(uint32_t)] = { 0x0fffffff, 0 }, /* software trigger route 3 clock */
    [0xd1c / sizeof(uint32_t)] = { 0x800003ff, 0 }, /* software trigger route 3 fine/enable */
    [0xd70 / sizeof(uint32_t)] = { 0x000002f0, 0 }, /* four terminal-count capture mode bits */
    [0xd28 / sizeof(uint32_t)] = { 0x80000000, 0 }, /* software-trigger sync interrupt latch */
    [0xd7c / sizeof(uint32_t)] = { 0x03ffffff, 0 }, /* BLE 4M RX timing */
    [0xd80 / sizeof(uint32_t)] = { 0x0fffffff, 0 }, /* software trigger route 4 clock */
    [0xd84 / sizeof(uint32_t)] = { 0x800003ff, 0 }, /* software trigger route 4 fine/enable */
    [0xd88 / sizeof(uint32_t)] = { 0x0fffffff, 0 }, /* software trigger route 5 clock */
    [0xd8c / sizeof(uint32_t)] = { 0x800003ff, 0 }, /* software trigger route 5 fine/enable */
    [0xe60 / sizeof(uint32_t)] = { 0x001fff80, 0 }, /* trigger capture format and route flags */
    [0xc94 / sizeof(uint32_t)] = { 0x0fffffff, 0 }, /* software trigger route 0 clock */
    [0xc98 / sizeof(uint32_t)] = { 0x800003ff, 0 }, /* software trigger route 0 fine/enable */
};

static uint32_t baseband_config_mask(hwaddr offset)
{
    if (offset >= BEST1700_BTC_BASEBAND_SIZE || offset % sizeof(uint32_t)) {
        return 0;
    }
    return baseband_config[offset / sizeof(uint32_t)].mask;
}

static MemTxResult baseband_unsupported(hwaddr offset, bool write,
                                         uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 BTC baseband unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 "\n", write ? "write" : "read", offset, value);
    return MEMTX_ERROR;
}

static MemTxResult baseband_sleep_write(Best1700BtcBaseband *s, uint64_t value)
{
    uint32_t commands = value & (SLEEP_START | CORRECTION_START);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t duration, elapsed;

    /* Hardware status may be carried through a guest RMW, never created by
     * that write. A configuration restore does not reissue sleep. */
    if (value & ~(SLEEP_CONFIG | SLEEP_START | CORRECTION_START | SLEEP_STATUS)) {
        return baseband_unsupported(0x30, true, value);
    }
    if (commands == (SLEEP_START | CORRECTION_START)) {
        return baseband_unsupported(0x30, true, value);
    }
    if (commands == SLEEP_START) {
        if (s->sleep_state != BEST1700_BTC_AWAKE || !s->sleep_cycles ||
            !s->lpo_hz || (value & 3) != 3) {
            return baseband_unsupported(0x30, true, value);
        }
        duration = ((uint64_t)s->sleep_cycles * NANOSECONDS_PER_SECOND +
                    s->lpo_hz - 1) / s->lpo_hz;
        if (now > INT64_MAX - duration) {
            return baseband_unsupported(0x30, true, value);
        }
        elapsed = baseband_elapsed_hus(s);
        s->native_base_hus = (s->native_base_hus + elapsed) % CLOCK_PERIOD_HUS;
        s->iso_base_hus = (s->iso_base_hus + elapsed) % ISO_PERIOD_HUS;
        s->clock_epoch_ns = now;
        s->sleep_epoch_ns = now;
        s->sleep_elapsed = 0;
        s->correction_written = 0;
        s->sleep_state = BEST1700_BTC_SLEEPING;
        baseband_refresh_trigger_status(s);
        for (unsigned i = 0; i < G_N_ELEMENTS(s->targets); i++) {
            timer_del(s->targets[i].timer);
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
            timer_del(s->sw_targets[i].timer);
        }
        timer_mod_ns(s->sleep_timer, now + duration);
        baseband_sleep_sync(s);
    } else if (commands == CORRECTION_START) {
        unsigned required = (s->iso_control & 1) ? 15 : 3;

        if (s->sleep_state != BEST1700_BTC_NEEDS_CORRECTION ||
            (s->correction_written & required) != required ||
            now > INT64_MAX - s->correction_latency_ns) {
            return baseband_unsupported(0x30, true, value);
        }
        /* Latency is explicit model policy, not measured silicon. MTTCG
         * guest execution can fall behind virtual peripheral time, so the
         * launcher may select a longer completion window. W1C remains final:
         * neither the mask nor an ACK can defer or regenerate completion. */
        s->sleep_state = BEST1700_BTC_CORRECTING;
        timer_mod_ns(s->correction_timer, now + s->correction_latency_ns);
        qemu_log_mask(CPU_LOG_INT, "best1700 BTC correction start ns=%" PRId64
                      " due=%" PRId64 " irq=0x%x mask=0x%x\n",
                      now, now + s->correction_latency_ns,
                      s->irq_status, s->irq_enable);
    } else if (s->sleep_state != BEST1700_BTC_AWAKE &&
               s->sleep_state != BEST1700_BTC_SLEEPING &&
               s->sleep_state != BEST1700_BTC_NEEDS_CORRECTION &&
               (value & SLEEP_CONFIG) != s->sleep_control) {
        return baseband_unsupported(0x30, true, value);
    }
    s->sleep_control = value & SLEEP_CONFIG;
    best1700_btc_baseband_set_external_wake(s, s->external_wake);
    return MEMTX_OK;
}

static bool baseband_zero_restore(hwaddr offset)
{
    switch (offset) {
    case 0x0e4:
    case 0x194: case 0x474: case 0x600: case 0x604: case 0x82c:
    case 0x610: case 0x630: case 0x650: case 0x680: case 0x8d0:
    case 0x934: case 0x990:
    case 0x9a0: case 0x9a4: case 0x9a8: case 0x9ac: case 0x9b0: case 0x9b4:
    case 0x9f0: case 0x9f4: case 0x9f8: case 0x9fc:
    case 0xa00: case 0xa04: case 0xa08: case 0xa0c:
    case 0xc30: case 0xc34: case 0xc40: case 0xc44: case 0xc48:
    case 0xc78:
    case 0xcd0: case 0xcd4: case 0xcd8: case 0xcdc:
    case 0xd2c: case 0xd30: case 0xd34: case 0xd38: case 0xd3c:
    case 0xd40: case 0xd44: case 0xd48: case 0xd4c: case 0xd50:
    case 0xd58: case 0xd5c:
        /* Exact inactive context views. Replaying zero preserves this
         * state without programming an engine, issuing a command, or
         * acknowledging an interrupt. This is not a general RW bank. */
        return true;
    default:
        return false;
    }
}

static bool baseband_capture_offset(hwaddr offset, unsigned *index, bool *fine)
{
    static const hwaddr clkn[] = { 0xe00, 0xe1c, 0xe24, 0xe2c,
                                   0xe34, 0xe3c, 0xe44, 0xe4c };
    static const hwaddr fine_offsets[] = { 0xe08, 0xe20, 0xe28, 0xe30,
                                           0xe38, 0xe40, 0xe48, 0xe50 };

    for (unsigned i = 0; i < G_N_ELEMENTS(clkn); i++) {
        if (offset == clkn[i]) {
            *index = i;
            *fine = false;
            return true;
        }
        if (offset == fine_offsets[i]) {
            *index = i;
            *fine = true;
            return true;
        }
    }
    return false;
}

static MemTxResult baseband_read(void *opaque, hwaddr offset, uint64_t *value,
                                unsigned size, MemTxAttrs attrs)
{
    Best1700BtcBaseband *s = opaque;

    *value = 0;
    switch (offset) {
    case 0x000:
        /* Reset and software-interrupt request are self-clearing commands.
         * The latter is observed through pending status until guest ACK. */
        break;
    case 0x404:
        *value = BT_CORE_VERSION;
        break;
    case 0x800:
        *value = s->config[offset / 4] | s->ble_abort_pending;
        break;
    case 0x804:
        *value = BLE_CORE_VERSION;
        break;
    case 0x018:
        *value = s->irq_enable;
        break;
    case 0x01c:
        /* The original ISR consumes this directly, without applying +018.
         * Keep masked pending internally, expose only enabled sources. */
        *value = s->irq_status & s->irq_enable;
        break;
    case 0x020:
    case 0xc08:
    case 0x414:
    case 0x814:
        /* Acknowledge commands have no writable readback latch. */
        break;
    case 0x030:
        *value = s->sleep_control;
        if (s->sleep_state == BEST1700_BTC_SLEEPING) {
            *value |= SLEEP_START | SLEEP_STATUS;
        } else if (s->sleep_state == BEST1700_BTC_CORRECTING) {
            *value |= CORRECTION_START;
        }
        break;
    case 0x034:
        *value = s->sleep_cycles;
        break;
    case 0x038:
        *value = s->sleep_elapsed;
        break;
    case 0x040:
        *value = s->correction_fine;
        break;
    case 0x044:
        *value = s->correction_slot;
        break;
    case 0x9c0:
        *value = s->iso_control;
        break;
    case 0x9c8:
        *value = s->correction_iso_us;
        break;
    case 0x9cc:
        *value = s->correction_iso_hus;
        break;
    case 0x410:
        *value = s->bt_irq_status;
        break;
    case 0x460:
        *value = s->bt_error;
        break;
    case 0x810:
        *value = s->ble_irq_status;
        break;
    case 0x860:
        *value = s->ble_error;
        break;
    case 0xd28:
        /* SW trigger synchronization is a level latch. The controller
         * firmware sets bit 31 before changing the trigger route and clears
         * it through CNTL1 bit 14 after the route update. */
        *value = s->sync_interrupt;
        break;
    case 0x978:
        *value = s->ble_local_seed;
        break;
    case 0x97c:
        *value = s->ble_peer_seed;
        break;
    case 0x0e8:
    case 0x0f0:
    case 0x0f8:
        *value = s->targets[(offset - 0xe8) / 8].slot;
        break;
    case 0x0ec:
    case 0x0f4:
    case 0x0fc:
        *value = s->targets[(offset - 0xec) / 8].fine;
        break;
    case 0x100:
        *value = s->sampled_slot;
        break;
    case 0x104:
        *value = s->sampled_fine;
        break;
    case 0xd00:
        /* Command pulse has no sticky readback. */
        break;
    case 0x110:
        /* BTC context restore reads this idle status word after issuing a
         * local MAC command.  The command engine is not active in the
         * emulator, so the hardware reset value is the only safe status.
         * Treating the read as an unimplemented transaction raises a
         * guest BusFault and prevents the AP from reaching the UI. */
        *value = 0;
        break;
    case 0x9c4:
        *value = s->sampled_iso;
        break;
    case 0xe04:
        /* The current-clock register is a live 28-bit BTC slot counter. */
        *value = (uint32_t)((s->native_base_hus + baseband_elapsed_hus(s)) /
                            HALF_SLOT_HUS) & SLOT_MASK;
        break;
    case 0xe78:
        /* Live 32-bit ISO microsecond counter, independent of the sampled
         * +9c4 latch. Share its epoch, sleep freeze and correction phase. */
        *value = (uint32_t)((s->iso_base_hus + baseband_elapsed_hus(s)) / 2);
        break;
    default: {
        unsigned index;
        bool fine;
        if (baseband_capture_offset(offset, &index, &fine)) {
            *value = fine ? s->capture_fine[index] : s->capture_clkn[index];
            break;
        }
        if (baseband_zero_restore(offset)) {
            break;
        }
        if (!baseband_config_mask(offset)) {
            return baseband_unsupported(offset, false, 0);
        }
        *value = s->config[offset / sizeof(uint32_t)];
        break;
    }
    }
    return MEMTX_OK;
}

static MemTxResult baseband_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    Best1700BtcBaseband *s = opaque;
    uint32_t mask;

    switch (offset) {
    case 0x400:
    case 0x800:
        mask = baseband_config_mask(offset);
        if ((value & ~(mask | CORE_RESET |
                       (offset == 0x800 ? BLE_IDLE_ABORT_COMMANDS : 0))) ||
            ((value & CORE_RESET) && s->ble_abort_pending) ||
            (offset == 0x800 && (value & BLE_IDLE_ABORT_COMMANDS) &&
             ((value & CORE_RESET) || s->sleep_state != BEST1700_BTC_AWAKE ||
              !baseband_event_table_idle(s))) ||
            ((value & CORE_RESET) &&
             ((value | s->config[offset / sizeof(uint32_t)]) & (1u << 8)))) {
            /* Only an idle local-core reset is implemented. Reject active
             * radio reset until an actual scheduler owns its completion. */
            return baseband_unsupported(offset, true, value);
        }
        if (value & CORE_RESET) {
            /* Self-clearing local reset touches only that core's transient
             * status, not common clocks, IRQ deadlines, shared descriptors,
             * extension configuration or the other radio core. */
            if (offset == 0x400) {
                s->bt_irq_status = 0;
                s->bt_error = 0;
            } else {
                s->ble_irq_status = 0;
                s->ble_error = 0;
            }
        }
        if (offset == 0x800 && (value & BLE_IDLE_ABORT_COMMANDS)) {
            s->ble_abort_pending |= value & BLE_IDLE_ABORT_COMMANDS;
            if (!timer_pending(s->ble_abort_timer)) {
                /* Consume idle requests on the next modeled baseband tick.
                 * This latency is model policy, not a measured RF abort. */
                timer_mod(s->ble_abort_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + HALF_US_NS);
            }
        }
        s->config[offset / sizeof(uint32_t)] = value & mask;
        break;
    case 0x404:
    case 0x804:
        /* Immutable IP revision; guest writes cannot change identity. */
        break;
    case 0x000:
        if (value & ~(CORE_RESET | CORE_SOFTWARE_REQUEST)) {
            return baseband_unsupported(offset, true, value);
        }
        if (value & CORE_RESET) {
            /* No radio engine is running. Reset the modeled transient
             * state immediately; no ready flag or radio event is created.
             * Extension configuration reset domains are unestablished.
             * Loaded RAL seeds are retained with configuration by policy. */
            s->irq_status = 0;
            s->bt_irq_status = 0;
            s->bt_error = 0;
            s->ble_irq_status = 0;
            s->ble_error = 0;
            s->sync_interrupt = 0;
            timer_del(s->ble_abort_timer);
            s->ble_abort_pending = 0;
            for (unsigned i = 0; i < G_N_ELEMENTS(s->targets); i++) {
                timer_del(s->targets[i].timer);
                s->targets[i].written = 0;
            }
            for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
                timer_del(s->sw_targets[i].timer);
                s->sw_targets[i].written = 0;
                s->sw_targets[i].enabled = false;
            }
            timer_del(s->sleep_timer);
            timer_del(s->correction_timer);
            if (s->sleep_state != BEST1700_BTC_AWAKE) {
                uint64_t elapsed = baseband_elapsed_hus(s);

                s->native_base_hus = (s->native_base_hus + elapsed) %
                                     CLOCK_PERIOD_HUS;
                s->iso_base_hus = (s->iso_base_hus + elapsed) % ISO_PERIOD_HUS;
                if (s->sleep_state == BEST1700_BTC_SLEEPING) {
                    s->clock_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                } else {
                    s->clock_epoch_ns += elapsed * HALF_US_NS;
                }
            }
            s->sleep_state = BEST1700_BTC_AWAKE;
            s->sleep_elapsed = 0;
            s->correction_written = 0;
            baseband_sleep_sync(s);
            baseband_update_irq(s);
        }
        if (value & CORE_SOFTWARE_REQUEST) {
            /* Original BTC clears +020 bit 3, enables +018 bit 3, then
             * requests dispatch through +000 bit 27. Latch independently
             * of the mask; only the original ISR's W1C clears pending. */
            s->irq_status |= CORE_SOFTWARE_IRQ;
            baseband_update_irq(s);
        }
        break;
    case 0x018:
        if (value & ~CORE_IRQ_FIELDS) {
            return baseband_unsupported(offset, true, value);
        }
        s->irq_enable = value;
        baseband_update_irq(s);
        break;
    case 0x01c:
    case 0x410:
    case 0x460:
    case 0x810:
    case 0x860:
        /* Guest writes cannot create a hardware interrupt or error. */
        break;
    case 0x020:
        if (value & (SLEEP_IRQ | CORRECTION_IRQ)) {
            qemu_log_mask(CPU_LOG_INT, "best1700 BTC sleep/correction ACK ns=%"
                          PRId64 " value=0x%" PRIx64 " irq=0x%x state=%d\n",
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), value,
                          s->irq_status, s->sleep_state);
        }
        s->irq_status &= ~value;
        baseband_update_irq(s);
        break;
    case 0xc08:
        if (value & ~0x03ffffffu) {
            return baseband_unsupported(offset, true, value);
        }
        /* CNTL1 is a write-one command register.  The SDK names the low
         * bits explicitly; they clear transient controller flags and do not
         * become a retained configuration word.  Only the modeled status
         * sources are changed here; commands for radio-only state are
         * accepted as synchronous no-ops until that producer is modeled. */
        if (value & CNTL1_TIME_IRQ_CLEAR) {
            s->irq_status &= ~(1u << 4);
        }
        if (value & CNTL1_SYNC_COUNT_CLEAR) {
            s->config[0xd28 / sizeof(uint32_t)] &= ~0x80000000u;
        }
        if (value & CNTL1_SETTRIG) {
            /* SETTRIG consumes the currently enabled software routes.  A
             * radio event producer is absent, so route arming is the only
             * observable operation and the request remains self-clearing. */
            for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
                if (s->sw_targets[i].enabled) {
                    baseband_program_sw_target(s, i);
                }
            }
        }
        for (unsigned i = 0; i < 6; i++) {
            if (value & (1u << (20 + i))) {
                baseband_clear_sw_trigger(s, i);
            }
        }
        if (value & CNTL1_ROUTE0_CLEAR) {
            baseband_clear_sw_trigger(s, 0);
        }
        if (value & CNTL1_ROUTE1_CLEAR) {
            baseband_clear_sw_trigger(s, 1);
        }
        if (value & CNTL1_ROUTE2_CLEAR) {
            baseband_clear_sw_trigger(s, 2);
        }
        if (value & CNTL1_ROUTE3_CLEAR) {
            baseband_clear_sw_trigger(s, 3);
        }
        if (value & CNTL1_ROUTE4_CLEAR) {
            baseband_clear_sw_trigger(s, 4);
        }
        if (value & CNTL1_ROUTE5_CLEAR) {
            baseband_clear_sw_trigger(s, 5);
        }
        if (value & (1u << 15)) {
            s->irq_status &= ~SLEEP_IRQ;
        }
        if (value & (1u << 16)) {
            s->ble_irq_status = 0;
        }
        if (value & (1u << 17)) {
            s->bt_irq_status = 0;
        }
        if (value & (1u << 14)) {
            s->sync_interrupt = 0;
        }
        baseband_update_irq(s);
        break;
    case 0x030:
        return baseband_sleep_write(s, value);
    case 0x034:
        if (s->sleep_state != BEST1700_BTC_AWAKE) {
            return baseband_unsupported(offset, true, value);
        }
        s->sleep_cycles = value;
        break;
    case 0x9c0:
        if (value & ~(ISO_MODE_MASK | ISO_UPDATE | ISO_SAMPLE)) {
            return baseband_unsupported(offset, true, value);
        }
        if ((value & ISO_UPDATE) && !baseband_update_iso_clock(s)) {
            return baseband_unsupported(offset, true, value);
        }
        s->iso_control = value & ISO_MODE_MASK;
        if (value & ISO_SAMPLE) {
            baseband_sample_iso(s);
        }
        break;
    case 0x040:
    case 0x044:
    case 0x9c8:
    case 0x9cc:
        if (s->sleep_state == BEST1700_BTC_SLEEPING ||
            s->sleep_state == BEST1700_BTC_CORRECTING ||
            (offset == 0x040 && value >= HALF_SLOT_HUS) ||
            (offset == 0x044 && ((value & ~SLOT_MASK) != CORRECTION_DELTA)) ||
            (offset == 0x9cc && value > 1)) {
            return baseband_unsupported(offset, true, value);
        }
        switch (offset) {
        case 0x040:
            s->correction_fine = value;
            s->correction_written |= 1;
            break;
        case 0x044:
            s->correction_slot = value;
            s->correction_written |= 2;
            break;
        case 0x9c8:
            s->correction_iso_us = value;
            s->correction_written |= 4;
            break;
        case 0x9cc:
            s->correction_iso_hus = value;
            s->correction_written |= 8;
            break;
        }
        break;
    case 0x414:
        s->bt_irq_status &= ~value;
        break;
    case 0x814:
        s->ble_irq_status &= ~value;
        break;
    case 0xd28:
        /* The register map exposes only the synchronization latch at bit 31;
         * writes are ordinary set/clear operations, not interrupt pulses. */
        if (value & ~0x80000000u) {
            return baseband_unsupported(offset, true, value);
        }
        s->sync_interrupt = value & 0x80000000u;
        break;
    case 0x978:
    case 0x97c:
        if (!(value & BLE_SEED_INIT) ||
            (value & ~(BLE_SEED_INIT | BLE_SEED_VALUE))) {
            return baseband_unsupported(offset, true, value);
        }
        /* Idle seed initialization completes synchronously by model policy.
         * INIT reads clear; no random generator, list search or radio event
         * runs. Writes without INIT have no established semantics. */
        if (offset == 0x978) {
            s->ble_local_seed = value & BLE_SEED_VALUE;
        } else {
            s->ble_peer_seed = value & BLE_SEED_VALUE;
        }
        break;
    case 0x070:
        if (value & ~(SPI_CONFIG_FIELDS | SPI_COMPLETE)) {
            /* In particular SPIGO (bit 0) needs an actual SPI engine. */
            return baseband_unsupported(offset, true, value);
        }
        /* SPICOMP is read-only. No transaction has completed, and neither
         * a status write nor passive configuration can make it complete. */
        s->config[offset / sizeof(uint32_t)] = value & SPI_CONFIG_FIELDS;
        break;
    case 0x0e8:
    case 0x0f0:
    case 0x0f8:
        if (value & ~SLOT_MASK) {
            return baseband_unsupported(offset, true, value);
        }
        s->targets[(offset - 0xe8) / 8].slot = value;
        s->targets[(offset - 0xe8) / 8].written |= 1;
        baseband_program_target(s, (offset - 0xe8) / 8);
        break;
    case 0x0ec:
    case 0x0f4:
    case 0x0fc:
        if (value >= HALF_SLOT_HUS) {
            return baseband_unsupported(offset, true, value);
        }
        s->targets[(offset - 0xec) / 8].fine = value;
        s->targets[(offset - 0xec) / 8].written |= 2;
        baseband_program_target(s, (offset - 0xec) / 8);
        break;
    case 0x110:
        /* The BTC restore/maintenance path issues the observed bit31 command
         * through this window.  It is a self-clearing command: accepting it
         * avoids turning a valid maintenance request into a BusFault, while
         * unsupported command bits remain fail-closed. */
        if (value != (1u << 31)) {
            return baseband_unsupported(offset, true, value);
        }
        break;
    case 0xd00:
        /* MAC context-restore command. The observed warm path writes the
         * single command bit after replaying retained configuration. Zero
         * is an idempotent restore-image write; other bits are rejected
         * atomically. The command is a pulse and therefore reads as zero. */
        if (value & ~1u) {
            return baseband_unsupported(offset, true, value);
        }
        if (value & 1u) {
            baseband_mac_context_restore(s);
        }
        break;
    case 0x100:
        if (value & ~(CLOCK_UPDATE | CLOCK_SAMPLE | SLOT_MASK)) {
            return baseband_unsupported(offset, true, value);
        }
        /* CLKN_UPD is a command pulse. Update first so a combined update and
         * sample observes the newly requested slot counter. */
        if (value & CLOCK_UPDATE) {
            if (!baseband_update_slot_clock(s, value)) {
                return baseband_unsupported(offset, true, value);
            }
        }
        if (value & CLOCK_SAMPLE) {
            baseband_sample_clock(s);
        }
        break;
    case 0xc7c:
        mask = baseband_config_mask(offset);
        if (value & ~mask) {
            return baseband_unsupported(offset, true, value);
        }
        /* CNTLX contains the software-trigger route-0 enable together with
         * playback selection and synchronization parameters.  These are
         * ordinary retained fields in the silicon; enabling route 0 also
         * arms the route-0 timestamp pair already supplied via C94/C98. */
        s->config[offset / sizeof(uint32_t)] = value & mask;
        s->sw_targets[0].enabled = (value & C7C_ROUTE0_ENABLE) != 0;
        baseband_program_sw_target(s, 0);
        break;
    case 0xc94:
    case 0xd08:
    case 0xd10:
    case 0xd18:
    case 0xd80:
    case 0xd88: {
        static const hwaddr offsets[] = {
            0xc94, 0xd08, 0xd10, 0xd18, 0xd80, 0xd88,
        };
        unsigned index = 0;

        while (offsets[index] != offset) {
            index++;
        }
        mask = baseband_config_mask(offset);
        if (value & ~mask) {
            return baseband_unsupported(offset, true, value);
        }
        s->config[offset / sizeof(uint32_t)] = value & mask;
        s->sw_targets[index].slot = value & SLOT_MASK;
        s->sw_targets[index].written |= 1;
        baseband_program_sw_target(s, index);
        break;
    }
    case 0xc98:
    case 0xd0c:
    case 0xd14:
    case 0xd1c:
    case 0xd84:
    case 0xd8c: {
        static const hwaddr offsets[] = {
            0xc98, 0xd0c, 0xd14, 0xd1c, 0xd84, 0xd8c,
        };
        unsigned index = 0;

        while (offsets[index] != offset) {
            index++;
        }
        mask = baseband_config_mask(offset);
        if (value & ~mask) {
            return baseband_unsupported(offset, true, value);
        }
        s->config[offset / sizeof(uint32_t)] = value & mask;
        s->sw_targets[index].fine = value & 0x3ff;
        s->sw_targets[index].written |= 2;
        s->sw_targets[index].enabled = (value & CORE_RESET) != 0;
        if (index == 0) {
            s->config[0xc7c / sizeof(uint32_t)] =
                (s->config[0xc7c / sizeof(uint32_t)] & ~C7C_ROUTE0_ENABLE) |
                (s->sw_targets[index].enabled ? C7C_ROUTE0_ENABLE : 0);
        }
        baseband_program_sw_target(s, index);
        break;
    }
    case 0x104:
    case 0x9c4:
        /* Sampled counters are read-only. */
        break;
    default: {
        unsigned index;
        bool fine;
        if (baseband_capture_offset(offset, &index, &fine)) {
            /* Clock-capture registers are hardware-owned read-only latches.
             * Firmware accesses them as read-only state; terminal-count events
             * update the latch and software only reads the captured pair.
             * Silicon ignores software writes, so a restore-time write must
             * be a successful no-op rather than a guest bus fault. */
            return MEMTX_OK;
        }
        if (baseband_zero_restore(offset)) {
            if (value) {
                return baseband_unsupported(offset, true, value);
            }
            /* Zero asserts no C08 clear/start strobes or 9A0 START, and
             * cannot set C34 status. No timer, state, or memory is changed. */
            break;
        }
        mask = baseband_config_mask(offset);
        if (!mask || (value & ~mask)) {
            return baseband_unsupported(offset, true, value);
        }
        s->config[offset / sizeof(uint32_t)] = value;
        break;
    }
    }
    return MEMTX_OK;
}

void best1700_btc_baseband_dma_terminal_source(Best1700BtcBaseband *s,
                                               unsigned remapped_channel,
                                               bool bth_dma)
{
    uint32_t selectors = s->config[(bth_dma ? 0xc4c : 0xc50) /
                                   sizeof(uint32_t)];
    uint32_t modes = s->config[0xd70 / sizeof(uint32_t)];

    /*
     * The enable bit lives in CAP_SEL for both source groups. C4C carries
     * the four BTH DMA selectors while C50 carries the four SYS selectors.
     */
    if (!(s->config[0xc50 / sizeof(uint32_t)] & (1u << 31))) {
        return;
    }
    for (unsigned slot = 0; slot < 4; slot++) {
        uint32_t selected = (selectors >> (slot * 8)) &
                            (bth_dma ? 0x7f : 0x1f);

        if (selected == remapped_channel &&
            (modes & (1u << (slot + 4)))) {
            baseband_capture_now(s, slot + (bth_dma ? 0 : 4));
        }
    }
}

void best1700_btc_baseband_dma_terminal(void *opaque, unsigned channel)
{
    /* SYS DMA0 channels are exposed to BTC as IDs 1..8 and use CAP4..7. */
    best1700_btc_baseband_dma_terminal_source(opaque, channel + 1, false);
}

static const MemoryRegionOps baseband_ops = {
    .read_with_attrs = baseband_read,
    .write_with_attrs = baseband_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_btc_baseband_reset(Best1700BtcBaseband *s)
{
    timer_del(s->ble_abort_timer);
    s->ble_abort_pending = 0;
    for (unsigned i = 0; i < G_N_ELEMENTS(s->targets); i++) {
        timer_del(s->targets[i].timer);
        s->targets[i].slot = 0;
        s->targets[i].fine = 0;
        s->targets[i].written = 0;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
        timer_del(s->sw_targets[i].timer);
        s->sw_targets[i].slot = 0;
        s->sw_targets[i].fine = 0;
        s->sw_targets[i].written = 0;
        s->sw_targets[i].enabled = false;
    }
    timer_del(s->sleep_timer);
    timer_del(s->correction_timer);
    for (unsigned i = 0; i < G_N_ELEMENTS(s->config); i++) {
        s->config[i] = baseband_config[i].reset;
    }
    s->irq_enable = 0;
    s->irq_status = 0;
    s->bt_irq_status = 0;
    s->bt_error = 0;
    s->ble_irq_status = 0;
    s->ble_error = 0;
    s->sync_interrupt = 0;
    s->mac_restore_count = 0;
    s->ble_local_seed = 0;
    s->ble_peer_seed = 0;
    s->clock_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->native_base_hus = 0;
    s->iso_base_hus = 0;
    s->sampled_slot = 0;
    s->sampled_fine = 624;
    s->sampled_iso = 0;
    memset(s->capture_clkn, 0, sizeof(s->capture_clkn));
    memset(s->capture_fine, 0, sizeof(s->capture_fine));
    s->sleep_control = 0;
    s->sleep_cycles = 0;
    s->sleep_elapsed = 0;
    s->sleep_epoch_ns = 0;
    s->external_wake = false;
    s->correction_slot = 0;
    s->correction_fine = 0;
    s->correction_iso_us = 0;
    s->correction_iso_hus = 0;
    s->iso_control = 0;
    s->correction_written = 0;
    s->sleep_state = BEST1700_BTC_AWAKE;
    baseband_refresh_trigger_status(s);
    baseband_sleep_sync(s);
    baseband_update_irq(s);
}

void best1700_btc_baseband_init(Best1700BtcBaseband *s, Object *owner,
                              uint32_t lpo_hz, uint32_t correction_latency_ns)
{
    memset(s, 0, sizeof(*s));
    s->lpo_hz = lpo_hz;
    assert(correction_latency_ns);
    s->correction_latency_ns = correction_latency_ns;
    for (unsigned i = 0; i < G_N_ELEMENTS(s->targets); i++) {
        s->targets[i].owner = s;
        s->targets[i].index = i;
        s->targets[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                          baseband_target_expired,
                                          &s->targets[i]);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
        s->sw_targets[i].owner = s;
        s->sw_targets[i].index = i;
        s->sw_targets[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                               baseband_target_expired,
                                               &s->sw_targets[i]);
    }
    s->sleep_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                 baseband_sleep_expired, s);
    s->correction_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      baseband_correction_expired, s);
    s->ble_abort_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     baseband_idle_abort_complete, s);
    memory_region_init_io(&s->iomem, owner, &baseband_ops, s,
                          "best1700.btc-baseband", BEST1700_BTC_BASEBAND_SIZE);
    best1700_btc_baseband_reset(s);
}

void best1700_btc_baseband_set_irq(Best1700BtcBaseband *s, qemu_irq irq)
{
    s->irq = irq;
    baseband_update_irq(s);
}

void best1700_btc_baseband_destroy(Best1700BtcBaseband *s)
{
    if (s->ble_abort_timer) {
        timer_del(s->ble_abort_timer);
        timer_free(s->ble_abort_timer);
        s->ble_abort_timer = NULL;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->targets); i++) {
        if (s->targets[i].timer) {
            timer_del(s->targets[i].timer);
            timer_free(s->targets[i].timer);
            s->targets[i].timer = NULL;
        }
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->sw_targets); i++) {
        if (s->sw_targets[i].timer) {
            timer_del(s->sw_targets[i].timer);
            timer_free(s->sw_targets[i].timer);
            s->sw_targets[i].timer = NULL;
        }
    }
    if (s->sleep_timer) {
        timer_del(s->sleep_timer);
        timer_free(s->sleep_timer);
        s->sleep_timer = NULL;
    }
    if (s->correction_timer) {
        timer_del(s->correction_timer);
        timer_free(s->correction_timer);
        s->correction_timer = NULL;
    }
}

void best1700_btc_baseband_connect_sleep(Best1700BtcBaseband *s, void *opaque,
                                       void (*sync)(void *, bool))
{
    s->sleep_sync_opaque = opaque;
    s->sleep_sync = sync;
    baseband_sleep_sync(s);
}

void best1700_btc_baseband_connect_exchange(Best1700BtcBaseband *s,
                                          MemoryRegion *exchange)
{
    assert(memory_region_is_ram(exchange));
    s->exchange = exchange;
}
