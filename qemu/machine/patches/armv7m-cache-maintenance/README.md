# ARMv7M cache maintenance fast path

This emulator optimization targets QEMU 8.2.10 after the pinned TCG IOMMU
package. It is a local optimization, not an upstream cache implementation.
`tools/integrate_qemu.py` validates all packages as an ordered stack before
writing any installed source. Exact pristine, partial and final states are
accepted; unknown local edits fail without modifying the checkout.

QEMU models coherent guest RAM and already treats cache and branch predictor
maintenance writes as NOPs. With MTTCG, every word of an ordinary cache range
otherwise acquires the global I/O lock and competes with the other AMP cores
and display work. The optional `MemoryRegionOps.write_is_nop` callback lets
TCG finish these existing NOP writes without acquiring that lock.

The NVIC callback accepts only privileged 32-bit writes to its ten existing
cache/branch maintenance registers. Other sizes, offsets and unprivileged
accesses use the original dispatch and fault paths. It reads no mutable CPU
or device state. NVIC tracing remains available. Reads, interrupt enables,
pending bits, MPU/SAU configuration, reset and barrier instructions are
unchanged. There is no firmware entry hook, synthesized IRQ or cache-size
claim.

Native guest tests run the maintenance loop under MTTCG and icount, verify
ordinary NVIC pending-bit writes, and prove that an unprivileged maintenance
write still raises the original precise BusFault/HardFault. Integration tests
cover pristine, partial, repeated and conflicting patch stacks.

```sh
MIBAND_BEST1700_QEMU=/path/to/qemu-system-arm python3 -m unittest \
  tests.test_armv7m_cache_maintenance tests.test_qemu_cache_backport \
  tests.test_source_integration
```

These tests establish execution and privilege contracts. Firmware startup,
frame timing and touch responsiveness require separate real-firmware runs;
no frame-rate improvement is implied by the synthetic tests.
