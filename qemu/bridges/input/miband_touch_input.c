/* Host QMP/VNC absolute input to a device report interface. */
#include "qemu/osdep.h"
#include "sysemu/qtest.h"
#include "miband_touch_input.h"

static void touch_input_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    MiBandTouchInput *input = (MiBandTouchInput *)dev;
    /* Host gestures must not speed up with the AP's icount clock. QTest
     * owns its event timeline explicitly through clock_step instead. VNC
     * and QMP can both use a NULL console, so src cannot distinguish them. */
    input->event_clock = qtest_enabled() ? QEMU_CLOCK_VIRTUAL : QEMU_CLOCK_VIRTUAL_RT;
    if (evt->type == INPUT_EVENT_KIND_ABS) {
        InputMoveEvent *move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            input->x = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                             INPUT_EVENT_ABS_MAX, 0, input->width - 1);
        } else if (move->axis == INPUT_AXIS_Y) {
            input->y = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                             INPUT_EVENT_ABS_MAX, 0, input->height - 1);
        }
    } else if (evt->type == INPUT_EVENT_KIND_BTN) {
        InputBtnEvent *button = evt->u.btn.data;
        if (button->button == INPUT_BUTTON_LEFT) {
            input->pressed = button->down;
        }
    }
}

static void touch_report(MiBandTouchInput *input, uint8_t event)
{
    if (input->report_clocked) {
        input->report_clocked(input->report_opaque, input->x, input->y,
                              event, input->event_clock);
    } else {
        input->report(input->report_opaque, input->x, input->y, event);
    }
}

static void touch_input_sync(DeviceState *dev)
{
    MiBandTouchInput *input = (MiBandTouchInput *)dev;
    if (input->pressed != input->reported_pressed) {
        if (!input->pressed && input->on_release) {
            input->on_release();
        }
        touch_report(input, input->pressed ? 2 : 8);
    } else if (input->pressed &&
               (input->x != input->reported_x || input->y != input->reported_y)) {
        touch_report(input, 1);
    }
    input->reported_pressed = input->pressed;
    input->reported_x = input->x;
    input->reported_y = input->y;
}

static const QemuInputHandler touch_input_handler = {
    .name = "Mi Band touch",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = touch_input_event,
    .sync = touch_input_sync,
};

void miband_touch_input_init(MiBandTouchInput *input,
                            void (*report)(void *, uint16_t, uint16_t, uint8_t),
                            void *opaque,
                            unsigned width, unsigned height,
                            void (*on_release)(void))
{
    g_assert(width > 0 && height > 0 && width <= UINT16_MAX && height <= UINT16_MAX);
    input->report = report;
    input->report_clocked = NULL;
    input->report_opaque = opaque;
    input->width = width;
    input->height = height;
    input->on_release = on_release;
    input->x = (width - 1) / 2;
    input->y = (height - 1) / 2;
    input->reported_x = input->x;
    input->reported_y = input->y;
    input->input = qemu_input_handler_register((DeviceState *)input, &touch_input_handler);
    qemu_input_handler_activate(input->input);
}

void miband_touch_input_reset(MiBandTouchInput *input)
{
    input->pressed = input->reported_pressed = false;
    input->reported_x = input->x;
    input->reported_y = input->y;
}

void miband_touch_input_cleanup(MiBandTouchInput *input)
{
    if (input->input) {
        qemu_input_handler_unregister(input->input);
        input->input = NULL;
    }
}
