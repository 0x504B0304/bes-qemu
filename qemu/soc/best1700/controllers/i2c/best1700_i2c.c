/*
 * BEST1700 I2C master with a DesignWare-compatible register interface.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "best1700_i2c.h"

#define IC_CON               0x00
#define IC_TAR               0x04
#define IC_DATA_CMD          0x10
#define IC_INTR_STAT         0x2c
#define IC_INTR_MASK         0x30
#define IC_RAW_INTR_STAT     0x34
#define IC_RX_TL             0x38
#define IC_TX_TL             0x3c
#define IC_CLR_INTR          0x40
#define IC_CLR_RX_UNDER      0x44
#define IC_CLR_RX_OVER       0x48
#define IC_CLR_TX_OVER       0x4c
#define IC_CLR_RD_REQ        0x50
#define IC_CLR_TX_ABRT       0x54
#define IC_CLR_RX_DONE       0x58
#define IC_CLR_ACTIVITY      0x5c
#define IC_CLR_STOP_DET      0x60
#define IC_CLR_START_DET     0x64
#define IC_CLR_GEN_CALL      0x68
#define IC_ENABLE            0x6c
#define IC_STATUS            0x70
#define IC_TXFLR             0x74
#define IC_RXFLR             0x78
#define IC_TX_ABRT_SOURCE    0x80
#define IC_ENABLE_STATUS     0x9c
#define IC_COMP_PARAM_1      0xf4
#define IC_COMP_VERSION      0xf8
#define IC_COMP_TYPE         0xfc

#define CON_MASTER          (1u << 0)
#define CON_RESTART_EN      (1u << 5)
#define TAR_10BIT           (1u << 12)
#define CMD_READ            (1u << 8)
#define CMD_STOP            (1u << 9)
#define CMD_RESTART         (1u << 10)

#define INTR_RX_UNDER       (1u << 0)
#define INTR_RX_OVER        (1u << 1)
#define INTR_RX_FULL        (1u << 2)
#define INTR_TX_EMPTY       (1u << 4)
#define INTR_TX_ABRT        (1u << 6)
#define INTR_ACTIVITY       (1u << 8)
#define INTR_STOP_DET       (1u << 9)
#define INTR_START_DET      (1u << 10)
#define INTR_MASK_BITS      0x0fffu
#define ABRT_7B_ADDR_NOACK  (1u << 0)
#define ABRT_10ADDR_NOACK   (1u << 1)
#define ABRT_TXDATA_NOACK   (1u << 3)
#define ABRT_MASTER_DIS     (1u << 11)
#define ABRT_USER           (1u << 16)

static bool best1700_i2c_enabled(Best1700I2CState *s)
{
    return s->regs[IC_ENABLE / 4] & 1;
}

static uint32_t best1700_i2c_raw_status(Best1700I2CState *s)
{
    uint32_t status = s->raw_intr;

    if (best1700_i2c_enabled(s)) {
        /* QEMU's I2C bus completes commands synchronously, so the transmit
         * FIFO has drained before the next guest access. */
        status |= INTR_TX_EMPTY;
        if (s->rx_count > s->regs[IC_RX_TL / 4]) {
            status |= INTR_RX_FULL;
        }
        if (s->active) {
            status |= INTR_ACTIVITY;
        }
    }
    return status;
}

static void best1700_i2c_update_irq(Best1700I2CState *s)
{
    qemu_set_irq(s->irq, best1700_i2c_enabled(s) &&
                 (best1700_i2c_raw_status(s) & s->regs[IC_INTR_MASK / 4]));
}

static void best1700_i2c_stop(Best1700I2CState *s)
{
    if (s->active) {
        if (s->receiving) {
            i2c_nack(s->bus);
        }
        i2c_end_transfer(s->bus);
    }
    s->active = false;
    s->raw_intr |= INTR_STOP_DET;
}

static void best1700_i2c_abort(Best1700I2CState *s, uint32_t source)
{
    s->abort_source |= source;
    s->raw_intr |= INTR_TX_ABRT;
    s->rx_head = 0;
    s->rx_count = 0;
    best1700_i2c_stop(s);
}

static void best1700_i2c_command(Best1700I2CState *s, uint32_t command)
{
    bool receiving = command & CMD_READ;
    bool restart = command & CMD_RESTART;
    uint32_t target = s->regs[IC_TAR / 4];

    if (s->commands++ < 64) {
        qemu_log_mask(LOG_UNIMP,
                      "best1700 %s DATA_CMD tar=0x%04" PRIx32
                      " command=0x%04" PRIx32 " enabled=%u\n",
                      object_get_canonical_path_component(OBJECT(s)),
                      target, command, best1700_i2c_enabled(s));
    }
    if (!best1700_i2c_enabled(s) || (s->raw_intr & INTR_TX_ABRT)) {
        return;
    }
    if (!(s->regs[IC_CON / 4] & CON_MASTER)) {
        best1700_i2c_abort(s, ABRT_MASTER_DIS);
        return;
    }
    /* The QEMU bus uses seven-bit addresses. Do not truncate an unsupported
     * ten-bit address and accidentally acknowledge another slave. */
    if (target & TAR_10BIT) {
        qemu_log_mask(LOG_UNIMP, "best1700 i2c: ten-bit address unsupported\n");
        best1700_i2c_abort(s, ABRT_10ADDR_NOACK);
        return;
    }
    if (!s->active || receiving != s->receiving || restart) {
        if (s->active && !(s->regs[IC_CON / 4] & CON_RESTART_EN)) {
            best1700_i2c_stop(s);
        } else if (s->active && s->receiving) {
            i2c_nack(s->bus);
        }
        s->raw_intr |= INTR_START_DET | INTR_ACTIVITY;
        if (i2c_start_transfer(s->bus, target & 0x7f, receiving)) {
            /* A failed repeated start may still leave a previous slave on
             * the QEMU bus. End the attempt before reporting an abort. */
            i2c_end_transfer(s->bus);
            s->active = false;
            best1700_i2c_abort(s, ABRT_7B_ADDR_NOACK);
            return;
        }
        s->active = true;
        s->receiving = receiving;
    }
    if (receiving) {
        uint8_t byte = i2c_recv(s->bus);

        if (s->rx_count == BEST1700_I2C_FIFO_DEPTH) {
            s->raw_intr |= INTR_RX_OVER;
        } else {
            s->rx_fifo[(s->rx_head + s->rx_count) %
                       BEST1700_I2C_FIFO_DEPTH] = byte;
            s->rx_count++;
        }
    } else if (i2c_send(s->bus, command & 0xff)) {
        best1700_i2c_abort(s, ABRT_TXDATA_NOACK);
        return;
    }
    if (command & CMD_STOP) {
        best1700_i2c_stop(s);
    }
}

static uint64_t best1700_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700I2CState *s = opaque;
    uint32_t value = 0;

    if (offset >= sizeof(s->regs) || (offset & 3)) {
        return 0;
    }
    switch (offset) {
    case IC_DATA_CMD:
        if (s->rx_count) {
            value = s->rx_fifo[s->rx_head];
            s->rx_head = (s->rx_head + 1) % BEST1700_I2C_FIFO_DEPTH;
            s->rx_count--;
        } else if (best1700_i2c_enabled(s)) {
            s->raw_intr |= INTR_RX_UNDER;
        }
        break;
    case IC_INTR_STAT:
        value = best1700_i2c_raw_status(s) & s->regs[IC_INTR_MASK / 4];
        break;
    case IC_RAW_INTR_STAT:
        value = best1700_i2c_raw_status(s);
        break;
    case IC_CLR_INTR:
        s->raw_intr = 0;
        s->abort_source = 0;
        break;
    case IC_CLR_RX_UNDER ... IC_CLR_GEN_CALL:
    {
        static const uint32_t clear_bits[] = {
            1u << 0, 1u << 1, 1u << 3, 1u << 5, 1u << 6,
            1u << 7, 1u << 8, 1u << 9, 1u << 10, 1u << 11,
        };

        s->raw_intr &= ~clear_bits[(offset - IC_CLR_RX_UNDER) / 4];
        if (offset == IC_CLR_TX_ABRT) {
            s->abort_source = 0;
        }
        break;
    }
    case IC_ENABLE_STATUS:
        value = best1700_i2c_enabled(s);
        break;
    case IC_STATUS:
        value = (1u << 1) | (1u << 2); /* TFNF and TFE */
        if (s->active) {
            value |= (1u << 0) | (1u << 5); /* ACTIVITY and MST_ACTIVITY */
        }
        if (s->rx_count) {
            value |= 1u << 3;
        }
        if (s->rx_count == BEST1700_I2C_FIFO_DEPTH) {
            value |= 1u << 4;
        }
        break;
    case IC_TXFLR:
        value = 0;
        break;
    case IC_RXFLR:
        value = s->rx_count;
        break;
    case IC_TX_ABRT_SOURCE:
        value = s->abort_source;
        break;
    case IC_COMP_PARAM_1:
        /* Encoded parameters: 32-bit APB, three speed modes, eight-byte
         * FIFOs. DMA is not advertised by this initial controller. */
        value = ((BEST1700_I2C_FIFO_DEPTH - 1) << 16) |
                ((BEST1700_I2C_FIFO_DEPTH - 1) << 8) | 0x8e;
        break;
    case IC_COMP_VERSION:
        value = 0x3131312a;
        break;
    case IC_COMP_TYPE:
        value = 0x44570140;
        break;
    default:
        value = s->regs[offset / 4];
        break;
    }
    best1700_i2c_update_irq(s);
    return value;
}

static void best1700_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Best1700I2CState *s = opaque;

    if (offset >= sizeof(s->regs) || (offset & 3)) {
        return;
    }
    switch (offset) {
    case IC_DATA_CMD:
        best1700_i2c_command(s, value);
        break;
    case IC_ENABLE:
        if (!(value & 1)) {
            best1700_i2c_stop(s);
            s->rx_head = 0;
            s->rx_count = 0;
            s->raw_intr = 0;
            s->abort_source = 0;
        }
        s->regs[IC_ENABLE / 4] = value & 1;
        if ((value & 3) == 3) {
            best1700_i2c_abort(s, ABRT_USER);
        }
        break;
    case IC_INTR_MASK:
        s->regs[offset / 4] = value & INTR_MASK_BITS;
        break;
    case IC_RX_TL:
    case IC_TX_TL:
        s->regs[offset / 4] = MIN(value & 0xff, BEST1700_I2C_FIFO_DEPTH - 1);
        break;
    case IC_CON:
        if (!best1700_i2c_enabled(s)) {
            s->regs[offset / 4] = value & 0x7ff;
        }
        break;
    case IC_TAR:
        if (!s->active) {
            s->regs[offset / 4] = value & 0x1fff;
        }
        break;
    case IC_INTR_STAT:
    case IC_RAW_INTR_STAT:
    case IC_CLR_INTR ... IC_CLR_GEN_CALL:
    case IC_STATUS:
    case IC_TXFLR:
    case IC_RXFLR:
    case IC_TX_ABRT_SOURCE:
    case IC_ENABLE_STATUS:
    case IC_COMP_PARAM_1:
    case IC_COMP_VERSION:
    case IC_COMP_TYPE:
        break;
    default:
        s->regs[offset / 4] = value;
        break;
    }
    best1700_i2c_update_irq(s);
}

static const MemoryRegionOps best1700_i2c_ops = {
    .read = best1700_i2c_read,
    .write = best1700_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void best1700_i2c_reset(Best1700I2CState *s)
{
    best1700_i2c_stop(s);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    s->regs[IC_CON / 4] = 0x65;
    s->raw_intr = 0;
    s->abort_source = 0;
    s->commands = 0;
    s->rx_head = 0;
    s->rx_count = 0;
    s->receiving = false;
    best1700_i2c_update_irq(s);
}

static void best1700_i2c_device_reset(DeviceState *dev)
{
    best1700_i2c_reset(BEST1700_I2C(dev));
}

static void best1700_i2c_init(Object *obj)
{
    Best1700I2CState *s = BEST1700_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
    memory_region_init_io(&s->iomem, obj, &best1700_i2c_ops, s,
                          TYPE_BEST1700_I2C, BEST1700_I2C_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void best1700_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BEST1700 I2C master";
    dc->reset = best1700_i2c_device_reset;
}

static const TypeInfo best1700_i2c_info = {
    .name = TYPE_BEST1700_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700I2CState),
    .instance_init = best1700_i2c_init,
    .class_init = best1700_i2c_class_init,
};

static void best1700_i2c_register_types(void)
{
    type_register_static(&best1700_i2c_info);
}

type_init(best1700_i2c_register_types)
