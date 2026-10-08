/* BEST1700 GPIO banks and their common interrupt gates.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "best1700_gpio.h"

const hwaddr best1700_gpio_mmio_base[BEST1700_GPIO_MMIO_COUNT] = {
    0x40081000, 0x40089000, 0x4008a000,
    0x400850fc, 0x400851a4, 0x400851c8,
    0x40085108, 0x400851a8, 0x400851cc,
    0x40086064, 0x40086068, 0x40086080,
    0x4008606c, 0x40086070, 0x40086084,
};

static uint32_t best1700_gpio_input_levels(const Best1700GpioBank *b)
{
    /* External board signals override weak pad bias. Floating pads retain
     * the existing low default; simultaneous pulls resolve low by policy. */
    return (b->input & b->driven) |
           (b->pull_up & ~b->pull_down & ~b->driven);
}

static uint32_t best1700_gpio_raw(const Best1700GpioBank *b)
{
    uint32_t level = ~(best1700_gpio_input_levels(b) ^ b->polarity) &
                     (b->driven | b->pull_up | b->pull_down) &
                     ~b->edge_type & ~b->direction;

    return (b->edge_pending | level) & b->enable;
}

static uint32_t best1700_gpio_pending(const Best1700GpioBank *b)
{
    uint32_t primary = best1700_gpio_raw(b) & ~b->mask;
    uint32_t falling = b->fall_pending & b->fall_enable & ~b->fall_mask;

    return (primary | falling) & b->common_enable;
}

static void best1700_gpio_update_irq(Best1700GpioState *s)
{
    bool pending = false;

    for (unsigned bank = 0; bank < BEST1700_GPIO_BANKS; bank++) {
        pending |= best1700_gpio_pending(&s->bank[bank]) != 0;
    }
    qemu_set_irq(s->irq, pending);
}

void best1700_gpio_set_input(Best1700GpioState *s, unsigned pin, bool high)
{
    Best1700GpioBank *b;
    uint32_t bit;
    uint32_t previous, levels;

    assert(pin < BEST1700_GPIO_PINS);
    b = &s->bank[pin / 32];
    bit = 1u << (pin % 32);
    previous = best1700_gpio_input_levels(b);
    b->driven |= bit;
    b->input = (b->input & ~bit) | (high ? bit : 0);
    levels = best1700_gpio_input_levels(b);
    if (((previous ^ levels) & bit) && !(b->direction & bit)) {
        if ((b->edge_type & b->enable & bit) &&
            high == ((b->polarity & bit) != 0)) {
            b->edge_pending |= bit;
        }
        if ((previous & bit) && (b->fall_enable & bit)) {
            b->fall_pending |= bit;
        }
    }
    best1700_gpio_update_irq(s);
}

static void best1700_gpio_input(void *opaque, int pin, int level)
{
    best1700_gpio_set_input(opaque, pin, level != 0);
}

static void best1700_gpio_outputs(Best1700GpioState *s, unsigned bank)
{
    Best1700GpioBank *b = &s->bank[bank];
    /* Undriven lines are pulled high by attached board devices. */
    uint32_t levels = b->output | ~b->direction;
    for (unsigned pin = 0; pin < 32; pin++) {
        qemu_set_irq(s->output[bank * 32 + pin], (levels >> pin) & 1);
    }
}

static uint64_t best1700_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700GpioRegion *region = opaque;
    Best1700GpioBank *b = &region->controller->bank[region->bank];

    if (region->kind == 1) {
        return b->common_enable;
    }
    if (region->kind == 2) {
        return best1700_gpio_pending(b);
    }
    if (region->kind == 3) {
        return b->pull_up;
    }
    if (region->kind == 4) {
        return b->pull_down;
    }
    switch (offset) {
    case 0x00: case 0x0c: return b->output;
    case 0x04: case 0x10: return b->direction;
    case 0x08: case 0x14: return b->control;
    case 0x18: case 0x30: return b->enable;
    case 0x1c: case 0x34: return b->mask;
    case 0x20: case 0x38: return b->edge_type;
    case 0x24: case 0x3c: return b->polarity;
    case 0x28: case 0x48: return b->debounce;
    case 0x40: return best1700_gpio_raw(b) & ~b->mask;
    case 0x44: return best1700_gpio_raw(b);
    case 0x50:
        return (best1700_gpio_input_levels(b) & ~b->direction) |
               (b->output & b->direction);
    case 0x60: return b->sync;
    case 0x80: case 0x84: return b->fall_enable;
    case 0x88: case 0x8c: return b->fall_mask;
    case 0x90: return b->fall_pending & b->fall_enable & ~b->fall_mask;
    case 0x94: return b->fall_pending & b->fall_enable;
    default: return 0;
    }
}

static void best1700_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    Best1700GpioRegion *region = opaque;
    Best1700GpioState *s = region->controller;
    Best1700GpioBank *b = &s->bank[region->bank];
    uint32_t bits = value;

    if (region->kind == 1) {
        b->common_enable = bits;
    } else if (region->kind == 3 || region->kind == 4) {
        uint32_t previous = best1700_gpio_input_levels(b), levels, changed;

        if (region->kind == 3) {
            b->pull_up = bits;
        } else {
            b->pull_down = bits;
        }
        levels = best1700_gpio_input_levels(b);
        changed = (previous ^ levels) & ~b->direction;
        b->edge_pending |= changed & b->edge_type & b->enable &
                           ~(levels ^ b->polarity);
        b->fall_pending |= changed & previous & b->fall_enable;
    } else if (region->kind == 0) {
        switch (offset) {
        case 0x00: b->output |= bits; break;
        case 0x0c: b->output &= ~bits; break;
        case 0x04: b->direction |= bits; break;
        case 0x10: b->direction &= ~bits; break;
        case 0x08: b->control |= bits; break;
        case 0x14: b->control &= ~bits; break;
        case 0x18: b->enable &= ~bits; break;
        case 0x30: b->enable |= bits; break;
        case 0x1c: b->mask &= ~bits; break;
        case 0x34: b->mask |= bits; break;
        case 0x20: b->edge_type &= ~bits; break;
        case 0x38: b->edge_type |= bits; break;
        case 0x24: b->polarity &= ~bits; break;
        case 0x3c: b->polarity |= bits; break;
        case 0x28: b->debounce &= ~bits; break;
        case 0x48: b->debounce |= bits; break;
        case 0x4c: b->edge_pending &= ~bits; break;
        case 0x60: b->sync = bits; break;
        case 0x80: b->fall_enable &= ~bits; break;
        case 0x84: b->fall_enable |= bits; break;
        case 0x88: b->fall_mask &= ~bits; break;
        case 0x8c: b->fall_mask |= bits; break;
        case 0x98: b->fall_pending &= ~bits; break;
        default: break;
        }
    }
    best1700_gpio_outputs(s, region->bank);
    best1700_gpio_update_irq(s);
}

static const MemoryRegionOps best1700_gpio_ops = {
    .read = best1700_gpio_read,
    .write = best1700_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void best1700_gpio_reset(Best1700GpioState *s)
{
    for (unsigned i = 0; i < BEST1700_GPIO_BANKS; i++) {
        Best1700GpioBank *b = &s->bank[i];
        uint32_t input = b->input, driven = b->driven;

        *b = (Best1700GpioBank) {
            .input = input, .driven = driven,
            .mask = UINT32_MAX, .fall_mask = UINT32_MAX,
        };
        best1700_gpio_outputs(s, i);
    }
    best1700_gpio_update_irq(s);
}

static void best1700_gpio_device_reset(DeviceState *dev)
{
    best1700_gpio_reset(BEST1700_GPIO(dev));
}

static void best1700_gpio_init(Object *obj)
{
    Best1700GpioState *s = BEST1700_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    for (unsigned i = 0; i < BEST1700_GPIO_MMIO_COUNT; i++) {
        unsigned kind = i / BEST1700_GPIO_BANKS;
        char *name = g_strdup_printf(TYPE_BEST1700_GPIO "-%u", i);

        s->region[i] = (Best1700GpioRegion) { s, i % BEST1700_GPIO_BANKS, kind };
        memory_region_init_io(&s->iomem[i], obj, &best1700_gpio_ops,
                              &s->region[i], name, kind ? 4 : 0x1000);
        sysbus_init_mmio(sbd, &s->iomem[i]);
        g_free(name);
    }
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(DEVICE(s), best1700_gpio_input, "input",
                           BEST1700_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(s), s->output, "output",
                            BEST1700_GPIO_PINS);
}

static void best1700_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BEST1700 GPIO banks and interrupt gates";
    dc->reset = best1700_gpio_device_reset;
}

static const TypeInfo best1700_gpio_info = {
    .name = TYPE_BEST1700_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700GpioState),
    .instance_init = best1700_gpio_init,
    .class_init = best1700_gpio_class_init,
};

static void best1700_gpio_register_types(void)
{
    type_register_static(&best1700_gpio_info);
}

type_init(best1700_gpio_register_types)
