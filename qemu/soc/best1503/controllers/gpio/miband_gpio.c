/* BEST1503 GPIO interrupt banks; board assembly selects external pins.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "miband_peripherals.h"

/* GPIO0 is not contiguous with GPIO1/2. reference configures GPIO27 at 0x40081000. */
static const hwaddr gpio_bank_base[3] = { 0x40081000, 0x40089000, 0x4008a000 };
#define GPIO_COMMON_ENABLE 0x40085080
#define GPIO_COMMON_STATUS 0x4008508c

static uint32_t gpio_pending(const MiBandGpio *gpio, unsigned bank)
{
    uint32_t pending = gpio->status[bank] & gpio->irq_enable[bank] &
                       gpio->common_enable[bank] & ~gpio->irq_mask[bank];
    /* Preserve the existing inferred touch-report adapter's unmasked latch.
     * Its interrupt gating still needs a hardware trace; ordinary GPIO
     * edges, including TE, always use the bank's programmed masks. */
    pending |= gpio->status[bank] & gpio->inferred_unmasked[bank];
    return pending;
}

static void miband_gpio_update_irq(MiBandGpio *gpio)
{
    if (gpio->irq) {
        qemu_set_irq(gpio->irq, gpio_pending(gpio, 0) ||
                     gpio_pending(gpio, 1) || gpio_pending(gpio, 2));
    }
}

void miband_gpio_raise_edge(MiBandGpio *gpio, unsigned pin)
{
    unsigned bank = pin / 32;
    uint32_t bit = 1u << (pin % 32);

    assert(bank < 3);
    if (!(gpio->common_enable[bank] & bit) ||
        !(gpio->irq_enable[bank] & bit) || (gpio->irq_mask[bank] & bit)) {
        return;
    }
    gpio->status[bank] |= bit;
    miband_gpio_update_irq(gpio);
    if (++gpio->edges <= 64 || gpio->edges % 600 == 0) {
        qemu_log_mask(LOG_UNIMP, "miband GPIO edge=%u pin=%u status=%08x "
                      "virtual_ms=%" PRId64 "\n", gpio->edges, pin,
                      gpio->status[bank], qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
}

void miband_gpio_connect_irq(MiBandGpio *gpio, qemu_irq irq)
{
    gpio->irq = irq;
    miband_gpio_update_irq(gpio);
}

static void gpio_notify_output(MiBandGpio *gpio, unsigned pin)
{
    unsigned bank = pin / 32;
    uint32_t bit = 1u << (pin % 32);
    if (gpio->outputs[pin].notify) {
        gpio->outputs[pin].notify(gpio->outputs[pin].opaque,
                                  !!(gpio->output_enable[bank] & bit),
                                  !!(gpio->output[bank] & bit));
    }
}

void miband_gpio_connect_output(MiBandGpio *gpio, unsigned pin,
                                void (*notify)(void *, bool, bool), void *opaque)
{
    assert(pin < 96);
    gpio->outputs[pin].notify = notify;
    gpio->outputs[pin].opaque = opaque;
    gpio_notify_output(gpio, pin);
}

void miband_gpio_reset_outputs(MiBandGpio *gpio)
{
    memset(gpio->output, 0, sizeof(gpio->output));
    memset(gpio->output_enable, 0, sizeof(gpio->output_enable));
    for (unsigned pin = 0; pin < 96; pin++) {
        gpio_notify_output(gpio, pin);
    }
}

void miband_gpio_set_input(MiBandGpio *gpio, unsigned pin, bool high)
{
    unsigned bank = pin / 32;
    uint32_t bit = 1u << (pin % 32);
    assert(bank < 3);
    gpio->input_driven[bank] |= bit;
    if (high) {
        gpio->input[bank] |= bit;
    } else {
        gpio->input[bank] &= ~bit;
    }
}

void miband_gpio_raise_inferred_edge(MiBandGpio *gpio, unsigned pin)
{
    unsigned bank = pin / 32;
    uint32_t bit = 1u << (pin % 32);
    assert(bank < 3);
    gpio->inferred_unmasked[bank] |= bit;
    gpio->status[bank] |= bit;
    miband_gpio_update_irq(gpio);
    qemu_log_mask(LOG_UNIMP, "miband GPIO touch IRQ raised\n");
}

bool miband_gpio_read(MiBandGpio *gpio, hwaddr address, unsigned size,
                      uint64_t *result)
{
    for (unsigned bank = 0; bank < 3; bank++) {
        if (address == gpio_bank_base[bank]) {
            *result = gpio->output[bank];
            return true;
        }
        if (address == gpio_bank_base[bank] + 4) {
            *result = gpio->output_enable[bank];
            return true;
        }
        if (address == gpio_bank_base[bank] + 0x50 &&
            gpio->input_driven[bank]) {
            *result = gpio->input[bank];
            return true;
        }
        if (address == GPIO_COMMON_ENABLE + bank * 4) {
            *result = gpio->common_enable[bank];
            return true;
        }
        if (address == GPIO_COMMON_STATUS + bank * 4 ||
            address == gpio_bank_base[bank] + 0x44) {
            *result = gpio_pending(gpio, bank);
            return true;
        }
    }
    return false;
}

bool miband_gpio_write(MiBandGpio *gpio, hwaddr address, unsigned size,
                       uint64_t value)
{
    for (unsigned bank = 0; bank < 3; bank++) {
        hwaddr base = gpio_bank_base[bank];
        if (address == base || address == base + 4 ||
            address == base + 0xc || address == base + 0x10) {
            /* SET/CLEAR output latch and direction. reference HAL writes these
             * aliases to pulse its board-selected active-low touch reset. */
            uint32_t old_output = gpio->output[bank];
            uint32_t old_enable = gpio->output_enable[bank];
            uint32_t changed;
            if (address == base) {
                gpio->output[bank] |= value;
            } else if (address == base + 0xc) {
                gpio->output[bank] &= ~value;
            } else if (address == base + 4) {
                gpio->output_enable[bank] |= value;
            } else {
                gpio->output_enable[bank] &= ~value;
            }
            changed = (old_output ^ gpio->output[bank]) |
                      (old_enable ^ gpio->output_enable[bank]);
            for (unsigned bit = 0; bit < 32; bit++) {
                if (changed & (1u << bit)) {
                    gpio_notify_output(gpio, bank * 32 + bit);
                }
            }
            return true;
        }
        if (address == GPIO_COMMON_ENABLE + bank * 4) {
            gpio->common_enable[bank] = value;
        } else if (address == base + 0x18) {
            gpio->irq_enable[bank] &= ~value;
        } else if (address == base + 0x1c) {
            gpio->irq_enable[bank] |= value;
        } else if (address == base + 0x30) {
            gpio->irq_mask[bank] &= ~value;
        } else if (address == base + 0x34) {
            gpio->irq_mask[bank] |= value;
        } else if (address == base + 0x4c) {
            uint32_t cleared = gpio->status[bank] & value;
            gpio->status[bank] &= ~value;
            if (cleared && ++gpio->acks <= 64) {
                qemu_log_mask(LOG_UNIMP, "miband GPIO ack=%u bank=%u mask=%08x "
                              "virtual_ms=%" PRId64 "\n", gpio->acks, bank,
                              cleared, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
            }
        } else {
            continue;
        }
        miband_gpio_update_irq(gpio);
        return true;
    }
    return false;
}

bool miband_gpio_pin_pending(MiBandGpio *gpio, unsigned pin)
{
    assert(pin < 96);
    return (gpio->status[pin / 32] & (1u << (pin % 32))) != 0;
}
