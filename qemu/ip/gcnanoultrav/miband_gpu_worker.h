/* Private GPU replay worker. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_GPU_WORKER_H
#define MIBAND_GPU_WORKER_H

#include "miband_gpu.h"
#include "miband_display_memory.h"

typedef struct MiBandGpuReplay MiBandGpuReplay;
typedef struct MiBandGpuWorker MiBandGpuWorker;

/*
 * These APIs operate ONLY on private, isolated replay memory. create() copies
 * both memory blocks and every state/CLUT register while the owner is stopped.
 * No guest pointer is retained; the worker never publishes an IRQ or surface.
 * The board has no bypass from gpu-worker=on to this replay API. Live admission
 * remains gated until driver fence, input lifetime and output ownership match a
 * firmware trace. Full-block snapshots are deliberately only a replay tool.
 */
MiBandGpuReplay *miband_gpu_replay_create(const MiBandGpu *source,
                                         uint64_t sequence);
void miband_gpu_replay_free(MiBandGpuReplay *replay);
bool miband_gpu_replay_accepted(const MiBandGpuReplay *replay);
uint64_t miband_gpu_replay_sequence(const MiBandGpuReplay *replay);
int64_t miband_gpu_replay_compute_ns(const MiBandGpuReplay *replay);
/* Copies only recorded raster spans plus final GPU state; never whole frames.
 * The caller MUST use an isolated replay owner, not live guest RAM. */
bool miband_gpu_replay_commit_private(const MiBandGpuReplay *replay,
                                      MiBandGpu *owner);

MiBandGpuWorker *miband_gpu_worker_new(void);
/* Success transfers ownership; bounded to two outstanding replay jobs. */
bool miband_gpu_worker_submit(MiBandGpuWorker *worker, MiBandGpuReplay *replay);
/* Nonblocking. Caller owns the returned completion. */
MiBandGpuReplay *miband_gpu_worker_take(MiBandGpuWorker *worker);
/* Drops queued/completed old-generation jobs; running late completion discarded. */
void miband_gpu_worker_reset(MiBandGpuWorker *worker);
void miband_gpu_worker_free(MiBandGpuWorker *worker);

/* Private renderer entry; caller supplies detached memory and state. */
bool miband_gpu_render_private(MiBandGpu *gpu, GArray *writes);

/* Board integration: requested is observable but unsafe live jobs stay gated. */
void miband_gpu_set_worker(MiBandGpu *gpu, CPUState *cpu, bool requested);
void miband_gpu_async_reset(MiBandGpu *gpu);
void miband_gpu_cleanup(MiBandGpu *gpu);
bool miband_gpu_pending_memory(uint32_t address, size_t length);

#endif
