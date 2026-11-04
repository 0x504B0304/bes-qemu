/*
 * MiBand virtual network card.
 *
 * The card deliberately exposes a small, transport-neutral MMIO contract.
 * The current transport is QEMU's standard netdev (slirp user networking in
 * Studio).  A later USB-WiFi or host-pass-through transport can feed the same
 * Ethernet frames without changing the guest agent or the MMIO ABI.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "net/net.h"
#include "miband_net.h"

#define MIBAND_NET_GATEWAY_MAC0 0x52
#define MIBAND_NET_GATEWAY_MAC1 0x55
#define MIBAND_NET_GATEWAY_MAC2 0x0a
#define MIBAND_NET_GATEWAY_MAC3 0x00
#define MIBAND_NET_GATEWAY_MAC4 0x02
#define MIBAND_NET_GATEWAY_MAC5 0x02

#define MIBAND_NET_GUEST_IP 0x0a00020fu /* 10.0.2.15 */
#define MIBAND_NET_GATEWAY_IP 0x0a000202u /* 10.0.2.2 */

typedef struct MiBandNetRxPacket {
    uint16_t len;
    uint8_t data[MIBAND_NET_FIFO_SIZE];
} MiBandNetRxPacket;

typedef struct MiBandNetState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion *memory;
    NICConf conf;
    NICState *nic;
    MemReentrancyGuard reentrancy_guard;
    uint32_t mmio_base;
    char *backend;

    uint8_t tx[MIBAND_NET_FIFO_SIZE];
    uint32_t tx_len;
    MiBandNetRxPacket rx[MIBAND_NET_RX_QUEUE];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;

    uint32_t ready;
    uint32_t guest_state;
    uint32_t guest_error;
    uint32_t last_error;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_drops;
    uint64_t tx_errors;
} MiBandNetState;

OBJECT_DECLARE_SIMPLE_TYPE(MiBandNetState, MIBAND_NET)

static void miband_net_set_error(MiBandNetState *s, uint32_t code);

static void miband_net_reset_state(MiBandNetState *s)
{
    s->tx_len = 0;
    s->rx_head = 0;
    s->rx_tail = 0;
    s->rx_count = 0;
    s->ready = 0;
    s->guest_state = 0;
    s->guest_error = 0;
    miband_net_set_error(s, MIBAND_NET_ERR_NONE);
    s->rx_packets = 0;
    s->tx_packets = 0;
    s->rx_drops = 0;
    s->tx_errors = 0;
    memset(s->tx, 0, sizeof(s->tx));
    memset(s->rx, 0, sizeof(s->rx));
}

static void miband_net_trace(const char *format, ...)
{
    const char *path = getenv("MIBAND_NET_TRACE");
    FILE *stream;
    va_list arguments;

    if (path == NULL || *path == '\0') {
        return;
    }
    stream = fopen(path, "a");
    if (stream == NULL) {
        return;
    }
    va_start(arguments, format);
    fprintf(stream, "%" PRId64 " ", qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    vfprintf(stream, format, arguments);
    va_end(arguments);
    fclose(stream);
}

static void miband_net_set_error(MiBandNetState *s, uint32_t code)
{
    if (s->last_error != code) {
        miband_net_trace("miband-net error set %u (was %u)\n",
                         code, s->last_error);
    }
    s->last_error = code;
}

static uint32_t miband_net_status(MiBandNetState *s)
{
    uint32_t status = MIBAND_NET_STATUS_TX_READY;
    NetClientState *nc = s->nic ? qemu_get_queue(s->nic) : NULL;

    if (nc && !nc->link_down) {
        status |= MIBAND_NET_STATUS_LINK;
    }
    if (s->rx_count) {
        status |= MIBAND_NET_STATUS_RX_READY;
    }
    if (s->last_error) {
        status |= MIBAND_NET_STATUS_FAULT;
    }
    return status;
}

static void miband_net_arp_reply(MiBandNetState *s, const uint8_t *arp,
                                 size_t arp_len)
{
    uint8_t reply[60];
    NetClientState *nc = qemu_get_queue(s->nic);

    if (arp_len < 28) {
        return;
    }
    /* ARP request for our guest address: answer it locally so a future TAP
     * backend and slirp can both resolve the gateway. */
    if (arp[0] != 0 || arp[1] != 1 || arp[2] != 0x08 || arp[3] != 0x00 ||
        arp[4] != 6 || arp[5] != 4 || arp[6] != 0 || arp[7] != 1 ||
        ldl_be_p(arp + 24) != MIBAND_NET_GUEST_IP) {
        return;
    }

    memset(reply, 0, sizeof(reply));
    memcpy(reply, arp + 8, 6);            /* destination: requester */
    memcpy(reply + 6, s->conf.macaddr.a, 6); /* source: guest MAC */
    reply[12] = 0x08;
    reply[13] = 0x06;

    /* Ethernet/ARP payload at +14. */
    stw_be_p(reply + 14, 1);              /* htype */
    stw_be_p(reply + 16, 0x0800);         /* ptype */
    reply[18] = 6;                        /* hlen */
    reply[19] = 4;                        /* plen */
    stw_be_p(reply + 20, 2);              /* reply */
    memcpy(reply + 22, s->conf.macaddr.a, 6); /* sha: guest MAC */
    stl_be_p(reply + 28, MIBAND_NET_GUEST_IP);   /* sender protocol: guest */
    memcpy(reply + 32, arp + 8, 6);       /* target: requester */
    stl_be_p(reply + 38, ldl_be_p(arp + 14)); /* target protocol: requester */

    qemu_send_packet(nc, reply, sizeof(reply));
}

static size_t miband_net_dns_skip_name(const uint8_t *packet, size_t offset,
                                       size_t end)
{
    while (offset < end) {
        uint8_t length = packet[offset];

        if (length == 0) {
            return offset + 1;
        }
        if ((length & 0xc0) == 0xc0) {
            return offset + 2;
        }
        offset += 1 + length;
    }
    return end;
}

static void miband_net_dns_checksum(uint8_t *packet, size_t size)
{
    uint32_t sum = 0;
    unsigned length = (unsigned)(size - 20);
    size_t i;

    packet[26] = 0;
    packet[27] = 0;
    for (i = 12; i < 20; i += 2) {
        sum += packet[i] << 8 | packet[i + 1];
    }
    sum += 17;
    sum += length;
    for (i = 20; i + 1 < size; i += 2) {
        sum += packet[i] << 8 | packet[i + 1];
    }
    if (size & 1) {
        sum += packet[size - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = ~sum & 0xffff;
    packet[26] = sum >> 8;
    packet[27] = sum & 0xff;
}

/* TCP checksum over the IPv4 pseudo header; the payload stays untouched. */
static void miband_net_tcp_checksum(uint8_t *packet, size_t size)
{
    uint32_t sum = 0;
    unsigned ip_hdr = (packet[0] & 0x0f) * 4;
    unsigned length = (unsigned)(size - ip_hdr);
    size_t i;

    packet[ip_hdr + 16] = 0;
    packet[ip_hdr + 17] = 0;
    for (i = 12; i < 20; i += 2) {
        sum += packet[i] << 8 | packet[i + 1];
    }
    sum += 6;
    sum += length;
    for (i = ip_hdr; i + 1 < size; i += 2) {
        sum += packet[i] << 8 | packet[i + 1];
    }
    if (length & 1) {
        sum += packet[size - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = ~sum & 0xffff;
    packet[ip_hdr + 16] = sum >> 8;
    packet[ip_hdr + 17] = sum & 0xff;
}

/* Development-only MSS clamp.
 *
 * The reference tun module hands packets to the native stack through its
 * netdev; full-size segments can be dropped there while the socket buffer is
 * full, which stalls a TLS handshake that needs one large certificate record.
 * Clamping the MSS the guest advertises makes the peer send smaller segments.
 * Disabled unless MIBAND_NET_MSS is set. */
static uint16_t miband_net_mss_limit(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *spec = getenv("MIBAND_NET_MSS");
        cached = spec ? atoi(spec) : 0;
        if (cached > 0 && cached < 536) {
            cached = 536;
        }
    }
    return cached > 0 ? (uint16_t)cached : 0;
}

static void miband_net_clamp_mss(uint8_t *packet, size_t size)
{
    uint16_t limit = miband_net_mss_limit();
    unsigned ip_hdr, tcp, opt, end;

    if (!limit || size < 40 || packet[9] != 6) {
        return;
    }
    ip_hdr = (packet[0] & 0x0f) * 4;
    if (size < ip_hdr + 20) {
        return;
    }
    tcp = ip_hdr;
    if ((packet[tcp + 13] & 0x02) == 0 || (packet[tcp + 13] & 0x10) != 0) {
        return;
    }
    end = tcp + (unsigned)(packet[tcp + 12] >> 4) * 4;
    if (end > size) {
        return;
    }
    for (opt = tcp + 20; opt + 1 < end;) {
        unsigned kind = packet[opt];
        unsigned length;

        if (kind == 0) {
            break;
        }
        if (kind == 1) {
            opt++;
            continue;
        }
        length = packet[opt + 1];
        if (length < 2 || opt + length > end) {
            break;
        }
        if (kind == 2 && length == 4) {
            unsigned value = (unsigned)(packet[opt + 2] << 8 | packet[opt + 3]);

            if (value > limit) {
                packet[opt + 2] = limit >> 8;
                packet[opt + 3] = limit & 0xff;
                miband_net_tcp_checksum(packet, size);
                miband_net_trace("miband-net mss clamp %u -> %u\n", value, limit);
            }
            break;
        }
        opt += length;
    }
}

/* Development-only DNS answer rewrite.
 *
 * The guest reaches a real resolver through slirp, so an A record may name an
 * address outside the emulated 10.0.2.0/24 network.  slirp's guest forwarding
 * only accepts guest-subnet targets, so the answer is rewritten to one the
 * bridge can route; the UDP checksum is recomputed so the guest stack accepts
 * the frame.  Enabled only through MIBAND_NET_DNS_NAT.
 */
static void miband_net_dns_nat(uint8_t *packet, size_t size)
{
    const char *spec = getenv("MIBAND_NET_DNS_NAT");
    uint8_t target[4];
    unsigned answers, index;
    size_t end, offset;

    if (spec == NULL || *spec == '\0' || size < 42) {
        return;
    }
    if (inet_pton(AF_INET, spec, target) != 1) {
        return;
    }
    if ((packet[0] >> 4) != 4 || packet[9] != 17 ||
        packet[20] != 0 || packet[21] != 53) {
        return;
    }
    if ((packet[30] & 0x80) == 0) {           /* QR: only answers */
        return;
    }
    answers = packet[34] << 8 | packet[35];   /* ANCOUNT */
    if (answers == 0) {
        return;
    }
    end = 20 + (packet[24] << 8 | packet[25]);
    if (end > size) {
        end = size;
    }
    offset = miband_net_dns_skip_name(packet, 40, end) + 4;
    for (index = 0; index < answers && offset + 10 <= end; index++) {
        unsigned type, rdlength;

        offset = miband_net_dns_skip_name(packet, offset, end);
        if (offset + 10 > end) {
            break;
        }
        type = packet[offset] << 8 | packet[offset + 1];
        rdlength = packet[offset + 8] << 8 | packet[offset + 9];
        offset += 10;
        if (type == 1 && rdlength == 4 && offset + 4 <= end) {
            memcpy(packet + offset, target, 4);
            miband_net_dns_checksum(packet, end);
            miband_net_trace("miband-net nat dns answer -> %u.%u.%u.%u\n",
                             target[0], target[1], target[2], target[3]);
            return;
        }
        offset += rdlength;
    }
}

static ssize_t miband_net_receive(NetClientState *nc, const uint8_t *buf,
                                  size_t size)
{
    MiBandNetState *s = qemu_get_nic_opaque(nc);
    uint16_t proto;
    MiBandNetRxPacket *packet;

    if (size < 14) {
        s->rx_drops++;
        return size;
    }
    proto = lduw_be_p(buf + 12);
    miband_net_trace("miband-net rx proto=0x%04x len=%zu\n",
                     (unsigned)proto, size);
    if (proto == 0x0806) {
        miband_net_arp_reply(s, buf + 14, size - 14);
        return size;
    }
    if (proto != 0x0800) {
        s->rx_drops++;
        return size;
    }
    if (size >= 34) {
        const uint8_t *ip = buf + 14;
        unsigned ip_hdr = (ip[0] & 0x0f) * 4;
        unsigned ip_len = lduw_be_p(ip + 2);

        /* Trace arguments are evaluated even with tracing disabled. Never
         * interpret fragments or truncated/invalid headers as a TCP segment. */
        if ((ip[0] >> 4) == 4 && ip_hdr >= 20 && ip_len <= size - 14 &&
            ip_len >= ip_hdr + 20 && !(lduw_be_p(ip + 6) & 0x3fff) &&
            ip[9] == 6) {
            unsigned tcp_hdr = (unsigned)(ip[ip_hdr + 12] >> 4) * 4;
            unsigned payload = 14 + ip_hdr + tcp_hdr;
            unsigned sport = (unsigned)(ip[ip_hdr] << 8 | ip[ip_hdr + 1]);

            if (tcp_hdr < 20 || tcp_hdr > ip_len - ip_hdr) {
                goto queue_packet;
            }
            miband_net_trace("miband-net tcp rx sport=%u dport=%u len=%zu\n",
                             sport,
                             (unsigned)(ip[ip_hdr + 2] << 8 | ip[ip_hdr + 3]),
                             size);
            if (sport == 443) {
                miband_net_trace(
                        "miband-net tcp rx seq=%u ack=%u flags=0x%02x\n",
                        ldl_be_p(ip + ip_hdr + 4), ldl_be_p(ip + ip_hdr + 8),
                        ip[ip_hdr + 13]);
            }
            if (payload + 5 <= 14 + ip_len && sport == 443) {
                miband_net_trace(
                        "miband-net tls rx rec=%u ver=%u.%u reclen=%u\n",
                        buf[payload], buf[payload + 1], buf[payload + 2],
                        (unsigned)(buf[payload + 3] << 8 | buf[payload + 4]));
            }
        }
    }
queue_packet:
    if (size - 14 > MIBAND_NET_FIFO_SIZE) {
        miband_net_set_error(s, MIBAND_NET_ERR_RX_OVERSIZE);
        s->rx_drops++;
        return size;
    }
    if (s->rx_count == MIBAND_NET_RX_QUEUE) {
        /* Retain the packet until the guest acknowledges an RX slot. */
        return 0;
    }

    packet = &s->rx[s->rx_tail];
    packet->len = size - 14;
    memcpy(packet->data, buf + 14, packet->len);
    miband_net_dns_nat(packet->data, packet->len);
    s->rx_tail = (s->rx_tail + 1) % MIBAND_NET_RX_QUEUE;
    s->rx_count++;
    s->rx_packets++;
    return size;
}

static bool miband_net_can_receive(NetClientState *nc)
{
    MiBandNetState *s = qemu_get_nic_opaque(nc);
    return s->rx_count < MIBAND_NET_RX_QUEUE;
}

static void miband_net_link_status_changed(NetClientState *nc)
{
    MiBandNetState *s = qemu_get_nic_opaque(nc);
    if (nc->link_down) {
        miband_net_set_error(s, MIBAND_NET_ERR_BACKEND);
    } else if (s->last_error == MIBAND_NET_ERR_BACKEND) {
        miband_net_set_error(s, MIBAND_NET_ERR_NONE);
    }
}

static NetClientInfo miband_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = miband_net_receive,
    .can_receive = miband_net_can_receive,
    .link_status_changed = miband_net_link_status_changed,
};

static void miband_net_send(MiBandNetState *s)
{
    uint8_t frame[14 + MIBAND_NET_FIFO_SIZE];
    uint32_t total;

    if (!s->nic) {
        miband_net_set_error(s, MIBAND_NET_ERR_BACKEND);
        s->tx_errors++;
        return;
    }
    if (s->tx_len == 0 || s->tx_len > MIBAND_NET_FIFO_SIZE) {
        miband_net_set_error(s, MIBAND_NET_ERR_TX_OVERSIZE);
        s->tx_errors++;
        return;
    }
    miband_net_clamp_mss(s->tx, s->tx_len);

    memcpy(frame, s->conf.macaddr.a, 6);
    frame[6] = MIBAND_NET_GATEWAY_MAC0;
    frame[7] = MIBAND_NET_GATEWAY_MAC1;
    frame[8] = MIBAND_NET_GATEWAY_MAC2;
    frame[9] = MIBAND_NET_GATEWAY_MAC3;
    frame[10] = MIBAND_NET_GATEWAY_MAC4;
    frame[11] = MIBAND_NET_GATEWAY_MAC5;
    frame[12] = 0x08;
    frame[13] = 0x00;
    memcpy(frame + 14, s->tx, s->tx_len);
    if (s->tx_len >= 20) {
        miband_net_trace(
                "miband-net tx proto=%u %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len=%u\n",
                s->tx[9], s->tx[12], s->tx[13], s->tx[14], s->tx[15],
                (unsigned)(s->tx[20] << 8 | s->tx[21]),
                s->tx[16], s->tx[17], s->tx[18], s->tx[19],
                (unsigned)(s->tx[22] << 8 | s->tx[23]), (unsigned)s->tx_len);
        /* A DNS query carries the name in plain labels after eth/ip/udp/dns
         * headers; printing it identifies which host the guest resolves. */
        if (s->tx[9] == 17 && (unsigned)(s->tx[22] << 8 | s->tx[23]) == 53 &&
            s->tx_len > 54) {
            char name[96];
            unsigned in = 40, out = 0, label = 0;
            while (in < s->tx_len && out < sizeof(name) - 1) {
                unsigned length = s->tx[in];
                if (length == 0 || (length & 0xc0) != 0 || in + 1 + length > s->tx_len) {
                    break;
                }
                if (label++ != 0) {
                    name[out++] = '.';
                }
                in++;
                while (length-- != 0 && out < sizeof(name) - 1) {
                    name[out++] = (char)s->tx[in++];
                }
            }
            name[out] = '\0';
            miband_net_trace("miband-net dns query name=%s\n", name);
        }
        /* TLS record header (type/version/length) on port 443 tells whether the
         * guest finished the handshake or is still waiting for more records. */
        if (s->tx[9] == 6 && (unsigned)(s->tx[22] << 8 | s->tx[23]) == 443) {
            unsigned ip_hdr = (s->tx[0] & 0x0f) * 4;
            unsigned payload = ip_hdr + (unsigned)(s->tx[ip_hdr + 12] >> 4) * 4;
            miband_net_trace(
                    "miband-net tcp tx flags=0x%02x seq=%u ack=%u win=%u\n",
                    s->tx[ip_hdr + 13], ldl_be_p(s->tx + ip_hdr + 4),
                    ldl_be_p(s->tx + ip_hdr + 8),
                    (unsigned)(s->tx[ip_hdr + 14] << 8 | s->tx[ip_hdr + 15]));
            if (payload + 5 <= s->tx_len) {
                miband_net_trace(
                        "miband-net tls tx rec=%u ver=%u.%u reclen=%u\n",
                        s->tx[payload], s->tx[payload + 1], s->tx[payload + 2],
                        (unsigned)(s->tx[payload + 3] << 8 | s->tx[payload + 4]));
            }
        }
    }

    total = 14 + s->tx_len;
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    if (qemu_send_packet(qemu_get_queue(s->nic), frame, total) < 0) {
        miband_net_set_error(s, MIBAND_NET_ERR_BACKEND);
        s->tx_errors++;
        return;
    }
    s->tx_packets++;
    s->tx_len = 0;
}

static uint64_t miband_net_read(void *opaque, hwaddr offset, unsigned size)
{
    MiBandNetState *s = opaque;
    uint64_t value = 0;

    switch (offset) {
    case MIBAND_NET_REG_MAGIC:
        return MIBAND_NET_MAGIC;
    case MIBAND_NET_REG_VERSION:
        return MIBAND_NET_ABI_VERSION;
    case MIBAND_NET_REG_STATUS:
        return miband_net_status(s);
    case MIBAND_NET_REG_RX_LEN:
        return s->rx_count ? s->rx[s->rx_head].len : 0;
    case MIBAND_NET_REG_MAC_LO:
        return s->conf.macaddr.a[0] | (s->conf.macaddr.a[1] << 8) |
               (s->conf.macaddr.a[2] << 16) |
               ((uint32_t)s->conf.macaddr.a[3] << 24);
    case MIBAND_NET_REG_MAC_HI:
        return s->conf.macaddr.a[4] | (s->conf.macaddr.a[5] << 8);
    case MIBAND_NET_REG_MTU:
        return MIBAND_NET_MTU;
    case MIBAND_NET_REG_GUEST_STATE:
        return s->guest_state;
    case MIBAND_NET_REG_GUEST_ERROR:
        return s->guest_error;
    case MIBAND_NET_REG_RX_PACKETS:
        return s->rx_packets;
    case MIBAND_NET_REG_TX_PACKETS:
        return s->tx_packets;
    case MIBAND_NET_REG_RX_DROPS:
        return s->rx_drops;
    case MIBAND_NET_REG_ERRORS:
        return s->last_error;
    default:
        break;
    }

    if (offset >= MIBAND_NET_REG_RX_FIFO &&
        offset < MIBAND_NET_REG_RX_FIFO + MIBAND_NET_FIFO_SIZE) {
        MiBandNetRxPacket *packet;
        uint32_t pos = offset - MIBAND_NET_REG_RX_FIFO;
        unsigned i;

        if (!s->rx_count) {
            return 0;
        }
        packet = &s->rx[s->rx_head];
        for (i = 0; i < size && pos + i < packet->len; i++) {
            value |= (uint64_t)packet->data[pos + i] << (8 * i);
        }
        return value;
    }
    return 0;
}

static void miband_net_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    MiBandNetState *s = opaque;
    unsigned i;

    switch (offset) {
    case MIBAND_NET_REG_CONTROL:
        if (value & MIBAND_NET_CONTROL_RESET) {
            miband_net_reset_state(s);
            if (s->nic) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
        }
        return;
    case MIBAND_NET_REG_TX_LEN:
        s->tx_len = value;
        if (s->tx_len > MIBAND_NET_FIFO_SIZE) {
            miband_net_set_error(s, MIBAND_NET_ERR_TX_OVERSIZE);
        }
        return;
    case MIBAND_NET_REG_TX_KICK:
        if (value & 1) {
            miband_net_send(s);
        }
        return;
    case MIBAND_NET_REG_RX_ACK:
        if (value & 1 && s->rx_count) {
            miband_net_trace("miband-net consume len=%zu remaining=%u\n",
                             s->rx[s->rx_head].len, s->rx_count - 1);
            s->rx_head = (s->rx_head + 1) % MIBAND_NET_RX_QUEUE;
            s->rx_count--;
            if (s->nic) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
        }
        return;
    case MIBAND_NET_REG_READY:
        s->ready = value & 1;
        return;
    case MIBAND_NET_REG_GUEST_STATE:
        s->guest_state = value;
        return;
    case MIBAND_NET_REG_GUEST_ERROR:
        s->guest_error = value;
        miband_net_trace("miband-net guest error=0x%x\n", value);
        return;
    default:
        break;
    }

    if (offset >= MIBAND_NET_REG_TX_FIFO &&
        offset < MIBAND_NET_REG_TX_FIFO + MIBAND_NET_FIFO_SIZE) {
        uint32_t pos = offset - MIBAND_NET_REG_TX_FIFO;
        for (i = 0; i < size && pos + i < MIBAND_NET_FIFO_SIZE; i++) {
            s->tx[pos + i] = value >> (8 * i);
        }
    }
}

static const MemoryRegionOps miband_net_ops = {
    .read = miband_net_read,
    .write = miband_net_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void miband_net_reset(DeviceState *dev)
{
    MiBandNetState *s = MIBAND_NET(dev);
    miband_net_reset_state(s);
}

static void miband_net_realize(DeviceState *dev, Error **errp)
{
    MiBandNetState *s = MIBAND_NET(dev);
    NetClientState *nc;

    if (s->mmio_base & (MIBAND_NET_MMIO_SIZE - 1)) {
        error_setg(errp, "miband-net mmio-base must be 4 KiB aligned");
        return;
    }
    if (s->mmio_base < MIBAND_NET_MMIO_SIZE) {
        error_setg(errp, "miband-net mmio-base overlaps the reset vectors");
        return;
    }
    if (s->backend && strcmp(s->backend, "slirp") &&
        strcmp(s->backend, "auto")) {
        error_setg(errp, "miband-net backend '%s' is not implemented; "
                         "the guest MMIO ABI is reserved for a future "
                         "passthrough transport", s->backend);
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &miband_net_ops, s,
                          "miband-net", MIBAND_NET_MMIO_SIZE);
    if (!s->memory) {
        s->memory = get_system_memory();
    }
    memory_region_add_subregion_overlap(s->memory, s->mmio_base, &s->iomem, 1);

    miband_net_reset_state(s);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&miband_net_info, &s->conf,
                          object_get_typename(OBJECT(s)), dev->id,
                          &s->reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    nc = qemu_get_queue(s->nic);
    if (nc->link_down) {
        miband_net_set_error(s, MIBAND_NET_ERR_BACKEND);
    }
}

static void miband_net_unrealize(DeviceState *dev)
{
    MiBandNetState *s = MIBAND_NET(dev);
    memory_region_del_subregion(s->memory, &s->iomem);
    if (s->nic) {
        qemu_del_nic(s->nic);
        s->nic = NULL;
    }
}

static char *miband_net_stats(Object *obj, Error **errp)
{
    MiBandNetState *s = MIBAND_NET(obj);
    return g_strdup_printf(
        "{\"abi\":%u,\"mmio_base\":%u,\"backend\":\"%s\","
        "\"link\":%s,\"ready\":%s,\"rx_packets\":%" PRIu64 ","
        "\"tx_packets\":%" PRIu64 ",\"rx_drops\":%" PRIu64 ","
        "\"errors\":%" PRIu64 ",\"last_error\":%u,"
        "\"guest_state\":%u,\"guest_error\":%u}",
        MIBAND_NET_ABI_VERSION, s->mmio_base,
        s->backend ? s->backend : "slirp",
        miband_net_status(s) & MIBAND_NET_STATUS_LINK ? "true" : "false",
        s->ready ? "true" : "false",
        s->rx_packets, s->tx_packets, s->rx_drops, s->tx_errors,
        s->last_error, s->guest_state, s->guest_error);
}

static char *miband_net_abi(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", MIBAND_NET_ABI_VERSION);
}

static Property miband_net_properties[] = {
    DEFINE_NIC_PROPERTIES(MiBandNetState, conf),
    DEFINE_PROP_UINT32("mmio-base", MiBandNetState, mmio_base,
                       0x4ff00000),
    DEFINE_PROP_STRING("backend", MiBandNetState, backend),
    DEFINE_PROP_END_OF_LIST(),
};

static void miband_net_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = miband_net_realize;
    dc->unrealize = miband_net_unrealize;
    dc->reset = miband_net_reset;
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    device_class_set_props(dc, miband_net_properties);
    object_class_property_add_str(oc, "net-stats", miband_net_stats, NULL);
    object_class_property_add_str(oc, "net-abi", miband_net_abi, NULL);
}

static const TypeInfo miband_net_info_type = {
    .name = TYPE_MIBAND_NET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MiBandNetState),
    .class_init = miband_net_class_init,
};

void miband_net_machine_init(MachineState *ms, MemoryRegion *memory,
                             const char *netdev,
                             Error **errp)
{
    DeviceState *dev;

    if (!netdev || !*netdev) {
        return;
    }
    dev = qdev_new(TYPE_MIBAND_NET);
    MIBAND_NET(dev)->memory = memory;
    qdev_prop_set_string(dev, "netdev", netdev);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        object_unref(OBJECT(dev));
        return;
    }
}

static void miband_net_register_types(void)
{
    type_register_static(&miband_net_info_type);
}

type_init(miband_net_register_types)
