/* Bosch BMI2 family (BMI270, BMI220) 6-axis IMU on 4-wire SPI: register
 * file, config-blob initialisation handshake, frame FIFO and INT1/INT2.
 * Samples are generated on the virtual clock at the guest-programmed ODR
 * from the latest host motion (see miband_imu_motion.h). The on-chip
 * feature engine (step counter, gestures) is not executed: feature pages
 * are stored and read back, and feature interrupt status stays clear.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "miband_imu_motion.h"

#define TYPE_BMI2   "bmi2"
#define TYPE_BMI270 "bmi270"
#define TYPE_BMI220 "bmi220"
OBJECT_DECLARE_SIMPLE_TYPE(Bmi2State, BMI2)

#define REG_CHIP_ID         0x00
#define REG_STATUS          0x03
#define REG_ACC_X_LSB       0x0c
#define REG_GYR_X_LSB       0x12
#define REG_SENSORTIME_0    0x18
#define REG_INT_STATUS_0    0x1c
#define REG_INT_STATUS_1    0x1d
#define REG_INTERNAL_STATUS 0x21
#define REG_TEMPERATURE_0   0x22
#define REG_FIFO_LENGTH_0   0x24
#define REG_FIFO_LENGTH_1   0x25
#define REG_FIFO_DATA       0x26
#define REG_FEAT_PAGE       0x2f
#define REG_FEATURES        0x30
#define REG_ACC_CONF        0x40
#define REG_ACC_RANGE       0x41
#define REG_GYR_CONF        0x42
#define REG_GYR_RANGE       0x43
#define REG_FIFO_DOWNS      0x45
#define REG_FIFO_WTM_0      0x46
#define REG_FIFO_WTM_1      0x47
#define REG_FIFO_CONFIG_0   0x48
#define REG_FIFO_CONFIG_1   0x49
#define REG_INT1_IO_CTRL    0x53
#define REG_INT2_IO_CTRL    0x54
#define REG_INT_LATCH       0x55
#define REG_INT_MAP_DATA    0x58
#define REG_INIT_CTRL       0x59
#define REG_INIT_ADDR_0     0x5b
#define REG_INIT_ADDR_1     0x5c
#define REG_INIT_DATA       0x5e
#define REG_PWR_CONF        0x7c
#define REG_PWR_CTRL        0x7d
#define REG_CMD             0x7e

#define STATUS_DRDY_ACC     (1u << 7)
#define STATUS_DRDY_GYR     (1u << 6)
#define STATUS_CMD_RDY      (1u << 4)
#define INT1_FFULL          (1u << 0)
#define INT1_FWM            (1u << 1)
#define INT1_DRDY           (1u << 2)
#define INT_STATUS_ACC_DRDY (1u << 7)
#define INT_STATUS_GYR_DRDY (1u << 6)
#define INT_STATUS_FWM      (1u << 1)
#define INT_STATUS_FFULL    (1u << 0)
#define IO_CTRL_LVL_HIGH    (1u << 1)
#define IO_CTRL_OUTPUT_EN   (1u << 3)
#define FIFO_CFG0_STOP_FULL (1u << 0)
#define FIFO_CFG0_TIME_EN   (1u << 1)
#define FIFO_CFG1_HEADER    (1u << 4)
#define FIFO_CFG1_ACC       (1u << 6)
#define FIFO_CFG1_GYR       (1u << 7)
#define PWR_CTRL_GYR        (1u << 1)
#define PWR_CTRL_ACC        (1u << 2)
#define INIT_OK             0x01
#define CMD_SOFTRESET       0xb6
#define CMD_FIFO_FLUSH      0xb0

#define FH_ACC              0x04
#define FH_GYR              0x08
#define FH_REGULAR          0x80
#define FH_SENSORTIME       0x44
#define FH_EMPTY            0x80

#define FIFO_BYTES          6144
#define FEATURE_PAGES       8
#define SENSORTIME_NS       39063           /* 25.6 kHz, rounded */
#define MAX_CATCH_UP_NS     (500 * SCALE_MS)
#define FRAME_MAX           13

enum { SENSOR_ACC, SENSOR_GYR, SENSOR_COUNT };

typedef struct Bmi2Frame {
    uint8_t bytes[FRAME_MAX];
    uint8_t length;
} Bmi2Frame;

struct Bmi2State {
    SSIPeripheral parent_obj;

    uint8_t regs[128];
    uint8_t features[FEATURE_PAGES][16];

    unsigned frame;
    uint8_t address;
    bool reading;

    /* FIFO: whole frames queued, byte offset into the head frame. */
    Bmi2Frame fifo[FIFO_BYTES / 7];
    unsigned fifo_head, fifo_frames, fifo_bytes, head_offset;
    bool sensortime_pending;
    uint8_t sensortime_frame[4];
    unsigned sensortime_offset;

    QEMUTimer *timer;
    int64_t next_ns[SENSOR_COUNT];
    unsigned down_phase[SENSOR_COUNT];
    int64_t time_base_ns;
    MiBandImuMotion motion;

    qemu_irq irq[2];
    bool irq_level[2];
    DeviceState *irq_target;
    uint32_t irq_pin;
    uint8_t chip_id;

    uint64_t samples, fifo_frames_total, fifo_overruns, init_bytes;
    uint32_t accel_odr_mhz, gyro_odr_mhz;
};

static uint32_t bmi2_rate_mhz(unsigned code)
{
    /* ODR code n is 100 Hz * 2^(n - 8); codes 1..13 are defined. */
    if (code < 1 || code > 13) {
        return 0;
    }
    return code >= 8 ? 100000u << (code - 8) : 100000u >> (8 - code);
}

static uint32_t bmi2_odr_mhz(Bmi2State *s, unsigned sensor)
{
    if (sensor == SENSOR_ACC) {
        return (s->regs[REG_PWR_CTRL] & PWR_CTRL_ACC) ?
               bmi2_rate_mhz(s->regs[REG_ACC_CONF] & 0xf) : 0;
    }
    return (s->regs[REG_PWR_CTRL] & PWR_CTRL_GYR) ?
           bmi2_rate_mhz(s->regs[REG_GYR_CONF] & 0xf) : 0;
}

static uint32_t bmi2_sensortime(Bmi2State *s)
{
    return ((qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->time_base_ns) /
            SENSORTIME_NS) & 0xffffff;
}

static unsigned bmi2_fifo_watermark(Bmi2State *s)
{
    return s->regs[REG_FIFO_WTM_0] | ((s->regs[REG_FIFO_WTM_1] & 0x1f) << 8);
}

static void bmi2_update_irq(Bmi2State *s)
{
    uint8_t map = s->regs[REG_INT_MAP_DATA];
    uint8_t status = s->regs[REG_INT_STATUS_1];
    unsigned wtm = bmi2_fifo_watermark(s);
    bool fwm = wtm && s->fifo_bytes >= wtm;
    bool ffull = s->fifo_bytes + FRAME_MAX > FIFO_BYTES;
    bool drdy = status & (INT_STATUS_ACC_DRDY | INT_STATUS_GYR_DRDY);

    for (unsigned pin = 0; pin < 2; pin++) {
        uint8_t mapped = map >> (pin * 4);
        uint8_t io = s->regs[REG_INT1_IO_CTRL + pin];
        bool active = ((mapped & INT1_FWM) && fwm) ||
                      ((mapped & INT1_FFULL) && ffull) ||
                      ((mapped & INT1_DRDY) && drdy);
        /* A disabled output floats low; active-low idles high. */
        bool level = (io & IO_CTRL_OUTPUT_EN) &&
                     (active ^ !(io & IO_CTRL_LVL_HIGH));

        if (level != s->irq_level[pin]) {
            s->irq_level[pin] = level;
            qemu_set_irq(s->irq[pin], level);
        }
    }
}

static void bmi2_fifo_clear(Bmi2State *s)
{
    s->fifo_head = s->fifo_frames = s->fifo_bytes = s->head_offset = 0;
    s->sensortime_pending = false;
    s->sensortime_offset = 0;
}

static void bmi2_fifo_push(Bmi2State *s, const Bmi2Frame *frame)
{
    while (s->fifo_bytes + frame->length > FIFO_BYTES ||
           s->fifo_frames == ARRAY_SIZE(s->fifo)) {
        Bmi2Frame *oldest = &s->fifo[s->fifo_head];

        s->fifo_overruns++;
        if (s->regs[REG_FIFO_CONFIG_0] & FIFO_CFG0_STOP_FULL) {
            return;
        }
        s->fifo_bytes -= oldest->length - s->head_offset;
        s->head_offset = 0;
        s->fifo_head = (s->fifo_head + 1) % ARRAY_SIZE(s->fifo);
        s->fifo_frames--;
    }
    s->fifo[(s->fifo_head + s->fifo_frames) % ARRAY_SIZE(s->fifo)] = *frame;
    s->fifo_frames++;
    s->fifo_bytes += frame->length;
    s->fifo_frames_total++;
}

static uint8_t bmi2_fifo_read(Bmi2State *s)
{
    if (s->fifo_frames) {
        Bmi2Frame *frame = &s->fifo[s->fifo_head];
        uint8_t value = frame->bytes[s->head_offset++];

        s->fifo_bytes--;
        if (s->head_offset == frame->length) {
            s->head_offset = 0;
            s->fifo_head = (s->fifo_head + 1) % ARRAY_SIZE(s->fifo);
            s->fifo_frames--;
            if (!s->fifo_frames &&
                (s->regs[REG_FIFO_CONFIG_0] & FIFO_CFG0_TIME_EN) &&
                (s->regs[REG_FIFO_CONFIG_1] & FIFO_CFG1_HEADER)) {
                /* Reading the FIFO empty appends one sensortime frame. */
                uint32_t time = bmi2_sensortime(s);
                s->sensortime_frame[0] = FH_SENSORTIME;
                s->sensortime_frame[1] = time;
                s->sensortime_frame[2] = time >> 8;
                s->sensortime_frame[3] = time >> 16;
                s->sensortime_pending = true;
                s->sensortime_offset = 0;
            }
        }
        bmi2_update_irq(s);
        return value;
    }
    if (s->sensortime_pending) {
        uint8_t value = s->sensortime_frame[s->sensortime_offset++];
        if (s->sensortime_offset == sizeof(s->sensortime_frame)) {
            s->sensortime_pending = false;
        }
        return value;
    }
    return (s->regs[REG_FIFO_CONFIG_1] & FIFO_CFG1_HEADER) ? FH_EMPTY : 0;
}

static void bmi2_encode(Bmi2State *s, unsigned sensor, uint8_t out[6])
{
    for (unsigned axis = 0; axis < 3; axis++) {
        int16_t value;

        if (sensor == SENSOR_ACC) {
            /* +-2/4/8/16 g -> 16384/8192/4096/2048 LSB/g */
            double mg_per_lsb = 1000.0 / (16384 >> (s->regs[REG_ACC_RANGE] & 3));
            value = miband_imu_counts(miband_imu_accel_mg(&s->motion, axis),
                                      mg_per_lsb);
        } else {
            /* 2000/1000/500/250/125 dps -> 16.384 * 2^range LSB/dps */
            unsigned range = MIN(s->regs[REG_GYR_RANGE] & 7, 4);
            double mdps_per_lsb = 1000.0 / (16.384 * (1u << range));
            value = miband_imu_counts(miband_imu_gyro_mdps(&s->motion, axis),
                                      mdps_per_lsb);
        }
        stw_le_p(out + axis * 2, value);
    }
}

static bool bmi2_fifo_takes(Bmi2State *s, unsigned sensor)
{
    uint8_t downs = s->regs[REG_FIFO_DOWNS];
    unsigned shift = sensor == SENSOR_ACC ? (downs >> 4) & 7 : downs & 7;

    if (!(s->regs[REG_FIFO_CONFIG_1] &
          (sensor == SENSOR_ACC ? FIFO_CFG1_ACC : FIFO_CFG1_GYR))) {
        return false;
    }
    return (s->down_phase[sensor]++ & ((1u << shift) - 1)) == 0;
}

static void bmi2_tick(void *opaque)
{
    Bmi2State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t data[SENSOR_COUNT][6];
    bool fresh[SENSOR_COUNT] = { false, false };
    bool batched[SENSOR_COUNT] = { false, false };
    int64_t next = INT64_MAX;

    for (unsigned sensor = 0; sensor < SENSOR_COUNT; sensor++) {
        uint32_t odr = bmi2_odr_mhz(s, sensor);
        int64_t period;

        if (!odr || s->next_ns[sensor] > now) {
            continue;
        }
        period = 1000000000000LL / odr;
        bmi2_encode(s, sensor, data[sensor]);
        memcpy(s->regs + (sensor == SENSOR_ACC ? REG_ACC_X_LSB : REG_GYR_X_LSB),
               data[sensor], 6);
        s->regs[REG_STATUS] |= sensor == SENSOR_ACC ? STATUS_DRDY_ACC
                                                    : STATUS_DRDY_GYR;
        s->regs[REG_INT_STATUS_1] |= sensor == SENSOR_ACC ? INT_STATUS_ACC_DRDY
                                                          : INT_STATUS_GYR_DRDY;
        fresh[sensor] = true;
        batched[sensor] = bmi2_fifo_takes(s, sensor);
        s->samples++;
        s->next_ns[sensor] += period;
        if (s->next_ns[sensor] <= now - MAX_CATCH_UP_NS) {
            s->next_ns[sensor] = now + period;   /* resume after a VM pause */
        } else if (s->next_ns[sensor] <= now) {
            s->next_ns[sensor] = now + 1;
        }
    }

    if (batched[SENSOR_ACC] || batched[SENSOR_GYR]) {
        Bmi2Frame frame = { .length = 0 };
        bool header = s->regs[REG_FIFO_CONFIG_1] & FIFO_CFG1_HEADER;

        if (header) {
            frame.bytes[frame.length++] = FH_REGULAR |
                (batched[SENSOR_ACC] ? FH_ACC : 0) |
                (batched[SENSOR_GYR] ? FH_GYR : 0);
        }
        /* Payload order is aux, gyroscope, accelerometer. Headerless
         * frames always carry every FIFO-enabled sensor. */
        if (batched[SENSOR_GYR] ||
            (!header && (s->regs[REG_FIFO_CONFIG_1] & FIFO_CFG1_GYR))) {
            if (!fresh[SENSOR_GYR]) {
                memcpy(data[SENSOR_GYR], s->regs + REG_GYR_X_LSB, 6);
            }
            memcpy(frame.bytes + frame.length, data[SENSOR_GYR], 6);
            frame.length += 6;
        }
        if (batched[SENSOR_ACC] ||
            (!header && (s->regs[REG_FIFO_CONFIG_1] & FIFO_CFG1_ACC))) {
            if (!fresh[SENSOR_ACC]) {
                memcpy(data[SENSOR_ACC], s->regs + REG_ACC_X_LSB, 6);
            }
            memcpy(frame.bytes + frame.length, data[SENSOR_ACC], 6);
            frame.length += 6;
        }
        bmi2_fifo_push(s, &frame);
    }
    bmi2_update_irq(s);

    for (unsigned sensor = 0; sensor < SENSOR_COUNT; sensor++) {
        if (bmi2_odr_mhz(s, sensor)) {
            next = MIN(next, s->next_ns[sensor]);
        }
    }
    if (next != INT64_MAX) {
        timer_mod(s->timer, next);
    }
}

static void bmi2_rates_changed(Bmi2State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t odr[SENSOR_COUNT] = {
        bmi2_odr_mhz(s, SENSOR_ACC), bmi2_odr_mhz(s, SENSOR_GYR),
    };
    uint32_t *published[SENSOR_COUNT] = { &s->accel_odr_mhz, &s->gyro_odr_mhz };
    int64_t next = INT64_MAX;

    for (unsigned sensor = 0; sensor < SENSOR_COUNT; sensor++) {
        if (odr[sensor] && odr[sensor] != *published[sensor]) {
            s->next_ns[sensor] = now + 1000000000000LL / odr[sensor];
            s->down_phase[sensor] = 0;
        }
        *published[sensor] = odr[sensor];
        if (odr[sensor]) {
            next = MIN(next, s->next_ns[sensor]);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, next);
    }
}

static void bmi2_registers_reset(Bmi2State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->features, 0, sizeof(s->features));
    s->regs[REG_CHIP_ID] = s->chip_id;
    s->regs[REG_STATUS] = STATUS_CMD_RDY;
    s->regs[REG_ACC_CONF] = 0xa8;
    s->regs[REG_ACC_RANGE] = 0x02;
    s->regs[REG_GYR_CONF] = 0xa9;
    s->regs[REG_FIFO_DOWNS] = 0x88;
    s->regs[REG_FIFO_WTM_0] = 0x00;
    s->regs[REG_FIFO_WTM_1] = 0x02;
    s->regs[REG_FIFO_CONFIG_0] = FIFO_CFG0_TIME_EN;
    s->regs[REG_FIFO_CONFIG_1] = FIFO_CFG1_HEADER;
    s->regs[REG_PWR_CONF] = 0x03;
    bmi2_fifo_clear(s);
    s->time_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->accel_odr_mhz = s->gyro_odr_mhz = 0;
    timer_del(s->timer);
}

static uint8_t bmi2_read(Bmi2State *s, uint8_t reg)
{
    uint8_t value;

    switch (reg) {
    case REG_SENSORTIME_0 ... REG_SENSORTIME_0 + 2:
        return bmi2_sensortime(s) >> ((reg - REG_SENSORTIME_0) * 8);
    case REG_INT_STATUS_1:
        value = s->regs[reg];
        s->regs[reg] = 0;                    /* clear on read */
        bmi2_update_irq(s);
        return value;
    case REG_FIFO_LENGTH_0:
        return s->fifo_bytes & 0xff;
    case REG_FIFO_LENGTH_1:
        return (s->fifo_bytes >> 8) & 0x3f;
    case REG_FIFO_DATA:
        return bmi2_fifo_read(s);
    case REG_FEATURES ... REG_FEATURES + 15:
        return s->features[s->regs[REG_FEAT_PAGE] & 7][reg - REG_FEATURES];
    case REG_INIT_DATA:
        return 0;                            /* config read-back not kept */
    }
    value = s->regs[reg];
    if (reg >= REG_ACC_X_LSB && reg < REG_ACC_X_LSB + 6) {
        s->regs[REG_STATUS] &= ~STATUS_DRDY_ACC;
    } else if (reg >= REG_GYR_X_LSB && reg < REG_GYR_X_LSB + 6) {
        s->regs[REG_STATUS] &= ~STATUS_DRDY_GYR;
    }
    return value;
}

static void bmi2_write(Bmi2State *s, uint8_t reg, uint8_t value)
{
    switch (reg) {
    case REG_CHIP_ID ... REG_INTERNAL_STATUS:
    case REG_TEMPERATURE_0 ... REG_FIFO_DATA:
        return;                              /* read-only */
    case REG_FEATURES ... REG_FEATURES + 15:
        s->features[s->regs[REG_FEAT_PAGE] & 7][reg - REG_FEATURES] = value;
        return;
    case REG_INIT_DATA:
        s->init_bytes++;                     /* config blob content ignored */
        return;
    case REG_CMD:
        if (value == CMD_SOFTRESET) {
            bmi2_registers_reset(s);
            bmi2_update_irq(s);
        } else if (value == CMD_FIFO_FLUSH) {
            bmi2_fifo_clear(s);
            bmi2_update_irq(s);
        }
        return;
    }
    s->regs[reg] = value;
    switch (reg) {
    case REG_INIT_CTRL:
        /* Loading completes immediately; firmware polls the message. */
        s->regs[REG_INTERNAL_STATUS] = (value & 1) ? INIT_OK : 0;
        break;
    case REG_ACC_CONF:
    case REG_GYR_CONF:
    case REG_PWR_CTRL:
        bmi2_rates_changed(s);
        break;
    }
    bmi2_update_irq(s);
}

static uint32_t bmi2_transfer(SSIPeripheral *dev, uint32_t tx)
{
    Bmi2State *s = BMI2(dev);
    unsigned frame = s->frame++;
    uint8_t rx = 0;

    if (frame == 0) {
        s->reading = tx & 0x80;
        s->address = tx & 0x7f;
        return 0;
    }
    if (s->reading && frame == 1) {
        return 0;                            /* SPI read dummy byte */
    }
    if (s->reading) {
        rx = bmi2_read(s, s->address);
    } else {
        bmi2_write(s, s->address, tx);
    }
    /* FIFO and config data ports do not auto-increment. */
    if (s->address != REG_FIFO_DATA && s->address != REG_INIT_DATA) {
        s->address = (s->address + 1) & 0x7f;
    }
    return rx;
}

static int bmi2_set_cs(SSIPeripheral *dev, bool line_high)
{
    BMI2(dev)->frame = 0;
    return 0;
}

static void bmi2_reset(DeviceState *dev)
{
    Bmi2State *s = BMI2(dev);

    SSI_PERIPHERAL(dev)->cs = true;
    s->frame = 0;
    bmi2_registers_reset(s);
    for (unsigned pin = 0; pin < 2; pin++) {
        s->irq_level[pin] = false;
        qemu_set_irq(s->irq[pin], 0);
    }
}

static void bmi2_realize(SSIPeripheral *dev, Error **errp)
{
    Bmi2State *s = BMI2(dev);

    if (s->irq_target) {
        if (s->irq_pin >= 96) {
            error_setg(errp, "bmi2 irq-pin out of range");
            return;
        }
        qdev_connect_gpio_out(DEVICE(dev), 0,
            qdev_get_gpio_in_named(s->irq_target, "input", s->irq_pin));
    }
}

static void bmi2_init(Object *obj)
{
    Bmi2State *s = BMI2(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bmi2_tick, s);
    qdev_init_gpio_out(DEVICE(obj), s->irq, 2);
    miband_imu_motion_init(&s->motion);
    miband_imu_motion_add_properties(obj, &s->motion);
    object_property_add_uint64_ptr(obj, "samples", &s->samples, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "fifo-words", &s->fifo_frames_total,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "fifo-overruns", &s->fifo_overruns,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "init-bytes", &s->init_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "accel-odr-mhz", &s->accel_odr_mhz,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "gyro-odr-mhz", &s->gyro_odr_mhz,
                                   OBJ_PROP_FLAG_READ);
}

static void bmi2_finalize(Object *obj)
{
    timer_free(BMI2(obj)->timer);
}

static const VMStateDescription vmstate_bmi2 = {
    .name = TYPE_BMI2,
    .unmigratable = true,
};

static Property bmi2_props[] = {
    DEFINE_PROP_LINK("irq-target", Bmi2State, irq_target, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_UINT32("irq-pin", Bmi2State, irq_pin, 0),
    DEFINE_PROP_UINT8("chip-id", Bmi2State, chip_id, 0x24),
    DEFINE_PROP_END_OF_LIST(),
};

static void bmi2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    dc->desc = "Bosch BMI2 6-axis IMU (SPI)";
    dc->reset = bmi2_reset;
    dc->vmsd = &vmstate_bmi2;
    device_class_set_props(dc, bmi2_props);
    k->realize = bmi2_realize;
    k->transfer = bmi2_transfer;
    k->set_cs = bmi2_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static void bmi270_init(Object *obj)
{
    BMI2(obj)->chip_id = 0x24;
}

static void bmi220_init(Object *obj)
{
    BMI2(obj)->chip_id = 0x26;
}

static const TypeInfo bmi2_types[] = {
    {
        .name = TYPE_BMI2,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(Bmi2State),
        .instance_init = bmi2_init,
        .instance_finalize = bmi2_finalize,
        .class_init = bmi2_class_init,
    }, {
        .name = TYPE_BMI270,
        .parent = TYPE_BMI2,
        .instance_init = bmi270_init,
    }, {
        .name = TYPE_BMI220,
        .parent = TYPE_BMI2,
        .instance_init = bmi220_init,
    },
};

DEFINE_TYPES(bmi2_types)
