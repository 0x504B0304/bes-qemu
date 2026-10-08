#include "miband_xiaomi_pb.h"
#include <assert.h>
#include <string.h>

static size_t put_varint(uint8_t *p, unsigned value)
{
    size_t n = 0;
    do { p[n++] = (value & 0x7f) | (value >= 0x80 ? 0x80 : 0); value >>= 7; } while (value);
    return n;
}

static size_t field_u32(uint8_t *p, unsigned field, unsigned value)
{
    size_t n = put_varint(p, field << 3);
    return n + put_varint(p + n, value);
}

static size_t field_bytes(uint8_t *p, unsigned field, const uint8_t *data, size_t len)
{
    size_t n = put_varint(p, (field << 3) | 2);
    n += put_varint(p + n, (unsigned)len);
    memcpy(p + n, data, len);
    return n + len;
}

int main(void)
{
    uint8_t req[256], mass[128], prepare[128], id[16], response[256];
    size_t mass_len, prep_len, req_len, response_len;
    MiBandXiaomiPbRequest parsed;
    memset(id, 0x5a, sizeof(id));

    prep_len = field_u32(prepare, 1, 64);
    prep_len += field_bytes(prepare + prep_len, 2, id, sizeof(id));
    prep_len += field_u32(prepare + prep_len, 3, 123456);
    mass_len = field_bytes(mass, 1, prepare, prep_len);
    req_len = field_u32(req, 1, 22);
    req_len += field_u32(req + req_len, 2, 0);
    req_len += field_bytes(req + req_len, 24, mass, mass_len);
    assert(miband_xiaomi_pb_decode_prepare(req, req_len, &parsed));
    assert(parsed.kind == MIBAND_XIAOMI_PB_MASS_PREPARE);
    assert(parsed.data_type == 64 && parsed.data_length == 123456);
    assert(parsed.data_id_len == sizeof(id) && !memcmp(parsed.data_id, id, sizeof(id)));
    response_len = miband_xiaomi_pb_encode_prepare_response(&parsed, 12282, 0,
                                                             response, sizeof(response));
    assert(response_len > 0);
    assert(miband_xiaomi_pb_decode_prepare(response, response_len, &parsed) == false);
    memset(&parsed, 0, sizeof(parsed));
    parsed.kind = MIBAND_XIAOMI_PB_WATCHFACE_PREPARE;
    strcpy(parsed.id, "123456789");
    assert(miband_xiaomi_pb_encode_install_result(&parsed, response,
                                                  sizeof(response)) > 0);
    {
        uint8_t watch_info[128], watch_payload[160], watch_packet[256];
        size_t watch_info_len = field_bytes(watch_info, 1,
                                            (const uint8_t *)"123456789", 9);
        watch_info_len += field_u32(watch_info + watch_info_len, 2, 17);
        watch_info_len += field_u32(watch_info + watch_info_len, 3, 65536);
        size_t watch_payload_len = field_bytes(watch_payload, 6,
                                                watch_info, watch_info_len);
        size_t watch_len = field_u32(watch_packet, 1, 4);
        watch_len += field_u32(watch_packet + watch_len, 2, 4);
        watch_len += field_bytes(watch_packet + watch_len, 6,
                                 watch_payload, watch_payload_len);
        assert(miband_xiaomi_pb_decode_prepare(watch_packet, watch_len, &parsed));
        assert(parsed.kind == MIBAND_XIAOMI_PB_WATCHFACE_PREPARE &&
               !strcmp(parsed.id, "123456789") && parsed.data_length == 17);
    }
    memset(&parsed, 0, sizeof(parsed));
    parsed.kind = MIBAND_XIAOMI_PB_APP_PREPARE;
    strcpy(parsed.package_name, "com.example.app");
    assert(miband_xiaomi_pb_encode_install_result(&parsed, response,
                                                  sizeof(response)) > 0);
    return 0;
}
