#include "miband_xiaomi_protocol.h"
#include <string.h>

typedef struct MiBandMd5 {
    uint32_t h[4];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} MiBandMd5;

static uint32_t md5_rotate(uint32_t x, unsigned n)
{ return (x << n) | (x >> (32 - n)); }

static void md5_block(MiBandMd5 *ctx, const uint8_t *data)
{
    static const uint32_t k[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
        0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
        0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
        0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
        0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
        0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const uint8_t s[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    uint32_t w[16], a, b, c, d;
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)data[i*4] | (uint32_t)data[i*4+1] << 8 |
               (uint32_t)data[i*4+2] << 16 | (uint32_t)data[i*4+3] << 24;
    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7*i) & 15; }
        f += a + k[i] + w[g];
        a = d; d = c; c = b; b += md5_rotate(f, s[i]);
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
}

void miband_xiaomi_md5(const uint8_t *data, size_t length, uint8_t digest[16])
{
    MiBandMd5 ctx = { { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, 0, { 0 }, 0 };
    ctx.bits = (uint64_t)length * 8;
    while (length) {
        size_t take = 64 - ctx.used;
        if (take > length) take = length;
        memcpy(ctx.block + ctx.used, data, take);
        ctx.used += take; data += take; length -= take;
        if (ctx.used == 64) { md5_block(&ctx, ctx.block); ctx.used = 0; }
    }
    ctx.block[ctx.used++] = 0x80;
    if (ctx.used > 56) { memset(ctx.block + ctx.used, 0, 64 - ctx.used); md5_block(&ctx, ctx.block); ctx.used = 0; }
    memset(ctx.block + ctx.used, 0, 56 - ctx.used);
    for (unsigned i = 0; i < 8; i++) ctx.block[56 + i] = (uint8_t)(ctx.bits >> (8 * i));
    md5_block(&ctx, ctx.block);
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 4; j++)
        digest[i*4+j] = (uint8_t)(ctx.h[i] >> (8*j));
}

static uint16_t get16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t get32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void put16(uint8_t *data, uint16_t value)
{
    data[0] = value;
    data[1] = value >> 8;
}

static void put32(uint8_t *data, uint32_t value)
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

uint16_t miband_xiaomi_crc16_arc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;
    while (length--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

uint32_t miband_xiaomi_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    while (length--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
        }
    }
    return ~crc;
}

bool miband_xiaomi_l1_decode(const uint8_t *data, size_t length,
                             MiBandXiaomiL1 *packet)
{
    uint8_t type;
    uint16_t payload_len;

    if (!data || !packet || length < 8 || data[0] != 0xa5 || data[1] != 0xa5)
        return false;
    type = data[2] & 0x0f;
    if (type > MIBAND_XIAOMI_L1_DATA || (data[2] & 0xe0)) return false;
    payload_len = get16(data + 4);
    if ((size_t)payload_len + 8 != length ||
        get16(data + 6) != miband_xiaomi_crc16_arc(data + 8, payload_len))
        return false;
    packet->type = (MiBandXiaomiL1Type)type;
    packet->frx = !!(data[2] & 0x10);
    packet->sequence = data[3];
    packet->payload = data + 8;
    packet->payload_len = payload_len;
    return true;
}

size_t miband_xiaomi_l1_encode(MiBandXiaomiL1Type type, bool frx,
                               uint8_t sequence, const uint8_t *payload,
                               size_t payload_len, uint8_t *output,
                               size_t capacity)
{
    if (type > MIBAND_XIAOMI_L1_DATA || (!payload && payload_len) || payload_len > 0xffff ||
        capacity < payload_len + 8) return 0;
    output[0] = 0xa5; output[1] = 0xa5;
    output[2] = (uint8_t)type | (frx ? 0x10 : 0);
    output[3] = sequence;
    put16(output + 4, (uint16_t)payload_len);
    memcpy(output + 8, payload, payload_len);
    put16(output + 6, miband_xiaomi_crc16_arc(output + 8, payload_len));
    return payload_len + 8;
}

bool miband_xiaomi_l2_decode(const uint8_t *data, size_t length,
                             MiBandXiaomiL2 *packet)
{
    if (!data || !packet || length < 2 || data[0] < 1 || data[0] > 10 ||
        data[1] < 1 || data[1] > 3) return false;
    packet->channel = data[0]; packet->opcode = data[1];
    packet->payload = data + 2; packet->payload_len = length - 2;
    return true;
}

size_t miband_xiaomi_l2_encode(uint8_t channel, uint8_t opcode,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t *output, size_t capacity)
{
    if (channel < 1 || channel > 10 || opcode < 1 || opcode > 3 ||
        (!payload && payload_len) ||
        capacity < payload_len + 2) return 0;
    output[0] = channel; output[1] = opcode;
    memcpy(output + 2, payload, payload_len);
    return payload_len + 2;
}

bool miband_xiaomi_mass_decode(const uint8_t *data, size_t length,
                               MiBandXiaomiMass *packet)
{
    uint32_t payload_len, declared_crc;

    if (!data || !packet || length < 26) return false;
    payload_len = get32(data + 18);
    if ((size_t)payload_len + 26 != length) return false;
    declared_crc = get32(data + 22 + payload_len);
    if (declared_crc != miband_xiaomi_crc32(data, 22 + payload_len)) return false;
    packet->compression = data[0]; packet->type = data[1];
    memcpy(packet->md5, data + 2, sizeof(packet->md5));
    packet->remaining_length = payload_len;
    packet->payload = data + 22; packet->payload_len = payload_len;
    packet->crc32 = declared_crc;
    return true;
}

bool miband_xiaomi_mass_verify(const MiBandXiaomiMass *packet)
{
    uint8_t digest[16];
    if (!packet || packet->compression != 0 || packet->payload_len != packet->remaining_length)
        return packet && packet->compression != 0;
    miband_xiaomi_md5(packet->payload, packet->payload_len, digest);
    return !memcmp(digest, packet->md5, sizeof(digest));
}
