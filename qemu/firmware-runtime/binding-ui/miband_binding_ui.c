#include "qemu/osdep.h"
#include "bes_runtime_config.h"
#include "qemu/error-report.h"
#include "hw/loader.h"
#include "miband_binding_ui.h"
#include "miband_binding_auth.h"

static const MiBandBindingUiProfile *binding_profile(const char *firmware_id)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(miband_binding_ui_profiles); ++i)
        if (!strcmp(firmware_id, miband_binding_ui_profiles[i].id))
            return &miband_binding_ui_profiles[i];
    error_report("binding UI adapter: unavailable for %s", firmware_id);
    exit(1);
}

static void binding_ui_patch_image(char *image, size_t size,
                                   const char *firmware_id, bool verify_hash)
{
    const MiBandBindingUiProfile *p = binding_profile(firmware_id);
    g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                       (guchar *)image, size);
    if (verify_hash && strcmp(hash, p->ap_sha256)) {
        error_report("binding UI adapter: AP hash/instruction mismatch"); exit(1);
    }
    /* Validate the entire set before modifying any original instructions. */
    for (unsigned i = 0; i < p->patch_count; i++) {
        const MiBandBindingUiPatch *patch = &p->patches[i];
        size_t offset = patch->address - p->flash_base;
        size_t guard_offset = patch->guard_address - p->flash_base;
        if (offset > size || guard_offset > size ||
            patch->patch_size > size - offset || patch->guard_size > size - guard_offset ||
            memcmp(image + guard_offset, patch->guard, patch->guard_size) ||
            memcmp(image + offset, patch->expected, patch->patch_size)) {
            error_report("binding UI adapter: AP hash/instruction mismatch"); exit(1);
        }
    }
    for (unsigned i = 0; i < p->patch_count; i++) {
        const MiBandBindingUiPatch *patch = &p->patches[i];
        memcpy(image + patch->address - p->flash_base, patch->replacement, patch->patch_size);
        error_report("binding UI adapter: %s site=%#x config=%s; no Bluetooth bond",
                     p->id, patch->address, p->config_sha256);
    }
}

void miband_binding_ui_patch(char *image, size_t size, const char *firmware_id)
{ binding_ui_patch_image(image, size, firmware_id, true); }

void miband_binding_ui_patch_after_auth(char *image, size_t size,
                                        const char *firmware_id)
{ binding_ui_patch_image(image, size, firmware_id, false); }

void miband_binding_ui_install(const char *ap, const char *firmware_id,
                               bool apply_ui, bool bypass_binding_auth)
{
    g_autofree char *image = NULL;
    gsize size = 0;
    if (!g_file_get_contents(ap, &image, &size, NULL)) {
        error_report("binding UI adapter: cannot read AP"); exit(1);
    }
    if (bypass_binding_auth) {
        miband_binding_auth_patch(image, size, firmware_id);
        if (apply_ui)
            miband_binding_ui_patch_after_auth(image, size, firmware_id);
    } else if (apply_ui) {
        miband_binding_ui_patch(image, size, firmware_id);
    }
    rom_add_blob_fixed("binding-ui-runtime-ap", image, size, binding_profile(firmware_id)->flash_base);
}

void miband_binding_ui_install_at(const char *ap, const char *firmware_id,
                                  uint32_t flash_base)
{
    if (!firmware_id || binding_profile(firmware_id)->flash_base != flash_base) {
        error_report("binding UI adapter: firmware/mapping mismatch"); exit(1);
    }
    miband_binding_ui_install(ap, firmware_id, true, false);
}
