/* BEST1700 AON bus-master security control.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "best1700_sec.h"

#define NONSEC_SEC_ENG 0x44
#define EMMC_NONSEC (1u << 2)
#define NONSEC_DMA 0x48
#define CP_NONSEC (1u << 21)
#define GPU_NONSEC (1u << 6)
#define HDMA0_NONSEC (1u << 7)
#define CORE_VTOR_SEL 0x90
#define CP_SEC_SELECT (1u << 5)

static void best1700_sec_cp_changed(Best1700SecState *s)
{
    for (unsigned i = 0; i < s->cp_notifier_count; i++) {
        s->cp_notifiers[i].changed(s->cp_notifiers[i].opaque);
    }
}

MemTxAttrs best1700_sec_cp_attrs(Best1700SecState *s, MemTxAttrs attrs)
{
    if (s->regs[CORE_VTOR_SEL / 4] & CP_SEC_SELECT) {
        attrs.secure = !(s->regs[NONSEC_DMA / 4] & CP_NONSEC);
        attrs.unspecified = false;
    }
    return attrs;
}

MemTxAttrs best1700_sec_gpu_attrs(Best1700SecState *s)
{
    return (MemTxAttrs) {
        .secure = !(s->regs[NONSEC_DMA / 4] & GPU_NONSEC),
    };
}

MemTxAttrs best1700_sec_hdma_attrs(Best1700SecState *s, unsigned controller)
{
    g_assert(controller < 2);
    return (MemTxAttrs) {
        .secure = !(s->regs[NONSEC_DMA / 4] &
                    (HDMA0_NONSEC << controller)),
    };
}

void best1700_sec_connect_cp_policy(Best1700SecState *s, void *opaque,
                                     void (*changed)(void *opaque))
{
    g_assert(s->cp_notifier_count < G_N_ELEMENTS(s->cp_notifiers));
    s->cp_notifiers[s->cp_notifier_count++] = (Best1700SecPolicyNotifier) {
        .opaque = opaque, .changed = changed,
    };
}

static void best1700_sec_update(Best1700SecState *s)
{
    qemu_set_irq(s->emmc_nonsecure,
                 (s->regs[NONSEC_SEC_ENG / 4] & EMMC_NONSEC) != 0);
}

static MemTxResult best1700_sec_read(void *opaque, hwaddr addr,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    Best1700SecState *s = opaque;
    *value = attrs.secure ? s->regs[addr / 4] >> ((addr & 3) * 8) : 0;
    return MEMTX_OK;
}

static MemTxResult best1700_sec_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    Best1700SecState *s = opaque;
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = (size == 1 ? 0xffu : size == 2 ? 0xffffu : UINT32_MAX)
                    << shift;
    uint32_t old = s->regs[addr / 4];

    if (!attrs.secure) {
        return MEMTX_OK;
    }
    /* Preserve the remaining security configuration; currently eMMC and
     * CP/GPU master attribution have consumers for their interconnect effects. */
    s->regs[addr / 4] = (s->regs[addr / 4] & ~mask) | ((value << shift) & mask);
    if ((addr & ~3) == NONSEC_SEC_ENG) {
        s->regs[addr / 4] &= 0xf;
        best1700_sec_update(s);
    }
    if (((addr & ~3) == NONSEC_DMA &&
         ((old ^ s->regs[addr / 4]) & CP_NONSEC)) ||
        ((addr & ~3) == CORE_VTOR_SEL &&
         ((old ^ s->regs[addr / 4]) & CP_SEC_SELECT))) {
        best1700_sec_cp_changed(s);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps best1700_sec_ops = {
    .read_with_attrs = best1700_sec_read,
    .write_with_attrs = best1700_sec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void best1700_sec_reset(DeviceState *dev)
{
    Best1700SecState *s = BEST1700_SEC(dev);
    memset(s->regs, 0, sizeof(s->regs));
    best1700_sec_update(s);
    best1700_sec_cp_changed(s);
}

static int best1700_sec_post_load(void *opaque, int version)
{
    best1700_sec_update(opaque);
    best1700_sec_cp_changed(opaque);
    return 0;
}

static const VMStateDescription vmstate_best1700_sec = {
    .name = TYPE_BEST1700_SEC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = best1700_sec_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Best1700SecState, 0x1000 / sizeof(uint32_t)),
        VMSTATE_END_OF_LIST()
    },
};

static void best1700_sec_init(Object *obj)
{
    Best1700SecState *s = BEST1700_SEC(obj);
    memory_region_init_io(&s->iomem, obj, &best1700_sec_ops, s,
                          TYPE_BEST1700_SEC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(s), &s->emmc_nonsecure, "emmc-nonsecure", 1);
}

static void best1700_sec_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "BEST1700 AON bus-master security control";
    dc->reset = best1700_sec_reset;
    dc->vmsd = &vmstate_best1700_sec;
}

static const TypeInfo best1700_sec_info = {
    .name = TYPE_BEST1700_SEC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700SecState),
    .instance_init = best1700_sec_init,
    .class_init = best1700_sec_class_init,
};

static void best1700_sec_register_types(void)
{
    type_register_static(&best1700_sec_info);
}
type_init(best1700_sec_register_types)
