#include "qemu/osdep.h"
#include "best1700_cmu_clock.h"

typedef struct Best1700CmuClockPair {
    unsigned enable;
    unsigned disable;
    uint32_t mask;
} Best1700CmuClockPair;

static const Best1700CmuClockPair sys_pairs[] = {
    { 0x000, 0x004, UINT32_MAX }, /* HCLK */
    { 0x008, 0x00c, UINT32_MAX }, /* PCLK */
    { 0x010, 0x014, UINT32_MAX }, /* OCLK */
    { 0x060, 0x064, 0x3fffffff }, /* SYS_CLK */
    { 0x0e4, 0x0e8, 0x000000ff }, /* PLL_CLK */
    { 0x100, 0x104, UINT32_MAX }, /* DSI_CLK */
    { 0x160, 0x164, UINT32_MAX }, /* QCLK */
    { 0x178, 0x17c, UINT32_MAX }, /* XCLK */
    { 0x244, 0x248, UINT32_MAX }, /* UCLK */
};

static const Best1700CmuClockPair aon_pairs[] = {
    { 0x004, 0x008, UINT32_MAX }, /* TOP_CLK */
    { 0x040, 0x044, UINT32_MAX }, /* ACLK */
    { 0x0a0, 0x0a4, 0x007fffff }, /* TOP_CLK1 */
    { 0x180, 0x184, UINT32_MAX }, /* OCLK */
};

static const Best1700CmuClockPair *best1700_cmu_clock_pairs(
    Best1700CmuDomain domain, size_t *count)
{
    switch (domain) {
    case BEST1700_CMU_SYS:
        *count = G_N_ELEMENTS(sys_pairs);
        return sys_pairs;
    case BEST1700_CMU_AON:
        *count = G_N_ELEMENTS(aon_pairs);
        return aon_pairs;
    default:
        *count = 0;
        return NULL;
    }
}

bool best1700_cmu_clock_write(uint32_t *regs, size_t regs_count,
                              Best1700CmuDomain domain, unsigned address,
                              uint64_t value, unsigned size)
{
    const Best1700CmuClockPair *pairs;
    size_t count;
    unsigned offset = address & ~3u;
    unsigned shift = (address & 3) * 8;
    uint32_t lane_mask;

    if ((size != 1 && size != 2 && size != 4) ||
        (address & 3) + size > 4) {
        return false;
    }
    pairs = best1700_cmu_clock_pairs(domain, &count);
    lane_mask = size == 1 ? UINT8_MAX : size == 2 ? UINT16_MAX : UINT32_MAX;
    for (size_t i = 0; i < count; i++) {
        const Best1700CmuClockPair *pair = &pairs[i];
        uint32_t bits, state;

        if (offset != pair->enable && offset != pair->disable) {
            continue;
        }
        if (pair->enable / 4 >= regs_count || pair->disable / 4 >= regs_count) {
            return false;
        }
        bits = (((uint32_t)value & lane_mask) << shift) & pair->mask;
        state = regs[pair->enable / 4] & pair->mask;
        state = offset == pair->enable ? state | bits : state & ~bits;
        regs[pair->enable / 4] = state;
        regs[pair->disable / 4] = state;
        return true;
    }
    return false;
}

void best1700_cmu_clock_reset(uint32_t *regs, size_t regs_count,
                              Best1700CmuDomain domain)
{
    size_t count;
    const Best1700CmuClockPair *pairs = best1700_cmu_clock_pairs(domain, &count);

    for (size_t i = 0; i < count; i++) {
        if (pairs[i].enable / 4 < regs_count) {
            regs[pairs[i].enable / 4] = 0;
        }
        if (pairs[i].disable / 4 < regs_count) {
            regs[pairs[i].disable / 4] = 0;
        }
    }
}
