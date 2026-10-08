/* Display memory ownership validation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "exec/ramlist.h"
#include "exec/target_page.h"
#include "miband_display_memory.h"

typedef struct DisplayBlock {
    MemoryRegion *mr;
    uint64_t *versions;
    size_t pages;
    uint32_t size;
} DisplayBlock;

typedef struct WatchedPage {
    MiBandDisplayBlock block;
    size_t page;
    uint64_t version;
} WatchedPage;

struct MiBandDisplayWatch {
    uint64_t generation;
    GArray *pages;
};

static struct {
    DisplayBlock blocks[MIBAND_DISPLAY_BLOCKS];
    size_t page_size;
    uint64_t generation;
    bool ready;
} display_memory;

bool miband_display_normalize(uint32_t address, size_t length,
                              uint32_t ram_size, uint32_t psram_size,
                              MiBandDisplayRange *range)
{
    static const uint32_t ram_aliases[] = {0x20000000, 0x00200000};
    static const uint32_t psram_aliases[] = {
        0x3c000000, 0x38000000, 0x1c000000, 0x18000000,
    };
    unsigned block, i;

    if (!length || length > UINT32_MAX) {
        return false;
    }
    for (block = 0; block < MIBAND_DISPLAY_BLOCKS; block++) {
        const uint32_t *aliases = block ? psram_aliases : ram_aliases;
        unsigned count = block ? ARRAY_SIZE(psram_aliases) :
                                 ARRAY_SIZE(ram_aliases);
        uint32_t size = block ? psram_size : ram_size;
        for (i = 0; i < count; i++) {
            if (address >= aliases[i] && address - aliases[i] < size &&
                length <= size - (address - aliases[i])) {
                *range = (MiBandDisplayRange) {
                    .block = block, .offset = address - aliases[i],
                    .length = length,
                };
                return true;
            }
        }
    }
    return false;
}

bool miband_display_ranges_overlap(const MiBandDisplayRange *a,
                                   const MiBandDisplayRange *b)
{
    return a->block == b->block &&
           (uint64_t)a->offset < (uint64_t)b->offset + b->length &&
           (uint64_t)b->offset < (uint64_t)a->offset + a->length;
}

bool miband_display_memory_init(MemoryRegion *ram, MemoryRegion *psram)
{
    MemoryRegion *regions[] = {ram, psram};
    unsigned i;

    if (display_memory.ready || !ram || !psram) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(regions); i++) {
        if (!memory_region_is_ram(regions[i]) ||
            !memory_region_size(regions[i]) ||
            memory_region_size(regions[i]) > UINT32_MAX ||
            (memory_region_get_dirty_log_mask(regions[i]) &
             (1 << DIRTY_MEMORY_VGA))) {
            return false;
        }
    }
    display_memory.page_size = qemu_target_page_size();
    display_memory.generation++;
    for (i = 0; i < ARRAY_SIZE(regions); i++) {
        DisplayBlock *block = &display_memory.blocks[i];
        block->mr = regions[i];
        block->size = memory_region_size(regions[i]);
        block->pages = DIV_ROUND_UP(block->size, display_memory.page_size);
        block->versions = g_new0(uint64_t, block->pages);
        memory_region_set_log(block->mr, true, DIRTY_MEMORY_VGA);
    }
    display_memory.ready = true;
    return true;
}

bool miband_display_memory_ready(void)
{
    return display_memory.ready;
}

void miband_display_memory_cleanup(void)
{
    unsigned i;
    if (!display_memory.ready) {
        return;
    }
    display_memory.ready = false;
    display_memory.generation++;
    for (i = 0; i < MIBAND_DISPLAY_BLOCKS; i++) {
        DisplayBlock *block = &display_memory.blocks[i];
        memory_region_set_log(block->mr, false, DIRTY_MEMORY_VGA);
        g_free(block->versions);
        memset(block, 0, sizeof(*block));
    }
}

void miband_display_memory_note_write(uint32_t address, size_t length)
{
    MiBandDisplayRange range;
    DisplayBlock *block;
    size_t first, last, i;

    if (!display_memory.ready ||
        !miband_display_normalize(address, length,
            display_memory.blocks[0].size, display_memory.blocks[1].size,
            &range)) {
        return;
    }
    block = &display_memory.blocks[range.block];
    memory_region_set_dirty(block->mr, range.offset, range.length);
    first = range.offset / display_memory.page_size;
    last = (range.offset + range.length - 1) / display_memory.page_size;
    for (i = first; i <= last; i++) {
        block->versions[i]++;
    }
}

static void display_memory_collect(void)
{
    unsigned i;
    size_t page;

    /* Collect a whole block: dirty snapshots round to bitmap-word boundaries.
     * Clearing separate overlapping watches would lose another watch's writes. */
    for (i = 0; i < MIBAND_DISPLAY_BLOCKS; i++) {
        DisplayBlock *block = &display_memory.blocks[i];
        DirtyBitmapSnapshot *snapshot = memory_region_snapshot_and_clear_dirty(
            block->mr, 0, block->size, DIRTY_MEMORY_VGA);
        for (page = 0; page < block->pages; page++) {
            size_t offset = page * display_memory.page_size;
            size_t bytes = MIN(display_memory.page_size, block->size - offset);
            if (memory_region_snapshot_get_dirty(block->mr, snapshot,
                                                 offset, bytes)) {
                block->versions[page]++;
            }
        }
        g_free(snapshot);
    }
}

MiBandDisplayWatch *miband_display_memory_watch(
    const MiBandDisplayRange *ranges, size_t count)
{
    MiBandDisplayWatch *watch;
    size_t i, page;

    if (!display_memory.ready || !count || !ranges) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        if ((unsigned)ranges[i].block >= MIBAND_DISPLAY_BLOCKS ||
            !ranges[i].length ||
            (uint64_t)ranges[i].offset + ranges[i].length >
                display_memory.blocks[ranges[i].block].size) {
            return NULL;
        }
    }
    display_memory_collect();
    watch = g_new0(MiBandDisplayWatch, 1);
    watch->generation = display_memory.generation;
    watch->pages = g_array_new(false, false, sizeof(WatchedPage));
    for (i = 0; i < count; i++) {
        const MiBandDisplayRange *range = &ranges[i];
        size_t first = range->offset / display_memory.page_size;
        size_t last = (range->offset + range->length - 1) /
                       display_memory.page_size;
        for (page = first; page <= last; page++) {
            WatchedPage record = {
                .block = range->block, .page = page,
                .version = display_memory.blocks[range->block].versions[page],
            };
            g_array_append_val(watch->pages, record);
        }
    }
    return watch;
}

bool miband_display_memory_unchanged(MiBandDisplayWatch *watch)
{
    unsigned i;
    if (!watch || !display_memory.ready ||
        watch->generation != display_memory.generation) {
        return false;
    }
    display_memory_collect();
    for (i = 0; i < watch->pages->len; i++) {
        WatchedPage *page = &g_array_index(watch->pages, WatchedPage, i);
        if (page->version !=
            display_memory.blocks[page->block].versions[page->page]) {
            return false;
        }
    }
    return true;
}

void miband_display_memory_watch_free(MiBandDisplayWatch *watch)
{
    if (watch) {
        g_array_free(watch->pages, true);
        g_free(watch);
    }
}
