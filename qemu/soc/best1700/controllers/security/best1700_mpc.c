/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The register interface and per-block access checks are QEMU's tz-mpc.
 * This subclass adds reset notification for cached IOMMU translations. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "best1700_mpc.h"

static void best1700_mpc_reset(DeviceState *dev)
{
    Best1700MpcClass *klass = BEST1700_MPC_GET_CLASS(dev);
    TZMPC *mpc = TZ_MPC(dev);
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = &mpc->downstream_as,
            .iova = 0,
            .translated_addr = 0,
            .addr_mask = memory_region_size(mpc->downstream) - 1,
            .perm = IOMMU_NONE,
        },
    };

    klass->parent_reset(dev);
    /* Upstream reset clears the LUT but does not notify clients or lower
     * the IRQ. A device reset must revoke prior NS mappings in both indexes,
     * including CPU translations reached through an alternate bus alias. */
    memory_region_notify_iommu(&mpc->upstream, 0, event);
    memory_region_notify_iommu(&mpc->upstream, 1, event);
    qemu_irq_lower(mpc->irq);
}

static void best1700_mpc_class_init(ObjectClass *oc, void *data)
{
    Best1700MpcClass *klass = BEST1700_MPC_CLASS(oc);

    device_class_set_parent_reset(DEVICE_CLASS(oc), best1700_mpc_reset,
                                  &klass->parent_reset);
}

static const TypeInfo best1700_mpc_info = {
    .name = TYPE_BEST1700_MPC,
    .parent = TYPE_TZ_MPC,
    .instance_size = sizeof(Best1700MpcState),
    .class_size = sizeof(Best1700MpcClass),
    .class_init = best1700_mpc_class_init,
};

static void best1700_mpc_register_types(void)
{
    type_register_static(&best1700_mpc_info);
}

type_init(best1700_mpc_register_types);
