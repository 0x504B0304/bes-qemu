/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_BTH_H
#define BEST1700_BTH_H

#include "exec/memory.h"
#include "best1700_aon.h"

#define BEST1700_BTH_RAM_BASE 0x23000000u
#define BEST1700_BTH_RAMX_BASE 0x03000000u
#define BEST1700_BTH_RAM_SIZE 0x00260000u
#define BEST1700_BTH_PRIVATE_SIZE 0x000c0000u
#define BEST1700_SHARED_BANK_SIZE 0x00020000u
#define BEST1700_SHARED_BANK_COUNT 13
#define BEST1700_BTC_SHARED_BASE 0x68400000u
#define BEST1700_BTC_SHARED_XBASE 0x60400000u
#define BEST1700_BTC_SHARED_SIZE \
    (BEST1700_SHARED_BANK_COUNT * BEST1700_SHARED_BANK_SIZE)

typedef struct Best1700Bth {
    MemoryRegion ram;
    MemoryRegion private_ram;
    MemoryRegion unselected;
    MemoryRegion btc_ram;
    MemoryRegion btc_unselected;
    MemoryRegion sys_ram[2];
    MemoryRegion sys_unselected[2];
    MemoryRegion sys_bank[16];
    MemoryRegion bth_bank[BEST1700_SHARED_BANK_COUNT];
    MemoryRegion btc_bank[BEST1700_SHARED_BANK_COUNT];
    uint32_t ram_cfg;
    uint32_t vtor[2];
    uint32_t reset_asserted;
    uint64_t unsupported_accesses;
    bool storage_bound;
    void *reset_opaque;
    void (*set_reset)(void *opaque, bool asserted, uint32_t vtor);
} Best1700Bth;

void best1700_bth_init(Best1700Bth *s, Object *owner);
void best1700_bth_bind_storage(Best1700Bth *s, Object *owner,
                               MemoryRegion *sram6, MemoryRegion *sram7);
void best1700_bth_reset(Best1700Bth *s);
void best1700_bth_connect_reset(Best1700Bth *s, void *opaque,
    void (*set_reset)(void *opaque, bool asserted, uint32_t vtor));
Best1700AonEndpoint best1700_bth_endpoint(Best1700Bth *s);
bool best1700_bth_cpu_held(Best1700Bth *s);

#endif
