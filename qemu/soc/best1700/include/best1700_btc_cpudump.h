/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTC_CPUDUMP_H
#define BEST1700_BTC_CPUDUMP_H

#include "exec/memory.h"
#include "qemu/timer.h"

#define BEST1700_BTC_CPUDUMP_DEFAULT_SAMPLE_NS 1000u
#define BEST1700_BTC_CPUDUMP_BASE 0x6e100000u
#define BEST1700_BTC_CPUDUMP_WINDOW 0x3000u
#define BEST1700_BTC_CPUDUMP_TRACE_SIZE 0x2000u

typedef struct Best1700BtcCpudump {
    MemoryRegion iomem;
    MemoryRegion trace;
    MemoryRegion control;
    QEMUTimer *sample_timer;
    bool enabled;
    bool capture_requested;
    uint32_t trace_index;
    uint32_t sample_ns;
    const uint32_t *pc;
} Best1700BtcCpudump;

void best1700_btc_cpudump_init(Best1700BtcCpudump *s, Object *owner,
                                uint32_t sample_ns);
void best1700_btc_cpudump_reset(Best1700BtcCpudump *s);
void best1700_btc_cpudump_set_pc(Best1700BtcCpudump *s,
                                 const uint32_t *pc);

#endif
