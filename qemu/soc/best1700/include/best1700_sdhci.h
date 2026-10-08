/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_SDHCI_H
#define BEST1700_SDHCI_H

#include "hw/sd/sdhci.h"

#define TYPE_BEST1700_SDHCI "best1700-sdhci"
OBJECT_DECLARE_SIMPLE_TYPE(Best1700SdhciState, BEST1700_SDHCI)

struct Best1700SdhciState {
    SysBusDevice parent_obj;
    SDHCIState host;
    MemoryRegion container;
    MemoryRegion vendor;
    uint32_t regs[0x1000 / sizeof(uint32_t)];
    unsigned diagnostics;
    bool dma_nonsecure;
};

#endif
