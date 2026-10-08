#ifndef BEST1502P_LPU_H
#define BEST1502P_LPU_H
#include "exec/memory.h"

typedef struct Best1502pLpu {
    MemoryRegion region;
    uint32_t config;
    int64_t ready_at[2];
    unsigned traces;
} Best1502pLpu;

void best1502p_lpu_init(Best1502pLpu *lpu, MemoryRegion *memory);
#endif
