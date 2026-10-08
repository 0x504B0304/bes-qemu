/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_MASTER_GATE_H
#define BEST1700_MASTER_GATE_H

#include "best1700_sec.h"

typedef struct Best1700MasterGate Best1700MasterGate;

typedef struct Best1700MasterGateNotifier {
    IOMMUNotifier notifier;
    Best1700MasterGate *gate;
} Best1700MasterGateNotifier;

struct Best1700MasterGate {
    IOMMUMemoryRegion iommu;
    IOMMUMemoryRegion *downstream;
    Best1700SecState *sec;
    Best1700MasterGateNotifier notifiers[2];
    MemoryRegion slow_io;
    AddressSpace slow_as;
};

/* Wrap an existing two-index MPC/TGU IOMMU. Allowed RAM remains directly
 * executable; denied/non-RAM transactions still pass through that gate with
 * the selected outbound attributes, preserving its checks and diagnostics. */
MemoryRegion *best1700_master_gate_init(Best1700MasterGate *s, Object *owner,
                                        const char *name, Best1700SecState *sec,
                                        MemoryRegion *downstream);

#endif
