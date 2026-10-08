#ifndef MIBAND_AUDIO_H
#define MIBAND_AUDIO_H
#include "miband_pcm.h"
#include "audio/audio.h"
#include "sysemu/runstate.h"
#include "qemu/timer.h"
typedef struct MiBandAudio {
    MiBandPcm pcm;
    QEMUSoundCard card;
    SWVoiceIn *voice;
    uint8_t partial[MIBAND_PCM_FRAME];
    size_t partial_bytes;
    bool registered;
    VMChangeStateEntry *vm_change;
    bool browser;
    char browser_session[33];
    uint64_t browser_sequence, browser_received, browser_rejected;
    QEMUTimer *browser_lease;
} MiBandAudio;
bool miband_audio_init(MiBandAudio *a, const char *source, const char *path,
                       const char *audiodev, Error **errp);
bool miband_audio_start(MiBandAudio *a, int64_t now_ns);
void miband_audio_stop(MiBandAudio *a);
void miband_audio_destroy(MiBandAudio *a);
void miband_audio_browser_command(MiBandAudio *a, const char *value, Error **errp);
#endif
