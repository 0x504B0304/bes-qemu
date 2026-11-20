/* SIH6889 haptic register and waveform model.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_SIH6889 "sih6889"
OBJECT_DECLARE_SIMPLE_TYPE(Sih6889State, SIH6889)

#define RAM_SIZE 8192
#define REG_ID 0x00
#define REG_IRQ 0x02
#define REG_CONTROL 0x04
#define REG_STATUS 0x0e
#define REG_GO 0x0f
#define REG_RTP_DATA 0x2b
#define REG_RAM_ADDR_H 0x28
#define REG_RAM_ADDR_L 0x29
#define REG_RAM_DATA 0x2a
#define REG_RATE 0x34
#define REG_SEQUENCE 0x10
#define REG_MAIN_LOOP 0x1c
#define REG_LOOPS 0x5c
#define RAM_INIT 0x80
#define PLAY_RAM 1
#define PLAY_RTP 2
#define PLAY_TRACKING 4
#define PLAY_STOP 16
#define STATUS_STANDBY (1 << 0)
#define STATUS_RAM (1 << 2)
#define STATUS_RTP (1 << 3)
#define STATUS_TRACKING (1 << 5)

struct Sih6889State {
    I2CSlave parent_obj;
    uint8_t regs[256], ram[RAM_SIZE];
    uint16_t ram_address;
    uint8_t pointer;
    bool selected;
    bool active_rtp;
    bool trace_bus;
    uint64_t rtp_deadline_ns;
    uint64_t plays, last_duration_ns, ram_writes;
    QEMUTimer *timer;
};

static unsigned sih6889_sample_rate(const Sih6889State *s)
{
    static const unsigned rates[] = { 48000, 24000, 12000, 24000 };
    return rates[(s->regs[REG_RATE] >> 1) & 3];
}

static void sih6889_stop(Sih6889State *s)
{
    timer_del(s->timer);
    s->regs[REG_STATUS] = STATUS_STANDBY;
    s->regs[REG_GO] = 0;
    s->active_rtp = false;
    s->rtp_deadline_ns = 0;
}

static void sih6889_complete(void *opaque)
{
    sih6889_stop(opaque);
}

static void sih6889_reset(DeviceState *dev)
{
    Sih6889State *s = SIH6889(dev);
    /* 250 Hz nominal LRA. Firmware divides 0x75300000 by this result
     * to obtain its tracking value in units of 0.1 Hz. */
    uint32_t tracking = 0x75300000 / 2500;

    sih6889_stop(s);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ram, 0, sizeof(s->ram));
    s->regs[REG_ID] = 0x89;
    s->regs[REG_STATUS] = STATUS_STANDBY;
    s->regs[0xda] = tracking;
    s->regs[0xdb] = tracking >> 8;
    s->regs[0xdc] = tracking >> 16;
    s->regs[REG_RATE] = 2;
    s->ram_address = 0;
    s->pointer = 0;
    s->selected = false;
    s->plays = s->last_duration_ns = s->ram_writes = 0;
}

static int64_t sih6889_ram_duration(const Sih6889State *s)
{
    unsigned base = (s->regs[0x20] << 8) | s->regs[0x21];
    unsigned main_loop = s->regs[REG_MAIN_LOOP] & 15;
    uint64_t samples = 0, playable_samples = 0;

    /* Each waveform table entry has a big-endian inclusive start/end pair.
     * Bit 7 selects a delay slot in units of 128 samples. */
    for (unsigned slot = 0; slot < 8; slot++) {
        unsigned sequence = s->regs[REG_SEQUENCE + slot];
        unsigned wave = sequence & 0x7f;
        unsigned loop = (s->regs[REG_LOOPS + slot / 2] >> (4 * (slot % 2))) & 15;
        unsigned entry, start, end;

        if (!sequence) {
            break;
        }
        if (sequence & 0x80) {
            samples += wave * 128 * (loop + 1);
            continue;
        }
        entry = base + 1 + (wave - 1) * 4;
        if (entry + 3 >= RAM_SIZE) {
            return 0;
        }
        start = (s->ram[entry] << 8) | s->ram[entry + 1];
        end = (s->ram[entry + 2] << 8) | s->ram[entry + 3];
        if (start < entry + 4 || end < start || end >= RAM_SIZE) {
            if (s->trace_bus) {
                qemu_log_mask(LOG_GUEST_ERROR, "sih6889: invalid wave %u: "
                              "entry=%04x start=%04x end=%04x\n",
                              wave, entry, start, end);
            }
            return 0;
        }
        if (loop == 15) {
            return -1;
        }
        playable_samples = (end - start + 1) * (loop + 1);
        samples += playable_samples;
    }
    if (!playable_samples) {
        return 0;
    }
    if (main_loop == 15) {
        return -1;
    }
    return samples * (main_loop + 1) * 1000000000ULL / sih6889_sample_rate(s);
}

static void sih6889_go(Sih6889State *s, uint8_t value)
{
    int64_t duration;
    sih6889_stop(s);
    if (value & PLAY_STOP) {
        return;
    }
    switch (value & (PLAY_RAM | PLAY_RTP | PLAY_TRACKING)) {
    case PLAY_RAM:
        if (s->regs[REG_CONTROL] & RAM_INIT) {
            return;
        }
        duration = sih6889_ram_duration(s);
        if (duration == 0) {
            return;
        }
        s->regs[REG_STATUS] = STATUS_RAM;
        s->last_duration_ns = duration > 0 ? duration : 100000000;
        s->plays++;
        break;
    case PLAY_RTP:
        s->regs[REG_STATUS] = STATUS_RTP;
        s->active_rtp = true;
        duration = 100000000;
        break;
    case PLAY_TRACKING:
        s->regs[REG_STATUS] = STATUS_TRACKING;
        duration = 20000000;
        break;
    default:
        return;
    }
    s->regs[REG_GO] = value;
    if (duration > 0) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration);
    }
}

static int sih6889_event(I2CSlave *dev, enum i2c_event event)
{
    Sih6889State *s = SIH6889(dev);
    if (event == I2C_START_SEND || event == I2C_FINISH) {
        s->selected = false;
    }
    return 0;
}

static int sih6889_send(I2CSlave *dev, uint8_t value)
{
    Sih6889State *s = SIH6889(dev);
    uint8_t reg = s->pointer;

    if (!s->selected) {
        s->pointer = value;
        s->selected = true;
        return 0;
    }
    if (s->trace_bus && reg != REG_RAM_DATA && reg != REG_RTP_DATA) {
        qemu_log_mask(LOG_GUEST_ERROR, "sih6889: write %02x=%02x\n", reg, value);
    }
    switch (reg) {
    case REG_ID:
        if (value == 0xaa || value == 0x55) {
            uint64_t plays = s->plays;
            uint64_t last_duration_ns = s->last_duration_ns;
            sih6889_reset(DEVICE(s));
            s->plays = plays;
            s->last_duration_ns = last_duration_ns;
        }
        break;
    case REG_GO:
        sih6889_go(s, value);
        break;
    case REG_RAM_ADDR_H:
        s->ram_address = ((value << 8) | (s->ram_address & 0xff)) % RAM_SIZE;
        break;
    case REG_RAM_ADDR_L:
        s->ram_address = ((s->ram_address & 0xff00) | value) % RAM_SIZE;
        break;
    case REG_RAM_DATA:
        if (s->regs[REG_CONTROL] & RAM_INIT) {
            s->ram[s->ram_address] = value;
            s->ram_writes++;
            s->ram_address = (s->ram_address + 1) % RAM_SIZE;
        }
        return 0;
    case REG_RTP_DATA:
        if (s->active_rtp) {
            uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if (!s->rtp_deadline_ns) {
                s->plays++;
                s->last_duration_ns = 30000000;
            }
            s->rtp_deadline_ns = MAX(now, s->rtp_deadline_ns) +
                                 1000000000ULL / sih6889_sample_rate(s);
            timer_mod(s->timer, s->rtp_deadline_ns + 10000000);
        }
        return 0;
    case REG_IRQ:
    case REG_STATUS:
        break;
    default:
        s->regs[reg] = value;
        break;
    }
    s->pointer++;
    return 0;
}

static uint8_t sih6889_recv(I2CSlave *dev)
{
    Sih6889State *s = SIH6889(dev);
    unsigned reg = s->pointer;
    uint8_t value = s->regs[reg];

    if (reg == REG_RAM_DATA) {
        if (!(s->regs[REG_CONTROL] & RAM_INIT)) {
            return 0xff;
        }
        value = s->ram[s->ram_address];
        s->ram_address = (s->ram_address + 1) % RAM_SIZE;
        return value;
    }
    if (reg == REG_IRQ) {
        s->regs[reg] = 0;
    }
    if (reg == REG_RAM_ADDR_H) {
        value = s->ram_address >> 8;
    } else if (reg == REG_RAM_ADDR_L) {
        value = s->ram_address;
    }
    s->pointer++;
    return value;
}

static void sih6889_init(Object *obj)
{
    Sih6889State *s = SIH6889(obj);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sih6889_complete, s);
    object_property_add_uint64_ptr(obj, "plays", &s->plays, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "ram-writes", &s->ram_writes, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "last-duration-ns", &s->last_duration_ns,
                                   OBJ_PROP_FLAG_READ);
}

static void sih6889_finalize(Object *obj)
{
    Sih6889State *s = SIH6889(obj);
    timer_del(s->timer);
    timer_free(s->timer);
}

static const VMStateDescription vmstate_sih6889 = {
    .name = TYPE_SIH6889,
    .unmigratable = true,
};

static Property sih6889_props[] = {
    DEFINE_PROP_BOOL("trace-bus", Sih6889State, trace_bus, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void sih6889_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    dc->desc = "SIH6889 haptic controller";
    dc->reset = sih6889_reset;
    dc->vmsd = &vmstate_sih6889;
    device_class_set_props(dc, sih6889_props);
    ic->event = sih6889_event;
    ic->send = sih6889_send;
    ic->recv = sih6889_recv;
}

static const TypeInfo sih6889_info = {
    .name = TYPE_SIH6889,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Sih6889State),
    .instance_init = sih6889_init,
    .instance_finalize = sih6889_finalize,
    .class_init = sih6889_class_init,
};

static void sih6889_register_types(void)
{
    type_register_static(&sih6889_info);
}

type_init(sih6889_register_types)
