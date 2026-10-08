/* BEST1700 SDHCI host with its vendor configuration/PHY register window.
 * Card protocols and DMA remain in QEMU's common SD/MMC implementation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "best1700_sdhci.h"

static MemTxAttrs best1700_sdhci_dma_attrs(SDHCIState *host, MemTxAttrs attrs)
{
    Best1700SdhciState *s = container_of(host, Best1700SdhciState, host);
    attrs.unspecified = false;
    attrs.secure = !s->dma_nonsecure;
    attrs.memory = true;
    return attrs;
}

static void best1700_sdhci_nonsecure(void *opaque, int line, int level)
{
    BEST1700_SDHCI(opaque)->dma_nonsecure = level;
}

static uint64_t best1700_sdhci_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700SdhciState *s = opaque;
    uint32_t value = s->regs[offset / 4];

    if (s->diagnostics++ < 64) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 sdhci vendor read +0x%03" HWADDR_PRIx " = %08x\n",
            offset, value);
    }
    return value >> ((offset & 3) * 8);
}

static void best1700_sdhci_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    Best1700SdhciState *s = opaque;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size == 1 ? 0xffu : size == 2 ? 0xffffu : UINT32_MAX)
                    << shift;

    s->regs[offset / 4] = (s->regs[offset / 4] & ~mask) | (value << shift);
    if (s->diagnostics++ < 64) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 sdhci vendor write +0x%03" HWADDR_PRIx " = %08x\n",
            offset, (uint32_t)value);
    }
}

static const MemoryRegionOps best1700_sdhci_ops = {
    .read = best1700_sdhci_read,
    .write = best1700_sdhci_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void best1700_sdhci_reset(DeviceState *dev)
{
    Best1700SdhciState *s = BEST1700_SDHCI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->diagnostics = 0;
}

static void best1700_sdhci_realize(DeviceState *dev, Error **errp)
{
    Best1700SdhciState *s = BEST1700_SDHCI(dev);
    SysBusDevice *host = SYS_BUS_DEVICE(&s->host);

    if (!sysbus_realize(host, errp)) {
        return;
    }
    /* Keep the vendor region page-aligned for CPU TLB MMIO translations.
     * The common host owns the standard register window at higher priority. */
    memory_region_add_subregion(&s->container, 0, &s->vendor);
    memory_region_add_subregion_overlap(&s->container, 0,
                                 sysbus_mmio_get_region(host, 0), 1);
    sysbus_pass_irq(SYS_BUS_DEVICE(dev), host);
}

static void best1700_sdhci_init(Object *obj)
{
    Best1700SdhciState *s = BEST1700_SDHCI(obj);

    object_initialize_child(obj, "host", &s->host, TYPE_SYSBUS_SDHCI);
    s->host.dma_attrs = best1700_sdhci_dma_attrs;
    qdev_init_gpio_in_named(DEVICE(obj), best1700_sdhci_nonsecure,
                            "dma-nonsecure", 1);
    object_property_add_alias(obj, "dma", OBJECT(&s->host), "dma");
    memory_region_init(&s->container, obj, TYPE_BEST1700_SDHCI, 0x1000);
    memory_region_init_io(&s->vendor, obj, &best1700_sdhci_ops, s,
                          "best1700-sdhci-vendor", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->container);
}

static void best1700_sdhci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "BEST1700 SD/MMC host controller";
    dc->realize = best1700_sdhci_realize;
    dc->reset = best1700_sdhci_reset;
}

static const TypeInfo best1700_sdhci_info = {
    .name = TYPE_BEST1700_SDHCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700SdhciState),
    .instance_init = best1700_sdhci_init,
    .class_init = best1700_sdhci_class_init,
};

static void best1700_sdhci_register_types(void)
{
    type_register_static(&best1700_sdhci_info);
}
type_init(best1700_sdhci_register_types)
