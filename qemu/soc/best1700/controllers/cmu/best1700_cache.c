/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_cache.h"

#define CACHE_CONTROL 0x00
#define CACHE_INITIALIZE 0x04
#define CACHE_CONFIG 0x08
#define CACHE_INVALIDATE_LINE 0x18
#define CACHE_WRAP 0x1c
#define CACHE_INVALIDATE_ALL 0x38

static uint32_t *best1700_cache_latch(Best1700Cache *s, hwaddr offset)
{
    switch (offset) {
    case CACHE_CONTROL:
        return &s->control;
    case CACHE_CONFIG:
        return &s->config;
    case CACHE_WRAP:
        return &s->wrap;
    default:
        return NULL;
    }
}

static bool best1700_cache_command(hwaddr offset)
{
    return offset == CACHE_INITIALIZE || offset == CACHE_INVALIDATE_LINE ||
        offset == CACHE_INVALIDATE_ALL;
}

static uint64_t best1700_cache_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700Cache *s = opaque;
    uint32_t *latch = best1700_cache_latch(s, offset & ~3u);

    if (latch) {
        uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : UINT32_MAX;

        return (*latch >> ((offset & 3) * 8)) & mask;
    }
    if (!best1700_cache_command(offset & ~3u) && s->unsupported++ < 16) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 cache unsupported read offset=0x%" HWADDR_PRIx "\n",
            offset);
    }
    /* Maintenance is synchronous in the coherent functional cache model. */
    return 0;
}

static void best1700_cache_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Best1700Cache *s = opaque;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size == 1 ? 0xffu : size == 2 ? 0xffffu :
                     UINT32_MAX) << shift;
    uint32_t data = (uint32_t)value << shift;
    uint32_t *latch = best1700_cache_latch(s, offset & ~3u);

    if (latch) {
        *latch = (*latch & ~mask) | (data & mask);
        return;
    }
    switch (offset & ~3u) {
    case CACHE_INITIALIZE:
        s->initialize_count += (data & 1) != 0;
        break;
    case CACHE_INVALIDATE_ALL:
        s->invalidate_all_count += (data & 1) != 0;
        break;
    case CACHE_INVALIDATE_LINE:
        s->invalidate_line_count++;
        s->last_line = data & ~31u;
        break;
    default:
        if (s->unsupported++ < 16) {
            qemu_log_mask(LOG_UNIMP,
                "best1700 cache unsupported write offset=0x%" HWADDR_PRIx
                " value=0x%08x\n", offset, data);
        }
        break;
    }
    /* QEMU's RAM/ROM aliases are coherent and dirty writes already invalidate
     * translated instructions. Maintenance neither changes memory contents
     * nor needs a fabricated busy period or an interrupt. */
}

static const MemoryRegionOps best1700_cache_ops = {
    .read = best1700_cache_read,
    .write = best1700_cache_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void best1700_cache_reset(Best1700Cache *s)
{
    s->control = 0;
    s->config = 0;
    s->wrap = 0;
    s->initialize_count = 0;
    s->invalidate_all_count = 0;
    s->invalidate_line_count = 0;
    s->last_line = 0;
    s->unsupported = 0;
}

void best1700_cache_init(Best1700Cache *s, Object *owner, const char *name)
{
    best1700_cache_reset(s);
    memory_region_init_io(&s->region, owner, &best1700_cache_ops, s, name,
                          BEST1700_CACHE_REG_SIZE);
}
