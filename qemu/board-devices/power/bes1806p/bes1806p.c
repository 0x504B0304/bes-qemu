/* External PMU identity and physical PWRKEY input.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "ui/input.h"
#include "best1700_ispi_bus.h"
#include "miband_keyboard.h"

#define TYPE_BES1806P_PMU "bes1806p-pmu"
OBJECT_DECLARE_SIMPLE_TYPE(Bes1806pState, BES1806P_PMU)

#define PMU_POWER_OFF 0x4f
#define SOFT_POWER_OFF (1u << 0)
#define PMU_KEY_CONFIG 0x44
#define PMU_KEY_STATUS 0x5e
#define KEY_PRESS_ENABLE (1u << 3)
#define KEY_RELEASE_ENABLE (1u << 4)
#define KEY_PRESSED (1u << 9)
#define KEY_PRESS_PENDING (1u << 10)
#define KEY_RELEASE_PENDING (1u << 11)
#define KEY_PENDING (KEY_PRESS_PENDING | KEY_RELEASE_PENDING)

struct Bes1806pState {
    Best1700IspiSlave parent_obj;
    uint16_t key_config, pending;
    bool pressed, poweroff_requested;
    qemu_irq poweroff;
    char *key_code;
    QKeyCode key;
};

static void pmu_update_irq(Bes1806pState *s)
{
    unsigned enabled = 0;
    if (s->key_config & KEY_PRESS_ENABLE) {
        enabled |= KEY_PRESS_PENDING;
    }
    if (s->key_config & KEY_RELEASE_ENABLE) {
        enabled |= KEY_RELEASE_PENDING;
    }
    qemu_set_irq(s->parent_obj.irq, (s->pending & enabled) != 0);
}

static bool pmu_read(Best1700IspiSlave *dev, unsigned page, unsigned reg,
                      uint16_t *value)
{
    Bes1806pState *s = BES1806P_PMU(dev);
    if (page != 0) {
        return false;
    }
    switch (reg) {
    case 0: *value = 0x18f0; break;
    case 1: *value = 0x0650; break;
    case 2: *value = 0; break; /* Revision zero simulation scenario. */
    case PMU_KEY_CONFIG: *value = s->key_config; break;
    case PMU_KEY_STATUS:
        *value = s->pending | (s->pressed ? KEY_PRESSED : 0);
        break;
    default: return false;
    }
    return true;
}

static bool pmu_write(Best1700IspiSlave *dev, unsigned page, unsigned reg,
                       uint16_t value)
{
    Bes1806pState *s = BES1806P_PMU(dev);
    if (page != 0) {
        return false;
    }
    switch (reg) {
    case 0: case 1: case 2: break; /* Identity is read-only. */
    case PMU_KEY_CONFIG: s->key_config = value; break;
    case PMU_KEY_STATUS: s->pending &= ~(value & KEY_PENDING); break;
    default: return false;
    }
    pmu_update_irq(s);
    return true;
}

static void pmu_write_observed(Best1700IspiSlave *dev, unsigned page,
                               unsigned reg, uint16_t value)
{
    Bes1806pState *s = BES1806P_PMU(dev);
    /* Observe the command without owning GPADC_START or its shared backing.
     * Bit 6 has variant-dependent semantics and is not a poweroff command. */
    if (page == 0 && reg == PMU_POWER_OFF && (value & SOFT_POWER_OFF) &&
        !s->poweroff_requested) {
        s->poweroff_requested = true;
        qemu_set_irq(s->poweroff, 1);
    }
}

static void pmu_event(DeviceState *dev, bool down)
{
    Bes1806pState *s = BES1806P_PMU(dev);
    if (s->pressed == down) {
        return;
    }
    s->pressed = down;
    /* Latch edges independently of mask; ACK never alters physical level. */
    s->pending |= s->pressed ? KEY_PRESS_PENDING : KEY_RELEASE_PENDING;
    pmu_update_irq(s);
}

static void pmu_reset(DeviceState *dev)
{
    Bes1806pState *s = BES1806P_PMU(dev);
    s->key_config = 0;
    s->pending = 0;
    s->pressed = false;
    s->poweroff_requested = false;
    qemu_set_irq(s->poweroff, 0);
    pmu_update_irq(s);
}

static void pmu_realize(DeviceState *dev, Error **errp)
{
    Bes1806pState *s = BES1806P_PMU(dev);
    Error *local_err = NULL;
    s->key = s->key_code ?
        qapi_enum_parse(&QKeyCode_lookup, s->key_code, Q_KEY_CODE__MAX, NULL) :
        Q_KEY_CODE__MAX;
    if (s->key == Q_KEY_CODE__MAX) {
        error_setg(errp, "PMU PWRKEY requires a valid key-code");
        return;
    }
    best1700_ispi_slave_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    if (!miband_keyboard_register(dev, s->key, pmu_event, errp)) {
        best1700_ispi_slave_unrealize(dev);
        return;
    }
    pmu_reset(dev);
}

static void pmu_unrealize(DeviceState *dev)
{
    miband_keyboard_unregister(dev);
    pmu_reset(dev);
    best1700_ispi_slave_unrealize(dev);
}

static void pmu_init(Object *obj)
{
    Bes1806pState *s = BES1806P_PMU(obj);
    qdev_init_gpio_out_named(DEVICE(obj), &s->poweroff, "poweroff", 1);
}

static Property pmu_properties[] = {
    DEFINE_PROP_STRING("key-code", Bes1806pState, key_code),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription pmu_vmstate = {
    .name = TYPE_BES1806P_PMU, .unmigratable = true,
};

static void pmu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    Best1700IspiSlaveClass *sc = BEST1700_ISPI_SLAVE_CLASS(klass);
    sc->read = pmu_read;
    sc->write = pmu_write;
    sc->write_observed = pmu_write_observed;
    dc->desc = "BES1806P candidate PMU identity and PWRKEY";
    dc->realize = pmu_realize;
    dc->unrealize = pmu_unrealize;
    dc->reset = pmu_reset;
    dc->vmsd = &pmu_vmstate;
    device_class_set_props(dc, pmu_properties);
}

static const TypeInfo pmu_info = {
    .name = TYPE_BES1806P_PMU, .parent = TYPE_BEST1700_ISPI_SLAVE,
    .instance_size = sizeof(Bes1806pState), .instance_init = pmu_init,
    .class_init = pmu_class_init,
};
static void pmu_register_types(void) { type_register_static(&pmu_info); }
type_init(pmu_register_types)
