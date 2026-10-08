# M55 memory system controls and TCM gates

The device exposes one page of MemSysCtl registers and two IOMMU ports.
Port 1 protects ITCM, port 2 protects DTCM. Each downstream region starts
at TCM offset zero; CPU, secure and AXI aliases must all use the upstream
port, never the unfiltered RAM. Downstreams can contain unmapped holes.

The register layout, access rules and block encoding follow the public
Cortex-M55 Technical Reference Manual (101051, r1p1), Arm CMSIS
`core_cm55.h` and Trusted Firmware-M `tgu_armv8_m_drv.c`.

All supported registers require aligned 32-bit privileged accesses.
Unprivileged accesses return a bus error. The `armv7m` QOM link supplies
the core's current AIRCR.BFHFNMINS value; it may be wired before the linked
container creates its CPU. Debugger/image-loader transactions with
unspecified attributes may access the registers.

| Offset | Registers | Nonsecure privileged behavior |
| --- | --- | --- |
| 0x000 | MSCR | RAZ/WI when BFHFNMINS=0; otherwise shared with Secure |
| 0x004 | PFCR | RAZ/WI when BFHFNMINS=0; otherwise shared with Secure |
| 0x500/0x504/0x510 | ITGU CTRL/CFG/LUT | Always RAZ/WI |
| 0x600/0x604/0x610 | DTGU CTRL/CFG/LUT | Always RAZ/WI |

MSCR retains ICACTIVE, DCACTIVE, DCCLEAN and FORCEWT when the corresponding
cache is present. CPWRDN and the ECC fields read zero: cache power states,
ECC and automatic dirty-line tracking are not modeled. PFCR retains its
ten defined bits. TCG does not simulate caches or prefetch requests, so
these controls do not change timing or generate memory transactions.
The `icache-present` and `dcache-present` properties default to true;
absent-cache fields are RAZ/WI, including all of PFCR without a data cache.
Other registers in this page remain unimplemented and RAZ/WI for
privileged accesses, including ITCMCR, DTCMCR and PAHBCR.

MSCR resets its implemented ICACTIVE/DCACTIVE bits to one and its other
fields to zero. PFCR uses the detailed r1p0/r1p1 field resets:
MAX_OS=2, MAX_LA=6, MIN_LA=2, ENABLE=1 (0x165). The same r1p1 manual's
register summary gives the conflicting value 0x61; the emulator follows
the detailed field table, not a measured hardware value. Tests verify
access rules using explicitly written values and verify that system reset
restores the initial value without treating 0x165 as a hardware oracle.

TCM gate behavior:

- ITGU CTRL/CFG/LUT: page offsets 0x500/0x504/0x510.
- DTGU CTRL/CFG/LUT: page offsets 0x600/0x604/0x610.
- CFG reports PRESENT, log2(block count), and log2(block bytes)-5.
- Each of the sixteen LUT words describes 32 blocks; a set bit allows
  nonsecure transactions. Reset clears both tables and sets each CTRL to 3.
- CFG is read-only. Reserved words read zero and ignore writes. Nonsecure
  writes cannot change CTRL or LUT permissions, regardless of BFHFNMINS.
- Changes invalidate the affected IOMMU translations, including CPU TLB
  mappings. A rejected access returns a bus transaction error and cannot
  modify the backing RAM.

This implementation deliberately has a limited protection contract:
nonsecure transactions cannot access secure blocks. Secure transactions
are currently allowed to both kinds of block. The exact rejection behavior
of secure transactions to nonsecure blocks, and the DBFEN/DEREN-dependent
fault/error response, remain unmodeled. CTRL retains its two defined bits;
neither value disables filtering. LOCKITGU/LOCKDTGU inputs are not modeled.
This is not a complete M55 TGU model.

The regression tests exercise CPU transactions through the core, secure and
AXI address views. DMA controller security attribution is not modeled yet:
some controllers still issue unspecified transaction attributes, which the
gate treats as secure for debugger and image-loader compatibility. These
controllers therefore do not yet provide the same protection guarantee as
the CPU tests. Their eventual attributes must follow controller/channel
security configuration, rather than being inferred from an address alias.

PSRAM, flash and the eight SYS_SRAM banks have separate MPCs when secure
boot is enabled. Each bank's aliases share one MPC upstream, so changing
SAU attributes on an alternate address view cannot bypass its LUT. See
`../security/README.md` for the memory map and remaining bus limitations.

Default geometry is 512 x 4 KiB ITCM and 512 x 8 KiB DTCM when the board
provides 2 MiB and 4 MiB downstream windows. Those are emulated capacities,
not a claim about configuration registers measured on physical hardware.

Register references:

- https://developer.arm.com/documentation/101051/0101/System-registers/MSCR--Memory-System-Control-Register
- https://developer.arm.com/documentation/101051/0101/System-registers/PFCR--Prefetcher-Control-Register
- https://github.com/ARM-software/CMSIS_5/blob/develop/CMSIS/Core/Include/core_cm55.h
- https://github.com/TrustedFirmware-M/trusted-firmware-m/blob/main/platform/ext/target/arm/drivers/tgu/tgu_armv8_m_drv.c

The IOMMU filtering design follows QEMU's GPL-2.0-or-later `tz-mpc.c`;
the device retains that license and attribution.
