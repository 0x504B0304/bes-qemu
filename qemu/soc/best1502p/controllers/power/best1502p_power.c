#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1502p_power.h"

#define BEST1502P_PMU_CS 3u
#define BEST1502P_PMU_PAGE 0u
#define PMU_REG_3048 0x48u
#define PMU_REG_304D 0x4du
#define PMU_REG_3036 0x36u
#define PMU_REG_3043 0x43u
#define PMU_REG_3037 0x37u
#define BEST1502P_AON_STATUS 0x00u
#define BEST1502P_SOFTWARE_OFF_REASON 0x02210000u

void best1502p_power_init(Best1502pPower *power)
{
    memset(power, 0, sizeof(*power));
}

void best1502p_power_reset(Best1502pPower *power)
{
    power->shutdown_stage = 0;
    power->poweroff_requested = false;
    if (power->persistent_reset_reason) {
        power->aon_status = BEST1502P_SOFTWARE_OFF_REASON;
    }
}

void best1502p_power_set_reset_request(Best1502pPower *power,
                                       Best1502pPowerResetRequest request,
                                       void *opaque)
{
    power->reset_request = request;
    power->reset_opaque = opaque;
}

void best1502p_power_observe_ispi_write(Best1502pPower *power,
                                        unsigned chip_select,
                                        unsigned page, unsigned reg,
                                        uint16_t data)
{
    if (chip_select != BEST1502P_PMU_CS || page != BEST1502P_PMU_PAGE ||
        power->poweroff_requested) {
        return;
    }

    switch (reg) {
    case PMU_REG_3048:
        if (data == 0x2000) {
            power->shutdown_stage = 1;
        }
        break;
    case PMU_REG_304D:
        if (power->shutdown_stage == 1 && data == 2) {
            power->shutdown_stage = 2;
        }
        break;
    case PMU_REG_3036:
        if (power->shutdown_stage == 2 && data == 2) {
            power->shutdown_stage = 3;
        }
        break;
    case PMU_REG_3043:
        if (power->shutdown_stage == 3 && (data & 0x6000) == 0x6000) {
            power->shutdown_stage = 4;
        }
        break;
    case PMU_REG_3037:
        if (power->shutdown_stage == 4 && (data & 0x0180) == 0x0180) {
            power->poweroff_requested = true;
            power->persistent_reset_reason = true;
            power->aon_status = BEST1502P_SOFTWARE_OFF_REASON;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "best1502p power: PMU software-off sequence complete\n");
            if (power->reset_request) {
                power->reset_request(power->reset_opaque,
                                     BEST1502P_POWER_REASON_SOFTWARE_OFF);
            }
        }
        break;
    default:
        break;
    }
}

bool best1502p_power_read(Best1502pPower *power, hwaddr offset,
                          unsigned size, uint64_t *result)
{
    if (offset != BEST1502P_AON_STATUS || size != sizeof(uint32_t)) {
        return false;
    }
    *result = power->aon_status;
    return true;
}

bool best1502p_power_write(Best1502pPower *power, hwaddr offset,
                           unsigned size, uint64_t value)
{
    if (offset != BEST1502P_AON_STATUS || size != sizeof(uint32_t)) {
        return false;
    }
    power->aon_status = (uint32_t)value;
    return true;
}
