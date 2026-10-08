#ifndef MIBAND_NAND_DEVICE_H
#define MIBAND_NAND_DEVICE_H
#include "miband_flash_endpoint.h"
#include "miband_store.h"

typedef struct MiBandNandDevice MiBandNandDevice;
typedef struct MiBandNandGeometry {
    unsigned page_size, spare_size, pages_per_block, page_count;
    uint8_t jedec_id[3];
} MiBandNandGeometry;

MiBandNandDevice *miband_nand_new(const MiBandNandGeometry *geometry);
void miband_nand_free(MiBandNandDevice *nand);
MiBandFlashEndpoint miband_nand_endpoint(MiBandNandDevice *nand);
/* Images are borrowed read-only. Raw images include per-page spare bytes. */
void miband_nand_set_image(MiBandNandDevice *nand, const uint8_t *image,
                            size_t size, bool raw);
void miband_nand_set_resource(MiBandNandDevice *nand, const uint8_t *image,
                               size_t size, unsigned first_page, bool raw);
void miband_nand_set_store(MiBandNandDevice *nand, MiBandStore *store);
/* Expose completed MASS payloads through the guest's writable YAFFS data
 * partition.  The overlay is rebuilt from the persistent install catalog and
 * therefore survives a QEMU process restart without changing the read-only
 * resource image. */
void miband_nand_set_mass_files(MiBandNandDevice *nand, unsigned first_page,
                                const uint8_t *watchface, size_t watchface_size,
                                const uint8_t *quickapp, size_t quickapp_size);
void miband_nand_restore_page(MiBandNandDevice *nand, unsigned row,
                               const uint8_t *data, unsigned size);
void miband_nand_restore_erase(MiBandNandDevice *nand, unsigned block);
bool miband_nand_store_compatible(MiBandNandDevice *nand);
const MiBandNandGeometry *miband_nand_geometry(const MiBandNandDevice *nand);
#endif
