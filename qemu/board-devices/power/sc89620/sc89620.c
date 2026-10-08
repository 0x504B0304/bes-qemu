/* Limited SC89620-compatible charger register endpoint.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "sc89620.h"

#define SC_CHARGE_CONTROL 0x16
#define SC_CHARGE_ENABLE 0x20
#define SC_RESET_CONTROL 0x17
#define SC_RESET 0x80
#define SC_STATUS 0x1e
#define SC_VBUS_ONLINE 0x40
#define SC_ID 0x38

static void sc89620_reset_registers(Sc89620State *s)
{
    /* Conservative simulation defaults, not an asserted silicon reset map. */
    memset(s->regs, 0, sizeof(s->regs));
}

static int sc89620_event(I2CSlave *dev, enum i2c_event event)
{
    if (event == I2C_START_SEND) {
        SC89620(dev)->selected = false;
    }
    return 0;
}

static int sc89620_send(I2CSlave *dev, uint8_t data)
{
    Sc89620State *s = SC89620(dev);
    uint8_t reg;

    if (!s->selected) {
        s->pointer = data;
        s->selected = true;
        return 0;
    }
    reg = s->pointer++;
    switch (reg) {
    case SC_RESET_CONTROL:
        if (data & SC_RESET) {
            sc89620_reset_registers(s);
        } else {
            s->regs[reg] = data;
        }
        return 0;
    case 0x00 ... 0x16:
    case 0x18 ... 0x1a:
    case 0x23: /* Interrupt mask. No asynchronous faults are synthesized. */
    case 0x40:
    case 0x90 ... 0x92:
        s->regs[reg] = data;
        return 0;
    case 0x1b ... 0x22:
    case 0x24 ... 0x2b:
    case SC_ID:
        return 0; /* Read-only status/measurement/identity. */
    default:
        qemu_log_mask(LOG_UNIMP, "sc89620: unmodeled register write 0x%02x\n", reg);
        return -1;
    }
}

static uint8_t sc89620_recv(I2CSlave *dev)
{
    Sc89620State *s = SC89620(dev);
    uint8_t reg = s->pointer++;

    switch (reg) {
    case SC_ID:
        return 0x58;
    case SC_STATUS:
        /* Phase 1 is within the firmware's charging-state group (1..5).
         * Detailed analog charge phases are outside this subset. */
        return s->online ? SC_VBUS_ONLINE |
            ((s->regs[SC_CHARGE_CONTROL] & SC_CHARGE_ENABLE) ? 0x08 : 0) : 0;
    case 0x29:
        /* The guest converts this ADC byte as 18 * (raw + 1) millivolts. */
        return s->voltage_mv / 18 - 1;
    case 0x00 ... 0x1d:
    case 0x1f ... 0x28:
    case 0x2a ... 0x2b:
    case 0x40:
    case 0x90 ... 0x92:
        return s->regs[reg];
    default:
        qemu_log_mask(LOG_UNIMP, "sc89620: unmodeled register read 0x%02x\n", reg);
        return 0xff;
    }
}

static void sc89620_reset(DeviceState *dev)
{
    Sc89620State *s = SC89620(dev);

    sc89620_reset_registers(s);
    s->pointer = 0;
    s->selected = false;
}

static void sc89620_realize(DeviceState *dev, Error **errp)
{
    Sc89620State *s = SC89620(dev);

    if (s->voltage_mv < 18 || s->voltage_mv > 4608) {
        error_setg(errp, "sc89620: voltage-mv must be in 18..4608");
    }
}

static Property sc89620_properties[] = {
    DEFINE_PROP_UINT32("voltage-mv", Sc89620State, voltage_mv, 4000),
    DEFINE_PROP_BOOL("online", Sc89620State, online, false),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_sc89620 = {
    .name = "sc89620",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Sc89620State),
        VMSTATE_UINT8_ARRAY(regs, Sc89620State, 256),
        VMSTATE_UINT8(pointer, Sc89620State),
        VMSTATE_BOOL(selected, Sc89620State),
        VMSTATE_END_OF_LIST()
    },
};

static void sc89620_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);

    dc->desc = "SC89620-compatible charger (limited register model)";
    dc->realize = sc89620_realize;
    dc->reset = sc89620_reset;
    dc->vmsd = &vmstate_sc89620;
    device_class_set_props(dc, sc89620_properties);
    ic->event = sc89620_event;
    ic->send = sc89620_send;
    ic->recv = sc89620_recv;
}

static const TypeInfo sc89620_info = {
    .name = TYPE_SC89620,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Sc89620State),
    .class_init = sc89620_class_init,
};

static void sc89620_register_types(void)
{
    type_register_static(&sc89620_info);
}

type_init(sc89620_register_types)
