#include "qemu/osdep.h"
#include "qemu/log.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "best1502p_aon_reset.h"

/* SDK reg_aoncmu_best1502p.h: GBL_RESET_PULSE/SET/CLR at +c4/c8/cc,
 * GLOBAL_RESETN bit7. reference native reboot writes 0x80 to SET. */
#define AON_GLOBAL_PORTS 0x400800c4u
#define GLOBAL_RESETN (1u << 7)

static uint64_t aon_read(void *opaque, hwaddr offset, unsigned size)
{
    Best1502pAonReset *reset = opaque;
    return offset == 4 ? reset->asserted : 0;
}

static void aon_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Best1502pAonReset *reset = opaque;
    if (offset == 8) {
        reset->asserted &= ~(uint32_t)value;
        return;
    }
    if (offset == 4) {
        reset->asserted |= value;
    }
    if ((offset == 0 || offset == 4) && (value & GLOBAL_RESETN)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "best1502p AON global reset: port=%08" HWADDR_PRIx
                      " value=%08" PRIx64 "\n", AON_GLOBAL_PORTS + offset, value);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static const MemoryRegionOps aon_ops = {
    .read = aon_read,
    .write = aon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void aon_reset(void *opaque)
{
    Best1502pAonReset *reset = opaque;
    reset->asserted = 0;
}

void best1502p_aon_reset_init(Best1502pAonReset *reset, MemoryRegion *memory)
{
    memset(reset, 0, sizeof(*reset));
    memory_region_init_io(&reset->region, NULL, &aon_ops, reset,
                          "best1502p.aon-global-reset", 12);
    memory_region_add_subregion_overlap(memory, AON_GLOBAL_PORTS,
                                       &reset->region, 3);
    qemu_register_reset(aon_reset, reset);
}
