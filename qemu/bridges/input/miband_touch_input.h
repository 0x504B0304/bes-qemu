#ifndef MIBAND_TOUCH_INPUT_H
#define MIBAND_TOUCH_INPUT_H
#include "ui/input.h"
#include "qemu/timer.h"

typedef struct MiBandTouchInput {
    QemuInputHandlerState *input;
    void (*report)(void *opaque, uint16_t x, uint16_t y, uint8_t event);
    void (*report_clocked)(void *, uint16_t, uint16_t, uint8_t, QEMUClockType);
    QEMUClockType event_clock;
    void *report_opaque;
    unsigned width, height;
    uint16_t x, y, reported_x, reported_y;
    bool pressed, reported_pressed;
    void (*on_release)(void);
} MiBandTouchInput;

void miband_touch_input_init(MiBandTouchInput *input,
                            void (*report)(void *, uint16_t, uint16_t, uint8_t),
                            void *opaque,
                            unsigned width, unsigned height,
                            void (*on_release)(void));
void miband_touch_input_reset(MiBandTouchInput *input);
void miband_touch_input_cleanup(MiBandTouchInput *input);
#endif
