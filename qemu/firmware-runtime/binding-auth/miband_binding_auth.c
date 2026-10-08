#include "qemu/osdep.h"
#include "bes_runtime_config.h"
#include "qemu/error-report.h"
#include "miband_binding_auth.h"

static const MiBandBindingAuthProfile *binding_profile(const char *firmware_id)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(miband_binding_auth_profiles); ++i)
        if (!strcmp(firmware_id, miband_binding_auth_profiles[i].id))
            return &miband_binding_auth_profiles[i];
    error_report("binding auth adapter: unavailable for %s", firmware_id);
    exit(1);
}

void miband_binding_auth_patch(char *image, size_t size, const char *firmware_id)
{
    const MiBandBindingAuthProfile *profile = binding_profile(firmware_id);
    g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                        (guchar *)image, size);
    if (strcmp(hash, profile->ap_sha256)) {
        error_report("binding auth adapter: AP hash mismatch");
        exit(1);
    }
    for (unsigned i = 0; i < profile->patch_count; ++i) {
        const MiBandBindingAuthPatch *patch = &profile->patches[i];
        size_t offset = patch->address - profile->flash_base;
        size_t guard_offset = patch->guard_address - profile->flash_base;
        if (offset > size || guard_offset > size || patch->patch_size > size - offset ||
            patch->guard_size > size - guard_offset ||
            memcmp(image + guard_offset, patch->guard, patch->guard_size) ||
            memcmp(image + offset, patch->expected, patch->patch_size)) {
            error_report("binding auth adapter: instruction guard mismatch at %#x",
                         patch->address);
            exit(1);
        }
    }
    for (unsigned i = 0; i < profile->patch_count; ++i) {
        const MiBandBindingAuthPatch *patch = &profile->patches[i];
        memcpy(image + patch->address - profile->flash_base,
               patch->replacement, patch->patch_size);
    }
    error_report("binding auth adapter: %s applied (%u filter patch%s)",
                 profile->id, profile->patch_count, profile->patch_count == 1 ? "" : "es");
}
