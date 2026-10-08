/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTC_IPC_H
#define BEST1700_BTC_IPC_H

#include "best1700_aon.h"

#define BEST1700_BTC_IPC_LANES 4
#define BEST1700_BTC_IPC_REGISTERS 6
#define BEST1700_BTC_IPC_BANK_SIZE 0x1000

/* Each adjacent pair has independent IND/DONE/mask state. */
typedef enum Best1700BtcIpcSide {
    BEST1700_BTC_IPC_SYS,
    BEST1700_BTC_IPC_BTC_SYS,
    BEST1700_BTC_IPC_BTH,
    BEST1700_BTC_IPC_BTC_BTH,
    BEST1700_BTC_IPC_SIDES,
} Best1700BtcIpcSide;

typedef enum Best1700BtcIpcBankId {
    BEST1700_BTC_IPC_BANK_SYS,
    BEST1700_BTC_IPC_BANK_BTC,
    BEST1700_BTC_IPC_BANK_BTH,
    BEST1700_BTC_IPC_BANKS,
} Best1700BtcIpcBankId;

typedef struct Best1700BtcIpc Best1700BtcIpc;
typedef struct Best1700BtcIpcBank Best1700BtcIpcBank;

typedef struct Best1700BtcIpcWindow {
    MemoryRegion region;
    Best1700BtcIpcBank *bank;
    hwaddr offset;
} Best1700BtcIpcWindow;

typedef struct Best1700BtcIpcPort {
    uint8_t ind;
    uint8_t done;
    uint8_t rx_mask;
    uint8_t done_mask;
    qemu_irq rx[BEST1700_BTC_IPC_LANES];
    qemu_irq tx_done[BEST1700_BTC_IPC_LANES];
} Best1700BtcIpcPort;

struct Best1700BtcIpcBank {
    Best1700BtcIpc *controller;
    Best1700BtcIpcBankId id;
    Best1700BtcIpcWindow window[BEST1700_BTC_IPC_REGISTERS];
};

struct Best1700BtcIpc {
    Best1700BtcIpcPort port[BEST1700_BTC_IPC_SIDES];
    /* CMU wake request is a level until the paired clear command. */
    bool core_wakeup;
    MemoryRegion bank[BEST1700_BTC_IPC_BANKS];
    Best1700BtcIpcBank bank_context[BEST1700_BTC_IPC_BANKS];
    void *wake_opaque;
    void (*wake)(void *opaque, bool level);
};

void best1700_btc_ipc_init(Best1700BtcIpc *s, Object *owner);
void best1700_btc_ipc_reset(Best1700BtcIpc *s);
void best1700_btc_ipc_refresh(Best1700BtcIpc *s);
void best1700_btc_ipc_connect_wake(Best1700BtcIpc *s, void *opaque,
                                  void (*wake)(void *opaque, bool level));
void best1700_btc_ipc_connect_irq(Best1700BtcIpc *s,
                                Best1700BtcIpcSide side, unsigned lane,
                                qemu_irq rx, qemu_irq done);
Best1700AonEndpoint best1700_btc_ipc_endpoint(Best1700BtcIpc *s,
                                            Best1700BtcIpcBankId bank);

#endif
