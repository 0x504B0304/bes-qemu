/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTC_BASEBAND_H
#define BEST1700_BTC_BASEBAND_H

#include "exec/memory.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define BEST1700_BTC_BASEBAND_SIZE 0x1000
#define BEST1700_BTC_DEFAULT_CORRECTION_NS 312500u

typedef enum Best1700BtcSleepState {
    BEST1700_BTC_AWAKE,
    BEST1700_BTC_SLEEPING,
    BEST1700_BTC_NEEDS_CORRECTION,
    BEST1700_BTC_CORRECTING,
} Best1700BtcSleepState;

typedef struct Best1700BtcTimestampTarget {
    struct Best1700BtcBaseband *owner;
    QEMUTimer *timer;
    uint32_t slot;
    uint32_t fine;
    uint8_t written;
    bool enabled;
    unsigned index;
} Best1700BtcTimestampTarget;

typedef struct Best1700BtcBaseband {
    MemoryRegion iomem;
    uint32_t config[BEST1700_BTC_BASEBAND_SIZE / sizeof(uint32_t)];
    uint32_t irq_enable;
    uint32_t irq_status;
    uint32_t bt_irq_status;
    uint32_t bt_error;
    uint32_t ble_irq_status;
    uint32_t ble_error;
    /* Software-trigger synchronization interrupt latch at +d28 bit 31. */
    uint32_t sync_interrupt;
    /* Number of accepted MAC context-restore command pulses at +d00.
     * This is diagnostic state only; the register itself is self-clearing. */
    uint32_t mac_restore_count;
    uint32_t ble_local_seed;
    uint32_t ble_peer_seed;
    MemoryRegion *exchange;
    uint32_t ble_abort_pending;
    QEMUTimer *ble_abort_timer;
    int64_t clock_epoch_ns;
    uint64_t native_base_hus;
    uint64_t iso_base_hus;
    uint32_t sampled_slot;
    uint32_t sampled_fine;
    uint32_t sampled_iso;
    /* DMA/GPIO terminal-count capture latches.  The silicon exposes eight
     * clock/fine pairs; they remain zero until a connected capture source
     * supplies an edge. */
    uint32_t capture_clkn[8];
    uint32_t capture_fine[8];
    qemu_irq irq;
    Best1700BtcTimestampTarget targets[3];
    Best1700BtcTimestampTarget sw_targets[6];
    uint32_t lpo_hz;
    /* Startup model latency; retained across device reset. */
    uint32_t correction_latency_ns;
    uint32_t sleep_control;
    uint32_t sleep_cycles;
    uint32_t sleep_elapsed;
    int64_t sleep_epoch_ns;
    bool external_wake;
    uint32_t correction_slot;
    uint32_t correction_fine;
    uint32_t correction_iso_us;
    uint32_t correction_iso_hus;
    uint32_t iso_control;
    uint8_t correction_written;
    Best1700BtcSleepState sleep_state;
    QEMUTimer *sleep_timer;
    QEMUTimer *correction_timer;
    void *sleep_sync_opaque;
    void (*sleep_sync)(void *opaque, bool ready);
} Best1700BtcBaseband;

void best1700_btc_baseband_init(Best1700BtcBaseband *s, Object *owner,
                              uint32_t lpo_hz, uint32_t correction_latency_ns);
void best1700_btc_baseband_reset(Best1700BtcBaseband *s);
void best1700_btc_baseband_set_irq(Best1700BtcBaseband *s, qemu_irq irq);
void best1700_btc_baseband_destroy(Best1700BtcBaseband *s);
void best1700_btc_baseband_connect_sleep(Best1700BtcBaseband *s, void *opaque,
                                       void (*sync)(void *, bool));
void best1700_btc_baseband_set_external_wake(Best1700BtcBaseband *s, bool level);
void best1700_btc_baseband_connect_exchange(Best1700BtcBaseband *s,
                                          MemoryRegion *exchange);
void best1700_btc_baseband_dma_terminal(void *opaque, unsigned channel);
void best1700_btc_baseband_dma_terminal_source(Best1700BtcBaseband *s,
                                              unsigned remapped_channel,
                                              bool bth_dma);

#endif
