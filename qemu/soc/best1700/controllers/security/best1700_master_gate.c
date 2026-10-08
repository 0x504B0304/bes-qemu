/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "best1700_master_gate.h"

#define TYPE_BEST1700_MASTER_GATE "best1700-master-gate"
enum { MASTER_SECURE, MASTER_NONSECURE, MASTER_INDEX_COUNT };

static void best1700_master_gate_flush(Best1700MasterGate *s,
                                       hwaddr address, hwaddr mask)
{
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .iova = address,
            .translated_addr = address,
            .addr_mask = mask,
            .perm = IOMMU_NONE,
        },
    };

    for (int i = 0; i < MASTER_INDEX_COUNT; i++) {
        memory_region_notify_iommu(&s->iommu, i, event);
    }
}

static void best1700_master_policy_changed(void *opaque)
{
    Best1700MasterGate *s = opaque;

    best1700_master_gate_flush(s, 0, UINT64_MAX);
}

static void best1700_master_downstream_changed(IOMMUNotifier *notifier,
                                               IOMMUTLBEntry *entry)
{
    Best1700MasterGateNotifier *n =
        container_of(notifier, Best1700MasterGateNotifier, notifier);

    /* Either underlying attribute table may feed either CPU attribute
     * index, depending on the current policy. Invalidating both is safe. */
    best1700_master_gate_flush(n->gate, entry->iova, entry->addr_mask);
}

static MemTxResult best1700_master_downstream_rw(Best1700MasterGate *s,
                                                 hwaddr address,
                                                 MemTxAttrs attrs,
                                                 uint8_t *bytes, unsigned size,
                                                 bool is_write)
{
    IOMMUMemoryRegionClass *klass = IOMMU_MEMORY_REGION_GET_CLASS(s->downstream);
    IOMMUAccessFlags flags = is_write ? IOMMU_WO : IOMMU_RO;
    int index = memory_region_iommu_attrs_to_index(s->downstream, attrs);
    MemTxResult result = MEMTX_OK;

    while (size) {
        IOMMUTLBEntry entry = klass->translate(s->downstream, address, flags,
                                                index);
        hwaddr translated = (entry.translated_addr & ~entry.addr_mask) |
                            (address & entry.addr_mask);
        unsigned chunk = MIN((hwaddr)size - 1,
                             entry.addr_mask - (address & entry.addr_mask)) + 1;

        /* Query the original protection controller for each block. Its
         * already-mapped upstream is not a zero-based AddressSpace root;
         * forwarding via the returned target avoids applying its bus base
         * a second time and still runs its original deny/fault handlers. */
        if (!(entry.perm & flags)) {
            result |= MEMTX_ERROR;
        } else {
            result |= address_space_rw(entry.target_as, translated, attrs,
                                         bytes, chunk, is_write);
        }
        address += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return result;
}

static MemTxResult best1700_master_slow_read(void *opaque, hwaddr address,
                                            uint64_t *value, unsigned size,
                                            MemTxAttrs attrs)
{
    Best1700MasterGate *s = opaque;
    uint8_t bytes[8] = { 0 };
    MemTxResult result;

    attrs = best1700_sec_cp_attrs(s->sec, attrs);
    result = best1700_master_downstream_rw(s, address, attrs, bytes, size, false);
    *value = 0;
    for (unsigned i = 0; i < size; i++) {
        *value |= (uint64_t)bytes[i] << (8 * i);
    }
    return result;
}

static MemTxResult best1700_master_slow_write(void *opaque, hwaddr address,
                                             uint64_t value, unsigned size,
                                             MemTxAttrs attrs)
{
    Best1700MasterGate *s = opaque;
    uint8_t bytes[8];

    for (unsigned i = 0; i < size; i++) {
        bytes[i] = value >> (8 * i);
    }
    attrs = best1700_sec_cp_attrs(s->sec, attrs);
    return best1700_master_downstream_rw(s, address, attrs, bytes, size, true);
}

static const MemoryRegionOps best1700_master_slow_ops = {
    .read_with_attrs = best1700_master_slow_read,
    .write_with_attrs = best1700_master_slow_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static IOMMUTLBEntry best1700_master_translate(IOMMUMemoryRegion *iommu,
                                               hwaddr address,
                                               IOMMUAccessFlags flags,
                                               int index)
{
    Best1700MasterGate *s = container_of(iommu, Best1700MasterGate, iommu);
    IOMMUMemoryRegionClass *klass = IOMMU_MEMORY_REGION_GET_CLASS(s->downstream);
    MemTxAttrs attrs = { .secure = index == MASTER_SECURE };
    IOMMUTLBEntry entry;
    MemoryRegion *target;
    hwaddr xlat, length;
    int downstream_index;

    attrs = best1700_sec_cp_attrs(s->sec, attrs);
    downstream_index = memory_region_iommu_attrs_to_index(s->downstream, attrs);
    entry = klass->translate(s->downstream, address, flags, downstream_index);
    length = entry.addr_mask + 1;
    target = address_space_translate(entry.target_as, entry.translated_addr,
                                      &xlat, &length, flags == IOMMU_WO, attrs);
    if (!memory_region_is_ram(target)) {
        /* IOMMU entries select translations, not MemTxAttrs. Route blocked
         * accesses through the original gate so it receives the overridden
         * attributes when capturing errors. Successful RAM stays direct. */
        entry.target_as = &s->slow_as;
        entry.translated_addr = entry.iova;
    }
    return entry;
}

static int best1700_master_attrs_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    return attrs.unspecified || attrs.secure ? MASTER_SECURE : MASTER_NONSECURE;
}

static int best1700_master_num_indexes(IOMMUMemoryRegion *iommu)
{
    return MASTER_INDEX_COUNT;
}

static uint64_t best1700_master_page_size(IOMMUMemoryRegion *iommu)
{
    Best1700MasterGate *s = container_of(iommu, Best1700MasterGate, iommu);

    return memory_region_iommu_get_min_page_size(s->downstream);
}

MemoryRegion *best1700_master_gate_init(Best1700MasterGate *s, Object *owner,
                                        const char *name, Best1700SecState *sec,
                                        MemoryRegion *downstream)
{
    uint64_t size = memory_region_size(downstream);

    memory_region_init_iommu(s, sizeof(*s), TYPE_BEST1700_MASTER_GATE,
                              owner, name, size);
    s->downstream = memory_region_get_iommu(downstream);
    g_assert(s->downstream);
    g_assert(memory_region_iommu_num_indexes(s->downstream) == MASTER_INDEX_COUNT);
    s->sec = sec;
    memory_region_init_io(&s->slow_io, owner, &best1700_master_slow_ops, s,
                          "best1700.master-gate-slow", size);
    address_space_init(&s->slow_as, &s->slow_io, "best1700.master-gate-slow");
    for (int i = 0; i < MASTER_INDEX_COUNT; i++) {
        Best1700MasterGateNotifier *n = &s->notifiers[i];

        n->gate = s;
        iommu_notifier_init(&n->notifier, best1700_master_downstream_changed,
                            IOMMU_NOTIFIER_UNMAP, 0, size - 1, i);
        memory_region_register_iommu_notifier(downstream, &n->notifier,
                                               &error_fatal);
    }
    best1700_sec_connect_cp_policy(sec, s, best1700_master_policy_changed);
    return MEMORY_REGION(&s->iommu);
}

static void best1700_master_gate_class_init(ObjectClass *klass, void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = best1700_master_translate;
    imrc->attrs_to_index = best1700_master_attrs_index;
    imrc->num_indexes = best1700_master_num_indexes;
    imrc->get_min_page_size = best1700_master_page_size;
}

static const TypeInfo best1700_master_gate_info = {
    .name = TYPE_BEST1700_MASTER_GATE,
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .instance_size = sizeof(Best1700MasterGate),
    .class_init = best1700_master_gate_class_init,
};

static void best1700_master_gate_register_types(void)
{
    type_register_static(&best1700_master_gate_info);
}
type_init(best1700_master_gate_register_types)
