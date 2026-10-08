#include "qemu/osdep.h"
#include "sysemu/reset.h"
#include "best1502p_timer.h"

#define TIMER_SIZE 0x100u
#define ONESHOT 0x01u
#define WIDTH32 0x02u
#define IRQ_ENABLE 0x20u
#define PERIODIC 0x40u
#define ENABLE 0x80u

static void timer_update_irq(Best1502pTimerChannel *c)
{
    qemu_set_irq(c->irq, c->pending && (c->control & IRQ_ENABLE));
}

static void best1502p_timer_expired(void *opaque)
{
    Best1502pTimerChannel *c = opaque;

    c->pending = true;
    timer_update_irq(c);
}

/* BES uses two downcounters at a 0x20 stride. Clocks and IRQ routes are
 * supplied by the SoC; this block does not depend on firmware addresses. */
static void timer_limit(Best1502pTimerChannel *c, bool reload)
{
    uint32_t limit = c->load;

    if (!(c->control & (ONESHOT | PERIODIC))) {
        limit = c->control & WIDTH32 ? UINT32_MAX : UINT16_MAX;
    }
    ptimer_set_limit(c->counter, limit, reload);
}

static uint64_t best1502p_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1502pTimer *timer = opaque;
    Best1502pTimerChannel *c;

    if (offset >= 0x40) {
        return timer->regs[offset >> 2];
    }
    c = &timer->channel[offset >> 5];
    switch (offset & 0x1f) {
    case 0x00:
    case 0x18:
        return c->load;
    case 0x04:
        return ptimer_get_count(c->counter);
    case 0x08:
        return c->control;
    case 0x10:
        return c->pending;
    case 0x14:
        return c->pending && (c->control & IRQ_ENABLE);
    default:
        return 0;
    }
}

static void best1502p_timer_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Best1502pTimer *timer = opaque;
    Best1502pTimerChannel *c;
    uint32_t frequency;

    if (offset >= 0x40) {
        timer->regs[offset >> 2] = value;
        return;
    }
    c = &timer->channel[offset >> 5];
    switch (offset & 0x1f) {
    case 0x00:
    case 0x18:
        c->load = value;
        ptimer_transaction_begin(c->counter);
        timer_limit(c, (offset & 0x1f) == 0);
        ptimer_transaction_commit(c->counter);
        break;
    case 0x08:
        ptimer_transaction_begin(c->counter);
        ptimer_stop(c->counter);
        c->control = value & 0xff;
        frequency = c->frequency;
        switch ((c->control >> 2) & 3) {
        case 1: frequency >>= 4; break;
        case 2: frequency >>= 8; break;
        }
        timer_limit(c, c->control & ENABLE);
        ptimer_set_freq(c->counter, frequency);
        if (c->control & ENABLE) {
            ptimer_run(c->counter, !!(c->control & ONESHOT));
        }
        ptimer_transaction_commit(c->counter);
        break;
    case 0x0c:
        c->pending = false;
        break;
    default:
        break;
    }
    timer_update_irq(c);
}

static const MemoryRegionOps best1502p_timer_ops = {
    .read = best1502p_timer_read,
    .write = best1502p_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void best1502p_timer_reset(void *opaque)
{
    Best1502pTimer *timer = opaque;

    memset(timer->regs, 0, sizeof(timer->regs));
    for (unsigned i = 0; i < 2; i++) {
        Best1502pTimerChannel *c = &timer->channel[i];

        ptimer_transaction_begin(c->counter);
        ptimer_stop(c->counter);
        c->load = 0;
        c->control = 0;
        c->pending = false;
        ptimer_set_count(c->counter, 0);
        ptimer_transaction_commit(c->counter);
        timer_update_irq(c);
    }
}

void best1502p_timer_init(Best1502pTimer *timer, MemoryRegion *region,
                         const char *name, hwaddr base,
                         uint32_t frequency0, uint32_t frequency1)
{
    memset(timer, 0, sizeof(*timer));
    timer->base = base;
    for (unsigned i = 0; i < 2; i++) {
        Best1502pTimerChannel *c = &timer->channel[i];

        c->frequency = i ? frequency1 : frequency0;
        c->counter = ptimer_init(best1502p_timer_expired, c, PTIMER_POLICY_LEGACY);
        ptimer_transaction_begin(c->counter);
        ptimer_set_freq(c->counter, c->frequency);
        ptimer_transaction_commit(c->counter);
    }
    memory_region_init_io(region, NULL, &best1502p_timer_ops, timer, name,
                         TIMER_SIZE);
    qemu_register_reset(best1502p_timer_reset, timer);
}
