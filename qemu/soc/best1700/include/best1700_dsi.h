/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_DSI_H
#define BEST1700_DSI_H

#include "hw/sysbus.h"

#define TYPE_BEST1700_DSI "best1700-dsi"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700DsiState, BEST1700_DSI)

#define BEST1700_DSI_MMIO_SIZE 0x1000
#define BEST1700_DSI_MAX_PAYLOAD 4096

/* Board-owned DCS endpoint. Read returns a byte count, or a negative error.
 * Buffers are borrowed only for the duration of the callback. */
typedef struct Best1700DsiPanelEndpoint {
    void *opaque;
    int (*read)(void *opaque, uint8_t command, uint8_t *data, size_t capacity);
    bool (*write)(void *opaque, uint8_t command, const uint8_t *data,
                  size_t length);
} Best1700DsiPanelEndpoint;

struct Best1700DsiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Best1700DsiPanelEndpoint panel;
    uint32_t regs[BEST1700_DSI_MMIO_SIZE / sizeof(uint32_t)];
    uint32_t packet_header;
    uint32_t events;
    uint32_t commands;
    uint16_t max_return;
    uint8_t tx_payload[BEST1700_DSI_MAX_PAYLOAD];
    unsigned tx_count;
    bool tx_overflow;
    uint8_t rx_fifo[BEST1700_DSI_MAX_PAYLOAD + 7];
    unsigned rx_head;
    unsigned rx_count;
};

void best1700_dsi_connect_panel(Best1700DsiState *s,
                                const Best1700DsiPanelEndpoint *panel);
void best1700_dsi_te(Best1700DsiState *s);
void best1700_dsi_reset(Best1700DsiState *s);

#endif
