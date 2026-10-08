/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_TZ_TRIGGER_H
#define BEST1700_TZ_TRIGGER_H

#include "hw/sysbus.h"

#define TYPE_BEST1700_TZ_TRIGGER "best1700-tz-trigger"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700TzTriggerState, BEST1700_TZ_TRIGGER)

#define BEST1700_TZ_TRIGGER_MMIO_SIZE 8
#define BEST1700_TZ_TRIGGER_DONE (1u << 12)
#define BEST1700_TZ_TRIGGER_IND  (1u << 13)

enum {
    BEST1700_TZ_TRIGGER_IRQ_IND,
    BEST1700_TZ_TRIGGER_IRQ_DONE,
    BEST1700_TZ_TRIGGER_IRQ_COUNT,
};

struct Best1700TzTriggerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[BEST1700_TZ_TRIGGER_IRQ_COUNT];
    uint32_t pending;
};

void best1700_tz_trigger_reset(Best1700TzTriggerState *s);

#endif
