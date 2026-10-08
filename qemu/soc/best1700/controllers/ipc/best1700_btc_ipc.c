/*
 * BEST1700 independent SYS/BTC and BTH/BTC interrupt latches.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Guest software owns mailbox memory and selects its communication route.
 * This device has no guest address space and cannot consume an HCI packet.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_btc_ipc.h"

#define NO_FIELD (-1)
#define BTC_CORE_WAKEUP_REQ (1u << 4)

typedef struct BtcIpcLayout {
    hwaddr offset;
    Best1700BtcIpcSide side;
    unsigned first_lane;
    unsigned lanes;
    bool clear;
    int done;
    int ind;
    int rx_mask;
    int done_mask;
    int raw_rx;
    int raw_done;
    int masked_rx;
    int masked_done;
} BtcIpcLayout;

static const BtcIpcLayout sys_layout[] = {
    { 0x140, BEST1700_BTC_IPC_SYS, 0, 4, false,
      0, 4, 8, 12, 16, 20, 24, 28 },
    { 0x144, BEST1700_BTC_IPC_SYS, 0, 4, true,
      0, 4, 8, 12, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
};

/* BTC's low and high halfwords are separate physical interfaces. */
static const BtcIpcLayout btc_layout[] = {
    { 0x50, BEST1700_BTC_IPC_BTC_BTH, 0, 2, false,
      0, 2, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
    { 0x50, BEST1700_BTC_IPC_BTC_SYS, 0, 2, false,
      16, 18, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
    { 0x54, BEST1700_BTC_IPC_BTC_BTH, 0, 2, true,
      0, 2, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
    { 0x54, BEST1700_BTC_IPC_BTC_SYS, 0, 2, true,
      16, 18, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
    { 0xa0, BEST1700_BTC_IPC_BTC_BTH, 0, 2, false,
      NO_FIELD, NO_FIELD, 2, 0, 6, 4, 10, 8 },
    { 0xa0, BEST1700_BTC_IPC_BTC_SYS, 0, 2, false,
      NO_FIELD, NO_FIELD, 18, 16, 22, 20, 26, 24 },
    { 0xa4, BEST1700_BTC_IPC_BTC_BTH, 0, 2, true,
      NO_FIELD, NO_FIELD, 2, 0, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
    { 0xa4, BEST1700_BTC_IPC_BTC_SYS, 0, 2, true,
      NO_FIELD, NO_FIELD, 18, 16, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
    { 0xac, BEST1700_BTC_IPC_BTC_BTH, 2, 2, false,
      0, 2, 4, 6, 8, 10, 12, 14 },
    { 0xac, BEST1700_BTC_IPC_BTC_SYS, 2, 2, false,
      16, 18, 20, 22, 24, 26, 28, 30 },
    { 0xb0, BEST1700_BTC_IPC_BTC_BTH, 2, 2, true,
      0, 2, 4, 6, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
    { 0xb0, BEST1700_BTC_IPC_BTC_SYS, 2, 2, true,
      16, 18, 20, 22, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
};

static const BtcIpcLayout bth_layout[] = {
    { 0x78, BEST1700_BTC_IPC_BTH, 2, 2, false,
      0, 2, 4, 6, 8, 10, 12, 14 },
    { 0x7c, BEST1700_BTC_IPC_BTH, 2, 2, true,
      0, 2, 4, 6, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
    { 0xa0, BEST1700_BTC_IPC_BTH, 0, 2, false,
      0, 2, 13, 15, 17, 19, 21, 23 },
    { 0xa4, BEST1700_BTC_IPC_BTH, 0, 2, true,
      0, 2, 13, 15, NO_FIELD, NO_FIELD, NO_FIELD, NO_FIELD },
};

static const char *const bank_names[] = { "SYS", "BTC", "BTH" };

static const BtcIpcLayout *btc_ipc_layout(Best1700BtcIpcBankId bank,
                                         size_t *count)
{
    switch (bank) {
    case BEST1700_BTC_IPC_BANK_SYS:
        *count = G_N_ELEMENTS(sys_layout);
        return sys_layout;
    case BEST1700_BTC_IPC_BANK_BTC:
        *count = G_N_ELEMENTS(btc_layout);
        return btc_layout;
    case BEST1700_BTC_IPC_BANK_BTH:
        *count = G_N_ELEMENTS(bth_layout);
        return bth_layout;
    default:
        g_assert_not_reached();
    }
}

static uint32_t btc_ipc_field(const BtcIpcLayout *r, uint8_t lanes, int shift)
{
    uint32_t mask = (1u << r->lanes) - 1;

    return shift == NO_FIELD ? 0 :
        (((uint32_t)lanes >> r->first_lane) & mask) << shift;
}

static uint8_t btc_ipc_extract(const BtcIpcLayout *r, uint32_t value, int shift)
{
    uint32_t mask = (1u << r->lanes) - 1;

    return shift == NO_FIELD ? 0 :
        ((value >> shift) & mask) << r->first_lane;
}

static void btc_ipc_update_irqs(Best1700BtcIpc *s)
{
    /* The always-on wake input observes request levels independently of
     * the CPU receive mask. It neither reads a message nor acknowledges it. */
    if (s->wake) {
        s->wake(s->wake_opaque,
                s->core_wakeup ||
                (s->port[BEST1700_BTC_IPC_SYS].ind |
                 s->port[BEST1700_BTC_IPC_BTH].ind) != 0);
    }
    for (unsigned side = 0; side < BEST1700_BTC_IPC_SIDES; side++) {
        Best1700BtcIpcPort *p = &s->port[side];
        uint8_t rx = s->port[side ^ 1].ind & p->rx_mask;
        uint8_t done = p->done & p->done_mask;

        for (unsigned lane = 0; lane < BEST1700_BTC_IPC_LANES; lane++) {
            qemu_set_irq(p->rx[lane], (rx >> lane) & 1);
            qemu_set_irq(p->tx_done[lane], (done >> lane) & 1);
        }
    }
}

static uint32_t btc_ipc_read_fields(Best1700BtcIpc *s, const BtcIpcLayout *r)
{
    Best1700BtcIpcPort *p = &s->port[r->side];
    uint8_t rx = s->port[r->side ^ 1].ind;

    return btc_ipc_field(r, p->done, r->done) |
        btc_ipc_field(r, p->ind, r->ind) |
        btc_ipc_field(r, p->rx_mask, r->rx_mask) |
        btc_ipc_field(r, p->done_mask, r->done_mask) |
        btc_ipc_field(r, rx, r->raw_rx) |
        btc_ipc_field(r, p->done, r->raw_done) |
        btc_ipc_field(r, rx & p->rx_mask, r->masked_rx) |
        btc_ipc_field(r, p->done & p->done_mask, r->masked_done);
}

static uint32_t btc_ipc_supported_fields(const BtcIpcLayout *r)
{
    return btc_ipc_field(r, 0xf, r->done) |
        btc_ipc_field(r, 0xf, r->ind) |
        btc_ipc_field(r, 0xf, r->rx_mask) |
        btc_ipc_field(r, 0xf, r->done_mask) |
        btc_ipc_field(r, 0xf, r->raw_rx) |
        btc_ipc_field(r, 0xf, r->raw_done) |
        btc_ipc_field(r, 0xf, r->masked_rx) |
        btc_ipc_field(r, 0xf, r->masked_done);
}

static void btc_ipc_write_fields(Best1700BtcIpc *s, const BtcIpcLayout *r,
                                 uint32_t value)
{
    Best1700BtcIpcPort *p = &s->port[r->side];
    /* Only control fields are extracted; status writes have no effect. */
    uint8_t ind = btc_ipc_extract(r, value, r->ind);
    uint8_t done = btc_ipc_extract(r, value, r->done);
    uint8_t rx_mask = btc_ipc_extract(r, value, r->rx_mask);
    uint8_t done_mask = btc_ipc_extract(r, value, r->done_mask);

    if (r->clear) {
        uint8_t completed = p->ind & ind;

        p->ind &= ~ind;
        p->done = (p->done & ~done) | completed;
        p->rx_mask &= ~rx_mask;
        p->done_mask &= ~done_mask;
    } else {
        p->ind |= ind;
        p->done |= done;
        p->rx_mask |= rx_mask;
        p->done_mask |= done_mask;
    }
}

static bool btc_ipc_read_register(Best1700BtcIpcBank *bank, hwaddr offset,
                                   uint32_t *value)
{
    size_t count;
    const BtcIpcLayout *layout = btc_ipc_layout(bank->id, &count);
    bool found = false;

    *value = 0;
    for (unsigned i = 0; i < count; i++) {
        if (layout[i].offset == offset) {
            *value |= btc_ipc_read_fields(bank->controller, &layout[i]);
            found = true;
        }
    }
    if (bank->id == BEST1700_BTC_IPC_BANK_BTC && offset == 0x50 &&
        bank->controller->core_wakeup) {
        *value |= BTC_CORE_WAKEUP_REQ;
        found = true;
    }
    return found;
}

static MemTxResult btc_ipc_write_register(Best1700BtcIpcBank *bank,
                                          hwaddr offset, uint32_t value)
{
    size_t count;
    const BtcIpcLayout *layout = btc_ipc_layout(bank->id, &count);
    uint32_t supported = 0;

    for (unsigned i = 0; i < count; i++) {
        if (layout[i].offset == offset) {
            supported |= btc_ipc_supported_fields(&layout[i]);
        }
    }
    if (bank->id == BEST1700_BTC_IPC_BANK_BTC &&
        (offset == 0x50 || offset == 0x54)) {
        supported |= BTC_CORE_WAKEUP_REQ;
    }
    /* Validate the entire physical word before changing either interface. */
    if (value & ~supported) {
        qemu_log_mask(LOG_UNIMP,
            "best1700 BTC IPC unsupported %s write offset=0x%"
            HWADDR_PRIx " value=0x%08" PRIx32 "\n",
            bank_names[bank->id], offset, value);
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < count; i++) {
        if (layout[i].offset == offset) {
            btc_ipc_write_fields(bank->controller, &layout[i], value);
        }
    }
    if (bank->id == BEST1700_BTC_IPC_BANK_BTC &&
        (offset == 0x50 || offset == 0x54) &&
        (value & BTC_CORE_WAKEUP_REQ)) {
        bank->controller->core_wakeup = offset == 0x50;
    }
    /* Empty clears and mask initialization cannot create completions. */
    btc_ipc_update_irqs(bank->controller);
    return MEMTX_OK;
}

static MemTxResult btc_ipc_window_read(void *opaque, hwaddr address,
                                      uint64_t *value, unsigned size,
                                      MemTxAttrs attrs)
{
    Best1700BtcIpcWindow *window = opaque;
    uint32_t word;

    btc_ipc_read_register(window->bank, window->offset, &word);
    *value = word >> (address * 8);
    return MEMTX_OK;
}

static MemTxResult btc_ipc_window_write(void *opaque, hwaddr address,
                                       uint64_t value, unsigned size,
                                       MemTxAttrs attrs)
{
    Best1700BtcIpcWindow *window = opaque;

    return btc_ipc_write_register(window->bank, window->offset,
                                  (uint32_t)value << (address * 8));
}

static const MemoryRegionOps btc_ipc_window_ops = {
    .read_with_attrs = btc_ipc_window_read,
    .write_with_attrs = btc_ipc_window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static uint32_t btc_ipc_endpoint_read(void *opaque, hwaddr offset,
                                     uint32_t *value)
{
    return btc_ipc_read_register(opaque, offset, value) ? UINT32_MAX : 0;
}

static uint32_t btc_ipc_endpoint_write(void *opaque, hwaddr offset,
                                      uint32_t value, uint32_t lane_mask)
{
    uint32_t ignored;

    if (!btc_ipc_read_register(opaque, offset, &ignored)) {
        return 0;
    }
    /* This legacy endpoint cannot propagate MemTxResult. */
    btc_ipc_write_register(opaque, offset, value & lane_mask);
    return UINT32_MAX;
}

Best1700AonEndpoint best1700_btc_ipc_endpoint(Best1700BtcIpc *s,
                                            Best1700BtcIpcBankId bank)
{
    assert(bank < BEST1700_BTC_IPC_BANKS);
    return (Best1700AonEndpoint) {
        .opaque = &s->bank_context[bank],
        .read = btc_ipc_endpoint_read,
        .write = btc_ipc_endpoint_write,
    };
}

void best1700_btc_ipc_init(Best1700BtcIpc *s, Object *owner)
{
    memset(s, 0, sizeof(*s));
    for (unsigned id = 0; id < BEST1700_BTC_IPC_BANKS; id++) {
        Best1700BtcIpcBank *bank = &s->bank_context[id];
        size_t count;
        unsigned reg = 0;
        const BtcIpcLayout *layout = btc_ipc_layout(id, &count);
        g_autofree char *bank_name = g_strdup_printf("best1700.btc-ipc.%s",
                                                     bank_names[id]);

        bank->controller = s;
        bank->id = id;
        memory_region_init(&s->bank[id], owner, bank_name,
                            BEST1700_BTC_IPC_BANK_SIZE);
        for (unsigned i = 0; i < count; i++) {
            Best1700BtcIpcWindow *window;
            g_autofree char *name = NULL;

            /* Entries sharing a physical word are adjacent in the table. */
            if (i && layout[i].offset == layout[i - 1].offset) {
                continue;
            }
            assert(reg < BEST1700_BTC_IPC_REGISTERS);
            window = &bank->window[reg++];
            name = g_strdup_printf("%s.%03" HWADDR_PRIx,
                                    bank_name, layout[i].offset);
            window->bank = bank;
            window->offset = layout[i].offset;
            memory_region_init_io(&window->region, owner, &btc_ipc_window_ops,
                                  window, name, sizeof(uint32_t));
            memory_region_add_subregion(&s->bank[id], window->offset,
                                         &window->region);
        }
    }
}

void best1700_btc_ipc_connect_irq(Best1700BtcIpc *s,
                                Best1700BtcIpcSide side, unsigned lane,
                                qemu_irq rx, qemu_irq done)
{
    assert(side < BEST1700_BTC_IPC_SIDES);
    assert(lane < BEST1700_BTC_IPC_LANES);
    s->port[side].rx[lane] = rx;
    s->port[side].tx_done[lane] = done;
    btc_ipc_update_irqs(s);
}

void best1700_btc_ipc_reset(Best1700BtcIpc *s)
{
    s->core_wakeup = false;
    for (unsigned side = 0; side < BEST1700_BTC_IPC_SIDES; side++) {
        Best1700BtcIpcPort *p = &s->port[side];

        p->ind = 0;
        p->done = 0;
        p->rx_mask = 0;
        p->done_mask = 0;
    }
    /* Reset is not a peer receive acknowledgment. */
    btc_ipc_update_irqs(s);
}

void best1700_btc_ipc_refresh(Best1700BtcIpc *s)
{
    btc_ipc_update_irqs(s);
}

void best1700_btc_ipc_connect_wake(Best1700BtcIpc *s, void *opaque,
                                  void (*wake)(void *opaque, bool level))
{
    s->wake_opaque = opaque;
    s->wake = wake;
    btc_ipc_update_irqs(s);
}
