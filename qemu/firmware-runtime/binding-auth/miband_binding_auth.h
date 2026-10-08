#ifndef MIBAND_BINDING_AUTH_H
#define MIBAND_BINDING_AUTH_H

typedef struct MiBandBindingAuthPatch {
    uint32_t address, guard_address;
    unsigned patch_size, guard_size;
    uint8_t expected[8], replacement[8], guard[32];
} MiBandBindingAuthPatch;

typedef struct MiBandBindingAuthProfile {
    const char *id, *ap_sha256, *config_sha256;
    uint32_t flash_base;
    unsigned patch_count;
    MiBandBindingAuthPatch patches[8];
} MiBandBindingAuthProfile;

void miband_binding_auth_patch(char *image, size_t size, const char *firmware_id);

#endif
