/* Limited CW221X-compatible fuel gauge, driven by an external battery scenario.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "cw221x.h"

#define CW_MODE 0x08
#define CW_GPIO_CONFIG 0x0a
#define CW_SOC_ALERT 0x0b
#define CW_PROFILE_START 0x10
#define CW_PROFILE_END 0x5f
#define CW_IC_STATE 0xa7
#define CW_READY 0x0c

static bool cw221x_ready(Cw221xState *s)
{
    return s->regs[CW_MODE] == 0 && (s->regs[CW_SOC_ALERT] & 0x80);
}

static int cw221x_event(I2CSlave *dev, enum i2c_event event)
{
    if (event == I2C_START_SEND) {
        CW221X(dev)->selected = false;
    }
    return 0;
}

static int cw221x_send(I2CSlave *dev, uint8_t data)
{
    Cw221xState *s = CW221X(dev);
    uint8_t reg;

    if (!s->selected) {
        s->pointer = data;
        s->selected = true;
        return 0;
    }
    reg = s->pointer++;
    switch (reg) {
    case CW_MODE:
        if (data != 0 && data != 0x30 && data != 0xf0) {
            qemu_log_mask(LOG_UNIMP, "cw221x: unmodeled mode 0x%02x\n", data);
            return -1;
        }
        s->regs[reg] = data;
        return 0;
    case CW_GPIO_CONFIG:
    case CW_SOC_ALERT:
    case CW_PROFILE_START ... CW_PROFILE_END:
        s->regs[reg] = data;
        return 0;
    case 0x02 ... 0x06: /* Measurements cannot be forged by register writes. */
    case 0x0e ... 0x0f:
    case 0xa4 ... 0xa7:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "cw221x: unmodeled register write 0x%02x\n", reg);
        return -1;
    }
}

static uint8_t cw221x_recv(I2CSlave *dev)
{
    Cw221xState *s = CW221X(dev);
    uint8_t reg = s->pointer++;
    bool ready = cw221x_ready(s);
    uint16_t voltage = ready ? s->voltage_mv * 16 / 5 : 0;
    uint16_t soc = ready ? s->capacity_pct * 256 : 0;

    switch (reg) {
    case 0x02:
        return voltage >> 8;
    case 0x03:
        return voltage;
    case 0x04:
        return soc >> 8;
    case 0x05:
        return soc;
    case 0x06:
        return (s->temperature_decic + 400) / 5;
    case 0x0e ... 0x0f:
        return 0; /* Static battery scenario has no modeled current flow. */
    case CW_MODE:
    case CW_GPIO_CONFIG:
    case CW_SOC_ALERT:
    case CW_PROFILE_START ... CW_PROFILE_END:
        return s->regs[reg];
    case 0xa4:
    case 0xa5:
    case 0xa6:
        /* Cycle count and SOH are retained in the guest-written profile. */
        return s->regs[0x50 + reg - 0xa4];
    case CW_IC_STATE:
        return ready ? CW_READY : 0;
    default:
        /* The firmware does not validate its diagnostic ID/version reads.
         * Leave these explicitly unknown instead of inventing a silicon ID. */
        qemu_log_mask(LOG_UNIMP, "cw221x: unmodeled register read 0x%02x\n", reg);
        return 0xff;
    }
}

static void cw221x_reset(DeviceState *dev)
{
    Cw221xState *s = CW221X(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* An unconfigured, sleeping simulation state makes the guest upload its
     * own battery profile. This is not a physical power-on register table. */
    s->regs[CW_MODE] = 0xf0;
    s->regs[0x52] = 100;
    s->pointer = 0;
    s->selected = false;
}

static void cw221x_realize(DeviceState *dev, Error **errp)
{
    Cw221xState *s = CW221X(dev);

    if (s->voltage_mv > 5000 || s->capacity_pct > 100 ||
        s->temperature_decic < -400 || s->temperature_decic > 875 ||
        s->temperature_decic % 5) {
        error_setg(errp, "cw221x: invalid battery scenario (mV, percent, 0.1 C)");
    }
}

static Property cw221x_properties[] = {
    DEFINE_PROP_UINT32("voltage-mv", Cw221xState, voltage_mv, 4000),
    DEFINE_PROP_UINT32("capacity-pct", Cw221xState, capacity_pct, 75),
    DEFINE_PROP_INT32("temperature-decic", Cw221xState, temperature_decic, 250),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_cw221x = {
    .name = "cw221x",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Cw221xState),
        VMSTATE_UINT8_ARRAY(regs, Cw221xState, 256),
        VMSTATE_UINT8(pointer, Cw221xState),
        VMSTATE_BOOL(selected, Cw221xState),
        VMSTATE_END_OF_LIST()
    },
};

static void cw221x_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);

    dc->desc = "CW221X-compatible fuel gauge (limited register model)";
    dc->realize = cw221x_realize;
    dc->reset = cw221x_reset;
    dc->vmsd = &vmstate_cw221x;
    device_class_set_props(dc, cw221x_properties);
    ic->event = cw221x_event;
    ic->send = cw221x_send;
    ic->recv = cw221x_recv;
}

static const TypeInfo cw221x_info = {
    .name = TYPE_CW221X,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Cw221xState),
    .class_init = cw221x_class_init,
};

static void cw221x_register_types(void)
{
    type_register_static(&cw221x_info);
}

type_init(cw221x_register_types)
