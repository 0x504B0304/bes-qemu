#ifndef MIBAND_XIAOMI_PROTOCOL_H
#define MIBAND_XIAOMI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum MiBandXiaomiL1Type {
    MIBAND_XIAOMI_L1_NAK = 0,
    MIBAND_XIAOMI_L1_ACK = 1,
    MIBAND_XIAOMI_L1_CMD = 2,
    MIBAND_XIAOMI_L1_DATA = 3,
} MiBandXiaomiL1Type;

typedef struct MiBandXiaomiL1 {
    MiBandXiaomiL1Type type;
    bool frx;
    uint8_t sequence;
    const uint8_t *payload;
    size_t payload_len;
} MiBandXiaomiL1;

typedef struct MiBandXiaomiL2 {
    uint8_t channel;
    uint8_t opcode;
    const uint8_t *payload;
    size_t payload_len;
} MiBandXiaomiL2;

typedef enum MiBandXiaomiMassType {
    MIBAND_XIAOMI_MASS_WATCHFACE = 16,
    MIBAND_XIAOMI_MASS_FIRMWARE = 32,
    MIBAND_XIAOMI_MASS_WATCHFACE_IMAGE = 48,
    MIBAND_XIAOMI_MASS_NOTIFICATION_ICON = 50,
    MIBAND_XIAOMI_MASS_MUSIC = 52,
    MIBAND_XIAOMI_MASS_WATCHFACE_FONT = 53,
    MIBAND_XIAOMI_MASS_THIRD_PARTY_APP = 64,
} MiBandXiaomiMassType;

typedef struct MiBandXiaomiMass {
    uint8_t compression;
    uint8_t type;
    uint8_t md5[16];
    uint32_t remaining_length;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t crc32;
} MiBandXiaomiMass;

uint16_t miband_xiaomi_crc16_arc(const uint8_t *data, size_t length);
uint32_t miband_xiaomi_crc32(const uint8_t *data, size_t length);
void miband_xiaomi_md5(const uint8_t *data, size_t length, uint8_t digest[16]);
bool miband_xiaomi_l1_decode(const uint8_t *data, size_t length,
                             MiBandXiaomiL1 *packet);
size_t miband_xiaomi_l1_encode(MiBandXiaomiL1Type type, bool frx,
                               uint8_t sequence, const uint8_t *payload,
                               size_t payload_len, uint8_t *output,
                               size_t capacity);
bool miband_xiaomi_l2_decode(const uint8_t *data, size_t length,
                             MiBandXiaomiL2 *packet);
size_t miband_xiaomi_l2_encode(uint8_t channel, uint8_t opcode,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t *output, size_t capacity);
bool miband_xiaomi_mass_decode(const uint8_t *data, size_t length,
                               MiBandXiaomiMass *packet);
bool miband_xiaomi_mass_verify(const MiBandXiaomiMass *packet);

#endif
