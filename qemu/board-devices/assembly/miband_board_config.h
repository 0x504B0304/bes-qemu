#ifndef MIBAND_BOARD_CONFIG_H
#define MIBAND_BOARD_CONFIG_H
#include <stdint.h>
#include "miband_nand_device.h"

typedef struct MiBandBoardConfig {
    const char *id;
    uint32_t width, height, framebuffer_bpp, charger_bus;
    unsigned panel_te_gpio;
    unsigned battery_channel, thermistor_mask, thermistor_mv;
    unsigned battery_divider_uv_per_mv;
    const uint32_t *curve_uv;
    const char *touch_model;
    unsigned touch_bus, touch_address, touch_irq_gpio, touch_chip_id;
    uint32_t panel_id;
    const char *config_sha256;
    unsigned nand_bus;
    MiBandNandGeometry nand;
    unsigned charger_probe_revision;
    unsigned touch_sleep_gesture;
    unsigned touch_double_tap_status;
    int touch_reset_gpio;
} MiBandBoardConfig;
#endif
