/*
 * Minimal AP bring-up machine for BES best1502p firmware.
 *
 * This machine intentionally models only the Cortex-M33 execution envelope
 * (XIP, AP SRAM, PL011 UART and an observable MMIO window).  It is the first
 * stage of the reference integration; board devices and shared IP are added only
 * after their addresses are backed by SDK/static/runtime evidence.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "bes_runtime_config.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/char/pl011.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/loader.h"
#include "chardev/char.h"
#include "exec/address-spaces.h"
#include "qom/object.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "best1502p_nor.h"
#include "best1502p_timer.h"
#include "best1502p_ispi.h"
#include "best1502p_spi.h"
#include "best1502p_psram.h"
#include "best1502p_memsc.h"
#include "best1502p_aon_reset.h"
#include "best1502p_lpu.h"
#include "miband_gpu.h"
#include "miband_lcdc.h"
#include "miband_panel_device.h"
#include "miband_display_bridge.h"
#include "best1700_dsi.h"
#include "miband_peripherals.h"
#include "miband_flash.h"
#include "miband_nand_device.h"
#include "miband_zinitix.h"
#include "miband_touch_input.h"
#include "miband_binding_ui.h"

#define BEST1502P_XIP_BASE       0x0c000000u
#define BEST1502P_NC_BASE        0x28000000u
#define BEST1502P_C_BASE         0x2c000000u
#define BEST1502P_RAM_BASE       0x20000000u
#define BEST1502P_RAMX_BASE      0x00200000u
#define BEST1502P_RAM_SIZE       0x00200000u /* SDK RAM0..RAM9 */
#define BEST1502P_FLASH_SIZE     0x01000000u /* AP FLASH_SIZE string/SDK window */
#define BEST1502P_PSRAM_BASE     0x3c000000u
#define BEST1502P_PSRAM_SIZE     0x02000000u /* capacity remains an explicit TODO */
#define BEST1502P_PSRAM_NC_BASE  0x38000000u
#define BEST1502P_PSRAM_C_BASE   0x18000000u
#define BEST1502P_PSRAM_X_BASE   0x1c000000u
#define BEST1502P_BT_RAM_BASE    0xc0000000u
#define BEST1502P_BT_RAM_SIZE    0x00010000u
#define BEST1502P_BTC_RAM_BASE   0xa0000000u
#define BEST1502P_BTC_RAM_SIZE   0x00200000u
#define BEST1502P_BT_CPUDUMP_BASE 0xd0700000u
#define BEST1502P_BT_CPUDUMP_SIZE 0x00010000u
#define BEST1502P_BT_REG_BASE    0xd0220000u
#define BEST1502P_BT_REG_SIZE    0x00010000u
#define BEST1502P_BT_EXCH_BASE   0xd0200000u
#define BEST1502P_BT_EXCH_SIZE   0x00020000u
#define BEST1502P_BT_DIGITAL_BASE 0xd0310000u
#define BEST1502P_BT_CMU_BASE    0xd0330000u
#define BEST1502P_BT_RF_BASE     0xd0350000u
#define BEST1502P_MMIO_BASE      0x40000000u
#define BEST1502P_MMIO_SIZE      0x20000000u
#define BEST1502P_ICACHE_BASE    0x03000000u
#define BEST1502P_DCACHE_BASE    0x23000000u
#define BEST1502P_CACHE_SIZE     0x01000000u
#define BEST1502P_MPU_REGIONS    8u
#define BEST1502P_UART1_BASE     0x4000c000u
#define BEST1502P_UART2_BASE     0x4000d000u
#define BEST1502P_TIMER1_BASE    0x40084000u
#define BEST1502P_TIMER0_BASE    0x40003000u
#define BEST1502P_TIMER1_SIZE    0x00000100u
#define BEST1502P_AON_MEMSC_BASE 0x40080024u
#define BEST1502P_AON_WDT_BASE   0x40082000u
#define BEST1502P_AON_WDT_SIZE   0x00001000u
#define BEST1502P_CMU_MEMSC_BASE 0x400000c0u

typedef struct Best1502pMachineState Best1502pMachineState;

typedef struct Best1502pMmio {
    Best1502pMachineState *machine;
    hwaddr base;
    uint32_t words[0x2000];
    uint32_t traces;
} Best1502pMmio;

typedef struct Best1502pUartIrq {
    PL011State *uart;
    qemu_irq nvic;
} Best1502pUartIrq;

static void best1502p_uart_irq(void *opaque, int number, int level)
{
    Best1502pUartIrq *route = opaque;
    PL011State *uart = route->uart;

    /* BES acknowledges RX while a new FIFO batch can already be pending.
     * RX is a level: ICR must not strand unread bytes. Upstream PL011 8.2
     * clears the latch unconditionally; qualify this behavior only here. */
    if (uart->read_count >= uart->read_trigger) {
        uart->int_level |= 1u << 4;
    }
    qemu_set_irq(route->nvic, (uart->int_level & uart->int_enabled) != 0);
}

struct Best1502pMachineState {
    MachineState parent;
    ARMv7MState armv7m;
    PL011State uart1;
    PL011State uart2;
    Best1502pUartIrq uart_irq[2];
    MemoryRegion vectors;
    MemoryRegion flash;
    MemoryRegion flash_nc;
    MemoryRegion flash_c;
    MemoryRegion ram;
    MemoryRegion ramx;
    MemoryRegion psram;
    MemoryRegion psram_nc;
    MemoryRegion psram_c;
    MemoryRegion psram_x;
    MemoryRegion bt_ram;
    MemoryRegion btc_ram;
    MemoryRegion bt_cpudump;
    Best1502pMmio bt_regs_state;
    MemoryRegion bt_regs;
    MemoryRegion bt_exchange;
    Best1502pMmio bt_digital_state;
    MemoryRegion bt_digital;
    Best1502pMmio bt_cmu_state;
    MemoryRegion bt_cmu;
    Best1502pMmio bt_rf_state;
    MemoryRegion bt_rf;
    MemoryRegion psram_ctrl;
    MemoryRegion mmio;
    MemoryRegion icache_mmio;
    MemoryRegion dcache_mmio;
    Best1502pMmio mmio_state;
    Best1502pMmio icache_state;
    Best1502pMmio dcache_state;
    Best1502pTimer timer1_state;
    MemoryRegion timer1;
    Best1502pTimer timer0_state;
    MemoryRegion timer0;
    Best1502pIspi ispi_state;
    Best1502pLpu lpu;
    uint32_t gpadc_input[8];
    MemoryRegion ispi;
    Best1502pSpi spi0_state;
    Best1502pNor nor_state;
    MemoryRegion spi0;
    Best1502pPsram psram_ctrl_state;
    Best1502pMemsc aon_memsc_state;
    Best1502pMemsc cmu_memsc_state;
    Best1502pAonReset aon_reset;
    MemoryRegion aon_memsc;
    MemoryRegion cmu_memsc;
    Best1502pMmio aon_wdt_state;
    MemoryRegion aon_wdt;
    Clock *sysclk;
    Clock *refclk;
    uint32_t boot_sp;
    uint32_t boot_pc;
    uint32_t flash_offset;
    uint32_t display_width, display_height, panel_id;
    uint32_t display_te_pin;
    MiBandGpio gpio;
    MiBandGpu gpu;
    MiBandLcdc lcdc;
    MiBandPanelDevice *panel;
    MiBandDisplayBridge *display_bridge;
    Best1700DsiState dsi;
    MemoryRegion gpu_region, lcdc_region;
    char *resource_image;
    GMappedFile *resource_map;
    MiBandNandDevice *nand;
    char *flash_state;
    char *firmware_profile;
    bool diag_skip_binding, initialized;
    MiBandStore *store;
    MiBandFlash nand_ctrl[MIBAND_FLASH_COUNT];
    MiBandDma nand_dma;
    MemoryRegion nand_cache, nand_cache_c;
    MemoryRegion nand_region;
    Best1502pMmio nand_mmio_state;
    uint32_t nand_id, nand_page_size, nand_spare_size;
    uint32_t nand_page_count, nand_pages_per_block;
    MiBandI2c touch_i2c[MIBAND_I2C_COUNT];
    MiBandZinitix touch;
    MiBandTouchInput touch_input;
    uint32_t touch_chip_id, touch_i2c_bus, touch_address, touch_irq_pin;
    uint32_t touch_sleep_gesture;
};

#define TYPE_BEST1502P_MACHINE MACHINE_TYPE_NAME("miband-best1502p")
OBJECT_DECLARE_SIMPLE_TYPE(Best1502pMachineState, BEST1502P_MACHINE)

/* Opt-in diagnostic wiring of the qualified VG-Lite 0x265/LCDC apertures.
 * Board dimensions come from launch configuration. Only guest-rendered
 * pixels reach the host display bridge. */
static uint64_t best1502p_gpu_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1502pMachineState *s = opaque;
    uint64_t value = 0;
    miband_gpu_read(&s->gpu, 0x40220000 + addr, size, &value);
    return value;
}

static void best1502p_gpu_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    Best1502pMachineState *s = opaque;
    miband_gpu_write(&s->gpu, 0x40220000 + addr, size, value);
}

static uint64_t best1502p_lcdc_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1502pMachineState *s = opaque;
    uint64_t value = 0;
    miband_lcdc_read(&s->lcdc, 0x40200000 + addr, size, &value);
    return value;
}

static void best1502p_lcdc_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    Best1502pMachineState *s = opaque;
    miband_lcdc_write(&s->lcdc, 0x40200000 + addr, size, value);
}

static const MemoryRegionOps best1502p_gpu_ops = {
    .read = best1502p_gpu_read, .write = best1502p_gpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static const MemoryRegionOps best1502p_lcdc_ops = {
    .read = best1502p_lcdc_read, .write = best1502p_lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
};

static bool best1502p_dsi_panel_write(void *opaque, uint8_t command,
                                      const uint8_t *data, size_t length)
{
    Best1502pMachineState *s = opaque;
    return miband_panel_dcs_write(s->panel, command, data, length);
}

static int best1502p_dsi_panel_read(void *opaque, uint8_t command,
                                   uint8_t *data, size_t capacity)
{
    Best1502pMachineState *s = opaque;
    return miband_panel_dcs_read(s->panel, command, data, capacity);
}

static void best1502p_panel_te(void *opaque)
{
    Best1502pMachineState *s = opaque;
    if (s->display_te_pin < 96) {
        miband_gpio_raise_edge(&s->gpio, s->display_te_pin);
    }
}

static const MemoryRegionOps best1502p_nand_ops;

static void best1502p_touch_irq(void *opaque, bool asserted)
{
    Best1502pMachineState *s = opaque;
    miband_gpio_set_input(&s->gpio, s->touch_irq_pin, !asserted);
    if (asserted) {
        miband_gpio_raise_edge(&s->gpio, s->touch_irq_pin);
    }
}

static void best1502p_nand_init(Best1502pMachineState *s, MemoryRegion *sysmem)
{
    if (!s->resource_image) {
        return;
    }
    /* Experimental external NAND binding: geometry and identity are supplied
     * by the board launch profile, never inferred from the resource file. */
    if (!s->nand_id || s->nand_id > 0xffffff ||
        s->nand_page_size != 2048 || s->nand_spare_size > 128 ||
        !s->nand_pages_per_block ||
        (s->nand_pages_per_block & (s->nand_pages_per_block - 1)) ||
        !s->nand_page_count || s->nand_page_count > 0x1000000 ||
        s->nand_page_count % s->nand_pages_per_block) {
        error_report("resource-image requires qualified 2KiB NAND geometry and JEDEC ID");
        exit(1);
    }
    GError *err = NULL;
    s->resource_map = g_mapped_file_new(s->resource_image, false, &err);
    if (!s->resource_map) {
        error_report("cannot map resource image: %s", err->message);
        g_error_free(err);
        exit(1);
    }
    size_t length = g_mapped_file_get_length(s->resource_map);
    const uint8_t *image = (const uint8_t *)g_mapped_file_get_contents(s->resource_map);
    if (length < 16 || memcmp(image, "-rom1fs-", 8) ||
        length > (uint64_t)s->nand_page_size * s->nand_page_count) {
        error_report("resource-image must be an in-band ROMFS fitting the NAND");
        exit(1);
    }
    MiBandNandGeometry geometry = {
        .page_size = s->nand_page_size, .spare_size = s->nand_spare_size,
        .pages_per_block = s->nand_pages_per_block,
        .page_count = s->nand_page_count,
        .jedec_id = {s->nand_id >> 16, s->nand_id >> 8, s->nand_id},
    };
    s->nand = miband_nand_new(&geometry);
    miband_nand_set_resource(s->nand, image, length, 0, false);
    memory_region_init_ram(&s->nand_cache, NULL, "best1502p.nand-cache",
                           0x10000, &error_fatal);
    memory_region_add_subregion(sysmem, 0x30000000, &s->nand_cache);
    memory_region_init_alias(&s->nand_cache_c, NULL, "best1502p.nand-cache-c",
                             &s->nand_cache, 0, 0x10000);
    memory_region_add_subregion(sysmem, 0x34000000, &s->nand_cache_c);
    miband_flash_init(&s->nand_ctrl[1], miband_nand_endpoint(s->nand),
                       memory_region_get_ram_ptr(&s->nand_cache), 0x10000);
    /* reference native bread trace: GPDMA ch0, FIFO 0x40500008 -> SRAM,
     * control 0x88089200 (512 words). Keep this opt-in with the NAND. */
    miband_dma_init(&s->nand_dma, &address_space_memory);
    miband_flash_connect_dma(&s->nand_ctrl[1], (MiBandFlashDmaCallbacks) {
        .opaque = &s->nand_dma,
        .read = miband_dma_flash_rx, .write = miband_dma_flash_tx,
    });
    s->nand_mmio_state = (Best1502pMmio) {.machine = s, .base = 0x40500000};
    memory_region_init_io(&s->nand_region, OBJECT(s), &best1502p_nand_ops,
                          &s->nand_mmio_state, "best1502p.nand-controller", 0x1000);
    memory_region_add_subregion_overlap(sysmem, 0x40500000, &s->nand_region, 2);
}

static char *best1502p_get_resource(Object *obj, Error **errp)
{
    return g_strdup(BEST1502P_MACHINE(obj)->resource_image);
}

static char *best1502p_get_flash_state(Object *obj, Error **errp)
{
    return g_strdup(BEST1502P_MACHINE(obj)->flash_state);
}

static void best1502p_set_flash_state(Object *obj, const char *value, Error **errp)
{
    Best1502pMachineState *s = BEST1502P_MACHINE(obj);
    g_free(s->flash_state);
    s->flash_state = g_strdup(value);
}

static void best1502p_restore(void *opaque, unsigned type, unsigned address,
                              const uint8_t *data, unsigned size)
{
    Best1502pMachineState *s = opaque;
    if (type == MB_STORE_PAGE) {
        miband_nand_restore_page(s->nand, address, data, size);
    } else if (type == MB_STORE_ERASE) {
        miband_nand_restore_erase(s->nand, address);
    }
    /* NOR is currently read-only. Its journal seed binds the AP contents;
     * guest NOR writes remain unsupported rather than being acknowledged. */
}

static void best1502p_open_storage(Best1502pMachineState *s)
{
    if (!s->flash_state) {
        return;
    }
    if (!s->nand || !miband_nand_store_compatible(s->nand) ||
        s->nand_pages_per_block != 64) {
        error_report("reference persistent storage requires compatible external NAND");
        exit(EXIT_FAILURE);
    }
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    uint8_t digest[32];
    gsize length = sizeof(digest);
    char *image = NULL;
    gsize image_size = 0;
    if (!g_file_get_contents(MACHINE(s)->kernel_filename, &image, &image_size, NULL) ||
        image_size > BEST1502P_FLASH_SIZE - s->flash_offset) {
        error_report("cannot read AP seed for reference storage");
        exit(EXIT_FAILURE);
    }
    uint8_t *ap = g_malloc(BEST1502P_FLASH_SIZE);
    memset(ap, 0xff, BEST1502P_FLASH_SIZE);
    memcpy(ap + s->flash_offset, image, image_size);
    g_free(image);
    g_checksum_update(sum, (const guchar *)"best1502p-nand-v1", 16);
    g_checksum_update(sum, ap, BEST1502P_FLASH_SIZE);
    g_checksum_update(sum, (const guchar *)g_mapped_file_get_contents(s->resource_map),
                       g_mapped_file_get_length(s->resource_map));
    g_checksum_get_digest(sum, digest, &length);
    g_checksum_free(sum);
    s->store = miband_store_open_geometry(s->flash_state, digest, ap,
        BEST1502P_FLASH_SIZE, s->nand_page_size + s->nand_spare_size,
        s->nand_page_count, best1502p_restore, s);
    g_free(ap);
    miband_nand_set_store(s->nand, s->store);
    best1502p_ispi_open_rtc(&s->ispi_state, s->flash_state, digest);
}

static void best1502p_set_resource(Object *obj, const char *value, Error **errp)
{
    Best1502pMachineState *s = BEST1502P_MACHINE(obj);
    g_free(s->resource_image);
    s->resource_image = g_strdup(value);
}

static void best1502p_display_init(Best1502pMachineState *s, MemoryRegion *sysmem)
{
    if (!s->display_width && !s->display_height) {
        return;
    }
    if (!s->display_width || !s->display_height ||
        s->display_width > 1024 || s->display_height > 1024) {
        error_report("reference display dimensions must both be in 1..1024");
        exit(1);
    }
    s->display_bridge = miband_display_bridge_new(s->display_width,
                                                  s->display_height);
    if (s->panel_id > 0xffffff) {
        error_report("panel-id must fit the three DCS identity bytes");
        exit(1);
    }
    s->panel = miband_panel_new(s->panel_id, s->display_width, s->display_height);
    /* reference trace: GPIO bank2 bit1 (pin65), NVIC priority44/enable44.
     * The board pin is opt-in; use the guest's actual enable/mask/ACK. */
    miband_gpio_connect_irq(&s->gpio,
                            qdev_get_gpio_in(DEVICE(&s->armv7m), 44));
    miband_panel_connect_te(s->panel, s, best1502p_panel_te);
    miband_panel_connect_display(s->panel, s->display_bridge,
                                 miband_display_bridge_present);
    miband_lcdc_init(&s->lcdc, miband_panel_endpoint(s->panel), 24,
                     s->display_width, s->display_height,
                     memory_region_get_ram_ptr(&s->psram), BEST1502P_PSRAM_SIZE,
                     memory_region_get_ram_ptr(&s->ram), BEST1502P_RAM_SIZE);
    miband_lcdc_connect_irq(&s->lcdc,
                            qdev_get_gpio_in(DEVICE(&s->armv7m), 67));
    memory_region_init_io(&s->lcdc_region, OBJECT(s), &best1502p_lcdc_ops,
                          s, "best1502p.lcdc", 0x400);
    memory_region_add_subregion_overlap(sysmem, 0x40200000,
                                        &s->lcdc_region, 4);
    miband_gpu_init(&s->gpu,
                    memory_region_get_ram_ptr(&s->psram), BEST1502P_PSRAM_SIZE,
                    memory_region_get_ram_ptr(&s->ram), BEST1502P_RAM_SIZE,
                    false, true);
    /* BEST1502P SDK gcFEATURE_VG_MATH_PRECISION_FIX; also observed in
     * native AP texture matrices (identity step=1 rather than 1/width). */
    s->gpu.texture_texel_coordinates = true;
    miband_gpu_connect_irq(&s->gpu, qdev_get_gpio_in(DEVICE(&s->armv7m), 70));
    miband_gpu_connect_lcdc(&s->gpu, &s->lcdc, false);
    memory_region_init_io(&s->gpu_region, OBJECT(s), &best1502p_gpu_ops,
                          s, "best1502p.gpu", 0x1000);
    memory_region_add_subregion_overlap(sysmem, 0x40220000,
                                        &s->gpu_region, 4);
    /* The observed reference read/write packet and FIFO layouts match this DSI
     * transport. Panel state stays board-owned; its IRQ route is unqualified. */
    object_initialize_child(OBJECT(s), "dsi", &s->dsi, TYPE_BEST1700_DSI);
    Best1700DsiPanelEndpoint endpoint = {
        .opaque = s, .write = best1502p_dsi_panel_write,
        .read = best1502p_dsi_panel_read,
    };
    best1700_dsi_connect_panel(&s->dsi, &endpoint);
    sysbus_realize(SYS_BUS_DEVICE(&s->dsi), &error_fatal);
    memory_region_add_subregion_overlap(sysmem, 0x40230000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dsi), 0), 4);
    if (s->touch_chip_id) {
        if (s->touch_chip_id > UINT16_MAX || s->touch_i2c_bus >= MIBAND_I2C_COUNT ||
            !s->touch_address || s->touch_address > 0x7f || s->touch_irq_pin >= 96) {
            error_report("reference touch requires explicit chip-id, I2C bus/address and GPIO pin");
            exit(1);
        }
        miband_i2c_init(s->touch_i2c);
        miband_zinitix_init(&s->touch, s->touch_chip_id, best1502p_touch_irq, s);
        if (s->touch_sleep_gesture > 1) {
            error_report("touch-sleep-gesture must be 0 (AF protocol) or 1 (command5 double tap)");
            exit(1);
        }
        s->touch.sleep_gesture = s->touch_sleep_gesture;
        miband_gpio_set_input(&s->gpio, s->touch_irq_pin, true);
        s->touch_i2c[s->touch_i2c_bus].endpoint =
            miband_zinitix_endpoint(&s->touch, s->touch_address);
        miband_touch_input_init(&s->touch_input, miband_zinitix_report, &s->touch,
                                s->display_width, s->display_height, NULL);
        s->touch_input.report_clocked = miband_zinitix_report_clocked;
    }
}

static uint64_t best1502p_mmio_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    Best1502pMmio *mmio = opaque;
    uint32_t index = offset >> 2;
    uint32_t value = index < ARRAY_SIZE(mmio->words) ? mmio->words[index] : 0;
    uint64_t gpio_value;

    if (mmio->machine->touch_chip_id &&
        miband_i2c_read(mmio->machine->touch_i2c, mmio->base + offset,
                        size, &gpio_value)) {
        return gpio_value;
    }
    if (mmio->machine->nand &&
        miband_dma_read(&mmio->machine->nand_dma, mmio->base + offset,
                        size, &gpio_value)) {
        return gpio_value;
    }
    if (mmio->machine->nand && mmio->base + offset >= 0x40500000 &&
        mmio->base + offset < 0x40501000 &&
        miband_flash_read(mmio->machine->nand_ctrl, mmio->base + offset,
                          mmio->base + offset == 0x40500010 ? 4 : size, &gpio_value)) {
        return gpio_value;
    }

    if (mmio->machine->display_width &&
        miband_gpio_read(&mmio->machine->gpio, mmio->base + offset,
                         size, &gpio_value)) {
        return gpio_value;
    }

    if (size != 4 || (offset & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid MMIO read offset=%08" HWADDR_PRIx
                      " size=%u\n", offset, size);
        return 0;
    }
    if (mmio->traces++ < 4096) {
        uint32_t pc = current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;
        qemu_log_mask(LOG_UNIMP,
                      "best1502p MMIO read %08" HWADDR_PRIx
                      " -> %08x pc=%08x\n", mmio->base + offset, value,
                      pc);
    }
    return value;
}

static void best1502p_mmio_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Best1502pMmio *mmio = opaque;
    uint32_t index = offset >> 2;

    if (mmio->machine->touch_chip_id &&
        miband_i2c_write(mmio->machine->touch_i2c, mmio->base + offset,
                         size, value)) {
        return;
    }
    if (mmio->machine->nand &&
        miband_dma_write(&mmio->machine->nand_dma, mmio->base + offset,
                         size, value)) {
        return;
    }
    if (mmio->machine->nand && mmio->base + offset >= 0x40500000 &&
        mmio->base + offset < 0x40501000 &&
        size <= 4) {
        bool handled = miband_flash_write(mmio->machine->nand_ctrl,
                                          mmio->base + offset, size, value);
        if (handled || (mmio->base + offset == 0x40500008 && size < 4)) {
            return;
        }
    }

    if (mmio->machine->display_width &&
        miband_gpio_write(&mmio->machine->gpio, mmio->base + offset,
                          size, value)) {
        return;
    }

    if (size != 4 || (offset & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid MMIO write offset=%08" HWADDR_PRIx
                      " size=%u value=%08" PRIx64 "\n", offset, size, value);
        return;
    }
    if (index < ARRAY_SIZE(mmio->words)) {
        mmio->words[index] = value;
    }
    if (mmio->traces++ < 4096) {
        uint32_t pc = current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;
        qemu_log_mask(LOG_UNIMP,
                      "best1502p MMIO write %08" HWADDR_PRIx " <- %08" PRIx64
                      " pc=%08x\n", mmio->base + offset, value, pc);
    }
}

static const MemoryRegionOps best1502p_mmio_ops = {
    .read = best1502p_mmio_read,
    .write = best1502p_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps best1502p_nand_ops = {
    .read = best1502p_mmio_read,
    .write = best1502p_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t best1502p_ispi_mmio_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    Best1502pIspi *ispi = opaque;
    uint64_t value = 0;

    if (!best1502p_ispi_read(ispi, offset, size, &value)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid ISPI read offset=%08" HWADDR_PRIx
                      " size=%u\n", offset, size);
    }
    return value;
}

static void best1502p_ispi_mmio_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    Best1502pIspi *ispi = opaque;

    if (!best1502p_ispi_write(ispi, offset, size, value)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid ISPI write offset=%08" HWADDR_PRIx
                      " size=%u value=%08" PRIx64 "\n", offset, size,
                      value);
    }
}

static const MemoryRegionOps best1502p_ispi_ops = {
    .read = best1502p_ispi_mmio_read,
    .write = best1502p_ispi_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t best1502p_spi_mmio_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    Best1502pSpi *spi = opaque;
    uint64_t value = 0;

    if (!best1502p_spi_read(spi, offset, size, &value)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid SPI read offset=%08" HWADDR_PRIx
                      " size=%u\n", offset, size);
    }
    return value;
}

static void best1502p_spi_mmio_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    Best1502pSpi *spi = opaque;

    if (!best1502p_spi_write(spi, offset, size, value)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid SPI write offset=%08" HWADDR_PRIx
                      " size=%u value=%08" PRIx64 "\n", offset, size,
                      value);
    }
}

static const MemoryRegionOps best1502p_spi_ops = {
    .read = best1502p_spi_mmio_read,
    .write = best1502p_spi_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t best1502p_psram_mmio_read(void *opaque, hwaddr offset,
                                          unsigned size)
{
    Best1502pPsram *psram = opaque;
    uint64_t value = 0;

    if (!best1502p_psram_read(psram, offset, size, &value)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid PSRAM read offset=%08" HWADDR_PRIx
                      " size=%u\n", offset, size);
    }
    return value;
}

static void best1502p_psram_mmio_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size)
{
    Best1502pPsram *psram = opaque;

    if (!best1502p_psram_write(psram, offset, size, value)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid PSRAM write offset=%08" HWADDR_PRIx
                      " size=%u value=%08" PRIx64 "\n", offset, size,
                      value);
    }
}

static const MemoryRegionOps best1502p_psram_ops = {
    .read = best1502p_psram_mmio_read,
    .write = best1502p_psram_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t best1502p_memsc_mmio_read(void *opaque, hwaddr offset,
                                          unsigned size)
{
    Best1502pMemsc *memsc = opaque;
    uint64_t value = 0;

    if (!best1502p_memsc_read(memsc, offset, size, &value)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid MEMSC read offset=%08" HWADDR_PRIx
                      " size=%u\n", offset, size);
    }
    return value;
}

static void best1502p_memsc_mmio_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size)
{
    Best1502pMemsc *memsc = opaque;

    if (!best1502p_memsc_write(memsc, offset, size, value)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p invalid MEMSC write offset=%08" HWADDR_PRIx
                      " size=%u value=%08" PRIx64 "\n", offset, size,
                      value);
    }
}

static const MemoryRegionOps best1502p_memsc_ops = {
    .read = best1502p_memsc_mmio_read,
    .write = best1502p_memsc_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void best1502p_get_vector(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Best1502pMachineState *s = BEST1502P_MACHINE(obj);
    uint32_t value = !strcmp(name, "boot-sp") ? s->boot_sp : s->boot_pc;
    visit_type_uint32(v, name, &value, errp);
}

static void best1502p_set_vector(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Best1502pMachineState *s = BEST1502P_MACHINE(obj);
    uint32_t value;
    bool stack = !strcmp(name, "boot-sp");

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (stack) {
        if ((value & 7) || value < BEST1502P_RAM_BASE ||
            value > BEST1502P_RAM_BASE + BEST1502P_RAM_SIZE) {
            error_setg(errp, "boot-sp must be aligned inside best1502p AP RAM");
            return;
        }
        s->boot_sp = value;
    } else {
        uint32_t pc = value & ~1u;
        if (!(value & 1) ||
            !((pc >= BEST1502P_XIP_BASE && pc < BEST1502P_XIP_BASE + BEST1502P_FLASH_SIZE) ||
              (pc >= BEST1502P_NC_BASE && pc < BEST1502P_NC_BASE + BEST1502P_FLASH_SIZE) ||
              (pc >= BEST1502P_C_BASE && pc < BEST1502P_C_BASE + BEST1502P_FLASH_SIZE))) {
            error_setg(errp, "boot-pc must be a Thumb address in best1502p XIP");
            return;
        }
        s->boot_pc = value;
    }
}

static void best1502p_get_flash_offset(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    visit_type_uint32(v, name, &BEST1502P_MACHINE(obj)->flash_offset, errp);
}

static void best1502p_set_flash_offset(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Best1502pMachineState *s = BEST1502P_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value >= BEST1502P_FLASH_SIZE || value & 0xfff) {
        error_setg(errp, "flash-offset must be a 4 KiB aligned AP offset");
        return;
    }
    s->flash_offset = value;
}

static char *best1502p_get_firmware(Object *obj, Error **errp)
{
    return g_strdup(BEST1502P_MACHINE(obj)->firmware_profile);
}

static void best1502p_set_firmware(Object *obj, const char *value, Error **errp)
{
    Best1502pMachineState *s = BEST1502P_MACHINE(obj);
    if (s->initialized) {
        error_setg(errp, "firmware-profile is startup-only"); return;
    }
    g_free(s->firmware_profile);
    s->firmware_profile = g_strdup(value);
}

static bool best1502p_get_skip_binding(Object *obj, Error **errp)
{
    return BEST1502P_MACHINE(obj)->diag_skip_binding;
}

static void best1502p_set_skip_binding(Object *obj, bool value, Error **errp)
{
    Best1502pMachineState *s = BEST1502P_MACHINE(obj);
    if (s->initialized) {
        error_setg(errp, "diag-skip-binding is startup-only"); return;
    }
    s->diag_skip_binding = value;
}

static void best1502p_init(MachineState *machine)
{
    bes_runtime_config_lock();
    Best1502pMachineState *s = BEST1502P_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *armv7m;
    const uint32_t vectors[] = { s->boot_sp, s->boot_pc };
    uint32_t code_base = BEST1502P_XIP_BASE + s->flash_offset;

    if (!machine->kernel_filename) {
        error_report("miband-best1502p requires -kernel vela_ap.bin");
        exit(EXIT_FAILURE);
    }
    if ((s->boot_pc & ~1u) < code_base ||
        (s->boot_pc & ~1u) >= BEST1502P_XIP_BASE + BEST1502P_FLASH_SIZE) {
        error_setg(&error_fatal, "boot-pc must lie in selected best1502p XIP");
        return;
    }

    /* reference's early loader patches the exception table at +0x40.  Keep this
     * bootstrap page writable; the supplied SP/PC are the initial reset
     * values and are not a replacement for the firmware's own vector setup. */
    memory_region_init_ram(&s->vectors, NULL, "best1502p.vectors",
                           0x1000, &error_fatal);
    memory_region_add_subregion(sysmem, 0, &s->vectors);
    memcpy(memory_region_get_ram_ptr(&s->vectors), vectors, sizeof(vectors));

    memory_region_init_rom(&s->flash, NULL, "best1502p.ap-xip",
                           BEST1502P_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BEST1502P_XIP_BASE, &s->flash);
    memset(memory_region_get_ram_ptr(&s->flash), 0xff, BEST1502P_FLASH_SIZE);
    memory_region_init_alias(&s->flash_nc, NULL, "best1502p.ap-nc",
                             &s->flash, 0, BEST1502P_FLASH_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_NC_BASE, &s->flash_nc);
    memory_region_init_alias(&s->flash_c, NULL, "best1502p.ap-cached",
                             &s->flash, 0, BEST1502P_FLASH_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_C_BASE, &s->flash_c);

    memory_region_init_ram(&s->ram, NULL, "best1502p.ap-ram",
                           BEST1502P_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BEST1502P_RAM_BASE, &s->ram);
    memory_region_init_alias(&s->ramx, NULL, "best1502p.ap-ramx",
                             &s->ram, 0, BEST1502P_RAM_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_RAMX_BASE, &s->ramx);
    memory_region_init_ram(&s->psram, NULL, "best1502p.ap-psram",
                           BEST1502P_PSRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BEST1502P_PSRAM_BASE, &s->psram);
    memory_region_init_alias(&s->psram_nc, NULL, "best1502p.ap-psram-nc",
                             &s->psram, 0, BEST1502P_PSRAM_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_PSRAM_NC_BASE, &s->psram_nc);
    memory_region_init_alias(&s->psram_c, NULL, "best1502p.ap-psram-c",
                             &s->psram, 0, BEST1502P_PSRAM_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_PSRAM_C_BASE, &s->psram_c);
    memory_region_init_alias(&s->psram_x, NULL, "best1502p.ap-psram-x",
                             &s->psram, 0, BEST1502P_PSRAM_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_PSRAM_X_BASE, &s->psram_x);

    /* The AP firmware shares a small Bluetooth controller RAM aperture at
     * 0xc0000000.  reference reaches it through the same address used by the
     * BEST1503 board model; leave the contents in a distinct RAM region so
     * AP/BT hand-off data survives normal guest accesses. */
    memory_region_init_ram(&s->bt_ram, NULL, "best1502p.bt-ram",
                           BEST1502P_BT_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BEST1502P_BT_RAM_BASE, &s->bt_ram);

    /* The Bluetooth controller has a separate 2 MiB RAM aperture.  The
     * firmware copies its controller image here before bringing up RPTUN;
     * keep it independent from the AP-visible BT exchange RAM. */
    memory_region_init_ram(&s->btc_ram, NULL, "best1502p.btc-ram",
                           BEST1502P_BTC_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BEST1502P_BTC_RAM_BASE, &s->btc_ram);

    /* The controller publishes its crash/register snapshot through this
     * small shared window during startup. */
    memory_region_init_ram(&s->bt_cpudump, NULL, "best1502p.bt-cpudump",
                           BEST1502P_BT_CPUDUMP_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BEST1502P_BT_CPUDUMP_BASE, &s->bt_cpudump);

    /* Keep the controller register aperture observable while its detailed
     * BT IP model is brought up from SDK evidence. */
    s->bt_regs_state = (Best1502pMmio) { .machine = s,
                                         .base = BEST1502P_BT_REG_BASE };
    memory_region_init_io(&s->bt_regs, NULL, &best1502p_mmio_ops,
                          &s->bt_regs_state, "best1502p.bt-registers",
                          BEST1502P_BT_REG_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_BT_REG_BASE, &s->bt_regs);

    memory_region_init_ram(&s->bt_exchange, NULL, "best1502p.bt-exchange",
                           BEST1502P_BT_EXCH_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BEST1502P_BT_EXCH_BASE, &s->bt_exchange);

    s->bt_digital_state = (Best1502pMmio) { .machine = s,
                                             .base = BEST1502P_BT_DIGITAL_BASE };
    memory_region_init_io(&s->bt_digital, NULL, &best1502p_mmio_ops,
                          &s->bt_digital_state, "best1502p.bt-digital",
                          BEST1502P_BT_REG_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_BT_DIGITAL_BASE, &s->bt_digital);

    s->bt_cmu_state = (Best1502pMmio) { .machine = s,
                                        .base = BEST1502P_BT_CMU_BASE };
    memory_region_init_io(&s->bt_cmu, NULL, &best1502p_mmio_ops,
                          &s->bt_cmu_state, "best1502p.bt-cmu",
                          BEST1502P_BT_REG_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_BT_CMU_BASE, &s->bt_cmu);

    s->bt_rf_state = (Best1502pMmio) { .machine = s,
                                       .base = BEST1502P_BT_RF_BASE };
    memory_region_init_io(&s->bt_rf, NULL, &best1502p_mmio_ops,
                          &s->bt_rf_state, "best1502p.bt-rf",
                          BEST1502P_BT_REG_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_BT_RF_BASE, &s->bt_rf);

    best1502p_psram_init(&s->psram_ctrl_state,
                         BEST1502P_PSRAM_CTRL_BASE,
                         memory_region_get_ram_ptr(&s->psram),
                         BEST1502P_PSRAM_SIZE);
    memory_region_init_io(&s->psram_ctrl, NULL, &best1502p_psram_ops,
                          &s->psram_ctrl_state, "best1502p.psram-controller",
                          BEST1502P_PSRAM_CTRL_SIZE);
    memory_region_add_subregion_overlap(sysmem, BEST1502P_PSRAM_CTRL_BASE,
                                        &s->psram_ctrl, 3);

    best1502p_memsc_init(&s->aon_memsc_state, BEST1502P_AON_MEMSC_BASE, 0);
    memory_region_init_io(&s->aon_memsc, NULL, &best1502p_memsc_ops,
                          &s->aon_memsc_state, "best1502p.aon-memsc",
                          BEST1502P_MEMSC_COUNT * sizeof(uint32_t));
    memory_region_add_subregion_overlap(sysmem, BEST1502P_AON_MEMSC_BASE,
                                        &s->aon_memsc, 3);
    best1502p_memsc_init(&s->cmu_memsc_state, BEST1502P_CMU_MEMSC_BASE, 1);
    memory_region_init_io(&s->cmu_memsc, NULL, &best1502p_memsc_ops,
                          &s->cmu_memsc_state, "best1502p.cmu-memsc",
                          BEST1502P_MEMSC_COUNT * sizeof(uint32_t));
    memory_region_add_subregion_overlap(sysmem, BEST1502P_CMU_MEMSC_BASE,
                                        &s->cmu_memsc, 3);

    /* reference firmware uses the AON watchdog aperture during clock/power setup.
     * It writes the lock and control words, then reads them back while
     * deciding whether the watchdog is available.  Keep this small window
     * stateful instead of letting the broad fallback MMIO return zero. */
    s->aon_wdt_state = (Best1502pMmio) { .machine = s,
                                        .base = BEST1502P_AON_WDT_BASE };
    memory_region_init_io(&s->aon_wdt, NULL, &best1502p_mmio_ops,
                          &s->aon_wdt_state, "best1502p.aon-wdt",
                          BEST1502P_AON_WDT_SIZE);
    memory_region_add_subregion_overlap(sysmem, BEST1502P_AON_WDT_BASE,
                                        &s->aon_wdt, 3);

    memset(&s->mmio_state, 0, sizeof(s->mmio_state));
    s->mmio_state.machine = s;
    s->mmio_state.base = BEST1502P_MMIO_BASE;
    memory_region_init_io(&s->mmio, NULL, &best1502p_mmio_ops,
                          &s->mmio_state, "best1502p.mmio", BEST1502P_MMIO_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_MMIO_BASE, &s->mmio);
    best1502p_aon_reset_init(&s->aon_reset, sysmem);

    best1502p_timer_init(&s->timer1_state, &s->timer1,
                         "best1502p.timer1", BEST1502P_TIMER1_BASE, 16384, 16000);
    memory_region_add_subregion_overlap(sysmem, BEST1502P_TIMER1_BASE,
                                        &s->timer1, 2);

    /* The SDK maps MCU_TIMER1 here; the AP delay helper reads its
     * free-running downcounter after clock setup. */
    best1502p_timer_init(&s->timer0_state, &s->timer0,
                         "best1502p.timer0", BEST1502P_TIMER0_BASE, 6000000, 6000000);
    memory_region_add_subregion_overlap(sysmem, BEST1502P_TIMER0_BASE,
                                        &s->timer0, 2);

    best1502p_ispi_init(&s->ispi_state);
    best1502p_lpu_init(&s->lpu, sysmem);
    for (unsigned ch = 0; ch < 8; ch++) {
        if (s->gpadc_input[ch] > UINT16_MAX && s->gpadc_input[ch] != UINT32_MAX) {
            error_report("gpadc-raw%u must be 0..65535 or 4294967295 (disconnected)", ch);
            exit(1);
        }
        s->ispi_state.adc_input[ch] = s->gpadc_input[ch];
    }
    memory_region_init_io(&s->ispi, NULL, &best1502p_ispi_ops,
                          &s->ispi_state, "best1502p.ispi",
                          BEST1502P_ISPI_SIZE);
    memory_region_add_subregion_overlap(sysmem, BEST1502P_ISPI_BASE,
                                        &s->ispi, 3);

    best1502p_spi_init(&s->spi0_state, BEST1502P_SPI0_BASE);
    best1502p_nor_init(&s->nor_state,
                       memory_region_get_ram_ptr(&s->flash),
                       BEST1502P_FLASH_SIZE);
    best1502p_spi_set_nor(&s->spi0_state, &s->nor_state);
    memory_region_init_io(&s->spi0, NULL, &best1502p_spi_ops,
                          &s->spi0_state, "best1502p.spi0",
                          BEST1502P_SPI0_SIZE);
    memory_region_add_subregion_overlap(sysmem, BEST1502P_SPI0_BASE,
                                        &s->spi0, 3);

    /* SDK cache control windows are outside the peripheral aperture.  Map
     * them as observable register banks so cache setup can proceed without a
     * bus fault; detailed cache semantics remain a separate SoC task. */
    s->icache_state = (Best1502pMmio) { .machine = s,
                                        .base = BEST1502P_ICACHE_BASE };
    memory_region_init_io(&s->icache_mmio, NULL, &best1502p_mmio_ops,
                          &s->icache_state, "best1502p.icache", BEST1502P_CACHE_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_ICACHE_BASE, &s->icache_mmio);
    s->dcache_state = (Best1502pMmio) { .machine = s,
                                        .base = BEST1502P_DCACHE_BASE };
    memory_region_init_io(&s->dcache_mmio, NULL, &best1502p_mmio_ops,
                          &s->dcache_state, "best1502p.dcache", BEST1502P_CACHE_SIZE);
    memory_region_add_subregion(sysmem, BEST1502P_DCACHE_BASE, &s->dcache_mmio);

    s->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(s->sysclk, 100000000);
    s->refclk = clock_new(OBJECT(machine), "REFCLK");
    /* board_late_initialize selects external SysTick at 16000 Hz. */
    clock_set_hz(s->refclk, 16000);

    object_initialize_child(OBJECT(machine), "uart1", &s->uart1, TYPE_PL011);
    qdev_prop_set_chr(DEVICE(&s->uart1), "chardev", serial_hd(0));
    qdev_connect_clock_in(DEVICE(&s->uart1), "clk", s->sysclk);
    sysbus_realize(SYS_BUS_DEVICE(&s->uart1), &error_fatal);
    memory_region_add_subregion_overlap(
        sysmem, BEST1502P_UART1_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->uart1), 0), 2);

    object_initialize_child(OBJECT(machine), "uart2", &s->uart2, TYPE_PL011);
    if (serial_hd(1)) {
        qdev_prop_set_chr(DEVICE(&s->uart2), "chardev", serial_hd(1));
    }
    qdev_connect_clock_in(DEVICE(&s->uart2), "clk", s->sysclk);
    sysbus_realize(SYS_BUS_DEVICE(&s->uart2), &error_fatal);
    memory_region_add_subregion_overlap(
        sysmem, BEST1502P_UART2_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->uart2), 0), 2);

    object_initialize_child(OBJECT(machine), "armv7m", &s->armv7m,
                            TYPE_ARMV7M);
    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", 128);
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);
    /* BEST1502P exposes eight unified MPU regions. */
    qdev_prop_set_uint32(armv7m, "mpu-ns-regions", BEST1502P_MPU_REGIONS);
    qdev_prop_set_uint32(armv7m, "mpu-s-regions", BEST1502P_MPU_REGIONS);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->refclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(sysmem), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), &error_fatal);
    /* reference UART table: UART1 0x4000c000 -> IRQ18, UART2 0x4000d000
     * -> IRQ26. Route PL011 RX/TX/error status through the AP NVIC. */
    s->uart_irq[0] = (Best1502pUartIrq) { &s->uart1, qdev_get_gpio_in(armv7m, 18) };
    s->uart_irq[1] = (Best1502pUartIrq) { &s->uart2, qdev_get_gpio_in(armv7m, 26) };
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart1), 0,
                       qemu_allocate_irq(best1502p_uart_irq, &s->uart_irq[0], 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart2), 0,
                       qemu_allocate_irq(best1502p_uart_irq, &s->uart_irq[1], 0));
    /* The AP selects AON channel 1 on IRQ 46. Other timer IRQ routes
     * remain unconnected until qualified. */
    s->timer1_state.channel[1].irq = qdev_get_gpio_in(armv7m, 46);
    best1502p_display_init(s, sysmem);
    best1502p_nand_init(s, sysmem);
    if (s->diag_skip_binding) {
        miband_binding_ui_install_at(machine->kernel_filename,
                                     s->firmware_profile, code_base);
    }
    armv7m_load_kernel(ARM_CPU(first_cpu),
                       s->diag_skip_binding ? NULL : machine->kernel_filename,
                       code_base, BEST1502P_FLASH_SIZE - s->flash_offset);
    best1502p_open_storage(s);
    s->initialized = true;
}

static void best1502p_class_init(ObjectClass *oc, void *data)
{
    static const char * const cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m33"), NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);
    bes_runtime_config_property(oc);

    mc->desc = "BES best1502p AP bring-up machine (Cortex-M33)";
    mc->init = best1502p_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->valid_cpu_types = cpu_types;
    mc->default_ram_size = BEST1502P_RAM_SIZE;
    mc->max_cpus = 1;
    object_class_property_add_str(oc, "firmware-profile", best1502p_get_firmware,
                                  best1502p_set_firmware);
    object_class_property_add_bool(oc, "diag-skip-binding", best1502p_get_skip_binding,
                                   best1502p_set_skip_binding);
    object_class_property_add_str(oc, "resource-image", best1502p_get_resource,
                                  best1502p_set_resource);
    object_class_property_add_str(oc, "flash-state", best1502p_get_flash_state,
                                  best1502p_set_flash_state);
    object_class_property_add(oc, "boot-sp", "uint32",
                              best1502p_get_vector, best1502p_set_vector,
                              NULL, NULL);
    object_class_property_add(oc, "boot-pc", "uint32",
                              best1502p_get_vector, best1502p_set_vector,
                              NULL, NULL);
    object_class_property_add(oc, "flash-offset", "uint32",
                              best1502p_get_flash_offset,
                              best1502p_set_flash_offset, NULL, NULL);
}

static void best1502p_instance_init(Object *obj)
{
    Best1502pMachineState *s = BEST1502P_MACHINE(obj);
    s->boot_sp = BEST1502P_RAM_BASE + BEST1502P_RAM_SIZE - 0x20;
    /* The AP package carries a 0xc0000-byte loader prefix.  Its first
     * Thumb stub is therefore executed at XIP+flash_offset+0x10. */
    s->boot_pc = BEST1502P_XIP_BASE + 0x000c0000u + 0x11;
    s->flash_offset = 0x000c0000u;
    s->display_te_pin = UINT32_MAX;
    for (unsigned ch = 0; ch < 8; ch++) {
        g_autofree char *name = g_strdup_printf("gpadc-raw%u", ch);
        s->gpadc_input[ch] = UINT32_MAX;
        object_property_add_uint32_ptr(obj, name, &s->gpadc_input[ch], OBJ_PROP_FLAG_READWRITE);
    }
    object_property_add_uint32_ptr(obj, "nand-id", &s->nand_id, OBJ_PROP_FLAG_READWRITE);
    s->touch_i2c_bus = s->touch_irq_pin = UINT32_MAX;
    object_property_add_uint32_ptr(obj, "touch-chip-id", &s->touch_chip_id, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "touch-i2c-bus", &s->touch_i2c_bus, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "touch-address", &s->touch_address, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "touch-irq-pin", &s->touch_irq_pin, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "touch-sleep-gesture", &s->touch_sleep_gesture, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "nand-page-size", &s->nand_page_size, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "nand-spare-size", &s->nand_spare_size, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "nand-page-count", &s->nand_page_count, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "nand-pages-per-block", &s->nand_pages_per_block, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "display-te-pin", &s->display_te_pin,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "panel-id", &s->panel_id,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "display-width", &s->display_width,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "display-height", &s->display_height,
                                   OBJ_PROP_FLAG_READWRITE);
}

static const TypeInfo best1502p_info = {
    .name = TYPE_BEST1502P_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Best1502pMachineState),
    .instance_init = best1502p_instance_init,
    .class_init = best1502p_class_init,
};

static void best1502p_register(void)
{
    type_register_static(&best1502p_info);
}

type_init(best1502p_register)
