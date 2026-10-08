/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_M55_IPC_H
#define BEST1700_M55_IPC_H

#include "hw/irq.h"

#define BEST1700_M55_IPC_CHANNELS 3
/* SYS CMU 0x90/0x94: other bits belong to DSP and other connections. */
#define BEST1700_M55_IPC_IRQ_MASK 0x00fff000u

typedef enum Best1700M55IpcSide {
    BEST1700_M55_IPC_AP,
    BEST1700_M55_IPC_CP,
    BEST1700_M55_IPC_SIDES,
} Best1700M55IpcSide;

typedef struct Best1700M55Ipc {
    uint32_t pending;
    qemu_irq rx[BEST1700_M55_IPC_SIDES][BEST1700_M55_IPC_CHANNELS];
    qemu_irq done[BEST1700_M55_IPC_SIDES][BEST1700_M55_IPC_CHANNELS];
} Best1700M55Ipc;

void best1700_m55_ipc_init(Best1700M55Ipc *s);
void best1700_m55_ipc_connect_irq(Best1700M55Ipc *s, Best1700M55IpcSide side,
                                unsigned channel, qemu_irq rx, qemu_irq done);
uint32_t best1700_m55_ipc_pending(const Best1700M55Ipc *s);
void best1700_m55_ipc_set(Best1700M55Ipc *s, uint32_t bits);
void best1700_m55_ipc_clear(Best1700M55Ipc *s, uint32_t bits);
void best1700_m55_ipc_reset(Best1700M55Ipc *s);

#endif
