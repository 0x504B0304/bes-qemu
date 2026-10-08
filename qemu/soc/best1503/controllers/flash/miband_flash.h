#ifndef MIBAND_FLASH_H
#define MIBAND_FLASH_H
#include "exec/hwaddr.h"
#include "miband_flash_endpoint.h"

#define MIBAND_FLASH_COUNT 2
typedef struct MiBandFlash MiBandFlash;
typedef struct MiBandFlashDmaCallbacks {
    void *opaque;
    void (*read)(void *, MiBandFlash *);
    void (*write)(void *, MiBandFlash *);
} MiBandFlashDmaCallbacks;

struct MiBandFlash {
    MiBandFlashEndpoint endpoint;
    MiBandFlashDmaCallbacks dma;
    uint32_t command;
    unsigned rx_index;
    uint64_t status_poll_count, read_cache_count;
    bool trace_current_command;
    uint8_t *mapped_cache;
    unsigned mapped_cache_size;
    /* Staging capacity for the supported 2048+128-byte NAND page. */
    uint8_t tx_fifo[2176];
    unsigned tx_count;
    bool tx_stream_active;
};

void miband_flash_init(MiBandFlash *flash, MiBandFlashEndpoint endpoint,
                        uint8_t *mapped_cache, unsigned mapped_cache_size);
void miband_flash_connect_dma(MiBandFlash *flash, MiBandFlashDmaCallbacks callbacks);
const uint8_t *miband_flash_dma_source(MiBandFlash *flash, unsigned bytes);
uint8_t *miband_flash_dma_target(MiBandFlash *flash, unsigned bytes);
bool miband_flash_read(MiBandFlash flash[MIBAND_FLASH_COUNT], hwaddr address,
                        unsigned size, uint64_t *result);
bool miband_flash_write(MiBandFlash flash[MIBAND_FLASH_COUNT], hwaddr address,
                         unsigned size, uint64_t value);
#endif
