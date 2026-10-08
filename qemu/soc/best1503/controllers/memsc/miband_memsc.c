#include "qemu/osdep.h"
#include "miband_peripherals.h"

#define AON_MEMSC_BASE 0x40080024
#define CMU_MEMSC_BASE 0x400000c0

static bool memsc_index(hwaddr address, unsigned size, unsigned *bank,
                        unsigned *index)
{
    hwaddr base;

    if (size != sizeof(uint32_t) || (address & 3)) {
        return false;
    }
    if (address >= AON_MEMSC_BASE &&
        address < AON_MEMSC_BASE + MIBAND_MEMSC_COUNT * sizeof(uint32_t)) {
        *bank = 0;
        base = AON_MEMSC_BASE;
    } else if (address >= CMU_MEMSC_BASE &&
               address < CMU_MEMSC_BASE +
                         MIBAND_MEMSC_COUNT * sizeof(uint32_t)) {
        *bank = 1;
        base = CMU_MEMSC_BASE;
    } else {
        return false;
    }
    *index = (address - base) / sizeof(uint32_t);
    return true;
}

void miband_memsc_init(MiBandMemsc *memsc)
{
    for (unsigned bank = 0; bank < MIBAND_MEMSC_BANKS; bank++) {
        for (unsigned i = 0; i < MIBAND_MEMSC_COUNT; i++) {
            memsc->free[bank][i] = true;
        }
    }
}

bool miband_memsc_read(MiBandMemsc *memsc, hwaddr address, unsigned size,
                       uint64_t *result)
{
    unsigned index;
    unsigned bank;

    if (!memsc_index(address, size, &bank, &index)) {
        return false;
    }
    *result = memsc->free[bank][index];
    memsc->free[bank][index] = false;
    return true;
}

bool miband_memsc_write(MiBandMemsc *memsc, hwaddr address, unsigned size,
                        uint64_t value)
{
    unsigned index;
    unsigned bank;

    if (!memsc_index(address, size, &bank, &index)) {
        return false;
    }
    if (value & 1) {
        memsc->free[bank][index] = true;
    }
    return true;
}
