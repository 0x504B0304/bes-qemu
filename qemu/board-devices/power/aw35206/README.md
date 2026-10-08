# AW35206-compatible load switch

This is a limited I2C register endpoint derived from firmware driver
transactions, not a claim that a physical board contains this exact part.
The board profile selects its bus and seven-bit address; neither belongs
to the SoC controller or this device model.

The simulated identity is `32h`, one of the driver's AW35206-compatible
identities. Identity `20h` belongs to its separate WS4636C branch and is not
implemented here. The device implements only the observed one-byte register
selector and one-byte register data transactions, including a repeated START
between selector write and data read.

| Register | Modeled behavior |
| --- | --- |
| `00h` | Read-only identity `32h`; writes do not change identity |
| `02h` | Six load-switch enable bits, retained across I2C transactions |
| `69h` | Writing `AEh` clears enable bits; identity remains unchanged |

Reset starts all modeled enables clear. This is a conservative simulation
initial state, not a verified power-on reset table. Enable bit `n-1` controls
channel `n` in the register model. No analog rails, pin gating, fault flags,
power-good signals, timing, or downstream-device power domains are simulated.
The full register map and hardware burst behavior have not been confirmed.

Unknown register writes, extra write bytes, and unrecognized reset keys
return a data NACK and emit a diagnostic. Unknown register reads and extra
read bytes return `FFh` and emit a diagnostic because the QEMU slave receive
interface has no data-NACK result. These are explicit unsupported-model
responses, not claims about the physical chip. Reads do not advance the
selector; a new selector write starts a new transaction. A controller STOP
retains register state and the last selector, while device reset clears both.

The model does not patch probe results, redirect firmware functions, or
change the controller's missing-device NACK behavior. The firmware must use
the normal I2C bus to probe, reset, read, and write this device.
