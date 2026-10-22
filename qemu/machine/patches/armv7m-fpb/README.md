# ARMv7M FPBv2 / DEMCR backport

This package adds the experimental Cortex-M33 FPBv2 execution comparators
and DEMCR storage used by the native debugger probes. It exposes eight
instruction address comparators at 0xe0002000, routes matches through the
architectural DebugMonitor exception, and stores MON_EN/SDME without
implementing halting debug or flash remapping.

The patch is pinned to QEMU 8.2.10 and is applied by `tools/integrate_qemu.py`
after the DWT and cache-maintenance packages, because all three extend the
upstream ARMv7M support used by the BES cores.
