#ifndef MIBAND_NOR_DEVICE_H
#define MIBAND_NOR_DEVICE_H
#include "miband_flash_endpoint.h"
#include "miband_store.h"

typedef struct MiBandNorDevice MiBandNorDevice;
/* The writable firmware window borrows the SoC XIP backing memory. */
MiBandNorDevice *miband_nor_new(uint8_t *window, uint32_t address,
    unsigned size, unsigned sector_size, const uint8_t jedec_id[3]);
void miband_nor_free(MiBandNorDevice *nor);
MiBandFlashEndpoint miband_nor_endpoint(MiBandNorDevice *nor);
void miband_nor_set_store(MiBandNorDevice *nor, MiBandStore *store);
const uint8_t *miband_nor_seed(MiBandNorDevice *nor);
unsigned miband_nor_window_size(MiBandNorDevice *nor);
void miband_nor_restore(MiBandNorDevice *nor, unsigned address,
                         const uint8_t *data, unsigned size);
#endif
