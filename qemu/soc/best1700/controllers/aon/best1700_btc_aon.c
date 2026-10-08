/* BEST1700 Bluetooth-controller boot state.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_btc_aon.h"

static MemTxResult best1700_btc_aon_unsupported(hwaddr offset,
                                               bool write, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 BTC AON unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 "\n", write ? "write" : "read", offset, value);
    return MEMTX_ERROR;
}

static MemTxResult best1700_btc_aon_read(void *opaque, hwaddr offset,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    Best1700BtcAon *s = opaque;

    switch (offset) {
    case 0x00:
        *value = s->boot_flags;
        return MEMTX_OK;
    case 0x04:
        /* Bit 31 selects the firmware's wake/resume path. Cold zero is a
         * compatibility choice, not a measured silicon reset value. */
        *value = 0;
        return MEMTX_OK;
    default:
        *value = 0;
        return best1700_btc_aon_unsupported(offset, false, 0);
    }
}

static MemTxResult best1700_btc_aon_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    Best1700BtcAon *s = opaque;

    if (offset == 0) {
        /* The guest records its own boot progress here. */
        s->boot_flags = value;
        return MEMTX_OK;
    }
    /* +4 bit 5/6 writes occur around power transitions, whose effects are
     * not established. Do not turn those requests into successful storage
     * or an invented sleep/resume completion. */
    return best1700_btc_aon_unsupported(offset, true, value);
}

static const MemoryRegionOps best1700_btc_aon_ops = {
    .read_with_attrs = best1700_btc_aon_read,
    .write_with_attrs = best1700_btc_aon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_btc_aon_init(Best1700BtcAon *s, Object *owner)
{
    memory_region_init_io(&s->iomem, owner, &best1700_btc_aon_ops, s,
                          "best1700.btc-aon", BEST1700_BTC_AON_WINDOW);
    best1700_btc_aon_reset(s);
}

void best1700_btc_aon_reset(Best1700BtcAon *s)
{
    s->boot_flags = 0;
}
