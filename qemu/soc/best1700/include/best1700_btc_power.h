/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTC_POWER_H
#define BEST1700_BTC_POWER_H

#include "exec/memory.h"

#define BEST1700_BTC_POWER_BASE 0x40085000u
#define BEST1700_BTC_POWER_WINDOW 0x1000u
#define BEST1700_BTC_POWER_REGISTERS 9

typedef struct Best1700BtcPower Best1700BtcPower;

typedef struct Best1700BtcPowerRegister {
    MemoryRegion iomem;
    Best1700BtcPower *power;
    hwaddr offset;
} Best1700BtcPowerRegister;

struct Best1700BtcPower {
    /* Sparse PSC overlay, shared by all participating bus masters. */
    MemoryRegion iomem;
    Best1700BtcPowerRegister registers[BEST1700_BTC_POWER_REGISTERS];
    uint32_t auto_control;
    uint32_t sleep_control;
    uint32_t manual_control;
    uint32_t wakeup_mask;
    bool wake_level;
    bool off;
    void *power_opaque;
    void (*set_power)(void *opaque, bool off);
};

void best1700_btc_power_init(Best1700BtcPower *s, Object *owner);
void best1700_btc_power_reset(Best1700BtcPower *s);
void best1700_btc_power_connect(Best1700BtcPower *s, void *opaque,
                              void (*power)(void *opaque, bool off));
/* Return the resulting power-off state. Pending enabled wake prevents entry. */
bool best1700_btc_power_request_sleep(Best1700BtcPower *s);
/* The input is the BTC always-on wake aggregate, not a mailbox transaction. */
void best1700_btc_power_set_wake(Best1700BtcPower *s, bool level);
bool best1700_btc_power_is_off(const Best1700BtcPower *s);

#endif
