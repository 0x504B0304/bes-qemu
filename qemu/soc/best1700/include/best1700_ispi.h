#ifndef BEST1700_ISPI_H
#define BEST1700_ISPI_H

#include "qemu/osdep.h"
#include "exec/memory.h"

#define BEST1700_ISPI_CS_COUNT 16
#define BEST1700_ISPI_PAGE_COUNT 4
#define BEST1700_ISPI_REG_COUNT 1024

typedef struct Best1700IspiEndpoint {
    void *opaque;
    bool (*read)(void *opaque, unsigned page, unsigned reg, uint16_t *value);
    bool (*write)(void *opaque, unsigned page, unsigned reg, uint16_t value);
    /* Non-owning notification after a decoded write and its backing effects.
     * Page-select commands and reads are not register writes. */
    void (*write_observed)(void *opaque, unsigned page, unsigned reg, uint16_t value);
} Best1700IspiEndpoint;

typedef struct Best1700Ispi {
    Best1700IspiEndpoint endpoints[BEST1700_ISPI_CS_COUNT];
    uint16_t regs[BEST1700_ISPI_CS_COUNT][BEST1700_ISPI_PAGE_COUNT][BEST1700_ISPI_REG_COUNT];
    uint16_t page[BEST1700_ISPI_CS_COUNT];
    uint32_t control;
    uint32_t frame_control;
    uint32_t tx_control;
    uint32_t rx_control;
    uint8_t chip_select;
    uint32_t rx_data;
    bool rx_ready;
    uint32_t trace_events;
    uint32_t internal_id_reads;
    /* The external PMU exposes the GPADC through the same ISPI window. */
    uint16_t gpadc_values[8];
    uint16_t gpadc_valid;
    uint32_t gpadc_events;
} Best1700Ispi;

void best1700_ispi_init(Best1700Ispi *ispi);
void best1700_ispi_connect_endpoint(Best1700Ispi *ispi, unsigned chip_select,
                                    const Best1700IspiEndpoint *endpoint);
bool best1700_ispi_read(Best1700Ispi *ispi, hwaddr address,
                        unsigned size, uint64_t *result);
bool best1700_ispi_write(Best1700Ispi *ispi, hwaddr address,
                         unsigned size, uint64_t value);

#endif
