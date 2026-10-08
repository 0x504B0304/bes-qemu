# BEST1700 UART

The UART streams are bidirectional character backends. AP UART0/1/2 use
interrupts 55/56/57; native BTH UART0/1/2 use 44/45/46. Guest CR enables,
FEN, IFLS and IMSC control reception and interrupt delivery. Empty DR reads
return zero rather than echoing the last transmitted byte.

The receive FIFO holds 16 bytes, or one byte with FEN cleared. Host stream
backpressure retains transfers larger than the FIFO. RX watermarks use
1/8, 1/4, 1/2, 3/4 and 7/8 of its capacity. Ordinary receive timeout is 32 bit
periods derived from IBRD, FBRD, OVSAMP and the supplied peripheral clock;
draining, disabling reception and machine reset cancel the timer. ICR
acknowledges latched timeout/error events. Unread data starts another timeout
after ACK. TX is synchronous and its enabled empty FIFO asserts the TX raw
interrupt.

Host serial ingress enters the guest RX FIFO. System DMA0 can consume wired
UART RX requests through its guest source, destination and descriptor registers;
empty or disabled receivers leave the transfer pending with its residual count.
The system routing implemented here connects AP UART0 request 4, UART1 request 6,
and CP UART2 request 8. Other routes and peripheral-controlled transfer sizes
remain unimplemented.

LCR bit 15 enables DMA receive idle detection. The idle interrupt survives DMA
draining the FIFO, allowing the original driver to consume a short input buffer
using its residual DMA count. Guest acknowledgement clears the idle activity;
receiver disable, FIFO reset and machine reset cancel it. The idle interval
currently uses the ordinary 32 bit periods computed from the baud divisor and
input clock. The extension's LCR bits 8-14 counter tuning is stored but its
hardware timing is not yet reproduced.

Machine wiring currently supplies the initial 26 MHz crystal clock. UART
CMU mux/PLL changes are not yet propagated to this clock. Host bytes arrive
at callback granularity, without emulating each serial bit on a wire.

`tests.test_best1700_uart_qtest` covers host ingress/egress, FIFO watermark
and backpressure, timeout/ACK/drain, loopback overrun, FEN transitions, DMA
request routing/gating, partial transfers, chained descriptors, DMA idle
detection and machine reset. Generated M55 programs additionally verify host
input wakes WFI and executes IRQ56, and UART0 DMA delivers 64 host bytes and
executes IRQ37 on two boots with MTTCG. These tests use no watch firmware and
do not establish cellular registration or networking.
