#ifndef MIBAND_FLASH_STORAGE_H
#define MIBAND_FLASH_STORAGE_H
#include "miband_nor_device.h"
#include "miband_nand_device.h"
#include "miband_install_catalog.h"

typedef struct MiBandFlashStorage {
    MiBandNorDevice *nor;
    MiBandNandDevice *nand;
    MiBandStore *store;
    MiBandInstallCatalog installs;
} MiBandFlashStorage;
void miband_flash_storage_open(MiBandFlashStorage *storage, const char *path,
                                const uint8_t base[32]);
void miband_flash_storage_close(MiBandFlashStorage *storage);
#endif
