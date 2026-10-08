#ifndef MIBAND_INSTALL_CATALOG_H
#define MIBAND_INSTALL_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "miband_store.h"

typedef struct MiBandNandDevice MiBandNandDevice;

typedef struct MiBandInstallCatalog {
    uint8_t *blob[256];
    size_t length[256];
    MiBandStore *store;
    MiBandNandDevice *nand;
    unsigned nand_data_first_page;
} MiBandInstallCatalog;

/* MASS records are never shorter than 26 bytes. This reserved journal
 * payload is used to persist a Web UI uninstall across QEMU restarts. */
#define MIBAND_INSTALL_TOMBSTONE_SIZE 26

void miband_install_catalog_clear(MiBandInstallCatalog *catalog);
void miband_install_catalog_set_store(MiBandInstallCatalog *catalog,
                                      MiBandStore *store);
void miband_install_catalog_set_nand(MiBandInstallCatalog *catalog,
                                     MiBandNandDevice *nand,
                                     unsigned data_first_page);
void miband_install_catalog_restore(void *opaque, unsigned type,
                                   unsigned address, const uint8_t *data,
                                   unsigned size);
bool miband_install_catalog_commit(void *opaque, uint8_t type,
                                   const uint8_t *data, size_t length);
bool miband_install_catalog_delete(void *opaque, uint8_t type);
const uint8_t *miband_install_catalog_get(const MiBandInstallCatalog *catalog,
                                          uint8_t type, size_t *length);

#endif
