#include "qemu/osdep.h"
#include "miband_install_catalog.h"
#include "miband_nand_device.h"
#include "miband_xiaomi_protocol.h"

static const uint8_t install_tombstone[MIBAND_INSTALL_TOMBSTONE_SIZE] = {
    'M', 'I', 'B', 'A', 'N', 'D', '-', 'D', 'E', 'L', 'E', 'T', 'E',
};

static void install_catalog_sync_nand(MiBandInstallCatalog *catalog)
{
    if (!catalog || !catalog->nand) return;
    miband_nand_set_mass_files(catalog->nand, catalog->nand_data_first_page,
                               catalog->blob[MIBAND_XIAOMI_MASS_WATCHFACE],
                               catalog->length[MIBAND_XIAOMI_MASS_WATCHFACE],
                               catalog->blob[MIBAND_XIAOMI_MASS_THIRD_PARTY_APP],
                               catalog->length[MIBAND_XIAOMI_MASS_THIRD_PARTY_APP]);
}

void miband_install_catalog_clear(MiBandInstallCatalog *catalog)
{
    if (!catalog) {
        return;
    }
    for (unsigned i = 0; i < 256; i++) {
        g_free(catalog->blob[i]);
        catalog->blob[i] = NULL;
        catalog->length[i] = 0;
    }
    catalog->store = NULL;
    catalog->nand = NULL;
    catalog->nand_data_first_page = 0;
}

void miband_install_catalog_set_store(MiBandInstallCatalog *catalog,
                                      MiBandStore *store)
{
    if (catalog) {
        catalog->store = store;
    }
}

void miband_install_catalog_set_nand(MiBandInstallCatalog *catalog,
                                     MiBandNandDevice *nand,
                                     unsigned data_first_page)
{
    if (!catalog) return;
    catalog->nand = nand;
    catalog->nand_data_first_page = data_first_page;
    install_catalog_sync_nand(catalog);
}

void miband_install_catalog_restore(void *opaque, unsigned type,
                                   unsigned address, const uint8_t *data,
                                   unsigned size)
{
    MiBandInstallCatalog *catalog = opaque;
    uint8_t *copy;

    if (!catalog || type != MB_STORE_MASS || address > 255 || !data || !size) {
        return;
    }
    if (size == sizeof(install_tombstone) &&
        !memcmp(data, install_tombstone, sizeof(install_tombstone))) {
        g_free(catalog->blob[address]);
        catalog->blob[address] = NULL;
        catalog->length[address] = 0;
        return;
    }
    copy = g_memdup2(data, size);
    g_free(catalog->blob[address]);
    catalog->blob[address] = copy;
    catalog->length[address] = size;
    install_catalog_sync_nand(catalog);
}

bool miband_install_catalog_commit(void *opaque, uint8_t type,
                                   const uint8_t *data, size_t length)
{
    MiBandInstallCatalog *catalog = opaque;

    if (!catalog || !catalog->store || !data || !length ||
        length > G_MAXUINT) {
        return false;
    }
    miband_store_append(catalog->store, MB_STORE_MASS, type, data,
                        (unsigned)length);
    miband_install_catalog_restore(catalog, MB_STORE_MASS, type, data,
                                   (unsigned)length);
    return true;
}

bool miband_install_catalog_delete(void *opaque, uint8_t type)
{
    MiBandInstallCatalog *catalog = opaque;

    if (!catalog || !catalog->store || !catalog->blob[type]) {
        return false;
    }
    miband_store_append(catalog->store, MB_STORE_MASS, type,
                        install_tombstone, sizeof(install_tombstone));
    miband_install_catalog_restore(catalog, MB_STORE_MASS, type,
                                   install_tombstone,
                                   sizeof(install_tombstone));
    return true;
}

const uint8_t *miband_install_catalog_get(const MiBandInstallCatalog *catalog,
                                          uint8_t type, size_t *length)
{
    if (length) {
        *length = catalog ? catalog->length[type] : 0;
    }
    return catalog ? catalog->blob[type] : NULL;
}
