/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "miband_pcm.h"
#include <string.h>
#include <limits.h>

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void miband_pcm_init(MiBandPcm *p, MiBandPcmSource source)
{
    memset(p, 0, sizeof(*p));
    p->source = source;
    p->connected = true;
}

bool miband_pcm_open_wav(MiBandPcm *p, const char *path)
{
    uint8_t h[16];
    bool fmt = false, data = false;
    uint32_t remaining;
    long offset = 0, length;
    uint32_t bytes = 0;
    FILE *f;
    if (p->active || p->source != MIBAND_PCM_FILE) return false;
    f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 12 ||
        fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12 ||
        memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4) ||
        le32(h + 4) < 4 || (uint64_t)le32(h + 4) + 8 > (uint64_t)length) goto bad;
    remaining = le32(h + 4) - 4;
    while (remaining >= 8) {
        if (fread(h, 1, 8, f) != 8) goto bad;
        uint32_t n = le32(h + 4);
        uint64_t padded = (uint64_t)n + (n & 1);
        remaining -= 8;
        if (padded > remaining || padded > LONG_MAX) goto bad;
        long start = ftell(f);
        if (!memcmp(h, "fmt ", 4)) {
            if (fmt || n < 16 || fread(h, 1, 16, f) != 16 ||
                h[0] != 1 || h[1] || h[2] != 1 || h[3] ||
                le32(h + 4) != MIBAND_PCM_RATE || le32(h + 8) != 32000 ||
                h[12] != 2 || h[13] || h[14] != 16 || h[15]) goto bad;
            fmt = true;
        } else if (!memcmp(h, "data", 4)) {
            if (data || (n & 1)) goto bad;
            data = true; offset = start; bytes = n;
        }
        if (fseek(f, start + (long)padded, SEEK_SET)) goto bad;
        remaining -= (uint32_t)padded;
    }
    if (!fmt || !data || remaining) goto bad;
    if (p->file) fclose(p->file);
    p->file = f; p->data_offset = offset; p->data_bytes = bytes;
    return true;
bad:
    fclose(f);
    return false;
}

bool miband_pcm_start(MiBandPcm *p, int64_t now_ns)
{
    if (p->active) return false;
    if (p->source == MIBAND_PCM_FILE &&
        (!p->file || fseek(p->file, p->data_offset, SEEK_SET))) return false;
    p->head = p->count = 0;
    p->remaining = p->data_bytes;
    p->eof = false;
    p->next_ns = now_ns + 20000000;
    p->stats.generation++;
    p->active = true;
    return true;
}

void miband_pcm_stop(MiBandPcm *p)
{
    p->active = false;
    p->head = p->count = 0;
}

void miband_pcm_destroy(MiBandPcm *p)
{
    miband_pcm_stop(p);
    if (p->file) fclose(p->file);
    p->file = NULL;
}

void miband_pcm_connected(MiBandPcm *p, bool connected)
{
    if (p->connected && !connected) p->stats.disconnects++;
    p->connected = connected;
}

bool miband_pcm_push(MiBandPcm *p, const uint8_t frame[MIBAND_PCM_FRAME])
{
    if (!p->active || !p->connected || p->source != MIBAND_PCM_MIC) return false;
    if (p->count == MIBAND_PCM_QUEUE_FRAMES) {
        p->head = (p->head + 1) % MIBAND_PCM_QUEUE_FRAMES;
        p->count--;
        p->stats.dropped_bytes += MIBAND_PCM_FRAME;
    }
    memcpy(p->queue[(p->head + p->count) % MIBAND_PCM_QUEUE_FRAMES], frame, MIBAND_PCM_FRAME);
    p->count++;
    p->stats.input_bytes += MIBAND_PCM_FRAME;
    return true;
}

MiBandPcmResult miband_pcm_read(MiBandPcm *p, int64_t now_ns,
                               uint8_t frame[MIBAND_PCM_FRAME], size_t *valid)
{
    *valid = 0;
    if (!p->active) return MIBAND_PCM_STOPPED;
    if (p->eof) return MIBAND_PCM_EOF;
    if (now_ns < p->next_ns) return MIBAND_PCM_AGAIN;
    memset(frame, 0, MIBAND_PCM_FRAME);
    if (p->source == MIBAND_PCM_FILE) {
        size_t n = p->remaining < MIBAND_PCM_FRAME ? p->remaining : MIBAND_PCM_FRAME;
        if (!n) { p->eof = true; return MIBAND_PCM_EOF; }
        if (fread(frame, 1, n, p->file) != n) return MIBAND_PCM_ERROR;
        p->remaining -= n;
        *valid = n;
        p->stats.input_bytes += n;
    } else if (p->count) {
        memcpy(frame, p->queue[p->head], MIBAND_PCM_FRAME);
        p->head = (p->head + 1) % MIBAND_PCM_QUEUE_FRAMES;
        p->count--;
        *valid = MIBAND_PCM_FRAME;
    } else {
        p->stats.silence_bytes += MIBAND_PCM_FRAME;
        *valid = MIBAND_PCM_FRAME;
    }
    p->next_ns += 20000000;
    p->stats.output_bytes += *valid;
    return MIBAND_PCM_OK;
}
