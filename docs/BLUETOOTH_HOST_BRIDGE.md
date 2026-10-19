# Authenticated HCI bridge for an ESP32 serial controller

Studio now owns an ESP32 UART H4 controller, not the computer Bluetooth driver. QEMU receives a generic loopback HCI endpoint; serial discovery, exclusive ownership, reset and firmware-specific vendor policies remain in Studio. No product/firmware addresses belong in this backend.

The v2 backend string is `host-bridge-v2:tcp://127.0.0.1:PORT|LEASE|LAUNCH|TOKEN`, delivered through the startup configuration pipe, never a normal command-line argument or diagnostic log. LEASE and LAUNCH are 32 hex characters, TOKEN is 64. Before H4 transfer QEMU sends `BES-HCI/2 LEASE LAUNCH TOKEN\n`; the peer must reply `BES-HCI/2 OK\n`. The ticket is one-use and launch-bound. A reconnect requires a new session.

QEMU starts with -S; Studio permits execution only after successful handshake. With qtest, starting VM timers is necessary to deliver the synthetic mailbox response, but no guest CPU executes. Socket calls use QEMU's cross-platform socket wrappers, including Windows CRT descriptors. Only loopback TCP and valid numeric ports are accepted by the proxy.

Command/Event/ACL H4 packets are bounded; SCO/ISO are not supported. Standard and vendor commands are forwarded to Studio; exact vendor response policy is supplied by the firmware package. Unknown vendor commands must fail instead of fabricating success. Transport disconnection causes Studio to pause the owning instance; never silently fall back to synthetic Bluetooth.

Legacy host-bridge and host-hci interfaces remain for standalone developer compatibility, but Studio's product path uses authenticated v2 only.

## Evidence, 2026-10-19

Windows x64 and macOS ARM64 native builds passed a synthetic mailbox test against a physical ESP32 + CP2102, UART 921600 8N1. Reset, Read Local Version and LE Read Buffer Size returned physical responses. macOS ran as an ordinary user. Studio test: `tests/test_esp32_native.py` (explicit opt-in port and QEMU/source paths). The board firmware and all proprietary watch images are external to this repository.

These initial transport tests do not qualify phone pairing or resource transfer. Linux and Mac Intel hardware and sustained ACL throughput remain unverified.

## BEST1502P and BEST1700 endpoints

BEST1502P now exposes startup-only `bt-backend`. Channel 0 carries HCI;
channel 1 acknowledges the observed eight-byte control message without creating
an HCI reply. Descriptor validation covers the complete AP SRAM range. No
backend means the original virtual path. A subsequent machine reset terminates
the old physical session instead of reusing controller connections.

BEST1700 requires real CP, BTH and BTC execution for its optional external
endpoint. `bt-mailbox` is `TABLE:RECEIVE:OPCODE:...`, with hexadecimal values
supplied by a firmware-bound descriptor. TABLE names the original pointer table;
RECEIVE is a page-aligned, previously unmapped 4 KiB aperture. Up to 128 exact
command opcodes remain owned by the original BTC; all other commands and ACL
go through the authenticated external endpoint. No opcodes or mailbox addresses
are selected by a product name in the core.

The CP sees an external response through a pointer overlay and a read-only
receive aperture. BTC always sees its original RAM and pointers. Separate
external RX latches prevent its acknowledgment from completing a native BTC TX.
If both sides have responses, the visible external response finishes first,
then the pending native response; new external packets wait behind native IND.
Channel 1 and BTH/BTC traffic remain native. Response bytes come from the actual
selected controller, never a generated success response. The bridge's TXDONE
means that a command/ACL packet was copied to the host queue, not HCI success.

The adapter snapshots and validates a complete descriptor list before sending.
Lists are bounded to 16 nodes; packets to 2048 bytes; queued inbound packets to
32 plus one visible response. Malformed/cyclic lists, SCO/ISO, queue exhaustion
and transport loss stop execution without virtual fallback. Mixed native and
external packets within one descriptor chain are currently explicitly rejected;
this limitation must be resolved or qualified before claiming sustained transfer.
`bt-status` exposes state/counters without session credentials. Exit closes the
endpoint and machine reset invalidates it.

Optional metadata-only tracing uses `MIBAND_HCI_MAILBOX_TRACE=TABLE_ADDRESS`
(integer with `0x` prefix), checked against BTC SRAM. It observes new mailbox
indications without consuming bytes or acknowledging them. Command Complete's
first return byte is labeled `return0`, since vendor replies need not encode a
status byte there. Payloads and keys are not traced.

`tests/test_best1700_hci_router_qtest.py` covers native/external arbitration,
CP-only pointer visibility, read-only receive memory, unchanged native pointers,
control-lane ownership, malformed lists, overflow and reset invalidation. Original
IPC tests remain applicable with tracing enabled. Studio's opt-in real ESP32 test
now exercises BEST1503, BEST1502P and BEST1700 independently; these tests still
use synthetic guests, separately from real-firmware discovery evidence.
