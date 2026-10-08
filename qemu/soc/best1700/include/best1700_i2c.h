/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_I2C_H
#define BEST1700_I2C_H

#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"

#define TYPE_BEST1700_I2C "best1700-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700I2CState, BEST1700_I2C)

#define BEST1700_I2C_FIFO_DEPTH 8
#define BEST1700_I2C_MMIO_SIZE 0x1000

struct Best1700I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;
    uint32_t regs[0x100 / sizeof(uint32_t)];
    uint32_t raw_intr;
    uint32_t abort_source;
    uint32_t commands;
    uint8_t rx_fifo[BEST1700_I2C_FIFO_DEPTH];
    unsigned rx_head;
    unsigned rx_count;
    bool active;
    bool receiving;
};

void best1700_i2c_reset(Best1700I2CState *s);

#endif
