/*
 * HIFI4 protocol peer for the BEST1700 shared-memory IPC transport.
 * This executes neither DSP instructions nor audio workloads.  The guest
 * remains responsible for virtio initialization and its audio service reply.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qnum.h"
#include "best1700_hifi4_endpoint.h"

static void peer_object_unref(QObject *obj)
{
    qobject_unref(obj);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(QObject, peer_object_unref)

#define AP_IND  (1u << 3)
#define AP_DONE (1u << 6)
#define DSP_IND (1u << 9)
#define DSP_DONE (1u << 0)
#define CFG_SIZE 0x58
#define CHANNEL_SIZE 0x34
#define RECORD_SIZE 20
#define RSC_SIZE 0xd0
#define RPMSG_HEADER_SIZE 16
#define RPMSG_NS_ADDRESS 0x35
#define RPMSG_NS_SIZE 40
#define PEER_TICK_NS 1000000
#define TOKEN_LIMIT 64

#define CONFIG_NUMBERS(F) \
    F(mailbox_ap) F(mailbox_dsp) F(peer_config) F(peer_channels) \
    F(peer_records) F(resource_table) F(resource_size) F(vring) \
    F(vring_num) F(buffer_size) F(channel_id) F(version) F(features) \
    F(image_config_offset) F(image_resource_offset) F(notify_mask) \
    F(start_token) F(reset_token) F(panic_token) F(service_address) \
    F(payload_offset) F(payload_size) F(timeout_ms) F(expected_reset_vector)

typedef struct Hifi4Config {
#define CONFIG_MEMBER(name) uint32_t name;
    CONFIG_NUMBERS(CONFIG_MEMBER)
#undef CONFIG_MEMBER
    char service_name[32];
    uint8_t *request;
    uint8_t *response;
    size_t request_size;
    size_t response_size;
} Hifi4Config;

struct Best1700Hifi4Endpoint {
    AddressSpace *as;
    qemu_irq rx_irq;
    qemu_irq done_irq;
    QEMUTimer *timer;
    Hifi4Config cfg;
    uint8_t config_template[CFG_SIZE];
    uint8_t *resource_template;
    uint32_t rsc;
    uint32_t pending;
    uint32_t tokens[TOKEN_LIMIT];
    unsigned token_head, token_count;
    uint16_t next_avail[2];
    uint32_t remote_service;
    int64_t deadline;
    int64_t paused_at;
    uint64_t ipc_tx, ipc_rx, rpmsg_tx, rpmsg_rx, resets, unsupported;
    bool started, published, transport_ready, ns_sent, ns_bound;
    bool request_sent, service_ready, tx_busy;
    bool paused;
    const char *error;
};

static void peer_tick(void *opaque);

static void update_irq(Best1700Hifi4Endpoint *s)
{
    qemu_set_irq(s->rx_irq, !!(s->pending & DSP_IND));
    qemu_set_irq(s->done_irq, !!(s->pending & AP_DONE));
}

static void fail(Best1700Hifi4Endpoint *s, const char *reason)
{
    if (!s->error) {
        s->error = reason;
        s->service_ready = false;
        timer_del(s->timer);
        qemu_log_mask(LOG_GUEST_ERROR, "best1700 hifi4: %s\n", reason);
    }
}

static bool read_mem(Best1700Hifi4Endpoint *s, uint32_t addr,
                     void *data, size_t size)
{
    MemTxAttrs attrs = { .secure = false, .memory = true };
    if ((uint64_t)addr + size > (1ULL << 32) ||
        address_space_read(s->as, addr, attrs, data, size) != MEMTX_OK) {
        fail(s, "guest shared-memory read failed");
        return false;
    }
    return true;
}

static bool write_mem(Best1700Hifi4Endpoint *s, uint32_t addr,
                      const void *data, size_t size)
{
    MemTxAttrs attrs = { .secure = false, .memory = true };
    if ((uint64_t)addr + size > (1ULL << 32) ||
        address_space_write(s->as, addr, attrs, data, size) != MEMTX_OK) {
        fail(s, "guest shared-memory write failed");
        return false;
    }
    return true;
}

static bool read32(Best1700Hifi4Endpoint *s, uint32_t addr, uint32_t *value)
{
    uint8_t data[4];
    if (!read_mem(s, addr, data, sizeof(data))) {
        return false;
    }
    *value = ldl_le_p(data);
    return true;
}

static bool write32(Best1700Hifi4Endpoint *s, uint32_t addr, uint32_t value)
{
    uint8_t data[4];
    stl_le_p(data, value);
    return write_mem(s, addr, data, sizeof(data));
}

static bool write16(Best1700Hifi4Endpoint *s, uint32_t addr, uint16_t value)
{
    uint8_t data[2];
    stw_le_p(data, value);
    return write_mem(s, addr, data, sizeof(data));
}

static uint32_t ring_bytes(uint32_t num)
{
    return QEMU_ALIGN_UP(16 * num + 2 * (num + 3), 8) + 8 * num + 6;
}

static void schedule(Best1700Hifi4Endpoint *s)
{
    if (s->started && !s->paused && !s->error) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  PEER_TICK_NS);
    }
}

static void queue_token(Best1700Hifi4Endpoint *s, uint32_t token)
{
    if (s->token_count == TOKEN_LIMIT) {
        fail(s, "IPC notification queue overflow");
        return;
    }
    s->tokens[(s->token_head + s->token_count) % TOKEN_LIMIT] = token;
    s->token_count++;
}

static void send_token(Best1700Hifi4Endpoint *s)
{
    uint8_t record[RECORD_SIZE] = { 0 };
    if (s->tx_busy || !s->token_count || s->error) {
        return;
    }
    /* Zero-length IPC messages carry the notification in the data field. */
    stl_le_p(record + 8, s->tokens[s->token_head]);
    record[16] = 1;
    if (!write_mem(s, s->cfg.peer_records, record, sizeof(record)) ||
        !write32(s, s->cfg.peer_channels + 4, s->cfg.peer_records)) {
        return;
    }
    s->token_head = (s->token_head + 1) % TOKEN_LIMIT;
    s->token_count--;
    s->tx_busy = true;
    s->pending |= DSP_IND;
    s->ipc_tx++;
    update_irq(s);
}

static bool publish_peer(Best1700Hifi4Endpoint *s)
{
    Hifi4Config *c = &s->cfg;
    uint8_t config[CFG_SIZE];
    uint8_t channels[CHANNEL_SIZE * 2] = { 0 };
    g_autofree uint8_t *rsc = g_memdup2(s->resource_template, c->resource_size);
    uint32_t buffer_bytes = QEMU_ALIGN_UP(ring_bytes(c->vring_num), 8) +
                            c->vring_num * c->buffer_size;
    g_autofree uint8_t *zero = g_malloc0(buffer_bytes);
    uint8_t *resource = rsc + (s->rsc - c->resource_table);

    memcpy(config, s->config_template, sizeof(config));
    stl_le_p(config + 4, c->mailbox_ap);
    stl_le_p(config + 8, c->mailbox_dsp);
    stl_le_p(config + 0x3c, c->peer_channels);
    stl_le_p(config + 0x40, c->peer_records);
    stl_le_p(config + 0x50, c->resource_table);
    stl_le_p(config + 0x54, c->resource_table + c->resource_size);
    stl_le_p(channels, c->peer_config);
    stl_le_p(channels + 0xc, c->peer_records);
    stw_le_p(channels + 0x2c, lduw_le_p(config + 0x3a));
    channels[0x33] = 1;

    /* The peer advertises one RPMsg vdev. The AP owns ring 1 and DRIVER_OK. */
    stl_le_p(resource + 0x10, 0x48);
    stl_le_p(resource + 0x48, 3);
    stl_le_p(resource + 0x4c, 7);
    stl_le_p(resource + 0x50, UINT32_MAX);
    stl_le_p(resource + 0x54, c->features);
    stl_le_p(resource + 0x58, 0);
    stl_le_p(resource + 0x5c, 0x40);
    resource[0x60] = 0;
    resource[0x61] = 2;
    stl_le_p(resource + 0x64, c->vring);
    stl_le_p(resource + 0x68, 8);
    stl_le_p(resource + 0x6c, c->vring_num);
    stl_le_p(resource + 0x70, UINT32_MAX);
    stl_le_p(resource + 0x90, c->buffer_size);
    if (!write_mem(s, c->vring, zero, buffer_bytes) ||
        !write_mem(s, c->peer_config, config, sizeof(config)) ||
        !write_mem(s, c->peer_channels, channels, sizeof(channels)) ||
        !write_mem(s, c->resource_table, rsc, c->resource_size) ||
        !write32(s, c->mailbox_dsp, c->peer_config)) {
        return false;
    }
    s->published = true;
    queue_token(s, c->start_token);
    return true;
}

typedef struct PeerDescriptor {
    uint32_t ring, used, data, length;
    uint16_t id;
} PeerDescriptor;

/* A remote virtio peer consumes the AP-owned available ring and produces
 * used entries.  No descriptor is completed without reading its contents. */
static int get_descriptor(Best1700Hifi4Endpoint *s, unsigned ring,
                          PeerDescriptor *desc)
{
    uint8_t rsc[20], avail[4], index[2], raw[16];
    uint32_t base, num, align;
    uint16_t count;
    uint64_t data;
    if (!read_mem(s, s->rsc + 0x64 + ring * 20, rsc, sizeof(rsc))) {
        return -1;
    }
    base = ldl_le_p(rsc);
    align = ldl_le_p(rsc + 4);
    num = ldl_le_p(rsc + 8);
    if (!base || (base & 3) || num != s->cfg.vring_num || align != 8 ||
        (uint64_t)base + ring_bytes(num) > UINT32_MAX) {
        fail(s, "invalid guest virtqueue layout");
        return -1;
    }
    if (!read_mem(s, base + 16 * num, avail, sizeof(avail))) {
        return -1;
    }
    count = lduw_le_p(avail + 2) - s->next_avail[ring];
    if (!count) {
        return 0;
    }
    if (count > num) {
        fail(s, "guest virtqueue available index overrun");
        return -1;
    }
    if (!read_mem(s, base + 16 * num + 4 +
                  2 * (s->next_avail[ring] % num), index, sizeof(index))) {
        return -1;
    }
    desc->id = lduw_le_p(index);
    if (desc->id >= num ||
        !read_mem(s, base + 16 * desc->id, raw, sizeof(raw))) {
        fail(s, "guest virtqueue descriptor index invalid");
        return -1;
    }
    data = ldq_le_p(raw);
    desc->length = ldl_le_p(raw + 8);
    if (!data || data > UINT32_MAX || desc->length > s->cfg.buffer_size ||
        desc->length < RPMSG_HEADER_SIZE ||
        data + desc->length > (1ULL << 32) ||
        lduw_le_p(raw + 12) != (ring == 0 ? 2 : 0)) {
        fail(s, "unsupported or invalid RPMsg descriptor");
        return -1;
    }
    desc->data = data;
    desc->ring = base;
    desc->used = base + QEMU_ALIGN_UP(16 * num + 2 * (num + 3), align);
    return 1;
}

static bool complete_descriptor(Best1700Hifi4Endpoint *s, unsigned ring,
                                 PeerDescriptor *desc, uint32_t length)
{
    uint8_t raw[4], used[8];
    uint16_t index;
    if (!read_mem(s, desc->used, raw, sizeof(raw))) {
        return false;
    }
    index = lduw_le_p(raw + 2);
    stl_le_p(used, desc->id);
    stl_le_p(used + 4, length);
    if (!write_mem(s, desc->used + 4 + 8 * (index % s->cfg.vring_num),
                   used, sizeof(used)) ||
        !write16(s, desc->used + 2, index + 1)) {
        return false;
    }
    s->next_avail[ring]++;
    return true;
}

static bool send_rpmsg(Best1700Hifi4Endpoint *s, uint32_t src, uint32_t dst,
                       const uint8_t *payload, size_t size)
{
    PeerDescriptor desc;
    g_autofree uint8_t *message = NULL;
    int ready = get_descriptor(s, 0, &desc);
    if (ready <= 0) {
        return false;
    }
    if (size + RPMSG_HEADER_SIZE > desc.length) {
        fail(s, "guest RPMsg receive buffer is too small");
        return false;
    }
    message = g_malloc0(RPMSG_HEADER_SIZE + size);
    stl_le_p(message, src);
    stl_le_p(message + 4, dst);
    stw_le_p(message + 12, size);
    memcpy(message + RPMSG_HEADER_SIZE, payload, size);
    if (!write_mem(s, desc.data, message, RPMSG_HEADER_SIZE + size) ||
        !complete_descriptor(s, 0, &desc, RPMSG_HEADER_SIZE + size)) {
        return false;
    }
    s->rpmsg_tx++;
    queue_token(s, s->cfg.notify_mask);
    return true;
}

static void receive_rpmsg(Best1700Hifi4Endpoint *s)
{
    for (unsigned i = 0; i < s->cfg.vring_num && !s->error; i++) {
        PeerDescriptor desc;
        uint32_t src, dst;
        uint16_t size;
        g_autofree uint8_t *message = NULL;
        const uint8_t *payload;
        if (get_descriptor(s, 1, &desc) <= 0) {
            break;
        }
        message = g_malloc(desc.length);
        if (!read_mem(s, desc.data, message, desc.length)) {
            break;
        }
        src = ldl_le_p(message);
        dst = ldl_le_p(message + 4);
        size = lduw_le_p(message + 12);
        payload = message + RPMSG_HEADER_SIZE;
        if (size > desc.length - RPMSG_HEADER_SIZE) {
            fail(s, "guest RPMsg payload exceeds descriptor");
            break;
        }
        if (dst == RPMSG_NS_ADDRESS && size == RPMSG_NS_SIZE &&
            !memcmp(payload, s->cfg.service_name, sizeof(s->cfg.service_name))) {
            uint32_t flag = ldl_le_p(payload + 36);
            if (flag == 2 && s->ns_sent && src >= 1024 &&
                src < 0xffffff00u && src == ldl_le_p(payload + 32)) {
                s->remote_service = src;
                s->ns_bound = true;
            } else if (flag == 1) {
                fail(s, "guest audio service was removed");
            } else {
                s->unsupported++;
            }
        } else if (dst == s->cfg.service_address && s->ns_bound &&
                   src == s->remote_service && s->request_sent) {
            if (size == s->cfg.response_size &&
                !memcmp(payload, s->cfg.response, size)) {
                s->service_ready = true;
            } else {
                fail(s, "guest audio capability response did not match profile");
            }
        } else {
            /* Return the consumed buffer, but never invent an RPC result. */
            s->unsupported++;
        }
        if (!complete_descriptor(s, 1, &desc, desc.length)) {
            break;
        }
        s->rpmsg_rx++;
        queue_token(s, s->cfg.notify_mask | 1);
    }
}

static void receive_ipc(Best1700Hifi4Endpoint *s)
{
    uint32_t cfg, channels, node;
    if (!read32(s, s->cfg.mailbox_ap, &cfg) || !cfg) {
        return;
    }
    if ((cfg & 3) || cfg > UINT32_MAX - CFG_SIZE ||
        !read32(s, cfg + 0x3c, &channels) || !channels ||
        (channels & 3) || channels > UINT32_MAX - CHANNEL_SIZE ||
        !read32(s, channels + 4, &node)) {
        fail(s, "guest IPC configuration pointer is invalid");
        return;
    }
    for (unsigned i = 0; node; i++) {
        uint8_t raw[RECORD_SIZE];
        uint32_t token;
        if (i == 32 || (node & 3) || !read_mem(s, node, raw, sizeof(raw))) {
            fail(s, "invalid guest IPC message list");
            return;
        }
        token = ldl_le_p(raw + 8);
        if (ldl_le_p(raw + 4) != 0 ||
            (token & 0xffffff00u) != s->cfg.notify_mask) {
            s->unsupported++;
        } else if (token == s->cfg.panic_token) {
            fail(s, "guest reported a peer panic");
            return;
        } else if (token == s->cfg.reset_token) {
            /* A reset request invalidates the transport until reset release. */
            best1700_hifi4_reset(s);
            return;
        }
        s->ipc_rx++;
        node = ldl_le_p(raw);
    }
    s->pending &= ~AP_IND;
    s->pending |= AP_DONE;
    update_irq(s);
}

static void peer_tick(void *opaque)
{
    Best1700Hifi4Endpoint *s = opaque;
    uint8_t status[2];
    uint32_t ring1;
    if (!s->started || s->paused || s->error) {
        return;
    }
    if (!s->published && !publish_peer(s)) {
        return;
    }
    if (s->pending & DSP_DONE) {
        s->pending &= ~DSP_DONE;
        s->tx_busy = false;
        if (!write32(s, s->cfg.peer_channels + 4, 0)) {
            return;
        }
    }
    if (s->pending & AP_IND) {
        receive_ipc(s);
    }
    if (s->error || !s->started) {
        return;
    }
    if (!read_mem(s, s->rsc + 0x60, status, sizeof(status)) ||
        !read32(s, s->rsc + 0x78, &ring1)) {
        return;
    }
    if (status[0] & 0xc0) {
        fail(s, "guest virtio endpoint requested reset or reported failure");
        return;
    }
    if ((status[0] & 4) && ring1 && status[1] == 2) {
        s->transport_ready = true;
        receive_rpmsg(s);
        if (!s->ns_sent && !s->error) {
            uint8_t ns[RPMSG_NS_SIZE] = { 0 };
            memcpy(ns, s->cfg.service_name, sizeof(s->cfg.service_name));
            stl_le_p(ns + 32, s->cfg.service_address);
            s->ns_sent = send_rpmsg(s, s->cfg.service_address,
                                   RPMSG_NS_ADDRESS, ns, sizeof(ns));
        }
        if (s->ns_bound && !s->request_sent && !s->error) {
            s->request_sent = send_rpmsg(s, s->cfg.service_address,
                                         s->remote_service,
                                         s->cfg.request, s->cfg.request_size);
        }
    }
    send_token(s);
    if (!s->service_ready &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->deadline) {
        fail(s, "guest IPC or audio service handshake timed out");
        return;
    }
    schedule(s);
}

static bool uint_field(QDict *dict, const char *name, uint32_t *value,
                        Error **errp)
{
    QNum *number = qobject_to(QNum, qdict_get(dict, name));
    int64_t integer;
    if (!number || !qnum_get_try_int(number, &integer) || integer < 0 ||
        integer > UINT32_MAX) {
        error_setg(errp, "HIFI4 endpoint %s must be uint32", name);
        return false;
    }
    *value = integer;
    return true;
}

static uint8_t *hex_bytes(const char *text, size_t *length, Error **errp)
{
    uint8_t *result;
    if (!text || !*text || strlen(text) % 2 || strlen(text) > 2048) {
        error_setg(errp, "HIFI4 endpoint requires a bounded hexadecimal message");
        return NULL;
    }
    *length = strlen(text) / 2;
    result = g_malloc(*length);
    for (size_t i = 0; i < *length; i++) {
        int hi = g_ascii_xdigit_value(text[i * 2]);
        int lo = g_ascii_xdigit_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            error_setg(errp, "HIFI4 endpoint message contains invalid hex");
            g_free(result);
            return NULL;
        }
        result[i] = hi * 16 + lo;
    }
    return result;
}

static bool digest_matches(const uint8_t *data, size_t size, const char *hash)
{
    g_autofree char *actual = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                        data, size);
    return hash && strlen(hash) == 64 && !strcmp(actual, hash);
}

static bool in_range(uint32_t address, uint64_t length,
                     uint32_t base, uint32_t size)
{
    return address >= base && (uint64_t)address + length <= (uint64_t)base + size;
}

static bool layout_disjoint(const Hifi4Config *c)
{
    struct { uint32_t address; uint64_t size; } regions[] = {
        { c->mailbox_ap, 4 }, { c->mailbox_dsp, 4 },
        { c->peer_config, CFG_SIZE },
        { c->peer_channels, 2 * CHANNEL_SIZE },
        { c->peer_records, 2 * 32 * RECORD_SIZE },
        { c->resource_table, c->resource_size },
        { c->vring, QEMU_ALIGN_UP(ring_bytes(c->vring_num), 8) +
                     c->vring_num * c->buffer_size },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(regions); i++) {
        if (regions[i].address & 3) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (regions[i].address < regions[j].address + regions[j].size &&
                regions[j].address < regions[i].address + regions[i].size) {
                return false;
            }
        }
    }
    return !(c->vring & 7);
}

static bool valid_capability_message(const uint8_t *data, size_t size,
                                      bool response)
{
    return size == 20 && data[0] == 1 && data[1] == (response ? 16 : 0) &&
           lduw_le_p(data + 2) == size && data[4] == 16 &&
           data[5] == 0 && data[6] == 0;
}

static bool load_profile(Best1700Hifi4Endpoint *s, const char *profile,
                          const char *image, Error **errp)
{
    g_autofree char *json = NULL;
    g_autofree char *binary = NULL;
    g_autoptr(QObject) root_object = NULL;
    QDict *root, *ipc, *dict, *images, *audio, *memory;
    const char *name, *schema;
    const uint8_t *payload;
    size_t image_size;
    uint32_t dtcm_base, dtcm_size, itcm_size;
    int64_t alias_delta;
    Hifi4Config *c = &s->cfg;
    GError *error = NULL;
    unsigned numeric_fields = 0;

    if (!g_file_get_contents(profile, &json, NULL, &error)) {
        error_setg(errp, "cannot read HIFI4 profile: %s", error->message);
        g_error_free(error);
        return false;
    }
    root_object = qobject_from_json(json, errp);
    root = qobject_to(QDict, root_object);
    ipc = root ? qobject_to(QDict, qdict_get(root, "ipc")) : NULL;
    dict = ipc ? qobject_to(QDict, qdict_get(ipc, "audio_endpoint")) : NULL;
    images = root ? qobject_to(QDict, qdict_get(root, "images")) : NULL;
    audio = images ? qobject_to(QDict, qdict_get(images, "audio")) : NULL;
    memory = audio ? qobject_to(QDict, qdict_get(audio, "memory")) : NULL;
    if (!dict || !audio || !memory) {
        if (!errp || !*errp) {
            error_setg(errp, "profile lacks HIFI4 endpoint or audio image metadata");
        }
        return false;
    }
    schema = qdict_get_try_str(dict, "schema");
    name = qdict_get_try_str(dict, "service_name");
    if (!schema || strcmp(schema, "bes.hifi4.endpoint.v1") ||
        !name || !*name || strlen(name) >= sizeof(c->service_name)) {
        error_setg(errp, "invalid HIFI4 endpoint schema or service name");
        return false;
    }
    g_strlcpy(c->service_name, name, sizeof(c->service_name));
#define LOAD_FIELD(field) \
    if (!uint_field(dict, #field, &c->field, errp)) { return false; } \
    numeric_fields++;
    CONFIG_NUMBERS(LOAD_FIELD)
#undef LOAD_FIELD
    if (qdict_size(dict) != numeric_fields + 5 ||
        !uint_field(memory, "dtcm_base", &dtcm_base, errp) ||
        !uint_field(memory, "dtcm_size", &dtcm_size, errp) ||
        !uint_field(memory, "itcm_size", &itcm_size, errp)) {
        if (!errp || !*errp) {
            error_setg(errp, "unknown HIFI4 endpoint fields");
        }
        return false;
    }
    c->request = hex_bytes(qdict_get_try_str(dict, "request_hex"),
                            &c->request_size, errp);
    if (!c->request) {
        return false;
    }
    c->response = hex_bytes(qdict_get_try_str(dict, "response_hex"),
                             &c->response_size, errp);
    if (!c->response) {
        return false;
    }
    if (c->vring_num < 2 || c->vring_num > 256 ||
        (c->vring_num & (c->vring_num - 1)) ||
        c->buffer_size < 64 || c->buffer_size > 4096 ||
        (c->buffer_size & 3) || c->resource_size != RSC_SIZE * 2 ||
        c->timeout_ms < 100 || c->timeout_ms > 120000 ||
        !c->notify_mask || (c->notify_mask & 0xff) ||
        (c->start_token & 0xffffff00u) != c->notify_mask ||
        (c->reset_token & 0xffffff00u) != c->notify_mask ||
        (c->panic_token & 0xffffff00u) != c->notify_mask ||
        c->start_token == c->reset_token || c->start_token == c->panic_token ||
        c->reset_token == c->panic_token ||
        (c->start_token & 0xff) < 2 || (c->reset_token & 0xff) < 2 ||
        (c->panic_token & 0xff) < 2 ||
        c->service_address < 1024 || c->service_address >= 0xffffff00u ||
        !valid_capability_message(c->request, c->request_size, false) ||
        !valid_capability_message(c->response, c->response_size, true) ||
        c->request_size + RPMSG_HEADER_SIZE > c->buffer_size ||
        c->response_size + RPMSG_HEADER_SIZE > c->buffer_size ||
        !in_range(c->peer_config, CFG_SIZE, dtcm_base, dtcm_size) ||
        !in_range(c->peer_channels, CHANNEL_SIZE * 2, dtcm_base, dtcm_size) ||
        !in_range(c->peer_records, 2 * 32 * RECORD_SIZE, dtcm_base, dtcm_size) ||
        !in_range(c->resource_table, c->resource_size, dtcm_base, dtcm_size) ||
        !in_range(c->vring, QEMU_ALIGN_UP(ring_bytes(c->vring_num), 8) +
                   c->vring_num * c->buffer_size, dtcm_base, dtcm_size) ||
        !in_range(c->mailbox_ap, 4, dtcm_base, dtcm_size) ||
        !in_range(c->mailbox_dsp, 4, dtcm_base, dtcm_size) ||
        !layout_disjoint(c)) {
        error_setg(errp, "HIFI4 endpoint memory layout or protocol bounds invalid");
        return false;
    }
    if (!image || !g_file_get_contents(image, &binary, &image_size, &error)) {
        error_setg(errp, "cannot read HIFI4 audio image%s%s",
                   error ? ": " : "", error ? error->message : "");
        g_clear_error(&error);
        return false;
    }
    payload = (const uint8_t *)binary;
    if (image_size >= 8 && !memcmp(payload, "-rom1fs-", 8)) {
        if (!digest_matches(payload, image_size,
                            qdict_get_try_str(audio, "sha256")) ||
            (uint64_t)c->payload_offset + c->payload_size > image_size) {
            error_setg(errp, "HIFI4 ROMFS identity or payload bounds mismatch");
            return false;
        }
        payload += c->payload_offset;
        image_size = c->payload_size;
    }
    if (image_size != c->payload_size || image_size < 0xc8 ||
        (c->image_config_offset & 3) || (c->image_resource_offset & 3) ||
        (c->expected_reset_vector & 3) ||
        !digest_matches(payload, image_size,
                        qdict_get_try_str(dict, "payload_sha256")) ||
        (uint64_t)c->image_config_offset + CFG_SIZE > image_size ||
        (uint64_t)c->image_resource_offset + c->resource_size > image_size ||
        ldl_le_p(payload + 0x10) != c->expected_reset_vector ||
        ldl_le_p(payload + 0x18) - ldl_le_p(payload + 0x14) != itcm_size ||
        ldl_le_p(payload + 0x20) - ldl_le_p(payload + 0x1c) != dtcm_size) {
        error_setg(errp, "HIFI4 payload identity, reset vector or layout mismatch");
        return false;
    }
    memcpy(s->config_template, payload + c->image_config_offset, CFG_SIZE);
    alias_delta = (int64_t)dtcm_base - ldl_le_p(payload + 0x1c);
    if (ldl_le_p(s->config_template + 0x38) != 0x00200002 ||
        ldl_le_p(s->config_template + 4) != c->mailbox_ap ||
        ldl_le_p(s->config_template + 8) != c->mailbox_dsp ||
        (int64_t)ldl_le_p(s->config_template + 0x3c) + alias_delta != c->peer_channels ||
        (int64_t)ldl_le_p(s->config_template + 0x40) + alias_delta != c->peer_records ||
        (int64_t)ldl_le_p(s->config_template + 0x50) + alias_delta != c->resource_table ||
        (int64_t)ldl_le_p(s->config_template + 0x54) + alias_delta !=
            (int64_t)c->resource_table + c->resource_size) {
        error_setg(errp, "HIFI4 image IPC configuration format mismatch");
        return false;
    }
    s->resource_template = g_memdup2(payload + c->image_resource_offset,
                                    c->resource_size);
    for (uint32_t i = 0; i < c->resource_size; i += RSC_SIZE) {
        uint8_t *table = s->resource_template + i;
        uint8_t zero[0x4c] = { 0 };
        if (ldl_le_p(table + 0x98) != c->version ||
            ldl_le_p(table) != 1 || ldl_le_p(table + 4) != 1 ||
            !ldl_le_p(table + 0x94) ||
            memcmp(table + 0x48, zero, sizeof(zero))) {
            error_setg(errp, "audio image RPTUN template format mismatch");
            return false;
        }
        if (ldl_le_p(table + 0x94) == c->channel_id) {
            if (s->rsc) {
                error_setg(errp, "audio image repeats the RPTUN channel id");
                return false;
            }
            s->rsc = c->resource_table + i;
        }
    }
    if (s->rsc) {
        return true;
    }
    error_setg(errp, "audio image lacks the configured RPTUN resource table");
    return false;
}

Best1700Hifi4Endpoint *best1700_hifi4_new(AddressSpace *as,
                                        const char *profile_path,
                                        const char *audio_image,
                                        qemu_irq rx_irq, qemu_irq done_irq,
                                        Error **errp)
{
    Best1700Hifi4Endpoint *s;
    if (!profile_path || !*profile_path) {
        return NULL;
    }
    s = g_new0(Best1700Hifi4Endpoint, 1);
    s->as = as;
    s->rx_irq = rx_irq;
    s->done_irq = done_irq;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, peer_tick, s);
    if (!load_profile(s, profile_path, audio_image, errp)) {
        best1700_hifi4_free(s);
        return NULL;
    }
    return s;
}

void best1700_hifi4_start(Best1700Hifi4Endpoint *s, uint32_t vector)
{
    if (!s || s->started) {
        return;
    }
    if (vector != s->cfg.expected_reset_vector) {
        fail(s, "guest DSP reset vector does not match the image");
        return;
    }
    if (s->error) {
        return;
    }
    s->started = true;
    s->deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)s->cfg.timeout_ms * 1000000;
    schedule(s);
}

void best1700_hifi4_reset(Best1700Hifi4Endpoint *s)
{
    if (!s) {
        return;
    }
    timer_del(s->timer);
    s->started = s->published = s->transport_ready = false;
    s->ns_sent = s->ns_bound = s->request_sent = s->service_ready = false;
    s->tx_busy = false;
    s->paused = false;
    s->error = NULL;
    s->pending = 0;
    s->token_head = s->token_count = 0;
    s->next_avail[0] = s->next_avail[1] = 0;
    s->remote_service = 0;
    s->resets++;
    write32(s, s->cfg.mailbox_dsp, 0);
    write32(s, s->cfg.peer_channels + 4, 0);
    update_irq(s);
}

void best1700_hifi4_pause(Best1700Hifi4Endpoint *s, bool paused)
{
    int64_t now;
    if (!s || !s->started || s->paused == paused) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->paused = paused;
    if (paused) {
        s->paused_at = now;
        timer_del(s->timer);
    } else {
        s->deadline += now - s->paused_at;
        schedule(s);
    }
}

void best1700_hifi4_free(Best1700Hifi4Endpoint *s)
{
    if (!s) {
        return;
    }
    timer_free(s->timer);
    g_free(s->resource_template);
    g_free(s->cfg.request);
    g_free(s->cfg.response);
    g_free(s);
}

uint32_t best1700_hifi4_pending(const Best1700Hifi4Endpoint *s)
{
    return s ? s->pending : 0;
}

void best1700_hifi4_set(Best1700Hifi4Endpoint *s, uint32_t bits)
{
    if (s) {
        s->pending |= bits & BEST1700_HIFI4_IRQ_MASK;
        update_irq(s);
        schedule(s);
    }
}

void best1700_hifi4_clear(Best1700Hifi4Endpoint *s, uint32_t bits)
{
    if (s) {
        s->pending &= ~(bits & BEST1700_HIFI4_IRQ_MASK);
        update_irq(s);
    }
}

bool best1700_hifi4_service_ready(const Best1700Hifi4Endpoint *s)
{
    return s && s->service_ready && !s->error;
}

char *best1700_hifi4_status_json(const Best1700Hifi4Endpoint *s)
{
    const char *state;
    QDict *status = qdict_new();
    GString *json;
    if (!s) {
        qdict_put_str(status, "state", "disabled");
    } else {
        state = s->error ? "failed" : s->service_ready ? "service-ready" :
                s->ns_bound ? "service-bound" : s->transport_ready ?
                "transport-ready" : s->published ? "ipc-open" :
                s->started ? "starting" : "reset";
        qdict_put_str(status, "state", state);
        qdict_put_bool(status, "ready", best1700_hifi4_service_ready(s));
        qdict_put_bool(status, "transport_ready", s->transport_ready);
        qdict_put_bool(status, "registered", s->ns_bound);
        qdict_put_bool(status, "paused", s->paused);
        qdict_put_str(status, "service", s->cfg.service_name);
        qdict_put_int(status, "ipc_tx", s->ipc_tx);
        qdict_put_int(status, "ipc_rx", s->ipc_rx);
        qdict_put_int(status, "rpmsg_tx", s->rpmsg_tx);
        qdict_put_int(status, "rpmsg_rx", s->rpmsg_rx);
        qdict_put_int(status, "resets", s->resets);
        qdict_put_int(status, "unsupported", s->unsupported);
        if (s->error) {
            qdict_put_str(status, "error", s->error);
        }
    }
    json = qobject_to_json(QOBJECT(status));
    qobject_unref(status);
    return g_string_free(json, false);
}
