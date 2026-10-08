#!/usr/bin/env python3
"""Isolated PTY regression for guest reset after physical-HCI-worker shutdown.

Uses no real serial device. Exercises active RX/trace work and pending BT timers
under qtest, paused TCG, running WFI TCG, and the actual running AP firmware.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import signal
import socket
import subprocess
import time


def run_case(qemu, ap, directory, mode, port):
    directory.mkdir(parents=True, exist_ok=False)
    fixture = directory / "wfi-ap.bin"
    fixture.write_bytes(bytes(16) + bytes.fromhex("72b630bffde7"))
    master, slave = pty.openpty()
    slave_path = os.ttyname(slave)
    assert slave_path.startswith("/dev/pts/"), slave_path
    kernel = ap if mode == "tcg-real-ap" else fixture
    accel = "qtest" if mode == "qtest-pending-timer" else "tcg,thread=single"
    paused = mode == "tcg-paused"
    env = dict(os.environ, MIBAND_HCI_TRACE=str(directory / "hci.trace"))
    args = [
        str(qemu), "-M",
        f"bes1503,bt-backend=serial-btdm:{slave_path}@921600,hci-worker=on",
        "-kernel", str(kernel), "-display", "none", "-monitor", "none",
        "-serial", "none", "-serial", "none", "-accel", accel,
        "-qtest", "stdio", "-qmp",
        f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-d", "unimp,guest_errors", "-D", str(directory / "model.log"),
    ]
    if paused:
        args.append("-S")
    (directory / "command.json").write_text(json.dumps(args, indent=2) + "\n")
    stderr = (directory / "stderr.log").open("w")
    process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=stderr, text=True, env=env)
    events = []
    connection = None
    transport_released = False

    def qt(line):
        process.stdin.write(line + "\n")
        process.stdin.flush()
        reply = process.stdout.readline().strip()
        assert reply.startswith("OK"), (mode, line[:80], reply)
        return reply.split()[-1]

    try:
        for _ in range(100):
            try:
                connection = socket.create_connection(("127.0.0.1", port), .2)
                break
            except OSError:
                assert process.poll() is None, (directory / "stderr.log").read_text()
                time.sleep(.02)
        else:
            raise RuntimeError("isolated QMP socket did not open")
        connection.settimeout(5)
        with connection, connection.makefile("rwb") as stream:
            assert "QMP" in json.loads(stream.readline())

            def qmp(command):
                stream.write(json.dumps({"execute": command}).encode() + b"\n")
                stream.flush()
                while True:
                    line = stream.readline()
                    assert line, (mode, command, "unexpected QMP EOF")
                    value = json.loads(line)
                    if "event" in value:
                        events.append(value)
                    assert "error" not in value, value
                    if "return" in value:
                        return value["return"]

            qmp("qmp_capabilities")
            status = qmp("query-status")
            assert status["running"] != paused, (mode, status)
            model = (directory / "model.log").read_text()
            assert "miband BT worker started" in model, model[-1000:]
            os.close(slave)
            slave = None

            # Leave real H4 traffic and a partial frame in the worker's parser.
            incoming = b"".join(bytes((4, 0xFF, 1, n)) for n in range(100))
            os.write(master, incoming + b"\x04\xff\x20\xa5")
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                model = (directory / "model.log").read_text()
                if "rx=100/404" in model and "failed=0" in model:
                    break
                assert process.poll() is None, (directory / "stderr.log").read_text()
                time.sleep(.02)
            else:
                raise AssertionError((mode, "worker did not read PTY", model[-2000:]))

            # Deterministic guest-origin command and pending virtual TX timer.
            # The actual AP owns these structures in its separate scenario.
            if mode != "tcg-real-ap":
                for address, value in (
                    (0xC0004004, 0x20010000), (0x20010000, 0x20010100),
                    (0x20010100, 0), (0x20010104, 0),
                    (0x20010108, 5), (0x2001010C, 0x20010200),
                    (0x20010110, 1),
                ):
                    qt(f"writel {address:#x} {value:#x}")
                qt("write 0x20010200 5 0x011a0c0103")
                qt("writel 0x400000a0 0x8004")

            before_reset = len(events)
            began = time.monotonic()
            if mode == "tcg-aircr-reset":
                # Exercise the Cortex-M system-reset-request MMIO path in
                # addition to QMP's board-reset entry point.
                qt("writel 0xe000ed0c 0x05fa0004")
            else:
                qmp("system_reset")
            while True:
                line = stream.readline()
                if not line:
                    break
                value = json.loads(line)
                if "event" in value:
                    events.append(value)
                assert "error" not in value, value
            process.wait(timeout=5)
            elapsed = time.monotonic() - began
            assert process.returncode == 0, (mode, process.returncode)
            reset_events = events[before_reset:]
            names = [item["event"] for item in reset_events]
            assert "RESET" in names and "SHUTDOWN" in names, reset_events
            assert "RESUME" not in names, reset_events
            shutdown = [item for item in reset_events if item["event"] == "SHUTDOWN"]
            assert any(item["data"].get("reason") == "guest-reset"
                       for item in shutdown), shutdown

        # The parent closed its PTY slave earlier. After process shutdown the
        # master must reach EIO, proving no serial owner remained open.
        os.set_blocking(master, False)
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            try:
                os.read(master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    transport_released = True
                    break
                if error.errno != errno.EAGAIN:
                    raise
            select.select([master], [], [], .02)
        assert transport_released, mode
        stderr.flush()
        diagnostic = (directory / "stderr.log").read_text()
        assert "guest reset invalidated physical HCI session; restart the instance" in diagnostic
        for forbidden in ("Segmentation fault", "Assertion", "AddressSanitizer",
                          "qemu: fatal", "double free", "runtime error:"):
            assert forbidden not in diagnostic, diagnostic[-3000:]
        result = {
            "mode": mode, "port": port, "pty": slave_path,
            "exit_code": process.returncode, "reset_to_exit_seconds": elapsed,
            "transport_released": transport_released,
            "events_after_reset": reset_events,
            "worker_rx_packets": 100, "worker_rx_bytes": 404,
            "passed": True,
        }
        (directory / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"HCI_WORKER_SYSTEM_RESET_OK mode={mode} exit=0 no-resume=1 pty-released=1",
              flush=True)
        return result
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if connection:
            connection.close()
        process.stdin.close()
        process.stdout.close()
        stderr.close()
        os.close(master)
        if slave is not None:
            os.close(slave)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("qemu", type=Path)
    p.add_argument("ap", type=Path)
    p.add_argument("run", type=Path)
    p.add_argument("--repetitions", type=int, default=2)
    args = p.parse_args()
    args.run.mkdir(parents=True, exist_ok=False)

    def timeout(*_):
        raise TimeoutError("HCI reset lifecycle regression stalled")

    signal.signal(signal.SIGALRM, timeout)
    signal.alarm(60)
    results = []
    try:
        for index in range(args.repetitions):
            for mode, port in (
                ("qtest-pending-timer", 4710), ("tcg-paused", 4711),
                ("tcg-wfi-running", 4712), ("tcg-real-ap", 4715),
                ("tcg-aircr-reset", 4716),
            ):
                results.append(run_case(args.qemu, args.ap,
                    args.run / f"{index + 1}-{mode}", mode, port))
        (args.run / "results.json").write_text(json.dumps({
            "binary_sha256": hashlib.file_digest(args.qemu.open("rb"), "sha256").hexdigest(),
            "cases": results, "physical_serial_used": False,
        }, indent=2) + "\n")
    finally:
        signal.alarm(0)


if __name__ == "__main__":
    main()
