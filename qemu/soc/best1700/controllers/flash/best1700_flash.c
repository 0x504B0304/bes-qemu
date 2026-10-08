/* BEST1700 NOR command controller. Register transactions are independent of
 * the package die's identity, contents and write-protection state.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "best1700_flash.h"

#define FLASH_CMD       0x00
#define FLASH_CTRL      0x04
#define FLASH_TX        0x08
#define FLASH_STATUS    0x0c
#define FLASH_RX        0x10
#define FLASH_CLEAR     0x18
#define FLASH_ADDR_HIGH 0x30
#define FLASH_DUAL      0x204
#define DUAL_ENABLE     (1u << 19)
#define TX_COPY         (1u << 21)
#define FIFO_DEPTH      16
#define TRANSFER_NS     1000

static void reject(Best1700FlashState *s, const char *reason)
{
    if (!s->failed) {
        error_report("best1700 NOR transaction rejected: opcode=0x%02x "
                     "address=0x%08x tx=%u rx=%u: %s "
                     "(controller remains busy until FIFO clear/reset)",
                     s->command & 255, s->address, s->tx_needed,
                     s->rx_needed, reason);
    }
    s->failed = true;
    s->pending = false;
    s->rx_count = s->rx_head = 0;
    timer_del(s->timer);
}

static uint8_t combine(uint8_t a, uint8_t b, unsigned half)
{
    return half ? (a & 15) | (b << 4) : (a >> 4) | (b & 0xf0);
}

static void complete_command(void *opaque)
{
    Best1700FlashState *s = opaque;
    uint8_t opcode = s->command;
    unsigned count = s->dual ? 2 : 1;
    bool read_memory = best1700_nor_memory_read(opcode);
    bool write_memory = best1700_nor_memory_write(opcode);
    uint32_t die_address = s->address;
    unsigned skip = 0;
    unsigned tx_len = s->tx_needed;
    unsigned rx_len = s->rx_needed;
    uint8_t die_tx[2][BEST1700_FLASH_MAX_TRANSFER];
    uint8_t die_rx[2][BEST1700_FLASH_MAX_TRANSFER];
    uint64_t changed_start[2] = { 0 }, changed_length[2] = { 0 };
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->pending || s->failed) {
        return;
    }
    /* Reject an empty aggregate payload before an odd address's padding can
     * turn it into a nonempty physical-die transaction. */
    if (write_memory && !s->tx_needed) {
        reject(s, "page program requires a nonzero TX length");
        return;
    }
    if (s->extended && (read_memory || write_memory || opcode == 0x20 ||
        opcode == 0x52 || opcode == 0xd8 || opcode == 0x48)) {
        reject(s, "extended addressed command framing is not implemented");
        return;
    }
    if (s->dual) {
        if (read_memory) {
            skip = s->address & 1;
            rx_len = (s->rx_needed + skip + 1) / 2;
        } else {
            rx_len = (s->rx_needed + 1) / 2;
        }
        if (read_memory || write_memory || opcode == 0x20 || opcode == 0x52 ||
            opcode == 0xd8 || opcode == 0x60 || opcode == 0xc7) {
            die_address /= 2;
        }
    }
    for (unsigned die = 0; die < count; die++) {
        memcpy(die_tx[die], s->tx, tx_len);
    }
    if (s->dual && write_memory) {
        uint64_t aligned = s->address & ~UINT64_C(1);
        unsigned first_half = s->address & 1;

        tx_len = (s->tx_needed + first_half + 1) / 2;
        if (tx_len > 256 || aligned >= (uint64_t)s->dies[0].capacity * 2) {
            reject(s, "dual program range is invalid");
            return;
        }
        for (unsigned i = 0; i < tx_len; i++) {
            uint8_t wire[2];
            for (unsigned half = 0; half < 2; half++) {
                unsigned source = 2 * i + half;
                /* Page program wraps within the physical die page. Preserve
                 * a half-pair from that wrapped location, never the following
                 * linear page (which can contain unrelated programmed bits). */
                uint64_t physical = (aligned & ~UINT64_C(511)) |
                                    ((aligned + source) & 511);
                wire[half] = source >= first_half &&
                    source - first_half < s->tx_needed ?
                    s->tx[source - first_half] : s->backing[physical];
            }
            die_tx[0][i] = ((wire[0] & 15) << 4) | (wire[1] & 15);
            die_tx[1][i] = (wire[0] & 0xf0) | (wire[1] >> 4);
        }
    } else if (s->dual && !s->tx_copy &&
               (opcode == 0x01 || opcode == 0x31 || opcode == 0x11)) {
        if (tx_len & 1) {
            reject(s, "dual status data requires pairs or TX_COPY");
            return;
        }
        tx_len /= 2;
        for (unsigned i = 0; i < tx_len; i++) {
            die_tx[0][i] = ((s->tx[2 * i] & 15) << 4) |
                          (s->tx[2 * i + 1] & 15);
            die_tx[1][i] = (s->tx[2 * i] & 0xf0) |
                          (s->tx[2 * i + 1] >> 4);
        }
    }
    if ((write_memory || opcode == 0x20 || opcode == 0x52 || opcode == 0xd8 ||
         opcode == 0x60 || opcode == 0xc7) && !s->changed) {
        reject(s, "array writes require an XIP invalidation callback");
        return;
    }
    /* Validate every die before mutating either die or the shared XIP bytes. */
    for (unsigned die = 0; die < count; die++) {
        Best1700NorResult result = best1700_nor_command(&s->dies[die], opcode,
            die_address, die_tx[die], tx_len, die_rx[die], rx_len, now, false,
            &changed_start[die], &changed_length[die]);
        if (result != BEST1700_NOR_OK) {
            g_autofree char *reason = g_strdup_printf(
                "%s (die=%u WEL=%u SR=%02x/%02x/%02x)",
                best1700_nor_result_name(result), die, s->dies[die].wel,
                s->dies[die].status[0], s->dies[die].status[1],
                s->dies[die].status[2]);
            reject(s, reason);
            return;
        }
    }
    for (unsigned die = 0; die < count; die++) {
        Best1700NorResult result = best1700_nor_command(&s->dies[die], opcode,
            die_address, die_tx[die], tx_len, die_rx[die], rx_len, now, true,
            &changed_start[die], &changed_length[die]);
        g_assert(result == BEST1700_NOR_OK);
    }
    for (unsigned i = 0; i < s->rx_needed; i++) {
        unsigned wire = i + skip;
        s->rx[i] = s->dual ? combine(die_rx[0][wire / 2],
                                    die_rx[1][wire / 2], wire & 1) : die_rx[0][i];
    }
    s->rx_count = s->rx_needed;
    s->rx_head = 0;
    s->pending = false;
    s->tx_count = 0;
    for (unsigned die = 0; die < count; die++) {
        if (changed_length[die]) {
            s->changed(s->changed_opaque, changed_start[die], changed_length[die]);
        }
    }
}

static void schedule(Best1700FlashState *s)
{
    if (s->pending && !s->failed && s->tx_count == s->tx_needed) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TRANSFER_NS);
    }
}

static void start_command(Best1700FlashState *s, uint32_t command)
{
    uint8_t opcode = command;
    unsigned block = (s->regs[FLASH_CTRL / 4] >> 12) & 0x1fff;

    if (s->pending || s->failed) {
        reject(s, "command submitted while controller busy");
        return;
    }
    s->command = command;
    s->address = (command >> 8) | (s->regs[FLASH_ADDR_HIGH / 4] << 24);
    s->extended = s->regs[FLASH_CTRL / 4] & 1;
    s->dual = s->regs[FLASH_DUAL / 4] & DUAL_ENABLE;
    s->tx_copy = s->regs[FLASH_DUAL / 4] & TX_COPY;
    s->tx_needed = s->rx_needed = 0;
    s->tx_count = 0;
    if (s->dual && s->die_count != 2) {
        /* Command-only power transitions broadcast the same opcode on
         * both chip selects. An unpopulated second select does not add a
         * data lane. Keep the programmed DUAL register, but execute the
         * no-payload command against the single attached die. Data-bearing
         * dual transfers still require two physical dies. */
        if (s->die_count == 1 && !s->extended &&
            (opcode == 0xb9 || opcode == 0xab)) {
            s->dual = false;
        } else {
            reject(s, "dual mode requires two configured dies");
            return;
        }
    }
    if (s->extended) {
        s->address = 0;
        s->tx_needed = block;
        if (command & 0x100) {
            s->rx_needed = (command >> 13) & 0x1fff;
        }
    } else if (best1700_nor_memory_read(opcode) || opcode == 0x9f ||
               opcode == 0x48) {
        s->rx_needed = block;
    } else if (opcode == 0x05 || opcode == 0x35 || opcode == 0x15) {
        s->rx_needed = s->dual ? 2 : 1;
    } else if (best1700_nor_memory_write(opcode) || opcode == 0x77 ||
               opcode == 0x01 || opcode == 0x31 || opcode == 0x11) {
        s->tx_needed = block;
    }
    s->rx_count = s->rx_head = 0;
    s->pending = true;
    if (s->tx_needed) {
        unsigned take = MIN(s->tx_bytes, s->tx_needed);
        memcpy(s->tx, s->tx_fifo, take);
        s->tx_count = take;
        if (take != s->tx_bytes) {
            reject(s, "TX FIFO contains more bytes than declared");
            return;
        }
        s->tx_bytes = s->tx_slots = 0;
    }
    schedule(s);
}

static uint64_t flash_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700FlashState *s = opaque;

    if (offset >= sizeof(s->regs) || (offset & 3)) {
        return 0;
    }
    switch (offset) {
    case FLASH_STATUS:
        return ((FIFO_DEPTH - s->tx_slots) << 9) |
               (MIN(s->rx_count, FIFO_DEPTH) << 4) |
               (s->rx_count ? 0 : 8) | (s->pending || s->failed ? 1 : 0);
    case FLASH_RX:
        /* In the default byte FIFO mode, a word load still pops one byte. */
        if (s->rx_count) {
            s->rx_count--;
            return s->rx[s->rx_head++];
        }
        return 0xff;
    case FLASH_CLEAR:
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static void flash_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Best1700FlashState *s = opaque;

    if (offset >= sizeof(s->regs) || (offset & 3)) {
        return;
    }
    if (offset == FLASH_TX) {
        if (s->failed) {
            return;
        }
        if (s->pending && s->tx_count < s->tx_needed) {
            if (size > s->tx_needed - s->tx_count) {
                reject(s, "TX write exceeds declared length");
                return;
            }
            for (unsigned i = 0; i < size; i++) {
                s->tx[s->tx_count++] = value >> (8 * i);
            }
            schedule(s);
        } else {
            if (s->tx_slots == FIFO_DEPTH || size > sizeof(s->tx_fifo) - s->tx_bytes) {
                reject(s, "TX FIFO overflow");
                return;
            }
            for (unsigned i = 0; i < size; i++) {
                s->tx_fifo[s->tx_bytes++] = value >> (8 * i);
            }
            s->tx_slots++;
        }
        return;
    }
    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "best1700 flash: register write requires word access\n");
        return;
    }
    switch (offset) {
    case FLASH_STATUS: case FLASH_RX:
        break;
    case FLASH_CLEAR:
        if (value & 1) {
            s->rx_count = s->rx_head = 0;
        }
        if (value & 2) {
            s->tx_bytes = s->tx_slots = 0;
        }
        if ((value & 3) == 3) {
            timer_del(s->timer);
            s->pending = s->failed = false;
            s->tx_count = 0;
        }
        break;
    case FLASH_CMD:
        s->regs[offset / 4] = value;
        start_command(s, value);
        break;
    default:
        s->regs[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps flash_ops = {
    .read = flash_read,
    .write = flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void best1700_flash_set_backing(Best1700FlashState *s, uint8_t *data, uint64_t size)
{
    s->backing = data;
    s->backing_size = size;
}

void best1700_flash_set_changed(Best1700FlashState *s,
    void (*changed)(void *, uint64_t, uint64_t), void *opaque)
{
    s->changed = changed;
    s->changed_opaque = opaque;
}

void best1700_flash_reset(Best1700FlashState *s)
{
    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->pending = s->failed = false;
    s->tx_bytes = s->tx_slots = s->tx_count = s->tx_needed = 0;
    s->rx_count = s->rx_head = s->rx_needed = 0;
    for (unsigned i = 0; i < s->die_count && i < 2; i++) {
        best1700_nor_reset(&s->dies[i]);
    }
}

static void device_reset(DeviceState *dev)
{
    best1700_flash_reset(BEST1700_FLASH(dev));
}

static void flash_realize(DeviceState *dev, Error **errp)
{
    Best1700FlashState *s = BEST1700_FLASH(dev);

    if (s->die_count != 1 && s->die_count != 2) {
        error_setg(errp, "best1700 flash die-count must be one or two");
        return;
    }
    for (unsigned i = 0; i < s->die_count; i++) {
        if (!best1700_nor_init(&s->dies[i], s->backing, s->backing_size,
                              i, s->die_count, s->jedec_id, i ? s->uid1 : s->uid0)) {
            error_setg(errp, "best1700 flash needs borrowed XIP storage covering all configured dies");
            return;
        }
    }
    best1700_flash_reset(s);
}

static void flash_init(Object *obj)
{
    Best1700FlashState *s = BEST1700_FLASH(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, complete_command, s);
    memory_region_init_io(&s->iomem, obj, &flash_ops, s,
                          TYPE_BEST1700_FLASH, BEST1700_FLASH_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void flash_finalize(Object *obj)
{
    timer_free(BEST1700_FLASH(obj)->timer);
}

static Property flash_properties[] = {
    DEFINE_PROP_UINT32("die-count", Best1700FlashState, die_count, 2),
    /* C8-60-17, an 8 MiB compatible simulated die, not hardware identification. */
    DEFINE_PROP_UINT32("jedec-id", Best1700FlashState, jedec_id, 0xc86017),
    DEFINE_PROP_UINT64("uid0", Best1700FlashState, uid0, UINT64_C(0x1700000000000001)),
    DEFINE_PROP_UINT64("uid1", Best1700FlashState, uid1, UINT64_C(0x1700000000000002)),
    DEFINE_PROP_END_OF_LIST(),
};

static void flash_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BEST1700 NOR command controller";
    dc->realize = flash_realize;
    dc->reset = device_reset;
    device_class_set_props(dc, flash_properties);
}

static const TypeInfo flash_info = {
    .name = TYPE_BEST1700_FLASH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700FlashState),
    .instance_init = flash_init,
    .instance_finalize = flash_finalize,
    .class_init = flash_class_init,
};

static void flash_register_types(void)
{
    type_register_static(&flash_info);
}

type_init(flash_register_types)
