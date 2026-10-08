/* BEST1700 execution with optional AP secure-world boot.
 *
 * AP/TEE share one Cortex-M55, CP has a second M55 and BTH uses Cortex-M33.
 * CP and BTH can be held and released through their hardware reset controls.
 * RPMsg transport is guest-owned in hardware mode. HIFI4 remains
 * an endpoint until a CPU backend is available. Image identity and all firmware addresses are
 * supplied by the launcher/profile; this file contains no Watch-specific
 * hook addresses.
 */
#include "qemu/osdep.h"
#include "bes_runtime_config.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/armv7m.h"
#include "target/arm/idau.h"
#include "hw/arm/boot.h"
#include "target/arm/cpu.h"
#include "hw/boards.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "qemu/bswap.h"
#include "qom/object.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "miband_display_bridge.h"
#include "miband_lcdc.h"
#include "miband_gpu.h"
#include "miband_panel_device.h"
#include "best1700_ispi.h"
#include "best1700_ispi_bus.h"
#include "best1700_spi.h"
#include "best1700_apb_window.h"
#include "best1700_uart.h"
#include "best1700_aon.h"
#include "best1700_memsc.h"
#include "best1700_bth.h"
#include "best1700_btc_cmu.h"
#include "best1700_btc_aon.h"
#include "best1700_btc_power.h"
#include "best1700_bth_power.h"
#include "best1700_btc_cpudump.h"
#include "best1700_btc_gpio.h"
#include "best1700_btc_modem.h"
#include "best1700_rf_matrix.h"
#include "best1700_btc_baseband.h"
#include "best1700_cache.h"
#include "best1700_bth_timing.h"
#include "best1700_bth_memsc.h"
#include "best1700_cmu_clock.h"
#include "best1700_i2c.h"
#include "best1700_dsi.h"
#include "best1700_gpio.h"
#include "best1700_sdhci.h"
#include "best1700_tgu.h"
#include "best1700_flash.h"
#include "best1700_psram.h"
#include "best1700_page_spy.h"
#include "best1700_mpc.h"
#include "best1700_tz_trigger.h"
#include "best1700_sec.h"
#include "best1700_master_gate.h"
#include "best1700_hifi4_endpoint.h"
#include "best1700_cp.h"
#include "best1700_sys_bth_ipc.h"
#include "best1700_btc_ipc.h"
#include "best1700_dma.h"
#include "best1700_m55_ipc.h"
#include "miband_emmc.h"

#define TYPE_MIBAND_BEST1700 MACHINE_TYPE_NAME("miband-best1700")
OBJECT_DECLARE_SIMPLE_TYPE(Best1700MachineState, MIBAND_BEST1700)

#define BEST1700_CORE_COUNT 3
#define BEST1700_IMAGE_COUNT 4
#define BEST1700_FLASH_SIZE 0x01000000u
#define BEST1700_BTH_FLASH_NC_BASE 0x28000000u
#define BEST1700_BTH_FLASH_BASE 0x2c000000u
#define BEST1700_BTH_FLASHX_BASE 0x0c000000u
#define BEST1700_RAM_BASE 0x20000000u
#define BEST1700_RAM_SIZE 0x08000000u
#define BEST1700_M55_DTCM_AXI_BASE 0x21000000u
#define BEST1700_M55C1_DTCM_AXI_BASE 0x21c00000u
#define BEST1700_M55_DTCM_SIZE 0x00400000u
#define BEST1700_PSRAM_BASE 0x18000000u
#define BEST1700_PSRAM_SIZE 0x04000000u
#define BEST1700_SYS_SRAM_BASE 0x02000000u
#define BEST1700_SYS_SRAM_DATA_BASE 0x22000000u
#define BEST1700_SYS_SRAM_BANK_SIZE 0x00100000u
#define BEST1700_SYS_SRAM_BANKS 8
#define BEST1700_FLASH_MPC_BASE 0x40054000u
#define BEST1700_PSRAM_MPC_BASE 0x40056000u
#define BEST1700_SRAM_MPC_BASE 0x4005a000u
#define BEST1700_SRAM_MPC_STRIDE 0x00002000u
#define BEST1700_SHARED_BASE 0x23c00000u
#define BEST1700_SHARED_SIZE 0x00040000u
#define BEST1700_DOORBELL_BASE 0x60080000u
#define BEST1700_DOORBELL_SIZE 0x00010000u
#define BEST1700_AON_CMU_BASE 0x40080000u
#define BEST1700_AON_WDT_BASE 0x40082000u
#define BEST1700_AON_TIMER0_BASE 0x40083000u
#define BEST1700_AON_TIMER1_BASE 0x40084000u
#define BEST1700_SYS_TIMER0_BASE 0x40002000u
#define BEST1700_SLOW_TIMER_HZ 16384u
#define BEST1700_FAST_TIMER_HZ 6000000u
#define BEST1700_AON_PSC_BASE 0x40085000u
#define BEST1700_AON_GENERIC_BASE 0x40081000u
#define BEST1700_AON_GENERIC_SIZE 0x00010000u
#define BEST1700_AON_WINDOW_SIZE 0x00001000u
#define BEST1700_LOW_RAM_BASE 0x00100000u
#define BEST1700_LOW_RAM_SIZE 0x00100000u
#define BEST1700_SYSCTRL_BASE 0x40000000u
#define BEST1700_SYSCTRL_SIZE 0x00010000u
#define BEST1700_SYS_PERIPH_BASE 0x40100000u
#define BEST1700_SYS_PERIPH_SIZE 0x00300000u
#define BEST1700_BTH_PERIPH_BASE 0x50000000u
#define BEST1700_BTH_PERIPH_SIZE 0x00200000u
#define BEST1700_ISPI_BASE 0x4000a000u
#define BEST1700_ISPI_SIZE 0x00000100u
#define BEST1700_AP_BASE 0x10000000u
#define BEST1700_STUB_PC (BEST1700_RAM_BASE + 0x101u)
#define BEST1700_DISPLAY_WIDTH 480
#define BEST1700_DISPLAY_HEIGHT 480
#define BEST1700_LCDC_BASE 0x401c0000u
#define BEST1700_LCDC_SIZE 0x00000400u
#define BEST1700_LCDC_IRQ 27
#define BEST1700_DSI_BASE 0x401d0000u
#define BEST1700_DSI_IRQ 28
#define BEST1700_GPU_BASE 0x401f0000u
#define BEST1700_GPU_SIZE 0x00001000u
#define BEST1700_GPU_IRQ 33
#define BEST1700_FLASH_CTRL_BASE 0x40190000u
#define BEST1700_PSRAM_CTRL_BASE 0x40320000u
#define BEST1700_DMA0_IRQ 37
#define BEST1700_DMA1_IRQ 38
#define BEST1700_DMA0_OFFSET 0x000a0000u
#define BEST1700_DMA1_OFFSET 0x000a8000u
#define BEST1700_DMA_SIZE 0x00008000u
#define BEST1700_DMA0_ALIAS_BASE 0x400a0000u
#define BEST1700_DMA1_ALIAS_BASE 0x400a8000u
#define BEST1700_DMA_REG_BYTES 0x00000800u
#define BEST1700_DMA_CHANNELS 6
#define BEST1700_DMA_CH_BASE 0x00000100u
#define BEST1700_DMA_CH_STRIDE 0x00000020u
#define BEST1700_DMA_CH_SRC 0x00000000u
#define BEST1700_DMA_CH_DST 0x00000004u
#define BEST1700_DMA_CH_LLI 0x00000008u
#define BEST1700_DMA_CH_CONTROL 0x0000000cu
#define BEST1700_DMA_CH_CFG 0x00000010u
#define BEST1700_DMA_INT_STATUS 0x00000000u
#define BEST1700_DMA_INT_TC_STATUS 0x00000004u
#define BEST1700_DMA_INT_TC_CLEAR 0x00000008u
#define BEST1700_DMA_INT_ERR_STATUS 0x0000000cu
#define BEST1700_DMA_INT_ERR_CLEAR 0x00000010u
#define BEST1700_DMA_RAW_TC_STATUS 0x00000014u
#define BEST1700_DMA_RAW_ERR_STATUS 0x00000018u
#define BEST1700_DMA_ENABLED_CHANNELS 0x0000001cu
#define BEST1700_DMA_CONFIG 0x00000030u
#define BEST1700_DMA_DESCRIPTOR_BYTES 16u
#define BEST1700_DMA_TICK_NS 1000
#define BEST1700_UART_SIZE 0x00001000u
#define BEST1700_UART0_BASE 0x4000b000u
#define BEST1700_UART1_BASE 0x4000c000u
#define BEST1700_UART2_BASE 0x40011000u
#define BEST1700_I2C_COUNT 5

static const hwaddr best1700_page_spy_base[] = {
    0x40042000, 0x40044000, 0x40046000, 0x40048000,
    0x4004a000, 0x4004c000, 0x4004e000, 0x40050000,
    0x40055000, 0x40057000, 0x40059000, 0x4005b000,
    0x4005d000, 0x4005f000, 0x40061000, 0x40063000,
    0x40065000, 0x40067000, 0x40069000, 0x4006b000,
};

static bool best1700_doorbell_overlaps_aon(uint32_t base)
{
    /* Keep legacy command lines that still pass 0x40080000 from shadowing
     * the AON CMU/WDT/timer apertures.  The profile's synthetic doorbells
     * use BEST1700_DOORBELL_BASE by default. */
    return base < BEST1700_AON_TIMER1_BASE + BEST1700_AON_WINDOW_SIZE &&
           base + BEST1700_DOORBELL_SIZE > BEST1700_AON_CMU_BASE;
}

typedef struct Best1700Core {
    ARMv7MState cpu;
    MemoryRegion space;
    AddressSpace address_space;
    AddressSpace flash_storage_as;
    Best1700SecState *dma_sec;
    qemu_irq dma_irq[2];
    QEMUTimer *dma_timer;
    MemoryRegion flash;
    MemoryRegion flash_nc;
    MemoryRegion flash_x;
    MemoryRegion bth_flash_x;
    MemoryRegion bth_ram[2];
    MemoryRegion bth_cache[2];
    MemoryRegion bth_flash_controller_view;
    MemoryRegion bth_timing_cmu;
    MemoryRegion bth_memsc_cmu;
    MemoryRegion bth_i2c[BEST1700_I2C_COUNT];
    MemoryRegion bth_spi[2];
    MemoryRegion bth_timers[BEST1700_BTH_TIMING_COUNT];
    MemoryRegion shared_sys_ram[4];
    MemoryRegion sys_bth_ipc_bank[2];
    MemoryRegion ram;
    MemoryRegion dtcm_axi;
    MemoryRegion itcm_axi;
    MemoryRegion psram;
    MemoryRegion psram_nc;
    MemoryRegion low_ram;
    MemoryRegion secure_itcm;
    MemoryRegion secure_dtcm;
    MemoryRegion secure_peripherals;
    MemoryRegion sysctrl;
    MemoryRegion sys_periph;
    MemoryRegion sys_ispi;
    MemoryRegion dma0_alias;
    MemoryRegion dma1_alias;
    Best1700ApbWindow bth_periph;
    Best1700Uart uart0;
    Best1700Uart uart1;
    Best1700Uart uart2;
    Best1700Aon aon_cmu;
    Best1700Aon aon_wdt;
    Best1700Aon aon_timer0;
    Best1700Aon aon_timer1;
    Best1700Aon sys_timer0;
    Best1700Aon aon_psc;
    Best1700Aon aon_generic;
    MemoryRegion vectors;
    MemoryRegion shared;
    MemoryRegion doorbell;
    const char *name;
    uint32_t flash_base;
    uint32_t code_offset;
    uint32_t boot_rom_size;
    uint32_t boot_sp;
    uint32_t boot_pc;
    bool image_loaded;
    bool ready;
    bool secure_boot;
    bool real_bth;
    bool real_cp;
    bool real_btc;
    void *peer_reset_opaque;
    void (*peer_reset_done)(void *opaque);
    bool peer_needs_reset;
    bool peer_waiting;
    bool peer_running;
    bool peer_reset_pending;
    bool peer_held;
    uint64_t peer_reset_generation;
    uint32_t sysctrl_regs[BEST1700_SYSCTRL_SIZE / sizeof(uint32_t)];
    uint32_t sysctrl_reads;
    uint32_t sysctrl_writes;
    Best1700Memsc *sys_memsc;
    Best1700Hifi4Endpoint *hifi4;
    Best1700M55Ipc *m55_ipc;
    uint64_t fast_ticks;
    int64_t fast_epoch_ns;
    uint32_t sys_periph_reads;
    uint32_t sys_periph_writes;
    uint32_t dma_reads;
    uint32_t dma_writes;
    uint32_t dma_regs[2][BEST1700_DMA_REG_BYTES / sizeof(uint32_t)];
    uint32_t dma_pending[2];
} Best1700Core;

/* Each bus view refers to the same controller storage and CMU. */
typedef struct Best1700BtcView {
    MemoryRegion ram;
    MemoryRegion ramrun[2];
    MemoryRegion exchange;
    MemoryRegion cmu;
    MemoryRegion uart;
    MemoryRegion aon;
    MemoryRegion retention;
    MemoryRegion power;
    MemoryRegion cpudump;
    MemoryRegion gpio;
    MemoryRegion modem;
    MemoryRegion baseband;
    MemoryRegion ipc[BEST1700_BTC_IPC_BANKS];
    MemoryRegion dma;
} Best1700BtcView;

typedef struct Best1700Channel {
    uint32_t shared_base;
    uint32_t doorbell;
    uint16_t vring_size;
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t resets;
    bool registered;
    bool ready;
    bool shutdown;
} Best1700Channel;

struct Best1700MachineState {
    MachineState parent_obj;
    Best1700Core core[BEST1700_CORE_COUNT];
    Best1700Core btc_core;
    Best1700BtcView btc_view[BEST1700_CORE_COUNT + 1];
    Best1700BtcCmu btc_cmu;
    Best1700BtcAon btc_aon;
    Best1700BtcPower btc_power;
    bool btc_ipc_wake;
    Best1700BtcCpudump btc_cpudump;
    Best1700BtcGpio btc_gpio;
    Best1700BtcModem btc_modem;
    Best1700RfMatrix rf_matrix;
    MemoryRegion rf_matrix_view[BEST1700_CORE_COUNT];
    Best1700BtcBaseband btc_baseband;
    Best1700BtcIpc btc_ipc;
    Best1700Dma btc_dma;
    Best1700Dma bth_dma[2];
    MemoryRegion btc_ram;
    MemoryRegion btc_exchange;
    Best1700Uart btc_uart;
    bool btc_real_boot;
    uint32_t btc_cal_ref_hz;
    uint32_t btc_lpo_hz;
    uint32_t btc_correction_ns;
    uint32_t btc_cpudump_sample_ns;
    MemoryRegion shared_storage;
    MemoryRegion system_view;
    MemoryRegion bth_flash_view[3];
    Best1700Bth bth;
    Best1700BthPower bth_power;
    MemoryRegion bth_power_view[BEST1700_CORE_COUNT + 1];
    qemu_irq *bth_irq;
    bool bth_irq_level[128];
    bool bth_cmu_reset;
    bool bth_wake_aggregate;
    Best1700SysBthIpc sys_bth_ipc;
    Best1700Cache bth_cache[2];
    Best1700BthTiming bth_timing;
    Best1700Spi bth_spi[2];
    Best1700BthMemsc bth_memsc;
    bool bth_real_boot;
    bool cp_real_boot;
    Best1700Cp cp_control;
    Best1700M55Ipc m55_ipc;
    MemoryRegion cp_psram_view;
    MemoryRegion cp_itcm_view;
    MemoryRegion cp_dtcm_view;
    MemoryRegion cp_ap_itcm_view;
    MemoryRegion cp_ap_dtcm_view;
    MemoryRegion cp_sys_sram[2];
    MemoryRegion cp_sram_master_view;
    Best1700MasterGate cp_psram_gate;
    Best1700MasterGate cp_ap_itcm_gate;
    Best1700MasterGate cp_ap_dtcm_gate;
    Best1700MasterGate cp_sram_gate[BEST1700_SYS_SRAM_BANKS];
    Notifier machine_done;
    Best1700I2CState i2c[BEST1700_I2C_COUNT];
    Best1700I2CState bth_i2c[BEST1700_I2C_COUNT];
    Best1700DsiState dsi;
    Best1700GpioState gpio;
    Best1700SdhciState emmc;
    Best1700TguState tgu;
    Best1700FlashState flash_controller;
    Best1700FlashState bth_flash_controller;
    Best1700PsramState psram_controller;
    Best1700PageSpyState page_spy[G_N_ELEMENTS(best1700_page_spy_base)];
    Best1700TzTriggerState tz_trigger;
    Best1700SecState sec;
    Best1700MpcState flash_mpc;
    Best1700MpcState psram_mpc;
    Best1700MpcState sram_mpc[BEST1700_SYS_SRAM_BANKS];
    MemoryRegion sram_storage[BEST1700_SYS_SRAM_BANKS];
    MemoryRegion sram_data[BEST1700_SYS_SRAM_BANKS];
    MemoryRegion itcm_storage;
    MemoryRegion itcm_ram;
    MemoryRegion itcm_vectors;
    MemoryRegion dtcm_storage;
    MemoryRegion emmc_dma;
    char *emmc_drive;
    uint32_t panel_te_pin;
    char *panel_read_responses;
    GBytes *panel_reads[256];
    Best1700Memsc sys_memsc;
    Best1700Memsc aon_memsc;
    Clock *sysclk;
    Clock *refclk;
    char *cp_image;
    char *bth_image;
    char *tee_image;
    char *audio_image;
    char *audio_endpoint_profile;
    Best1700Hifi4Endpoint *hifi4;
    bool dsp_reset_asserted;
    bool dsp_started;
    uint32_t dsp_runstall;
    uint32_t dsp_core_wait;
    uint32_t dsp_vector;
    char *profile_id;
    char *core_status;
    char *ipc_status;
    uint32_t boot_sp;
    uint32_t boot_pc;
    uint32_t boot_rom_size;
    bool workers_started;
    bool secure_boot;
    Best1700Channel channels[4];
    MiBandDisplayBridge *display_bridge;
    MiBandPanelDevice *panel;
    MiBandLcdc lcdc;
    MiBandGpu gpu;
    MemoryRegion gpu_region;
    MemoryRegion lcdc_region;
    MemoryRegion ispi_region;
    Best1700Ispi ispi;
    uint32_t image_flash_base[BEST1700_IMAGE_COUNT];
    uint32_t image_code_offset[BEST1700_IMAGE_COUNT];
    uint32_t image_entry[BEST1700_IMAGE_COUNT];
    uint32_t shared_base;
    uint32_t doorbell_base;
    uint32_t display_frame;
    uint32_t lcdc_writes;
    uint32_t aon_timer;
    int charger_level;
    bool charger_charging;
};

/* Display frames come exclusively from the guest LCDC/panel path.  The
 * emulator deliberately does not synthesize a watch face or application UI.
 */
static void best1700_panel_present(void *opaque, const uint8_t *pixels,
                                   unsigned width, unsigned height)
{
    Best1700MachineState *s = opaque;
    s->display_frame++;
    miband_display_bridge_present(s->display_bridge, pixels, width, height);
}

static int best1700_panel_read(void *opaque, uint8_t command, uint8_t *data,
                               size_t capacity)
{
    Best1700MachineState *s = opaque;
    gsize length;
    const void *bytes;

    if (!s->panel_reads[command]) {
        return -1;
    }
    bytes = g_bytes_get_data(s->panel_reads[command], &length);
    length = MIN(length, capacity);
    memcpy(data, bytes, length);
    return length;
}

static bool best1700_panel_write(void *opaque, uint8_t command,
                                 const uint8_t *data, size_t length)
{
    Best1700MachineState *s = opaque;
    MiBandPanelEndpoint panel = miband_panel_endpoint(s->panel);
    MiBandPanelReply reply = panel.command(panel.opaque, command, false);

    /* The compatibility endpoint implements power/GRAM state. Vendor
     * configuration parameters are retained by neither endpoint nor SoC. */
    if (reply.reset_transfer) {
        miband_lcdc_async_reset(&s->lcdc);
    }
    return true;
}

static void best1700_panel_te(void *opaque)
{
    Best1700MachineState *s = opaque;
    if (s->panel_te_pin < 96) {
        best1700_gpio_set_input(&s->gpio, s->panel_te_pin, false);
        best1700_gpio_set_input(&s->gpio, s->panel_te_pin, true);
        best1700_gpio_set_input(&s->gpio, s->panel_te_pin, false);
    }
}

static void best1700_get_panel_te_pin(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    uint32_t pin = MIBAND_BEST1700(obj)->panel_te_pin;
    visit_type_uint32(v, name, &pin, errp);
}

static void best1700_set_panel_te_pin(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    uint32_t pin;
    if (s->workers_started) {
        error_setg(errp, "panel-te-pin is startup-only");
        return;
    }
    if (!visit_type_uint32(v, name, &pin, errp)) {
        return;
    }
    if (pin >= 96) {
        error_setg(errp, "panel-te-pin must be a GPIO in 0..95");
        return;
    }
    s->panel_te_pin = pin;
}

static char *best1700_get_panel_reads(Object *obj, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    return g_strdup(s->panel_read_responses ? s->panel_read_responses : "");
}

static void best1700_get_btc_frequency(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    uint32_t value = !strcmp(name, "btc-lpo-hz") ? s->btc_lpo_hz :
                                                s->btc_cal_ref_hz;
    visit_type_uint32(v, name, &value, errp);
}

static void best1700_set_btc_frequency(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    bool lpo = !strcmp(name, "btc-lpo-hz");
    uint32_t value;

    if (s->workers_started) {
        error_setg(errp, "%s is startup-only", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value || value > (lpo ? 1000000 : 1000000000)) {
        error_setg(errp, "%s is outside the supported frequency range", name);
        return;
    }
    if (lpo) {
        s->btc_lpo_hz = value;
    } else {
        s->btc_cal_ref_hz = value;
    }
}

static void best1700_get_btc_correction_ns(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    visit_type_uint32(v, name, &s->btc_correction_ns, errp);
}

static void best1700_set_btc_correction_ns(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    uint32_t value;

    if (s->workers_started) {
        error_setg(errp, "%s is startup-only", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value || value > 1000000000u) {
        error_setg(errp, "%s must be between 1 and 1000000000", name);
        return;
    }
    s->btc_correction_ns = value;
}

static void best1700_get_btc_cpudump_sample_ns(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    visit_type_uint32(v, name, &s->btc_cpudump_sample_ns, errp);
}

static void best1700_set_btc_cpudump_sample_ns(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    uint32_t value;

    if (s->workers_started) {
        error_setg(errp, "%s is startup-only", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value || value > 1000000000u) {
        error_setg(errp, "%s must be between 1 and 1000000000", name);
        return;
    }
    s->btc_cpudump_sample_ns = value;
}

static char *best1700_get_emmc_drive(Object *obj, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    return g_strdup(s->emmc_drive ? s->emmc_drive : "");
}

static bool best1700_get_secure_boot(Object *obj, Error **errp)
{
    return MIBAND_BEST1700(obj)->secure_boot;
}

static bool best1700_get_bth_real_boot(Object *obj, Error **errp)
{
    return MIBAND_BEST1700(obj)->bth_real_boot;
}

static void best1700_set_bth_real_boot(Object *obj, bool value, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);

    if (s->workers_started) {
        error_setg(errp, "bth-real-boot is startup-only");
        return;
    }
    s->bth_real_boot = value;
}

static bool best1700_get_cp_real_boot(Object *obj, Error **errp)
{
    return MIBAND_BEST1700(obj)->cp_real_boot;
}

static void best1700_set_cp_real_boot(Object *obj, bool value, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);

    if (s->workers_started) {
        error_setg(errp, "cp-real-boot is startup-only");
        return;
    }
    s->cp_real_boot = value;
}

static bool best1700_get_btc_real_boot(Object *obj, Error **errp)
{
    return MIBAND_BEST1700(obj)->btc_real_boot;
}

static void best1700_set_btc_real_boot(Object *obj, bool value, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);

    if (s->workers_started) {
        error_setg(errp, "btc-real-boot is startup-only");
        return;
    }
    s->btc_real_boot = value;
}

static void best1700_set_secure_boot(Object *obj, bool value, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    if (s->workers_started) {
        error_setg(errp, "secure-boot is startup-only");
        return;
    }
    s->secure_boot = value;
}

static void best1700_set_emmc_drive(Object *obj, const char *value, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    if (s->workers_started) {
        error_setg(errp, "emmc-drive is startup-only");
        return;
    }
    g_free(s->emmc_drive);
    s->emmc_drive = g_strdup(value);
}

static void best1700_set_panel_reads(Object *obj, const char *value, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    GBytes *reads[256] = {0};
    g_auto(GStrv) entries = g_strsplit(value, ";", -1);

    if (s->workers_started) {
        error_setg(errp, "panel-read-responses is startup-only");
        return;
    }
    for (unsigned i = 0; *value && entries[i]; i++) {
        const char *entry = entries[i];
        size_t length = strlen(entry);
        uint8_t bytes[122];
        int command;
        if (length < 5 || length > 3 + sizeof(bytes) * 2 ||
            entry[2] != ':' || (length - 3) % 2 ||
            !g_ascii_isxdigit(entry[0]) || !g_ascii_isxdigit(entry[1])) {
            goto invalid;
        }
        command = (g_ascii_xdigit_value(entry[0]) << 4) |
                   g_ascii_xdigit_value(entry[1]);
        if (reads[command]) {
            goto invalid;
        }
        for (size_t j = 3; j < length; j += 2) {
            if (!g_ascii_isxdigit(entry[j]) || !g_ascii_isxdigit(entry[j + 1])) {
                goto invalid;
            }
            bytes[(j - 3) / 2] = (g_ascii_xdigit_value(entry[j]) << 4) |
                                 g_ascii_xdigit_value(entry[j + 1]);
        }
        reads[command] = g_bytes_new(bytes, (length - 3) / 2);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(reads); i++) {
        g_clear_pointer(&s->panel_reads[i], g_bytes_unref);
        s->panel_reads[i] = reads[i];
    }
    g_free(s->panel_read_responses);
    s->panel_read_responses = g_strdup(value);
    return;

invalid:
    for (unsigned i = 0; i < G_N_ELEMENTS(reads); i++) {
        g_clear_pointer(&reads[i], g_bytes_unref);
    }
    error_setg(errp, "panel-read-responses requires unique CC:HEX entries "
                     "separated by semicolons (1..122 response bytes)");
}

static uint64_t best1700_lcdc_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1700MachineState *s = opaque;
    uint64_t result = 0;
    if (!miband_lcdc_read(&s->lcdc, BEST1700_LCDC_BASE + addr, size, &result)) {
        return 0;
    }
    return result;
}

static uint8_t *best1700_graphics_memory(void *opaque, uint32_t address,
                                        size_t length, bool write,
                                        MemTxAttrs attrs)
{
    Best1700MachineState *s = opaque;
    MemoryRegion *first_region = NULL;
    hwaddr first_offset = 0;
    size_t checked = 0;

    if (!length || length > UINT64_C(0x100000000) - address) {
        return NULL;
    }
    /* A TGU/MPC translation ends at a protection granule, not necessarily
     * the end of contiguous RAM. Validate every segment before exposing
     * one contiguous frame pointer; never bypass a gate or read MMIO.
     * Retain existing unspecified bus attributes: LCDC DMA security-control
     * attribution is not modeled yet and must not be inferred from a CPU.
     */
    while (checked < length) {
        hwaddr offset, available = length - checked;
        MemoryRegion *region = address_space_translate(&s->core[0].address_space,
            (hwaddr)address + checked, &offset, &available, write,
            attrs);

        if (!region || !memory_region_is_ram(region) || !available ||
            (write && region->readonly) ||
            offset > memory_region_size(region) ||
            available > memory_region_size(region) - offset) {
            return NULL;
        }
        if (!first_region) {
            first_region = region;
            first_offset = offset;
        } else if (region != first_region || offset != first_offset + checked) {
            return NULL;
        }
        checked += available;
    }
    return memory_region_get_ram_ptr(first_region) + first_offset;
}

static uint8_t *best1700_display_memory(void *opaque, uint32_t address,
                                       size_t length)
{
    return best1700_graphics_memory(opaque, address, length, false,
                                    MEMTXATTRS_UNSPECIFIED);
}

static uint8_t *best1700_gpu_memory(void *opaque, uint32_t address,
                                   size_t length, bool write)
{
    Best1700MachineState *s = opaque;

    return best1700_graphics_memory(s, address, length, write,
                                    best1700_sec_gpu_attrs(&s->sec));
}

static uint64_t best1700_gpu_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1700MachineState *s = opaque;
    uint64_t value = 0;

    if (!miband_gpu_read(&s->gpu, BEST1700_GPU_BASE + addr, size, &value)) {
        qemu_log_mask(LOG_UNIMP, "best1700 GPU read offset=0x%" HWADDR_PRIx
                      " size=%u\n", addr, size);
    }
    return value;
}

static void best1700_gpu_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Best1700MachineState *s = opaque;

    if (!miband_gpu_write(&s->gpu, BEST1700_GPU_BASE + addr, size, value)) {
        qemu_log_mask(LOG_UNIMP, "best1700 GPU write offset=0x%" HWADDR_PRIx
                      " size=%u value=0x%" PRIx64 "\n", addr, size, value);
    }
}

static const MemoryRegionOps best1700_gpu_ops = {
    .read = best1700_gpu_read,
    .write = best1700_gpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void best1700_lcdc_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    Best1700MachineState *s = opaque;
    s->lcdc_writes++;
    if (s->lcdc_writes <= 64) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 LCDC write offset=0x%03" HWADDR_PRIx
                      " size=%u value=0x%" PRIx64 "\n",
                      addr, size, value);
    }
    miband_lcdc_write(&s->lcdc, BEST1700_LCDC_BASE + addr, size, value);
}

static const MemoryRegionOps best1700_lcdc_ops = {
    .read = best1700_lcdc_read,
    .write = best1700_lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void best1700_get_charger_state(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    int64_t level = s->charger_level;
    bool charging = s->charger_charging;
    if (!visit_start_struct(v, name, NULL, 0, errp)) {
        return;
    }
    if (visit_type_int(v, "level", &level, errp) &&
        visit_type_bool(v, "charging", &charging, errp)) {
        visit_check_struct(v, errp);
    }
    visit_end_struct(v, NULL);
}

static void best1700_set_charger_state(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    int64_t level;
    bool charging;
    bool valid;
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
    s->charger_level = level;
    s->charger_charging = charging;
}

static uint64_t best1700_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1700MachineState *s = opaque;
    if (addr == 0x50 && size == sizeof(uint32_t)) {
        return ++s->aon_timer;
    }
    if (addr == 0x100 && size == sizeof(uint32_t)) {
        return 1;
    }
    /* AON status fields power up with clocks and reset deasserted. */
    return 1;
}

static void best1700_mmio_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    Best1700MachineState *s = opaque;
    unsigned channel = (addr / 4) & 3;
    if (addr < 0x1000) {
        s->channels[channel].tx_count++;
        /* A doorbell moves one descriptor from the guest-facing vring and
         * delivers the matching completion interrupt to the peer endpoint. */
        s->channels[channel].rx_count++;
        s->channels[channel].ready = true;
        s->channels[channel].shutdown = false;
    }
}

static const MemoryRegionOps best1700_mmio_ops = {
    .read = best1700_mmio_read,
    .write = best1700_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t best1700_fast_ticks(Best1700Core *core)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                      core->fast_epoch_ns;
    return core->fast_ticks + muldiv64(MAX(elapsed, 0),
                                      BEST1700_FAST_TIMER_HZ,
                                      NANOSECONDS_PER_SECOND);
}

static uint64_t best1700_sysctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1700Core *core = opaque;
    uint32_t value = core->sysctrl_regs[addr >> 2];
    if ((addr & ~3u) == 0x90 || (addr & ~3u) == 0x94) {
        if (core->hifi4) {
            value = (value & ~BEST1700_HIFI4_IRQ_MASK) |
                    best1700_hifi4_pending(core->hifi4);
        }
        if (core->m55_ipc) {
            value = (value & ~BEST1700_M55_IPC_IRQ_MASK) |
                    best1700_m55_ipc_pending(core->m55_ipc);
        }
    }
    if (addr >= 0xc0 && addr < 0xe0 && !(addr & 3) && size == 4) {
        unsigned index = (addr - 0xc0) / 4;
        return best1700_memsc_claim(core->sys_memsc, index);
    }
    if (addr == 0xe0 && size == 4) {
        return best1700_memsc_status(core->sys_memsc);
    }
    /* Both halves of the free-running counter use the virtual timebase,
     * including while the CPU sleeps between timer interrupts. */
    if ((addr & ~3u) == 0x288) {
        value = best1700_fast_ticks(core);
    } else if ((addr & ~3u) == 0x28c) {
        value = best1700_fast_ticks(core) >> 32;
    } else if ((addr & ~3u) == 0x1ac) {
        /* Display, GPU, DSP0 and PSRAM bus transactions are synchronous in
         * this model, so quiescence can be acknowledged immediately.
         * PSRAM1's request and acknowledgement use different bit positions. */
        value = core->sysctrl_regs[0x1a0 / 4] &
                ((1u << 3) | (1u << 5) | (1u << 7) |
                 (1u << 11) | (1u << 25));
        value |= (core->sysctrl_regs[0x1a0 / 4] & (1u << 26)) << 1;
    }
    if (core->sysctrl_reads++ < 128) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 %s SYSCTRL read offset=0x%04" HWADDR_PRIx
                      " value=0x%08x pc=0x%08x\n", core->name, addr, value,
                      core->cpu.cpu->env.regs[15]);
    }
    if (size == 1) {
        return (value >> ((addr & 3) * 8)) & 0xff;
    }
    if (size == 2) {
        return (value >> ((addr & 2) * 8)) & 0xffff;
    }
    return value;
}

static void best1700_sysctrl_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size)
{
    Best1700Core *core = opaque;
    uint32_t *reg = &core->sysctrl_regs[addr >> 2];
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
    if ((core->hifi4 || core->m55_ipc) &&
        ((addr & ~3u) == 0x90 || (addr & ~3u) == 0x94)) {
        uint32_t bits = ((uint32_t)value & mask) << shift;
        uint32_t owned = 0;

        if (core->hifi4) {
            owned |= BEST1700_HIFI4_IRQ_MASK;
            if ((addr & ~3u) == 0x90) {
                best1700_hifi4_set(core->hifi4, bits);
            } else {
                best1700_hifi4_clear(core->hifi4, bits);
            }
        }
        if (core->m55_ipc) {
            owned |= BEST1700_M55_IPC_IRQ_MASK;
            if ((addr & ~3u) == 0x90) {
                best1700_m55_ipc_set(core->m55_ipc, bits);
            } else {
                best1700_m55_ipc_clear(core->m55_ipc, bits);
            }
        }
        /* Preserve unrelated peer state without treating command bits as
         * storage. Each controller owns a disjoint set of latches. */
        if ((addr & ~3u) == 0x90) {
            core->sysctrl_regs[0x90 / 4] |= bits & ~owned;
        } else {
            core->sysctrl_regs[0x90 / 4] &= ~(bits & ~owned);
        }
        core->sysctrl_regs[0x94 / 4] = core->sysctrl_regs[0x90 / 4];
        return;
    }
    if (best1700_cmu_clock_write(core->sysctrl_regs,
                                G_N_ELEMENTS(core->sysctrl_regs),
                                BEST1700_CMU_SYS, addr, value, size)) {
        return;
    }
    if ((addr & ~3u) == 0x1ac) {
        return; /* FORCE_ACK is read-only status. */
    }
    if (addr >= 0xc0 && addr < 0xe0 && !(addr & 3) && size == 4) {
        best1700_memsc_release(core->sys_memsc, (addr - 0xc0) / 4,
                               value);
        return;
    }
    if (addr == 0xe0 && !(addr & 3) && size == 4) {
        /* SYS MEMSC_STATUS is read-only. */
        return;
    }
    *reg = (*reg & ~(mask << shift)) | ((uint32_t)value << shift);
    if ((addr & ~3u) == 0x5c) {
        unsigned divider = (*reg >> 21) & 3;
        uint32_t frequency = (*reg & (1u << 17)) ?
                             BEST1700_FAST_TIMER_HZ >> MIN(divider, 2u) :
                             BEST1700_SLOW_TIMER_HZ;
        best1700_aon_set_frequency(&core->sys_timer0, frequency);
    }
    if ((addr & ~3u) == 0x290 || (addr & ~3u) == 0x294) {
        uint64_t ticks = best1700_fast_ticks(core);
        core->fast_ticks = (addr & ~3u) == 0x290 ?
                          (ticks & 0xffffffff00000000ULL) | *reg :
                          ((uint64_t)*reg << 32) | (uint32_t)ticks;
        core->fast_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (core->sysctrl_writes++ < 128) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 %s SYSCTRL write offset=0x%04" HWADDR_PRIx
                      " value=0x%08" PRIx64 "\n", core->name, addr, value);
    }
}

static const MemoryRegionOps best1700_sysctrl_ops = {
    .read = best1700_sysctrl_read,
    .write = best1700_sysctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static bool best1700_dma_decode(hwaddr addr, unsigned *dma, hwaddr *offset)
{
    if (addr >= BEST1700_DMA0_OFFSET &&
        addr < BEST1700_DMA0_OFFSET + BEST1700_DMA_SIZE) {
        *dma = 0;
        *offset = addr - BEST1700_DMA0_OFFSET;
        return true;
    }
    if (addr >= BEST1700_DMA1_OFFSET &&
        addr < BEST1700_DMA1_OFFSET + BEST1700_DMA_SIZE) {
        *dma = 1;
        *offset = addr - BEST1700_DMA1_OFFSET;
        return true;
    }
    return false;
}

static uint32_t best1700_dma_merge(uint32_t old, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
    return (old & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
}

static void best1700_dma_schedule(Best1700Core *core)
{
    if (!timer_pending(core->dma_timer)) {
        timer_mod(core->dma_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  BEST1700_DMA_TICK_NS);
    }
}

static void best1700_dma_refresh_irq(Best1700Core *core, unsigned dma)
{
    uint32_t *regs = core->dma_regs[dma];
    uint32_t tc = 0, err = 0;
    for (unsigned channel = 0; channel < BEST1700_DMA_CHANNELS; channel++) {
        uint32_t cfg = regs[(BEST1700_DMA_CH_BASE +
                             channel * BEST1700_DMA_CH_STRIDE +
                             BEST1700_DMA_CH_CFG) >> 2];
        if (cfg & (1u << 15)) {
            tc |= regs[BEST1700_DMA_RAW_TC_STATUS >> 2] & (1u << channel);
        }
        if (cfg & (1u << 14)) {
            err |= regs[BEST1700_DMA_RAW_ERR_STATUS >> 2] & (1u << channel);
        }
    }
    regs[BEST1700_DMA_INT_TC_STATUS >> 2] = tc;
    regs[BEST1700_DMA_INT_ERR_STATUS >> 2] = err;
    regs[BEST1700_DMA_INT_STATUS >> 2] = tc | err;
    if (dma >= G_N_ELEMENTS(core->dma_irq) || !core->dma_irq[dma]) {
        return;
    }
    if (core->dma_regs[dma][BEST1700_DMA_INT_STATUS >> 2]) {
        qemu_irq_raise(core->dma_irq[dma]);
    } else {
        qemu_irq_lower(core->dma_irq[dma]);
    }
}

static void best1700_uart_dma_notify(void *opaque)
{
    Best1700Core *core = opaque;
    if (core->dma_pending[0] || core->dma_pending[1]) {
        best1700_dma_schedule(core);
    }
}

static Best1700Uart *best1700_dma_rx_source(Best1700Core *core, unsigned dma,
                                          unsigned channel)
{
    uint32_t *regs = core->dma_regs[dma];
    hwaddr base = BEST1700_DMA_CH_BASE + channel * BEST1700_DMA_CH_STRIDE;
    uint32_t cfg = regs[(base + BEST1700_DMA_CH_CFG) >> 2];
    unsigned request = (cfg >> 1) & 31;
    uint32_t source = regs[(base + BEST1700_DMA_CH_SRC) >> 2];

    /* System DMA0 request routing. Both the selector and fixed FIFO address
     * must match; unrelated endpoints must never consume UART input. */
    if (dma != 0 || core->real_bth || ((cfg >> 11) & 7) != 2) {
        return NULL;
    }
    if (!core->real_cp && request == 4 && source == BEST1700_UART0_BASE) {
        return &core->uart0;
    }
    if (request == 6 && source == BEST1700_UART1_BASE) {
        return &core->uart1;
    }
    if (core->real_cp && request == 8 && source == BEST1700_UART2_BASE) {
        return &core->uart2;
    }
    return NULL;
}

static bool best1700_dma_transfer(Best1700Core *core, unsigned dma,
                                  unsigned channel)
{
    uint32_t *regs = core->dma_regs[dma];
    hwaddr base = BEST1700_DMA_CH_BASE +
                  channel * BEST1700_DMA_CH_STRIDE;
    hwaddr src = regs[(base + BEST1700_DMA_CH_SRC) >> 2];
    hwaddr dst = regs[(base + BEST1700_DMA_CH_DST) >> 2];
    uint32_t control = regs[(base + BEST1700_DMA_CH_CONTROL) >> 2];
    unsigned count = control & 0xfffu;
    unsigned src_width = 1u << ((control >> 18) & 7u);
    unsigned dst_width = 1u << ((control >> 21) & 7u);
    bool src_inc = (control & (1u << 26)) != 0;
    bool dst_inc = (control & (1u << 27)) != 0;
    Best1700Uart *rx_uart = best1700_dma_rx_source(core, dma, channel);
    MemTxAttrs attrs = core->dma_sec ?
        best1700_sec_hdma_attrs(core->dma_sec, dma) : MEMTXATTRS_UNSPECIFIED;

    /* Different bus widths require FIFO packing/unpacking.  Until that is
     * modelled, reject them explicitly instead of losing or padding bytes.
     * The supported widths are byte, halfword and word on both buses. */
    if (src_width > 4 || dst_width > 4 || src_width != dst_width ||
        (rx_uart && src_inc) ||
        (src & (src_width - 1)) || (dst & (dst_width - 1))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 %s DMA%u ch%u unsupported transfer "
                      "src=0x%08" HWADDR_PRIx " dst=0x%08" HWADDR_PRIx
                      " widths=%u/%u\n",
                      core->name, dma, channel, src, dst,
                      src_width, dst_width);
        return false;
    }

    for (unsigned i = 0; i < count; i++) {
        uint8_t payload[4] = { 0 };
        MemTxResult result;

        if (rx_uart && !best1700_uart_dma_rx_pending(rx_uart)) {
            /* Retain the live residual count until another physical byte
             * raises a request. An empty FIFO is not a zero-valued byte. */
            return true;
        }

        result = address_space_read(&core->address_space, src,
                                    attrs,
                                    payload, src_width);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "best1700 %s DMA%u ch%u read failed "
                          "src=0x%08" HWADDR_PRIx " bytes=%u\n",
                          core->name, dma, channel, src, src_width);
            return false;
        }
        result = address_space_write(&core->address_space, dst,
                                     attrs,
                                     payload, dst_width);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "best1700 %s DMA%u ch%u write failed "
                          "dst=0x%08" HWADDR_PRIx " bytes=%u\n",
                          core->name, dma, channel, dst, dst_width);
            return false;
        }
        if (src_inc) {
            src += src_width;
        }
        if (dst_inc) {
            dst += dst_width;
        }
        /* Preserve progress if a later beat encounters a bus error.  The
         * ISR uses these live addresses and residual count in its callback. */
        regs[(base + BEST1700_DMA_CH_SRC) >> 2] = src;
        regs[(base + BEST1700_DMA_CH_DST) >> 2] = dst;
        regs[(base + BEST1700_DMA_CH_CONTROL) >> 2] =
            (control & ~0xfffu) | (count - i - 1);
    }

    return true;
}

static bool best1700_dma_load_lli(Best1700Core *core, unsigned dma,
                                 unsigned channel, uint32_t lli)
{
    uint8_t descriptor[BEST1700_DMA_DESCRIPTOR_BYTES];
    uint32_t *regs = core->dma_regs[dma];
    hwaddr base = BEST1700_DMA_CH_BASE +
                  channel * BEST1700_DMA_CH_STRIDE;
    /* Bit zero selects an AHB master, not an address bit.  Both masters use
     * the same guest address space; the descriptor itself is word aligned. */
    hwaddr address = lli & ~1u;
    MemTxAttrs attrs = core->dma_sec ?
        best1700_sec_hdma_attrs(core->dma_sec, dma) : MEMTXATTRS_UNSPECIFIED;

    if ((address & 3) ||
        address > UINT32_MAX - (BEST1700_DMA_DESCRIPTOR_BYTES - 1) ||
        address_space_read(&core->address_space, address,
                           attrs, descriptor,
                           sizeof(descriptor)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 %s DMA%u ch%u invalid LLI=0x%08x\n",
                      core->name, dma, channel, lli);
        return false;
    }
    regs[(base + BEST1700_DMA_CH_SRC) >> 2] = ldl_le_p(descriptor);
    regs[(base + BEST1700_DMA_CH_DST) >> 2] = ldl_le_p(descriptor + 4);
    regs[(base + BEST1700_DMA_CH_LLI) >> 2] = ldl_le_p(descriptor + 8);
    regs[(base + BEST1700_DMA_CH_CONTROL) >> 2] = ldl_le_p(descriptor + 12);
    return true;
}

static uint64_t best1700_dma_read(Best1700Core *core, hwaddr addr,
                                  unsigned size)
{
    unsigned dma;
    hwaddr offset;
    uint32_t value;

    if (!best1700_dma_decode(addr, &dma, &offset) ||
        offset >= BEST1700_DMA_REG_BYTES) {
        return 0;
    }
    value = core->dma_regs[dma][offset >> 2];
    if (core->dma_reads++ < 1024) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 %s DMA%u read offset=0x%06" HWADDR_PRIx
                      " size=%u value=0x%08x pc=0x%08x\n",
                      core->name, dma, offset, size, value,
                      core->cpu.cpu->env.regs[15]);
    }
    if (size == 1) {
        return (value >> ((addr & 3) * 8)) & 0xff;
    }
    if (size == 2) {
        return (value >> ((addr & 2) * 8)) & 0xffff;
    }
    return value;
}

static void best1700_dma_complete(Best1700Core *core, unsigned dma,
                                  unsigned channel)
{
    uint32_t bit = 1u << channel;
    uint32_t *regs = core->dma_regs[dma];
    hwaddr ch_cfg = BEST1700_DMA_CH_BASE +
                    channel * BEST1700_DMA_CH_STRIDE + BEST1700_DMA_CH_CFG;
    hwaddr ctrl = ch_cfg - BEST1700_DMA_CH_CFG + BEST1700_DMA_CH_CONTROL;
    hwaddr lli_reg = ch_cfg - BEST1700_DMA_CH_CFG + BEST1700_DMA_CH_LLI;
    bool success = best1700_dma_transfer(core, dma, channel);

    if (success && (regs[ctrl >> 2] & 0xfff)) {
        core->dma_pending[dma] |= bit;
        best1700_dma_refresh_irq(core, dma);
        return;
    }

    /* Terminal count belongs to the completed descriptor, even when a
     * following descriptor keeps the channel enabled.  Raw status remains
     * latched until the guest acknowledges it through the W1C register. */
    if (success) {
        uint32_t lli = regs[lli_reg >> 2];

        if (regs[ctrl >> 2] & (1u << 31)) {
            regs[BEST1700_DMA_RAW_TC_STATUS >> 2] |= bit;
        }
        if (lli & ~1u) {
            success = best1700_dma_load_lli(core, dma, channel, lli);
            if (success) {
                /* Execute the loaded descriptor on the next timer tick.
                 * Circular chains must never spin inside one callback. */
                core->dma_pending[dma] |= bit;
                best1700_dma_refresh_irq(core, dma);
                return;
            }
        } else {
            regs[lli_reg >> 2] = 0;
        }
    }
    if (!success) {
        regs[BEST1700_DMA_RAW_ERR_STATUS >> 2] |= bit;
    }
    core->dma_pending[dma] &= ~bit;
    regs[BEST1700_DMA_ENABLED_CHANNELS >> 2] &= ~bit;
    regs[ch_cfg >> 2] &= ~1u;
    best1700_dma_refresh_irq(core, dma);
}

static void best1700_dma_tick(void *opaque)
{
    Best1700Core *core = opaque;
    bool reschedule = false;

    for (unsigned dma = 0; dma < 2; dma++) {
        for (unsigned channel = 0; channel < BEST1700_DMA_CHANNELS; channel++) {
            uint32_t bit = 1u << channel;
            uint32_t cfg = core->dma_regs[dma][(BEST1700_DMA_CH_BASE +
                           channel * BEST1700_DMA_CH_STRIDE +
                           BEST1700_DMA_CH_CFG) >> 2];
            unsigned flow = (cfg >> 11) & 7;
            if (!(core->dma_pending[dma] & bit)) {
                continue;
            }
            if (!(cfg & 1)) {
                core->dma_pending[dma] &= ~bit;
                continue;
            }
            if (!(core->dma_regs[dma][BEST1700_DMA_CONFIG >> 2] & 1) ||
                (cfg & (1u << 18))) {
                continue;
            }
            /* Only wired receive endpoints can supply physical requests. */
            if (flow == 2 || flow == 3 || flow == 4 || flow == 6 || flow == 7) {
                Best1700Uart *uart = best1700_dma_rx_source(core, dma, channel);
                if (!uart || !best1700_uart_dma_rx_pending(uart)) {
                    continue;
                }
            }
            core->dma_pending[dma] &= ~bit;
            best1700_dma_complete(core, dma, channel);
            if (core->dma_pending[dma] & bit) {
                Best1700Uart *uart = best1700_dma_rx_source(core, dma, channel);
                reschedule |= !uart || best1700_uart_dma_rx_pending(uart);
            }
        }
    }
    if (reschedule) {
        best1700_dma_schedule(core);
    }
}

static void best1700_dma_write(Best1700Core *core, hwaddr addr,
                               uint64_t value, unsigned size)
{
    unsigned dma;
    hwaddr offset;
    uint32_t *regs;

    if (!best1700_dma_decode(addr, &dma, &offset) ||
        offset >= BEST1700_DMA_REG_BYTES) {
        return;
    }
    regs = core->dma_regs[dma];
    if (core->dma_writes++ < 1024) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 %s DMA%u write offset=0x%06" HWADDR_PRIx
                      " size=%u value=0x%08" PRIx64
                      " pc=0x%08x\n",
                      core->name, dma, offset, size, value,
                      core->cpu.cpu->env.regs[15]);
    }
    if (offset == BEST1700_DMA_INT_TC_CLEAR) {
        regs[BEST1700_DMA_RAW_TC_STATUS >> 2] &= ~((uint32_t)value);
        best1700_dma_refresh_irq(core, dma);
        return;
    }
    if (offset == BEST1700_DMA_INT_ERR_CLEAR) {
        regs[BEST1700_DMA_RAW_ERR_STATUS >> 2] &= ~((uint32_t)value);
        best1700_dma_refresh_irq(core, dma);
        return;
    }

    if (offset == BEST1700_DMA_ENABLED_CHANNELS ||
        offset == BEST1700_DMA_INT_STATUS ||
        offset == BEST1700_DMA_INT_TC_STATUS ||
        offset == BEST1700_DMA_INT_ERR_STATUS ||
        offset == BEST1700_DMA_RAW_TC_STATUS ||
        offset == BEST1700_DMA_RAW_ERR_STATUS) {
        /* These are controller status registers and are not writable. */
        return;
    }

    regs[offset >> 2] = best1700_dma_merge(regs[offset >> 2], addr,
                                            value, size);
    if (offset == BEST1700_DMA_CONFIG && (regs[offset >> 2] & 1)) {
        best1700_dma_schedule(core);
    }

    if (offset >= BEST1700_DMA_CH_BASE &&
        offset < BEST1700_DMA_CH_BASE +
                 BEST1700_DMA_CHANNELS * BEST1700_DMA_CH_STRIDE) {
        hwaddr rel = offset - BEST1700_DMA_CH_BASE;
        unsigned channel = rel / BEST1700_DMA_CH_STRIDE;
        unsigned reg = rel % BEST1700_DMA_CH_STRIDE;
        if (channel < BEST1700_DMA_CHANNELS &&
            (reg & ~3u) == BEST1700_DMA_CH_CFG &&
            (regs[offset >> 2] & 1)) {
            regs[BEST1700_DMA_ENABLED_CHANNELS >> 2] |= 1u << channel;
            core->dma_pending[dma] |= 1u << channel;
            best1700_dma_schedule(core);
        } else if (channel < BEST1700_DMA_CHANNELS &&
                   (reg & ~3u) == BEST1700_DMA_CH_CFG) {
            regs[BEST1700_DMA_ENABLED_CHANNELS >> 2] &= ~(1u << channel);
            core->dma_pending[dma] &= ~(1u << channel);
        }
        best1700_dma_refresh_irq(core, dma);
    }
}

static uint64_t best1700_sys_periph_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    Best1700Core *core = opaque;
    unsigned dma;
    hwaddr dma_offset;
    if (best1700_dma_decode(addr, &dma, &dma_offset)) {
        return best1700_dma_read(core, addr, size);
    }
    if (core->sys_periph_reads++ < 512) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 %s SYS_PERIPH read offset=0x%06" HWADDR_PRIx
                      " size=%u pc=0x%08x\n", core->name, addr, size,
                      core->cpu.cpu->env.regs[15]);
    }
    return 0;
}

static void best1700_sys_periph_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    Best1700Core *core = opaque;
    unsigned dma;
    hwaddr dma_offset;
    if (best1700_dma_decode(addr, &dma, &dma_offset)) {
        best1700_dma_write(core, addr, value, size);
        return;
    }
    if (core->sys_periph_writes++ < 512) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 %s SYS_PERIPH write offset=0x%06" HWADDR_PRIx
                      " size=%u value=0x%08" PRIx64 " pc=0x%08x\n",
                      core->name, addr, size, value,
                      core->cpu.cpu->env.regs[15]);
    }
}

static const MemoryRegionOps best1700_sys_periph_ops = {
    .read = best1700_sys_periph_read,
    .write = best1700_sys_periph_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t best1700_ispi_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Best1700MachineState *s = opaque;
    uint64_t result = 0;
    best1700_ispi_read(&s->ispi, addr, size, &result);
    return result;
}

static void best1700_ispi_mmio_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size)
{
    Best1700MachineState *s = opaque;
    best1700_ispi_write(&s->ispi, addr, size, value);
}

static const MemoryRegionOps best1700_ispi_ops = {
    .read = best1700_ispi_mmio_read,
    .write = best1700_ispi_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void best1700_write_vectors(Best1700Core *core)
{
    uint32_t *vectors = memory_region_get_ram_ptr(&core->vectors);
    vectors[0] = core->boot_sp;
    vectors[1] = core->boot_pc;
    /* Boot configuration word consumed by the early clock-mode query. */
    *(uint16_t *)((uint8_t *)vectors + 0x2c) = 1;
}

static void best1700_stub_code(Best1700Core *core)
{
    uint8_t *ram = memory_region_get_ram_ptr(&core->ram);
    /* WFI; B . (Thumb), enough to keep a synthetic endpoint alive. */
    ram[0x100] = 0x30;
    ram[0x101] = 0xbf;
    ram[0x102] = 0xfe;
    ram[0x103] = 0xe7;
}

static void best1700_nonsecure_handoff(Best1700Core *core)
{
    if (core->secure_boot || core->real_bth || core->real_cp || core->real_btc) {
        /* Real reset starts in Secure state. Only the legacy application
         * entry bypasses a boot stage below. Peer firmware initializes its
         * own exception state; it has no AP TEE-to-NS handoff. */
        return;
    }

    /* The compatibility handoff starts application images in the nonsecure
     * view. Secure boot instead lets the TEE initialize and enter the AP. */
    if (arm_feature(&core->cpu.cpu->env, ARM_FEATURE_M_SECURITY)) {
        /* The application images use the non-secure memory view.  The board
         * does not expose a secure-only aperture to these cores, so model one
         * enabled SAU region covering the guest address space. */
        core->cpu.cpu->env.sau.rnr = 0;
        core->cpu.cpu->env.sau.rbar[0] = 0;
        core->cpu.cpu->env.sau.rlar[0] = 0xfffff000u | 1u;
        core->cpu.cpu->env.sau.ctrl = 1;
        /* Firmware uses the M55 floating-point context from the application
         * world; grant CP10/CP11 access in both banks for deterministic boot. */
        core->cpu.cpu->env.v7m.cpacr[M_REG_NS] = 0x00f00000u;
        core->cpu.cpu->env.v7m.cpacr[M_REG_S] = 0x00f00000u;
        core->cpu.cpu->env.v7m.nsacr = 0x00000c00u;
        /* This direct application handoff also assigns external interrupts
         * to the non-secure world.  Otherwise its NVIC enable writes are
         * ignored and a pending peripheral IRQ cannot wake WFI. */
        for (unsigned irq = NVIC_INTERNAL_VECTORS;
             irq < core->cpu.nvic.num_irq; irq++) {
            core->cpu.nvic.itns[irq] = true;
        }
        core->cpu.cpu->env.v7m.secure = false;
        arm_rebuild_hflags(&core->cpu.cpu->env);
    }
}

static void best1700_core_reset(void *opaque)
{
    Best1700Core *core = opaque;

    if (core->real_bth || core->real_cp || core->real_btc) {
        /* This callback runs during machine reset, when CPUs are stopped.
         * Guest reset commands take the asynchronous path below. */
        core->peer_reset_generation++;
        core->peer_running = false;
        core->peer_reset_pending = false;
        core->peer_held = true;
        core->peer_needs_reset = true;
        core->peer_waiting = false;
        core->cpu.cpu->power_state = PSCI_OFF;
        CPU(core->cpu.cpu)->halted = 1;
        return;
    }
    best1700_nonsecure_handoff(core);
}

typedef struct Best1700PeerResetWork {
    Best1700Core *core;
    uint64_t generation;
    uint32_t vtor;
    bool asserted;
    bool reset_cpu;
} Best1700PeerResetWork;

static void best1700_peer_reset_work(CPUState *cpu, run_on_cpu_data data)
{
    g_autofree Best1700PeerResetWork *work = data.host_ptr;
    Best1700Core *core = work->core;

    if (work->generation != core->peer_reset_generation) {
        return;
    }
    /* Execute reset in the target CPU context, as ARM's power-control code
     * does. In particular, never reset a running peer from an AP MMIO call. */
    if (work->reset_cpu) {
        core->cpu.cpu->init_svtor = work->vtor & ~127u;
        core->cpu.cpu->init_nsvtor = work->vtor & ~127u;
        cpu_reset(cpu);
        device_cold_reset(DEVICE(&core->cpu.nvic));
        best1700_nonsecure_handoff(core);
        core->peer_needs_reset = false;
        if (core->peer_reset_done) {
            core->peer_reset_done(core->peer_reset_opaque);
        }
    }
    if (work->asserted) {
        core->cpu.cpu->power_state = PSCI_OFF;
        cpu->halted = 1;
        cpu->exception_index = EXCP_HLT;
    } else {
        core->cpu.cpu->power_state = PSCI_ON;
        cpu->halted = 0;
        if (cpu->exception_index == EXCP_HLT) {
            cpu->exception_index = -1;
        }
    }
    core->peer_running = !work->asserted;
    core->peer_reset_pending = false;
}

static void best1700_peer_schedule(Best1700Core *core, bool asserted,
                                    bool reset_cpu, uint32_t vtor)
{
    Best1700PeerResetWork *work = g_new0(Best1700PeerResetWork, 1);

    core->peer_held = asserted;
    core->peer_reset_pending = true;
    work->core = core;
    work->generation = ++core->peer_reset_generation;
    work->vtor = vtor;
    work->asserted = asserted;
    work->reset_cpu = reset_cpu;
    async_run_on_cpu(CPU(core->cpu.cpu), best1700_peer_reset_work,
                      RUN_ON_CPU_HOST_PTR(work));
}

static void best1700_bth_update_control(Best1700MachineState *s)
{
    Best1700Core *core = &s->core[2];
    bool reset = s->bth_cmu_reset ||
                 best1700_bth_power_is_off(&s->bth_power) ||
                 best1700_bth_power_is_reset_asserted(&s->bth_power);
    bool held = reset || best1700_bth_power_is_clock_stopped(&s->bth_power);

    /* Keep the need for a reset latched until target-CPU work completes.
     * A retained wake may overtake the queued power-down work under MTTCG. */
    if (reset) {
        core->peer_needs_reset = true;
    }
    if (core->ready && (held != core->peer_held ||
                       (!held && core->peer_needs_reset))) {
        best1700_peer_schedule(core, held,
            !held && core->peer_needs_reset, s->bth.vtor[0]);
    }
}

static void best1700_bth_set_reset(void *opaque, bool asserted, uint32_t vtor)
{
    Best1700MachineState *s = opaque;

    /* The callback also covers CMU reset pulses without changing its latch. */
    s->bth_cmu_reset = asserted;
    best1700_bth_update_control(s);
}

static void best1700_bth_lpu_completed(void *opaque, bool awake)
{
    Best1700MachineState *s = opaque;

    /* This is the automatic sequencer's completed oscillator wake, not
     * cold/manual power availability. No PLL lock is fabricated. */
    best1700_bth_timing_set_lpu_clock_status(&s->bth_timing, awake, false);
}

static void best1700_bth_power_changed(void *opaque, bool off,
                                      bool reset, bool clock_stopped)
{
    Best1700MachineState *s = opaque;

    best1700_bth_update_control(s);
}

static void best1700_bth_refresh_wake(Best1700MachineState *s)
{
    /* Timer20/21 are the two channels of the BTH always-on timer2.
     * Other established peripheral routes pass through the CMU wake masks. */
    uint32_t sources = (s->bth_wake_aggregate ? (1u << 16) : 0) |
                       (s->bth_irq_level[19] ? (1u << 5) : 0) |
                       (s->bth_irq_level[20] ? (1u << 6) : 0);

    best1700_bth_power_set_wake(&s->bth_power, sources);
}

static void best1700_bth_wake_changed(void *opaque, bool level)
{
    Best1700MachineState *s = opaque;

    s->bth_wake_aggregate = level;
    best1700_bth_refresh_wake(s);
}

static void best1700_bth_irq_input(void *opaque, int irq, int level)
{
    Best1700MachineState *s = opaque;

    g_assert(irq >= 0 && irq < G_N_ELEMENTS(s->bth_irq_level));
    s->bth_irq_level[irq] = level != 0;
    best1700_bth_timing_set_irq_level(&s->bth_timing, irq, level != 0);
    best1700_bth_refresh_wake(s);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->core[2].cpu), irq), level);
}

static void best1700_bth_cpu_reset_done(void *opaque)
{
    Best1700MachineState *s = opaque;

    /* NVIC reset loses its input levels, not the peripheral requests in the
     * retained domain. Re-drive all connected sources without ACK side effects. */
    for (unsigned irq = 0; irq < G_N_ELEMENTS(s->bth_irq_level); irq++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->core[2].cpu), irq),
                     s->bth_irq_level[irq]);
    }
}

static void best1700_cp_set_control(void *opaque, bool reset_asserted,
                                     bool waiting, uint32_t vtor)
{
    Best1700Core *core = opaque;
    bool held = reset_asserted || waiting;

    if (reset_asserted) {
        core->peer_needs_reset = true;
    }
    core->peer_waiting = waiting;
    /* CORE_WAIT can hold a live processor without destroying its context.
     * Initial reset vectors are fetched only when both controls release. */
    best1700_peer_schedule(core, held, !held && core->peer_needs_reset, vtor);
}

static void best1700_load_image(Best1700Core *core, const char *path)
{
    gsize size;
    g_autofree gchar *data = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t *flash;

    if (!path || !*path) {
        return;
    }
    if (!g_file_get_contents(path, &data, &size, &error)) {
        error_setg(&error_fatal, "cannot load BEST1700 %s image %s: %s",
                   core->name, path, error->message);
        return;
    }
    if (size > BEST1700_FLASH_SIZE) {
        error_setg(&error_fatal, "BEST1700 %s image is larger than its flash window",
                   core->name);
        return;
    }
    flash = memory_region_get_ram_ptr(&core->flash);
    memcpy(flash, data, size);
    core->image_loaded = true;
}

static void best1700_seed_boot_rom(Best1700Core *core)
{
    uint8_t *flash = memory_region_get_ram_ptr(&core->flash);
    uint32_t start = core->code_offset > core->boot_rom_size ?
                     core->code_offset - core->boot_rom_size : 0;

    /* The application calls a small immutable boot ROM window immediately
     * before its flash slot.  Return stubs keep those calls deterministic
     * until the individual ROM services are modelled. */
    for (uint32_t off = start; off + 1 < core->code_offset; off += 2) {
        flash[off] = 0x70;
        flash[off + 1] = 0x47;
    }
}

static void best1700_load_secure_image(Best1700MachineState *s,
                                       Best1700Core *core)
{
    g_autofree gchar *data = NULL;
    g_autoptr(GError) error = NULL;
    gsize size;
    uint64_t address = (uint64_t)s->image_flash_base[3] + s->image_code_offset[3];
    uint64_t offset;

    if (!s->tee_image || !*s->tee_image ||
        !g_file_get_contents(s->tee_image, &data, &size, &error)) {
        error_report("secure-boot requires a readable tee-image: %s",
                      error ? error->message : "no image supplied");
        exit(1);
    }
    if (address < core->flash_base) {
        error_report("TEE image starts before the AP flash window");
        exit(1);
    }
    offset = address - core->flash_base;
    if (offset >= core->code_offset || size > core->code_offset - offset ||
        core->boot_pc < address || (uint64_t)core->boot_pc >= address + size) {
        error_report("TEE image or secure boot entry lies outside its flash slot");
        exit(1);
    }
    memcpy((uint8_t *)memory_region_get_ram_ptr(&core->flash) + offset, data, size);
}

static MemoryRegion *best1700_mpc_map(Best1700Core *core,
                                      Best1700MpcState *mpc,
                                      MemoryRegion *storage, hwaddr registers)
{
    SysBusDevice *device = SYS_BUS_DEVICE(mpc);

    object_property_set_link(OBJECT(mpc), "downstream", OBJECT(storage),
                              &error_fatal);
    sysbus_realize(device, &error_fatal);
    memory_region_add_subregion_overlap(&core->space, registers,
        sysbus_mmio_get_region(device, 0), 2);
    return sysbus_mmio_get_region(device, 1);
}

static void best1700_map_memory(Best1700MachineState *s, Best1700Core *core)
{
    MemoryRegion *flash_view = &core->flash;
    MemoryRegion *psram_view = &core->psram;
    char name[64];

    /* Realize after the M55 chooses the target page size, before loading any
     * image. MPC geometry follows that page size; guest code reads BLK_CFG.
     * The legacy direct NS boot path retains its existing unfiltered map. */
    if (core->secure_boot) {
        object_initialize_child(OBJECT(s), "flash-mpc", &s->flash_mpc,
                                  TYPE_BEST1700_MPC);
        object_initialize_child(OBJECT(s), "psram-mpc", &s->psram_mpc,
                                  TYPE_BEST1700_MPC);
        flash_view = best1700_mpc_map(core, &s->flash_mpc, &core->flash,
                                       BEST1700_FLASH_MPC_BASE);
        psram_view = best1700_mpc_map(core, &s->psram_mpc, &core->psram,
                                       BEST1700_PSRAM_MPC_BASE);
    }
    if (core->real_cp) {
        if (s->secure_boot) {
            psram_view = best1700_master_gate_init(&s->cp_psram_gate, OBJECT(s),
                "best1700.cp.psram-master", &s->sec,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psram_mpc), 1));
        } else {
            memory_region_init_alias(&s->cp_psram_view, OBJECT(s),
                "best1700.cp.psram-shared", &s->core[0].space,
                BEST1700_PSRAM_BASE, BEST1700_PSRAM_SIZE);
            psram_view = &s->cp_psram_view;
        }
    }
    if (core == &s->core[0] &&
        (core->secure_boot || s->bth_real_boot || s->cp_real_boot)) {
        for (unsigned i = 0; i < BEST1700_SYS_SRAM_BANKS; i++) {
            g_snprintf(name, sizeof(name), "best1700.sram%u", i);
            memory_region_init_ram(&s->sram_storage[i], NULL, name,
                                     BEST1700_SYS_SRAM_BANK_SIZE, &error_fatal);
        }
        if (s->bth_real_boot) {
            best1700_bth_bind_storage(&s->bth, OBJECT(s),
                                       &s->sram_storage[6], &s->sram_storage[7]);
        }
        for (unsigned i = 0; i < BEST1700_SYS_SRAM_BANKS; i++) {
            MemoryRegion *view = s->bth_real_boot && i >= 6 ?
                &s->bth.sys_ram[i - 6] : &s->sram_storage[i];

            if (core->secure_boot) {
                g_snprintf(name, sizeof(name), "sram-mpc[%u]", i);
                object_initialize_child(OBJECT(s), name, &s->sram_mpc[i],
                                          TYPE_BEST1700_MPC);
                view = best1700_mpc_map(core, &s->sram_mpc[i], view,
                    BEST1700_SRAM_MPC_BASE + i * BEST1700_SRAM_MPC_STRIDE);
            }
            memory_region_add_subregion(&core->space,
                BEST1700_SYS_SRAM_BASE + i * BEST1700_SYS_SRAM_BANK_SIZE, view);
            g_snprintf(name, sizeof(name), "best1700.sram%u-data", i);
            memory_region_init_alias(&s->sram_data[i], OBJECT(s), name,
                                      view, 0, BEST1700_SYS_SRAM_BANK_SIZE);
            /* Only the eight explicit SYS_SRAM banks override the generic
             * RAM scaffold. TCM core/AXI views remain behind their TGU. */
            memory_region_add_subregion_overlap(&core->space,
                BEST1700_SYS_SRAM_DATA_BASE + i * BEST1700_SYS_SRAM_BANK_SIZE,
                &s->sram_data[i], 2);
        }
    }

    if (s->bth_real_boot) {
        static const hwaddr bth_ram_base[] = {
            BEST1700_BTH_RAM_BASE, BEST1700_BTH_RAMX_BASE,
        };
        for (unsigned i = 0; i < G_N_ELEMENTS(bth_ram_base); i++) {
            g_snprintf(name, sizeof(name), "best1700.%s.bth-ram%u", core->name, i);
            memory_region_init_alias(&core->bth_ram[i], OBJECT(s), name,
                                      &s->bth.ram, 0, BEST1700_BTH_RAM_SIZE);
            memory_region_add_subregion_overlap(&core->space, bth_ram_base[i],
                                                 &core->bth_ram[i], 2);
        }
        if (core != &s->core[0]) {
            for (unsigned i = 0; i < 4; i++) {
                hwaddr base = 0x02600000u + (i % 2) * 0x100000u +
                    (i / 2) * 0x20000000u;

                g_snprintf(name, sizeof(name), "best1700.%s.sys-shared%u", core->name, i);
                memory_region_init_alias(&core->shared_sys_ram[i], OBJECT(s),
                    name, &s->bth.sys_ram[i % 2], 0, 0x100000);
                memory_region_add_subregion_overlap(&core->space, base,
                    &core->shared_sys_ram[i], 2);
            }
        }
        Best1700AonEndpoint endpoint = best1700_bth_endpoint(&s->bth);
        best1700_aon_connect_endpoint(&core->aon_cmu, &endpoint);
        for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_cache); i++) {
            g_snprintf(name, sizeof(name), "best1700.%s.bth-cache%u", core->name, i);
            memory_region_init_alias(&core->bth_cache[i], OBJECT(s), name,
                &s->bth_cache[i].region, 0, BEST1700_CACHE_REG_SIZE);
            memory_region_add_subregion(&core->space,
                0x0338a000 + i * 0x100000, &core->bth_cache[i]);
        }
        static const hwaddr bth_timer_base[] = {
            0x4008b000, 0x50002000, 0x50003000,
        };
        g_snprintf(name, sizeof(name), "best1700.%s.bth-timing-cmu", core->name);
        memory_region_init_alias(&core->bth_timing_cmu, OBJECT(s), name,
            &s->bth_timing.cmu, 0, BEST1700_BTH_TIMING_WINDOW);
        memory_region_add_subregion_overlap(&core->space, 0x50000000,
                                              &core->bth_timing_cmu, 4);
        g_snprintf(name, sizeof(name), "best1700.%s.bth-memsc-cmu", core->name);
        memory_region_init_alias(&core->bth_memsc_cmu, OBJECT(s), name,
            &s->bth_memsc.cmu, 0, BEST1700_BTH_MEMSC_WINDOW);
        memory_region_add_subregion_overlap(&core->space, 0x50000000,
                                              &core->bth_memsc_cmu, 4);
        for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_spi); i++) {
            g_snprintf(name, sizeof(name), "best1700.%s.bth-spi%u", core->name, i);
            memory_region_init_alias(&core->bth_spi[i], OBJECT(s), name,
                &s->bth_spi[i].iomem, 0, BEST1700_SPI_WINDOW);
            memory_region_add_subregion_overlap(&core->space,
                0x50010000 + i * 0x1000, &core->bth_spi[i], 2);
        }
        for (unsigned i = 0; i < BEST1700_BTH_TIMING_COUNT; i++) {
            g_snprintf(name, sizeof(name), "best1700.%s.bth-timer%u", core->name, i);
            memory_region_init_alias(&core->bth_timers[i], OBJECT(s), name,
                &s->bth_timing.timers[i].region, 0, BEST1700_BTH_TIMING_WINDOW);
            memory_region_add_subregion_overlap(&core->space, bth_timer_base[i],
                                                  &core->bth_timers[i], 3);
        }
    }

    memory_region_add_subregion(&core->space, core->flash_base, flash_view);
    g_snprintf(name, sizeof(name), "best1700.%s.flash-nc", core->name);
    memory_region_init_alias(&core->flash_nc, OBJECT(s), name, flash_view,
                              0, BEST1700_FLASH_SIZE);
    memory_region_add_subregion(&core->space,
                                core == &s->core[2] ?
                                BEST1700_BTH_FLASH_NC_BASE :
                                core->flash_base + 0x20000000u,
                                &core->flash_nc);
    g_snprintf(name, sizeof(name), "best1700.%s.flash-x", core->name);
    memory_region_init_alias(&core->flash_x, OBJECT(s), name, flash_view,
                              0, BEST1700_FLASH_SIZE);
    memory_region_add_subregion(&core->space, 0x08000000, &core->flash_x);
    if (core == &s->core[2]) {
        memory_region_init_alias(&core->bth_flash_x, OBJECT(s),
                                  "best1700.bth.flash-x-cached", flash_view,
                                  0, BEST1700_FLASH_SIZE);
        memory_region_add_subregion(&core->space, BEST1700_BTH_FLASHX_BASE,
                                    &core->bth_flash_x);
    }

    memory_region_add_subregion(&core->space, BEST1700_PSRAM_BASE, psram_view);
    g_snprintf(name, sizeof(name), "best1700.%s.psram-nc", core->name);
    memory_region_init_alias(&core->psram_nc, OBJECT(s), name, psram_view,
                              0, BEST1700_PSRAM_SIZE);
    memory_region_add_subregion(&core->space, 0x38000000, &core->psram_nc);
}

static void best1700_init_core(Best1700MachineState *s, unsigned index,
                               const char *name, uint32_t flash_base,
                               uint32_t code_offset, uint32_t boot_sp,
                               uint32_t boot_pc, bool execute)
{
    Best1700Core *core = &s->core[index];
    DeviceState *device;
    char region_name[64];
    core->name = name;
    core->flash_base = flash_base;
    core->code_offset = code_offset;
    core->boot_rom_size = s->boot_rom_size;
    core->boot_sp = boot_sp;
    core->boot_pc = boot_pc;
    core->sys_memsc = &s->sys_memsc;
    core->secure_boot = execute && s->secure_boot;
    /* AP system HDMA transactions use the AON master policy, independently
     * of the CPU's current Secure/Nonsecure execution state. Peer-local
     * DMA security routing is not established by this AP integration. */
    core->dma_sec = core->secure_boot ? &s->sec : NULL;
    core->real_bth = index == 2 && s->bth_real_boot;
    core->real_cp = index == 1 && s->cp_real_boot;
    memory_region_init(&core->space, OBJECT(s), name, UINT64_MAX);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.flash", name);
    memory_region_init_rom(&core->flash, NULL, region_name,
                           BEST1700_FLASH_SIZE, &error_fatal);
    memset(memory_region_get_ram_ptr(&core->flash), 0xff, BEST1700_FLASH_SIZE);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.flash-storage", name);
    address_space_init(&core->flash_storage_as, &core->flash, region_name);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.ram", name);
    memory_region_init_ram(&core->ram, NULL, region_name,
                           BEST1700_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(&core->space, BEST1700_RAM_BASE, &core->ram);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.psram", name);
    memory_region_init_ram(&core->psram, NULL, region_name,
                           BEST1700_PSRAM_SIZE, &error_fatal);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.low-ram", name);
    memory_region_init_ram(&core->low_ram, NULL, region_name,
                           BEST1700_LOW_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(&core->space, BEST1700_LOW_RAM_BASE,
                                &core->low_ram);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.sysctrl", name);
    memory_region_init_io(&core->sysctrl, NULL, &best1700_sysctrl_ops, core,
                          region_name, BEST1700_SYSCTRL_SIZE);
    memory_region_add_subregion(&core->space, BEST1700_SYSCTRL_BASE,
                                &core->sysctrl);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.sys-periph", name);
    memory_region_init_io(&core->sys_periph, NULL, &best1700_sys_periph_ops,
                          core, region_name, BEST1700_SYS_PERIPH_SIZE);
    memory_region_add_subregion_overlap(&core->space, BEST1700_SYS_PERIPH_BASE,
                                        &core->sys_periph, -2);
    /* Early boot code in some BEST1700 images uses the legacy 0x400aXXXX
     * aliases while later drivers use the documented 0x401aXXXX windows.
     * Keep both views backed by the same DMA state. */
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.dma0-alias",
               name);
    memory_region_init_alias(&core->dma0_alias, NULL, region_name,
                             &core->sys_periph, BEST1700_DMA0_OFFSET,
                             BEST1700_DMA_SIZE);
    memory_region_add_subregion_overlap(&core->space,
                                        BEST1700_DMA0_ALIAS_BASE,
                                        &core->dma0_alias, -1);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.dma1-alias",
               name);
    memory_region_init_alias(&core->dma1_alias, NULL, region_name,
                             &core->sys_periph, BEST1700_DMA1_OFFSET,
                             BEST1700_DMA_SIZE);
    memory_region_add_subregion_overlap(&core->space,
                                        BEST1700_DMA1_ALIAS_BASE,
                                        &core->dma1_alias, -1);
    /* The AP configures the BTH subsystem during early boot.  Keep this
     * window separate from the AP system-peripheral bank so it can later be
     * replaced by clock, timer, and sensor IP models without changing the
     * machine or board profile. */
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.bth-periph",
               name);
    best1700_apb_window_init(&core->bth_periph, OBJECT(s), region_name,
                             BEST1700_BTH_PERIPH_SIZE, NULL);
    best1700_apb_window_add(&core->bth_periph, &core->space,
                            BEST1700_BTH_PERIPH_BASE, -1);
    /* UART0/1 live inside the generic system-control aperture and need a
     * higher-priority IP window.  UART2 is a separate 0x40011000 window. */
    best1700_uart_init(&core->uart0, OBJECT(s), "uart0",
                       BEST1700_UART_SIZE, NULL);
    best1700_uart_init(&core->uart1, OBJECT(s), "uart1",
                       BEST1700_UART_SIZE, NULL);
    best1700_uart_init(&core->uart2, OBJECT(s), "uart2",
                       BEST1700_UART_SIZE, NULL);
    /* UART timing starts on the 26 MHz crystal source. */
    best1700_uart_set_clock(&core->uart0, 26000000);
    best1700_uart_set_clock(&core->uart1, 26000000);
    best1700_uart_set_clock(&core->uart2, 26000000);
    best1700_uart_add(&core->uart0, &core->space,
        core->real_bth ? 0x5000b000 : BEST1700_UART0_BASE, 1);
    best1700_uart_add(&core->uart1, &core->space,
        core->real_bth ? 0x5000c000 : BEST1700_UART1_BASE, 1);
    best1700_uart_add(&core->uart2, &core->space,
        core->real_bth ? 0x50071000 : BEST1700_UART2_BASE, 1);
    if (index == 0) {
        best1700_uart_set_chardev(&core->uart0, serial_hd(0));
        best1700_uart_set_chardev(&core->uart1, serial_hd(1));
        best1700_uart_set_chardev(&core->uart2, serial_hd(2));
    } else if (core->real_bth) {
        best1700_uart_set_chardev(&core->uart0, serial_hd(3));
    }
    /* 0x40080000 is the always-on clock-management aperture.  Keep it
     * separate from the synthetic IPC doorbell window: the latter is placed
     * at a high private address so firmware CMU accesses cannot be consumed
     * as RPMsg notifications. */
    best1700_aon_init(&core->aon_cmu, OBJECT(s), "aon-cmu",
                      BEST1700_AON_CMU, BEST1700_AON_WINDOW_SIZE, NULL);
    best1700_aon_init(&core->aon_wdt, OBJECT(s), "aon-wdt",
                      BEST1700_AON_WDT, BEST1700_AON_WINDOW_SIZE, NULL);
    best1700_aon_init(&core->aon_timer0, OBJECT(s), "aon-timer0",
                      BEST1700_AON_TIMER, BEST1700_AON_WINDOW_SIZE, NULL);
    best1700_aon_init(&core->aon_timer1, OBJECT(s), "aon-timer1",
                      BEST1700_AON_TIMER, BEST1700_AON_WINDOW_SIZE, NULL);
    best1700_aon_init(&core->sys_timer0, OBJECT(s), "sys-timer0",
                      BEST1700_AON_TIMER, BEST1700_AON_WINDOW_SIZE, NULL);
    best1700_aon_set_frequency(&core->aon_cmu, BEST1700_SLOW_TIMER_HZ);
    best1700_aon_set_frequency(&core->aon_timer0, BEST1700_SLOW_TIMER_HZ);
    best1700_aon_set_frequency(&core->aon_timer1, BEST1700_SLOW_TIMER_HZ);
    best1700_aon_set_frequency(&core->sys_timer0, BEST1700_SLOW_TIMER_HZ);
    best1700_aon_init(&core->aon_psc, OBJECT(s), "aon-psc",
                      BEST1700_AON_GENERIC, BEST1700_AON_WINDOW_SIZE, NULL);
    best1700_aon_init(&core->aon_generic, OBJECT(s), "aon-generic",
                      BEST1700_AON_GENERIC, BEST1700_AON_GENERIC_SIZE, NULL);
    best1700_aon_set_memsc(&core->aon_cmu, &s->aon_memsc);
    best1700_aon_add(&core->aon_cmu, &core->space, BEST1700_AON_CMU_BASE, 2);
    best1700_aon_add(&core->aon_wdt, &core->space, BEST1700_AON_WDT_BASE, 2);
    best1700_aon_add(&core->aon_timer0, &core->space,
                     BEST1700_AON_TIMER0_BASE, 2);
    best1700_aon_add(&core->aon_timer1, &core->space,
                     BEST1700_AON_TIMER1_BASE, 2);
    best1700_aon_add(&core->sys_timer0, &core->space,
                     BEST1700_SYS_TIMER0_BASE, 2);
    best1700_aon_add(&core->aon_psc, &core->space,
                     BEST1700_AON_PSC_BASE, 2);
    /* GPIO/IOMUX/PMU and the remaining AON blocks share a sparse 0x4008xxxx
     * aperture.  Keep an ordinary storage-backed fallback underneath the
     * typed windows above so firmware probes do not fault on an unmodelled
     * register. */
    best1700_aon_add(&core->aon_generic, &core->space,
                     BEST1700_AON_GENERIC_BASE, -1);
    if (index == 0) {
        memory_region_init_io(&s->ispi_region, NULL, &best1700_ispi_ops, s,
                              "best1700.ispi", BEST1700_ISPI_SIZE);
        memory_region_add_subregion_overlap(&core->space, BEST1700_ISPI_BASE,
                                            &s->ispi_region, 2);
    } else if (core->real_cp) {
        memory_region_init_alias(&core->sys_ispi, OBJECT(s),
            "best1700.cp.sys-ispi", &s->ispi_region, 0, BEST1700_ISPI_SIZE);
        memory_region_add_subregion_overlap(&core->space, BEST1700_ISPI_BASE,
                                             &core->sys_ispi, 2);
    }
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.vectors", name);
    memory_region_init_ram(&core->vectors, NULL, region_name,
                           0x1000, &error_fatal);
    memory_region_add_subregion(&core->space, 0, &core->vectors);
    MemoryRegion *dtcm_view = &core->ram;
    if (core->secure_boot) {
        object_initialize_child(OBJECT(s), "tgu", &s->tgu, TYPE_BEST1700_TGU);
        /* Gates span architectural TCM offsets, retaining holes in ITCM. */
        memory_region_init(&s->itcm_storage, OBJECT(s), "best1700.itcm-storage", 0x200000);
        memory_region_init_alias(&s->itcm_ram, OBJECT(s), "best1700.itcm-ram",
                                 &core->low_ram, 0, BEST1700_LOW_RAM_SIZE);
        memory_region_add_subregion(&s->itcm_storage, BEST1700_LOW_RAM_BASE,
                                    &s->itcm_ram);
        memory_region_init_alias(&s->itcm_vectors, OBJECT(s), "best1700.itcm-vectors",
                                 &core->vectors, 0, 0x1000);
        memory_region_add_subregion(&s->itcm_storage, 0, &s->itcm_vectors);
        memory_region_init_alias(&s->dtcm_storage, OBJECT(s), "best1700.dtcm-storage",
                                 &core->ram, 0, BEST1700_M55_DTCM_SIZE);
        object_property_set_link(OBJECT(&s->tgu), "itcm-downstream",
                                  OBJECT(&s->itcm_storage), &error_fatal);
        object_property_set_link(OBJECT(&s->tgu), "dtcm-downstream",
                                  OBJECT(&s->dtcm_storage), &error_fatal);
        object_property_set_link(OBJECT(&s->tgu), "armv7m",
                                  OBJECT(&core->cpu), &error_fatal);
        sysbus_realize(SYS_BUS_DEVICE(&s->tgu), &error_fatal);
        MemoryRegion *itcm_view = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->tgu), 1);
        dtcm_view = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->tgu), 2);
        memory_region_add_subregion_overlap(&core->space, 0, itcm_view, 3);
        memory_region_add_subregion_overlap(&core->space, BEST1700_RAM_BASE,
                                             dtcm_view, 3);
        memory_region_init_alias(&core->secure_itcm, OBJECT(s),
            "best1700.secure-itcm", itcm_view, BEST1700_LOW_RAM_BASE,
            BEST1700_LOW_RAM_SIZE);
        memory_region_add_subregion(&core->space, 0x04100000, &core->secure_itcm);
        memory_region_init_alias(&core->secure_dtcm, OBJECT(s),
            "best1700.secure-dtcm", dtcm_view, 0, BEST1700_M55_DTCM_SIZE);
        memory_region_add_subregion_overlap(&core->space, 0x24000000,
                                             &core->secure_dtcm, 2);
        memory_region_init_alias(&core->secure_peripherals, OBJECT(s),
            "best1700.secure-peripherals", &core->space, 0x40000000, 0x00400000);
        memory_region_add_subregion(&core->space, 0x44000000,
                                    &core->secure_peripherals);
    }
    if (index != 2) {
        /* Preserve the complete core ITCM view, including the TGU overlay
         * when enabled. The AXI window must not address separate RAM. */
        g_snprintf(region_name, sizeof(region_name), "best1700.%s.itcm-axi", name);
        memory_region_init_alias(&core->itcm_axi, NULL, region_name,
                                 &core->space, 0, 0x00200000);
        memory_region_add_subregion_overlap(&core->space,
            index == 1 ? 0x20e00000 : 0x21800000, &core->itcm_axi, 1);
        g_snprintf(region_name, sizeof(region_name), "best1700.%s.dtcm-axi", name);
        memory_region_init_alias(&core->dtcm_axi, NULL, region_name,
                                 dtcm_view, 0, BEST1700_M55_DTCM_SIZE);
        memory_region_add_subregion_overlap(&core->space,
            index == 1 ? BEST1700_M55C1_DTCM_AXI_BASE : BEST1700_M55_DTCM_AXI_BASE,
            &core->dtcm_axi, 1);
    }
    memory_region_init_alias(&core->shared, NULL, "best1700.shared",
                             &s->shared_storage, 0, BEST1700_SHARED_SIZE);
    /* The shared window is also inside the system RAM aperture.  Give the
     * alias priority so all CPU address spaces see the same backing store. */
    memory_region_add_subregion_overlap(&core->space, BEST1700_SHARED_BASE,
                                        &core->shared, 1);
    memory_region_init_io(&core->doorbell, NULL, &best1700_mmio_ops, s,
                          "best1700.doorbell", BEST1700_DOORBELL_SIZE);
    memory_region_add_subregion(&core->space, s->doorbell_base,
                                &core->doorbell);
    g_snprintf(region_name, sizeof(region_name), "best1700.%s.address-space",
               name);
    address_space_init(&core->address_space, &core->space, region_name);

    device = DEVICE(&core->cpu);
    qdev_prop_set_uint32(device, "num-irq", 128);
    /* Two M55 cores and a separate M33 BTH subsystem. TEE uses AP secure state. */
    qdev_prop_set_string(device, "cpu-type",
                         index == 2 ? ARM_CPU_TYPE_NAME("cortex-m33") :
                                      ARM_CPU_TYPE_NAME("cortex-m55"));
    if (core->real_bth || core->real_cp) {
        qdev_prop_set_bit(device, "start-powered-off", true);
    }
    qdev_connect_clock_in(device, "cpuclk", s->sysclk);
    qdev_connect_clock_in(device, "refclk", s->refclk);
    object_property_set_link(OBJECT(&core->cpu), "memory",
                             OBJECT(&core->space), &error_fatal);
    if (core->secure_boot) {
        object_property_set_link(OBJECT(&core->cpu), "idau", OBJECT(s), &error_fatal);
    }
    sysbus_realize(SYS_BUS_DEVICE(&core->cpu), &error_fatal);
    best1700_map_memory(s, core);
    best1700_uart_set_irq(&core->uart0, core->real_bth ? s->bth_irq[44] :
                             qdev_get_gpio_in(device, 55));
    best1700_uart_set_irq(&core->uart1, core->real_bth ? s->bth_irq[45] :
                             qdev_get_gpio_in(device, 56));
    best1700_uart_set_irq(&core->uart2, core->real_bth ? s->bth_irq[46] :
                             qdev_get_gpio_in(device, 57));
    if (core->secure_boot) {
        memory_region_add_subregion_overlap(&core->cpu.container, 0xe001e000,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->tgu), 0), 1);
    }
    core->dma_irq[0] = core->real_bth ? s->bth_irq[BEST1700_DMA0_IRQ] :
        qdev_get_gpio_in(DEVICE(&core->cpu), BEST1700_DMA0_IRQ);
    core->dma_irq[1] = core->real_bth ? s->bth_irq[BEST1700_DMA1_IRQ] :
        qdev_get_gpio_in(DEVICE(&core->cpu), BEST1700_DMA1_IRQ);
    qemu_irq_lower(core->dma_irq[0]);
    qemu_irq_lower(core->dma_irq[1]);
    core->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, best1700_dma_tick, core);
    best1700_uart_set_dma_notify(&core->uart0, best1700_uart_dma_notify, core);
    best1700_uart_set_dma_notify(&core->uart1, best1700_uart_dma_notify, core);
    best1700_uart_set_dma_notify(&core->uart2, best1700_uart_dma_notify, core);
    if (!core->real_bth) {
        for (unsigned timer = 0; timer < 2; timer++) {
            best1700_aon_set_timer_irq(&core->aon_timer0, timer,
                                       qdev_get_gpio_in(device, 58 + timer));
            best1700_aon_set_timer_irq(&core->aon_timer1, timer,
                                       qdev_get_gpio_in(device, 60 + timer));
            best1700_aon_set_timer_irq(&core->sys_timer0, timer,
                                       qdev_get_gpio_in(device, 66 + timer));
        }
    }
    if (core->real_bth) {
        static const unsigned bth_timer_irq[] = { 19, 23, 25 };

        for (unsigned i = 0; i < BEST1700_BTH_TIMING_COUNT; i++) {
            for (unsigned timer = 0; timer < 2; timer++) {
                best1700_aon_set_timer_irq(&s->bth_timing.timers[i], timer,
                    s->bth_irq[bth_timer_irq[i] + timer]);
            }
            best1700_aon_set_pc(&s->bth_timing.timers[i],
                                  &core->cpu.cpu->env.regs[15]);
        }
    }
    best1700_apb_window_set_pc(&core->bth_periph,
                               &core->cpu.cpu->env.regs[15]);
    best1700_aon_set_pc(&core->aon_cmu, &core->cpu.cpu->env.regs[15]);
    best1700_aon_set_pc(&core->aon_wdt, &core->cpu.cpu->env.regs[15]);
    best1700_aon_set_pc(&core->aon_timer0, &core->cpu.cpu->env.regs[15]);
    best1700_aon_set_pc(&core->aon_timer1, &core->cpu.cpu->env.regs[15]);
    best1700_aon_set_pc(&core->sys_timer0, &core->cpu.cpu->env.regs[15]);
    best1700_aon_set_pc(&core->aon_psc, &core->cpu.cpu->env.regs[15]);
    best1700_aon_set_pc(&core->aon_generic, &core->cpu.cpu->env.regs[15]);
    best1700_uart_set_pc(&core->uart0, &core->cpu.cpu->env.regs[15]);
    best1700_uart_set_pc(&core->uart1, &core->cpu.cpu->env.regs[15]);
    best1700_uart_set_pc(&core->uart2, &core->cpu.cpu->env.regs[15]);
    if (execute) {
        if (!s->parent_obj.kernel_filename) {
            error_setg(&error_fatal, "miband-best1700 requires -kernel vela_ap.bin");
        }
        armv7m_load_kernel(core->cpu.cpu, s->parent_obj.kernel_filename,
                           flash_base + code_offset, BEST1700_FLASH_SIZE - code_offset);
        if (core->secure_boot) {
            best1700_load_secure_image(s, core);
        } else {
            best1700_seed_boot_rom(core);
        }
    } else {
        const char *image = index == 1 ? s->cp_image : s->bth_image;
        best1700_load_image(core, image);
        if (!core->real_bth && !core->real_cp) {
            best1700_stub_code(core);
        }
        armv7m_load_kernel(core->cpu.cpu, NULL, 0, BEST1700_FLASH_SIZE);
    }
    best1700_core_reset(core);
    best1700_write_vectors(core);
    core->ready = true;
}

static void best1700_connect_cp_memory(Best1700MachineState *s)
{
    Best1700Core *ap = &s->core[0];
    Best1700Core *cp = &s->core[1];
    MemoryRegion *ap_itcm = &ap->itcm_axi;
    MemoryRegion *ap_dtcm = &ap->dtcm_axi;
    static const hwaddr sys_base[] = {
        BEST1700_SYS_SRAM_BASE, BEST1700_SYS_SRAM_DATA_BASE,
    };

    /* Bus masters use the peer's physical TCM storage through its AXI view;
     * the two processors' local TCM addresses must remain independent. */
    memory_region_init_alias(&s->cp_itcm_view, OBJECT(s),
        "best1700.ap.cp-itcm", &cp->itcm_axi, 0, 0x200000);
    memory_region_add_subregion_overlap(&ap->space, BEST1700_CP_ITCM_AXI_BASE,
                                        &s->cp_itcm_view, 2);
    memory_region_init_alias(&s->cp_dtcm_view, OBJECT(s),
        "best1700.ap.cp-dtcm", &cp->dtcm_axi, 0, BEST1700_M55_DTCM_SIZE);
    memory_region_add_subregion_overlap(&ap->space, BEST1700_CP_DTCM_AXI_BASE,
                                        &s->cp_dtcm_view, 2);
    if (s->secure_boot) {
        ap_itcm = best1700_master_gate_init(&s->cp_ap_itcm_gate, OBJECT(s),
            "best1700.cp.ap-itcm-master", &s->sec,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->tgu), 1));
        ap_dtcm = best1700_master_gate_init(&s->cp_ap_dtcm_gate, OBJECT(s),
            "best1700.cp.ap-dtcm-master", &s->sec,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->tgu), 2));
        memory_region_init(&s->cp_sram_master_view, OBJECT(s),
            "best1700.cp.sram-masters",
            BEST1700_SYS_SRAM_BANKS * BEST1700_SYS_SRAM_BANK_SIZE);
        for (unsigned i = 0; i < BEST1700_SYS_SRAM_BANKS; i++) {
            g_autofree char *name = g_strdup_printf("best1700.cp.sram%u-master", i);
            MemoryRegion *view = best1700_master_gate_init(&s->cp_sram_gate[i],
                OBJECT(s), name, &s->sec,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->sram_mpc[i]), 1));

            memory_region_add_subregion(&s->cp_sram_master_view,
                i * BEST1700_SYS_SRAM_BANK_SIZE, view);
        }
    }
    memory_region_init_alias(&s->cp_ap_itcm_view, OBJECT(s),
        "best1700.cp.ap-itcm", ap_itcm, 0, 0x200000);
    memory_region_add_subregion_overlap(&cp->space, 0x21800000,
                                        &s->cp_ap_itcm_view, 2);
    memory_region_init_alias(&s->cp_ap_dtcm_view, OBJECT(s),
        "best1700.cp.ap-dtcm", ap_dtcm, 0, BEST1700_M55_DTCM_SIZE);
    memory_region_add_subregion_overlap(&cp->space, BEST1700_M55_DTCM_AXI_BASE,
                                        &s->cp_ap_dtcm_view, 2);
    for (unsigned i = 0; i < G_N_ELEMENTS(sys_base); i++) {
        g_autofree char *name = g_strdup_printf("best1700.cp.sys-sram%u", i);

        memory_region_init_alias(&s->cp_sys_sram[i], OBJECT(s), name,
            s->secure_boot ? &s->cp_sram_master_view : &ap->space,
            s->secure_boot ? 0 : sys_base[i],
            BEST1700_SYS_SRAM_BANKS * BEST1700_SYS_SRAM_BANK_SIZE);
        memory_region_add_subregion_overlap(&cp->space, sys_base[i],
                                             &s->cp_sys_sram[i], 3);
    }
}

/* DSP reset, runstall and core-wait are hardware controls. Image addresses
 * and the software peer protocol belong to the configured HIFI endpoint. */
static void best1700_dsp_update(Best1700MachineState *s)
{
    best1700_hifi4_pause(s->hifi4,
                         s->dsp_reset_asserted || (s->dsp_runstall & 1) ||
                         s->dsp_core_wait);
    if (!s->dsp_reset_asserted && !(s->dsp_runstall & 1) &&
        !s->dsp_core_wait && !s->dsp_started) {
        s->dsp_started = true;
        best1700_hifi4_start(s->hifi4, s->dsp_vector);
    }
}

static uint32_t best1700_dsp_aon_read(void *opaque, hwaddr offset,
                                      uint32_t *value)
{
    Best1700MachineState *s = opaque;

    switch (offset) {
    case 0xc4:
        *value = 0;
        return 1u << 10;
    case 0xc8:
    case 0xcc:
        *value = s->dsp_reset_asserted ? 0 : 1u << 10;
        return 1u << 10;
    case 0x160:
        *value = s->dsp_runstall;
        return 3;
    case 0x164:
        *value = s->dsp_vector;
        return UINT32_MAX;
    case 0x1a4:
        *value = s->dsp_core_wait;
        return 1u << 2;
    default:
        return 0;
    }
}

static uint32_t best1700_dsp_aon_write(void *opaque, hwaddr offset,
                                       uint32_t value, uint32_t lane_mask)
{
    Best1700MachineState *s = opaque;
    uint32_t handled;

    switch (offset) {
    case 0xc4:
    case 0xc8:
    case 0xcc:
        handled = 1u << 10;
        if (value & lane_mask & handled) {
            if (offset != 0xcc) {
                best1700_hifi4_reset(s->hifi4);
                s->dsp_started = false;
            }
            if (offset != 0xc4) {
                s->dsp_reset_asserted = offset == 0xc8;
            }
        }
        break;
    case 0x160:
        handled = 3;
        lane_mask &= handled;
        s->dsp_runstall = (s->dsp_runstall & ~lane_mask) | (value & lane_mask);
        break;
    case 0x164:
        handled = UINT32_MAX;
        s->dsp_vector = (s->dsp_vector & ~lane_mask) | (value & lane_mask);
        break;
    case 0x1a4:
        handled = 1u << 2;
        lane_mask &= handled;
        s->dsp_core_wait = (s->dsp_core_wait & ~lane_mask) | (value & lane_mask);
        break;
    default:
        return 0;
    }
    best1700_dsp_update(s);
    return handled;
}

static void best1700_dsp_reset(Best1700MachineState *s)
{
    best1700_hifi4_reset(s->hifi4);
    s->dsp_reset_asserted = true;
    s->dsp_started = false;
    s->dsp_runstall = 1;
    s->dsp_core_wait = 1u << 2;
    s->dsp_vector = 0;
}

static void best1700_reset(void *opaque)
{
    Best1700MachineState *s = opaque;
    best1700_rf_matrix_reset(&s->rf_matrix);
    miband_gpu_async_reset(&s->gpu);
    memset(s->gpu.states, 0, sizeof(s->gpu.states));
    s->gpu.clock_control = 0;
    s->gpu.intr_enable = 0;
    s->gpu.cmd_address = 0;
    s->gpu.cmd_size = 0;
    best1700_dsp_reset(s);
    best1700_memsc_reset(&s->sys_memsc);
    best1700_memsc_reset(&s->aon_memsc);
    for (unsigned i = 0; i < BEST1700_CORE_COUNT; i++) {
        s->core[i].ready = true;
        best1700_uart_reset(&s->core[i].uart0);
        best1700_uart_reset(&s->core[i].uart1);
        best1700_uart_reset(&s->core[i].uart2);
        timer_del(s->core[i].dma_timer);
        memset(s->core[i].dma_regs, 0, sizeof(s->core[i].dma_regs));
        memset(s->core[i].dma_pending, 0, sizeof(s->core[i].dma_pending));
        best1700_dma_refresh_irq(&s->core[i], 0);
        best1700_dma_refresh_irq(&s->core[i], 1);
        best1700_aon_reset(&s->core[i].aon_cmu);
        best1700_aon_reset(&s->core[i].aon_wdt);
        best1700_aon_reset(&s->core[i].aon_timer0);
        best1700_aon_reset(&s->core[i].aon_timer1);
        best1700_aon_reset(&s->core[i].sys_timer0);
        best1700_aon_set_frequency(&s->core[i].sys_timer0,
                                   BEST1700_SLOW_TIMER_HZ);
        s->core[i].sysctrl_regs[0x5c >> 2] = 0;
        s->core[i].sysctrl_regs[0x90 >> 2] = 0;
        s->core[i].sysctrl_regs[0x94 >> 2] = 0;
        best1700_cmu_clock_reset(s->core[i].sysctrl_regs,
                                G_N_ELEMENTS(s->core[i].sysctrl_regs),
                                BEST1700_CMU_SYS);
        s->core[i].sysctrl_regs[0x1a0 >> 2] = 0;
        best1700_aon_reset(&s->core[i].aon_psc);
        best1700_aon_reset(&s->core[i].aon_generic);
        s->core[i].fast_ticks = 0;
        s->core[i].fast_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (s->bth_real_boot) {
        best1700_sys_bth_ipc_reset(&s->sys_bth_ipc);
        best1700_bth_reset(&s->bth);
        best1700_bth_power_reset(&s->bth_power);
        memset(s->bth_irq_level, 0, sizeof(s->bth_irq_level));
        s->bth_wake_aggregate = false;
        for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_spi); i++) {
            best1700_spi_reset(&s->bth_spi[i]);
        }
        best1700_bth_timing_reset(&s->bth_timing);
        best1700_bth_memsc_reset(&s->bth_memsc);
        for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_cache); i++) {
            best1700_cache_reset(&s->bth_cache[i]);
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_dma); i++) {
            best1700_dma_reset(&s->bth_dma[i]);
        }
    }
    if (s->cp_real_boot) {
        best1700_m55_ipc_reset(&s->m55_ipc);
        best1700_cp_reset(&s->cp_control);
    }
    if (s->btc_real_boot) {
        s->btc_ipc_wake = false;
        best1700_btc_power_reset(&s->btc_power);
        best1700_btc_cmu_reset(&s->btc_cmu);
        best1700_btc_aon_reset(&s->btc_aon);
        best1700_btc_cpudump_reset(&s->btc_cpudump);
        best1700_btc_gpio_reset(&s->btc_gpio);
        best1700_btc_modem_reset(&s->btc_modem);
        best1700_btc_baseband_reset(&s->btc_baseband);
        best1700_btc_ipc_reset(&s->btc_ipc);
        best1700_dma_reset(&s->btc_dma);
        best1700_uart_reset(&s->btc_uart);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->channels); i++) {
        s->channels[i].resets++;
        s->channels[i].tx_count = 0;
        s->channels[i].rx_count = 0;
        s->channels[i].registered = true;
        s->channels[i].ready = true;
        s->channels[i].shutdown = false;
    }
    if (s->bth_real_boot) {
        s->channels[1].registered = false;
        s->channels[1].ready = false;
    }
    if (s->cp_real_boot) {
        s->channels[0].registered = false;
        s->channels[0].ready = false;
    }
}

static char *best1700_get_profile(Object *obj, Error **errp)
{
    return g_strdup(MIBAND_BEST1700(obj)->profile_id);
}

static void best1700_set_profile(Object *obj, const char *value, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    if (s->workers_started) {
        error_setg(errp, "profile-id is startup-only");
        return;
    }
    g_free(s->profile_id);
    s->profile_id = g_strdup(value);
}

static char *best1700_get_image(Object *obj, Error **errp, char **slot)
{
    return g_strdup(*slot ? *slot : "");
}

static void best1700_set_image(Object *obj, const char *value, Error **errp,
                               char **slot)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    if (s->workers_started) {
        error_setg(errp, "image paths are startup-only");
        return;
    }
    g_free(*slot);
    *slot = g_strdup(value);
}

static char *best1700_get_cp_image(Object *obj, Error **errp)
{
    return best1700_get_image(obj, errp, &MIBAND_BEST1700(obj)->cp_image);
}

static void best1700_set_cp_image(Object *obj, const char *value, Error **errp)
{
    best1700_set_image(obj, value, errp, &MIBAND_BEST1700(obj)->cp_image);
}

static char *best1700_get_bth_image(Object *obj, Error **errp)
{
    return best1700_get_image(obj, errp, &MIBAND_BEST1700(obj)->bth_image);
}

static void best1700_set_bth_image(Object *obj, const char *value, Error **errp)
{
    best1700_set_image(obj, value, errp, &MIBAND_BEST1700(obj)->bth_image);
}

static char *best1700_get_tee_image(Object *obj, Error **errp)
{
    return best1700_get_image(obj, errp, &MIBAND_BEST1700(obj)->tee_image);
}

static void best1700_set_tee_image(Object *obj, const char *value, Error **errp)
{
    best1700_set_image(obj, value, errp, &MIBAND_BEST1700(obj)->tee_image);
}

static char *best1700_get_audio_image(Object *obj, Error **errp)
{
    return best1700_get_image(obj, errp, &MIBAND_BEST1700(obj)->audio_image);
}

static void best1700_set_audio_image(Object *obj, const char *value, Error **errp)
{
    best1700_set_image(obj, value, errp, &MIBAND_BEST1700(obj)->audio_image);
}

static char *best1700_get_audio_profile(Object *obj, Error **errp)
{
    return best1700_get_image(obj, errp,
                              &MIBAND_BEST1700(obj)->audio_endpoint_profile);
}

static void best1700_set_audio_profile(Object *obj, const char *value,
                                       Error **errp)
{
    best1700_set_image(obj, value, errp,
                        &MIBAND_BEST1700(obj)->audio_endpoint_profile);
}

static char *best1700_get_audio_status(Object *obj, Error **errp)
{
    return best1700_hifi4_status_json(MIBAND_BEST1700(obj)->hifi4);
}

static char *best1700_get_core_status(Object *obj, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    Best1700Core *bth = &s->core[2];
    Best1700Core *cp = &s->core[1];
    const char *cp_status = !s->cp_real_boot ? "stub" :
        cp->peer_reset_pending ? "reset-pending" :
        cp->peer_running ? "running" :
        s->cp_control.reset_asserted ? "held-reset" : "waiting";
    const char *bth_status = !s->bth_real_boot ? "stub" :
        best1700_bth_power_is_off(&s->bth_power) ? "power-gated" :
        best1700_bth_power_is_clock_stopped(&s->bth_power) ? "clock-stopped" :
        bth->peer_reset_pending ? "reset-pending" :
        bth->peer_running ? "running" : "held-reset";
    const char *btc_status = !s->btc_real_boot ? "disabled" :
        best1700_btc_power_is_off(&s->btc_power) ? "power-gated" :
        s->btc_core.peer_reset_pending ? "reset-pending" :
        s->btc_core.peer_running ? "running" : "held-reset";
    const char *audio_status = !s->hifi4 ? "scaffold" :
        best1700_hifi4_service_ready(s->hifi4) ? "service-ready" :
        s->dsp_reset_asserted ? "held-reset" : "endpoint";
    return g_strdup_printf("{\"ap\":\"%s\",\"cp\":\"%s\",\"bth\":\"%s\","
                           "\"tee\":\"%s\",\"audio\":\"%s\",\"btc\":\"%s\"}",
                           s->core[0].cpu.cpu->env.v7m.secure ? "secure" : "nonsecure",
                           cp_status, bth_status,
                           s->secure_boot ? "secure-world" : "endpoint",
                           audio_status, btc_status);
}

static char *best1700_get_cp_status(Object *obj, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);

    return g_strdup_printf("{\"mode\":\"%s\",\"vtor\":%u,"
        "\"reset_asserted\":%s,\"waiting\":%s}",
        s->cp_real_boot ? "hardware" : "stub", s->cp_control.vtor,
        s->cp_control.reset_asserted ? "true" : "false",
        s->cp_control.core_wait ? "true" : "false");
}

static char *best1700_get_bth_status(Object *obj, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);

    return g_strdup_printf("{\"mode\":\"%s\",\"ram_cfg\":%u,"
        "\"vtor\":%u,\"unsupported_accesses\":%" PRIu64 "}",
        s->bth_real_boot ? "hardware" : "stub", s->bth.ram_cfg,
        s->bth.vtor[0], s->bth.unsupported_accesses);
}

static char *best1700_get_ipc_status(Object *obj, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    g_autofree char *audio = s->hifi4 ? best1700_hifi4_status_json(s->hifi4) :
        g_strdup_printf("{\"model\":\"scaffold\",\"tx\":%u,\"rx\":%u,"
                        "\"ready\":%s,\"registered\":%s,\"resets\":%u}",
            s->channels[2].tx_count, s->channels[2].rx_count,
            s->channels[2].ready ? "true" : "false",
            s->channels[2].registered ? "true" : "false",
            s->channels[2].resets);
    return g_strdup_printf(
        "{\"ap-cp\":{\"tx\":%u,\"rx\":%u,\"ready\":%s,\"registered\":%s,\"resets\":%u},"
        "\"ap-bth\":{\"tx\":%u,\"rx\":%u,\"ready\":%s,\"registered\":%s,\"resets\":%u},"
        "\"ap-audio\":%s,"
        "\"ap-tee\":{\"tx\":%u,\"rx\":%u,\"ready\":%s,\"registered\":%s,\"resets\":%u}}",
        s->channels[0].tx_count, s->channels[0].rx_count,
        s->channels[0].ready ? "true" : "false", s->channels[0].registered ? "true" : "false", s->channels[0].resets,
        s->channels[1].tx_count, s->channels[1].rx_count,
        s->channels[1].ready ? "true" : "false", s->channels[1].registered ? "true" : "false", s->channels[1].resets,
        audio,
        s->channels[3].tx_count, s->channels[3].rx_count,
        s->channels[3].ready ? "true" : "false", s->channels[3].registered ? "true" : "false", s->channels[3].resets);
}

static void best1700_get_display_frame(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    visit_type_uint32(v, name, &MIBAND_BEST1700(obj)->display_frame, errp);
}

static bool best1700_get_ipc_reset(Object *obj, Error **errp)
{
    return false;
}

static void best1700_set_ipc_reset(Object *obj, bool value, Error **errp)
{
    if (value) {
        best1700_reset(MIBAND_BEST1700(obj));
    }
}

static void best1700_get_boot_vector(Object *obj, Visitor *v,
                                     const char *name, void *opaque, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    uint32_t value = !strcmp(name, "boot-sp") ? s->boot_sp :
                     !strcmp(name, "boot-pc") ? s->boot_pc : s->boot_rom_size;
    visit_type_uint32(v, name, &value, errp);
}

static void best1700_set_boot_vector(Object *obj, Visitor *v,
                                     const char *name, void *opaque, Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    uint32_t value;
    if (s->workers_started) {
        error_setg(errp, "%s is startup-only", name);
        return;
    }
    visit_type_uint32(v, name, &value, errp);
    if (*errp) {
        return;
    }
    if (!strcmp(name, "boot-sp")) {
        s->boot_sp = value;
    } else if (!strcmp(name, "boot-pc")) {
        s->boot_pc = value;
    } else {
        s->boot_rom_size = value;
    }
}

static uint32_t *best1700_image_property(Best1700MachineState *s,
                                         const char *name)
{
    static const char * const prefixes[BEST1700_IMAGE_COUNT] = {
        "ap", "cp", "bth", "tee"
    };
    for (unsigned i = 0; i < BEST1700_IMAGE_COUNT; i++) {
        char property[32];
        g_snprintf(property, sizeof(property), "%s-flash-base", prefixes[i]);
        if (!strcmp(name, property)) {
            return &s->image_flash_base[i];
        }
        g_snprintf(property, sizeof(property), "%s-code-offset", prefixes[i]);
        if (!strcmp(name, property)) {
            return &s->image_code_offset[i];
        }
        g_snprintf(property, sizeof(property), "%s-entry", prefixes[i]);
        if (!strcmp(name, property)) {
            return &s->image_entry[i];
        }
    }
    if (!strcmp(name, "shared-base")) {
        return &s->shared_base;
    }
    if (!strcmp(name, "doorbell-base")) {
        return &s->doorbell_base;
    }
    return NULL;
}

static void best1700_get_image_property(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    uint32_t *value = best1700_image_property(s, name);
    if (!value) {
        error_setg(errp, "unknown BEST1700 layout property %s", name);
        return;
    }
    visit_type_uint32(v, name, value, errp);
}

static void best1700_set_image_property(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    uint32_t *value = best1700_image_property(s, name);
    if (!value) {
        error_setg(errp, "unknown BEST1700 layout property %s", name);
        return;
    }
    if (s->workers_started) {
        error_setg(errp, "%s is startup-only", name);
        return;
    }
    visit_type_uint32(v, name, value, errp);
}

static void best1700_machine_done(Notifier *notifier, void *data)
{
    Best1700MachineState *s = container_of(notifier, Best1700MachineState,
                                          machine_done);
    /* The default bus resets NVIC after CPU reset callbacks.  Install the
     * application handoff after that reset so ITNS is not overwritten. */
    for (unsigned i = 0; i < BEST1700_CORE_COUNT; i++) {
        qemu_register_reset(best1700_core_reset, &s->core[i]);
    }
    if (s->btc_real_boot) {
        qemu_register_reset(best1700_core_reset, &s->btc_core);
    }
}

static void best1700_nor_changed(void *opaque, uint64_t offset, uint64_t length)
{
    Best1700Core *core = opaque;
    const uint8_t *backing = memory_region_get_ram_ptr(&core->flash);
    uint8_t page[4096];

    g_assert(offset <= BEST1700_FLASH_SIZE &&
             length <= BEST1700_FLASH_SIZE - offset);
    /* The die has already committed program/erase to its borrowed backing.
     * Re-publish through the ROM write API to dirty pages and invalidate
     * translated instructions in every alias. This is an internal die update,
     * so use its backing address space, regardless of the CPU MPC partition.
     * Use a separate bounded buffer because write_rom uses memcpy internally. */
    while (length) {
        size_t count = MIN(length, sizeof(page));
        memcpy(page, backing + offset, count);
        address_space_write_rom(&core->flash_storage_as, offset,
                                 MEMTXATTRS_UNSPECIFIED, page, count);
        offset += count;
        length -= count;
    }
}

static void best1700_btc_alias(Best1700MachineState *s, MemoryRegion *view,
                                MemoryRegion *space, MemoryRegion *storage,
                                hwaddr base, const char *name)
{
    memory_region_init_alias(view, OBJECT(s), name, storage, 0,
                              memory_region_size(storage));
    memory_region_add_subregion(space, base, view);
}

static void best1700_btc_refresh_wake(Best1700MachineState *s)
{
    /* The established always-on source aggregates IPC requests and the
     * LPO deadline. This is a functional route, not a pin timing model. */
    bool timer_wake = s->btc_baseband.sleep_state ==
                     BEST1700_BTC_NEEDS_CORRECTION ||
                     s->btc_baseband.sleep_state == BEST1700_BTC_CORRECTING;

    best1700_btc_power_set_wake(&s->btc_power, s->btc_ipc_wake || timer_wake);
}

static void best1700_btc_mac_sleep(void *opaque, bool ready)
{
    Best1700MachineState *s = opaque;

    best1700_btc_cmu_set_mac_sleep(&s->btc_cmu, ready);
    best1700_btc_refresh_wake(s);
}

static void best1700_btc_ipc_wake(void *opaque, bool level)
{
    Best1700MachineState *s = opaque;

    s->btc_ipc_wake = level;
    best1700_btc_refresh_wake(s);
    best1700_btc_baseband_set_external_wake(&s->btc_baseband, level);
}

static void best1700_btc_power_sleep(void *opaque)
{
    Best1700MachineState *s = opaque;

    best1700_btc_power_request_sleep(&s->btc_power);
}

static void best1700_btc_power_changed(void *opaque, bool off)
{
    Best1700MachineState *s = opaque;
    bool held = off || best1700_btc_cmu_cpu_held(&s->btc_cmu);

    /* PSC gates the CPU and releases it through its real vector on wake.
     * SRAM, IPC latches, retention flags and LPO time belong to other domains. */
    best1700_peer_schedule(&s->btc_core, held, !off, s->btc_cmu.vtor);
}

static void best1700_btc_cpu_reset(void *opaque, bool asserted, uint32_t vtor)
{
    Best1700MachineState *s = opaque;

    best1700_peer_schedule(&s->btc_core,
        asserted || best1700_btc_power_is_off(&s->btc_power), true, vtor);
}

static void best1700_btc_cpu_reset_done(void *opaque)
{
    Best1700MachineState *s = opaque;

    /* NVIC reset discards its input state. Re-drive retained peripheral
     * levels without consuming any pending request or completion. */
    best1700_btc_ipc_refresh(&s->btc_ipc);
    best1700_btc_baseband_set_irq(&s->btc_baseband,
        qdev_get_gpio_in(DEVICE(&s->btc_core.cpu), 1));
    best1700_dma_set_irq(&s->btc_dma,
        qdev_get_gpio_in(DEVICE(&s->btc_core.cpu), 3));
    best1700_btc_cmu_set_irq(&s->btc_cmu,
        qdev_get_gpio_in(DEVICE(&s->btc_core.cpu), 6));
}

static void best1700_bth_audma_terminal(void *opaque, unsigned channel)
{
    Best1700MachineState *s = opaque;

    /* BTH AUDMA channels occupy capture source IDs 34..39. */
    best1700_btc_baseband_dma_terminal_source(&s->btc_baseband,
                                              channel + 34, true);
}

static void best1700_bth_gpdma_terminal(void *opaque, unsigned channel)
{
    Best1700MachineState *s = opaque;

    /* BTH GPDMA channels occupy capture source IDs 28..31. */
    best1700_btc_baseband_dma_terminal_source(&s->btc_baseband,
                                              channel + 28, true);
}

static void best1700_init_btc(Best1700MachineState *s)
{
    Best1700Core *btc = &s->btc_core;
    DeviceState *device;
    Best1700AonEndpoint endpoint;

    btc->name = "btc";
    btc->real_btc = true;
    memory_region_init(&btc->space, OBJECT(s), "best1700.btc", UINT64_MAX);
    address_space_init(&btc->address_space, &btc->space,
                         "best1700.btc.address-space");
    /* The controller follows peer-owned message pointers in the existing
     * inter-core shared window. The IPC device itself never consumes them. */
    best1700_btc_alias(s, &btc->shared, &btc->space, &s->shared_storage,
                        BEST1700_SHARED_BASE, "best1700.btc.shared");
    memory_region_init_ram(&s->btc_ram, NULL, "best1700.btc-private",
                            0x18000, &error_fatal);
    memory_region_init_ram(&s->btc_exchange, NULL, "best1700.btc-exchange",
                            0x1c000, &error_fatal);
    best1700_btc_cmu_init(&s->btc_cmu, OBJECT(s), s->btc_cal_ref_hz,
                           s->btc_lpo_hz);
    best1700_btc_aon_init(&s->btc_aon, OBJECT(s));
    best1700_btc_power_init(&s->btc_power, OBJECT(s));
    best1700_btc_cpudump_init(&s->btc_cpudump, OBJECT(s),
                                s->btc_cpudump_sample_ns);
    best1700_btc_gpio_init(&s->btc_gpio, OBJECT(s));
    best1700_btc_modem_init(&s->btc_modem, OBJECT(s));
    best1700_btc_baseband_init(&s->btc_baseband, OBJECT(s), s->btc_lpo_hz,
                               s->btc_correction_ns);
    best1700_btc_baseband_connect_exchange(&s->btc_baseband, &s->btc_exchange);
    best1700_btc_baseband_connect_sleep(&s->btc_baseband, s,
                                       best1700_btc_mac_sleep);
    best1700_btc_ipc_init(&s->btc_ipc, OBJECT(s));
    best1700_dma_init(&s->btc_dma, OBJECT(s), "best1700.btc-dma", 8,
                       &btc->address_space);
    best1700_dma_connect_terminal(&s->btc_dma, &s->btc_baseband,
                                  best1700_btc_baseband_dma_terminal);
    best1700_uart_init(&s->btc_uart, OBJECT(s), "btc-uart", 0x1000, NULL);
    best1700_uart_set_chardev(&s->btc_uart, serial_hd(4));
    endpoint = best1700_btc_cmu_endpoint(&s->btc_cmu);
    for (unsigned i = 0; i < G_N_ELEMENTS(s->btc_view); i++) {
        Best1700BtcView *view = &s->btc_view[i];
        Best1700Core *core = i < BEST1700_CORE_COUNT ? &s->core[i] : btc;
        g_autofree char *prefix = g_strdup_printf("best1700.%s.btc", core->name);
        g_autofree char *ram = g_strdup_printf("%s.ram", prefix);
        g_autofree char *ramrun = g_strdup_printf("%s.ramrun", prefix);
        g_autofree char *ramrun_x = g_strdup_printf("%s.ramrun-x", prefix);
        g_autofree char *exchange = g_strdup_printf("%s.exchange", prefix);
        g_autofree char *cmu = g_strdup_printf("%s.cmu", prefix);
        g_autofree char *uart = g_strdup_printf("%s.uart", prefix);
        g_autofree char *retention = g_strdup_printf("%s.retention", prefix);
        g_autofree char *cpudump = g_strdup_printf("%s.cpudump", prefix);
        g_autofree char *gpio = g_strdup_printf("%s.gpio", prefix);
        g_autofree char *modem = g_strdup_printf("%s.modem", prefix);
        g_autofree char *baseband = g_strdup_printf("%s.baseband", prefix);
        g_autofree char *dma = g_strdup_printf("%s.dma", prefix);

        best1700_btc_alias(s, &view->ram, &core->space, &s->btc_ram,
                            0x68000000, ram);
        best1700_btc_alias(s, &view->ramrun[0], &core->space, &s->bth.btc_ram,
                            0x68400000, ramrun);
        best1700_btc_alias(s, &view->ramrun[1], &core->space, &s->bth.btc_ram,
                            0x60400000, ramrun_x);
        best1700_btc_alias(s, &view->exchange, &core->space, &s->btc_exchange,
                            0x6d020000, exchange);
        best1700_btc_alias(s, &view->cmu, &core->space, &s->btc_cmu.iomem,
                            BEST1700_BTC_CMU_BASE, cmu);
        best1700_btc_alias(s, &view->uart, &core->space, &s->btc_uart.region,
                            0x6e000000, uart);
        best1700_btc_alias(s, &view->retention, &core->space, &s->btc_aon.iomem,
                            BEST1700_BTC_AON_BASE, retention);
        memory_region_init_alias(&view->power, OBJECT(s), "best1700.btc-power",
                                  &s->btc_power.iomem, 0, 0x1000);
        memory_region_add_subregion_overlap(&core->space, BEST1700_AON_PSC_BASE,
                                             &view->power, 5);
        best1700_btc_alias(s, &view->cpudump, &core->space, &s->btc_cpudump.iomem,
                            BEST1700_BTC_CPUDUMP_BASE, cpudump);
        best1700_btc_alias(s, &view->gpio, &core->space, &s->btc_gpio.iomem,
                            BEST1700_BTC_GPIO_BASE, gpio);
        best1700_btc_alias(s, &view->modem, &core->space, &s->btc_modem.iomem,
                            0x6e050000, modem);
        best1700_btc_alias(s, &view->baseband, &core->space, &s->btc_baseband.iomem,
                            0x6d040000, baseband);
        best1700_btc_alias(s, &view->dma, &core->space, &s->btc_dma.iomem,
                            0x6d010000, dma);
        static const hwaddr ipc_base[BEST1700_BTC_IPC_BANKS] = {
            [BEST1700_BTC_IPC_BANK_SYS] = BEST1700_SYSCTRL_BASE,
            [BEST1700_BTC_IPC_BANK_BTC] = BEST1700_BTC_CMU_BASE,
            [BEST1700_BTC_IPC_BANK_BTH] = 0x50000000,
        };
        for (unsigned bank = 0; bank < BEST1700_BTC_IPC_BANKS; bank++) {
            g_autofree char *ipc = g_strdup_printf("%s.ipc%u", prefix, bank);

            memory_region_init_alias(&view->ipc[bank], OBJECT(s), ipc,
                &s->btc_ipc.bank[bank], 0, BEST1700_BTC_IPC_BANK_SIZE);
            memory_region_add_subregion_overlap(&core->space, ipc_base[bank],
                                                 &view->ipc[bank], 5);
        }
        if (i < BEST1700_CORE_COUNT) {
            best1700_aon_connect_endpoint(&core->aon_cmu, &endpoint);
        } else {
            best1700_btc_alias(s, &view->aon, &core->space,
                                &s->core[0].aon_cmu.region,
                                BEST1700_AON_CMU_BASE, "best1700.btc.aon");
        }
    }
    if (s->bth_real_boot) {
        memory_region_init_alias(&s->bth_power_view[BEST1700_CORE_COUNT],
            OBJECT(s), "best1700.btc.bth-power", &s->bth_power.iomem, 0,
            BEST1700_AON_WINDOW_SIZE);
        memory_region_add_subregion_overlap(&btc->space, BEST1700_AON_PSC_BASE,
            &s->bth_power_view[BEST1700_CORE_COUNT], 6);
    }
    object_initialize_child(OBJECT(s), "btc-core", &btc->cpu, TYPE_ARMV7M);
    device = DEVICE(&btc->cpu);
    qdev_prop_set_uint32(device, "num-irq", 64);
    qdev_prop_set_string(device, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));
    qdev_prop_set_bit(device, "start-powered-off", true);
    qdev_connect_clock_in(device, "cpuclk", s->sysclk);
    qdev_connect_clock_in(device, "refclk", s->refclk);
    object_property_set_link(OBJECT(&btc->cpu), "memory", OBJECT(&btc->space),
                             &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&btc->cpu), &error_fatal);
    static const unsigned cp_rx[] = { 104, 105, 43, 119 };
    static const unsigned cp_done[] = { 102, 103, 29, 30 };
    static const unsigned btc_sys_rx[] = { 19, 18, 36, 37 };
    static const unsigned btc_sys_done[] = { 17, 16, 34, 35 };
    static const unsigned btc_bth_rx[] = { 14, 13, 23, 24 };
    static const unsigned btc_bth_done[] = { 12, 11, 21, 22 };

    for (unsigned lane = 0; lane < BEST1700_BTC_IPC_LANES; lane++) {
        best1700_btc_ipc_connect_irq(&s->btc_ipc,
            BEST1700_BTC_IPC_SYS, lane,
            qdev_get_gpio_in(DEVICE(&s->core[1].cpu), cp_rx[lane]),
            qdev_get_gpio_in(DEVICE(&s->core[1].cpu), cp_done[lane]));
        best1700_btc_ipc_connect_irq(&s->btc_ipc,
            BEST1700_BTC_IPC_BTC_SYS, lane,
            qdev_get_gpio_in(device, btc_sys_rx[lane]),
            qdev_get_gpio_in(device, btc_sys_done[lane]));
        best1700_btc_ipc_connect_irq(&s->btc_ipc,
            BEST1700_BTC_IPC_BTC_BTH, lane,
            qdev_get_gpio_in(device, btc_bth_rx[lane]),
            qdev_get_gpio_in(device, btc_bth_done[lane]));
    }
    best1700_dma_set_irq(&s->btc_dma, qdev_get_gpio_in(device, 3));
    best1700_btc_cmu_set_irq(&s->btc_cmu, qdev_get_gpio_in(device, 6));
    best1700_btc_baseband_set_irq(&s->btc_baseband,
                                 qdev_get_gpio_in(device, 1));
    best1700_uart_set_pc(&s->btc_uart, &btc->cpu.cpu->env.regs[15]);
    best1700_btc_cmu_set_pc(&s->btc_cmu, &btc->cpu.cpu->env.regs[15]);
    best1700_btc_cpudump_set_pc(&s->btc_cpudump,
                                &btc->cpu.cpu->env.regs[15]);
    /* CP copies the embedded controller image through the shared bus and
     * publishes its own vectors before releasing reset. No host loading or
     * instruction patching participates in this startup. */
    armv7m_load_kernel(btc->cpu.cpu, NULL, 0, 0);
    best1700_core_reset(btc);
    btc->peer_reset_opaque = s;
    btc->peer_reset_done = best1700_btc_cpu_reset_done;
    best1700_btc_cmu_connect_reset(&s->btc_cmu, s, best1700_btc_cpu_reset);
    best1700_btc_power_connect(&s->btc_power, s, best1700_btc_power_changed);
    best1700_btc_cmu_connect_sleep(&s->btc_cmu, s, best1700_btc_power_sleep);
    best1700_btc_ipc_connect_wake(&s->btc_ipc, s, best1700_btc_ipc_wake);
}

static void best1700_bth_spi_clock(void *opaque, unsigned index, uint32_t hz)
{
    Best1700MachineState *s = opaque;

    g_assert(index < G_N_ELEMENTS(s->bth_spi));
    best1700_spi_set_clock_hz(&s->bth_spi[index], hz);
}

static void best1700_init(MachineState *machine)
{
    bes_runtime_config_lock();
    Best1700MachineState *s = MIBAND_BEST1700(machine);
    static const hwaddr i2c_base[BEST1700_I2C_COUNT] = {
        0x40006000, 0x40007000, 0x40075000, 0x40076000, 0x40077000,
    };
    static const hwaddr bth_i2c_base[BEST1700_I2C_COUNT] = {
        0x50006000, 0x50007000, 0x50008000, 0x5000a000, 0x5000d000,
    };
    if (s->btc_real_boot && !s->bth_real_boot) {
        error_setg(&error_fatal,
                   "btc-real-boot requires bth-real-boot for shared SRAM ownership");
    }
    /* Validate after all properties are set, independently of option order.
     * Every supported calibration interval must fit the 31-bit counter. */
    if (s->btc_cal_ref_hz < s->btc_lpo_hz ||
        (uint64_t)s->btc_cal_ref_hz * 256 / s->btc_lpo_hz > INT32_MAX) {
        error_setg(&error_fatal, "invalid BTC calibration clock ratio");
    }
    unsigned cpu_count = BEST1700_CORE_COUNT + s->btc_real_boot;

    if (machine->smp.cpus != cpu_count || machine->smp.max_cpus != cpu_count) {
        error_setg(&error_fatal, "this BEST1700 configuration requires -smp %u",
                   cpu_count);
    }
    if (best1700_doorbell_overlaps_aon(s->doorbell_base)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1700 doorbell base 0x%08" PRIx32
                      " overlaps AON; using 0x%08" PRIx32 "\n",
                      s->doorbell_base, BEST1700_DOORBELL_BASE);
        s->doorbell_base = BEST1700_DOORBELL_BASE;
    }
    memory_region_init_ram(&s->shared_storage, NULL, "best1700.ipc-shared",
                           BEST1700_SHARED_SIZE, &error_fatal);
    if (s->cp_real_boot) {
        best1700_cp_init(&s->cp_control);
        best1700_m55_ipc_init(&s->m55_ipc);
    }
    if (s->bth_real_boot) {
        best1700_bth_init(&s->bth, OBJECT(s));
        s->bth_cmu_reset = true;
        best1700_bth_power_init(&s->bth_power, OBJECT(s));
        s->bth_irq = qemu_allocate_irqs(best1700_bth_irq_input, s,
                                       G_N_ELEMENTS(s->bth_irq_level));
        best1700_sys_bth_ipc_init(&s->sys_bth_ipc, OBJECT(s));
        best1700_bth_memsc_init(&s->bth_memsc, OBJECT(s));
        best1700_bth_timing_init(&s->bth_timing, OBJECT(s),
                                 BEST1700_SLOW_TIMER_HZ, BEST1700_FAST_TIMER_HZ);
        best1700_bth_timing_set_wake_callback(&s->bth_timing,
                                             best1700_bth_wake_changed, s);
        for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_spi); i++) {
            g_autofree char *name = g_strdup_printf("best1700.bth-spi%u", i);

            best1700_spi_init(&s->bth_spi[i], OBJECT(s), name,
                               BEST1700_FAST_TIMER_HZ * 4);
        }
        best1700_bth_timing_set_spi_clock_callback(&s->bth_timing,
            BEST1700_FAST_TIMER_HZ * 4, best1700_bth_spi_clock, s);
        for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_cache); i++) {
            g_autofree char *name = g_strdup_printf("best1700.bth-cache%u", i);
            best1700_cache_init(&s->bth_cache[i], OBJECT(s), name);
        }
    }
    best1700_memsc_init(&s->sys_memsc, 8);
    best1700_memsc_init(&s->aon_memsc, 16);
    s->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(s->sysclk, 100000000);
    s->refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(s->refclk, 1000000);
    best1700_ispi_init(&s->ispi);
    s->display_bridge = miband_display_bridge_new(BEST1700_DISPLAY_WIDTH,
                                                   BEST1700_DISPLAY_HEIGHT);
    s->panel = miband_panel_new(0, BEST1700_DISPLAY_WIDTH,
                                BEST1700_DISPLAY_HEIGHT);
    g_assert(s->panel);
    miband_panel_connect_display(s->panel, s,
                                 best1700_panel_present);
    miband_panel_connect_te(s->panel, s, best1700_panel_te);
    s->charger_level = 80;
    s->charger_charging = false;
    for (unsigned i = 0; i < G_N_ELEMENTS(s->channels); i++) {
        s->channels[i].shared_base = s->shared_base + i * 0x10000;
        s->channels[i].doorbell = s->doorbell_base + i * 4;
        s->channels[i].vring_size = 256;
        s->channels[i].registered = true;
        s->channels[i].ready = true;
    }
    if (s->bth_real_boot) {
        s->channels[1].registered = false;
        s->channels[1].ready = false;
    }
    best1700_init_core(s, 0, "ap", s->image_flash_base[0],
                       s->image_code_offset[0],
                       s->boot_sp, s->boot_pc, true);
    best1700_ispi_create_bus(&s->ispi,
                            qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 23));
    best1700_flash_set_backing(&s->flash_controller,
        memory_region_get_ram_ptr(&s->core[0].flash), BEST1700_FLASH_SIZE);
    best1700_flash_set_changed(&s->flash_controller, best1700_nor_changed,
                               &s->core[0]);
    sysbus_realize(SYS_BUS_DEVICE(&s->flash_controller), &error_fatal);
    memory_region_add_subregion_overlap(&s->core[0].space,
        BEST1700_FLASH_CTRL_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->flash_controller), 0), 2);
    sysbus_realize(SYS_BUS_DEVICE(&s->psram_controller), &error_fatal);
    memory_region_add_subregion_overlap(&s->core[0].space,
        BEST1700_PSRAM_CTRL_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psram_controller), 0), 2);
    Best1700IspiEndpoint psram_analog = {
        .opaque = &s->psram_controller,
        .read = best1700_psram_ispi_read,
        .write = best1700_psram_ispi_write,
    };
    best1700_ispi_connect_endpoint(&s->ispi, 5, &psram_analog);
    for (unsigned i = 0; i < G_N_ELEMENTS(s->page_spy); i++) {
        SysBusDevice *spy = SYS_BUS_DEVICE(&s->page_spy[i]);
        sysbus_realize(spy, &error_fatal);
        memory_region_add_subregion_overlap(&s->core[0].space,
            best1700_page_spy_base[i], sysbus_mmio_get_region(spy, 0), 2);
    }
    SysBusDevice *tz_trigger = SYS_BUS_DEVICE(&s->tz_trigger);
    sysbus_realize(tz_trigger, &error_fatal);
    memory_region_add_subregion_overlap(&s->core[0].space, 0x400001e4,
        sysbus_mmio_get_region(tz_trigger, 0), 3);
    sysbus_connect_irq(tz_trigger, BEST1700_TZ_TRIGGER_IRQ_IND,
        qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 74));
    sysbus_connect_irq(tz_trigger, BEST1700_TZ_TRIGGER_IRQ_DONE,
        qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 75));
    memory_region_init_alias(&s->emmc_dma, OBJECT(s), "best1700.emmc-dma",
                             &s->core[0].space, 0, UINT64_MAX);
    object_property_set_link(OBJECT(&s->emmc), "dma", OBJECT(&s->emmc_dma),
                              &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&s->emmc), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&s->sec), &error_fatal);
    memory_region_add_subregion_overlap(&s->core[0].space, 0x40088000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->sec), 0), 2);
    qdev_connect_gpio_out_named(DEVICE(&s->sec), "emmc-nonsecure", 0,
        qdev_get_gpio_in_named(DEVICE(&s->emmc), "dma-nonsecure", 0));
    memory_region_add_subregion_overlap(&s->core[0].space, 0x40150000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->emmc), 0), 2);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->emmc), 0,
        qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 26));
    if (s->emmc_drive && *s->emmc_drive) {
        miband_emmc_attach(qdev_get_child_bus(DEVICE(&s->emmc.host), "sd-bus"),
                           s->emmc_drive, &error_fatal);
    }
    sysbus_realize(SYS_BUS_DEVICE(&s->gpio), &error_fatal);
    for (unsigned i = 0; i < G_N_ELEMENTS(best1700_gpio_mmio_base); i++) {
        memory_region_add_subregion_overlap(&s->core[0].space,
            best1700_gpio_mmio_base[i],
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->gpio), i), 2);
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio), 0,
        qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 2));
    Best1700DsiPanelEndpoint panel = {
        .opaque = s,
        .read = best1700_panel_read,
        .write = best1700_panel_write,
    };
    sysbus_realize(SYS_BUS_DEVICE(&s->dsi), &error_fatal);
    best1700_dsi_connect_panel(&s->dsi, &panel);
    memory_region_add_subregion_overlap(&s->core[0].space, BEST1700_DSI_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dsi), 0), 2);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dsi), 0,
        qdev_get_gpio_in(DEVICE(&s->core[0].cpu), BEST1700_DSI_IRQ));
    for (unsigned i = 0; i < BEST1700_I2C_COUNT; i++) {
        SysBusDevice *controller = SYS_BUS_DEVICE(&s->i2c[i]);
        DEVICE(controller)->id = g_strdup_printf("best1700-i2c%u", i);
        sysbus_realize(controller, &error_fatal);
        memory_region_add_subregion_overlap(&s->core[0].space, i2c_base[i],
                                             sysbus_mmio_get_region(controller, 0),
                                             2);
        sysbus_connect_irq(controller, 0,
                           qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 50 + i));
    }
    miband_lcdc_init(&s->lcdc, miband_panel_endpoint(s->panel),
                     32, BEST1700_DISPLAY_WIDTH, BEST1700_DISPLAY_HEIGHT,
                     memory_region_get_ram_ptr(&s->core[0].psram),
                     BEST1700_PSRAM_SIZE,
                     memory_region_get_ram_ptr(&s->core[0].ram),
                     BEST1700_RAM_SIZE);
    miband_lcdc_set_base(&s->lcdc, BEST1700_LCDC_BASE);
    miband_lcdc_set_memory(&s->lcdc, s, best1700_display_memory);
    miband_lcdc_connect_irq(&s->lcdc,
                            qdev_get_gpio_in(DEVICE(&s->core[0].cpu),
                                             BEST1700_LCDC_IRQ));
    memory_region_init_io(&s->lcdc_region, NULL, &best1700_lcdc_ops, s,
                          "best1700.lcdc", BEST1700_LCDC_SIZE);
    memory_region_add_subregion(&s->core[0].space, BEST1700_LCDC_BASE,
                                &s->lcdc_region);
    /* The shared GC NanoUltraV engine executes guest command buffers and
     * writes guest RAM. BEST1700 owns its identity, bus mapping and IRQ. */
    miband_gpu_init(&s->gpu, NULL, 0, NULL, 0, false, true);
    miband_gpu_set_bus(&s->gpu, BEST1700_GPU_BASE, 0x265, 0x2002, 0x420, 0,
                       s, best1700_gpu_memory);
    miband_gpu_connect_irq(&s->gpu,
                          qdev_get_gpio_in(DEVICE(&s->core[0].cpu),
                                           BEST1700_GPU_IRQ));
    memory_region_init_io(&s->gpu_region, OBJECT(s), &best1700_gpu_ops, s,
                          "best1700.gpu", BEST1700_GPU_SIZE);
    memory_region_add_subregion_overlap(&s->core[0].space, BEST1700_GPU_BASE,
                                        &s->gpu_region, 1);
    best1700_init_core(s, 1, "cp", s->image_flash_base[1],
                       s->image_code_offset[1],
                       BEST1700_RAM_BASE + BEST1700_RAM_SIZE - 16,
                       BEST1700_STUB_PC, false);
    if (s->cp_real_boot) {
        best1700_connect_cp_memory(s);
    }
    best1700_init_core(s, 2, "bth", s->image_flash_base[2],
                       s->image_code_offset[2],
                       BEST1700_RAM_BASE + BEST1700_RAM_SIZE - 16,
                       BEST1700_STUB_PC, false);
    if (s->bth_real_boot) {
        /*
         * BTH owns two DMA instances. Their register blocks share the
         * eight-channel register layout. The driver's allocation limits
         * and the six/four timestamp capture inputs are not register-window
         * limits: original initialization clears all eight CONFIG words.
         */
        best1700_dma_init(&s->bth_dma[0], OBJECT(s), "best1700.bth-audma",
                          6, &s->core[2].address_space);
        best1700_dma_init(&s->bth_dma[1], OBJECT(s), "best1700.bth-gpdma",
                          4, &s->core[2].address_space);
        best1700_dma_connect_terminal(&s->bth_dma[0], s,
                                      best1700_bth_audma_terminal);
        best1700_dma_connect_terminal(&s->bth_dma[1], s,
                                      best1700_bth_gpdma_terminal);
        memory_region_add_subregion_overlap(&s->core[2].space, 0x50170000,
                                            &s->bth_dma[0].iomem, 5);
        memory_region_add_subregion_overlap(&s->core[2].space, 0x50130000,
                                            &s->bth_dma[1].iomem, 5);
        best1700_dma_set_irq(&s->bth_dma[0],
                             s->bth_irq[30]);
        best1700_dma_set_irq(&s->bth_dma[1],
                             s->bth_irq[29]);
    }
    best1700_rf_matrix_init(&s->rf_matrix, OBJECT(s));
    for (unsigned i = 0; i < BEST1700_CORE_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("best1700.%s.rf-matrix",
                                               s->core[i].name);
        memory_region_init_alias(&s->rf_matrix_view[i], OBJECT(s), name,
            &s->rf_matrix.iomem, 0, BEST1700_RF_MATRIX_SIZE);
        memory_region_add_subregion(&s->core[i].space,
            BEST1700_RF_MATRIX_BASE, &s->rf_matrix_view[i]);
    }
    best1700_flash_set_backing(&s->bth_flash_controller,
        memory_region_get_ram_ptr(&s->core[2].flash), BEST1700_FLASH_SIZE);
    best1700_flash_set_changed(&s->bth_flash_controller,
                               best1700_nor_changed, &s->core[2]);
    sysbus_realize(SYS_BUS_DEVICE(&s->bth_flash_controller), &error_fatal);
    for (unsigned i = 0; i < BEST1700_I2C_COUNT; i++) {
        SysBusDevice *controller = SYS_BUS_DEVICE(&s->bth_i2c[i]);

        DEVICE(controller)->id = g_strdup_printf("best1700-bth-i2c%u", i);
        sysbus_realize(controller, &error_fatal);
        if (s->bth_real_boot) {
            for (unsigned index = 0; index < BEST1700_CORE_COUNT; index++) {
                Best1700Core *core = &s->core[index];
                g_autofree char *name = g_strdup_printf(
                    "best1700.%s.bth-i2c%u", core->name, i);

                memory_region_init_alias(&core->bth_i2c[i], OBJECT(s), name,
                    sysbus_mmio_get_region(controller, 0), 0,
                    BEST1700_I2C_MMIO_SIZE);
                memory_region_add_subregion_overlap(&core->space,
                    bth_i2c_base[i], &core->bth_i2c[i], 4);
            }
            sysbus_connect_irq(controller, 0,
                s->bth_irq[39 + i]);
        }
    }
    if (s->bth_real_boot) {
        for (unsigned index = 0; index < BEST1700_CORE_COUNT; index++) {
            Best1700Core *core = &s->core[index];
            g_autofree char *flash_name = g_strdup_printf(
                "best1700.%s.bth-flash-controller", core->name);

            memory_region_init_alias(&core->bth_flash_controller_view, OBJECT(s),
                flash_name,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->bth_flash_controller), 0),
                0, BEST1700_FLASH_MMIO_SIZE);
            memory_region_add_subregion_overlap(&core->space, 0x50140000,
                &core->bth_flash_controller_view, 4);

            for (unsigned side = 0; side < 2; side++) {
                g_autofree char *name = g_strdup_printf(
                    "best1700.%s.ipc-bank%u", core->name, side);

                memory_region_init_alias(&core->sys_bth_ipc_bank[side],
                    OBJECT(s), name, &s->sys_bth_ipc.bank[side], 0, 0x1000);
                memory_region_add_subregion_overlap(&core->space,
                    side == BEST1700_SYS_BTH_SYS ? 0x40000000 : 0x50000000,
                    &core->sys_bth_ipc_bank[side], 4);
            }
        }
        for (unsigned lane = 0; lane < 2; lane++) {
            best1700_sys_bth_ipc_connect_irq(&s->sys_bth_ipc,
                BEST1700_SYS_BTH_SYS, lane,
                qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 96 + lane),
                qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 94 + lane));
            best1700_sys_bth_ipc_connect_irq(&s->sys_bth_ipc,
                BEST1700_SYS_BTH_BTH, lane,
                s->bth_irq[73 + lane],
                s->bth_irq[65 + lane]);
            if (s->cp_real_boot) {
                best1700_sys_bth_ipc_connect_irq(&s->sys_bth_ipc,
                    BEST1700_SYS_BTH_SYS, lane + 2,
                    qdev_get_gpio_in(DEVICE(&s->core[1].cpu), 96 + lane),
                    qdev_get_gpio_in(DEVICE(&s->core[1].cpu), 94 + lane));
                best1700_sys_bth_ipc_connect_irq(&s->sys_bth_ipc,
                    BEST1700_SYS_BTH_BTH, lane + 2,
                    s->bth_irq[75 + lane],
                    s->bth_irq[67 + lane]);
            }
        }
        best1700_bth_connect_reset(&s->bth, s, best1700_bth_set_reset);
        best1700_bth_power_connect(&s->bth_power, s, best1700_bth_power_changed);
        best1700_bth_power_connect_lpu(&s->bth_power, s, best1700_bth_lpu_completed);
        s->core[2].peer_reset_done = best1700_bth_cpu_reset_done;
        s->core[2].peer_reset_opaque = s;
        for (unsigned i = 0; i < BEST1700_CORE_COUNT; i++) {
            memory_region_init_alias(&s->bth_power_view[i], OBJECT(s),
                "best1700.bth-power", &s->bth_power.iomem, 0,
                BEST1700_AON_WINDOW_SIZE);
            memory_region_add_subregion_overlap(&s->core[i].space,
                BEST1700_AON_PSC_BASE, &s->bth_power_view[i], 6);
        }
    }
    if (s->cp_real_boot) {
        Best1700AonEndpoint endpoint = best1700_cp_endpoint(&s->cp_control);

        for (unsigned i = 0; i < BEST1700_CORE_COUNT; i++) {
            s->core[i].m55_ipc = &s->m55_ipc;
            best1700_aon_connect_endpoint(&s->core[i].aon_cmu, &endpoint);
        }
        for (unsigned side = 0; side < BEST1700_M55_IPC_SIDES; side++) {
            for (unsigned channel = 0; channel < BEST1700_M55_IPC_CHANNELS;
                 channel++) {
                best1700_m55_ipc_connect_irq(&s->m55_ipc, side, channel,
                    qdev_get_gpio_in(DEVICE(&s->core[side].cpu), 88 + channel),
                    qdev_get_gpio_in(DEVICE(&s->core[side].cpu), 91 + channel));
            }
        }
        best1700_cp_connect_reset(&s->cp_control, &s->core[1],
                                   best1700_cp_set_control);
        s->channels[0].registered = false;
        s->channels[0].ready = false;
    }
    /* AP inspects the sensor boot header over the BTH flash bus. These are
     * views of the peer's storage, not copies in the AP NOR package. Keep
     * the legacy AP 0x08000000 compatibility view separate for now. */
    static const hwaddr bth_flash_base[] = {
        BEST1700_BTH_FLASH_NC_BASE, BEST1700_BTH_FLASH_BASE,
        BEST1700_BTH_FLASHX_BASE,
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(bth_flash_base); i++) {
        char name[48];

        g_snprintf(name, sizeof(name), "best1700.ap.bth-flash[%u]", i);
        memory_region_init_alias(&s->bth_flash_view[i], OBJECT(s), name,
                                  &s->core[2].flash, 0, BEST1700_FLASH_SIZE);
        memory_region_add_subregion(&s->core[0].space, bth_flash_base[i],
                                    &s->bth_flash_view[i]);
    }
    s->hifi4 = best1700_hifi4_new(&s->core[0].address_space,
        s->audio_endpoint_profile, s->audio_image,
        qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 76),
        qdev_get_gpio_in(DEVICE(&s->core[0].cpu), 79), &error_fatal);
    s->core[0].hifi4 = s->hifi4;
    best1700_dsp_reset(s);
    if (s->hifi4) {
        Best1700AonEndpoint endpoint = {
            .opaque = s,
            .read = best1700_dsp_aon_read,
            .write = best1700_dsp_aon_write,
        };
        for (unsigned i = 0; i < BEST1700_CORE_COUNT; i++) {
            best1700_aon_connect_endpoint(&s->core[i].aon_cmu, &endpoint);
        }
    }
    if (s->btc_real_boot) {
        best1700_init_btc(s);
    }
    /* Host physical-memory clients use the AP bus view.  Peer CPUs retain
     * their independent address spaces and only share explicit windows. */
    memory_region_init_alias(&s->system_view, OBJECT(s), "best1700.ap-system",
                             &s->core[0].space, 0, UINT64_MAX);
    memory_region_add_subregion(get_system_memory(), 0, &s->system_view);
    s->workers_started = true;
    qemu_register_reset(best1700_reset, s);
    s->machine_done.notify = best1700_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void best1700_instance_init(Object *obj)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);
    object_initialize_child(obj, "dsi", &s->dsi, TYPE_BEST1700_DSI);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_BEST1700_GPIO);
    object_initialize_child(obj, "emmc", &s->emmc, TYPE_BEST1700_SDHCI);
    object_initialize_child(obj, "sec", &s->sec, TYPE_BEST1700_SEC);
    object_initialize_child(obj, "flash-controller", &s->flash_controller,
                              TYPE_BEST1700_FLASH);
    object_initialize_child(obj, "bth-flash-controller", &s->bth_flash_controller,
                              TYPE_BEST1700_FLASH);
    object_initialize_child(obj, "psram-controller", &s->psram_controller,
                             TYPE_BEST1700_PSRAM);
    object_initialize_child(obj, "tz-trigger", &s->tz_trigger,
                             TYPE_BEST1700_TZ_TRIGGER);
    for (unsigned i = 0; i < G_N_ELEMENTS(s->page_spy); i++) {
        char name[24];
        g_snprintf(name, sizeof(name), "page-spy[%u]", i);
        object_initialize_child(obj, name, &s->page_spy[i], TYPE_BEST1700_PAGE_SPY);
    }
    static const char * const nor_properties[] = {
        "die-count", "jedec-id", "uid0", "uid1",
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(nor_properties); i++) {
        g_autofree char *name = g_strdup_printf("nor-%s", nor_properties[i]);
        object_property_add_alias(obj, name, OBJECT(&s->flash_controller),
                                   nor_properties[i]);
        g_autofree char *bth_name = g_strdup_printf("bth-nor-%s", nor_properties[i]);
        object_property_add_alias(obj, bth_name, OBJECT(&s->bth_flash_controller),
                                   nor_properties[i]);
    }
    s->panel_te_pin = UINT32_MAX;
    /* Configurable simulation defaults, not measured board oscillators. */
    s->btc_cal_ref_hz = 48000000;
    s->btc_lpo_hz = 32000;
    s->btc_correction_ns = BEST1700_BTC_DEFAULT_CORRECTION_NS;
    s->btc_cpudump_sample_ns = BEST1700_BTC_CPUDUMP_DEFAULT_SAMPLE_NS;
    /* Child devices must be composed while the machine object is being
     * initialized.  Initializing ARMv7M children from machine->init leaves
     * them as plain Objects during the first DEVICE() cast on this QEMU
     * branch. */
    for (unsigned i = 0; i < BEST1700_CORE_COUNT; i++) {
        char name[16];
        g_snprintf(name, sizeof(name), "core[%u]", i);
        object_initialize_child(obj, name, &s->core[i].cpu, TYPE_ARMV7M);
    }
    for (unsigned i = 0; i < BEST1700_I2C_COUNT; i++) {
        char name[16];
        g_snprintf(name, sizeof(name), "i2c[%u]", i);
        object_initialize_child(obj, name, &s->i2c[i], TYPE_BEST1700_I2C);
        g_snprintf(name, sizeof(name), "bth-i2c[%u]", i);
        object_initialize_child(obj, name, &s->bth_i2c[i], TYPE_BEST1700_I2C);
    }
    s->boot_sp = BEST1700_RAM_BASE + BEST1700_RAM_SIZE - 16;
    s->boot_pc = BEST1700_AP_BASE + 0x1c0000u + 1;
    s->boot_rom_size = 0x20000u;
    s->image_flash_base[0] = BEST1700_AP_BASE;
    s->image_flash_base[1] = 0x10980000u;
    s->image_flash_base[2] = 0x2c000000u;
    s->image_flash_base[3] = 0x100c0000u;
    s->image_code_offset[0] = 0x1c0000u;
    s->image_code_offset[1] = 0x980000u;
    s->image_code_offset[2] = 0;
    s->image_code_offset[3] = 0xc0000u;
    s->image_entry[0] = s->boot_pc;
    s->image_entry[1] = 0x10980001u;
    s->image_entry[2] = 0x2c000001u;
    s->image_entry[3] = 0x100c0001u;
    s->shared_base = BEST1700_SHARED_BASE;
    s->doorbell_base = BEST1700_DOORBELL_BASE;
    s->profile_id = g_strdup("synthetic");
}

static void best1700_idau_check(IDAUInterface *ii, uint32_t address,
                               int *iregion, bool *exempt, bool *ns, bool *nsc)
{
    Best1700MachineState *s = MIBAND_BEST1700(ii);
    uint64_t flash_base = s->core[0].flash_base;
    uint64_t normalized = address;
    bool flash_alias = false;

    if (address >= 0x08000000 && address < 0x08000000 + BEST1700_FLASH_SIZE) {
        normalized = flash_base + (address - 0x08000000);
        flash_alias = true;
    } else if (address >= flash_base + 0x20000000 &&
               address < flash_base + 0x20000000 + BEST1700_FLASH_SIZE) {
        normalized = address - 0x20000000;
        flash_alias = true;
    }
    /* The explicit secure TCM/peripheral aliases retain secure attribution
     * even where software declares the encompassing address range nonsecure.
     * Other addresses remain governed by the CPU's guest-programmed SAU. */
    bool secure_alias = (address >= 0x04100000 && address < 0x04200000) ||
                        (address >= 0x24000000 && address < 0x24400000) ||
                        (address >= 0x44000000 && address < 0x44400000);
    /* A broad SAU region over an alternate flash view must not expose the
     * secure image slot. Keep the NS AP slot accessible through those views.
     * Only the primary view supplies NSC gateways under guest SAU control.
     * The loader has validated that this slot ends before the AP image. */
    if (s->secure_boot && flash_alias &&
        normalized >= (uint64_t)s->image_flash_base[3] + s->image_code_offset[3] &&
        normalized < flash_base + s->core[0].code_offset) {
        secure_alias = true;
    }
    *iregion = address >> 26;
    *exempt = (address & 0xeff00000) == 0xe0000000;
    *ns = !secure_alias;
    *nsc = !secure_alias;
}

static void best1700_machine_class_init(ObjectClass *oc, void *data)
{
    bes_runtime_config_property(oc);
    MachineClass *mc = MACHINE_CLASS(oc);
    IDAUInterfaceClass *iic = IDAU_INTERFACE_CLASS(oc);
    iic->check = best1700_idau_check;
    mc->desc = "Configurable BEST1700 multi-core execution scaffold";
    mc->init = best1700_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m55");
    mc->default_ram_size = BEST1700_RAM_SIZE;
    mc->min_cpus = BEST1700_CORE_COUNT;
    mc->default_cpus = BEST1700_CORE_COUNT;
    mc->max_cpus = BEST1700_CORE_COUNT + 1;
    object_class_property_add(oc, "boot-sp", "uint32",
                              best1700_get_boot_vector, best1700_set_boot_vector,
                              NULL, NULL);
    object_class_property_add(oc, "boot-pc", "uint32",
                              best1700_get_boot_vector, best1700_set_boot_vector,
                              NULL, NULL);
    object_class_property_add(oc, "boot-rom-size", "uint32",
                              best1700_get_boot_vector, best1700_set_boot_vector,
                              NULL, NULL);
    static const char * const layout_properties[] = {
        "ap-flash-base", "ap-code-offset", "ap-entry",
        "cp-flash-base", "cp-code-offset", "cp-entry",
        "bth-flash-base", "bth-code-offset", "bth-entry",
        "tee-flash-base", "tee-code-offset", "tee-entry",
        "shared-base", "doorbell-base",
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(layout_properties); i++) {
        object_class_property_add(oc, layout_properties[i], "uint32",
                                  best1700_get_image_property,
                                  best1700_set_image_property, NULL, NULL);
    }
    object_class_property_add_str(oc, "profile-id", best1700_get_profile,
                                  best1700_set_profile);
    object_class_property_add_str(oc, "emmc-drive", best1700_get_emmc_drive,
                                  best1700_set_emmc_drive);
    object_class_property_add_bool(oc, "secure-boot", best1700_get_secure_boot,
                                   best1700_set_secure_boot);
    object_class_property_add_bool(oc, "bth-real-boot", best1700_get_bth_real_boot,
                                   best1700_set_bth_real_boot);
    object_class_property_add_bool(oc, "btc-real-boot", best1700_get_btc_real_boot,
                                   best1700_set_btc_real_boot);
    object_class_property_add(oc, "btc-cal-ref-hz", "uint32",
                              best1700_get_btc_frequency,
                              best1700_set_btc_frequency, NULL, NULL);
    object_class_property_add(oc, "btc-lpo-hz", "uint32",
                              best1700_get_btc_frequency,
                              best1700_set_btc_frequency, NULL, NULL);
    object_class_property_add(oc, "btc-cpudump-sample-ns", "uint32",
                              best1700_get_btc_cpudump_sample_ns,
                              best1700_set_btc_cpudump_sample_ns, NULL, NULL);
    object_class_property_add(oc, "btc-correction-ns", "uint32",
                              best1700_get_btc_correction_ns,
                              best1700_set_btc_correction_ns, NULL, NULL);
    object_class_property_add_str(oc, "bth-status", best1700_get_bth_status, NULL);
    object_class_property_add_bool(oc, "cp-real-boot", best1700_get_cp_real_boot,
                                   best1700_set_cp_real_boot);
    object_class_property_add_str(oc, "cp-status", best1700_get_cp_status, NULL);
    object_class_property_add_str(oc, "panel-read-responses",
                                  best1700_get_panel_reads,
                                  best1700_set_panel_reads);
    object_class_property_add(oc, "panel-te-pin", "uint32",
                              best1700_get_panel_te_pin,
                              best1700_set_panel_te_pin, NULL, NULL);
    object_class_property_add_str(oc, "cp-image", best1700_get_cp_image,
                                  best1700_set_cp_image);
    object_class_property_add_str(oc, "bth-image", best1700_get_bth_image,
                                  best1700_set_bth_image);
    object_class_property_add_str(oc, "tee-image", best1700_get_tee_image,
                                  best1700_set_tee_image);
    object_class_property_add_str(oc, "audio-image", best1700_get_audio_image,
                                  best1700_set_audio_image);
    object_class_property_add_str(oc, "audio-endpoint-profile",
                                  best1700_get_audio_profile,
                                  best1700_set_audio_profile);
    object_class_property_add_str(oc, "audio-status", best1700_get_audio_status,
                                  NULL);
    object_class_property_add_str(oc, "core-status", best1700_get_core_status, NULL);
    object_class_property_add_str(oc, "ipc-status", best1700_get_ipc_status, NULL);
    object_class_property_add(oc, "display-frame", "uint32",
                              best1700_get_display_frame, NULL, NULL, NULL);
    object_class_property_add(oc, "charger-state", "MiBandChargerState",
                              best1700_get_charger_state,
                              best1700_set_charger_state, NULL, NULL);
    object_class_property_add_bool(oc, "ipc-reset", best1700_get_ipc_reset,
                                   best1700_set_ipc_reset);
}

static void best1700_instance_finalize(Object *obj)
{
    Best1700MachineState *s = MIBAND_BEST1700(obj);

    best1700_hifi4_free(s->hifi4);
    miband_gpu_cleanup(&s->gpu);
    best1700_btc_baseband_destroy(&s->btc_baseband);
    best1700_btc_cmu_destroy(&s->btc_cmu);
    if (s->btc_dma.timer) {
        best1700_dma_destroy(&s->btc_dma);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_dma); i++) {
        if (s->bth_dma[i].timer) {
            best1700_dma_destroy(&s->bth_dma[i]);
        }
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->bth_spi); i++) {
        if (s->bth_spi[i].timer) {
            best1700_spi_destroy(&s->bth_spi[i]);
        }
    }
    if (s->bth_real_boot) {
        best1700_bth_power_destroy(&s->bth_power);
        qemu_free_irqs(s->bth_irq, G_N_ELEMENTS(s->bth_irq_level));
    }
    g_free(s->audio_endpoint_profile);
}

static const TypeInfo best1700_machine_type = {
    .name = TYPE_MIBAND_BEST1700,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Best1700MachineState),
    .instance_init = best1700_instance_init,
    .instance_finalize = best1700_instance_finalize,
    .class_init = best1700_machine_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_IDAU_INTERFACE },
        { }
    },
};

static void best1700_machine_register_types(void)
{
    type_register_static(&best1700_machine_type);
}

type_init(best1700_machine_register_types)
