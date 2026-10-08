# Emulator regression tests

Reusable regression scripts, native C harnesses and test-only helpers belong
in this directory. Production launchers and integration helpers remain outside
it. Temporary investigations, captures and logs belong in external run or
process-record directories.

Run from the repository root in Linux/WSL:

```sh
python3 tools/run_regression.py --qemu /path/to/qemu-system-arm \
  --ap /path/to/vela_ap.bin --resource /path/to/vela_resource.bin \
  --run /tmp/bes-regression-unique

python3 -m unittest tests.test_identity_env tests.test_source_integration
python3 -m tests.test_flash_store
bash tests/test_bt_serial_worker.sh
node tests/test_web_rfb_client.mjs
```

The runner selects host, native, Qtest and Bluetooth groups with `--groups`.
Python scripts run as `python3 -m tests.<name>` so repository production
modules and sibling test helpers resolve without path manipulation. Native
harnesses use their companion C files here and production sources from the
source manifest. Artifacts must be written outside the repository; Qtest
sockets need a Linux filesystem under WSL. PTY and synthetic tests do not use
the physical ESP32 serial bridge.

## External GPU capture fixtures

`test_gpu_bgr24_texture_qtest.py`, `test_gpu_linear_pad_qtest.py`,
`test_gpu_path_aa_qtest.py` and `test_gpu_requested_features_qtest.py`
replay previously captured GPU inputs. They are excluded from the standard
regression groups because those fixtures are external. Their historical
defaults still point at `10pro/evidence/runs/`; use each script's `--help`
for fixture and output options. Captures must not be committed alongside tests.

The host integration check rejects root-level `test_*` source/scripts and
`bt_qtest.py` to prevent the old layout from returning.

BEST1700 eMMC coverage includes native MMC identification/EXT_CSD, IRQ masking
and W1C, SDMA/ADMA2 transfers through AP RAM aliases, and reset persistence.
`test_best1700_emmc_qtest` also executes an M55 MMIO probe under TCG, because
Qtest accesses alone do not exercise CPU TLB translations. The probe requires
the `arm-none-eabi` assembler, linker and objcopy. These tests create their own
temporary disks and need no product firmware.

`test_best1700_psram_qtest` exercises the PSRAM manager FIFO, command timing,
rejected and incomplete transfers, power states, and PHY clock/reset handshakes.
Its secure-address coverage executes an M55 guest under TCG. These functional
checks do not validate analog sampling margins or identify a physical RAM die.
The configured DLL tap result is a compatibility parameter, not a calibration
measurement. These cases run in the standard Qtest group.

The Qtest regression group also runs `test_best1700_secure_tcg` and
`test_best1700_tgu_qtest` with synthetic secure/nonsecure M55 programs.
They cover SG/BXNS handoff, TCM aliases, access attribution, TGU permission
revocation and reset. The programs open only their required nonsecure TCM
blocks and leave the secure stack protected. GNU Arm binutils are required;
missing tools cause explicit test skips, which are not validation passes.

```sh
MIBAND_BEST1700_QEMU=/path/to/qemu-system-arm python3 -m unittest \
  tests.test_best1700_secure_tcg tests.test_best1700_tgu_qtest \
  tests.test_best1700_qtest -v
```

The QMP smoke test checks three physical processors and distinguishes AP
execution state from CP/BTH stubs. Its IPC assertions exercise scaffold
bookkeeping; they do not demonstrate real firmware protocol exchanges.

`test_best1700_amp_mttcg` executes a synthetic AP under MTTCG with four separate
host vCPU threads. It releases CP, BTH and BTC sequentially, verifies that CP
reset release alone cannot execute an unpopulated vector while CORE_WAIT is
set, and checks reset reassertion. It is included in the Qtest regression group.
`test_s5_mttcg_wait` and `test_s5_peer_policies` exercise the qualified debugger
policies, original completion checks, retry limits and fail-closed mismatches;
they are included in host regression. Product firmware boot and actual Web
interaction remain separate live acceptance checks.

`test_tcg_block_limit` validates opt-in MTTCG block limits, rejects invalid
configuration, and checks side-effecting MMIO read replay across reset under
default, bounded, one-instruction and icount execution. Set
`MIBAND_BEST1700_QEMU` to run its CPU execution cases.

- `test_s5_power_lifecycle.py`: offline dedicated QMP event wire, exit ordering,
  cleanup failures, live local HTTP after guest shutdown, resource invalidation,
  explicit starts and the supervisor's persistent-disk selection.
- `test_pmu_poweroff_qtest.py`: opt-in synthetic PMU bit-0 shutdown, GPADC shared
  register, frame/page/chip-select isolation and reset. Set
  `MIBAND_BEST1700_QEMU` only when intentionally running these QEMU tests.
