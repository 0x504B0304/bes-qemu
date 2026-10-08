#include "miband_pcm.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    MiBandPcm p;
    uint8_t frame[MIBAND_PCM_FRAME], out[MIBAND_PCM_FRAME];
    size_t valid;
    miband_pcm_init(&p, MIBAND_PCM_MIC);
    assert(miband_pcm_start(&p, 0));
    assert(!miband_pcm_start(&p, 0));
    for (unsigned i = 0; i < 11; ++i) {
        memset(frame, i, sizeof(frame));
        assert(miband_pcm_push(&p, frame));
    }
    assert(p.count == 10 && p.stats.dropped_bytes == 640);
    assert(miband_pcm_read(&p, 19999999, out, &valid) == MIBAND_PCM_AGAIN);
    for (unsigned i = 1; i <= 10; ++i) {
        assert(miband_pcm_read(&p, i * 20000000LL, out, &valid) == MIBAND_PCM_OK);
        assert(valid == 640 && out[0] == i && out[639] == i);
    }
    miband_pcm_connected(&p, false);
    assert(!miband_pcm_push(&p, frame));
    assert(miband_pcm_read(&p, 220000000, out, &valid) == MIBAND_PCM_OK);
    assert(p.stats.disconnects == 1 && p.stats.silence_bytes == 640);
    assert(out[0] == 0 && out[639] == 0);
    miband_pcm_stop(&p);
    assert(miband_pcm_read(&p, 999999999, out, &valid) == MIBAND_PCM_STOPPED);
    assert(miband_pcm_start(&p, 1000000000));
    assert(p.count == 0 && p.stats.generation == 2);
    miband_pcm_destroy(&p);
    assert(argc == 3);
    miband_pcm_init(&p, MIBAND_PCM_FILE);
    assert(!miband_pcm_open_wav(&p, argv[2]));
    assert(miband_pcm_open_wav(&p, argv[1]));
    assert(miband_pcm_start(&p, 0));
    /* Slow host: 1s elapsed, but file data must remain contiguous. */
    for (unsigned i = 0; i < 3; ++i) {
        assert(miband_pcm_read(&p, 1000000000, out, &valid) == MIBAND_PCM_OK);
        assert(valid == (i == 2 ? 2 : 640));
        for (unsigned j = 0; j < valid; ++j) assert(out[j] == (uint8_t)(i * 640 + j));
    }
    assert(out[2] == 0 && out[639] == 0);
    assert(miband_pcm_read(&p, 1000000000, out, &valid) == MIBAND_PCM_EOF);
    assert(p.stats.output_bytes == 1282 && p.stats.dropped_bytes == 0);
    miband_pcm_stop(&p);
    assert(miband_pcm_start(&p, 2000000000));
    assert(miband_pcm_read(&p, 2010000000, out, &valid) == MIBAND_PCM_AGAIN);
    assert(miband_pcm_read(&p, 2020000000, out, &valid) == MIBAND_PCM_OK);
    assert(out[0] == 0 && out[1] == 1);
    miband_pcm_destroy(&p);
    puts("PCM queue, pacing, overflow, underrun, disconnect, EOF, restart: PASS");
    return 0;
}
