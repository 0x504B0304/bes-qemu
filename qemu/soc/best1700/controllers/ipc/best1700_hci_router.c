/* Optional external HCI endpoint at the CP/BTC mailbox boundary.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Native BTC executes firmware-specific commands. Its RAM, pointer list,
 * control channel and TX acknowledgments remain guest-owned. External
 * packets use a separate read-only aperture and a CP-only pointer view;
 * they never overwrite the BTC heap or manufacture a native completion.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "best1700_hci_router.h"
#include "miband_hci_backend.h"
#include "miband_hci_bridge.h"

#define PACKET_MAX 2048
#define QUEUE_MAX 32
#define CHAIN_MAX 16
#define WINDOW_SIZE 4096

struct Best1700HciRouter {
    AddressSpace *cp;
    Best1700BtcIpc *ipc;
    MiBandHciBridge *bridge;
    MemoryRegion pointer_view, receive, ap_receive;
    uint8_t *table, *buffer;
    uint32_t receive_base;
    bool native[65536];
    bool active, failed, closed;
    unsigned head, count;
    uint8_t packets[QUEUE_MAX][PACKET_MAX];
    unsigned lengths[QUEUE_MAX];
    uint64_t tx, rx;
};

static uint8_t *ram_pointer(AddressSpace *as, uint32_t address, unsigned size)
{
    hwaddr offset, length = size;
    MemoryRegion *mr = address_space_translate(as, address, &offset, &length,
                                               false, MEMTXATTRS_UNSPECIFIED);
    return memory_region_is_ram(mr) && !memory_region_is_rom(mr) && length >= size ?
        memory_region_get_ram_ptr(mr) + offset : NULL;
}

static bool read_ram(Best1700HciRouter *s, uint32_t address, void *out, unsigned size)
{
    uint8_t *p;
    rcu_read_lock();
    p = ram_pointer(s->cp, address, size);
    if (p) {
        memcpy(out, p, size);
    }
    rcu_read_unlock();
    return p != NULL;
}

static void router_fail(void *opaque, const char *reason)
{
    Best1700HciRouter *s = opaque;
    if (!s->failed && !s->closed) {
        s->failed = true;
        miband_hci_bridge_close(s->bridge);
        error_report("BEST1700 external HCI stopped: %s", reason);
        /* Leave guest memory/registers available for diagnosis. */
        vm_stop(RUN_STATE_IO_ERROR);
    }
}

static void publish(Best1700HciRouter *s)
{
    if (s->active || !s->count || s->failed || s->closed ||
        (s->ipc->port[BEST1700_BTC_IPC_BTC_SYS].ind & 1)) {
        return;
    }
    unsigned length = s->lengths[s->head];
    memset(s->buffer, 0, WINDOW_SIZE);
    stl_le_p(s->buffer + 8, length);
    stl_le_p(s->buffer + 12, s->receive_base + 16);
    memcpy(s->buffer + 16, s->packets[s->head], length);
    memory_region_set_dirty(&s->receive, 0, 16 + length);
    s->head = (s->head + 1) % QUEUE_MAX;
    s->count--;
    s->active = true;
    best1700_btc_ipc_raise_external(s->ipc, BEST1700_BTC_IPC_SYS, 0);
}

static void receive_ready(void *opaque)
{
    Best1700HciRouter *s = opaque;
    uint8_t packet[PACKET_MAX];
    unsigned length;
    while (!s->failed && !s->closed &&
           (length = miband_hci_bridge_next_packet(s->bridge, packet))) {
        if ((packet[0] != 2 && packet[0] != 4) || s->count == QUEUE_MAX) {
            router_fail(s, "invalid packet type or receive queue exhausted");
            return;
        }
        unsigned slot = (s->head + s->count) % QUEUE_MAX;
        memcpy(s->packets[slot], packet, length);
        s->lengths[slot] = length;
        s->count++;
        s->rx++;
        publish(s);
    }
}

static void mailbox_changed(void *opaque)
{
    Best1700HciRouter *s = opaque;
    uint8_t desc[16], word[4];
    uint8_t packets[CHAIN_MAX][PACKET_MAX];
    unsigned lengths[CHAIN_MAX], count = 0;
    uint32_t node;
    int destination = -1;

    if (s->failed || s->closed) {
        return;
    }
    if (s->active && !(s->ipc->external_rx[BEST1700_BTC_IPC_SYS] & 1)) {
        s->active = false;
    }
    if (!(s->ipc->port[BEST1700_BTC_IPC_SYS].ind & 1)) {
        publish(s);
        return;
    }
    /* Snapshot and validate the entire list before any send or TXDONE. */
    if (!read_ram(s, ldl_le_p(s->table + 4), word, sizeof(word))) {
        router_fail(s, "unreadable CP transmit head");
        return;
    }
    node = ldl_le_p(word);
    while (node) {
        if (count == CHAIN_MAX || !read_ram(s, node, desc, sizeof(desc))) {
            router_fail(s, "invalid or cyclic CP descriptor list");
            return;
        }
        unsigned length = ldl_le_p(desc + 8);
        uint32_t data = ldl_le_p(desc + 12);
        if (length < 4 || length > PACKET_MAX ||
            !read_ram(s, data, packets[count], length)) {
            router_fail(s, "invalid CP HCI packet buffer");
            return;
        }
        uint8_t *packet = packets[count];
        int route;
        if (packet[0] == 1 && length == 4u + packet[3]) {
            route = s->native[lduw_le_p(packet + 1)];
        } else if (packet[0] == 2 && length >= 5 &&
                   length == 5u + lduw_le_p(packet + 3)) {
            route = 0;
        } else {
            router_fail(s, "invalid H4 command/ACL length or unsupported SCO/ISO");
            return;
        }
        if (destination != -1 && destination != route) {
            router_fail(s, "mixed native/external descriptor chain is not supported");
            return;
        }
        destination = route;
        lengths[count++] = length;
        node = ldl_le_p(desc);
    }
    if (!count) {
        router_fail(s, "empty CP descriptor list");
        return;
    }
    if (!destination) {
        for (unsigned i = 0; i < count; i++) {
            if (!miband_hci_bridge_queue(s->bridge, packets[i], lengths[i])) {
                router_fail(s, "external transmit queue exhausted");
                return;
            }
            s->tx++;
        }
        /* This is completion of copying the actual host-bound packet, not
         * an HCI success response. The controller supplies that response. */
        best1700_btc_ipc_take_tx(s->ipc, BEST1700_BTC_IPC_SYS, 0);
    }
    publish(s);
}

static uint64_t pointer_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700HciRouter *s = opaque;
    uint32_t pointer = s->active ? s->receive_base : ldl_le_p(s->table);
    return (pointer >> (offset * 8)) & (UINT32_MAX >> ((4 - size) * 8));
}

static void pointer_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Best1700HciRouter *s = opaque;
    /* CP initialization still writes its original table, even while a host
     * packet is visible. The BTC core always sees the unmodified RAM view. */
    for (unsigned i = 0; i < size; i++) {
        s->table[offset + i] = value >> (8 * i);
    }
}

static const MemoryRegionOps pointer_ops = {
    .read = pointer_read, .write = pointer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .impl.min_access_size = 1, .impl.max_access_size = 4,
};

static bool hex_value(const char *text, uint32_t maximum, uint32_t *value)
{
    char *end;
    unsigned long long n;
    if (!text[0] || strspn(text, "0123456789abcdefABCDEF") != strlen(text)) {
        return false;
    }
    errno = 0;
    n = strtoull(text, &end, 16);
    if (errno || *end || n > maximum) {
        return false;
    }
    *value = n;
    return true;
}

Best1700HciRouter *best1700_hci_router_new(Object *owner, AddressSpace *cp,
    MemoryRegion *cp_space, MemoryRegion *ap_space, Best1700BtcIpc *ipc,
    const char *layout, const char *backend, Error **errp)
{
    g_auto(GStrv) fields = g_strsplit(layout ? layout : "", ":", -1);
    unsigned field_count = g_strv_length(fields);
    uint32_t table, base;
    MiBandHciBackendConfig config;
    Best1700HciRouter *s;

    if (field_count < 2 || field_count > 130 ||
        !hex_value(fields[0], UINT32_MAX - 8, &table) || (table & 3) ||
        !hex_value(fields[1], UINT32_MAX - WINDOW_SIZE, &base) ||
        (base & (WINDOW_SIZE - 1))) {
        error_setg(errp, "invalid external HCI mailbox layout");
        return NULL;
    }
    /* No overlap is allowed: an aperture must never borrow live BTC RAM. */
    MemoryRegion *spaces[] = { cp_space, ap_space };
    for (unsigned i = 0; i < G_N_ELEMENTS(spaces); i++) {
        MemoryRegionSection section = memory_region_find(spaces[i], base, WINDOW_SIZE);
        if (section.mr) {
            memory_region_unref(section.mr);
            error_setg(errp, "external HCI receive aperture overlaps existing memory");
            return NULL;
        }
    }
    s = g_new0(Best1700HciRouter, 1);
    s->cp = cp;
    s->ipc = ipc;
    s->receive_base = base;
    rcu_read_lock();
    s->table = ram_pointer(cp, table, 8);
    rcu_read_unlock();
    if (!s->table) {
        error_setg(errp, "external HCI table is not writable RAM");
        g_free(s);
        return NULL;
    }
    for (unsigned i = 2; i < field_count; i++) {
        uint32_t opcode;
        if (!hex_value(fields[i], 65535, &opcode) || s->native[opcode]) {
            error_setg(errp, "invalid or duplicate native HCI opcode");
            g_free(s);
            return NULL;
        }
        s->native[opcode] = true;
    }
    if (!backend || !g_str_has_prefix(backend, "host-bridge-v2:")) {
        error_setg(errp, "external HCI requires an authenticated bridge session");
        g_free(s);
        return NULL;
    }
    if (!miband_hci_backend_parse(backend, &config, errp)) {
        miband_hci_backend_clear(&config);
        g_free(s);
        return NULL;
    }
    s->bridge = miband_hci_bridge_new((MiBandHciBridgeCallbacks) {
        s, receive_ready, router_fail
    });
    miband_hci_bridge_set_strict_rx(s->bridge, true);
    bool opened = miband_hci_bridge_open_proxy(s->bridge, config.path);
    miband_hci_backend_clear(&config);
    if (!opened) {
        error_setg(errp, "external HCI session handshake failed");
        miband_hci_bridge_free(s->bridge);
        g_free(s);
        return NULL;
    }
    memory_region_init_ram(&s->receive, NULL, "best1700.host-hci.receive",
                            WINDOW_SIZE, &error_fatal);
    memory_region_set_readonly(&s->receive, true);
    s->buffer = memory_region_get_ram_ptr(&s->receive);
    memory_region_add_subregion(cp_space, base, &s->receive);
    /* Read-only AP debug view for physical-memory clients; no table overlay. */
    memory_region_init_alias(&s->ap_receive, owner, "best1700.host-hci.debug",
                             &s->receive, 0, WINDOW_SIZE);
    memory_region_add_subregion(ap_space, base, &s->ap_receive);
    memory_region_init_io(&s->pointer_view, owner, &pointer_ops, s,
                          "best1700.host-hci.pointer", 4);
    memory_region_add_subregion_overlap(cp_space, table, &s->pointer_view, 10);
    ipc->external_opaque = s;
    ipc->external_changed = mailbox_changed;
    return s;
}

void best1700_hci_router_close(Best1700HciRouter *s)
{
    if (s) {
        s->closed = true;
        miband_hci_bridge_close(s->bridge);
    }
}

char *best1700_hci_router_status(Best1700HciRouter *s)
{
    if (!s) {
        return g_strdup("{\"state\":\"disabled\"}");
    }
    return g_strdup_printf("{\"state\":\"%s\",\"tx\":%" PRIu64
        ",\"rx\":%" PRIu64 ",\"queued\":%u,\"pending\":%s}",
        s->failed ? "failed" : s->closed ? "closed" : "attached", s->tx, s->rx,
        s->count, s->active ? "true" : "false");
}
