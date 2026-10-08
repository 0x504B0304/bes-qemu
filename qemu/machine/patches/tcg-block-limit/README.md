# TCG translation block limit

This pinned QEMU 8.2.10 integration adds the startup-only accelerator option
`-accel tcg,thread=multi,max-insns-per-tb=16`. The accepted range is 1..512;
omitting it preserves the upstream limit of 512. It controls ordinary block
generation, leaving explicit instruction counts for I/O replay, debugger
single-step and icount unchanged. It does not change guest clocks, register
semantics, IRQ delivery or firmware timeouts. Live changes are rejected because
already cached blocks were compiled with the startup limit.

A smaller limit can reduce overlapping cold translations in straight-line MMIO
save/restore code. It trades that benefit for more dispatches in warm code.
Multi-core native startup experiments identified this cost in BTC context saving;
16 is an opt-in tuning value, not a device register or a guaranteed deadline.
Measure startup, steady-state GUI and long-run behavior before selecting a
value. Single-instruction blocks are not an equivalent performance policy.

Run configurations can set `execution.max_insns_per_tb` in MTTCG mode. The S5
launcher exposes `--tcg-max-insns 16 --execution mttcg`. Existing configurations
retain their prior command lines. This option alone does not remove diagnostic
hooks installed by the UI launcher or establish native whole-device stability.
