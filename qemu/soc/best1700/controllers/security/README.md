# BEST1700 page-spy v2

`best1700-page-spy` exposes one 4 KiB MMIO window per page-spy/PPC block.
The machine selects each SoC address and provides the secure alias. This
device does not occupy the intervening MPC windows.

Implemented register behavior:

| Offset | Meaning |
| --- | --- |
| 0x00 | Configuration bits 0..4 and 6..9; PPC interrupt clear bit 5 is a command and does not latch |
| 0x04 | Two region control nibbles: enable, read detection, write detection, detect once |
| 0x08 / 0x0c | Region 0 start and inclusive end |
| 0x10 / 0x14 | Region 1 start and inclusive end |
| 0x18 / 0x1c / 0x20 | Region 0 read-only hit status and captured addresses |
| 0x24 / 0x28 / 0x2c | Region 1 read-only hit status and captured addresses |

Disabling a region clears its captured state. Reserved configuration bits
read as zero. Unimplemented offsets log under `unimp`, read zero and ignore
writes. Reset clears all state; zero configuration is an emulator startup
default, not a measured silicon reset value.

This is currently a **register interface**, without interconnect transaction
observation or protection enforcement. No actual bus violations or page hits
are generated, captured status stays zero, and there is no active interrupt
source. PPC interrupt-enable/response bits and the nonsecure/MPC bypass bits
are retained configuration; they do not claim to grant or deny accesses.
Clearing the PPC interrupt is therefore currently a command with no pending
event. This must not be confused with TGU enforcement or a complete MPC/PPC
security model. Region detection, detect-once capture, interrupt routing and
master attribution require a separately wired transaction observer.

## Memory protection controllers

With `secure-boot=on`, the AP bus maps ten `best1700-mpc` devices. Each is a
thin subclass of QEMU's `tz-mpc`; the register interface, LUT access checks,
lockdown and fault capture come from that upstream implementation. The
subclass also invalidates both security indexes and lowers the IRQ on reset.
The integration tool selects `CONFIG_TZ_MPC` with the emulator's `ARM_V7M`
source bundle, including when unrelated default boards are disabled.
It also installs the pinned upstream TCG IOMMU dispatch fix from
`qemu/machine/patches/tcg-iommu-2026/`; without it, QEMU 8.2 can route a
blocked MPC access to the wrong MMIO section and lose fault capture.

| Bank | Registers | Protected views | Capacity |
| --- | --- | --- | --- |
| Flash0 | 0x40054000 / 0x44054000 | 0x10000000, 0x30000000, 0x08000000 | 16 MiB |
| PSRAM0 | 0x40056000 / 0x44056000 | 0x18000000, 0x38000000 | 64 MiB |
| SYS_SRAM0..7 | 0x4005a000 + bank × 0x2000; secure alias +0x04000000 | 0x02000000 and 0x22000000 + bank × 1 MiB | 1 MiB each |

All views of a bank point to the same filtered upstream and storage. The
flash primary base follows the AP machine property; the table shows its
default. LUT reset marks every block secure. A set bit marks a block
nonsecure: only transactions with matching security attributes pass, so
secure transfers to NS blocks are also rejected. `CTRL.SEC_RESP` chooses a
bus error or RAZ/WI. Only secure transactions can configure the controller;
normal NS register accesses return zero and ignore writes. Debug/image-load
transactions with unspecified attributes use the secure memory view.

`BLK_CFG` reports QEMU's ARM target page size, normally 1 KiB on this
machine; software must derive the geometry from the register. This is an
emulated geometry, not a measured silicon configuration. No firmware
partition is installed by the host. Secure firmware programs the LUTs,
including keeping SG gateways in secure flash blocks; Arm NSC instruction
fetches use secure transaction attributes.

The SYS_SRAM banks have explicit code/data aliases and independent backing.
They do not wrap the previous 128 MiB RAM scaffold. Core/AXI ITCM and DTCM
views remain behind the TGU. Dynamic CMU allocation that shares physical
storage between SYS_SRAM and TCM is not modeled. Shared AON RAM at 0x23c00000
is a separate region. The real CP view shares the AP PSRAM MPC, SYS_SRAM
MPCs and AP cross-TCM TGUs through the master-security policy described
below. BTH shared-SRAM ownership and mapping are documented separately in
`docs/BEST1700_BTH.md`; this does not establish complete protection for
every cross-subsystem path.

Current limits: filtering is active for the secure boot configuration from
reset; AON security-controller MPC enable/bypass controls and aggregate
interrupt routing are not yet modeled. Individual MPC status/capture and
named IRQ outputs are implemented, but the outputs are not connected to an
invented NVIC interrupt. DMA master security attribution still requires
controller-specific work: clients using unspecified attributes select the
secure index and raw host-pointer clients may bypass filtering. The legacy
direct nonsecure boot mode retains its unfiltered map.

NOR program/erase updates use a private address space over the flash die's
backing to dirty pages and invalidate translated instructions, independent
of the CPU's current MPC partition. That address space is not guest-mapped.

## eMMC master security

`best1700-sec` maps the AON security aperture at 0x40088000 (secure alias
0x44088000). NONSEC_SEC_ENG at offset 0x44 retains bits 0..3; bit 2 selects
the eMMC master's security attribute. Clear means Secure, set means
Nonsecure. Byte and halfword writes merge with the existing register.
Nonsecure CPU accesses are RAZ/WI. Reset clears the register; this is the
model's conservative initial state, not a measured silicon reset value.
Migration saves the registers and restores the eMMC output signal.

The master attribute applies to SDMA data, ADMA data and ADMA descriptors,
independently of the CPU issuing a command. These accesses use the normal
AP bus and pass through MPC/TGU protection with explicit attributes. They
are restricted to RAM, so an invalid descriptor cannot target device MMIO.
QEMU's common host has an optional attribute callback; other machines keep
their existing defaults. The eMMC tests execute a Secure guest to configure
permissions and inspect the actual DMA destination, including denied cases.

The eMMC, CP, GPU and AP HDMA master controls currently affect transactions.
Other AON security registers retain values without implementing their
interconnect effects. USB, display and BTH attribution remain unmodeled.
The common SDMA implementation still lacks a modeled bus-error interrupt
for denied memory accesses; memory protection prevents the access but
completion status alone does not prove that bytes were transferred.

## AP HDMA master security

In secure boot mode, the AP's two system HDMA controllers use NONSEC_DMA
at offset 0x48: bit 7 selects HDMA0 and bit 8 selects HDMA1. Clear means
Secure; set means Nonsecure. The same attribute applies to source reads,
destination writes and linked descriptor fetches, independently of the
issuing CPU's architectural security state. The normal bus still enforces
MPC/TGU permissions; this does not introduce an unrestricted RAM view.
The policy is read for each transfer and descriptor fetch, so subsequent
work observes guest changes without a separate translation cache.

Nonsecure software cannot change these secure-only registers. Reset returns
the master policy to Secure as the existing controller's emulator default.
RAZ/WI MPC denial can complete a DMA transfer without delivering data;
software must distinguish terminal count from successful memory delivery.
Tests use real M55 transactions to inspect both allowed and denied PSRAM
writes, protected descriptor/source reads, separate controller bits and
ownership after machine reset. qtest reads with unspecified attributes are
not used to inspect protected destinations.

This integration covers AP system HDMA in secure boot mode. CP/BTH local
DMA routing and legacy direct boot retain their previous attributes; their
master security integration requires separate validation.

## CP outbound security

With `secure-boot=on` and `cp-real-boot=on`, CP external RAM transactions
apply the AON security controller's M55C1 master policy before the existing
MPC/TGU check:

| Register | Field | Behavior |
| --- | --- | --- |
| NONSEC_DMA, offset 0x48 | bit 21 | Selected outbound attribute: 0 Secure, 1 Nonsecure |
| CORE_VTOR_SEL, offset 0x90 | bit 5 | 0 preserves the incoming CPU attribute; 1 applies bit 21 |

These fields are secure-only, support the controller's byte/halfword write
merging, and reset to zero as an emulator default. The policy changes the
bus transaction attribute. It does not change CP architectural security,
exception entry, VTOR banking or SVC routing. CP local TCM does not use this
external-bus override.

`best1700-master-gate` wraps the original PSRAM MPC, all eight SYS_SRAM MPCs,
and the AP ITCM/DTCM TGUs used by CP. Code/data aliases retain the same gate
and backing. It selects the downstream security index and preserves the
original access decision. In particular, the MPC still rejects both NS
accesses to Secure blocks and Secure accesses to NS blocks. The TGU retains
its existing contract, including its documented incomplete S-to-NS match
behavior.

Allowed RAM translations remain directly executable by TCG. Denied accesses
go back through the original protection controller with the selected bus
attribute, preserving MPC fault capture, including HNONSEC, and its
RAZ/WI or bus-error response. No unrestricted RAM alias is introduced by
the proxy.

Changes to either CP policy bit invalidate both incoming security indexes.
Downstream MPC/TGU invalidations are also forwarded to both indexes, so
translations cached before a policy or LUT change cannot retain access.
SEC reset and migration load invalidate the policy consumers; the registers
are migrated and notifier wiring is reconstructed with the machine. The
proxy has no independent mutable guest state and assumes the machine's
static device lifetime.

The implementation covers these CP external RAM paths. Other CP peripheral
paths, M55C0 controls, and the distinct BTH core/bridge security selectors
are not implemented by this policy. Hardware power-on values for the AON
security fields have not been established.
