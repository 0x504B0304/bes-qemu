/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_AW35206_H
#define MIBAND_AW35206_H

#include "hw/i2c/i2c.h"

#define TYPE_AW35206 "aw35206"
OBJECT_DECLARE_SIMPLE_TYPE(Aw35206State, AW35206)

struct Aw35206State {
    I2CSlave parent_obj;
    uint8_t pointer;
    uint8_t enables;
    bool selected;
    bool transferred;
};

#endif
