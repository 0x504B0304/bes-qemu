# TCG IOMMU MMIO dispatch fix for QEMU 8.2.10

This package adapts two upstream QEMU commits by Jim Shu (2026-01-28):

- [94c6e9cf04409e38563080348d6136879f91ffee](https://github.com/qemu/qemu/commit/94c6e9cf04409e38563080348d6136879f91ffee): pass `CPUTLBEntryFull` to `io_prepare()`.
- [854cd16e318eed12de2995014b28d9f374c64bf7](https://github.com/qemu/qemu/commit/854cd16e318eed12de2995014b28d9f374c64bf7): retain the actual memory section when an IOMMU translates CPU access into another address space.

The original section index is meaningful only in its owning address space.
Looking it up in the CPU's original space can dispatch a blocked MPC access
to an unrelated region. The observed consequence is a BusFault without
`INT_STAT`/address capture, even when the MPC requests RAZ/WI.

The fix stores the actual `MemoryRegionSection *` in the full TLB entry and
uses it for MMIO and plugin lookup. It retains QEMU's existing TLB/RCU
lifetime rules and does not expose the internal blocked memory to the guest.
The 8.2 adaptation combines the two commits and removes declarations from
`include/exec/exec-all.h`, where they lived before the upstream header split.
The code retains the licenses of the affected upstream files.

`manifest.json` pins the original and resulting SHA256 of all four files and
the patch itself. The offline integration tool validates this package and
the SD/MMC package completely before writing any upstream or owned source.
Exact already-applied files are no-ops; unknown versions or edits fail.

`tests/test_best1700_mpc_qtest.py` executes real secure/nonsecure CPU reads
and writes through the IOMMU, verifies RAZ/WI, SEC_RESP faults with captured
attributes, all memory aliases, permission revocation, reset, and NSC calls.
