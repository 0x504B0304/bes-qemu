#ifndef BEST1502P_TIMER_H
#define BEST1502P_TIMER_H

#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/ptimer.h"

typedef struct Best1502pTimerChannel {
    ptimer_state *counter;
    qemu_irq irq;
    uint32_t frequency;
    uint32_t load;
    uint32_t control;
    bool pending;
} Best1502pTimerChannel;

typedef struct Best1502pTimer {
    uint32_t regs[0x100 / sizeof(uint32_t)];
    Best1502pTimerChannel channel[2];
    unsigned reads;
    hwaddr base;
} Best1502pTimer;

void best1502p_timer_init(Best1502pTimer *timer, MemoryRegion *region,
                          const char *name, hwaddr base,
                          uint32_t frequency0, uint32_t frequency1);

#endif
