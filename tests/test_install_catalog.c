#include "miband_install_catalog.h"
#include <assert.h>
#include <string.h>

/* This journal fixture has no attached NAND; bus/device coverage is separate. */
void miband_nand_set_mass_files(MiBandNandDevice *nand, unsigned first_page,
                                const uint8_t *watchface, size_t watchface_size,
                                const uint8_t *quickapp, size_t quickapp_size)
{
    assert(!"journal-only catalog unexpectedly synchronized an attached NAND");
}

static void restore(void *opaque, unsigned type, unsigned address,
                    const uint8_t *data, unsigned size)
{
    miband_install_catalog_restore(opaque, type, address, data, size);
}

int main(int argc, char **argv)
{
    uint8_t base[32] = { 0x42 }, nor[0x40000];
    uint8_t blob[26] = { 0, 16 };
    MiBandInstallCatalog first = { 0 }, second = { 0 }, third = { 0 };
    MiBandStore *store;
    size_t size;

    assert(argc == 2);
    memset(nor, 0xff, sizeof(nor));
    store = miband_store_open(argv[1], base, nor, restore, &first);
    miband_install_catalog_set_store(&first, store);
    assert(miband_install_catalog_commit(&first, 16, blob, sizeof(blob)));
    miband_store_close(store);
    miband_install_catalog_clear(&first);

    store = miband_store_open(argv[1], base, nor, restore, &second);
    assert(miband_install_catalog_get(&second, 16, &size) == second.blob[16]);
    assert(size == sizeof(blob));
    assert(!memcmp(second.blob[16], blob, sizeof(blob)));
    miband_install_catalog_set_store(&second, store);
    assert(miband_install_catalog_delete(&second, 16));
    miband_store_close(store);
    miband_install_catalog_clear(&second);

    store = miband_store_open(argv[1], base, nor, restore, &third);
    assert(miband_install_catalog_get(&third, 16, &size) == NULL);
    assert(size == 0);
    miband_store_close(store);
    miband_install_catalog_clear(&third);
    return 0;
}
