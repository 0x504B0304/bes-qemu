/* BEST1700 Bluetooth GPIO/test-port configuration latches.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_btc_gpio.h"

#define BTC_GPIO_MODE_MASK 0x00000006u

static MemTxResult gpio_unsupported(hwaddr offset, bool write, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 BTC GPIO config unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 "\n", write ? "write" : "read", offset, value);
    return MEMTX_ERROR;
}

static MemTxResult gpio_read(void *opaque, hwaddr offset, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    Best1700BtcGpio *s = opaque;

    *value = 0;
    switch (offset) {
    case 0x000:
        *value = s->test_port;
        return MEMTX_OK;
    case 0x00c:
        *value = s->mode;
        return MEMTX_OK;
    default:
        return gpio_unsupported(offset, false, 0);
    }
}

static MemTxResult gpio_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    Best1700BtcGpio *s = opaque;

    switch (offset) {
    case 0x000:
        /* Firmware saves/restores this complete test-port word. The storage
         * width does not assert a verified silicon mask or pin behavior. */
        s->test_port = value;
        return MEMTX_OK;
    case 0x00c:
        /* Only cold configuration bits 1:2 have retained-readback semantics.
         * Bit 0 is written by the original IRQ29 handler; its interrupt-side
         * command is not established and must not become passive storage.
         * Reject unsupported bits before changing either supported field. */
        if (value & ~BTC_GPIO_MODE_MASK) {
            return gpio_unsupported(offset, true, value);
        }
        s->mode = value;
        return MEMTX_OK;
    default:
        return gpio_unsupported(offset, true, value);
    }
}

static const MemoryRegionOps gpio_ops = {
    .read_with_attrs = gpio_read,
    .write_with_attrs = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_btc_gpio_init(Best1700BtcGpio *s, Object *owner)
{
    memory_region_init_io(&s->iomem, owner, &gpio_ops, s,
                          "best1700.btc-gpio", BEST1700_BTC_GPIO_WINDOW);
    best1700_btc_gpio_reset(s);
}

void best1700_btc_gpio_reset(Best1700BtcGpio *s)
{
    /* Explicit model defaults, not measured silicon reset values. */
    s->test_port = 0;
    s->mode = 0;
}
