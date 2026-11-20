/* QST QMC6308 3-axis magnetometer on I2C: chip ID, mode/ODR/range control
 * and DRDY-latched output registers. Measurements are taken on the
 * virtual clock from the host "field" property (microtesla).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "miband_imu_motion.h"

#define TYPE_QMC6308 "qmc6308"
OBJECT_DECLARE_SIMPLE_TYPE(Qmc6308State, QMC6308)

#define REG_CHIP_ID   0x00
#define REG_DATA_X_L  0x01
#define REG_DATA_Z_H  0x06
#define REG_STATUS    0x09
#define REG_CTRL1     0x0a
#define REG_CTRL2     0x0b
#define REG_COUNT     0x40

#define CHIP_ID       0x80
#define STATUS_DRDY   (1u << 0)
#define STATUS_OVFL   (1u << 1)
#define CTRL2_RESET   (1u << 7)

enum { MODE_SUSPEND, MODE_NORMAL, MODE_SINGLE, MODE_CONTINUOUS };

struct Qmc6308State {
    I2CSlave parent_obj;
    uint8_t regs[REG_COUNT];
    uint8_t pointer;
    bool pointer_set;
    int64_t next_ns;
    MiBandMagField field;
    uint64_t measurements;
    uint32_t odr_mhz;
};

static unsigned qmc6308_mode(Qmc6308State *s)
{
    return s->regs[REG_CTRL1] & 3;
}

/* Normal mode samples at ODR 10/50/100/200 Hz; continuous mode runs as
 * fast as the oversampling allows and is bounded here at 200 Hz. */
static uint32_t qmc6308_rate_mhz(Qmc6308State *s)
{
    static const uint32_t rates[] = { 10000, 50000, 100000, 200000 };

    switch (qmc6308_mode(s)) {
    case MODE_NORMAL:
        return rates[(s->regs[REG_CTRL1] >> 2) & 3];
    case MODE_CONTINUOUS:
        return 200000;
    default:
        return 0;
    }
}

static void qmc6308_measure(Qmc6308State *s)
{
    /* Range 30/12/8/2 G -> 1000/2500/3750/15000 LSB per gauss (100 uT). */
    static const double lsb_per_gauss[] = { 1000, 2500, 3750, 15000 };
    double ut_per_lsb = 100.0 / lsb_per_gauss[(s->regs[REG_CTRL2] >> 2) & 3];
    bool overflow = false;

    for (unsigned axis = 0; axis < 3; axis++) {
        int16_t value = miband_imu_counts(s->field.ut[axis], ut_per_lsb);

        overflow |= value == INT16_MAX || value == INT16_MIN;
        stw_le_p(s->regs + REG_DATA_X_L + axis * 2, value);
    }
    s->regs[REG_STATUS] = STATUS_DRDY | (overflow ? STATUS_OVFL : 0);
    s->measurements++;
}

/* Measurements are produced lazily when the guest looks at the device:
 * the latest one due on the virtual clock is latched, like a part that
 * kept converting while nobody read it. */
static void qmc6308_catch_up(Qmc6308State *s)
{
    uint32_t rate = qmc6308_rate_mhz(s);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t period;

    if (!rate || now < s->next_ns) {
        return;
    }
    period = 1000000000000LL / rate;
    qmc6308_measure(s);
    s->next_ns = MAX(s->next_ns + period, now - now % period + period);
}

static void qmc6308_registers_reset(Qmc6308State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[REG_CHIP_ID] = CHIP_ID;
    s->pointer = 0;
    s->pointer_set = false;
    s->next_ns = 0;
    s->odr_mhz = 0;
}

static void qmc6308_write(Qmc6308State *s, uint8_t reg, uint8_t value)
{
    if (reg >= REG_COUNT || reg == REG_CHIP_ID ||
        (reg >= REG_DATA_X_L && reg <= REG_STATUS)) {
        return;                              /* read-only or unmapped */
    }
    if (reg == REG_CTRL2 && (value & CTRL2_RESET)) {
        qmc6308_registers_reset(s);
        return;
    }
    s->regs[reg] = value;
    if (reg == REG_CTRL1) {
        s->odr_mhz = qmc6308_rate_mhz(s);
        s->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     (s->odr_mhz ? 1000000000000LL / s->odr_mhz : 0);
        if (qmc6308_mode(s) == MODE_SINGLE) {
            /* One conversion, then the part returns to suspend. */
            qmc6308_measure(s);
            s->regs[REG_CTRL1] &= ~3;
        }
    }
}

static int qmc6308_event(I2CSlave *dev, enum i2c_event event)
{
    Qmc6308State *s = QMC6308(dev);

    if (event == I2C_START_SEND) {
        s->pointer_set = false;
    } else if (event == I2C_START_RECV) {
        qmc6308_catch_up(s);
    }
    return 0;
}

static int qmc6308_send(I2CSlave *dev, uint8_t value)
{
    Qmc6308State *s = QMC6308(dev);

    if (!s->pointer_set) {
        s->pointer = value;
        s->pointer_set = true;
        return 0;
    }
    qmc6308_write(s, s->pointer, value);
    s->pointer = (s->pointer + 1) % REG_COUNT;
    return 0;
}

static uint8_t qmc6308_recv(I2CSlave *dev)
{
    Qmc6308State *s = QMC6308(dev);
    uint8_t reg = s->pointer % REG_COUNT;
    uint8_t value = s->regs[reg];

    if (reg >= REG_DATA_X_L && reg <= REG_DATA_Z_H) {
        s->regs[REG_STATUS] &= ~STATUS_DRDY;  /* data read clears DRDY */
    }
    s->pointer = (reg + 1) % REG_COUNT;
    return value;
}

static void qmc6308_reset(DeviceState *dev)
{
    qmc6308_registers_reset(QMC6308(dev));
}

static void qmc6308_init(Object *obj)
{
    Qmc6308State *s = QMC6308(obj);

    miband_mag_field_init(&s->field);
    miband_mag_field_add_properties(obj, &s->field);
    object_property_add_uint64_ptr(obj, "measurements", &s->measurements,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "odr-mhz", &s->odr_mhz, OBJ_PROP_FLAG_READ);
}

static const VMStateDescription vmstate_qmc6308 = {
    .name = TYPE_QMC6308,
    .unmigratable = true,
};

static void qmc6308_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);

    dc->desc = "QST QMC6308 3-axis magnetometer";
    dc->reset = qmc6308_reset;
    dc->vmsd = &vmstate_qmc6308;
    ic->event = qmc6308_event;
    ic->send = qmc6308_send;
    ic->recv = qmc6308_recv;
}

static const TypeInfo qmc6308_info = {
    .name = TYPE_QMC6308,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Qmc6308State),
    .instance_init = qmc6308_init,
    .class_init = qmc6308_class_init,
};

static void qmc6308_register_types(void)
{
    type_register_static(&qmc6308_info);
}

type_init(qmc6308_register_types)
