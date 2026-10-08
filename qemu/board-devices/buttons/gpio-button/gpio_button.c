/* Physical button input connected to a board-selected GPIO.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "sysemu/reset.h"
#include "ui/input.h"
#include "miband_keyboard.h"

#define TYPE_MIBAND_GPIO_BUTTON "miband-gpio-button"
OBJECT_DECLARE_SIMPLE_TYPE(GpioButtonState, MIBAND_GPIO_BUTTON)

struct GpioButtonState {
    DeviceState parent_obj;
    DeviceState *target;
    uint32_t pin;
    bool active_low, pressed;
    char *key_code;
    QKeyCode key;
    qemu_irq output;
};

static void gpio_button_set(GpioButtonState *s, bool pressed)
{
    s->pressed = pressed;
    qemu_set_irq(s->output, pressed ^ s->active_low);
}

static void gpio_button_event(DeviceState *dev, bool down)
{
    GpioButtonState *s = MIBAND_GPIO_BUTTON(dev);
    gpio_button_set(s, down);
}

static void gpio_button_reset(DeviceState *dev)
{
    gpio_button_set(MIBAND_GPIO_BUTTON(dev), false);
}

static void gpio_button_system_reset(void *opaque)
{
    gpio_button_reset(DEVICE(opaque));
}

static void gpio_button_realize(DeviceState *dev, Error **errp)
{
    GpioButtonState *s = MIBAND_GPIO_BUTTON(dev);

    s->key = s->key_code ?
        qapi_enum_parse(&QKeyCode_lookup, s->key_code, Q_KEY_CODE__MAX, NULL) :
        Q_KEY_CODE__MAX;
    if (!s->target || s->pin >= 96 || s->key == Q_KEY_CODE__MAX) {
        error_setg(errp, "GPIO button requires target, pin 0..95 and a valid key-code");
        return;
    }
    if (!miband_keyboard_register(dev, s->key, gpio_button_event, errp)) {
        return;
    }
    qdev_connect_gpio_out(dev, 0, qdev_get_gpio_in_named(s->target, "input", s->pin));
    /* A discrete GPIO endpoint has no parent bus to traverse on reset. */
    qemu_register_reset(gpio_button_system_reset, dev);
    gpio_button_reset(dev);
}

static void gpio_button_unrealize(DeviceState *dev)
{
    GpioButtonState *s = MIBAND_GPIO_BUTTON(dev);
    qemu_unregister_reset(gpio_button_system_reset, dev);
    miband_keyboard_unregister(dev);
    gpio_button_reset(dev);
}

static void gpio_button_init(Object *obj)
{
    GpioButtonState *s = MIBAND_GPIO_BUTTON(obj);
    qdev_init_gpio_out(DEVICE(s), &s->output, 1);
}

static Property gpio_button_properties[] = {
    DEFINE_PROP_LINK("target", GpioButtonState, target, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT32("pin", GpioButtonState, pin, 0),
    DEFINE_PROP_BOOL("active-low", GpioButtonState, active_low, true),
    DEFINE_PROP_STRING("key-code", GpioButtonState, key_code),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_gpio_button = {
    .name = TYPE_MIBAND_GPIO_BUTTON,
    .unmigratable = true,
};

static void gpio_button_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "Board GPIO button with host keyboard input";
    dc->user_creatable = true;
    dc->hotpluggable = false;
    dc->realize = gpio_button_realize;
    dc->unrealize = gpio_button_unrealize;
    dc->reset = gpio_button_reset;
    dc->vmsd = &vmstate_gpio_button;
    device_class_set_props(dc, gpio_button_properties);
}

static const TypeInfo gpio_button_info = {
    .name = TYPE_MIBAND_GPIO_BUTTON,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(GpioButtonState),
    .instance_init = gpio_button_init,
    .class_init = gpio_button_class_init,
};

static void gpio_button_register_types(void)
{
    type_register_static(&gpio_button_info);
}
type_init(gpio_button_register_types)
