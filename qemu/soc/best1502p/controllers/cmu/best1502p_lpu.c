#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "sysemu/reset.h"
#include "best1502p_lpu.h"

/* BEST1502P WAKEUP_CLK_CFG. Firmware/SDK use 64 oscillator wait ticks
 * for 2000 us and 7 PLL ticks for 200 us (rounded up), nominal 32 kHz.
 * This models polled readiness, not the entire clock/power tree. */
#define LPU_TICK_NS 31250
#define LPU_STATUS (3u << 14)

static uint64_t lpu_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1502pLpu *lpu = opaque;
    uint32_t value = lpu->config;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (unsigned i = 0; i < 2; i++) {
        if (lpu->ready_at[i] && now >= lpu->ready_at[i]) {
            value |= 1u << (14 + i);
        }
    }
    return value;
}

static void lpu_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Best1502pLpu *lpu = opaque;
    uint32_t next = value & ~LPU_STATUS;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (unsigned i = 0; i < 2; i++) {
        uint32_t enable = 1u << (12 + i);
        uint32_t timer_mask = i ? 0xf00 : 0xff;
        unsigned ticks = (next & timer_mask) >> (i ? 8 : 0);
        if (!(next & enable)) {
            lpu->ready_at[i] = 0;
        } else if ((next ^ lpu->config) & (enable | timer_mask)) {
            lpu->ready_at[i] = now + MAX(ticks, 1u) * LPU_TICK_NS;
        }
    }
    lpu->config = next;
    if (lpu->traces++ < 64) {
        qemu_log_mask(LOG_UNIMP, "best1502p LPU config=%08x "
                      "osc_deadline=%" PRId64 " pll_deadline=%" PRId64 "\n",
                      next, lpu->ready_at[0], lpu->ready_at[1]);
    }
}

static void lpu_reset(void *opaque)
{
    Best1502pLpu *lpu = opaque;
    lpu->config = 0;
    lpu->ready_at[0] = lpu->ready_at[1] = 0;
    lpu->traces = 0;
}

static const MemoryRegionOps lpu_ops = {
    .read = lpu_read,
    .write = lpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void best1502p_lpu_init(Best1502pLpu *lpu, MemoryRegion *memory)
{
    memset(lpu, 0, sizeof(*lpu));
    memory_region_init_io(&lpu->region, NULL, &lpu_ops, lpu,
                          "best1502p.cmu-lpu", 4);
    memory_region_add_subregion_overlap(memory, 0x4000008c, &lpu->region, 3);
    qemu_register_reset(lpu_reset, lpu);
}
