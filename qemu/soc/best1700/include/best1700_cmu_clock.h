#ifndef BEST1700_CMU_CLOCK_H
#define BEST1700_CMU_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum Best1700CmuDomain {
    BEST1700_CMU_SYS,
    BEST1700_CMU_AON,
} Best1700CmuDomain;

/* Apply a W1S/W1C clock command to an existing CMU register bank. Both
 * command words mirror the resulting state, including byte/halfword writes.
 * Return false for offsets outside the clock command ports. */
bool best1700_cmu_clock_write(uint32_t *regs, size_t regs_count,
                              Best1700CmuDomain domain, unsigned address,
                              uint64_t value, unsigned size);

/* Clear only the paired clock command words, preserving other registers. */
void best1700_cmu_clock_reset(uint32_t *regs, size_t regs_count,
                              Best1700CmuDomain domain);

#endif
