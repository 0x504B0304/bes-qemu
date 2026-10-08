/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_SPI_H
#define BEST1700_SPI_H

#include "exec/memory.h"
#include "qemu/timer.h"

#define BEST1700_SPI_WINDOW 0x1000u
/* Bounded model capacity; the physical FIFO depth is not established. */
#define BEST1700_SPI_MODEL_FIFO_DEPTH 8

typedef struct Best1700Spi {
    MemoryRegion iomem;
    QEMUTimer *timer;
    char *name;
    uint32_t input_hz;
    uint32_t cr0;
    uint32_t cr1;
    uint32_t prescale;
    uint32_t sample_max;
    uint32_t sample_delay;
    uint32_t tx[BEST1700_SPI_MODEL_FIFO_DEPTH];
    uint32_t rx[BEST1700_SPI_MODEL_FIFO_DEPTH];
    unsigned tx_head;
    unsigned tx_count;
    unsigned rx_head;
    unsigned rx_count;
    unsigned diagnostics;
    bool active;
    unsigned active_bits;
    uint32_t active_divider;
} Best1700Spi;

/* This is the digital SSP controller, not the analog ISPI protocol. The
 * caller supplies its bus address and input clock. No board device is
 * attached yet, so the sampled input is high for every transferred bit. */
void best1700_spi_init(Best1700Spi *s, Object *owner, const char *name,
                       uint32_t input_hz);
void best1700_spi_set_clock_hz(Best1700Spi *s, uint32_t input_hz);
void best1700_spi_reset(Best1700Spi *s);
void best1700_spi_destroy(Best1700Spi *s);

#endif
