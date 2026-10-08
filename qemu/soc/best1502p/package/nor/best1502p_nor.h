#ifndef BEST1502P_NOR_H
#define BEST1502P_NOR_H

#include <stdbool.h>
#include <stdint.h>

/* The package model owns the serial-NOR command semantics.  The controller
 * only transports bytes and exposes the FIFO/status registers. */
typedef struct Best1502pNor {
    uint8_t *backing;
    uint32_t size;
    uint8_t jedec_id[3];
    uint8_t status;
    bool write_enable;
} Best1502pNor;

void best1502p_nor_init(Best1502pNor *nor, uint8_t *backing, uint32_t size);
void best1502p_nor_reset(Best1502pNor *nor);
unsigned best1502p_nor_execute(Best1502pNor *nor, uint8_t opcode,
                               const uint8_t *tx, unsigned tx_count,
                               uint8_t *rx, unsigned rx_capacity);

#endif
