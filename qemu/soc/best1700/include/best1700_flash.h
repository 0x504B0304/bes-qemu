/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_FLASH_H
#define BEST1700_FLASH_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "best1700_nor.h"

#define TYPE_BEST1700_FLASH "best1700-flash"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700FlashState, BEST1700_FLASH)
#define BEST1700_FLASH_MMIO_SIZE 0x1000
#define BEST1700_FLASH_MAX_TRANSFER 8191

struct Best1700FlashState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    uint32_t regs[BEST1700_FLASH_MMIO_SIZE / 4];
    uint8_t *backing;
    uint64_t backing_size;
    uint32_t die_count, jedec_id;
    uint64_t uid0, uid1;
    Best1700Nor dies[2];
    uint8_t tx_fifo[64];
    unsigned tx_bytes, tx_slots;
    uint8_t tx[BEST1700_FLASH_MAX_TRANSFER];
    uint8_t rx[BEST1700_FLASH_MAX_TRANSFER];
    unsigned tx_count, tx_needed, rx_count, rx_head, rx_needed;
    uint32_t command, address;
    bool pending, failed, dual, tx_copy, extended;
    void (*changed)(void *opaque, uint64_t offset, uint64_t length);
    void *changed_opaque;
};

void best1700_flash_set_backing(Best1700FlashState *s, uint8_t *data,
                               uint64_t size);
void best1700_flash_set_changed(Best1700FlashState *s,
    void (*changed)(void *opaque, uint64_t offset, uint64_t length), void *opaque);
void best1700_flash_reset(Best1700FlashState *s);

#endif
