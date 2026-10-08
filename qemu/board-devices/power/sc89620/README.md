# SC89620-compatible charger

This is a limited I2C slave selected by the board profile. The Configurable
firmware probes I2C0/address `6Bh`, reads register `38h`, and requires `58h`.
A second probe checks the same byte's part-number bits `[6:3]` equal 11.
This proves the firmware accepts this candidate, not that physical units
necessarily contain this part.

The model retains the observed control register bank `00h–1Ah`, interrupt
mask `23h`, and extended configuration `40h`, `90h–92h`. A write of bit 7
to `17h` resets control state and self-clears. In `16h`, bit 5 controls
charge enable. Reset starts control and fault flags clear as a conservative
simulation state; this is not a verified physical power-on register table.

| Register | Implemented behavior |
| --- | --- |
| `38h` | Read-only accepted candidate identity `58h` |
| `1Eh` bit 6 | Board-configured VBUS online state |
| `1Eh` bits 5:3 | Phase 0 when inactive; phase 1 when online and charge-enabled |
| `29h` | Battery ADC byte; the driver converts `18 * (raw + 1)` to mV |
| `1Dh`, `1Fh` | No simulated fault flags |

The firmware maps phases 1–5 to charging; phase 2 additionally signals a
special low-power state. This subset uses phase 1 for its nominal charging
state and does not simulate analog charging stages. Voltage quantizes down
in 18 mV steps. VBUS and voltage are static environment properties shared
with the board's fuel-gauge scenario; controls do not change battery charge.
No interrupt pin, watchdog expiry, charge timer, thermal limits or real
fault injection is modeled. Status and ADC writes are ignored. Unimplemented
register writes NACK; reads return `FFh` and log a diagnostic.

The driver uses a one-byte register selector followed by byte data, including
selector STOP/repeated START reads. The selector is retained until the next
selector write, and consecutive bytes advance it. Device reset resets the
transaction parser; register reset leaves the current transaction intact.
The guest performs probe, configuration and device-node registration itself.
