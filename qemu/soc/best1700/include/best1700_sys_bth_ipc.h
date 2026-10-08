/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_SYS_BTH_IPC_H
#define BEST1700_SYS_BTH_IPC_H

#include "best1700_aon.h"

#define BEST1700_SYS_BTH_LANES 16
#define BEST1700_SYS_BTH_REGISTERS 6
#define BEST1700_SYS_BTH_BANK_SIZE 0x1000

typedef enum Best1700SysBthSide {
    BEST1700_SYS_BTH_SYS,
    BEST1700_SYS_BTH_BTH,
    BEST1700_SYS_BTH_SIDES,
} Best1700SysBthSide;

typedef struct Best1700SysBthIpc Best1700SysBthIpc;
typedef struct Best1700SysBthPort Best1700SysBthPort;

typedef struct Best1700SysBthRegisterWindow {
    MemoryRegion region;
    Best1700SysBthPort *port;
    hwaddr offset;
} Best1700SysBthRegisterWindow;

struct Best1700SysBthPort {
    Best1700SysBthIpc *controller;
    Best1700SysBthSide side;
    uint32_t pending; /* Low: local TX DONE; high: outgoing DATA IND. */
    uint32_t mask;    /* Low: RX enable; high: local TX DONE enable. */
    qemu_irq rx[BEST1700_SYS_BTH_LANES];
    qemu_irq done[BEST1700_SYS_BTH_LANES];
    Best1700SysBthRegisterWindow window[BEST1700_SYS_BTH_REGISTERS];
};

struct Best1700SysBthIpc {
    Best1700SysBthPort port[BEST1700_SYS_BTH_SIDES];
    /* Sparse containers: unowned CMU offsets fall through to other owners. */
    MemoryRegion bank[BEST1700_SYS_BTH_SIDES];
};

void best1700_sys_bth_ipc_init(Best1700SysBthIpc *s, Object *owner);
Best1700AonEndpoint best1700_sys_bth_ipc_endpoint(Best1700SysBthIpc *s,
                                                Best1700SysBthSide side);
void best1700_sys_bth_ipc_connect_irq(Best1700SysBthIpc *s,
                                    Best1700SysBthSide side, unsigned lane,
                                    qemu_irq rx, qemu_irq done);
void best1700_sys_bth_ipc_reset(Best1700SysBthIpc *s);
void best1700_sys_bth_ipc_reset_lanes(Best1700SysBthIpc *s, uint16_t lanes);

#endif
