/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTH_TIMING_H
#define BEST1700_BTH_TIMING_H

#include "best1700_aon.h"

#define BEST1700_BTH_TIMING_COUNT 3
#define BEST1700_BTH_TIMING_PORTS 11
#define BEST1700_BTH_TIMING_WINDOW 0x1000
#define BEST1700_BTH_WAKE_BANKS 4
#define BEST1700_BTH_WAKE_IRQS (32 * BEST1700_BTH_WAKE_BANKS)

typedef struct Best1700BthTiming Best1700BthTiming;
typedef void (*Best1700BthSpiClockFn)(void *opaque, unsigned index, uint32_t hz);
typedef void (*Best1700BthWakeFn)(void *opaque, bool level);

typedef struct Best1700BthTimingPort {
    MemoryRegion region;
    Best1700BthTiming *timing;
    unsigned offset;
} Best1700BthTimingPort;

struct Best1700BthTiming {
    MemoryRegion cmu;
    Best1700BthTimingPort ports[BEST1700_BTH_TIMING_PORTS];
    Best1700Aon timers[BEST1700_BTH_TIMING_COUNT];
    uint32_t periph_clk;
    uint32_t i2c_clk;
    uint32_t wakeup_clk_cfg;
    uint32_t lpu_clock_status;
    uint32_t wakeup_mask[BEST1700_BTH_WAKE_BANKS];
    uint32_t irq_levels[BEST1700_BTH_WAKE_BANKS];
    bool wake_level;
    Best1700BthWakeFn wake_changed;
    void *wake_opaque;
    uint32_t crystal_hz;
    unsigned spi_clock_diagnostics;
    Best1700BthSpiClockFn spi_clock_changed;
    void *spi_clock_opaque;
    uint32_t slow_hz;
    uint32_t fast_hz;
    uint32_t counter_hz;
    uint64_t ticks;
    int64_t epoch_ns;
};

void best1700_bth_timing_init(Best1700BthTiming *s, Object *owner,
                              uint32_t slow_hz, uint32_t fast_hz);
void best1700_bth_timing_reset(Best1700BthTiming *s);
void best1700_bth_timing_set_spi_clock_callback(Best1700BthTiming *s,
    uint32_t crystal_hz, Best1700BthSpiClockFn callback, void *opaque);

/* Raw peripheral inputs survive CPU power gating and are never acknowledged
 * by the CMU. All setters and callbacks run with the device/BQL lock held. */
void best1700_bth_timing_set_wake_callback(Best1700BthTiming *s,
    Best1700BthWakeFn callback, void *opaque);
void best1700_bth_timing_set_irq_level(Best1700BthTiming *s,
    unsigned irq, bool level);

/* LPU completion inputs from the clock/power controller. Configuration writes
 * alone neither lock a PLL nor establish that the oscillator is ready. */
void best1700_bth_timing_set_lpu_clock_status(Best1700BthTiming *s,
    bool osc_ready, bool pll_ready);

#endif
