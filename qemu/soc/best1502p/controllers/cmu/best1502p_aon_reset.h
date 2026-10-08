/* Qualified BEST1502P AON global reset ports; peer reset domains are separate. */
#ifndef BEST1502P_AON_RESET_H
#define BEST1502P_AON_RESET_H

#include "exec/memory.h"

typedef struct Best1502pAonReset {
    uint32_t asserted;
    MemoryRegion region;
} Best1502pAonReset;

void best1502p_aon_reset_init(Best1502pAonReset *reset, MemoryRegion *memory);

#endif
