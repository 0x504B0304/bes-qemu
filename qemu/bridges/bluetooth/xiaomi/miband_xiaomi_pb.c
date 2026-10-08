#include "miband_xiaomi_pb.h"
#include <string.h>

typedef struct PbReader {
    const uint8_t *data;
    size_t length;
    size_t offset;
} PbReader;

static bool varint(PbReader *r, uint64_t *value)
{
    uint64_t result = 0;
    unsigned shift = 0;
    while (r->offset < r->length && shift < 64) {
        uint8_t byte = r->data[r->offset++];
        if (shift == 63 && byte > 1) return false;
        result |= ((uint64_t)(byte & 0x7f)) << shift;
        if (!(byte & 0x80)) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool field(PbReader *r, unsigned *number, unsigned *wire,
                  const uint8_t **value, size_t *length)
{
    uint64_t tag, size;
    if (!varint(r, &tag) || tag > UINT32_MAX || !(tag >> 3) ||
        (tag & 7) == 3 || (tag & 7) == 4 || (tag & 7) > 5) return false;
    *number = (unsigned)(tag >> 3);
    *wire = (unsigned)(tag & 7);
    if (*wire == 0) {
        *value = r->data + r->offset;
        size_t start = r->offset;
        if (!varint(r, &size)) return false;
        *length = r->offset - start;
        return true;
    }
    if (*wire != 2 || !varint(r, &size) || size > r->length - r->offset)
        return false;
    *value = r->data + r->offset;
    *length = (size_t)size;
    r->offset += (size_t)size;
    return true;
}

static bool decode_varint(const uint8_t *data, size_t length, uint64_t *value)
{
    PbReader r = { data, length, 0 };
    return varint(&r, value) && r.offset == r.length;
}

static bool copy_string(char *dst, size_t capacity, const uint8_t *data,
                        size_t length)
{
    if (length >= capacity) return false;
    memcpy(dst, data, length);
    dst[length] = 0;
    return true;
}

static bool decode_mass_request(const uint8_t *data, size_t length,
                                MiBandXiaomiPbRequest *request)
{
    PbReader r = { data, length, 0 };
    unsigned number, wire;
    const uint8_t *value;
    size_t value_length;
    while (r.offset < r.length) {
        if (!field(&r, &number, &wire, &value, &value_length)) return false;
        if (number == 1 && wire == 2) {
            PbReader nested = { value, value_length, 0 };
            while (nested.offset < nested.length) {
                uint64_t scalar;
                if (!field(&nested, &number, &wire, &value, &value_length)) return false;
                if (number == 1 && wire == 0) {
                    if (!decode_varint(value, value_length, &scalar) || scalar > UINT32_MAX) return false;
                    request->data_type = (uint32_t)scalar;
                } else if (number == 2 && wire == 2) {
                    if (value_length > sizeof(request->data_id)) return false;
                    memcpy(request->data_id, value, value_length);
                    request->data_id_len = value_length;
                } else if (number == 3 && wire == 0) {
                    if (!decode_varint(value, value_length, &scalar) || scalar > UINT32_MAX) return false;
                    request->data_length = (uint32_t)scalar;
                }
            }
        }
    }
    return request->data_id_len != 0 && request->data_type != 0;
}

static bool decode_watchface_request(const uint8_t *data, size_t length,
                                     MiBandXiaomiPbRequest *request)
{
    PbReader r = { data, length, 0 };
    unsigned number, wire;
    const uint8_t *value;
    size_t value_length;
    while (r.offset < r.length) {
        if (!field(&r, &number, &wire, &value, &value_length)) return false;
        if (number != 6 || wire != 2) continue;
        PbReader info = { value, value_length, 0 };
        while (info.offset < info.length) {
            uint64_t scalar;
            if (!field(&info, &number, &wire, &value, &value_length)) return false;
            if (number == 1 && wire == 2) {
                if (!copy_string(request->id, sizeof(request->id), value, value_length)) return false;
            } else if (number == 2 && wire == 0) {
                if (!decode_varint(value, value_length, &scalar) || scalar > UINT32_MAX) return false;
                request->data_length = (uint32_t)scalar;
            }
        }
    }
    return request->id[0] != 0;
}

static bool decode_app_request(const uint8_t *data, size_t length,
                               MiBandXiaomiPbRequest *request)
{
    PbReader r = { data, length, 0 };
    unsigned number, wire;
    const uint8_t *value;
    size_t value_length;
    while (r.offset < r.length) {
        if (!field(&r, &number, &wire, &value, &value_length)) return false;
        if (number != 2 || wire != 2) continue;
        PbReader nested = { value, value_length, 0 };
        while (nested.offset < nested.length) {
            uint64_t scalar;
            if (!field(&nested, &number, &wire, &value, &value_length)) return false;
            if (number == 1 && wire == 2) {
                if (!copy_string(request->package_name, sizeof(request->package_name), value, value_length)) return false;
            } else if (number == 2 && wire == 0) {
                if (!decode_varint(value, value_length, &scalar) || scalar > UINT32_MAX) return false;
                request->version_code = (uint32_t)scalar;
            } else if (number == 3 && wire == 0) {
                if (!decode_varint(value, value_length, &scalar) || scalar > UINT32_MAX) return false;
                request->data_length = (uint32_t)scalar;
            }
        }
    }
    return request->package_name[0] != 0;
}

bool miband_xiaomi_pb_decode_prepare(const uint8_t *data, size_t length,
                                     MiBandXiaomiPbRequest *request)
{
    PbReader r = { data, length, 0 };
    unsigned number, wire;
    const uint8_t *value;
    size_t value_length;
    uint64_t type = 0, id = 0;
    const uint8_t *payload = NULL;
    size_t payload_length = 0;

    if (!data || !request) return false;
    memset(request, 0, sizeof(*request));
    while (r.offset < r.length) {
        if (!field(&r, &number, &wire, &value, &value_length)) return false;
        if (number == 1 && wire == 0) {
            if (!decode_varint(value, value_length, &type) || type > UINT32_MAX) return false;
        } else if (number == 2 && wire == 0) {
            if (!decode_varint(value, value_length, &id) || id > UINT32_MAX) return false;
        } else if (wire == 2 && ((number == 24) || (number == 6) ||
                                 (number == 22))) {
            payload = value;
            payload_length = value_length;
        }
    }
    if (type == 22 && id == 0 && payload) {
        request->kind = MIBAND_XIAOMI_PB_MASS_PREPARE;
        return decode_mass_request(payload, payload_length, request);
    }
    if (type == 4 && id == 4 && payload) {
        request->kind = MIBAND_XIAOMI_PB_WATCHFACE_PREPARE;
        return decode_watchface_request(payload, payload_length, request);
    }
    if (type == 20 && id == 1 && payload) {
        request->kind = MIBAND_XIAOMI_PB_APP_PREPARE;
        return decode_app_request(payload, payload_length, request);
    }
    return false;
}

static size_t put_varint(uint8_t *output, size_t capacity, uint64_t value)
{
    size_t count = 0;
    do {
        if (count >= capacity) return 0;
        output[count++] = (uint8_t)(value & 0x7f) | (value >= 0x80 ? 0x80 : 0);
        value >>= 7;
    } while (value);
    return count;
}

static size_t put_key(uint8_t *output, size_t capacity, unsigned number,
                      unsigned wire)
{
    return put_varint(output, capacity, ((uint64_t)number << 3) | wire);
}

static size_t put_bytes(uint8_t *output, size_t capacity, unsigned number,
                        const uint8_t *data, size_t length)
{
    size_t n = put_key(output, capacity, number, 2);
    size_t m;
    if (!n || n + length > capacity) return 0;
    m = put_varint(output + n, capacity - n, length);
    if (!m || n + m + length > capacity) return 0;
    memcpy(output + n + m, data, length);
    return n + m + length;
}

static size_t put_u32(uint8_t *output, size_t capacity, unsigned number,
                      uint32_t value)
{
    size_t n = put_key(output, capacity, number, 0);
    size_t m;
    if (!n) return 0;
    m = put_varint(output + n, capacity - n, value);
    return m ? n + m : 0;
}

size_t miband_xiaomi_pb_encode_prepare_response(
    const MiBandXiaomiPbRequest *request, uint32_t expected_slice_length,
    uint32_t remained_data_length, uint8_t *output, size_t capacity)
{
    uint8_t inner[256], nested[320];
    size_t inner_len = 0, nested_len = 0, m, id_len, payload_len;
    const uint8_t *id = NULL;
    unsigned top_type, top_payload, payload_field;

    if (!request || !output) return 0;
    switch (request->kind) {
    case MIBAND_XIAOMI_PB_MASS_PREPARE:
        id = request->data_id; id_len = request->data_id_len;
        inner_len += put_bytes(inner + inner_len, sizeof(inner) - inner_len, 1, id, id_len);
        inner_len += put_u32(inner + inner_len, sizeof(inner) - inner_len, 2, 0);
        if (remained_data_length) inner_len += put_u32(inner + inner_len, sizeof(inner) - inner_len, 4, remained_data_length);
        inner_len += put_u32(inner + inner_len, sizeof(inner) - inner_len, 5, expected_slice_length);
        top_type = 22; top_payload = 24; payload_field = 2;
        break;
    case MIBAND_XIAOMI_PB_WATCHFACE_PREPARE:
        inner_len += put_bytes(inner + inner_len, sizeof(inner) - inner_len, 1,
                               (const uint8_t *)request->id, strlen(request->id));
        inner_len += put_u32(inner + inner_len, sizeof(inner) - inner_len, 2, 0);
        inner_len += put_u32(inner + inner_len, sizeof(inner) - inner_len, 4, expected_slice_length);
        top_type = 4; top_payload = 6; payload_field = 9;
        break;
    case MIBAND_XIAOMI_PB_APP_PREPARE:
        inner_len += put_u32(inner + inner_len, sizeof(inner) - inner_len, 1, 0);
        inner_len += put_u32(inner + inner_len, sizeof(inner) - inner_len, 2, expected_slice_length);
        top_type = 20; top_payload = 22; payload_field = 3;
        break;
    default:
        return 0;
    }
    if (!inner_len) return 0;
    nested_len = put_bytes(nested, sizeof(nested), payload_field, inner, inner_len);
    if (!nested_len) return 0;
    m = put_u32(output, capacity, 1, top_type);
    if (!m) return 0;
    id_len = put_u32(output + m, capacity - m, 2,
                     request->kind == MIBAND_XIAOMI_PB_MASS_PREPARE ? 0 : 1);
    if (!id_len) return 0;
    payload_len = put_bytes(output + m + id_len, capacity - m - id_len,
                            top_payload, nested, nested_len);
    return payload_len ? m + id_len + payload_len : 0;
}

size_t miband_xiaomi_pb_encode_install_result(
    const MiBandXiaomiPbRequest *request, uint8_t *output, size_t capacity)
{
    uint8_t result[192], wrapper[224];
    size_t result_len = 0, wrapper_len, type_len, id_len, payload_len;
    unsigned top_type, top_id, top_payload, wrapper_field;

    if (!request || !output) return 0;
    if (request->kind == MIBAND_XIAOMI_PB_WATCHFACE_PREPARE) {
        result_len += put_bytes(result + result_len, sizeof(result) - result_len, 1,
                                (const uint8_t *)request->id, strlen(request->id));
        result_len += put_u32(result + result_len, sizeof(result) - result_len, 2, 2);
        top_type = 4; top_id = 5; top_payload = 6; wrapper_field = 7;
    } else if (request->kind == MIBAND_XIAOMI_PB_APP_PREPARE) {
        result_len += put_u32(result + result_len, sizeof(result) - result_len, 1, 0);
        result_len += put_bytes(result + result_len, sizeof(result) - result_len, 2,
                                (const uint8_t *)request->package_name,
                                strlen(request->package_name));
        top_type = 20; top_id = 2; top_payload = 22; wrapper_field = 4;
    } else {
        return 0;
    }
    if (!result_len) return 0;
    wrapper_len = put_bytes(wrapper, sizeof(wrapper), wrapper_field,
                            result, result_len);
    if (!wrapper_len) return 0;
    type_len = put_u32(output, capacity, 1, top_type);
    if (!type_len) return 0;
    id_len = put_u32(output + type_len, capacity - type_len, 2, top_id);
    if (!id_len) return 0;
    payload_len = put_bytes(output + type_len + id_len,
                            capacity - type_len - id_len, top_payload,
                            wrapper, wrapper_len);
    return payload_len ? type_len + id_len + payload_len : 0;
}
