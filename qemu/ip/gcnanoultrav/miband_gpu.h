#ifndef MIBAND_GPU_H
#define MIBAND_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "exec/hwaddr.h"
#include "hw/core/cpu.h"
#include "hw/irq.h"
#include "miband_lcdc.h"

typedef struct MiBandGpu {
    struct MiBandGpuAsync *async;
    hwaddr base;
    uint32_t chip_id, revision, cid, eco_id;
    void *memory_opaque;
    uint8_t *(*memory_ptr)(void *, uint32_t, size_t, bool);
    uint8_t *psram;
    uint32_t psram_size;
    uint8_t *ram;
    uint32_t ram_size;
    uint32_t states[0x1000];
    uint32_t clock_control;
    uint32_t intr_status;
    uint32_t intr_enable;
    uint32_t cmd_address;
    uint32_t cmd_size;
    unsigned acknowledged;
    unsigned intr_trace_events;
    unsigned rendered;
    unsigned path_rendered;
    bool ack_no_render;
    bool render_index8;
    /* VGLite MATH_PRECISION_FIX: texture matrix is in texels, not UV units. */
    bool texture_texel_coordinates;
    struct MiBandLcdc *lcdc;
    bool scanout_bridge;
    qemu_irq irq;
} MiBandGpu;

void miband_gpu_set_worker(MiBandGpu *, CPUState *, bool);
void miband_gpu_cleanup(MiBandGpu *);
void miband_gpu_async_reset(MiBandGpu *);
bool miband_gpu_pending_memory(uint32_t, size_t);
void miband_gpu_init(MiBandGpu *gpu, uint8_t *psram, uint32_t psram_size,
                     uint8_t *ram, uint32_t ram_size,
                     bool ack_no_render, bool render_index8);
void miband_gpu_set_bus(MiBandGpu *, hwaddr, uint32_t, uint32_t, uint32_t,
                        uint32_t, void *,
                        uint8_t *(*)(void *, uint32_t, size_t, bool));
void miband_gpu_connect_irq(MiBandGpu *gpu, qemu_irq irq);
void miband_gpu_connect_lcdc(MiBandGpu *gpu, MiBandLcdc *lcdc,
                             bool scanout_bridge);
void miband_gpu_diag_touch_release(void);
bool miband_gpu_read(MiBandGpu *gpu, hwaddr address, unsigned size,
                     uint64_t *result);
bool miband_gpu_write(MiBandGpu *gpu, hwaddr address, unsigned size,
                      uint64_t value);

#endif
