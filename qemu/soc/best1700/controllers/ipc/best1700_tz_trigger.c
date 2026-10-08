/*
 * BEST1700 M55C0 TrustZone software interrupt latches.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "best1700_tz_trigger.h"

#define TZ_TRIGGER_MASK (BEST1700_TZ_TRIGGER_IND | BEST1700_TZ_TRIGGER_DONE)

static void tz_trigger_update_irq(Best1700TzTriggerState *s)
{
    qemu_set_irq(s->irq[BEST1700_TZ_TRIGGER_IRQ_IND],
                 (s->pending & BEST1700_TZ_TRIGGER_IND) != 0);
    qemu_set_irq(s->irq[BEST1700_TZ_TRIGGER_IRQ_DONE],
                 (s->pending & BEST1700_TZ_TRIGGER_DONE) != 0);
}

static uint64_t tz_trigger_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1700TzTriggerState *s = opaque;

    /* SET and CLR are aliases of the same two interrupt latches. */
    return s->pending;
}

static void tz_trigger_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    Best1700TzTriggerState *s = opaque;

    if (value & ~TZ_TRIGGER_MASK) {
        qemu_log_mask(LOG_UNIMP, TYPE_BEST1700_TZ_TRIGGER
                      ": unmodeled interprocessor IRQ bits 0x%08" PRIx64
                      " at +0x%" HWADDR_PRIx "\n",
                      value & ~TZ_TRIGGER_MASK, addr);
    }
    if (addr == 0) {
        s->pending |= value & TZ_TRIGGER_MASK;
    } else {
        s->pending &= ~(value & TZ_TRIGGER_MASK);
    }
    /* A repeated SET coalesces until the receiving guest acknowledges it.
     * Security targeting and masking belong to the real M55 NVIC, not to
     * this device. No queue, vring or guest memory is changed here. */
    tz_trigger_update_irq(s);
}

static const MemoryRegionOps tz_trigger_ops = {
    .read = tz_trigger_read,
    .write = tz_trigger_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_tz_trigger_reset(Best1700TzTriggerState *s)
{
    s->pending = 0;
    tz_trigger_update_irq(s);
}

static void tz_trigger_reset(DeviceState *dev)
{
    best1700_tz_trigger_reset(BEST1700_TZ_TRIGGER(dev));
}

static int tz_trigger_post_load(void *opaque, int version_id)
{
    Best1700TzTriggerState *s = opaque;

    s->pending &= TZ_TRIGGER_MASK;
    tz_trigger_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_tz_trigger = {
    .name = TYPE_BEST1700_TZ_TRIGGER,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = tz_trigger_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(pending, Best1700TzTriggerState),
        VMSTATE_END_OF_LIST()
    },
};

static void tz_trigger_init(Object *obj)
{
    Best1700TzTriggerState *s = BEST1700_TZ_TRIGGER(obj);

    memory_region_init_io(&s->iomem, obj, &tz_trigger_ops, s,
                          TYPE_BEST1700_TZ_TRIGGER,
                          BEST1700_TZ_TRIGGER_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned i = 0; i < BEST1700_TZ_TRIGGER_IRQ_COUNT; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void tz_trigger_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BEST1700 M55 TrustZone interrupt trigger";
    dc->reset = tz_trigger_reset;
    dc->vmsd = &vmstate_tz_trigger;
}

static const TypeInfo tz_trigger_info = {
    .name = TYPE_BEST1700_TZ_TRIGGER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700TzTriggerState),
    .instance_init = tz_trigger_init,
    .class_init = tz_trigger_class_init,
};

static void tz_trigger_register_types(void)
{
    type_register_static(&tz_trigger_info);
}

type_init(tz_trigger_register_types)
