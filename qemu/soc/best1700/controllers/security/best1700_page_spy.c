/*
 * BEST1700 page-spy v2 configuration and status register interface.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "best1700_page_spy.h"

#define SPY_CTRL_MASK 0x3dfu
#define SPY_PPC_CLEAR (1u << 5)

static uint64_t spy_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1700PageSpyState *s = opaque;

    switch (addr) {
    case 0x00: return s->ctrl;
    case 0x04: return s->region_ctrl;
    case 0x08: return s->start[0];
    case 0x0c: return s->end[0];
    case 0x10: return s->start[1];
    case 0x14: return s->end[1];
    case 0x18: return s->hit[0];
    case 0x1c: return s->hit_start[0];
    case 0x20: return s->hit_end[0];
    case 0x24: return s->hit[1];
    case 0x28: return s->hit_start[1];
    case 0x2c: return s->hit_end[1];
    default:
        qemu_log_mask(LOG_UNIMP, TYPE_BEST1700_PAGE_SPY
                      ": unimplemented read +0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void spy_write(void *opaque, hwaddr addr, uint64_t value,
                      unsigned size)
{
    Best1700PageSpyState *s = opaque;
    unsigned i;

    switch (addr) {
    case 0x00:
        /* PPC_CLEAR is a command, not persistent configuration. There is
         * currently no attached interconnect generating PPC violations.
         * It does not clear the independent page-spy region state.
         */
        s->ctrl = value & SPY_CTRL_MASK & ~SPY_PPC_CLEAR;
        return;
    case 0x04:
        s->region_ctrl = value & 0xff;
        for (i = 0; i < 2; i++) {
            if (!(s->region_ctrl & (1u << (i * 4)))) {
                s->hit[i] = s->hit_start[i] = s->hit_end[i] = 0;
            }
        }
        return;
    case 0x08: s->start[0] = value; return;
    case 0x0c: s->end[0] = value; return;
    case 0x10: s->start[1] = value; return;
    case 0x14: s->end[1] = value; return;
    case 0x18: case 0x1c: case 0x20:
    case 0x24: case 0x28: case 0x2c:
        /* Captured status is read-only, not guest-injectable event state. */
        return;
    default:
        qemu_log_mask(LOG_UNIMP, TYPE_BEST1700_PAGE_SPY
                      ": unimplemented write +0x%" HWADDR_PRIx "\n", addr);
    }
}

static const MemoryRegionOps spy_ops = {
    .read = spy_read,
    .write = spy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_page_spy_reset(Best1700PageSpyState *s)
{
    s->ctrl = s->region_ctrl = 0;
    memset(s->start, 0, sizeof(s->start));
    memset(s->end, 0, sizeof(s->end));
    memset(s->hit, 0, sizeof(s->hit));
    memset(s->hit_start, 0, sizeof(s->hit_start));
    memset(s->hit_end, 0, sizeof(s->hit_end));
}

static void spy_reset(DeviceState *dev)
{
    best1700_page_spy_reset(BEST1700_PAGE_SPY(dev));
}

static const VMStateDescription vmstate_spy = {
    .name = TYPE_BEST1700_PAGE_SPY,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(ctrl, Best1700PageSpyState),
        VMSTATE_UINT32(region_ctrl, Best1700PageSpyState),
        VMSTATE_UINT32_ARRAY(start, Best1700PageSpyState, 2),
        VMSTATE_UINT32_ARRAY(end, Best1700PageSpyState, 2),
        VMSTATE_UINT32_ARRAY(hit, Best1700PageSpyState, 2),
        VMSTATE_UINT32_ARRAY(hit_start, Best1700PageSpyState, 2),
        VMSTATE_UINT32_ARRAY(hit_end, Best1700PageSpyState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void spy_init(Object *obj)
{
    Best1700PageSpyState *s = BEST1700_PAGE_SPY(obj);

    memory_region_init_io(&s->iomem, obj, &spy_ops, s,
                          TYPE_BEST1700_PAGE_SPY,
                          BEST1700_PAGE_SPY_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void spy_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BEST1700 page-spy v2 register interface";
    dc->reset = spy_reset;
    dc->vmsd = &vmstate_spy;
}

static const TypeInfo spy_info = {
    .name = TYPE_BEST1700_PAGE_SPY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700PageSpyState),
    .instance_init = spy_init,
    .class_init = spy_class_init,
};

static void spy_register_types(void)
{
    type_register_static(&spy_info);
}

type_init(spy_register_types)
