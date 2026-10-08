/* AW86225 candidate haptic controller: register bus, wave RAM and LRA timing.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_AW86225 "aw86225"
OBJECT_DECLARE_SIMPLE_TYPE(Aw86225State, AW86225)

#define RAM_SIZE 4096
#define REG_ID 0x00
#define REG_SYSST 0x01
#define REG_SYSINT 0x02
#define REG_SYSINTM 0x03
#define REG_PLAYCFG3 0x08
#define REG_PLAYCFG4 0x09
#define REG_WAVCFG1 0x0a
#define REG_WAVLOOP1 0x12
#define REG_CONTCFG1 0x18
#define REG_F0 0x25
#define REG_CONT_F0 0x27
#define REG_STATE 0x3f
#define REG_RAMADDRH 0x40
#define REG_RAMADDRL 0x41
#define REG_RAMDATA 0x42
#define REG_SYSCTRL1 0x43
#define REG_SYSCTRL2 0x44
#define REG_DETCFG2 0x52
#define REG_DET_VBAT 0x55
#define REG_DET_LO 0x57
#define INT_DONE (1 << 5)
#define RAMINIT (1 << 3)

struct Aw86225State {
    I2CSlave parent_obj;
    uint8_t regs[0x78], ram[RAM_SIZE];
    uint16_t ram_address;
    uint8_t pointer;
    bool selected, held_reset;
    uint32_t f0_hz, voltage_mv;
    uint8_t factory_osc_trim;
    DeviceState *reset_target, *irq_target;
    uint32_t reset_pin, irq_pin;
    qemu_irq irq;
    QEMUTimer *play_timer, *detect_timer;
    bool calibrating;
    bool trace_bus;
    uint64_t ram_writes, ram_reads, plays, completions, calibrations;
};

static void aw86225_irq(Aw86225State *s)
{
    qemu_set_irq(s->irq, !(s->regs[REG_SYSINT] & ~s->regs[REG_SYSINTM]));
}

static void aw86225_stop(Aw86225State *s)
{
    timer_del(s->play_timer);
    s->regs[REG_STATE] = 0;
    s->regs[REG_PLAYCFG4] = 0;
    s->calibrating = false;
}

static void aw86225_reset(DeviceState *dev)
{
    Aw86225State *s = AW86225(dev);
    aw86225_stop(s);
    timer_del(s->detect_timer);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ram, 0, sizeof(s->ram));
    s->regs[REG_SYSINTM] = 0xff;
    s->regs[0x07] = 0x80; /* Gain at power-on. */
    s->ram_address = s->pointer = 0;
    s->selected = false;
    s->ram_writes = s->ram_reads = s->plays = s->completions = s->calibrations = 0;
    aw86225_irq(s);
}

static void aw86225_reset_input(void *opaque, int line, int level)
{
    Aw86225State *s = opaque;
    if (!level && !s->held_reset) {
        aw86225_reset(DEVICE(s));
    }
    s->held_reset = !level;
}

static void aw86225_complete(void *opaque)
{
    Aw86225State *s = opaque;
    if (s->calibrating) {
        unsigned raw = 3840000 / (s->f0_hz * 10);
        s->regs[REG_F0] = s->regs[REG_CONT_F0] = raw >> 8;
        s->regs[REG_F0 + 1] = s->regs[REG_CONT_F0 + 1] = raw;
        s->calibrations++;
    }
    aw86225_stop(s);
    s->regs[REG_SYSINT] |= INT_DONE;
    s->completions++;
    aw86225_irq(s);
}

static void aw86225_detect(void *opaque)
{
    Aw86225State *s = opaque;
    if (s->regs[REG_DETCFG2] & 2) {
        unsigned adc = MIN(1023, s->voltage_mv * 1024 / 6100);
        s->regs[REG_DET_VBAT] = adc >> 2;
        s->regs[REG_DET_LO] = (s->regs[REG_DET_LO] & ~0x30) | ((adc & 3) << 4);
    }
    s->regs[REG_DETCFG2] &= ~3;
}

/* The uploaded RAM begins with a version byte, followed by big-endian
 * inclusive start/end pairs. Duration follows the guest's waveform and loop
 * selection. WAVCFG bit 7 encodes a wait in 128-sample units, not a RAM
 * waveform ID; an infinite loop remains active until STOP or reset. */
static int64_t aw86225_wave_ns(Aw86225State *s)
{
    uint64_t samples = 0;
    unsigned hz;
    switch (s->regs[REG_SYSCTRL2] & 3) {
    case 2: hz = 24000; break;
    case 3: hz = 12000; break;
    case 0: hz = 48000; break;
    default: return 0;
    }
    for (unsigned slot = 0; slot < 8; slot++) {
        unsigned sequence = s->regs[REG_WAVCFG1 + slot];
        unsigned wave = sequence & 0x7f;
        unsigned loop = (s->regs[REG_WAVLOOP1 + slot / 2] >>
                         (slot % 2 ? 0 : 4)) & 15;
        unsigned table, start, end;
        if (!sequence) {
            break;
        }
        if (sequence & 0x80) {
            if (loop == 15) {
                return -1;
            }
            samples += wave * 128 * (loop + 1);
            continue;
        }
        table = 1 + (wave - 1) * 4;
        start = (s->ram[table] << 8) | s->ram[table + 1];
        end = (s->ram[table + 2] << 8) | s->ram[table + 3];
        if (end < start || end >= RAM_SIZE || start < table + 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "aw86225: invalid wave slot=%u wave=%u "
                          "start=%u end=%u table=%u\n", slot, wave, start, end, table);
            return 0;
        }
        if (loop == 15) {
            return -1;
        }
        samples += (end - start + 1) * (loop + 1);
    }
    return samples * 1000000000ULL / hz;
}

static int aw86225_play(Aw86225State *s)
{
    int64_t duration;
    unsigned mode = s->regs[REG_PLAYCFG3] & 3;
    if (s->regs[REG_SYSCTRL1] & RAMINIT) {
        return -1;
    }
    aw86225_stop(s);
    if (mode == 2) {
        s->calibrating = s->regs[REG_CONTCFG1] & 8;
        duration = s->calibrating ? 100000000 : -1;
        s->regs[REG_STATE] = 6;
    } else if (mode == 0) {
        duration = aw86225_wave_ns(s);
        if (!duration) {
            return -1; /* Invalid/unloaded wave must not report a completed play. */
        }
        s->regs[REG_STATE] = 7;
    } else {
        return -1; /* Streaming RTP and trigger mode are not yet implemented. */
    }
    s->regs[REG_PLAYCFG4] = 1;
    s->plays++;
    if (duration > 0) {
        timer_mod(s->play_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration);
    }
    return 0;
}

static bool aw86225_config_register(unsigned reg)
{
    /* Configuration windows used by the original register driver. Holes and
     * read-only result/status registers are deliberately not scratch RAM. */
    return (reg >= 0x03 && reg <= 0x24 && reg != REG_PLAYCFG4) ||
           (reg >= 0x2d && reg <= 0x31) || reg == 0x3a ||
           reg == REG_SYSCTRL1 || reg == REG_SYSCTRL2 || reg == 0x49 ||
           reg == 0x58 || reg == 0x5a || reg == 0x5b || reg == 0x77;
}

static int aw86225_event(I2CSlave *dev, enum i2c_event event)
{
    Aw86225State *s = AW86225(dev);
    if (s->held_reset && (event == I2C_START_SEND || event == I2C_START_RECV)) {
        return -1;
    }
    if (event == I2C_START_SEND) {
        s->selected = false;
    }
    return 0;
}

static int aw86225_send(I2CSlave *dev, uint8_t data)
{
    Aw86225State *s = AW86225(dev);
    unsigned reg = s->pointer;
    int result = 0;
    if (s->held_reset) {
        return -1;
    }
    if (!s->selected) {
        s->pointer = data;
        s->selected = true;
        return 0;
    }
    if (s->trace_bus && reg != REG_RAMDATA) {
        qemu_log_mask(LOG_GUEST_ERROR, "aw86225: write %02x=%02x\n", reg, data);
    }
    switch (reg) {
    case REG_ID:
        if (data == 0xaa) {
            aw86225_reset(DEVICE(s));
        } else {
            result = -1;
        }
        break;
    case REG_RAMADDRH:
        s->ram_address = (s->ram_address & 0xff) | ((data & 0x0f) << 8);
        break;
    case REG_RAMADDRL:
        s->ram_address = (s->ram_address & 0xf00) | data;
        break;
    case REG_RAMDATA:
        if (!(s->regs[REG_SYSCTRL1] & RAMINIT)) {
            return -1;
        }
        s->ram[s->ram_address] = data;
        s->ram_writes++;
        s->ram_address = (s->ram_address + 1) % RAM_SIZE;
        return 0; /* FIFO data port retains the register selector. */
    case REG_PLAYCFG4:
        if (data & 2) {
            aw86225_stop(s);
        } else if (data & 1) {
            result = aw86225_play(s);
        }
        break;
    case REG_DETCFG2:
        s->regs[reg] = data;
        timer_del(s->detect_timer);
        if (data & 3) {
            timer_mod(s->detect_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      ((data & 2) ? 20000000 : 10000));
        }
        break;
    default:
        if (aw86225_config_register(reg)) {
            s->regs[reg] = data;
            if (reg == REG_SYSINTM) {
                aw86225_irq(s);
            }
        } else {
            result = -1;
        }
        break;
    }
    if (result) {
        qemu_log_mask(LOG_UNIMP, "aw86225: rejected register %02x value %02x "
                      "mode=%u raminit=%u wave=%u loop=%02x\n", reg, data,
                      s->regs[REG_PLAYCFG3] & 3,
                      !!(s->regs[REG_SYSCTRL1] & RAMINIT),
                      s->regs[REG_WAVCFG1], s->regs[REG_WAVLOOP1]);
    }
    s->pointer++;
    return result;
}

static uint8_t aw86225_read(I2CSlave *dev)
{
    Aw86225State *s = AW86225(dev);
    unsigned reg = s->pointer;
    uint8_t value;
    if (reg == REG_RAMDATA) {
        if (!(s->regs[REG_SYSCTRL1] & RAMINIT)) {
            return 0xff;
        }
        value = s->ram[s->ram_address];
        s->ram_reads++;
        s->ram_address = (s->ram_address + 1) % RAM_SIZE;
        return value;
    }
    s->pointer++;
    switch (reg) {
    case REG_RAMADDRH: return s->ram_address >> 8;
    case REG_RAMADDRL: return s->ram_address;
    case 0x65: return s->factory_osc_trim;
    case REG_SYSINT:
        value = s->regs[reg];
        s->regs[reg] = 0; /* Read-to-clear interrupt latch. */
        aw86225_irq(s);
        return value;
    case REG_ID: case REG_SYSST: case REG_PLAYCFG4: case REG_STATE:
    case REG_F0: case REG_F0 + 1: case REG_CONT_F0: case REG_CONT_F0 + 1:
    case REG_DETCFG2: case REG_DET_VBAT: case REG_DET_LO:
        return s->regs[reg];
    default:
        if (aw86225_config_register(reg)) {
            return s->regs[reg];
        }
        qemu_log_mask(LOG_UNIMP, "aw86225: unmodeled read %02x\n", reg);
        return 0xff;
    }
}

static uint8_t aw86225_recv(I2CSlave *dev)
{
    Aw86225State *s = AW86225(dev);
    unsigned reg = s->pointer;
    uint8_t value = aw86225_read(dev);
    if (s->trace_bus && reg != REG_RAMDATA) {
        qemu_log_mask(LOG_GUEST_ERROR, "aw86225: read %02x=%02x\n", reg, value);
    }
    return value;
}

static void aw86225_realize(DeviceState *dev, Error **errp)
{
    Aw86225State *s = AW86225(dev);
    if (s->f0_hz < 100 || s->f0_hz > 500 ||
        s->voltage_mv < 3000 || s->voltage_mv > 5500 ||
        (s->reset_target && s->reset_pin >= 96) ||
        (s->irq_target && s->irq_pin >= 96)) {
        error_setg(errp, "invalid AW86225 physical scenario or GPIO pin");
        return;
    }
    if (s->reset_target) {
        qdev_connect_gpio_out_named(s->reset_target, "output", s->reset_pin,
                                   qdev_get_gpio_in_named(dev, "reset", 0));
    }
    if (s->irq_target) {
        qdev_connect_gpio_out(dev, 0,
            qdev_get_gpio_in_named(s->irq_target, "input", s->irq_pin));
    }
    aw86225_reset(dev);
}

static void aw86225_unrealize(DeviceState *dev)
{
    Aw86225State *s = AW86225(dev);
    timer_del(s->play_timer);
    timer_del(s->detect_timer);
}

static void aw86225_init(Object *obj)
{
    Aw86225State *s = AW86225(obj);
    qdev_init_gpio_out(DEVICE(s), &s->irq, 1);
    qdev_init_gpio_in_named(DEVICE(s), aw86225_reset_input, "reset", 1);
    s->play_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, aw86225_complete, s);
    s->detect_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, aw86225_detect, s);
    object_property_add_uint64_ptr(obj, "ram-writes", &s->ram_writes, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "ram-reads", &s->ram_reads, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "plays", &s->plays, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "completions", &s->completions, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "calibrations", &s->calibrations, OBJ_PROP_FLAG_READ);
}

static void aw86225_finalize(Object *obj)
{
    Aw86225State *s = AW86225(obj);
    timer_free(s->play_timer);
    timer_free(s->detect_timer);
}

static Property aw86225_properties[] = {
    DEFINE_PROP_BOOL("trace-bus", Aw86225State, trace_bus, false),
    DEFINE_PROP_UINT32("f0-hz", Aw86225State, f0_hz, 240),
    DEFINE_PROP_UINT8("factory-osc-trim", Aw86225State, factory_osc_trim, 0),
    DEFINE_PROP_UINT32("voltage-mv", Aw86225State, voltage_mv, 4000),
    DEFINE_PROP_LINK("reset-target", Aw86225State, reset_target, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT32("reset-pin", Aw86225State, reset_pin, 0),
    DEFINE_PROP_LINK("irq-target", Aw86225State, irq_target, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT32("irq-pin", Aw86225State, irq_pin, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_aw86225 = {
    .name = TYPE_AW86225,
    .unmigratable = true,
};

static void aw86225_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    dc->desc = "AW86225 candidate RAM haptic controller";
    dc->realize = aw86225_realize;
    dc->unrealize = aw86225_unrealize;
    dc->reset = aw86225_reset;
    dc->vmsd = &vmstate_aw86225;
    device_class_set_props(dc, aw86225_properties);
    ic->event = aw86225_event;
    ic->send = aw86225_send;
    ic->recv = aw86225_recv;
}

static const TypeInfo aw86225_info = {
    .name = TYPE_AW86225,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Aw86225State),
    .instance_init = aw86225_init,
    .instance_finalize = aw86225_finalize,
    .class_init = aw86225_class_init,
};

static void aw86225_register_types(void)
{
    type_register_static(&aw86225_info);
}
type_init(aw86225_register_types)
