"""Exercise real host serial ingress, UART1 FIFO/timeout and M55 ISR execution."""
import os
from pathlib import Path
import socket
import struct
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine
from tests import test_best1700_cp_execution_qtest as cp_qtest
from tests import test_best1700_peripherals_qtest as periph

UART = 0x4000c000
RX, TX, RT = 1 << 4, 1 << 5, 1 << 6


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class UartQtest(unittest.TestCase):
    assemble = cp_qtest.CpExecutionQtest.assemble

    def machine(self, *, serial_index=1, **kwargs):
        temp = tempfile.TemporaryDirectory(prefix='best1700-uart-host-')
        self.addCleanup(temp.cleanup)
        serial = Path(temp.name) / 'serial.sock'
        args = ('-chardev', f'socket,id=modem,path={serial},server=on,wait=off')
        if kwargs.get('accel', 'qtest') != 'qtest':
            args += ('-S',)
        backends=['null'] * 3
        backends[serial_index]='chardev:modem'
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], extra_args=args,
                         serial_backends=backends, **kwargs)
        self.addCleanup(q.close)
        self.await_value(lambda: serial.exists(), 'serial server did not start')
        host = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(host.close)
        host.settimeout(3)
        host.connect(str(serial))
        q.command('irq_intercept_in /machine/core[0]')
        return q, host

    def await_value(self, reader, message):
        deadline = time.monotonic() + 3
        while True:
            result = reader()
            if result:
                return result
            if time.monotonic() > deadline:
                self.fail(message)
            time.sleep(.005)

    def enable(self, q, threshold=0):
        q.write32(UART + 0x24, 13)
        q.write32(UART + 0x28, 1)
        q.write32(UART + 0x2c, 0x70)
        q.write32(UART + 0x34, threshold << 3)
        q.write32(UART + 0x30, 0x301)

    def received(self, q):
        self.await_value(lambda: not q.read32(UART + 0x18) & 0x10,
                         'host bytes were not received')

    def test_host_bidirectional_fifo_mask_watermark_and_backpressure(self):
        q, host = self.machine()
        self.enable(q, threshold=2)  # half of the 16-byte FIFO
        host.sendall(b'abc')
        self.received(q)
        q.write32(UART + 0x38, RX)
        self.assertFalse(q.irq(56))
        host.sendall(b'defgh')
        self.await_value(lambda: q.read32(UART + 0x3c) & RX, 'no RX watermark')
        self.assertTrue(q.irq(56))
        q.write32(UART + 0x38, 0)
        self.assertFalse(q.irq(56))
        q.write32(UART + 0x38, RX)
        self.assertTrue(q.irq(56))
        self.assertEqual(q.read32(UART), ord('a'))
        self.assertFalse(q.irq(56), 'draining below watermark must clear IRQ')
        self.assertEqual(bytes(q.read32(UART) for _ in range(7)), b'bcdefgh')
        self.assertEqual(q.read32(UART), 0)
        payload = bytes(range(96))
        host.sendall(payload)
        output = bytearray()
        for _ in payload:
            self.received(q)
            output.append(q.read32(UART))
        self.assertEqual(output, payload, 'host stream must retain bytes above FIFO capacity')
        self.assertEqual(q.read32(UART + 4), 0, 'host backpressure must avoid overrun')
        for byte in b'boot-ack\0':
            q.write32(UART, byte)
        reply = bytearray()
        while len(reply) < 9:
            reply.extend(host.recv(9 - len(reply)))
        self.assertEqual(reply, b'boot-ack\0')

    def test_virtual_timeout_ack_drain_disable_and_machine_reset(self):
        q, host = self.machine()
        self.enable(q, threshold=4)
        q.write32(UART + 0x38, RT)
        host.sendall(b'Z')
        self.received(q)
        q.step(200000)
        self.assertFalse(q.irq(56))
        q.step(100000)
        self.assertTrue(q.irq(56), '32-bit receive timeout must reach NVIC')
        q.write32(UART + 0x44, RT)
        self.assertFalse(q.irq(56))
        q.step(300000)
        self.assertTrue(q.irq(56), 'unread input starts a new timeout after ACK')
        self.assertEqual(q.read32(UART), ord('Z'))
        self.assertFalse(q.irq(56))
        host.sendall(b'Q')
        self.received(q)
        q.write32(UART + 0x30, 0x101)
        q.step(1000000)
        self.assertFalse(q.irq(56), 'disabled RX must cancel timeout')
        q.qmp('system_reset')
        q.qmp('query-status')
        self.assertEqual(q.read32(UART + 0x18), 0x91)
        self.assertEqual(q.read32(UART + 0x38), 0)
        q.step(1000000)
        self.assertFalse(q.irq(56))
        self.enable(q)
        host.sendall(b'R')
        self.received(q)
        self.assertEqual(q.read32(UART), ord('R'), 'host stream survives reset')

    def test_nonfifo_loopback_overrun_and_fifo_toggle(self):
        q, host = self.machine()
        self.enable(q)
        q.write32(UART + 0x2c, 0x60)
        q.write32(UART + 0x30, 0x381)
        q.write32(UART + 0x38, RX | (1 << 10))
        q.write32(UART, ord('A'))
        self.assertEqual(q.read32(UART + 0x18) & 0x50, 0x40)
        q.write32(UART, ord('B'))
        self.assertEqual(q.read32(UART + 4), 8)
        self.assertEqual(q.read32(UART), ord('A'))
        self.assertTrue(q.irq(56), 'overrun remains until guest ACK')
        q.write32(UART + 0x44, 1 << 10)
        q.write32(UART + 4, 0)
        self.assertFalse(q.irq(56))
        self.assertEqual(q.read32(UART + 4), 0)
        q.write32(UART, ord('C'))
        q.write32(UART + 0x2c, 0x70)
        self.assertEqual(q.read32(UART), 0, 'FEN transition clears FIFO')
        self.assertFalse(q.irq(56))

    def configure_rx_dma(self, q, count, *, request=6, destination=0x21007000,
                         lli=0, controller_enabled=True):
        control = periph.DMA_TC | periph.DMA_DST_INC | count
        q.write32(periph.DMA0 + 0x30, int(controller_enabled))
        for offset, value in ((0, UART), (4, destination), (8, lli),
                              (12, control),
                              (16, periph.DMA_TC_IE | (2 << 11) | (request << 1) | 1)):
            q.write32(periph.DMA_CH0 + offset, value)

    def test_rx_dma_partial_fifo_waits_preserves_count_and_completes(self):
        q, host = self.machine()
        self.enable(q)
        q.write32(UART + 0x48, 1)
        self.configure_rx_dma(q, 7)
        q.step(1000)
        self.assertEqual(q.read32(periph.DMA_CH0 + 12) & 0xfff, 7)
        self.assertFalse(q.irq(37))
        host.sendall(b'abc')
        self.received(q)
        q.step(1000)
        self.assertEqual(q.read(0x21007000, 7), b'abc'+bytes(4))
        self.assertEqual(q.read32(periph.DMA_CH0 + 12) & 0xfff, 4)
        self.assertEqual(q.read32(periph.DMA_CH0 + 4), 0x21007003)
        self.assertEqual(q.read32(periph.DMA0 + 0x14), 0)
        self.assertEqual(q.read32(periph.DMA_CH0 + 16) & 1, 1)
        q.step(1000000000)
        self.assertEqual(q.read32(periph.DMA_CH0 + 12) & 0xfff, 4)
        host.sendall(b'defg')
        self.received(q)
        q.step(1000)
        self.assertEqual(q.read(0x21007000, 7), b'abcdefg')
        self.assertEqual(q.read32(periph.DMA_CH0 + 12) & 0xfff, 0)
        self.assertEqual(q.read32(periph.DMA_CH0 + 16) & 1, 0)
        self.assertTrue(q.irq(37))
        q.write32(periph.DMA0 + 8, 1)
        self.assertFalse(q.irq(37))

    def test_rx_dma_masks_halt_controller_gate_and_machine_reset(self):
        q, host = self.machine()
        self.enable(q)
        self.configure_rx_dma(q, 1, request=4)  # UART0 request cannot read UART1
        host.sendall(b'Q')
        self.received(q)
        q.step(1000)
        self.assertEqual(q.read32(periph.DMA_CH0 + 12) & 0xfff, 1)
        self.configure_rx_dma(q, 1, controller_enabled=False)
        q.write32(UART + 0x48, 1)
        q.step(1000)
        self.assertEqual(q.read32(periph.DMA_CH0 + 12) & 0xfff, 1)
        q.write32(periph.DMA0 + 0x30, 1)
        cfg=q.read32(periph.DMA_CH0 + 16)
        q.write32(periph.DMA_CH0 + 16, cfg | (1 << 18))
        q.step(1000)
        self.assertEqual(q.read32(periph.DMA_CH0 + 12) & 0xfff, 1)
        q.write32(periph.DMA_CH0 + 16, cfg)
        q.write32(UART + 0x48, 0)
        q.step(1000)
        self.assertEqual(q.read32(periph.DMA_CH0 + 12) & 0xfff, 1)
        q.write32(UART + 0x48, 1)
        q.step(1000)
        self.assertEqual(q.read(0x21007000, 1), b'Q')
        self.assertTrue(q.irq(37))
        q.qmp('system_reset')
        q.qmp('query-status')
        self.assertFalse(q.irq(37))
        self.assertEqual(q.read32(periph.DMA0 + 0x1c), 0)
        self.assertEqual(q.read32(UART + 0x48), 0)

    def test_rx_dma_lli_consumes_actual_stream_without_zero_padding(self):
        q, host = self.machine()
        self.enable(q)
        q.write32(UART + 0x48, 1)
        control=periph.DMA_TC | periph.DMA_DST_INC | 3
        q.write(0x21008000, struct.pack('<4I', UART, 0x21007003, 0, control))
        self.configure_rx_dma(q, 3, lli=0x21008000)
        host.sendall(b'abcdef')
        self.received(q)
        q.step(1000)
        self.assertEqual(q.read(0x21007000, 6), b'abc'+bytes(3))
        self.assertEqual(q.read32(periph.DMA_CH0 + 16) & 1, 1)
        q.write32(periph.DMA0 + 8, 1)
        q.step(1000)
        self.assertEqual(q.read(0x21007000, 6), b'abcdef')
        self.assertTrue(q.irq(37))
        self.assertEqual(q.read32(periph.DMA_CH0 + 16) & 1, 0)

    def test_dma_idle_timeout_survives_fifo_drain_ack_and_receiver_disable(self):
        q, host = self.machine()
        self.enable(q)
        q.write32(UART + 0x2c, 0x8970)
        q.write32(UART + 0x48, 1)
        q.write32(UART + 0x38, RT)
        self.configure_rx_dma(q, 128)
        host.sendall(b'help\r\n')
        self.received(q)
        q.step(1000)
        self.assertEqual(q.read(0x21007000,6),b'help\r\n')
        self.assertEqual(q.read32(periph.DMA_CH0+12)&0xfff,122)
        self.assertTrue(q.read32(UART+0x18)&0x10,'DMA drains the FIFO')
        self.assertFalse(q.irq(56))
        q.step(300000)
        self.assertTrue(q.irq(56),'DMA idle detection must survive FIFO drain')
        self.assertFalse(q.irq(37),'short request must not signal terminal count')
        q.write32(UART+0x44,RT)
        q.step(1000000)
        self.assertFalse(q.irq(56),'empty FIFO ACK must not create periodic idle IRQs')
        host.sendall(b'!')
        self.received(q)
        q.step(1000)
        self.assertEqual(q.read(0x21007000,7),b'help\r\n!')
        q.write32(UART+0x30,0x101)
        q.step(300000)
        self.assertFalse(q.irq(56),'RX disable cancels DMA idle detection')

    def test_mttcg_uart0_dma_host_stream_and_native_irq37_after_reset(self):
        image = self.assemble(r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
vectors:
    .word 0x203ff000
    .word reset_a
    .rept 51
    .word unexpected
    .endr
    .word dma_irq
    .rept 90
    .word unexpected
    .endr
.org 0x400
.thumb_func
reset_a:
    ldr r4, =0x20001000
    movs r0, #0
    str r0, [r4]
    str r0, [r4, #4]
    ldr r0, =vectors
    ldr r1, =0xe000ed08
    str r0, [r1]
    ldr r1, =0xe000e104
    movs r0, #32
    str r0, [r1]
    ldr r1, =0x4000b000
    movs r0, #0x70
    str r0, [r1, #0x2c]
    movw r0, #0x301
    str r0, [r1, #0x30]
    movs r0, #1
    str r0, [r1, #0x48]
    ldr r2, =0x400a0000
    str r0, [r2, #0x30]
    str r1, [r2, #0x100]
    ldr r1, =0x38007000
    str r1, [r2, #0x104]
    movs r1, #0
    str r1, [r2, #0x108]
    ldr r1, =0x88000040
    str r1, [r2, #0x10c]
    movw r1, #0xd009
    str r1, [r2, #0x110]
    str r0, [r4]
    cpsie i
    dsb sy
1:  wfi
    b 1b
.thumb_func
dma_irq:
    push {r0-r2, lr}
    ldr r0, =0x400a0000
    movs r1, #1
    str r1, [r0, #8]
    ldr r0, =0x20001000
    mrs r1, IPSR
    str r1, [r0, #8]
    movs r1, #0xd5
    str r1, [r0, #4]
    pop {r0-r2, pc}
.thumb_func
unexpected:
    ldr r0, =0x20001000
    movs r1, #255
    str r1, [r0]
2:  b 2b
    .ltorg
''', 0x101c0000)
        q, host = self.machine(serial_index=0, image_payload=image,
                              accel='tcg,thread=multi',
                              machine_options=',boot-sp=0x203ff000,boot-pc=0x101c0401')
        payload=bytes(range(64))
        for cycle in range(2):
            if cycle:
                q.qmp('system_reset')
                q.write(0x20001000, bytes(16))
                q.write(0x18007000, bytes(64))
            q.qmp('cont')
            self.await_value(lambda: q.read32(0x20001000)==1, 'guest DMA not configured')
            host.sendall(payload)
            self.await_value(lambda: q.read32(0x20001004)==0xd5, 'guest DMA ISR did not run')
            q.qmp('stop')
            self.assertEqual(q.read(0x18007000,64),payload)
            self.assertEqual(q.read32(0x20001008),16+37)
            self.assertEqual(q.read32(0x20001000),1,'unexpected exception')
            self.assertEqual(q.read32(periph.DMA_CH0+12)&0xfff,0)
            self.assertFalse(q.irq(37))

    def test_mttcg_host_input_wakes_wfi_and_executes_native_irq56(self):
        image = self.assemble(r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
vectors:
    .word 0x203ff000
    .word reset_a
    .rept 70
    .word unexpected
    .endr
    .word uart_irq
    .rept 71
    .word unexpected
    .endr
.org 0x400
.thumb_func
reset_a:
    ldr r4, =0x20001000
    movs r0, #0
    str r0, [r4]
    str r0, [r4, #4]
    ldr r0, =vectors
    ldr r1, =0xe000ed08
    str r0, [r1]
    ldr r0, =0x01000000
    ldr r1, =0xe000e104
    str r0, [r1]
    ldr r1, =0x4000c000
    movw r0, #0x301
    str r0, [r1, #0x30]
    movs r0, #0x10
    str r0, [r1, #0x38]
    movs r0, #1
    str r0, [r4]
    cpsie i
    dsb sy
1:  wfi
    b 1b
.thumb_func
uart_irq:
    push {r0-r3, lr}
    ldr r0, =0x20001000
    ldr r1, =0x4000c000
    ldr r2, [r1]
    str r2, [r0, #8]
    str r2, [r1]
    ldr r2, [r0, #4]
    adds r2, #1
    str r2, [r0, #4]
    mrs r2, IPSR
    str r2, [r0, #12]
    pop {r0-r3, pc}
.thumb_func
unexpected:
    ldr r0, =0x20001000
    movs r1, #255
    str r1, [r0]
2:  b 2b
    .ltorg
''', 0x101c0000)
        q, host = self.machine(image_payload=image, accel='tcg,thread=multi',
                              machine_options=',boot-sp=0x203ff000,boot-pc=0x101c0401')
        for cycle in range(2):
            if cycle:
                q.qmp('system_reset')
                q.write(0x20001000, bytes(16))
            q.qmp('cont')
            self.await_value(lambda: q.read32(0x20001000) == 1, 'guest did not enable UART')
            # A one-byte FIFO makes each IRQ independently observable.
            for index, byte in enumerate(b'~AP!'):
                host.sendall(bytes([byte]))
                self.assertEqual(host.recv(1), bytes([byte]))
                self.await_value(lambda: q.read32(0x20001004) == index + 1,
                                 'guest UART ISR did not finish')
                self.assertEqual(q.read32(0x20001008), byte)
                self.assertEqual(q.read32(0x2000100c), 16 + 56)
                self.assertEqual(q.read32(0x20001000), 1, 'unexpected exception')
            q.qmp('stop')
            self.assertFalse(q.irq(56))


if __name__ == '__main__':
    unittest.main()
