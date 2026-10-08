/*
 * BES I2C controller probe. Register layout matches the DesignWare-style
 * controller accessed by the firmware. Touch and charger subsets attach only
 * to the addresses selected by their explicit machine options.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "miband_peripherals.h"
#include "miband_charger.h"

#define IC_DATA_CMD         0x10
#define IC_INTR_STAT        0x2c
#define IC_INTR_MASK        0x30
#define IC_RAW_INTR_STAT    0x34
#define IC_CLR_INTR         0x40
#define IC_CLR_TX_ABRT      0x54
#define IC_CLR_STOP_DET     0x60
#define IC_ENABLE          0x6c
#define IC_STATUS          0x70
#define IC_TXFLR           0x74
#define IC_RXFLR           0x78
#define IC_TX_ABRT_SOURCE  0x80
#define IC_ENABLE_STATUS   0x9c

#define INTR_TX_ABRT       (1u << 6)
#define INTR_STOP_DET      (1u << 9)
#define INTR_RX_FULL       (1u << 2)
#define ABRT_7B_ADDR_NOACK (1u << 0)
#define STATUS_TFNF        (1u << 1)
#define STATUS_TFE         (1u << 2)
#define STATUS_RFNE        (1u << 3)

static MiBandI2c *miband_i2c_find(MiBandI2c i2c[MIBAND_I2C_COUNT],
                                   hwaddr address, hwaddr *offset)
{
    unsigned i;

    for (i = 0; i < MIBAND_I2C_COUNT; i++) {
        if (address >= i2c[i].base && address < i2c[i].base + 0x1000) {
            *offset = address - i2c[i].base;
            return &i2c[i];
        }
    }
    return NULL;
}

static void miband_i2c_probe_latch_register(MiBandI2c *c)
{
    if (c->tx_count == 1) {
        c->probe_register = c->tx_bytes[0];
    } else if (c->tx_count == 2) {
        c->probe_register = (c->tx_bytes[0] << 8) | c->tx_bytes[1];
    }
    c->tx_count = 0;
    c->read_offset = 0;
}

static uint8_t miband_i2c_probe_read_byte(MiBandI2c *c)
{
    uint8_t result = 0;

    if (c->probe_register == 0 && c->read_offset == 0) {
        result = c->probe_register0;
    } else if (c->probe_address == 0x20 && c->probe_chip_id &&
               c->probe_register == 0xcc && c->read_offset < 2) {
        result = c->probe_chip_id >> (8 * c->read_offset);
    } else if (c->probe_address == 0x20 && c->probe_chip_id &&
               c->probe_register == 0x1c00 && c->read_offset < 2) {
        result = 0x5a49 >> (8 * c->read_offset);

    }
    c->read_offset++;
    return result;
}

void miband_i2c_init(MiBandI2c i2c[MIBAND_I2C_COUNT])
{
    static const hwaddr bases[MIBAND_I2C_COUNT] = {
        0x40005000, 0x40006000, 0x40060000, 0x40061000,
        0x40062000, 0x40063000, 0x40064000, 0x40065000,
    };
    unsigned i;

    for (i = 0; i < MIBAND_I2C_COUNT; i++) {
        i2c[i].base = bases[i];
    }
}

bool miband_i2c_read(MiBandI2c i2c[MIBAND_I2C_COUNT], hwaddr address,
                     unsigned size, uint64_t *result)
{
    hwaddr offset;
    MiBandI2c *c = miband_i2c_find(i2c, address, &offset);

    if (!c) {
        return false;
    }
    if (size != 4 || (offset & 3) || offset >= 0x100) {
        qemu_log_mask(LOG_GUEST_ERROR, "miband I2C invalid read %08"
                      HWADDR_PRIx " size=%u\n", address, size);
        *result = 0;
        return true;
    }

    switch (offset) {
    case IC_DATA_CMD:
        if (c->rx_count) {
            *result = c->rx_fifo[c->rx_head];
            c->rx_head = (c->rx_head + 1) % ARRAY_SIZE(c->rx_fifo);
            c->rx_count--;
            if (!c->rx_count) {
                c->raw_intr &= ~INTR_RX_FULL;
            }
        } else {
            *result = 0;
        }
        break;
    case IC_INTR_STAT:
        *result = c->raw_intr & c->regs[IC_INTR_MASK / 4];
        break;
    case IC_RAW_INTR_STAT:
        *result = c->raw_intr;
        break;
    case IC_CLR_INTR:
        c->raw_intr = 0;
        c->abort_source = 0;
        *result = 0;
        break;
    case IC_CLR_TX_ABRT:
        c->raw_intr &= ~INTR_TX_ABRT;
        c->abort_source = 0;
        *result = 0;
        break;
    case IC_CLR_STOP_DET:
        c->raw_intr &= ~INTR_STOP_DET;
        *result = 0;
        break;
    case IC_STATUS:
        *result = STATUS_TFNF | STATUS_TFE |
                  (c->rx_count ? STATUS_RFNE : 0);
        break;
    case IC_TXFLR:
        *result = 0;
        break;
    case IC_RXFLR:
        *result = c->rx_count;
        break;
    case IC_TX_ABRT_SOURCE:
        *result = c->abort_source;
        break;
    case IC_ENABLE_STATUS:
        *result = c->regs[IC_ENABLE / 4] & 1;
        break;
    default:
        *result = c->regs[offset / 4];
        if (c->trace_events++ < 64) {
            qemu_log_mask(LOG_UNIMP, "miband I2C read %08" HWADDR_PRIx
                          " -> %08" PRIx64 "\n", address, *result);
        }
        break;
    }
    return true;
}

bool miband_i2c_write(MiBandI2c i2c[MIBAND_I2C_COUNT], hwaddr address,
                      unsigned size, uint64_t value)
{
    hwaddr offset;
    MiBandI2c *c = miband_i2c_find(i2c, address, &offset);

    if (!c) {
        return false;
    }
    if (size != 4 || (offset & 3) || offset >= 0x100) {
        qemu_log_mask(LOG_GUEST_ERROR, "miband I2C invalid write %08"
                      HWADDR_PRIx " size=%u value=%08" PRIx64 "\n",
                      address, size, value);
        return true;
    }

    if (offset == IC_DATA_CMD) {
        bool endpoint_ack = c->endpoint.address &&
                            (c->regs[0x04 / 4] & 0x7f) == c->endpoint.address;
        bool charger_ack = c->charger && c->charger->enabled &&
                           (c->regs[0x04 / 4] & 0x7f) ==
                           MIBAND_CHARGER_I2C_ADDRESS;
        bool probe_ack = c->probe_address &&
                         (c->regs[0x04 / 4] & 0x7f) == c->probe_address;

        c->commands++;
        if (endpoint_ack) {
            bool read = value & (1u << 8);
            if (!c->endpoint_active || c->endpoint_read != read ||
                (value & (1u << 10))) {
                c->endpoint.start(c->endpoint.opaque, read);
                c->endpoint_active = true;
                c->endpoint_read = read;
            }
            if (read) {
                if (c->rx_count < ARRAY_SIZE(c->rx_fifo)) {
                    c->rx_fifo[(c->rx_head + c->rx_count) % ARRAY_SIZE(c->rx_fifo)] =
                        c->endpoint.read(c->endpoint.opaque);
                    c->rx_count++;
                    c->raw_intr |= INTR_RX_FULL;
                }
            } else {
                c->endpoint.write(c->endpoint.opaque, value & 0xff);
            }
            if (value & (1u << 9)) {
                c->endpoint.stop(c->endpoint.opaque);
                c->endpoint_active = false;
            }
        } else if (charger_ack) {
            if (value & (1u << 8)) {
                if (c->rx_count < ARRAY_SIZE(c->rx_fifo)) {
                    c->rx_fifo[(c->rx_head + c->rx_count) %
                               ARRAY_SIZE(c->rx_fifo)] =
                        miband_charger_read_command(c->charger, value);
                    c->rx_count++;
                    c->raw_intr |= INTR_RX_FULL;
                }
            } else {
                miband_charger_write_command(c->charger, value);
            }
        } else if (probe_ack) {
            if (value & (1u << 8)) {
                if (c->tx_count) {
                    miband_i2c_probe_latch_register(c);
                }
                if (c->rx_count < ARRAY_SIZE(c->rx_fifo)) {
                    c->rx_fifo[(c->rx_head + c->rx_count) %
                               ARRAY_SIZE(c->rx_fifo)] =
                        miband_i2c_probe_read_byte(c);
                    c->rx_count++;
                    c->raw_intr |= INTR_RX_FULL;
                }
            } else {
                if ((value & (1u << 10)) && c->tx_count) {
                    miband_i2c_probe_latch_register(c);
                }
                if (c->tx_count < ARRAY_SIZE(c->tx_bytes)) {
                    c->tx_bytes[c->tx_count++] = value & 0xff;
                }
                if (value & (1u << 9)) {
                    miband_i2c_probe_latch_register(c);
                }
            }
        } else {
            c->abort_source = ABRT_7B_ADDR_NOACK;
            c->raw_intr |= INTR_TX_ABRT;
        }
        if (value & (1u << 9)) {
            c->raw_intr |= INTR_STOP_DET;
        }
        if (c->commands <= 64) {
            qemu_log_mask(LOG_UNIMP, "miband I2C %08" HWADDR_PRIx
                          " target=%02x command=%03" PRIx64
                          " -> %s\n", c->base,
                          c->regs[0x04 / 4] & 0x3ff, value & 0x7ff,
                          endpoint_ack ? "device ACK" : charger_ack ? "charger ACK" :
                          probe_ack ? "probe ACK" : "NACK");
        }
    } else {
        c->regs[offset / 4] = value;
        if (offset == 0x04 || (offset == IC_ENABLE && !(value & 1))) {
            if (c->endpoint.reset) {
                c->endpoint.reset(c->endpoint.opaque);
            }
            c->endpoint_active = false;
            if (c->charger) {
                miband_charger_bus_reset(c->charger);
            }
            c->tx_count = 0;
            c->read_offset = 0;
            c->rx_head = 0;
            c->rx_count = 0;
            c->raw_intr &= ~INTR_RX_FULL;
        }
        if ((offset == 0x04 || offset == IC_ENABLE) &&
            c->trace_events++ < 64) {
            qemu_log_mask(LOG_UNIMP, "miband I2C %08" HWADDR_PRIx
                          " reg=%03" HWADDR_PRIx " value=%08" PRIx64
                          "\n", c->base, offset, value);
        }
    }
    return true;
}
