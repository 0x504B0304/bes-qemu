# BEST1503 PMU power removal

The embedded PMU uses ISPI chip select 3. On page 0, register `0x4f`
shares two independent commands: bit 0 requests soft poweroff and bit 5 starts
GPADC. The firmware ROM's shutdown sequence repeatedly sets bit 0, then falls
back to system reset if power was not removed. A reset is not a successful
poweroff.

The model queues QEMU's standard `guest-shutdown` request once per power cycle.
The MMIO callback does not reset vCPUs or destroy their devices. With
`-action shutdown=pause`, QEMU remains in `shutdown`; `cont` cannot power it on.
An explicit machine reset clears the poweroff latch and command bit.

ADC-only writes, other chip selects and other pages retain their existing
behavior. There is no firmware PC check, guest function hook, charger-state
requirement or host-injected reset in this path. Watchdog and complete PMU power
sequencing are separate capabilities.

Synthetic regression, with a development QEMU selected in `MIBAND_TEST_QEMU`:

```sh
python -m unittest tests.test_best1503_poweroff_qtest -v
```

The tests check the QMP event reason, reject a reset event, exercise the shared
ADC register, exclude other pages/selects, and verify reset/rearm.

## Static board analog inputs

`gpadc-mv0` through `gpadc-mv6` configure a static electrical voltage on a
GPADC input before machine realization. Values are millivolts in `0..1800`;
the default `UINT32_MAX` leaves the existing sample unchanged. Inputs require
`gpadc-model=on` and cannot overlap the board's battery/thermistor channels
when `charger-model=on`. Live QOM changes are rejected. Reset clears ADC
completion state while retaining the wired voltage.

The voltage uses the existing calibrated ADC transfer and is read through
PMU ISPI data registers, not through a firmware board-ID override. No firmware
addresses, product strings or board-specific thresholds enter the core.
The versioned board description exports these values via the existing typed
hardware-config ABI 2 machine properties.

`tests.test_best1503_gpadc_inputs_qtest` exercises bus samples, neighboring
channels, voltage limits, reset retention, live-change rejection and exclusion
of dynamic power inputs using only a synthetic guest.
