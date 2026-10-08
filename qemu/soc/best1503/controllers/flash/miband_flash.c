/* BEST1503 flash command/FIFO and mapped-cache controller subset.
 * Device command semantics live in package NOR and external SPI-NAND endpoints.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "miband_flash.h"

#define FLASH_CTRL_BASE  0x40140000
#define FLASH1_CTRL_BASE 0x40500000
#define FLASH_CTRL_SIZE 0x1000
#define FLASH_COMMAND   0x00
#define FLASH_CONTROL   0x04
#define FLASH_FIFO_STATUS 0x0c
#define FLASH_RX_DATA   0x10
#define FLASH_RX_COUNT_SHIFT 4
#define FLASH_TX_FREE_SHIFT 9
#define FLASH_TX_FREE_SLOTS 16

void miband_flash_init(MiBandFlash *flash, MiBandFlashEndpoint endpoint,
                        uint8_t *mapped_cache, unsigned mapped_cache_size)
{
    assert(endpoint.command && endpoint.response && endpoint.is_poll);
    assert(endpoint.cache_size <= mapped_cache_size);
    assert(!endpoint.cache_size || (mapped_cache && endpoint.read_cache));
    memset(flash, 0, sizeof(*flash));
    flash->endpoint = endpoint;
    flash->mapped_cache = mapped_cache;
    flash->mapped_cache_size = mapped_cache_size;
}

void miband_flash_connect_dma(MiBandFlash *flash, MiBandFlashDmaCallbacks callbacks)
{
    flash->dma = callbacks;
}

const uint8_t *miband_flash_dma_source(MiBandFlash *flash, unsigned bytes)
{
    unsigned column = flash->command >> 8;
    if (!bytes || !flash->mapped_cache || column > flash->endpoint.cache_size ||
        bytes > flash->endpoint.cache_size - column) return NULL;
    return flash->mapped_cache + column;
}

uint8_t *miband_flash_dma_target(MiBandFlash *flash, unsigned bytes)
{
    if (!bytes || !flash->endpoint.program_buffer) return NULL;
    return flash->endpoint.program_buffer(flash->endpoint.opaque,
                                           flash->command >> 8, bytes);
}

static MiBandFlash *miband_flash_bank(MiBandFlash flash[MIBAND_FLASH_COUNT],
                                      hwaddr address, hwaddr *offset)
{
    static const hwaddr bases[MIBAND_FLASH_COUNT] = {
        FLASH_CTRL_BASE, FLASH1_CTRL_BASE,
    };
    for (unsigned i = 0; i < MIBAND_FLASH_COUNT; i++) {
        if (address >= bases[i] && address - bases[i] < FLASH_CTRL_SIZE) {
            *offset = address - bases[i];
            return &flash[i];
        }
    }
    return NULL;
}

bool miband_flash_read(MiBandFlash banks[MIBAND_FLASH_COUNT], hwaddr address,
                        unsigned size, uint64_t *result)
{
    hwaddr offset;
    MiBandFlash *flash = miband_flash_bank(banks, address, &offset);
    uint8_t byte;
    if (!flash || size != sizeof(uint32_t)) return false;
    unsigned rx_len = (flash->command >> 13) & 7;
    bool response = flash->endpoint.response(flash->endpoint.opaque,
                    flash->command & 0xff, flash->rx_index, rx_len, &byte);
    if (offset == FLASH_FIFO_STATUS) {
        *result = FLASH_TX_FREE_SLOTS << FLASH_TX_FREE_SHIFT;
        if (response && flash->rx_index < rx_len)
            *result |= (rx_len - flash->rx_index) << FLASH_RX_COUNT_SHIFT;
        return true;
    }
    if (offset == FLASH_RX_DATA && response) {
        unsigned index = flash->rx_index++;
        *result = byte;
        if (flash->trace_current_command)
            qemu_log_mask(LOG_UNIMP, "miband flash rx cmd=%08x index=%u -> %02" PRIx64 "\n",
                          flash->command, index, *result);
        return true;
    }
    return false;
}

bool miband_flash_write(MiBandFlash banks[MIBAND_FLASH_COUNT], hwaddr address,
                         unsigned size, uint64_t value)
{
    hwaddr offset;
    MiBandFlash *flash = miband_flash_bank(banks, address, &offset);
    if (flash && offset == 0x18 && size == 4 && (value & 2)) {
        flash->tx_stream_active = false;
        if (value == 2) flash->tx_count = 0;
        else if (flash->endpoint.commit_tx)
            flash->endpoint.commit_tx(flash->endpoint.opaque, flash->command & 0xff,
                                       flash->command >> 8, flash->tx_fifo, flash->tx_count);
    }
    if (flash && offset == 0x08 && size <= 4 &&
        flash->tx_count + size <= sizeof(flash->tx_fifo)) {
        unsigned start = flash->tx_count;
        for (unsigned i = 0; i < size; i++) {
            flash->tx_fifo[flash->tx_count++] = value >> (i * 8);
        }
        /* NAND may prefill a FIFO burst before PROGRAM LOAD, then stream
         * the remaining page while that command is active. The initial
         * bytes were consumed by endpoint.command(); deliver only this
         * new suffix. PROGRAM EXECUTE still owns the actual array write. */
        if (flash->tx_stream_active && flash->endpoint.program_buffer) {
            uint8_t *target = flash->endpoint.program_buffer(
                flash->endpoint.opaque, (flash->command >> 8) + start, size);
            if (target) {
                memcpy(target, flash->tx_fifo + start, size);
            }
        }
    }
    if (!flash || size != sizeof(uint32_t)) return false;
    if (offset == FLASH_COMMAND) {
        flash->command = value;
        flash->trace_current_command = true;
        if (flash->endpoint.is_poll(value & 0xff)) {
            uint64_t count = ++flash->status_poll_count;
            flash->trace_current_command = count <= 32 || (count & (count - 1)) == 0;
        } else if ((value & 0xff) == 0x03 || (value & 0xff) == 0x0b ||
                   (value & 0xff) == 0x3b || (value & 0xff) == 0x6b ||
                   (value & 0xff) == 0xbb || (value & 0xff) == 0xeb) {
            uint64_t count = ++flash->read_cache_count;
            flash->trace_current_command = count <= 32 || (count & (count - 1)) == 0;
        }
        unsigned transfer = flash->endpoint.command(flash->endpoint.opaque,
                              value & 0xff, value >> 8, flash->tx_fifo, flash->tx_count);
        flash->tx_stream_active = (transfer & MB_FLASH_DMA_WRITE) != 0;
        if (transfer & MB_FLASH_CACHE_REFRESH) {
            memset(flash->mapped_cache, 0xff, flash->mapped_cache_size);
            memcpy(flash->mapped_cache, flash->endpoint.read_cache(flash->endpoint.opaque),
                   flash->endpoint.cache_size);
        }
        if (flash->trace_current_command)
            qemu_log_mask(LOG_UNIMP, "miband flash command %08" PRIx64
                          " status_poll=%" PRIu64 " read_cache=%" PRIu64 "\n",
                          value, flash->status_poll_count, flash->read_cache_count);
        if ((transfer & MB_FLASH_DMA_READ) && flash->dma.read)
            flash->dma.read(flash->dma.opaque, flash);
        if ((transfer & MB_FLASH_DMA_WRITE) && flash->dma.write)
            flash->dma.write(flash->dma.opaque, flash);
        return true;
    }
    if (offset == FLASH_CONTROL && (value & 1)) {
        flash->rx_index = 0;
        if (flash->trace_current_command)
            qemu_log_mask(LOG_UNIMP, "miband flash control %08" PRIx64 "\n", value);
    }
    return false;
}
