# Validation scope

The core builds without Studio or product data. `tools/check_project.py` runs
unittest-based offline cases and the runtime ABI tests when `--qemu` is supplied.
Some legacy native/script tests have their own command-line interfaces and are
not counted by that entry point. A skip is not a pass of the skipped behavior.

ABI regression uses an independently generated synthetic guest and board. It
checks real QEMU boot setup, startup immutability, invalid ranges/types, unknown
fields, mismatched bindings and array lengths. UART/DMA/timer/eMMC regressions
use synthetic programs and temporary storage, without commercial firmware.

`tests/guest/config_perf.S` is the deterministic performance workload: 12 million
SRAM read/write and timer MMIO iterations, followed by a real guest semihosting
exit. The before/after comparison uses the same board data, binary guest, TCG
mode, virtual RTC and alternating trials. Product-bearing invocation details
and hashes belong to Studio's validation report rather than this core tree.

Performance evidence currently covers that CPU/memory/MMIO workload. Unchanged
native GPU and storage algorithms are corroborating source evidence, not proof
that every end-to-end workload or supported host has identical performance.
