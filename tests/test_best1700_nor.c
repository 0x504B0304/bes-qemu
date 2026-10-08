/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "best1700_nor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t now;
static uint64_t changed_start, changed_length;

static Best1700NorResult command(Best1700Nor *s, uint8_t op, uint32_t address,
    const uint8_t *tx, unsigned tx_len, uint8_t *rx, unsigned rx_len)
{
    return best1700_nor_command(s, op, address, tx, tx_len, rx, rx_len,
        now, true, &changed_start, &changed_length);
}

static void ok(Best1700Nor *s, uint8_t op)
{
    assert(command(s, op, 0, NULL, 0, NULL, 0) == BEST1700_NOR_OK);
}

static void status(Best1700Nor *s, uint8_t sr1, uint8_t sr2)
{
    uint8_t tx[2] = {sr1, sr2};

    ok(s, 0x06);
    assert(command(s, 0x01, 0, tx, 2, NULL, 0) == BEST1700_NOR_OK);
    now += 10000;
}

static void program_check(Best1700Nor *s, uint32_t address, bool protected)
{
    uint8_t before, after, zero = 0;
    uint64_t start, length;
    Best1700NorResult expected = protected ? BEST1700_NOR_PROTECTED : BEST1700_NOR_OK;

    assert(command(s, 0x03, address, NULL, 0, &before, 1) == BEST1700_NOR_OK);
    ok(s, 0x06);
    assert(best1700_nor_command(s, 0x02, address, &zero, 1, NULL, 0,
        now, false, &start, &length) == expected);
    assert(s->wel && start == 0 && length == 0);
    assert(command(s, 0x03, address, NULL, 0, &after, 1) == BEST1700_NOR_OK);
    assert(after == before);
    assert(command(s, 0x02, address, &zero, 1, NULL, 0) == expected);
    assert(s->wel == protected);
    if (protected) {
        assert(changed_start == 0 && changed_length == 0);
    }
    now += 50000;
    assert(command(s, 0x03, address, NULL, 0, &after, 1) == BEST1700_NOR_OK);
    assert(after == (protected ? before : 0));
}

static void test_protection(uint8_t *backing, unsigned backing_size)
{
    /* Explicit 64-Mbit protection lengths, including SEC's clamped rows. */
    const uint32_t lengths[2][8] = {
        {0, 128 << 10, 256 << 10, 512 << 10,
         1 << 20, 2 << 20, 4 << 20, 8 << 20},
        {0, 4 << 10, 8 << 10, 16 << 10,
         32 << 10, 32 << 10, 32 << 10, 8 << 20},
    };
    Best1700Nor die;
    uint8_t tx[2] = {0x12, 0x34};

    memset(backing, 0xff, backing_size);
    assert(best1700_nor_init(&die, backing, backing_size, 0, 1, 0xc86017, 1));
    for (unsigned sec = 0; sec < 2; sec++) {
        for (unsigned bp = 0; bp < 8; bp++) {
            for (unsigned bottom = 0; bottom < 2; bottom++) {
                for (unsigned cmp = 0; cmp < 2; cmp++) {
                    uint32_t count = lengths[sec][bp];
                    uint32_t first = bottom ? 0 : die.capacity - count;
                    uint32_t last = first + count;
                    uint32_t probes[] = {0, die.capacity - 1,
                        first, first ? first - 1 : 0,
                        last, last ? last - 1 : 0};

                    status(&die, (sec << 6) | (bottom << 5) | (bp << 2), cmp << 6);
                    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
                        uint32_t address = probes[i];
                        bool protected = address >= first && address < last;

                        if (address < die.capacity) {
                            program_check(&die, address, protected != !!cmp);
                        }
                    }
                }
            }
        }
    }
    /* Firmware status 08/40 leaves exactly the top 256 KiB writable. */
    status(&die, 0x08, 0x40);
    program_check(&die, 0x7bffff, true);
    program_check(&die, 0x7c0000, false);
    memset(backing + 0x7c7000, 0, 4096);
    ok(&die, 0x06);
    assert(command(&die, 0x20, 0x7c7000, NULL, 0, NULL, 0) == BEST1700_NOR_OK);
    assert(changed_start == 0x7c7000 && changed_length == 4096 && !die.wel);
    for (unsigned i = 0; i < 4096; i++) {
        assert(backing[0x7c7000 + i] == 0xff);
    }
    now += 200000;
    /* Erase checks the entire aligned range, not only the command address. */
    status(&die, 0x44, 0);
    memset(backing + 0x7f0000, 0, 65536);
    ok(&die, 0x06);
    assert(command(&die, 0xd8, 0x7f0000, NULL, 0, NULL, 0) == BEST1700_NOR_PROTECTED);
    assert(die.wel && changed_length == 0);
    for (unsigned i = 0; i < 65536; i++) {
        assert(backing[0x7f0000 + i] == 0);
    }
    status(&die, 0x44, 0x40);
    ok(&die, 0x06);
    assert(command(&die, 0x52, 0x7fffff, NULL, 0, NULL, 0) == BEST1700_NOR_PROTECTED);
    assert(die.wel && changed_length == 0 && backing[0x7f8000] == 0);
    assert(command(&die, 0x20, 0x7fffff, NULL, 0, NULL, 0) == BEST1700_NOR_OK);
    now += 200000;
    /* A final-page wrap stays in the allowed sector, including at capacity. */
    ok(&die, 0x06);
    assert(command(&die, 0x02, 0x7fffff, tx, 2, NULL, 0) == BEST1700_NOR_OK);
    assert(backing[0x7fffff] == 0x12 && backing[0x7fff00] == 0x34);
    assert(backing[0x7fefff] == 0 && !die.wel);
    now += 50000;
    status(&die, 0x44, 0);
    ok(&die, 0x06);
    assert(command(&die, 0x02, 0x7fffff, tx, 2, NULL, 0) == BEST1700_NOR_PROTECTED);
    assert(backing[0x7fffff] == 0x12 && backing[0x7fff00] == 0x34 && die.wel);
    /* Any protected byte blocks both chip-erase opcodes. */
    for (unsigned opcode = 0x60; opcode <= 0xc7; opcode += 0x67) {
        assert(command(&die, opcode, 0, NULL, 0, NULL, 0) == BEST1700_NOR_PROTECTED);
        assert(die.wel && backing[0x7fffff] == 0x12 && changed_length == 0);
    }
    status(&die, 0, 0x40); /* Complement of no protection protects everything. */
    program_check(&die, 0, true);
    program_check(&die, 0x7fffff, true);
    status(&die, 0x1c, 0x40); /* Complement of all protection permits chip erase. */
    ok(&die, 0x06);
    assert(command(&die, 0xc7, 0xdeadbeef, NULL, 0, NULL, 0) == BEST1700_NOR_OK);
    assert(changed_start == 0 && changed_length == die.capacity && !die.wel);
    now += 200000;
    for (unsigned i = 0; i < die.capacity; i++) {
        assert(backing[i] == 0xff);
    }
    /* Unconfirmed identities keep the documented conservative policy. */
    assert(best1700_nor_init(&die, backing, backing_size, 0, 1, 0xc86018, 1));
    status(&die, 0x1c, 0x40);
    program_check(&die, 0, true);
    status(&die, 0, 0);
    program_check(&die, 0, false);
}

static void test_security(uint8_t *backing, unsigned backing_size)
{
    Best1700Nor dies[2];
    uint8_t rx[32], tx = 0;
    uint64_t start, length;
    const uint32_t invalid[] = {0, 0xfff, 0x1400, 0x2000 - 1,
                                0x3400, 0x4000, 0x1003000, UINT32_MAX};

    memset(backing, 0, backing_size);
    for (unsigned die = 0; die < 2; die++) {
        assert(best1700_nor_init(&dies[die], backing, backing_size,
                                die, 2, 0xc86017, die + 1));
        for (unsigned bank = 1; bank <= 3; bank++) {
            for (unsigned offset = 0; offset <= 1024 - sizeof(rx); offset += sizeof(rx)) {
                assert(command(&dies[die], 0x48, bank * 0x1000 + offset,
                               NULL, 0, rx, sizeof(rx)) == BEST1700_NOR_OK);
                for (unsigned i = 0; i < sizeof(rx); i++) {
                    assert(rx[i] == 0xff);
                }
                assert(!dies[die].wel && changed_start == 0 && changed_length == 0);
            }
        }
    }
    /* Distinct fixture bytes prove bank/die selection and storage lifetime. */
    dies[0].security[2][0] = 0x12;
    dies[0].security[2][1023] = 0x34;
    dies[1].security[2][0] = 0x56;
    ok(&dies[0], 0x06);
    memset(rx, 0xa5, sizeof(rx));
    assert(best1700_nor_command(&dies[0], 0x48, 0x3000, NULL, 0,
        rx, sizeof(rx), now, false, &start, &length) == BEST1700_NOR_OK);
    assert(dies[0].wel && start == 0 && length == 0);
    for (unsigned i = 0; i < sizeof(rx); i++) {
        assert(rx[i] == 0xa5);
    }
    assert(command(&dies[0], 0x48, 0x3000, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0x12 && dies[0].wel);
    assert(command(&dies[1], 0x48, 0x3000, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0x56);
    assert(command(&dies[0], 0x48, 0x2000, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0xff);
    assert(command(&dies[0], 0x48, 0x33ff, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0x34);
    memset(rx, 0xa5, sizeof(rx));
    assert(command(&dies[0], 0x48, 0x33ff, NULL, 0, rx, 2) == BEST1700_NOR_INVALID);
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(command(&dies[0], 0x48, invalid[i], NULL, 0, rx, 1) == BEST1700_NOR_INVALID);
    }
    assert(command(&dies[0], 0x48, 0x3000, NULL, 0, NULL, 0) == BEST1700_NOR_INVALID);
    assert(command(&dies[0], 0x48, 0x3000, &tx, 1, rx, 1) == BEST1700_NOR_INVALID);
    assert(command(&dies[0], 0x42, 0x3000, &tx, 1, NULL, 0) == BEST1700_NOR_UNSUPPORTED);
    assert(command(&dies[0], 0x44, 0x3000, NULL, 0, NULL, 0) == BEST1700_NOR_UNSUPPORTED);
    for (unsigned i = 0; i < sizeof(rx); i++) {
        assert(rx[i] == 0xa5);
    }
    assert(changed_start == 0 && changed_length == 0);
    assert(dies[0].wel);
    ok(&dies[0], 0x66); ok(&dies[0], 0x99);
    assert(command(&dies[0], 0x48, 0x3000, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0x12 && !dies[0].wel);
    for (unsigned i = 0; i < backing_size; i++) {
        assert(backing[i] == 0);
    }
    assert(best1700_nor_init(&dies[0], backing, backing_size, 0, 1, 0xc86018, 1));
    assert(command(&dies[0], 0x48, 0x3000, NULL, 0, rx, 1) == BEST1700_NOR_UNSUPPORTED);
}

int main(void)
{
    const unsigned size = 16 * 1024 * 1024;
    uint8_t *backing = malloc(size);
    Best1700Nor dies[2];
    uint8_t rx[32], tx[4];
    uint64_t start, length;

    assert(backing);
    memset(backing, 0xff, size);
    for (unsigned i = 0; i < 2; i++) {
        assert(best1700_nor_init(&dies[i], backing, size, i, 2, 0xc86017, i + 1));
        assert(command(&dies[i], 0x9f, 0, NULL, 0, rx, 3) == BEST1700_NOR_OK);
        assert(!memcmp(rx, "\xc8\x60\x17", 3));
        memset(tx, 0, sizeof(tx));
        assert(command(&dies[i], 0x4b, 0, tx, 4, rx, 18) == BEST1700_NOR_OK);
        assert(rx[0] == i + 1);
        assert(rx[8] == ((i + 1) ^ 0x31));
    }
    /* Programming each die reproduces the controller's nibble wire order. */
    tx[0] = 0xa5;
    assert(command(&dies[0], 0x02, 8, tx, 1, NULL, 0) == BEST1700_NOR_PROTECTED);
    assert(backing[16] == 0xff);
    ok(&dies[0], 0x06);
    assert(command(&dies[0], 0x05, 0, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 2);
    assert(best1700_nor_command(&dies[0], 0x02, 8, tx, 1, NULL, 0,
        now, false, &start, &length) == BEST1700_NOR_OK);
    assert(backing[16] == 0xff && dies[0].wel);
    assert(command(&dies[0], 0x02, 8, tx, 1, NULL, 0) == BEST1700_NOR_OK);
    assert(backing[16] == 0xfa && backing[17] == 0xf5);
    assert(changed_start == 0 && changed_length == 512);
    assert(command(&dies[0], 0x05, 0, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 1);
    assert(command(&dies[0], 0x06, 0, NULL, 0, NULL, 0) == BEST1700_NOR_BUSY);
    now += 50000;
    ok(&dies[1], 0x06);
    tx[0] = 0x3c;
    assert(command(&dies[1], 0x32, 8, tx, 1, NULL, 0) == BEST1700_NOR_OK);
    assert(backing[16] == 0x3a && backing[17] == 0xc5);
    now += 50000;
    assert(command(&dies[0], 0x03, 8, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0xa5);
    assert(command(&dies[1], 0x03, 8, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0x3c);
    /* NOR programming cannot set cleared bits, and wraps within a die page. */
    tx[0] = 0xff;
    ok(&dies[0], 0x06);
    assert(command(&dies[0], 0x02, 8, tx, 1, NULL, 0) == BEST1700_NOR_OK);
    assert(backing[16] == 0x3a && backing[17] == 0xc5);
    now += 50000;
    tx[0] = 0x12; tx[1] = 0x34;
    ok(&dies[0], 0x06);
    assert(command(&dies[0], 0x02, 255, tx, 2, NULL, 0) == BEST1700_NOR_OK);
    now += 50000;
    assert(command(&dies[0], 0x03, 255, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0x12);
    assert(command(&dies[0], 0x03, 0, NULL, 0, rx, 1) == BEST1700_NOR_OK);
    assert(rx[0] == 0x34);
    ok(&dies[0], 0x06);
    assert(command(&dies[0], 0x20, 10, NULL, 0, NULL, 0) == BEST1700_NOR_OK);
    assert(backing[16] == 0x3f && backing[17] == 0xcf);
    assert(changed_length == 8192);
    now += 200000;
    /* Nonvolatile QE survives reset; volatile writes do not. */
    tx[0] = 2;
    ok(&dies[0], 0x06);
    assert(command(&dies[0], 0x31, 0, tx, 1, NULL, 0) == BEST1700_NOR_OK);
    now += 10000;
    ok(&dies[0], 0x66); ok(&dies[0], 0x99);
    assert(dies[0].status[1] == 2 && !dies[0].wel);
    ok(&dies[0], 0x50);
    tx[0] = 0;
    assert(command(&dies[0], 0x31, 0, tx, 1, NULL, 0) == BEST1700_NOR_OK);
    now += 10000;
    assert(dies[0].status[1] == 0);
    ok(&dies[0], 0x66); ok(&dies[0], 0x99);
    assert(dies[0].status[1] == 2);
    assert(command(&dies[0], 0x99, 0, NULL, 0, NULL, 0) == BEST1700_NOR_INVALID);
    assert(command(&dies[0], 0x5a, 0, tx, 4, rx, 8) == BEST1700_NOR_UNSUPPORTED);
    assert(command(&dies[0], 0x03, dies[0].capacity, NULL, 0, rx, 1) == BEST1700_NOR_INVALID);
    ok(&dies[0], 0xb9);
    assert(command(&dies[0], 0x9f, 0, NULL, 0, rx, 3) == BEST1700_NOR_BUSY);
    ok(&dies[0], 0xab);
    assert(command(&dies[0], 0x9f, 0, NULL, 0, rx, 3) == BEST1700_NOR_OK);
    test_protection(backing, size);
    test_security(backing, size);
    free(backing);
    puts("BEST1700 NOR die: identity, state, dual backing, program, erase and rejection passed");
    return 0;
}
