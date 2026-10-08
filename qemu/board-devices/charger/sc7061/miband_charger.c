/*
 * External SC7061 charger subset observed in this firmware.
 *
 * The register bus and firmware-tested status fields are modeled here. This
 * does not emulate charge chemistry, a charge timer, or unobserved fault bits.
 * No part of this model writes the firmware's battery cache or application RAM.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "miband_charger.h"

#define I2C_STOP     (1u << 9)
#define I2C_RESTART  (1u << 10)

/* The probe revision is a board-selected silicon identity. The reference driver
 * accepts 4 or 7; reference sub_C0C9480 requires 6. */
#define SC7061_REG_PROBE   0x01
#define SC7061_REG_FAMILY  0x60
#define SC7061_REG_ID      0x00
#define SC7061_REG_STATUS  0x30
#define SC7061_REG_IRQ     0x40
#define SC7061_PG          (1u << 3)
#define SC7061_PG_IRQ      (1u << 4)
#define SC7061_CHARGING    (1u << 5)
#define SC7061_REG_CONTROL 0x1d
#define SC7061_DISABLE_CHARGING (1u << 6)

void miband_charger_connect(MiBandCharger *charger,
                             const MiBandChargerSignals *signals)
{
    charger->signals = *signals;
    signals->environment_changed(signals->opaque, charger->level,
                                  charger->charging, false);
}

static void charger_refresh_status(MiBandCharger *charger)
{
    bool charge_enabled = !(charger->registers[SC7061_REG_CONTROL] &
                            SC7061_DISABLE_CHARGING);

    charger->registers[SC7061_REG_STATUS] = charger->charging ?
        SC7061_PG | (charge_enabled ? SC7061_CHARGING : 0) : 0;
}

static void charger_refresh(MiBandCharger *charger)
{
    charger->registers[SC7061_REG_PROBE] = charger->probe_revision;
    charger->registers[SC7061_REG_FAMILY] = 0;
    charger->registers[SC7061_REG_ID] = 0x1e;
    charger_refresh_status(charger);
}

void miband_charger_bus_reset(MiBandCharger *charger)
{
    charger->expect_register = true;
}

void miband_charger_reset(MiBandCharger *charger)
{
    memset(charger->registers, 0, sizeof(charger->registers));
    charger->pointer = 0;
    miband_charger_bus_reset(charger);
    charger_refresh(charger);
    if (charger->signals.reset_irq) {
        charger->signals.reset_irq(charger->signals.opaque);
    }
}

void miband_charger_init(MiBandCharger *charger, bool enabled,
                         unsigned level, bool charging, uint8_t probe_revision)
{
    memset(charger, 0, sizeof(*charger));
    charger->enabled = enabled;
    charger->level = MIN(level, 100);
    charger->charging = charging;
    charger->probe_revision = probe_revision;
    miband_charger_reset(charger);
    qemu_log_mask(LOG_UNIMP, "miband charger model=%s level=%u charging=%u\n",
                  enabled ? "SC7061-subset" : "off", charger->level,
                  charger->charging);
}

bool miband_charger_apply(MiBandCharger *charger, unsigned level,
                          bool charging)
{
    bool cable_changed;

    if (!charger->enabled || level > 100) {
        return false;
    }
    cable_changed = charger->charging != charging;
    if (cable_changed) {
        charger->registers[SC7061_REG_IRQ] |= SC7061_PG_IRQ;
    }
    charger->level = level;
    charger->charging = charging;
    charger->state_generation++;
    charger_refresh(charger);
    if (charger->signals.environment_changed) {
        charger->signals.environment_changed(charger->signals.opaque, level,
                                              charging, cable_changed);
    }
    qemu_log_mask(LOG_UNIMP, "miband charger environment generation=%" PRIu64
                  " level=%u charging=%u\n", charger->state_generation,
                  charger->level, charger->charging);
    return true;
}

static void charger_write(MiBandCharger *charger, uint8_t reg, uint8_t value)
{
    charger->write_count++;
    if (charger->trace_events++ < 256) {
        qemu_log_mask(LOG_UNIMP,
                      "miband charger write reg=%02x value=%02x\n", reg, value);
    }
    /* Configuration readback is retained. Side effects are implemented only
     * where established by the original driver and the transaction trace. */
    if (reg != SC7061_REG_PROBE && reg != SC7061_REG_FAMILY &&
        reg != SC7061_REG_ID && reg != SC7061_REG_STATUS) {
        charger->registers[reg] = value;
        if (reg == SC7061_REG_CONTROL) {
            /* sub_C403400 case 2: bit 6 disables charging while leaving
             * the external PG input asserted. The firmware waits for this
             * status change during stop; do not report permanent charging. */
            charger_refresh_status(charger);
        }
    }
}

static uint8_t charger_read(MiBandCharger *charger, uint8_t reg)
{
    uint8_t value = charger->registers[reg];

    charger->read_count++;
    if (charger->trace_events++ < 256) {
        qemu_log_mask(LOG_UNIMP,
                      "miband charger read reg=%02x value=%02x\n", reg, value);
    }
    return value;
}

void miband_charger_write_command(MiBandCharger *charger, uint32_t command)
{
    if (command & I2C_RESTART) {
        charger->expect_register = true;
    }
    if (charger->expect_register) {
        charger->pointer = command & 0xff;
        charger->expect_register = false;
    } else {
        charger_write(charger, charger->pointer++, command & 0xff);
    }
    if (command & I2C_STOP) {
        charger->expect_register = true;
    }
}

uint8_t miband_charger_read_command(MiBandCharger *charger,
                                   uint32_t command)
{
    uint8_t value = charger_read(charger, charger->pointer++);

    /* A following write starts a new register-address phase, including the
     * case where the master uses a repeated START to change direction. */
    charger->expect_register = true;
    return value;
}
