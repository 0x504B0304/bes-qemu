#include <assert.h>
#include <string.h>
#include "miband_xiaomi_mass.h"

static void put32le(unsigned char *p, unsigned value)
{
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}

int main(void)
{
    unsigned char mass[64], first[32], second[32];
    const MiBandXiaomiMass *result;
    MiBandXiaomiMassReceiver *receiver = miband_xiaomi_mass_receiver_new();
    assert(receiver);
    memset(mass, 0, sizeof(mass));
    mass[1] = MIBAND_XIAOMI_MASS_THIRD_PARTY_APP;
    put32le(mass + 18, 4);
    memcpy(mass + 22, "test", 4);
    miband_xiaomi_md5(mass + 22, 4, mass + 2);
    put32le(mass + 26, miband_xiaomi_crc32(mass, 26));
    first[0] = 2; first[1] = 0; first[2] = 1; first[3] = 0;
    memcpy(first + 4, mass, 15);
    second[0] = 2; second[1] = 0; second[2] = 2; second[3] = 0;
    memcpy(second + 4, mass + 15, 15);
    assert(miband_xiaomi_mass_receiver_push(receiver, first, 19));
    assert(!miband_xiaomi_mass_receiver_complete(receiver));
    assert(miband_xiaomi_mass_receiver_push(receiver, second, 19));
    assert(miband_xiaomi_mass_receiver_complete(receiver));
    result = miband_xiaomi_mass_receiver_result(receiver);
    assert(result && result->type == MIBAND_XIAOMI_MASS_THIRD_PARTY_APP &&
           result->payload_len == 4 && !memcmp(result->payload, "test", 4));
    miband_xiaomi_mass_receiver_reset(receiver);
    assert(!miband_xiaomi_mass_receiver_complete(receiver));
    miband_xiaomi_mass_receiver_free(receiver);
    return 0;
}
