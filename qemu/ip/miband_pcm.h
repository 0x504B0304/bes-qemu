/* Shared S16LE/16kHz/mono endpoint. Caller serializes all operations. */
#ifndef MIBAND_PCM_H
#define MIBAND_PCM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define MIBAND_PCM_RATE 16000
#define MIBAND_PCM_FRAME 640
#define MIBAND_PCM_QUEUE_FRAMES 10
typedef enum { MIBAND_PCM_MIC, MIBAND_PCM_FILE } MiBandPcmSource;
typedef enum { MIBAND_PCM_OK, MIBAND_PCM_AGAIN, MIBAND_PCM_EOF,
               MIBAND_PCM_STOPPED, MIBAND_PCM_ERROR } MiBandPcmResult;
typedef struct MiBandPcmStats {
    uint64_t input_bytes, output_bytes, dropped_bytes, silence_bytes;
    uint64_t disconnects, generation;
} MiBandPcmStats;
typedef struct MiBandPcm {
    MiBandPcmSource source;
    bool active, connected, eof;
    uint8_t queue[MIBAND_PCM_QUEUE_FRAMES][MIBAND_PCM_FRAME];
    unsigned head, count;
    FILE *file;
    long data_offset;
    uint32_t data_bytes, remaining;
    int64_t next_ns;
    MiBandPcmStats stats;
} MiBandPcm;
void miband_pcm_init(MiBandPcm *p, MiBandPcmSource source);
/* WAV: strict RIFF PCM, S16LE 16kHz mono; does not load whole file. */
bool miband_pcm_open_wav(MiBandPcm *p, const char *path);
bool miband_pcm_start(MiBandPcm *p, int64_t now_ns);
void miband_pcm_stop(MiBandPcm *p);
void miband_pcm_destroy(MiBandPcm *p);
void miband_pcm_connected(MiBandPcm *p, bool connected);
/* Mic only; complete 20ms frames. Overflow discards oldest full frame. */
bool miband_pcm_push(MiBandPcm *p, const uint8_t frame[MIBAND_PCM_FRAME]);
/* At most one frame per call. Final file frame is padded, valid excludes pad.
 * EOF is returned only after all valid data has been delivered. Virtual time
 * belongs to the consumer; falling behind never skips file samples. */
MiBandPcmResult miband_pcm_read(MiBandPcm *p, int64_t now_ns,
                               uint8_t frame[MIBAND_PCM_FRAME], size_t *valid);
#endif
