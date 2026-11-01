# ARMv7M FPBv2 / DEMCR backport

This package adds the ARMv7-M FPBv2 execution comparators and DEMCR state
used by the native debugger probes. It exposes eight instruction address
comparators at 0xe0002000, supports the legacy bit-0 encoding used by older
firmware as well as the FPBv2 `REPLACE` field, and routes matches through
the architectural DebugMonitor exception. A comparator with `REPLACE=00`
and a configured `FP_REMAP` redirects a matching instruction fetch to that
comparator's remap-table word. The NVIC property `fpb-remap-base` selects
the implementation SRAM base and defaults to `0x20000000`. When MON_EN is clear
an FPB breakpoint leaves the vCPU halted for an external debugger. DEMCR
MON_STEP, MON_PEND and SDME state are modeled, including Secure/Non-secure
access checks.

FPB-owned breakpoints have a separate lifetime from GDB and architectural
CPU breakpoints. Reset, migration and comparator updates remove only the
breakpoints owned by FPB. The model does not add product-specific addresses;
those remain in the firmware package and debugger layer.

The patch is pinned to QEMU 8.2.10 and is applied by `tools/integrate_qemu.py`
after the DWT and cache-maintenance packages, because all three extend the
upstream ARMv7M support used by the BES cores.
