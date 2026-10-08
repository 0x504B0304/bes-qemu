#ifndef MIBAND_BINDING_UI_H
#define MIBAND_BINDING_UI_H
/* Validates the ORIGINAL image before any adapter changes. */
typedef struct MiBandBindingUiPatch {
    uint32_t address, guard_address;
    unsigned patch_size, guard_size;
    uint8_t expected[8], replacement[8], guard[32];
} MiBandBindingUiPatch;
typedef struct MiBandBindingUiProfile {
    const char *id, *ap_sha256, *config_sha256;
    uint32_t flash_base;
    unsigned patch_count;
    MiBandBindingUiPatch patches[2];
} MiBandBindingUiProfile;
void miband_binding_ui_patch(char *image, size_t size, const char *firmware_id);
void miband_binding_ui_patch_after_auth(char *image, size_t size,
                                         const char *firmware_id);
void miband_binding_ui_install(const char *ap, const char *firmware_id,
                               bool apply_ui, bool bypass_binding_auth);
void miband_binding_ui_install_at(const char *ap, const char *firmware_id,
                                  uint32_t flash_base);
#endif
