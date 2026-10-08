/* One host keyboard handler, with independent board-selected key sinks.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "miband_keyboard.h"

static struct {
    DeviceState *dev;
    MiBandKeySink sink;
} key_sinks[Q_KEY_CODE__MAX];
static QemuInputHandlerState *keyboard_handler;
static unsigned sink_count;

static void keyboard_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    InputKeyEvent *event = evt->u.key.data;
    QKeyCode key = qemu_input_key_value_to_qcode(event->key);
    if (key < Q_KEY_CODE__MAX && key_sinks[key].sink) {
        key_sinks[key].sink(key_sinks[key].dev, event->down);
    }
}

static const QemuInputHandler handler = {
    .name = "Board keyboard bridge", .mask = INPUT_EVENT_MASK_KEY,
    .event = keyboard_event,
};

bool miband_keyboard_register(DeviceState *dev, QKeyCode key,
                               MiBandKeySink sink, Error **errp)
{
    if (key >= Q_KEY_CODE__MAX || !sink || key_sinks[key].dev) {
        error_setg(errp, "board keyboard key is invalid or already connected");
        return false;
    }
    key_sinks[key].dev = dev;
    key_sinks[key].sink = sink;
    sink_count++;
    if (!keyboard_handler) {
        keyboard_handler = qemu_input_handler_register(NULL, &handler);
        qemu_input_handler_activate(keyboard_handler);
    }
    return true;
}

void miband_keyboard_unregister(DeviceState *dev)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(key_sinks); i++) {
        if (key_sinks[i].dev == dev) {
            key_sinks[i].sink(dev, false);
            key_sinks[i].dev = NULL;
            key_sinks[i].sink = NULL;
            sink_count--;
        }
    }
    if (!sink_count && keyboard_handler) {
        qemu_input_handler_unregister(keyboard_handler);
        keyboard_handler = NULL;
    }
}
