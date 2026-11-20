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
    /* Previous host sample and virtual arrival times, for interpolation. */
    double prev_accel[3];
    double prev_gyro[3];
    int64_t updated_ns, prev_updated_ns;
} MiBandImuMotion;

/* Host magnetic flux density in the same body frame, in microtesla (the
 * Android TYPE_MAGNETIC_FIELD unit). The default is a typical mid-latitude
 * northern field for a flat device whose +Y axis points north. */
typedef struct MiBandMagField {
    double ut[3];
    uint64_t updates;
} MiBandMagField;

void miband_mag_field_init(MiBandMagField *field);
/* Adds "field" (read/write "mx my mz") and "field-updates". */
void miband_mag_field_add_properties(Object *obj, MiBandMagField *field);

void miband_imu_motion_init(MiBandImuMotion *motion);
/* Adds "motion" (read/write "ax ay az gx gy gz"), "motion-updates" and the
 * read-only guest clock "virtual-ns". */
void miband_imu_motion_add_properties(Object *obj, MiBandImuMotion *motion);
bool miband_imu_motion_parse(MiBandImuMotion *motion, const char *text,
                             Error **errp);
/* Saturating conversion of a physical value to a signed 16-bit count. */
int16_t miband_imu_counts(double value, double units_per_lsb);
/* Values at the current virtual time. Host updates arrive at 25..50 Hz
 * with jitter while parts sample at their own ODR, so samples are linearly
 * interpolated between the last two updates, one host interval late, rather
 * than held as steps; gaps over 100 ms are taken as steps. Rhythm-based
 * firmware features (pedometer) still need a guest clock that tracks wall
 * time: under icount sleep=off it drifts, see the "virtual-ns" property. */
double miband_imu_accel_mg(const MiBandImuMotion *motion, unsigned axis);
double miband_imu_gyro_mdps(const MiBandImuMotion *motion, unsigned axis);

#endif
