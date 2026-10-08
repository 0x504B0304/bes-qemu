#include <assert.h>
#include <string.h>
#include "miband_xiaomi_protocol.h"

static void put32le(unsigned char *p, unsigned value)
{
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}

int main(void)
{
    unsigned char encoded[128], mass[64];
    const unsigned char payload[] = { 1, 2, 3, 0xa5 };
    MiBandXiaomiL1 l1;
    MiBandXiaomiL2 l2;
    MiBandXiaomiMass decoded;
    size_t length;

    length = miband_xiaomi_l1_encode(MIBAND_XIAOMI_L1_DATA, true, 7,
                                     payload, sizeof(payload), encoded,
                                     sizeof(encoded));
    assert(length == 12 && miband_xiaomi_l1_decode(encoded, length, &l1));
    assert(l1.frx && l1.sequence == 7 && l1.payload_len == sizeof(payload));
    assert(!miband_xiaomi_l1_decode(encoded, length - 1, &l1));

    length = miband_xiaomi_l2_encode(2, 1, payload, sizeof(payload), encoded,
                                     sizeof(encoded));
    assert(length == 6 && miband_xiaomi_l2_decode(encoded, length, &l2));
    assert(l2.channel == 2 && l2.opcode == 1 && l2.payload_len == sizeof(payload));

    memset(mass, 0, sizeof(mass));
    mass[0] = 0; mass[1] = MIBAND_XIAOMI_MASS_WATCHFACE;
    put32le(mass + 18, 4);
    memcpy(mass + 22, payload, sizeof(payload));
    miband_xiaomi_md5(payload, sizeof(payload), mass + 2);
    put32le(mass + 26, miband_xiaomi_crc32(mass, 26));
    assert(miband_xiaomi_mass_decode(mass, 30, &decoded));
    assert(decoded.type == MIBAND_XIAOMI_MASS_WATCHFACE &&
           decoded.payload_len == sizeof(payload) &&
           miband_xiaomi_mass_verify(&decoded));
    mass[2] ^= 1;
    put32le(mass + 26, miband_xiaomi_crc32(mass, 26));
    assert(miband_xiaomi_mass_decode(mass, 30, &decoded));
    assert(!miband_xiaomi_mass_verify(&decoded));
    mass[29] ^= 1;
    assert(!miband_xiaomi_mass_decode(mass, 30, &decoded));
    return 0;
}
