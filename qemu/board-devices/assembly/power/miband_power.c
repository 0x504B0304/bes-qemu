/* Board analog connections; compatibility percentage selects physical voltage.
 * The guest gauge retains its own calibration, curve and smoothing.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "miband_peripherals.h"
#include "miband_power.h"

static unsigned charger_voltage_level(const uint32_t *curve_uv, uint32_t uv)
{
    if (uv >= curve_uv[0]) {
        return 100;
    }
    if (uv <= curve_uv[100]) {
        return 0;
    }
    for (unsigned i = 1; i <= 100; i++) {
        if (uv > curve_uv[i] && uv <= curve_uv[i - 1]) {
            return 101 - i;
        }
    }
    g_assert_not_reached();
}

void miband_board_power_changed(void *opaque, unsigned level,
                                bool charging, bool cable_changed)
{
    MiBandBoardPower *power = opaque;
    const MiBandBoardConfig *config = power->config;
    const uint32_t *curve_uv = config->curve_uv;
    unsigned best_mv = 0;
    uint64_t best_score = UINT64_MAX;
    uint32_t target_uv;

    if (!power->ispi) {
        return;
    }
    if (level == 0) {
        target_uv = curve_uv[100] - config->battery_divider_uv_per_mv;
    } else if (level == 100) {
        target_uv = curve_uv[0] + config->battery_divider_uv_per_mv;
    } else {
        target_uv = (curve_uv[101 - level] +
                     curve_uv[100 - level]) / 2;
    }
    /* Prefer
     * the requested SOC interval if it is representable; otherwise select
     * its nearest voltage. Never promise one-percent resolution where the
     * firmware's table interval is narrower than the ADC conversion step. */
    for (unsigned mv = 0; mv <= 1800; mv++) {
        uint32_t uv = mv * config->battery_divider_uv_per_mv;
        unsigned curve_level = charger_voltage_level(curve_uv, uv);
        unsigned level_error = abs((int)curve_level - (int)level);
        uint32_t voltage_error = abs((int)uv - (int)target_uv);
        uint64_t score = (uint64_t)level_error * 10000000 + voltage_error;

        if (score < best_score) {
            best_score = score;
            best_mv = mv;
        }
    }
    miband_ispi_set_analog_mv(power->ispi, config->battery_channel, best_mv);
    for (unsigned i = 0; i < 7; i++) {
        if (config->thermistor_mask & (1u << i)) {
            miband_ispi_set_analog_mv(power->ispi, i, config->thermistor_mv);
        }
    }
    if (cable_changed) {
        miband_ispi_charger_changed(power->ispi, charging);
    }
    qemu_log_mask(LOG_UNIMP, "miband board power board=%s level=%u "
                  "battery_channel=%u battery_mv=%u thermistor_mask=%02x\n",
                  config->id, level, config->battery_channel, best_mv,
                  config->thermistor_mask);
}

void miband_board_power_reset(void *opaque)
{
    MiBandBoardPower *power = opaque;
    miband_ispi_charger_reset(power->ispi);
}
