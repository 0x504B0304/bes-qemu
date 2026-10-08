/* Stateful simulated NOR die; no firmware image is owned by this model.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "best1700_nor.h"
#include <string.h>

void best1700_nor_reset(Best1700Nor *s)
{
    memcpy(s->status, s->nonvolatile_status, sizeof(s->status));
    s->wel = s->volatile_wel = s->reset_armed = false;
    s->asleep = s->address4 = s->qpi = false;
    s->wrap = 0;
    s->busy_until = 0;
}

bool best1700_nor_init(Best1700Nor *s, uint8_t *backing, uint64_t size,
                      unsigned index, unsigned count, uint32_t jedec,
                      uint64_t uid_seed)
{
    unsigned exponent = jedec & 0xff;

    if (!backing || (count != 1 && count != 2) || index >= count ||
        exponent < 18 || exponent > 26 || size < (UINT64_C(1) << exponent) * count) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->backing = backing;
    s->backing_size = size;
    s->capacity = 1u << exponent;
    s->index = index;
    s->die_count = count;
    s->jedec[0] = jedec >> 16;
    s->jedec[1] = jedec >> 8;
    s->jedec[2] = jedec;
    /* Security registers are separate nonvolatile storage, not XIP bytes. */
    memset(s->security, 0xff, sizeof(s->security));
    /* A deterministic configured emulator identity, never a silicon claim. */
    for (unsigned i = 0; i < sizeof(s->uid); i++) {
        s->uid[i] = (uid_seed >> (8 * (i % 8))) ^ (0x31 * (i / 8));
    }
    best1700_nor_reset(s);
    return true;
}

static uint8_t read_byte(const Best1700Nor *s, uint32_t address)
{
    const uint8_t *p = s->backing + (uint64_t)address * s->die_count;

    if (s->die_count == 1) {
        return *p;
    }
    if (s->index == 0) {
        return ((p[0] & 15) << 4) | (p[1] & 15);
    }
    return (p[0] & 0xf0) | (p[1] >> 4);
}

static void write_byte(Best1700Nor *s, uint32_t address, uint8_t value,
                       bool erase)
{
    uint8_t *p = s->backing + (uint64_t)address * s->die_count;

    if (s->die_count == 1) {
        *p = erase ? value : *p & value;
    } else if (s->index == 0) {
        uint8_t first = 0xf0 | (value >> 4);
        uint8_t second = 0xf0 | (value & 15);
        p[0] = erase ? (p[0] & 0xf0) | (first & 15) : p[0] & first;
        p[1] = erase ? (p[1] & 0xf0) | (second & 15) : p[1] & second;
    } else {
        uint8_t first = 15 | (value & 0xf0);
        uint8_t second = 15 | (value << 4);
        p[0] = erase ? (p[0] & 15) | (first & 0xf0) : p[0] & first;
        p[1] = erase ? (p[1] & 15) | (second & 0xf0) : p[1] & second;
    }
}

bool best1700_nor_memory_read(uint8_t opcode)
{
    return opcode == 0x03 || opcode == 0x0b || opcode == 0x3b ||
           opcode == 0x6b || opcode == 0xbb || opcode == 0xeb;
}

bool best1700_nor_memory_write(uint8_t opcode)
{
    return opcode == 0x02 || opcode == 0x32 || opcode == 0xa2;
}

static bool range_valid(const Best1700Nor *s, uint32_t address, unsigned length)
{
    return address <= s->capacity && length <= s->capacity - address;
}

static bool range_protected(const Best1700Nor *s, uint32_t address,
                             unsigned length)
{
    static const uint32_t block_bytes[8] = {
        0, 0x20000, 0x40000, 0x80000,
        0x100000, 0x200000, 0x400000, 0x800000,
    };
    static const uint32_t sector_bytes[8] = {
        0, 0x1000, 0x2000, 0x4000, 0x8000, 0x8000, 0x8000, 0x800000,
    };
    uint32_t bp, protected_bytes, low, high;
    uint64_t end = (uint64_t)address + length;

    if (!length) {
        return false;
    }
    if (s->jedec[0] != 0xc8 || s->jedec[1] != 0x60 ||
        s->jedec[2] != 0x17) {
        /* Protection tables are part-specific. Retain the conservative
         * policy for identities whose block geometry is not modeled.
         */
        return (s->status[0] & 0x7c) || (s->status[1] & 0x40);
    }
    /* GD25LQ64-compatible BP0..2, TB (BP3), SEC (BP4), and CMP.
     * The interval is expressed in physical die bytes, before dual-die
     * nibble interleaving. BP=0/7 mean none/all, even with SEC set.
     */
    bp = (s->status[0] >> 2) & 7;
    protected_bytes = (s->status[0] & 0x40) ? sector_bytes[bp] : block_bytes[bp];
    low = (s->status[0] & 0x20) ? 0 : s->capacity - protected_bytes;
    high = low + protected_bytes;
    if (s->status[1] & 0x40) {
        /* CMP protects the complement, including the empty/full cases. */
        return address < low || end > high;
    }
    return address < high && end > low;
}

Best1700NorResult best1700_nor_command(Best1700Nor *s, uint8_t opcode,
    uint32_t address, const uint8_t *tx, unsigned tx_len,
    uint8_t *rx, unsigned rx_len, int64_t now, bool apply,
    uint64_t *changed_start, uint64_t *changed_length)
{
    unsigned erase_size = 0;
    unsigned status_index;

    *changed_start = *changed_length = 0;
    if ((tx_len && !tx) || (rx_len && !rx)) {
        return BEST1700_NOR_INVALID;
    }
    if (s->asleep && opcode != 0xab && opcode != 0x66 && opcode != 0x99) {
        return BEST1700_NOR_BUSY;
    }
    if (now < s->busy_until && opcode != 0x05 && opcode != 0x35 &&
        opcode != 0x15 && opcode != 0x66 && opcode != 0x99) {
        return BEST1700_NOR_BUSY;
    }
    if (best1700_nor_memory_read(opcode)) {
        if (tx_len || !range_valid(s, address, rx_len)) {
            return BEST1700_NOR_INVALID;
        }
        if (apply) {
            for (unsigned i = 0; i < rx_len; i++) {
                rx[i] = read_byte(s, address + i);
            }
        }
        return BEST1700_NOR_OK;
    }
    if (best1700_nor_memory_write(opcode)) {
        if (rx_len || !tx_len || tx_len > 256 || address >= s->capacity) {
            return BEST1700_NOR_INVALID;
        }
        if (!s->wel) {
            return BEST1700_NOR_PROTECTED;
        }
        /* Validate every wrapped destination before changing any byte or
         * consuming WEL. A page program never spills into the next page.
         */
        for (unsigned i = 0; i < tx_len; i++) {
            uint32_t destination = (address & ~255u) | ((address + i) & 255);

            if (range_protected(s, destination, 1)) {
                return BEST1700_NOR_PROTECTED;
            }
        }
        if (apply) {
            uint32_t page = address & ~255u;
            for (unsigned i = 0; i < tx_len; i++) {
                write_byte(s, page | ((address + i) & 255), tx[i], false);
            }
            s->wel = false;
            s->busy_until = now + 50000;
            *changed_start = (uint64_t)page * s->die_count;
            *changed_length = 256 * s->die_count;
        }
        return BEST1700_NOR_OK;
    }
    switch (opcode) {
    case 0x20: erase_size = 4096; break;
    case 0x52: erase_size = 32768; break;
    case 0xd8: erase_size = 65536; break;
    case 0x60: case 0xc7: erase_size = s->capacity; break;
    }
    if (erase_size) {
        uint32_t start = (opcode == 0x60 || opcode == 0xc7) ? 0 :
                         address & ~(erase_size - 1);

        if (tx_len || rx_len || !range_valid(s, start, erase_size)) {
            return BEST1700_NOR_INVALID;
        }
        if (!s->wel || range_protected(s, start, erase_size)) {
            return BEST1700_NOR_PROTECTED;
        }
        if (apply) {
            for (unsigned i = 0; i < erase_size; i++) {
                write_byte(s, start + i, 0xff, true);
            }
            s->wel = false;
            s->busy_until = now + 200000;
            *changed_start = (uint64_t)start * s->die_count;
            *changed_length = (uint64_t)erase_size * s->die_count;
        }
        return BEST1700_NOR_OK;
    }
    switch (opcode) {
    case 0x48: {
        unsigned bank = address >> 12;
        unsigned offset = address & 0xfff;

        if (s->jedec[0] != 0xc8 || s->jedec[1] != 0x60 ||
            s->jedec[2] != 0x17) {
            return BEST1700_NOR_UNSUPPORTED;
        }
        /* Model only confirmed, in-bank GD25LQ64C transactions. Do not
         * alias reserved address bits or infer cross-bank wrap behavior.
         * The controller supplies the read dummy clocks, not TX payload.
         */
        if (tx_len || !rx_len || bank < 1 ||
            bank > BEST1700_NOR_SECURITY_BANKS ||
            offset >= BEST1700_NOR_SECURITY_SIZE ||
            rx_len > BEST1700_NOR_SECURITY_SIZE - offset) {
            return BEST1700_NOR_INVALID;
        }
        if (apply) {
            memcpy(rx, s->security[bank - 1] + offset, rx_len);
        }
        break;
    }
    case 0x9f:
        if (tx_len || !rx_len) {
            return BEST1700_NOR_INVALID;
        }
        if (apply) {
            for (unsigned i = 0; i < rx_len; i++) {
                rx[i] = i < 3 ? s->jedec[i] : 0xff;
            }
        }
        break;
    case 0x4b:
        if (tx_len != 4 || !rx_len) {
            return BEST1700_NOR_INVALID;
        }
        if (apply) {
            for (unsigned i = 0; i < rx_len; i++) {
                rx[i] = i < sizeof(s->uid) ? s->uid[i] : 0xff;
            }
        }
        break;
    case 0x05: case 0x35: case 0x15:
        if (tx_len || !rx_len) {
            return BEST1700_NOR_INVALID;
        }
        status_index = opcode == 0x05 ? 0 : opcode == 0x35 ? 1 : 2;
        if (apply) {
            uint8_t value = s->status[status_index];
            if (status_index == 0) {
                value = (value & ~3u) | (s->wel ? 2 : 0) |
                        (now < s->busy_until ? 1 : 0);
            }
            memset(rx, value, rx_len);
        }
        break;
    case 0x01: case 0x31: case 0x11:
        if (rx_len || !tx_len || tx_len > (opcode == 0x01 ? 2 : 1)) {
            return BEST1700_NOR_INVALID;
        }
        if (!s->wel && !s->volatile_wel) {
            return BEST1700_NOR_PROTECTED;
        }
        status_index = opcode == 0x01 ? 0 : opcode == 0x31 ? 1 : 2;
        if (apply) {
            memcpy(s->status + status_index, tx, tx_len);
            s->status[0] &= ~3u;
            if (s->wel && !s->volatile_wel) {
                memcpy(s->nonvolatile_status, s->status, sizeof(s->status));
            }
            s->wel = s->volatile_wel = false;
            s->busy_until = now + 10000;
        }
        break;
    case 0x77:
        if (tx_len != 1 || rx_len) {
            return BEST1700_NOR_INVALID;
        }
        if (apply) {
            s->wrap = tx[0];
        }
        break;
    case 0xff: /* Exit continuous-read/QPI framing, with optional dummy bytes. */
        if (rx_len || (tx_len != 0 && tx_len != 4)) {
            return BEST1700_NOR_INVALID;
        }
        if (apply) {
            s->qpi = false;
        }
        break;
    case 0x06: case 0x04: case 0x50: case 0x66: case 0x99:
    case 0xab: case 0xb9: case 0xb7: case 0xe9: case 0x38:
        if (tx_len || rx_len) {
            return BEST1700_NOR_INVALID;
        }
        if (opcode == 0x99 && !s->reset_armed) {
            return BEST1700_NOR_INVALID;
        }
        if (!apply) {
            break;
        }
        switch (opcode) {
        case 0x06: s->wel = true; break;
        case 0x04: s->wel = s->volatile_wel = false; break;
        case 0x50: s->volatile_wel = true; break;
        case 0x66: s->reset_armed = true; break;
        case 0x99: best1700_nor_reset(s); break;
        case 0xab: s->asleep = false; break;
        case 0xb9: s->asleep = true; break;
        case 0xb7: s->address4 = true; break;
        case 0xe9: s->address4 = false; break;
        case 0x38: s->qpi = true; break;
        }
        break;
    default:
        /* SFDP, security programming/erase and suspend are outside this
         * die model. Callers must report the rejected transaction. */
        return BEST1700_NOR_UNSUPPORTED;
    }
    return BEST1700_NOR_OK;
}

const char *best1700_nor_result_name(Best1700NorResult result)
{
    static const char *const names[] = {
        "ok", "unsupported command", "invalid transaction",
        "write protected or WEL unset", "die busy or asleep",
    };
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ?
        names[result] : "unknown error";
}
