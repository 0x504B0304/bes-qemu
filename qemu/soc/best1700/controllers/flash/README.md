# BEST1700 NOR controller

The controller implements command framing, a 16-entry transmit FIFO, receive
FIFO counts, asynchronous command completion, FIFO clearing, and the two-die
wire format. It contains no firmware addresses or die identification policy.
The machine maps its MMIO window and supplies the package configuration.

The byte FIFO interface uses command at `+0x00`, control at `+0x04`, transmit
data at `+0x08`, status at `+0x0c`, receive data at `+0x10`, and FIFO clearing at
`+0x18`. Status contains busy in bit 0, receive-empty in bit 3, receive count in
bits 8:4 and free transmit entries in bits 13:9. Clearing bits 0/1 clears the
receive/transmit FIFO respectively. Clearing both also aborts a pending or
rejected transaction. Extended commands use control bit 0, transmit length in
control bits 24:12, receive direction in command bit 8, and receive length in
command bits 25:13. Command completion is scheduled after one simulated
microsecond; this is functional timing rather than cycle-accurate SPI timing.

Register `+0x204` bit 19 enables two dies and bit 21 broadcasts status data to
both dies. With two dies, each response byte from each die becomes two wire
bytes: the first carries both high nibbles and the second both low nibbles.
Die 0 supplies the low wire nibble and die 1 the high wire nibble. Normal array
commands use aggregate XIP byte addresses and lengths. Their data and the
borrowed XIP backing use this same representation, so program and read are
inverse operations, including odd aggregate addresses.

With one configured die, normal command-only deep-power-down (`B9`) and
release (`AB`) broadcasts reach that die even if the DUAL register is set.
Residual CTRL block length is irrelevant for those opcodes. The programmed
DUAL bit remains unchanged; subsequent data-bearing dual transactions still
require two dies and are rejected otherwise.

An unsupported opcode, malformed transfer, FIFO overflow or protected write
prints a transaction rejection and leaves busy asserted until both FIFOs are
cleared or the device is reset. It never supplies invented receive bytes to
make the guest continue. Extended array-command framing, non-byte receive
FIFO modes, interrupt/DMA transfers, and clock-dependent sampling errors are
not implemented. Normal command reads, page program and erase are supported.
Array modifications require the machine's change callback to invalidate XIP
translations for every alias.

Tests: `tests.test_best1700_flash_qtest` covers FIFO counts, delayed completion,
clear/reset, JEDEC/UID, status writes, streaming program and erase, XIP readback,
and actual Cortex-M55 secure accesses through the machine's secure alias.
