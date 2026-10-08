/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_DMA_H
#define BEST1700_DMA_H

#include "exec/memory.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define BEST1700_DMA_MAX_CHANNELS 8
#define BEST1700_DMA_WINDOW 0x1000u

typedef struct Best1700DmaChannel {
    uint32_t source;
    uint32_t destination;
    uint32_t lli;
    uint32_t control;
    uint32_t config;
} Best1700DmaChannel;

typedef void (*Best1700DmaTerminalCallback)(void *opaque, unsigned channel);

typedef struct Best1700Dma {
    MemoryRegion iomem;
    AddressSpace *address_space;
    QEMUTimer *timer;
    qemu_irq irq;
    char *name;
    unsigned channel_count;
    uint32_t config;
    uint32_t raw_tc;
    uint32_t raw_error;
    Best1700DmaTerminalCallback terminal_callback;
    void *terminal_opaque;
    Best1700DmaChannel channel[BEST1700_DMA_MAX_CHANNELS];
} Best1700Dma;

/* The caller chooses the bus address, interrupt and DMA master's address
 * space. Neither a CPU index nor an address alias is embedded in this IP. */
void best1700_dma_init(Best1700Dma *s, Object *owner, const char *name,
                       unsigned channels, AddressSpace *address_space);
void best1700_dma_set_irq(Best1700Dma *s, qemu_irq irq);
void best1700_dma_connect_terminal(Best1700Dma *s, void *opaque,
                                   Best1700DmaTerminalCallback callback);
void best1700_dma_reset(Best1700Dma *s);
void best1700_dma_destroy(Best1700Dma *s);

#endif
