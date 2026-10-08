#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "miband_peripherals.h"

#define AON_TIMER0_BASE   0x40084000
#define AON_TIMER1_BASE   0x40084020
#define MCU_TIMER1_BASE   0x40003000
#define TIMER_AON0_HZ     16384
#define TIMER_AON1_HZ     16000
#define TIMER_MCU_HZ      6000000
#define TIMER_CTRL_ENABLE 0x80
#define TIMER_CTRL_PERIODIC 0x40
#define TIMER_CTRL_IRQ_ENABLE 0x20
#define TIMER_CTRL_ONESHOT 0x01

static uint32_t timer_tick_hz(const MiBandTimer *timer)
{
    /* This AP converts AON0 count to us as count * 1000000 >> 14
     * (0x0c19d49a), but arms AON1 with us * 16 / 1000 (0x0c19d3c6).
     * Keep the independently observed counter and alarm rates separate.
     */
    switch (timer->base) {
    case AON_TIMER0_BASE:
        return TIMER_AON0_HZ;
    case AON_TIMER1_BASE:
        return TIMER_AON1_HZ;
    default:
        return TIMER_MCU_HZ;
    }
}

static void timer_arm(MiBandTimer *timer)
{
    uint64_t ticks;
    int64_t delay_ns;

    if (!timer->event) {
        return;
    }
    timer_del(timer->event);
    if (!(timer->ctrl & TIMER_CTRL_ENABLE)) {
        return;
    }

    ticks = timer->value ? timer->value : 1;
    delay_ns = (ticks * 1000000000ULL + timer_tick_hz(timer) - 1) /
               timer_tick_hz(timer);
    timer_mod(timer->event, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
    timer->expired = false;
}

static void timer_expire(void *opaque)
{
    MiBandTimer *timer = opaque;

    if (timer->base == AON_TIMER1_BASE && timer->trace_events++ < 4096) {
        qemu_log_mask(LOG_UNIMP,
                      "miband timer %08" HWADDR_PRIx
                      " expired ctrl=%08x load=%08x irq=%u\n",
                      timer->base, timer->ctrl, timer->load,
                      !!(timer->ctrl & TIMER_CTRL_IRQ_ENABLE));
    }
    timer->value = 0;
    timer->expired = true;
    timer->pending = true;
    if (timer->ctrl & TIMER_CTRL_IRQ_ENABLE) {
        qemu_irq_raise(timer->irq);
    }
    if (!(timer->ctrl & TIMER_CTRL_ONESHOT)) {
        /* Non-periodic mode free-runs from UINT32_MAX after the first wrap. */
        timer->value = timer->ctrl & TIMER_CTRL_PERIODIC ?
                       timer->load : UINT32_MAX;
        timer->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_arm(timer);
    }
}

static MiBandTimer *timer_at(MiBandTimer timers[MIBAND_TIMER_COUNT],
                            hwaddr address)
{
    for (unsigned i = 0; i < MIBAND_TIMER_COUNT; i++) {
        if (address >= timers[i].base && address <= timers[i].base + 0x18 &&
            !(address & 3)) {
            return &timers[i];
        }
    }
    return NULL;
}

static uint32_t timer_value(MiBandTimer *timer)
{
    int64_t elapsed_ns;
    uint64_t ticks;

    if (!(timer->ctrl & TIMER_CTRL_ENABLE)) {
        return timer->value;
    }
    if (timer->expired) {
        return 0;
    }

    elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - timer->start_ns;
    ticks = elapsed_ns > 0 ?
            (uint64_t)elapsed_ns / 1000000000ULL * timer_tick_hz(timer) +
            ((uint64_t)elapsed_ns % 1000000000ULL) * timer_tick_hz(timer) /
            1000000000ULL : 0;
    if (timer->base == MCU_TIMER1_BASE &&
        !(timer->ctrl & (TIMER_CTRL_PERIODIC | TIMER_CTRL_ONESHOT))) {
        return timer->value - (uint32_t)ticks;
    }
    return ticks >= timer->value ? 0 : timer->value - ticks;
}

void miband_timers_init(MiBandTimer timers[MIBAND_TIMER_COUNT])
{
    timers[0] = (MiBandTimer) { .base = AON_TIMER0_BASE,
                               .value = UINT32_MAX };
    timers[1] = (MiBandTimer) { .base = AON_TIMER1_BASE,
                               .value = UINT32_MAX };
    timers[2] = (MiBandTimer) { .base = MCU_TIMER1_BASE,
                               .value = UINT32_MAX };
}

void miband_timer_connect_irq(MiBandTimer *timer, qemu_irq irq)
{
    timer->irq = irq;
    timer->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, timer_expire, timer);
}

bool miband_timer_read(MiBandTimer timers[MIBAND_TIMER_COUNT], hwaddr address,
                       unsigned size, uint64_t *result)
{
    MiBandTimer *timer = size == sizeof(uint32_t) ?
                         timer_at(timers, address) : NULL;

    if (!timer) {
        return false;
    }

    switch (address - timer->base) {
    case 0:
        *result = timer->load;
        break;
    case 4:
        *result = timer_value(timer);
        break;
    case 8:
        *result = timer->ctrl;
        break;
    case 0x0c:
        *result = 0;
        break;
    case 0x10:
        *result = timer->pending;
        break;
    case 0x14:
        *result = timer->pending && (timer->ctrl & TIMER_CTRL_IRQ_ENABLE);
        break;
    case 0x18:
        *result = timer->load;
        break;
    }
    return true;
}

bool miband_timer_write(MiBandTimer timers[MIBAND_TIMER_COUNT], hwaddr address,
                        unsigned size, uint64_t value)
{
    MiBandTimer *timer = size == sizeof(uint32_t) ?
                         timer_at(timers, address) : NULL;
    hwaddr offset;
    bool trace;

    if (!timer) {
        return false;
    }

    offset = address - timer->base;
    trace = timer->trace_events < 64 ||
            (timer->base == AON_TIMER1_BASE &&
             (offset == 0 ||
              (offset == 8 && ((timer->ctrl ^ value) & TIMER_CTRL_ENABLE))) &&
             timer->trace_events < 4096);
    timer->trace_events++;
    if (trace) {
        qemu_log_mask(LOG_UNIMP,
                      "miband timer %08" HWADDR_PRIx
                      " write +%02" HWADDR_PRIx "=%08" PRIx64
                      " ctrl=%08x remaining=%08x\n",
                      timer->base, offset, value,
                      timer->ctrl, timer_value(timer));
    }

    switch (offset) {
    case 0:
        timer->load = value;
        timer->value = value ? value : UINT32_MAX;
        timer->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer->expired = false;
        timer_arm(timer);
        break;
    case 8:
        if (!(timer->ctrl & TIMER_CTRL_ENABLE) &&
            (value & TIMER_CTRL_ENABLE)) {
            timer->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        } else if ((timer->ctrl & TIMER_CTRL_ENABLE) &&
                   !(value & TIMER_CTRL_ENABLE)) {
            timer->value = timer_value(timer);
        }
        timer->ctrl = value;
        timer_arm(timer);
        break;
    case 0x0c:
        timer->pending = false;
        if (timer->irq) {
            qemu_irq_lower(timer->irq);
        }
        break;
    case 0x18:
        timer->load = value;
        break;
    }
    return true;
}
