# BEST1700 DMA

Each instance exposes eight channel register banks at +0x100 with a 0x20
stride. Original BTH initialization clears all eight CONFIG words even when
the transfer/capture implementation uses only six AUDMA or four GPDMA
channels. Disabled banks outside that implementation retain configuration
readback; enabling an unimplemented channel is rejected without queuing work.

DMACONFIG models the global enable, CROSS_1KB and automatic/manual channel
clocks. Manual mode requires the matching channel clock bit before scheduling.
Transfers use individual aligned bus beats; burst aggregation is not modeled.
Big-endian masters, 2D transfers and additional global mode bits remain
unsupported. RX peripheral request lines remain pending without invented data.

Run the synthetic BTH initialization/copy/reset and shared DMA regressions:

    MIBAND_BEST1700_QEMU=/path/to/qemu-system-arm python3 -m unittest \
      tests.test_best1700_bth_dma_qtest tests.test_best1700_btc_dma_qtest -v

The BTH test executes through its own address space, initializes both
controllers' eight register banks, copies memory and repeats after machine
reset. S5 diagnostic configuration also guards the original early BTH fault
entry, before the later NuttX exception handlers are installed.
