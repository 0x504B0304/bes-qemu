# GC NanoUltraV command engine

BEST1503 and BEST1700 select this shared GPU IP. Register and command decoding,
texture sampling, affine transforms, path rasterization and blending belong
here; the SoC owns MMIO placement, identity, guest-memory translation and IRQ.
A8 targets support glyph rasterization with one-byte pitch and alpha-only
SRC_OVER/DST_IN composition. The rectangle FIFO workaround bit does not alter
texture or blending semantics.

BEST1503 retains its existing `0x40220000` window, CID `0x424`, memory aliases
and worker interface. BEST1700 selects `0x401f0000`, IRQ 33, chip `0x265`,
revision `0x2002`, CID `0x420`, ECO 0. Its synchronous memory resolver walks
the AP address space, including TCM AXI aliases and PSRAM protection gates,
and rejects MMIO, discontinuous buffers and writes to ROM.
GPU DMA security follows AON_SEC `0x48` bit 6: reset selects Secure,
and a set bit selects Nonsecure. SRAM command buffers pass through the same
MPC checks as texture reads and target writes; CPU security state is irrelevant.

BEST1700 executes commands even with completion masked. Completion remains
latched until the interrupt-status register is read; unmasking exposes it.
Unsupported commands or paths do not signal completion. No acknowledgement
without rendering option is enabled, and no GPU-to-screen shortcut is used.
Display output still requires the guest LCDC/panel transfer.

The renderer implements a subset of the command language, not a complete
physical GPU. Synthetic qtests check identity, reset, scaled texture pixels,
TCM/PSRAM aliases, buffer guards, interrupt masking and invalid submissions.
They also verify A8 path alpha, retained surrounding pixels and byte pitch.
Actual firmware rendering must be verified separately from those tests.
