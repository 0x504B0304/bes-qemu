#include "qemu/osdep.h"
#include "miband_peripherals.h"
#include "best1503_package_config.h"

#define PSRAM_MC_BASE     0x40160000
#define PSRAM_MC_COMMAND  0x04
#define PSRAM_MC_ADDRESS  0x08
#define PSRAM_MC_COUNT    0x0c
#define PSRAM_MC_WRITE    0x14
#define PSRAM_MC_READ     0x18
#define PSRAM_READ_WORDS  2
#define PSRAM_WRITE_MODE  5
#define PSRAM_READ_MODE   6
#define PSRAM_CTRL_STATUS 0x40168058
#define PSRAM_READY       (1U << 6)
#define PSRAM_DLL_DLY_IN  32U
#define PSRAM_FIFO_STATUS 0x40160404
#define PSRAM_RX_COUNT    (8U << 7)

void miband_psram_init(MiBandPsram *psram, uint8_t *storage,
                       uint32_t storage_size)
{
    miband_psram_device_init(&psram->device, storage, storage_size,
                             BEST1503_PACKAGE_PSRAM_MR1);
    psram->command_address = 0;
    psram->words_remaining = 0;
    psram->command = 0;
    psram->tx_word = 0;
}

bool miband_psram_read(MiBandPsram *psram, hwaddr address, unsigned size,
                       uint64_t *result)
{
    uint32_t word;
    if (size != sizeof(uint32_t)) {
        return false;
    }

    if (address == PSRAM_CTRL_STATUS) {
        *result = PSRAM_READY | PSRAM_DLL_DLY_IN;
        return true;
    }
    if (address == PSRAM_FIFO_STATUS) {
        *result = PSRAM_RX_COUNT;
        return true;
    }
    if (address == PSRAM_MC_BASE + PSRAM_MC_READ) {
        *result = 0;
        if (psram->command == PSRAM_READ_MODE && psram->words_remaining) {
            *result = miband_psram_device_read_mode(&psram->device, psram->command_address);
            psram->words_remaining--;
        } else if (psram->command == PSRAM_READ_WORDS &&
            psram->words_remaining &&
            miband_psram_device_read_word(&psram->device, psram->command_address,
                                          &word)) {
            *result = word;
            psram->command_address += sizeof(uint32_t);
            psram->words_remaining--;
        }
        return true;
    }
    return false;
}

bool miband_psram_write(MiBandPsram *psram, hwaddr address, unsigned size,
                        uint64_t value)
{
    if (size != sizeof(uint32_t)) {
        return false;
    }
    switch (address) {
    case PSRAM_MC_BASE + PSRAM_MC_WRITE:
        psram->tx_word = value;
        return true;
    case PSRAM_MC_BASE + PSRAM_MC_ADDRESS:
        psram->command_address = value;
        return true;
    case PSRAM_MC_BASE + PSRAM_MC_COUNT:
        psram->words_remaining = value;
        return true;
    case PSRAM_MC_BASE + PSRAM_MC_COMMAND:
        psram->command = value;
        if (value == PSRAM_WRITE_MODE && psram->words_remaining) {
            /* MR1 is the read-only identity register. The early driver
             * programs MR0/MR2/MR4/MR8 through single-word transactions. */
            miband_psram_device_write_mode(&psram->device, psram->command_address,
                                           psram->tx_word);
            psram->words_remaining = 0;
        }
        return true;
    default:
        return false;
    }
}
