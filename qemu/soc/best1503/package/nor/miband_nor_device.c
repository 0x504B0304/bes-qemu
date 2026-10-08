/* NOR die compatibility subset, assembled inside the BEST1503 package.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "miband_nor_device.h"
#include <glib.h>
#include <assert.h>
#include <string.h>

struct MiBandNorDevice {
    uint8_t *window;
    uint32_t address;
    unsigned size, sector_size;
    uint8_t jedec_id[3];
    MiBandStore *store;
};

MiBandNorDevice *miband_nor_new(uint8_t *window, uint32_t address,
    unsigned size, unsigned sector_size, const uint8_t jedec_id[3])
{
    MiBandNorDevice *nor = g_new0(MiBandNorDevice, 1);
    assert(window && size && sector_size && !(sector_size & (sector_size - 1)));
    assert(sector_size <= size && address <= UINT32_MAX - size);
    nor->window = window;
    nor->address = address;
    nor->size = size;
    nor->sector_size = sector_size;
    memcpy(nor->jedec_id, jedec_id, sizeof(nor->jedec_id));
    memset(window, 0xff, size);
    return nor;
}

void miband_nor_free(MiBandNorDevice *nor) { g_free(nor); }
void miband_nor_set_store(MiBandNorDevice *nor, MiBandStore *store) { nor->store = store; }
const uint8_t *miband_nor_seed(MiBandNorDevice *nor) { return nor->window; }
unsigned miband_nor_window_size(MiBandNorDevice *nor) { return nor->size; }

void miband_nor_restore(MiBandNorDevice *nor, unsigned address,
                         const uint8_t *data, unsigned size)
{
    assert(address <= nor->size && size <= nor->size - address);
    memcpy(nor->window + address, data, size);
}

static bool nor_poll(uint8_t opcode)
{
    return opcode == 0x05 || opcode == 0x35 || opcode == 0x15;
}

static bool nor_response(void *opaque, uint8_t opcode, unsigned index,
                          unsigned requested, uint8_t *value)
{
    MiBandNorDevice *nor = opaque;
    *value = 0;
    if (nor_poll(opcode)) return true;
    if (opcode != 0x9f) return false;
    if (requested == 3 && index < 3) {
        *value = nor->jedec_id[index];
    } else if (requested == 6 && index < 6) {
        /* Existing dual-die package wiring interleaves each ID nibble. */
        uint8_t byte = nor->jedec_id[index / 2];
        *value = ((index & 1) ? (byte & 15) : (byte >> 4)) * 0x11;
    }
    return true;
}

static unsigned nor_command(void *opaque, uint8_t opcode, uint32_t address,
                         const uint8_t *tx, unsigned length)
{
    MiBandNorDevice *nor = opaque;
    if (opcode == 0x20) {
        address &= ~(nor->sector_size - 1);
        if (address >= nor->address &&
            address - nor->address <= nor->size - nor->sector_size) {
            unsigned offset = address - nor->address;
            memset(nor->window + offset, 0xff, nor->sector_size);
            miband_store_append(nor->store, MB_STORE_NOR, offset,
                                nor->window + offset, nor->sector_size);
        }
    }
    return 0;
}

static void nor_commit(void *opaque, uint8_t opcode, uint32_t address,
                        const uint8_t *tx, unsigned length)
{
    MiBandNorDevice *nor = opaque;
    if (opcode != 0x32 || address < nor->address ||
        address - nor->address >= nor->size ||
        length > nor->size - (address - nor->address)) return;
    unsigned offset = address - nor->address;
    for (unsigned i = 0; i < length; i++) nor->window[offset + i] &= tx[i];
    if (length) miband_store_append(nor->store, MB_STORE_NOR, offset,
                                    nor->window + offset, length);
}

MiBandFlashEndpoint miband_nor_endpoint(MiBandNorDevice *nor)
{
    return (MiBandFlashEndpoint) {
        .opaque = nor, .command = nor_command, .commit_tx = nor_commit,
        .response = nor_response, .is_poll = nor_poll,
    };
}
