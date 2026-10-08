/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "best1700_bth.h"

#define BTH_SUBSYS_RESET (1u << 8)
#define BTH_CPU0_RESET (1u << 9)
#define BTH_CPU1_RESET (1u << 12)
#define BTH_RESET_MASK (BTH_SUBSYS_RESET | BTH_CPU0_RESET | BTH_CPU1_RESET)
#define RAM_CFG_MASK 0x03ffffffu

/* Ownership of an unselected aperture is known, its electrical response is
 * not. Keep unsupported accesses visible without inventing bus faults or
 * allocating a second copy of the physical memory behind that aperture. */
static uint64_t best1700_bth_unselected_read(void *opaque, hwaddr offset,
                                            unsigned size)
{
    Best1700Bth *s = opaque;

    if (s->unsupported_accesses++ < 16) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 shared RAM read through unselected owner aperture "
            "offset=0x%" HWADDR_PRIx " size=%u (unmodelled: RAZ/WI)\n",
            offset, size);
    }
    return 0;
}

static void best1700_bth_unselected_write(void *opaque, hwaddr offset,
                                          uint64_t value, unsigned size)
{
    Best1700Bth *s = opaque;

    if (s->unsupported_accesses++ < 16) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 shared RAM write through unselected owner aperture "
            "offset=0x%" HWADDR_PRIx " size=%u (unmodelled: RAZ/WI)\n",
            offset, size);
    }
}

static const MemoryRegionOps best1700_bth_unselected_ops = {
    .read = best1700_bth_unselected_read,
    .write = best1700_bth_unselected_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void best1700_bth_update_ram(Best1700Bth *s)
{
    if (!s->storage_bound) {
        return;
    }
    memory_region_transaction_begin();
    for (unsigned i = 0; i < BEST1700_SHARED_BANK_COUNT; i++) {
        unsigned owner = (s->ram_cfg >> (2 * i)) & 3;

        memory_region_set_enabled(&s->sys_bank[i], owner == 3);
        memory_region_set_enabled(&s->bth_bank[i], owner == 0);
        memory_region_set_enabled(&s->btc_bank[i], owner == 1);
    }
    memory_region_transaction_commit();
}

bool best1700_bth_cpu_held(Best1700Bth *s)
{
    return (s->reset_asserted & (BTH_SUBSYS_RESET | BTH_CPU0_RESET)) != 0;
}

static void best1700_bth_notify_reset(Best1700Bth *s)
{
    if (s->set_reset) {
        s->set_reset(s->reset_opaque, best1700_bth_cpu_held(s), s->vtor[0]);
    }
}

static uint32_t best1700_bth_cmu_read(void *opaque, hwaddr offset,
                                      uint32_t *value)
{
    Best1700Bth *s = opaque;

    switch (offset) {
    case 0x98:
        *value = s->ram_cfg;
        return RAM_CFG_MASK;
    case 0xb8:
    case 0xbc:
        *value = s->vtor[(offset - 0xb8) / 4];
        return 0xffffff80u;
    case 0xc4:
        *value = 0;
        return BTH_RESET_MASK;
    case 0xc8:
    case 0xcc:
        *value = ~s->reset_asserted;
        return BTH_RESET_MASK;
    default:
        return 0;
    }
}

static uint32_t best1700_bth_cmu_write(void *opaque, hwaddr offset,
                                       uint32_t value, uint32_t lane_mask)
{
    Best1700Bth *s = opaque;
    uint32_t mask;
    bool held = best1700_bth_cpu_held(s);

    switch (offset) {
    case 0x98:
        mask = lane_mask & RAM_CFG_MASK;
        s->ram_cfg = (s->ram_cfg & ~mask) | (value & mask);
        best1700_bth_update_ram(s);
        return RAM_CFG_MASK;
    case 0xb8:
    case 0xbc:
        mask = lane_mask & 0xffffff80u;
        s->vtor[(offset - 0xb8) / 4] =
            (s->vtor[(offset - 0xb8) / 4] & ~mask) | (value & mask);
        return 0xffffff80u;
    case 0xc4:
        if (value & lane_mask & (BTH_SUBSYS_RESET | BTH_CPU0_RESET)) {
            /* A pulse resets a running CPU without changing the latch. */
            if (s->set_reset) {
                s->set_reset(s->reset_opaque, true, s->vtor[0]);
            }
            best1700_bth_notify_reset(s);
        }
        return BTH_RESET_MASK;
    case 0xc8:
        s->reset_asserted |= value & lane_mask & BTH_RESET_MASK;
        break;
    case 0xcc:
        s->reset_asserted &= ~(value & lane_mask & BTH_RESET_MASK);
        break;
    default:
        return 0;
    }
    if (held != best1700_bth_cpu_held(s)) {
        best1700_bth_notify_reset(s);
    }
    return BTH_RESET_MASK;
}

Best1700AonEndpoint best1700_bth_endpoint(Best1700Bth *s)
{
    return (Best1700AonEndpoint) {
        .opaque = s,
        .read = best1700_bth_cmu_read,
        .write = best1700_bth_cmu_write,
    };
}

void best1700_bth_init(Best1700Bth *s, Object *owner)
{
    memory_region_init(&s->ram, owner, "best1700.bth-ram",
                        BEST1700_BTH_RAM_SIZE);
    memory_region_init_ram(&s->private_ram, NULL, "best1700.bth-private",
                            BEST1700_BTH_PRIVATE_SIZE, &error_fatal);
    memory_region_add_subregion(&s->ram, 0, &s->private_ram);
    memory_region_init_io(&s->unselected, owner,
        &best1700_bth_unselected_ops, s, "best1700.bth-unselected",
        BEST1700_BTH_RAM_SIZE);
    memory_region_add_subregion_overlap(&s->ram, 0, &s->unselected, -1);
    /* Only the thirteen confirmed ownership-controlled banks are exposed.
     * A larger BTC ramrun window must not create duplicate physical storage. */
    memory_region_init(&s->btc_ram, owner, "best1700.btc-shared-ram",
                        BEST1700_BTC_SHARED_SIZE);
    memory_region_init_io(&s->btc_unselected, owner,
        &best1700_bth_unselected_ops, s, "best1700.btc-unselected",
        BEST1700_BTC_SHARED_SIZE);
    memory_region_add_subregion_overlap(&s->btc_ram, 0,
                                         &s->btc_unselected, -1);
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("best1700.shared-sys%u", i + 6);
        g_autofree char *fallback = g_strdup_printf("%s-unselected", name);

        memory_region_init(&s->sys_ram[i], owner, name, 0x100000);
        memory_region_init_io(&s->sys_unselected[i], owner,
            &best1700_bth_unselected_ops, s, fallback, 0x100000);
        memory_region_add_subregion_overlap(&s->sys_ram[i], 0,
                                             &s->sys_unselected[i], -1);
    }
    best1700_bth_reset(s);
}

void best1700_bth_bind_storage(Best1700Bth *s, Object *owner,
                               MemoryRegion *sram6, MemoryRegion *sram7)
{
    MemoryRegion *storage[] = { sram6, sram7 };

    g_assert(!s->storage_bound);
    for (unsigned i = 0; i < 16; i++) {
        hwaddr offset = (i % 8) * BEST1700_SHARED_BANK_SIZE;
        g_autofree char *name = g_strdup_printf("best1700.shared-sys-bank%u", i);

        memory_region_init_alias(&s->sys_bank[i], owner, name, storage[i / 8],
                                  offset, BEST1700_SHARED_BANK_SIZE);
        memory_region_add_subregion(&s->sys_ram[i / 8], offset, &s->sys_bank[i]);
        if (i < BEST1700_SHARED_BANK_COUNT) {
            g_autofree char *bth_name = g_strdup_printf("best1700.shared-bth-bank%u", i);
            g_autofree char *btc_name = g_strdup_printf("best1700.shared-btc-bank%u", i);
            hwaddr bth_offset = BEST1700_BTH_PRIVATE_SIZE +
                (BEST1700_SHARED_BANK_COUNT - 1 - i) * BEST1700_SHARED_BANK_SIZE;

            memory_region_init_alias(&s->bth_bank[i], owner, bth_name,
                storage[i / 8], offset, BEST1700_SHARED_BANK_SIZE);
            memory_region_add_subregion(&s->ram, bth_offset, &s->bth_bank[i]);
            /* BTC and SYS enumerate the same physical blocks in ascending
             * order; BTH addresses above enumerate them in reverse order. */
            memory_region_init_alias(&s->btc_bank[i], owner, btc_name,
                storage[i / 8], offset, BEST1700_SHARED_BANK_SIZE);
            memory_region_add_subregion(&s->btc_ram,
                i * BEST1700_SHARED_BANK_SIZE, &s->btc_bank[i]);
        }
    }
    s->storage_bound = true;
    best1700_bth_update_ram(s);
}

void best1700_bth_reset(Best1700Bth *s)
{
    s->ram_cfg = 0;
    s->vtor[0] = s->vtor[1] = 0;
    s->reset_asserted = BTH_RESET_MASK;
    s->unsupported_accesses = 0;
    best1700_bth_update_ram(s);
    best1700_bth_notify_reset(s);
}

void best1700_bth_connect_reset(Best1700Bth *s, void *opaque,
    void (*set_reset)(void *opaque, bool asserted, uint32_t vtor))
{
    s->reset_opaque = opaque;
    s->set_reset = set_reset;
}
