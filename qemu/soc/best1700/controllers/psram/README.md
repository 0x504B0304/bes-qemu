# BEST1700 PSRAM controller

The controller owns a 64 KiB MMIO window. Its manager registers begin at
offset zero and its digital PHY registers at `0x8000`. The machine maps the
same controller at `0x40320000` and its secure alias `0x44320000`.

The implemented contracts are the digital PHY power handshake, eight-word
manager FIFOs, timed command completion, mode-register transport, frequency
point status, sleep/wakeup state and controller reset. Command and PHY
completion use a one-microsecond virtual-time delay; this is a transaction
model and does not specify physical clock or analog timing.

PHY control bits 11 and 12 at `+0x8040` enable the automatic low-power controls.
The exposed power state at `+0x8044[28:25]` reaches 3 after that request. Clearing
either control cancels completion and clears the state. Rewriting unrelated
sample-selection bits does not restart the handshake. This models the digital
startup sequence observed in the current firmware. Its register layout follows
the v6 PHY: `+0x8050` has bypass/transition controls in bits 30/31, and `+0x8054`
is DLL delay/lock status. DLL lock requires the PHY power handshake and released
reset with an enabled clock. Drive bits 28/29 at `+0x8050` select manual reset
and clock control through bits 19/16 at `+0x8058`; otherwise they follow the
automatic power state. Lock is delayed and is lost immediately on clock disable,
reset assertion or loss of power. `dll-delay-taps` is an ideal-clock compatibility
parameter (default 16), not a measured silicon value. Its endpoints 0 and 63
report the all-zero/all-one saturation flags. Analog waveform calibration and
other PHY revisions are not supported.

The analog register endpoint can be attached to an ISPI chip select. Register
`0x51` exposes the same DLL status as MMIO; register `0x50` shares the DLL
configuration word, and register `0x52[12:11]` shares the range selection.
Changing the range restarts lock acquisition. The ISPI controller dispatches
transactions to this endpoint without owning PSRAM register semantics.

Package mode registers use `Best1700PsramEndpoint`. The controller contains no
vendor identity or firmware-specific mode-register values. Without an endpoint,
mode reads return open-bus words and emit an unimplemented diagnostic. The
machine owns the existing array RAM; manager array transfers are not yet
connected to it. Unsupported commands and mode transfers longer than the FIFO
emit a diagnostic and retain busy until FIFO clear or reset. A mode write with
too few queued words waits for the remaining words without discarding data.

The isolated `tests.test_best1700_psram_qtest` suite verifies power sequencing,
secure/nonsecure register aliases, FIFO limits and lifetime, command busy
protection, frequency acknowledgement, sleep/wakeup and cancellation on reset.
