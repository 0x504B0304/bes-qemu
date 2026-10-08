#include "qemu/osdep.h"
#include "best1502p_memsc.h"

void best1502p_memsc_init(Best1502pMemsc *memsc, hwaddr base, unsigned bank)
{
    memset(memsc, 0, sizeof(*memsc));
    memsc->base = base;
    memsc->bank = bank < BEST1502P_MEMSC_BANKS ? bank : 0;
    for (unsigned i = 0; i < BEST1502P_MEMSC_COUNT; i++) {
        memsc->free[memsc->bank][i] = true;
    }
}

static bool memsc_index(const Best1502pMemsc *memsc, hwaddr offset,
                        unsigned size, unsigned *index)
{
    if (size != sizeof(uint32_t) || (offset & 3) ||
        offset >= BEST1502P_MEMSC_COUNT * sizeof(uint32_t)) {
        return false;
    }
    *index = offset / sizeof(uint32_t);
    return true;
}

bool best1502p_memsc_read(Best1502pMemsc *memsc, hwaddr offset,
                          unsigned size, uint64_t *result)
{
    unsigned index;
    if (!memsc_index(memsc, offset, size, &index)) {
        return false;
    }
    *result = memsc->free[memsc->bank][index] ? 1 : 0;
    memsc->free[memsc->bank][index] = false;
    return true;
}

bool best1502p_memsc_write(Best1502pMemsc *memsc, hwaddr offset,
                           unsigned size, uint64_t value)
{
    unsigned index;
    if (!memsc_index(memsc, offset, size, &index)) {
        return false;
    }
    if (value & 1) {
        memsc->free[memsc->bank][index] = true;
    }
    return true;
}
