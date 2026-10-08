#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "best1700_aon.h"
#include "best1700_cmu_clock.h"

#define BEST1700_AON_CMU_CHIP_ID       0x00
#define BEST1700_AON_CMU_OSC_PU_STATUS 0x34
#define BEST1700_AON_CMU_FREE_TIMER    0x50
#define BEST1700_AON_CMU_FREE_TIMER_HI 0x54
#define BEST1700_AON_CMU_MEMSC_BASE    0x100
#define BEST1700_AON_CMU_MEMSC_COUNT   16
#define BEST1700_AON_CMU_MEMSC_STATUS  0x140

/* Each timer window contains two independently interrupting down-counters. */
#define BEST1700_TIMER_LOAD      0x00
#define BEST1700_TIMER_VALUE     0x04
#define BEST1700_TIMER_CONTROL   0x08
#define BEST1700_TIMER_INTCLR    0x0c
#define BEST1700_TIMER_RIS       0x10
#define BEST1700_TIMER_MIS       0x14
#define BEST1700_TIMER_BGLOAD    0x18
#define BEST1700_WDT_LOCK        0xc00
#define BEST1700_TIMER_ONESHOT   (1u << 0)
#define BEST1700_TIMER_32BIT     (1u << 1)
#define BEST1700_TIMER_IE        (1u << 5)
#define BEST1700_TIMER_PERIODIC  (1u << 6)
#define BEST1700_TIMER_ENABLE    (1u << 7)

static void best1700_timer_update_irq(Best1700AonTimer *timer)
{
    qemu_set_irq(timer->irq, timer->pending &&
                 (timer->control & BEST1700_TIMER_IE));
}

static void best1700_timer_expired(void *opaque)
{
    Best1700AonTimer *timer = opaque;

    timer->pending = true;
    best1700_timer_update_irq(timer);
}

static uint32_t best1700_timer_mask(Best1700AonTimer *timer)
{
    return timer->control & BEST1700_TIMER_32BIT ? UINT32_MAX : UINT16_MAX;
}

/* Called inside a ptimer transaction; BGLoad changes only the reload limit. */
static void best1700_timer_limit(Best1700AonTimer *timer)
{
    uint32_t mask = best1700_timer_mask(timer);
    bool loaded = timer->control &
                  (BEST1700_TIMER_PERIODIC | BEST1700_TIMER_ONESHOT);

    /* The hardware holds Value=0 for one clock before expiry/reload.  Store
     * that final clock in ptimer's count, including the 2^32-clock case. */
    uint64_t limit = loaded ? timer->load & mask : mask;

    ptimer_set_limit(timer->timer, limit + 1, 0);
}

static void best1700_timer_period(Best1700AonTimer *timer, uint32_t frequency)
{
    unsigned scale = (timer->control >> 2) & 3;
    unsigned divisor = scale == 1 ? 16 : scale == 2 ? 256 : 1;

    if (frequency % divisor == 0) {
        /* Preserve sub-nanosecond precision for clocks such as 6 MHz. */
        ptimer_set_freq(timer->timer, frequency / divisor);
    } else {
        /* A fractional-Hz prescaled clock must not be rounded to whole Hz. */
        ptimer_set_period(timer->timer,
                          MAX(1, (int64_t)NANOSECONDS_PER_SECOND * divisor /
                                 frequency));
    }
}

static uint32_t best1700_timer_read(Best1700AonTimer *timer, hwaddr address)
{
    switch (address) {
    case BEST1700_TIMER_LOAD:
    case BEST1700_TIMER_BGLOAD:
        return timer->load;
    case BEST1700_TIMER_VALUE:
    {
        uint64_t count = ptimer_get_count(timer->timer);

        return count ? count - 1 : 0;
    }
    case BEST1700_TIMER_CONTROL:
        return timer->control;
    case BEST1700_TIMER_RIS:
        return timer->pending;
    case BEST1700_TIMER_MIS:
        return timer->pending && (timer->control & BEST1700_TIMER_IE);
    default:
        return 0;
    }
}

static void best1700_timer_write(Best1700AonTimer *timer, hwaddr address,
                                  uint32_t value, uint32_t frequency)
{
    uint64_t count;

    ptimer_transaction_begin(timer->timer);
    switch (address) {
    case BEST1700_TIMER_LOAD:
        timer->load = value;
        best1700_timer_limit(timer);
        ptimer_set_count(timer->timer,
                         (uint64_t)(value & best1700_timer_mask(timer)) + 1);
        break;
    case BEST1700_TIMER_BGLOAD:
        timer->load = value;
        best1700_timer_limit(timer);
        break;
    case BEST1700_TIMER_CONTROL:
        count = ptimer_get_count(timer->timer);
        ptimer_stop(timer->timer);
        timer->control = value & 0xff;
        best1700_timer_period(timer, frequency);
        best1700_timer_limit(timer);
        /* Preserve the internal final-zero clock across pause/resume.
         * Mask only the visible hardware value when the width changes. */
        if (count) {
            count = ((count - 1) & best1700_timer_mask(timer)) + 1;
        }
        ptimer_set_count(timer->timer, count);
        if (timer->control & BEST1700_TIMER_ENABLE) {
            ptimer_run(timer->timer,
                       (timer->control & BEST1700_TIMER_ONESHOT) != 0);
        }
        break;
    case BEST1700_TIMER_INTCLR:
        timer->pending = false;
        break;
    default:
        break;
    }
    ptimer_transaction_commit(timer->timer);
    best1700_timer_update_irq(timer);
}

static uint64_t best1700_aon_ticks(Best1700Aon *aon)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - aon->epoch_ns;

    return aon->tick + muldiv64(MAX(elapsed, 0), aon->frequency,
                               NANOSECONDS_PER_SECOND);
}

static uint32_t best1700_aon_merge(uint32_t old, hwaddr address,
                                   uint64_t value, unsigned size)
{
    unsigned shift = (address & 3) * 8;
    uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
    return (old & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
}

static uint64_t best1700_aon_read(void *opaque, hwaddr address,
                                  unsigned size)
{
    Best1700Aon *aon = opaque;
    uint32_t value = 0;

    if (address < aon->size && address / 4 < aon->size / 4) {
        value = aon->regs[address >> 2];
        if (aon->kind == BEST1700_AON_CMU) {
            switch (address & ~3u) {
            case BEST1700_AON_CMU_CHIP_ID:
                value = aon->chip_id;
                break;
            case BEST1700_AON_CMU_OSC_PU_STATUS:
                /* Clock/oscillator ready bits are asserted after reset. */
                value = 0xffffffffu;
                break;
            case BEST1700_AON_CMU_FREE_TIMER:
                value = best1700_aon_ticks(aon);
                break;
            case BEST1700_AON_CMU_FREE_TIMER_HI:
                value = best1700_aon_ticks(aon) >> 32;
                break;
            default:
                if (aon->memsc && size == sizeof(uint32_t) && !(address & 3) &&
                    address >= BEST1700_AON_CMU_MEMSC_BASE &&
                    address < BEST1700_AON_CMU_MEMSC_BASE +
                              BEST1700_AON_CMU_MEMSC_COUNT * sizeof(uint32_t)) {
                    unsigned index = (address - BEST1700_AON_CMU_MEMSC_BASE) /
                                     sizeof(uint32_t);
                    /* A lock read is an atomic test-and-set. */
                    value = best1700_memsc_claim(aon->memsc, index);
                } else if (aon->memsc && size == sizeof(uint32_t) &&
                           address == BEST1700_AON_CMU_MEMSC_STATUS) {
                    /* Status is a non-consuming snapshot of all 16 locks. */
                    value = best1700_memsc_status(aon->memsc);
                }
                break;
            }
        } else if (aon->kind == BEST1700_AON_TIMER && address < 0x40) {
            value = best1700_timer_read(&aon->timers[address / 0x20],
                                        address & 0x1c);
        } else if (aon->kind == BEST1700_AON_WDT &&
                   (address == BEST1700_TIMER_RIS ||
                    address == BEST1700_TIMER_MIS)) {
            value = 0;
        }
        for (unsigned i = 0; i < aon->endpoint_count; i++) {
            Best1700AonEndpoint *endpoint = &aon->endpoints[i];
            uint32_t supplied = 0;
            uint32_t mask = endpoint->read ?
                endpoint->read(endpoint->opaque, address & ~3u, &supplied) : 0;

            value = (value & ~mask) | (supplied & mask);
        }
        aon->reads++;
        if (aon->reads <= 48) {
            qemu_log_mask(LOG_UNIMP,
                          "best1700 %s read offset=0x%03" HWADDR_PRIx
                          " value=0x%08" PRIx32 " pc=0x%08" PRIx32 "\n",
                          aon->name, address, value,
                          aon->pc ? *aon->pc : 0);
        }
    }
    if (size == 1) {
        return (value >> ((address & 3) * 8)) & 0xff;
    }
    if (size == 2) {
        return (value >> ((address & 2) * 8)) & 0xffff;
    }
    return value;
}

static void best1700_aon_write(void *opaque, hwaddr address,
                               uint64_t value, unsigned size)
{
    Best1700Aon *aon = opaque;
    uint32_t *reg;
    unsigned shift = (address & 3) * 8;
    uint32_t lane_mask = (size == 1 ? 0xffu : size == 2 ? 0xffffu :
                          UINT32_MAX) << shift;
    uint32_t shifted = (uint32_t)value << shift;
    uint32_t handled = 0;

    if (address >= aon->size || address / 4 >= aon->size / 4) {
        return;
    }
    if (aon->kind == BEST1700_AON_CMU && aon->memsc &&
        size == sizeof(uint32_t) && !(address & 3) &&
        address >= BEST1700_AON_CMU_MEMSC_BASE &&
        address < BEST1700_AON_CMU_MEMSC_BASE +
                  BEST1700_AON_CMU_MEMSC_COUNT * sizeof(uint32_t)) {
        unsigned index = (address - BEST1700_AON_CMU_MEMSC_BASE) /
                         sizeof(uint32_t);
        best1700_memsc_release(aon->memsc, index, value);
        return;
    }
    if (aon->kind == BEST1700_AON_CMU && aon->memsc &&
        size == sizeof(uint32_t) && !(address & 3) &&
        address == BEST1700_AON_CMU_MEMSC_STATUS) {
        /* MEMSC_STATUS is read-only. */
        return;
    }
    reg = &aon->regs[address >> 2];
    for (unsigned i = 0; i < aon->endpoint_count; i++) {
        Best1700AonEndpoint *endpoint = &aon->endpoints[i];

        if (endpoint->write) {
            handled |= endpoint->write(endpoint->opaque, address & ~3u,
                                        shifted, lane_mask) & lane_mask;
        }
    }
    if (handled) {
        /* Unowned bits retain the storage-backed register behavior. Owners
         * of separate reset domains have already received the same write. */
        uint32_t remaining = lane_mask & ~handled;

        *reg = (*reg & ~remaining) | (shifted & remaining);
    } else if (aon->kind == BEST1700_AON_CMU &&
        best1700_cmu_clock_write(aon->regs, aon->size / sizeof(uint32_t),
                                 BEST1700_CMU_AON, address, value, size)) {
        /* Paired clock command ports share one control latch. */
    } else if (aon->kind == BEST1700_AON_CMU) {
        /* Reset commands remain owned by the AON controller. */
        switch (address) {
        case 0x0c: /* ARESET_PULSE */
            aon->regs[0x10 >> 2] |= (uint32_t)value;
            aon->regs[0x14 >> 2] &= ~((uint32_t)value);
            break;
        case 0x14: /* ARESET_CLR */
            aon->regs[0x10 >> 2] &= ~((uint32_t)value);
            break;
        default:
            *reg = best1700_aon_merge(*reg, address, value, size);
            break;
        }
    } else if (aon->kind == BEST1700_AON_TIMER && address < 0x40) {
        Best1700AonTimer *timer = &aon->timers[address / 0x20];
        hwaddr offset = address & 0x1c;
        uint32_t merged = best1700_aon_merge(
            best1700_timer_read(timer, offset), address, value, size);

        best1700_timer_write(timer, offset, merged, timer->frequency);
    } else if (aon->kind == BEST1700_AON_WDT &&
               address == BEST1700_TIMER_INTCLR) {
        aon->regs[BEST1700_TIMER_RIS >> 2] = 0;
        aon->regs[BEST1700_TIMER_MIS >> 2] = 0;
    } else if (aon->kind == BEST1700_AON_WDT &&
               address == BEST1700_WDT_LOCK) {
        /* The lock is accepted so firmware can safely configure the block. */
        *reg = (uint32_t)value;
    } else {
        *reg = best1700_aon_merge(*reg, address, value, size);
    }
    aon->writes++;
    if (aon->writes <= 48) {
        qemu_log_mask(LOG_UNIMP,
                      "best1700 %s write offset=0x%03" HWADDR_PRIx
                      " value=0x%08" PRIx64 " pc=0x%08" PRIx32 "\n",
                      aon->name, address, value,
                      aon->pc ? *aon->pc : 0);
    }
}

static const MemoryRegionOps best1700_aon_ops = {
    .read = best1700_aon_read,
    .write = best1700_aon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void best1700_aon_init(Best1700Aon *aon, Object *owner, const char *name,
                       Best1700AonKind kind, unsigned size,
                       const uint32_t *pc)
{
    memset(aon, 0, sizeof(*aon));
    aon->size = size;
    aon->kind = kind;
    aon->name = g_strdup(name);
    aon->pc = pc;
    aon->chip_id = 0x00001700u;
    aon->regs = g_new0(uint32_t, size / sizeof(uint32_t));
    aon->frequency = 16384;
    if (kind == BEST1700_AON_TIMER) {
        for (unsigned i = 0; i < 2; i++) {
            aon->timers[i].frequency = aon->frequency;
            aon->timers[i].timer = ptimer_init(
                best1700_timer_expired, &aon->timers[i],
                PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                PTIMER_POLICY_NO_IMMEDIATE_RELOAD |
                PTIMER_POLICY_NO_COUNTER_ROUND_DOWN |
                PTIMER_POLICY_CONTINUOUS_TRIGGER);
        }
    }
    best1700_aon_reset(aon);
    memory_region_init_io(&aon->region, owner, &best1700_aon_ops, aon,
                          name, size);
}

void best1700_aon_reset(Best1700Aon *aon)
{
    memset(aon->regs, 0, aon->size / sizeof(uint32_t) * sizeof(uint32_t));
    aon->reads = 0;
    aon->writes = 0;
    aon->tick = 1;
    aon->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /* Clocks and reset lines are available from the first guest access. */
    if (aon->kind == BEST1700_AON_CMU) {
        aon->regs[0x34 >> 2] = 0xffffffffu;
    }
    if (aon->kind == BEST1700_AON_TIMER) {
        for (unsigned i = 0; i < 2; i++) {
            Best1700AonTimer *timer = &aon->timers[i];

            ptimer_transaction_begin(timer->timer);
            ptimer_stop(timer->timer);
            timer->load = 0;
            timer->control = BEST1700_TIMER_IE;
            timer->pending = false;
            best1700_timer_period(timer, timer->frequency);
            best1700_timer_limit(timer);
            ptimer_set_count(timer->timer, (uint64_t)UINT16_MAX + 1);
            ptimer_transaction_commit(timer->timer);
            best1700_timer_update_irq(timer);
        }
    }
}

void best1700_aon_add(Best1700Aon *aon, MemoryRegion *space, hwaddr base,
                      int priority)
{
    memory_region_add_subregion_overlap(space, base, &aon->region, priority);
}

void best1700_aon_set_pc(Best1700Aon *aon, const uint32_t *pc)
{
    aon->pc = pc;
}

void best1700_aon_set_memsc(Best1700Aon *aon, Best1700Memsc *memsc)
{
    aon->memsc = memsc;
}

void best1700_aon_connect_endpoint(Best1700Aon *aon,
                                    const Best1700AonEndpoint *endpoint)
{
    g_assert(aon->endpoint_count < G_N_ELEMENTS(aon->endpoints));
    aon->endpoints[aon->endpoint_count++] = *endpoint;
}

void best1700_aon_set_timer_irq(Best1700Aon *aon, unsigned index, qemu_irq irq)
{
    assert(aon->kind == BEST1700_AON_TIMER && index < 2);
    aon->timers[index].irq = irq;
    best1700_timer_update_irq(&aon->timers[index]);
}

void best1700_aon_set_frequency(Best1700Aon *aon, uint32_t frequency)
{
    assert(frequency != 0);
    aon->tick = best1700_aon_ticks(aon);
    aon->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    aon->frequency = frequency;
    if (aon->kind == BEST1700_AON_TIMER) {
        for (unsigned i = 0; i < 2; i++) {
            best1700_aon_set_timer_frequency(aon, i, frequency);
        }
    }
}

void best1700_aon_set_timer_frequency(Best1700Aon *aon, unsigned index,
                                     uint32_t frequency)
{
    assert(aon->kind == BEST1700_AON_TIMER && index < 2 && frequency != 0);
    aon->timers[index].frequency = frequency;
    ptimer_transaction_begin(aon->timers[index].timer);
    best1700_timer_period(&aon->timers[index], frequency);
    ptimer_transaction_commit(aon->timers[index].timer);
}
