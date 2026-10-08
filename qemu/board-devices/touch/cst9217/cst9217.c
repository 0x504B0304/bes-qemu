/* CST9217 candidate: observed I2C command and single-contact report protocol.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "miband_touch_input.h"

#define TYPE_CST9217 "cst9217"
OBJECT_DECLARE_SIMPLE_TYPE(Cst9217State, CST9217)
#define REPORTS 64

struct Cst9217State {
    I2CSlave parent_obj;
    DeviceState *irq_target, *reset_target;
    uint32_t irq_pin, reset_pin, width, height;
    uint32_t tap_duration_ms, tap_gap_ms, tap_slop;
    bool host_input, reading_report, reset_asserted, wake_pending;
    bool reset_contact;
    bool tap_down, tap_valid, first_tap, second_tap;
    int64_t tap_started, tap_released;
    uint16_t tap_x, tap_y, first_x, first_y;
    uint8_t tx[16], mode, report[REPORTS][10];
    unsigned tx_count, rx_count, head, count;
    uint16_t selector, boot_address, wake_x, wake_y;
    uint8_t boot_bank;
    qemu_irq irq;
    QEMUTimer *next_irq;
    MiBandTouchInput input;
};

static void cst9217_assert_irq(void *opaque)
{
    Cst9217State *s = opaque;
    qemu_set_irq(s->irq, !s->count);
}

static void cst9217_clear_reports(Cst9217State *s)
{
    timer_del(s->next_irq);
    s->head = s->count = 0;
    s->reading_report = false;
    s->wake_pending = false;
    s->tap_down = s->tap_valid = s->first_tap = s->second_tap = false;
    miband_touch_input_reset(&s->input);
    qemu_set_irq(s->irq, 1);
}

static bool cst9217_near(Cst9217State *s, uint16_t x, uint16_t y,
                         uint16_t origin_x, uint16_t origin_y)
{
    return abs((int)x - origin_x) <= s->tap_slop &&
           abs((int)y - origin_y) <= s->tap_slop;
}

/* Recognition limits describe the host pointer gesture policy, not measured
 * controller scan timing. Guest-visible gesture encoding comes from D000. */
static bool cst9217_double_tap(Cst9217State *s, uint16_t x, uint16_t y,
                               uint8_t kind)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->wake_pending) {
        return false;
    }
    if (kind == 2) {
        s->second_tap = s->first_tap && now >= s->tap_released &&
            now - s->tap_released <= (int64_t)s->tap_gap_ms * 1000000 &&
            cst9217_near(s, x, y, s->first_x, s->first_y);
        s->first_tap = false;
        s->tap_down = s->tap_valid = true;
        s->tap_started = now;
        s->tap_x = x;
        s->tap_y = y;
        return false;
    }
    if (!s->tap_down) {
        return false;
    }
    if (!cst9217_near(s, x, y, s->tap_x, s->tap_y)) {
        s->tap_valid = false;
    }
    if (kind != 8) {
        return false;
    }
    s->tap_down = false;
    if (!s->tap_valid || now < s->tap_started ||
        now - s->tap_started > (int64_t)s->tap_duration_ms * 1000000) {
        s->first_tap = s->second_tap = false;
        return false;
    }
    if (s->second_tap) {
        s->second_tap = false;
        return true;
    }
    s->first_tap = true;
    s->tap_released = now;
    s->first_x = x;
    s->first_y = y;
    return false;
}

static void cst9217_report(void *opaque, uint16_t x, uint16_t y, uint8_t kind)
{
    Cst9217State *s = opaque;
    uint8_t *r;
    unsigned tail;
    bool gesture = false;

    if (s->reset_asserted) {
        return;
    }

    /* D105 disables scanning. D104 enables gesture reports, rather than
     * ordinary contacts, until the guest resumes the controller. */
    if (s->mode == 5) {
        return;
    }
    if (s->mode == 4) {
        if (!cst9217_double_tap(s, x, y, kind)) {
            return;
        }
        gesture = true;
        s->wake_pending = true;
        s->wake_x = x;
        s->wake_y = y;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cst9217 double-tap gesture x=%u y=%u\n", x, y);
    }

    /* Coalesce motion, keeping press/release edges when the guest is slow. */
    if (kind == 1 && s->count > 1) {
        tail = (s->head + s->count - 1) % REPORTS;
        if (s->report[tail][0] == 7) {
            s->count--;
        }
    }
    if (s->count == REPORTS) {
        /* Remove an intermediate motion, never the head being read. */
        for (unsigned i = 1; i < s->count; i++) {
            if (s->report[(s->head + i) % REPORTS][0] == 7) {
                for (unsigned j = i; j + 1 < s->count; j++) {
                    memcpy(s->report[(s->head + j) % REPORTS],
                           s->report[(s->head + j + 1) % REPORTS], 10);
                }
                s->count--;
                break;
            }
        }
        if (s->count == REPORTS) {
            return;
        }
    }
    tail = (s->head + s->count) % REPORTS;
    r = s->report[tail];
    memset(r, 0, 10);
    r[0] = gesture ? 0 : kind == 2 ? 6 : kind == 8 ? 0 : 7;
    r[1] = x >> 4;
    r[2] = y >> 4;
    r[3] = ((x & 15) << 4) | (y & 15);
    r[4] = gesture ? 0x10 : 0;
    r[5] = gesture ? 0 : 1;
    r[6] = 0xab;
    s->count++;
    if (s->count == 1 && !timer_pending(s->next_irq)) {
        cst9217_assert_irq(s);
    }
}

static void cst9217_command(Cst9217State *s)
{
    if (s->tx_count < 2) {
        return;
    }
    s->selector = ((uint16_t)s->tx[0] << 8) | s->tx[1];
    s->rx_count = 0;
    if (s->tx[0] == 0xd1) {
        s->mode = s->tx[1];
        if (s->mode == 4 || s->mode == 5) {
            cst9217_clear_reports(s);
        } else {
            s->tap_down = s->tap_valid = s->first_tap = s->second_tap = false;
        }
    } else if (s->selector == 0xa010 && s->tx_count == 3) {
        s->boot_bank = s->tx[2];
    } else if (s->selector == 0xa00c && s->tx_count == 4) {
        s->boot_address = s->tx[2] | (s->tx[3] << 8);
    } else if (s->selector == 0x0005 && s->tx_count == 3 &&
               s->tx[2] == 0 && s->reading_report) {
        s->reading_report = false;
        if (s->count) {
            s->head = (s->head + 1) % REPORTS;
            s->count--;
        }
        s->wake_pending = false;
        qemu_set_irq(s->irq, 1);
        if (s->count) {
            /* The driver masks GPIO while its worker reads/acknowledges. */
            timer_mod(s->next_irq, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
        }
    }
}

static int cst9217_event(I2CSlave *dev, enum i2c_event event)
{
    Cst9217State *s = CST9217(dev);
    if (s->reset_asserted &&
        (event == I2C_START_SEND || event == I2C_START_RECV)) {
        return -1;
    }
    if (event == I2C_START_SEND) {
        s->tx_count = 0;
    } else if (event == I2C_START_RECV) {
        cst9217_command(s);
        s->rx_count = 0;
    } else if (event == I2C_FINISH && s->tx_count) {
        cst9217_command(s);
        s->tx_count = 0;
    }
    return 0;
}

static int cst9217_send(I2CSlave *dev, uint8_t data)
{
    Cst9217State *s = CST9217(dev);
    if (s->tx_count == sizeof(s->tx)) {
        return -1;
    }
    s->tx[s->tx_count++] = data;
    return 0;
}

static uint8_t cst9217_recv(I2CSlave *dev)
{
    Cst9217State *s = CST9217(dev);
    unsigned i = s->rx_count++;
    uint32_t value = 0;
    if (s->selector == 0xd000) {
        s->reading_report = true;
        if (i >= 10) {
            return 0;
        }
        return s->count ? s->report[s->head][i] : (i == 6 ? 0xab : 0);
    }
    switch (s->selector) {
    case 0x2601:
        if (i == 0 && s->wake_pending) {
            s->wake_pending = false;
            cst9217_clear_reports(s);
            value = 1;
        }
        break;
    case 0x0002: value = (uint32_t)s->mode << 8; break;
    case 0xd224: value = 0xcaca9217; break;
    case 0xd220: value = 1; break;
    case 0xd204: value = 0x92170000; break;
    case 0xd21c: value = 0x92170000; break;
    case 0xa002: value = 0xb055; break;
    case 0xa018:
        if (s->boot_bank == 1 && s->boot_address == 0x077c) {
            value = 0xcaca9217;
        } else if (s->boot_address == 0x7fc0) {
            value = 1;
        }
        break;
    default: break;
    }
    return i < 4 ? value >> (8 * i) : 0;
}

static void cst9217_reset(DeviceState *dev)
{
    Cst9217State *s = CST9217(dev);
    timer_del(s->next_irq);
    s->mode = s->tx_count = s->rx_count = s->head = s->count = 0;
    s->selector = s->boot_address = s->boot_bank = 0;
    s->wake_x = s->wake_y = 0;
    s->wake_pending = false;
    s->tap_down = s->tap_valid = s->first_tap = s->second_tap = false;
    s->tap_started = s->tap_released = 0;
    s->tap_x = s->tap_y = s->first_x = s->first_y = 0;
    s->reset_contact = false;
    s->reading_report = false;
    miband_touch_input_reset(&s->input);
    qemu_set_irq(s->irq, 1);
}

static void cst9217_reset_input(void *opaque, int pin, int level)
{
    Cst9217State *s = opaque;
    bool asserted = level == 0;
    bool rescan = !asserted && s->reset_asserted;

    if (asserted && !s->reset_asserted) {
        bool pressed = s->input.pressed;

        cst9217_reset(DEVICE(s));
        /* A controller reset does not lift a finger from its surface. Keep
         * sampling host transitions while reports are disabled, then scan
         * the current contact when reset is released. In particular, a
         * release after the wake worker's reset must still reach the guest.
         * A full machine reset uses cst9217_reset and discards input state. */
        s->reset_contact = pressed;
        s->input.pressed = s->input.reported_pressed = pressed;
    }
    s->reset_asserted = asserted;
    if (rescan) {
        if (s->input.pressed || s->reset_contact) {
            cst9217_report(s, s->input.x, s->input.y,
                           s->input.pressed ? 2 : 8);
        }
        s->reset_contact = false;
    }
}

static void cst9217_realize(DeviceState *dev, Error **errp)
{
    Cst9217State *s = CST9217(dev);
    if (!s->width || !s->height || s->width > 4096 || s->height > 4096 ||
        !s->tap_duration_ms || s->tap_duration_ms > 10000 ||
        !s->tap_gap_ms || s->tap_gap_ms > 10000 || s->tap_slop > 4096 ||
        (s->irq_target && s->irq_pin >= 96) ||
        (s->reset_target && s->reset_pin >= 96)) {
        error_setg(errp, "invalid touch geometry or GPIO pin");
        return;
    }
    if (s->irq_target) {
        qdev_connect_gpio_out(dev, 0,
            qdev_get_gpio_in_named(s->irq_target, "input", s->irq_pin));
    }
    if (s->host_input) {
        miband_touch_input_init(&s->input, cst9217_report, s,
                                s->width, s->height, NULL);
    }
    if (s->reset_target) {
        qdev_connect_gpio_out_named(s->reset_target, "output", s->reset_pin,
                                   qdev_get_gpio_in_named(dev, "reset", 0));
    }
    cst9217_reset(dev);
}

static void cst9217_unrealize(DeviceState *dev)
{
    Cst9217State *s = CST9217(dev);
    miband_touch_input_cleanup(&s->input);
    timer_del(s->next_irq);
    qemu_set_irq(s->irq, 1);
}

static void cst9217_init(Object *obj)
{
    Cst9217State *s = CST9217(obj);
    qdev_init_gpio_out(DEVICE(s), &s->irq, 1);
    qdev_init_gpio_in_named(DEVICE(s), cst9217_reset_input, "reset", 1);
    s->next_irq = timer_new_ns(QEMU_CLOCK_VIRTUAL, cst9217_assert_irq, s);
}

static void cst9217_finalize(Object *obj)
{
    timer_free(CST9217(obj)->next_irq);
}

static Property cst9217_properties[] = {
    DEFINE_PROP_LINK("irq-target", Cst9217State, irq_target, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT32("irq-pin", Cst9217State, irq_pin, 0),
    DEFINE_PROP_LINK("reset-target", Cst9217State, reset_target, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT32("reset-pin", Cst9217State, reset_pin, 0),
    DEFINE_PROP_UINT32("width", Cst9217State, width, 480),
    DEFINE_PROP_UINT32("height", Cst9217State, height, 480),
    DEFINE_PROP_BOOL("host-input", Cst9217State, host_input, false),
    DEFINE_PROP_UINT32("tap-duration-ms", Cst9217State, tap_duration_ms, 250),
    DEFINE_PROP_UINT32("tap-gap-ms", Cst9217State, tap_gap_ms, 500),
    DEFINE_PROP_UINT32("tap-slop", Cst9217State, tap_slop, 24),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_cst9217 = {
    .name = TYPE_CST9217,
    .unmigratable = true,
};

static void cst9217_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    dc->desc = "CST9217 candidate single-contact touch controller";
    dc->realize = cst9217_realize;
    dc->unrealize = cst9217_unrealize;
    dc->reset = cst9217_reset;
    dc->vmsd = &vmstate_cst9217;
    device_class_set_props(dc, cst9217_properties);
    ic->event = cst9217_event;
    ic->send = cst9217_send;
    ic->recv = cst9217_recv;
}

static const TypeInfo cst9217_info = {
    .name = TYPE_CST9217,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Cst9217State),
    .instance_init = cst9217_init,
    .instance_finalize = cst9217_finalize,
    .class_init = cst9217_class_init,
};

static void cst9217_register_types(void)
{
    type_register_static(&cst9217_info);
}
type_init(cst9217_register_types)
