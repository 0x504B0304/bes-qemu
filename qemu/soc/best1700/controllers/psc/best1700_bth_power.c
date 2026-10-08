/* BEST1700 BTH automatic power gating and retained wake controls.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_bth_power.h"

#define BTH_PSC_WRITE_KEY 0xcafe0000u
#define BTH_PSC_AUTO_ENABLE (1u << 0)
#define BTH_PSC_HW_ENABLE (1u << 1)
#define BTH_PSC_BYPASS_MEMORY (1u << 2)
#define BTH_PSC_AUTO_MASK (BTH_PSC_AUTO_ENABLE | BTH_PSC_HW_ENABLE | \
                           BTH_PSC_BYPASS_MEMORY)
/* BISR controls are intentionally excluded until their effects are modeled. */
#define BTH_PSC_MANUAL_MASK 0x000003ffu
#define BTH_PSC_CLK_STOP (1u << 0)
#define BTH_PSC_ISOLATE (1u << 1)
#define BTH_PSC_RESET (1u << 2)
#define BTH_PSC_OFF (1u << 3)
#define BTH_PSC_CLK_STOP_DR (1u << 4)
#define BTH_PSC_ISOLATE_DR (1u << 5)
#define BTH_PSC_RESET_DR (1u << 6)
#define BTH_PSC_OFF_DR (1u << 7)
#define BTH_PSC_MEMORY_OFF (1u << 8)
#define BTH_PSC_MEMORY_OFF_DR (1u << 9)

#define BTH_PSC_IDLE 1u
#define BTH_PSC_POWERUP 2u
#define BTH_PSC_POWERDOWN 3u
/* A functional sequencing boundary, not an analog settling-time estimate. */
#define BTH_PSC_TRANSITION_NS 1

static MemTxResult bth_power_unsupported(hwaddr offset, bool write,
                                         uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 BTH PSC unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 "\n", write ? "write" : "read", offset, value);
    return MEMTX_ERROR;
}

static bool bth_power_keyed(uint64_t value, uint32_t mask)
{
    return (value & ~(uint64_t)mask) == BTH_PSC_WRITE_KEY;
}

static uint32_t bth_power_wake_status(const Best1700BthPower *s)
{
    uint32_t sources = s->wake_sources;

    for (unsigned i = 0; i < BEST1700_BTH_POWER_GPIO_BANKS; i++) {
        if (s->gpio_sources[i] & s->gpio_mask[i]) {
            sources |= BEST1700_BTH_WAKE_GPIO;
        }
    }
    return sources & s->wakeup_mask;
}

static bool bth_power_control(const Best1700BthPower *s, uint32_t drive,
                              uint32_t value, bool automatic)
{
    return s->manual_control & drive ? !!(s->manual_control & value) :
                                      automatic;
}

static void bth_power_update_control(Best1700BthPower *s)
{
    bool off = bth_power_control(s, BTH_PSC_OFF_DR, BTH_PSC_OFF,
                                 s->automatic_off);
    bool reset = bth_power_control(s, BTH_PSC_RESET_DR, BTH_PSC_RESET,
                                   s->automatic_off);
    bool clock = bth_power_control(s, BTH_PSC_CLK_STOP_DR, BTH_PSC_CLK_STOP,
                                   s->automatic_off);
    bool isolated = bth_power_control(s, BTH_PSC_ISOLATE_DR, BTH_PSC_ISOLATE,
                                      s->automatic_off);
    bool memory = bth_power_control(s, BTH_PSC_MEMORY_OFF_DR,
                                    BTH_PSC_MEMORY_OFF,
                                    s->automatic_off &&
                                    !(s->auto_control & BTH_PSC_BYPASS_MEMORY));
    bool changed = off != s->off || reset != s->reset_asserted ||
                   clock != s->clock_stopped;

    s->off = off;
    s->reset_asserted = reset;
    s->clock_stopped = clock;
    s->isolated = isolated;
    s->memory_off = memory;
    if (changed && s->set_control) {
        /* The machine owns CPU-thread reset and retention-domain policy. */
        s->set_control(s->control_opaque, off, reset, clock);
    }
}

static void bth_power_schedule(Best1700BthPower *s, unsigned state)
{
    s->main_state = state;
    timer_mod(s->transition_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BTH_PSC_TRANSITION_NS);
}

static void bth_power_update_wake(Best1700BthPower *s)
{
    if (s->main_state == BTH_PSC_POWERDOWN && bth_power_wake_status(s)) {
        /* Retain an accepted wake that races the asynchronous down step,
         * even if the external level falls before that step executes. */
        s->wake_latched = true;
    }
    if (s->automatic_off && s->main_state != BTH_PSC_POWERUP &&
        (s->wake_latched || bth_power_wake_status(s))) {
        bth_power_schedule(s, BTH_PSC_POWERUP);
    }
}

static void bth_power_transition(void *opaque)
{
    Best1700BthPower *s = opaque;

    if (s->main_state == BTH_PSC_POWERDOWN) {
        s->automatic_off = true;
        bth_power_update_control(s);
        /* A previously asserted source must survive entry. Commit the down
         * state before releasing via the guest's retained reset vector. */
        bth_power_update_wake(s);
    } else if (s->main_state == BTH_PSC_POWERUP) {
        bool completed_cycle = s->automatic_off && s->automatic_cycle;

        s->automatic_off = false;
        s->automatic_cycle = false;
        s->wake_latched = false;
        s->main_state = BTH_PSC_IDLE;
        /* Publish completion before the retained CPU can resume. A cold or
         * manual power-up does not complete an automatic low-power cycle. */
        if (completed_cycle && s->set_lpu) {
            s->set_lpu(s->lpu_opaque, true);
        }
        bth_power_update_control(s);
    }
}

static bool bth_power_start_down(Best1700BthPower *s)
{
    if (s->automatic_off || s->main_state == BTH_PSC_POWERDOWN) {
        return true;
    }
    if (s->main_state != BTH_PSC_IDLE ||
        !(s->auto_control & BTH_PSC_AUTO_ENABLE)) {
        return false;
    }
    s->automatic_cycle = true;
    s->wake_latched = bth_power_wake_status(s) != 0;
    bth_power_schedule(s, BTH_PSC_POWERDOWN);
    if (s->set_lpu) {
        s->set_lpu(s->lpu_opaque, false);
    }
    return true;
}

static int bth_power_gpio_bank(hwaddr offset, bool *status)
{
    static const hwaddr mask_offsets[BEST1700_BTH_POWER_GPIO_BANKS] = {
        0x12c, 0x1b4, 0x1d8,
    };
    static const hwaddr status_offsets[BEST1700_BTH_POWER_GPIO_BANKS] = {
        0x138, 0x1b8, 0x1dc,
    };

    for (unsigned i = 0; i < BEST1700_BTH_POWER_GPIO_BANKS; i++) {
        if (offset == mask_offsets[i] || offset == status_offsets[i]) {
            *status = offset == status_offsets[i];
            return i;
        }
    }
    return -1;
}

static MemTxResult bth_power_read(void *opaque, hwaddr offset,
                                 uint64_t *value, unsigned size,
                                 MemTxAttrs attrs)
{
    Best1700BthPowerRegister *reg = opaque;
    Best1700BthPower *s = reg->power;
    bool status;
    int bank = bth_power_gpio_bank(reg->offset, &status);

    if (bank >= 0) {
        *value = status ? s->gpio_sources[bank] & s->gpio_mask[bank] :
                          s->gpio_mask[bank];
        return MEMTX_OK;
    }
    switch (reg->offset) {
    case 0x034:
        *value = s->auto_control;
        break;
    case 0x038:
        /* Only the established main states are exposed. Do not synthesize
         * analog acknowledgement, BISR completion, or sequencer substates. */
        *value = s->main_state << 8;
        break;
    case 0x03c:
    case 0x040:
        /* Model policy: accepted command strobes do not remain asserted. */
        *value = 0;
        break;
    case 0x044:
        *value = s->manual_control;
        break;
    case 0x130:
        *value = s->wakeup_mask;
        break;
    case 0x13c:
        /* Non-consuming masked input levels, not an acknowledgement. */
        *value = bth_power_wake_status(s);
        break;
    default:
        *value = 0;
        return bth_power_unsupported(reg->offset, false, 0);
    }
    return MEMTX_OK;
}

static MemTxResult bth_power_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size,
                                  MemTxAttrs attrs)
{
    Best1700BthPowerRegister *reg = opaque;
    Best1700BthPower *s = reg->power;
    bool status;
    int bank = bth_power_gpio_bank(reg->offset, &status);

    if (bank >= 0) {
        if (!status) {
            s->gpio_mask[bank] = value;
            bth_power_update_wake(s);
        }
        return MEMTX_OK;
    }
    switch (reg->offset) {
    case 0x034:
        if (!bth_power_keyed(value, BTH_PSC_AUTO_MASK)) {
            break;
        }
        s->auto_control = value & BTH_PSC_AUTO_MASK;
        if (!(s->auto_control & BTH_PSC_AUTO_ENABLE) && !s->automatic_off &&
            s->main_state == BTH_PSC_POWERDOWN) {
            timer_del(s->transition_timer);
            s->automatic_cycle = false;
            s->wake_latched = false;
            s->main_state = BTH_PSC_IDLE;
        }
        bth_power_update_control(s);
        bth_power_update_wake(s);
        return MEMTX_OK;
    case 0x038:
    case 0x13c:
        return MEMTX_OK; /* Read-only state and wake levels. */
    case 0x03c:
        if (!bth_power_keyed(value, 1)) {
            break;
        }
        if (!(value & 1) || bth_power_start_down(s)) {
            return MEMTX_OK;
        }
        break;
    case 0x040:
        if (!bth_power_keyed(value, 1)) {
            break;
        }
        if (value & 1) {
            bth_power_schedule(s, BTH_PSC_POWERUP);
        }
        return MEMTX_OK;
    case 0x044:
        if (!bth_power_keyed(value, BTH_PSC_MANUAL_MASK)) {
            break;
        }
        s->manual_control = value & BTH_PSC_MANUAL_MASK;
        bth_power_update_control(s);
        bth_power_update_wake(s);
        return MEMTX_OK;
    case 0x130:
        if (value & ~((uint64_t)BEST1700_BTH_WAKE_MASK)) {
            break;
        }
        s->wakeup_mask = value;
        bth_power_update_wake(s);
        return MEMTX_OK;
    default:
        break;
    }
    return bth_power_unsupported(reg->offset, true, value);
}

static const MemoryRegionOps bth_power_ops = {
    .read_with_attrs = bth_power_read,
    .write_with_attrs = bth_power_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_bth_power_init(Best1700BthPower *s, Object *owner)
{
    static const hwaddr offsets[BEST1700_BTH_POWER_REGISTERS] = {
        0x034, 0x038, 0x03c, 0x040, 0x044, 0x12c, 0x130, 0x138, 0x13c,
        0x1b4, 0x1b8, 0x1d8, 0x1dc,
    };

    memory_region_init(&s->iomem, owner, "best1700.bth-power",
                       BEST1700_BTH_POWER_WINDOW);
    s->transition_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       bth_power_transition, s);
    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        Best1700BthPowerRegister *reg = &s->registers[i];
        g_autofree char *name = g_strdup_printf("best1700.bth-power.%03x",
                                               (unsigned)offsets[i]);

        reg->power = s;
        reg->offset = offsets[i];
        memory_region_init_io(&reg->iomem, owner, &bth_power_ops, reg, name, 4);
        memory_region_add_subregion(&s->iomem, offsets[i], &reg->iomem);
    }
    best1700_bth_power_reset(s);
}

void best1700_bth_power_destroy(Best1700BthPower *s)
{
    if (s->transition_timer) {
        timer_del(s->transition_timer);
        timer_free(s->transition_timer);
        s->transition_timer = NULL;
    }
}

void best1700_bth_power_reset(Best1700BthPower *s)
{
    timer_del(s->transition_timer);
    s->auto_control = 0;
    s->manual_control = 0;
    s->wakeup_mask = 0;
    s->wake_sources = 0;
    memset(s->gpio_mask, 0, sizeof(s->gpio_mask));
    memset(s->gpio_sources, 0, sizeof(s->gpio_sources));
    /* Functional reset policy: powered, sequencer idle, no manual override.
     * The machine's CMU reset owner still holds a not-yet-started CPU. */
    s->main_state = BTH_PSC_IDLE;
    s->automatic_off = false;
    s->automatic_cycle = false;
    s->wake_latched = false;
    s->off = false;
    s->reset_asserted = false;
    s->clock_stopped = false;
    s->isolated = false;
    s->memory_off = false;
}

void best1700_bth_power_connect(Best1700BthPower *s, void *opaque,
                              Best1700BthPowerControlFn control)
{
    s->control_opaque = opaque;
    s->set_control = control;
}

void best1700_bth_power_connect_lpu(Best1700BthPower *s, void *opaque,
                                  Best1700BthPowerLpuFn callback)
{
    s->lpu_opaque = opaque;
    s->set_lpu = callback;
}

bool best1700_bth_power_request_sleep(Best1700BthPower *s)
{
    if ((s->auto_control & (BTH_PSC_AUTO_ENABLE | BTH_PSC_HW_ENABLE)) !=
        (BTH_PSC_AUTO_ENABLE | BTH_PSC_HW_ENABLE)) {
        return false;
    }
    return bth_power_start_down(s);
}

void best1700_bth_power_set_wake(Best1700BthPower *s, uint32_t sources)
{
    s->wake_sources = sources & BEST1700_BTH_WAKE_MASK;
    bth_power_update_wake(s);
}

void best1700_bth_power_set_gpio_wake(Best1700BthPower *s, unsigned bank,
                                    uint32_t sources)
{
    g_assert(bank < BEST1700_BTH_POWER_GPIO_BANKS);
    s->gpio_sources[bank] = sources;
    bth_power_update_wake(s);
}

bool best1700_bth_power_is_off(const Best1700BthPower *s)
{
    return s->off;
}

bool best1700_bth_power_is_reset_asserted(const Best1700BthPower *s)
{
    return s->reset_asserted;
}

bool best1700_bth_power_is_clock_stopped(const Best1700BthPower *s)
{
    return s->clock_stopped;
}

bool best1700_bth_power_is_memory_off(const Best1700BthPower *s)
{
    return s->memory_off;
}
