/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_CACHE_H
#define BEST1700_CACHE_H

#include "exec/memory.h"

#define BEST1700_CACHE_REG_SIZE 0x1000

typedef struct Best1700Cache {
    MemoryRegion region;
    uint32_t control;
    uint32_t config;
    uint32_t wrap;
    uint64_t initialize_count;
    uint64_t invalidate_all_count;
    uint64_t invalidate_line_count;
    uint32_t last_line;
    unsigned unsupported;
} Best1700Cache;

void best1700_cache_init(Best1700Cache *s, Object *owner, const char *name);
void best1700_cache_reset(Best1700Cache *s);

#endif
