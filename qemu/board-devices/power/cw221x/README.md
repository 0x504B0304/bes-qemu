# CW221X-compatible fuel gauge

This limited I2C slave follows the firmware's fuel-gauge transactions. The
board chooses its bus/address and a static battery scenario in millivolts,
integer percent and tenths of a degree Celsius. The Configurable candidate uses
I2C0/address `64h`; the firmware also contains an OM70X0X alternative at `38h`.
Physical part identification is still unconfirmed.

| Register | Implemented behavior |
| --- | --- |
| `02h–03h` | Big-endian voltage; guest converts `raw * 5 / 16` to mV |
| `04h–05h` | Big-endian capacity; `raw / 256` is percent |
| `06h` | Temperature; `raw * 5 - 400` is 0.1 C |
| `08h` | Mode `30h` restart, `F0h` sleep, `00h` active |
| `0Ah` | Retained GPIO configuration; pins are not modeled |
| `0Bh` | Retained SOC alert configuration; bit 7 marks profile updated |
| `0Eh–0Fh` | Zero current for the static environment |
| `10h–5Fh` | Guest-written battery profile, retained over sleep/active |
| `A4h–A5h` | Cycle count from profile `50h–51h`; guest divides by 16 |
| `A6h` | SOH from profile `52h` |
| `A7h` | Ready bits `0Ch` when active and profile-updated |

The one-byte selector is retained across repeated START and STOP, and reads
or writes advance it. On device reset the profile is empty, SOH starts at
100 and mode is sleep. These are explicit simulation defaults, not a claimed
physical reset table. The guest uploads its own profile and starts conversion;
there is no copied firmware battery table in the model. Activation is immediate
and the measurements remain fixed; chemistry, aging, alert interrupts and
conversion latency are not modeled.

No exact chip-ID/version byte is established by the analyzed driver: it only
prints the ID and does not compare it. Unimplemented reads, including `00h`
and `ABh`, therefore report a diagnostic and return `FFh`. This is an explicit
unsupported-model response, not a fabricated chip identity. Unknown writes
and unsupported mode values NACK. Measurement writes cannot change the
configured environment. Firmware register-dump reads of reserved addresses
also return `FFh` with a diagnostic.

The driver creates `/dev/charge/batt_gauge` itself. Its capacity callback first
opens `/dev/charge/batt_charger`, so the companion charger must also initialize.
The startup low-power guard reads voltage (`BATIOC 0E04h`), capacity (`0E07h`),
temperature (`0E0Ah`) and current (`0E05h`). Capacity greater than 1 percent
permits normal startup. No device nodes, battery cache or return values are
patched by the model.
