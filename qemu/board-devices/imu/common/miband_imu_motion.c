/* Host motion state shared by emulated IMU parts.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/timer.h"
#include "miband_imu_motion.h"

/* Bounds are far outside any wearable full scale; they only reject
 * malformed host input before it reaches a register encoding. */
#define MOTION_ACCEL_LIMIT (64.0 * MIBAND_IMU_STANDARD_GRAVITY)
#define MOTION_GYRO_LIMIT  100.0
#define INTERPOLATE_MAX_NS (100 * SCALE_MS)

void miband_imu_motion_init(MiBandImuMotion *motion)
{
    memset(motion, 0, sizeof(*motion));
    motion->accel[2] = MIBAND_IMU_STANDARD_GRAVITY;
    motion->prev_accel[2] = MIBAND_IMU_STANDARD_GRAVITY;
}

bool miband_imu_motion_parse(MiBandImuMotion *motion, const char *text,
                             Error **errp)
{
    double values[6];
    const char *cursor = text;

    for (unsigned i = 0; i < 6; i++) {
        char *end;

        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t') {
            cursor++;
        }
        values[i] = g_ascii_strtod(cursor, &end);
        if (end == cursor || !isfinite(values[i]) ||
            fabs(values[i]) > (i < 3 ? MOTION_ACCEL_LIMIT : MOTION_GYRO_LIMIT)) {
            error_setg(errp, "motion expects six finite values: "
                       "ax ay az (m/s^2) gx gy gz (rad/s)");
            return false;
        }
        cursor = end;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor) {
        error_setg(errp, "motion has trailing data");
        return false;
    }
    memcpy(motion->prev_accel, motion->accel, sizeof(motion->accel));
    memcpy(motion->prev_gyro, motion->gyro, sizeof(motion->gyro));
    motion->prev_updated_ns = motion->updated_ns;
    motion->updated_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memcpy(motion->accel, values, sizeof(motion->accel));
    memcpy(motion->gyro, values + 3, sizeof(motion->gyro));
    motion->updates++;
    return true;
}

static void motion_get(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    MiBandImuMotion *motion = opaque;
    g_autofree char *text = g_strdup_printf("%.6f %.6f %.6f %.6f %.6f %.6f",
        motion->accel[0], motion->accel[1], motion->accel[2],
        motion->gyro[0], motion->gyro[1], motion->gyro[2]);
    char *value = text;

    visit_type_str(v, name, &value, errp);
}

static void motion_set(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    g_autofree char *text = NULL;

    if (!visit_type_str(v, name, &text, errp)) {
        return;
    }
    miband_imu_motion_parse(opaque, text, errp);
}

static void virtual_ns_get(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    visit_type_uint64(v, name, &now, errp);
}

void miband_imu_motion_add_properties(Object *obj, MiBandImuMotion *motion)
{
    /* Host motion is real-time input; under icount the guest clock can run
     * well away from wall time. Hosts compare this with their own clock. */
    object_property_add(obj, "virtual-ns", "uint64", virtual_ns_get, NULL, NULL, NULL);
    object_property_add(obj, "motion", "str", motion_get, motion_set,
                        NULL, motion);
    object_property_add_uint64_ptr(obj, "motion-updates", &motion->updates,
                                   OBJ_PROP_FLAG_READ);
}

/* Earth's field is 25..65 uT; 2000 uT only rejects malformed input. */
#define FIELD_LIMIT_UT 2000.0

void miband_mag_field_init(MiBandMagField *field)
{
    field->ut[0] = 0.0;
    field->ut[1] = 28.0;
    field->ut[2] = -46.0;
    field->updates = 0;
}

static void field_get(Object *obj, Visitor *v, const char *name,
                      void *opaque, Error **errp)
{
    MiBandMagField *field = opaque;
    g_autofree char *text = g_strdup_printf("%.4f %.4f %.4f",
        field->ut[0], field->ut[1], field->ut[2]);
    char *value = text;

    visit_type_str(v, name, &value, errp);
}

static void field_set(Object *obj, Visitor *v, const char *name,
                      void *opaque, Error **errp)
{
    MiBandMagField *field = opaque;
    g_autofree char *text = NULL;
    double values[3];
    const char *cursor;

    if (!visit_type_str(v, name, &text, errp)) {
        return;
    }
    cursor = text;
    for (unsigned i = 0; i < 3; i++) {
        char *end;

        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t') {
            cursor++;
        }
        values[i] = g_ascii_strtod(cursor, &end);
        if (end == cursor || !isfinite(values[i]) || fabs(values[i]) > FIELD_LIMIT_UT) {
            error_setg(errp, "field expects three finite values: mx my mz (uT)");
            return;
        }
        cursor = end;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor) {
        error_setg(errp, "field has trailing data");
        return;
    }
    memcpy(field->ut, values, sizeof(field->ut));
    field->updates++;
}

void miband_mag_field_add_properties(Object *obj, MiBandMagField *field)
{
    object_property_add(obj, "field", "str", field_get, field_set, NULL, field);
    object_property_add_uint64_ptr(obj, "field-updates", &field->updates,
                                   OBJ_PROP_FLAG_READ);
}

int16_t miband_imu_counts(double value, double units_per_lsb)
{
    double counts = round(value / units_per_lsb);

    if (counts > INT16_MAX) {
        return INT16_MAX;
    }
    if (counts < INT16_MIN) {
        return INT16_MIN;
    }
    return counts;
}

static double motion_value(const MiBandImuMotion *motion, const double *latest,
                           const double *previous, unsigned axis)
{
    int64_t interval = motion->updated_ns - motion->prev_updated_ns;
    double fraction;

    if (motion->updates < 2 || interval <= 0 || interval > INTERPOLATE_MAX_NS) {
        return latest[axis];
    }
    fraction = (double)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - motion->updated_ns) /
               interval;
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    return previous[axis] + (latest[axis] - previous[axis]) * fraction;
}

double miband_imu_accel_mg(const MiBandImuMotion *motion, unsigned axis)
{
    return motion_value(motion, motion->accel, motion->prev_accel, axis) *
           1000.0 / MIBAND_IMU_STANDARD_GRAVITY;
}

double miband_imu_gyro_mdps(const MiBandImuMotion *motion, unsigned axis)
{
    return motion_value(motion, motion->gyro, motion->prev_gyro, axis) *
           180000.0 / G_PI;
}
