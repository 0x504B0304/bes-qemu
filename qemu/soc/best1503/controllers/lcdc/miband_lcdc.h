#ifndef MIBAND_LCDC_H
#define MIBAND_LCDC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "exec/hwaddr.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "miband_panel_endpoint.h"

typedef struct MiBandLcdc {
    /* Register window base varies between SoCs; BEST1503 keeps the
     * historical default while other profiles configure their own window. */
    uint32_t base;
    struct MiBandLcdcAsync *async;
    MiBandPanelEndpoint panel;
    uint8_t *output_frame;
    bool (*memory_busy)(uint32_t address, size_t length);
    void *memory_opaque;
    uint8_t *(*memory_ptr)(void *opaque, uint32_t address, size_t length);
    uint32_t regs[0x400 / 4];
    uint8_t *psram;
    uint32_t psram_size;
    uint8_t *ram;
    uint32_t ram_size;
    uint32_t framebuffer_bpp;
    uint32_t width, height;
    uint32_t transactions;
    uint32_t buffer_writes;
    uint32_t pixel_commands;
    unsigned bridge_frames;
    bool bridge_active;
    uint32_t bridge_address;
    uint32_t bridge_width;
    uint32_t bridge_height;
    uint32_t bridge_stride;
    const uint8_t *pending_gpu_source;
    uint32_t pending_gpu_address;
    uint32_t pending_gpu_format;
    uint32_t pending_gpu_stride;
    unsigned pending_gpu_skipped;
    bool pending_gpu;
    bool lcdc_dirty;
    bool lcdc_hash_valid;
    uint64_t lcdc_hash;
    unsigned scanout_frames;
    uint32_t dma_next_address;
    size_t dma_bytes;
    bool dma_in_progress;
    uint8_t *dma_completed_frame;
    bool dma_frame_ready;
    unsigned dma_frames;
    unsigned diagnostic_writes;
    bool trace_control;
    bool frame_requested;
    QEMUTimer *frame_event;
    qemu_irq irq;
    unsigned frame_irqs;
} MiBandLcdc;

void miband_lcdc_init(MiBandLcdc *lcdc, MiBandPanelEndpoint panel,
                      uint32_t framebuffer_bpp, uint32_t width, uint32_t height,
                      uint8_t *psram,
                      uint32_t psram_size, uint8_t *ram, uint32_t ram_size);
void miband_lcdc_set_base(MiBandLcdc *lcdc, uint32_t base);
/* The resolver owns SoC address aliases and validates the complete span.
 * A NULL result rejects the access; it does not use the default aliases. */
void miband_lcdc_set_memory(MiBandLcdc *lcdc, void *opaque,
                            uint8_t *(*memory_ptr)(void *, uint32_t, size_t));
void miband_lcdc_connect_irq(MiBandLcdc *lcdc, qemu_irq irq);
bool miband_lcdc_read(MiBandLcdc *lcdc, hwaddr address, unsigned size,
                      uint64_t *result);
bool miband_lcdc_write(MiBandLcdc *lcdc, hwaddr address, unsigned size,
                       uint64_t value);
void miband_lcdc_queue_gpu_frame(MiBandLcdc *lcdc, const uint8_t *source,
                                 uint32_t address, uint32_t format,
                                 uint32_t stride, unsigned skipped);
void miband_lcdc_note_dma_write(MiBandLcdc *lcdc, uint32_t address,
                                size_t length);

void miband_lcdc_set_async(MiBandLcdc *, bool);
void miband_lcdc_async_reset(MiBandLcdc *);
void miband_lcdc_async_shutdown(MiBandLcdc *);
#endif
