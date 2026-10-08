/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_SC89620_H
#define MIBAND_SC89620_H

#include "hw/i2c/i2c.h"

#define TYPE_SC89620 "sc89620"
OBJECT_DECLARE_SIMPLE_TYPE(Sc89620State, SC89620)

struct Sc89620State {
    I2CSlave parent_obj;
    uint8_t regs[256];
    uint8_t pointer;
    bool selected;
    uint32_t voltage_mv;
    bool online;
};

#endif
