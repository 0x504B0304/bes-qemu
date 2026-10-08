#!/usr/bin/env python3
"""Real QEMU + PTY adapter regressions. Never opens a physical serial port.

The existing dual-mode and RX-credit tests run unchanged with hci-worker=on.
A stopped TCG instance separately proves physical RX progresses without guest
RAM delivery, then exits while the transport is still active.
"""
import json
import os
from pathlib import Path
import pty
import runpy
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent
QEMU = os.environ.get(
    "MIBAND_QEMU", "/tmp/miband-qemu-parallel-20260930/build/qemu-system-arm")
ORIGINAL_POPEN = subprocess.Popen


def worker_popen(command, *args, **kwargs):
    command = list(command)
    if "-M" in command:
        i = command.index("-M") + 1
        if "bt-backend=serial" in command[i]:
            command[i] += ",hci-worker=on"
    return ORIGINAL_POPEN(command, *args, **kwargs)


def legacy_protocol_tests():
    os.environ["MIBAND_QEMU"] = QEMU
    subprocess.Popen = worker_popen
    try:
        for name in ("test_bt_dual_forward.py", "test_bt_rx_flow.py"):
            try:
                runpy.run_path(str(ROOT / name), run_name="__main__")
            except SystemExit as done:
                assert done.code in (None, 0), (name, done.code)
    finally:
        subprocess.Popen = ORIGINAL_POPEN


def qmp(stream, execute, arguments=None):
    request = {"execute": execute}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode() + b"\n")
    stream.flush()
    while True:
        response = json.loads(stream.readline())
        if "error" in response:
            raise AssertionError(response)
        if "return" in response:
            return response["return"]


def stopped_tcg_test():
    # Keep evidence rather than deleting a failed run's logs.
    directory = Path(tempfile.mkdtemp(prefix="miband-worker-paused-"))
    master, slave = pty.openpty()
    env = dict(os.environ)
    env.pop("MIBAND_HCI_TRACE", None)
    command = [
        QEMU, "-M", f"bes1503,bt-backend=serial-btdm:"
        f"{os.ttyname(slave)}@921600,hci-worker=on",
        "-kernel", os.environ.get("MIBAND_AP", "/mnt/e/HWRE/Miband/vela_ap.bin"),
        "-display", "none", "-monitor", "none", "-serial", "none",
        "-serial", "none", "-accel", "tcg,thread=single", "-S",
        "-qmp", f"unix:{directory}/qmp,server=on,wait=off",
        "-d", "unimp", "-D", str(directory / "qemu.log"),
    ]
    with (directory / "stderr").open("w") as err:
        process = ORIGINAL_POPEN(command, stdout=subprocess.DEVNULL,
                                 stderr=err, env=env)
        connection = socket.socket(socket.AF_UNIX)
        connection.settimeout(3)
        try:
            deadline = time.monotonic() + 5
            while not (directory / "qmp").exists():
                assert process.poll() is None, (directory / "stderr").read_text()
                assert time.monotonic() < deadline
                time.sleep(0.01)
            connection.connect(str(directory / "qmp"))
            with connection.makefile("rwb") as stream:
                json.loads(stream.readline())
                qmp(stream, "qmp_capabilities")
                assert not qmp(stream, "query-status")["running"]
                before = qmp(stream, "human-monitor-command", {
                    "command-line": "xp /16wx 0xc0004000"})
                os.write(master, b"".join(bytes((4, 0xff, 1, n)) for n in range(100)))
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    log = (directory / "qemu.log").read_text()
                    if "rx-queue=100/400" in log and "paused=1 failed=0" in log:
                        break
                    time.sleep(0.03)
                else:
                    raise AssertionError("paused worker did not drain PTY: " + log[-2000:])
                after = qmp(stream, "human-monitor-command", {
                    "command-line": "xp /16wx 0xc0004000"})
                assert before == after, (before, after)
                qmp(stream, "quit")
            process.wait(timeout=3)
            assert process.returncode == 0
            print(f"PASS stopped single-TCG CPU: 100 packets received, guest RAM unchanged, "
                  f"joined shutdown; evidence={directory}")
        finally:
            connection.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
            os.close(master)
            os.close(slave)


def main():
    legacy_protocol_tests()
    stopped_tcg_test()


if __name__ == "__main__":
    main()
