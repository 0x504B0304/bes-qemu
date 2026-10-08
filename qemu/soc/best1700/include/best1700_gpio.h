/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_GPIO_H
#define BEST1700_GPIO_H

#include "hw/sysbus.h"

#define TYPE_BEST1700_GPIO "best1700-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700GpioState, BEST1700_GPIO)

#define BEST1700_GPIO_BANKS 3
#define BEST1700_GPIO_PINS (BEST1700_GPIO_BANKS * 32)
#define BEST1700_GPIO_MMIO_COUNT (BEST1700_GPIO_BANKS * 5)

/* MMIO indices 0..2: GPIO banks, 3..5: common interrupt gates,
 * 6..8: common interrupt status, 9..11: pad pull-up, 12..14: pad pull-down.
 * The narrow common and pad windows leave adjacent
 * clock and pin-multiplexing registers with their owning controllers. */
extern const hwaddr best1700_gpio_mmio_base[BEST1700_GPIO_MMIO_COUNT];

typedef struct Best1700GpioBank {
    uint32_t output;
    uint32_t direction;
    uint32_t control;
    uint32_t input;
    uint32_t driven;
    uint32_t enable;
    uint32_t mask;
    uint32_t edge_type;
    uint32_t polarity;
    uint32_t debounce;
    uint32_t edge_pending;
    uint32_t fall_enable;
    uint32_t fall_mask;
    uint32_t fall_pending;
    uint32_t common_enable;
    uint32_t sync;
    uint32_t pull_up;
    uint32_t pull_down;
} Best1700GpioBank;

typedef struct Best1700GpioRegion {
    Best1700GpioState *controller;
    unsigned bank;
    unsigned kind;
} Best1700GpioRegion;

struct Best1700GpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem[BEST1700_GPIO_MMIO_COUNT];
    Best1700GpioRegion region[BEST1700_GPIO_MMIO_COUNT];
    Best1700GpioBank bank[BEST1700_GPIO_BANKS];
    qemu_irq irq;
    qemu_irq output[BEST1700_GPIO_PINS];
};

void best1700_gpio_set_input(Best1700GpioState *s, unsigned pin, bool high);
void best1700_gpio_reset(Best1700GpioState *s);

#endif
