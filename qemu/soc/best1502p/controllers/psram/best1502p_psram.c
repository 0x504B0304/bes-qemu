#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "best1502p_psram.h"

#define PSRAM_MC_COMMAND 0x04u
#define PSRAM_MC_ADDRESS 0x08u
#define PSRAM_MC_COUNT   0x0cu
#define PSRAM_MC_WRITE   0x14u
#define PSRAM_MC_READ    0x18u
#define PSRAM_READ_WORDS 2u
#define PSRAM_WRITE_MODE 5u
#define PSRAM_READ_MODE  6u
#define PSRAM_CTRL_STATUS 0x08058u
#define PSRAM_FIFO_STATUS 0x00404u
#define PSRAM_READY       (1u << 6)
#define PSRAM_DLL_DLY_IN  32u
#define PSRAM_RX_COUNT    (8u << 7)

static void psram_trace(Best1502pPsram *psram, const char *kind,
                        hwaddr offset, uint64_t value)
{
    if (psram->trace_events++ >= 1024) {
        return;
    }
    uint32_t pc = current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;
    qemu_log_mask(LOG_UNIMP,
                  "best1502p PSRAM %s %08" HWADDR_PRIx
                  " value=%08" PRIx64 " pc=%08" PRIx32 "\n",
                  kind, psram->base + offset, value, pc);
}

static uint32_t psram_read_word(const Best1502pPsram *psram,
                                uint32_t address, bool *valid)
{
    if (!psram->storage || address > psram->storage_size - sizeof(uint32_t)) {
        *valid = false;
        return 0;
    }
    *valid = true;
    return ldl_le_p(psram->storage + address);
}

void best1502p_psram_init(Best1502pPsram *psram, hwaddr base,
                          uint8_t *storage, uint32_t storage_size)
{
    memset(psram, 0, sizeof(*psram));
    psram->base = base;
    psram->storage = storage;
    psram->storage_size = storage_size;
    /* The existing BEST1503 package model and qtest evidence identify MR1
     * with 0x0d. Keep this as a die-level default until reference package dumps
     * provide a different identity. */
    psram->mode_registers[1] = 0x0d;
}

bool best1502p_psram_read(Best1502pPsram *psram, hwaddr offset,
                          unsigned size, uint64_t *result)
{
    bool valid;
    uint32_t word;

    if (size != sizeof(uint32_t) || offset >= BEST1502P_PSRAM_CTRL_SIZE) {
        return false;
    }
    switch (offset) {
    case PSRAM_CTRL_STATUS:
        *result = PSRAM_READY | PSRAM_DLL_DLY_IN;
        break;
    case PSRAM_FIFO_STATUS:
        *result = PSRAM_RX_COUNT;
        break;
    case PSRAM_MC_READ:
        *result = 0;
        if (psram->command == PSRAM_READ_MODE && psram->words_remaining) {
            *result = psram->mode_registers[psram->command_address & 0xff];
            psram->words_remaining--;
        } else if (psram->command == PSRAM_READ_WORDS &&
                   psram->words_remaining) {
            word = psram_read_word(psram, psram->command_address, &valid);
            if (valid) {
                *result = word;
                psram->command_address += sizeof(uint32_t);
            }
            psram->words_remaining--;
        }
        break;
    default:
        *result = psram->regs[offset >> 2];
        break;
    }
    psram_trace(psram, "read", offset, *result);
    return true;
}

bool best1502p_psram_write(Best1502pPsram *psram, hwaddr offset,
                           unsigned size, uint64_t value)
{
    uint32_t command = (uint32_t)value;

    if (size != sizeof(uint32_t) || offset >= BEST1502P_PSRAM_CTRL_SIZE) {
        return false;
    }
    switch (offset) {
    case PSRAM_MC_WRITE:
        psram->tx_word = command;
        break;
    case PSRAM_MC_ADDRESS:
        psram->command_address = command;
        break;
    case PSRAM_MC_COUNT:
        psram->words_remaining = command;
        break;
    case PSRAM_MC_COMMAND:
        psram->command = command;
        if (command == PSRAM_WRITE_MODE && psram->words_remaining) {
            if ((psram->command_address & 0xff) != 1) {
                psram->mode_registers[psram->command_address & 0xff] =
                    psram->tx_word;
            }
            psram->words_remaining--;
        }
        break;
    default:
        psram->regs[offset >> 2] = command;
        break;
    }
    psram_trace(psram, "write", offset, command);
    return true;
}
