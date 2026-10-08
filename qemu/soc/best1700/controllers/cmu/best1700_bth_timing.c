/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "best1700_bth_timing.h"

#define BTH_PERIPH_CLK 0x5c
#define BTH_WAKEUP_CLK_CFG 0x8c
#define BTH_FAST_TIMER 0x90
#define BTH_FAST_TIMER_HI 0x94
#define BTH_FAST_TIMER_SET 0x98
#define BTH_FAST_TIMER_HI_SET 0x9c
#define BTH_WAKEUP_MASK0 0x1a8
#define BTH_WAKEUP_MASK1 0x1ac
#define BTH_WAKEUP_MASK2 0x1b0
#define BTH_WAKEUP_MASK3 0x1b4
#define BTH_I2C_CLK 0x1c8

#define BTH_LPU_TIMER_WTPLL_MASK 0x1fu
#define BTH_LPU_AUTO_SWITCH26 (1u << 5)
#define BTH_LPU_AUTO_SWITCHPLL (1u << 6)
#define BTH_LPU_STATUS_26M (1u << 7)
#define BTH_LPU_STATUS_PLL (1u << 8)
#define BTH_LPU_AUTO_MID (1u << 9)
#define BTH_LPU_CONFIG_MASK (BTH_LPU_TIMER_WTPLL_MASK | \
    BTH_LPU_AUTO_SWITCH26 | BTH_LPU_AUTO_SWITCHPLL | BTH_LPU_AUTO_MID)

static void best1700_bth_update_wake(Best1700BthTiming *s)
{
    bool level = false;

    for (unsigned i = 0; i < BEST1700_BTH_WAKE_BANKS; i++) {
        uint32_t irq_mask = i == BEST1700_BTH_WAKE_BANKS - 1 ?
                            0x7fffffffu : UINT32_MAX;

        /* Bank 3 bit 31 selects NMI, not external IRQ127. NMI is not
         * connected to this peripheral IRQ aggregate. */
        level |= (s->irq_levels[i] & s->wakeup_mask[i] & irq_mask) != 0;
    }
    if (level != s->wake_level) {
        s->wake_level = level;
        if (s->wake_changed) {
            s->wake_changed(s->wake_opaque, level);
        }
    }
}

static void best1700_bth_spi_clocks(Best1700BthTiming *s)
{
    if (!s->spi_clock_changed) {
        return;
    }
    for (unsigned i = 0; i < 2; i++) {
        unsigned config = (s->i2c_clk >> (16 + 6 * i)) & 0x3f;
        unsigned source = config & 3;
        uint32_t hz = 0;

        /* Each SPI mux has x4, x2, PLL select, divider[1:0], PLL enable.
         * The PLL source is not connected. Do not synthesize a live clock
         * from its divider or accept an unestablished x4-without-x2 mode. */
        if (!(config & ((1u << 2) | (1u << 5))) && source != 1) {
            hz = s->crystal_hz * (source == 3 ? 4 : source == 2 ? 2 : 1);
        } else if (s->spi_clock_diagnostics++ < 16) {
            qemu_log_mask(LOG_UNIMP,
                "best1700 BTH SPI%u unsupported clock config=0x%x\n", i,
                config);
        }
        s->spi_clock_changed(s->spi_clock_opaque, i, hz);
    }
}

static uint64_t best1700_bth_ticks(Best1700BthTiming *s)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->epoch_ns;

    return s->ticks + muldiv64(MAX(elapsed, 0), s->counter_hz,
                               NANOSECONDS_PER_SECOND);
}

static void best1700_bth_set_ticks(Best1700BthTiming *s, uint64_t ticks)
{
    s->ticks = ticks;
    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void best1700_bth_timer_clocks(Best1700BthTiming *s)
{
    unsigned divider = (s->periph_clk >> 6) & 3;

    s->counter_hz = s->fast_hz >> MIN(divider, 2u);
    best1700_aon_set_frequency(&s->timers[0], s->slow_hz);
    for (unsigned i = 1; i < BEST1700_BTH_TIMING_COUNT; i++) {
        uint32_t frequency = s->periph_clk & (1u << (7 + i)) ?
                             s->counter_hz : s->slow_hz;

        best1700_aon_set_frequency(&s->timers[i], frequency);
    }
}

static uint64_t best1700_bth_timing_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    Best1700BthTimingPort *port = opaque;
    Best1700BthTiming *s = port->timing;
    uint32_t value;
    uint32_t mask = size == 1 ? UINT8_MAX : size == 2 ? UINT16_MAX : UINT32_MAX;

    switch (port->offset) {
    case BTH_PERIPH_CLK:
        value = s->periph_clk;
        break;
    case BTH_I2C_CLK:
        value = s->i2c_clk;
        break;
    case BTH_WAKEUP_CLK_CFG:
        value = s->wakeup_clk_cfg | s->lpu_clock_status;
        break;
    case BTH_WAKEUP_MASK0:
    case BTH_WAKEUP_MASK1:
    case BTH_WAKEUP_MASK2:
    case BTH_WAKEUP_MASK3:
        value = s->wakeup_mask[(port->offset - BTH_WAKEUP_MASK0) / 4];
        break;
    case BTH_FAST_TIMER:
        value = best1700_bth_ticks(s);
        break;
    case BTH_FAST_TIMER_HI:
        value = best1700_bth_ticks(s) >> 32;
        break;
    case BTH_FAST_TIMER_SET:
        value = best1700_bth_ticks(s);
        break;
    case BTH_FAST_TIMER_HI_SET:
        value = best1700_bth_ticks(s) >> 32;
        break;
    default:
        g_assert_not_reached();
    }
    return (value >> (offset * 8)) & mask;
}

static void best1700_bth_timing_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    Best1700BthTimingPort *port = opaque;
    Best1700BthTiming *s = port->timing;
    uint32_t mask = size == 1 ? UINT8_MAX : size == 2 ? UINT16_MAX : UINT32_MAX;
    unsigned shift = offset * 8;

    if (port->offset == BTH_WAKEUP_CLK_CFG) {
        uint32_t writable = (mask << shift) & BTH_LPU_CONFIG_MASK;

        /* The two LPU status bits are hardware completion inputs. The
         * oscillator wait counter belongs to the always-on clock block;
         * keeping configuration here does not synthesize that transition. */
        s->wakeup_clk_cfg = (s->wakeup_clk_cfg & ~writable) |
                           (((uint32_t)value << shift) & writable);
        return;
    }
    if (port->offset >= BTH_WAKEUP_MASK0 &&
        port->offset <= BTH_WAKEUP_MASK3) {
        unsigned bank = (port->offset - BTH_WAKEUP_MASK0) / 4;

        s->wakeup_mask[bank] = (s->wakeup_mask[bank] & ~(mask << shift)) |
                              (((uint32_t)value & mask) << shift);
        best1700_bth_update_wake(s);
        return;
    }
    if (port->offset == BTH_I2C_CLK) {
        /* Preserve the shared register's other fields for existing users;
         * only the two established SPI muxes have modeled side effects. */
        s->i2c_clk = (s->i2c_clk & ~(mask << shift)) |
                     (((uint32_t)value & mask) << shift);
        best1700_bth_spi_clocks(s);
        return;
    }
    if (port->offset == BTH_FAST_TIMER_SET ||
        port->offset == BTH_FAST_TIMER_HI_SET) {
        uint64_t ticks = best1700_bth_ticks(s);

        if (port->offset == BTH_FAST_TIMER_SET) {
            ticks = (ticks & UINT64_C(0xffffffff00000000)) |
                    (((uint32_t)value & mask) << shift);
        } else {
            ticks = (ticks & UINT64_C(0x00000000ffffffff)) |
                    ((uint64_t)((uint32_t)value & mask) << (32 + shift));
        }
        best1700_bth_set_ticks(s, ticks);
        return;
    }
    if (port->offset != BTH_PERIPH_CLK) {
        return; /* The running counter is read-only. */
    }
    s->ticks = best1700_bth_ticks(s);
    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->periph_clk = (s->periph_clk & ~(mask << shift)) |
                    (((uint32_t)value & mask) << shift);
    best1700_bth_timer_clocks(s);
}

static const MemoryRegionOps best1700_bth_timing_ops = {
    .read = best1700_bth_timing_read,
    .write = best1700_bth_timing_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void best1700_bth_timing_reset(Best1700BthTiming *s)
{
    s->periph_clk = 0;
    s->i2c_clk = 0;
    s->wakeup_clk_cfg = 0;
    s->lpu_clock_status = 0;
    memset(s->wakeup_mask, 0, sizeof(s->wakeup_mask));
    memset(s->irq_levels, 0, sizeof(s->irq_levels));
    /* This entry point is the full-machine reset, not a warm CPU reset. */
    best1700_bth_update_wake(s);
    s->spi_clock_diagnostics = 0;
    s->ticks = 0;
    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < BEST1700_BTH_TIMING_COUNT; i++) {
        best1700_aon_reset(&s->timers[i]);
    }
    best1700_bth_timer_clocks(s);
    best1700_bth_spi_clocks(s);
}

void best1700_bth_timing_init(Best1700BthTiming *s, Object *owner,
                              uint32_t slow_hz, uint32_t fast_hz)
{
    static const unsigned offsets[] = {
        BTH_PERIPH_CLK, BTH_FAST_TIMER, BTH_FAST_TIMER_HI,
        BTH_FAST_TIMER_SET, BTH_FAST_TIMER_HI_SET, BTH_I2C_CLK,
        BTH_WAKEUP_CLK_CFG, BTH_WAKEUP_MASK0, BTH_WAKEUP_MASK1,
        BTH_WAKEUP_MASK2, BTH_WAKEUP_MASK3,
    };
    static const char *const timer_names[] = {
        "best1700.bth-aon-timer2", "best1700.bth-timer0", "best1700.bth-timer1",
    };

    s->slow_hz = slow_hz;
    s->fast_hz = fast_hz;
    s->crystal_hz = 0;
    s->spi_clock_changed = NULL;
    s->spi_clock_opaque = NULL;
    s->wake_level = false;
    s->wake_changed = NULL;
    s->wake_opaque = NULL;
    memset(s->irq_levels, 0, sizeof(s->irq_levels));
    g_assert(G_N_ELEMENTS(offsets) == BEST1700_BTH_TIMING_PORTS);
    memory_region_init(&s->cmu, owner, "best1700.bth-timing-cmu",
                        BEST1700_BTH_TIMING_WINDOW);
    /* Sparse ports preserve other CMU blocks, including hardware IPC. */
    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        Best1700BthTimingPort *port = &s->ports[i];
        g_autofree char *name = g_strdup_printf("best1700.bth-timing-%x",
                                                 offsets[i]);

        port->timing = s;
        port->offset = offsets[i];
        memory_region_init_io(&port->region, owner, &best1700_bth_timing_ops,
                              port, name, 4);
        memory_region_add_subregion(&s->cmu, offsets[i], &port->region);
    }
    for (unsigned i = 0; i < BEST1700_BTH_TIMING_COUNT; i++) {
        best1700_aon_init(&s->timers[i], owner, timer_names[i], BEST1700_AON_TIMER,
                           BEST1700_BTH_TIMING_WINDOW, NULL);
    }
    best1700_bth_timing_reset(s);
}

void best1700_bth_timing_set_spi_clock_callback(Best1700BthTiming *s,
    uint32_t crystal_hz, Best1700BthSpiClockFn callback, void *opaque)
{
    g_assert(crystal_hz <= UINT32_MAX / 4);
    s->crystal_hz = crystal_hz;
    s->spi_clock_changed = callback;
    s->spi_clock_opaque = opaque;
    best1700_bth_spi_clocks(s);
}

void best1700_bth_timing_set_wake_callback(Best1700BthTiming *s,
    Best1700BthWakeFn callback, void *opaque)
{
    s->wake_changed = callback;
    s->wake_opaque = opaque;
    if (callback) {
        callback(opaque, s->wake_level);
    }
}

void best1700_bth_timing_set_irq_level(Best1700BthTiming *s,
    unsigned irq, bool level)
{
    unsigned bank = irq / 32;
    uint32_t bit = 1u << (irq % 32);

    g_assert(irq < BEST1700_BTH_WAKE_IRQS);
    if (level) {
        s->irq_levels[bank] |= bit;
    } else {
        s->irq_levels[bank] &= ~bit;
    }
    best1700_bth_update_wake(s);
}

void best1700_bth_timing_set_lpu_clock_status(Best1700BthTiming *s,
    bool osc_ready, bool pll_ready)
{
    s->lpu_clock_status = (osc_ready ? BTH_LPU_STATUS_26M : 0) |
                          (pll_ready ? BTH_LPU_STATUS_PLL : 0);
}
