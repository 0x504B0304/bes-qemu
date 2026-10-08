#ifndef BEST1502P_SPI_H
#define BEST1502P_SPI_H

#include "exec/memory.h"

typedef struct Best1502pNor Best1502pNor;

#define BEST1502P_SPI0_BASE 0x40140000u
#define BEST1502P_SPI0_SIZE 0x00000400u

typedef struct Best1502pSpi {
    uint32_t regs[BEST1502P_SPI0_SIZE / sizeof(uint32_t)];
    hwaddr base;
    unsigned trace_events;
    Best1502pNor *nor;
    uint8_t tx_fifo[256];
    unsigned tx_count;
    uint8_t rx_fifo[256];
    unsigned rx_count;
    unsigned rx_index;
} Best1502pSpi;

void best1502p_spi_init(Best1502pSpi *spi, hwaddr base);
void best1502p_spi_set_nor(Best1502pSpi *spi, Best1502pNor *nor);
bool best1502p_spi_read(Best1502pSpi *spi, hwaddr offset,
                        unsigned size, uint64_t *result);
bool best1502p_spi_write(Best1502pSpi *spi, hwaddr offset,
                         unsigned size, uint64_t value);

#endif
