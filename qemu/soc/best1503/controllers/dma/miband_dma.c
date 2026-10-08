#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
#include "hw/core/cpu.h"
#include "sysemu/qtest.h"
#include "sysemu/runstate.h"
#include "miband_peripherals.h"
#include "miband_gpu.h"

#define GPDMA_CH0_BASE 0x40130100
#define GPDMA_CH7_BASE 0x401301e0
#define GPDMA_BASE     0x40130000
#define GPDMA_IRQ_TC   0x04
#define GPDMA_TC_CLEAR 0x08
#define GPDMA_IRQ_ERR  0x0c
#define GPDMA_ERR_CLEAR 0x10
#define GPDMA_RAW_TC   0x14
#define GPDMA_RAW_ERR  0x18
#define GPDMA_ENABLED  0x1c
#define GPDMA_GLOBAL_CONFIG 0x30
#define GPDMA_CH7_BIT  (1u << 7)
#define GPDMA_CH_ENABLE 1u
#define GPDMA_CH_TC_IRQ (1u << 15)
#define GPDMA_CH_ERR_IRQ (1u << 14)
#define GPDMA_CTRL_TC_IRQ (1u << 31)
#define GPDMA_CTRL_SRC_INC (1u << 26)
#define GPDMA_CTRL_DST_INC (1u << 27)
#define DISPLAY_DMA_MAX_BYTES (512 * 1024)
#define DISPLAY_DMA_MAX_LINKS 64
#define AP_RAM_BASE 0x20000000
#define AP_RAM_SIZE 0x00160000
#define AP_PSRAM_BASE 0x3c000000
#define AP_PSRAM_SIZE 0x02000000
#define NAND_RX_FIFO  0x40500008

struct MiBandDmaAsync {
    uint64_t generation;
    uint32_t src, dst, link, control, config;
    bool active;
    bool scheduled;
};

typedef struct MiBandDmaWork {
    MiBandDma *dma;
    uint64_t generation;
} MiBandDmaWork;

typedef struct MiBandDmaSegment {
    uint32_t src, dst, link, control;
    unsigned bytes;
} MiBandDmaSegment;

static void miband_dma_update_irq(MiBandDma *dma)
{
    qemu_set_irq(dma->irq, ((dma->tc_status & GPDMA_CH7_BIT) &&
                            (dma->display_config & GPDMA_CH_TC_IRQ)) ||
                           ((dma->error_status & GPDMA_CH7_BIT) &&
                            (dma->display_config & GPDMA_CH_ERR_IRQ)));
}

static bool miband_dma_ram_range(uint32_t address, unsigned bytes,
                                 uint32_t base, uint32_t size)
{
    return address >= base && bytes <= size && address - base <= size - bytes;
}

static void miband_dma_display_execute(CPUState *cpu, run_on_cpu_data data)
{
    MiBandDmaWork *work = data.host_ptr;
    MiBandDma *dma = work->dma;
    struct MiBandDmaAsync *async = dma->async;
    uint32_t src = async->src;
    uint32_t dst = async->dst;
    uint32_t first_src = src;
    uint32_t first_dst = dst;
    uint32_t link = async->link;
    uint32_t control = async->control;
    unsigned total = 0;
    unsigned segments = 0;
    unsigned planned = 0;
    MiBandDmaSegment plan[DISPLAY_DMA_MAX_LINKS];
    bool finished = false;

    if (work->generation != async->generation || !async->active) {
        g_free(work);
        return;
    }
    async->scheduled = false;
    g_free(work);
    if (!qtest_enabled() && !runstate_is_running()) {
        timer_mod(dma->display_event,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
        return;
    }

    /* Decode the entire supported chain before writing anything. A pending
     * GPU producer defers the transaction without a partially copied prefix. */
    for (unsigned i = 0; i < DISPLAY_DMA_MAX_LINKS; i++) {
        unsigned transfers = control & 0xfff;
        unsigned source_width = (control >> 18) & 7;
        unsigned target_width = (control >> 21) & 7;
        unsigned bytes = transfers << source_width;
        uint32_t descriptor[4];

        if (!transfers || source_width > 2 || source_width != target_width ||
            (control & (GPDMA_CTRL_SRC_INC | GPDMA_CTRL_DST_INC)) !=
                      (GPDMA_CTRL_SRC_INC | GPDMA_CTRL_DST_INC) ||
            (src & ((1u << source_width) - 1)) ||
            (dst & ((1u << target_width) - 1)) ||
            bytes > DISPLAY_DMA_MAX_BYTES - total ||
            !miband_dma_ram_range(src, bytes, AP_RAM_BASE, AP_RAM_SIZE) ||
            !miband_dma_ram_range(dst, bytes, AP_PSRAM_BASE, AP_PSRAM_SIZE)) {
            break;
        }

        if (miband_gpu_pending_memory(src, bytes) ||
            miband_gpu_pending_memory(dst, bytes) ||
            (link && miband_gpu_pending_memory(link, sizeof(descriptor)))) {
            timer_mod(dma->display_event,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
            return;
        }
        plan[planned++] = (MiBandDmaSegment) {
            .src = src, .dst = dst, .link = link,
            .control = control, .bytes = bytes,
        };
        total += bytes;

        if (!link) {
            finished = (control & GPDMA_CTRL_TC_IRQ) != 0;
            break;
        }
        if (!miband_dma_ram_range(link, sizeof(descriptor),
                                  AP_RAM_BASE, AP_RAM_SIZE) ||
            address_space_read(dma->as, link, MEMTXATTRS_UNSPECIFIED,
                               (uint8_t *)descriptor,
                               sizeof(descriptor)) != MEMTX_OK) {
            break;
        }
        src = le32_to_cpu(descriptor[0]);
        dst = le32_to_cpu(descriptor[1]);
        link = le32_to_cpu(descriptor[2]);
        control = le32_to_cpu(descriptor[3]);
    }

    total = 0;
    for (unsigned i = 0; i < planned; i++) {
        MiBandDmaSegment *segment = &plan[i];
        uint8_t *payload = g_malloc(segment->bytes);

        /* Sole-vCPU safe point plus BQL makes these guest accesses atomic
         * relative to CPU stores and owner device callbacks. AddressSpace
         * writes also retain QEMU's dirty-page tracking for every alias. */
        if (address_space_read(dma->as, segment->src, MEMTXATTRS_UNSPECIFIED,
                               payload, segment->bytes) != MEMTX_OK ||
            address_space_write(dma->as, segment->dst, MEMTXATTRS_UNSPECIFIED,
                                 payload, segment->bytes) != MEMTX_OK) {
            g_free(payload);
            finished = false;
            break;
        }
        g_free(payload);
        total += segment->bytes;
        segments++;
        dma->display_src = segment->src + segment->bytes;
        dma->display_dst = segment->dst + segment->bytes;
        dma->display_control = segment->control & ~0xfffu;
        dma->display_link = segment->link;
    }
    async->active = false;
    dma->display_config &= ~GPDMA_CH_ENABLE;
    if (finished && dma->lcdc && dma->display_dst >= first_dst &&
        dma->display_dst - first_dst == total) {
        miband_lcdc_note_dma_write(dma->lcdc, first_dst, total);
    }
    if (finished) {
        dma->tc_status |= GPDMA_CH7_BIT;
    } else {
        dma->error_status |= GPDMA_CH7_BIT;
    }
    miband_dma_update_irq(dma);
    if (dma->display_transfers++ < 256 || !finished) {
        qemu_log_mask(finished ? LOG_UNIMP : LOG_GUEST_ERROR,
                      "miband GPDMA ch7 %s segments=%u bytes=%u "
                      "src=%08x dst=%08x end_src=%08x end_dst=%08x "
                      "control=%08x irq=%u"
                      " virtual_ms=%" PRId64 "\n",
                      finished ? "complete" : "invalid chain",
                      segments, total, first_src, first_dst,
                      dma->display_src, dma->display_dst,
                      dma->display_control, finished &&
                      !!(dma->display_config & GPDMA_CH_TC_IRQ),
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
}

static void miband_dma_display_complete(void *opaque)
{
    MiBandDma *dma = opaque;
    struct MiBandDmaAsync *async = dma->async;
    MiBandDmaWork *work;

    if (!async->active || async->scheduled) {
        return;
    }
    work = g_new(MiBandDmaWork, 1);
    work->dma = dma;
    work->generation = async->generation;
    async->scheduled = true;
    if (qtest_enabled() || !first_cpu) {
        miband_dma_display_execute(first_cpu, RUN_ON_CPU_HOST_PTR(work));
    } else {
        async_run_on_cpu(first_cpu, miband_dma_display_execute,
                         RUN_ON_CPU_HOST_PTR(work));
    }
}

void miband_dma_async_reset(MiBandDma *dma)
{
    if (!dma->async) {
        return;
    }
    dma->async->generation++;
    dma->async->active = false;
    dma->async->scheduled = false;
    dma->display_config &= ~GPDMA_CH_ENABLE;
    dma->tc_status = 0;
    dma->error_status = 0;
    if (dma->display_event) {
        timer_del(dma->display_event);
    }
    if (dma->irq) {
        miband_dma_update_irq(dma);
    }
}

void miband_dma_init(MiBandDma *dma, AddressSpace *as)
{
    memset(dma, 0, sizeof(*dma));
    dma->as = as;
    dma->async = g_new0(struct MiBandDmaAsync, 1);
}

void miband_dma_connect_lcdc(MiBandDma *dma, MiBandLcdc *lcdc)
{
    dma->lcdc = lcdc;
}

void miband_dma_connect_irq(MiBandDma *dma, qemu_irq irq)
{
    dma->irq = irq;
    dma->display_event = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      miband_dma_display_complete, dma);
}

static uint32_t *miband_dma_reg(MiBandDma *dma, hwaddr address)
{
    if (address >= GPDMA_CH7_BASE && address < GPDMA_CH7_BASE + 0x14) {
        switch (address - GPDMA_CH7_BASE) {
        case 0x00: return &dma->display_src;
        case 0x04: return &dma->display_dst;
        case 0x08: return &dma->display_link;
        case 0x0c: return &dma->display_control;
        case 0x10: return &dma->display_config;
        default: return NULL;
        }
    }
    if (address < GPDMA_CH0_BASE || address >= GPDMA_CH0_BASE + 0x14) {
        return NULL;
    }
    switch (address - GPDMA_CH0_BASE) {
    case 0x00: return &dma->src;
    case 0x04: return &dma->dst;
    case 0x08: return &dma->link;
    case 0x0c: return &dma->control;
    case 0x10: return &dma->config;
    default: return NULL;
    }
}

bool miband_dma_read(MiBandDma *dma, hwaddr address, unsigned size,
                     uint64_t *result)
{
    uint32_t *reg = miband_dma_reg(dma, address);

    if (address >= GPDMA_BASE && address < GPDMA_BASE + 0x34 && size == 4) {
        switch (address - GPDMA_BASE) {
        case 0x00: *result = dma->tc_status | dma->error_status; return true;
        case GPDMA_IRQ_TC:
        case GPDMA_RAW_TC: *result = dma->tc_status; return true;
        case GPDMA_IRQ_ERR:
        case GPDMA_RAW_ERR: *result = dma->error_status; return true;
        case GPDMA_ENABLED:
            *result = (dma->display_config & GPDMA_CH_ENABLE) ?
                      GPDMA_CH7_BIT : 0;
            return true;
        case GPDMA_GLOBAL_CONFIG:
            *result = dma->global_config;
            return true;
        default: break;
        }
    }
    if (!reg || size != 4) {
        return false;
    }
    *result = *reg;
    return true;
}

bool miband_dma_write(MiBandDma *dma, hwaddr address, unsigned size,
                      uint64_t value)
{
    uint32_t *reg = miband_dma_reg(dma, address);

    if (size == 4) {
        switch (address) {
        case GPDMA_BASE + GPDMA_TC_CLEAR:
            dma->tc_status &= ~value;
            miband_dma_update_irq(dma);
            return true;
        case GPDMA_BASE + GPDMA_ERR_CLEAR:
            dma->error_status &= ~value;
            miband_dma_update_irq(dma);
            return true;
        case GPDMA_BASE + GPDMA_GLOBAL_CONFIG:
            dma->global_config = value;
            return true;
        default: break;
        }
    }
    if (!reg || size != 4) {
        return false;
    }
    if (address == GPDMA_CH7_BASE + 0x10 &&
        !(dma->display_config & GPDMA_CH_ENABLE) &&
        (value & GPDMA_CH_ENABLE) && dma->display_event) {
        dma->async->generation++;
        dma->async->active = true;
        dma->async->scheduled = false;
        dma->async->src = dma->display_src;
        dma->async->dst = dma->display_dst;
        dma->async->link = dma->display_link;
        dma->async->control = dma->display_control;
        dma->async->config = value;
        timer_mod(dma->display_event,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    } else if (address == GPDMA_CH7_BASE + 0x10 &&
               (dma->display_config & GPDMA_CH_ENABLE) &&
               !(value & GPDMA_CH_ENABLE)) {
        dma->async->generation++;
        dma->async->active = false;
        dma->async->scheduled = false;
        timer_del(dma->display_event);
    }
    *reg = value;
    return true;
}

void miband_dma_flash_rx(void *opaque, MiBandFlash *flash)
{
    MiBandDma *dma = opaque;
    unsigned words = dma->control & 0xfff;
    unsigned bytes = words * 4;
    MemTxResult result;

    const uint8_t *source = miband_flash_dma_source(flash, bytes);
    if (!(dma->config & 1) || dma->src != NAND_RX_FIFO || !source) {
        return;
    }
    result = address_space_write(dma->as, dma->dst, MEMTXATTRS_UNSPECIFIED,
                                 source, bytes);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "miband NAND DMA failed dst=%08x bytes=%u\n",
                      dma->dst, bytes);
    }
}

void miband_dma_flash_tx(void *opaque, MiBandFlash *flash)
{
    MiBandDma *dma = opaque;
    unsigned bytes = dma->control & 0xfff;
    MemTxResult result;

    uint8_t *target = miband_flash_dma_target(flash, bytes);
    if (!(dma->config & 1) || dma->dst != NAND_RX_FIFO || !target) {
        return;
    }
    result = address_space_read(dma->as, dma->src, MEMTXATTRS_UNSPECIFIED,
                                target, bytes);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "miband NAND DMA source read failed src=%08x bytes=%u\n",
                      dma->src, bytes);
    }
}
