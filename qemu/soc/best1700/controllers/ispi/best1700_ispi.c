#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "exec/memory.h"
#include "best1700_ispi.h"

#define ISPI_DATA 0x08
#define ISPI_FRAME_CONTROL 0x00
#define ISPI_CONTROL 0x04
#define ISPI_FIFO_STATUS 0x0c
#define ISPI_TX_CONTROL 0x28
#define ISPI_RX_CONTROL 0x2c
#define ISPI_FRAME_BITS_SHIFT 16
#define ISPI_FRAME_BITS_MASK 0x1f
#define ISPI_TX_ROOM 0x02
#define ISPI_RX_READY 0x04
#define ISPI_NORMAL_READ (1u << 28)
#define ISPI_EXTERNAL_READ (1u << 24)
#define ISPI_COMMAND_SHIFT 2
#define ISPI_CS_SHIFT 7
#define ISPI_CS_MASK 0xf
#define ISPI_PAGE_BASE 0xa000
#define ISPI_INTERNAL_CS 7
#define ISPI_EXTERNAL_PMU_CS 0

/* External-PMU GPADC register window.  These offsets are part of the
 * BEST1700 analog interface and are kept here as SoC behaviour; board code
 * only supplies the physical environment around it. */
#define GPADC_REG_INTERVAL_ENABLE 0x18
#define GPADC_REG_INTERVAL         0x1c
#define GPADC_REG_CHANNEL_ENABLE   0x1d
#define GPADC_REG_INTERRUPT_MASK   0x1f
#define GPADC_REG_INTERRUPT_ENABLE 0x20
#define GPADC_REG_START            0x4f
#define GPADC_REG_RAW_STATUS       0x50
#define GPADC_REG_STATUS_CLEAR     0x51
#define GPADC_REG_CHANNEL_DATA     0x56
#define GPADC_INTERVAL_MODE        (1u << 12)
#define GPADC_START                (1u << 5)

/* The S5 PMU firmware selects the 1810-compatible GPADC register layout.
 * Keep the earlier BEST1700 layout above because other profiles may use it,
 * but recognize the layout actually exercised by this image as well. */
#define GPADC_S5_REG_INTERVAL      0x26
#define GPADC_S5_REG_INTERVAL_EN   0x27
#define GPADC_S5_REG_CHANNEL_EN    0x24
#define GPADC_S5_REG_STATUS        0x52
#define GPADC_S5_INTERVAL_MODE     (1u << 13)

static bool ispi_gpadc_window(const Best1700Ispi *ispi, unsigned page)
{
    return ispi->chip_select == ISPI_EXTERNAL_PMU_CS && page == 0;
}

static void ispi_gpadc_sample(Best1700Ispi *ispi)
{
    uint16_t *regs = ispi->regs[ISPI_EXTERNAL_PMU_CS][0];
    /* A conversion produces channel data independently of the interrupt
     * enable bits.  The blocked HAL path polls RAW_STATUS and is used by
     * early PMU initialization with no callback installed. */
    uint16_t enabled = regs[GPADC_REG_CHANNEL_ENABLE] & 0xff;
    /* Configurable uses the 1810-compatible channel-enable register. */
    if (!enabled) {
        enabled = regs[GPADC_S5_REG_CHANNEL_EN] & 0xff;
    }
    if (!enabled) {
        enabled = regs[0x27] & 0xff;
    }

    ispi->gpadc_valid |= enabled;
    if (ispi->gpadc_events++ < 32) {
        qemu_log_mask(LOG_UNIMP,
                      "best1700 PMU GPADC sample channels=%04x raw=%04x\n",
                      ispi->gpadc_valid, ispi->gpadc_values[0]);
    }
}

static uint16_t ispi_gpadc_read(Best1700Ispi *ispi, unsigned reg,
                                uint16_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "best1700 PMU GPADC read reg=%02x value=%04x valid=%04x\n",
                  reg, value, ispi->gpadc_valid);
    if (reg == GPADC_REG_RAW_STATUS || reg == GPADC_S5_REG_STATUS) {
        return ispi->gpadc_valid;
    }
    if (reg == GPADC_REG_STATUS_CLEAR) {
        uint16_t mask = ispi->regs[ISPI_EXTERNAL_PMU_CS][0]
            [GPADC_REG_INTERRUPT_MASK];
        return ispi->gpadc_valid & mask;
    }
    if (reg >= GPADC_REG_CHANNEL_DATA &&
        reg < GPADC_REG_CHANNEL_DATA + ARRAY_SIZE(ispi->gpadc_values)) {
        return ispi->gpadc_values[reg - GPADC_REG_CHANNEL_DATA];
    }
    return value;
}

static void ispi_gpadc_write(Best1700Ispi *ispi, unsigned reg,
                             uint16_t value)
{
    uint16_t *regs = ispi->regs[ISPI_EXTERNAL_PMU_CS][0];
    bool interval = (regs[GPADC_REG_INTERVAL_ENABLE] & GPADC_INTERVAL_MODE) ||
                    (regs[GPADC_S5_REG_INTERVAL_EN] != 0) ||
                    (regs[GPADC_S5_REG_INTERVAL] & GPADC_S5_INTERVAL_MODE);

    qemu_log_mask(LOG_UNIMP,
                  "best1700 PMU GPADC write reg=%02x value=%04x\n",
                  reg, value);

    if (reg == GPADC_REG_STATUS_CLEAR || reg == GPADC_S5_REG_STATUS) {
        ispi->gpadc_valid &= ~value;
        /* Interval-mode conversions continue while START and a channel are
         * enabled.  Complete the next sample synchronously so firmware that
         * polls during early boot observes real GPADC progress. */
        if (interval &&
            (regs[GPADC_REG_START] & GPADC_START)) {
            ispi_gpadc_sample(ispi);
        }
    } else if (reg == GPADC_REG_START && (value & GPADC_START)) {
        ispi_gpadc_sample(ispi);
    }
}

static bool ispi_page_value(uint16_t value)
{
    unsigned page = (value - ISPI_PAGE_BASE) >> 4;

    return value >= ISPI_PAGE_BASE && page < BEST1700_ISPI_PAGE_COUNT &&
           (value & 0xf) == 0;
}

static unsigned ispi_page_index(const Best1700Ispi *ispi)
{
    uint16_t page = ispi->page[ispi->chip_select];

    return ispi_page_value(page) ? (page - ISPI_PAGE_BASE) >> 4 : 0;
}

static void ispi_trace(Best1700Ispi *ispi, const char *kind, uint32_t value,
                       unsigned reg, unsigned page, uint16_t data)
{
    if (ispi->trace_events++ >= 512) {
        return;
    }
    uint32_t pc = current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;
    qemu_log_mask(LOG_UNIMP,
                  "best1700 ISPI %s cs=%u value=%08" PRIx32
                  " reg=%03x page=%u data=%04x pc=%08" PRIx32 "\n",
                  kind, ispi->chip_select, value, reg, page, data, pc);
}

void best1700_ispi_init(Best1700Ispi *ispi)
{
    memset(ispi, 0, sizeof(*ispi));
    ispi->frame_control = 24u << ISPI_FRAME_BITS_SHIFT;
    ispi->regs[ISPI_INTERNAL_CS][0][0] = 0x1700;
    /* BEST1700 Configurable uses the 1810 PMU identification path. */
    ispi->regs[0][0][0] = 0x18a0;
    /* A room-temperature analog value keeps the NTC monitor in its normal
     * initialization path while leaving the value observable to the guest. */
    for (unsigned i = 0; i < ARRAY_SIZE(ispi->gpadc_values); i++) {
        ispi->gpadc_values[i] = 0x6e00;
    }
}

void best1700_ispi_connect_endpoint(Best1700Ispi *ispi, unsigned chip_select,
                                    const Best1700IspiEndpoint *endpoint)
{
    g_assert(chip_select < BEST1700_ISPI_CS_COUNT);
    ispi->endpoints[chip_select] = endpoint ? *endpoint : (Best1700IspiEndpoint) {0};
}

static uint16_t best1700_ispi_endpoint_read(Best1700Ispi *ispi,
                                           unsigned page, unsigned reg,
                                           uint16_t fallback)
{
    Best1700IspiEndpoint *ep = &ispi->endpoints[ispi->chip_select];
    uint16_t value;

    if (ep->read && ep->read(ep->opaque, page, reg, &value)) {
        return value;
    }
    return fallback;
}

bool best1700_ispi_read(Best1700Ispi *ispi, hwaddr address,
                        unsigned size, uint64_t *result)
{
    if (size != sizeof(uint32_t)) {
        return false;
    }
    if (address == ISPI_DATA) {
        ispi->rx_ready = false;
        *result = ispi->rx_data;
        return true;
    }
    if (address == ISPI_CONTROL) {
        *result = ispi->control;
        return true;
    }
    if (address == ISPI_FRAME_CONTROL) {
        *result = ispi->frame_control;
        return true;
    }
    if (address == ISPI_TX_CONTROL || address == ISPI_RX_CONTROL) {
        *result = address == ISPI_TX_CONTROL ? ispi->tx_control : ispi->rx_control;
        return true;
    }
    if (address == ISPI_FIFO_STATUS) {
        *result = ISPI_TX_ROOM | (ispi->rx_ready ? ISPI_RX_READY : 0);
        return true;
    }
    return false;
}

bool best1700_ispi_write(Best1700Ispi *ispi, hwaddr address,
                         unsigned size, uint64_t value)
{
    uint32_t command = value;
    unsigned reg;
    unsigned page;
    uint16_t data;
    bool normal_read;
    bool external_read;

    if (size != sizeof(uint32_t)) {
        return false;
    }
    if (address == ISPI_FRAME_CONTROL) {
        ispi->frame_control = command;
        ispi_trace(ispi, "frame-control", command, address, 0, 0);
        return true;
    }
    if (address == ISPI_TX_CONTROL || address == ISPI_RX_CONTROL) {
        if (address == ISPI_TX_CONTROL) {
            ispi->tx_control = command;
        } else {
            ispi->rx_control = command;
        }
        ispi_trace(ispi, "frame-control", command, address, 0, 0);
        return true;
    }
    if (address == ISPI_CONTROL) {
        ispi->control = command;
        ispi->chip_select = (command >> ISPI_CS_SHIFT) & ISPI_CS_MASK;
        ispi_trace(ispi, "control", command, 0, 0, 0);
        return true;
    }
    if (address != ISPI_DATA) {
        return false;
    }

    normal_read = command & ISPI_NORMAL_READ;
    bool external_format = ispi->chip_select == ISPI_EXTERNAL_PMU_CS &&
        ((ispi->frame_control >> ISPI_FRAME_BITS_SHIFT) & ISPI_FRAME_BITS_MASK) == 24;
    external_read = external_format &&
                    (command & ISPI_EXTERNAL_READ);
    if (normal_read) {
        reg = (command >> (16 + ISPI_COMMAND_SHIFT)) & 0x3ff;
        page = ispi_page_index(ispi);
        data = ispi->regs[ispi->chip_select][page][reg];
        if (ispi->chip_select == ISPI_INTERNAL_CS && page == 0 && reg == 0) {
            ispi->internal_id_reads++;
            data = 0x1700;
        }
        if (ispi_gpadc_window(ispi, page)) {
            data = ispi_gpadc_read(ispi, reg, data);
        }
        data = best1700_ispi_endpoint_read(ispi, page, reg, data);
        ispi->rx_data = (uint32_t)data << ISPI_COMMAND_SHIFT;
        ispi_trace(ispi, "read", command, reg, page, data);
    } else if (external_read) {
        reg = (command >> 16) & 0xff;
        page = ispi_page_index(ispi);
        data = ispi->regs[ispi->chip_select][page][reg];
        if (ispi_gpadc_window(ispi, page)) {
            data = ispi_gpadc_read(ispi, reg, data);
        }
        data = best1700_ispi_endpoint_read(ispi, page, reg, data);
        ispi->rx_data = data;
        ispi_trace(ispi, "ext-read", command, reg, page, data);
    } else {
        /* A 25-bit external frame has an 8-bit register and 16-bit payload.
         * Other external-PMU variants use the same padded normal command
         * format as internal devices. The guest selects framing in CR0. */
        if (external_format) {
            reg = (command >> 16) & 0xff;
        } else {
            reg = (command >> (16 + ISPI_COMMAND_SHIFT)) & 0x3ff;
        }
        page = ispi_page_index(ispi);
        data = external_format ?
            (uint16_t)command : (command >> ISPI_COMMAND_SHIFT) & 0xffff;
        if (reg == 0 && ispi_page_value(data)) {
            ispi->page[ispi->chip_select] = data;
        } else {
            Best1700IspiEndpoint *ep = &ispi->endpoints[ispi->chip_select];

            if (!ep->write || !ep->write(ep->opaque, page, reg, data)) {
                ispi->regs[ispi->chip_select][page][reg] = data;
                if (ispi_gpadc_window(ispi, page)) {
                    ispi_gpadc_write(ispi, reg, data);
                }
            }
            if (ep->write_observed) {
                ep->write_observed(ep->opaque, page, reg, data);
            }
        }
        ispi->rx_data = 0;
        ispi_trace(ispi, "write", command, reg, page, data);
    }
    ispi->rx_ready = true;
    return true;
}
