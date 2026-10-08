/*
 * BEST1700 SYS/BTH hardware interrupt latches.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Firmware owns the mailbox records and shared-memory protocol. This
 * controller has no guest address space and never accesses message memory.
 */
#include "qemu/osdep.h"
#include "best1700_sys_bth_ipc.h"

typedef enum SysBthRegister {
    SYS_BTH_SET,
    SYS_BTH_CLEAR,
    SYS_BTH_MASK_SET,
    SYS_BTH_MASK_CLEAR,
    SYS_BTH_TX_STATUS,
    SYS_BTH_RX_STATUS,
    SYS_BTH_REGISTER_COUNT,
} SysBthRegister;

/*
 * Each bank is accessible to both processors. In particular, receive
 * completion acknowledges the IND latch in the sending processor's bank.
 */
static const hwaddr sys_bth_offsets[BEST1700_SYS_BTH_SIDES]
                                    [SYS_BTH_REGISTER_COUNT] = {
    [BEST1700_SYS_BTH_SYS] = { 0x98, 0x9c, 0x1f4, 0x1f8, 0x224, 0xec },
    [BEST1700_SYS_BTH_BTH] = { 0x134, 0x138, 0x168, 0x16c, 0x170, 0x174 },
};

static Best1700SysBthPort *sys_bth_peer(Best1700SysBthPort *p)
{
    return &p->controller->port[p->side ^ 1];
}

static uint32_t sys_bth_rx_status(Best1700SysBthPort *p)
{
    uint32_t raw = sys_bth_peer(p)->pending >> 16;

    return raw | ((raw & p->mask) << 16);
}

static uint32_t sys_bth_tx_status(Best1700SysBthPort *p)
{
    uint32_t raw = p->pending & UINT16_MAX;

    return raw | ((raw & (p->mask >> 16)) << 16);
}

static void sys_bth_update_irqs(Best1700SysBthIpc *s)
{
    for (unsigned side = 0; side < BEST1700_SYS_BTH_SIDES; side++) {
        Best1700SysBthPort *p = &s->port[side];
        uint32_t rx = sys_bth_rx_status(p) >> 16;
        uint32_t done = sys_bth_tx_status(p) >> 16;

        for (unsigned lane = 0; lane < BEST1700_SYS_BTH_LANES; lane++) {
            qemu_set_irq(p->rx[lane], (rx >> lane) & 1);
            qemu_set_irq(p->done[lane], (done >> lane) & 1);
        }
    }
}

static int sys_bth_register(Best1700SysBthPort *p, hwaddr offset)
{
    for (unsigned reg = 0; reg < SYS_BTH_REGISTER_COUNT; reg++) {
        if (sys_bth_offsets[p->side][reg] == offset) {
            return reg;
        }
    }
    return -1;
}

static uint32_t sys_bth_read(void *opaque, hwaddr offset, uint32_t *value)
{
    Best1700SysBthPort *p = opaque;

    switch (sys_bth_register(p, offset)) {
    case SYS_BTH_SET:
    case SYS_BTH_CLEAR:
        *value = p->pending;
        break;
    case SYS_BTH_MASK_SET:
    case SYS_BTH_MASK_CLEAR:
        *value = p->mask;
        break;
    case SYS_BTH_TX_STATUS:
        *value = sys_bth_tx_status(p);
        break;
    case SYS_BTH_RX_STATUS:
        *value = sys_bth_rx_status(p);
        break;
    default:
        return 0;
    }
    return UINT32_MAX;
}

static uint32_t sys_bth_write(void *opaque, hwaddr offset, uint32_t value,
                              uint32_t lane_mask)
{
    Best1700SysBthPort *p = opaque;
    uint32_t completed;

    value &= lane_mask;
    switch (sys_bth_register(p, offset)) {
    case SYS_BTH_SET:
        p->pending |= value;
        break;
    case SYS_BTH_CLEAR:
        /*
         * Acknowledging an asserted outgoing IND reports completion to
         * its sender. An empty clear (including firmware IRQ init) must
         * not create a completion. One pending latch coalesces repeated
         * notifications until the receiver acknowledges it.
         */
        completed = (p->pending & value) >> 16;
        p->pending &= ~value;
        p->pending |= completed;
        break;
    case SYS_BTH_MASK_SET:
        p->mask |= value;
        break;
    case SYS_BTH_MASK_CLEAR:
        p->mask &= ~value;
        break;
    case SYS_BTH_TX_STATUS:
    case SYS_BTH_RX_STATUS:
        /* Read-only status views. */
        return UINT32_MAX;
    default:
        return 0;
    }
    sys_bth_update_irqs(p->controller);
    return UINT32_MAX;
}

static uint64_t sys_bth_window_read(void *opaque, hwaddr address,
                                    unsigned size)
{
    Best1700SysBthRegisterWindow *window = opaque;
    uint32_t value = 0;

    sys_bth_read(window->port, window->offset, &value);
    return value >> (address * 8);
}

static void sys_bth_window_write(void *opaque, hwaddr address, uint64_t value,
                                  unsigned size)
{
    Best1700SysBthRegisterWindow *window = opaque;
    unsigned shift = address * 8;
    uint32_t lane_mask = (size == 1 ? 0xffu : size == 2 ? 0xffffu :
                           UINT32_MAX) << shift;

    sys_bth_write(window->port, window->offset, (uint32_t)value << shift,
                  lane_mask);
}

static const MemoryRegionOps sys_bth_window_ops = {
    .read = sys_bth_window_read,
    .write = sys_bth_window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

void best1700_sys_bth_ipc_init(Best1700SysBthIpc *s, Object *owner)
{
    static const char *const side_names[] = { "sys", "bth" };
    static const char *const register_names[] = {
        "set", "clear", "mask-set", "mask-clear", "tx-status", "rx-status",
    };

    G_STATIC_ASSERT(SYS_BTH_REGISTER_COUNT == BEST1700_SYS_BTH_REGISTERS);
    memset(s, 0, sizeof(*s));
    for (unsigned side = 0; side < BEST1700_SYS_BTH_SIDES; side++) {
        Best1700SysBthPort *p = &s->port[side];
        g_autofree char *bank_name = g_strdup_printf("best1700-sys-bth-%s",
                                                     side_names[side]);

        p->controller = s;
        p->side = side;
        memory_region_init(&s->bank[side], owner, bank_name,
                            BEST1700_SYS_BTH_BANK_SIZE);
        for (unsigned reg = 0; reg < SYS_BTH_REGISTER_COUNT; reg++) {
            Best1700SysBthRegisterWindow *window = &p->window[reg];
            g_autofree char *name = g_strdup_printf("%s-%s", bank_name,
                                                     register_names[reg]);

            window->port = p;
            window->offset = sys_bth_offsets[side][reg];
            memory_region_init_io(&window->region, owner, &sys_bth_window_ops,
                                   window, name, sizeof(uint32_t));
            memory_region_add_subregion(&s->bank[side], window->offset,
                                         &window->region);
        }
    }
}

Best1700AonEndpoint best1700_sys_bth_ipc_endpoint(Best1700SysBthIpc *s,
                                                Best1700SysBthSide side)
{
    assert(side < BEST1700_SYS_BTH_SIDES);
    return (Best1700AonEndpoint) {
        .opaque = &s->port[side],
        .read = sys_bth_read,
        .write = sys_bth_write,
    };
}

void best1700_sys_bth_ipc_connect_irq(Best1700SysBthIpc *s,
                                    Best1700SysBthSide side, unsigned lane,
                                    qemu_irq rx, qemu_irq done)
{
    assert(side < BEST1700_SYS_BTH_SIDES);
    assert(lane < BEST1700_SYS_BTH_LANES);
    s->port[side].rx[lane] = rx;
    s->port[side].done[lane] = done;
    sys_bth_update_irqs(s);
}

void best1700_sys_bth_ipc_reset_lanes(Best1700SysBthIpc *s, uint16_t lanes)
{
    uint32_t bits = (uint32_t)lanes | ((uint32_t)lanes << 16);

    for (unsigned side = 0; side < BEST1700_SYS_BTH_SIDES; side++) {
        s->port[side].pending &= ~bits;
        s->port[side].mask &= ~bits;
    }
    /* Reset is not a receive acknowledgment and produces no DONE. */
    sys_bth_update_irqs(s);
}

void best1700_sys_bth_ipc_reset(Best1700SysBthIpc *s)
{
    best1700_sys_bth_ipc_reset_lanes(s, UINT16_MAX);
}
