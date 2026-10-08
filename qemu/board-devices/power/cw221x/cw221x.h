/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_CW221X_H
#define MIBAND_CW221X_H

#include "hw/i2c/i2c.h"

#define TYPE_CW221X "cw221x"
OBJECT_DECLARE_SIMPLE_TYPE(Cw221xState, CW221X)

struct Cw221xState {
    I2CSlave parent_obj;
    uint8_t regs[256];
    uint8_t pointer;
    bool selected;
    uint32_t voltage_mv;
    uint32_t capacity_pct;
    int32_t temperature_decic;
};

#endif
