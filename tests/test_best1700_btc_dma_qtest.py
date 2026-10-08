"""Exercise the BTC DMA master using synthetic RAM and UART transactions."""

import os
import struct
import unittest

from tests import test_best1700_btc_machine_qtest as btc_qtest


DMA = 0x6D010000
SRC_INC = 1 << 26
DST_INC = 1 << 27
TC = 1 << 31
TC_IE = 1 << 15
ERR_IE = 1 << 14


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcDmaTests(unittest.TestCase):
    machine = btc_qtest.BtcMachineTests.machine
    assemble = btc_qtest.BtcMachineTests.assemble
    await_word = btc_qtest.BtcMachineTests.await_word

    def dma_machine(self, **kwargs):
        machine = self.machine(**kwargs)
        machine.command("irq_intercept_in /machine/btc-core")
        return machine

    @staticmethod
    def channel(index):
        return DMA + 0x100 + 0x20 * index

    def configure(self, q, index, source, destination, count, *,
                  control=TC | SRC_INC | DST_INC, config=TC_IE | ERR_IE | 1,
                  lli=0):
        base = self.channel(index)
        for offset, value in ((0, source), (4, destination), (8, lli),
                              (12, control | count), (16, config)):
            q.write32(base + offset, value)

    def test_manual_channel_clock_gate_and_boundary_split(self):
        q = self.dma_machine()
        # A four-byte transfer crosses a 1 KB boundary as separate byte beats.
        q.write(0x680053fe, b"gate")
        self.configure(q, 7, 0x680053fe, 0x680063fe, 4)
        q.write32(DMA + 0x30, 0x80000009)
        self.assertEqual(q.read32(DMA + 0x30), 0x80000009)
        q.step(10_000)
        self.assertEqual(q.read(0x680063fe, 4), bytes(4))
        self.assertEqual(q.read32(DMA + 0x14), 0)
        q.write32(DMA + 0x30, 0x80080009)
        q.step(1000)
        self.assertEqual(q.read(0x680063fe, 4), b"gate")
        self.assertEqual(q.read32(DMA + 0x14), 0x80)
        q.write32(DMA + 8, 0x80)
        self.configure(q, 7, 0x680053fe, 0x68006400, 4)
        q.write32(DMA + 0x30, 9)  # Automatic clocks do not need a manual mask.
        q.step(1000)
        self.assertEqual(q.read(0x68006400, 4), b"gate")

    def test_original_initialization_keeps_both_channels_disabled(self):
        q = self.dma_machine()
        for index, control, config in ((1, 0x8C014000, 0xC000),
                                        (0, 0x84014000, 0xC800)):
            q.write32(DMA + 8, 1 << index)
            q.write32(DMA + 0x10, 1 << index)
            q.write32(self.channel(index) + 8, 0)
            q.write32(self.channel(index) + 12, control)
            q.write32(self.channel(index) + 16, config)
            q.write32(DMA + 0x30, q.read32(DMA + 0x30) | 1)
        q.write32(self.channel(0) + 4, 0x6E000000)
        q.step(10_000)
        self.assertEqual(q.read32(DMA + 0x1C), 0)
        self.assertEqual(q.read32(DMA + 0x14), 0)
        self.assertEqual(q.read32(DMA + 0x18), 0)
        self.assertFalse(q.irq(3))

    def test_eighth_channel_copy_masked_tc_and_per_channel_w1c(self):
        q = self.dma_machine()
        payload = bytes(range(32))
        q.write(0x68005000, payload)
        q.write32(DMA + 0x30, 1)
        self.configure(q, 7, 0x68005000, 0x68006000, len(payload) // 4,
                       control=TC | SRC_INC | DST_INC | (2 << 18) | (2 << 21),
                       config=1)
        self.assertEqual(q.read32(DMA + 0x1C), 0x80)
        self.assertEqual(q.read(0x68006000, len(payload)), bytes(len(payload)))
        q.step(1000)
        self.assertEqual(q.read(0x68006000, len(payload)), payload)
        self.assertEqual(q.read32(self.channel(7)), 0x68005020)
        self.assertEqual(q.read32(self.channel(7) + 4), 0x68006020)
        self.assertEqual(q.read32(self.channel(7) + 12) & 0xFFF, 0)
        self.assertEqual(q.read32(DMA + 0x1C), 0)
        self.assertEqual(q.read32(DMA + 0x14), 0x80)
        self.assertEqual(q.read32(DMA + 4), 0)
        self.assertFalse(q.irq(3))
        q.write32(self.channel(7) + 16, TC_IE)
        self.assertEqual(q.read32(DMA), 0x80)
        self.assertTrue(q.irq(3))
        self.assertFalse(q.irq(37))
        q.write32(DMA + 8, 1)
        self.assertTrue(q.irq(3))
        q.write32(DMA + 8, 0x80)
        self.assertFalse(q.irq(3))

    def test_global_enable_halt_cancel_and_system_reset_control_pending_work(self):
        q = self.dma_machine()
        q.write(0x68005000, b"dma gates")
        self.configure(q, 1, 0x68005000, 0x68006000, 9)
        q.step(10_000)
        self.assertEqual(q.read(0x68006000, 9), bytes(9))
        self.assertEqual(q.read32(DMA + 0x1C), 2)
        q.write32(DMA + 0x30, 1)
        q.write32(self.channel(1) + 16, TC_IE | ERR_IE | 1 | (1 << 18))
        q.step(10_000)
        self.assertEqual(q.read(0x68006000, 9), bytes(9))
        q.write32(self.channel(1) + 16, TC_IE | ERR_IE | 1)
        q.step(1000)
        self.assertEqual(q.read(0x68006000, 9), b"dma gates")
        q.write32(DMA + 8, 2)
        self.configure(q, 1, 0x68005000, 0x68006100, 9)
        q.write32(self.channel(1) + 16, 0)
        q.step(10_000)
        self.assertEqual(q.read(0x68006100, 9), bytes(9))
        self.configure(q, 1, 0x68005000, 0x68006200, 9)
        q.qmp("system_reset")
        q.qmp("query-status")
        q.step(10_000)
        self.assertEqual(q.read(0x68006200, 9), bytes(9))
        for offset in (0, 4, 0xC, 0x14, 0x18, 0x1C, 0x30):
            self.assertEqual(q.read32(DMA + offset), 0)
        self.assertFalse(q.irq(3))

    def test_dma_uses_btc_bus_and_preserves_progress_on_partial_bus_error(self):
        q = self.dma_machine()
        q.write32(DMA + 0x30, 1)
        # AP has this DTCM view. BTC's DMA master has no such mapping.
        q.write32(0x20005000, 0x12345678)
        self.configure(q, 1, 0x20005000, 0x68006000, 4)
        q.step(1000)
        self.assertEqual(q.read32(0x68006000), 0)
        self.assertEqual(q.read32(DMA + 0x18), 2)
        self.assertEqual(q.read32(DMA + 0x14), 0)
        self.assertTrue(q.irq(3))
        q.write32(DMA + 0x10, 2)
        q.write(0x68017FF8, b"last RAM")
        self.configure(q, 1, 0x68017FF8, 0x68006100, 16)
        q.step(1000)
        self.assertEqual(q.read(0x68006100, 16), b"last RAM" + bytes(8))
        self.assertEqual(q.read32(self.channel(1)), 0x68018000)
        self.assertEqual(q.read32(self.channel(1) + 4), 0x68006108)
        self.assertEqual(q.read32(self.channel(1) + 12) & 0xFFF, 8)
        self.assertEqual(q.read32(DMA + 0x18), 2)
        self.assertEqual(q.read32(DMA + 0x14), 0)
        q.write32(DMA + 0x10, 1)
        self.assertTrue(q.irq(3))
        q.write32(DMA + 0x10, 2)
        self.assertFalse(q.irq(3))

    def test_lli_advances_one_descriptor_per_tick_and_reports_bad_descriptors(self):
        q = self.dma_machine()
        q.write32(DMA + 0x30, 1)
        q.write32(0x40080098, 1)
        q.write(0x68005000, b"first")
        q.write(0x68005100, b"second")
        q.write(0x68004000, struct.pack("<IIII", 0x68005100, 0x68406100,
                                      0, TC | SRC_INC | DST_INC | 6))
        self.configure(q, 1, 0x68005000, 0x68406000, 5,
                       control=SRC_INC | DST_INC, lli=0x68004001)
        q.step(1000)
        self.assertEqual(q.read(0x60406000, 5), b"first")
        self.assertEqual(q.read(0x60406100, 6), bytes(6))
        self.assertEqual(q.read32(DMA + 0x1C), 2)
        self.assertFalse(q.irq(3))
        q.step(1000)
        self.assertEqual(q.read(0x60406100, 6), b"second")
        self.assertEqual(q.read32(DMA + 0x14), 2)
        self.assertTrue(q.irq(3))
        q.write32(DMA + 8, 2)
        self.configure(q, 1, 0x68005000, 0x68406200, 5,
                       control=SRC_INC | DST_INC, lli=0x70000000)
        q.step(1000)
        self.assertEqual(q.read(0x60406200, 5), b"first")
        self.assertEqual(q.read32(DMA + 0x18), 2)
        self.assertEqual(q.read32(DMA + 0x14), 0)
        self.assertEqual(q.read32(DMA + 0x1C), 0)

    def test_uart_tx_fixed_destination_and_rx_wait_without_invented_bytes(self):
        q = self.dma_machine(uart=True)
        payload = b"BTC DMA UART\r\n"
        q.write32(0x6E000030, 0x101)
        q.write(0x68005000, payload)
        q.write32(DMA + 0x30, 1)
        self.configure(q, 0, 0x68005000, 0x6E000000, len(payload),
                       control=0x84014000, config=0xC801)
        q.step(1000)
        self.assertEqual(q.serial(2), payload)
        self.assertEqual(q.read32(self.channel(0) + 4), 0x6E000000)
        self.assertEqual(q.read32(DMA + 0x14), 1)
        q.write32(DMA + 8, 1)
        q.write(0x68006000, b"receive?")
        self.configure(q, 2, 0x6E000000, 0x68006000, 8,
                       control=TC | DST_INC,
                       config=TC_IE | ERR_IE | (2 << 11) | 1)
        q.step(1_000_000)
        self.assertEqual(q.read(0x68006000, 8), b"receive?")
        self.assertEqual(q.read32(DMA + 0x1C), 4)
        self.assertEqual(q.read32(DMA + 0x14), 0)
        self.assertEqual(q.read32(DMA + 0x18), 0)
        self.assertFalse(q.irq(3))

    def test_btc_guest_receives_dma_irq3_and_acknowledges_its_channel(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0xe000e100
    movs r0, #8
    str r0, [r4]
    ldr r4, =0x6d010000
    movs r0, #1
    str r0, [r4, #0x30]
    ldr r0, =0x8001
    str r0, [r4, #0x130]
    dsb sy
    cpsie i
1:  wfi
    b 1b
    .ltorg
.org 0x100
.thumb_func
dma_irq:
    ldr r4, =0x6d010000
    ldr r0, [r4, #4]
    ldr r5, =0x68001000
    str r0, [r5, #4]
    movs r0, #2
    str r0, [r4, #8]
    movs r0, #0xd1
    str r0, [r5]
    bx lr
    .ltorg
""", base=0x68004000)
        q = self.dma_machine(tcg=True)
        q.write(0x68004000, code)
        q.write(0x68005000, b"interrupt copy")
        self.configure(q, 1, 0x68005000, 0x68006000, 14, config=TC_IE)
        vectors = bytearray(128)
        struct.pack_into("<II", vectors, 0, 0x68017F00, 0x68004001)
        struct.pack_into("<I", vectors, (16 + 3) * 4, 0x68004101)
        q.write(0x68002000, vectors)
        q.write32(0x40080168, 0x68002000)
        q.qmp("cont")
        q.write32(0x400800CC, 0x88)
        self.await_word(q, 0x68001000, 0xD1)
        q.qmp("stop")
        self.assertEqual(q.read(0x68006000, 14), b"interrupt copy")
        self.assertEqual(q.read32(0x68001004), 2)
        self.assertEqual(q.read32(DMA + 0x14), 0)
        self.assertFalse(q.irq(3))


if __name__ == "__main__":
    unittest.main()
