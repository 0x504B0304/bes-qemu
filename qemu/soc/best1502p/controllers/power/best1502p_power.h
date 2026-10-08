#ifndef BEST1502P_POWER_H
#define BEST1502P_POWER_H

#include <stdbool.h>
#include <stdint.h>
#include "exec/memory.h"

typedef void (*Best1502pPowerResetRequest)(void *opaque, uint32_t reason);

typedef struct Best1502pPower {
    unsigned shutdown_stage;
    bool poweroff_requested;
    bool persistent_reset_reason;
    uint32_t aon_status;
    Best1502pPowerResetRequest reset_request;
    void *reset_opaque;
} Best1502pPower;

#define BEST1502P_POWER_REASON_SOFTWARE_OFF 1u

void best1502p_power_init(Best1502pPower *power);
void best1502p_power_reset(Best1502pPower *power);
void best1502p_power_set_reset_request(Best1502pPower *power,
                                       Best1502pPowerResetRequest request,
                                       void *opaque);
void best1502p_power_observe_ispi_write(Best1502pPower *power,
                                        unsigned chip_select,
                                        unsigned page, unsigned reg,
                                        uint16_t data);
bool best1502p_power_read(Best1502pPower *power, hwaddr offset,
                          unsigned size, uint64_t *result);
bool best1502p_power_write(Best1502pPower *power, hwaddr offset,
                           unsigned size, uint64_t value);

#endif
