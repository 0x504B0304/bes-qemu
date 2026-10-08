/* Private GPU replay worker. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "miband_gpu_worker.h"

#define GPU_REPLAY_MEMORY_LIMIT (64u * 1024u * 1024u)
#define GPU_REPLAY_QUEUE_LIMIT 2

struct MiBandGpuReplay {
    MiBandGpu gpu;
    GArray *writes;
    uint64_t sequence;
    uint64_t generation;
    int64_t compute_ns;
    bool accepted;
};

struct MiBandGpuWorker {
    QemuThread thread;
    QemuMutex mutex;
    QemuCond wake;
    GQueue queued;
    GQueue completed;
    uint64_t generation;
    unsigned outstanding;
    bool stopping;
};

MiBandGpuReplay *miband_gpu_replay_create(const MiBandGpu *source,
                                         uint64_t sequence)
{
    MiBandGpuReplay *job;
    if (!source || !source->ram || !source->psram ||
        !source->ram_size || !source->psram_size ||
        (uint64_t)source->ram_size + source->psram_size > GPU_REPLAY_MEMORY_LIMIT) {
        return NULL;
    }
    job = g_new0(MiBandGpuReplay, 1);
    job->gpu = *source;
    job->gpu.ram = g_memdup2(source->ram, source->ram_size);
    job->gpu.psram = g_memdup2(source->psram, source->psram_size);
    job->gpu.irq = NULL;
    job->gpu.lcdc = NULL;
    job->gpu.async = NULL;
    job->gpu.scanout_bridge = false;
    job->gpu.ack_no_render = false;
    /* Disable bounded legacy informational logging in this pure render copy. */
    job->gpu.acknowledged = UINT_MAX;
    job->sequence = sequence;
    job->writes = g_array_new(false, false, sizeof(MiBandDisplayRange));
    return job;
}

void miband_gpu_replay_free(MiBandGpuReplay *job)
{
    if (job) {
        g_free(job->gpu.ram);
        g_free(job->gpu.psram);
        g_array_free(job->writes, true);
        g_free(job);
    }
}

bool miband_gpu_replay_accepted(const MiBandGpuReplay *job)
{
    return job && job->accepted;
}

uint64_t miband_gpu_replay_sequence(const MiBandGpuReplay *job)
{
    return job->sequence;
}

int64_t miband_gpu_replay_compute_ns(const MiBandGpuReplay *job)
{
    return job->compute_ns;
}

bool miband_gpu_replay_commit_private(const MiBandGpuReplay *job,
                                      MiBandGpu *owner)
{
    unsigned i;
    if (!job || !job->accepted || !owner ||
        owner->ram_size != job->gpu.ram_size ||
        owner->psram_size != job->gpu.psram_size) {
        return false;
    }
    for (i = 0; i < job->writes->len; i++) {
        const MiBandDisplayRange *r = &g_array_index(job->writes,
                                                   MiBandDisplayRange, i);
        uint8_t *dst = r->block == MIBAND_DISPLAY_AP_RAM ? owner->ram :
                                                        owner->psram;
        uint8_t *src = r->block == MIBAND_DISPLAY_AP_RAM ? job->gpu.ram :
                                                        job->gpu.psram;
        memcpy(dst + r->offset, src + r->offset, r->length);
    }
    memcpy(owner->states, job->gpu.states, sizeof(owner->states));
    owner->rendered = job->gpu.rendered;
    owner->path_rendered = job->gpu.path_rendered;
    return true;
}

static void *gpu_worker_thread(void *opaque)
{
    MiBandGpuWorker *worker = opaque;
    qemu_mutex_lock(&worker->mutex);
    while (!worker->stopping) {
        MiBandGpuReplay *job = g_queue_pop_head(&worker->queued);
        int64_t begin;
        if (!job) {
            qemu_cond_wait(&worker->wake, &worker->mutex);
            continue;
        }
        qemu_mutex_unlock(&worker->mutex);
        begin = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        job->accepted = miband_gpu_render_private(&job->gpu, job->writes);
        job->compute_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - begin;
        qemu_mutex_lock(&worker->mutex);
        if (worker->stopping || job->generation != worker->generation) {
            worker->outstanding--;
            miband_gpu_replay_free(job);
        } else {
            g_queue_push_tail(&worker->completed, job);
        }
    }
    qemu_mutex_unlock(&worker->mutex);
    return NULL;
}

MiBandGpuWorker *miband_gpu_worker_new(void)
{
    MiBandGpuWorker *worker = g_new0(MiBandGpuWorker, 1);
    qemu_mutex_init(&worker->mutex);
    qemu_cond_init(&worker->wake);
    worker->generation = 1;
    qemu_thread_create(&worker->thread, "miband-gpu", gpu_worker_thread,
                       worker, QEMU_THREAD_JOINABLE);
    return worker;
}

bool miband_gpu_worker_submit(MiBandGpuWorker *worker, MiBandGpuReplay *job)
{
    bool accepted;
    qemu_mutex_lock(&worker->mutex);
    accepted = !worker->stopping &&
               worker->outstanding < GPU_REPLAY_QUEUE_LIMIT;
    if (accepted) {
        job->generation = worker->generation;
        worker->outstanding++;
        g_queue_push_tail(&worker->queued, job);
        qemu_cond_signal(&worker->wake);
    }
    qemu_mutex_unlock(&worker->mutex);
    return accepted;
}

MiBandGpuReplay *miband_gpu_worker_take(MiBandGpuWorker *worker)
{
    MiBandGpuReplay *job;
    qemu_mutex_lock(&worker->mutex);
    job = g_queue_pop_head(&worker->completed);
    if (job) {
        worker->outstanding--;
    }
    qemu_mutex_unlock(&worker->mutex);
    return job;
}

static void gpu_worker_discard(MiBandGpuWorker *worker, GQueue *queue)
{
    MiBandGpuReplay *job;
    while ((job = g_queue_pop_head(queue))) {
        worker->outstanding--;
        miband_gpu_replay_free(job);
    }
}

void miband_gpu_worker_reset(MiBandGpuWorker *worker)
{
    qemu_mutex_lock(&worker->mutex);
    worker->generation++;
    gpu_worker_discard(worker, &worker->queued);
    gpu_worker_discard(worker, &worker->completed);
    qemu_mutex_unlock(&worker->mutex);
}

void miband_gpu_worker_free(MiBandGpuWorker *worker)
{
    if (!worker) {
        return;
    }
    qemu_mutex_lock(&worker->mutex);
    worker->stopping = true;
    worker->generation++;
    gpu_worker_discard(worker, &worker->queued);
    gpu_worker_discard(worker, &worker->completed);
    qemu_cond_signal(&worker->wake);
    qemu_mutex_unlock(&worker->mutex);
    /* No BQL callback and no queue lock is required for the worker to finish. */
    qemu_thread_join(&worker->thread);
    qemu_cond_destroy(&worker->wake);
    qemu_mutex_destroy(&worker->mutex);
    g_free(worker);
}
