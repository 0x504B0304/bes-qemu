# ARMv7M DWT backport

This package installs the minimal DWT `CTRL` and `CYCCNT` registers required
by the BEST1700 firmware's timeout and progress accounting. `CYCCNT` advances
on enabled reads, which preserves guest forward progress without tying the
firmware polling path to host wall-clock timing.

The patch is pinned to QEMU 8.2.10 and is applied by `tools/integrate_qemu.py`
with exact source hashes. It is intentionally separate from the board and SoC
models because it extends the upstream Cortex-M CPU support used by all AMP
cores.
