/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_RF_MATRIX_H
#define BEST1700_RF_MATRIX_H
#include "exec/memory.h"

#define BEST1700_RF_MATRIX_BASE 0x72c0b6c8u
#define BEST1700_RF_MATRIX_SIZE 32u

typedef struct Best1700RfMatrix {
    MemoryRegion iomem;
    uint32_t words[BEST1700_RF_MATRIX_SIZE / sizeof(uint32_t)];
} Best1700RfMatrix;

void best1700_rf_matrix_init(Best1700RfMatrix *s, Object *owner);
void best1700_rf_matrix_reset(Best1700RfMatrix *s);
#endif
