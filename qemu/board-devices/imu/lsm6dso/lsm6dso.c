/* ST LSM6DSO 6-axis IMU on 4-wire SPI: register file, tagged FIFO, INT1.
 * Samples are generated on the virtual clock at the guest-programmed ODR
 * from the latest host motion (see miband_imu_motion.h).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "miband_imu_motion.h"

#define TYPE_LSM6DSO "lsm6dso"
OBJECT_DECLARE_SIMPLE_TYPE(Lsm6dsoState, LSM6DSO)

#define REG_FUNC_CFG_ACCESS 0x01
#define REG_PIN_CTRL        0x02
#define REG_FIFO_CTRL1      0x07
#define REG_FIFO_CTRL2      0x08
#define REG_FIFO_CTRL3      0x09
#define REG_FIFO_CTRL4      0x0a
#define REG_COUNTER_BDR1    0x0b
#define REG_INT1_CTRL       0x0d
#define REG_INT2_CTRL       0x0e
#define REG_WHO_AM_I        0x0f
#define REG_CTRL1_XL        0x10
#define REG_CTRL2_G         0x11
#define REG_CTRL3_C         0x12
#define REG_CTRL4_C         0x13
#define REG_CTRL8_XL        0x17
#define REG_CTRL9_XL        0x18
#define REG_CTRL10_C        0x19
#define REG_STATUS          0x1e
#define REG_OUT_TEMP_L      0x20
#define REG_OUTX_L_G        0x22
#define REG_OUTX_L_A        0x28
#define REG_FIFO_STATUS1    0x3a
#define REG_FIFO_STATUS2    0x3b
#define REG_TIMESTAMP0      0x40
#define REG_TIMESTAMP2      0x42
#define REG_FIFO_TAG        0x78
#define REG_FIFO_Z_H        0x7e

#define EMB_PAGE_SEL        0x02
#define EMB_PAGE_ADDRESS    0x08
#define EMB_PAGE_VALUE      0x09
#define EMB_PAGE_RW         0x17

#define WHO_AM_I_VALUE      0x6c
#define CTRL3_BOOT          (1u << 7)
#define CTRL3_H_LACTIVE     (1u << 5)
#define CTRL3_IF_INC        (1u << 2)
#define CTRL3_SW_RESET      (1u << 0)
#define CTRL4_SLEEP_G       (1u << 6)
#define CTRL8_XL_FS_MODE    (1u << 1)
#define CTRL10_TIMESTAMP_EN (1u << 5)
#define COUNTER_BDR_PULSED  (1u << 7)
#define STATUS_XLDA         (1u << 0)
#define STATUS_GDA          (1u << 1)
#define FIFO_STOP_ON_WTM    (1u << 7)
#define FIFO2_WTM_IA        (1u << 7)
#define FIFO2_OVR_IA        (1u << 6)
#define FIFO2_FULL_IA       (1u << 5)
#define FIFO2_OVR_LATCHED   (1u << 3)
#define INT1_DRDY_XL        (1u << 0)
#define INT1_DRDY_G         (1u << 1)
#define INT1_FIFO_TH        (1u << 3)
#define INT1_FIFO_OVR       (1u << 4)
#define INT1_FIFO_FULL      (1u << 5)
#define FUNC_CFG_EMBEDDED   (1u << 7)
#define FUNC_CFG_SHUB       (1u << 6)
#define PAGE_READ           (1u << 5)
#define PAGE_WRITE          (1u << 6)

#define TAG_GYRO            0x01
#define TAG_ACCEL           0x02
#define TAG_TIMESTAMP       0x04

#define FIFO_WORDS          512
#define TIMESTAMP_LSB_NS    25000
#define MAX_CATCH_UP_NS     (500 * SCALE_MS)

enum { SENSOR_XL, SENSOR_G, SENSOR_COUNT };

typedef struct Lsm6dsoWord {
    uint8_t bytes[7];
} Lsm6dsoWord;

struct Lsm6dsoState {
    SSIPeripheral parent_obj;

    uint8_t regs[128];
    uint8_t embedded[128];
    uint8_t sensor_hub[128];
    uint8_t pages[16][256];

    /* SPI transaction */
    unsigned frame;
    uint8_t address;
    bool reading;

    /* FIFO */
    Lsm6dsoWord fifo[FIFO_WORDS];
    unsigned fifo_head, fifo_count;
    Lsm6dsoWord fifo_out;
    bool fifo_overrun;
    uint8_t tag_count;
    unsigned timestamp_decimation;

    /* sampling */
    QEMUTimer *timer;
    int64_t next_ns[SENSOR_COUNT];
    unsigned batch_phase[SENSOR_COUNT];
    int64_t timestamp_base_ns;
    MiBandImuMotion motion;

    qemu_irq int1;
    bool int1_level;
    DeviceState *irq_target;
    uint32_t irq_pin;

    uint64_t samples, fifo_words, fifo_overruns;
    uint32_t accel_odr_mhz, gyro_odr_mhz;
};

/* ODR/BDR codes 1..10 share one table; code 11 is 1.6 Hz (XL only). */
static uint32_t lsm6dso_rate_mhz(unsigned code, bool accel)
{
    static const uint32_t rates[] = {
        0, 12500, 26000, 52000, 104000, 208000, 416000, 833000,
        1666000, 3332000, 6667000,
    };
    if (code < ARRAY_SIZE(rates)) {
        return rates[code];
    }
    return code == 11 && accel ? 1600 : 0;
}

static uint32_t lsm6dso_odr_mhz(Lsm6dsoState *s, unsigned sensor)
{
    if (sensor == SENSOR_XL) {
        return lsm6dso_rate_mhz(s->regs[REG_CTRL1_XL] >> 4, true);
    }
    if (s->regs[REG_CTRL4_C] & CTRL4_SLEEP_G) {
        return 0;
    }
    return lsm6dso_rate_mhz(s->regs[REG_CTRL2_G] >> 4, false);
}

static uint32_t lsm6dso_bdr_mhz(Lsm6dsoState *s, unsigned sensor)
{
    uint8_t ctrl3 = s->regs[REG_FIFO_CTRL3];
    return sensor == SENSOR_XL ? lsm6dso_rate_mhz(ctrl3 & 0xf, true)
                               : lsm6dso_rate_mhz(ctrl3 >> 4, false);
}

static unsigned lsm6dso_fifo_mode(Lsm6dsoState *s)
{
    return s->regs[REG_FIFO_CTRL4] & 7;
}

static bool lsm6dso_fifo_collecting(Lsm6dsoState *s)
{
    /* 1 FIFO, 3 continuous-to-FIFO, 6 continuous. Event-triggered
     * bypass modes (4, 7) stay in bypass: no trigger events are modeled. */
    unsigned mode = lsm6dso_fifo_mode(s);
    return mode == 1 || mode == 3 || mode == 6;
}

static unsigned lsm6dso_fifo_watermark(Lsm6dsoState *s)
{
    return s->regs[REG_FIFO_CTRL1] | ((s->regs[REG_FIFO_CTRL2] & 1) << 8);
}

static unsigned lsm6dso_fifo_capacity(Lsm6dsoState *s)
{
    unsigned wtm = lsm6dso_fifo_watermark(s);
    if ((s->regs[REG_FIFO_CTRL2] & FIFO_STOP_ON_WTM) && wtm) {
        return MIN(wtm, FIFO_WORDS);
    }
    return FIFO_WORDS;
}

static uint8_t lsm6dso_fifo_status2(Lsm6dsoState *s)
{
    unsigned wtm = lsm6dso_fifo_watermark(s);
    uint8_t value = (s->fifo_count >> 8) & 3;

    if (wtm && s->fifo_count >= wtm) {
        value |= FIFO2_WTM_IA;
    }
    if (s->fifo_count >= lsm6dso_fifo_capacity(s)) {
        value |= FIFO2_FULL_IA;
        if (s->fifo_overrun) {
            value |= FIFO2_OVR_IA;
        }
    }
    if (s->fifo_overrun) {
        value |= FIFO2_OVR_LATCHED;
    }
    return value;
}

static void lsm6dso_update_irq(Lsm6dsoState *s)
{
    uint8_t ctrl = s->regs[REG_INT1_CTRL];
    uint8_t fifo = lsm6dso_fifo_status2(s);
    bool pulsed = s->regs[REG_COUNTER_BDR1] & COUNTER_BDR_PULSED;
    bool active =
        ((ctrl & INT1_FIFO_TH) && (fifo & FIFO2_WTM_IA)) ||
        ((ctrl & INT1_FIFO_OVR) && (fifo & FIFO2_OVR_IA)) ||
        ((ctrl & INT1_FIFO_FULL) && (fifo & FIFO2_FULL_IA)) ||
        (!pulsed && (ctrl & INT1_DRDY_XL) && (s->regs[REG_STATUS] & STATUS_XLDA)) ||
        (!pulsed && (ctrl & INT1_DRDY_G) && (s->regs[REG_STATUS] & STATUS_GDA));
    bool level = active ^ !!(s->regs[REG_CTRL3_C] & CTRL3_H_LACTIVE);

    if (level != s->int1_level) {
        s->int1_level = level;
        qemu_set_irq(s->int1, level);
    }
}

static void lsm6dso_pulse_drdy(Lsm6dsoState *s, uint8_t mask)
{
    /* Pulsed data-ready is ~75 us on hardware; the AP GPIO bank only
     * latches edges, so an immediate assert/release is equivalent. */
    if (!(s->regs[REG_COUNTER_BDR1] & COUNTER_BDR_PULSED) ||
        !(s->regs[REG_INT1_CTRL] & mask)) {
        return;
    }
    bool low = s->regs[REG_CTRL3_C] & CTRL3_H_LACTIVE;
    qemu_set_irq(s->int1, !low);
    qemu_set_irq(s->int1, s->int1_level);
}

static void lsm6dso_fifo_clear(Lsm6dsoState *s)
{
    s->fifo_head = s->fifo_count = 0;
    s->fifo_overrun = false;
    s->timestamp_decimation = 0;
    memset(&s->fifo_out, 0, sizeof(s->fifo_out));
}

static void lsm6dso_fifo_push(Lsm6dsoState *s, uint8_t sensor_tag,
                              const uint8_t data[6])
{
    unsigned capacity = lsm6dso_fifo_capacity(s);
    unsigned tag = (sensor_tag << 3) | ((s->tag_count & 3) << 1);
    Lsm6dsoWord *word;

    if (s->fifo_count >= capacity) {
        s->fifo_overrun = true;
        s->fifo_overruns++;
        if (lsm6dso_fifo_mode(s) == 1) {
            return;                         /* FIFO mode stops when full */
        }
        s->fifo_head = (s->fifo_head + 1) % FIFO_WORDS;
        s->fifo_count--;
    }
    word = &s->fifo[(s->fifo_head + s->fifo_count) % FIFO_WORDS];
    /* TAG_PARITY makes the tag byte's population count odd. */
    word->bytes[0] = tag | !(ctpop8(tag) & 1);
    memcpy(word->bytes + 1, data, 6);
    s->fifo_count++;
    s->fifo_words++;
}

static uint32_t lsm6dso_timestamp(Lsm6dsoState *s)
{
    if (!(s->regs[REG_CTRL10_C] & CTRL10_TIMESTAMP_EN)) {
        return 0;
    }
    return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->timestamp_base_ns) /
           TIMESTAMP_LSB_NS;
}

static void lsm6dso_encode(Lsm6dsoState *s, unsigned sensor, uint8_t out[6])
{
    for (unsigned axis = 0; axis < 3; axis++) {
        int16_t value;

        if (sensor == SENSOR_XL) {
            static const double fs_mg[] = { 0.061, 0.488, 0.122, 0.244 };
            unsigned fs = (s->regs[REG_CTRL1_XL] >> 2) & 3;
            double lsb = fs_mg[fs];
            if (fs == 1 && (s->regs[REG_CTRL8_XL] & CTRL8_XL_FS_MODE)) {
                lsb = fs_mg[0];             /* XL_FS_MODE: 01 means +-2 g */
            }
            value = miband_imu_counts(miband_imu_accel_mg(&s->motion, axis), lsb);
        } else {
            static const double fs_mdps[] = { 8.75, 17.5, 35.0, 70.0 };
            uint8_t ctrl2 = s->regs[REG_CTRL2_G];
            double lsb = (ctrl2 & 2) ? 4.375 : fs_mdps[(ctrl2 >> 2) & 3];
            value = miband_imu_counts(miband_imu_gyro_mdps(&s->motion, axis), lsb);
        }
        stw_le_p(out + axis * 2, value);
    }
}

static void lsm6dso_sample(Lsm6dsoState *s, unsigned sensor, bool *batched)
{
    uint8_t data[6];
    uint32_t odr = lsm6dso_odr_mhz(s, sensor);
    uint32_t bdr = lsm6dso_bdr_mhz(s, sensor);

    lsm6dso_encode(s, sensor, data);
    memcpy(s->regs + (sensor == SENSOR_XL ? REG_OUTX_L_A : REG_OUTX_L_G), data, 6);
    s->regs[REG_STATUS] |= sensor == SENSOR_XL ? STATUS_XLDA : STATUS_GDA;
    s->samples++;
    lsm6dso_pulse_drdy(s, sensor == SENSOR_XL ? INT1_DRDY_XL : INT1_DRDY_G);

    if (!bdr || !lsm6dso_fifo_collecting(s)) {
        return;
    }
    /* Batch every ODR/BDR-th sample (BDR above ODR batches every one). */
    unsigned ratio = MAX(1, (odr + bdr / 2) / bdr);
    if (s->batch_phase[sensor]++ % ratio) {
        return;
    }
    if (!*batched) {
        unsigned decimation = (s->regs[REG_FIFO_CTRL4] >> 6) & 3;
        static const unsigned every[] = { 0, 1, 8, 32 };
        if (decimation && s->timestamp_decimation++ % every[decimation] == 0) {
            uint8_t ts[6] = { 0 };
            stl_le_p(ts, lsm6dso_timestamp(s));
            lsm6dso_fifo_push(s, TAG_TIMESTAMP, ts);
        }
        *batched = true;
    }
    lsm6dso_fifo_push(s, sensor == SENSOR_XL ? TAG_ACCEL : TAG_GYRO, data);
}

static void lsm6dso_schedule(Lsm6dsoState *s)
{
    int64_t next = INT64_MAX;

    for (unsigned sensor = 0; sensor < SENSOR_COUNT; sensor++) {
        if (lsm6dso_odr_mhz(s, sensor)) {
            next = MIN(next, s->next_ns[sensor]);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, next);
    }
}

static void lsm6dso_tick(void *opaque)
{
    Lsm6dsoState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool batched = false;

    for (unsigned sensor = 0; sensor < SENSOR_COUNT; sensor++) {
        uint32_t odr = lsm6dso_odr_mhz(s, sensor);
        int64_t period;

        if (!odr || s->next_ns[sensor] > now) {
            continue;
        }
        period = 1000000000000LL / odr;
        lsm6dso_sample(s, sensor, &batched);
        s->next_ns[sensor] += period;
        if (s->next_ns[sensor] <= now - MAX_CATCH_UP_NS) {
            /* After a long VM pause, resume cadence instead of bursting. */
            s->next_ns[sensor] = now + period;
        } else if (s->next_ns[sensor] <= now) {
            s->next_ns[sensor] = now + 1;
        }
    }
    if (batched) {
        s->tag_count++;
    }
    lsm6dso_update_irq(s);
    lsm6dso_schedule(s);
}

static void lsm6dso_rates_changed(Lsm6dsoState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t odr[SENSOR_COUNT] = {
        lsm6dso_odr_mhz(s, SENSOR_XL), lsm6dso_odr_mhz(s, SENSOR_G),
    };
    uint32_t *published[SENSOR_COUNT] = { &s->accel_odr_mhz, &s->gyro_odr_mhz };

    for (unsigned sensor = 0; sensor < SENSOR_COUNT; sensor++) {
        if (odr[sensor] && odr[sensor] != *published[sensor]) {
            s->next_ns[sensor] = now + 1000000000000LL / odr[sensor];
            s->batch_phase[sensor] = 0;
        }
        *published[sensor] = odr[sensor];
    }
    lsm6dso_schedule(s);
}

static void lsm6dso_registers_reset(Lsm6dsoState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->embedded, 0, sizeof(s->embedded));
    memset(s->sensor_hub, 0, sizeof(s->sensor_hub));
    memset(s->pages, 0, sizeof(s->pages));
    s->regs[REG_PIN_CTRL] = 0x3f;
    s->regs[REG_WHO_AM_I] = WHO_AM_I_VALUE;
    s->regs[REG_CTRL3_C] = CTRL3_IF_INC;
    s->regs[REG_CTRL9_XL] = 0xe0;
    lsm6dso_fifo_clear(s);
    s->tag_count = 0;
    s->timestamp_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->accel_odr_mhz = s->gyro_odr_mhz = 0;
    timer_del(s->timer);
}

static bool lsm6dso_read_only(uint8_t reg)
{
    return reg == REG_WHO_AM_I || (reg >= 0x1a && reg <= 0x2d) ||
           (reg >= 0x35 && reg <= 0x3b) ||
           (reg >= REG_TIMESTAMP0 && reg <= 0x43 && reg != REG_TIMESTAMP2) ||
           reg >= REG_FIFO_TAG;
}

static uint8_t *lsm6dso_bank(Lsm6dsoState *s)
{
    uint8_t access = s->regs[REG_FUNC_CFG_ACCESS];
    if (access & FUNC_CFG_EMBEDDED) {
        return s->embedded;
    }
    if (access & FUNC_CFG_SHUB) {
        return s->sensor_hub;
    }
    return s->regs;
}

static uint8_t *lsm6dso_page_cell(Lsm6dsoState *s)
{
    return &s->pages[s->embedded[EMB_PAGE_SEL] >> 4][s->embedded[EMB_PAGE_ADDRESS]];
}

static uint8_t lsm6dso_read(Lsm6dsoState *s, uint8_t reg)
{
    uint8_t *bank = lsm6dso_bank(s);
    uint8_t value;

    if (reg != REG_FUNC_CFG_ACCESS && bank == s->embedded) {
        if (reg == EMB_PAGE_VALUE && (s->embedded[EMB_PAGE_RW] & PAGE_READ)) {
            value = *lsm6dso_page_cell(s);
            s->embedded[EMB_PAGE_ADDRESS]++;
            return value;
        }
        return s->embedded[reg];
    }
    if (reg != REG_FUNC_CFG_ACCESS && bank == s->sensor_hub) {
        return s->sensor_hub[reg];
    }

    switch (reg) {
    case REG_FIFO_STATUS1:
        return s->fifo_count & 0xff;
    case REG_FIFO_STATUS2:
        value = lsm6dso_fifo_status2(s);
        s->fifo_overrun = false;             /* FIFO_OVR_LATCHED clears */
        lsm6dso_update_irq(s);
        return value;
    case REG_TIMESTAMP0 ... REG_TIMESTAMP0 + 3:
        return lsm6dso_timestamp(s) >> ((reg - REG_TIMESTAMP0) * 8);
    case REG_FIFO_TAG:
        /* Reading the tag dequeues one word; X..Z then read its data. */
        if (s->fifo_count) {
            s->fifo_out = s->fifo[s->fifo_head];
            s->fifo_head = (s->fifo_head + 1) % FIFO_WORDS;
            s->fifo_count--;
            lsm6dso_update_irq(s);
        } else {
            memset(&s->fifo_out, 0, sizeof(s->fifo_out));
        }
        return s->fifo_out.bytes[0];
    case REG_FIFO_TAG + 1 ... REG_FIFO_Z_H:
        return s->fifo_out.bytes[reg - REG_FIFO_TAG];
    }

    value = s->regs[reg];
    if (reg >= REG_OUTX_L_A && reg <= REG_OUTX_L_A + 5) {
        s->regs[REG_STATUS] &= ~STATUS_XLDA;
        lsm6dso_update_irq(s);
    } else if (reg >= REG_OUTX_L_G && reg <= REG_OUTX_L_G + 5) {
        s->regs[REG_STATUS] &= ~STATUS_GDA;
        lsm6dso_update_irq(s);
    }
    return value;
}

static void lsm6dso_write(Lsm6dsoState *s, uint8_t reg, uint8_t value)
{
    uint8_t *bank = lsm6dso_bank(s);

    if (reg != REG_FUNC_CFG_ACCESS && bank == s->embedded) {
        if (reg == EMB_PAGE_VALUE && (s->embedded[EMB_PAGE_RW] & PAGE_WRITE)) {
            *lsm6dso_page_cell(s) = value;
            s->embedded[EMB_PAGE_ADDRESS]++;
        } else {
            s->embedded[reg] = value;
        }
        return;
    }
    if (reg != REG_FUNC_CFG_ACCESS && bank == s->sensor_hub) {
        s->sensor_hub[reg] = value;
        return;
    }
    if (lsm6dso_read_only(reg)) {
        return;
    }

    switch (reg) {
    case REG_CTRL3_C:
        if (value & (CTRL3_SW_RESET | CTRL3_BOOT)) {
            /* Both self-clear; reboot also reloads trimming (no state). */
            lsm6dso_registers_reset(s);
            lsm6dso_update_irq(s);
            return;
        }
        break;
    case REG_TIMESTAMP2:
        if (value == 0xaa) {
            s->timestamp_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        return;
    case REG_FIFO_CTRL4:
        if ((value & 7) == 0) {
            lsm6dso_fifo_clear(s);
        }
        break;
    }
    s->regs[reg] = value;

    switch (reg) {
    case REG_CTRL1_XL:
    case REG_CTRL2_G:
    case REG_CTRL4_C:
    case REG_FIFO_CTRL3:
    case REG_FIFO_CTRL4:
        lsm6dso_rates_changed(s);
        break;
    }
    lsm6dso_update_irq(s);
}

static uint32_t lsm6dso_transfer(SSIPeripheral *dev, uint32_t tx)
{
    Lsm6dsoState *s = LSM6DSO(dev);
    uint8_t rx = 0;

    if (s->frame++ == 0) {
        s->reading = tx & 0x80;
        s->address = tx & 0x7f;
        return 0;
    }
    if (s->reading) {
        rx = lsm6dso_read(s, s->address);
    } else {
        lsm6dso_write(s, s->address, tx);
    }
    if (s->regs[REG_CTRL3_C] & CTRL3_IF_INC) {
        /* FIFO output reads roll over from Z_H back to the tag. */
        s->address = s->address == REG_FIFO_Z_H ? REG_FIFO_TAG
                                                : (s->address + 1) & 0x7f;
    }
    return rx;
}

static int lsm6dso_set_cs(SSIPeripheral *dev, bool line_high)
{
    LSM6DSO(dev)->frame = 0;
    return 0;
}

static void lsm6dso_reset(DeviceState *dev)
{
    Lsm6dsoState *s = LSM6DSO(dev);

    SSI_PERIPHERAL(dev)->cs = true;         /* deselected until driven */
    s->frame = 0;
    lsm6dso_registers_reset(s);
    s->int1_level = false;
    qemu_set_irq(s->int1, 0);
}

static void lsm6dso_realize(SSIPeripheral *dev, Error **errp)
{
    Lsm6dsoState *s = LSM6DSO(dev);

    if (s->irq_target) {
        if (s->irq_pin >= 96) {
            error_setg(errp, "lsm6dso irq-pin out of range");
            return;
        }
        qdev_connect_gpio_out(DEVICE(dev), 0,
            qdev_get_gpio_in_named(s->irq_target, "input", s->irq_pin));
    }
}

static void lsm6dso_init(Object *obj)
{
    Lsm6dsoState *s = LSM6DSO(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lsm6dso_tick, s);
    qdev_init_gpio_out(DEVICE(obj), &s->int1, 1);
    miband_imu_motion_init(&s->motion);
    miband_imu_motion_add_properties(obj, &s->motion);
    object_property_add_uint64_ptr(obj, "samples", &s->samples, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "fifo-words", &s->fifo_words, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "fifo-overruns", &s->fifo_overruns,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "accel-odr-mhz", &s->accel_odr_mhz,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "gyro-odr-mhz", &s->gyro_odr_mhz,
                                   OBJ_PROP_FLAG_READ);
}

static void lsm6dso_finalize(Object *obj)
{
    Lsm6dsoState *s = LSM6DSO(obj);
    timer_free(s->timer);
}

static const VMStateDescription vmstate_lsm6dso = {
    .name = TYPE_LSM6DSO,
    .unmigratable = true,
};

static Property lsm6dso_props[] = {
    DEFINE_PROP_LINK("irq-target", Lsm6dsoState, irq_target, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_UINT32("irq-pin", Lsm6dsoState, irq_pin, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void lsm6dso_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    dc->desc = "ST LSM6DSO 6-axis IMU (SPI)";
    dc->reset = lsm6dso_reset;
    dc->vmsd = &vmstate_lsm6dso;
    device_class_set_props(dc, lsm6dso_props);
    k->realize = lsm6dso_realize;
    k->transfer = lsm6dso_transfer;
    k->set_cs = lsm6dso_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo lsm6dso_info = {
    .name = TYPE_LSM6DSO,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(Lsm6dsoState),
    .instance_init = lsm6dso_init,
    .instance_finalize = lsm6dso_finalize,
    .class_init = lsm6dso_class_init,
};

static void lsm6dso_register_types(void)
{
    type_register_static(&lsm6dso_info);
}

type_init(lsm6dso_register_types)
