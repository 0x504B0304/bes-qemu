#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "best1502p_spi.h"
#include "best1502p_nor.h"

#define SPI_FIFO_STATUS 0x0c

/* The reference boot driver extracts bits [13:9] from this register as the number
 * of words that can be written to the TX FIFO.  A second helper extracts
 * bits [8:4] as the RX/FIFO level.  Four words is the smallest value that
 * lets the observed 4-byte boot transaction complete. */
#define SPI_FIFO_READY ((4u << 9) | (4u << 4))

void best1502p_spi_init(Best1502pSpi *spi, hwaddr base)
{
    memset(spi, 0, sizeof(*spi));
    spi->base = base;
}

void best1502p_spi_set_nor(Best1502pSpi *spi, Best1502pNor *nor)
{
    spi->nor = nor;
}

static void spi_execute(Best1502pSpi *spi, uint8_t opcode)
{
    unsigned rx_capacity = sizeof(spi->rx_fifo);

    spi->rx_count = 0;
    spi->rx_index = 0;
    /* The boot ROM uses the controller's dual-die mode for one JEDEC probe:
     * mode 0x84000 clocks six nibble-encoded bytes.  The follow-up table scan
     * uses the normal three-byte command.  Reflect that wire length at the
     * package boundary so the NOR model can keep its protocol semantics
     * independent from controller register details. */
    if (opcode == 0x9f && spi->regs[0x14 >> 2] == 0x00084000) {
        rx_capacity = 6;
    } else if (opcode == 0x9f) {
        rx_capacity = 3;
    }
    if (spi->nor) {
        spi->rx_count = best1502p_nor_execute(
            spi->nor, opcode, spi->tx_fifo, spi->tx_count,
            spi->rx_fifo, rx_capacity);
    }
    spi->tx_count = 0;
}

static void spi_trace(Best1502pSpi *spi, const char *kind, hwaddr offset,
                      uint64_t value)
{
    if (spi->trace_events++ >= 1024) {
        return;
    }
    uint32_t pc = current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;

    qemu_log_mask(LOG_UNIMP,
                  "best1502p SPI %s %08" HWADDR_PRIx " value=%08" PRIx64
                  " pc=%08" PRIx32 "\n", kind, spi->base + offset, value,
                  pc);
}

bool best1502p_spi_read(Best1502pSpi *spi, hwaddr offset,
                        unsigned size, uint64_t *result)
{
    if ((size != 1 && size != 2 && size != sizeof(uint32_t)) ||
        offset >= BEST1502P_SPI0_SIZE || offset + size > BEST1502P_SPI0_SIZE) {
        return false;
    }
    if (size == sizeof(uint32_t) && offset == SPI_FIFO_STATUS) {
        unsigned ready = MIN(spi->rx_count - MIN(spi->rx_index,
                                                  spi->rx_count), 31u);
        *result = SPI_FIFO_READY | (ready << 4);
    } else if (offset == 0x10 && spi->rx_index < spi->rx_count) {
        *result = spi->rx_fifo[spi->rx_index++];
    } else {
        uint32_t word = spi->regs[offset >> 2];
        unsigned shift = (offset & 3) * 8;
        uint32_t mask = size == 4 ? UINT32_MAX : ((1u << (size * 8)) - 1);
        *result = (word >> shift) & mask;
    }
    spi_trace(spi, "read", offset, *result);
    return true;
}

bool best1502p_spi_write(Best1502pSpi *spi, hwaddr offset,
                         unsigned size, uint64_t value)
{
    if ((size != 1 && size != 2 && size != sizeof(uint32_t)) ||
        offset >= BEST1502P_SPI0_SIZE || offset + size > BEST1502P_SPI0_SIZE) {
        return false;
    }
    uint32_t *word = &spi->regs[offset >> 2];
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = size == 4 ? UINT32_MAX : ((1u << (size * 8)) - 1);
    *word = (*word & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
    if (offset == 0x08) {
        for (unsigned i = 0; i < size && spi->tx_count < sizeof(spi->tx_fifo); i++) {
            spi->tx_fifo[spi->tx_count++] = (uint8_t)(value >> (i * 8));
        }
    } else if (offset == 0x00 && size == sizeof(uint32_t)) {
        spi_execute(spi, (uint8_t)value);
    } else if (offset == 0x04 && (value & 1)) {
        spi->rx_index = 0;
    }
    spi_trace(spi, "write", offset, value);
    return true;
}
