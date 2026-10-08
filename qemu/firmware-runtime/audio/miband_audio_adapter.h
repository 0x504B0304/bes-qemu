#ifndef MIBAND_AUDIO_ADAPTER_H
#define MIBAND_AUDIO_ADAPTER_H
#include "miband_audio.h"
#include "exec/memory.h"

typedef struct MiBandAudioHook {
    uint32_t address, entry;
    uint8_t expected[8];
} MiBandAudioHook;
typedef struct MiBandAudioProfile {
    const char *id, *ap_sha256, *config_sha256;
    uint32_t flash_base, ram_base, mmio_base;
    bool legacy_abi2;
    MiBandAudioHook hooks[7];
} MiBandAudioProfile;
typedef struct MiBandAudioAdapter {
    MemoryRegion ram;
    MiBandAudio *audio;
    const MiBandAudioProfile *profile;
    uint8_t frame[MIBAND_PCM_FRAME];
    unsigned cursor;
    bool pending;
    uint32_t status;
} MiBandAudioAdapter;
void miband_audio_adapter_install(MiBandAudioAdapter *a, MiBandAudio *audio,
                                  MemoryRegion *sysmem, const char *ap,
                                  const char *shim, const char *firmware_id,
                                  bool skip_binding);
void miband_audio_adapter_reset(MiBandAudioAdapter *a);
bool miband_audio_adapter_read(MiBandAudioAdapter *a, hwaddr address,
                              unsigned size, uint64_t *value);
bool miband_audio_adapter_write(MiBandAudioAdapter *a, hwaddr address,
                               unsigned size, uint64_t value);
void miband_audio_adapter_append_stats(MiBandAudioAdapter *a, GString *json);
#endif
