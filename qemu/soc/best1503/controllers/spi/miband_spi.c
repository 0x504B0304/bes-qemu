/* BEST1503 AP SPI master: polled data path with an optional SSI bus.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/ssi/ssi.h"
#include "miband_peripherals.h"

#define SPI_CR0     0x00
#define SPI_CR1     0x04
#define SPI_DATA    0x08
#define SPI_STATUS  0x0c
#define SPI_TX_ROOM 0x02
#define SPI_RX_DATA 0x04
#define SPI_ENABLE  (1u << 1)
#define SPI_TRACE_LIMIT 4096

void miband_spi_init(MiBandSpi spi[2])
{
    spi[0].base = 0x40007000;
    spi[1].base = 0x40068000;
}

void miband_spi_expose_bus(MiBandSpi *spi, DeviceState *owner, unsigned index)
{
    g_autofree char *name = g_strdup_printf("miband-ap-spi%u", index);
    spi->index = index;
    spi->bus = ssi_create_bus(owner, name);
}

static unsigned miband_spi_bits(const MiBandSpi *spi)
{
    /* Same CR0 layout as BEST1700: DSS extends to [20:16]. Before the
     * guest programs CR0 the frame width is treated as eight bits. */
    unsigned bits = ((spi->cr0 >> 16) & 0x1f) + 1;
    return bits < 4 ? 8 : bits;
}

static void miband_spi_trace(MiBandSpi *spi, const char *fmt, uint32_t a,
                             uint32_t b)
{
    if (spi->trace_events++ < SPI_TRACE_LIMIT) {
        qemu_log_mask(LOG_UNIMP, fmt, spi->index, a, b);
    }
}

static void miband_spi_drive_cs(MiBandSpi *spi, unsigned index, bool active)
{
    DeviceState *dev = spi->bus ? ssi_get_cs(spi->bus, index) : NULL;

    if (dev) {
        qemu_set_irq(qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0), !active);
    }
}

/* Hardware chip select follows the controller enable bit: the BES HAL
 * sets CR1 bit 1 around each transfer and clears it afterwards. CR1[10:7]
 * selects the line (Band 9: 0 = IMU, 1 = PPG AFE on SPI0). */
static void miband_spi_set_cs(MiBandSpi *spi, bool active, unsigned index)
{
    if (spi->cs_active == active && (!active || spi->cs_index == index)) {
        return;
    }
    if (spi->cs_active) {
        miband_spi_drive_cs(spi, spi->cs_index, false);
    }
    spi->cs_active = active;
    spi->cs_index = index;
    miband_spi_trace(spi, "miband SPI%u cs=%u cr1=%08x\n", active, spi->cr1);
    if (active) {
        miband_spi_drive_cs(spi, index, true);
    }
}

void miband_spi_reset(MiBandSpi *spi)
{
    spi->cr0 = spi->cr1 = 0;
    spi->rx_data = 0;
    spi->rx_ready = false;
    spi->cs_active = false;
    spi->cs_index = 0;
    for (unsigned index = 0; index < 16; index++) {
        miband_spi_drive_cs(spi, index, false);
    }
}

bool miband_spi_read(MiBandSpi spi[2], hwaddr address, unsigned size,
                     uint64_t *result)
{
    unsigned i;

    if (size != sizeof(uint32_t)) {
        return false;
    }
    for (i = 0; i < 2; i++) {
        if (address == spi[i].base + SPI_STATUS) {
            *result = SPI_TX_ROOM | (spi[i].rx_ready ? SPI_RX_DATA : 0);
            return true;
        }
        if (address == spi[i].base + SPI_DATA) {
            *result = spi[i].rx_data;
            spi[i].rx_ready = false;
            return true;
        }
    }
    return false;
}

bool miband_spi_write(MiBandSpi spi[2], hwaddr address, unsigned size,
                      uint64_t value)
{
    unsigned i;

    if (size != sizeof(uint32_t)) {
        return false;
    }
    for (i = 0; i < 2; i++) {
        MiBandSpi *s = &spi[i];

        if (address == s->base + SPI_CR0) {
            /* Observe only: the machine's register file keeps the value
             * for read-back exactly as before this bus existed. */
            s->cr0 = value;
            return false;
        }
        if (address == s->base + SPI_CR1) {
            s->cr1 = value;
            miband_spi_set_cs(s, value & SPI_ENABLE, (value >> 7) & 0xf);
            return false;
        }
        if (address == s->base + SPI_DATA) {
            uint32_t mask = UINT32_MAX >> (32 - miband_spi_bits(s));
            uint32_t tx = value & mask;
            /* An absent device leaves MISO idle high. */
            uint32_t rx = mask;

            if (s->bus && s->cs_active && ssi_get_cs(s->bus, s->cs_index)) {
                rx = ssi_transfer(s->bus, tx) & mask;
            }
            miband_spi_trace(s, "miband SPI%u tx=%02x rx=%02x\n", tx, rx);
            s->rx_data = rx;
            s->rx_ready = true;
            return true;
        }
    }
    return false;
}
