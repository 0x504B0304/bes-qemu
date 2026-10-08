/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_PSRAM_H
#define BEST1700_PSRAM_H

#include "hw/sysbus.h"
#include "qemu/timer.h"

#define TYPE_BEST1700_PSRAM "best1700-psram"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700PsramState, BEST1700_PSRAM)
#define BEST1700_PSRAM_MMIO_SIZE 0x10000
#define BEST1700_PSRAM_PHY_OFFSET 0x8000
#define BEST1700_PSRAM_FIFO_WORDS 8

/* Package-owned mode registers. The channel selection is the raw two-bit
 * manager channel mask. An absent package returns an open-bus read value. */
typedef struct Best1700PsramEndpoint {
    void *opaque;
    uint32_t (*read_mode)(void *opaque, unsigned channels, uint32_t address);
    void (*write_mode)(void *opaque, unsigned channels, uint32_t address,
                       uint32_t value);
    void (*reset)(void *opaque, unsigned channels);
} Best1700PsramEndpoint;

struct Best1700PsramState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *command_timer;
    QEMUTimer *phy_timer;
    QEMUTimer *dll_timer;
    Best1700PsramEndpoint endpoint;
    uint32_t regs[0x500 / 4];
    uint32_t phy[0x100 / 4];
    uint16_t analog[256];
    uint32_t tx[BEST1700_PSRAM_FIFO_WORDS];
    uint32_t rx[BEST1700_PSRAM_FIFO_WORDS];
    unsigned tx_count, rx_count, rx_head;
    uint32_t command_address;
    uint8_t command, command_channels, command_words;
    uint8_t phy_lp_state;
    uint32_t dll_delay_taps;
    bool pending, rejected, sleeping, dll_enabled, dll_locked;
};

void best1700_psram_connect_endpoint(Best1700PsramState *s,
                                    const Best1700PsramEndpoint *endpoint);
void best1700_psram_reset(Best1700PsramState *s);
bool best1700_psram_ispi_read(void *opaque, unsigned page, unsigned reg,
                             uint16_t *value);
bool best1700_psram_ispi_write(void *opaque, unsigned page, unsigned reg,
                              uint16_t value);

#endif
