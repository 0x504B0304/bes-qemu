"""Verify CP/BTH lanes of the shared SYS/BTH interrupt controller.

The original processors use lanes 2 and 3 with distinct NVIC routes from
AP/BTH lanes 0 and 1. Tests observe real NVIC inputs without a peer service.
"""

from contextlib import contextmanager
import os
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


SIDES = {
    "cp": dict(core=1, set=0x40000098, clear=0x4000009C,
               mask_set=0x400001F4, mask_clear=0x400001F8,
               tx=0x40000224, rx=0x400000EC,
               rx_irq=(96, 97), done_irq=(94, 95)),
    "bth": dict(core=2, set=0x50000134, clear=0x50000138,
                mask_set=0x50000168, mask_clear=0x5000016C,
                tx=0x50000170, rx=0x50000174,
                rx_irq=(75, 76), done_irq=(67, 68)),
}


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class CpBthIpcQtest(unittest.TestCase):
    @contextmanager
    def observer(self, side):
        machine = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"],
            machine_options=",cp-real-boot=on,bth-real-boot=on")
        try:
            local = SIDES[side]
            peer = SIDES["bth" if side == "cp" else "cp"]
            machine.command(f'irq_intercept_in /machine/core[{local["core"]}]')
            yield machine, local, peer
        finally:
            machine.close()

    def assert_irqs(self, q, local, *, rx=(False, False), done=(False, False)):
        for number, expected in zip(local["rx_irq"], rx):
            self.assertEqual(q.irq(number), expected, f"RX IRQ {number}")
        for number, expected in zip(local["done_irq"], done):
            self.assertEqual(q.irq(number), expected, f"DONE IRQ {number}")

    def test_original_lane_bits_route_rx_and_ack_done_to_both_processors(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                # Original firmware uses channels 0/1 shifted by two lanes.
                for registers in (local, peer):
                    q.write32(registers["mask_set"], 0x000C000C)
                q.write32(peer["set"], 0x000C0000)
                self.assertEqual(q.read32(local["rx"]), 0x000C000C)
                self.assert_irqs(q, local, rx=(True, True))
                q.write32(peer["clear"], 0x00040000)
                self.assertEqual(q.read32(local["rx"]), 0x00080008)
                self.assertEqual(q.read32(peer["tx"]), 0x00040004)
                self.assert_irqs(q, local, rx=(False, True))
                q.write32(peer["clear"], 0x00080000)
                self.assert_irqs(q, local)

                q.write32(local["set"], 0x000C0000)
                q.write32(local["clear"], 0x000C0000)
                self.assertEqual(q.read32(local["tx"]), 0x000C000C)
                self.assert_irqs(q, local, done=(True, True))
                q.write32(local["clear"], 4)
                self.assert_irqs(q, local, done=(False, True))
                q.write32(local["clear"], 8)
                q.write32(local["clear"], 0x000C0000)  # Empty IND clear.
                self.assert_irqs(q, local)

    def test_masks_retain_pending_state_and_do_not_signal_ap_lanes(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                q.write32(local["mask_set"], 0x00030003)
                q.write32(peer["set"], 0x00030000)
                q.write32(local["set"], 3)
                self.assert_irqs(q, local)
                q.write32(peer["set"], 0x000C0000)
                self.assertEqual(q.read32(local["rx"]), 0x0003000F)
                self.assert_irqs(q, local)
                q.write32(local["mask_set"], 0x000C000C)
                self.assert_irqs(q, local, rx=(True, True))
                q.write32(local["mask_clear"], 4)
                self.assert_irqs(q, local, rx=(False, True))
                self.assertEqual(q.read32(local["rx"]), 0x000B000F)
                q.write32(local["mask_set"], 4)
                q.write32(peer["clear"], 0x000C0000)
                self.assertEqual(q.read32(local["rx"]), 0x00030003)
                self.assertEqual(q.read32(local["tx"]), 0x00030003)
                self.assert_irqs(q, local)
                if side == "bth":
                    # Existing AP lanes continue to assert their own inputs.
                    for irq in (73, 74, 65, 66):
                        self.assertTrue(q.irq(irq))

    def test_cp_lanes_do_not_raise_ap_nvic_inputs(self):
        q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"],
            machine_options=",cp-real-boot=on,bth-real-boot=on")
        self.addCleanup(q.close)
        q.command("irq_intercept_in /machine/core[0]")
        q.write32(SIDES["cp"]["mask_set"], 0x000F000F)
        q.write32(SIDES["bth"]["set"], 0x000C0000)
        q.write32(SIDES["cp"]["set"], 0xC)
        for irq in (94, 95, 96, 97):
            self.assertFalse(q.irq(irq))
        q.write32(SIDES["bth"]["set"], 0x00030000)
        q.write32(SIDES["cp"]["set"], 3)
        for irq in (94, 95, 96, 97):
            self.assertTrue(q.irq(irq))

    def test_machine_reset_clears_latches_and_does_not_modify_mailboxes(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                # These are the original CP/BTH peer-discovery slots.
                q.write32(0x23C0EFA0, 0x23C07000)
                q.write32(0x23C0EFA4, 0x23006320)
                for registers in (local, peer):
                    q.write32(registers["mask_set"], 0x000C000C)
                    q.write32(registers["set"], 0x000C000C)
                self.assert_irqs(q, local, rx=(True, True), done=(True, True))
                self.assertEqual(q.read32(0x23C0EFA0), 0x23C07000)
                self.assertEqual(q.read32(0x23C0EFA4), 0x23006320)
                q.qmp("system_reset")
                q.qmp("query-status")
                self.assert_irqs(q, local)
                for registers in (local, peer):
                    for field in ("set", "clear", "mask_set", "mask_clear", "rx", "tx"):
                        self.assertEqual(q.read32(registers[field]), 0)


if __name__ == "__main__":
    unittest.main()
