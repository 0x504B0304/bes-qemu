#include "qemu/osdep.h"
#include "best1502p_nor.h"

/* reference's boot banner reports a 16 MiB FLASH_SIZE.  Its built-in table maps the
 * GD25Q64-compatible entry C8 60 18 to a 16 MiB die (two dies expose a
 * 32 MiB aggregate), while C8 60 19 would make the firmware compute 64 MiB
 * and reject the package during norflash_api_init. */
#define BEST1502P_DEFAULT_JEDEC0 0xc8
#define BEST1502P_DEFAULT_JEDEC1 0x60
#define BEST1502P_DEFAULT_JEDEC2 0x18

void best1502p_nor_init(Best1502pNor *nor, uint8_t *backing, uint32_t size)
{
    memset(nor, 0, sizeof(*nor));
    nor->backing = backing;
    nor->size = size;
    nor->jedec_id[0] = BEST1502P_DEFAULT_JEDEC0;
    nor->jedec_id[1] = BEST1502P_DEFAULT_JEDEC1;
    nor->jedec_id[2] = BEST1502P_DEFAULT_JEDEC2;
    best1502p_nor_reset(nor);
}

void best1502p_nor_reset(Best1502pNor *nor)
{
    nor->status = 0;
    nor->write_enable = false;
}

static unsigned nor_copy(Best1502pNor *nor, uint32_t address,
                         uint8_t *rx, unsigned count)
{
    unsigned available;

    if (!nor->backing || address >= nor->size) {
        memset(rx, 0xff, count);
        return count;
    }
    available = nor->size - address;
    count = MIN(count, available);
    memcpy(rx, nor->backing + address, count);
    return count;
}

unsigned best1502p_nor_execute(Best1502pNor *nor, uint8_t opcode,
                               const uint8_t *tx, unsigned tx_count,
                               uint8_t *rx, unsigned rx_capacity)
{
    uint32_t address;
    unsigned count;

    switch (opcode) {
    case 0x06: /* WREN */
        nor->write_enable = true;
        return 0;
    case 0x04: /* WRDI */
        nor->write_enable = false;
        return 0;
    case 0x05: /* RDSR */
    case 0x35: /* RDSR2 */
        if (rx_capacity) {
            rx[0] = nor->status;
            return 1;
        }
        return 0;
    case 0x9f: /* JEDEC ID */
        /* reference uses the dual-die read helper for this command.  It clocks
         * six wire bytes and reconstructs three logical bytes by taking one
         * nibble from each die.  Encode each nibble on a full byte (the
         * silicon returns 0xNN for a nibble N) so the firmware's conversion
         * produces the configured JEDEC ID. */
        if (rx_capacity >= 6) {
            for (unsigned i = 0; i < 3; i++) {
                rx[i * 2] = (nor->jedec_id[i] >> 4) * 0x11;
                rx[i * 2 + 1] = (nor->jedec_id[i] & 0x0f) * 0x11;
            }
            return 6;
        }
        count = MIN(rx_capacity, 3u);
        memcpy(rx, nor->jedec_id, count);
        return count;
    case 0x03: /* READ */
    case 0x0b: /* FAST READ */
    case 0x3b: /* DUAL OUT */
    case 0x6b: /* QUAD OUT */
    case 0xbb: /* DUAL I/O */
    case 0xeb: /* QUAD I/O */
        if (tx_count < 3) {
            memset(rx, 0xff, rx_capacity);
            return rx_capacity;
        }
        address = ((uint32_t)tx[0] << 16) |
                  ((uint32_t)tx[1] << 8) | tx[2];
        count = nor_copy(nor, address, rx, rx_capacity);
        return count;
    case 0x66: /* RESET ENABLE */
    case 0x99: /* RESET */
    case 0xab: /* RELEASE POWER DOWN */
    case 0xe9: /* EXIT QPI */
        if (opcode == 0x99) {
            best1502p_nor_reset(nor);
        }
        return 0;
    default:
        /* Unknown commands complete with an empty response.  This preserves
         * the controller's ready/FIFO contract while keeping unsupported
         * package features observable at the controller boundary. */
        return 0;
    }
}
