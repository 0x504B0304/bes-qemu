# VNC 60 Hz refresh

This presentation optimization targets QEMU 8.2.10. It changes only the VNC
server's refresh timer; guest timing, the LCDC tick and TE are unchanged.

Upstream VNC polls the console every 30 ms and backs off by 50 ms per idle
tick up to 3000 ms. Only client keyboard and pointer messages reset it. A
guest animation that starts without input after an idle period could
therefore reach the client up to 3 s late, and the ceiling was about 33 fps.

The patch sets the interval to 16 ms, backs off by 16 ms to at most 80 ms,
and resets it to 16 ms whenever the display bridge reports a dirty region
while a client is connected. Change detection, encodings and the client
protocol are unchanged. With no client attached the timer still idles at the
maximum interval.

No frame-rate measurement is implied; confirm with a real-firmware run.
