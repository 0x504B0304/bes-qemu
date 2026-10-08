/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTH_MEMSC_H
#define BEST1700_BTH_MEMSC_H

#include "exec/memory.h"

#define BEST1700_BTH_MEMSC_COUNT 4
#define BEST1700_BTH_MEMSC_WINDOW 0x1000

typedef struct Best1700BthMemsc {
    MemoryRegion cmu;
    MemoryRegion registers;
    uint32_t free_mask;
} Best1700BthMemsc;

/* Map aliases of this single sparse container in every participating bus.
 * Only offsets 0xc0..0xd3 are owned; SYS/AON semaphore banks are independent. */
void best1700_bth_memsc_init(Best1700BthMemsc *s, Object *owner);
void best1700_bth_memsc_reset(Best1700BthMemsc *s);

#endif
