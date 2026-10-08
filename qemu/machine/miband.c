/*
 * Minimal Xiaomi Band 10 Pro AP execution board for QEMU 8.2.
 *
 * This is an execution scaffold, not a BES SoC model. The AP image starts
 * with a BES boot header rather than a Cortex-M vector table, so the board
 * supplies only the initial SP/PC at zero and leaves the image untouched.
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
#include "exec/address-spaces.h"
#include "qom/object.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "miband_flash_storage.h"
#include "miband_display_memory.h"
#include "miband_peripherals.h"
#include "miband_charger.h"
#include "miband_touch_input.h"
#include "miband_touch.h"
#include "miband_zinitix.h"
#include "best1503_package_config.h"
#include "miband_power.h"
#include "miband_signal.h"
#include "miband_panel_device.h"
#include "miband_display_bridge.h"
#include "miband_gpu.h"
#include "miband_bt.h"
#include "miband_audio_adapter.h"
#include "miband_binding_ui.h"
#include "miband_xiaomi_protocol.h"

#define AP_XIP_BASE     0x0c000000
#define AP_NC_BASE      0x28000000
#define AP_C_BASE       0x2c000000
#define AP_FLASH_SIZE   BEST1503_PACKAGE_NOR_SIZE
#define AP_RAM_BASE     0x20000000
#define AP_RAMX_BASE    0x00200000
#define AP_RAM_SIZE     0x00160000
#define AP_PSRAM_BASE   0x3c000000
#define AP_PSRAM_NC     0x38000000
#define AP_PSRAMX_BASE  0x1c000000
#define AP_PSRAMX_NC    0x18000000
#define AP_PSRAM_SIZE   BEST1503_PACKAGE_PSRAM_SIZE
#define NAND_CACHE_NC   0x30000000
#define NAND_CACHE_C    0x34000000
#define NAND_CACHE_SIZE 0x00010000
#define BT_RAM_BASE     0xc0000000
#define BT_RAM_SIZE     0x00010000
#define BTC_RAM_BASE    0xa0000000
#define BTC_RAM_SIZE    0x00200000
#define BT_EXCH_BASE    0xd0200000
#define BT_EXCH_SIZE    0x00020000
#define BT_CPUDUMP_BASE 0xd0700000
#define BT_CPUDUMP_SIZE 0x00010000
#define BT_REG_BASE    0xd0220000
#define BT_REG_SIZE    0x00010000
#define BT_DIGITAL_BASE 0xd0310000
#define BT_CMU_BASE    0xd0330000
#define BT_RF_BASE     0xd0350000
#define UART1_BASE      0x4000c000
#define UART2_BASE      0x4000d000
#define UART1_IRQ       18
#define UART2_IRQ       26
#define AON_TIMER1_IRQ  46
#define BT_TX0_IRQ      32
#define BT_TX1_IRQ      33
#define BT_RX0_IRQ      34
#define BT_RX1_IRQ      35
#define GPU_IRQ         70
#define LCDC_IRQ        67
#define GPIO_IRQ        44
#define PMU_IRQ         42
#define GPDMA_IRQ        4
#define MMIO_BASE       0x40000000
#define MMIO_SIZE       0x20000000
#define LOW_MMIO_BASE   0x07000000
#define LOW_MMIO_SIZE   0x01000000

typedef struct MiBandMachineState MiBandMachineState;

typedef struct MiBandMMIOBank {
    MiBandMachineState *machine;
    hwaddr base;
} MiBandMMIOBank;

struct MiBandMachineState {
    MachineState parent;
    ARMv7MState armv7m;
    PL011State uart1;
    PL011State uart2;
    MemoryRegion vectors;
    MemoryRegion flash;
    MemoryRegion flash_nc;
    MemoryRegion flash_c;
    MemoryRegion ram;
    MemoryRegion ramx;
    MemoryRegion psram;
    MemoryRegion psram_nc;
    MemoryRegion psramx;
    MemoryRegion psramx_nc;
    MemoryRegion nand_cache;
    MemoryRegion nand_cache_c;
    MemoryRegion bt_ram;
    MemoryRegion btc_ram;
    MemoryRegion bt_exch;
    MemoryRegion bt_cpudump;
    MemoryRegion bt_regs;
    MemoryRegion bt_digital_regs;
    MemoryRegion bt_cmu_regs;
    MemoryRegion bt_rf_regs;
    MemoryRegion mmio;
    MemoryRegion low_mmio;
    MiBandMMIOBank mmio_banks[6];
    GHashTable *mmio_words;
    GHashTable *mmio_log_counts;
    MiBandMemsc memsc;
    MiBandAonCmu aoncmu;
    MiBandTimer timers[MIBAND_TIMER_COUNT];
    MiBandIspi ispi;
    MiBandCharger charger;
    MiBandSpi spi[2];
    MiBandFlash flash_ctrl[MIBAND_FLASH_COUNT];
    MiBandFlashStorage flash_storage;
    MiBandDma dma;
    MiBandAudio audio;
    MiBandAudioAdapter audio_adapter;
    char *audio_mode, *audio_source, *audio_file, *audio_device, *audio_shim;
    bool diag_skip_binding;
    bool diag_bypass_binding_auth;
    bool audio_initialized;
    MiBandLcdc lcdc;
    MiBandPanelDevice *panel;
    MiBandDisplayBridge *display_bridge;
    MiBandGpu gpu;
    MiBandBt bt;
    MiBandI2c i2c[MIBAND_I2C_COUNT];
    MiBandGpio gpio;
    MiBandTouch touch;
    MiBandZinitix zinitix;
    MiBandBoardGpioLine touch_irq_line;
    MiBandTouchInput touch_input;
    uint32_t boot_sp, boot_pc;
    uint32_t panel_id;
    uint32_t framebuffer_bpp;
    uint32_t i2c5_probe_address;
    uint32_t i2c5_probe_chip_id;
    uint32_t i2c2_probe_address;
    uint32_t i2c2_probe_register0;
    bool gpu_ack_no_render;
    bool gpu_render_index8;
    bool gpu_scanout_bridge;
    bool hci_worker;
    bool lcdc_worker;
    bool gpu_worker;
    bool flash_worker;
    /* Disposable regression option: turn a guest SYSRESETREQ into a
     * deterministic QEMU shutdown so the harness can observe the reset
     * boundary.  It is intentionally opt-in and independent of HCI. */
    bool guest_reset_shutdown;
    bool workers_started;
    bool initial_reset_seen;
    Notifier worker_exit;
    bool bt_smp_just_works;
    uint32_t bt_passkey;
    uint32_t bt_le_passkey;
    bool gpadc_model;
    bool pmu_status_model;
    bool pmu_rtc_model;
    bool charger_model;
    uint32_t charger_i2c_bus;
    uint32_t display_width, display_height;
    const MiBandBoardConfig *board;
    const MiBandFirmwareConfig *firmware;
    bool firmware_selected;
    MiBandBoardPower power;
    MiBandBoardGpioLine panel_te_line;
    uint32_t aon_metal_revision;
    char *bt_backend;
    bool bt_synthetic_peer_passive;
    bool bt_rx_flow_control;
    uint32_t bt_serial_tx_rate;
    char *resource_image;
    char *flash_state;
    GMappedFile *resource_map;
    char *nand_image;
    GMappedFile *nand_map;
    char *nor_fs_image;
    GMappedFile *nor_fs_map;
    MiBandPsram psram_ctrl;
    Clock *sysclk;
    Clock *refclk;
};

#define TYPE_MIBAND_MACHINE MACHINE_TYPE_NAME("bes1503")
OBJECT_DECLARE_SIMPLE_TYPE(MiBandMachineState, MIBAND_MACHINE)

static void miband_workers_exit(Notifier *notifier, void *data)
{
    MiBandMachineState *s = container_of(notifier, MiBandMachineState, worker_exit);
    miband_ispi_shutdown(&s->ispi);
    miband_zinitix_shutdown(&s->zinitix);
    miband_bt_shutdown(&s->bt);
    /* QEMU audio_cleanup precedes exit notifiers, including when quit is
     * issued while still paused (-S) and no VM state callback ran. Only our
     * file/queue storage is safe to close at this late process boundary. */
    if (s->audio_initialized) miband_pcm_destroy(&s->audio.pcm);
    miband_dma_async_reset(&s->dma);
    miband_lcdc_async_shutdown(&s->lcdc);
    miband_gpu_cleanup(&s->gpu);
    if (s->gpu_worker) miband_display_memory_cleanup();
    miband_flash_storage_close(&s->flash_storage);
    miband_nor_free(s->flash_storage.nor);
    miband_nand_free(s->flash_storage.nand);
    s->flash_storage.nor = NULL;
    s->flash_storage.nand = NULL;
}

static void miband_workers_reset(void *opaque)
{
    MiBandMachineState *s = opaque;
    if (!s->initial_reset_seen) {
        s->initial_reset_seen = true;
        return;
    }
    miband_aoncmu_reset(&s->aoncmu);
    miband_audio_adapter_reset(&s->audio_adapter);
    miband_dma_async_reset(&s->dma);
    miband_ispi_gpadc_reset(&s->ispi);
    miband_lcdc_async_reset(&s->lcdc);
    miband_gpu_async_reset(&s->gpu);
    miband_charger_reset(&s->charger);
    if (s->zinitix.next_report) {
        miband_zinitix_reset(&s->zinitix);
    }
    miband_gpio_reset_outputs(&s->gpio);
    if (s->guest_reset_shutdown) {
        /* The synthetic backend has no external transport owner to tear
         * down.  Stop before asking QEMU to exit, matching the physical-HCI
         * reset ordering and preventing the vCPU from resuming briefly. */
        vm_stop(RUN_STATE_SHUTDOWN);
        error_report("miband: guest reset requested; shutting down disposable instance");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    if (s->hci_worker) {
        /* Reset otherwise resumes the vCPU before the queued shutdown request.
         * Keep it stopped before releasing transport timers and callbacks. */
        vm_stop(RUN_STATE_SHUTDOWN);
        miband_bt_shutdown(&s->bt);
        error_report("miband: guest reset invalidated physical HCI session; restart the instance");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static void miband_aoncmu_reset_request(void *opaque, uint32_t mask)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "miband AON CMU global reset request mask=%08" PRIx32
                  "\n", mask);
    /* Defer reset to QEMU's runstate loop, matching AIRCR.SYSRESETREQ. */
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    (void)opaque;
}

static bool miband_mmio_should_log(MiBandMachineState *s, hwaddr address)
{
    unsigned count;

    if (!qemu_loglevel_mask(LOG_UNIMP)) {
        return false;
    }

    count = GPOINTER_TO_UINT(g_hash_table_lookup(s->mmio_log_counts,
                                                 GUINT_TO_POINTER(address)));
    if (count < 16) {
        g_hash_table_insert(s->mmio_log_counts, GUINT_TO_POINTER(address),
                            GUINT_TO_POINTER(count + 1));
        return true;
    }
    if (count == 16) {
        qemu_log_mask(LOG_UNIMP,
                      "miband MMIO %08" HWADDR_PRIx " further accesses suppressed\n",
                      address);
        g_hash_table_insert(s->mmio_log_counts, GUINT_TO_POINTER(address),
                            GUINT_TO_POINTER(17));
    }
    return false;
}

static uint64_t miband_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    MiBandMMIOBank *bank = opaque;
    MiBandMachineState *s = bank->machine;
    hwaddr address = bank->base + offset;
    gpointer value;
    uint64_t result;

    if (miband_audio_adapter_read(&s->audio_adapter, address, size, &result) ||
        miband_psram_read(&s->psram_ctrl, address, size, &result) ||
        miband_flash_read(s->flash_ctrl, address, size, &result) ||
        miband_dma_read(&s->dma, address, size, &result) ||
        miband_lcdc_read(&s->lcdc, address, size, &result) ||
        miband_gpu_read(&s->gpu, address, size, &result) ||
        miband_bt_mailbox_read(&s->bt, address, size, &result) ||
        miband_bt_read(&s->bt, address, size, &result) ||
        miband_gpio_read(&s->gpio, address, size, &result) ||
        miband_i2c_read(s->i2c, address, size, &result) ||
        miband_spi_read(s->spi, address, size, &result) ||
        miband_ispi_read(&s->ispi, address, size, &result) ||
        miband_timer_read(s->timers, address, size, &result) ||
        miband_memsc_read(&s->memsc, address, size, &result)) {
        return result;
    }
    if (miband_aoncmu_read(&s->aoncmu, address, size, &result)) {
        return result;
    }

    bool found = g_hash_table_lookup_extended(s->mmio_words,
                                               GUINT_TO_POINTER(address),
                                               NULL, &value);
    result = found ? GPOINTER_TO_UINT(value) : 0;

    if (miband_mmio_should_log(s, address)) {
        qemu_log_mask(LOG_UNIMP, "miband MMIO read %08" HWADDR_PRIx
                      " size=%u -> %08" PRIx64 "\n", address, size, result);
    }
    return result;
}

static void miband_mmio_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    MiBandMMIOBank *bank = opaque;
    MiBandMachineState *s = bank->machine;
    hwaddr address = bank->base + offset;

    if (miband_psram_write(&s->psram_ctrl, address, size, value)) {
        return;
    }
    if (miband_flash_write(s->flash_ctrl, address, size, value)) {
        return;
    }
    if (miband_audio_adapter_write(&s->audio_adapter, address, size, value) ||
        miband_dma_write(&s->dma, address, size, value) ||
        miband_lcdc_write(&s->lcdc, address, size, value) ||
        miband_gpu_write(&s->gpu, address, size, value) ||
        miband_bt_mailbox_write(&s->bt, address, size, value) ||
        miband_bt_write(&s->bt, address, size, value) ||
        miband_gpio_write(&s->gpio, address, size, value) ||
        miband_i2c_write(s->i2c, address, size, value) ||
        miband_spi_write(s->spi, address, size, value) ||
        miband_ispi_write(&s->ispi, address, size, value) ||
        miband_timer_write(s->timers, address, size, value) ||
        miband_memsc_write(&s->memsc, address, size, value)) {
        return;
    }
    if (miband_aoncmu_write(&s->aoncmu, address, size, value)) {
        return;
    }

    if (miband_mmio_should_log(s, address)) {
        qemu_log_mask(LOG_UNIMP, "miband MMIO write %08" HWADDR_PRIx
                      " size=%u value=%08" PRIx64 "\n",
                      address, size, value);
    }
    g_hash_table_insert(s->mmio_words, GUINT_TO_POINTER(address),
                        GUINT_TO_POINTER((uint32_t)value));
}

static const MemoryRegionOps miband_mmio_ops = {
    .read = miband_mmio_read,
    .write = miband_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void miband_init(MachineState *machine)
{
    bes_runtime_config_lock();
    MiBandMachineState *s = MIBAND_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *armv7m;
    const uint32_t vectors[] = { s->boot_sp, s->boot_pc };
    const MiBandFirmwareConfig *firmware = s->firmware;
    const uint32_t ap_base = AP_XIP_BASE + firmware->flash_offset;
    unsigned sector_size = BEST1503_PACKAGE_NOR_SECTOR_SIZE;
    uint64_t resource_limit = (uint64_t)firmware->resource_page_count * s->board->nand.page_size;
    uint64_t raw_image_size = (uint64_t)s->board->nand.page_count *
        (s->board->nand.page_size + s->board->nand.spare_size);
    uint32_t pc = s->boot_pc & ~1u;
    if (firmware->flash_offset >= AP_FLASH_SIZE || !(s->boot_pc & 1) ||
        !((pc >= ap_base && pc < AP_XIP_BASE + AP_FLASH_SIZE) ||
          (pc >= AP_NC_BASE + firmware->flash_offset && pc < AP_NC_BASE + AP_FLASH_SIZE) ||
          (pc >= AP_C_BASE + firmware->flash_offset && pc < AP_C_BASE + AP_FLASH_SIZE))) {
        error_setg(&error_fatal, "boot-pc must lie in the selected firmware AP flash mapping");
        return;
    }
    if ((s->firmware_selected && strcmp(firmware->board, s->board->id)) ||
        firmware->nor_size > AP_FLASH_SIZE ||
        firmware->nor_address > AP_FLASH_SIZE - firmware->nor_size ||
        (int64_t)firmware->nor_image_offset != (int64_t)firmware->nor_address - firmware->flash_offset ||
        firmware->nor_size < sector_size || firmware->nor_size % sector_size ||
        firmware->nor_address % sector_size ||
        firmware->resource_first_page > s->board->nand.page_count ||
        firmware->resource_page_count > s->board->nand.page_count - firmware->resource_first_page) {
        error_setg(&error_fatal, "firmware storage/board/package configuration is inconsistent");
        return;
    }


    if (s->charger_model && !s->gpadc_model) {
        error_setg(&error_fatal, "charger-model requires gpadc-model=on for battery and thermistor samples");
    }

    if (s->flash_worker) {
        error_setg(&error_fatal, "flash-worker is gated: firmware WIP/OIP and durable completion ordering are not verified");
    }

    if (!machine->kernel_filename) {
        error_report("bes1503 requires -kernel vela_ap.bin");
        exit(EXIT_FAILURE);
    }

    memory_region_init_rom(&s->vectors, NULL, "miband.vectors",
                           0x1000, &error_fatal);
    memory_region_add_subregion(sysmem, 0, &s->vectors);
    rom_add_blob_fixed("miband.reset-vectors", vectors, sizeof(vectors), 0);

    memory_region_init_rom(&s->flash, NULL, "miband.ap-xip",
                           AP_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, AP_XIP_BASE, &s->flash);
    memset(memory_region_get_ram_ptr(&s->flash), 0xff, AP_FLASH_SIZE);
    s->flash_storage.nor = miband_nor_new(
        memory_region_get_ram_ptr(&s->flash) + firmware->nor_address,
        firmware->nor_address, firmware->nor_size, sector_size,
        (const uint8_t[])BEST1503_PACKAGE_NOR_JEDEC_ID);
    miband_flash_init(&s->flash_ctrl[0], miband_nor_endpoint(s->flash_storage.nor),
                       NULL, 0);
    if (s->nor_fs_image) {
        GError *map_error = NULL;

        s->nor_fs_map = g_mapped_file_new(s->nor_fs_image, false,
                                          &map_error);
        if (!s->nor_fs_map) {
            error_report("cannot map NOR FS image %s: %s",
                         s->nor_fs_image, map_error->message);
            g_error_free(map_error);
            exit(EXIT_FAILURE);
        }
        if (g_mapped_file_get_length(s->nor_fs_map) != firmware->nor_size) {
            error_report("nor-fs-image must be exactly 0x%x bytes",
                         firmware->nor_size);
            exit(EXIT_FAILURE);
        }
        memcpy(memory_region_get_ram_ptr(&s->flash) + firmware->nor_address,
               g_mapped_file_get_contents(s->nor_fs_map), firmware->nor_size);
    }
    memory_region_init_alias(&s->flash_nc, NULL, "miband.ap-nc",
                             &s->flash, 0, AP_FLASH_SIZE);
    memory_region_add_subregion(sysmem, AP_NC_BASE, &s->flash_nc);
    memory_region_init_alias(&s->flash_c, NULL, "miband.ap-cached",
                             &s->flash, 0, AP_FLASH_SIZE);
    memory_region_add_subregion(sysmem, AP_C_BASE, &s->flash_c);

    memory_region_init_ram(&s->ram, NULL, "miband.ap-ram",
                           AP_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, AP_RAM_BASE, &s->ram);
    memory_region_init_alias(&s->ramx, NULL, "miband.ap-ramx",
                             &s->ram, 0, AP_RAM_SIZE);
    memory_region_add_subregion(sysmem, AP_RAMX_BASE, &s->ramx);
    memory_region_init_ram(&s->psram, NULL, "miband.ap-psram",
                           AP_PSRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, AP_PSRAM_BASE, &s->psram);
    memory_region_init_alias(&s->psram_nc, NULL, "miband.ap-psram-nc",
                             &s->psram, 0, AP_PSRAM_SIZE);
    memory_region_add_subregion(sysmem, AP_PSRAM_NC, &s->psram_nc);
    memory_region_init_alias(&s->psramx, NULL, "miband.ap-psramx",
                             &s->psram, 0, AP_PSRAM_SIZE);
    memory_region_add_subregion(sysmem, AP_PSRAMX_BASE, &s->psramx);
    memory_region_init_alias(&s->psramx_nc, NULL, "miband.ap-psramx-nc",
                             &s->psram, 0, AP_PSRAM_SIZE);
    memory_region_add_subregion(sysmem, AP_PSRAMX_NC, &s->psramx_nc);
    memory_region_init_ram(&s->nand_cache, NULL, "miband.nand-page-cache",
                           NAND_CACHE_SIZE, &error_fatal);
    memset(memory_region_get_ram_ptr(&s->nand_cache), 0xff, NAND_CACHE_SIZE);
    memory_region_add_subregion(sysmem, NAND_CACHE_NC, &s->nand_cache);
    memory_region_init_alias(&s->nand_cache_c, NULL, "miband.nand-cache-cached",
                             &s->nand_cache, 0, NAND_CACHE_SIZE);
    memory_region_add_subregion(sysmem, NAND_CACHE_C, &s->nand_cache_c);
    memory_region_init_ram(&s->bt_ram, NULL, "miband.bt-ram",
                           BT_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BT_RAM_BASE, &s->bt_ram);
    memory_region_init_ram(&s->btc_ram, NULL, "miband.btc-rom-ram",
                           BTC_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BTC_RAM_BASE, &s->btc_ram);
    memory_region_init_ram(&s->bt_exch, NULL, "miband.bt-exchange",
                           BT_EXCH_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BT_EXCH_BASE, &s->bt_exch);
    memory_region_init_ram(&s->bt_cpudump, NULL, "miband.bt-cpudump",
                           BT_CPUDUMP_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BT_CPUDUMP_BASE, &s->bt_cpudump);
    s->flash_storage.nand = miband_nand_new(&s->board->nand);
    miband_flash_init(&s->flash_ctrl[s->board->nand_bus], miband_nand_endpoint(s->flash_storage.nand),
                       memory_region_get_ram_ptr(&s->nand_cache), NAND_CACHE_SIZE);
    if (s->resource_image && s->nand_image) {
        error_report("resource-image and nand-image are mutually exclusive");
        exit(EXIT_FAILURE);
    }
    if (s->resource_image) {
        GError *map_error = NULL;

        s->resource_map = g_mapped_file_new(s->resource_image, false,
                                             &map_error);
        if (!s->resource_map) {
            error_report("cannot map resource image %s: %s",
                         s->resource_image, map_error->message);
            g_error_free(map_error);
            exit(EXIT_FAILURE);
        }
        bool raw = !strcmp(firmware->resource_format, "yaffs2-oob");
        bool inband = !strcmp(firmware->resource_format, "yaffs2-inband");
        uint64_t stride = s->board->nand.page_size + s->board->nand.spare_size;
        uint64_t limit = raw ? (uint64_t)firmware->resource_page_count * stride : resource_limit;
        size_t size = g_mapped_file_get_length(s->resource_map);
        if (size > limit || size < 8 ||
            (raw ? size % stride != 0 : inband ? size % s->board->nand.page_size != 0 :
             memcmp(g_mapped_file_get_contents(s->resource_map), "-rom1fs-", 8) != 0)) {
            error_report("resource image must match the firmware filesystem and NAND partition");
            exit(EXIT_FAILURE);
        }
        miband_nand_set_resource(s->flash_storage.nand,
            (const uint8_t *)g_mapped_file_get_contents(s->resource_map),
            size, firmware->resource_first_page, raw);
    }
    if (s->nand_image) {
        GError *map_error = NULL;

        s->nand_map = g_mapped_file_new(s->nand_image, false, &map_error);
        if (!s->nand_map) {
            error_report("cannot map NAND image %s: %s",
                         s->nand_image, map_error->message);
            g_error_free(map_error);
            exit(EXIT_FAILURE);
        }
        if (g_mapped_file_get_length(s->nand_map) != raw_image_size ||
            memcmp(g_mapped_file_get_contents(s->nand_map), "-rom1fs-", 8)) {
            error_report("nand-image must match the board NAND geometry "
                         "with ROMFS at page zero");
            exit(EXIT_FAILURE);
        }
        miband_nand_set_image(s->flash_storage.nand,
            (const uint8_t *)g_mapped_file_get_contents(s->nand_map),
            g_mapped_file_get_length(s->nand_map), true);
    }

    if (s->flash_state) {
        GChecksum *sum=g_checksum_new(G_CHECKSUM_SHA256);
        char *ap_data=NULL;gsize ap_size=0,digest_size=32;
        uint8_t digest[32];
        if (!g_file_get_contents(machine->kernel_filename,&ap_data,&ap_size,NULL)) {
            error_report("cannot hash AP for flash state");exit(EXIT_FAILURE);
        }
        g_checksum_update(sum,(const guchar *)ap_data,ap_size);g_free(ap_data);
        GMappedFile *base_map = s->nand_map ? s->nand_map : s->resource_map;
        if (base_map) {
            g_checksum_update(sum, (const guchar *)g_mapped_file_get_contents(base_map),
                                g_mapped_file_get_length(base_map));
        }
        /* Historical MBFLASH1 states bind the image pair. A changed software
         * layout additionally binds its fingerprint, rejecting older states. */
        if (!firmware->legacy_journal) {
            g_checksum_update(sum, (const guchar *)firmware->storage_sha256,
                                strlen(firmware->storage_sha256));
        }
        g_checksum_get_digest(sum,digest,&digest_size);g_checksum_free(sum);
        miband_flash_storage_open(&s->flash_storage, s->flash_state, digest);
        /* The firmware mounts the writable NAND partition as /data.  MASS
         * records restored from the journal must be visible there before the
         * AP starts, otherwise the WebUI would only update a host-side
         * catalog. */
        miband_install_catalog_set_nand(
            &s->flash_storage.installs, s->flash_storage.nand,
            firmware->resource_first_page + firmware->resource_page_count);
    }
    s->mmio_words = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->mmio_log_counts = g_hash_table_new(g_direct_hash, g_direct_equal);
    miband_psram_init(&s->psram_ctrl, memory_region_get_ram_ptr(&s->psram),
                      AP_PSRAM_SIZE);
    miband_dma_init(&s->dma, &address_space_memory);
    miband_flash_connect_dma(&s->flash_ctrl[s->board->nand_bus], (MiBandFlashDmaCallbacks) {
        &s->dma, miband_dma_flash_rx, miband_dma_flash_tx
    });
    s->panel = miband_panel_new(s->panel_id, s->display_width, s->display_height);
    g_assert(s->panel);
    s->display_bridge = miband_display_bridge_new(s->display_width, s->display_height);
    miband_panel_connect_display(s->panel, s->display_bridge,
                                 miband_display_bridge_present);
    miband_lcdc_init(&s->lcdc, miband_panel_endpoint(s->panel),
                     s->framebuffer_bpp ? s->framebuffer_bpp : 24,
                     s->display_width, s->display_height,
                     memory_region_get_ram_ptr(&s->psram), AP_PSRAM_SIZE,
                     memory_region_get_ram_ptr(&s->ram), AP_RAM_SIZE);
    s->lcdc.memory_busy = miband_gpu_pending_memory;
    miband_dma_connect_lcdc(&s->dma, &s->lcdc);
    s->panel_te_line = (MiBandBoardGpioLine) { &s->gpio, s->board->panel_te_gpio };
    miband_panel_connect_te(s->panel, &s->panel_te_line, miband_board_gpio_edge);
    miband_gpu_init(&s->gpu, memory_region_get_ram_ptr(&s->psram),
                    AP_PSRAM_SIZE, memory_region_get_ram_ptr(&s->ram),
                    AP_RAM_SIZE, s->gpu_ack_no_render,
                    s->gpu_render_index8);
    miband_gpu_connect_lcdc(&s->gpu, &s->lcdc, s->gpu_scanout_bridge);
    miband_bt_init(&s->bt, &address_space_memory,
                   memory_region_get_ram_ptr(&s->bt_ram));
    miband_bt_set_gatt_mtu(&s->bt, s->firmware->ble_mtu);
    miband_bt_set_gatt_characteristics(&s->bt, s->firmware->ble_tx_uuid,
                                       s->firmware->ble_rx_uuid);
    miband_bt_set_mass_types(&s->bt, s->firmware->mass_types,
                             s->firmware->mass_type_count);
    miband_bt_set_mass_slice_length(&s->bt, s->firmware->mass_slice_length);
    miband_bt_set_backend(&s->bt, s->bt_backend);
    miband_bt_set_synthetic_peer_passive(&s->bt, s->bt_synthetic_peer_passive);
    if (s->flash_storage.store) {
        miband_bt_set_mass_commit(&s->bt, miband_install_catalog_commit,
                                  &s->flash_storage.installs);
    }
    miband_bt_set_rx_flow_requested(&s->bt, s->bt_rx_flow_control);
    miband_bt_set_serial_tx_rate(&s->bt, s->bt_serial_tx_rate);
    if (s->bt_serial_tx_rate && !s->bt.serial_active)
        error_setg(&error_fatal, "bt-serial-tx-rate requires physical serial backend");
    if (s->bt_rx_flow_control && !s->bt.serial_active)
        error_setg(&error_fatal, "bt-rx-flow-control requires physical serial backend");
    miband_bt_set_smp_just_works(&s->bt, s->bt_smp_just_works);
    miband_bt_set_fixed_passkey(&s->bt, s->bt_passkey);
    miband_bt_set_le_passkey(&s->bt, s->bt_le_passkey);
    miband_i2c_init(s->i2c);
    miband_charger_init(&s->charger, s->charger_model, 75, false,
                         s->board->charger_probe_revision);
    s->i2c[s->charger_i2c_bus].charger = &s->charger;
    s->i2c[5].probe_address = s->i2c5_probe_address;
    s->i2c[5].probe_chip_id = s->i2c5_probe_chip_id;
    if (!strcmp(s->board->touch_model, "zinitix")) {
        s->touch_irq_line = (MiBandBoardGpioLine) { &s->gpio, s->board->touch_irq_gpio };
        miband_zinitix_init(&s->zinitix, s->board->touch_chip_id,
                            miband_board_gpio_active_low, &s->touch_irq_line);
        s->zinitix.sleep_gesture = s->board->touch_sleep_gesture;
        s->zinitix.double_tap_status = s->board->touch_double_tap_status;
        if (s->board->touch_reset_gpio >= 0) {
            miband_gpio_connect_output(&s->gpio, s->board->touch_reset_gpio,
                                        miband_zinitix_reset_line, &s->zinitix);
        }
        s->i2c[s->board->touch_bus].endpoint =
            miband_zinitix_endpoint(&s->zinitix, s->board->touch_address);
        miband_touch_input_init(&s->touch_input, miband_zinitix_report, &s->zinitix,
                                s->display_width, s->display_height,
                                miband_gpu_diag_touch_release);
        s->touch_input.report_clocked = miband_zinitix_report_clocked;
    } else if (s->i2c5_probe_address == 0x20 && s->i2c5_probe_chip_id) {
        s->touch_irq_line = (MiBandBoardGpioLine) { &s->gpio, s->board->touch_irq_gpio };
        miband_touch_init(&s->touch, s->i2c5_probe_chip_id,
                          miband_board_gpio_inferred_edge, miband_board_gpio_pending,
                          &s->touch_irq_line);
        miband_touch_input_init(&s->touch_input, miband_touch_input_report, &s->touch,
                                s->display_width, s->display_height,
                                miband_gpu_diag_touch_release);
        s->i2c[s->board->touch_bus].endpoint =
            miband_touch_endpoint(&s->touch, s->board->touch_address);
    }
    s->i2c[2].probe_address = s->i2c2_probe_address;
    s->i2c[2].probe_register0 = s->i2c2_probe_register0;
    miband_timers_init(s->timers);
    miband_ispi_init(&s->ispi);
    miband_ispi_set_gpadc_model(&s->ispi, s->gpadc_model);
    miband_ispi_set_pmu_status_model(&s->ispi, s->pmu_status_model);
    miband_ispi_set_pmu_rtc_model(&s->ispi, s->pmu_rtc_model);
    s->power = (MiBandBoardPower) { &s->ispi, s->board };
    s->ispi.charger = &s->charger;
    if (s->charger_model) {
        MiBandChargerSignals signals = {
            .opaque = &s->power,
            .environment_changed = miband_board_power_changed,
            .reset_irq = miband_board_power_reset,
        };
        miband_charger_connect(&s->charger, &signals);
    }
    miband_spi_init(s->spi);
    miband_memsc_init(&s->memsc);
    /* The AP rejects the reset path when the cached metal byte is zero;
     * best1503-compatible traces accept revisions 3..5.  Use the lowest
     * accepted revision unless the caller supplied an explicit value. */
    if (!s->aon_metal_revision) {
        s->aon_metal_revision = 3;
    }
    miband_aoncmu_init(&s->aoncmu, s->aon_metal_revision);
    miband_aoncmu_set_reset_request(&s->aoncmu,
                                    miband_aoncmu_reset_request, s);
    s->mmio_banks[0] = (MiBandMMIOBank) { s, MMIO_BASE };
    s->mmio_banks[1] = (MiBandMMIOBank) { s, LOW_MMIO_BASE };
    s->mmio_banks[2] = (MiBandMMIOBank) { s, BT_REG_BASE };
    s->mmio_banks[3] = (MiBandMMIOBank) { s, BT_CMU_BASE };
    s->mmio_banks[4] = (MiBandMMIOBank) { s, BT_RF_BASE };
    s->mmio_banks[5] = (MiBandMMIOBank) { s, BT_DIGITAL_BASE };
    memory_region_init_io(&s->mmio, NULL, &miband_mmio_ops,
                          &s->mmio_banks[0],
                          "miband.unmodeled-mmio", MMIO_SIZE);
    memory_region_add_subregion(sysmem, MMIO_BASE, &s->mmio);
    memory_region_init_io(&s->low_mmio, NULL, &miband_mmio_ops,
                          &s->mmio_banks[1],
                          "miband.unmodeled-low-mmio", LOW_MMIO_SIZE);
    memory_region_add_subregion(sysmem, LOW_MMIO_BASE, &s->low_mmio);
    memory_region_init_io(&s->bt_regs, NULL, &miband_mmio_ops,
                          &s->mmio_banks[2], "miband.bt-registers",
                          BT_REG_SIZE);
    memory_region_add_subregion(sysmem, BT_REG_BASE, &s->bt_regs);
    memory_region_init_io(&s->bt_digital_regs, NULL, &miband_mmio_ops,
                          &s->mmio_banks[5], "miband.bt-digital",
                          BT_REG_SIZE);
    memory_region_add_subregion(sysmem, BT_DIGITAL_BASE, &s->bt_digital_regs);
    memory_region_init_io(&s->bt_cmu_regs, NULL, &miband_mmio_ops,
                          &s->mmio_banks[3], "miband.bt-cmu",
                          BT_REG_SIZE);
    memory_region_add_subregion(sysmem, BT_CMU_BASE, &s->bt_cmu_regs);
    memory_region_init_io(&s->bt_rf_regs, NULL, &miband_mmio_ops,
                          &s->mmio_banks[4], "miband.bt-rf",
                          BT_REG_SIZE);
    memory_region_add_subregion(sysmem, BT_RF_BASE, &s->bt_rf_regs);

    s->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(s->sysclk, 100000000);
    s->refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(s->refclk, 1000000);

    object_initialize_child(OBJECT(machine), "uart1", &s->uart1, TYPE_PL011);
    qdev_prop_set_chr(DEVICE(&s->uart1), "chardev", serial_hd(0));
    qdev_connect_clock_in(DEVICE(&s->uart1), "clk", s->sysclk);
    sysbus_realize(SYS_BUS_DEVICE(&s->uart1), &error_fatal);
    memory_region_add_subregion_overlap(
        sysmem, UART1_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->uart1), 0), 1);

    object_initialize_child(OBJECT(machine), "uart2", &s->uart2, TYPE_PL011);
    if (serial_hd(1)) {
        qdev_prop_set_chr(DEVICE(&s->uart2), "chardev", serial_hd(1));
    }
    qdev_connect_clock_in(DEVICE(&s->uart2), "clk", s->sysclk);
    sysbus_realize(SYS_BUS_DEVICE(&s->uart2), &error_fatal);
    memory_region_add_subregion_overlap(
        sysmem, UART2_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->uart2), 0), 1);

    object_initialize_child(OBJECT(machine), "armv7m", &s->armv7m,
                            TYPE_ARMV7M);
    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", 128);
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->refclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(sysmem), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), &error_fatal);
    /* The AP firmware uses interrupt-driven UART RX.  Wire the PL011 RX
     * interrupt to the BES1503 UART lines; TX remains available through the
     * chardev even when RX is idle. */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart1), 1,
                       qdev_get_gpio_in(armv7m, UART1_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart2), 1,
                       qdev_get_gpio_in(armv7m, UART2_IRQ));
    miband_dma_connect_irq(&s->dma, qdev_get_gpio_in(armv7m, GPDMA_IRQ));
    miband_timer_connect_irq(&s->timers[1],
                             qdev_get_gpio_in(armv7m, AON_TIMER1_IRQ));
    miband_bt_connect_tx_irq(&s->bt, 0,
                             qdev_get_gpio_in(armv7m, BT_TX0_IRQ));
    miband_bt_connect_tx_irq(&s->bt, 1,
                             qdev_get_gpio_in(armv7m, BT_TX1_IRQ));
    miband_bt_connect_rx_irq(&s->bt, 0,
                             qdev_get_gpio_in(armv7m, BT_RX0_IRQ));
    miband_bt_connect_rx_irq(&s->bt, 1,
                             qdev_get_gpio_in(armv7m, BT_RX1_IRQ));
    miband_gpu_connect_irq(&s->gpu, qdev_get_gpio_in(armv7m, GPU_IRQ));
    miband_lcdc_connect_irq(&s->lcdc, qdev_get_gpio_in(armv7m, LCDC_IRQ));
    miband_gpio_connect_irq(&s->gpio, qdev_get_gpio_in(armv7m, GPIO_IRQ));
    miband_ispi_connect_pmu_irq(&s->ispi, qdev_get_gpio_in(armv7m, PMU_IRQ));

    if (s->gpu_worker) {
        if (!miband_display_memory_init(&s->ram, &s->psram))
            error_setg(&error_fatal, "GPU memory tracker cannot claim RAM dirty tracking");
    }
    miband_gpu_set_worker(&s->gpu, first_cpu, s->gpu_worker);
    miband_lcdc_set_async(&s->lcdc, s->lcdc_worker);
    if (s->hci_worker) {
        if (!s->bt.serial_active)
            error_setg(&error_fatal, "hci-worker requires a physical serial backend");
        miband_bt_start_serial_worker(&s->bt, first_cpu, getenv("MIBAND_HCI_TRACE"));
    }
    s->workers_started = true;
    s->worker_exit.notify = miband_workers_exit;
    qemu_add_exit_notifier(&s->worker_exit);
    qemu_register_reset(miband_workers_reset, s);

    if (s->audio_mode && strcmp(s->audio_mode, "off")) {
        if (strcmp(s->audio_mode, "firmware")) {
            error_report("audio-mode hardware is pending model implementation and guest lifecycle validation");
            exit(1);
        }
        if (!miband_audio_init(&s->audio, s->audio_source ? s->audio_source : "mic",
                               s->audio_file, s->audio_device, &error_fatal)) exit(1);
        s->audio_initialized = true;
        miband_audio_adapter_install(&s->audio_adapter, &s->audio, sysmem,
                                     machine->kernel_filename, s->audio_shim, firmware->id,
                                     s->diag_skip_binding);
        armv7m_load_kernel(ARM_CPU(first_cpu), NULL, ap_base, AP_FLASH_SIZE - firmware->flash_offset);
    } else if (s->diag_skip_binding || s->diag_bypass_binding_auth) {
        miband_binding_ui_install(machine->kernel_filename, firmware->id,
                                  s->diag_skip_binding,
                                  s->diag_bypass_binding_auth);
        armv7m_load_kernel(ARM_CPU(first_cpu), NULL, ap_base, AP_FLASH_SIZE - firmware->flash_offset);
    } else {
        armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename,
                           ap_base, AP_FLASH_SIZE - firmware->flash_offset);
    }
}

/* Audio ownership is fixed at startup. Never switch two consumers at runtime. */
#define AUDIO_STRING_PROPERTY(field) \
static char *get_##field(Object *obj, Error **errp) \
{ return g_strdup(MIBAND_MACHINE(obj)->field); } \
static void set_##field(Object *obj, const char *value, Error **errp) \
{ MiBandMachineState *s = MIBAND_MACHINE(obj); \
  if (s->workers_started) { error_setg(errp, "audio configuration is startup-only"); return; } \
  g_free(s->field); s->field = g_strdup(value); }
AUDIO_STRING_PROPERTY(audio_mode)
AUDIO_STRING_PROPERTY(audio_source)
AUDIO_STRING_PROPERTY(audio_file)
AUDIO_STRING_PROPERTY(audio_device)
AUDIO_STRING_PROPERTY(audio_shim)
#undef AUDIO_STRING_PROPERTY

static bool get_skip_binding(Object *obj, Error **errp)
{ return MIBAND_MACHINE(obj)->diag_skip_binding; }
static void set_skip_binding(Object *obj, bool value, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    if (s->workers_started) {
        error_setg(errp, "diag-skip-binding is startup-only"); return;
    }
    s->diag_skip_binding = value;
}

static bool get_bypass_binding_auth(Object *obj, Error **errp)
{ return MIBAND_MACHINE(obj)->diag_bypass_binding_auth; }
static void set_bypass_binding_auth(Object *obj, bool value, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    if (s->workers_started) {
        error_setg(errp, "diag-bypass-binding-auth is startup-only"); return;
    }
    s->diag_bypass_binding_auth = value;
}

static void set_audio_browser(Object *obj, const char *value, Error **errp)
{
    miband_audio_browser_command(&MIBAND_MACHINE(obj)->audio, value, errp);
}

static char *get_audio_stats(Object *obj, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    MiBandPcm *p = &s->audio.pcm;
    GString *json = g_string_new(NULL);
    g_string_append_printf(json, "{\"active\":%s,\"eof\":%s,\"generation\":%"PRIu64
        ",\"input_bytes\":%"PRIu64",\"output_bytes\":%"PRIu64
        ",\"dropped_bytes\":%"PRIu64",\"silence_bytes\":%"PRIu64,
        p->active ? "true" : "false", p->eof ? "true" : "false",
        p->stats.generation, p->stats.input_bytes, p->stats.output_bytes,
        p->stats.dropped_bytes, p->stats.silence_bytes);
    g_string_append_printf(json, ",\"browser\":{\"enabled\":%s,\"connected\":%s,"
        "\"received_bytes\":%"PRIu64",\"rejected_bytes\":%"PRIu64",\"queued_frames\":%u}",
        s->audio.browser ? "true" : "false",
        s->audio.browser_session[0] ? "true" : "false",
        s->audio.browser_received, s->audio.browser_rejected, p->count);
    miband_audio_adapter_append_stats(&s->audio_adapter, json);
    g_string_append(json, "}");
    return g_string_free(json, false);
}

static char *get_audio_profile_sha256(Object *obj, Error **errp)
{
    const MiBandAudioProfile *profile = MIBAND_MACHINE(obj)->audio_adapter.profile;
    return g_strdup(profile ? profile->config_sha256 : "");
}

static char *miband_get_flash_state(Object *obj, Error **errp)
{ return g_strdup(MIBAND_MACHINE(obj)->flash_state); }
static void miband_set_flash_state(Object *obj, const char *value, Error **errp)
{ g_free(MIBAND_MACHINE(obj)->flash_state);MIBAND_MACHINE(obj)->flash_state=g_strdup(value); }

/* Read-only persistence evidence for completed MASS transfers. The catalog
 * deliberately exposes type, length and digest only; installed payloads stay
 * in the QEMU catalog/journal and are never returned through QMP. */
static char *miband_get_install_catalog(Object *obj, Error **errp)
{
    MiBandInstallCatalog *catalog = &MIBAND_MACHINE(obj)->flash_storage.installs;
    GString *summary = g_string_new("");
    bool first = true;

    for (unsigned type = 0; type < G_N_ELEMENTS(catalog->blob); type++) {
        size_t length = catalog->length[type];
        if (!length || !catalog->blob[type]) {
            continue;
        }
        char *digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, catalog->blob[type], length);
        g_string_append_printf(summary, "%s%u:%zu:%s", first ? "" : ",",
                               type, length, digest);
        g_free(digest);
        first = false;
    }
    return g_string_free(summary, false);
}

/* Import a completed MASS payload from the private Web UI resource directory.
 * The setter is intentionally write-only: QMP never returns package bytes.
 * Value format is "<mass type>:<absolute path>" and uses the same catalog
 * commit callback as the Bluetooth receiver, including flash-journal sync. */
static void miband_set_install_file(Object *obj, const char *value,
                                    Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    char *end = NULL;
    guint64 type;
    const char *path;
    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (!value || !*value) {
        error_setg(errp, "install-file requires <mass type>:<path>");
        return;
    }
    type = g_ascii_strtoull(value, &end, 10);
    if (end == value || *end != ':' || type > 255 ||
        (type != MIBAND_XIAOMI_MASS_WATCHFACE &&
         type != MIBAND_XIAOMI_MASS_THIRD_PARTY_APP)) {
        error_setg(errp, "install-file supports MASS types 16 (watchface) and 64 (quick app)");
        return;
    }
    path = end + 1;
    if (!g_path_is_absolute(path) || strlen(path) > 4096) {
        error_setg(errp, "install-file path must be an absolute local path");
        return;
    }
    if (!g_file_get_contents(path, &contents, &length, &error)) {
        error_setg(errp, "cannot read install-file payload: %s",
                   error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }
    if (!length || length > 64u * 1024u * 1024u) {
        error_setg(errp, "install-file payload must be 1..67108864 bytes");
        g_free(contents);
        return;
    }
    if (!s->flash_storage.store ||
        !miband_install_catalog_commit(&s->flash_storage.installs,
                                       (uint8_t)type,
                                       (const uint8_t *)contents, length)) {
        error_setg(errp, "install-file could not commit payload to flash journal");
    }
    g_free(contents);
}

/* Remove one completed MASS record after checking its digest. The digest
 * prevents a stale browser tab from deleting a newer resource of the same
 * MASS type. The storage layer records a tombstone so the deletion survives
 * a QEMU restart. */
static void miband_set_remove_file(Object *obj, const char *value,
                                   Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    char *end = NULL;
    guint64 type;
    const char *digest;
    size_t length = 0;
    const uint8_t *blob;
    char *current;

    if (!value || !*value) {
        error_setg(errp, "remove-file requires <mass type>:<sha256>");
        return;
    }
    type = g_ascii_strtoull(value, &end, 10);
    if (end == value || *end != ':' || type > 255 ||
        (type != MIBAND_XIAOMI_MASS_WATCHFACE &&
         type != MIBAND_XIAOMI_MASS_THIRD_PARTY_APP)) {
        error_setg(errp, "remove-file supports MASS types 16 (watchface) and 64 (quick app)");
        return;
    }
    digest = end + 1;
    if (strlen(digest) != 64) {
        error_setg(errp, "remove-file requires a 64-character SHA-256 digest");
        return;
    }
    for (const char *cursor = digest; *cursor; cursor++) {
        if (!g_ascii_isxdigit(*cursor)) {
            error_setg(errp, "remove-file digest must be hexadecimal");
            return;
        }
    }
    blob = miband_install_catalog_get(&s->flash_storage.installs,
                                      (uint8_t)type, &length);
    if (!blob || !length) {
        error_setg(errp, "remove-file resource is not installed");
        return;
    }
    current = g_compute_checksum_for_data(G_CHECKSUM_SHA256, blob, length);
    if (g_ascii_strcasecmp(current, digest) != 0) {
        error_setg(errp, "remove-file digest does not match the installed resource");
        g_free(current);
        return;
    }
    g_free(current);
    if (!miband_install_catalog_delete(&s->flash_storage.installs,
                                       (uint8_t)type)) {
        error_setg(errp, "remove-file could not commit the deletion tombstone");
    }
}

static char *miband_get_resource_image(Object *obj, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    return g_strdup(s->resource_image);
}

static void miband_set_resource_image(Object *obj, const char *value,
                                      Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    g_free(s->resource_image);
    s->resource_image = g_strdup(value);
}

static char *miband_get_nand_image(Object *obj, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    return g_strdup(s->nand_image);
}

static void miband_set_nand_image(Object *obj, const char *value,
                                  Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    g_free(s->nand_image);
    s->nand_image = g_strdup(value);
}

static char *miband_get_nor_fs_image(Object *obj, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    return g_strdup(s->nor_fs_image);
}

static char *miband_get_bt_backend(Object *obj, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    return g_strdup(s->bt_backend ? s->bt_backend : "synthetic");
}

static void miband_get_bt_tx_rate(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    uint32_t rate = MIBAND_MACHINE(obj)->bt_serial_tx_rate;
    visit_type_uint32(v, name, &rate, errp);
}

static void miband_set_bt_tx_rate(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    uint32_t rate;
    if (!visit_type_uint32(v, name, &rate, errp)) return;
    if (MIBAND_MACHINE(obj)->bt.as) {
        error_setg(errp, "serial TX pacing is startup-only");
        return;
    }
    if (rate && (rate < 64 || rate > 1000000)) {
        error_setg(errp, "serial TX rate must be 0 (off) or 64..1000000 bytes/s");
        return;
    }
    MIBAND_MACHINE(obj)->bt_serial_tx_rate = rate;
}

static void miband_get_bt_rx_flow(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{ bool b=MIBAND_MACHINE(obj)->bt_rx_flow_control; visit_type_bool(v,name,&b,errp); }
static void miband_set_bt_rx_flow(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    bool b;
    if (!visit_type_bool(v,name,&b,errp)) return;
    if (MIBAND_MACHINE(obj)->bt.as) { error_setg(errp,"RX flow policy is startup-only"); return; }
    MIBAND_MACHINE(obj)->bt_rx_flow_control=b;
}

static void miband_get_bt_stats(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{ bool value=false;visit_type_bool(v,name,&value,errp); }
static void miband_set_bt_stats(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    bool value;
    if (!visit_type_bool(v,name,&value,errp)) return;
    if (value && !miband_bt_request_transport_stats(&MIBAND_MACHINE(obj)->bt))
        error_setg(errp,"serial transport inactive or diagnostic request already pending");
}

static void miband_get_bt_debug(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{ uint32_t value = 0; visit_type_uint32(v, name, &value, errp); }
static void miband_set_bt_debug(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint32_t page;
    if (!visit_type_uint32(v, name, &page, errp)) return;
    if (!miband_bt_request_transport_debug(&MIBAND_MACHINE(obj)->bt, page))
        error_setg(errp, "debug page must be 1..3; serial must be active and diagnostic idle");
}

static void miband_set_bt_backend(Object *obj, const char *value,
                                  Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);

    g_free(s->bt_backend);
    s->bt_backend = g_strdup(value && *value ? value : "synthetic");
}

static void miband_get_bt_synthetic_peer_passive(Object *obj, Visitor *v,
                                                 const char *name, void *opaque,
                                                 Error **errp)
{
    visit_type_bool(v, name, &MIBAND_MACHINE(obj)->bt_synthetic_peer_passive,
                    errp);
}

static void miband_set_bt_synthetic_peer_passive(Object *obj, Visitor *v,
                                                 const char *name, void *opaque,
                                                 Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (s->bt.as) {
        error_setg(errp, "synthetic peer mode is startup-only");
        return;
    }
    s->bt_synthetic_peer_passive = value;
}

static char *miband_get_bt_synthetic_inject(Object *obj, Error **errp)
{
    return g_strdup("");
}

static char *miband_get_bt_synthetic_last_write(Object *obj, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint8_t packet[MIBAND_BT_PACKET_MAX];
    unsigned length = miband_bt_copy_synthetic_last_write(&s->bt, packet,
                                                          sizeof(packet));
    char *text = g_malloc(length * 2 + 1);

    for (unsigned i = 0; i < length; i++) {
        g_snprintf(text + i * 2, 3, "%02x", packet[i]);
    }
    text[length * 2] = '\0';
    return text;
}

static char *miband_get_bt_synthetic_last_ap_packet(Object *obj,
                                                    Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint8_t packet[MIBAND_BT_PACKET_MAX];
    unsigned length = miband_bt_copy_synthetic_last_ap_packet(&s->bt, packet,
                                                              sizeof(packet));
    char *text = g_malloc(length * 2 + 1);

    for (unsigned i = 0; i < length; i++) {
        g_snprintf(text + i * 2, 3, "%02x", packet[i]);
    }
    text[length * 2] = '\0';
    return text;
}

static char *miband_get_bt_synthetic_last_bt_packet(Object *obj,
                                                    Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint8_t packet[MIBAND_BT_PACKET_MAX];
    unsigned length = miband_bt_copy_synthetic_last_bt_packet(&s->bt, packet,
                                                              sizeof(packet));
    char *text = g_malloc(length * 2 + 1);

    for (unsigned i = 0; i < length; i++) {
        g_snprintf(text + i * 2, 3, "%02x", packet[i]);
    }
    text[length * 2] = '\0';
    return text;
}

static char *miband_get_bt_synthetic_pop_ap_packet(Object *obj,
                                                   Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint8_t packet[MIBAND_BT_PACKET_MAX];
    unsigned channel = 0;
    unsigned length = miband_bt_pop_synthetic_ap_packet(&s->bt, packet,
                                                         sizeof(packet),
                                                         &channel);
    char *text = g_malloc(3 + length * 2);

    g_snprintf(text, 3, "%u:", channel);
    for (unsigned i = 0; i < length; i++) {
        g_snprintf(text + 2 + i * 2, 3, "%02x", packet[i]);
    }
    text[2 + length * 2] = '\0';
    return text;
}

static void miband_get_bt_synthetic_last_ap_channel(Object *obj, Visitor *v,
                                                     const char *name,
                                                     void *opaque, Error **errp)
{
    uint32_t channel = MIBAND_MACHINE(obj)->bt.synthetic_last_ap_channel;
    visit_type_uint32(v, name, &channel, errp);
}

static int miband_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void miband_set_bt_synthetic_inject(Object *obj, const char *value,
                                           Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint8_t packet[MIBAND_BT_PACKET_MAX];
    unsigned digits = 0, length = 0, channel = 0;
    int high = -1;
    const char *hex = value ? value : "";

    if (!s->bt.as) {
        error_setg(errp, "synthetic peer injection requires an initialized BT controller");
        return;
    }
    if (hex[0] && hex[0] >= '0' && hex[0] <= '1' && hex[1] == ':') {
        channel = (unsigned)(hex[0] - '0');
        hex += 2;
    }
    for (const char *p = hex; *p; p++) {
        int digit = miband_hex_digit(*p);
        if (digit < 0) {
            if (g_ascii_isspace(*p) || *p == ':' || *p == '-') {
                continue;
            }
            error_setg(errp, "bt-synthetic-inject must be hexadecimal HCI bytes");
            return;
        }
        if (++digits > sizeof(packet) * 2) {
            error_setg(errp, "bt-synthetic-inject packet is too large");
            return;
        }
        if (high < 0) {
            high = digit;
        } else {
            packet[length++] = (uint8_t)((high << 4) | digit);
            high = -1;
        }
    }
    if (!digits || high >= 0 ||
        !miband_bt_inject_hci(&s->bt, channel, packet, length)) {
        error_setg(errp, "bt-synthetic-inject packet could not be queued");
    }
}

static void miband_get_bt_smp_just_works(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->bt_smp_just_works, errp);
}

static void miband_set_bt_smp_just_works(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->bt_smp_just_works, errp);
}

static void miband_get_bt_passkey(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_uint32(v, name, &s->bt_passkey, errp);
}

static void miband_set_bt_passkey(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 999999) {
        error_setg(errp, "bt-passkey must be a six-digit value");
        return;
    }
    s->bt_passkey = value;
}

static void miband_get_bt_le_passkey(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_uint32(v, name, &s->bt_le_passkey, errp);
}

static void miband_set_bt_le_passkey(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 999999) {
        error_setg(errp, "bt-le-passkey must be a six-digit value");
        return;
    }
    s->bt_le_passkey = value;
}

static void miband_set_nor_fs_image(Object *obj, const char *value,
                                    Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    g_free(s->nor_fs_image);
    s->nor_fs_image = g_strdup(value);
}

static void miband_get_panel_id(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_uint32(v, name, &s->panel_id, errp);
}

static void miband_set_panel_id(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 0xffffff) {
        error_setg(errp, "panel-id must be a three-byte value");
        return;
    }
    s->panel_id = value;
}

static void miband_get_framebuffer_bpp(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value = s->framebuffer_bpp ? s->framebuffer_bpp : 24;

    visit_type_uint32(v, name, &value, errp);
}

static void miband_get_i2c5_probe_address(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_uint32(v, name, &s->i2c5_probe_address, errp);
}

static void miband_set_i2c5_probe_address(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 0x7f) {
        error_setg(errp, "i2c5-probe-address must be a 7-bit address");
        return;
    }
    s->i2c5_probe_address = value;
}

static void miband_get_i2c5_probe_chip_id(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_uint32(v, name, &s->i2c5_probe_chip_id, errp);
}

static void miband_set_i2c5_probe_chip_id(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value && value != 0xe623 && value != 0xe631) {
        error_setg(errp, "i2c5-probe-chip-id must be 0, 0xe623, or 0xe631");
        return;
    }
    s->i2c5_probe_chip_id = value;
}

static void miband_get_i2c2_probe_address(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_uint32(v, name, &s->i2c2_probe_address, errp);
}

static void miband_set_i2c2_probe_address(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 0x7f) {
        error_setg(errp, "i2c2-probe-address must be a 7-bit address");
        return;
    }
    s->i2c2_probe_address = value;
}

static void miband_get_i2c2_probe_register0(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_uint32(v, name, &s->i2c2_probe_register0, errp);
}

static void miband_set_i2c2_probe_register0(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 0xff) {
        error_setg(errp, "i2c2-probe-register0 must be one byte");
        return;
    }
    s->i2c2_probe_register0 = value;
}

static void miband_set_framebuffer_bpp(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value != 16 && value != 24 && value != 32) {
        error_setg(errp, "framebuffer-bpp must be 16, 24, or 32");
        return;
    }
    s->framebuffer_bpp = value;
}

static void miband_get_gpu_ack_no_render(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->gpu_ack_no_render, errp);
}

static void miband_set_gpu_ack_no_render(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->gpu_ack_no_render, errp);
}

static void miband_get_gpu_render_index8(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->gpu_render_index8, errp);
}

static void miband_set_gpu_render_index8(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->gpu_render_index8, errp);
}

static void miband_get_gpu_scanout_bridge(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->gpu_scanout_bridge, errp);
}

static void miband_set_gpu_scanout_bridge(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->gpu_scanout_bridge, errp);
}

static void miband_get_gpadc_model(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->gpadc_model, errp);
}

static void miband_set_gpadc_model(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->gpadc_model, errp);
}

static void miband_get_pmu_status_model(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->pmu_status_model, errp);
}

static void miband_set_pmu_status_model(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->pmu_status_model, errp);
}

static void miband_get_pmu_rtc_model(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->pmu_rtc_model, errp);
}

static void miband_set_pmu_rtc_model(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_bool(v, name, &s->pmu_rtc_model, errp);
}

static void miband_get_aon_metal_revision(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    visit_type_uint32(v, name, &s->aon_metal_revision, errp);
}

static void miband_set_aon_metal_revision(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 0xffff) {
        error_setg(errp, "aon-metal-revision must be <= 65535");
        return;
    }
    s->aon_metal_revision = value;
}

static void miband_get_charger_model(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    visit_type_bool(v, name, &MIBAND_MACHINE(obj)->charger_model, errp);
}

static void miband_set_charger_model(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    if (s->workers_started) {
        error_setg(errp, "charger-model is a startup-only option");
        return;
    }
    visit_type_bool(v, name, &s->charger_model, errp);
}

/* Board wiring; the SC7061 itself has no SoC MMIO base. */
static void miband_get_charger_i2c_bus(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    visit_type_uint32(v, name, &MIBAND_MACHINE(obj)->charger_i2c_bus, errp);
}

static void miband_set_charger_i2c_bus(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;
    if (s->workers_started) {
        error_setg(errp, "charger-i2c-bus is startup-only");
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value >= MIBAND_I2C_COUNT) {
        error_setg(errp, "charger-i2c-bus must select a controller from 0 to 5");
        return;
    }
    s->charger_i2c_bus = value;
}

static char *miband_get_board_profile(Object *obj, Error **errp)
{
    return g_strdup(MIBAND_MACHINE(obj)->board->id);
}

static char *miband_get_board_config_sha256(Object *obj, Error **errp)
{
    return g_strdup(MIBAND_MACHINE(obj)->board->config_sha256);
}

static void miband_select_board(MiBandMachineState *s,
                                 const MiBandBoardConfig *board)
{
    s->board = board;
    s->display_width = board->width;
    s->display_height = board->height;
    s->framebuffer_bpp = board->framebuffer_bpp;
    s->charger_i2c_bus = board->charger_bus;
    s->panel_id = board->panel_id;
}

static void miband_set_board_profile(Object *obj, const char *value, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    if (s->workers_started) {
        error_setg(errp, "board-profile is startup-only");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(miband_boards); i++) {
        if (!g_strcmp0(value, miband_boards[i].id)) {
            miband_select_board(s, &miband_boards[i]);
            return;
        }
    }
    error_setg(errp, "unknown board-profile: %s", value ? value : "(null)");
}

static void miband_get_display_width(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    visit_type_uint32(v, name, &MIBAND_MACHINE(obj)->display_width, errp);
}

static void miband_set_display_width(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;
    if (s->workers_started) {
        error_setg(errp, "display-width is startup-only");
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value || value > 1024) {
        error_setg(errp, "display-width must be between 1 and 1024");
        return;
    }
    s->display_width = value;
}

static void miband_get_display_height(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    visit_type_uint32(v, name, &MIBAND_MACHINE(obj)->display_height, errp);
}

static void miband_set_display_height(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;
    if (s->workers_started) {
        error_setg(errp, "display-height is startup-only");
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value || value > 1024) {
        error_setg(errp, "display-height must be between 1 and 1024");
        return;
    }
    s->display_height = value;
}

static void miband_get_charger_state(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    int64_t level = s->charger.level;
    bool charging = s->charger.charging;
    if (!s->workers_started || !s->charger.enabled) {
        error_setg(errp, "charger model is unavailable; start with charger-model=on");
        return;
    }
    if (!visit_start_struct(v, name, NULL, 0, errp)) {
        return;
    }
    if (visit_type_int(v, "level", &level, errp) &&
        visit_type_bool(v, "charging", &charging, errp)) {
        visit_check_struct(v, errp);
    }
    visit_end_struct(v, NULL);
}

static void miband_set_charger_state(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    int64_t level;
    bool charging;
    bool valid;
    if (!s->workers_started || !s->charger.enabled) {
        error_setg(errp, "charger model is unavailable; start with charger-model=on");
        return;
    }
    if (!visit_start_struct(v, name, NULL, 0, errp)) {
        return;
    }
    valid = visit_type_int(v, "level", &level, errp) &&
            visit_type_bool(v, "charging", &charging, errp) &&
            visit_check_struct(v, errp);
    visit_end_struct(v, NULL);
    if (!valid) {
        return;
    }
    if (level < 0 || level > 100) {
        error_setg(errp, "charger level must be an integer between 0 and 100");
        return;
    }
    /* QMP and MMIO serialize device state under the BQL. No guest RAM is
     * patched: the firmware observes the new environment through hardware. */
    if (!miband_charger_apply(&s->charger, level, charging)) {
        error_setg(errp, "charger state could not be applied");
    }
}

#define WORKER_PROPERTY(name, field) \
static void miband_get_##name(Object *obj, Visitor *v, const char *key, \
                              void *opaque, Error **errp) \
{ visit_type_bool(v, key, &MIBAND_MACHINE(obj)->field, errp); } \
static void miband_set_##name(Object *obj, Visitor *v, const char *key, \
                              void *opaque, Error **errp) \
{ \
    MiBandMachineState *s = MIBAND_MACHINE(obj); \
    if (s->workers_started) { error_setg(errp, "worker options are startup-only"); return; } \
    visit_type_bool(v, key, &s->field, errp); \
}
WORKER_PROPERTY(hci_worker, hci_worker)
WORKER_PROPERTY(lcdc_worker, lcdc_worker)
WORKER_PROPERTY(gpu_worker, gpu_worker)
WORKER_PROPERTY(flash_worker, flash_worker)

static void miband_get_guest_reset_shutdown(Object *obj, Visitor *v,
                                             const char *key, void *opaque,
                                             Error **errp)
{
    visit_type_bool(v, key, &MIBAND_MACHINE(obj)->guest_reset_shutdown, errp);
}

static void miband_set_guest_reset_shutdown(Object *obj, Visitor *v,
                                             const char *key, void *opaque,
                                             Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    if (s->workers_started) {
        error_setg(errp, "guest-reset-shutdown is startup-only");
        return;
    }
    visit_type_bool(v, key, &s->guest_reset_shutdown, errp);
}

static void miband_get_boot_vector(Object *obj, Visitor *v,
                                    const char *name, void *opaque, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value = !strcmp(name, "boot-sp") ? s->boot_sp : s->boot_pc;
    visit_type_uint32(v, name, &value, errp);
}

static void miband_set_boot_vector(Object *obj, Visitor *v,
                                    const char *name, void *opaque, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    uint32_t value;
    /* QOM applies properties independently of command-line ordering. The
     * firmware binding is checked at realization, after all properties. */
    const uint32_t ap_base = AP_XIP_BASE;
    const uint32_t ap_alias_nc = AP_NC_BASE;
    const uint32_t ap_alias_c = AP_C_BASE;
    bool stack = !strcmp(name, "boot-sp");
    if (s->workers_started) {
        error_setg(errp, "%s is startup-only", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (stack) {
        if ((value & 7) || value < AP_RAM_BASE || value > AP_RAM_BASE + AP_RAM_SIZE) {
            error_setg(errp, "boot-sp must be aligned inside AP RAM");
            return;
        }
        s->boot_sp = value;
    } else {
        uint32_t pc = value & ~1u;
        if (!(value & 1) || !((pc >= ap_base && pc < ap_base + AP_FLASH_SIZE) ||
              (pc >= ap_alias_nc && pc < ap_alias_nc + AP_FLASH_SIZE) ||
              (pc >= ap_alias_c && pc < ap_alias_c + AP_FLASH_SIZE))) {
            error_setg(errp, "boot-pc must be a Thumb address in AP XIP");
            return;
        }
        s->boot_pc = value;
    }
}

static char *miband_get_firmware_profile(Object *obj, Error **errp)
{
    return g_strdup(MIBAND_MACHINE(obj)->firmware->id);
}

static char *miband_get_firmware_config_sha256(Object *obj, Error **errp)
{
    return g_strdup(MIBAND_MACHINE(obj)->firmware->config_sha256);
}

static char *miband_get_package_config_sha256(Object *obj, Error **errp)
{
    return g_strdup(BEST1503_PACKAGE_CONFIG_SHA256);
}

static void miband_set_firmware_profile(Object *obj, const char *value, Error **errp)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    if (s->workers_started) {
        error_setg(errp, "firmware-profile is startup-only");
        return;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(miband_firmwares); i++) {
        if (!strcmp(value, miband_firmwares[i].id)) {
            s->firmware = &miband_firmwares[i];
            s->boot_sp = s->firmware->boot_sp;
            s->boot_pc = s->firmware->boot_pc;
            s->firmware_selected = true;
            return;
        }
    }
    error_setg(errp, "unknown loaded firmware profile %s", value);
}

static void miband_class_init(ObjectClass *oc, void *data)
{
    static const char * const cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m33"), NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);
    bes_runtime_config_property(oc);
    object_class_property_add_str(oc, "audio-mode", get_audio_mode, set_audio_mode);
    object_class_property_add_str(oc, "audio-source", get_audio_source, set_audio_source);
    object_class_property_add_str(oc, "audio-file", get_audio_file, set_audio_file);
    object_class_property_add_str(oc, "audio-device", get_audio_device, set_audio_device);
    object_class_property_add_str(oc, "audio-shim", get_audio_shim, set_audio_shim);
    object_class_property_add_str(oc, "audio-stats", get_audio_stats, NULL);
    object_class_property_add_str(oc, "audio-profile-sha256", get_audio_profile_sha256, NULL);
    object_class_property_add_bool(oc, "diag-skip-binding", get_skip_binding, set_skip_binding);
    object_class_property_add_bool(oc, "diag-bypass-binding-auth",
                                  get_bypass_binding_auth, set_bypass_binding_auth);
    object_class_property_add_str(oc, "audio-browser", NULL, set_audio_browser);

    mc->desc = "BES1503 configurable AP platform (Cortex-M33)";
    mc->init = miband_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->valid_cpu_types = cpu_types;
    mc->default_ram_size = AP_RAM_SIZE;
    mc->max_cpus = 1;
    object_class_property_add_str(oc, "firmware-profile",
                                  miband_get_firmware_profile, miband_set_firmware_profile);
    object_class_property_add_str(oc, "firmware-config-sha256",
                                  miband_get_firmware_config_sha256, NULL);
    object_class_property_add_str(oc, "package-config-sha256",
                                  miband_get_package_config_sha256, NULL);
    object_class_property_add(oc, "boot-sp", "uint32",
                              miband_get_boot_vector, miband_set_boot_vector, NULL, NULL);
    object_class_property_add(oc, "boot-pc", "uint32",
                              miband_get_boot_vector, miband_set_boot_vector, NULL, NULL);
    object_class_property_add(oc, "charger-model", "bool",
                              miband_get_charger_model,
                              miband_set_charger_model, NULL, NULL);
    object_class_property_add(oc, "charger-i2c-bus", "uint32",
                              miband_get_charger_i2c_bus,
                              miband_set_charger_i2c_bus, NULL, NULL);
    object_class_property_add_str(oc, "board-config-sha256",
                                  miband_get_board_config_sha256, NULL);
    object_class_property_add_str(oc, "board-profile",
                                  miband_get_board_profile,
                                  miband_set_board_profile);
    object_class_property_add(oc, "display-width", "uint32",
                              miband_get_display_width,
                              miband_set_display_width, NULL, NULL);
    object_class_property_add(oc, "display-height", "uint32",
                              miband_get_display_height,
                              miband_set_display_height, NULL, NULL);
    object_class_property_add(oc, "charger-state", "MiBandChargerState",
                              miband_get_charger_state,
                              miband_set_charger_state, NULL, NULL);
    object_class_property_add(oc, "hci-worker", "bool", miband_get_hci_worker, miband_set_hci_worker, NULL, NULL);
    object_class_property_add(oc, "lcdc-worker", "bool", miband_get_lcdc_worker, miband_set_lcdc_worker, NULL, NULL);
    object_class_property_add(oc, "gpu-worker", "bool", miband_get_gpu_worker, miband_set_gpu_worker, NULL, NULL);
    object_class_property_add(oc, "flash-worker", "bool", miband_get_flash_worker, miband_set_flash_worker, NULL, NULL);
    object_class_property_add(oc, "guest-reset-shutdown", "bool",
                              miband_get_guest_reset_shutdown,
                              miband_set_guest_reset_shutdown, NULL, NULL);
    object_class_property_add_str(oc, "resource-image",
                                  miband_get_resource_image,
                                  miband_set_resource_image);
    object_class_property_add_str(oc,"flash-state",miband_get_flash_state,miband_set_flash_state);
    object_class_property_add_str(oc, "install-catalog",
                                  miband_get_install_catalog, NULL);
    object_class_property_add_str(oc, "install-file", NULL,
                                  miband_set_install_file);
    object_class_property_add_str(oc, "remove-file", NULL,
                                  miband_set_remove_file);
    object_class_property_add_str(oc, "nand-image",
                                  miband_get_nand_image,
                                  miband_set_nand_image);
    object_class_property_add_str(oc, "nor-fs-image",
                                  miband_get_nor_fs_image,
                                  miband_set_nor_fs_image);
    object_class_property_add_str(oc, "bt-backend",
                                  miband_get_bt_backend,
                                  miband_set_bt_backend);
    object_class_property_add(oc, "bt-synthetic-peer-passive", "bool",
                              miband_get_bt_synthetic_peer_passive,
                              miband_set_bt_synthetic_peer_passive,
                              NULL, NULL);
    object_class_property_add_str(oc, "bt-synthetic-inject",
                                  miband_get_bt_synthetic_inject,
                                  miband_set_bt_synthetic_inject);
    object_class_property_add_str(oc, "bt-synthetic-last-write",
                                  miband_get_bt_synthetic_last_write, NULL);
    object_class_property_add_str(oc, "bt-synthetic-last-ap-packet",
                                  miband_get_bt_synthetic_last_ap_packet, NULL);
    object_class_property_add_str(oc, "bt-synthetic-last-bt-packet",
                                  miband_get_bt_synthetic_last_bt_packet, NULL);
    object_class_property_add_str(oc, "bt-synthetic-pop-ap-packet",
                                  miband_get_bt_synthetic_pop_ap_packet, NULL);
    object_class_property_add(oc, "bt-synthetic-last-ap-channel", "uint32",
                              miband_get_bt_synthetic_last_ap_channel, NULL,
                              NULL, NULL);
    object_class_property_add(oc,"bt-transport-stats","bool",miband_get_bt_stats,
                              miband_set_bt_stats,NULL,NULL);
    object_class_property_add(oc, "bt-rx-flow-control", "bool",
                              miband_get_bt_rx_flow, miband_set_bt_rx_flow, NULL, NULL);
    object_class_property_add(oc, "bt-serial-tx-rate", "uint32",
                              miband_get_bt_tx_rate, miband_set_bt_tx_rate, NULL, NULL);
    object_class_property_add(oc, "bt-transport-debug-page", "uint32",
                              miband_get_bt_debug, miband_set_bt_debug, NULL, NULL);
    object_class_property_add(oc, "bt-smp-just-works", "bool",
                              miband_get_bt_smp_just_works,
                              miband_set_bt_smp_just_works, NULL, NULL);
    object_class_property_add(oc, "bt-passkey", "uint32",
                              miband_get_bt_passkey, miband_set_bt_passkey,
                              NULL, NULL);
    object_class_property_add(oc, "bt-le-passkey", "uint32",
                              miband_get_bt_le_passkey,
                              miband_set_bt_le_passkey, NULL, NULL);
    object_class_property_add(oc, "panel-id", "uint32",
                              miband_get_panel_id, miband_set_panel_id,
                              NULL, NULL);
    object_class_property_add(oc, "framebuffer-bpp", "uint32",
                              miband_get_framebuffer_bpp,
                              miband_set_framebuffer_bpp, NULL, NULL);
    object_class_property_add(oc, "gpu-ack-no-render", "bool",
                              miband_get_gpu_ack_no_render,
                              miband_set_gpu_ack_no_render, NULL, NULL);
    object_class_property_add(oc, "gpu-render-index8", "bool",
                              miband_get_gpu_render_index8,
                              miband_set_gpu_render_index8, NULL, NULL);
    object_class_property_add(oc, "gpu-scanout-bridge", "bool",
                              miband_get_gpu_scanout_bridge,
                              miband_set_gpu_scanout_bridge, NULL, NULL);
    object_class_property_add(oc, "gpadc-model", "bool",
                              miband_get_gpadc_model,
                              miband_set_gpadc_model, NULL, NULL);
    object_class_property_add(oc, "pmu-status-model", "bool",
                              miband_get_pmu_status_model,
                              miband_set_pmu_status_model, NULL, NULL);
    object_class_property_add(oc, "aon-metal-revision", "uint32",
                              miband_get_aon_metal_revision,
                              miband_set_aon_metal_revision, NULL, NULL);
    object_class_property_add(oc, "pmu-rtc-model", "bool",
                              miband_get_pmu_rtc_model,
                              miband_set_pmu_rtc_model, NULL, NULL);
    object_class_property_add(oc, "i2c5-probe-address", "uint32",
                              miband_get_i2c5_probe_address,
                              miband_set_i2c5_probe_address, NULL, NULL);
    object_class_property_add(oc, "i2c5-probe-chip-id", "uint32",
                              miband_get_i2c5_probe_chip_id,
                              miband_set_i2c5_probe_chip_id, NULL, NULL);
    object_class_property_add(oc, "i2c2-probe-address", "uint32",
                              miband_get_i2c2_probe_address,
                              miband_set_i2c2_probe_address, NULL, NULL);
    object_class_property_add(oc, "i2c2-probe-register0", "uint32",
                              miband_get_i2c2_probe_register0,
                              miband_set_i2c2_probe_register0, NULL, NULL);
}

static void miband_instance_init(Object *obj)
{
    MiBandMachineState *s = MIBAND_MACHINE(obj);
    miband_select_board(s, &miband_boards[0]);
    miband_set_firmware_profile(obj, MIBAND_DEFAULT_FIRMWARE_ID, &error_fatal);
    s->firmware_selected = false;
    s->boot_sp = MIBAND_DEFAULT_BOOT_SP;
    s->boot_pc = MIBAND_DEFAULT_BOOT_PC;
}

static const TypeInfo miband_info = {
    .name = TYPE_MIBAND_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MiBandMachineState),
    .instance_init = miband_instance_init,
    .class_init = miband_class_init,
};

static void miband_register(void)
{
    type_register_static(&miband_info);
}

type_init(miband_register)
