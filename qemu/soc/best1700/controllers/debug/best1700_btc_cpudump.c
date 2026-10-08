/* BEST1700 Bluetooth-controller instruction trace controls.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "best1700_btc_cpudump.h"

#define BTC_CPUDUMP_ENABLE 0x0u
#define BTC_CPUDUMP_COMMAND 0x4u
#define BTC_CPUDUMP_CONTROL_BASE 0x2000u
#define BTC_CPUDUMP_STOP 1u
#define BTC_CPUDUMP_START 2u
#define BTC_CPUDUMP_TRACE_WORDS \
    (BEST1700_BTC_CPUDUMP_TRACE_SIZE / sizeof(uint32_t))

static MemTxResult best1700_btc_cpudump_unsupported(hwaddr offset,
                                                   bool write, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
        "best1700 BTC CPUDUMP unsupported %s offset=0x%" HWADDR_PRIx
        " value=0x%" PRIx64 " (trace capture unavailable)\n",
        write ? "write" : "read", offset + BTC_CPUDUMP_CONTROL_BASE, value);
    return MEMTX_ERROR;
}

static void best1700_btc_cpudump_sample(void *opaque)
{
    Best1700BtcCpudump *s = opaque;
    uint32_t *trace;
    int64_t now;

    if (s->enabled && s->capture_requested && s->pc) {
        trace = memory_region_get_ram_ptr(&s->trace);
        /* The firmware's dump scanner identifies valid samples by bit 0.
         * Store the current instruction address with that marker and advance
         * a power-of-two ring exactly like the hardware write pointer. */
        trace[s->trace_index] = (*s->pc & ~1u) | 1u;
        s->trace_index = (s->trace_index + 1) % BTC_CPUDUMP_TRACE_WORDS;
    }
    if (!s->enabled || !s->capture_requested) {
        timer_del(s->sample_timer);
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now <= INT64_MAX - s->sample_ns) {
        timer_mod_ns(s->sample_timer, now + s->sample_ns);
    }
}

static MemTxResult best1700_btc_cpudump_read(void *opaque, hwaddr offset,
                                           uint64_t *value, unsigned size,
                                           MemTxAttrs attrs)
{
    Best1700BtcCpudump *s = opaque;

    if (offset == BTC_CPUDUMP_ENABLE) {
        *value = s->enabled;
        return MEMTX_OK;
    }
    if (offset == BTC_CPUDUMP_COMMAND) {
        /* No pending command strobe is model policy, not capture status.
         * Reads neither expose nor consume the recorded capture request. */
        *value = 0;
        return MEMTX_OK;
    }
    *value = 0;
    return best1700_btc_cpudump_unsupported(offset, false, 0);
}

static MemTxResult best1700_btc_cpudump_write(void *opaque, hwaddr offset,
                                            uint64_t value, unsigned size,
                                            MemTxAttrs attrs)
{
    Best1700BtcCpudump *s = opaque;

    switch (offset) {
    case BTC_CPUDUMP_ENABLE:
        if (value > 1) {
            break;
        }
        s->enabled = value != 0;
        if (!s->enabled) {
            s->capture_requested = false;
            timer_del(s->sample_timer);
        } else if (s->capture_requested) {
            timer_mod_ns(s->sample_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         s->sample_ns);
        }
        return MEMTX_OK;
    case BTC_CPUDUMP_COMMAND:
        switch (value) {
        case 0:
            return MEMTX_OK;
        case BTC_CPUDUMP_STOP:
            s->capture_requested = false;
            timer_del(s->sample_timer);
            return MEMTX_OK;
        case BTC_CPUDUMP_START:
            s->capture_requested = true;
            if (s->enabled) {
                timer_mod_ns(s->sample_timer,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                             s->sample_ns);
            }
            return MEMTX_OK;
        default:
            /* Simultaneous start/stop priority and reserved bits are not
             * established. Reject them without changing command state. */
            break;
        }
        break;
    default:
        break;
    }
    return best1700_btc_cpudump_unsupported(offset, true, value);
}

static const MemoryRegionOps best1700_btc_cpudump_ops = {
    .read_with_attrs = best1700_btc_cpudump_read,
    .write_with_attrs = best1700_btc_cpudump_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void best1700_btc_cpudump_init(Best1700BtcCpudump *s, Object *owner,
                                uint32_t sample_ns)
{
    /* This is a sampled-PC diagnostic model, not instruction trace. */
    assert(sample_ns);
    s->sample_ns = sample_ns;
    memory_region_init(&s->iomem, owner, "best1700.btc-cpudump",
                       BEST1700_BTC_CPUDUMP_WINDOW);
    /* QEMU's RAM-region helper requires a DeviceState owner.  The machine
     * object owns this controller but is not itself a DeviceState, so leave
     * this small trace buffer unowned like the other BEST1700 SRAM regions. */
    memory_region_init_ram(&s->trace, NULL, "best1700.btc-cpudump.trace",
                           BEST1700_BTC_CPUDUMP_TRACE_SIZE, &error_fatal);
    memory_region_init_io(&s->control, owner, &best1700_btc_cpudump_ops, s,
                          "best1700.btc-cpudump.control", 0x1000);
    memory_region_add_subregion(&s->iomem, 0, &s->trace);
    memory_region_add_subregion(&s->iomem, 0x2000, &s->control);
    s->sample_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   best1700_btc_cpudump_sample, s);
    best1700_btc_cpudump_reset(s);
}

void best1700_btc_cpudump_reset(Best1700BtcCpudump *s)
{
    if (s->sample_timer) {
        timer_del(s->sample_timer);
    }
    if (s->trace.ram_block) {
        memset(memory_region_get_ram_ptr(&s->trace), 0,
               BEST1700_BTC_CPUDUMP_TRACE_SIZE);
    }
    s->enabled = false;
    s->capture_requested = false;
    s->trace_index = 0;
    /* Capture is explicitly armed by the guest.  Leaving the timer idle
     * avoids waking QEMU once per microsecond while the block is unused. */
}

void best1700_btc_cpudump_set_pc(Best1700BtcCpudump *s,
                                 const uint32_t *pc)
{
    s->pc = pc;
}
