# BES AP I2C controller

`qemu/soc/best1503/controllers/i2c/miband_i2c.c` is shared by the
BEST1503-compatible machine and BEST1502P. It models the DesignWare-style
master register interface with explicitly attached touch/charger endpoints.

The interrupt register behaviour and AP wiring were compared with
`qemu-miwear` revision `ceac001066440c7ed1a4ff71b6e0923bc352cfbd`,
`hw/misc/bes_i2c.c` and `hw/arm/best1503.c` (GPL-2.0-or-later). The source
retains its GPL notice. This implementation deliberately does not add that
reference's fallback register-file slave at unknown I2C addresses.

## Implemented contract

- TX_EMPTY and RX_FULL are level sources based on FIFO counts/thresholds.
  CLR_INTR clears latched events, not active FIFO levels. Mask changes
  immediately recompute the IRQ. Disabled controllers do not assert IRQs.
- Each controller has independent 16-entry command/RX FIFOs. Commands execute
  on QEMU virtual timers, rather than completing in the submitting MMIO
  write. A synchronous completion could race an asynchronous driver's
  initialization of its completion status.
- The current timing subset is nine clocks per command byte at 400 kHz
  (22.5 us). SCL divider and clock-tree dependent rates, address/restart
  timing, clock stretching, arbitration and I2C DMA requests are not modeled.
  This is a bounded behavioural timing model, not cycle-accurate I2C.
- Unknown slave addresses produce ADDR_NOACK/TX_ABRT, flush pending commands
  and deliver the interrupt. They never receive a synthetic ACK or chip ID.
- STOP, abort, RX underflow/overflow and TX overflow have read-to-clear
  latches. RX threshold changes and FIFO reads recompute RX_FULL.
- Disable/reset cancels queued timers, clears controller FIFO/IRQ state and
  preserves the board's attached endpoints and IRQ wiring. Reset does not
  pretend that a user disconnected the physical charger.
- AP I2C0..6 route to IRQ 14,29,64,65,53,54,55. I2C7 MMIO remains available,
  but its IRQ is intentionally unconnected pending evidence.

The separate BEST1700 I2C implementation and BTH bus are unchanged.

## Native regression

Set `MIBAND_BEST1502P_QEMU` and `MIBAND_TEST_QEMU` to the native executable:

```text
python -m unittest tests.test_best1502p_i2c_qtest tests.test_sc7061_qtest -v
```

These 11 tests use synthetic guest images and cover controller isolation,
NVIC routing, masked/reasserted FIFO levels, delayed NACK, recovery after an
absent slave, repeated-start reads from the existing touch endpoint,
thresholds, overflow/underflow, queued-command cancellation, machine reset,
charger identity and physical cable/PMU edges.

Qtest does not advance virtual time automatically: clients must enable the
controller and step time before expecting a queued transfer result. Existing
charger and legacy touch tests were updated accordingly. Firmware-specific
startup traces and visual checks are held outside the public core.
