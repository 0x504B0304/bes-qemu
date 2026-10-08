/*
 * BEST1700 M55 TCM gate register interface and memory access filter.
 *
 * The two-index IOMMU technique follows QEMU's hw/misc/tz-mpc.c,
 * Copyright (c) 2018 Linaro Limited, written by Peter Maydell.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "best1700_tgu.h"

#define TYPE_BEST1700_TGU_IOMMU "best1700-tgu-iommu"
#define TGU_PRESENT (1u << 31)
#define TGU_CTRL_MASK 3u
#define MEMSYSCTL_MSCR 0x000
#define MEMSYSCTL_PFCR 0x004
#define MSCR_DCCLEAN (1u << 16)
#define MSCR_ICACTIVE (1u << 13)
#define MSCR_DCACTIVE (1u << 12)
#define MSCR_FORCEWT (1u << 2)
#define PFCR_MASK 0x3ffu
/* Detailed r1p0/r1p1 PFCR field resets, not the inconsistent summary table. */
#define PFCR_RESET ((2u << 7) | (6u << 4) | (2u << 1) | 1u)

enum { TGU_SECURE, TGU_NONSECURE, TGU_INDEX_COUNT };

static bool tgu_nonsecure(Best1700TguBank *b, hwaddr offset)
{
    unsigned block = offset / b->block_size;

    assert(block < BEST1700_TGU_LUT_WORDS * 32);
    return (b->lut[block / 32] >> (block % 32)) & 1;
}

static void tgu_invalidate(Best1700TguBank *b, unsigned word, uint32_t changed)
{
    unsigned bit;

    for (bit = 0; bit < 32; bit++) {
        hwaddr offset = (word * 32 + bit) * (hwaddr)b->block_size;
        IOMMUTLBEvent event = {
            .type = IOMMU_NOTIFIER_UNMAP,
            .entry = {
                .iova = offset,
                .translated_addr = offset,
                .addr_mask = b->block_size - 1,
                .perm = IOMMU_NONE,
            },
        };

        if (!(changed & (1u << bit)) || offset >= b->size) {
            continue;
        }
        memory_region_notify_iommu(&b->upstream, TGU_SECURE, event);
        memory_region_notify_iommu(&b->upstream, TGU_NONSECURE, event);
    }
}

static MemTxResult tgu_blocked_read(void *opaque, hwaddr addr, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    *data = 0;
    return MEMTX_ERROR;
}

static MemTxResult tgu_blocked_write(void *opaque, hwaddr addr, uint64_t data,
                                    unsigned size, MemTxAttrs attrs)
{
    return MEMTX_ERROR;
}

static const MemoryRegionOps tgu_blocked_ops = {
    .read_with_attrs = tgu_blocked_read,
    .write_with_attrs = tgu_blocked_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static IOMMUTLBEntry tgu_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                  IOMMUAccessFlags flags, int index)
{
    Best1700TguBank *b = container_of(iommu, Best1700TguBank, upstream);
    bool allowed = index == TGU_SECURE || tgu_nonsecure(b, addr);
    IOMMUTLBEntry entry = {
        .iova = addr & ~((hwaddr)b->block_size - 1),
        .translated_addr = addr & ~((hwaddr)b->block_size - 1),
        .addr_mask = b->block_size - 1,
        .perm = IOMMU_RW,
        .target_as = allowed ? &b->downstream_as : &b->blocked_as,
    };

    /* NS-to-S accesses are rejected regardless of CTRL. S-to-NS matching
     * and the DBFEN/DEREN-dependent error response are not modeled yet:
     * CTRL is not a protection enable, and must never open secure RAM.
     */
    return entry;
}

static int tgu_attrs_to_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    /* Debug/loader transactions with unspecified attrs may initialize RAM. */
    return attrs.unspecified || attrs.secure ? TGU_SECURE : TGU_NONSECURE;
}

static int tgu_num_indexes(IOMMUMemoryRegion *iommu)
{
    return TGU_INDEX_COUNT;
}

static Best1700TguBank *tgu_decode(Best1700TguState *s, hwaddr *offset)
{
    if (*offset >= 0x500 && *offset < 0x550) {
        *offset -= 0x500;
        return &s->bank[0];
    }
    if (*offset >= 0x600 && *offset < 0x650) {
        *offset -= 0x600;
        return &s->bank[1];
    }
    return NULL;
}

static bool memsysctl_access(Best1700TguState *s, MemTxAttrs attrs)
{
    /* These unbanked controls become visible to NS only when BFHFNMINS
     * permits it. Hidden privileged accesses are RAZ/WI, not BusFaults.
     * The ARMv7M container exists before its CPU is created at realize.
     */
    return attrs.unspecified || attrs.secure ||
           (s->armv7m->cpu &&
            (s->armv7m->cpu->env.v7m.aircr & R_V7M_AIRCR_BFHFNMINS_MASK));
}

static uint32_t memsysctl_mscr_mask(Best1700TguState *s)
{
    /* TCG does not model cache RAM or ECC. Retain the cache control state,
     * report neither powerdown nor ECC, and never invent ECC completion.
     */
    return (s->icache_present ? MSCR_ICACTIVE : 0) |
           (s->dcache_present ? MSCR_DCCLEAN | MSCR_DCACTIVE | MSCR_FORCEWT : 0);
}

static void memsysctl_reset(Best1700TguState *s)
{
    s->mscr = (s->icache_present ? MSCR_ICACTIVE : 0) |
              (s->dcache_present ? MSCR_DCACTIVE : 0);
    s->pfcr = s->dcache_present ? PFCR_RESET : 0;
}

static MemTxResult tgu_read(void *opaque, hwaddr addr, uint64_t *data,
                           unsigned size, MemTxAttrs attrs)
{
    Best1700TguState *s = opaque;
    Best1700TguBank *b;

    *data = 0;
    if (!attrs.unspecified && attrs.user) {
        return MEMTX_ERROR;
    }
    if (addr == MEMSYSCTL_MSCR || addr == MEMSYSCTL_PFCR) {
        if (memsysctl_access(s, attrs)) {
            *data = addr == MEMSYSCTL_MSCR ? s->mscr : s->pfcr;
        }
        return MEMTX_OK;
    }
    b = tgu_decode(s, &addr);
    if (!b || (!attrs.unspecified && !attrs.secure)) {
        return MEMTX_OK;
    }
    if (addr == 0) {
        *data = b->ctrl;
    } else if (addr == 4) {
        *data = b->cfg;
    } else if (addr >= 0x10) {
        *data = b->lut[(addr - 0x10) / 4];
    }
    return MEMTX_OK;
}

static MemTxResult tgu_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size, MemTxAttrs attrs)
{
    Best1700TguState *s = opaque;
    Best1700TguBank *b;
    unsigned word, blocks, valid;
    uint32_t value, changed;

    if (!attrs.unspecified && attrs.user) {
        return MEMTX_ERROR;
    }
    if (addr == MEMSYSCTL_MSCR || addr == MEMSYSCTL_PFCR) {
        if (memsysctl_access(s, attrs)) {
            if (addr == MEMSYSCTL_MSCR) {
                s->mscr = data & memsysctl_mscr_mask(s);
            } else {
                s->pfcr = s->dcache_present ? data & PFCR_MASK : 0;
            }
        }
        return MEMTX_OK;
    }
    b = tgu_decode(s, &addr);
    if (!b || (!attrs.unspecified && !attrs.secure)) {
        return MEMTX_OK;
    }
    if (addr == 0) {
        b->ctrl = data & TGU_CTRL_MASK;
    } else if (addr >= 0x10) {
        word = (addr - 0x10) / 4;
        blocks = b->size / b->block_size;
        valid = blocks > word * 32 ? MIN(32, blocks - word * 32) : 0;
        value = valid == 32 ? data : data & ((1u << valid) - 1);
        changed = b->lut[word] ^ value;
        b->lut[word] = value;
        tgu_invalidate(b, word, changed);
    }
    /* CFG is read-only; reserved locations are RAZ/WI. */
    return MEMTX_OK;
}

static const MemoryRegionOps tgu_ops = {
    .read_with_attrs = tgu_read,
    .write_with_attrs = tgu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void tgu_reset(DeviceState *dev)
{
    Best1700TguState *s = BEST1700_TGU(dev);
    unsigned bank, word;

    memsysctl_reset(s);
    for (bank = 0; bank < BEST1700_TGU_BANKS; bank++) {
        Best1700TguBank *b = &s->bank[bank];

        b->ctrl = TGU_CTRL_MASK;
        for (word = 0; word < BEST1700_TGU_LUT_WORDS; word++) {
            uint32_t changed = b->lut[word];

            b->lut[word] = 0;
            tgu_invalidate(b, word, changed);
        }
    }
}

static void tgu_realize(DeviceState *dev, Error **errp)
{
    Best1700TguState *s = BEST1700_TGU(dev);
    unsigned bank;

    if (!s->armv7m) {
        error_setg(errp, "TGU armv7m link is required");
        return;
    }
    /* Validate both ports before creating any address spaces. */
    for (bank = 0; bank < BEST1700_TGU_BANKS; bank++) {
        Best1700TguBank *b = &s->bank[bank];
        uint64_t blocks;

        if (!b->downstream) {
            error_setg(errp, "TGU %s downstream is required", bank ? "D" : "I");
            return;
        }
        b->size = memory_region_size(b->downstream);
        if (!is_power_of_2(b->block_size) || b->block_size < 32 ||
            b->block_size > (1u << 20) || b->size % b->block_size) {
            error_setg(errp, "TGU block size must be a power of two, 32..1MiB, "
                       "and divide its downstream size");
            return;
        }
        blocks = b->size / b->block_size;
        if (!is_power_of_2(blocks) || blocks > 512) {
            error_setg(errp, "TGU downstream must contain 1..512 power-of-two blocks");
            return;
        }
        b->cfg = TGU_PRESENT | (ctz64(blocks) << 8) |
                 (ctz32(b->block_size) - 5);
    }
    memory_region_init_io(&s->registers, OBJECT(s), &tgu_ops, s,
                          "best1700-tgu-registers", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->registers);

    for (bank = 0; bank < BEST1700_TGU_BANKS; bank++) {
        Best1700TguBank *b = &s->bank[bank];
        const char *name = bank ? "best1700-dtgu" : "best1700-itgu";

        memory_region_init_iommu(&b->upstream, sizeof(b->upstream),
                                 TYPE_BEST1700_TGU_IOMMU, OBJECT(s), name, b->size);
        if (b->block_size < memory_region_iommu_get_min_page_size(&b->upstream)) {
            error_setg(errp, "TGU block size is smaller than the CPU page size");
            return;
        }
        memory_region_init_io(&b->blocked, OBJECT(s), &tgu_blocked_ops, b,
                              bank ? "dtgu-denied" : "itgu-denied", b->size);
        address_space_init(&b->downstream_as, b->downstream, name);
        address_space_init(&b->blocked_as, &b->blocked,
                           bank ? "dtgu-denied" : "itgu-denied");
        sysbus_init_mmio(SYS_BUS_DEVICE(s), MEMORY_REGION(&b->upstream));
    }
}

static int tgu_post_load(void *opaque, int version_id)
{
    Best1700TguState *s = opaque;
    unsigned bank, word;

    if (version_id < 2) {
        memsysctl_reset(s);
    }
    for (bank = 0; bank < BEST1700_TGU_BANKS; bank++) {
        for (word = 0; word < BEST1700_TGU_LUT_WORDS; word++) {
            tgu_invalidate(&s->bank[bank], word, UINT32_MAX);
        }
    }
    return 0;
}

static const VMStateDescription tgu_bank_vmstate = {
    .name = "best1700-tgu/bank",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(ctrl, Best1700TguBank),
        VMSTATE_UINT32_ARRAY(lut, Best1700TguBank, BEST1700_TGU_LUT_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription tgu_vmstate = {
    .name = "best1700-tgu",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = tgu_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(bank, Best1700TguState, BEST1700_TGU_BANKS, 1,
                             tgu_bank_vmstate, Best1700TguBank),
        VMSTATE_UINT32_V(mscr, Best1700TguState, 2),
        VMSTATE_UINT32_V(pfcr, Best1700TguState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static Property tgu_properties[] = {
    DEFINE_PROP_LINK("armv7m", Best1700TguState, armv7m, TYPE_ARMV7M, ARMv7MState *),
    DEFINE_PROP_BOOL("icache-present", Best1700TguState, icache_present, true),
    DEFINE_PROP_BOOL("dcache-present", Best1700TguState, dcache_present, true),
    DEFINE_PROP_LINK("itcm-downstream", Best1700TguState, bank[0].downstream,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("dtcm-downstream", Best1700TguState, bank[1].downstream,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT32("itcm-block-size", Best1700TguState, bank[0].block_size, 4096),
    DEFINE_PROP_UINT32("dtcm-block-size", Best1700TguState, bank[1].block_size, 8192),
    DEFINE_PROP_END_OF_LIST(),
};

static void tgu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BEST1700 M55 ITCM/DTCM TrustZone gates";
    dc->realize = tgu_realize;
    dc->reset = tgu_reset;
    dc->vmsd = &tgu_vmstate;
    device_class_set_props(dc, tgu_properties);
}

static void tgu_iommu_class_init(ObjectClass *klass, void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = tgu_translate;
    imrc->attrs_to_index = tgu_attrs_to_index;
    imrc->num_indexes = tgu_num_indexes;
}

static const TypeInfo tgu_types[] = {
    {
        .name = TYPE_BEST1700_TGU_IOMMU,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = tgu_iommu_class_init,
    }, {
        .name = TYPE_BEST1700_TGU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Best1700TguState),
        .class_init = tgu_class_init,
    },
};

DEFINE_TYPES(tgu_types)
