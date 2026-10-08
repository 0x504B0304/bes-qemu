/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTC_AON_H
#define BEST1700_BTC_AON_H

#include "exec/memory.h"

#define BEST1700_BTC_AON_BASE 0x6e060000u
#define BEST1700_BTC_AON_WINDOW 0x1000u

typedef struct Best1700BtcAon {
    MemoryRegion iomem;
    uint32_t boot_flags;
} Best1700BtcAon;

/* All bus masters share this window. CPU-only reset retains its state. */
void best1700_btc_aon_init(Best1700BtcAon *s, Object *owner);
void best1700_btc_aon_reset(Best1700BtcAon *s);

#endif
