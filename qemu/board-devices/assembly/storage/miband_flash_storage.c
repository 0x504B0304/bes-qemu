/* Board assembly routes a shared journal to its package NOR and external NAND.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "miband_flash_storage.h"

static void storage_restore(void *opaque, unsigned type, unsigned address,
                             const uint8_t *data, unsigned size)
{
    MiBandFlashStorage *storage = opaque;
    if (type == MB_STORE_NOR) miband_nor_restore(storage->nor, address, data, size);
    else if (type == MB_STORE_PAGE) miband_nand_restore_page(storage->nand, address, data, size);
    else if (type == MB_STORE_ERASE) miband_nand_restore_erase(storage->nand, address);
    else miband_install_catalog_restore(&storage->installs, type, address, data, size);
}

void miband_flash_storage_open(MiBandFlashStorage *storage, const char *path,
                                const uint8_t base[32])
{
    if (!miband_nand_store_compatible(storage->nand)) {
        error_setg(&error_fatal, "current flash journal does not support this device geometry");
        return;
    }
    const MiBandNandGeometry *geometry = miband_nand_geometry(storage->nand);
    storage->store = miband_store_open_geometry(path, base, miband_nor_seed(storage->nor),
                                       miband_nor_window_size(storage->nor),
                                       geometry->page_size + geometry->spare_size,
                                       geometry->page_count,
                                       storage_restore, storage);
    miband_install_catalog_set_store(&storage->installs, storage->store);
    miband_nor_set_store(storage->nor, storage->store);
    miband_nand_set_store(storage->nand, storage->store);
}

void miband_flash_storage_close(MiBandFlashStorage *storage)
{
    if (storage->nor) miband_nor_set_store(storage->nor, NULL);
    if (storage->nand) miband_nand_set_store(storage->nand, NULL);
    miband_store_close(storage->store);
    storage->store = NULL;
    miband_install_catalog_clear(&storage->installs);
}
