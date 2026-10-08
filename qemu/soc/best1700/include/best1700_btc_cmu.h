/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTC_CMU_H
#define BEST1700_BTC_CMU_H

#include "best1700_aon.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define BEST1700_BTC_CMU_BASE 0x6e030000u
#define BEST1700_BTC_CMU_WINDOW 0x1000u

typedef struct Best1700BtcCmu {
    MemoryRegion iomem;
    uint32_t clocks;
    uint32_t reset_n;
    uint32_t clock_mode;
    uint32_t mode_mclk;
    uint32_t target_addr;
    uint32_t ram_timing;
    bool write_unlocked;
    uint32_t div_timer;
    uint32_t div_wdt;
    uint32_t sleep_config;
    bool mac_sleep_ready;
    uint32_t cpu_cfg;
    uint32_t rf_mem_timing;
    uint32_t clock_select;
    uint32_t wakeup_mask;
    uint32_t wakeup_mask_ext;
    uint32_t idle_request;
    uint32_t clk_out;
    uint32_t sel_sys_gt;
    uint32_t mresetn_mode;
    uint32_t mresetn_rxtx;
    uint32_t mresetn_state;
    uint32_t mclk_enable;
    uint32_t mresetn_mode2;
    uint32_t mresetn_rxtx2;
    uint32_t mresetn_state2;
    uint32_t mclk_enable2;
    uint32_t mclk_mode2;
    uint32_t irq_masked_ext;
    uint32_t bt_pc;
    const uint32_t *cpu_pc;
    uint32_t wifi_irq_mask;
    uint32_t irq_masked;
    uint32_t ext_clocks;
    uint32_t ext_clock_mode;
    uint32_t ext_reset_n;
    uint32_t pvt_cfg;
    uint32_t pvt_count;
    bool pvt_busy;
    QEMUTimer *pvt_timer;
    uint32_t reference_hz;
    uint32_t lpo_hz;
    uint32_t calibration_count;
    uint32_t calibration_result;
    bool calibration_busy;
    bool calibration_pending;
    QEMUTimer *calibration_timer;
    qemu_irq calibration_irq;
    uint32_t aon_reset_asserted;
    uint32_t vtor;
    void *reset_opaque;
    void (*set_reset)(void *opaque, bool asserted, uint32_t vtor);
    void *sleep_opaque;
    void (*request_sleep)(void *opaque);
} Best1700BtcCmu;

/* Every participating bus maps the same iomem, not a private register bank. */
void best1700_btc_cmu_init(Best1700BtcCmu *s, Object *owner,
                         uint32_t reference_hz, uint32_t lpo_hz);
void best1700_btc_cmu_set_irq(Best1700BtcCmu *s, qemu_irq irq);
void best1700_btc_cmu_destroy(Best1700BtcCmu *s);
void best1700_btc_cmu_set_mac_sleep(void *opaque, bool ready);
void best1700_btc_cmu_reset(Best1700BtcCmu *s);
void best1700_btc_cmu_connect_reset(Best1700BtcCmu *s, void *opaque,
    void (*set_reset)(void *opaque, bool asserted, uint32_t vtor));
void best1700_btc_cmu_connect_sleep(Best1700BtcCmu *s, void *opaque,
                                   void (*request)(void *opaque));
void best1700_btc_cmu_set_pc(Best1700BtcCmu *s, const uint32_t *pc);
Best1700AonEndpoint best1700_btc_cmu_endpoint(Best1700BtcCmu *s);
bool best1700_btc_cmu_cpu_held(const Best1700BtcCmu *s);

#endif
