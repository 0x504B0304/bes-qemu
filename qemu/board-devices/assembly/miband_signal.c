#include "qemu/osdep.h"
#include "miband_peripherals.h"
#include "miband_signal.h"

void miband_board_gpio_edge(void *opaque)
{
    MiBandBoardGpioLine *line = opaque;
    miband_gpio_raise_edge(line->gpio, line->pin);
}

void miband_board_gpio_active_low(void *opaque, bool asserted)
{
    MiBandBoardGpioLine *line = opaque;
    miband_gpio_set_input(line->gpio, line->pin, !asserted);
    if (asserted) {
        miband_gpio_raise_edge(line->gpio, line->pin);
    }
}

void miband_board_gpio_inferred_edge(void *opaque)
{
    MiBandBoardGpioLine *line = opaque;
    miband_gpio_raise_inferred_edge(line->gpio, line->pin);
}

bool miband_board_gpio_pending(void *opaque)
{
    MiBandBoardGpioLine *line = opaque;
    return miband_gpio_pin_pending(line->gpio, line->pin);
}
