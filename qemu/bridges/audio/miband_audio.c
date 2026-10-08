/* The QEMU audio core performs host format/rate conversion. No guest memory
 * or firmware addresses belong in this layer. Audio callbacks hold the BQL. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "miband_audio.h"

static void browser_expire(void *opaque)
{
    MiBandAudio *a = opaque;
    a->browser_session[0] = 0;
    a->pcm.head = a->pcm.count = 0;
    miband_pcm_connected(&a->pcm, false);
}

/* QOM setters run under the BQL, like the capture callback. This endpoint
 * never starts the guest recorder or invokes guest functions. A two-second
 * lease bounds tab/network failure; generation checks exclude stale capture. */
void miband_audio_browser_command(MiBandAudio *a, const char *value, Error **errp)
{
    if (!a->browser || !a->browser_lease) {
        error_setg(errp, "restart with audio-source=browser and audio-mode=firmware");
        return;
    }
    if (strlen(value) > 6500) {
        error_setg(errp, "browser audio batch too large"); return;
    }
    g_auto(GStrv) parts = g_strsplit(value, ":", -1);
    size_t n = g_strv_length(parts);
    if (n < 2 || strlen(parts[1]) != 32 ||
        strspn(parts[1], "0123456789abcdef") != 32) {
        error_setg(errp, "invalid browser audio session"); return;
    }
    if (n == 2 && !strcmp(parts[0], "begin")) {
        if (a->browser_session[0]) {
            error_setg(errp, "microphone already owned by another session"); return;
        }
        memcpy(a->browser_session, parts[1], sizeof(a->browser_session));
        a->browser_sequence = 0;
        a->pcm.head = a->pcm.count = 0;
        miband_pcm_connected(&a->pcm, true);
    } else {
        if (strcmp(parts[1], a->browser_session)) {
            error_setg(errp, "browser audio lease expired or wrong session"); return;
        }
        if (n == 2 && !strcmp(parts[0], "end")) {
            timer_del(a->browser_lease);
            browser_expire(a);
            return;
        }
        if (n != 5 || strcmp(parts[0], "pcm") || !*parts[2] || !*parts[3] ||
            strlen(parts[2]) > 16 || strlen(parts[3]) > 16 ||
            strspn(parts[2], "0123456789") != strlen(parts[2]) ||
            strspn(parts[3], "0123456789") != strlen(parts[3])) {
            error_setg(errp, "invalid browser audio command"); return;
        }
        uint64_t seq = g_ascii_strtoull(parts[2], NULL, 10);
        uint64_t generation = g_ascii_strtoull(parts[3], NULL, 10);
        size_t len = strlen(parts[4]);
        if (seq != a->browser_sequence || !len || len > 6400 || len % 1280 ||
            strspn(parts[4], "0123456789abcdef") != len) {
            error_setg(errp, "expected sequential batches of 1..5 complete PCM frames"); return;
        }
        a->browser_sequence++;
        a->browser_received += len / 2;
        for (size_t pos = 0; pos < len; pos += MIBAND_PCM_FRAME * 2) {
            uint8_t frame[MIBAND_PCM_FRAME];
            for (unsigned i = 0; i < sizeof(frame); ++i) {
                frame[i] = (g_ascii_xdigit_value(parts[4][pos + i*2]) << 4) |
                            g_ascii_xdigit_value(parts[4][pos + i*2 + 1]);
            }
            if (generation != a->pcm.stats.generation || !runstate_is_running() ||
                !miband_pcm_push(&a->pcm, frame)) {
                a->browser_rejected += sizeof(frame);
            }
        }
    }
    timer_mod(a->browser_lease, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
}

static void capture(void *opaque, int available)
{
    MiBandAudio *a = opaque;
    while (available > 0 && a->pcm.active) {
        size_t n = MIN((size_t)available, MIBAND_PCM_FRAME - a->partial_bytes);
        n = AUD_read(a->voice, a->partial + a->partial_bytes, n);
        if (!n) break;
        available -= n;
        a->partial_bytes += n;
        if (a->partial_bytes == MIBAND_PCM_FRAME) {
            miband_pcm_push(&a->pcm, a->partial);
            a->partial_bytes = 0;
        }
    }
}

static void vm_change(void *opaque, bool running, RunState state)
{
    MiBandAudio *a = opaque;
    if (a->browser && !running) {
        a->pcm.head = a->pcm.count = 0;
        if (a->pcm.active) a->pcm.stats.generation++;
    }
    /* Exit notifiers run AFTER QEMU audio_cleanup: release voices before
     * that boundary, otherwise a late AUD_close_in dereferences freed state. */
    if (state == RUN_STATE_SHUTDOWN) miband_audio_destroy(a);
}

bool miband_audio_init(MiBandAudio *a, const char *source, const char *path,
                       const char *audiodev, Error **errp)
{
    memset(a, 0, sizeof(*a));
    if (!strcmp(source, "browser")) {
        a->browser = true;
        miband_pcm_init(&a->pcm, MIBAND_PCM_MIC);
        miband_pcm_connected(&a->pcm, false);
        a->browser_lease = timer_new_ms(QEMU_CLOCK_REALTIME, browser_expire, a);
        a->vm_change = qemu_add_vm_change_state_handler(vm_change, a);
        return true;
    }
    if (!strcmp(source, "file")) {
        miband_pcm_init(&a->pcm, MIBAND_PCM_FILE);
        if (!path || !miband_pcm_open_wav(&a->pcm, path)) {
            error_setg(errp, "audio-file must be a RIFF PCM S16LE 16000Hz mono WAV");
            return false;
        }
        a->vm_change = qemu_add_vm_change_state_handler(vm_change, a);
        return true;
    }
    if (strcmp(source, "mic") || !audiodev || !*audiodev) {
        error_setg(errp, "mic source requires an explicit standard QEMU audiodev ID");
        return false;
    }
    miband_pcm_init(&a->pcm, MIBAND_PCM_MIC);
    a->card.state = audio_state_by_name(audiodev, errp);
    if (!a->card.state || !AUD_register_card("best1503-capture", &a->card, errp)) return false;
    a->registered = true;
    struct audsettings settings = { .freq = MIBAND_PCM_RATE, .nchannels = 1,
                                   .fmt = AUDIO_FORMAT_S16, .endianness = 0 };
    a->voice = AUD_open_in(&a->card, NULL, "pcm-input", a, capture, &settings);
    if (!a->voice) {
        error_setg(errp, "cannot open host microphone input");
        miband_audio_destroy(a);
        return false;
    }
    a->vm_change = qemu_add_vm_change_state_handler(vm_change, a);
    return true;
}

bool miband_audio_start(MiBandAudio *a, int64_t now_ns)
{
    if (!miband_pcm_start(&a->pcm, now_ns)) return false;
    a->partial_bytes = 0;
    if (a->voice) AUD_set_active_in(a->voice, 1);
    return true;
}

void miband_audio_stop(MiBandAudio *a)
{
    if (a->voice) AUD_set_active_in(a->voice, 0);
    miband_pcm_stop(&a->pcm);
    a->partial_bytes = 0;
}

void miband_audio_destroy(MiBandAudio *a)
{
    if (a->browser_lease) timer_free(a->browser_lease);
    a->browser_lease = NULL;
    if (a->vm_change) qemu_del_vm_change_state_handler(a->vm_change);
    a->vm_change = NULL;
    miband_audio_stop(a);
    if (a->voice) AUD_close_in(&a->card, a->voice);
    if (a->registered) AUD_remove_card(&a->card);
    a->voice = NULL; a->registered = false;
    miband_pcm_destroy(&a->pcm);
}
