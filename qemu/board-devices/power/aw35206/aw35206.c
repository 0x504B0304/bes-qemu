/* Limited AW35206-compatible load-switch register endpoint.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "aw35206.h"

#define AW_CHIP_ID 0x32
#define AW_REG_ID 0x00
#define AW_REG_ENABLE 0x02
#define AW_REG_RESET 0x69
#define AW_RESET_KEY 0xae
#define AW_ENABLE_MASK 0x3f

static int aw35206_event(I2CSlave *dev, enum i2c_event event)
{
    Aw35206State *s = AW35206(dev);

    switch (event) {
    case I2C_START_SEND:
        s->selected = false;
        s->transferred = false;
        break;
    case I2C_START_RECV:
        /* Retain the selector across the register-read repeated START. */
        s->transferred = false;
        break;
    default:
        break;
    }
    return 0;
}

static int aw35206_send(I2CSlave *dev, uint8_t data)
{
    Aw35206State *s = AW35206(dev);

    if (!s->selected) {
        s->pointer = data;
        s->selected = true;
        return 0;
    }
    if (s->transferred) {
        qemu_log_mask(LOG_UNIMP, "aw35206: burst writes are not modeled\n");
        return -1;
    }
    s->transferred = true;
    switch (s->pointer) {
    case AW_REG_ID:
        return 0; /* Read-only identity; never let guest writes forge it. */
    case AW_REG_ENABLE:
        s->enables = data & AW_ENABLE_MASK;
        return 0;
    case AW_REG_RESET:
        if (data == AW_RESET_KEY) {
            s->enables = 0;
            return 0;
        }
        qemu_log_mask(LOG_UNIMP, "aw35206: unsupported reset key 0x%02x\n", data);
        return -1;
    default:
        qemu_log_mask(LOG_UNIMP, "aw35206: unmodeled register write 0x%02x\n",
                      s->pointer);
        return -1;
    }
}

static uint8_t aw35206_recv(I2CSlave *dev)
{
    Aw35206State *s = AW35206(dev);

    if (s->transferred) {
        qemu_log_mask(LOG_UNIMP, "aw35206: burst reads are not modeled\n");
        return 0xff;
    }
    s->transferred = true;
    switch (s->pointer) {
    case AW_REG_ID:
        return AW_CHIP_ID;
    case AW_REG_ENABLE:
        return s->enables;
    default:
        qemu_log_mask(LOG_UNIMP, "aw35206: unmodeled register read 0x%02x\n",
                      s->pointer);
        return 0xff;
    }
}

static void aw35206_reset(DeviceState *dev)
{
    Aw35206State *s = AW35206(dev);

    s->pointer = 0;
    s->enables = 0;
    s->selected = s->transferred = false;
}

static const VMStateDescription vmstate_aw35206 = {
    .name = "aw35206",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Aw35206State),
        VMSTATE_UINT8(pointer, Aw35206State),
        VMSTATE_UINT8(enables, Aw35206State),
        VMSTATE_BOOL(selected, Aw35206State),
        VMSTATE_BOOL(transferred, Aw35206State),
        VMSTATE_END_OF_LIST()
    },
};

static void aw35206_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);

    dc->desc = "AW35206-compatible load switch (limited register model)";
    dc->reset = aw35206_reset;
    dc->vmsd = &vmstate_aw35206;
    ic->event = aw35206_event;
    ic->send = aw35206_send;
    ic->recv = aw35206_recv;
}

static const TypeInfo aw35206_info = {
    .name = TYPE_AW35206,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Aw35206State),
    .class_init = aw35206_class_init,
};

static void aw35206_register_types(void)
{
    type_register_static(&aw35206_info);
}

type_init(aw35206_register_types)
