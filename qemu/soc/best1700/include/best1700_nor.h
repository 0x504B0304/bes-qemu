/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_NOR_H
#define BEST1700_NOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BEST1700_NOR_UID_SIZE 18
#define BEST1700_NOR_SECURITY_BANKS 3
#define BEST1700_NOR_SECURITY_SIZE 1024

typedef enum Best1700NorResult {
    BEST1700_NOR_OK,
    BEST1700_NOR_UNSUPPORTED,
    BEST1700_NOR_INVALID,
    BEST1700_NOR_PROTECTED,
    BEST1700_NOR_BUSY,
} Best1700NorResult;

/* Two dies share the aggregate XIP bytes without owning or copying the image.
 * Die 0 supplies the low nibble of each wire byte, die 1 the high nibble.
 * A die byte therefore occupies two consecutive aggregate bytes. */
typedef struct Best1700Nor {
    uint8_t *backing;
    uint64_t backing_size;
    uint32_t capacity;
    unsigned index, die_count;
    uint8_t jedec[3], uid[BEST1700_NOR_UID_SIZE];
    uint8_t status[3], nonvolatile_status[3];
    uint8_t security[BEST1700_NOR_SECURITY_BANKS][BEST1700_NOR_SECURITY_SIZE];
    uint8_t wrap;
    bool wel, volatile_wel, reset_armed, asleep, address4, qpi;
    int64_t busy_until;
} Best1700Nor;

bool best1700_nor_init(Best1700Nor *s, uint8_t *backing, uint64_t size,
                      unsigned index, unsigned count, uint32_t jedec,
                      uint64_t uid_seed);
void best1700_nor_reset(Best1700Nor *s);
bool best1700_nor_memory_read(uint8_t opcode);
bool best1700_nor_memory_write(uint8_t opcode);
/* apply=false validates without changing die state or borrowed storage. */
Best1700NorResult best1700_nor_command(Best1700Nor *s, uint8_t opcode,
    uint32_t address, const uint8_t *tx, unsigned tx_len,
    uint8_t *rx, unsigned rx_len, int64_t now, bool apply,
    uint64_t *changed_start, uint64_t *changed_length);
const char *best1700_nor_result_name(Best1700NorResult result);

#endif
