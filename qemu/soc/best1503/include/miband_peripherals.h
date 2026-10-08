#ifndef MIBAND_PERIPHERALS_H
#define MIBAND_PERIPHERALS_H

#include <stdbool.h>
#include <stdint.h>
#include "exec/hwaddr.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "miband_i2c_endpoint.h"
#include "miband_psram_device.h"
#include "miband_bt.h"
#include "miband_flash.h"
#include "miband_lcdc.h"

#define MIBAND_TIMER_COUNT 3
#define MIBAND_MEMSC_COUNT 4
#define MIBAND_MEMSC_BANKS 2
#define MIBAND_I2C_COUNT 8
#define MIBAND_ISPI_PAGE_COUNT 6

typedef struct MiBandTimer {
    hwaddr base;
    uint32_t load;
    uint32_t value;
    uint32_t ctrl;
    int64_t start_ns;
    QEMUTimer *event;
    qemu_irq irq;
    bool pending;
    bool expired;
    unsigned trace_events;
} MiBandTimer;

typedef struct MiBandIspi {
    uint16_t regs[16][MIBAND_ISPI_PAGE_COUNT][256];
    uint16_t page[16];
    uint32_t control;
    uint8_t chip_select;
    uint32_t rx_data;
    bool rx_ready;
    bool gpadc_model;
    bool pmu_status_model;
    bool pmu_rtc_model;
    struct MiBandCharger *charger;
    qemu_irq pmu_irq;
    int64_t rtc_base_seconds;
    int64_t rtc_start_ns;
    uint8_t rtc_base_week;
    unsigned rtc_trace_events;
    uint16_t gpadc_valid;
    QEMUTimer *gpadc_timer;
    unsigned gpadc_trace_events;
    uint16_t gpadc_values[16];
    unsigned trace_events;
} MiBandIspi;

typedef struct MiBandSpi {
    hwaddr base;
    uint32_t rx_data;
    bool rx_ready;
} MiBandSpi;

typedef struct MiBandMemsc {
    bool free[MIBAND_MEMSC_BANKS][MIBAND_MEMSC_COUNT];
} MiBandMemsc;

typedef struct MiBandAonCmu {
    uint32_t chip_id;
    unsigned chip_id_reads;
    /* BEST1503 GBL_RESET_SET is a write-one-to-assert latch.  Keep the
     * modeled latch in the SoC block; the machine owns the resulting reset. */
    uint32_t global_reset_set;
    void (*reset_request)(void *opaque, uint32_t mask);
    void *reset_opaque;
} MiBandAonCmu;

typedef struct MiBandPsram {
    MiBandPsramDevice device;
    uint32_t command_address;
    uint32_t words_remaining;
    uint32_t command;
    uint32_t tx_word;
} MiBandPsram;

typedef struct MiBandDma {
    struct MiBandDmaAsync *async;
    AddressSpace *as;
    struct MiBandLcdc *lcdc;
    uint32_t src;
    uint32_t dst;
    uint32_t link;
    uint32_t control;
    uint32_t config;
    uint32_t display_src;
    uint32_t display_dst;
    uint32_t display_link;
    uint32_t display_control;
    uint32_t display_config;
    uint32_t global_config;
    uint32_t tc_status;
    uint32_t error_status;
    QEMUTimer *display_event;
    qemu_irq irq;
    unsigned display_transfers;
} MiBandDma;





void miband_dma_async_reset(MiBandDma *);

typedef struct MiBandI2c {
    hwaddr base;
    uint32_t regs[0x100 / 4];
    uint32_t raw_intr;
    uint32_t abort_source;
    unsigned commands;
    unsigned trace_events;
    uint8_t probe_address;
    uint8_t probe_register0;
    uint16_t probe_chip_id;
    uint16_t probe_register;
    uint8_t tx_bytes[8];
    unsigned tx_count;
    unsigned read_offset;
    uint8_t rx_fifo[16];
    unsigned rx_head;
    unsigned rx_count;
    MiBandI2cEndpoint endpoint;
    bool endpoint_active, endpoint_read;
    struct MiBandCharger *charger;
} MiBandI2c;

typedef struct MiBandGpio {
    qemu_irq irq;
    uint32_t output[3], output_enable[3];
    struct {
        void (*notify)(void *opaque, bool driven, bool high);
        void *opaque;
    } outputs[96];
    uint32_t status[3];
    uint32_t irq_enable[3];
    uint32_t common_enable[3];
    uint32_t irq_mask[3];
    uint32_t input[3], input_driven[3];
    uint32_t inferred_unmasked[3];
    unsigned edges;
    unsigned acks;
} MiBandGpio;

void miband_gpio_raise_inferred_edge(MiBandGpio *gpio, unsigned pin);
bool miband_gpio_pin_pending(MiBandGpio *gpio, unsigned pin);

void miband_gpio_connect_irq(MiBandGpio *gpio, qemu_irq irq);
void miband_gpio_connect_output(MiBandGpio *gpio, unsigned pin,
                                void (*notify)(void *, bool, bool), void *opaque);
void miband_gpio_reset_outputs(MiBandGpio *gpio);
void miband_gpio_set_input(MiBandGpio *gpio, unsigned pin, bool high);
void miband_gpio_raise_edge(MiBandGpio *gpio, unsigned pin);
bool miband_gpio_read(MiBandGpio *gpio, hwaddr address, unsigned size,
                      uint64_t *result);
bool miband_gpio_write(MiBandGpio *gpio, hwaddr address, unsigned size,
                       uint64_t value);

void miband_i2c_init(MiBandI2c i2c[MIBAND_I2C_COUNT]);
bool miband_i2c_read(MiBandI2c i2c[MIBAND_I2C_COUNT], hwaddr address,
                     unsigned size, uint64_t *result);
bool miband_i2c_write(MiBandI2c i2c[MIBAND_I2C_COUNT], hwaddr address,
                      unsigned size, uint64_t value);


void miband_timers_init(MiBandTimer timers[MIBAND_TIMER_COUNT]);
void miband_timer_connect_irq(MiBandTimer *timer, qemu_irq irq);
bool miband_timer_read(MiBandTimer timers[MIBAND_TIMER_COUNT], hwaddr address,
                       unsigned size, uint64_t *result);
bool miband_timer_write(MiBandTimer timers[MIBAND_TIMER_COUNT], hwaddr address,
                        unsigned size, uint64_t value);

void miband_ispi_init(MiBandIspi *ispi);
void miband_ispi_shutdown(MiBandIspi *ispi);
void miband_ispi_gpadc_reset(MiBandIspi *ispi);
void miband_ispi_set_gpadc_model(MiBandIspi *ispi, bool enabled);
void miband_ispi_set_analog_mv(MiBandIspi *ispi, unsigned channel, unsigned mv);
void miband_ispi_set_pmu_status_model(MiBandIspi *ispi, bool enabled);
void miband_ispi_set_pmu_rtc_model(MiBandIspi *ispi, bool enabled);
void miband_ispi_connect_pmu_irq(MiBandIspi *ispi, qemu_irq irq);
void miband_ispi_charger_changed(MiBandIspi *ispi, bool plugged);
void miband_ispi_charger_reset(MiBandIspi *ispi);
bool miband_ispi_read(MiBandIspi *ispi, hwaddr address, unsigned size,
                      uint64_t *result);
bool miband_ispi_write(MiBandIspi *ispi, hwaddr address, unsigned size,
                       uint64_t value);

void miband_spi_init(MiBandSpi spi[2]);
bool miband_spi_read(MiBandSpi spi[2], hwaddr address, unsigned size,
                     uint64_t *result);
bool miband_spi_write(MiBandSpi spi[2], hwaddr address, unsigned size,
                      uint64_t value);

void miband_memsc_init(MiBandMemsc *memsc);
bool miband_memsc_read(MiBandMemsc *memsc, hwaddr address, unsigned size,
                       uint64_t *result);
bool miband_memsc_write(MiBandMemsc *memsc, hwaddr address, unsigned size,
                        uint64_t value);

void miband_aoncmu_init(MiBandAonCmu *aoncmu, uint32_t metal_revision);
void miband_aoncmu_reset(MiBandAonCmu *aoncmu);
void miband_aoncmu_set_reset_request(MiBandAonCmu *aoncmu,
                                     void (*reset_request)(void *opaque,
                                                           uint32_t mask),
                                     void *opaque);
bool miband_aoncmu_read(MiBandAonCmu *aoncmu, hwaddr address, unsigned size,
                        uint64_t *result);
bool miband_aoncmu_write(MiBandAonCmu *aoncmu, hwaddr address, unsigned size,
                         uint64_t value);

void miband_dma_init(MiBandDma *dma, AddressSpace *as);
void miband_dma_connect_lcdc(MiBandDma *dma, MiBandLcdc *lcdc);
void miband_dma_connect_irq(MiBandDma *dma, qemu_irq irq);
bool miband_dma_read(MiBandDma *dma, hwaddr address, unsigned size,
                     uint64_t *result);
bool miband_dma_write(MiBandDma *dma, hwaddr address, unsigned size,
                      uint64_t value);
void miband_dma_flash_rx(void *opaque, MiBandFlash *flash);
void miband_dma_flash_tx(void *opaque, MiBandFlash *flash);
void miband_psram_init(MiBandPsram *psram, uint8_t *storage,
                       uint32_t storage_size);
bool miband_psram_read(MiBandPsram *psram, hwaddr address, unsigned size,
                       uint64_t *result);
bool miband_psram_write(MiBandPsram *psram, hwaddr address, unsigned size,
                        uint64_t value);

#endif
