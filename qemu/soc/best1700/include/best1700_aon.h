#ifndef BEST1700_AON_H
#define BEST1700_AON_H

#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/ptimer.h"
#include "best1700_memsc.h"

typedef enum Best1700AonKind {
    BEST1700_AON_CMU,
    BEST1700_AON_WDT,
    BEST1700_AON_TIMER,
    BEST1700_AON_GENERIC,
} Best1700AonKind;

typedef struct Best1700AonTimer {
    ptimer_state *timer;
    qemu_irq irq;
    uint32_t load;
    uint32_t control;
    uint32_t frequency;
    bool pending;
} Best1700AonTimer;

/* Register owners return a bit mask, so independent reset domains can share
 * command words. Offsets are word aligned; writes already contain byte-lane
 * shifts. Every endpoint observes a write, including shared command ports. */
typedef struct Best1700AonEndpoint {
    void *opaque;
    uint32_t (*read)(void *opaque, hwaddr offset, uint32_t *value);
    uint32_t (*write)(void *opaque, hwaddr offset, uint32_t value,
                      uint32_t lane_mask);
} Best1700AonEndpoint;

typedef struct Best1700Aon {
    MemoryRegion region;
    uint32_t *regs;
    unsigned size;
    Best1700AonKind kind;
    const char *name;
    const uint32_t *pc;
    uint32_t reads;
    uint32_t writes;
    uint64_t tick;
    int64_t epoch_ns;
    uint32_t frequency;
    Best1700AonTimer timers[2];
    uint32_t chip_id;
    Best1700Memsc *memsc;
    Best1700AonEndpoint endpoints[4];
    unsigned endpoint_count;
} Best1700Aon;

void best1700_aon_init(Best1700Aon *aon, Object *owner, const char *name,
                       Best1700AonKind kind, unsigned size,
                       const uint32_t *pc);
void best1700_aon_add(Best1700Aon *aon, MemoryRegion *space, hwaddr base,
                      int priority);
void best1700_aon_set_pc(Best1700Aon *aon, const uint32_t *pc);
void best1700_aon_connect_endpoint(Best1700Aon *aon,
                                    const Best1700AonEndpoint *endpoint);
void best1700_aon_set_timer_irq(Best1700Aon *aon, unsigned index,
                               qemu_irq irq);
void best1700_aon_set_frequency(Best1700Aon *aon, uint32_t frequency);
void best1700_aon_set_timer_frequency(Best1700Aon *aon, unsigned index,
                                     uint32_t frequency);
void best1700_aon_reset(Best1700Aon *aon);
void best1700_aon_set_memsc(Best1700Aon *aon, Best1700Memsc *memsc);

#endif
