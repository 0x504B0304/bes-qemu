/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_MPC_H
#define BEST1700_MPC_H

#include "hw/misc/tz-mpc.h"

#define TYPE_BEST1700_MPC "best1700-mpc"
OBJECT_DECLARE_TYPE(Best1700MpcState, Best1700MpcClass, BEST1700_MPC)

struct Best1700MpcState {
    TZMPC parent_obj;
};

struct Best1700MpcClass {
    SysBusDeviceClass parent_class;
    DeviceReset parent_reset;
};

#endif
