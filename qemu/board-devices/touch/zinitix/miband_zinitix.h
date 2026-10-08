#ifndef MIBAND_ZINITIX_H
#define MIBAND_ZINITIX_H
#include "qemu/timer.h"
#include "miband_i2c_endpoint.h"

#define MIBAND_ZINITIX_QUEUE_LENGTH 32
typedef struct MiBandZinitix {
    uint16_t chip_id;
    /* Word registers, including the vendor control range. */
    uint16_t registers[0x10000];
    uint8_t reports[MIBAND_ZINITIX_QUEUE_LENGTH][10];
    unsigned head, count, tx_count, read_offset, trace_events;
    unsigned irq_retries;
    uint16_t reg;
    uint16_t gesture_pending;
    uint16_t gesture_x, gesture_y;
    unsigned gesture_dx;
    uint8_t tx[6];
    bool vendor_unlocked;
    bool reading, irq_asserted, sleeping;
    bool reset_asserted;
    bool gesture_contact;
    /* Board-selected peripheral firmware protocol; 0 retains AF gestures. */
    unsigned sleep_gesture;
    uint16_t double_tap_status;
    bool first_tap;
    QEMUClockType gesture_clock;
    int64_t tap_down_ns, tap_up_ns;
    uint16_t tap_x, tap_y;
    QEMUTimer *next_report;
    void (*irq)(void *opaque, bool asserted);
    void *irq_opaque;
} MiBandZinitix;

void miband_zinitix_init(MiBandZinitix *touch, uint16_t chip_id,
                         void (*irq)(void *, bool), void *opaque);
void miband_zinitix_report(void *opaque, uint16_t x, uint16_t y, uint8_t event);
void miband_zinitix_report_clocked(void *opaque, uint16_t x, uint16_t y,
                                  uint8_t event, QEMUClockType clock);
MiBandI2cEndpoint miband_zinitix_endpoint(MiBandZinitix *touch, uint8_t address);
void miband_zinitix_reset(MiBandZinitix *touch);
void miband_zinitix_reset_line(void *opaque, bool driven, bool high);
void miband_zinitix_shutdown(MiBandZinitix *touch);
#endif
