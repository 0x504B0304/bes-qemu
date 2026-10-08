/* Zinitix single-contact I2C subset, observed in the reference AP driver.
 * No guest addresses or guest event-queue writes belong in this device.
 * Low-power tap/slide status follows the reference driver's register protocol.
 * Gesture recognition approximates direction/distance, not hardware timing.
 * Firmware download and raw-node scans are not modeled.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "miband_zinitix.h"

#define REPORT_RETRY_NS 50000000
#define REPORT_MAX_RETRIES 200
#define SLIDE_UP_MIN_DISTANCE 48
#define TAP_MAX_NS 300000000
#define DOUBLE_TAP_GAP_NS 500000000
#define TAP_MAX_DISTANCE 24

static void zinitix_irq(MiBandZinitix *touch, bool asserted)
{
    if (touch->irq_asserted != asserted) {
        touch->irq_asserted = asserted;
        touch->irq(touch->irq_opaque, asserted);
    }
}

static void zinitix_next_report(void *opaque)
{
    MiBandZinitix *touch = opaque;
    if (touch->count && !touch->sleeping && touch->irq_retries < REPORT_MAX_RETRIES) {
        /* Firmware temporarily disables its GPIO IRQ while processing a
         * report. A queued input may assert INT in that window. Re-notify a
         * pending report with a bounded device pulse; the GPIO gate remains
         * authoritative and the guest still reads/acknowledges over I2C.
         * This is a scan/notification timing approximation, not a measured
         * Zinitix scan interval or an AP event-queue injection. */
        ++touch->irq_retries;
        zinitix_irq(touch, false);
        zinitix_irq(touch, true);
        timer_mod(touch->next_report,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + REPORT_RETRY_NS);
    }
}

void miband_zinitix_reset(MiBandZinitix *touch)
{
    timer_del(touch->next_report);
    touch->head = touch->count = 0;
    touch->irq_retries = 0;
    touch->tx_count = touch->read_offset = 0;
    touch->sleeping = false;
    touch->reset_asserted = false;
    touch->gesture_pending = 0;
    touch->gesture_contact = false;
    touch->first_tap = false;
    touch->vendor_unlocked = false;
    memset(touch->registers, 0, sizeof(touch->registers));
    /* Read-only health signatures, not AP firmware version claims. */
    touch->registers[0x1c] = 0x5a49;
    touch->registers[0x12c] = 0x55aa;
    touch->registers[0xcc00] = touch->chip_id;
    zinitix_irq(touch, false);
}

void miband_zinitix_reset_line(void *opaque, bool driven, bool high)
{
    MiBandZinitix *touch = opaque;
    bool asserted = driven && !high;
    if (asserted != touch->reset_asserted) {
        if (asserted) {
            miband_zinitix_reset(touch);
        }
        touch->reset_asserted = asserted;
        qemu_log_mask(LOG_UNIMP, "miband Zinitix reset asserted=%u\n", asserted);
    }
}

static void zinitix_command(MiBandZinitix *touch)
{
    switch (touch->reg) {
    case 0: /* SWRESET */
        miband_zinitix_reset(touch);
        break;
    case 3: /* CLEAR_INT_STATUS: read does not consume the contact. */
        touch->gesture_pending = 0;
        touch->gesture_contact = false;
        touch->first_tap = false;
        if (touch->irq_asserted && touch->count) {
            touch->head = (touch->head + 1) % MIBAND_ZINITIX_QUEUE_LENGTH;
            touch->count--;
        }
        zinitix_irq(touch, false);
        touch->irq_retries = 0;
        if (touch->count) {
            timer_mod(touch->next_report,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000000);
        } else {
            timer_del(touch->next_report);
        }
        break;
    case 5: /* SLEEP */
        touch->sleeping = true;
        timer_del(touch->next_report);
        touch->gesture_pending = 0;
        touch->gesture_contact = false;
        touch->first_tap = false;
        zinitix_irq(touch, false);
        break;
    case 4: /* WAKEUP */
    case 0xff8: /* reference vendor resume command after gesture sleep. */
        touch->sleeping = false;
        touch->gesture_pending = 0;
        touch->gesture_contact = false;
        touch->first_tap = false;
        zinitix_irq(touch, false);
        zinitix_next_report(touch);
        break;
    default:
        break;
    }
}

static void zinitix_latch(MiBandZinitix *touch)
{
    if (touch->tx_count >= 2) {
        touch->reg = touch->tx[0] | (touch->tx[1] << 8);
    }
    touch->read_offset = 0;
}

static void zinitix_start(void *opaque, bool read)
{
    MiBandZinitix *touch = opaque;
    if (read) {
        zinitix_latch(touch);
    } else {
        touch->tx_count = 0;
    }
    touch->reading = read;
}

static void zinitix_write(void *opaque, uint8_t value)
{
    MiBandZinitix *touch = opaque;
    if (touch->tx_count < sizeof(touch->tx)) {
        touch->tx[touch->tx_count] = value;
    }
    touch->tx_count++;
}

static uint8_t zinitix_read(void *opaque)
{
    MiBandZinitix *touch = opaque;
    unsigned offset = touch->read_offset++;
    uint8_t result;
    if (touch->reset_asserted) {
        return 0;
    }
    if (touch->reg == 0xcc01) {
        /* reference writes the 32-bit vendor unlock key 0x0018be7c here,
         * then checks the four-byte acknowledgement 0xa5a5a5a5. */
        result = offset < 4 && touch->vendor_unlocked ? 0xa5 : 0;
    } else if (touch->reg == 0x9d2) {
        /* Identification block: chip code at byte 10, vendor revision unknown. */
        result = offset == 10 ? touch->chip_id & 0xff :
                 offset == 11 ? touch->chip_id >> 8 : 0;
    } else if (touch->reg == 0x80) {
        result = offset < 10 && touch->count ? touch->reports[touch->head][offset] : 0;
    } else if (touch->reg == 0x126) {
        /* reference reads two LE bytes, then acknowledges using CLEAR_INT_STATUS.
         * Its low-power path accepts 1 for enabled single-tap, 4 for slide-up.
         * Keep the status stable across reads, just like ordinary contacts. */
        result = offset < 2 ? touch->gesture_pending >> (8 * offset) : 0;
        if (!offset && touch->gesture_pending) {
            qemu_log_mask(LOG_UNIMP, "miband Zinitix gesture read status=%u\n",
                          touch->gesture_pending);
        }
    } else if (touch->reg == 0x1fb) {
        /* The controller's low-power status, checked by the reference health
         * worker. A wake gesture is only an indication: remain asleep until
         * the AP explicitly resumes the controller. */
        result = offset == 0 && touch->sleeping ? 1 : 0;
    } else {
        result = offset < 2 ? touch->registers[touch->reg] >> (8 * offset) : 0;
    }
    if (!offset && touch->trace_events++ < 120) {
        qemu_log_mask(LOG_UNIMP, "miband Zinitix read reg=%04x queued=%u\n",
                      touch->reg, touch->count);
    }
    return result;
}

static void zinitix_stop(void *opaque)
{
    MiBandZinitix *touch = opaque;
    if (!touch->reading && !touch->reset_asserted) {
        zinitix_latch(touch);
        if (touch->tx_count == 2) {
            zinitix_command(touch);
        } else if (touch->tx_count == 4) {
            touch->registers[touch->reg] = touch->tx[2] | (touch->tx[3] << 8);
            if (touch->reg == 0x126 && !touch->registers[0x126]) {
                /* reference/reference acknowledge wake status by writing zero here. */
                if (touch->gesture_pending) {
                    qemu_log_mask(LOG_UNIMP,
                                  "miband Zinitix gesture ack status=%u\n",
                                  touch->gesture_pending);
                }
                touch->gesture_pending = 0;
                touch->gesture_contact = touch->first_tap = false;
                zinitix_irq(touch, false);
            }
            if (touch->reg == 0xaf) {
                touch->gesture_contact = false;
            }
        } else if (touch->tx_count == 6 && touch->reg == 0xcc01) {
            touch->vendor_unlocked = touch->tx[2] == 0x7c &&
                touch->tx[3] == 0xbe && touch->tx[4] == 0x18 &&
                touch->tx[5] == 0;
        }
    }
    touch->tx_count = 0;
}

static void zinitix_bus_reset(void *opaque)
{
    MiBandZinitix *touch = opaque;
    touch->tx_count = touch->read_offset = 0;
    touch->reading = false;
}

MiBandI2cEndpoint miband_zinitix_endpoint(MiBandZinitix *touch, uint8_t address)
{
    return (MiBandI2cEndpoint) { address, touch, zinitix_start, zinitix_write,
                               zinitix_read, zinitix_stop, zinitix_bus_reset };
}

void miband_zinitix_init(MiBandZinitix *touch, uint16_t chip_id,
                         void (*irq)(void *, bool), void *opaque)
{
    touch->chip_id = chip_id;
    touch->double_tap_status = 1;
    touch->irq = irq;
    touch->irq_opaque = opaque;
    touch->next_report = timer_new_ns(QEMU_CLOCK_VIRTUAL, zinitix_next_report, touch);
    miband_zinitix_reset(touch);
    touch->irq(touch->irq_opaque, false);
}

void miband_zinitix_report(void *opaque, uint16_t x, uint16_t y, uint8_t event)
{
    miband_zinitix_report_clocked(opaque, x, y, event, QEMU_CLOCK_VIRTUAL);
}

void miband_zinitix_report_clocked(void *opaque, uint16_t x, uint16_t y,
                                  uint8_t event, QEMUClockType clock)
{
    MiBandZinitix *touch = opaque;
    uint8_t *report;
    if (touch->reset_asserted) {
        return;
    }
    if (touch->gesture_clock != clock) {
        /* Never join half gestures from different input time domains. */
        touch->gesture_contact = touch->first_tap = false;
        touch->gesture_clock = clock;
    }
    if (touch->sleeping && touch->sleep_gesture == 1) {
        /* Command5 arms double tap; the board-selected status at 0x126
         * is consumed and cleared over I2C. Timing/distance thresholds are
         * an explicit recognizer approximation, not measured silicon data. */
        int64_t now = qemu_clock_get_ns(clock);
        if (touch->gesture_pending) {
            return;
        }
        if (event == 2) {
            if (touch->first_tap &&
                (now - touch->tap_up_ns > DOUBLE_TAP_GAP_NS ||
                 abs((int)x - touch->tap_x) > TAP_MAX_DISTANCE ||
                 abs((int)y - touch->tap_y) > TAP_MAX_DISTANCE)) {
                touch->first_tap = false;
            }
            touch->gesture_contact = true;
            touch->tap_down_ns = now;
            touch->tap_x = x;
            touch->tap_y = y;
        } else if (touch->gesture_contact) {
            if (abs((int)x - touch->tap_x) > TAP_MAX_DISTANCE ||
                abs((int)y - touch->tap_y) > TAP_MAX_DISTANCE ||
                now - touch->tap_down_ns > TAP_MAX_NS) {
                touch->gesture_contact = touch->first_tap = false;
            } else if (event == 8) {
                touch->gesture_contact = false;
                if (touch->first_tap) {
                    touch->first_tap = false;
                    touch->gesture_pending = touch->double_tap_status;
                    zinitix_irq(touch, true);
                    qemu_log_mask(LOG_UNIMP,
                                  "miband Zinitix double-tap wake status=%u\n",
                                  touch->gesture_pending);
                } else {
                    touch->first_tap = true;
                    touch->tap_up_ns = now;
                }
            }
        }
        return;
    }
    if (touch->sleeping && (touch->registers[0xaf] & 0x12)) {
        /* In gesture mode a contact is a wake indication, not a queued UI
         * press. The AP resumes the controller before normal reports restart.
         * Do not invent a gesture when firmware disabled the tap bit. */
        if (touch->gesture_pending) {
            return; /* Retain the first indication until CLEAR_INT_STATUS. */
        }
        if (event == 2 && (touch->registers[0xaf] & 2)) {
            touch->gesture_pending = 1;
            zinitix_irq(touch, true);
            qemu_log_mask(LOG_UNIMP, "miband Zinitix single-tap wake status=1\n");
        } else if (touch->registers[0xaf] & 0x10) {
            /* Host coordinates are device coordinates. Recognize a completed
             * upward stroke with at least 48 units of travel and horizontal
             * excursion no greater than half its upward distance. This is an
             * explicit input approximation, not the proprietary recognizer.
             * A press alone, orphan move/release or another direction cannot
             * create status 4. The waking stroke never enters the UI FIFO. */
            if (event == 2) {
                touch->gesture_contact = true;
                touch->gesture_x = x;
                touch->gesture_y = y;
                touch->gesture_dx = 0;
            } else if (touch->gesture_contact) {
                unsigned dx = abs((int)x - touch->gesture_x);
                int dy = (int)touch->gesture_y - y;
                touch->gesture_dx = MAX(touch->gesture_dx, dx);
                if (event == 8) {
                    touch->gesture_contact = false;
                    if (dy >= SLIDE_UP_MIN_DISTANCE &&
                        touch->gesture_dx <= (unsigned)dy / 2) {
                        touch->gesture_pending = 4;
                        zinitix_irq(touch, true);
                        qemu_log_mask(LOG_UNIMP,
                                      "miband Zinitix slide-up wake status=4\n");
                    }
                }
            }
        }
        return;
    }
    /* Retain the legacy contact-wake fallback for boards without an armed
     * gesture protocol. Gesture-mode boards return above without producing
     * ordinary contacts until the AP resumes or resets the controller. */
    if (touch->sleeping) {
        touch->sleeping = false;
        qemu_log_mask(LOG_UNIMP,
                      "miband Zinitix host touch wake x=%u y=%u\n", x, y);
    }
    if (touch->count == MIBAND_ZINITIX_QUEUE_LENGTH) {
        return;
    }
    report = touch->reports[(touch->head + touch->count) % MIBAND_ZINITIX_QUEUE_LENGTH];
    memset(report, 0, 10);
    report[0] = 1; /* point exists; no palm flag */
    report[2] = event == 8 ? 0 : 1;
    report[4] = x;
    report[5] = x >> 8;
    report[6] = y;
    report[7] = y >> 8;
    report[8] = 1; /* contact width */
    report[9] = event == 2 ? 3 : event == 8 ? 8 : 5;
    touch->irq_retries = 0;
    touch->count++;
    if (touch->count == 1) {
        zinitix_irq(touch, true);
    }
    if (!timer_pending(touch->next_report)) {
        timer_mod(touch->next_report,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + REPORT_RETRY_NS);
    }
    qemu_log_mask(LOG_UNIMP, "miband Zinitix contact x=%u y=%u event=%u queued=%u\n",
                  x, y, event, touch->count);
}

void miband_zinitix_shutdown(MiBandZinitix *touch)
{
    if (touch->next_report) {
        timer_free(touch->next_report);
        touch->next_report = NULL;
    }
}
