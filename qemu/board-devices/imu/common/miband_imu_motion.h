/* Host motion state shared by emulated IMU parts.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_IMU_MOTION_H
#define MIBAND_IMU_MOTION_H

#include "qom/object.h"

#define MIBAND_IMU_STANDARD_GRAVITY 9.80665

/* Physical body-frame motion supplied by the host, in SI units: specific
 * force in m/s^2 (gravity included, +Z up when flat) and angular rate in
 * rad/s. Each part converts this to its own full-scale encoding. */
typedef struct MiBandImuMotion {
    double accel[3];
    double gyro[3];
    uint64_t updates;
} MiBandImuMotion;

void miband_imu_motion_init(MiBandImuMotion *motion);
/* Adds "motion" (read/write "ax ay az gx gy gz") and "motion-updates". */
void miband_imu_motion_add_properties(Object *obj, MiBandImuMotion *motion);
bool miband_imu_motion_parse(MiBandImuMotion *motion, const char *text,
                             Error **errp);
/* Saturating conversion of a physical value to a signed 16-bit count. */
int16_t miband_imu_counts(double value, double units_per_lsb);
double miband_imu_accel_mg(const MiBandImuMotion *motion, unsigned axis);
double miband_imu_gyro_mdps(const MiBandImuMotion *motion, unsigned axis);

#endif
