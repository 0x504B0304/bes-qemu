# Win32 timer resolution

This host-side fix targets QEMU 8.2.10 on Windows. It does not change guest
clocks or device models.

QEMU already requests the minimum multimedia timer period at startup. Since
Windows 11, that request is ignored for a process without a visible window
unless the process opts out of timer-resolution power throttling. Studio runs
QEMU headless, so the main loop woke on the 15.6 ms default tick. On Watch S5
the 60 Hz LCDC/TE timer was observed firing about 8 ms late on average, and
a frame transfer timer set for 8 ms completed after about 15.5 ms.

The patch calls `SetProcessInformation(ProcessPowerThrottling)` with
`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` before the existing
`timeBeginPeriod()` call. Windows releases without that information class
reject the call, and QEMU continues as before. The cost is the usual cost of
a 1 ms system timer while QEMU runs.

Confirm with `MIBAND_DISPLAY_TIMING_FILE`: tick intervals should be close
to 16.7 ms on a headless run.
