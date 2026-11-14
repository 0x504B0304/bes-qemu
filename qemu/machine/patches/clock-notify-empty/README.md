# Empty timer-list notification optimization

This package is in `tools/integrate_qemu.py`'s active source integration stack
as of 2026-11-14. It includes the measured Band 9 startup optimization in new
source builds. The Studio release lock is unchanged; existing released cores
are not replaced by this source change.

`qemu_clock_notify()` retains the main-loop notification and notifications for
every non-empty timer list, including lists with timers in the future. It skips
only empty non-main lists. Inserting a new earliest timer still independently
notifies through `timerlist_rearm()` after releasing the timer-list lock.

The patch includes three actual C AIO tests: empty lists, future timers for all
clocks, and 2,000 cross-thread empty-to-armed wake/callback iterations. The
candidate passes the complete AIO and AIO-multithread suites on Windows, macOS
and Ubuntu under WSL2, with five additional rearm repetitions per platform.
See Studio's `docs/BAND9.md` for binary identities, sampled startup comparisons,
native-firmware follow-ups and their limits.

The manifest pins exact QEMU 8.2.10 input and output hashes. Application to real
upstream files, idempotence, exact partial-install recovery and rejection of
unknown bytes without writes are checked separately. This is a local change to
existing QEMU GPL-licensed sources, not an upstream backport claim.

The integration fixture calls the default integrator directly, without
injecting this package into the test. Full-stack idempotence and
conflict-without-writes checks pass, as does a separate integration audit using
the actual current QEMU inputs. All 207 installed C/header/include/timer inputs
match the tested development core. Only the two manifest-owned files differ
from the earlier baseline; original inputs remain unchanged. The related
Python suites pass 23 tests.

Windows bounded cross-model startup/NSH/resources and slirp network setup checks
are recorded in Studio's qualification document. For 10 Pro and 9 Pro, the
network-enabled checks explicitly overlay their existing `source_network`
descriptions; their local default packages remain unconfigured. Agent readiness
is not real RPK HTTP/HTTPS qualification. Separate real RPK GET/POST checks pass
on 10 Pro, 9 Pro, RW6 and S5, with native callback order and response validation;
DNS, TLS and concurrent-request qualification are separate. The candidate also
passes bounded thirty-minute Band 9 native mixed workloads on Windows, macOS
ARM64 and Ubuntu under WSL2, including real display sleep/wake, resource queries,
battery, debug leases and independent peers. These results support source
integration, not full other-model lifecycle or release-package acceptance.
The patch retains the exact tested bytes, including its original diagnostic
comment; that comment does not describe the current integration selection.
No firmware readiness check, animation, register value or AP image is changed
by this patch.

A later Windows lifecycle check passes bounded app install/restart/relaunch/
delete persistence on Band 10, 10 Pro, 11, RW6 and S5. N67 has a separate idle
resource-entry gap: after native renderer readiness and actual display sleep,
both the preserved baseline and candidate time out at roughly 40 seconds,
retain healthy MI/running QMP and recover after explicit user wake. This
reproduces that condition before the optimization, but is not a proof of full
timing parity or automatic idle wake. RW6/S5 resident startup hooks continue
to refuse external owner detach by design. See the detailed runs and scopes
in Studio's qualification document; broad product acceptance remains separate.
