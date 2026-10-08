# Optional SD/eMMC host read-ahead

Local performance patch on top of the pinned eMMC integration, not an upstream
card implementation. `read-ahead-bytes=0` (default) retains the original path;
powers of two from 512 to 65536 enable bounded backend read-ahead.

Cache keys use physical backend offsets, including boot-partition offsets.
Overlapping guest writes (including erase), device reset, migration load and
media change invalidate the cache. Writes to unrelated offsets retain it. Failed speculative reads fall back to the original
request. Card commands, DMA, IRQ, guest data and persistent writes are unchanged.
The cache is host-only and is not migrated. Enabling it refuses shared backend
write/resize permissions; modifying a live disk outside QEMU is unsupported.

Install after `emmc-9.2` with the strict source integration stack. The S5 launcher
exposes an opt-in byte count and records it in each boot manifest.
