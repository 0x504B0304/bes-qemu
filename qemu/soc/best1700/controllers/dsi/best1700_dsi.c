/*
 * BEST1700 DSI command transport. Panel commands belong to a board endpoint;
 * this device only handles packet framing, FIFOs and interrupt delivery.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "best1700_dsi.h"

#define DSI_RX_STATUS  0x10
#define DSI_COMMAND    0x14
#define DSI_TX_HEADER  0x18
#define DSI_TX_WORD    0x1c
#define DSI_TX_FINAL1  0x20
#define DSI_TX_FINAL2  0x24
#define DSI_TX_FINAL3  0x28
#define DSI_TX_FINAL4  0x2c

#define RX_READ_ENABLE (1u << 15)
#define EVENT_CMD_DONE (1u << 0)
#define EVENT_BTA_DONE (1u << 1)
#define EVENT_BTA_FAIL (1u << 2)
#define EVENT_RX_DONE  (1u << 3)
#define EVENT_RX_ERROR (1u << 4)
#define EVENT_TIMEOUT  (1u << 6)
#define EVENT_TE       (1u << 7)

static void best1700_dsi_update_irq(Best1700DsiState *s)
{
    qemu_set_irq(s->irq, (s->events & (s->regs[DSI_RX_STATUS / 4] >> 24)) != 0);
}

static uint8_t best1700_dsi_ecc(const uint8_t header[3])
{
    static const uint32_t parity_masks[] = {
        0xf12cb7, 0xf2555b, 0x749a6d, 0xb8e38e, 0xdf03f0, 0xeffc00,
    };
    uint32_t word = header[0] | (header[1] << 8) | (header[2] << 16);
    uint8_t ecc = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(parity_masks); i++) {
        uint32_t bits = word & parity_masks[i];
        unsigned parity = 0;

        while (bits) {
            parity ^= bits & 1;
            bits >>= 1;
        }
        ecc |= parity << i;
    }
    return ecc;
}

static uint16_t best1700_dsi_crc(const uint8_t *data, unsigned length)
{
    uint16_t crc = 0xffff;

    for (unsigned i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
        }
    }
    return crc;
}

static void best1700_dsi_read_reply(Best1700DsiState *s, uint8_t command)
{
    uint8_t payload[BEST1700_DSI_MAX_PAYLOAD];
    unsigned capacity = MIN(s->max_return, sizeof(payload));
    int length;

    if (!s->panel.read) {
        s->events |= EVENT_BTA_FAIL | EVENT_TIMEOUT;
        return;
    }
    if (s->rx_count) {
        s->events |= EVENT_BTA_FAIL | EVENT_RX_ERROR;
        return;
    }
    length = s->panel.read(s->panel.opaque, command, payload, capacity);
    if (length <= 0 || length > capacity) {
        s->events |= EVENT_BTA_FAIL | EVENT_TIMEOUT;
        return;
    }
    s->rx_head = 0;
    /* The receive FIFO includes the LP data-transmission escape byte. */
    s->rx_fifo[0] = 0x87;
    if (length <= 2) {
        s->rx_fifo[1] = (length == 1 ? 0x21 : 0x22) |
                        (s->packet_header & 0xc0);
        s->rx_fifo[2] = payload[0];
        s->rx_fifo[3] = length == 2 ? payload[1] : 0;
        s->rx_fifo[4] = best1700_dsi_ecc(&s->rx_fifo[1]);
        s->rx_count = 5;
    } else {
        uint16_t crc = best1700_dsi_crc(payload, length);

        s->rx_fifo[1] = 0x1c | (s->packet_header & 0xc0);
        s->rx_fifo[2] = length & 0xff;
        s->rx_fifo[3] = length >> 8;
        s->rx_fifo[4] = best1700_dsi_ecc(&s->rx_fifo[1]);
        memcpy(&s->rx_fifo[5], payload, length);
        s->rx_fifo[5 + length] = crc & 0xff;
        s->rx_fifo[6 + length] = crc >> 8;
        s->rx_count = length + 7;
    }
    s->events |= EVENT_BTA_DONE | EVENT_RX_DONE;
}

static void best1700_dsi_submit(Best1700DsiState *s, uint32_t value)
{
    unsigned operation = value & 7;
    unsigned wire_length = (value >> 16) & 0xfff;
    uint8_t data_type = s->packet_header & 0x3f;
    uint8_t command = (s->packet_header >> 8) & 0xff;
    uint8_t parameter = (s->packet_header >> 16) & 0xff;
    bool accepted = true;

    /* Bus transactions complete synchronously. The LP busy bit therefore
     * clears before the next register read, independently of IRQ masking. */
    s->regs[DSI_COMMAND / 4] = value & ~(1u << 4);
    if (operation < 4) {
        return; /* Clock-lane and ULPS commands have no packet payload. */
    }
    if (s->commands++ < 96) {
        qemu_log_mask(LOG_UNIMP,
                      "best1700 dsi packet type=0x%02x command=0x%02x"
                      " bytes=%u wire=%u operation=%u\n",
                      data_type, command, s->tx_count, wire_length, operation);
    }
    s->events |= EVENT_CMD_DONE;
    switch (data_type) {
    case 0x37: /* Set maximum return packet size. */
        s->max_return = (s->packet_header >> 8) & 0xffff;
        break;
    case 0x06: /* DCS read; the command must include bus turnaround. */
        if (operation >= 6) {
            best1700_dsi_read_reply(s, command);
        }
        break;
    case 0x05: /* DCS short write, no parameter. */
    case 0x15: /* DCS short write, one parameter. */
        if (s->panel.write) {
            accepted = s->panel.write(s->panel.opaque, command, &parameter,
                                      data_type == 0x15 ? 1 : 0);
        }
        break;
    case 0x39: /* DCS long write: word count includes the DCS command. */
    {
        unsigned length = (s->packet_header >> 8) & 0xffff;

        if (s->tx_overflow || !length || length > s->tx_count ||
            wire_length != length + 7) {
            s->events |= EVENT_RX_ERROR;
        } else if (s->panel.write) {
            accepted = s->panel.write(s->panel.opaque, s->tx_payload[0],
                                      &s->tx_payload[1], length - 1);
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "best1700 dsi: unsupported data type 0x%02x\n",
                      data_type);
        accepted = false;
        break;
    }
    if (!accepted && operation >= 6) {
        s->events |= EVENT_BTA_FAIL;
    }
    s->tx_count = 0;
    s->tx_overflow = false;
    best1700_dsi_update_irq(s);
}

static uint64_t best1700_dsi_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1700DsiState *s = opaque;
    uint32_t value;

    if (offset >= sizeof(s->regs) || (offset & 3)) {
        return 0;
    }
    value = s->regs[offset / 4];
    if (offset == DSI_RX_STATUS) {
        value |= (s->events & 0xff) << 16;
        /* Count describes this read, before its low-byte FIFO pop. */
        value |= MIN(s->rx_count, 31u) << 8;
        if (s->rx_count) {
            value |= s->rx_fifo[s->rx_head];
            if (value & RX_READ_ENABLE) {
                s->rx_head++;
                s->rx_count--;
            }
        }
    }
    return value;
}

static void best1700_dsi_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Best1700DsiState *s = opaque;

    if (offset >= sizeof(s->regs) || (offset & 3)) {
        return;
    }
    switch (offset) {
    case DSI_RX_STATUS:
        s->events &= ~((value >> 16) & 0xff);
        s->regs[offset / 4] = value & 0xff00e000;
        best1700_dsi_update_irq(s);
        break;
    case DSI_COMMAND:
        best1700_dsi_submit(s, value);
        break;
    case DSI_TX_HEADER:
        s->packet_header = value;
        s->regs[offset / 4] = value;
        s->tx_count = 0;
        s->tx_overflow = false;
        break;
    case DSI_TX_WORD:
    case DSI_TX_FINAL1:
    case DSI_TX_FINAL2:
    case DSI_TX_FINAL3:
    case DSI_TX_FINAL4:
    {
        unsigned bytes = offset == DSI_TX_WORD ? 4 :
                         (offset - DSI_TX_FINAL1) / 4 + 1;

        for (unsigned i = 0; i < bytes; i++) {
            if (s->tx_count < sizeof(s->tx_payload)) {
                s->tx_payload[s->tx_count++] = value >> (8 * i);
            } else {
                s->tx_overflow = true;
            }
        }
        break;
    }
    default:
        s->regs[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps best1700_dsi_ops = {
    .read = best1700_dsi_read,
    .write = best1700_dsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void best1700_dsi_connect_panel(Best1700DsiState *s,
                                const Best1700DsiPanelEndpoint *panel)
{
    s->panel = panel ? *panel : (Best1700DsiPanelEndpoint) {0};
}

void best1700_dsi_te(Best1700DsiState *s)
{
    s->events |= EVENT_TE;
    best1700_dsi_update_irq(s);
}

void best1700_dsi_reset(Best1700DsiState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    memset(s->tx_payload, 0, sizeof(s->tx_payload));
    s->packet_header = 0;
    s->events = 0;
    s->commands = 0;
    s->max_return = 1;
    s->tx_count = 0;
    s->tx_overflow = false;
    s->rx_head = 0;
    s->rx_count = 0;
    best1700_dsi_update_irq(s);
}

static void best1700_dsi_device_reset(DeviceState *dev)
{
    best1700_dsi_reset(BEST1700_DSI(dev));
}

static void best1700_dsi_init(Object *obj)
{
    Best1700DsiState *s = BEST1700_DSI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &best1700_dsi_ops, s,
                          TYPE_BEST1700_DSI, BEST1700_DSI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void best1700_dsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "BEST1700 DSI command controller";
    dc->reset = best1700_dsi_device_reset;
}

static const TypeInfo best1700_dsi_info = {
    .name = TYPE_BEST1700_DSI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Best1700DsiState),
    .instance_init = best1700_dsi_init,
    .class_init = best1700_dsi_class_init,
};

static void best1700_dsi_register_types(void)
{
    type_register_static(&best1700_dsi_info);
}

type_init(best1700_dsi_register_types)
