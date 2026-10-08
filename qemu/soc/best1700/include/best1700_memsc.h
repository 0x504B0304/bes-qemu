/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_MEMSC_H
#define BEST1700_MEMSC_H

#include <stdint.h>

typedef struct Best1700Memsc {
    uint32_t count;
    uint32_t free_mask;
} Best1700Memsc;

void best1700_memsc_init(Best1700Memsc *s, unsigned count);
void best1700_memsc_reset(Best1700Memsc *s);
uint32_t best1700_memsc_claim(Best1700Memsc *s, unsigned index);
void best1700_memsc_release(Best1700Memsc *s, unsigned index, uint32_t value);
uint32_t best1700_memsc_status(const Best1700Memsc *s);

#endif