#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "hw/core/cpu.h"
#include "sysemu/qtest.h"
#include "sysemu/runstate.h"
#include "miband_lcdc.h"
#include "miband_gpu_pixels.h"
#include "miband_display_memory.h"

#define LCDC_BASE       0x40200000
#define LCDC_SIZE       0x400
#define LCDC_CMD_DONE   (1u << 18)
#define LCDC_TXC_FRAME_DONE (1u << 21)
/* AP accepts either buffer-done bit. Layer-0's register group matches the
 * observed GRA group (bit 27); the exact hardware bit still needs a real trace. */
#define LCDC_SCANOUT_DONE (1u << 27)
#define LCDC_IRQ_ENABLE 0x1c0
#define LCDC_IRQ_STATUS 0x1c4
#define LCDC_FRAME_START 0x224
#define LCDC_FRAME_NS   (1000000000ULL / 60)
#define LCDC_GRA_PITCH  0x0fc
#define LCDC_GRA_SIZE   0x104
#define LCDC_GRA_CONFIG 0x264
#define LCDC_DISPLAY_DUMP_DEFAULT 64U
#define LCDC_DISPLAY_DUMP_MAX 4096U
#define LCDC_ASYNC_REQUEST_LIMIT 256

typedef struct LcdcFrameRequest {
    uint64_t sequence;
    uint64_t generation;
    uint64_t config_generation;
    uint32_t address, width, height, stride;
    unsigned pixel_bytes;
    uint32_t panel_width, panel_height;
    uint32_t gpu_format; /* Only the optional diagnostic GPU bridge. */
    bool valid;
    GBytes *dma_input;
} LcdcFrameRequest;

typedef struct LcdcFrameJob {
    LcdcFrameRequest request;
    uint8_t *input;
    uint8_t *output;
    uint64_t hash;
    const char *layer;
    bool guest_completion;
    uint64_t capture_ns;
    uint64_t compute_ns;
} LcdcFrameJob;

typedef struct LcdcTiming {
    uint64_t buckets[64];
    uint64_t count;
    uint64_t maximum;
} LcdcTiming;

struct MiBandLcdcAsync {
    MiBandLcdc *lcdc;
    QemuThread worker;
    QemuMutex lock;
    QemuCond wake;
    QEMUBH *completion_bh;
    GQueue requests;
    LcdcFrameJob *pending;
    LcdcFrameJob *completed;
    bool busy;
    bool stopping;
    bool stopped;
    bool capture_scheduled;
    uint64_t capture_generation;
    uint64_t generation;
    uint64_t config_generation;
    uint64_t next_sequence;
    uint64_t completed_sequence;
    uint64_t submitted;
    uint64_t committed;
    uint64_t cancelled;
    GBytes *dma_frame;
    uint32_t dma_address;
    unsigned dma_pixel_bytes;
    unsigned test_delay_us;
    LcdcTiming timing[3];
};

static void lcdc_async_commit(void *opaque);
static void lcdc_async_capture(CPUState *cpu, run_on_cpu_data data);

static unsigned lcdc_dump_index;

static unsigned lcdc_diagnostic_limit(void)
{
    static unsigned limit = LCDC_DISPLAY_DUMP_DEFAULT;
    static bool parsed;
    const char *text;
    unsigned configured;

    if (parsed) {
        return limit;
    }
    parsed = true;
    text = g_getenv("MIBAND_DISPLAY_DUMP_LIMIT");
    if (!text) {
        return limit;
    }
    /* Match the remote cap validation while retaining local worker lifetime
     * and input/panel pairing. Zero disables initial exports and logs. */
    if (!*text || strspn(text, "0123456789") != strlen(text) ||
        qemu_strtoui(text, NULL, 10, &configured) < 0 ||
        configured > LCDC_DISPLAY_DUMP_MAX) {
        warn_report("invalid MIBAND_DISPLAY_DUMP_LIMIT: expected a decimal "
                    "integer in 0..%u; using %u",
                    LCDC_DISPLAY_DUMP_MAX, LCDC_DISPLAY_DUMP_DEFAULT);
        return limit;
    }
    limit = configured;
    return limit;
}

static const char *lcdc_dump_dir(void)
{
    const char *directory = g_getenv("MIBAND_DISPLAY_DUMP_DIR");

    return directory && *directory ? directory : NULL;
}

static uint8_t *lcdc_memory_ptr(MiBandLcdc *lcdc, uint32_t address,
                               size_t length)
{
    MiBandDisplayRange range;

    if (lcdc->memory_ptr) {
        return lcdc->memory_ptr(lcdc->memory_opaque, address, length);
    }
    if (!miband_display_normalize(address, length, lcdc->ram_size,
                                  lcdc->psram_size, &range)) {
        return NULL;
    }
    return (range.block == MIBAND_DISPLAY_AP_RAM ? lcdc->ram : lcdc->psram) +
           range.offset;
}

static uint64_t lcdc_hash_bytes(const uint8_t *source, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0; i < length; i++) {
        hash = (hash ^ source[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t lcdc_hash_frame(const uint8_t *source, uint32_t stride,
                                uint32_t width, uint32_t height,
                                unsigned pixel_bytes)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    uint32_t y;

    for (y = 0; y < height; y++) {
        const uint8_t *row = source + (size_t)y * stride;
        size_t bytes = (size_t)width * pixel_bytes;
        size_t i;

        for (i = 0; i < bytes; i++) {
            hash = (hash ^ row[i]) * UINT64_C(1099511628211);
        }
    }
    return hash;
}

static unsigned lcdc_format_bpp(const MiBandLcdc *lcdc)
{
    unsigned format = (lcdc->regs[LCDC_GRA_CONFIG / 4] >> 8) & 0xf;

    /* The AP programs layer 0 at +264.  Unhandled format values retain the
     * machine-property fallback; their hardware interpretation is unverified. */
    switch (format) {
    case 0:
        return lcdc->framebuffer_bpp / 8;
    case 2:
        return 3;
    case 4:
    case 0xb:
        return 4;
    default:
        return lcdc->framebuffer_bpp / 8;
    }
}

static bool lcdc_frame_config(const MiBandLcdc *lcdc, uint32_t *width,
                              uint32_t *height, uint32_t *stride,
                              unsigned *pixel_bytes)
{
    uint32_t dimensions = lcdc->regs[LCDC_GRA_SIZE / 4];
    uint32_t configured_width = dimensions & 0xffff;
    uint32_t configured_height = dimensions >> 16;
    unsigned bytes = lcdc_format_bpp(lcdc);
    uint32_t pitch = lcdc->regs[LCDC_GRA_PITCH / 4] & 0xffff;

    /* The AP layer-0 register table selects +104, +fc, +264 and +f4.
     * +f0 belongs to another layer and must not override this geometry. */
    if (!configured_width || !configured_height || !bytes || bytes > 4) {
        return false;
    }
    if (!pitch) {
        pitch = configured_width * bytes;
    }
    if (pitch < configured_width * bytes) {
        return false;
    }
    *width = configured_width;
    *height = configured_height;
    *stride = pitch;
    *pixel_bytes = bytes;
    return true;
}

static bool lcdc_dump_frame(const MiBandLcdc *lcdc, const char *layer,
                            const uint8_t *source, uint32_t stride,
                            uint32_t width, uint32_t height,
                             unsigned pixel_bytes, uint64_t hash,
                             uint32_t source_address)
{
    const char *directory = lcdc_dump_dir();
    char *path;
    char *meta;
    size_t bytes;

    if (!directory || lcdc_dump_index >= lcdc_diagnostic_limit() || !source ||
        width > lcdc->width || height > lcdc->height ||
        stride < width * pixel_bytes) {
        return false;
    }
    bytes = (size_t)stride * height;
    path = g_strdup_printf("%s/lcdc-%03u-%s.bin", directory,
                           lcdc_dump_index, layer);
    g_file_set_contents(path, (const char *)source, bytes, NULL);
    g_free(path);
    path = g_strdup_printf("%s/lcdc-%03u-%s.txt", directory,
                           lcdc_dump_index, layer);
    meta = g_strdup_printf("layer=%s source=%08x width=%u height=%u stride=%u "
                           "pixel_bytes=%u hash=%016" PRIx64 " panel_frame=%u\n",
                            layer, source_address, width, height, stride,
                           pixel_bytes, hash, lcdc->panel.status(lcdc->panel.opaque).frame_id);
    g_file_set_contents(path, meta, -1, NULL);
    g_free(meta);
    g_free(path);
    lcdc_dump_index++;
    return true;
}

static void lcdc_dump_panel(const MiBandLcdc *lcdc)
{
    const char *directory = lcdc_dump_dir();
    unsigned index;
    char *path;
    char *meta;
    size_t bytes = (size_t)lcdc->width * lcdc->height * 4;
    uint64_t hash;

    if (!directory || !lcdc->panel.status(lcdc->panel.opaque).gram_valid || !lcdc->panel.snapshot(lcdc->panel.opaque) ||
        !lcdc_dump_index) {
        return;
    }
    index = lcdc_dump_index - 1;
    hash = lcdc_hash_bytes(lcdc->panel.snapshot(lcdc->panel.opaque), bytes);
    path = g_strdup_printf("%s/panel-%03u-output.bin", directory, index);
    g_file_set_contents(path, (const char *)lcdc->panel.snapshot(lcdc->panel.opaque), bytes, NULL);
    g_free(path);
    path = g_strdup_printf("%s/panel-%03u-output.txt", directory, index);
    meta = g_strdup_printf("layer=panel-gram width=%u height=%u stride=%u "
                           "pixel_bytes=4 hash=%016" PRIx64 " frame=%u\n",
                           lcdc->width, lcdc->height, lcdc->width * 4,
                           hash, lcdc->panel.status(lcdc->panel.opaque).frame_id);
    g_file_set_contents(path, meta, -1, NULL);
    g_free(meta);
    g_free(path);
}

static bool lcdc_panel_visible(const MiBandLcdc *lcdc)
{
    return lcdc->panel.status(lcdc->panel.opaque).visible;
}

static void lcdc_convert_frame(uint8_t *output, const uint8_t *source,
                               uint32_t stride, uint32_t width,
                               uint32_t height, unsigned pixel_bytes,
                               uint32_t gpu_format,
                               uint32_t panel_width, uint32_t panel_height)
{
    unsigned x, y;

    memset(output, 0, (size_t)panel_width * panel_height * 4);
    for (y = 0; y < MIN(height, panel_height); y++) {
        const uint8_t *src = source + (size_t)y * stride;
        uint32_t *dst = (uint32_t *)(output +
                                     (size_t)y * panel_width * 4);

        for (x = 0; x < MIN(width, panel_width); x++) {
            if (gpu_format) {
                dst[x] = miband_gpu_pixel_read(src, gpu_format) | 0xff000000;
            } else if (pixel_bytes == 2) {
                uint16_t pixel = src[0] | ((uint16_t)src[1] << 8);
                uint32_t r = (pixel >> 11) & 0x1f;
                uint32_t g = (pixel >> 5) & 0x3f;
                uint32_t b = pixel & 0x1f;

                dst[x] = 0xff000000 | ((r << 3 | r >> 2) << 16) |
                         ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            } else {
                dst[x] = 0xff000000 | src[0] | ((uint32_t)src[1] << 8) |
                         ((uint32_t)src[2] << 16);
            }
            src += pixel_bytes;
        }
    }
}

static void lcdc_present_full_frame(MiBandLcdc *lcdc, const uint8_t *source,
                                    uint32_t stride, uint32_t width,
                                    uint32_t height, unsigned pixel_bytes,
                                    uint64_t hash, const char *layer,
                                    uint32_t gpu_format)
{
    if (!source) {
        return;
    }
    lcdc_convert_frame(lcdc->output_frame, source, stride, width, height,
                       pixel_bytes, gpu_format, lcdc->width, lcdc->height);
    bool transferred = lcdc->panel.transfer(lcdc->panel.opaque,
        lcdc->output_frame, (size_t)lcdc->width * lcdc->height * 4);
    g_assert(transferred);
    if (lcdc_dump_frame(lcdc, layer, source, stride, width, height, pixel_bytes,
                        hash, lcdc->regs[0xf4 / 4])) {
        lcdc_dump_panel(lcdc);
    }
}

static void lcdc_frame_job_free(LcdcFrameJob *job)
{
    if (job) {
        g_free(job->input);
        g_free(job->output);
        g_free(job);
    }
}

static void lcdc_frame_request_free(LcdcFrameRequest *request)
{
    if (request) {
        g_clear_pointer(&request->dma_input, g_bytes_unref);
        g_free(request);
    }
}

static uint64_t lcdc_host_ns(void)
{
    return (uint64_t)g_get_monotonic_time() * 1000;
}

static void lcdc_timing_add(LcdcTiming *timing, uint64_t ns)
{
    unsigned bucket = 0;
    uint64_t value = ns;

    while (value && bucket < 63) {
        value >>= 1;
        bucket++;
    }
    timing->buckets[bucket]++;
    timing->count++;
    timing->maximum = MAX(timing->maximum, ns);
}

static uint64_t lcdc_timing_percentile(const LcdcTiming *timing, unsigned pct)
{
    uint64_t target = (timing->count * pct + 99) / 100;
    uint64_t seen = 0;

    if (!target) {
        return 0;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(timing->buckets); i++) {
        seen += timing->buckets[i];
        if (seen >= target) {
            return MIN(timing->maximum, UINT64_C(1) << i);
        }
    }
    return timing->maximum;
}

static void lcdc_async_timing_report(struct MiBandLcdcAsync *async)
{
    static const char *const stages[] = {"capture", "compute", "commit"};

    for (unsigned i = 0; i < G_N_ELEMENTS(stages); i++) {
        const LcdcTiming *timing = &async->timing[i];

        qemu_log_mask(LOG_UNIMP,
                      "miband LCDC worker timing stage=%s count=%" PRIu64
                      " p50-upper-ns=%" PRIu64 " p95-upper-ns=%" PRIu64
                      " p99-upper-ns=%" PRIu64 " max-ns=%" PRIu64 "\n",
                      stages[i], timing->count,
                      lcdc_timing_percentile(timing, 50),
                      lcdc_timing_percentile(timing, 95),
                      lcdc_timing_percentile(timing, 99), timing->maximum);
    }
}

static void *lcdc_async_worker(void *opaque)
{
    struct MiBandLcdcAsync *async = opaque;

    qemu_mutex_lock(&async->lock);
    for (;;) {
        LcdcFrameJob *job;
        uint64_t started;

        while (!async->pending && !async->stopping) {
            qemu_cond_wait(&async->wake, &async->lock);
        }
        if (async->stopping) {
            break;
        }
        job = async->pending;
        async->pending = NULL;
        qemu_mutex_unlock(&async->lock);
        if (async->test_delay_us) {
            g_usleep(async->test_delay_us);
        }
        started = lcdc_host_ns();
        /* The worker owns only these buffers. Never dereference guest RAM,
         * device registers, panel GRAM, console, or IRQ objects here. */
        job->hash = lcdc_hash_frame(job->input, job->request.stride,
                                    job->request.width, job->request.height,
                                    job->request.pixel_bytes);
        lcdc_convert_frame(job->output, job->input, job->request.stride,
                           job->request.width, job->request.height,
                           job->request.pixel_bytes, job->request.gpu_format,
                           job->request.panel_width, job->request.panel_height);
        job->compute_ns = lcdc_host_ns() - started;
        qemu_mutex_lock(&async->lock);
        assert(!async->completed);
        async->completed = job;
        qemu_bh_schedule(async->completion_bh);
    }
    qemu_mutex_unlock(&async->lock);
    return NULL;
}

static bool lcdc_owner_can_commit(void)
{
    /* QTest explicitly advances virtual time without running its vCPU. */
    return qtest_enabled() || runstate_is_running();
}

static void lcdc_async_request(MiBandLcdc *lcdc)
{
    struct MiBandLcdcAsync *async = lcdc->async;
    LcdcFrameRequest *request = g_new0(LcdcFrameRequest, 1);

    request->panel_width = lcdc->width;
    request->panel_height = lcdc->height;
    request->sequence = ++async->next_sequence;
    request->generation = async->generation;
    request->config_generation = async->config_generation;
    request->address = lcdc->regs[0xf4 / 4];
    request->valid = lcdc_frame_config(lcdc, &request->width, &request->height,
                                      &request->stride,
                                      &request->pixel_bytes) &&
                     request->width == lcdc->width &&
                     request->height == lcdc->height;
    if (lcdc->dma_frame_ready && async->dma_frame &&
        request->address == async->dma_address &&
        request->pixel_bytes == async->dma_pixel_bytes) {
        request->dma_input = g_bytes_ref(async->dma_frame);
    }
    g_queue_push_tail(&async->requests, request);
    /* Keep every accepted guest completion. An unexpected unbounded producer
     * is an explicit stopped model error, never latest-wins frame replacement. */
    if (g_queue_get_length(&async->requests) > LCDC_ASYNC_REQUEST_LIMIT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "miband LCDC async queue overflow seq=%" PRIu64
                      "; retaining requests and stopping VM\n",
                      request->sequence);
        vm_stop(RUN_STATE_INTERNAL_ERROR);
    }
}

static void lcdc_async_commit(void *opaque)
{
    struct MiBandLcdcAsync *async = opaque;
    MiBandLcdc *lcdc = async->lcdc;
    LcdcFrameJob *job;
    uint64_t started = lcdc_host_ns();

    qemu_mutex_lock(&async->lock);
    job = async->completed;
    if (!job || (!async->stopping &&
                 job->request.generation == async->generation &&
                 (!lcdc_owner_can_commit() || lcdc->panel.status(lcdc->panel.opaque).sleeping))) {
        qemu_mutex_unlock(&async->lock);
        return;
    }
    async->completed = NULL;
    qemu_mutex_unlock(&async->lock);
    async->busy = false;
    if (async->stopping || job->request.generation != async->generation) {
        async->cancelled++;
        lcdc_frame_job_free(job);
        return;
    }

    /* Register reprogramming does not cancel an already accepted transfer.
     * Its immutable config travels with the job; only reset invalidates it. */
    if (job->guest_completion) {
        assert(job->request.sequence > async->completed_sequence);
        async->completed_sequence = job->request.sequence;
    }
    bool transferred = lcdc->panel.transfer(lcdc->panel.opaque, job->output,
        (size_t)lcdc->width * lcdc->height * 4);
    g_assert(transferred);
    lcdc->scanout_frames++;
    lcdc->lcdc_hash = job->hash;
    lcdc->lcdc_hash_valid = true;
    lcdc->bridge_active = !job->guest_completion;
    if (lcdc->bridge_active) {
        lcdc->bridge_address = job->request.address;
        lcdc->bridge_width = job->request.width;
        lcdc->bridge_height = job->request.height;
        lcdc->bridge_stride = job->request.stride;
    }
    if (job->request.config_generation == async->config_generation) {
        lcdc->lcdc_dirty = false;
    }
    if (lcdc_dump_frame(lcdc, job->layer, job->input, job->request.stride,
                        job->request.width, job->request.height,
                        job->request.pixel_bytes, job->hash,
                        job->request.address)) {
        lcdc_dump_panel(lcdc);
    }
    if (job->guest_completion) {
        lcdc->frame_requested = !g_queue_is_empty(&async->requests);
        if (!lcdc->frame_requested) {
            lcdc->regs[LCDC_FRAME_START / 4] &= ~1u;
        }
        lcdc->regs[LCDC_IRQ_STATUS / 4] |= LCDC_TXC_FRAME_DONE | LCDC_SCANOUT_DONE;
        qemu_set_irq(lcdc->irq, (lcdc->regs[LCDC_IRQ_STATUS / 4] &
                               lcdc->regs[LCDC_IRQ_ENABLE / 4]) != 0);
    }
    async->committed++;
    lcdc_timing_add(&async->timing[0], job->capture_ns);
    lcdc_timing_add(&async->timing[1], job->compute_ns);
    lcdc_timing_add(&async->timing[2], lcdc_host_ns() - started);
    if (!(async->committed % 600)) {
        lcdc_async_timing_report(async);
    }
    if (async->committed <= lcdc_diagnostic_limit() || !(async->committed % 600)) {
        qemu_log_mask(LOG_UNIMP,
                      "miband LCDC async commit seq=%" PRIu64
                      " generation=%" PRIu64 " config=%" PRIu64
                      " source=%08x hash=%016" PRIx64
                      " queued=%u virtual_ms=%" PRId64 "\n",
                      job->request.sequence, job->request.generation,
                      job->request.config_generation, job->request.address,
                      job->hash, g_queue_get_length(&async->requests),
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
    lcdc_frame_job_free(job);
}

static void lcdc_async_capture(CPUState *cpu, run_on_cpu_data data)
{
    struct MiBandLcdcAsync *async = data.host_ptr;
    MiBandLcdc *lcdc = async->lcdc;
    LcdcFrameRequest *request;
    LcdcFrameRequest bridge = {0};
    LcdcFrameJob *job;
    const uint8_t *source;
    bool from_dma;
    bool consumes_current_dma;
    bool diagnostic = false;
    size_t bytes;
    uint64_t started = lcdc_host_ns();

    async->capture_scheduled = false;
    if (async->capture_generation != async->generation ||
        async->stopping || !lcdc_owner_can_commit() ||
        lcdc->panel.status(lcdc->panel.opaque).sleeping || async->busy) {
        return;
    }
    request = g_queue_peek_head(&async->requests);
    if (!request && lcdc->pending_gpu && lcdc_panel_visible(lcdc)) {
        bridge.generation = async->generation;
        bridge.config_generation = async->config_generation;
        bridge.address = lcdc->pending_gpu_address;
        bridge.panel_width = lcdc->width;
        bridge.panel_height = lcdc->height;
        bridge.width = lcdc->width;
        bridge.height = lcdc->height;
        bridge.stride = lcdc->pending_gpu_stride;
        bridge.pixel_bytes = miband_gpu_pixel_bytes(lcdc->pending_gpu_format);
        bridge.gpu_format = lcdc->pending_gpu_format;
        bridge.valid = true;
        request = &bridge;
        diagnostic = true;
    }
    if (!request || !request->valid) {
        return;
    }
    bytes = (size_t)request->stride * request->height;
    if (lcdc->memory_busy && lcdc->memory_busy(request->address, bytes)) {
        return;
    }
    from_dma = !diagnostic && request->dma_input;
    consumes_current_dma = from_dma &&
                           request->dma_input == async->dma_frame;
    if (!from_dma && lcdc->dma_in_progress &&
        request->address == lcdc->regs[0xf4 / 4]) {
        return;
    }
    source = diagnostic ? lcdc->pending_gpu_source :
             from_dma ? g_bytes_get_data(request->dma_input, NULL) :
             lcdc_memory_ptr(lcdc, request->address, bytes);
    if (!source) {
        return;
    }
    job = g_new0(LcdcFrameJob, 1);
    job->request = *request;
    job->request.dma_input = NULL;
    job->guest_completion = !diagnostic;
    job->layer = diagnostic ? "gpu-bridge-diagnostic" :
                 from_dma ? "lcdc-input-dma" : "lcdc-input-memory";
    if (from_dma) {
        job->request.stride = request->width * request->pixel_bytes;
        bytes = (size_t)job->request.stride * request->height;
    }
    /* This callback runs on the sole vCPU outside guest execution with BQL.
     * BQL alone, in a main-loop callback, would not exclude TCG RAM stores. */
    job->input = g_memdup2(source, bytes);
    job->output = g_malloc((size_t)lcdc->width * lcdc->height * 4);
    if (!diagnostic) {
        lcdc_frame_request_free(g_queue_pop_head(&async->requests));
    }
    if (consumes_current_dma) {
        lcdc->dma_frame_ready = false;
    }
    lcdc->pending_gpu = false;
    async->busy = true;
    async->submitted++;
    job->capture_ns = lcdc_host_ns() - started;
    qemu_mutex_lock(&async->lock);
    assert(!async->pending && !async->completed);
    async->pending = job;
    qemu_cond_signal(&async->wake);
    qemu_mutex_unlock(&async->lock);
}

void miband_lcdc_set_async(MiBandLcdc *lcdc, bool enabled)
{
    struct MiBandLcdcAsync *async;

    if (!enabled || lcdc->async) {
        return;
    }
    async = g_new0(struct MiBandLcdcAsync, 1);
    async->lcdc = lcdc;
    async->generation = 1;
    if (qtest_enabled() || qtest_driver()) {
        const char *delay = g_getenv("MIBAND_LCDC_TEST_WORKER_DELAY_US");

        if (delay) {
            async->test_delay_us = MIN(g_ascii_strtoull(delay, NULL, 10),
                                       1000000);
        }
    }
    g_queue_init(&async->requests);
    qemu_mutex_init(&async->lock);
    qemu_cond_init(&async->wake);
    async->completion_bh = qemu_bh_new(lcdc_async_commit, async);
    lcdc->async = async;
    qemu_thread_create(&async->worker, "miband-lcdc", lcdc_async_worker,
                       async, QEMU_THREAD_JOINABLE);
}

void miband_lcdc_async_reset(MiBandLcdc *lcdc)
{
    struct MiBandLcdcAsync *async = lcdc->async;

    if (!async) {
        return;
    }
    async->generation++;
    async->config_generation++;
    while (!g_queue_is_empty(&async->requests)) {
        lcdc_frame_request_free(g_queue_pop_head(&async->requests));
        async->cancelled++;
    }
    lcdc->frame_requested = false;
    lcdc->regs[LCDC_FRAME_START / 4] &= ~1u;
    lcdc->regs[LCDC_IRQ_STATUS / 4] &=
        ~(LCDC_TXC_FRAME_DONE | LCDC_SCANOUT_DONE);
    lcdc->dma_in_progress = false;
    lcdc->dma_frame_ready = false;
    lcdc->pending_gpu = false;
    g_clear_pointer(&async->dma_frame, g_bytes_unref);
    qemu_set_irq(lcdc->irq, (lcdc->regs[LCDC_IRQ_STATUS / 4] &
                           lcdc->regs[LCDC_IRQ_ENABLE / 4]) != 0);
    /* In-flight jobs own their input. The worker may finish after reset; the
     * generation check consumes it without mutating GRAM or signalling IRQ. */
    lcdc_async_commit(async);
}

void miband_lcdc_async_shutdown(MiBandLcdc *lcdc)
{
    struct MiBandLcdcAsync *async = lcdc->async;

    if (!async || async->stopped) {
        return;
    }
    miband_lcdc_async_reset(lcdc);
    qemu_mutex_lock(&async->lock);
    async->stopping = true;
    qemu_cond_signal(&async->wake);
    qemu_mutex_unlock(&async->lock);
    /* The worker never needs BQL or a completion callback to finish. */
    qemu_thread_join(&async->worker);
    qemu_bh_delete(async->completion_bh);
    async->completion_bh = NULL;
    qemu_mutex_lock(&async->lock);
    lcdc_frame_job_free(async->pending);
    lcdc_frame_job_free(async->completed);
    async->pending = NULL;
    async->completed = NULL;
    qemu_mutex_unlock(&async->lock);
    async->stopped = true;
    lcdc_async_timing_report(async);
    qemu_log_mask(LOG_UNIMP,
                  "miband LCDC worker shutdown submitted=%" PRIu64
                  " committed=%" PRIu64 " cancelled=%" PRIu64
                  " retained-runtime=1 retained-bytes=%zu\n",
                  async->submitted, async->committed, async->cancelled,
                  sizeof(*async));
    /* Retain the small runtime until machine destruction: an already queued
     * async_run_on_cpu callback still carries its address and sees stopping. */
}

static void lcdc_commit_frame(MiBandLcdc *lcdc)
{
    uint32_t address = lcdc->regs[0xf4 / 4];
    uint32_t width, height, stride;
    unsigned pixel_bytes;
    size_t length;
    const uint8_t *normal = NULL;
    uint64_t hash = 0;
    bool changed = false;

    if (lcdc->panel.status(lcdc->panel.opaque).sleeping) {
        /* Candidate sleep policy: retain the request and memory snapshot until
         * awake.  Transfer acceptance during sleep needs hardware evidence. */
        return;
    }
    if (!lcdc_frame_config(lcdc, &width, &height, &stride, &pixel_bytes) ||
        width != lcdc->width || height != lcdc->height) {
        return;
    }
    length = (size_t)width * height * pixel_bytes;
    if (lcdc->dma_frame_ready) {
        hash = lcdc_hash_bytes(lcdc->dma_completed_frame, length);
        changed = lcdc->frame_requested || !lcdc->panel.status(lcdc->panel.opaque).gram_valid ||
                  !lcdc->lcdc_hash_valid || lcdc->bridge_active ||
                  hash != lcdc->lcdc_hash;
        if (lcdc->dma_frames <= lcdc_diagnostic_limit()) {
            qemu_log_mask(LOG_UNIMP,
                          "miband LCDC DMA tick ready=%u active=%u changed=%u "
                          "scanout_valid=%u bridge=%u next=%016" PRIx64
                          " previous=%016" PRIx64 " virtual_ms=%" PRId64 "\n",
                          lcdc->dma_frame_ready, lcdc->dma_in_progress,
                          changed, (lcdc_panel_visible(lcdc) && lcdc->panel.status(lcdc->panel.opaque).gram_valid),
                          lcdc->bridge_active, hash, lcdc->lcdc_hash,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
        if (changed) {
            lcdc_present_full_frame(lcdc, lcdc->dma_completed_frame,
                                    lcdc->width * pixel_bytes, width, height,
                                    pixel_bytes, hash, "lcdc-input-dma", 0);
            if (lcdc->scanout_frames++ < lcdc_diagnostic_limit()) {
                qemu_log_mask(LOG_UNIMP,
                              "miband LCDC completed DMA frame=%u source=%08x "
                              "format=%u hash=%016" PRIx64
                              " next_dma_active=%u virtual_ms=%" PRId64 "\n",
                              lcdc->scanout_frames, address,
                              lcdc->framebuffer_bpp, hash,
                              lcdc->dma_in_progress,
                              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
            }
        }
        lcdc->lcdc_hash = hash;
        lcdc->lcdc_hash_valid = true;
        lcdc->bridge_active = false;
        lcdc->pending_gpu = false;
        lcdc->lcdc_dirty = false;
        lcdc->dma_frame_ready = false;
        return;
    }
    if (lcdc->dma_in_progress) {
        return;
    }

    normal = lcdc_memory_ptr(lcdc, address,
                            (size_t)stride * height);
    if (normal) {
        hash = lcdc_hash_frame(normal, stride, width, height, pixel_bytes);
        /* A requested transfer is valid even when its bytes repeat the last
         * frame.  Hash equality is not a panel-frame acceptance condition. */
        changed = lcdc->frame_requested || !lcdc->panel.status(lcdc->panel.opaque).gram_valid ||
                  (lcdc->lcdc_dirty &&
                   (lcdc->bridge_active || !lcdc->lcdc_hash_valid ||
                    hash != lcdc->lcdc_hash)) ||
                  (!lcdc->bridge_active && lcdc->lcdc_hash_valid &&
                   hash != lcdc->lcdc_hash);
        lcdc->lcdc_hash = hash;
        lcdc->lcdc_hash_valid = true;
    }

    if (normal && changed) {
        lcdc_present_full_frame(lcdc, normal, stride, width, height,
                                pixel_bytes, hash, "lcdc-input-memory", 0);
        lcdc->bridge_active = false;
        lcdc->pending_gpu = false;
        if (lcdc->scanout_frames++ < lcdc_diagnostic_limit()) {
            qemu_log_mask(LOG_UNIMP,
                          "miband LCDC complete frame=%u source=%08x "
                          "format=%u hash=%016" PRIx64
                          " gpu_pending=%u virtual_ms=%" PRId64 "\n",
                          lcdc->scanout_frames, address,
                          lcdc->framebuffer_bpp, hash, lcdc->pending_gpu,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    } else if (lcdc->pending_gpu) {
        unsigned bytes = miband_gpu_pixel_bytes(lcdc->pending_gpu_format);

        uint64_t hash = lcdc_hash_frame(lcdc->pending_gpu_source,
                                        lcdc->pending_gpu_stride,
                                        lcdc->width, lcdc->height, bytes);

        lcdc_present_full_frame(lcdc, lcdc->pending_gpu_source,
                                lcdc->pending_gpu_stride, lcdc->width,
                                lcdc->height, bytes, hash,
                                "gpu-bridge-diagnostic",
                                lcdc->pending_gpu_format);
        lcdc->bridge_active = true;
        lcdc->bridge_address = lcdc->pending_gpu_address;
        lcdc->bridge_width = lcdc->width;
        lcdc->bridge_height = lcdc->height;
        lcdc->bridge_stride = lcdc->pending_gpu_stride;
        if (lcdc->scanout_frames++ < lcdc_diagnostic_limit()) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU->LCDC complete frame=%u source=%08x "
                          "format=%03x stride=%u skipped=%u lcdc_source=%08x "
                          "virtual_ms=%" PRId64 "\n",
                          lcdc->scanout_frames, lcdc->pending_gpu_address,
                          lcdc->pending_gpu_format, lcdc->pending_gpu_stride,
                          lcdc->pending_gpu_skipped, address,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
        lcdc->pending_gpu = false;
    }
    lcdc->lcdc_dirty = false;
}

void miband_lcdc_note_dma_write(MiBandLcdc *lcdc, uint32_t address,
                                size_t length)
{
    uint32_t scanout = lcdc->regs[0xf4 / 4];
    unsigned pixel_bytes = lcdc_format_bpp(lcdc);
    size_t frame_bytes = (size_t)lcdc->width * lcdc->height *
                         pixel_bytes;

    if (!frame_bytes || length > frame_bytes) {
        return;
    }
    if (address == scanout) {
        lcdc->dma_bytes = length;
        lcdc->dma_next_address = address + length;
        lcdc->dma_in_progress = length < frame_bytes;
    } else if (lcdc->dma_in_progress && address == lcdc->dma_next_address &&
               length <= frame_bytes - lcdc->dma_bytes) {
        lcdc->dma_bytes += length;
        lcdc->dma_next_address += length;
        lcdc->dma_in_progress = lcdc->dma_bytes < frame_bytes;
    } else {
        return;
    }
    if (!lcdc->dma_in_progress) {
        const uint8_t *complete = lcdc_memory_ptr(lcdc, scanout, frame_bytes);

        if (!complete || !lcdc->dma_completed_frame) {
            return;
        }
        memcpy(lcdc->dma_completed_frame, complete, frame_bytes);
        lcdc->dma_frame_ready = true;
        if (lcdc->async) {
            struct MiBandLcdcAsync *async = lcdc->async;

            g_clear_pointer(&async->dma_frame, g_bytes_unref);
            async->dma_frame = g_bytes_new(complete, frame_bytes);
            async->dma_address = scanout;
            async->dma_pixel_bytes = pixel_bytes;
            for (GList *item = async->requests.head; item; item = item->next) {
                LcdcFrameRequest *request = item->data;

                if (!request->dma_input && request->address == scanout &&
                    request->pixel_bytes == pixel_bytes) {
                    request->dma_input = g_bytes_ref(async->dma_frame);
                }
            }
        }
        lcdc->lcdc_dirty = true;
        lcdc->pending_gpu = false;
        lcdc->dma_frames++;
        if (lcdc->dma_frames <= lcdc_diagnostic_limit() || lcdc->dma_frames % 600 == 0) {
            qemu_log_mask(LOG_UNIMP,
                          "miband LCDC DMA frame ready count=%u source=%08x "
                          "bytes=%zu stride=%u pixel_bytes=%u "
                          "virtual_ms=%" PRId64 "\n",
                          lcdc->dma_frames, scanout, lcdc->dma_bytes,
                          lcdc->width * pixel_bytes, pixel_bytes,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
}

static void miband_lcdc_frame_tick(void *opaque)
{
    MiBandLcdc *lcdc = opaque;
    uint32_t width, height, stride;
    unsigned pixel_bytes;

    if (lcdc->async) {
        struct MiBandLcdcAsync *async = lcdc->async;

        lcdc_async_commit(async);
        if (!async->stopping && !async->busy &&
            !async->capture_scheduled &&
            (!g_queue_is_empty(&async->requests) || lcdc->pending_gpu)) {
            async->capture_scheduled = true;
            async->capture_generation = async->generation;
            if (qtest_enabled() || !first_cpu) {
                lcdc_async_capture(first_cpu, RUN_ON_CPU_HOST_PTR(async));
            } else {
                async_run_on_cpu(first_cpu, lcdc_async_capture,
                                 RUN_ON_CPU_HOST_PTR(async));
            }
        }
    } else if (lcdc->frame_requested && !lcdc->panel.status(lcdc->panel.opaque).sleeping &&
        (lcdc->dma_frame_ready || !lcdc->dma_in_progress) &&
        lcdc_frame_config(lcdc, &width, &height, &stride, &pixel_bytes) &&
        width == lcdc->width && height == lcdc->height &&
        lcdc_memory_ptr(lcdc, lcdc->regs[0xf4 / 4], (size_t)stride * height)) {
        lcdc_commit_frame(lcdc);
        lcdc->frame_requested = false;
        lcdc->regs[LCDC_FRAME_START / 4] &= ~1u;
        lcdc->regs[LCDC_IRQ_STATUS / 4] |= LCDC_TXC_FRAME_DONE | LCDC_SCANOUT_DONE;
    } else if (lcdc->pending_gpu && lcdc_panel_visible(lcdc)) {
        /* Opt-in diagnostic bridge only.  It must not signal a hardware
         * completion or consume the firmware's pending framebuffer queue. */
        lcdc_commit_frame(lcdc);
    }
    if (lcdc->regs[LCDC_IRQ_STATUS / 4] &
        lcdc->regs[LCDC_IRQ_ENABLE / 4]) {
        qemu_set_irq(lcdc->irq, 1);
        lcdc->frame_irqs++;
        if (lcdc->frame_irqs <= lcdc_diagnostic_limit() || lcdc->frame_irqs % 600 == 0) {
            qemu_log_mask(LOG_UNIMP,
                          "miband LCDC frame IRQ %u status=%08x virtual_ms=%"
                          PRId64 "\n", lcdc->frame_irqs,
                          lcdc->regs[LCDC_IRQ_STATUS / 4],
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
    /* Panel TE is a separate input to the GPIO controller. It schedules
     * the firmware worker; it is never a transfer completion itself.
     * 60 Hz remains a timing approximation pending panel-rate evidence. */
    lcdc->panel.tick(lcdc->panel.opaque);
    timer_mod(lcdc->frame_event,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LCDC_FRAME_NS);
}

void miband_lcdc_connect_irq(MiBandLcdc *lcdc, qemu_irq irq)
{
    lcdc->irq = irq;
    lcdc->frame_event = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     miband_lcdc_frame_tick, lcdc);
    timer_mod(lcdc->frame_event,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LCDC_FRAME_NS);
}

void miband_lcdc_init(MiBandLcdc *lcdc, MiBandPanelEndpoint panel,
                      uint32_t framebuffer_bpp, uint32_t width, uint32_t height,
                      uint8_t *psram,
                      uint32_t psram_size, uint8_t *ram, uint32_t ram_size)
{
    memset(lcdc, 0, sizeof(*lcdc));
    lcdc->base = LCDC_BASE;
    assert(width && height && width <= 1024 && height <= 1024);
    lcdc->width = width;
    lcdc->height = height;
    lcdc->panel = panel;
    lcdc->framebuffer_bpp = framebuffer_bpp;
    lcdc->psram = psram;
    lcdc->psram_size = psram_size;
    lcdc->ram = ram;
    lcdc->ram_size = ram_size;
    lcdc->trace_control = g_strcmp0(g_getenv("MIBAND_LCDC_TRACE_CONTROL"), "1") == 0;
    lcdc->dma_completed_frame = g_malloc((size_t)lcdc->width *
                                         lcdc->height * 4);
    lcdc->output_frame = g_malloc0((size_t)width * height * 4);
}

void miband_lcdc_set_base(MiBandLcdc *lcdc, uint32_t base)
{
    lcdc->base = base;
}

void miband_lcdc_set_memory(MiBandLcdc *lcdc, void *opaque,
                            uint8_t *(*memory_ptr)(void *, uint32_t, size_t))
{
    lcdc->memory_opaque = opaque;
    lcdc->memory_ptr = memory_ptr;
}

void miband_lcdc_queue_gpu_frame(MiBandLcdc *lcdc, const uint8_t *source,
                                 uint32_t address, uint32_t format,
                                 uint32_t stride, unsigned skipped)
{
    unsigned pixel_bytes = miband_gpu_pixel_bytes(format);

    if (!source || !pixel_bytes ||
        stride < lcdc->width * pixel_bytes) {
        return;
    }
    lcdc->pending_gpu_source = source;
    lcdc->pending_gpu_address = address;
    lcdc->pending_gpu_format = format;
    lcdc->pending_gpu_stride = stride;
    lcdc->pending_gpu_skipped = skipped;
    lcdc->pending_gpu = true;
    if (lcdc->bridge_frames++ < lcdc_diagnostic_limit()) {
        qemu_log_mask(LOG_UNIMP,
                      "miband GPU full-frame candidate=%u source=%08x "
                      "format=%03x stride=%u skipped=%u lcdc_source=%08x "
                      "virtual_ms=%" PRId64 "\n",
                      lcdc->bridge_frames, address, format, stride, skipped,
                      lcdc->regs[0xf4 / 4],
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
}

static bool lcdc_access(const MiBandLcdc *lcdc, hwaddr address, unsigned size)
{
    return address >= lcdc->base && address < lcdc->base + LCDC_SIZE &&
           size == sizeof(uint32_t) && !(address & 3);
}

bool miband_lcdc_read(MiBandLcdc *lcdc, hwaddr address, unsigned size,
                      uint64_t *result)
{
    if (!lcdc_access(lcdc, address, size)) {
        return false;
    }

    *result = lcdc->regs[(address - lcdc->base) / 4];
    return true;
}

bool miband_lcdc_write(MiBandLcdc *lcdc, hwaddr address, unsigned size,
                       uint64_t value)
{
    hwaddr offset;

    if (!lcdc_access(lcdc, address, size)) {
        return false;
    }

    offset = address - lcdc->base;
    if (lcdc->async &&
        (offset == 0xf4 || offset == LCDC_GRA_PITCH ||
         offset == LCDC_GRA_SIZE || offset == LCDC_GRA_CONFIG)) {
        lcdc->async->config_generation++;
    }
    if ((lcdc->trace_control && lcdc->diagnostic_writes < 32768) ||
        (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) >= 50000 &&
         lcdc->diagnostic_writes < 512 &&
         offset != LCDC_IRQ_STATUS && offset != 0x180)) {
        qemu_log_mask(LOG_UNIMP,
                      "miband LCDC MMIO write #%u +%03" HWADDR_PRIx
                      " old=%08x new=%08" PRIx64 " scanout=%08x"
                      " visible=%u dma_ready=%u dma_active=%u"
                      " virtual_ms=%" PRId64 "\n",
                      ++lcdc->diagnostic_writes, offset,
                      lcdc->regs[offset / 4], value,
                      lcdc->regs[0xf4 / 4], lcdc_panel_visible(lcdc),
                      lcdc->dma_frame_ready, lcdc->dma_in_progress,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
    if (offset == 0xf0 || offset == 0xf4) {
        lcdc->lcdc_dirty = true;
        if (offset == 0xf4 && lcdc->regs[offset / 4] != value) {
            lcdc->dma_in_progress = false;
            lcdc->dma_frame_ready = false;
        }
    }
    if (offset == LCDC_IRQ_ENABLE) {
        lcdc->regs[offset / 4] = value;
        qemu_set_irq(lcdc->irq,
                     (lcdc->regs[LCDC_IRQ_STATUS / 4] & value) != 0);
    } else if (offset == LCDC_IRQ_STATUS) {
        /* Firmware clears the command-complete latch by writing bit 18 low. */
        lcdc->regs[offset / 4] = value & ~LCDC_CMD_DONE;
        qemu_set_irq(lcdc->irq,
                     (lcdc->regs[offset / 4] &
                      lcdc->regs[LCDC_IRQ_ENABLE / 4]) != 0);
    } else {
        lcdc->regs[offset / 4] = value;
        if (offset == LCDC_FRAME_START && (value & 1)) {
            lcdc->frame_requested = true;
            if (lcdc->async) {
                lcdc_async_request(lcdc);
            }
            qemu_log_mask(LOG_UNIMP,
                          "miband LCDC frame request source=%08x "
                          "visible=%u dma_ready=%u virtual_ms=%" PRId64 "\n",
                          lcdc->regs[0xf4 / 4], lcdc_panel_visible(lcdc),
                          lcdc->dma_frame_ready,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
        if (value >= 0x38000000 && value < 0x3e000000 &&
            lcdc->buffer_writes++ < lcdc_diagnostic_limit()) {
            qemu_log_mask(LOG_UNIMP,
                          "miband LCDC buffer register +%03" HWADDR_PRIx
                          " = %08" PRIx64 "\n", offset, value);
        }
        if (offset == 0x280 && (value & 1)) {
            unsigned command = (lcdc->regs[0x284 / 4] >> 8) & 0xff;
            uint32_t reply = 0;
            bool read_command = (lcdc->regs[0x230 / 4] & 0x40) != 0;
            MiBandPanelReply response = lcdc->panel.command(lcdc->panel.opaque,
                                                           command, read_command);
            reply = response.value;
            if (read_command) {
                lcdc->regs[0x140 / 4] = reply;
            }
            if (response.reset_transfer) {
                miband_lcdc_async_reset(lcdc);
                lcdc->lcdc_hash_valid = false;
            }
            if (!read_command && (response.reset_transfer || command == 0x10 ||
                command == 0x11 || command == 0x28 || command == 0x29 ||
                command == 0x34 || command == 0x35)) {
                MiBandPanelStatus status = lcdc->panel.status(lcdc->panel.opaque);
                qemu_log_mask(LOG_UNIMP,
                    "miband panel command=%02x visible=%u sleep=%u "
                    "gram_valid=%u mem_write=%u virtual_ms=%" PRId64 "\n",
                    command, status.visible, status.sleeping, status.gram_valid,
                    status.memory_write, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
            }
            lcdc->regs[0x1c4 / 4] |= LCDC_CMD_DONE;
            lcdc->transactions++;
            if (command == 0x2c) {
                lcdc->pixel_commands++;
            }
            if (lcdc->transactions <= 32 ||
                (command >= 0xda && command <= 0xdc) ||
                (command == 0x2c && lcdc->pixel_commands <= 16)) {
                qemu_log_mask(LOG_UNIMP,
                              "miband LCDC command %u: cfg=%08x start=%08" PRIx64
                              " reply=%02x\n", lcdc->transactions,
                              lcdc->regs[0x284 / 4], value, reply);
            }
        }
    }

    return true;
}
