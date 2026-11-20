/* Host motion state shared by emulated IMU parts.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "miband_imu_motion.h"

/* Bounds are far outside any wearable full scale; they only reject
 * malformed host input before it reaches a register encoding. */
#define MOTION_ACCEL_LIMIT (64.0 * MIBAND_IMU_STANDARD_GRAVITY)
#define MOTION_GYRO_LIMIT  100.0

void miband_imu_motion_init(MiBandImuMotion *motion)
{
    memset(motion, 0, sizeof(*motion));
    motion->accel[2] = MIBAND_IMU_STANDARD_GRAVITY;
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

void miband_imu_motion_add_properties(Object *obj, MiBandImuMotion *motion)
{
    object_property_add(obj, "motion", "str", motion_get, motion_set,
                        NULL, motion);
    object_property_add_uint64_ptr(obj, "motion-updates", &motion->updates,
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

double miband_imu_accel_mg(const MiBandImuMotion *motion, unsigned axis)
{
    return motion->accel[axis] * 1000.0 / MIBAND_IMU_STANDARD_GRAVITY;
}

double miband_imu_gyro_mdps(const MiBandImuMotion *motion, unsigned axis)
{
    return motion->gyro[axis] * 180000.0 / G_PI;
}
