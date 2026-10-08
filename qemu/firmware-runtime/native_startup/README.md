# Native startup adapters

The S5 diagnostic launcher accepts `--prepare-nonsecure-stack` to prepare the
nonsecure stack before its original TEE-to-AP handoff. It is off by default.
This is a software compensation for the observed cold-start window in which
AON Timer0 can interrupt AP before AP executes its first `MSR MSP`.

The resident debugger verifies both image hashes, the manifest-bound source
and configuration hashes, the original instruction bytes, the AP target and
stack literal, and the cold thread-mode context. It executes one existing TEE
`MSR MSP_NS,R0` instruction, stops before the next instruction, verifies the
stack and unrelated registers, restores R0/PC, then disables both breakpoints.
The original timer interrupt handler and AP startup continue normally. Every
intervention is recorded in `ap-ui-hooks.jsonl`; a rejected guard stops the VM
and retains the scene. Each process created by Web restart installs fresh
one-shot breakpoints on the same persistent disk.

This path uses no second debugger, binary patch, writable banked GDB register,
interrupt mask, synthetic IRQ response, or device-model address. Profile-bound
addresses live in the firmware adapter configuration. Report its results as
hooked startup. Passing with this option does not prove natural boot, the
omitted loader's initial stack contract, timer completeness, or BLE readiness.
Retire the adapter once that startup contract or a demonstrated model defect
has been resolved and the unassisted path passes the same tests.

Validation covers rejected identities and instruction/context guards, execution
of the original MSR, full context restoration, disabled probes, and a controlled
interrupt at the cold handoff. It does not guarantee later panel initialization:
a separate observed failure occurs when the native DSI delayed-sleep callback
makes its window read-only before the panel sends sleep-out through an unguarded
low-level call. Keep that MemManage failure stopped and investigate the native
power-management sequence; do not disable MPU protection or treat a successful
retry as proof that the defect is fixed.

## Managed panel sleep-out

`--guard-panel-sleepout` is a separate, off-by-default software adapter. The
qualified panel initialization calls the low-level DSI function directly for
sleep-out, while its preceding commands use a managed API. A delayed native
sleep callback can make DSI read-only before the direct command reaches it.

The adapter routes only that caller and command through the firmware's existing
managed API. The original driver cancels its sleep timer, wakes DSI, sets its
own permissions, sends the original packet, and schedules sleep again. Three
temporary probes qualify entry, observe the original packet write, and check
the API result and preserved caller context. They disable after successful
return. Every process created by Web restart installs a fresh set.

Image, source, configuration and runtime instruction guards bind the route to
the qualified profile. Wrong context, packet, instruction, redirect, native
result or return state stops synchronously without retry or fault recovery.
There is no firmware-byte patch, manual MPU write, simulated success or skipped
panel command. Other low-level callers keep their original path. This is not
a DSI hardware model or proof of native power/timing correctness; retain those
acceptance failures until unassisted execution is verified.

Qualification includes a run where the delayed callback had already protected
DSI read-only: the managed driver reopened its own window, wrote the original
sleep-out packet, and returned with the caller context intact. Unit coverage
also rejects incorrect identity, instructions, packet state and return context.

The combined diagnostic reached the original watchface before and after two
same-disk Web restarts. Those runs also used a separate external BTC correction
ordering experiment; they qualify this panel route, not unmodified BTC boot,
native sleep recovery, touch wake, or complete S5 restart reliability. The BTC
experiment is not enabled or installed by this option.
