"""Validate independent SYS/BTC and BTH/BTC register and IRQ routes.

Each process is independent. The device must leave mailbox bytes to the guest
and report DONE only after an actual indication is acknowledged.
"""

from contextlib import contextmanager
import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


SYS = 0x40000000
BTC = 0x6E030000
BTH = 0x50000000
SIDES = {
    "cp": dict(path="/machine/core[1]", rx=(104, 105, 43, 119),
               done=(102, 103, 29, 30)),
    "btc": dict(path="/machine/btc-core", rx=(19, 18, 36, 37),
                done=(17, 16, 34, 35)),
    "btc_bth": dict(path="/machine/btc-core", rx=(14, 13, 23, 24),
                    done=(12, 11, 21, 22)),
}
PEERS = {"cp": "btc", "btc": "cp", "bth": "btc_bth", "btc_bth": "bth"}


def lane(side, channel):
    """Return the original physical register fields for one endpoint."""
    if side == "cp":
        return dict(send=SYS + 0x140, clear=SYS + 0x144,
                    mask_set=SYS + 0x140, mask_clear=SYS + 0x144,
                    status=SYS + 0x140, ind=1 << (4 + channel),
                    done=1 << channel, rx_mask=1 << (8 + channel),
                    done_mask=1 << (12 + channel), raw_rx=1 << (16 + channel),
                    raw_done=1 << (20 + channel), rx=1 << (24 + channel),
                    tx=1 << (28 + channel))
    bit = channel % 2
    if side == "bth":
        offset = 0xA0 if channel < 2 else 0x78
        rx_mask, done_mask, raw_rx, raw_done, rx, tx = (
            (13, 15, 17, 19, 21, 23) if channel < 2 else
            (4, 6, 8, 10, 12, 14))
        return dict(send=BTH + offset, clear=BTH + offset + 4,
                    mask_set=BTH + offset, mask_clear=BTH + offset + 4,
                    status=BTH + offset, ind=1 << (2 + bit),
                    done=1 << bit, rx_mask=1 << (rx_mask + bit),
                    done_mask=1 << (done_mask + bit), raw_rx=1 << (raw_rx + bit),
                    raw_done=1 << (raw_done + bit), rx=1 << (rx + bit),
                    tx=1 << (tx + bit))
    shift = 16 if side == "btc" else 0
    if channel < 2:
        return dict(send=BTC + 0x50, clear=BTC + 0x54,
                    mask_set=BTC + 0xA0, mask_clear=BTC + 0xA4,
                    status=BTC + 0xA0, ind=1 << (shift + 2 + bit),
                    done=1 << (shift + bit), rx_mask=1 << (shift + 2 + bit),
                    done_mask=1 << (shift + bit), raw_rx=1 << (shift + 6 + bit),
                    raw_done=1 << (shift + 4 + bit), rx=1 << (shift + 10 + bit),
                    tx=1 << (shift + 8 + bit))
    return dict(send=BTC + 0xAC, clear=BTC + 0xB0,
                mask_set=BTC + 0xAC, mask_clear=BTC + 0xB0,
                status=BTC + 0xAC, ind=1 << (shift + 2 + bit),
                done=1 << (shift + bit), rx_mask=1 << (shift + 4 + bit),
                done_mask=1 << (shift + 6 + bit), raw_rx=1 << (shift + 8 + bit),
                raw_done=1 << (shift + 10 + bit), rx=1 << (shift + 12 + bit),
                tx=1 << (shift + 14 + bit))


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcIpcTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, tcg=False):
        args = ["-smp", "4", "-d", "unimp"]
        if tcg:
            args += ["-S", "-icount", "shift=3,sleep=off"]
        return QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=(",cp-real-boot=on,bth-real-boot=on,btc-real-boot=on"
                             ",boot-sp=0x203ff000,boot-pc=0x101c0001"),
            accel="tcg" if tcg else "qtest", extra_args=args)

    @contextmanager
    def observer(self, side):
        q = self.machine()
        try:
            q.command(f'irq_intercept_in {SIDES[side]["path"]}')
            yield q
        finally:
            q.close()

    def assert_irqs(self, q, side, *, rx=(), done=()):
        for channel, irq in enumerate(SIDES[side]["rx"]):
            self.assertEqual(q.irq(irq), channel in rx, f"{side} RX {channel}")
        for channel, irq in enumerate(SIDES[side]["done"]):
            self.assertEqual(q.irq(irq), channel in done, f"{side} DONE {channel}")

    @staticmethod
    def enable(q, side, channel):
        r = lane(side, channel)
        q.write32(r["mask_set"], r["rx_mask"] | r["done_mask"])

    def test_btc_core_wakeup_request_is_set_and_cleared_by_cmu_pair(self):
        with self.observer("cp") as q:
            wake = BTC + 0x50
            clear = BTC + 0x54
            q.write32(wake, 1 << 4)
            self.assertEqual(q.read32(wake) & (1 << 4), 1 << 4)
            q.write32(clear, 1 << 4)
            self.assertEqual(q.read32(wake) & (1 << 4), 0)
            self.assertNotIn("BTC IPC unsupported BTC write offset=0x50",
                             q.stderr_path.read_text(errors="replace"))

    def test_all_original_irq_routes_and_masks_preserve_pending_state(self):
        for side in SIDES:
            peer = PEERS[side]
            with self.subTest(observer=side), self.observer(side) as q:
                for channel in range(4):
                    local, remote = lane(side, channel), lane(peer, channel)
                    q.write32(remote["send"], remote["ind"])
                    self.assertEqual(q.read32(local["status"]) & local["raw_rx"],
                                     local["raw_rx"])
                    self.assert_irqs(q, side)
                    q.write32(local["mask_set"], local["rx_mask"])
                    self.assert_irqs(q, side, rx=(channel,))
                    q.write32(local["mask_clear"], local["rx_mask"])
                    self.assertEqual(q.read32(local["status"]) & local["raw_rx"],
                                     local["raw_rx"])
                    self.assert_irqs(q, side)
                    q.write32(local["mask_set"], local["rx_mask"])
                    self.assert_irqs(q, side, rx=(channel,))
                    q.write32(remote["clear"], remote["ind"])
                    self.assert_irqs(q, side)

                    q.write32(local["send"], local["ind"])
                    self.assert_irqs(q, side)
                    q.write32(local["clear"], local["ind"])
                    self.assertEqual(q.read32(local["status"]) & local["raw_done"],
                                     local["raw_done"])
                    self.assert_irqs(q, side)
                    q.write32(local["mask_set"], local["done_mask"])
                    self.assert_irqs(q, side, done=(channel,))
                    q.write32(local["mask_clear"], local["done_mask"])
                    self.assert_irqs(q, side)
                    q.write32(local["mask_set"], local["done_mask"])
                    self.assert_irqs(q, side, done=(channel,))
                    q.write32(local["clear"], local["done"])
                    self.assert_irqs(q, side)

    def test_empty_ack_does_not_complete_and_repeated_ind_coalesces(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as q:
                for channel in range(4):
                    r = lane(side, channel)
                    self.enable(q, side, channel)
                    q.write32(r["clear"], r["ind"])
                    self.assertEqual(q.read32(r["status"]) & r["raw_done"], 0)
                    self.assert_irqs(q, side)
                    for _ in range(3):
                        q.write32(r["send"], r["ind"])
                    q.write32(r["clear"], r["ind"])
                    self.assert_irqs(q, side, done=(channel,))
                    q.write32(r["clear"], r["done"])
                    q.write32(r["clear"], r["ind"])
                    self.assert_irqs(q, side)
                    # Explicit software DONE_SET remains a real command.
                    q.write32(r["send"], r["done"])
                    self.assert_irqs(q, side, done=(channel,))
                    q.write32(r["clear"], r["done"])
                    self.assert_irqs(q, side)

    def test_full_duplex_lanes_do_not_acknowledge_each_other(self):
        for side in SIDES:
            peer = PEERS[side]
            with self.subTest(observer=side), self.observer(side) as q:
                for channel in range(4):
                    for name in (side, peer):
                        self.enable(q, name, channel)
                        r = lane(name, channel)
                        q.write32(r["send"], r["ind"])
                self.assert_irqs(q, side, rx=(0, 1, 2, 3))
                local = lane(side, 0)
                q.write32(local["clear"], local["ind"])
                self.assert_irqs(q, side, rx=(0, 1, 2, 3), done=(0,))
                remote = lane(peer, 2)
                q.write32(remote["clear"], remote["ind"])
                self.assert_irqs(q, side, rx=(0, 1, 3), done=(0,))
                q.write32(local["clear"], local["done"])
                self.assert_irqs(q, side, rx=(0, 1, 3))

    def test_status_read_only_subword_commands_and_sparse_neighbours(self):
        with self.observer("cp") as q:
            q.write32(BTC + 8, 0x200)
            q.write32(SYS + 0x140, 0xFFFF0000)
            q.write32(BTC + 0xA0, 0x0FF00FF0)
            q.write32(BTC + 0xAC, 0xFF00FF00)
            q.write32(BTH + 0xA0, 0x01FE0000)
            q.write32(BTH + 0x78, 0xFF00)
            self.assertEqual(q.read32(SYS + 0x140), 0)
            self.assertEqual(q.read32(BTC + 0xA0), 0)
            self.assertEqual(q.read32(BTC + 0xAC), 0)
            self.assertEqual(q.read32(BTH + 0xA0), 0)
            self.assertEqual(q.read32(BTH + 0x78), 0)
            # SYS byte 1 enables lane 1 RX; BTC byte 2 sends channel 1.
            q.command(f"writeb {SYS + 0x141:#x} 2")
            q.command(f"writeb {BTC + 0x52:#x} 8")
            self.assert_irqs(q, "cp", rx=(1,))
            self.assertEqual(q.read32(SYS + 0x140), 0x02020200)
            q.command(f"writeb {BTC + 0x56:#x} 8")
            self.assert_irqs(q, "cp")
            self.assertEqual(q.read32(BTC + 0x50), 0x20000)
            self.assertEqual(q.read32(BTC + 8), 0x200)
            self.assertEqual(q.read32(SYS + 0x98), 0)
            # The alternate BTC/BTH low halfword never becomes SYS traffic.
            q.write32(SYS + 0x140, 0xFF00)
            q.write32(BTC + 0x50, 0xF)
            q.write32(BTC + 0xAC, 0xFF)
            self.assert_irqs(q, "cp")
            self.assertEqual(q.read32(BTH + 0xA0) & 0x60000, 0x60000)
            self.assertEqual(q.read32(BTH + 0x78) & 0x300, 0x300)
            self.assertNotIn("BTC IPC unsupported", q.stderr_path.read_text(errors="replace"))
            # Reserved bits reject the whole write, including valid fields.
            before = q.read32(BTC + 0x50)
            q.write32(BTC + 0x50, 0x00140000)
            self.assertEqual(q.read32(BTC + 0x50), before)
            self.assertIn("BTC IPC unsupported BTC write", q.stderr_path.read_text(errors="replace"))

    def test_original_cold_mask_initialization_does_not_create_traffic(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as q:
                # Original BTC cold init at 0x60486c38 enables both interfaces.
                for offset, values in (
                        (0xA0, (4, 1, 8, 2)),
                        (0xAC, (0x10, 0x40, 0x20, 0x80)),
                        (0xA0, (0x40000, 0x10000, 0x80000, 0x20000)),
                        (0xAC, (0x100000, 0x400000, 0x200000, 0x800000))):
                    for value in values:
                        q.write32(BTC + offset, value)
                        self.assert_irqs(q, side)
                for name in ("cp", "bth"):
                    for channel in range(4):
                        self.enable(q, name, channel)
                for name in PEERS:
                    for channel in range(4):
                        r = lane(name, channel)
                        self.assertEqual(q.read32(r["status"]) &
                                         (r["raw_rx"] | r["raw_done"] |
                                          r["rx"] | r["tx"]), 0)
                self.assert_irqs(q, side)
                self.assertNotIn("BTC IPC unsupported",
                                 q.stderr_path.read_text(errors="replace"))

    def test_simultaneous_sys_bth_traffic_uses_independent_latches_and_irqs(self):
        with self.observer("btc") as q:
            for name in PEERS:
                for channel in range(4):
                    self.enable(q, name, channel)
                    r = lane(name, channel)
                    q.write32(r["send"], r["ind"])
            self.assert_irqs(q, "btc", rx=(0, 1, 2, 3))
            self.assert_irqs(q, "btc_bth", rx=(0, 1, 2, 3))

            bth = lane("bth", 1)
            q.write32(bth["clear"], bth["ind"])
            self.assert_irqs(q, "btc", rx=(0, 1, 2, 3))
            self.assert_irqs(q, "btc_bth", rx=(0, 2, 3))
            cp = lane("cp", 2)
            q.write32(cp["clear"], cp["ind"])
            self.assert_irqs(q, "btc", rx=(0, 1, 3))
            self.assert_irqs(q, "btc_bth", rx=(0, 2, 3))

            low, high = lane("btc_bth", 0), lane("btc", 2)
            q.write32(low["clear"], low["ind"])
            self.assert_irqs(q, "btc", rx=(0, 1, 3))
            self.assert_irqs(q, "btc_bth", rx=(0, 2, 3), done=(0,))
            self.assertEqual(q.read32(BTH + 0xA0) & (1 << 17), 0)
            self.assertEqual(q.read32(SYS + 0x140) & (1 << 16), 1 << 16)
            q.write32(high["clear"], high["ind"])
            self.assert_irqs(q, "btc", rx=(0, 1, 3), done=(2,))
            self.assert_irqs(q, "btc_bth", rx=(0, 2, 3), done=(0,))
            self.assertEqual(q.read32(SYS + 0x140) & (1 << 18), 0)
            self.assertEqual(q.read32(BTH + 0x78) & (1 << 8), 1 << 8)
            q.write32(low["clear"], low["done"])
            self.assert_irqs(q, "btc", rx=(0, 1, 3), done=(2,))
            self.assert_irqs(q, "btc_bth", rx=(0, 2, 3))

    def test_btc_combined_word_and_subword_accesses_keep_interfaces_independent(self):
        with self.observer("btc") as q:
            q.write32(BTC + 0xA0, 0x000F000F)
            q.write32(BTC + 0x50, 0x000C000C)
            self.assertEqual(q.read32(SYS + 0x140), 0x00030000)
            self.assertEqual(q.read32(BTH + 0xA0), 0x00060000)
            # Clear only low-half channel 1 IND+DONE. New DONE takes precedence.
            q.command(f"writew {BTC + 0x54:#x} 0xa")
            self.assertEqual(q.read32(BTC + 0x50), 0x000C0006)
            self.assert_irqs(q, "btc")
            self.assert_irqs(q, "btc_bth", done=(1,))
            self.assertEqual(q.read32(SYS + 0x140), 0x00030000)
            self.assertEqual(q.read32(BTH + 0xA0), 0x00020000)
            # High-half acknowledgment leaves the low halfword untouched.
            q.command(f"writew {BTC + 0x56:#x} 4")
            self.assertEqual(q.read32(BTC + 0x50), 0x00090006)
            self.assert_irqs(q, "btc", done=(0,))
            self.assert_irqs(q, "btc_bth", done=(1,))

    def test_machine_reset_clears_latches_without_touching_guest_mailboxes(self):
        for side in SIDES:
            peer = PEERS[side]
            with self.subTest(observer=side), self.observer(side) as q:
                data = struct.pack("<8I", *range(0x100, 0x108))
                q.write(0x68004000, data)
                for channel in range(4):
                    for name in (side, peer):
                        self.enable(q, name, channel)
                        r = lane(name, channel)
                        q.write32(r["send"], r["ind"] | r["done"])
                self.assert_irqs(q, side, rx=(0, 1, 2, 3), done=(0, 1, 2, 3))
                self.assertEqual(q.read(0x68004000, len(data)), data)
                q.qmp("system_reset")
                q.qmp("query-status")
                self.assert_irqs(q, side)
                for address in (SYS + 0x140, SYS + 0x144, BTC + 0x50,
                                BTC + 0x54, BTC + 0xA0, BTC + 0xA4,
                                BTC + 0xAC, BTC + 0xB0, BTH + 0x78,
                                BTH + 0x7C, BTH + 0xA0, BTH + 0xA4):
                    self.assertEqual(q.read32(address), 0)
                self.assertEqual(q.read(0x68004000, len(data)), data)

    def test_cp_traffic_does_not_raise_ap_irq_routes(self):
        q = self.machine()
        self.addCleanup(q.close)
        q.command("irq_intercept_in /machine/core[0]")
        for channel in range(4):
            self.enable(q, "cp", channel)
            cp, btc = lane("cp", channel), lane("btc", channel)
            q.write32(btc["send"], btc["ind"])
            q.write32(cp["send"], cp["done"])
        for irq in SIDES["cp"]["rx"] + SIDES["cp"]["done"]:
            self.assertFalse(q.irq(irq))

    def test_actual_btc_irq_handler_consumes_data_then_acknowledges_cp(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r0, =0xe000e100
    ldr r1, =0x00080000
    str r1, [r0]
    cpsie i
    ldr r0, =0x68001000
    movs r1, #0x51
    str r1, [r0]
1:  wfi
    b 1b
    .ltorg
    .balign 64
.thumb_func
rx_irq:
    ldr r0, =0x68006000
    ldr r1, [r0]
    adds r1, #1
    str r1, [r0, #4]
    ldr r0, =0x40000144
    movs r1, #0x10
    str r1, [r0]
    bx lr
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        self.addCleanup(q.close)
        q.command("irq_intercept_in /machine/core[1]")
        vectors = bytearray(0x100)
        struct.pack_into("<II", vectors, 0, 0x68017F00, 0x68004001)
        struct.pack_into("<I", vectors, (16 + 19) * 4, 0x68004041)
        q.write(0x68002000, vectors)
        q.write(0x68004000, code)
        q.write32(0x68006000, 0x12345678)
        q.write32(0x40080168, 0x68002000)
        q.write32(SYS + 0x140, 0x1000)
        q.write32(BTC + 0xA0, 0x40000)
        q.qmp("cont")
        q.write32(0x400800CC, 0x88)

        def await_word(address, expected):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if q.read32(address) == expected:
                    return
                time.sleep(0.01)
            self.fail(q.diagnostic(f"BTC guest did not write {expected:#x}"))

        await_word(0x68001000, 0x51)
        self.assertFalse(q.irq(102))
        self.assertEqual(q.read32(0x68006004), 0)
        q.write32(SYS + 0x140, 0x10)
        await_word(0x68006004, 0x12345679)
        await_word(SYS + 0x140, 0x10101001)
        self.assertTrue(q.irq(102))
        q.write32(SYS + 0x144, 1)
        self.assertFalse(q.irq(102))
        self.assertEqual(q.read32(0x68006000), 0x12345678)
        q.qmp("stop")
        self.assertNotIn("Lockup", q.stderr_path.read_text(errors="replace"))


if __name__ == "__main__":
    unittest.main()
