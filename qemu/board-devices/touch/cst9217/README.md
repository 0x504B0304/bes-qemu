# CST9217 candidate

An I2C board device, independent of SoC memory addresses and firmware hooks.
Board JSON selects its bus, seven-bit address, active-low GPIO and geometry.
`host-input` uses the shared absolute QMP/VNC input bridge. One contact is
supported: press, motion and release become event 6, 7 and 0 in the ten-byte
`D000` report. Coordinates are packed into bytes 1..3, byte 5 is one contact,
and byte 6 is `ab`. Reading retains the report; `00 05 00` acknowledges it.
Queued reports reassert the interrupt after a one-millisecond virtual delay,
so the masked guest worker can finish before the next edge. Motion is coalesced.
Reset drops queued reports. GPIO reset preserves the physical host contact
and scans it again on release, including a finger lifted during reset. This
keeps a wake press from becoming a stuck touch when the driver resets the
controller before the host releases the contact. A full machine reset clears
input state; unrealize unregisters the host handler.

The primary driver of the qualified S5 image probes I2C1/0x5a and attaches
GPIO86. The modeled `D1xx` mode commands, `0002` mode readback, `D220/D224`
identity and `D1FC/D200/D204/D208/D21C` metadata satisfy its normal startup.
Identity `9217`, module 1 and version/checksum zero are an explicit candidate
scenario, not a readout of a physical controller. Boot probe `A002` and chip
identity at bank 1/address 077c are limited responses. `D105` selects deep
sleep and ignores contacts; it does not generate a synthetic wake press.
`D104` selects gesture sleep. Two completed taps generate one `D000` report
with gesture byte 4 equal to `10`, no active contact in byte 5, and marker
`ab` in byte 6. The original guest reads and acknowledges that report before
its power manager resumes the device. A single tap, a held contact or a drag
does not generate a gesture. Reset and sleep-mode commands discard partially
recognized gestures and queued reports. `2601` retains the earlier optional
gesture-status read behavior; S5 uses `D000`/`000500`.

The pointer recognizer uses virtual time. `tap-duration-ms` (250),
`tap-gap-ms` (500) and `tap-slop` (24 pixels on each axis) are configurable
host input policy defaults, not measured physical-controller timing or
confirmed chip firmware constants. Both taps must finish within the duration
limit and stay within the movement bound; the second must start within the
gap and position bounds. Pending gestures remain until acknowledgment; new
gestures cannot overwrite them. The guest's double-tap-to-wake setting must
also be enabled. Web power-key fallback is not native gesture-wake evidence.

Flash programming, factory diagnostics, ESD simulation and palm recognition
are not modeled.
Unsupported reads return zero; oversized writes NACK. Migration is unsupported.

Verification: `MIBAND_BEST1700_QEMU=/path/to/qemu-system-arm python3 -m unittest
tests.test_cst9217_qtest`. This exercises the actual controller and GPIO input,
including QMP pointer conversion, queued press/move/release, acknowledgment
and held/released/new contacts across GPIO reset, plus machine reset. Gesture
tests cover read/ACK, rejection and boundary cases, and cancellation on mode,
GPIO and machine reset. The real Web/VNC UI is separately validated.

S5 runtime qualification (2026-10-07, AP SHA-256
`f3b3b4b9fd673c0fa263a4917b8bc1bb7fdfa48c7f5dddf7a9ec4f2f007fb999`):
with the original double-tap setting enabled and a 15-second dimmer, an
isolated MTTCG instance completed a cold process restart and reached the
native watchface. Physical QMP double taps in gesture sleep produced the
report above, GPIO86, guest I2C read/ACK, original GPIO reset and restored
framebuffer, without power-key input. Two additional sleep/wake cycles and
ordinary swiping after wake passed; single taps, held contacts and drags
did not wake it. Nine controller qtests and sixteen related I2C/GPIO/MTTCG
tests passed. These observations qualify the controller path under the
existing startup adapters, not natural boot or browser wake behavior.
A second cold restart stopped at an original BTH RPMsg TX-buffer timeout
while CP waited for BTC; its first-fault scene was retained. Full restart
reliability, Web power-key fallback removal and broader power management
remain separate, unqualified work.
