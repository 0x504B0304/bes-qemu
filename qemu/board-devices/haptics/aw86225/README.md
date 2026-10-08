# AW86225 candidate haptic controller

This board device attaches to an ordinary QEMU I2C bus. The Configurable board
profile owns its BTH I2C1/0x58 connection, active-low GPIO68 reset, and LRA
scenario (240 Hz, 4000 mV). The original firmware polls the device; no physical
IRQ pin has been confirmed, so the board profile leaves it disconnected.
The active-low interrupt output is independently usable and tested.

The model supports identity reads, the 0xaa software-reset command,
configuration registers, sequential register transactions, and a 4 KiB
RAM data port with an incrementing address and RAMINIT access gating. The guest
uploads its own waveform bytes; no firmware waveform is compiled into QEMU.
Wave records contain inclusive big-endian start/end addresses after a version
byte. Sequence bit 7 selects a wait of 128 samples. Loop nibbles select finite
repetition or an infinite loop requiring STOP/reset. Sample-rate codes 0/2/3
select 48/24/12 kHz. Playback completes on virtual time, sets the DONE latch,
and updates the interrupt mask/line; reading SYSINT clears the latch. STOP
cancels the timer without fabricating a completion.

Continuous F0 calibration produces big-endian frequency measurements after
its timer expires, from the configured LRA frequency. Voltage detection derives
its ADC result from the supply scenario and completes after 20 ms. Power-on
offset detection clears its GO bit after 10 us. Factory oscillator trim is a
read-only device scenario. These are behavioral timing models, not electrical
or acoustic simulations. RTP streaming, trigger playback, detailed protection
faults and analog wave output remain unimplemented. Unsupported writes NACK;
unsupported reads return 0xff and log the gap. Migration is explicitly disabled.

Read-only QOM counters `ram-writes`, `ram-reads`, `plays`, `completions` and
`calibrations` expose actual I2C/timer activity, and reset with the device.
`trace-bus=on` optionally logs register traffic (excluding RAM bytes).
The device neither implements RPMsg/socket services nor changes their results.
The original sensor driver, device registration and `vibratord` own that path.

Run `tests.test_aw86225_qtest` with `MIBAND_BEST1700_QEMU` pointing to the
built executable. Tests cover configuration bursts, RAM address crossing and
readback, bus isolation, finite/infinite and mixed wait sequences, timed
measurements, interrupt masking/clearing, STOP, GPIO/software reset and whole
machine reset. `tests.test_aw86225_config` checks profile domains and wiring.
