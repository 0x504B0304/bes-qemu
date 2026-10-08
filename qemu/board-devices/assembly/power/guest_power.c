/* Board power removal, separate from the guest's power-button interrupt.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"

#define TYPE_MIBAND_GUEST_POWER "miband-guest-power"
OBJECT_DECLARE_SIMPLE_TYPE(GuestPowerState, MIBAND_GUEST_POWER)

struct GuestPowerState {
    DeviceState parent_obj;
    DeviceState *source;
    bool requested;
};

static void guest_power_input(void *opaque, int n, int level)
{
    GuestPowerState *s = opaque;
    if (level && !s->requested) {
        s->requested = true;
        /* Schedule the standard main-loop shutdown; never tear down vCPUs
         * or reset peer cores from this MMIO/MTTCG callback. */
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

static void guest_power_reset(void *opaque)
{
    MIBAND_GUEST_POWER(opaque)->requested = false;
}

static void guest_power_realize(DeviceState *dev, Error **errp)
{
    GuestPowerState *s = MIBAND_GUEST_POWER(dev);
    ObjectProperty *output = s->source ?
        object_property_find(OBJECT(s->source), "poweroff[0]") : NULL;
    if (!output || strcmp(output->type, "link<irq>") ||
        qdev_get_gpio_out_connector(s->source, "poweroff", 0)) {
        error_setg(errp, "board power needs an unconnected poweroff[0] source");
        return;
    }
    qdev_connect_gpio_out_named(s->source, "poweroff", 0,
                               qdev_get_gpio_in_named(dev, "poweroff", 0));
    /* This discrete board endpoint has no parent bus reset traversal. */
    qemu_register_reset(guest_power_reset, dev);
    guest_power_reset(dev);
}

static void guest_power_unrealize(DeviceState *dev)
{
    GuestPowerState *s = MIBAND_GUEST_POWER(dev);
    qemu_unregister_reset(guest_power_reset, dev);
    qdev_connect_gpio_out_named(s->source, "poweroff", 0, NULL);
    guest_power_reset(dev);
}

static void guest_power_init(Object *obj)
{
    qdev_init_gpio_in_named(DEVICE(obj), guest_power_input, "poweroff", 1);
}

static Property guest_power_properties[] = {
    DEFINE_PROP_LINK("source", GuestPowerState, source, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};
static const VMStateDescription guest_power_vmstate = {
    .name = TYPE_MIBAND_GUEST_POWER, .unmigratable = true,
};
static void guest_power_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "Board power removal to standard guest shutdown";
    dc->hotpluggable = false;
    dc->realize = guest_power_realize;
    dc->unrealize = guest_power_unrealize;
    dc->vmsd = &guest_power_vmstate;
    device_class_set_props(dc, guest_power_properties);
}
static const TypeInfo guest_power_info = {
    .name = TYPE_MIBAND_GUEST_POWER, .parent = TYPE_DEVICE,
    .instance_size = sizeof(GuestPowerState),
    .instance_init = guest_power_init, .class_init = guest_power_class_init,
};
static void guest_power_register_types(void) { type_register_static(&guest_power_info); }
type_init(guest_power_register_types)
