/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_TGU_H
#define BEST1700_TGU_H

#include "hw/sysbus.h"
#include "hw/arm/armv7m.h"

#define TYPE_BEST1700_TGU "best1700-tgu"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700TguState, BEST1700_TGU)

#define BEST1700_TGU_BANKS 2
#define BEST1700_TGU_LUT_WORDS 16

/* MMIO 0: MemSysCtl page; MMIO 1: ITCM gate; MMIO 2: DTCM gate.
 * Each downstream spans its complete architectural TCM offset window,
 * including holes. Map CPU and bus aliases above the corresponding gate.
 */
typedef struct Best1700TguBank {
    IOMMUMemoryRegion upstream;
    MemoryRegion blocked;
    MemoryRegion *downstream;
    AddressSpace downstream_as;
    AddressSpace blocked_as;
    uint32_t block_size;
    uint32_t cfg;
    uint32_t ctrl;
    uint32_t lut[BEST1700_TGU_LUT_WORDS];
    uint64_t size;
} Best1700TguBank;

struct Best1700TguState {
    SysBusDevice parent_obj;
    MemoryRegion registers;
    ARMv7MState *armv7m;
    bool icache_present;
    bool dcache_present;
    uint32_t mscr;
    uint32_t pfcr;
    Best1700TguBank bank[BEST1700_TGU_BANKS];
};

#endif
