/* BEST1700 shared hardware semaphore primitive.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "best1700_memsc.h"

void best1700_memsc_init(Best1700Memsc *s, unsigned count)
{
    g_assert(count > 0 && count <= 32);
    s->count = count;
    best1700_memsc_reset(s);
}

void best1700_memsc_reset(Best1700Memsc *s)
{
    uint32_t mask = s->count == 32 ? UINT32_MAX : ((1u << s->count) - 1);
    qatomic_set(&s->free_mask, mask);
}

uint32_t best1700_memsc_claim(Best1700Memsc *s, unsigned index)
{
    uint32_t bit;

    if (index >= s->count) {
        return 0;
    }
    bit = 1u << index;
    return (qatomic_fetch_and(&s->free_mask, ~bit) & bit) != 0;
}

void best1700_memsc_release(Best1700Memsc *s, unsigned index, uint32_t value)
{
    if (index < s->count && (value & 1)) {
        qatomic_or(&s->free_mask, 1u << index);
    }
}

uint32_t best1700_memsc_status(const Best1700Memsc *s)
{
    return qatomic_read(&s->free_mask);
}