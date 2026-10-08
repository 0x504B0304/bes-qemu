# Host run profiles

`bes.run.v1` selects host Bluetooth, display/control ports, a fixed RTC,
instruction clock and supported boolean model/worker options. Board identities,
panel IDs, memory addresses and hooks belong to board/firmware configuration.
BEST1503 profiles support synthetic HCI or an ESP32 H4 serial controller in both
`run_firmware.py` and the integrated Web/NSH `run_audio.py` launcher. BEST1700
currently rejects physical Bluetooth rather than ignoring a host configuration.

For physical H4, copy a run JSON outside the repository and replace Bluetooth
with the following fields (the MAC is a synthetic example; use the MAC provisioned
in your firmware's factory state):

```json
{
  "backend": "serial-btdm",
  "serial": "/dev/ttyUSB0",
  "baud": 921600,
  "controller_mac": "02:67:09:26:10:05",
  "controller_awake": true,
  "hci_trace": true
}
```

Set `execution.icount_shift` to `null` and `machine_options.hci-worker` to `true`.
Real controllers use real time. `controller_awake` requires the ESP32 PWR1
protocol; set it to false only for a deployed image without that diagnostic.
The launcher holds a device-number serial lease for its lifetime, including
whole-machine Web restarts. Each boot verifies physical MAC readback/retention
before starting QEMU. Binary HCI traces and preflight logs remain in the external
run directory. Dry runs only produce a manifest and never open the serial port.
On WSL, use a private `XDG_RUNTIME_DIR` if WSLg's runtime directory is shared,
for example `/tmp/miband-runtime-1000`. The existing Bluetooth diagnostic CLI
retains its private identity/observer options and uses the same serial lease.

`headless-synthetic` uses a temporary QMP Unix socket under `/tmp`, including
when logs are saved on a Windows mount. `vnc-synthetic` binds VNC 5905,
WebSocket 5906 and QMP 4448 to loopback. Copy the JSON outside the repository to
select different ports; existing fields and types are validated. Use distinct
ports for simultaneous instances.

The launcher checks the compiled board configuration fingerprint before
resuming QEMU, then releases QMP for web controls. Rebuild after changing board
JSON or battery curves. VNC needs QEMU's `keymaps/en-us`; the runner discovers
the build's `pc-bios` directory or accepts `--qemu-data` explicitly.
