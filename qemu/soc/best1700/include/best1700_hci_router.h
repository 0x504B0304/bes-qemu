/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_HCI_ROUTER_H
#define BEST1700_HCI_ROUTER_H
#include "exec/memory.h"
#include "best1700_btc_ipc.h"

typedef struct Best1700HciRouter Best1700HciRouter;
/* Layout is supplied by the firmware package: hexadecimal table address,
 * unused receive aperture, then exact opcodes owned by the original BTC,
 * separated with ':'. No firmware identity or addresses are built in. */
Best1700HciRouter *best1700_hci_router_new(Object *owner, AddressSpace *cp,
    MemoryRegion *cp_space, MemoryRegion *ap_space, Best1700BtcIpc *ipc,
    const char *layout, const char *backend, Error **errp);
void best1700_hci_router_close(Best1700HciRouter *s);
char *best1700_hci_router_status(Best1700HciRouter *s);
#endif
