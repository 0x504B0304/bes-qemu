# SD/eMMC media state

Local QEMU 8.2.10 optimization applied after the pinned SD read-ahead package.
The SD card byte callbacks previously queried `blk_is_inserted()` for every
byte. In this QEMU version, calls outside a coroutine enter the block coroutine
wrapper and poll its completion. DMA resource reads therefore pay this cost
hundreds of times per card block, including read-ahead hits.

The card retains the result of the real backend presence query. Realization,
card reset, backend media-change notification and migration pre-load refresh it.
Command/data callbacks use that state under the existing BQL. Removal still
updates the controller's insertion signal and clears the read-ahead cache;
reinsertion still resets the card. No data, protocol response, IRQ completion
or successful I/O is synthesized. The derived flag is not migrated.

Run `tests.test_sd_media_state` and the existing eMMC/read-ahead tests with
`MIBAND_BEST1700_QEMU` set. The media test removes a partially consumed PIO
transfer, verifies absence through system reset, installs a different backend,
checks new data and persistent writes, and confirms the original disk was not
modified. It covers read-ahead both disabled and enabled. Source integration
checks preserve strict hashes and repeated/partial stack installation.

Firmware startup and UI acceptance remain separate: eliminating this coroutine
hotspot does not by itself establish that the GUI meets its watchdog deadline.
