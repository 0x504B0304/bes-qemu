/* Inferred I2C report device; no BEST1503 register or host UI dependencies. */
#ifndef MIBAND_TOUCH_H
#define MIBAND_TOUCH_H
#include <stdbool.h>
#include <stdint.h>
#include "qemu/timer.h"
#include "miband_i2c_endpoint.h"

#define MIBAND_TOUCH_QUEUE_LENGTH 32
#define MIBAND_TOUCH_REPORT_LENGTH 10

typedef struct MiBandTouch {
    void (*raise_irq)(void *opaque);
    bool (*irq_pending)(void *opaque);
    void *irq_opaque;
    QEMUTimer *pending_report_irq;
    unsigned pending_irq_retries;
    uint16_t chip_id;
    uint8_t reports[MIBAND_TOUCH_QUEUE_LENGTH][MIBAND_TOUCH_REPORT_LENGTH];
    unsigned head, count;
    bool read_has_report;
    unsigned bus_register, bus_offset, prefix_count;
    uint8_t prefix[2];
} MiBandTouch;

void miband_touch_init(MiBandTouch *touch, uint16_t chip_id,
                      void (*raise_irq)(void *), bool (*irq_pending)(void *),
                      void *opaque);
void miband_touch_report(MiBandTouch *touch, uint16_t x, uint16_t y, uint8_t event);
void miband_touch_input_report(void *opaque, uint16_t x, uint16_t y, uint8_t event);
MiBandI2cEndpoint miband_touch_endpoint(MiBandTouch *touch, uint8_t address);
uint8_t miband_touch_read_register(MiBandTouch *touch, unsigned reg, unsigned offset);
uint8_t miband_touch_read_byte(MiBandTouch *touch, unsigned offset);
uint8_t miband_touch_read_gesture_byte(MiBandTouch *touch, unsigned offset);
#endif
