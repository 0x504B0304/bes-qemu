/* Flash wire commands and bounded cache transfers, independent of any SoC. */
#ifndef MIBAND_FLASH_ENDPOINT_H
#define MIBAND_FLASH_ENDPOINT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MB_FLASH_CACHE_REFRESH = 1,
    MB_FLASH_DMA_READ = 2,
    MB_FLASH_DMA_WRITE = 4,
};

typedef struct MiBandFlashEndpoint {
    void *opaque;
    /* Return cache/transfer flags; the controller owns DMA and mapped aliases. */
    unsigned (*command)(void *, uint8_t opcode, uint32_t address,
                    const uint8_t *tx, unsigned length);
    void (*commit_tx)(void *, uint8_t opcode, uint32_t address,
                      const uint8_t *tx, unsigned length);
    bool (*response)(void *, uint8_t opcode, unsigned index,
                     unsigned requested, uint8_t *value);
    bool (*is_poll)(uint8_t opcode);
    unsigned cache_size;
    const uint8_t *(*read_cache)(void *);
    /* Bounded, borrowed transfer buffer, valid only during this command. */
    uint8_t *(*program_buffer)(void *, unsigned column, unsigned length);
} MiBandFlashEndpoint;
#endif
