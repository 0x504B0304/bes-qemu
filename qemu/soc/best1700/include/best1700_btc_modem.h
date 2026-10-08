/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTC_MODEM_H
#define BEST1700_BTC_MODEM_H

#include "exec/memory.h"

#define BEST1700_BTC_MODEM_WINDOW 0x1000u

typedef struct Best1700BtcModem {
    MemoryRegion iomem;
    uint32_t config[BEST1700_BTC_MODEM_WINDOW / sizeof(uint32_t)];
    /* MIX_MODE_CONFIG is a command/configuration word.  The firmware writes
     * undocumented mixer setup bits together with START_MIX, so retain the
     * last non-command fields instead of treating the word as bit30 only. */
    uint32_t mix_config;
    uint32_t energy_i;
    uint32_t energy_q;
} Best1700BtcModem;

void best1700_btc_modem_init(Best1700BtcModem *s, Object *owner);
void best1700_btc_modem_reset(Best1700BtcModem *s);

#endif
