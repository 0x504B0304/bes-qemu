#include "qemu/osdep.h"
#include "qemu/log.h"
#include "miband_peripherals.h"

#define AON_CMU_BASE 0x40080000u
#define AON_CMU_CHIP_ID 0x000u
#define AON_CMU_CHIP_ID_VALUE 0x0153u
#define AON_CMU_GBL_RESET_PULSE 0x0c4u
#define AON_CMU_GBL_RESET_SET   0x0c8u
#define AON_CMU_GBL_RESET_CLR   0x0ccu
#define AON_CMU_GLOBAL_RESETN   (1u << 7)

static uint32_t aoncmu_merge_write(uint32_t old, hwaddr address,
                                   unsigned size, uint64_t value)
{
    unsigned shift = (address & 3) * 8;
    uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : UINT32_MAX;

    return (old & ~(mask << shift)) |
           (((uint32_t)value & mask) << shift);
}

void miband_aoncmu_init(MiBandAonCmu *aoncmu, uint32_t metal_revision)
{
    memset(aoncmu, 0, sizeof(*aoncmu));
    /* best1503 exposes chip ID in bits 15:0 and metal revision in bits 31:16. */
    aoncmu->chip_id = (metal_revision << 16) | AON_CMU_CHIP_ID_VALUE;
}

void miband_aoncmu_reset(MiBandAonCmu *aoncmu)
{
    aoncmu->global_reset_set = 0;
    aoncmu->chip_id_reads = 0;
}

void miband_aoncmu_set_reset_request(MiBandAonCmu *aoncmu,
                                     void (*reset_request)(void *opaque,
                                                           uint32_t mask),
                                     void *opaque)
{
    aoncmu->reset_request = reset_request;
    aoncmu->reset_opaque = opaque;
}

bool miband_aoncmu_read(MiBandAonCmu *aoncmu, hwaddr address, unsigned size,
                        uint64_t *result)
{
    if (address == AON_CMU_BASE + AON_CMU_GBL_RESET_SET && size <= 4) {
        *result = (aoncmu->global_reset_set >> ((address & 3) * 8)) &
                  (size == 1 ? 0xffu : size == 2 ? 0xffffu : UINT32_MAX);
        return true;
    }
    if (address != AON_CMU_BASE + AON_CMU_CHIP_ID || size != 4) {
        return false;
    }
    *result = aoncmu->chip_id;
    if (aoncmu->chip_id_reads++ < 8) {
        qemu_log_mask(LOG_UNIMP,
                      "miband AON CMU CHIP_ID read -> %08" PRIx32 "\n",
                      aoncmu->chip_id);
    }
    return true;
}

bool miband_aoncmu_write(MiBandAonCmu *aoncmu, hwaddr address, unsigned size,
                         uint64_t value)
{
    uint32_t command;

    if (address < AON_CMU_BASE || size == 0 || size > 4 ||
        address + size > AON_CMU_BASE + 0x1000) {
        return false;
    }

    switch (address & ~3u) {
    case AON_CMU_GBL_RESET_SET + AON_CMU_BASE:
        /* The SDK's SET port is write-one-to-assert.  It is not a normal
         * storage register, so partial writes are merged into the command
         * word before applying the selected reset lines. */
        command = aoncmu_merge_write(0, address, size, value);
        aoncmu->global_reset_set |= command;
        if ((command & AON_CMU_GLOBAL_RESETN) && aoncmu->reset_request) {
            aoncmu->reset_request(aoncmu->reset_opaque, command);
        }
        return true;
    case AON_CMU_GBL_RESET_CLR + AON_CMU_BASE:
        command = aoncmu_merge_write(0, address, size, value);
        aoncmu->global_reset_set &= ~command;
        return true;
    case AON_CMU_GBL_RESET_PULSE + AON_CMU_BASE:
        command = aoncmu_merge_write(0, address, size, value);
        if ((command & AON_CMU_GLOBAL_RESETN) && aoncmu->reset_request) {
            aoncmu->reset_request(aoncmu->reset_opaque, command);
        }
        return true;
    default:
        /* CHIP_ID is read-only; other AON registers stay in the generic
         * compatibility bank until their SDK semantics are needed. */
        return false;
    }
}
