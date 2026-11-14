# Windows socket readiness and guest execution

QEMU 8.2.10's Windows `qio_channel_socket_source_check()` calls `select()`
whenever GLib checks the source, including iterations caused only by virtual
timers and vCPU notifications. These checks run with the main-loop lock held.
With the managed debugger, UART, and several VNC connections this consumes
system CPU time and delays the vCPU's MMIO/exception work.

The pinned `win32-socket-watch` integration initially gated checks on the
socket event's `GPollFD.revents`. Later qualification found that this event-only
gate can miss pending data during a context handoff. The follow-up
`win32-socket-poll` integration therefore keeps immediate signalled-event
checks and adds level-readiness checks at most every 10 milliseconds when no
notification arrives. `prepare()` bounds the context's next wait as well;
unrelated virtual-timer/vCPU wakeups do not each require a socket syscall.
A signalled event still uses the original `select()` filtering, because read
and write watches can share the channel's event. These changes affect only the
`CONFIG_WIN32` implementation; the POSIX fd source is unchanged. It does not
alter guest clocks, watchdogs, firmware bytes, or display refresh rates.

The package includes a real upstream I/O unit test for shared read/write
watches, idle context iterations, one-byte partial reads, attaching a new
watch while data is pending, graceful peer write shutdown/EOF, and a second
connection after cleanup. Run after integration:

```sh
ninja -C build tests/unit/test-io-channel-socket.exe
build/tests/unit/test-io-channel-socket.exe
```

The original nine Windows socket tests passed, as did the 20 integration/backport tests
and 76 Studio managed-debugger/readiness tests against the rebuilt core.
An initial test used a bare channel close as an immediate FIN guarantee; both
the original and candidate implementation failed that assumption. The EOF
case now explicitly performs `QIO_CHANNEL_SHUTDOWN_WRITE` before close. This
is not evidence that arbitrary abrupt disconnect behavior has been qualified.

Controlled Band 9 observations used the same AP, exact copied user journal,
and three RFB 3.8/ZRLE clients without early resource queries. Before the fix,
`native-2db691d7` spent 37.719 seconds of system CPU in a roughly 50-second
window and did not initialize the complete application catalogue in the test
budget. After the readiness gate, `native-7a87d1b1` spent 9.719 seconds of
system CPU, completed the resource scan in 3 seconds, and displayed the real
stock face after a double tap. Timings describe development builds on one
Windows host, not a cross-platform performance claim.

Native host sampling is supporting evidence: the original main thread was
frequently in `select()`/`NtDeviceIoControlFile`, while the vCPU waited for the
I/O-thread lock. The candidate main thread mostly waited for events instead.
The retained sampling artifacts explicitly distinguish sampled PCs from raw
stack words; the latter are candidate return addresses, not unwound stacks.

Evidence and exact binary hashes are retained outside the repository in
`band9-qualification-20261113`. No release core lock, published package, or
remote firmware object was changed by this development validation.

The final UTF-8 prepared engine also passed five subsequent three-client cold
starts with first complete catalogues in 13.359–14.875 seconds, repeated
queries, and real stock-face screenshots. A separate run passed 30 interleaved
clock reads and resource queries. Battery updates, native reboot/persistence,
and Band 10/10 Pro smoke tests passed against this engine as well.

One earlier final-engine run (`native-45823937`) completed its first query in
14.922 seconds but its second query remained incomplete for 90 seconds. That
run did not capture individual responses or host lock stacks, and subsequent
instrumented repetitions have not reproduced it. Its cause remains unknown;
the readiness gate is not claimed to fix every resource-query failure. Band 9
therefore retains its qualified icount policy pending further investigation.

A later QMP greeting-timeout investigation independently reproduced a Windows
socket-handle leak in the upstream CRT/Winsock close wrapper. See
[Windows socket cleanup](WINDOWS_SOCKET_CLOSE.md) for its separate pinned
correction and evidence. It is not a change to this readiness gate.

## Context handoff correction

A deterministic native test initializes two contexts watching one socket,
writes one byte, and lets the first context observe readiness and remove its
watch without reading. Windows' shared auto-reset event can be consumed there;
the second initialized context must still deliver the pending byte without
another peer write. The event-only gate and a first-check-only candidate both
failed this test. The bounded fallback passes. The test also runs on POSIX,
whose source implementation remains unchanged.

Close-only paused synthetic QEMU runs still reproduced greeting stalls at
17, 179 and 2454 reconnects. Thus fixing the handle leak alone was insufficient.
Removing the readiness gate passed 10,000 reconnects, but discards the earlier
CPU improvement and is retained only as a diagnostic candidate. The bounded
fallback passed all 11 native socket tests, 22 integration/backport tests and
10,000 QMP reconnects with no failures or accumulated CLOSE_WAIT connections
(`qmp-serial-37aa261b`). Its UTF-8 development executable SHA-256 is
`c782e4d60073c94147c2ea6df09416145911b6b2343a313130dbd29122231ce0`.

These results qualify the observed context-handoff failure, not every possible
QMP or resource timeout. Earlier CPU timings above concern the event-only
candidate; they must not be attributed to the bounded fallback without a new
matched comparison. Full real-firmware endurance qualification is separate.
Native sampling must use a symbol executable whose executable sections match
the target; raw stack words are never an unwound stack.

A subsequent paired three-ZRLE-client MTTCG observation used an identical copy
of the retained Band 9 journal and no early resource polling. The bounded
candidate (`native-0abcd3d4`) consumed 6.4375 system / 39.578125 user CPU seconds
in a 40.578-second sample interval. The no-gate diagnostic
(`native-15e565ad`) consumed 31.625 system / 19.875 user seconds in 40.609 seconds.
Both completed the final resource catalogue and stock-face screen checks. This
is one pair on one Windows host, with other isolated qualification guests
running; it does not establish a five-run performance median or explain the
older intermittent MTTCG resource-query stall. Band 9 still defaults to icount.

Separate real-firmware endurance scope: combined-engine `native-ffba099a`
failed at cycle 39 when a double tap left the framebuffer black for five
seconds. QMP and the managed MI owner remained running/unpoisoned. This is
not the earlier QMP greeting failure, and no attribution to this socket change
has been established. The successful six-minute actual-sleep follow-up does
not erase it. Controller/input timing diagnostics are retained in Studio's
Band 9 evidence; full endurance acceptance remains incomplete.
