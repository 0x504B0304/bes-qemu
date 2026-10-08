/*
 * Diagnostic model of the 10-byte touch report consumed by the AP firmware.
 * The report layout is inferred from sub_C3F5F94, not from a CST datasheet.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "miband_touch.h"

#define TOUCH_NEXT_REPORT_DELAY_NS 10000000
#define TOUCH_RETRY_DELAY_NS 50000000
#define TOUCH_MAX_IRQ_RETRIES 200

static void touch_bus_latch(MiBandTouch *touch)
{
    if (touch->prefix_count == 1) {
        touch->bus_register = touch->prefix[0];
    } else if (touch->prefix_count == 2) {
        touch->bus_register = (touch->prefix[0] << 8) | touch->prefix[1];
    }
    touch->prefix_count = 0;
    touch->bus_offset = 0;
}

static void touch_bus_start(void *opaque, bool read)
{
    MiBandTouch *touch = opaque;
    if (read) {
        touch_bus_latch(touch);
    } else {
        touch->prefix_count = 0;
    }
}

static void touch_bus_write(void *opaque, uint8_t value)
{
    MiBandTouch *touch = opaque;
    if (touch->prefix_count < sizeof(touch->prefix)) {
        touch->prefix[touch->prefix_count++] = value;
    }
}

static uint8_t touch_bus_read(void *opaque)
{
    MiBandTouch *touch = opaque;
    return miband_touch_read_register(touch, touch->bus_register, touch->bus_offset++);
}

static void touch_bus_stop(void *opaque)
{
    MiBandTouch *touch = opaque;
    if (touch->prefix_count) {
        touch_bus_latch(touch);
    }
}

static void touch_bus_reset(void *opaque)
{
    MiBandTouch *touch = opaque;
    touch->prefix_count = touch->bus_offset = 0;
}

MiBandI2cEndpoint miband_touch_endpoint(MiBandTouch *touch, uint8_t address)
{
    return (MiBandI2cEndpoint) { address, touch, touch_bus_start, touch_bus_write,
                               touch_bus_read, touch_bus_stop, touch_bus_reset };
}

void miband_touch_input_report(void *opaque, uint16_t x, uint16_t y, uint8_t event)
{
    miband_touch_report(opaque, x, y, event);
}

static void miband_touch_raise_pending_report(void *opaque)
{
    MiBandTouch *touch = opaque;

    if (!touch->count || touch->pending_irq_retries >= TOUCH_MAX_IRQ_RETRIES) {
        return;
    }
    touch->pending_irq_retries++;
    if (!touch->read_has_report && !touch->irq_pending(touch->irq_opaque)) {
        touch->raise_irq(touch->irq_opaque);
        if (touch->pending_irq_retries == 1 ||
            touch->pending_irq_retries % 20 == 0) {
            qemu_log_mask(LOG_UNIMP,
                          "miband touch pending report IRQ count=%u retry=%u\n",
                          touch->count, touch->pending_irq_retries);
        }
    }
    timer_mod(touch->pending_report_irq,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TOUCH_RETRY_DELAY_NS);
}

void miband_touch_report(MiBandTouch *touch, uint16_t x, uint16_t y, uint8_t event)
{
    uint8_t *report;
    bool was_empty = touch->count == 0;

    if (touch->count == MIBAND_TOUCH_QUEUE_LENGTH) {
        if (touch->read_has_report) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "miband touch report queue full, dropping newest pending\n");
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "miband touch report queue full, dropping oldest\n");
            touch->head = (touch->head + 1) % MIBAND_TOUCH_QUEUE_LENGTH;
        }
        touch->count--;
    }
    report = touch->reports[(touch->head + touch->count) %
                            MIBAND_TOUCH_QUEUE_LENGTH];
    memset(report, 0, MIBAND_TOUCH_REPORT_LENGTH);
    report[0] = 1;
    report[4] = x;
    report[5] = x >> 8;
    report[6] = y;
    report[7] = y >> 8;
    report[9] = event;
    touch->count++;
    if (was_empty) {
        touch->pending_irq_retries = 0;
    }
    /* A report can arrive while firmware is clearing the GPIO latch.  If the
     * line is low, the newly queued report must restore its one-shot IRQ. */
    if (was_empty || touch->irq_pending(touch->irq_opaque) == 0) {
        touch->raise_irq(touch->irq_opaque);
    }
    timer_mod(touch->pending_report_irq,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              TOUCH_RETRY_DELAY_NS);
    qemu_log_mask(LOG_UNIMP,
                  "miband touch queued x=%u y=%u event=%02x virtual_ms=%" PRIi64
                  " host_ms=%" PRIi64 "\n",
                  x, y, event,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000,
                  qemu_clock_get_ns(QEMU_CLOCK_HOST) / 1000000);
}

void miband_touch_init(MiBandTouch *touch, uint16_t chip_id,
                      void (*raise_irq)(void *), bool (*irq_pending)(void *),
                      void *opaque)
{
    touch->chip_id = chip_id;
    touch->raise_irq = raise_irq;
    touch->irq_pending = irq_pending;
    touch->irq_opaque = opaque;
    touch->pending_report_irq = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                            miband_touch_raise_pending_report, touch);
}

uint8_t miband_touch_read_register(MiBandTouch *touch, unsigned reg, unsigned offset)
{
    if (reg == 0xcc && offset < 2) {
        return touch->chip_id >> (8 * offset);
    }
    if (reg == 0x1c00 && offset < 2) {
        return 0x5a49 >> (8 * offset);
    }
    if (reg == 0x8000) {
        return miband_touch_read_byte(touch, offset);
    }
    if (reg == 0x2601) {
        return miband_touch_read_gesture_byte(touch, offset);
    }
    return 0;
}

uint8_t miband_touch_read_byte(MiBandTouch *touch, unsigned offset)
{
    uint8_t result = 0;

    if (offset == 0) {
        touch->read_has_report = touch->count != 0;
    }
    if (touch->read_has_report && offset < MIBAND_TOUCH_REPORT_LENGTH) {
        result = touch->reports[touch->head][offset];
        if (offset == MIBAND_TOUCH_REPORT_LENGTH - 1) {
            qemu_log_mask(LOG_UNIMP,
                          "miband touch report consumed count_before=%u\n",
                          touch->count);
            touch->head = (touch->head + 1) % MIBAND_TOUCH_QUEUE_LENGTH;
            touch->count--;
            touch->read_has_report = false;
            touch->pending_irq_retries = 0;
            if (touch->count) {
                timer_mod(touch->pending_report_irq,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          TOUCH_NEXT_REPORT_DELAY_NS);
            } else {
                timer_del(touch->pending_report_irq);
            }
        }
    }
    return result;
}

uint8_t miband_touch_read_gesture_byte(MiBandTouch *touch, unsigned offset)
{
    bool press_pending = false;

    if (offset != 0) {
        return 0;
    }
    for (unsigned i = 0; i < touch->count; i++) {
        if (touch->reports[(touch->head + i) % MIBAND_TOUCH_QUEUE_LENGTH][9]
            == 2) {
            press_pending = true;
            break;
        }
    }
    if (!press_pending) {
        return 0;
    }

    /* The low-power gesture read acknowledges the reports that caused it. */
    touch->head = (touch->head + touch->count) % MIBAND_TOUCH_QUEUE_LENGTH;
    touch->count = 0;
    touch->read_has_report = false;
    touch->pending_irq_retries = 0;
    timer_del(touch->pending_report_irq);
    qemu_log_mask(LOG_UNIMP, "miband touch wake gesture=1 consumed\n");
    return 1;
}


