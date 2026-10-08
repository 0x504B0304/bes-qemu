#ifndef BEST1502P_MEMSC_H
#define BEST1502P_MEMSC_H

#include "exec/hwaddr.h"
#include <stdbool.h>
#include <stdint.h>

#define BEST1502P_MEMSC_COUNT 4u
#define BEST1502P_MEMSC_BANKS 2u

typedef struct Best1502pMemsc {
    bool free[BEST1502P_MEMSC_BANKS][BEST1502P_MEMSC_COUNT];
    hwaddr base;
    unsigned bank;
} Best1502pMemsc;

void best1502p_memsc_init(Best1502pMemsc *memsc, hwaddr base, unsigned bank);
bool best1502p_memsc_read(Best1502pMemsc *memsc, hwaddr offset,
                          unsigned size, uint64_t *result);
bool best1502p_memsc_write(Best1502pMemsc *memsc, hwaddr offset,
                           unsigned size, uint64_t value);

#endif
