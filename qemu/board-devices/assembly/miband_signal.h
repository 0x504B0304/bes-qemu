#ifndef MIBAND_SIGNAL_H
#define MIBAND_SIGNAL_H
#include <stdbool.h>
typedef struct MiBandBoardGpioLine {
    struct MiBandGpio *gpio;
    unsigned pin;
} MiBandBoardGpioLine;
void miband_board_gpio_edge(void *opaque);
void miband_board_gpio_active_low(void *opaque, bool asserted);
void miband_board_gpio_inferred_edge(void *opaque);
bool miband_board_gpio_pending(void *opaque);
#endif
