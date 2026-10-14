# SC7061 register subset

The external device implements I2C register access, read-only identification,
cable-present and charging status, and the observed charging-disable bit.
It does not implement electrochemical capacity estimation.

`miband_charger_init()` receives separate probe revision and device ID bytes.
Both survive chip reset and ignore guest writes. Existing BEST1503 board
configuration keeps device ID `0x1e`. The BEST1502P machine exposes the
startup-only `charger-device-id` property (default `0x1e`, range 0..255).
This property also works through the generic hardware configuration channel.

BEST1502P routes cable changes to its PMU AC input and insertion/removal
latches. ISPI page0 register0x05 enables each edge, register0x40 enables the
unified charger slot, and IRQ42 asserts only when both enables permit a
latched edge. Writing status register0x5e acknowledges edge bits while
preserving the external AC input. Reset clears latches and enables, preserving
the cable input and IRQ connection. Guest battery workers consume this through
their own ISR; no GDB notification is required.

The BEST1502P `charger-state` readback reports `level-adjustable: false` until
percentage input is modeled. Hosts should allow cable changes while avoiding
presenting its internal nominal level as an adjustable or measured percentage.

Firmware-independent validation: set `MIBAND_TEST_QEMU` to the native QEMU
executable and run `python -m unittest tests.test_sc7061_qtest`.
