/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_SEC_H
#define BEST1700_SEC_H

#include "hw/sysbus.h"

#define TYPE_BEST1700_SEC "best1700-sec"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700SecState, BEST1700_SEC)

typedef struct Best1700SecPolicyNotifier {
    void *opaque;
    void (*changed)(void *opaque);
} Best1700SecPolicyNotifier;

struct Best1700SecState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq emmc_nonsecure;
    uint32_t regs[0x1000 / sizeof(uint32_t)];
    Best1700SecPolicyNotifier cp_notifiers[16];
    unsigned cp_notifier_count;
};

/* CPU architectural security state is unchanged. Only outbound CP AXI
 * transactions use this selected master attribute. */
MemTxAttrs best1700_sec_cp_attrs(Best1700SecState *s, MemTxAttrs attrs);
MemTxAttrs best1700_sec_gpu_attrs(Best1700SecState *s);
MemTxAttrs best1700_sec_hdma_attrs(Best1700SecState *s, unsigned controller);
void best1700_sec_connect_cp_policy(Best1700SecState *s, void *opaque,
                                     void (*changed)(void *opaque));

#endif
