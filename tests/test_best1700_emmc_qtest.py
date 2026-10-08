"""Guest SDHCI commands and real eMMC block I/O, without external firmware.

Run with MIBAND_BEST1700_QEMU pointing at the integrated QEMU executable.
Each test owns its raw disk and QEMU processes in system temporary storage.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


HOST = 0x40150000
IRQ = 26
CPU_BUFFER = 0x20160000
AXI_BUFFER = 0x21160000
CPU_DESCRIPTORS = 0x20162000
AXI_DESCRIPTORS = 0x21162000
BLOCK = 512
DISK_BYTES = 16 * 1024 * 1024
COMMAND_DONE = 1
TRANSFER_DONE = 2
READ_READY = 0x20
STATUS_ERROR = 0x8000
R1 = 0x1A
R1B = 0x1B
R2 = 0x09
R3 = 0x02
SEED_BLOCK = 8
SEED = bytes(((index * 37 + 13) ^ ((index // BLOCK) * 0x5B)) & 255
             for index in range(2 * BLOCK))


TCG_ASSEMBLY = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x203ff000
    .word Reset_Handler
    .rept 14
    .word Default_Handler
    .endr
    .org 0x400
.thumb_func
Reset_Handler:
    cpsid i
    ldr r4, =0x20001000
    ldr r5, =0x40150000
    movs r0, #0
    str r0, [r4]
    // The PHY is on a different CPU page from the standard SDHCI window.
    // These loads exercise TCG's IOTLB, which qtest MMIO accesses bypass.
    ldr r0, [r5, #0x800]
    str r0, [r4, #4]
    ldr r0, =0x1234abcd
    str r0, [r5, #0x800]
    ldr r0, [r5, #0x800]
    str r0, [r4, #8]
    ldr r0, [r5, #0x818]
    str r0, [r4, #12]
    ldr r0, =0x5678ef01
    str r0, [r5, #0x818]
    ldr r0, [r5, #0x818]
    str r0, [r4, #16]
    ldr r0, =0xa55a7890
    str r0, [r5, #8]
    ldr r0, [r5, #8]
    str r0, [r4, #20]
    ldr r0, [r5, #0x40]
    str r0, [r4, #24]
    movs r0, #1
    strb r0, [r5, #0x2f]
    ldr r0, [r5, #8]
    str r0, [r4, #28]
    dsb sy
    movs r0, #1
    str r0, [r4]
1:
    wfi
    b 1b
.thumb_func
Default_Handler:
    ldr r0, =0x20001000
    movs r1, #0xff
    str r1, [r0]
2:
    b 2b
    .ltorg
"""


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700EmmcQtest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='best1700-emmc-')
        self.addCleanup(self.temp.cleanup)
        self.disk = Path(self.temp.name) / 'card.raw'
        with self.disk.open('wb') as disk:
            disk.truncate(DISK_BYTES)
            disk.seek(SEED_BLOCK * BLOCK)
            disk.write(SEED)
        self.machine = None
        self.addCleanup(self.close_machine)
        self.start_machine()

    def close_machine(self):
        if self.machine is not None:
            self.machine.close()
            self.machine = None

    def start_machine(self):
        # eMMC is soldered on the board: its upstream QOM class deliberately
        # disallows -device. The machine attaches this named backend.
        self.machine = QtestMachine(
            os.environ['MIBAND_BEST1700_QEMU'],
            machine_options=',emmc-drive=emmc-test',
            extra_args=('-drive', f'if=none,id=emmc-test,file={self.disk},format=raw'))
        self.machine.command('irq_intercept_in /machine/core[0]')

    def write8(self, offset, value):
        self.machine.command(f'writeb {HOST + offset:#x} {value:#x}')

    def write16(self, offset, value):
        self.machine.command(f'writew {HOST + offset:#x} {value:#x}')

    def read16(self, offset):
        return int(self.machine.command(f'readw {HOST + offset:#x}'), 0)

    def configure_host(self, *, signal=True):
        q = self.machine
        self.write8(0x2F, 1)  # Software reset all host registers.
        self.assertEqual(self.read16(0x2E) >> 8, 0)
        self.assertTrue(q.read32(HOST + 0x24) & 0x10000, 'card must be inserted')
        self.write8(0x29, 0x0F)  # 3.3 V, power on.
        self.write16(0x2C, 1)  # Internal clock on.
        self.assertEqual(self.read16(0x2C) & 3, 3, 'internal clock must stabilize')
        self.write16(0x2C, 5)  # Card clock on as well.
        self.assertEqual(self.read16(0x2C) & 7, 7)
        self.write8(0x2E, 0x0E)
        q.write32(HOST + 0x34, 0xFFFFFFFF)  # Status enables, normal + error.
        q.write32(HOST + 0x38, 0xFFFFFFFF if signal else 0)
        q.write32(HOST + 0x30, 0xFFFFFFFF)

    def command(self, index, argument=0, response=R1, *, data=False, transfer=0):
        q = self.machine
        q.write32(HOST + 0x30, 0xFFFFFFFF)
        q.write32(HOST + 0x08, argument)
        flags = response | (0x20 if data else 0)
        q.write32(HOST + 0x0C, (index << 24) | (flags << 16) | transfer)
        status = q.read32(HOST + 0x30)
        self.assertEqual(status & 0xFFFF8000, 0,
                         f'CMD{index} arg={argument:#x}: SDHCI error {status:#x}')
        self.assertTrue(status & COMMAND_DONE, f'CMD{index} did not complete')
        return q.read32(HOST + 0x10)

    def initialize_card(self, *, signal=True):
        q = self.machine
        self.configure_host(signal=signal)
        self.command(0, response=0)
        ocr = self.command(1, 0x40FF8000, R3)
        self.assertTrue(ocr & 0x80000000, 'MMC CMD1 must report power-up complete')
        self.assertTrue(ocr & 0x00FF8000)
        self.block_addressed = bool(ocr & 0x40000000)
        self.command(2, response=R2)
        cid = tuple(q.read32(HOST + 0x10 + i * 4) for i in range(4))
        self.assertNotEqual(cid, (0, 0, 0, 0))
        self.command(3, 1 << 16)
        self.command(9, 1 << 16, R2)
        self.assertEqual((q.read32(HOST + 0x1C) >> 18) & 0xF, 4,
                         'CSD must describe MMC 4.x, not an SD card')
        self.command(7, 1 << 16, R1B)
        self.command(16, BLOCK)
        # CMD8 differs between SD and MMC; actual 512-byte EXT_CSD data is
        # essential evidence that the native eMMC protocol is attached.
        q.write32(HOST + 0x04, (1 << 16) | BLOCK)
        self.command(8, data=True, transfer=0x12)
        self.assertTrue(self.read16(0x30) & READ_READY)
        ext_csd = b''.join(struct.pack('<I', q.read32(HOST + 0x20))
                           for _ in range(BLOCK // 4))
        self.assertTrue(self.read16(0x30) & TRANSFER_DONE)
        self.assertEqual(ext_csd[192], 3)  # EXT_CSD revision for MMC 4.3.
        self.assertEqual(ext_csd[196] & 3, 3)
        self.assertEqual(struct.unpack_from('<I', ext_csd, 212)[0], DISK_BYTES // BLOCK)
        self.assertEqual((self.command(13, 1 << 16) >> 9) & 0xF, 4)
        return ext_csd

    def transfer(self, block, *, write=False, blocks=1, adma=False):
        q = self.machine
        self.command(23, blocks)
        self.write8(0x28, 0x10 if adma else 0)
        if adma:
            descriptors = b''.join(struct.pack('<HHI', 0x23 if i == blocks - 1 else 0x21,
                                               BLOCK, AXI_BUFFER + i * BLOCK)
                                   for i in range(blocks))
            q.write(CPU_DESCRIPTORS, descriptors)
            q.write32(HOST + 0x58, AXI_DESCRIPTORS)
            q.write32(HOST + 0x5C, 0)
        else:
            q.write32(HOST + 0x00, AXI_BUFFER)
        q.write32(HOST + 0x04, (blocks << 16) | BLOCK)
        argument = block if self.block_addressed else block * BLOCK
        index = (25 if blocks > 1 else 24) if write else (18 if blocks > 1 else 17)
        mode = 3 | (0 if write else 0x10) | (0x20 if blocks > 1 else 0)
        self.command(index, argument, data=True, transfer=mode)
        status = q.read32(HOST + 0x30)
        self.assertEqual(status & (COMMAND_DONE | TRANSFER_DONE), 3)
        self.assertEqual(status & 0xFFFF8000, 0)
        self.assertEqual(q.read32(HOST + 0x24) & 0x307, 0,
                         'transfer must release CMD/DAT inhibit and active flags')

    def adma_chain(self, block, descriptors, *, write=False, count_enabled=False, blocks=1):
        q = self.machine
        self.write8(0x28, 0x10)
        q.write(CPU_DESCRIPTORS, b''.join(struct.pack('<HHI', *entry) for entry in descriptors))
        q.write32(HOST + 0x58, AXI_DESCRIPTORS)
        q.write32(HOST + 0x5C, 0)
        q.write32(HOST + 0x04, (blocks << 16) | BLOCK)
        q.write32(HOST + 0x30, 0xFFFFFFFF)
        q.write32(HOST + 0x08, block if self.block_addressed else block * BLOCK)
        index = 24 if write else 17
        mode = 1 | (0 if write else 0x10) | (2 if count_enabled else 0)
        q.write32(HOST + 0x0C, (index << 24) | ((R1 | 0x20) << 16) | mode)
        return q.read32(HOST + 0x30)

    def test_native_mmc_initialization_ext_csd_and_irq_mask_w1c(self):
        q = self.machine
        self.initialize_card(signal=False)
        self.assertFalse(q.irq(IRQ), 'masked command completion must not raise IRQ26')
        self.assertTrue(self.read16(0x30) & COMMAND_DONE)
        q.write32(HOST + 0x38, 0xFFFFFFFF)
        self.assertTrue(q.irq(IRQ))
        q.write32(HOST + 0x30, 0)
        self.assertTrue(q.irq(IRQ), 'zero must not acknowledge a W1C register')
        self.write16(0x30, COMMAND_DONE)
        self.assertFalse(q.irq(IRQ))
        self.assertEqual(self.read16(0x30), 0)
        self.transfer(SEED_BLOCK)
        self.assertTrue(q.irq(IRQ))
        self.write16(0x30, COMMAND_DONE)
        self.assertTrue(q.irq(IRQ), 'transfer completion remains pending')
        self.assertEqual(self.read16(0x30), TRANSFER_DONE)
        self.write16(0x30, TRANSFER_DONE)
        self.assertFalse(q.irq(IRQ))

    def test_sdma_address_setup_does_not_start_an_idle_transfer(self):
        q = self.machine
        self.initialize_card()
        for old_mode in (0x10, 0x11, 0x13):
            with self.subTest(old_mode=old_mode):
                # An idle host may retain a block count and transfer mode from
                # an earlier command. Programming SYSAD is not a command.
                self.write16(0x0C, old_mode)
                q.write32(HOST + 0x04, (1 << 16) | BLOCK)
                q.write32(HOST + 0x30, 0xFFFFFFFF)
                sentinel = bytes([0xA5]) * BLOCK
                q.write(CPU_BUFFER, sentinel)
                q.write32(HOST + 0x00, AXI_BUFFER)
                self.assertEqual(q.read32(HOST + 0x30), 0,
                                 'address setup must not raise completion')
                self.assertFalse(q.irq(IRQ))
                self.assertEqual(self.read16(0x06), 1)
                self.assertEqual(q.read(CPU_BUFFER, BLOCK), sentinel)
                self.command(17, SEED_BLOCK if self.block_addressed else SEED_BLOCK * BLOCK,
                             data=True, transfer=0x13)
                self.assertEqual(q.read(CPU_BUFFER, BLOCK), SEED[:BLOCK])
                self.assertEqual(self.read16(0x30), COMMAND_DONE | TRANSFER_DONE)

    def test_sdma_address_write_resumes_at_a_dma_boundary(self):
        q = self.machine
        self.initialize_card()
        blocks = 9  # One block beyond the minimum 4 KiB SDMA boundary.
        self.command(23, blocks)
        q.write32(HOST + 0x00, AXI_BUFFER)
        q.write32(HOST + 0x04, (blocks << 16) | BLOCK)
        q.write(CPU_BUFFER, bytes([0xA5]) * (blocks * BLOCK))
        self.command(18, SEED_BLOCK if self.block_addressed else SEED_BLOCK * BLOCK,
                     data=True, transfer=0x33)
        self.assertEqual(self.read16(0x30), COMMAND_DONE | 8)
        self.assertEqual(self.read16(0x06), 1)
        self.assertEqual(q.read32(HOST + 0x00), AXI_BUFFER + 4096)
        self.assertTrue(q.read32(HOST + 0x24) & 2, 'DAT remains inhibited')
        q.write32(HOST + 0x30, 0xFFFFFFFF)
        # Firmware acknowledges the DMA interrupt before writing SYSAD; the
        # W1C status bit cannot be used as the sole resume-state indicator.
        q.write32(HOST + 0x00, AXI_BUFFER + 4096)
        self.assertEqual(self.read16(0x30), TRANSFER_DONE)
        self.assertEqual(self.read16(0x06), 0)
        self.assertEqual(q.read32(HOST + 0x24) & 0x307, 0)
        self.assertEqual(q.read(CPU_BUFFER, blocks * BLOCK),
                         SEED + bytes(blocks * BLOCK - len(SEED)))

    def test_sdma_address_write_cannot_consume_an_active_pio_read(self):
        q = self.machine
        self.initialize_card()
        q.write32(HOST + 0x04, (1 << 16) | BLOCK)
        sentinel = bytes([0xA5]) * BLOCK
        q.write(CPU_BUFFER, sentinel)
        self.command(17, SEED_BLOCK if self.block_addressed else SEED_BLOCK * BLOCK,
                     data=True, transfer=0x12)
        status = q.read32(HOST + 0x30)
        self.assertTrue(status & READ_READY)
        q.write32(HOST + 0x00, AXI_BUFFER)
        self.assertEqual(q.read32(HOST + 0x30), status)
        self.assertEqual(q.read(CPU_BUFFER, BLOCK), sentinel)
        actual = b''.join(struct.pack('<I', q.read32(HOST + 0x20))
                          for _ in range(BLOCK // 4))
        self.assertEqual(actual, SEED[:BLOCK])
        self.assertTrue(self.read16(0x30) & TRANSFER_DONE)

    def test_sdma_axi_alias_write_survives_host_system_and_process_reset(self):
        q = self.machine
        self.initialize_card()
        q.write(CPU_BUFFER, bytes(BLOCK))
        self.transfer(SEED_BLOCK)
        self.assertEqual(q.read(CPU_BUFFER, BLOCK), SEED[:BLOCK])
        self.assertEqual(q.read(AXI_BUFFER, BLOCK), SEED[:BLOCK])
        replacement = bytes((i * 17 + 91) & 255 for i in range(BLOCK))
        q.write(CPU_BUFFER, replacement)
        self.transfer(SEED_BLOCK + 4, write=True)
        self.write8(0x2F, 1)
        self.assertEqual(q.read32(HOST + 0x34), 0)
        self.assertEqual(q.read32(HOST + 0x30), 0)
        self.assertFalse(q.irq(IRQ))
        self.initialize_card()
        q.write(CPU_BUFFER, bytes(BLOCK))
        self.transfer(SEED_BLOCK + 4)
        self.assertEqual(q.read(CPU_BUFFER, BLOCK), replacement)
        q.qmp('system_reset')
        q.qmp('query-status')
        self.assertEqual(q.read32(HOST + 0x34), 0)
        self.assertFalse(q.irq(IRQ))
        self.initialize_card()
        q.write(CPU_BUFFER, bytes(BLOCK))
        self.transfer(SEED_BLOCK + 4)
        self.assertEqual(q.read(CPU_BUFFER, BLOCK), replacement)
        self.close_machine()
        with self.disk.open('rb') as disk:
            disk.seek((SEED_BLOCK + 4) * BLOCK)
            self.assertEqual(disk.read(BLOCK), replacement)
        self.start_machine()
        self.initialize_card()
        self.transfer(SEED_BLOCK + 4)
        self.assertEqual(self.machine.read(CPU_BUFFER, BLOCK), replacement)

    def test_adma2_descriptors_and_multiblock_read_write_use_ap_memory(self):
        q = self.machine
        self.initialize_card()
        self.assertTrue(q.read32(HOST + 0x40) & (1 << 19), 'ADMA2 capability missing')
        q.write(CPU_BUFFER, bytes(2 * BLOCK))
        self.transfer(SEED_BLOCK, blocks=2, adma=True)
        self.assertEqual(q.read(CPU_BUFFER, len(SEED)), SEED)
        self.assertEqual(self.read16(0x06), 0)
        self.assertEqual(q.read32(HOST + 0x58), AXI_DESCRIPTORS + 16)
        replacement = bytes(((index * 71 + 29) ^ ((index // BLOCK) * 0xA7)) & 255
                            for index in range(2 * BLOCK))
        q.write(CPU_BUFFER, replacement)
        self.transfer(SEED_BLOCK + 6, write=True, blocks=2, adma=True)
        q.write(CPU_BUFFER, bytes(2 * BLOCK))
        self.transfer(SEED_BLOCK + 6, blocks=2, adma=True)
        self.assertEqual(q.read(CPU_BUFFER, len(replacement)), replacement)
        self.close_machine()
        with self.disk.open('rb') as disk:
            disk.seek((SEED_BLOCK + 6) * BLOCK)
            self.assertEqual(disk.read(len(replacement)), replacement)

    def test_adma2_fragmented_blocks_allow_nop_end_without_block_count(self):
        q = self.machine
        fragments = ((28, 0), (448, 0x400), (36, 0x800))
        for write in (False, True):
            for middle_nop in (False, True):
                with self.subTest(write=write, middle_nop=middle_nop):
                    self.initialize_card()
                    expected = (bytes((i * 19 + 77) & 255 for i in range(BLOCK))
                                if write else SEED[:BLOCK])
                    descriptors = []
                    position = 0
                    for number, (length, offset) in enumerate(fragments):
                        payload = expected[position:position + length] if write else bytes(length)
                        q.write(CPU_BUFFER + offset, payload)
                        descriptors.append((0x21, length, AXI_BUFFER + offset))
                        if number == 0 and middle_nop:
                            descriptors.append((0x01, 0, 0))  # VALID NOP; no data length.
                        position += length
                    descriptors.append((0x03, 0, 0))  # Separate VALID|END NOP.
                    block = SEED_BLOCK + 10 + int(middle_nop) if write else SEED_BLOCK
                    status = self.adma_chain(block, descriptors, write=write)
                    self.assertEqual(status, COMMAND_DONE | TRANSFER_DONE,
                                     'NOP length must not produce an ADMA transfer-length error')
                    self.assertEqual(q.read32(HOST + 0x54), 0)
                    self.assertEqual(q.read32(HOST + 0x24) & 0x307, 0)
                    if write:
                        q.write(CPU_BUFFER, bytes(BLOCK))
                        self.transfer(block)
                        self.assertEqual(q.read(CPU_BUFFER, BLOCK), expected)
                    else:
                        actual = b''.join(q.read(CPU_BUFFER + offset, length)
                                          for length, offset in fragments)
                        self.assertEqual(actual, expected)

    def test_adma2_nop_end_rejects_partial_block_and_unfinished_block_count(self):
        q = self.machine
        for count_enabled in (False, True):
            for write in (False, True):
                with self.subTest(count_enabled=count_enabled, write=write):
                    self.initialize_card()
                    # Without block count, half a block cannot complete. With
                    # block count enabled, one complete block of two is short.
                    length = BLOCK if count_enabled else BLOCK // 2
                    blocks = 2 if count_enabled else 1
                    q.write(CPU_BUFFER, bytes([0xA5]) * BLOCK)
                    descriptors = ((0x21, length, AXI_BUFFER), (0x03, 0, 0))
                    status = self.adma_chain(SEED_BLOCK + 12, descriptors, write=write,
                                             count_enabled=count_enabled, blocks=blocks)
                    self.assertTrue(status & (1 << 25), 'short transfer must set ADMA error')
                    self.assertTrue(status & STATUS_ERROR)
                    self.assertTrue(q.read32(HOST + 0x54) & 4,
                                    'short transfer must retain LENGTH_MISMATCH')
                    self.assertTrue(q.irq(IRQ))
                    q.write32(HOST + 0x30, 0xFFFFFFFF)
                    self.assertFalse(q.irq(IRQ))


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700SdhciTcg(unittest.TestCase):
    def test_m55_phy_and_standard_mmio_across_cpu_pages(self):
        commands = ('arm-none-eabi-as', 'arm-none-eabi-ld', 'arm-none-eabi-objcopy')
        for command in commands:
            if shutil.which(command) is None:
                self.skipTest(f'{command} is not installed')
        with tempfile.TemporaryDirectory(prefix='best1700-sdhci-program-') as temp:
            directory = Path(temp)
            source, obj, elf, binary = (directory / name for name in
                                        ('probe.s', 'probe.o', 'probe.elf', 'probe.bin'))
            source.write_text(TCG_ASSEMBLY)
            for command in (
                [commands[0], '-march=armv8-m.main', '-mthumb', '-o', str(obj), str(source)],
                [commands[1], '-Ttext=0x101c0000', '-e', '0x101c0401', '-o', str(elf), str(obj)],
                [commands[2], '-O', 'binary', str(elf), str(binary)],
            ):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, f'{command[0]} failed:\n{result.stderr}')
            image = binary.read_bytes()
        self.assertEqual(struct.unpack_from('<I', image, 4)[0], 0x101C0401)
        machine = QtestMachine(
            os.environ['MIBAND_BEST1700_QEMU'], image_payload=image,
            machine_options=',boot-sp=0x203ff000,boot-pc=0x101c0401', accel='tcg',
            extra_args=('-S', '-icount', 'shift=0,sleep=off'))
        self.addCleanup(machine.close)
        for cycle in ('initial boot', 'system reset'):
            with self.subTest(cycle=cycle):
                if cycle == 'system reset':
                    machine.qmp('system_reset')
                    machine.qmp('query-status')
                    machine.write(0x20001000, bytes(32))
                machine.qmp('cont')
                deadline = time.monotonic() + 5
                words = (0,) * 8
                while time.monotonic() < deadline:
                    words = struct.unpack('<8I', machine.read(0x20001000, 32))
                    if words[0]:
                        break
                    time.sleep(0.01)
                machine.qmp('stop')
                self.assertEqual(words[0], 1, f'M55 MMIO probe did not finish: {words}')
                self.assertEqual(words[1:6], (0, 0x1234ABCD, 0, 0x5678EF01, 0xA55A7890))
                self.assertTrue(words[6] & (1 << 22), 'guest must see the SDMA capability')
                self.assertEqual(words[7], 0, 'guest host-reset must clear the argument')
                self.assertEqual(machine.read32(HOST + 0x800), 0x1234ABCD)
                self.assertEqual(machine.read32(HOST + 0x818), 0x5678EF01)


if __name__ == '__main__':
    unittest.main()
