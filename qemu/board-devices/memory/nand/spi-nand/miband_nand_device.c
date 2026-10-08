/* External SPI-NAND compatibility subset. No guest addresses or SoC state.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "miband_nand_device.h"
#include <glib.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>

struct MiBandNandDevice {
    MiBandNandGeometry geometry;
    unsigned raw_page_size, first_page;
    const uint8_t *image;
    size_t image_size;
    bool raw_image, stage_valid;
    uint8_t *read_cache, *program_cache;
    GHashTable *pages, *erased_blocks;
    GHashTable *overlay_pages;
    unsigned overlay_first_page;
    MiBandStore *store;
};

MiBandNandDevice *miband_nand_new(const MiBandNandGeometry *geometry)
{
    assert(geometry->page_size && geometry->spare_size <= 0x10000);
    assert(geometry->page_size <= 0x10000 - geometry->spare_size);
    assert(geometry->pages_per_block &&
           !(geometry->pages_per_block & (geometry->pages_per_block - 1)));
    assert(geometry->page_count && geometry->page_count <= 0x1000000 &&
           geometry->page_count % geometry->pages_per_block == 0);
    MiBandNandDevice *nand = g_new0(MiBandNandDevice, 1);
    nand->geometry = *geometry;
    nand->raw_page_size = geometry->page_size + geometry->spare_size;
    nand->read_cache = g_malloc(nand->raw_page_size);
    nand->program_cache = g_malloc0(nand->raw_page_size);
    memset(nand->read_cache, 0xff, nand->raw_page_size);
    nand->pages = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    nand->erased_blocks = g_hash_table_new(g_direct_hash, g_direct_equal);
    nand->overlay_pages = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    return nand;
}

void miband_nand_free(MiBandNandDevice *nand)
{
    if (!nand) return;
    g_hash_table_destroy(nand->pages);
    g_hash_table_destroy(nand->erased_blocks);
    g_hash_table_destroy(nand->overlay_pages);
    g_free(nand->read_cache);
    g_free(nand->program_cache);
    g_free(nand);
}

void miband_nand_set_image(MiBandNandDevice *nand, const uint8_t *image,
                            size_t size, bool raw)
{
    nand->image = image;
    nand->image_size = size;
    nand->raw_image = raw;
    nand->first_page = 0;
}

void miband_nand_set_resource(MiBandNandDevice *nand, const uint8_t *image,
                               size_t size, unsigned first_page, bool raw)
{
    assert(first_page < nand->geometry.page_count);
    miband_nand_set_image(nand, image, size, raw);
    nand->first_page = first_page;
}

void miband_nand_set_store(MiBandNandDevice *nand, MiBandStore *store) { nand->store = store; }

/* YAFFS2 uses a 16-byte tag.  reference stores it in NAND spare bytes while reference's
 * driver explicitly uses the final 16 bytes of the 2048-byte data area. */
static unsigned overlay_data_size(const MiBandNandDevice *nand)
{
    return nand->geometry.spare_size >= 128 ? nand->geometry.page_size - 16 :
                                               nand->geometry.page_size;
}

static void overlay_tag(const MiBandNandDevice *nand, uint8_t *page,
                        unsigned page_index, unsigned object, unsigned chunk,
                        unsigned count)
{
    uint8_t *tag = page + overlay_data_size(nand);
    uint32_t values[4] = { 4096 + page_index / nand->geometry.pages_per_block,
                           object, chunk, count };
    memcpy(tag, values, sizeof(values));
}

static unsigned overlay_page(MiBandNandDevice *nand, unsigned row,
                              unsigned page_index, unsigned object,
                              unsigned chunk, unsigned count,
                              const uint8_t *data, unsigned length)
{
    uint8_t *page = g_malloc(nand->raw_page_size);
    unsigned data_size = overlay_data_size(nand);
    memset(page, 0xff, nand->raw_page_size);
    if (data && length) memcpy(page, data, MIN(length, data_size));
    overlay_tag(nand, page, page_index, object, chunk, count);
    g_hash_table_replace(nand->overlay_pages, GUINT_TO_POINTER(row), page);
    return row + 1;
}

static void overlay_header(uint8_t *header, unsigned kind, unsigned parent,
                           const char *name, unsigned size)
{
    size_t length = strlen(name);
    memset(header, 0xff, 2032);
    if (length > 255) length = 255;
    uint16_t mode = kind == 3 ? (uint16_t)(S_IFDIR | 0755) : (uint16_t)(S_IFREG | 0644);
    uint32_t mode32 = mode;
    memcpy(header + 0, &kind, 4);
    memcpy(header + 4, &parent, 4);
    uint16_t checksum = 0xffff;
    memcpy(header + 8, &checksum, 2);
    memcpy(header + 10, name, length);
    memcpy(header + 268, &mode32, 4);
    memcpy(header + 292, &size, 4);
}

static unsigned overlay_file(MiBandNandDevice *nand, unsigned row,
                             unsigned *page_index, unsigned object,
                             unsigned parent, const char *name,
                             const uint8_t *data, size_t length)
{
    uint8_t header[2032];
    unsigned data_size = overlay_data_size(nand);
    unsigned pages = (unsigned)((length + data_size - 1) / data_size);
    overlay_header(header, 1, parent, name, (unsigned)length);
    row = overlay_page(nand, row, (*page_index)++, object, 0, 0xffff,
                       header, sizeof(header));
    for (unsigned chunk = 1; chunk <= pages; chunk++) {
        size_t offset = (size_t)(chunk - 1) * data_size;
        unsigned count = (unsigned)MIN((size_t)data_size, length - offset);
        row = overlay_page(nand, row, (*page_index)++, object, chunk, count,
                           data + offset, count);
    }
    return row;
}

void miband_nand_set_mass_files(MiBandNandDevice *nand, unsigned first_page,
                                const uint8_t *watchface, size_t watchface_size,
                                const uint8_t *quickapp, size_t quickapp_size)
{
    unsigned row, page_index = 0, object = 257;
    uint8_t header[2032];
    char watchface_id[25] = { 0 };
    if (!nand) return;
    g_hash_table_remove_all(nand->overlay_pages);
    nand->overlay_first_page = first_page;
    if ((!watchface || !watchface_size) && (!quickapp || !quickapp_size)) return;
    row = first_page;
    /* /data/mass, /data/app, /data/app/watchface and /data/app/quickapp. */
    overlay_header(header, 3, 1, "mass", 0);
    row = overlay_page(nand, row, page_index++, object++, 0, 0xffff, header, sizeof(header));
    overlay_header(header, 3, 1, "app", 0);
    row = overlay_page(nand, row, page_index++, object++, 0, 0xffff, header, sizeof(header));
    overlay_header(header, 3, 258, "watchface", 0);
    row = overlay_page(nand, row, page_index++, object++, 0, 0xffff, header, sizeof(header));
    overlay_header(header, 3, 258, "quickapp", 0);
    row = overlay_page(nand, row, page_index++, object++, 0, 0xffff, header, sizeof(header));
    if (watchface && watchface_size) {
        /* The native watchface manager installs completed MASS data below
         * /data/app/watchface/market/<id>/resource.bin.  Keep the generic
         * MASS copy for diagnostics, and expose the same path so a later
         * Bluetooth set-current request follows the firmware's normal lookup
         * path.  Xiaomi .bin packages store a 24-byte ID field at +0x22. */
        if (watchface_size >= 34) {
            const uint8_t *id = watchface + 34;
            unsigned count = 0;
            while (count < 24 && id[count] >= 0x21 && id[count] <= 0x7e)
                count++;
            if (count >= 9 && count < sizeof(watchface_id)) {
                memcpy(watchface_id, id, count);
                watchface_id[count] = 0;
            }
        }
        row = overlay_file(nand, row, &page_index, object++, 257,
                           "watchface.bin", watchface, watchface_size);
        row = overlay_file(nand, row, &page_index, object++, 259,
                           "active.bin", watchface, watchface_size);
        if (watchface_id[0]) {
            unsigned market = object++;
            unsigned installed = object++;
            overlay_header(header, 3, 259, "market", 0);
            row = overlay_page(nand, row, page_index++, market, 0, 0xffff,
                               header, sizeof(header));
            overlay_header(header, 3, market, watchface_id, 0);
            row = overlay_page(nand, row, page_index++, installed, 0, 0xffff,
                               header, sizeof(header));
            row = overlay_file(nand, row, &page_index, object++, installed,
                               "resource.bin", watchface, watchface_size);
        }
    }
    if (quickapp && quickapp_size) {
        row = overlay_file(nand, row, &page_index, object++, 257,
                           "quickapp.rpk", quickapp, quickapp_size);
        row = overlay_file(nand, row, &page_index, object++, 260,
                           "active.rpk", quickapp, quickapp_size);
    }
}

const MiBandNandGeometry *miband_nand_geometry(const MiBandNandDevice *nand)
{
    return &nand->geometry;
}

bool miband_nand_store_compatible(MiBandNandDevice *nand)
{
    return nand->geometry.page_size == 2048 &&
           (nand->geometry.spare_size == 64 || nand->geometry.spare_size == 128) &&
           nand->geometry.pages_per_block == 64;
}

void miband_nand_restore_page(MiBandNandDevice *nand, unsigned row,
                               const uint8_t *data, unsigned size)
{
    assert(row < nand->geometry.page_count && size == nand->raw_page_size);
    g_hash_table_replace(nand->pages, GUINT_TO_POINTER(row), g_memdup2(data, size));
}

void miband_nand_restore_erase(MiBandNandDevice *nand, unsigned block)
{
    unsigned count = nand->geometry.pages_per_block;
    assert(block < nand->geometry.page_count / count);
    g_hash_table_add(nand->erased_blocks, GUINT_TO_POINTER(block));
    for (unsigned i = 0; i < count; i++)
        g_hash_table_remove(nand->pages, GUINT_TO_POINTER(block * count + i));
}

static void nand_load_page(MiBandNandDevice *nand, unsigned row, uint8_t *page)
{
    memset(page, 0xff, nand->raw_page_size);
    if (row >= nand->geometry.page_count ||
        g_hash_table_contains(nand->erased_blocks,
                             GUINT_TO_POINTER(row / nand->geometry.pages_per_block))) return;
    uint8_t *overlay = g_hash_table_lookup(nand->overlay_pages,
                                            GUINT_TO_POINTER(row));
    if (overlay) {
        memcpy(page, overlay, nand->raw_page_size);
        return;
    }
    uint64_t stride = nand->raw_image ? nand->raw_page_size : nand->geometry.page_size;
    if (row < nand->first_page) return;
    uint64_t offset = (uint64_t)(row - nand->first_page) * stride;
    if (nand->image && offset < nand->image_size)
        memcpy(page, nand->image + offset, MIN(stride, nand->image_size - offset));
}

static bool nand_poll(uint8_t opcode)
{
    return opcode == 0x0f || opcode == 0x05 || opcode == 0x35 || opcode == 0x15;
}

static bool nand_response(void *opaque, uint8_t opcode, unsigned index,
                           unsigned requested, uint8_t *value)
{
    MiBandNandDevice *nand = opaque;
    *value = 0;
    if (nand_poll(opcode)) return true;
    if (opcode != 0x9f) return false;
    if (requested == 3 && index < 3) *value = nand->geometry.jedec_id[index];
    return true;
}

static const uint8_t *nand_read_cache(void *opaque)
{
    return ((MiBandNandDevice *)opaque)->read_cache;
}

static uint8_t *nand_program_buffer(void *opaque, unsigned column, unsigned length)
{
    MiBandNandDevice *nand = opaque;
    if (!nand->stage_valid || column > nand->raw_page_size ||
        length > nand->raw_page_size - column) return NULL;
    return nand->program_cache + column;
}

static unsigned nand_command(void *opaque, uint8_t opcode, uint32_t address,
                          const uint8_t *tx, unsigned length)
{
    MiBandNandDevice *nand = opaque;
    if (opcode == 0x13) {
        uint8_t *page = g_hash_table_lookup(nand->pages, GUINT_TO_POINTER(address));
        if (page) memcpy(nand->read_cache, page, nand->raw_page_size);
        else nand_load_page(nand, address, nand->read_cache);
        return MB_FLASH_CACHE_REFRESH;
    }
    if (opcode == 0x02 || opcode == 0x32 || opcode == 0x84 || opcode == 0x34) {
        if (opcode == 0x02 || opcode == 0x32)
            memset(nand->program_cache, 0xff, nand->raw_page_size);
        nand->stage_valid = true;
        if (length && address < nand->raw_page_size)
            memcpy(nand->program_cache + address, tx,
                   MIN(length, nand->raw_page_size - address));
        return MB_FLASH_DMA_WRITE;
    }
    if (opcode == 0x10 && nand->stage_valid && address < nand->geometry.page_count) {
        uint8_t *page = g_hash_table_lookup(nand->pages, GUINT_TO_POINTER(address));
        if (!page) {
            page = g_malloc(nand->raw_page_size);
            nand_load_page(nand, address, page);
            g_hash_table_insert(nand->pages, GUINT_TO_POINTER(address), page);
        }
        for (unsigned i = 0; i < nand->raw_page_size; i++) page[i] &= nand->program_cache[i];
        nand->stage_valid = false;
        miband_store_append(nand->store, MB_STORE_PAGE, address, page, nand->raw_page_size);
    }
    if (opcode == 0xd8 && address < nand->geometry.page_count) {
        unsigned block = address / nand->geometry.pages_per_block;
        miband_store_append(nand->store, MB_STORE_ERASE, block, NULL, 0);
        miband_nand_restore_erase(nand, block);
    }
    /* Standard cache reads: single, fast, dual/quad output and dual/quad I/O.
     * GD5F2GM7 selects 0x6b; the older compatibility part used 0xeb. */
    switch (opcode) {
    case 0x03: case 0x0b: case 0x3b: case 0x6b: case 0xbb: case 0xeb:
        return MB_FLASH_DMA_READ;
    default:
        return 0;
    }
}

MiBandFlashEndpoint miband_nand_endpoint(MiBandNandDevice *nand)
{
    return (MiBandFlashEndpoint) {
        .opaque = nand, .command = nand_command, .response = nand_response,
        .is_poll = nand_poll, .cache_size = nand->raw_page_size,
        .read_cache = nand_read_cache, .program_buffer = nand_program_buffer,
    };
}
