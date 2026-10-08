#ifndef BEST1502P_PSRAM_H
#define BEST1502P_PSRAM_H

#include "exec/hwaddr.h"
#include <stdbool.h>
#include <stdint.h>

#define BEST1502P_PSRAM_CTRL_BASE 0x40160000u
#define BEST1502P_PSRAM_CTRL_SIZE 0x00100000u

typedef struct Best1502pPsram {
    hwaddr base;
    uint8_t *storage;
    uint32_t storage_size;
    uint32_t regs[0x100000 / sizeof(uint32_t)];
    uint32_t command_address;
    uint32_t words_remaining;
    uint32_t command;
    uint32_t tx_word;
    uint32_t mode_registers[256];
    uint32_t trace_events;
} Best1502pPsram;

void best1502p_psram_init(Best1502pPsram *psram, hwaddr base,
                          uint8_t *storage, uint32_t storage_size);
bool best1502p_psram_read(Best1502pPsram *psram, hwaddr offset,
                          unsigned size, uint64_t *result);
bool best1502p_psram_write(Best1502pPsram *psram, hwaddr offset,
                           unsigned size, uint64_t value);

#endif
