# M55 TrustZone interrupt trigger

The M55C0 software interrupt latches occupy SYSCTRL offsets `0x1e4` (SET)
and `0x1e8` (CLR). This device models the two TrustZone notification bits:

| Bit | Event | Output | M55 interrupt |
| --- | --- | --- | --- |
| 13 | DATA_IND, AP to TEE | 0 | IRQ74 |
| 12 | DATA_DONE, TEE to AP | 1 | IRQ75 |

Writing one sets or clears the selected latch. Writing zero leaves it
unchanged. SET and CLR reads return the pending latch state. Repeated SETs
coalesce; the line stays asserted until CLR or device reset. Both security
views must map to the same instance. The M55 NVIC controls enable, priority
and Secure/Non-secure targeting through its ordinary registers.

The implementation only delivers interrupts. Firmware owns shared memory,
resource tables, vrings, messages and protocol state. It neither marks an
endpoint ready nor fabricates a reply. The other interprocessor interrupt
bits in these two registers are unimplemented and writes to them are logged.

Tests use synthetic programs to exchange a shared-memory request and reply
through actual Secure and Non-secure IRQ handlers, including a masked WFI
wakeup, acknowledgement and reset. Register tests also check independent
latches, repeated notifications and reserved bits.

## HIFI4 protocol endpoint

`best1700_hifi4_endpoint.c` supplies a software DSP peer using the real
M55C0/DSPC0 channel-zero transport. SYSCTRL `0x90/0x94` are W1S/W1C
interrupt latches: AP DATA_IND is bit 3, AP DATA_DONE bit 6, DSP DATA_IND
bit 9, and DSP DATA_DONE bit 0. AP receives IRQ76 and IRQ79. DSP reset,
reset vector, runstall and core-wait govern the endpoint lifecycle.

The firmware profile's `ipc.audio_endpoint` describes the image identity,
shared-memory addresses, resource-table identity, notification tokens and
one audio capability request. The original ROMFS container or its exact
extracted payload is accepted, with separate SHA256 checks. Configuration
and resource templates come from that image. Firmware pointers stay in the
profile; they are not embedded in the controller.

After reset release the peer publishes its IPC configuration and resource
table in guest RAM, then signals START through a zero-length IPC message.
The AP allocates its own vring and writes virtio DRIVER_OK. The endpoint
consumes available descriptors, fills used entries, and sends real RPMsg
name-service traffic. `service-ready` requires the AP's name-service ACK
and its exact response to the configured GETCAPS request. Interrupt writes
or elapsed time alone cannot produce this state.

This endpoint executes no HIFI4 instructions and does not play or record
PCM. Unrecognized RPMsg traffic is counted and consumed without a made-up
RPC response. Malformed descriptors, wrong image identity, a wrong reset
vector and handshake timeout fail visibly. Shared-memory accesses cannot
target MMIO. Reset clears publication and queue state; runstall/core-wait
pause progress while preserving the queue and remaining timeout.

`tests.test_best1700_hifi4_qtest` uses an independent synthetic AP peer to
exercise IPC interrupts, actual split-ring messages, service negotiation,
error handling, pause and reset. Original firmware boot is a separate
acceptance check; a capability handshake does not establish PCM support.

## SYS/BTH interrupt controller

`best1700_sys_bth_ipc.c` models the two CMU banks and sixteen interrupt
lanes. Each bank is a sparse `0x1000` container with six four-byte MMIO
registers; its unowned offsets fall through to other CMU devices. The
machine maps shared aliases in each processor's address space and connects
each lane's RX and TX-DONE outputs to the corresponding processors. AON
register endpoints are also available. Both processors can acknowledge
either bank, with word, halfword or byte accesses.

| Register | SYS offset | BTH offset | Meaning |
| --- | --- | --- | --- |
| SET | `0x98` | `0x134` | W1S: high half outgoing IND, low half local DONE |
| CLR | `0x9c` | `0x138` | W1C: high half outgoing IND, low half local DONE |
| MASKSET | `0x1f4` | `0x168` | W1S: low half RX enable, high half DONE enable |
| MASKCLR | `0x1f8` | `0x16c` | W1C of the same enables |
| TXSTATUS | `0x224` | `0x170` | Low half raw DONE, high half enabled DONE |
| RXSTATUS | `0xec` | `0x174` | Low half peer IND, high half enabled peer IND |

SET/CLR reads return the same pending state; MASKSET/MASKCLR reads return
the same enable state. Status registers are read-only. A masked notification
remains pending and asserts its IRQ when re-enabled. Repeated SETs coalesce.
Acknowledging an asserted IND creates a DONE latch for its sender; clearing
an empty IND creates no completion. Clearing DONE acknowledges that latch.
Machine reset clears latches and enables, deasserts IRQs and creates no
completion. Selected lanes can also be reset as a group.

The implemented BTHC0 connections share this physical controller:

| Connection | Lanes | SYS recipient RX / DONE IRQs | BTHC0 RX / DONE IRQs |
| --- | --- | --- | --- |
| M55C0 (AP) to BTHC0 | 0, 1 | AP: 96/97, 94/95 | 73/74, 65/66 |
| M55C1 (CP) to BTHC0 | 2, 3 | CP: 96/97, 94/95 | 75/76, 67/68 |

AP and CP use the same local IRQ numbers on different NVIC instances.
Their physical latch bits are distinct: CP channel 0/1 uses IND bits 18/19,
DONE bits 2/3 and the corresponding mask/status bits. CP lanes are wired
when both real CP and real BTH modes are enabled. No second register bank
or synthetic peer is needed. CPU-only reset does not clear these shared
CMU latches; only machine reset is wired until the peripheral reset-domain
behavior is established.

M55C0/BTHC1 is a separate, unwired connection on lanes 8 and 9. The register
implementation contains neither firmware mailbox addresses nor protocol
responses. The M55 and M33 firmware own the shared records, vrings and
RPMsg service registration.

`tests.test_best1700_sys_bth_ipc_qtest` verifies the two banks and actual
processor IRQ inputs with isolated QEMU processes. Firmware-level RPTUN
handshake remains a separate acceptance check.
`tests.test_best1700_cp_bth_ipc_qtest` additionally verifies CP/BTH lane
routing in both directions, masked-pending delivery, IND/DONE completion,
AP/CP isolation, machine reset and preservation of mailbox RAM during
notification. It is the `best1700-cp-bth-ipc` regression group.

## M55C0/M55C1 interrupt controller

`best1700_m55_ipc.c` owns bits 12 through 23 of SYS CMU SET `0x90`
and CLR `0x94`. These registers are shared with the HIFI4 connection;
the register owner merges their pending states and distributes writes
using disjoint masks. All processors see the same physical latch state.

| Bits, channels 0/1/2 | Recipient | Event |
| --- | --- | --- |
| 12/13/14 | CP | Transmission DONE |
| 15/16/17 | CP | Incoming DATA IND from AP |
| 18/19/20 | AP | Transmission DONE |
| 21/22/23 | AP | Incoming DATA IND from CP |

Each register reads the pending bits. Writes are W1S/W1C, and repeated
SETs coalesce. Interrupt masking belongs to each M55 NVIC. The two
firmware channels use local RX IRQ88/89 and DONE IRQ91/92 on both CPUs.

The receiver clears IND when suspending reception and explicitly sets
the sender's DONE bit when processing completes. **Clearing IND does not
create DONE.** Reset deasserts all owned outputs and clears their latches;
it does not alter the other connections or message RAM. Firmware owns
mailboxes, shared records, vrings and endpoint registration.

`tests.test_best1700_m55_ipc_qtest` checks independent IRQs, bidirectional
notification and acknowledgment, byte lanes, shared-register ownership
and reset using fresh QEMU instances with synthetic data.
