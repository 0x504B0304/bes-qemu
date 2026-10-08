/* BEST1700 RF switch matrix configuration readback.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "best1700_rf_matrix.h"

static uint64_t rf_matrix_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700RfMatrix *s = opaque;
    return s->words[offset / sizeof(uint32_t)];
}

static void rf_matrix_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Best1700RfMatrix *s = opaque;
    /* Eight opaque packed route words only. No pin or radio engine is
     * attached, and a write never starts a transaction or completes one. */
    s->words[offset / sizeof(uint32_t)] = value;
}

static const MemoryRegionOps rf_matrix_ops = {
    .read = rf_matrix_read,
    .write = rf_matrix_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_rf_matrix_reset(Best1700RfMatrix *s)
{
    /* Inactive initial configuration is emulator policy. CPU resets do not
     * own these shared route words. Whole-machine reset clears them. */
    memset(s->words, 0, sizeof(s->words));
}

void best1700_rf_matrix_init(Best1700RfMatrix *s, Object *owner)
{
    memory_region_init_io(&s->iomem, owner, &rf_matrix_ops, s,
                          "best1700.rf-matrix", BEST1700_RF_MATRIX_SIZE);
    best1700_rf_matrix_reset(s);
}
