/* Display memory ownership validation. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_DISPLAY_MEMORY_H
#define MIBAND_DISPLAY_MEMORY_H

#include "exec/memory.h"

typedef enum MiBandDisplayBlock {
    MIBAND_DISPLAY_AP_RAM,
    MIBAND_DISPLAY_PSRAM,
    MIBAND_DISPLAY_BLOCKS,
} MiBandDisplayBlock;

typedef struct MiBandDisplayRange {
    MiBandDisplayBlock block;
    uint32_t offset;
    uint32_t length;
} MiBandDisplayRange;

typedef struct MiBandDisplayWatch MiBandDisplayWatch;

/* All AP RAM/PSRAM aliases resolve to the same block and byte offset. */
bool miband_display_normalize(uint32_t address, size_t length,
                              uint32_t ram_size, uint32_t psram_size,
                              MiBandDisplayRange *range);
bool miband_display_ranges_overlap(const MiBandDisplayRange *a,
                                   const MiBandDisplayRange *b);

/*
 * One board owns the otherwise unused VGA dirty client. Never share its bitmap
 * with another consumer. Initialization/cleanup happen while the CPU is stopped;
 * watch/check happen at the only vCPU's safe point, under the BQL. Raw DMA host
 * writes must call note_write after memcpy (address_space_write already dirties
 * RAM). Versions detect write-then-restore, not just different final bytes.
 */
bool miband_display_memory_init(MemoryRegion *ram, MemoryRegion *psram);
void miband_display_memory_cleanup(void);
bool miband_display_memory_ready(void);
void miband_display_memory_note_write(uint32_t address, size_t length);
MiBandDisplayWatch *miband_display_memory_watch(
    const MiBandDisplayRange *ranges, size_t count);
bool miband_display_memory_unchanged(MiBandDisplayWatch *watch);
void miband_display_memory_watch_free(MiBandDisplayWatch *watch);

#endif
