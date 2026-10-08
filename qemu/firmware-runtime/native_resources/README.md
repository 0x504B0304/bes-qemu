# Resident native resource calls

`watchface.py` runs inside the supervisor's existing GDB session. It consumes
firmware-specific ABI configuration and calls the firmware's resource service
in its own event loop. It is a host adapter, not an emulated peripheral.

`tools/native_resource_actions.py` owns the host request, timeout and boot
generation. It signals the exact debugger subprocess owned by the supervisor;
it never opens a second GDB connection or resumes a stopped guest with QMP.
Only a pending request's SIGINT can arm an idle adapter. All other stops retain
the fault scene. Failed native calls are not forcibly unwound or freed.

The current configuration qualifies activation of already installed normal
Configurable watchfaces. Catalog queries continue through the real UART shell.
All addresses, guards, structure fields and service parameters belong to the
firmware profile. New firmware requires independent ABI and runtime evidence.

`watchface_package.py` provides a read-only S5 package preflight command,
independent of GDB. It checks metadata and resource bounds without enabling
installation. See [package preflight](../../../docs/watchface-package-preflight.md)
for the supported format, profile limits and command.


With the qualified S5 `--nsh --skip-binding` session, `launch.json` also enables
launching installed quickapps. Two one-shot preparations permit original local
page registration and call native `setenv` before libuv initializes its worker
pool. Both have profile-bound instruction, state and task guards and must finish
before Web launch is available. They do not change phone binding or bypass package
validation. These are native host adapters, not hardware modelling results.

Launch callbacks only acknowledge navigation. The broker additionally observes
the requested guest package, application object and event loop for a bounded
stability interval. An early native exit reports an error without stopping an
otherwise healthy guest. Missing completion, a guard failure or a fault retains
the guest for inspection. No error path automatically continues the VM. Runtime
state does not claim rendered content, connectivity, audio or account readiness.
Web queries read the current runtime again, so later exits clear the running badge.

Calculator arithmetic and Baidu's privacy page are qualified separately through
the actual Web/VNC display and input. Some installed applications still depend on
unavailable services and can exit or time out. The existing watchface activation and UART catalog paths are retained.

S5 quickapp queries use the installer's `/data/quickapp/apps.json` metadata.
The preloaded-package `rpk_info.json` does not contain newly installed packages.
Metadata is intersected with the firmware's active page registry: recoverably
uninstalled built-ins can retain files and installer metadata. The QMP observer
requires two matching, bounded traversals of the active and recoverable lists,
validates pointers, backlinks and package names, and never resumes a stopped VM.
All list locations and field layouts are firmware-profile data. Unready or
changing registration is an incomplete query, not an empty successful catalog.
Only original metadata supplies package names, versions and fingerprints; no
host installation catalog is maintained. Launch rejects inactive registration.

S5 quickapp removal calls the original removal event in the qualified miwear
context. The event return is not interpreted as success: a temporary probe
waits for the matching native removal notification, including asynchronous
closure of a running app. Completion also requires absence from the active
registry and running-app slot, the expected recoverable-list state, and matching
installer metadata. Built-ins may retain their files and metadata while becoming
recoverably uninstalled; newly installed packages follow the full removal path.
The probe disables itself on notification. Shutdown, mismatched notifications,
faults and unconfirmed completion retain the diagnostic scene without automatic
continuation. Profile guards and all existing exception protection remain active.


S5 quickapp installation uses the same resident debugger, with an immutable
per-request upload bound to its generation, nonce, length and SHA-256. The
profile limits uploads to 8 MiB and the operation to 180 seconds. Native libc
calls in the qualified miwear context allocate a buffer, write a unique input
under `/data/mass/`, close it, and read back every byte plus EOF. Only then does
the original package installer receive the path; validation, extraction and
metadata changes execute in the firmware. No Lua staging or host catalog is used.

The event-loop breakpoint is disabled while the asynchronous installer runs.
Temporary probes require the original detailed result, matching path callback
and Finished state. Success additionally requires active page registration and
installer metadata. Input cleanup calls native unlink/free and restores errno
and all saved registers. Ordinary package rejection returns an error after
cleanup and allows another request; faults, changed guards, cleanup failures and
timeouts preserve the scene. Temporary probes are disabled on completion or stop.

Firmware validation covers rejection of an invalid package, installation of an
unchanged new package, duplicate installation and native launch through the Web
HTTP API. Browser upload/automatic refresh, insufficient space, interrupted
installation and application-specific plugins remain separate verification gates.
