#ifndef MIBAND_KEYBOARD_H
#define MIBAND_KEYBOARD_H
#include "hw/qdev-core.h"
#include "ui/input.h"

typedef void (*MiBandKeySink)(DeviceState *dev, bool down);
bool miband_keyboard_register(DeviceState *dev, QKeyCode key,
                               MiBandKeySink sink, Error **errp);
void miband_keyboard_unregister(DeviceState *dev);
#endif
