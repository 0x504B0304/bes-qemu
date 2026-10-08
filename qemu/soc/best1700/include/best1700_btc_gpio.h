/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTC_GPIO_H
#define BEST1700_BTC_GPIO_H

#include "exec/memory.h"

#define BEST1700_BTC_GPIO_BASE 0x6e040000u
#define BEST1700_BTC_GPIO_WINDOW 0x1000u

typedef struct Best1700BtcGpio {
    MemoryRegion iomem;
    uint32_t test_port;
    uint32_t mode;
} Best1700BtcGpio;

void best1700_btc_gpio_init(Best1700BtcGpio *s, Object *owner);
void best1700_btc_gpio_reset(Best1700BtcGpio *s);

#endif
