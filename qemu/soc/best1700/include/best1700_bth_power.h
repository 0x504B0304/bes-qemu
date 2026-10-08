/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTH_POWER_H
#define BEST1700_BTH_POWER_H

#include "exec/memory.h"
#include "qemu/timer.h"

#define BEST1700_BTH_POWER_BASE 0x40085000u
#define BEST1700_BTH_POWER_WINDOW 0x1000u
#define BEST1700_BTH_POWER_REGISTERS 13
#define BEST1700_BTH_POWER_GPIO_BANKS 3

#define BEST1700_BTH_WAKE_GPIO (1u << 0)
#define BEST1700_BTH_WAKE_TIMER20 (1u << 5)
#define BEST1700_BTH_WAKE_TIMER21 (1u << 6)
#define BEST1700_BTH_WAKE_CORE (1u << 16)
#define BEST1700_BTH_WAKE_MASK 0x0fffffffu

typedef struct Best1700BthPower Best1700BthPower;

typedef struct Best1700BthPowerRegister {
    MemoryRegion iomem;
    Best1700BthPower *power;
    hwaddr offset;
} Best1700BthPowerRegister;

/* Inputs describe retained hardware levels, never mailbox completions. */
typedef void (*Best1700BthPowerControlFn)(void *opaque, bool off,
                                       bool reset_asserted,
                                       bool clock_stopped);
/* False on automatic sleep acceptance; true only after its wake completes. */
typedef void (*Best1700BthPowerLpuFn)(void *opaque, bool awake);

struct Best1700BthPower {
    /* One sparse PSC bank shared by every participating bus master. */
    MemoryRegion iomem;
    Best1700BthPowerRegister registers[BEST1700_BTH_POWER_REGISTERS];
    QEMUTimer *transition_timer;
    uint32_t auto_control;
    uint32_t manual_control;
    uint32_t wakeup_mask;
    uint32_t wake_sources;
    uint32_t gpio_mask[BEST1700_BTH_POWER_GPIO_BANKS];
    uint32_t gpio_sources[BEST1700_BTH_POWER_GPIO_BANKS];
    unsigned main_state;
    bool automatic_off;
    bool automatic_cycle;
    bool wake_latched;
    bool off;
    bool reset_asserted;
    bool clock_stopped;
    bool isolated;
    bool memory_off;
    void *control_opaque;
    Best1700BthPowerControlFn set_control;
    void *lpu_opaque;
    Best1700BthPowerLpuFn set_lpu;
};

void best1700_bth_power_init(Best1700BthPower *s, Object *owner);
void best1700_bth_power_destroy(Best1700BthPower *s);
/* Machine reset owns CPU/NVIC and LPU ordering; no callbacks are emitted. */
void best1700_bth_power_reset(Best1700BthPower *s);
void best1700_bth_power_connect(Best1700BthPower *s, void *opaque,
                              Best1700BthPowerControlFn control);
/* Connecting does not report a completed low-power cycle. */
void best1700_bth_power_connect_lpu(Best1700BthPower *s, void *opaque,
                                  Best1700BthPowerLpuFn callback);
/* True means a hardware sleep request was accepted; transition is deferred. */
bool best1700_bth_power_request_sleep(Best1700BthPower *s);
/* Set raw wake-source levels. The controller applies its retained masks. */
void best1700_bth_power_set_wake(Best1700BthPower *s, uint32_t sources);
void best1700_bth_power_set_gpio_wake(Best1700BthPower *s, unsigned bank,
                                    uint32_t sources);
bool best1700_bth_power_is_off(const Best1700BthPower *s);
bool best1700_bth_power_is_reset_asserted(const Best1700BthPower *s);
bool best1700_bth_power_is_clock_stopped(const Best1700BthPower *s);
bool best1700_bth_power_is_memory_off(const Best1700BthPower *s);

#endif
