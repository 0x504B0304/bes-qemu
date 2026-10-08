/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_PAGE_SPY_H
#define BEST1700_PAGE_SPY_H

#include "hw/sysbus.h"

#define TYPE_BEST1700_PAGE_SPY "best1700-page-spy"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700PageSpyState, BEST1700_PAGE_SPY)
#define BEST1700_PAGE_SPY_MMIO_SIZE 0x1000

struct Best1700PageSpyState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t ctrl;
    uint32_t region_ctrl;
    uint32_t start[2];
    uint32_t end[2];
    uint32_t hit[2];
    uint32_t hit_start[2];
    uint32_t hit_end[2];
};

void best1700_page_spy_reset(Best1700PageSpyState *s);

#endif
