/* Private-memory GPU worker tests; compiled with the real QEMU 8.2 headers.
 * MemoryRegion dirty callbacks below are a bitmap fixture, not a substitute for
 * validating TCG/DMA dirty instrumentation in a running full-system instance. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "exec/ramlist.h"
#include "exec/target_page.h"
#include "miband_gpu_worker.h"

#define BYTES 65536
static MemoryRegion test_ram, test_psram;
static uint8_t dirty[2][BYTES / 4096];
static uint8_t log_mask[2];
static unsigned irqs, surfaces;
int qemu_loglevel;

/* Thin pthread backing lets the real worker implementation run independently
 * of QEMU's process-wide QOM initialization. No simulated thread scheduler. */
void qemu_mutex_init(QemuMutex *m)
{
    assert(!pthread_mutex_init(&m->lock, NULL));
    m->initialized = true;
}
void qemu_mutex_destroy(QemuMutex *m)
{
    assert(!pthread_mutex_destroy(&m->lock));
}
void qemu_mutex_lock_impl(QemuMutex *m, const char *file, int line)
{
    assert(!pthread_mutex_lock(&m->lock));
}
void qemu_mutex_unlock_impl(QemuMutex *m, const char *file, int line)
{
    assert(!pthread_mutex_unlock(&m->lock));
}
void qemu_cond_init(QemuCond *c)
{
    assert(!pthread_cond_init(&c->cond, NULL));
    c->initialized = true;
}
void qemu_cond_destroy(QemuCond *c)
{
    assert(!pthread_cond_destroy(&c->cond));
}
void qemu_cond_signal(QemuCond *c) { assert(!pthread_cond_signal(&c->cond)); }
void qemu_cond_wait_impl(QemuCond *c, QemuMutex *m, const char *file, int line)
{
    assert(!pthread_cond_wait(&c->cond, &m->lock));
}
QemuMutexLockFunc qemu_mutex_lock_func = qemu_mutex_lock_impl;
QemuCondWaitFunc qemu_cond_wait_func = qemu_cond_wait_impl;
void qemu_thread_create(QemuThread *thread, const char *name,
                        void *(*start)(void *), void *arg, int mode)
{
    assert(mode == QEMU_THREAD_JOINABLE);
    assert(!pthread_create(&thread->thread, NULL, start, arg));
}
void *qemu_thread_join(QemuThread *thread)
{
    void *result;
    assert(!pthread_join(thread->thread, &result));
    return result;
}

/* External effects are errors if the private renderer attempts them. */
void qemu_set_irq(qemu_irq irq, int level) { irqs++; }
void miband_lcdc_queue_gpu_frame(MiBandLcdc *lcdc, const uint8_t *pixels,
                                 uint32_t address, uint32_t format,
                                 uint32_t stride, unsigned skipped)
{
    surfaces++;
}
void qemu_log(const char *fmt, ...) { }
int64_t qemu_clock_get_ns(QEMUClockType type)
{
    return g_get_monotonic_time() * 1000;
}

static unsigned block_index(MemoryRegion *mr)
{
    assert(mr == &test_ram || mr == &test_psram);
    return mr == &test_psram;
}
size_t qemu_target_page_size(void) { return 4096; }
uint64_t memory_region_size(MemoryRegion *mr) { return BYTES; }
uint8_t memory_region_get_dirty_log_mask(MemoryRegion *mr)
{
    return log_mask[block_index(mr)];
}
void memory_region_set_log(MemoryRegion *mr, bool enable, unsigned client)
{
    assert(client == DIRTY_MEMORY_VGA);
    log_mask[block_index(mr)] = enable ? 1 : 0;
}
void memory_region_set_dirty(MemoryRegion *mr, hwaddr offset, hwaddr length)
{
    size_t i;
    assert(length && offset + length <= BYTES);
    for (i = offset / 4096; i <= (offset + length - 1) / 4096; i++) {
        dirty[block_index(mr)][i] = 1;
    }
}
DirtyBitmapSnapshot *memory_region_snapshot_and_clear_dirty(
    MemoryRegion *mr, hwaddr offset, hwaddr size, unsigned client)
{
    unsigned block = block_index(mr);
    void *snapshot;
    assert(!offset && size == BYTES && client == DIRTY_MEMORY_VGA);
    snapshot = g_memdup2(dirty[block], sizeof(dirty[block]));
    memset(dirty[block], 0, sizeof(dirty[block]));
    return snapshot;
}
bool memory_region_snapshot_get_dirty(MemoryRegion *mr,
                                      DirtyBitmapSnapshot *snapshot,
                                      hwaddr offset, hwaddr length)
{
    uint8_t *bits = (uint8_t *)snapshot;
    size_t i;
    for (i = offset / 4096; i <= (offset + length - 1) / 4096; i++) {
        if (bits[i]) {
            return true;
        }
    }
    return false;
}

static void test_versions(void)
{
    MiBandDisplayRange a, b;
    MiBandDisplayWatch *wa, *wb;
    static const uint32_t aliases[] = {0x3c000000, 0x38000000,
                                       0x1c000000, 0x18000000};
    unsigned i;
    test_ram.ram = test_psram.ram = true;
    for (i = 0; i < ARRAY_SIZE(aliases); i++) {
        assert(miband_display_normalize(aliases[i] + 100, 40,
                                       BYTES, BYTES, &a));
        assert(a.block == MIBAND_DISPLAY_PSRAM && a.offset == 100);
    }
    assert(miband_display_normalize(0x00200100, 16, BYTES, BYTES, &a));
    assert(miband_display_normalize(0x20000108, 16, BYTES, BYTES, &b));
    assert(miband_display_ranges_overlap(&a, &b));
    assert(!miband_display_normalize(0xffffffff, 32, BYTES, BYTES, &a));
    assert(!miband_display_normalize(0x3800ffff, 2, BYTES, BYTES, &a));
    assert(miband_display_memory_init(&test_ram, &test_psram));
    assert(!miband_display_memory_init(&test_ram, &test_psram));
    a = (MiBandDisplayRange){MIBAND_DISPLAY_AP_RAM, 0x100, 16};
    b = (MiBandDisplayRange){MIBAND_DISPLAY_PSRAM, 0x1100, 16};
    wa = miband_display_memory_watch(&a, 1);
    wb = miband_display_memory_watch(&b, 1);
    assert(miband_display_memory_unchanged(wa));
    /* Models TCG's dirty callback even if bytes are subsequently restored. */
    memory_region_set_dirty(&test_ram, 0x100, 16);
    memory_region_set_dirty(&test_ram, 0x100, 16);
    assert(miband_display_memory_unchanged(wb));
    assert(!miband_display_memory_unchanged(wa));
    /* Checking wb cleared the bitmap, but accumulated versions retained ABA. */
    miband_display_memory_note_write(0x18001100, 16);
    assert(!miband_display_memory_unchanged(wb));
    miband_display_memory_cleanup();
    assert(!miband_display_memory_unchanged(wa));
    miband_display_memory_watch_free(wa);
    miband_display_memory_watch_free(wb);
    assert(miband_display_memory_init(&test_ram, &test_psram));
    miband_display_memory_cleanup();
    puts("PASS aliases, range overflow, shared dirty epochs, ABA, teardown");
}

static MiBandGpuReplay *wait_job(MiBandGpuWorker *worker, uint64_t sequence)
{
    int64_t deadline = g_get_monotonic_time() + 5000000;
    MiBandGpuReplay *job;
    do {
        job = miband_gpu_worker_take(worker);
        if (job) {
            assert(miband_gpu_replay_sequence(job) == sequence);
            return job;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    assert(!"worker timeout");
    return NULL;
}

static void configure(MiBandGpu *gpu)
{
    miband_gpu_init(gpu, g_malloc0(BYTES), BYTES, g_malloc0(BYTES), BYTES,
                    false, true);
    gpu->states[0xa00] = 0x10001181;
    gpu->states[0xa10] = 0x103;
    gpu->states[0xa11] = 0x00202000; /* AP executable alias. */
    gpu->states[0xa12] = 16;
    gpu->states[0xa13] = (4 << 16) | 4;
    gpu->states[0xa1a] = 0x3f800000;
    gpu->states[0xa25] = 0x01000900;
    gpu->states[0xa29] = 0x18003000;
    gpu->states[0xa2b] = 1;
    gpu->states[0xa2f] = (1 << 16) | 1;
    gpu->states[0xb01] = 0xff0000ff;
    gpu->psram[0x3000] = 1;
    gpu->cmd_address = 0x1c000000;
    gpu->cmd_size = 3;
    /* CLUT state followed by nested CALL/RETURN and root END. */
    stl_le_p(gpu->psram, 0x30010b01);
    stl_le_p(gpu->psram + 4, 0xff00ff00);
    stl_le_p(gpu->psram + 8, 0x60000003);
    stl_le_p(gpu->psram + 12, 0x38001000);
    stl_le_p(gpu->psram + 16, 0);
    stl_le_p(gpu->psram + 0x1000, 0x40000001);
    stw_le_p(gpu->psram + 0x1008, 1);
    stw_le_p(gpu->psram + 0x100a, 1);
    stw_le_p(gpu->psram + 0x100c, 2);
    stw_le_p(gpu->psram + 0x100e, 1);
    stl_le_p(gpu->psram + 0x1010, 0x70000000);
}

static void test_aligned_target_window(void)
{
    static const int16_t path[] = {
        2, 0, 0, 4, 224, 0, 4, 224, 2, 4, 0, 2, 1, 0, 0, 0,
    };
    MiBandGpu gpu;
    MiBandGpuWorker *worker = miband_gpu_worker_new();
    MiBandGpuReplay *job;
    unsigned i;

    configure(&gpu);
    gpu.states[0xa00] = 0x10000102;
    gpu.states[0xa02] = 0xff00ff00;
    gpu.states[0xa10] = 0x101;
    gpu.states[0xa12] = 424;
    gpu.states[0xa13] = (2 << 16) | 224;
    gpu.states[0xa34] = 0x100000;
    gpu.states[0xa39] = 0;
    gpu.states[0xa3a] = (2 << 16) | 212;
    gpu.states[0xa40] = gpu.states[0xa44] = 0x3f800000;
    memset(gpu.psram, 0, 48);
    stl_le_p(gpu.psram, 0x40000004);
    for (i = 0; i < ARRAY_SIZE(path); i++) {
        stw_le_p(gpu.psram + 8 + i * 2, path[i]);
    }
    gpu.cmd_size = 6;
    memset(gpu.ram + 0x2000 - 16, 0x55, 880);
    job = miband_gpu_replay_create(&gpu, 1);
    assert(miband_gpu_worker_submit(worker, job));
    job = wait_job(worker, 1);
    assert(miband_gpu_replay_accepted(job));
    assert(miband_gpu_replay_commit_private(job, &gpu));
    miband_gpu_replay_free(job);
    for (i = 0; i < 880; i++) {
        assert(gpu.ram[0x2000 - 16 + i] ==
               (i >= 16 && i < 864 ? ((i - 16) % 2 ? 0x07 : 0xe0) : 0x55));
    }
    /* One extra writable column crosses the pitch: reject the entire job. */
    gpu.states[0xa3a] = (2 << 16) | 213;
    job = miband_gpu_replay_create(&gpu, 2);
    assert(miband_gpu_worker_submit(worker, job));
    job = wait_job(worker, 2);
    assert(!miband_gpu_replay_accepted(job));
    assert(!miband_gpu_replay_commit_private(job, &gpu));
    miband_gpu_replay_free(job);
    /* Missing window cannot qualify an undersized pitch. */
    gpu.states[0xa3a] = 0;
    job = miband_gpu_replay_create(&gpu, 3);
    assert(miband_gpu_worker_submit(worker, job));
    job = wait_job(worker, 3);
    assert(!miband_gpu_replay_accepted(job));
    miband_gpu_replay_free(job);
    /* reference's QR blit also uses aligned geometry with a bounded rectangle.
     * Verify texture and solid draws, row boundaries and rejected overflow. */
    for (unsigned solid = 0; solid < 2; solid++) {
        gpu.states[0xa00] = solid ? 0x10000102 : 0x10001181;
        gpu.states[0xb01] = 0xff00ff00;
        stl_le_p(gpu.psram, 0x40000001);
        stw_le_p(gpu.psram + 8, 0);
        stw_le_p(gpu.psram + 10, 0);
        stw_le_p(gpu.psram + 12, 212);
        stw_le_p(gpu.psram + 14, 2);
        stl_le_p(gpu.psram + 16, 0);
        gpu.cmd_size = 3;
        memset(gpu.ram + 0x2000 - 16, 0x55, 880);
        job = miband_gpu_replay_create(&gpu, 4 + solid * 2);
        assert(miband_gpu_worker_submit(worker, job));
        job = wait_job(worker, 4 + solid * 2);
        assert(miband_gpu_replay_accepted(job));
        assert(miband_gpu_replay_commit_private(job, &gpu));
        miband_gpu_replay_free(job);
        for (i = 0; i < 880; i++) {
            assert(gpu.ram[0x2000 - 16 + i] ==
                (i >= 16 && i < 864 ? ((i - 16) % 2 ? 0x07 : 0xe0) : 0x55));
        }
        stw_le_p(gpu.psram + 12, 213);
        job = miband_gpu_replay_create(&gpu, 5 + solid * 2);
        assert(miband_gpu_worker_submit(worker, job));
        job = wait_job(worker, 5 + solid * 2);
        assert(!miband_gpu_replay_accepted(job));
        assert(!miband_gpu_replay_commit_private(job, &gpu));
        miband_gpu_replay_free(job);
    }
    miband_gpu_worker_free(worker);
    g_free(gpu.ram);
    g_free(gpu.psram);
    puts("PASS aligned tessellation width, RGB565 pitch, two-row bounds, "
         "cross-pitch and absent-window rejection");
}

static void test_packed_targets(void)
{
    /* Literal wire bytes form the oracle, independent of the codec. */
    static const struct {
        uint32_t format, stride;
        unsigned bytes;
        uint8_t green[3], half_red[3], masked[3];
    } cases[] = {
        {0x101, 12, 2, {0xe0, 0x07}, {0x00, 0x80}, {0x00, 0x40}},
        {0x10a, 24, 3, {0xe0, 0x07, 0xff}, {0x00, 0x80, 0x80},
                       {0x00, 0x40, 0x40}},
    };
    static const int16_t path[] = {
        2, 0, 0, 4, 4, 0, 4, 4, 4, 4, 0, 4, 1, 0, 0, 0,
    };
    unsigned c, i, x, y;
    for (c = 0; c < ARRAY_SIZE(cases); c++) {
        MiBandGpu gpu;
        MiBandGpuWorker *worker = miband_gpu_worker_new();
        MiBandGpuReplay *job;
        unsigned stride = cases[c].bytes == 3 ? cases[c].stride / 4 * 3 :
                                                cases[c].stride;
        configure(&gpu);
        gpu.states[0xa10] = cases[c].format;
        gpu.states[0xa12] = cases[c].stride;
        memset(gpu.ram + 0x2000, 0x55, 80);
        job = miband_gpu_replay_create(&gpu, 1);
        assert(miband_gpu_worker_submit(worker, job));
        job = wait_job(worker, 1);
        assert(miband_gpu_replay_commit_private(job, &gpu));
        miband_gpu_replay_free(job);
        for (i = 0; i < 80; i++) {
            unsigned start = stride + cases[c].bytes;
            assert(gpu.ram[0x2000 + i] ==
                (i >= start && i < start + 2 * cases[c].bytes ?
                 cases[c].green[(i - start) % cases[c].bytes] : 0x55));
        }
        gpu.states[0xa00] = 0x10000102;
        gpu.states[0xa02] = 0x80800000; /* Premultiplied half-red. */
        gpu.states[0xa34] = 0x100000;
        gpu.states[0xa39] = 2 << 16;
        gpu.states[0xa3a] = (2 << 16) | 4;
        gpu.states[0xa40] = gpu.states[0xa44] = 0x3f800000;
        memset(gpu.psram, 0, 48);
        stl_le_p(gpu.psram, 0x40000004);
        for (i = 0; i < ARRAY_SIZE(path); i++) {
            stw_le_p(gpu.psram + 8 + i * 2, path[i]);
        }
        gpu.cmd_size = 6;
        for (y = 2; y < 4; y++) {
            memset(gpu.ram + 0x2000 + y * stride, 0, 4 * cases[c].bytes);
        }
        for (i = 2; i <= 3; i++) {
            if (i == 3) {
                gpu.states[0xa00] = 0x10000402; /* DST_IN with half-alpha. */
            }
            job = miband_gpu_replay_create(&gpu, i);
            assert(miband_gpu_worker_submit(worker, job));
            job = wait_job(worker, i);
            assert(miband_gpu_replay_commit_private(job, &gpu));
            miband_gpu_replay_free(job);
            for (y = 2; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    assert(!memcmp(gpu.ram + 0x2000 + y * stride +
                                   x * cases[c].bytes,
                                   i == 2 ? cases[c].half_red : cases[c].masked,
                                   cases[c].bytes));
                }
                for (x = 4 * cases[c].bytes; x < stride; x++) {
                    assert(gpu.ram[0x2000 + y * stride + x] == 0x55);
                }
            }
        }
        /* Narrow/truncated pitch and unknown targets never publish pixels. */
        gpu.states[0xa12] = cases[c].bytes == 3 ? 15 : 6;
        job = miband_gpu_replay_create(&gpu, 4);
        assert(miband_gpu_worker_submit(worker, job));
        job = wait_job(worker, 4);
        assert(!miband_gpu_replay_accepted(job));
        miband_gpu_replay_free(job);
        miband_gpu_worker_free(worker);
        g_free(gpu.ram);
        g_free(gpu.psram);
    }
    puts("PASS BGR565/BGRA5658 rectangle, clipped paths, premultiplied alpha, "
         "DST_IN, padded pitch, exact commits and invalid pitch rejection");
}

static void test_packed_sources(void)
{
    static const struct {
        uint32_t point, bilinear, stride, first, middle, border;
        uint8_t red[3], blue[3];
        unsigned bytes;
    } cases[] = {
        {0x01000105, 0x01020105, 8, 0xffff0000, 0xff800080, 0x80800000,
         {0x00, 0xf8}, {0x1f, 0x00}, 2},
        {0x41000100, 0x41020100, 12, 0x80840000, 0x80420042, 0x40420000,
         {0x00, 0x80, 0x80}, {0x10, 0x00, 0x80}, 3},
    };
    unsigned c, sequence, x, y;
    for (c = 0; c < ARRAY_SIZE(cases); c++) {
        MiBandGpu gpu;
        MiBandGpuWorker *worker = miband_gpu_worker_new();
        unsigned stride = cases[c].bytes == 3 ? 9 : 8;
        configure(&gpu);
        /* Skip CLUT update; existing CALL rectangle covers two pixels. */
        stl_le_p(gpu.psram, 0x10000000);
        gpu.states[0xa2b] = cases[c].stride;
        gpu.states[0xa2f] = (2 << 16) | 2;
        memset(gpu.psram + 0x3000, 0x55, 32);
        for (y = 0; y < 2; y++) {
            memcpy(gpu.psram + 0x3000 + y * stride, cases[c].red,
                   cases[c].bytes);
            memcpy(gpu.psram + 0x3000 + y * stride + cases[c].bytes,
                   cases[c].blue, cases[c].bytes);
        }
        for (sequence = 1; sequence <= 3; sequence++) {
            MiBandGpuReplay *job;
            uint32_t expected = sequence == 1 ? cases[c].first :
                                sequence == 2 ? cases[c].middle :
                                                cases[c].border;
            gpu.states[0xa25] = sequence == 1 ? cases[c].point :
                                              cases[c].bilinear;
            gpu.states[0xa18] = sequence == 1 ? 0 : sequence == 2 ?
                               0x3e800000 : 0xbe800000; /* u=0, .5, -.5 */
            memset(gpu.ram + 0x2000, 0, 64);
            job = miband_gpu_replay_create(&gpu, sequence);
            assert(miband_gpu_worker_submit(worker, job));
            job = wait_job(worker, sequence);
            assert(miband_gpu_replay_commit_private(job, &gpu));
            miband_gpu_replay_free(job);
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    assert(ldl_le_p(gpu.ram + 0x2000 + y * 16 + x * 4) ==
                           (y == 1 && (x == 1 || x == 2) ? expected : 0));
                }
            }
        }
        miband_gpu_worker_free(worker);
        g_free(gpu.ram);
        g_free(gpu.psram);
    }
    puts("PASS BGR565/BGRA5658 source point/bilinear, premultiplied alpha, "
         "transparent border and padded source pitch");
}

int main(void)
{
    MiBandGpu gpu;
    MiBandGpuWorker *worker;
    MiBandGpuReplay *job, *extra;
    uint8_t saved[64];
    unsigned i;
    test_versions();
    test_packed_targets();
    test_aligned_target_window();
    test_packed_sources();
    configure(&gpu);
    worker = miband_gpu_worker_new();
    job = miband_gpu_replay_create(&gpu, 1);
    assert(job && miband_gpu_worker_submit(worker, job));
    /* Original texture/command change after snapshot cannot leak to worker. */
    gpu.psram[0x3000] = 0;
    memset(gpu.ram + 0x2000, 0x55, 64);
    job = wait_job(worker, 1);
    assert(miband_gpu_replay_accepted(job));
    assert(miband_gpu_replay_commit_private(job, &gpu));
    assert(ldl_le_p(gpu.ram + 0x2014) == 0xff00ff00);
    assert(ldl_le_p(gpu.ram + 0x2018) == 0xff00ff00);
    for (i = 0; i < 64; i++) {
        if (i < 20 || i >= 28) {
            assert(gpu.ram[0x2000 + i] == 0x55);
        }
    }
    assert(gpu.states[0xb01] == 0xff00ff00);
    miband_gpu_replay_free(job);
    /* Next submission snapshots committed state and target, retaining pixels. */
    gpu.psram[0x3000] = 1;
    stl_le_p(gpu.psram, 0x10000000);
    stw_le_p(gpu.psram + 0x100a, 2);
    memcpy(saved, gpu.ram + 0x2000, sizeof(saved));
    job = miband_gpu_replay_create(&gpu, 2);
    assert(miband_gpu_worker_submit(worker, job));
    job = wait_job(worker, 2);
    assert(miband_gpu_replay_commit_private(job, &gpu));
    assert(!memcmp(saved + 20, gpu.ram + 0x2014, 8));
    assert(ldl_le_p(gpu.ram + 0x2024) == 0xff00ff00);
    miband_gpu_replay_free(job);
    /* An unknown opcode produces no publishable result, including any prior
     * private raster writes. It never acquires ack-no-render semantics. */
    stl_le_p(gpu.psram + 16, 0xf0000000);
    job = miband_gpu_replay_create(&gpu, 3);
    assert(miband_gpu_worker_submit(worker, job));
    job = wait_job(worker, 3);
    assert(!miband_gpu_replay_accepted(job));
    assert(!miband_gpu_replay_commit_private(job, &gpu));
    miband_gpu_replay_free(job);
    stl_le_p(gpu.psram + 16, 0);
    assert(miband_gpu_worker_submit(worker, miband_gpu_replay_create(&gpu, 4)));
    assert(miband_gpu_worker_submit(worker, miband_gpu_replay_create(&gpu, 5)));
    extra = miband_gpu_replay_create(&gpu, 6);
    assert(!miband_gpu_worker_submit(worker, extra));
    miband_gpu_replay_free(extra);
    miband_gpu_worker_reset(worker);
    assert(!miband_gpu_worker_take(worker));
    assert(miband_gpu_worker_submit(worker, miband_gpu_replay_create(&gpu, 7)));
    job = wait_job(worker, 7);
    assert(miband_gpu_replay_accepted(job));
    miband_gpu_replay_free(job);
    /* Source and target different guest aliases of the same storage: reject. */
    gpu.states[0xa11] = 0x38003000;
    stw_le_p(gpu.psram + 0x1008, 0);
    stw_le_p(gpu.psram + 0x100a, 0);
    job = miband_gpu_replay_create(&gpu, 8);
    assert(miband_gpu_worker_submit(worker, job));
    job = wait_job(worker, 8);
    assert(!miband_gpu_replay_accepted(job));
    miband_gpu_replay_free(job);
    /* BGR888 solid and textured paths preserve pixels outside the clip. */
    {
        static const int16_t path[] = {
            2, 0, 0, 4, 4, 0, 4, 4, 4, 4, 0, 4, 1, 0, 0, 0,
        };
        gpu.states[0xa11] = 0x20002000;
        gpu.states[0xa10] = 0x109;
        /* The synced rasterizer rejects unknown paint modes explicitly. */
        gpu.states[0xa00] = 0x10000102;
        gpu.states[0xa02] = 0xff0000ff;
        gpu.states[0xa34] = 0x100000;
        gpu.states[0xa39] = 2 << 16;
        gpu.states[0xa3a] = (2 << 16) | 4;
        gpu.states[0xa40] = gpu.states[0xa44] = 0x3f800000;
        memset(gpu.psram, 0, 48);
        stl_le_p(gpu.psram, 0x40000004);
        for (i = 0; i < ARRAY_SIZE(path); i++) {
            stw_le_p(gpu.psram + 8 + i * 2, path[i]);
        }
        gpu.cmd_size = 6;
        memset(gpu.ram + 0x2000, 0x55, 48);
        job = miband_gpu_replay_create(&gpu, 9);
        assert(miband_gpu_worker_submit(worker, job));
        job = wait_job(worker, 9);
        assert(miband_gpu_replay_commit_private(job, &gpu));
        miband_gpu_replay_free(job);
        for (i = 0; i < 48; i++) {
            assert(gpu.ram[0x2000 + i] ==
                   (i < 24 ? 0x55 : ((i % 3) == 0 ? 0xff : 0)));
        }
        gpu.states[0xa00] = 0x10001102;
        gpu.states[0xa25] = 0x01000107;
        gpu.states[0xa2b] = 4;
        stl_le_p(gpu.psram + 0x3000, 0xff00ff00);
        gpu.states[0xa39] = 0;
        job = miband_gpu_replay_create(&gpu, 10);
        assert(miband_gpu_worker_submit(worker, job));
        job = wait_job(worker, 10);
        assert(miband_gpu_replay_commit_private(job, &gpu));
        miband_gpu_replay_free(job);
        for (i = 0; i < 48; i++) {
            assert(gpu.ram[0x2000 + i] ==
                   (i < 24 ? ((i % 3) == 1 ? 0xff : 0) :
                             ((i % 3) == 0 ? 0xff : 0)));
        }
        /* New HIGH-quality edge pixels must be included in private commits;
         * untouched holes must retain destination changes after the snapshot. */
        {
            static const int16_t triangle[] = {
                2, 0, 0, 4, 4, 0, 4, 0, 4, 1, 0, 0, 0, 0, 0, 0,
            };
            unsigned x, y, channel;

            gpu.states[0xa00] = 0x10000102;
            gpu.states[0xa34] = 0x100003;
            gpu.states[0xa3a] = (4 << 16) | 4;
            for (i = 0; i < ARRAY_SIZE(triangle); i++) {
                stw_le_p(gpu.psram + 8 + i * 2, triangle[i]);
            }
            memset(gpu.ram + 0x2000, 0x55, 48);
            job = miband_gpu_replay_create(&gpu, 11);
            assert(miband_gpu_worker_submit(worker, job));
            memset(gpu.ram + 0x2000, 0x44, 48);
            job = wait_job(worker, 11);
            assert(miband_gpu_replay_commit_private(job, &gpu));
            miband_gpu_replay_free(job);
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    for (channel = 0; channel < 3; channel++) {
                        unsigned expected = x + y < 3 ?
                            (channel ? 0 : 255) : x + y == 3 ?
                            (channel ? 53 : 149) : 0x44;
                        assert(gpu.ram[0x2000 + y * 12 + x * 3 + channel] ==
                               expected);
                    }
                }
            }
        }
        gpu.states[0xa00] = 0;
        memcpy(saved, gpu.ram + 0x2000, 48);
        job = miband_gpu_replay_create(&gpu, 12);
        assert(miband_gpu_worker_submit(worker, job));
        job = wait_job(worker, 12);
        assert(!miband_gpu_replay_accepted(job));
        assert(!miband_gpu_replay_commit_private(job, &gpu));
        assert(!memcmp(saved, gpu.ram + 0x2000, 48));
        miband_gpu_replay_free(job);
        /* Exercise both new diagnostic helpers with private replay. The
         * Python runner enables dumps and asserts that no file is created. */
        gpu.states[0xa00] = 0x10001102;
        gpu.states[0xa25] = 0x01011107;
        job = miband_gpu_replay_create(&gpu, 13);
        assert(miband_gpu_worker_submit(worker, job));
        job = wait_job(worker, 13);
        assert(miband_gpu_replay_accepted(job));
        miband_gpu_replay_free(job);
    }
    miband_gpu_worker_free(worker);
    assert(!irqs && !surfaces);
    g_free(gpu.ram);
    g_free(gpu.psram);
    puts("PASS real worker raster, CALL/CLUT, immutable inputs, exact spans, "
         "state inheritance, reject unknown/overlap, clipped solid/textured "
         "paths, AA edge ownership and holes, paint rejection, bounded queue, "
         "reset, shutdown");
    return 0;
}
