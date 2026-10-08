#ifndef MIBAND_XIAOMI_PB_H
#define MIBAND_XIAOMI_PB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum MiBandXiaomiPbRequestKind {
    MIBAND_XIAOMI_PB_NONE = 0,
    MIBAND_XIAOMI_PB_MASS_PREPARE,
    MIBAND_XIAOMI_PB_WATCHFACE_PREPARE,
    MIBAND_XIAOMI_PB_APP_PREPARE,
} MiBandXiaomiPbRequestKind;

typedef struct MiBandXiaomiPbRequest {
    MiBandXiaomiPbRequestKind kind;
    uint32_t data_type;
    uint8_t data_id[16];
    size_t data_id_len;
    uint32_t data_length;
    char id[64];
    char package_name[128];
    uint32_t version_code;
} MiBandXiaomiPbRequest;

bool miband_xiaomi_pb_decode_prepare(const uint8_t *data, size_t length,
                                     MiBandXiaomiPbRequest *request);

/* Encode the protocol's PrepareResponse/PrepareReply/InstallResponse. */
size_t miband_xiaomi_pb_encode_prepare_response(
    const MiBandXiaomiPbRequest *request, uint32_t expected_slice_length,
    uint32_t remained_data_length, uint8_t *output, size_t capacity);

size_t miband_xiaomi_pb_encode_install_result(
    const MiBandXiaomiPbRequest *request, uint8_t *output, size_t capacity);

#endif
