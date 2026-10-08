/* Minimal deterministic ATT server for synthetic BLE regression. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "miband_hci_gatt.h"

struct MiBandHciGatt {
    uint16_t server_mtu;
    uint16_t handle;
    bool connected;
    uint16_t tx_uuid;
    uint16_t rx_uuid;
    void *write_opaque;
    MiBandHciGattWriteCallback write_callback;
#define MIBAND_GATT_NOTIFICATION_QUEUE 4
    uint8_t pending_notification[MIBAND_GATT_NOTIFICATION_QUEUE][256];
    unsigned pending_notification_len[MIBAND_GATT_NOTIFICATION_QUEUE];
    unsigned pending_notification_head;
    unsigned pending_notification_count;
    uint8_t device_name[32];
    unsigned device_name_len;
};

static void put16(uint8_t *dst, uint16_t value)
{
    dst[0] = value;
    dst[1] = value >> 8;
}

static bool range_contains(uint16_t start, uint16_t end, uint16_t handle)
{
    return start <= handle && handle <= end;
}

static unsigned frame(uint16_t handle, const uint8_t *att, unsigned att_len,
                      uint8_t *packet, unsigned capacity)
{
    unsigned l2cap_len = att_len;
    unsigned acl_len = 4 + l2cap_len;
    unsigned total = 5 + acl_len;

    if (total > capacity || att_len > 251) {
        return 0;
    }
    packet[0] = 0x02;
    put16(packet + 1, (uint16_t)(handle | 0x2000));
    put16(packet + 3, (uint16_t)acl_len);
    put16(packet + 5, (uint16_t)l2cap_len);
    put16(packet + 7, 0x0004); /* ATT fixed channel */
    memcpy(packet + 9, att, att_len);
    return total;
}

static unsigned error_response(uint8_t request, uint16_t handle, uint8_t error,
                               uint16_t connection, uint8_t *response,
                               unsigned capacity)
{
    uint8_t att[] = { 0x01, request, handle, handle >> 8, error };
    return frame(connection, att, sizeof(att), response, capacity);
}

static unsigned read_value(const MiBandHciGatt *gatt, uint16_t handle,
                           uint8_t *value, unsigned capacity)
{
    static const uint8_t generic_access[] = { 0x00, 0x18 };
    static const uint8_t xiaomi_service[] = { 0x95, 0xfe };
    uint8_t tx_uuid[] = { gatt->tx_uuid, gatt->tx_uuid >> 8 };
    uint8_t rx_uuid[] = { gatt->rx_uuid, gatt->rx_uuid >> 8 };
    static const uint8_t cccd_uuid[] = { 0x02, 0x29 };
    const uint8_t *source = NULL;
    unsigned length = 0;

    switch (handle) {
    case 1:
        source = generic_access; length = sizeof(generic_access); break;
    case 2:
        if (capacity < 5) return 0;
        value[0] = 0x02; put16(value + 1, 3);
        value[3] = 0x00; value[4] = 0x2a;
        return 5;
    case 3:
        source = gatt->device_name; length = gatt->device_name_len; break;
    case 4:
        source = xiaomi_service; length = sizeof(xiaomi_service); break;
    case 5:
        if (capacity < 5) return 0;
        value[0] = 0x18; put16(value + 1, 6);
        value[3] = tx_uuid[0]; value[4] = tx_uuid[1];
        return 5;
    case 6:
        source = tx_uuid; length = sizeof(tx_uuid); break;
    case 7:
        if (capacity < 5) return 0;
        value[0] = 0x10; put16(value + 1, 8);
        value[3] = rx_uuid[0]; value[4] = rx_uuid[1];
        return 5;
    case 8:
        source = rx_uuid; length = sizeof(rx_uuid); break;
    case 9:
        source = cccd_uuid; length = sizeof(cccd_uuid); break;
    default:
        return 0;
    }
    if (length > capacity) return 0;
    memcpy(value, source, length);
    return length;
}

MiBandHciGatt *miband_hci_gatt_new(uint16_t server_mtu)
{
    MiBandHciGatt *gatt = g_new0(MiBandHciGatt, 1);
    gatt->server_mtu = MAX(server_mtu, 23);
    gatt->tx_uuid = 0x005f;
    gatt->rx_uuid = 0x005e;
    memcpy(gatt->device_name, "Synthetic peripheral", 17);
    gatt->device_name_len = 17;
    return gatt;
}

void miband_hci_gatt_set_characteristics(MiBandHciGatt *gatt,
                                          const char *tx_uuid,
                                          const char *rx_uuid)
{
    char *end;
    unsigned long tx = tx_uuid ? strtoul(tx_uuid, &end, 16) : 0;
    if (!tx_uuid || *end || tx > 0xffff) tx = 0x005f;
    end = NULL;
    unsigned long rx = rx_uuid ? strtoul(rx_uuid, &end, 16) : 0;
    if (!rx_uuid || *end || rx > 0xffff) rx = 0x005e;
    gatt->tx_uuid = tx;
    gatt->rx_uuid = rx;
}

void miband_hci_gatt_set_write_callback(MiBandHciGatt *gatt, void *opaque,
                                         MiBandHciGattWriteCallback callback)
{
    gatt->write_opaque = opaque;
    gatt->write_callback = callback;
}

bool miband_hci_gatt_queue_notification(MiBandHciGatt *gatt,
                                         const uint8_t *value, size_t length)
{
    unsigned slot;
    if (!gatt->connected || !value || length > sizeof(gatt->pending_notification[0]) - 3 ||
        gatt->pending_notification_count == MIBAND_GATT_NOTIFICATION_QUEUE) return false;
    slot = (gatt->pending_notification_head + gatt->pending_notification_count) %
           MIBAND_GATT_NOTIFICATION_QUEUE;
    gatt->pending_notification[slot][0] = 0x1b;
    gatt->pending_notification[slot][1] = 8;
    gatt->pending_notification[slot][2] = 0;
    memcpy(gatt->pending_notification[slot] + 3, value, length);
    gatt->pending_notification_len[slot] = length + 3;
    gatt->pending_notification_count++;
    return true;
}

bool miband_hci_gatt_take_notification(MiBandHciGatt *gatt, uint8_t *response,
                                        unsigned capacity, unsigned *length)
{
    unsigned slot = gatt->pending_notification_head;
    if (!gatt->pending_notification_count) {
        *length = 0;
        return false;
    }
    *length = frame(gatt->handle, gatt->pending_notification[slot],
                    gatt->pending_notification_len[slot], response, capacity);
    if (*length) {
        gatt->pending_notification_len[slot] = 0;
        gatt->pending_notification_head = (slot + 1) % MIBAND_GATT_NOTIFICATION_QUEUE;
        gatt->pending_notification_count--;
    }
    return *length != 0;
}

void miband_hci_gatt_free(MiBandHciGatt *gatt)
{
    g_free(gatt);
}

void miband_hci_gatt_connect(MiBandHciGatt *gatt, uint16_t handle)
{
    gatt->handle = handle & 0x0fff;
    gatt->connected = true;
}

void miband_hci_gatt_disconnect(MiBandHciGatt *gatt, uint16_t handle)
{
    if ((handle & 0x0fff) == gatt->handle) {
        gatt->connected = false;
    }
}

bool miband_hci_gatt_acl(MiBandHciGatt *gatt, const uint8_t *packet,
                         unsigned size, uint8_t *response, unsigned capacity,
                         unsigned *response_len)
{
    uint16_t connection, l2cap_len, start, end, type;
    const uint8_t *pdu;
    unsigned pdu_len, length;
    uint8_t att[256];

    *response_len = 0;
    if (!gatt->connected || size < 10 || packet[0] != 0x02) return false;
    connection = lduw_le_p(packet + 1) & 0x0fff;
    if (connection != gatt->handle || lduw_le_p(packet + 7) != 0x0004) {
        return false;
    }
    l2cap_len = lduw_le_p(packet + 5);
    if (!l2cap_len || l2cap_len + 9 > size) return false;
    pdu = packet + 9;
    pdu_len = l2cap_len;
    if (!pdu_len) return false;

    switch (pdu[0]) {
    case 0x02: /* Exchange MTU Request */
        if (pdu_len != 3) goto invalid;
        att[0] = 0x03;
        put16(att + 1, MIN(gatt->server_mtu, lduw_le_p(pdu + 1)));
        length = 3;
        break;
    case 0x04: /* Find Information Request */
        if (pdu_len != 5) goto invalid;
        start = lduw_le_p(pdu + 1); end = lduw_le_p(pdu + 3);
        att[0] = 0x05; att[1] = 0x01; length = 2;
        for (unsigned h = start; h <= end && h <= 9; h++) {
            uint8_t value[8]; unsigned value_len = read_value(gatt, h, value, sizeof(value));
            if (!value_len || length + 4 > sizeof(att)) continue;
            put16(att + length, h); put16(att + length + 2, h == 1 || h == 4 ? 0x2800 :
                                           h == 2 || h == 5 || h == 7 ? 0x2803 :
                                           h == 9 ? 0x2902 : h == 3 ? 0x2a00 :
                                           h == 6 || h == 8 ? (h == 6 ? gatt->tx_uuid : gatt->rx_uuid) : 0);
            length += 4;
        }
        if (length == 2) goto not_found;
        break;
    case 0x08: /* Read By Type Request, used for characteristic discovery */
        if (pdu_len != 7) goto invalid;
        start = lduw_le_p(pdu + 1); end = lduw_le_p(pdu + 3); type = lduw_le_p(pdu + 5);
        if (type != 0x2803) goto not_found;
        att[0] = 0x09; att[1] = 7; length = 2;
        for (unsigned h = start; h <= end && h <= 7; h++) {
            if (h != 2 && h != 5 && h != 7) continue;
            uint8_t value[8]; unsigned value_len = read_value(gatt, h, value, sizeof(value));
            put16(att + length, h); memcpy(att + length + 2, value, value_len); length += 2 + value_len;
        }
        if (length == 2) goto not_found;
        break;
    case 0x0a: /* Read Request */
        if (pdu_len != 3) goto invalid;
        length = read_value(gatt, lduw_le_p(pdu + 1), att + 1, sizeof(att) - 1);
        if (!length) goto not_found;
        att[0] = 0x0b; length++;
        break;
    case 0x10: /* Read By Group Type Request */
        if (pdu_len != 7) goto invalid;
        start = lduw_le_p(pdu + 1); end = lduw_le_p(pdu + 3); type = lduw_le_p(pdu + 5);
        if (type != 0x2800) goto not_found;
        att[0] = 0x11; att[1] = 6; length = 2;
        if (range_contains(start, end, 1)) {
            put16(att + length, 1); put16(att + length + 2, 3); put16(att + length + 4, 0x1800); length += 6;
        }
        if (range_contains(start, end, 4)) {
            put16(att + length, 4); put16(att + length + 2, 9); put16(att + length + 4, 0xfe95); length += 6;
        }
        if (length == 2) goto not_found;
        break;
    case 0x12: /* Write Request */
        if (pdu_len < 3) goto invalid;
        if (gatt->write_callback) {
            gatt->write_callback(gatt, gatt->write_opaque,
                                 lduw_le_p(pdu + 1), pdu + 3, pdu_len - 3,
                                 false);
        }
        att[0] = 0x13; length = 1;
        break;
    case 0x52: /* Write Command */
        if (pdu_len < 3) goto invalid;
        if (gatt->write_callback) {
            gatt->write_callback(gatt, gatt->write_opaque,
                                 lduw_le_p(pdu + 1), pdu + 3, pdu_len - 3,
                                 true);
        }
        return true;
    default:
        att[0] = 0x01; att[1] = pdu[0]; put16(att + 2, pdu_len >= 3 ? lduw_le_p(pdu + 1) : 0); att[4] = 0x06; length = 5;
        break;
    }
    *response_len = frame(connection, att, length, response, capacity);
    return true;

invalid:
    *response_len = error_response(pdu[0], pdu_len >= 3 ? lduw_le_p(pdu + 1) : 0,
                                   0x04, connection, response, capacity);
    return true;
not_found:
    *response_len = error_response(pdu[0], pdu_len >= 3 ? lduw_le_p(pdu + 1) : 0,
                                   0x0a, connection, response, capacity);
    return true;
}
