# Windows socket handle cleanup

The pinned QEMU 8.2.10 `qemu_close_socket_osfhandle()` temporarily sets
`HANDLE_FLAG_PROTECT_FROM_CLOSE` while releasing a CRT file descriptor. It
restores the flags with `SetHandleInformation(handle, flags, flags)`. When the
original flags do not contain the protection bit, that mask cannot clear the
temporary bit. The subsequent `closesocket()` leaves the handle protected.

A real Windows unit probe observed flags 1 before `close()` and flags 3 after
it returned success; `GetHandleInformation()` still accepted the handle.
Independent paused `mps2-an385` QMP reconnects accumulated one `CLOSE_WAIT`
connection per call: 2 at the first sample, 102 at call 100, and 802 at call
800. This is a host socket leak, independent of a particular guest firmware
or managed GDB owner. Evidence is in `band9-qualification-20261113` outside
the source repository.

The `win32-socket-close` integration restores only the bit it changed, using
`HANDLE_FLAG_PROTECT_FROM_CLOSE` as the mask and the original flags as the
value. Inheritance flags remain unchanged. The patch is restricted to the
Windows implementation and does not modify POSIX close behavior, guest
execution, firmware, timeouts, or command retry policy.

The package follows `win32-socket-watch` in the verified integration stack.
Input/output source hashes and the patch digest are pinned. Unknown source
edits refuse the complete installation before any source is written.

The added native unit test checks both inheritable and non-inheritable
sockets, original flag restoration, direct CRT descriptor release followed by
Winsock close, and the ordinary combined close wrapper. All ten upstream
socket tests passed on the development core, together with 22 Python
integration/backport tests. The failing baseline probe and successful
candidate results are retained.

Band 9 mixed-load run `native-1e3b001e` failed waiting for a QMP greeting after
six completed input/resource cycles. Its live snapshot retained 130 QMP
`CLOSE_WAIT` connections and a running, unpoisoned MI owner. Fixing the leak
does not by itself prove that every greeting timeout has the same cause.
Close-only synthetic QMP runs did reproduce a separate greeting timeout after
17, 179 and 2454 reconnects, despite eliminating the per-call handle leak.
The follow-up bounded readiness correction and its deterministic context
handoff test are described in [Windows socket readiness](WINDOWS_SOCKET_WATCH.md).
The combined candidate passes 11 native tests and 10,000 reconnects without
accumulating closed sockets. This does not establish that every historical
timeout had either cause. No release core lock or published package is updated
by this development change.
