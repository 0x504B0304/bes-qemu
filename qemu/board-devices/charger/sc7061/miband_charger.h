/* Firmware-facing external charger subset; not an electrochemical model. */
#ifndef MIBAND_CHARGER_H
#define MIBAND_CHARGER_H

#include <stdbool.h>
#include <stdint.h>

#define MIBAND_CHARGER_I2C_ADDRESS 0x07u

typedef struct MiBandChargerSignals {
    void *opaque;
    void (*environment_changed)(void *, unsigned level, bool charging, bool cable_changed);
    void (*reset_irq)(void *);
} MiBandChargerSignals;

typedef struct MiBandCharger {
    MiBandChargerSignals signals;
    bool enabled;
    unsigned level;
    bool charging;
    uint8_t probe_revision;
    uint8_t registers[256];
    uint8_t pointer;
    bool expect_register;
    uint64_t state_generation;
    uint64_t read_count;
    uint64_t write_count;
    uint32_t trace_events;
    uint8_t unknown_reads[256];
    uint8_t unknown_writes[256];
} MiBandCharger;

void miband_charger_init(MiBandCharger *charger, bool enabled,
                         unsigned level, bool charging, uint8_t probe_revision);
void miband_charger_connect(MiBandCharger *charger,
                             const MiBandChargerSignals *signals);
bool miband_charger_apply(MiBandCharger *charger, unsigned level,
                          bool charging);
/* Chip reset keeps the externally supplied battery and cable conditions. */
void miband_charger_reset(MiBandCharger *charger);
/* Controller reset terminates a transaction; it does not reset the chip. */
void miband_charger_bus_reset(MiBandCharger *charger);
void miband_charger_write_command(MiBandCharger *charger, uint32_t command);
uint8_t miband_charger_read_command(MiBandCharger *charger,
                                   uint32_t command);

#endif
