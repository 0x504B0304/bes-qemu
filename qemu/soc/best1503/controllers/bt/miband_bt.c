#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "sysemu/qtest.h"
#include "sysemu/runstate.h"
#include "miband_bt.h"
#include "miband_bt_serial.h"
#include "miband_hci_bridge.h"
#include "miband_hci_backend.h"
#include "miband_hci_synthetic.h"
#include "miband_hci_gatt.h"
#include "miband_xiaomi_protocol.h"
#include "miband_xiaomi_mass.h"
#include "miband_xiaomi_pb.h"

static void miband_bt_serial_read(void *opaque);
static void miband_bt_host_read(void *opaque);
static void miband_bt_bridge_read_ready(void *opaque);
static void miband_bt_bridge_failed(void *opaque, const char *reason);
static void miband_bt_log_packet(const char *direction, unsigned channel,
                                 const uint8_t *data, unsigned size);
static void miband_bt_send_reply(MiBandBt *bt, unsigned channel);
static bool miband_bt_queue_packet(MiBandBt *, unsigned, const uint8_t *, unsigned);
static void miband_bt_tx_done(void *opaque);
static bool miband_bt_consume_internal_event(MiBandBt *bt,
                                             const uint8_t *packet,
                                             unsigned size);
static bool miband_bt_rewrite_smp(MiBandBt *bt, uint8_t *data,
                                  unsigned size, const char *direction);
static bool miband_bt_auto_reply_user_passkey(MiBandBt *bt,
                                               const uint8_t *packet,
                                               unsigned size);
static void miband_bt_note_smp(MiBandBt *bt, const uint8_t *data,
                               unsigned size, const char *direction);
static void miband_bt_serial_kick(MiBandBt *bt);
static void miband_bt_stop_serial_worker(MiBandBt *bt);
static void miband_bt_gatt_write(MiBandHciGatt *gatt, void *opaque,
                                 uint16_t attribute, const uint8_t *value,
                                 size_t length, bool command);
static void miband_bt_serial_queued(MiBandBt *, unsigned, unsigned);
static void miband_bt_serial_promoted(MiBandBt *, unsigned, unsigned);
static void miband_bt_serial_delivered(MiBandBt *, unsigned);
static void miband_bt_serial_save_reset_stamp(MiBandBt *);

static void miband_bt_queue_install_result(MiBandBt *bt, MiBandHciGatt *gatt,
                                            const MiBandXiaomiPbRequest *request,
                                            uint8_t sequence)
{
    uint8_t pb[256], l2[300], l1[320];
    size_t pb_len = miband_xiaomi_pb_encode_install_result(request, pb,
                                                            sizeof(pb));
    size_t l2_len = miband_xiaomi_l2_encode(1, 1, pb, pb_len, l2, sizeof(l2));
    size_t l1_len = miband_xiaomi_l1_encode(MIBAND_XIAOMI_L1_DATA, false,
                                            sequence, l2, l2_len, l1,
                                            sizeof(l1));
    if (pb_len && l2_len && l1_len &&
        !miband_hci_gatt_queue_notification(gatt, l1, l1_len)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "miband BT Xiaomi install result queue full\n");
    }
}

#define BT_REG_BASE 0xd0220000
#define BT_REG_SIZE 0x10000
#define BT_DIGITAL_BASE 0xd0310000
#define BT_CMU_BASE 0xd0330000
#define BT_RF_BASE  0xd0350000
#define BT_MAILBOX_SET 0x400000a0
#define BT_MAILBOX_CLR 0x400000a4
#define BT_DATA_IND(channel) (1u << (2 + (channel)))
#define BT_TX_DONE(channel) (1u << (channel))
#define BT_TX_MASK(channel) (1u << (15 + (channel)))
#define BT_TX_INTR_MASKED(channel) (1u << (23 + (channel)))
#define BT_RX_IND(channel) (1u << (4 + (channel)))
#define BT_RX_MASK(channel) (1u << (13 + (channel)))
#define BT_RX_INTR_MASKED(channel) (1u << (21 + (channel)))
#define BT_CMU_RX_DONE 0xd0330054
#define BT_CMU_ISIRQ_SET 0xd0330050
#define BT_RAM_BASE 0xc0000000
#define BT_AP_TX_HEAD(channel) (0x4004 + (channel) * 8)
#define BT_PEER_RX_HEAD(channel) (0x4000 + (channel) * 8)
#define BT_RX_NODE(channel) (0x4100 + (channel) * 0x200)
#define BT_RX_PAYLOAD(channel) (BT_RX_NODE(channel) + 0x20)
/* Controller processing latency, not a measured BES hardware constant. */
#define BT_REPLY_DELAY_NS 1000000

/* Keep per-register/mailbox/ACL logging out of the worker performance path.
 * Explicit verbose diagnostics and the legacy backend retain those records. */
#define miband_bt_trace(bt, ...) do { \
    if (!(bt)->serial_worker || getenv("MIBAND_HCI_VERBOSE")) { \
        qemu_log_mask(LOG_UNIMP, __VA_ARGS__); \
    } \
} while (0)

static void miband_bt_close_host(MiBandBt *bt)
{
    miband_bt_stop_serial_worker(bt);
    miband_hci_bridge_close(bt->bridge);
    bt->host_active = false;
    bt->serial_active = false;
    bt->internal_cmd_count = 0;
    bt->diagnostic_deadline_us = 0;
    bt->rx_flow_active = false;
    bt->rx_flow_stage = 0;
    if (bt->rx_flow_timer) timer_del(bt->rx_flow_timer);
}

static bool miband_bt_mass_type_supported(const MiBandBt *bt, uint8_t type)
{
    if (!bt->mass_types || !bt->mass_type_count) {
        return true;
    }
    for (unsigned i = 0; i < bt->mass_type_count; i++) {
        if (bt->mass_types[i] == type) {
            return true;
        }
    }
    return false;
}

static bool miband_bt_host_queue(MiBandBt *bt, const uint8_t *data,
                                 unsigned size)
{
    if (bt->serial_worker) {
        return bt->host_active && miband_serial_send(bt->serial_worker, data, size);
    }
    return bt->host_active && miband_hci_bridge_queue(bt->bridge, data, size);
}

static void miband_bt_bridge_read_ready(void *opaque)
{
    MiBandBt *bt = opaque;
    if (bt->serial_active) miband_bt_serial_read(bt);
    else miband_bt_host_read(bt);
}

static void miband_bt_bridge_failed(void *opaque, const char *reason)
{
    miband_bt_close_host(opaque);
    error_setg(&error_fatal, "%s", reason);
}

static bool miband_bt_queue_internal_command(MiBandBt *bt, uint16_t opcode,
                                              const uint8_t *params,
                                              unsigned params_len)
{
    uint8_t packet[4 + 255];

    if (!bt->host_active || params_len > 255 ||
        bt->internal_cmd_count >= ARRAY_SIZE(bt->internal_cmd)) {
        return false;
    }
    packet[0] = 0x01;
    packet[1] = opcode;
    packet[2] = opcode >> 8;
    packet[3] = params_len;
    if (params_len) {
        memcpy(packet + 4, params, params_len);
    }
    if (!miband_bt_host_queue(bt, packet, 4 + params_len)) {
        return false;
    }
    bt->internal_cmd[bt->internal_cmd_count++] = opcode;
    qemu_log_mask(LOG_UNIMP,
                  "miband BT internal HCI command opcode=%04x len=%u\n",
                  opcode, params_len);
    return true;
}

static void miband_bt_forget_internal(MiBandBt *bt, unsigned i)
{
    memmove(bt->internal_cmd + i, bt->internal_cmd + i + 1,
            (bt->internal_cmd_count - i - 1) * sizeof(bt->internal_cmd[0]));
    bt->internal_cmd_count--;
}

static void miband_bt_flow_timeout(void *opaque)
{
    MiBandBt *bt = opaque;
    error_setg(&error_fatal, "physical RX flow setup timed out at stage %u", bt->rx_flow_stage);
}

/* Reserve only four controller->host ACL packets. ESP32 rejects zero SCO
 * buffer descriptors even in ACL-only mode; the following values were accepted
 * by the real controller. No SCO traffic is enabled by flow mode 1. */
static void miband_bt_flow_complete(MiBandBt *bt, uint16_t opcode, uint8_t status)
{
    uint16_t expected = bt->rx_flow_stage == 1 ? 0x0c33 : 0x0c31;
    if (opcode != expected || status) {
        error_setg(&error_fatal, "physical RX flow setup failed: opcode=%04x status=%02x stage=%u",
                   opcode, status, bt->rx_flow_stage);
    }
    if (bt->rx_flow_stage == 1) {
        const uint8_t acl_only = 1;
        bt->rx_flow_stage = 2;
        if (!miband_bt_queue_internal_command(bt, 0x0c31, &acl_only, 1))
            error_setg(&error_fatal, "cannot queue physical RX flow enable");
    } else {
        bt->rx_flow_stage = 0;
        bt->rx_flow_active = true;
        timer_del(bt->rx_flow_timer);
        qemu_log_mask(LOG_UNIMP, "miband BT RX flow active window=4 acl=1021 mode=1\n");
        if (!miband_bt_queue_packet(bt, bt->reset_channel, bt->saved_reset_complete, 7))
            error_setg(&error_fatal, "cannot deliver original Reset completion");
        miband_bt_send_reply(bt, bt->reset_channel);
    }
}

static bool miband_bt_consume_internal_event(MiBandBt *bt,
                                             const uint8_t *packet,
                                             unsigned size)
{
    uint16_t opcode;
    unsigned i;

    /* Command Complete: event, event code, length, num packets, opcode. */
    if (size < 7 || packet[0] != 0x04 || packet[1] != 0x0e ||
        packet[2] < 4) {
        return false;
    }
    opcode = packet[4] | (packet[5] << 8);
    if (opcode == 0x0c03 && size == 7 && packet[6] == 0 &&
            bt->rx_flow_requested && bt->serial_active) {
        static const uint8_t buffers[] = {0xfd, 3, 64, 4, 0, 1, 0};
        memcpy(bt->saved_reset_complete, packet, 7);
        miband_bt_serial_save_reset_stamp(bt);
        bt->rx_flow_active = false;
        memset(bt->connection_live, 0, sizeof(bt->connection_live));
        memset(bt->delivered_generation, 0, sizeof(bt->delivered_generation));
        bt->rx_flow_stage = 1;
        timer_mod(bt->rx_flow_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 10000000000LL);
        if (!miband_bt_queue_internal_command(bt, 0x0c33, buffers, sizeof(buffers)))
            error_setg(&error_fatal, "cannot queue physical Host Buffer Size");
        return true;
    }
    /* These schemas belong to our bridge only. Always absorb even unsolicited,
     * duplicated or late diagnostics, never inject them into the AP stack. */
    unsigned page = UINT_MAX;
    if (opcode == 0xfc02 && packet[6] == 0 && size == 3u + packet[2]) {
        if (size == 100 && packet[7] == 1) page = 0;
        if (size >= 10 && packet[7] == 2 && packet[8] >= 1 && packet[8] <= 3 &&
            packet[9] == (packet[8] == 3 ? 16 : 48) && size == 10u + packet[9]*4u)
            page = packet[8];
    }
    if (page != UINT_MAX) {
        if (page == bt->diagnostic_page) {
            for (i = 0; i < bt->internal_cmd_count; i++) {
                if (bt->internal_cmd[i] == opcode) { miband_bt_forget_internal(bt, i); break; }
            }
            bt->diagnostic_deadline_us = 0;
        }
        qemu_log_mask(LOG_UNIMP, "miband BT diagnostic absorbed page=%u\n", page);
        return true;
    }
    for (i = 0; i < bt->internal_cmd_count; i++) {
        if (bt->internal_cmd[i] != opcode) {
            continue;
        }
        miband_bt_forget_internal(bt, i);
        qemu_log_mask(LOG_UNIMP,
                      "miband BT consumed internal HCI complete opcode=%04x status=%02x\n",
                      opcode, packet[6]);
        if (bt->rx_flow_stage && (opcode == 0x0c33 || opcode == 0x0c31))
            miband_bt_flow_complete(bt, opcode, packet[6]);
        return true;
    }
    return false;
}

static bool miband_bt_diagnostic_ready(MiBandBt *bt, unsigned page)
{
    if (!bt->serial_active || bt->rx_flow_stage) return false;
    for (unsigned i = 0; i < bt->internal_cmd_count; i++) {
        if (bt->internal_cmd[i] != 0xfc02) continue;
        if (g_get_monotonic_time() < bt->diagnostic_deadline_us) return false;
        miband_bt_forget_internal(bt, i);
        qemu_log_mask(LOG_UNIMP, "miband BT diagnostic expired; retry enabled\n");
        break;
    }
    bt->diagnostic_page = page;
    bt->diagnostic_deadline_us = g_get_monotonic_time() + 5000000;
    return true;
}

bool miband_bt_request_transport_stats(MiBandBt *bt)
{
    static const uint8_t magic[]={'S','T','A','1'};
    if (!miband_bt_diagnostic_ready(bt, 0)) return false;
    return miband_bt_queue_internal_command(bt,0xfc02,magic,sizeof(magic));
}

bool miband_bt_request_transport_debug(MiBandBt *bt, unsigned page)
{
    uint8_t magic[] = {'D', 'B', 'G', '0' + page};
    if (page < 1 || page > 3 || !miband_bt_diagnostic_ready(bt, page)) return false;
    qemu_log_mask(LOG_UNIMP, "miband BT transport debug page=%u\n", page);
    return miband_bt_queue_internal_command(bt, 0xfc02, magic, sizeof(magic));
}

/*
 * Classic HCI exposes a real User Passkey Request event to the host.  When
 * the machine is explicitly configured with bt-passkey, answer that event at
 * the HCI boundary with the standard User Passkey Request Reply command.  The
 * event is still delivered to the AP host, so this is an observable controller
 * hook rather than a fabricated pairing-success event.  LE SMP passkeys are
 * intentionally not handled here: their TK/DHKey calculation belongs to the
 * AP host and cannot be safely replaced by an HCI packet edit.
 */
static bool miband_bt_auto_reply_user_passkey(MiBandBt *bt,
                                               const uint8_t *packet,
                                               unsigned size)
{
    uint8_t command[14];
    uint32_t passkey;

    if (!bt->fixed_passkey || size != 9 || packet[0] != 0x04 ||
        packet[1] != 0x16 || packet[2] != 0x06) {
        return false;
    }
    passkey = bt->fixed_passkey;
    command[0] = 0x01;
    command[1] = 0x2e; /* HCI User Passkey Request Reply. */
    command[2] = 0x0c;
    command[3] = 10;
    memcpy(command + 4, packet + 3, 6);
    command[10] = passkey;
    command[11] = passkey >> 8;
    command[12] = passkey >> 16;
    command[13] = passkey >> 24;
    if (!miband_bt_host_queue(bt, command, sizeof(command))) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT fixed passkey reply queue failed\n");
        return false;
    }
    bt->passkey_auto_replies++;
    qemu_log_mask(LOG_UNIMP,
                  "miband BT fixed passkey reply injected value=%06u\n",
                  passkey);
    return true;
}

static bool miband_bt_queue_packet(MiBandBt *bt, unsigned channel,
                                   const uint8_t *data, unsigned size)
{
    unsigned tail;

    if (channel >= 2 || size == 0 || size > MIBAND_BT_PACKET_MAX) {
        return false;
    }
    if (bt->rx_queue_count[channel] >= MIBAND_BT_RX_QUEUE_LENGTH) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT RX queue full channel=%u len=%u\n",
                      channel, size);
        return false;
    }
    tail = (bt->rx_queue_head[channel] + bt->rx_queue_count[channel]) %
           MIBAND_BT_RX_QUEUE_LENGTH;
    memcpy(bt->rx_queue[channel][tail], data, size);
    bt->rx_queue_len[channel][tail] = size;
    bt->rx_queue_generation[channel][tail] =
        bt->rx_flow_active && size >= 5 && data[0] == 2 ?
        bt->connection_generation[lduw_le_p(data + 1) & 0xfff] : 0;
    miband_bt_serial_queued(bt, channel, tail);
    bt->rx_queue_count[channel]++;
    return true;
}

static bool miband_bt_promote_packet(MiBandBt *bt, unsigned channel)
{
    unsigned head;

    if (bt->reply_len[channel] || !bt->rx_queue_count[channel]) {
        return bt->reply_len[channel] != 0;
    }
    head = bt->rx_queue_head[channel];
    bt->reply_len[channel] = bt->rx_queue_len[channel][head];
    bt->reply_generation[channel] = bt->rx_queue_generation[channel][head];
    memcpy(bt->reply[channel], bt->rx_queue[channel][head],
           bt->reply_len[channel]);
    miband_bt_serial_promoted(bt, channel, head);
    bt->rx_queue_head[channel] = (head + 1) % MIBAND_BT_RX_QUEUE_LENGTH;
    bt->rx_queue_count[channel]--;
    miband_bt_serial_kick(bt); /* Capacity became available; no polling BH. */
    return true;
}

static void miband_bt_poll_host(MiBandBt *bt, unsigned channel)
{
    uint8_t packet[MIBAND_BT_PACKET_MAX];
    unsigned size;

    if (!bt->host_active || bt->serial_active || channel >= 2) {
        return;
    }
    for (;;) {
        size = miband_hci_bridge_next_packet(bt->bridge, packet);
        if (!size) return;
        if (miband_bt_consume_internal_event(bt, packet, (unsigned)size)) {
            continue;
        }
        (void)miband_bt_auto_reply_user_passkey(bt, packet, (unsigned)size);
        (void)miband_bt_rewrite_smp(bt, packet, (unsigned)size,
                                    "BT->AP");
        miband_bt_note_smp(bt, packet, (unsigned)size, "BT->AP");
        if (miband_bt_queue_packet(bt, channel, packet, (unsigned)size)) {
            bt->host_events++;
            bt->host_pending[channel] = false;
            qemu_log_mask(LOG_UNIMP,
                          "miband BT host event channel=%u len=%u\n",
                          channel, (unsigned)size);
        }
    }
}

/* Raw HCI sockets deliver controller events independently of AP mailbox
 * reads.  Keep the same response-queue/IRQ path as the UART backend so an
 * advertising report or LE connection event cannot sit unnoticed until the
 * next AP poll. */
static void miband_bt_host_read(void *opaque)
{
    MiBandBt *bt = opaque;

    if (!bt->host_active || bt->serial_active) {
        return;
    }
    for (;;) {
        unsigned before = bt->host_events;

        miband_bt_poll_host(bt, 0);
        miband_bt_poll_host(bt, 1);
        if (bt->host_events == before) {
            break;
        }
        miband_bt_send_reply(bt, 0);
        miband_bt_send_reply(bt, 1);
    }
}

/*
 * Optional diagnostic pairing policy.  It only changes the SMP Pairing
 * Request/Response carried in ACL data: no pairing-success event or key is
 * fabricated.  Clearing MITM and selecting NoInputNoOutput makes both hosts
 * negotiate BLE Just Works, which is useful for bringing up the real HCI
 * path before implementing a passkey-backed SMP trace.
 */
static bool miband_bt_rewrite_smp(MiBandBt *bt, uint8_t *data,
                                  unsigned size, const char *direction)
{
    unsigned acl_len;
    unsigned l2cap_len;
    uint16_t cid;
    uint8_t code;

    if (!bt->smp_just_works || size < 16 || data[0] != 0x02) {
        return false;
    }
    acl_len = data[3] | (data[4] << 8);
    if (acl_len + 5 > size || acl_len < 11) {
        return false;
    }
    l2cap_len = data[5] | (data[6] << 8);
    cid = data[7] | (data[8] << 8);
    if (cid != 0x0006 || l2cap_len < 7 || 9 + l2cap_len > size) {
        return false;
    }
    code = data[9];
    if (code != 0x01 && code != 0x02) {
        return false;
    }
    /* SMP Pairing Request/Response: IO capability, OOB flag, auth_req. */
    data[10] = 0x03;
    data[11] = 0x00;
    data[12] &= (uint8_t)~0x04; /* MITM */
    qemu_log_mask(LOG_UNIMP,
                  "miband BT SMP just-works rewrite direction=%s code=%02x\n",
                  direction, code);
    return true;
}

/*
 * The BLE host in the AP obtains the passkey random value through HCI LE
 * Rand. Remember the pairing boundary so an optional fixed-key policy can
 * replace only the first random request belonging to that pairing. The
 * remaining random values stay controller-generated; replacing all of them
 * would change the SMP confirmation/encryption transcript.
 */
static void miband_bt_note_smp(MiBandBt *bt, const uint8_t *data,
                               unsigned size, const char *direction)
{
    unsigned acl_len;
    unsigned l2cap_len;
    uint16_t cid;
    uint8_t code;

    if (!bt->le_passkey || size < 10 || data[0] != 0x02) {
        return;
    }
    acl_len = data[3] | (data[4] << 8);
    if (acl_len + 5 > size || acl_len < 5) {
        return;
    }
    l2cap_len = data[5] | (data[6] << 8);
    cid = data[7] | (data[8] << 8);
    if (cid != 0x0006 || l2cap_len == 0 || 9 + l2cap_len > size) {
        return;
    }
    code = data[9];
    if (code == 0x01 || code == 0x02) {
        bt->le_smp_active = true;
        bt->le_passkey_rand_pending = true;
        qemu_log_mask(LOG_UNIMP,
                      "miband BT LE SMP pairing %s code=%02x; next LE Rand is fixed passkey seed\n",
                      direction, code);
    } else if (code == 0x05) {
        bt->le_smp_active = false;
        bt->le_passkey_rand_pending = false;
        qemu_log_mask(LOG_UNIMP,
                      "miband BT LE SMP pairing failed direction=%s\n",
                      direction);
    }
}

static void miband_bt_flow_note(MiBandBt *bt, const uint8_t *p, unsigned size)
{
    if (!bt->rx_flow_active) return;
    unsigned handle;
    if (size >= 7 && p[0] == 4 && p[1] == 3 && p[3] == 0) {
        handle = lduw_le_p(p + 4) & 0xfff;
    } else if (size >= 8 && p[0] == 4 && p[1] == 0x3e &&
               (p[3] == 1 || p[3] == 0x0a) && p[4] == 0) {
        handle = lduw_le_p(p + 5) & 0xfff;
    } else {
        if (size == 7 && p[0] == 4 && p[1] == 5 && p[3] == 0)
            bt->connection_live[lduw_le_p(p + 4) & 0xfff] = false;
        if (size >= 5 && p[0] == 2) {
            handle = lduw_le_p(p + 1) & 0xfff;
            if (!bt->connection_live[handle])
                error_setg(&error_fatal, "ACL received for unknown physical handle=%u", handle);
            bt->rx_flow_received++;
            miband_bt_trace(bt, "miband BT RX flow receive handle=%u total=%u\n", handle, bt->rx_flow_received);
        }
        return;
    }
    if (++bt->connection_sequence == 0) ++bt->connection_sequence;
    bt->connection_generation[handle] = bt->connection_sequence;
    bt->connection_live[handle] = true;
}

static void miband_bt_flow_ack(MiBandBt *bt, unsigned channel)
{
    uint32_t generation = bt->delivered_generation[channel];
    unsigned handle = bt->delivered_handle[channel];
    bt->delivered_generation[channel] = 0;
    if (!bt->rx_flow_active || !generation || !bt->connection_live[handle] ||
            generation != bt->connection_generation[handle]) return;
    /* Host Number Of Completed Packets is exempt from command credits and
     * has no normal Command Complete. Return exactly one consumed ACL buffer. */
    uint8_t command[] = {1, 0x35, 0x0c, 5, 1, handle, handle >> 8, 1, 0};
    if (!miband_bt_host_queue(bt, command, sizeof(command)))
        error_setg(&error_fatal, "cannot return physical ACL RX credit");
    bt->rx_flow_returned++;
    miband_bt_trace(bt, "miband BT RX flow credit handle=%u total=%u\n", handle, bt->rx_flow_returned);
}

static void miband_bt_serial_read(void *opaque)
{
    MiBandBt *bt = opaque;
    uint8_t packet[MIBAND_BT_PACKET_MAX];
    unsigned size;
    if (!bt->serial_active || bt->serial_worker) return;
    while ((size = miband_hci_bridge_next_packet(bt->bridge, packet))) {
        miband_bt_log_packet("controller->QEMU", 0, packet, size);
        if (miband_bt_consume_internal_event(bt, packet, size)) continue;
        miband_bt_flow_note(bt, packet, size);
        (void)miband_bt_auto_reply_user_passkey(bt, packet, size);
        (void)miband_bt_rewrite_smp(bt, packet, size, "BT->AP");
        miband_bt_note_smp(bt, packet, size, "BT->AP");
        if (miband_bt_queue_packet(bt, 0, packet, size)) {
            bt->host_events++;
            bt->host_pending[0] = false;
            qemu_log_mask(LOG_UNIMP, "miband BT serial H4 packet type=%02x len=%u\n",
                          packet[0], size);
            miband_bt_send_reply(bt, 0);
        } else {
            error_setg(&error_fatal, "miband BT AP RX queue overflow; physical controller packet would be lost");
        }
    }
}

typedef struct MiBandSerialMain {
    MiBandBt *bt;
    CPUState *cpu;
    MiBandBtSerial *io;
    QEMUBH *bh;
    QEMUTimer *stats_timer;
    VMChangeStateEntry *vm_handler;
    unsigned refs;
    bool closing;
    bool commit_pending;
    uint64_t epoch;
    uint64_t commits;
    uint64_t admitted;
    uint64_t internal_consumed;
    uint64_t publications;
    uint64_t acknowledged;
    uint64_t rx_publications;
    uint64_t rx_acknowledged;
    uint64_t max_admit_ns;
    uint64_t max_publication_ns;
    uint64_t max_publication_ack_ns;
    uint64_t max_read_ack_ns;
    uint64_t max_commit_ns;
    uint64_t current_rx_ns;
    uint64_t saved_reset_rx_ns;
    uint64_t ap_rx_ns[2][MIBAND_BT_RX_QUEUE_LENGTH];
    uint64_t reply_rx_ns[2];
    uint64_t published_ns[2];
    uint64_t published_rx_ns[2];
    bool publication_pending[2];
    uint64_t admit_hist[64];
    uint64_t publication_hist[64];
    uint64_t publication_ack_hist[64];
    uint64_t read_ack_hist[64];
    uint64_t commit_hist[64];
} MiBandSerialMain;

/* Log2 buckets return conservative upper bounds, never invented exact latency
 * samples. No allocation/formatting happens for each sample. */
static void miband_bt_histogram(uint64_t hist[64], uint64_t ns)
{
    unsigned bucket = 0;
    while (ns > 1 && bucket < 63) {
        ns = (ns >> 1) + (ns & 1);
        bucket++;
    }
    hist[bucket]++;
}

static uint64_t miband_bt_percentile(const uint64_t hist[64], unsigned percent)
{
    uint64_t count = 0, seen = 0;
    for (unsigned i = 0; i < 64; i++) {
        count += hist[i];
    }
    if (!count) {
        return 0;
    }
    uint64_t target = (count * percent + 99) / 100;
    for (unsigned i = 0; i < 64; i++) {
        seen += hist[i];
        if (seen >= target) {
            return UINT64_C(1) << i;
        }
    }
    return UINT64_MAX;
}

static void miband_bt_serial_queued(MiBandBt *bt, unsigned channel, unsigned tail)
{
    if (bt->serial_worker) {
        MiBandSerialMain *m = miband_serial_opaque(bt->serial_worker);
        m->ap_rx_ns[channel][tail] = m->current_rx_ns;
        if (m->saved_reset_rx_ns && channel == bt->reset_channel &&
            bt->rx_queue_len[channel][tail] == sizeof(bt->saved_reset_complete) &&
            !memcmp(bt->rx_queue[channel][tail], bt->saved_reset_complete,
                    sizeof(bt->saved_reset_complete))) {
            /* RX flow setup holds the original Reset event across two later
             * completions. Keep that event's own arrival timestamp. */
            m->ap_rx_ns[channel][tail] = m->saved_reset_rx_ns;
            m->saved_reset_rx_ns = 0;
        }
    }
}

static void miband_bt_serial_save_reset_stamp(MiBandBt *bt)
{
    if (bt->serial_worker) {
        MiBandSerialMain *m = miband_serial_opaque(bt->serial_worker);
        m->saved_reset_rx_ns = m->current_rx_ns;
    }
}

static void miband_bt_serial_promoted(MiBandBt *bt, unsigned channel, unsigned head)
{
    if (bt->serial_worker) {
        MiBandSerialMain *m = miband_serial_opaque(bt->serial_worker);
        m->reply_rx_ns[channel] = m->ap_rx_ns[channel][head];
        m->ap_rx_ns[channel][head] = 0;
    }
}

static void miband_bt_serial_delivered(MiBandBt *bt, unsigned channel)
{
    if (bt->serial_worker) {
        MiBandSerialMain *m = miband_serial_opaque(bt->serial_worker);
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        m->publications++;
        m->publication_pending[channel] = true;
        m->published_ns[channel] = now;
        m->published_rx_ns[channel] = m->reply_rx_ns[channel];
        if (m->reply_rx_ns[channel] && now >= m->reply_rx_ns[channel]) {
            uint64_t age = now - m->reply_rx_ns[channel];
            m->rx_publications++;
            miband_bt_histogram(m->publication_hist, age);
            m->max_publication_ns = MAX(m->max_publication_ns, age);
        }
        m->reply_rx_ns[channel] = 0;
    }
}

/* BT_CMU_RX_DONE releases the current mailbox buffer. It does not prove the
 * AP application processed the payload, wrote storage, or installed anything.
 * One live publication token per channel prevents duplicate ACK accounting. */
static void miband_bt_serial_acknowledged(MiBandBt *bt, unsigned channel)
{
    if (bt->serial_worker) {
        MiBandSerialMain *m = miband_serial_opaque(bt->serial_worker);
        if (!m->publication_pending[channel]) {
            return;
        }
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        m->publication_pending[channel] = false;
        m->acknowledged++;
        if (now >= m->published_ns[channel]) {
            uint64_t age = now - m->published_ns[channel];
            miband_bt_histogram(m->publication_ack_hist, age);
            m->max_publication_ack_ns = MAX(m->max_publication_ack_ns, age);
        }
        if (m->published_rx_ns[channel] && now >= m->published_rx_ns[channel]) {
            uint64_t age = now - m->published_rx_ns[channel];
            m->rx_acknowledged++;
            miband_bt_histogram(m->read_ack_hist, age);
            m->max_read_ack_ns = MAX(m->max_read_ack_ns, age);
        }
        m->published_ns[channel] = 0;
        m->published_rx_ns[channel] = 0;
    }
}

/* All adapter fields and device state belong to the BQL domain. The IO worker
 * only sees the immutable BH pointer, and is joined before that BH is deleted.
 * A queued CPU job owns a separate reference, so shutdown never waits for a
 * stopped CPU and a late job cannot access a destroyed device/transport. */
static void miband_bt_serial_main_unref(MiBandSerialMain *m)
{
    if (!--m->refs) {
        g_free(m);
    }
}

static bool miband_bt_serial_running(void)
{
    return qtest_enabled() || runstate_is_running();
}

static void miband_bt_serial_notify(void *opaque)
{
    MiBandSerialMain *m = opaque;
    qemu_bh_schedule(m->bh);
}

static void miband_bt_serial_kick(MiBandBt *bt)
{
    if (bt->serial_worker) {
        MiBandSerialMain *m = miband_serial_opaque(bt->serial_worker);
        if (!m->closing) {
            qemu_bh_schedule(m->bh);
        }
    }
}

static void miband_bt_serial_check_error(MiBandSerialMain *m)
{
    MiBandSerialStats stats;
    miband_serial_stats(m->io, &stats);
    if (stats.failed) {
        error_setg(&error_fatal,
                   "miband physical HCI transport epoch=%" PRIu64
                   " failed: %s; rx=%" PRIu64 " bytes, queued=%u/%u bytes; "
                   "refusing synthetic fallback or H4 resynchronization",
                   stats.epoch, stats.error, stats.rx_bytes,
                   stats.rx_count, stats.rx_bytes_queued);
    }
}

static void miband_bt_serial_commit(CPUState *cpu, run_on_cpu_data data)
{
    MiBandSerialMain *m = data.host_ptr;
    MiBandSerialPacket packet;
    int64_t started;
    unsigned count = 0;
    m->commit_pending = false;
    if (m->closing || !miband_bt_serial_running()) {
        miband_bt_serial_main_unref(m);
        return;
    }
    miband_bt_serial_check_error(m);
    started = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    /* Ordinary async_run_on_cpu runs with BQL, outside translated execution.
     * This is the only new consumer of received packets; it also commits the
     * BT shared RAM and IRQ transaction at this CPU boundary. */
    while (count < 32 && m->bt->rx_queue_count[0] < MIBAND_BT_RX_QUEUE_LENGTH &&
           miband_serial_peek(m->io, &packet)) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (count && now - started >= 200000) {
            break;
        }
        if (packet.epoch != m->epoch) {
            error_setg(&error_fatal, "stale physical HCI transport epoch");
        }
        MiBandBt *bt = m->bt;
        m->current_rx_ns = packet.received_ns;
        if (miband_bt_consume_internal_event(bt, packet.data, packet.length)) {
            m->internal_consumed++;
        } else {
            miband_bt_flow_note(bt, packet.data, packet.length);
            (void)miband_bt_auto_reply_user_passkey(bt, packet.data, packet.length);
            (void)miband_bt_rewrite_smp(bt, packet.data, packet.length, "BT->AP");
            miband_bt_note_smp(bt, packet.data, packet.length, "BT->AP");
            if (!miband_bt_queue_packet(bt, 0, packet.data, packet.length)) {
                error_setg(&error_fatal, "physical HCI RX admission invariant failed");
            }
            bt->host_events++;
            bt->host_pending[0] = false;
            miband_bt_send_reply(bt, 0);
        }
        m->current_rx_ns = 0;
        miband_serial_consume(m->io);
        count++;
        m->admitted++;
        /* Both clocks are host monotonic nanoseconds on the Linux transport. */
        uint64_t age = (uint64_t)now >= packet.received_ns ?
                       (uint64_t)now - packet.received_ns : 0;
        miband_bt_histogram(m->admit_hist, age);
        m->max_admit_ns = MAX(m->max_admit_ns, age);
    }
    miband_bt_send_reply(m->bt, 0);
    miband_bt_send_reply(m->bt, 1);
    m->commits++;
    uint64_t commit_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - started;
    m->max_commit_ns = MAX(m->max_commit_ns, commit_ns);
    miband_bt_histogram(m->commit_hist, commit_ns);
    /* Do not reschedule while the AP queue is full. Promotion/ack and resume
     * are explicit wakeups; draining a CPU job never spins on backpressure. */
    if (m->bt->rx_queue_count[0] < MIBAND_BT_RX_QUEUE_LENGTH &&
        miband_serial_peek(m->io, &packet)) {
        qemu_bh_schedule(m->bh);
    }
    miband_bt_serial_main_unref(m);
}

static void miband_bt_serial_bh(void *opaque)
{
    MiBandSerialMain *m = opaque;
    if (m->closing) {
        return;
    }
    /* Physical failures remain visible during guest debug stop. */
    miband_bt_serial_check_error(m);
    if (m->commit_pending || !miband_bt_serial_running()) {
        return;
    }
    m->commit_pending = true;
    m->refs++;
    if (qtest_enabled()) {
        miband_bt_serial_commit(m->cpu, RUN_ON_CPU_HOST_PTR(m));
    } else {
        async_run_on_cpu(m->cpu, miband_bt_serial_commit, RUN_ON_CPU_HOST_PTR(m));
    }
}

static void miband_bt_serial_vm_state(void *opaque, bool running, RunState state)
{
    MiBandSerialMain *m = opaque;
    if (running && !m->closing) {
        qemu_bh_schedule(m->bh);
    }
}

static void miband_bt_serial_metrics(void *opaque)
{
    MiBandSerialMain *m = opaque;
    MiBandSerialStats s;
    if (m->closing) {
        return;
    }
    miband_serial_stats(m->io, &s);
    qemu_log_mask(LOG_UNIMP,
        "miband BT worker epoch=%" PRIu64 " rx=%" PRIu64 "/%" PRIu64
        " tx=%" PRIu64 "/%" PRIu64 " rx-queue=%u/%u rx-high=%u/%u"
        " tx-queue=%u tx-high=%u oldest-ns=%" PRIu64
        " read-gap-including-idle-max-ns=%" PRIu64 " read-eagain=%" PRIu64
        " write-eagain=%" PRIu64 " commits=%" PRIu64
        " admitted=%" PRIu64 " internal-consumed=%" PRIu64
        " publications=%" PRIu64 " acknowledged=%" PRIu64
        " rx-publications=%" PRIu64 " rx-acknowledged=%" PRIu64
        " commit-max-ns=%" PRIu64 " trace-dropped=%" PRIu64
        " trace-errors=%" PRIu64 " paused=%d failed=%d\n",
        s.epoch, s.rx_packets, s.rx_bytes, s.tx_packets, s.tx_bytes,
        s.rx_count, s.rx_bytes_queued, s.rx_high_count, s.rx_high_bytes,
        s.tx_count, s.tx_high_count, s.queue_oldest_ns, s.max_read_gap_ns,
        s.read_eagain, s.write_eagain, m->commits, m->admitted,
        m->internal_consumed, m->publications, m->acknowledged,
        m->rx_publications, m->rx_acknowledged, m->max_commit_ns, s.trace_dropped,
        s.trace_errors, !miband_bt_serial_running(), s.failed);
    qemu_log_mask(LOG_UNIMP,
        "miband BT latency epoch=%" PRIu64 " histogram=log2-upper-bound-ns"
        " admit-p50=%" PRIu64 " admit-p95=%" PRIu64 " admit-p99=%" PRIu64
        " admit-max=%" PRIu64
        " read-to-publication-p50=%" PRIu64 " read-to-publication-p95=%" PRIu64
        " read-to-publication-p99=%" PRIu64 " read-to-publication-max=%" PRIu64
        " publication-to-ack-p50=%" PRIu64 " publication-to-ack-p95=%" PRIu64
        " publication-to-ack-p99=%" PRIu64 " publication-to-ack-max=%" PRIu64
        " read-to-ack-p50=%" PRIu64 " read-to-ack-p95=%" PRIu64
        " read-to-ack-p99=%" PRIu64 " read-to-ack-max=%" PRIu64
        " commit-p50=%" PRIu64 " commit-p95=%" PRIu64 " commit-p99=%" PRIu64
        " commit-max=%" PRIu64 "\n",
        m->epoch,
        miband_bt_percentile(m->admit_hist, 50),
        miband_bt_percentile(m->admit_hist, 95),
        miband_bt_percentile(m->admit_hist, 99),
        m->max_admit_ns,
        miband_bt_percentile(m->publication_hist, 50),
        miband_bt_percentile(m->publication_hist, 95),
        miband_bt_percentile(m->publication_hist, 99),
        m->max_publication_ns,
        miband_bt_percentile(m->publication_ack_hist, 50),
        miband_bt_percentile(m->publication_ack_hist, 95),
        miband_bt_percentile(m->publication_ack_hist, 99),
        m->max_publication_ack_ns,
        miband_bt_percentile(m->read_ack_hist, 50),
        miband_bt_percentile(m->read_ack_hist, 95),
        miband_bt_percentile(m->read_ack_hist, 99),
        m->max_read_ack_ns,
        miband_bt_percentile(m->commit_hist, 50),
        miband_bt_percentile(m->commit_hist, 95),
        miband_bt_percentile(m->commit_hist, 99),
        m->max_commit_ns);
    timer_mod(m->stats_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 1000000000);
}

void miband_bt_start_serial_worker(MiBandBt *bt, CPUState *cpu,
                                   const char *trace_path)
{
    MiBandSerialMain *m;
    MiBandSerialStats stats;
    char error[200];
    if (!bt->serial_active || bt->serial_worker) {
        return;
    }
    assert(cpu || qtest_enabled());
    m = g_new0(MiBandSerialMain, 1);
    m->bt = bt;
    m->cpu = cpu;
    m->refs = 1;
    m->bh = qemu_bh_new(miband_bt_serial_bh, m);
    m->stats_timer = timer_new_ns(QEMU_CLOCK_REALTIME, miband_bt_serial_metrics, m);
    /* Setup runs after properties/IRQs and before the main loop. Remove every
     * legacy serial handler before transferring ownership of this fd. */
    int fd = miband_hci_bridge_detach_fd(bt->bridge);
    m->io = miband_serial_start(fd, bt->serial_tx_rate, trace_path,
                               miband_bt_serial_notify, m, error, sizeof(error));
    if (!m->io) {
        timer_free(m->stats_timer);
        qemu_bh_delete(m->bh);
        g_free(m);
        error_setg(&error_fatal, "cannot start physical HCI worker: %s", error);
        return;
    }
    miband_hci_bridge_release_fd(bt->bridge); /* Worker owns the fd, including close. */
    bt->serial_worker = m->io;
    bt->serial_worker_cpu = cpu;
    miband_serial_stats(m->io, &stats);
    m->epoch = stats.epoch;
    m->vm_handler = qemu_add_vm_change_state_handler(miband_bt_serial_vm_state, m);
    timer_mod(m->stats_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 1000000000);
    qemu_log_mask(LOG_UNIMP,
        "miband BT worker started epoch=%" PRIu64
        " rx-limit=524288/1024 read=4096 batch=32/200us trace=%d\n",
        m->epoch, trace_path && trace_path[0]);
}

static void miband_bt_stop_serial_worker(MiBandBt *bt)
{
    MiBandSerialMain *m;
    if (!bt->serial_worker) {
        return;
    }
    m = miband_serial_opaque(bt->serial_worker);
    m->closing = true;
    bt->serial_worker = NULL;
    bt->serial_worker_cpu = NULL;
    miband_serial_stop(m->io); /* No worker waits for BQL or a vCPU job. */
    m->io = NULL;
    qemu_del_vm_change_state_handler(m->vm_handler);
    timer_free(m->stats_timer);
    qemu_bh_delete(m->bh);
    m->bt = NULL;
    miband_bt_serial_main_unref(m);
}

void miband_bt_shutdown(MiBandBt *bt)
{
    miband_bt_close_host(bt);
    if (!bt->tx_done_event) {
        return; /* Machine reset and process exit can both request shutdown. */
    }
    timer_free(bt->tx_done_event);
    timer_free(bt->rx_event[0]);
    timer_free(bt->rx_event[1]);
    timer_free(bt->rx_flow_timer);
    bt->tx_done_event = NULL;
    bt->rx_event[0] = bt->rx_event[1] = NULL;
    bt->rx_flow_timer = NULL;
    miband_hci_bridge_free(bt->bridge);
    bt->bridge = NULL;
    miband_hci_synthetic_free(bt->synthetic);
    bt->synthetic = NULL;
    miband_hci_gatt_free(bt->gatt);
    bt->gatt = NULL;
    miband_xiaomi_mass_receiver_free(bt->mass_receiver);
    bt->mass_receiver = NULL;
}

static bool miband_bt_guest_read(MiBandBt *bt, uint32_t address,
                                 void *data, unsigned size)
{
    /* IPC descriptors live in AP RAM; packet data can also live in PSRAM. */
    if (!((address >= 0x20000000 &&
           (uint64_t)address + size <= 0x20160000) ||
          (address >= 0x3c000000 &&
           (uint64_t)address + size <= 0x3e000000))) {
        return false;
    }
    return address_space_read(bt->as, address, MEMTXATTRS_UNSPECIFIED,
                              data, size) == MEMTX_OK;
}

static void miband_bt_log_packet(const char *direction, unsigned channel,
                                 const uint8_t *data, unsigned size)
{
    GString *bytes;
    unsigned i;

    /* Detailed dumps are an explicit diagnostic opt-in. The worker's binary
     * trace preserves physical bytes without formatting in the IO path. */
    if (!getenv("MIBAND_HCI_HEX_TRACE") || !qemu_loglevel_mask(LOG_UNIMP)) {
        return;
    }
    bytes = g_string_sized_new(size * 3);
    for (i = 0; i < size; i++) {
        g_string_append_printf(bytes, "%s%02x", i ? " " : "", data[i]);
    }
    qemu_log_mask(LOG_UNIMP, "miband BT %s channel=%u len=%u bytes=%s\n",
                  direction, channel, size, bytes->str);
    g_string_free(bytes, true);
}

static const char *miband_bt_command_name(uint16_t opcode)
{
    /* Names follow the controller command specification used by the AP. */
    switch (opcode) {
    case 0x0c01: return "set_event_mask";
    case 0x0c03: return "reset";
    case 0x0c05: return "set_event_filter";
    case 0x0c31: return "set_controller_to_host_flow_control";
    case 0x0c33: return "host_buffer_size";
    case 0x0c35: return "host_number_of_completed_packets";
    case 0x0c13: return "change_local_name";
    case 0x0c14: return "read_local_name";
    case 0x1001: return "read_local_version";
    case 0x1002: return "read_local_commands";
    case 0x1003: return "read_local_features";
    case 0x1004: return "read_local_ext_features";
    case 0x1005: return "read_buffer_size";
    case 0x1009: return "read_bd_addr";
    case 0x2001: return "le_set_event_mask";
    case 0x2002: return "le_read_buffer_size";
    case 0x2003: return "le_read_local_features";
    case 0xfc32: return "set_bd_addr";
    case 0x2005: return "le_set_random_address";
    case 0x2006: return "le_set_advertising_parameters";
    case 0x2007: return "le_read_advertising_channel_tx_power";
    case 0x2008: return "le_set_advertising_data";
    case 0x2009: return "le_set_scan_response_data";
    case 0x200a: return "le_set_advertising_enable";
    case 0x200b: return "le_set_scan_parameters";
    case 0x200c: return "le_set_scan_enable";
    case 0x200d: return "le_create_connection";
    case 0x200e: return "le_create_connection_cancel";
    case 0x200f: return "le_read_white_list_size";
    case 0x2010: return "le_clear_white_list";
    case 0x2011: return "le_add_device_to_white_list";
    case 0x2012: return "le_remove_device_from_white_list";
    case 0x2013: return "le_connection_update";
    case 0x2014: return "le_set_host_channel_classification";
    case 0x2015: return "le_read_channel_map";
    case 0x2016: return "le_read_remote_features";
    case 0x2017: return "le_encrypt";
    case 0x2018: return "le_rand";
    case 0x2019: return "le_start_encryption";
    case 0x201a: return "le_ltk_request_reply";
    case 0x201b: return "le_ltk_request_negative_reply";
    case 0x201c: return "le_read_supported_states";
    case 0x201d: return "le_receiver_test";
    case 0x201e: return "le_transmitter_test";
    case 0x201f: return "le_test_end";
    case 0x0406: return "disconnect";
    case 0xfc71: return "set_wakeup_time";
    case 0xfc72: return "set_bd_addr_alt";
    case 0xfc77: return "set_sleep_setting";
    case 0xfc81: return "set_local_feature";
    case 0xfc82: return "set_local_ex_feature";
    case 0xfc83: return "set_bt_rf_timing";
    case 0xfc84: return "set_ble_rf_timing";
    case 0xfc86: return "set_bt_setting";
    case 0xfc88: return "set_custom_param";
    case 0xfcb3: return "set_rssi_tx_power_threshold";
    case 0xfcb8: return "set_func_patch";
    case 0xfcc3: return "set_bt_schedule_setting";
    case 0xfcc4: return "set_bt_ibrt_setting";
    case 0xfcc5: return "set_bt_hw_feature_setting";
    case 0xfccd: return "set_bt_ble_txrx_gain";
    case 0xfcfe: return "set_poweroff_backup_rf_reg";
    case 0xfcff: return "set_poweron_reg";
    default: return "unknown";
    }
}

static void miband_bt_queue_command_complete(MiBandBt *bt, unsigned channel,
                                              uint16_t opcode, uint8_t status,
                                              const uint8_t *params,
                                              unsigned params_len)
{
    uint8_t event[MIBAND_BT_PACKET_MAX];
    unsigned size = 7 + params_len;

    if (params_len > MIBAND_BT_PACKET_MAX - 7) {
        params_len = MIBAND_BT_PACKET_MAX - 7;
        size = 7 + params_len;
    }
    event[0] = 0x04;
    event[1] = 0x0e;
    event[2] = 4 + params_len;
    event[3] = 1;
    event[4] = opcode;
    event[5] = opcode >> 8;
    event[6] = status;
    if (params_len) {
        memcpy(event + 7, params, params_len);
    }
    miband_bt_queue_packet(bt, channel, event, size);
}

static bool miband_bt_expected_length(uint16_t opcode, unsigned length)
{
    switch (opcode) {
    case 0xfc32:
        return length == 6;
    case 0xfc72:
        /* The alternate address command carries an address type byte. */
        return length == 7;
    case 0xfc71:
        return length == 16;
    case 0xfc77:
        return length == 8;
    case 0xfc81:
        return length == 8;
    case 0xfc82:
        /* The retail trace includes a one-byte page selector. */
        return length == 9;
    case 0xfcff:
        /* The controller sends 6 bytes plus a variable register page. */
        return length >= 6;
    default:
        return miband_hci_synthetic_expected_length(opcode, length);
    }
}

static bool miband_bt_is_vendor_command(uint16_t opcode)
{
    return (opcode & 0xfc00) == 0xfc00;
}

/* ESP32-C6 exposes the BLE controller HCI transport only.  Keep the small
 * common HCI subset that the AP uses for controller discovery on the UART,
 * but complete Classic-only commands locally.  This prevents a valid BLE
 * controller from being rejected merely because the BES dual-mode host sends
 * an optional Classic setup command during boot. */
static bool miband_bt_serial_forwards_command(uint16_t opcode)
{
    if ((opcode & 0xff00) == 0x2000) {
        return true;
    }
    switch (opcode) {
    case 0x0406: /* Disconnect (also required by BLE) */
    case 0x041d: /* Read Remote Version Information */
    case 0x0c01: /* Set Event Mask */
    case 0x0c03: /* Reset */
    case 0x0c31: /* Set Controller To Host Flow Control */
    case 0x0c33: /* Host Buffer Size */
    case 0x0c35: /* Host Number Of Completed Packets */
    case 0x1001: /* Read Local Version Information */
    case 0x1002: /* Read Local Supported Commands */
    case 0x1003: /* Read Local Supported Features */
    case 0x1004: /* Read Local Extended Features */
    case 0x1005: /* Read Buffer Size */
    case 0x1009: /* Read BD_ADDR */
    case 0x1405: /* Read RSSI */
        return true;
    default:
        return false;
    }
}

static bool miband_bt_forward_host(MiBandBt *bt, unsigned channel,
                                   uint16_t opcode, const uint8_t *data,
                                   unsigned size)
{
    if (!bt->host_active || miband_bt_is_vendor_command(opcode) ||
        (bt->serial_active && !bt->serial_dual_mode &&
         !miband_bt_serial_forwards_command(opcode))) {
        if (bt->serial_active && !bt->serial_dual_mode &&
            !miband_bt_is_vendor_command(opcode) &&
            !miband_bt_serial_forwards_command(opcode)) {
            qemu_log_mask(LOG_UNIMP,
                          "miband BT serial BLE-only local completion opcode=%04x\n",
                          opcode);
        }
        return false;
    }
    if (bt->rx_flow_requested) {
        if (opcode == 0x0c31 || opcode == 0x0c33 || opcode == 0x0c35)
            error_setg(&error_fatal, "guest also owns RX flow control; disable bt-rx-flow-control to avoid double credits");
        if (opcode == 0x0c03) {
            bt->reset_channel = channel;
            bt->rx_flow_active = false;
        }
    }
    if (!miband_bt_host_queue(bt, data, size)) {
        error_setg(&error_fatal, "miband BT physical TX queue failed "
                   "opcode=%04x len=%u; refusing synthetic fallback", opcode, size);
        return true;
    }
    bt->host_forwarded++;
    bt->host_pending[channel] = true;
    bt->host_pending_opcode[channel] = opcode;
    miband_bt_trace(bt,
                  "miband BT host forward channel=%u opcode=%04x len=%u\n",
                  channel, opcode, size);
    return true;
}

static void miband_bt_command(MiBandBt *bt, unsigned channel,
                              const uint8_t *data, unsigned size)
{
    uint8_t rewritten[MIBAND_BT_PACKET_MAX];
    uint16_t opcode;
    uint8_t status = 0;
    uint8_t params[256];
    unsigned params_len = 0;
    const char *action = "unsupported";

    if (size && data[0] == 0x02 &&
        size <= sizeof(bt->synthetic_last_ap_packet)) {
        memcpy(bt->synthetic_last_ap_packet, data, size);
        bt->synthetic_last_ap_packet_len = size;
        bt->synthetic_last_ap_channel = channel;
    }
    if (size && bt->synthetic_peer_passive &&
        size <= sizeof(bt->synthetic_ap_trace[0]) &&
        bt->synthetic_ap_trace_count < MIBAND_BT_TRACE_QUEUE_LENGTH) {
        unsigned slot = (bt->synthetic_ap_trace_head +
                          bt->synthetic_ap_trace_count) %
                         MIBAND_BT_TRACE_QUEUE_LENGTH;
        memcpy(bt->synthetic_ap_trace[slot], data, size);
        bt->synthetic_ap_trace_len[slot] = size;
        bt->synthetic_ap_trace_channel[slot] = channel;
        bt->synthetic_ap_trace_count++;
    }

    if (bt->smp_just_works && size <= sizeof(rewritten) && size &&
        data[0] == 0x02) {
        memcpy(rewritten, data, size);
        (void)miband_bt_rewrite_smp(bt, rewritten, size, "AP->BT");
        data = rewritten;
    }
    miband_bt_note_smp(bt, data, size, "AP->BT");
    miband_bt_log_packet("AP->BT", channel, data, size);
    if (!size || (data[0] == 0x01 && size < 4)) {
        qemu_log_mask(LOG_GUEST_ERROR, "miband BT truncated AP H4 packet len=%u\n", size);
        return;
    }
    if (bt->host_requested && !bt->host_active) {
        error_setg(&error_fatal, "miband BT requested physical backend is unavailable");
        return;
    }
    if (size < 1 || data[0] != 0x01) {
        if (bt->host_active && miband_bt_host_queue(bt, data, size)) {
            bt->host_forwarded++;
            miband_bt_trace(bt,
                          "miband BT host forward data channel=%u type=%02x len=%u\n",
                          channel, data[0], size);
        } else if (bt->host_requested) {
            error_setg(&error_fatal, "miband BT physical data TX failed; "
                       "refusing synthetic ACL completion");
        } else if (size && data[0] == 0x02) {
            uint8_t response[MIBAND_BT_PACKET_MAX];
            unsigned length = 0;
            bool handled = miband_hci_gatt_acl(bt->gatt, data, size,
                                               response, sizeof(response),
                                               &length);
            if (handled && length) {
                miband_bt_queue_packet(bt, channel, response, length);
            }
            if (handled) {
                length = 0;
                if (miband_hci_gatt_take_notification(bt->gatt, response,
                                                      sizeof(response), &length) && length) {
                    miband_bt_queue_packet(bt, channel, response, length);
                }
            } else {
                uint8_t event[8];
                length = miband_hci_synthetic_acl(data, size, event);
                if (length) miband_bt_queue_packet(bt, channel, event, length);
            }
        }
        miband_bt_trace(bt, "miband BT data packet channel=%u type=%02x len=%u\n",
                      channel, size ? data[0] : 0, size);
        return;
    }
    opcode = data[1] | (data[2] << 8);
    bt->command_count++;
    if (data[3] != size - 4) {
        status = 0x12; /* Invalid HCI Command Parameters. */
        action = "invalid-length";
    } else if (!miband_bt_expected_length(opcode, size - 4)) {
        status = 0x12;
        action = "invalid-parameters";
    } else if (opcode == 0x2018 && bt->le_passkey &&
               bt->le_smp_active && bt->le_passkey_rand_pending) {
        uint32_t passkey = bt->le_passkey;

        /* LE Rand returns eight bytes. The AP's passkey path reduces the
         * first little-endian 32-bit value to six decimal digits. */
        params[0] = passkey;
        params[1] = passkey >> 8;
        params[2] = passkey >> 16;
        params[3] = passkey >> 24;
        params[4] = 0x5a;
        params[5] = 0xc3;
        params[6] = 0x17;
        params[7] = 0xa9;
        params_len = 8;
        bt->le_passkey_rand_pending = false;
        action = "synthetic-le-passkey-rand";
        qemu_log_mask(LOG_UNIMP,
                      "miband BT LE fixed passkey seed returned value=%06u\n",
                      passkey);
    } else if (miband_bt_forward_host(bt, channel, opcode, data, size)) {
        action = "host-forward";
        return;
    } else {
        action = miband_bt_is_vendor_command(opcode) ? "synthetic-vendor" :
                 "synthetic-standard";
        if (size > 4) {
            params_len = MIN(size - 4, sizeof(params));
            memcpy(params, data + 4, params_len);
        }
        switch (opcode) {
        case 0xfc32:
            memcpy(bt->bd_addr, data + 4, sizeof(bt->bd_addr));
            bt->bd_addr_type = 0;
            qemu_log_mask(LOG_UNIMP,
                          "miband BT public address=%02x:%02x:%02x:%02x:%02x:%02x\n",
                          bt->bd_addr[5], bt->bd_addr[4], bt->bd_addr[3],
                          bt->bd_addr[2], bt->bd_addr[1], bt->bd_addr[0]);
            break;
        case 0xfc72:
            bt->bd_addr_type = data[4];
            memcpy(bt->bd_addr, data + 5, sizeof(bt->bd_addr));
            qemu_log_mask(LOG_UNIMP,
                          "miband BT LE address type=%u value=%02x:%02x:%02x:%02x:%02x:%02x\n",
                          bt->bd_addr_type, bt->bd_addr[5], bt->bd_addr[4],
                          bt->bd_addr[3], bt->bd_addr[2], bt->bd_addr[1],
                          bt->bd_addr[0]);
            /* BES FC72 carries the LE address type followed by the six
             * address octets.  ESP32 exposes the standard LE random-address
             * command, so translate only the random case and consume its
             * completion internally. */
            if (bt->bd_addr_type == 1) {
                (void)miband_bt_queue_internal_command(bt, 0x2005,
                                                        bt->bd_addr,
                                                        sizeof(bt->bd_addr));
            }
            break;
        case 0xfc71:
            memcpy(bt->wakeup_params, data + 4,
                   sizeof(bt->wakeup_params));
            break;
        case 0xfc77:
            memcpy(bt->sleep_params, data + 4,
                   sizeof(bt->sleep_params));
            bt->controller_sleeping = data[4] != 0;
            /* bes_serial_hci2's RX callback (AP 0x0c19eff6) accepts this
             * completion only with parameter length 4: credits, opcode,
             * status. Echoing the request prevents its sleep-enable
             * semaphore from being posted after HCI Reset. */
            params_len = 0;
            break;
        case 0xfc81:
            memcpy(bt->local_features, data + 4,
                   sizeof(bt->local_features));
            break;
        case 0xfc82:
            memcpy(bt->local_ext_features, data + 4,
                   sizeof(bt->local_ext_features));
            break;
        case 0xfcff:
            bt->controller_powered = true;
            bt->controller_ready = true;
            break;
        default:
            if (!miband_bt_is_vendor_command(opcode)) {
                MiBandHciSyntheticResult result;
                MiBandHciSyntheticIdentity identity = {
                    bt->bd_addr, bt->local_features, bt->local_ext_features
                };
                if (!miband_hci_synthetic_command(bt->synthetic, &identity,
                                                  data, size, &result)) {
                    action = "unknown-standard";
                }
                status = result.status;
                params_len = result.params_len;
                memcpy(params, result.params, params_len);
                if (!status && opcode == 0x0c03) {
                    bt->controller_powered = true;
                    bt->controller_ready = true;
                    bt->controller_sleeping = false;
                } else if (!status && opcode == 0x2005) {
                    memcpy(bt->bd_addr, data + 4, sizeof(bt->bd_addr));
                    bt->bd_addr_type = 1;
                }
            }
            break;
        }
    }
    qemu_log_mask(LOG_UNIMP,
                  "miband BT command channel=%u opcode=%04x name=%s params=%u action=%s status=%02x\n",
                  channel, opcode, miband_bt_command_name(opcode),
                  size - 4, action, status);
    miband_bt_queue_command_complete(bt, channel, opcode, status, params,
                                     params_len);
    if (!status && !bt->host_active && !bt->synthetic_peer_passive &&
            opcode == 0x200d) {
        uint8_t event[32];
        const uint16_t handle = 1;
        unsigned event_len = miband_hci_synthetic_connection_complete(
            data, size, handle, event);
        if (event_len && miband_bt_queue_packet(bt, channel, event, event_len)) {
            if (++bt->connection_sequence == 0) {
                ++bt->connection_sequence;
            }
            bt->connection_generation[handle] = bt->connection_sequence;
            bt->connection_live[handle] = true;
            miband_hci_gatt_connect(bt->gatt, handle);
            qemu_log_mask(LOG_UNIMP,
                          "miband BT synthetic LE peer connected handle=%u\n",
                          handle);
        }
    } else if (!status && !bt->host_active && opcode == 0x0406 && size >= 7) {
        const uint16_t handle = lduw_le_p(data + 4) & 0xfff;
        uint8_t event[] = { 0x04, 0x05, 0x04, 0x00,
                            handle, handle >> 8, 0x13 };
        bt->connection_live[handle] = false;
        miband_hci_gatt_disconnect(bt->gatt, handle);
        (void)miband_bt_queue_packet(bt, channel, event, sizeof(event));
    }
}

static void miband_bt_receive_ap_queue(MiBandBt *bt, unsigned channel)
{
    uint32_t head_address = ldl_le_p(bt->ram + BT_AP_TX_HEAD(channel));
    uint8_t word[4], descriptor[20], packet[MIBAND_BT_PACKET_MAX];
    uint32_t node, address, length;
    unsigned count;

    if (!head_address) {
        return; /* This IPC channel has not been opened by the AP. */
    }
    if (!miband_bt_guest_read(bt, head_address, word, sizeof(word))) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT invalid AP queue channel=%u head=%08x\n",
                      channel, head_address);
        return;
    }
    node = ldl_le_p(word);
    for (count = 0; node && count < 16; count++) {
        if (!miband_bt_guest_read(bt, node, descriptor,
                                  sizeof(descriptor))) {
            break;
        }
        length = ldl_le_p(descriptor + 8);
        address = ldl_le_p(descriptor + 12);
        if (length == 0 || length > sizeof(packet) ||
            !miband_bt_guest_read(bt, address, packet, length)) {
            qemu_log_mask(LOG_UNIMP,
                          "miband BT invalid AP payload channel=%u node=%08x addr=%08x len=%u\n",
                          channel, node, address, length);
            break;
        }
        miband_bt_command(bt, channel, packet, length);
        node = ldl_le_p(descriptor);
    }
    if (node) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT AP queue truncated channel=%u node=%08x\n",
                      channel, node);
    }
}

static void miband_bt_send_reply(MiBandBt *bt, unsigned channel)
{
    uint32_t node = BT_RX_NODE(channel);
    uint32_t payload = BT_RX_PAYLOAD(channel);
    uint32_t receiver_head;

    if (bt->serial_worker) {
        if (!miband_bt_serial_running()) {
            return;
        }
        if (!qtest_enabled() && current_cpu != bt->serial_worker_cpu) {
            miband_bt_serial_kick(bt);
            return;
        }
    }

    miband_bt_poll_host(bt, channel);
    if (!bt->reply_len[channel] && miband_bt_promote_packet(bt, channel)) {
        /* A controller reply is a separate asynchronous event, not part of
         * the AP's MMIO write/read or TX completion interrupt. In particular,
         * Reset -> FC77 can otherwise recursively clear the driver's TX
         * semaphore before the sending thread returns from intersys_send. */
        bt->reply_ready_ns[channel] =
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BT_REPLY_DELAY_NS;
    }
    if (!bt->reply_len[channel]) {
        return;
    }
    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < bt->reply_ready_ns[channel]) {
        timer_mod(bt->rx_event[channel], bt->reply_ready_ns[channel]);
        return;
    }
    receiver_head = ldl_le_p(bt->ram + BT_PEER_RX_HEAD(channel));
    if (receiver_head) {
        miband_bt_trace(bt,
                      "miband BT response deferred channel=%u receiver-busy head=%08x status=%08x mask=%u ind=%u\n",
                      channel, receiver_head, bt->mailbox_status,
                      !!(bt->mailbox_status & BT_RX_MASK(channel)),
                      !!(bt->mailbox_status & BT_RX_IND(channel)));
        return;
    }
    stl_le_p(bt->ram + BT_PEER_RX_HEAD(channel),
             BT_RAM_BASE + node);
    stl_le_p(bt->ram + node, 0);
    stl_le_p(bt->ram + node + 4, 0);
    stl_le_p(bt->ram + node + 8, bt->reply_len[channel]);
    stl_le_p(bt->ram + node + 12, BT_RAM_BASE + payload);
    stl_le_p(bt->ram + node + 16, 1);
    memcpy(bt->ram + payload, bt->reply[channel], bt->reply_len[channel]);
    if (!bt->host_active && bt->reply_len[channel] <=
            sizeof(bt->synthetic_last_bt_packet)) {
        memcpy(bt->synthetic_last_bt_packet, bt->reply[channel],
               bt->reply_len[channel]);
        bt->synthetic_last_bt_packet_len = bt->reply_len[channel];
    }
    miband_bt_serial_delivered(bt, channel);
    bt->delivered_generation[channel] = bt->reply_generation[channel];
    if (bt->reply_generation[channel])
        bt->delivered_handle[channel] = lduw_le_p(bt->reply[channel] + 1) & 0xfff;
    miband_bt_log_packet("BT->AP", channel, bt->reply[channel],
                         bt->reply_len[channel]);
    bt->reply_len[channel] = 0;
    bt->response_count++;
    bt->mailbox_status |= BT_RX_IND(channel);
    /* best1503 has a second, BT-side ISIRQ bank at D0330050.  The main
     * CMU mailbox state above is what the AP's generic intersys path sees,
     * while this bank carries the controller-facing indication bits.  Keep
     * both banks live so channel 1 wakeup traffic can complete through the
     * same interrupt/clear sequence as the real controller. */
    bt->regs[1][(BT_CMU_ISIRQ_SET - BT_CMU_BASE) / 4] |=
        BT_RX_IND(channel);
    if (bt->mailbox_status & BT_RX_MASK(channel)) {
        /* Latched mailbox status drives a level until mask/ack clears it.
         * A pulse is lost if the AP clears NVIC pending during reopen. */
        bt->mailbox_status |= BT_RX_INTR_MASKED(channel);
        qemu_set_irq(bt->rx_data[channel], 1);
    } else {
        miband_bt_trace(bt,
                      "miband BT RX indication latched channel=%u status=%08x mask=0\n",
                      channel, bt->mailbox_status);
    }
}

static void miband_bt_rx0(void *opaque)
{
    miband_bt_send_reply(opaque, 0);
}

static void miband_bt_rx1(void *opaque)
{
    miband_bt_send_reply(opaque, 1);
}

/*
 * hal_intersys_send() completes asynchronously through the transport's TX
 * callback.  The AP therefore must not infer completion from how many times
 * it happened to read ISIRQ_SET.  The synthetic controller has consumed the
 * AP queue synchronously, so model the remote callback on the next virtual
 * clock turn and keep the IRQ/mask state independent from RX indications.
 */
static void miband_bt_tx_done(void *opaque)
{
    MiBandBt *bt = opaque;
    unsigned channel;

    for (channel = 0; channel < 2; channel++) {
        if (!bt->tx_done_pending[channel]) {
            continue;
        }
        bt->tx_done_pending[channel] = false;
        bt->mailbox_pending[channel] = false;
        bt->mailbox_status &= ~BT_DATA_IND(channel);
        bt->mailbox_status |= BT_TX_DONE(channel);
        /* On best1503, MCU->BT completion is reported in BT CMU ISIRQ_SET
         * (D0330050), independently of the AP-side CMU mailbox bits. */
        bt->regs[1][(BT_CMU_ISIRQ_SET - BT_CMU_BASE) / 4] |=
            BT_TX_DONE(channel);
        miband_bt_trace(bt,
                      "miband BT mailbox tx callback channel=%u status=%08x bt-cmu-isirq=%08x\n",
                      channel, bt->mailbox_status,
                      bt->regs[1][(BT_CMU_ISIRQ_SET - BT_CMU_BASE) / 4]);
        if ((bt->mailbox_status & BT_TX_MASK(channel)) &&
            !(bt->mailbox_status & BT_TX_INTR_MASKED(channel))) {
            bt->mailbox_status |= BT_TX_INTR_MASKED(channel);
            qemu_set_irq(bt->tx_done[channel], 1);
        }
        miband_bt_send_reply(bt, channel);
    }
}

void miband_bt_init(MiBandBt *bt, AddressSpace *as, uint8_t *ram)
{
    memset(bt, 0, sizeof(*bt));
    bt->as = as;
    bt->ram = ram;
    bt->bridge = miband_hci_bridge_new((MiBandHciBridgeCallbacks) {
        bt, miband_bt_bridge_read_ready, miband_bt_bridge_failed
    });
    bt->tx_done_event = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     miband_bt_tx_done, bt);
    bt->rx_event[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, miband_bt_rx0, bt);
    bt->rx_event[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, miband_bt_rx1, bt);
    bt->rx_flow_timer = timer_new_ns(QEMU_CLOCK_REALTIME, miband_bt_flow_timeout, bt);
    bt->controller_powered = true;
    bt->mass_slice_length = 12282;
    bt->synthetic = miband_hci_synthetic_new("Synthetic peripheral");
    bt->gatt = miband_hci_gatt_new(247);
    miband_hci_gatt_set_write_callback(bt->gatt, bt, miband_bt_gatt_write);
    bt->mass_receiver = miband_xiaomi_mass_receiver_new();
    /* Values follow the feature bytes used by the BES driver.  They are
     * synthetic controller defaults and remain replaceable by FC81/FC82. */
    memcpy(bt->local_features,
           (const uint8_t[]){0xbf, 0xee, 0x4d, 0xfe,
                             0xdb, 0xff, 0x7b, 0x87},
           sizeof(bt->local_features));
}

static void miband_bt_gatt_write(MiBandHciGatt *gatt, void *opaque,
                                 uint16_t attribute, const uint8_t *value,
                                 size_t length, bool command)
{
    MiBandBt *bt = opaque;
    MiBandXiaomiL1 l1;
    MiBandXiaomiL2 l2;
    uint8_t ack[32];
    uint8_t pb_response[256], pb_l2[300], pb_l1[320];
    size_t ack_len;
    size_t pb_response_len, pb_l2_len, pb_l1_len;

    (void)command;

    /* In passive peer mode the synthetic GATT server only acknowledges ATT
     * writes.  A host-side test or bridge injects the peer's notifications
     * through miband_bt_inject_hci(), so the guest firmware remains the sole
     * Xiaomi protocol and installation consumer. */
    if (bt->synthetic_peer_passive) {
        if (attribute == 6 && length <= sizeof(bt->synthetic_last_write)) {
            memcpy(bt->synthetic_last_write, value, length);
            bt->synthetic_last_write_len = length;
        }
        return;
    }

    if (attribute != 6 || !miband_xiaomi_l1_decode(value, length, &l1)) {
        return;
    }
    if (l1.type == MIBAND_XIAOMI_L1_DATA &&
        miband_xiaomi_l2_decode(l1.payload, l1.payload_len, &l2)) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT Xiaomi L2 channel=%u opcode=%u len=%zu\n",
                      l2.channel, l2.opcode, l2.payload_len);
        if (l2.channel == 2 && l2.opcode == 1 &&
            miband_xiaomi_mass_receiver_push(bt->mass_receiver,
                                             l2.payload, l2.payload_len) &&
            miband_xiaomi_mass_receiver_complete(bt->mass_receiver)) {
            const MiBandXiaomiMass *mass =
                miband_xiaomi_mass_receiver_result(bt->mass_receiver);
            size_t mass_length = 0;
            const uint8_t *mass_data =
                miband_xiaomi_mass_receiver_data(bt->mass_receiver, &mass_length);
            qemu_log_mask(LOG_UNIMP,
                          "miband BT Xiaomi mass complete type=%u bytes=%zu\n",
                          mass->type, mass->payload_len);
            if (!miband_bt_mass_type_supported(bt, mass->type)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "miband BT Xiaomi unsupported mass type=%u\n",
                              mass->type);
            } else if (bt->mass_commit && !bt->mass_commit(bt->mass_commit_opaque,
                                                           mass->type, mass_data,
                                                           mass_length)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "miband BT Xiaomi mass persistence failed type=%u\n",
                              mass->type);
            } else if ((bt->pending_install.kind == MIBAND_XIAOMI_PB_WATCHFACE_PREPARE &&
                        mass->type == MIBAND_XIAOMI_MASS_WATCHFACE) ||
                       (bt->pending_install.kind == MIBAND_XIAOMI_PB_APP_PREPARE &&
                        mass->type == MIBAND_XIAOMI_MASS_THIRD_PARTY_APP)) {
                miband_bt_queue_install_result(bt, gatt, &bt->pending_install,
                                               l1.sequence);
                memset(&bt->pending_install, 0, sizeof(bt->pending_install));
            }
            /* A completed MASS transfer consumes the install transaction even
             * when its type or persistence result was rejected. */
            memset(&bt->pending_install, 0, sizeof(bt->pending_install));
        }
        if (l2.channel == 1 && l2.opcode == 1) {
            MiBandXiaomiPbRequest request;
            if (miband_xiaomi_pb_decode_prepare(l2.payload, l2.payload_len,
                                                &request)) {
                if (request.kind == MIBAND_XIAOMI_PB_MASS_PREPARE &&
                    !miband_bt_mass_type_supported(bt, (uint8_t)request.data_type)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "miband BT Xiaomi unsupported MASS prepare type=%u\n",
                                  request.data_type);
                } else {
                    pb_response_len = miband_xiaomi_pb_encode_prepare_response(
                        &request, bt->mass_slice_length, 0, pb_response,
                        sizeof(pb_response));
                    pb_l2_len = miband_xiaomi_l2_encode(
                        1, 1, pb_response, pb_response_len, pb_l2,
                        sizeof(pb_l2));
                    pb_l1_len = miband_xiaomi_l1_encode(
                        MIBAND_XIAOMI_L1_DATA, false, l1.sequence, pb_l2,
                        pb_l2_len, pb_l1, sizeof(pb_l1));
                    if (pb_response_len && pb_l2_len && pb_l1_len &&
                        !miband_hci_gatt_queue_notification(gatt, pb_l1,
                                                            pb_l1_len)) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "miband BT Xiaomi PB response queue full\n");
                    }
                    if (request.kind == MIBAND_XIAOMI_PB_MASS_PREPARE) {
                        miband_xiaomi_mass_receiver_reset(bt->mass_receiver);
                    } else {
                        bt->pending_install = request;
                    }
                }
            }
        }
    }
    if (l1.type != MIBAND_XIAOMI_L1_CMD && l1.type != MIBAND_XIAOMI_L1_DATA) {
        return;
    }
    if (l1.type == MIBAND_XIAOMI_L1_CMD) {
        miband_xiaomi_mass_receiver_reset(bt->mass_receiver);
    }
    ack_len = miband_xiaomi_l1_encode(MIBAND_XIAOMI_L1_ACK, false,
                                      l1.sequence, NULL, 0, ack, sizeof(ack));
    if (ack_len) {
        (void)miband_hci_gatt_queue_notification(gatt, ack, ack_len);
    }
    (void)bt;
}

void miband_bt_set_synthetic_peer_passive(MiBandBt *bt, bool enabled)
{
    bt->synthetic_peer_passive = enabled;
}

unsigned miband_bt_copy_synthetic_last_write(const MiBandBt *bt,
                                             uint8_t *packet, unsigned capacity)
{
    unsigned length = MIN(bt->synthetic_last_write_len, capacity);
    if (length && packet) {
        memcpy(packet, bt->synthetic_last_write, length);
    }
    return length;
}

unsigned miband_bt_copy_synthetic_last_ap_packet(const MiBandBt *bt,
                                                 uint8_t *packet,
                                                 unsigned capacity)
{
    unsigned length = MIN(bt->synthetic_last_ap_packet_len, capacity);
    if (length && packet) {
        memcpy(packet, bt->synthetic_last_ap_packet, length);
    }
    return length;
}

unsigned miband_bt_copy_synthetic_last_bt_packet(const MiBandBt *bt,
                                                 uint8_t *packet,
                                                 unsigned capacity)
{
    unsigned length = MIN(bt->synthetic_last_bt_packet_len, capacity);
    if (length && packet) {
        memcpy(packet, bt->synthetic_last_bt_packet, length);
    }
    return length;
}

unsigned miband_bt_pop_synthetic_ap_packet(MiBandBt *bt, uint8_t *packet,
                                            unsigned capacity,
                                            unsigned *channel)
{
    unsigned slot;
    unsigned length;

    if (!bt->synthetic_ap_trace_count) {
        if (channel) {
            *channel = 0;
        }
        return 0;
    }
    slot = bt->synthetic_ap_trace_head;
    length = MIN(bt->synthetic_ap_trace_len[slot], capacity);
    if (length && packet) {
        memcpy(packet, bt->synthetic_ap_trace[slot], length);
    }
    if (channel) {
        *channel = bt->synthetic_ap_trace_channel[slot];
    }
    bt->synthetic_ap_trace_len[slot] = 0;
    bt->synthetic_ap_trace_head = (slot + 1) % MIBAND_BT_TRACE_QUEUE_LENGTH;
    bt->synthetic_ap_trace_count--;
    return length;
}

bool miband_bt_inject_hci(MiBandBt *bt, unsigned channel,
                          const uint8_t *packet, unsigned size)
{
    uint8_t rewritten[MIBAND_BT_PACKET_MAX];

    /* QMP injection is a transport boundary, not the passive-peer policy.
     * Keep it available when the firmware-facing GATT callback is enabled so
     * diagnostics and the WebUI can drive the real Xiaomi protocol without a
     * host HCI connection.  ``synthetic_peer_passive`` still controls whether
     * the callback records writes or delivers them to the firmware path. */
    if (channel >= 2 || !packet || !size || size > MIBAND_BT_PACKET_MAX ||
        bt->host_active) {
        return false;
    }
    /* QMP synthetic injection enters at the same controller-to-host boundary
     * as the serial/raw-HCI backends.  Apply the diagnostic SMP policy here
     * too; otherwise a synthetic peer bypasses the just-works rewrite that
     * physical HCI packets receive and the real firmware rejects pairing.
     */
    if (packet[0] == 0x02 && size <= sizeof(rewritten)) {
        memcpy(rewritten, packet, size);
        /* Keep this boundary self-contained as QMP injection is also used
         * by the Web-native transport.  The shared helper validates the
         * complete ACL/L2CAP shape; the explicit fallback handles peers that
         * omit the controller PB bits while preserving the same wire bytes.
         */
        bool rewritten_smp = miband_bt_rewrite_smp(bt, rewritten, size, "BT->AP");
        if (!rewritten_smp && bt->smp_just_works && size >= 16) {
            unsigned acl_len = rewritten[3] | (rewritten[4] << 8);
            unsigned l2_len = rewritten[5] | (rewritten[6] << 8);
            uint16_t cid = rewritten[7] | (rewritten[8] << 8);
            if (acl_len + 5 <= size && l2_len >= 7 && cid == 0x0006 &&
                9 + l2_len <= size &&
                (rewritten[9] == 0x01 || rewritten[9] == 0x02)) {
                rewritten[10] = 0x03;
                rewritten[11] = 0x00;
                rewritten[12] &= (uint8_t)~0x04;
                rewritten_smp = true;
                qemu_log_mask(LOG_UNIMP,
                              "miband BT synthetic SMP just-works rewrite code=%02x\n",
                              rewritten[9]);
            }
        }
        miband_bt_note_smp(bt, rewritten, size, "BT->AP");
        packet = rewritten;
    }
    if (!miband_bt_queue_packet(bt, channel, packet, size)) {
        return false;
    }
    if (packet[0] == 0x04 && size >= 22 && packet[1] == 0x3e &&
            packet[2] >= 0x13 && packet[3] == 0x01 && packet[4] == 0) {
        uint16_t handle = lduw_le_p(packet + 5) & 0x0fff;
        if (++bt->connection_sequence == 0) {
            ++bt->connection_sequence;
        }
        bt->connection_generation[handle] = bt->connection_sequence;
        bt->connection_live[handle] = true;
        miband_hci_gatt_connect(bt->gatt, handle);
        qemu_log_mask(LOG_UNIMP,
                      "miband BT passive synthetic peer connected handle=%u\n",
                      handle);
    } else if (packet[0] == 0x04 && size == 7 && packet[1] == 0x05 &&
               packet[3] == 0) {
        uint16_t handle = lduw_le_p(packet + 5) & 0x0fff;
        bt->connection_live[handle] = false;
        miband_hci_gatt_disconnect(bt->gatt, handle);
    }
    /* Deliver immediately when the AP has already returned its receive node;
     * otherwise the normal mailbox path will promote the queued packet. */
    miband_bt_send_reply(bt, channel);
    /* QMP/bridge injection can happen while the AP is still consuming the
     * previous controller event.  The normal physical transport gets a
     * later readiness callback, but a one-shot synthetic injection has no
     * such edge.  Retry on the virtual clock until the AP clears its node. */
    if (bt->rx_queue_count[channel] || bt->reply_len[channel]) {
        timer_mod(bt->rx_event[channel],
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
    return true;
}

void miband_bt_set_mass_commit(MiBandBt *bt,
                               bool (*commit)(void *, uint8_t, const uint8_t *, size_t),
                               void *opaque)
{
    bt->mass_commit = commit;
    bt->mass_commit_opaque = opaque;
}

void miband_bt_set_mass_types(MiBandBt *bt, const uint8_t *types, unsigned count)
{
    bt->mass_types = types;
    bt->mass_type_count = count;
}

void miband_bt_set_mass_slice_length(MiBandBt *bt, uint32_t length)
{
    bt->mass_slice_length = length;
}

void miband_bt_set_rx_flow_requested(MiBandBt *bt, bool enabled)
{
    bt->rx_flow_requested = enabled;
    miband_hci_bridge_set_strict_rx(bt->bridge, enabled);
}

void miband_bt_set_serial_tx_rate(MiBandBt *bt, uint32_t rate)
{
    bt->serial_tx_rate = rate;
    miband_hci_bridge_set_rate(bt->bridge, rate);
}

void miband_bt_set_gatt_mtu(MiBandBt *bt, uint16_t mtu)
{
    if (!bt->gatt) {
        return;
    }
    miband_hci_gatt_free(bt->gatt);
    bt->gatt = miband_hci_gatt_new(mtu);
    miband_hci_gatt_set_write_callback(bt->gatt, bt, miband_bt_gatt_write);
}

void miband_bt_set_gatt_characteristics(MiBandBt *bt, const char *tx_uuid,
                                        const char *rx_uuid)
{
    if (bt->gatt) {
        miband_hci_gatt_set_characteristics(bt->gatt, tx_uuid, rx_uuid);
    }
}

void miband_bt_set_backend(MiBandBt *bt, const char *backend)
{
    MiBandHciBackendConfig config;
    bool opened = false;

    miband_bt_close_host(bt);
    bt->host_requested = false;
    bt->serial_dual_mode = false;
    if (!miband_hci_backend_parse(backend, &config, &error_fatal)) {
        miband_hci_backend_clear(&config);
        return;
    }
    bt->host_requested = config.kind != MIBAND_HCI_SYNTHETIC;
    bt->serial_dual_mode = config.dual_mode;
    switch (config.kind) {
    case MIBAND_HCI_SYNTHETIC:
        qemu_log_mask(LOG_UNIMP, "miband BT backend=synthetic\n");
        miband_hci_backend_clear(&config);
        return;
    case MIBAND_HCI_SERIAL:
        opened = miband_hci_bridge_open_serial(bt->bridge, config.path,
                                               config.baud);
        bt->serial_active = opened;
        break;
    case MIBAND_HCI_LINUX_RAW:
        opened = miband_hci_bridge_open_host(bt->bridge, config.device);
        break;
    }
    bt->host_active = opened;
    miband_hci_backend_clear(&config);
    if (!opened) {
        error_setg(&error_fatal, "miband BT cannot open backend=%s; "
                   "refusing synthetic fallback", backend);
    }
}

void miband_bt_set_smp_just_works(MiBandBt *bt, bool enabled)
{
    bt->smp_just_works = enabled;
    qemu_log_mask(LOG_UNIMP, "miband BT SMP policy=%s\n",
                  enabled ? "just-works" : "passthrough");
}

void miband_bt_set_fixed_passkey(MiBandBt *bt, uint32_t passkey)
{
    bt->fixed_passkey = passkey;
    qemu_log_mask(LOG_UNIMP, "miband BT fixed passkey=%s\n",
                  passkey ? "enabled" : "disabled");
}

void miband_bt_set_le_passkey(MiBandBt *bt, uint32_t passkey)
{
    bt->le_passkey = passkey;
    bt->le_smp_active = false;
    bt->le_passkey_rand_pending = false;
    qemu_log_mask(LOG_UNIMP, "miband BT LE fixed passkey=%s\n",
                  passkey ? "enabled" : "disabled");
}

void miband_bt_connect_tx_irq(MiBandBt *bt, unsigned channel, qemu_irq irq)
{
    g_assert(channel < 2);
    bt->tx_done[channel] = irq;
}

void miband_bt_connect_rx_irq(MiBandBt *bt, unsigned channel, qemu_irq irq)
{
    g_assert(channel < 2);
    bt->rx_data[channel] = irq;
}

bool miband_bt_mailbox_read(MiBandBt *bt, hwaddr address, unsigned size,
                            uint64_t *result)
{
    unsigned channel;

    if (size != 4 || (address != BT_MAILBOX_SET &&
                      address != BT_MAILBOX_CLR)) {
        return false;
    }

    if (address == BT_MAILBOX_SET) {
        for (channel = 0; channel < 2; channel++) {
            miband_bt_poll_host(bt, channel);
            miband_bt_send_reply(bt, channel);
        }
    }
    *result = address == BT_MAILBOX_SET ? bt->mailbox_status : 0;
    return true;
}

bool miband_bt_mailbox_write(MiBandBt *bt, hwaddr address, unsigned size,
                             uint64_t value)
{
    unsigned channel;

    if (size != 4 || (address != BT_MAILBOX_SET &&
                      address != BT_MAILBOX_CLR)) {
        return false;
    }

    miband_bt_trace(bt,
                  "miband BT mailbox write address=%08" HWADDR_PRIx
                  " value=%08" PRIx64 " before=%08x\n",
                  address, value, bt->mailbox_status);
    if (address == BT_MAILBOX_SET &&
        (value & (BT_DATA_IND(0) | BT_DATA_IND(1)))) {
        vaddr pc = current_cpu ? current_cpu->cc->get_pc(current_cpu) : 0;
        miband_bt_trace(bt,
                      "miband BT mailbox data-ind source-pc=%08" VADDR_PRIx
                      " value=%08" PRIx64 "\n", pc, value);
    }

    if (address == BT_MAILBOX_SET) {
        bt->mailbox_status |= value;
        for (channel = 0; channel < 2; channel++) {
            if ((value & BT_TX_MASK(channel)) &&
                (bt->mailbox_status & BT_TX_DONE(channel)) &&
                !(bt->mailbox_status & BT_TX_INTR_MASKED(channel))) {
                bt->mailbox_status |= BT_TX_INTR_MASKED(channel);
                qemu_set_irq(bt->tx_done[channel], 1);
            }
            if ((value & BT_RX_MASK(channel)) &&
                (bt->mailbox_status & BT_RX_IND(channel)) &&
                !(bt->mailbox_status & BT_RX_INTR_MASKED(channel))) {
                bt->mailbox_status |= BT_RX_INTR_MASKED(channel);
                qemu_set_irq(bt->rx_data[channel], 1);
            }
            if (value & BT_DATA_IND(channel)) {
                bt->mailbox_pending[channel] = true;
                bt->tx_done_pending[channel] = true;
                miband_bt_receive_ap_queue(bt, channel);
                timer_mod(bt->tx_done_event,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
            }
        }
    } else {
        bt->mailbox_status &= ~value;
        for (channel = 0; channel < 2; channel++) {
            if (value & BT_DATA_IND(channel)) {
                bt->mailbox_pending[channel] = false;
                bt->tx_done_pending[channel] = false;
            }
            if (value & (BT_TX_DONE(channel) | BT_TX_MASK(channel))) {
                bt->mailbox_status &= ~BT_TX_INTR_MASKED(channel);
                qemu_set_irq(bt->tx_done[channel], 0);
            }
            if (value & (BT_RX_IND(channel) | BT_RX_MASK(channel))) {
                bt->mailbox_status &= ~BT_RX_INTR_MASKED(channel);
                qemu_set_irq(bt->rx_data[channel], 0);
            }
        }
    }
    miband_bt_trace(bt,
                  "miband BT mailbox write complete address=%08" HWADDR_PRIx
                  " status=%08x\n", address, bt->mailbox_status);
    return true;
}

static uint32_t *miband_bt_word(MiBandBt *bt, hwaddr address, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4) || (address & (size - 1))) {
        return NULL;
    }
    if (address >= BT_REG_BASE && address - BT_REG_BASE <= BT_REG_SIZE - size) {
        return &bt->regs[0][(address - BT_REG_BASE) / 4];
    }
    if (address >= BT_DIGITAL_BASE &&
        address - BT_DIGITAL_BASE <= BT_REG_SIZE - size) {
        return &bt->regs[3][(address - BT_DIGITAL_BASE) / 4];
    }
    if (address >= BT_CMU_BASE && address - BT_CMU_BASE <= BT_REG_SIZE - size) {
        return &bt->regs[1][(address - BT_CMU_BASE) / 4];
    }
    if (address >= BT_RF_BASE && address - BT_RF_BASE <= BT_REG_SIZE - size) {
        return &bt->regs[2][(address - BT_RF_BASE) / 4];
    }
    return NULL;
}

bool miband_bt_read(MiBandBt *bt, hwaddr address, unsigned size,
                    uint64_t *result)
{
    uint32_t *word = miband_bt_word(bt, address, size);
    unsigned shift;

    if (!word) {
        return false;
    }

    shift = (address & 3) * 8;
    *result = (*word >> shift) &
              ((1ULL << (size * 8)) - 1);
    miband_bt_trace(bt, "miband BT register read %08" HWADDR_PRIx
                  " size=%u -> %08" PRIx64 "\n", address, size, *result);
    return true;
}

bool miband_bt_write(MiBandBt *bt, hwaddr address, unsigned size,
                     uint64_t value)
{
    unsigned channel;
    unsigned shift;
    uint32_t mask;
    uint32_t *word = miband_bt_word(bt, address, size);
    bool write_clear = address == BT_CMU_RX_DONE && size == 4;

    if (!word) {
        return false;
    }

    if (write_clear) {
        for (channel = 0; channel < 2; channel++) {
            if (value & BT_DATA_IND(channel)) {
                miband_bt_serial_acknowledged(bt, channel);
                miband_bt_flow_ack(bt, channel);
                stl_le_p(bt->ram + BT_PEER_RX_HEAD(channel), 0);
                bt->mailbox_status &= ~(BT_RX_IND(channel) |
                                        BT_RX_INTR_MASKED(channel));
                bt->regs[1][(BT_CMU_ISIRQ_SET - BT_CMU_BASE) / 4] &=
                    ~BT_RX_IND(channel);
                qemu_set_irq(bt->rx_data[channel], 0);
                miband_bt_trace(bt,
                              "miband BT response consumed channel=%u\n",
                              channel);
                miband_bt_send_reply(bt, channel);
            }
            if (value & BT_TX_DONE(channel)) {
                bt->regs[1][(BT_CMU_ISIRQ_SET - BT_CMU_BASE) / 4] &=
                    ~BT_TX_DONE(channel);
                miband_bt_trace(bt,
                              "miband BT controller tx done cleared channel=%u bt-cmu-isirq=%08x\n",
                              channel,
                              bt->regs[1][(BT_CMU_ISIRQ_SET - BT_CMU_BASE) / 4]);
            }
        }
    }

    /* ISIRQ_CLR is a write-to-clear register in the BES CMU map.  It does
     * not retain the value written by software; reads of the register return
     * zero on the observed best1503 path. */
    if (write_clear) {
        *word = 0;
        miband_bt_trace(bt,
                      "miband BT CMU ISIRQ_CLR write value=%08" PRIx64
                      " readback=00000000\n", value);
        return true;
    }

    shift = (address & 3) * 8;
    mask = (uint32_t)(((1ULL << (size * 8)) - 1) << shift);
    *word = (*word & ~mask) | ((uint32_t)(value << shift) & mask);
    miband_bt_trace(bt, "miband BT register write %08" HWADDR_PRIx
                  " size=%u value=%08" PRIx64 "\n", address, size, value);
    return true;
}
