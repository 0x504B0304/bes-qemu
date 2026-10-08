/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "best1700_cp.h"

#define CP_CPU_RESET (1u << 6)
#define CP_CORE_WAIT (1u << 1)
#define CP_VTOR_MASK 0xffffff80u

bool best1700_cp_cpu_held(const Best1700Cp *s)
{
    return s->reset_asserted || s->core_wait;
}

static void best1700_cp_notify(Best1700Cp *s)
{
    if (s->set_reset) {
        s->set_reset(s->reset_opaque, s->reset_asserted,
                     s->core_wait, s->vtor);
    }
}

static uint32_t best1700_cp_cmu_read(void *opaque, hwaddr offset,
                                    uint32_t *value)
{
    Best1700Cp *s = opaque;

    switch (offset) {
    case 0xb4:
        *value = s->vtor;
        return CP_VTOR_MASK;
    case 0xc4:
        *value = 0;
        return CP_CPU_RESET;
    case 0xc8:
    case 0xcc:
        *value = s->reset_asserted ? 0 : CP_CPU_RESET;
        return CP_CPU_RESET;
    case 0x1a4:
        *value = s->core_wait ? CP_CORE_WAIT : 0;
        return CP_CORE_WAIT;
    default:
        return 0;
    }
}

static uint32_t best1700_cp_cmu_write(void *opaque, hwaddr offset,
                                     uint32_t value, uint32_t lane_mask)
{
    Best1700Cp *s = opaque;
    bool was_reset = s->reset_asserted;
    bool was_waiting = s->core_wait;
    uint32_t owned;

    switch (offset) {
    case 0xb4:
        owned = lane_mask & CP_VTOR_MASK;
        s->vtor = (s->vtor & ~owned) | (value & owned);
        return CP_VTOR_MASK;
    case 0xc4:
        if ((value & lane_mask & CP_CPU_RESET) && s->set_reset) {
            /* Reset is a pulse, while the release latch and wait bit retain
             * their values. The consumer must remember the assertion even
             * if it defers applying the two events to the target vCPU. */
            s->set_reset(s->reset_opaque, true, s->core_wait, s->vtor);
            best1700_cp_notify(s);
        }
        return CP_CPU_RESET;
    case 0xc8:
        if (value & lane_mask & CP_CPU_RESET) {
            s->reset_asserted = true;
        }
        owned = CP_CPU_RESET;
        break;
    case 0xcc:
        if (value & lane_mask & CP_CPU_RESET) {
            s->reset_asserted = false;
        }
        owned = CP_CPU_RESET;
        break;
    case 0x1a4:
        if (lane_mask & CP_CORE_WAIT) {
            s->core_wait = (value & CP_CORE_WAIT) != 0;
        }
        owned = CP_CORE_WAIT;
        break;
    default:
        return 0;
    }
    if (was_reset != s->reset_asserted || was_waiting != s->core_wait) {
        /* CORE_WAIT is a halt request, not a reset. Preserve both causes
         * so a running context can resume without reloading the vector. */
        best1700_cp_notify(s);
    }
    return owned;
}

Best1700AonEndpoint best1700_cp_endpoint(Best1700Cp *s)
{
    return (Best1700AonEndpoint) {
        .opaque = s,
        .read = best1700_cp_cmu_read,
        .write = best1700_cp_cmu_write,
    };
}

void best1700_cp_init(Best1700Cp *s)
{
    memset(s, 0, sizeof(*s));
    best1700_cp_reset(s);
}

void best1700_cp_reset(Best1700Cp *s)
{
    s->vtor = 0;
    s->reset_asserted = true;
    s->core_wait = true;
    best1700_cp_notify(s);
}

void best1700_cp_connect_reset(Best1700Cp *s, void *opaque,
    void (*set_reset)(void *opaque, bool reset_asserted, bool waiting,
                      uint32_t vtor))
{
    s->reset_opaque = opaque;
    s->set_reset = set_reset;
}
