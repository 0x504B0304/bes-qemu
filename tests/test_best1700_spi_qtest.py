"""Exercise digital SPI polling, disconnected input and CMU clock selection.

Tests use synthetic guest code and their own QEMU processes. The bounded
eight-frame model FIFO is not a claim about the physical hardware depth.
"""

import os
import struct
import unittest

from tests import test_best1700_bth_qtest as bth


SPI0 = 0x50010000
SPI1 = 0x50011000
CLOCK = 0x500001C8
TNF, RNE, BUSY = 2, 4, 16


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class SpiTests(unittest.TestCase):
    machine = bth.BthTests.machine
    assemble = bth.BthTests.assemble
    await_marker = bth.BthTests.await_marker

    @staticmethod
    def configure(machine, base=SPI1, *, bits=8, prescale=2, scr=0,
                  enabled=True):
        machine.write32(base, ((bits - 1) << 16) | (scr << 8))
        machine.write32(base + 0x88, 0)
        machine.write32(base + 0x10, prescale)
        machine.write32(base + 4, 2 if enabled else 0)

    def test_disconnected_input_is_not_tx_echo_and_uses_extended_dss(self):
        machine = self.machine()
        for bits in (4, 8, 16, 24, 32):
            with self.subTest(bits=bits):
                self.configure(machine, bits=bits)
                machine.write32(SPI1 + 8, 0xC3)
                status = machine.read32(SPI1 + 0x0C)
                self.assertTrue(status & BUSY)
                self.assertFalse(status & RNE)
                machine.step(100_000)
                self.assertEqual(machine.read32(SPI1 + 0x0C) & (BUSY | RNE),
                                 RNE)
                self.assertEqual(machine.read32(SPI1 + 8), (1 << bits) - 1)
                self.assertFalse(machine.read32(SPI1 + 0x0C) & RNE)
        # The analog command port is a different controller and protocol.
        machine.write32(0x50009004, 7 << 7)
        self.assertEqual(machine.read32(SPI1 + 4), 2)

    def test_fifo_enable_backpressure_and_rx_drain(self):
        machine = self.machine()
        self.configure(machine, enabled=False)
        for index in range(8):
            self.assertTrue(machine.read32(SPI1 + 0x0C) & TNF)
            machine.write32(SPI1 + 8, index)
        self.assertFalse(machine.read32(SPI1 + 0x0C) & TNF)
        machine.write32(SPI1 + 8, 0xC3)  # Full FIFO cannot accept a ninth word.
        machine.step(1_000_000)
        self.assertEqual(machine.read32(SPI1 + 0x0C) & (RNE | BUSY), 0)
        machine.write32(SPI1 + 4, 2)
        for _ in range(8):
            machine.step(100_000)
        self.assertFalse(machine.read32(SPI1 + 0x0C) & BUSY)
        for _ in range(8):
            self.assertTrue(machine.read32(SPI1 + 0x0C) & RNE)
            self.assertEqual(machine.read32(SPI1 + 8), 0xFF)
        self.assertEqual(machine.read32(SPI1 + 0x0C) & (TNF | RNE), TNF)
        self.assertEqual(machine.read32(SPI1 + 8), 0)

    def test_spi_muxes_drive_independent_transfer_clocks(self):
        machine = self.machine()
        # Current machine crystal is 24 MHz. 8 * CPSR=2 gives 667 ns at x1.
        for source, frame_ns in ((0, 667), (2, 334), (3, 167)):
            machine.write32(CLOCK, source << 22)
            self.configure(machine, SPI0)
            self.configure(machine, SPI1)
            machine.write32(SPI0 + 8, 0)
            machine.write32(SPI1 + 8, 0)
            machine.step(frame_ns - 1)
            self.assertFalse(machine.read32(SPI1 + 0x0C) & RNE)
            machine.step(1)
            self.assertTrue(machine.read32(SPI1 + 0x0C) & RNE)
            self.assertEqual(machine.read32(SPI1 + 8), 0xFF)
            if source:
                self.assertFalse(machine.read32(SPI0 + 0x0C) & RNE)
                machine.step(667 - frame_ns)
            self.assertEqual(machine.read32(SPI0 + 8), 0xFF)
        # Shared-register writes retain other peripheral fields and support
        # byte lanes while updating only the selected clock.
        machine.write32(CLOCK, 0x1234)
        machine.command(f"writeb {CLOCK + 2:#x} 2")
        self.assertEqual(machine.read32(CLOCK), 0x21234)
        self.configure(machine, SPI0, bits=16, prescale=4, scr=2)
        machine.write32(SPI0 + 8, 0)
        machine.step(3999)  # 16 * 4 * 3 / 48 MHz = 4000 ns.
        self.assertFalse(machine.read32(SPI0 + 0x0C) & RNE)
        machine.step(1)
        self.assertEqual(machine.read32(SPI0 + 8), 0xFFFF)

    def test_clock_stop_and_enable_pause_do_not_create_receive_words(self):
        machine = self.machine()
        self.configure(machine)
        machine.write32(CLOCK, (1 << 24) | (1 << 27))  # Unconnected PLL.
        machine.write32(SPI1 + 8, 0xC3)
        machine.step(1_000_000)
        self.assertFalse(machine.read32(SPI1 + 0x0C) & RNE)
        machine.write32(CLOCK, 0)
        machine.step(100)
        machine.write32(SPI1 + 4, 0)
        machine.step(1_000_000)
        self.assertFalse(machine.read32(SPI1 + 0x0C) & (RNE | BUSY))
        machine.write32(SPI1 + 4, 2)
        machine.step(1_000)
        self.assertEqual(machine.read32(SPI1 + 8), 0xFF)
        self.assertFalse(machine.read32(SPI1 + 0x0C) & (RNE | BUSY))

    def test_reset_discards_active_transfer_fifo_and_configuration(self):
        machine = self.machine()
        self.configure(machine, bits=32, prescale=254, scr=255)
        machine.write32(SPI1 + 8, 0)
        machine.write32(SPI1 + 8, 1)
        machine.write32(SPI1 + 0x28, 7)
        machine.write32(SPI1 + 0x2C, 0x40003)
        machine.write32(CLOCK, 3 << 22)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        machine.step(1_000_000_000)
        for offset in (0, 4, 0x10, 0x28, 0x2C):
            self.assertEqual(machine.read32(SPI1 + offset), 0)
        self.assertEqual(machine.read32(CLOCK), 0)
        self.assertEqual(machine.read32(SPI1 + 0x0C) & (TNF | RNE | BUSY), TNF)
        self.configure(machine)
        machine.write32(SPI1 + 8, 0xC3)
        machine.step(1_000)
        self.assertEqual(machine.read32(SPI1 + 8), 0xFF)

    def test_bth_guest_polling_probe_finishes_with_absent_device(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x50011000
    ldr r5, =0x23001000
    ldr r0, =0x00070000
    str r0, [r4]
    movs r0, #0
    str r0, [r4, #0x88]
    movs r0, #2
    str r0, [r4, #0x10]
    str r0, [r4, #4]
    mvns r0, r0
    str r0, [r4, #0x20]
    movs r6, #40
    movs r7, #0
next:
    ldr r0, [r4, #0x0c]
    tst r0, #2
    beq next
    movs r0, #0xc3
    str r0, [r4, #8]
receive:
    ldr r0, [r4, #0x0c]
    tst r0, #4
    beq receive
    ldr r0, [r4, #8]
    str r0, [r5, #4]
    cmp r0, #0xff
    bne failure
    adds r7, #1
    subs r6, #1
    bne next
idle:
    ldr r0, [r4, #0x0c]
    tst r0, #16
    bne idle
    str r7, [r5, #8]
    movs r0, #0xa1
    b done
failure:
    movs r0, #0xee
done:
    str r0, [r5]
1:  wfi
    b 1b
    .ltorg
""")
        machine = self.machine(tcg=True)
        machine.write(0x23004000, code)
        machine.write(0x23002000, struct.pack("<II", 0x230BFF00, 0x03004001))
        machine.write32(bth.VTOR, 0x23002000)
        machine.qmp("cont")
        machine.write32(bth.RESET_CLEAR, 0x300)
        self.await_marker(machine, 0xA1)
        machine.qmp("stop")
        self.assertEqual(machine.read32(bth.RESULT + 4), 0xFF)
        self.assertEqual(machine.read32(bth.RESULT + 8), 40)
        self.assertEqual(machine.qmp("query-status")["status"], "paused")


if __name__ == "__main__":
    unittest.main()
