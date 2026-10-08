/* BEST1700 BTH CMU hardware semaphores.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "best1700_bth_memsc.h"

#define BTH_MEMSC_BASE 0xc0
#define BTH_MEMSC_STATUS (BEST1700_BTH_MEMSC_COUNT * sizeof(uint32_t))
#define BTH_MEMSC_FREE_MASK ((1u << BEST1700_BTH_MEMSC_COUNT) - 1)

static uint64_t best1700_bth_memsc_read(void *opaque, hwaddr address,
                                       unsigned size)
{
    Best1700BthMemsc *s = opaque;
    unsigned bit;

    if (address == BTH_MEMSC_STATUS) {
        return qatomic_read(&s->free_mask);
    }

    /* Reading returns the previous availability and atomically takes the
     * semaphore. A second master observes zero until a guest releases it. */
    bit = 1u << (address / sizeof(uint32_t));

    return (qatomic_fetch_and(&s->free_mask, ~bit) & bit) != 0;
}

static void best1700_bth_memsc_write(void *opaque, hwaddr address,
                                    uint64_t value, unsigned size)
{
    Best1700BthMemsc *s = opaque;

    if (address < BTH_MEMSC_STATUS && (value & 1)) {
        qatomic_or(&s->free_mask, 1u << (address / sizeof(uint32_t)));
    }
}

static const MemoryRegionOps best1700_bth_memsc_ops = {
    .read = best1700_bth_memsc_read,
    .write = best1700_bth_memsc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* The established firmware contract uses aligned 32-bit transactions.
     * Do not expand subword reads into an unintended lock acquisition. */
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_bth_memsc_reset(Best1700BthMemsc *s)
{
    qatomic_set(&s->free_mask, BTH_MEMSC_FREE_MASK);
}

void best1700_bth_memsc_init(Best1700BthMemsc *s, Object *owner)
{
    memory_region_init(&s->cmu, owner, "best1700.bth-memsc-cmu",
                        BEST1700_BTH_MEMSC_WINDOW);
    memory_region_init_io(&s->registers, owner, &best1700_bth_memsc_ops, s,
                          "best1700.bth-memsc", BTH_MEMSC_STATUS + 4);
    memory_region_add_subregion(&s->cmu, BTH_MEMSC_BASE, &s->registers);
    best1700_bth_memsc_reset(s);
}
