/* Explicit software adapter: modified runtime image, original file untouched. */
#include "qemu/osdep.h"
#include "bes_runtime_config.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "miband_audio_adapter.h"
#include "miband_binding_ui.h"

static bool digest_matches(const uint8_t *bytes, const char *digest)
{
    for (unsigned i = 0; i < 32; ++i) {
        char octet[3];
        g_snprintf(octet, sizeof(octet), "%02x", bytes[i]);
        if (memcmp(octet, digest + i * 2, 2)) return false;
    }
    return true;
}

void miband_audio_adapter_install(MiBandAudioAdapter *a, MiBandAudio *audio,
                                  MemoryRegion *sysmem, const char *ap,
                                  const char *shim, const char *firmware_id,
                                  bool skip_binding)
{
    g_autofree char *image = NULL, *code = NULL;
    gsize image_size = 0, code_size = 0;
    const MiBandAudioProfile *profile = NULL;
    for (unsigned i = 0; i < G_N_ELEMENTS(miband_audio_profiles); ++i)
        if (!strcmp(firmware_id, miband_audio_profiles[i].id))
            profile = &miband_audio_profiles[i];
    if (!profile) {
        error_report("audio adapter: no profile for %s", firmware_id); exit(1);
    }
    if (!g_file_get_contents(ap, &image, &image_size, NULL) ||
        !shim || !g_file_get_contents(shim, &code, &code_size, NULL)) {
        error_report("audio adapter: cannot read AP/shim"); exit(1);
    }
    g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)image, image_size);
    if (strcmp(hash, profile->ap_sha256) || code_size < 40 || code_size > 65536 ||
        ldl_le_p(code) != 0x4150434d) {
        error_report("audio adapter: AP hash or shim ABI mismatch"); exit(1);
    }
    uint32_t abi = ldl_le_p(code + 4);
    unsigned header_size = abi == 3 ? 104 : 40;
    if ((abi != 3 && !(abi == 2 && profile->legacy_abi2)) ||
        (abi == 3 && (code_size < header_size ||
                     !digest_matches((const uint8_t *)code + 40, profile->ap_sha256) ||
                     !digest_matches((const uint8_t *)code + 72, profile->config_sha256)))) {
        error_report("audio adapter: shim firmware/configuration binding mismatch"); exit(1);
    }
    if (skip_binding) miband_binding_ui_patch(image, image_size, firmware_id);
    for (unsigned i = 0; i < G_N_ELEMENTS(profile->hooks); ++i) {
        const MiBandAudioHook *hook = &profile->hooks[i];
        uint32_t target = ldl_le_p(code + hook->entry);
        size_t offset = hook->address - profile->flash_base;
        if (offset + 8 > image_size || memcmp(image + offset, hook->expected, 8) ||
            !(target & 1) || target < profile->ram_base + header_size + 1 ||
            target >= profile->ram_base + code_size) {
            error_report("audio adapter: hook %u validation failed", i); exit(1);
        }
        /* LDR.W PC,[PC,#0], then Thumb target; all sites are word aligned. */
        memcpy(image + offset, "\xdf\xf8\x00\xf0", 4);
        stl_le_p(image + offset + 4, target);
    }
    a->audio = audio;
    a->profile = profile;
    memory_region_init_ram(&a->ram, NULL, "audio-adapter-reserved-ram", 65536, &error_fatal);
    memory_region_add_subregion(sysmem, profile->ram_base, &a->ram);
    g_autofree uint8_t *reserved = g_malloc0(65536);
    memcpy(reserved, code, code_size);
    rom_add_blob_fixed("audio-adapter-shim", reserved, 65536, profile->ram_base);
    rom_add_blob_fixed("audio-adapter-runtime-ap", image, image_size, profile->flash_base);
    error_report("audio adapter: experimental device replacement installed; guest lifecycle not yet certified");
}

void miband_audio_adapter_reset(MiBandAudioAdapter *a)
{
    if (!a->audio) return;
    miband_audio_stop(a->audio);
    a->pending = false; a->cursor = a->status = 0;
}

bool miband_audio_adapter_read(MiBandAudioAdapter *a, hwaddr address,
                              unsigned size, uint64_t *value)
{
    if (!a->audio || address < a->profile->mmio_base || address >= a->profile->mmio_base + 4096) return false;
    *value = 0;
    if (size != 4 || (address & 3)) return true;
    unsigned offset = address - a->profile->mmio_base;
    if (offset == 4) {
        if (!a->pending) {
            size_t valid;
            MiBandPcmResult r = miband_pcm_read(&a->audio->pcm,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), a->frame, &valid);
            a->status = r == MIBAND_PCM_OK ? 1 : r == MIBAND_PCM_EOF ? 2 :
                        r == MIBAND_PCM_ERROR ? 3 : 0;
            a->pending = r == MIBAND_PCM_OK;
            a->cursor = 0;
        }
        *value = a->status;
    } else if (offset == 8 && a->pending) {
        *value = ldl_le_p(a->frame + a->cursor);
        a->cursor += 4;
        if (a->cursor == MIBAND_PCM_FRAME) { a->pending = false; a->status = 0; }
    }
    return true;
}

bool miband_audio_adapter_write(MiBandAudioAdapter *a, hwaddr address,
                               unsigned size, uint64_t value)
{
    if (!a->audio || address < a->profile->mmio_base || address >= a->profile->mmio_base + 4096) return false;
    if (size == 4 && address == a->profile->mmio_base) {
        miband_audio_adapter_reset(a);
        if (value == 1 && !miband_audio_start(a->audio, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))
            a->status = 3;
    }
    return true;
}

/* The shim header and guest counter layout belong to this firmware ABI. */
void miband_audio_adapter_append_stats(MiBandAudioAdapter *a, GString *json)
{
    if (a->audio) {
        const uint8_t *ram = memory_region_get_ram_ptr(&a->ram);
        uint32_t addr = ldl_le_p(ram + 24);
        static const char * const fields[] = {"generation", "entered", "submitted",
            "rejected", "wakeups", "ring", "occupancy", "consumed_snapshot",
            "error", "joined", "eof", "codec_state", "codec_frames", "codec_bytes",
            "codec_error", "codec_joined"};
        uint32_t base = a->profile->ram_base;
        if (addr >= base && addr <= base + 65536 - sizeof(fields)/sizeof(fields[0])*4) {
            g_string_append(json, ",\"guest\":{");
            for (unsigned i = 0; i < G_N_ELEMENTS(fields); ++i)
                g_string_append_printf(json, "%s\"%s\":%u", i ? "," : "", fields[i],
                                       ldl_le_p(ram + addr - base + i*4));
            g_string_append(json, "}");
        }
    }
}
