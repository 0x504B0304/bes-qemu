#ifndef MIBAND_POWER_H
#define MIBAND_POWER_H
#include <stdbool.h>
#include "miband_board_config.h"

typedef struct MiBandBoardPower {
    struct MiBandIspi *ispi;
    const MiBandBoardConfig *config;
} MiBandBoardPower;

void miband_board_power_changed(void *opaque, unsigned level,
                                bool charging, bool cable_changed);
void miband_board_power_reset(void *opaque);
#endif
