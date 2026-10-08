/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_CP_H
#define BEST1700_CP_H

#include "best1700_aon.h"

/* The M55C1's local TCM is distinct from M55C0 TCM. These bus aliases must
 * refer to the same M55C1 storage visible to both processors. */
#define BEST1700_CP_ITCM_AXI_BASE 0x20e00000u
#define BEST1700_CP_DTCM_AXI_BASE 0x21c00000u

typedef struct Best1700Cp {
    uint32_t vtor;
    bool reset_asserted;
    bool core_wait;
    void *reset_opaque;
    void (*set_reset)(void *opaque, bool reset_asserted, bool waiting,
                      uint32_t vtor);
} Best1700Cp;

void best1700_cp_init(Best1700Cp *s);
void best1700_cp_reset(Best1700Cp *s);
void best1700_cp_connect_reset(Best1700Cp *s, void *opaque,
    void (*set_reset)(void *opaque, bool reset_asserted, bool waiting,
                      uint32_t vtor));
Best1700AonEndpoint best1700_cp_endpoint(Best1700Cp *s);
bool best1700_cp_cpu_held(const Best1700Cp *s);

#endif
