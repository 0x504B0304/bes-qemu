# Simulated NOR dies

This package model owns JEDEC/UID responses, write-enable state, volatile and
nonvolatile status registers, reset, power-down and address-mode state. The
default JEDEC identity `C8 60 17` describes an 8 MiB compatible simulated die
accepted by the firmware's device table. It is not an identification of the
physical watch's flash. `jedec-id`, `die-count`, `uid0` and `uid1` are configurable
QOM properties, supplied by the board's package profile in normal launches.

The BEST1700 package schema requires `nor` for SYS flash and accepts an
independent optional `bth_nor` selection. Both contain `size`, `die_count`,
`jedec_id`, `uid0` and `uid1`; each selection currently covers 16 MiB and its
JEDEC capacity byte must match its die count. The launcher sends SYS values
as `nor-*` machine properties and BTH values as `bth-nor-*`, then verifies
the selected values through QMP before execution. Omitting `bth_nor` emits
no BTH overrides and never copies the SYS identity into the BTH selection.

The current compatibility profile selects two SYS dies with `C8 60 17`
and one BTH die with `C8 60 18`, with distinct synthetic UID seeds. These
selections describe emulator compatibility, not a physical identification.

Each 18-byte emulator UID derives from its configured 64-bit seed. Byte `i`
is seed byte `i % 8` in little-endian order, XOR `0x31 * (i / 8)`. Two dies
borrow one aggregate XIP backing; neither a firmware copy nor a backing file
is allocated by this model. The controller handles the nibble wire protocol.

Supported commands include JEDEC ID, unique ID, three status registers,
write-enable/disable, volatile-status write-enable, status writes, reset
enable/reset, power-down/release, QPI entry/exit and 3/4-byte mode state,
burst-wrap configuration, ordinary/fast/dual/quad array reads, page program,
4/32/64 KiB erase and chip erase. Programs only clear bits and wrap within a
256-byte die page. Erases set bits to one. Writes require WEL, clear it when
accepted, and expose WIP for a bounded simulated interval. Array mutations
commit atomically when accepted, rather than modeling partial power-loss
writes. Nonvolatile status survives reset during the process; volatile status
returns to the stored nonvolatile value.

For configured identity `C8 60 17`, array protection uses the GD25LQ64
compatible BP/TB/SEC/CMP table. SR1 bits 4:2 select BP, bit 5 selects the
bottom instead of the top, and bit 6 selects sector granularity. SR2 bit 6
complements the selected protected interval.

| BP | SEC=0 protected length | SEC=1 protected length |
| --- | --- | --- |
| 0 | None | None |
| 1 | 128 KiB | 4 KiB |
| 2 | 256 KiB | 8 KiB |
| 3 | 512 KiB | 16 KiB |
| 4 | 1 MiB | 32 KiB |
| 5 | 2 MiB | 32 KiB |
| 6 | 4 MiB | 32 KiB |
| 7 | All 8 MiB | All 8 MiB |

These field meanings and range sizes follow the public
[flashrom GD25LQ64 register map](https://github.com/flashrom/flashrom/blob/main/flashchips/gigadevice.c)
and its [SPI25 range decoder](https://github.com/flashrom/flashrom/blob/main/writeprotect_ranges.c).
SR1/SR2 `08/40` protects physical die addresses `[0, 0x7c0000)` and leaves
the top 256 KiB writable. Each die checks its own physical addresses before
the controller combines their nibble streams. Protection must be clear for
every byte touched by a wrapped page program or the full aligned erase
range. Chip erase requires no protected bytes. Rejected operations preserve
the array and WEL; validation-only calls never mutate state.

Other configurable JEDEC identities retain conservative protection: any
BP/TB/SEC/CMP bit rejects array writes. Their part-specific protection tables
are not inferred from capacity. Hardware WP/SRP register locking, SFDP,
security-register programming/erase/locking, suspend/resume, endurance and power-loss behavior
remain unimplemented. Unsupported commands are rejected.
The caller validates every participating die before committing a multi-die
operation, then invalidates XIP translations through the machine callback.

For `C8 60 17`, Read Security Registers (`48h`) accesses three separate 1 KiB
banks at die addresses `0x1000`, `0x2000`, and `0x3000`. Geometry follows the
public [ufprog GD25LE64C/GD25LQ64C description](https://github.com/hackpascal/ufprog/blob/master/flash/spi-nor/vendor-gigadevice.c).
Each die owns independent storage initialized to the erased value `ff`;
it never reads or modifies the main array or invents factory keys. Reads
preserve WEL and bank contents survive reset. Only nonempty reads entirely
within one bank are supported. Reserved address bits, out-of-bank ranges,
and cross-bank wrap are rejected rather than assigned unconfirmed behavior.
Other JEDEC identities do not inherit this geometry automatically.

The controller accepts `48h` with the normal address/BLKSIZE framing; the
dummy clock phase is implicit and consumes no TX FIFO bytes. In dual mode
the same security address is sent to each die, with received bytes nibble
interleaved as for other non-array commands. Main-array address halving and
odd-byte skipping do not apply. Extended `48h` framing is unsupported.

Persistence is currently limited to the running emulator process. Array and
status updates are not written back to the input firmware files, and no
external NOR persistence store is provided. A machine reset preserves the
borrowed array and nonvolatile status; restarting QEMU restores input data.

`tests.test_best1700_nor_native` compiles the standalone die test with address
and undefined-behavior sanitizers. It verifies identity, WEL/WIP, reset and
status persistence, die isolation in the shared backing, program/erase, page
wrap, all 64 BP/TB/SEC/CMP combinations for the compatible die, boundary and
overlapping-range protection, chip erase and explicit transaction rejection.
