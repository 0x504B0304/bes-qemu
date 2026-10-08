#!/usr/bin/env python3
"""Verify real single-thread TCG pause fences an already computing LCDC job."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time


def main():
    p = argparse.ArgumentParser()
    p.add_argument("qemu", type=Path)
    p.add_argument("run", type=Path)
    p.add_argument("--qmp-port", type=int, default=4700)
    a = p.parse_args()
    a.run.mkdir(parents=True, exist_ok=False)
    # BOOT_PC in this board is 0x0c0c0011. Disable exceptions and stay in WFI;
    # the real Cortex-M33 executes, but does not change our display registers.
    kernel = a.run / "wfi-ap.bin"
    kernel.write_bytes(bytes(16) + bytes.fromhex("72b630bffde7"))
    log = (a.run / "stderr.log").open("w")
    q = subprocess.Popen([
        str(a.qemu), "-M", "bes1503,lcdc-worker=on", "-kernel", str(kernel),
        "-display", "none", "-monitor", "none", "-serial", "none",
        "-serial", "none", "-accel", "tcg,thread=single", "-S",
        "-qtest", "stdio", "-qmp",
        f"tcp:127.0.0.1:{a.qmp_port},server=on,wait=off", "-d", "unimp,guest_errors",
        "-D", str(a.run / "model.log"),
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True,
        env=dict(os.environ, MIBAND_LCDC_TEST_WORKER_DELAY_US="300000"))

    def timeout(*_):
        raise TimeoutError("TCG pause test stalled")

    signal.signal(signal.SIGALRM, timeout)
    signal.alarm(20)

    def command(line):
        q.stdin.write(line + "\n")
        q.stdin.flush()
        reply = q.stdout.readline().strip()
        assert reply.startswith("OK"), (line[:100], reply)
        return reply.split()[-1]

    def write(address, value):
        command(f"writel {address:#x} {value:#x}")

    def read(address):
        return int(command(f"readl {address:#x}"), 16)

    try:
        for _ in range(100):
            try:
                sock = socket.create_connection(("127.0.0.1", a.qmp_port), 0.2)
                break
            except OSError:
                assert q.poll() is None, (a.run / "stderr.log").read_text()
                time.sleep(0.02)
        else:
            raise RuntimeError("new QMP listener did not start")
        sock.settimeout(5)
        with sock, sock.makefile("rwb") as stream:
            assert "QMP" in json.loads(stream.readline())

            def qmp(execute, arguments=None):
                request = {"execute": execute}
                if arguments:
                    request["arguments"] = arguments
                stream.write(json.dumps(request).encode() + b"\n")
                stream.flush()
                while True:
                    result = json.loads(stream.readline())
                    assert "error" not in result, result
                    if "return" in result:
                        return result["return"]

            qmp("qmp_capabilities")
            command(f"memset 0x3c085000 {336*480*3} 0x5a")
            write(0x40200104, (480 << 16) | 336)
            write(0x402000FC, 336 * 3)
            write(0x40200264, 0x201)
            write(0x402000F4, 0x3C085000)
            write(0x402001C0, 1 << 21)
            write(0x40200224, 1)
            qmp("cont")
            time.sleep(0.08)  # Beyond first scanout tick, before delayed worker.
            qmp("stop")
            assert not qmp("query-status")["running"]
            assert read(0x40200224) & 1
            time.sleep(0.4)   # Computation now completed while VM is paused.
            assert read(0x402001C4) & ((1 << 21) | (1 << 27)) == 0
            assert read(0x40200224) & 1
            # Seeing the original pixels after resume proves capture happened
            # before stop, rather than the task simply starting after resume.
            command(f"memset 0x3c085000 {336*480*3} 0xa5")
            qmp("cont")
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                if read(0x402001C4) & (1 << 21):
                    break
                time.sleep(0.01)
            else:
                raise AssertionError("resume did not commit held LCDC result")
            qmp("stop")
            assert read(0x40200224) & 1 == 0
            path = a.run / "after-resume.ppm"
            qmp("human-monitor-command", {"command-line": f"screendump {path}"})
            with path.open("rb") as image:
                assert image.readline() == b"P6\n"
                assert image.readline() == b"336 480\n"
                assert image.readline() == b"255\n"
                assert image.read() == b"\x5a" * (336 * 480 * 3)
            (a.run / "results.json").write_text(json.dumps({
                "passed": "LCDC_REAL_SINGLE_TCG_PAUSE_HOLDS_RESULT_UNTIL_RESUME_OK",
                "accel": "tcg,thread=single", "qmp_port": a.qmp_port,
            }, indent=2) + "\n")
            print("LCDC_REAL_SINGLE_TCG_PAUSE_HOLDS_RESULT_UNTIL_RESUME_OK")
    finally:
        signal.alarm(0)
        q.terminate()
        try:
            q.wait(timeout=5)
        except subprocess.TimeoutExpired:
            q.kill()
            q.wait()
        q.stdin.close()
        q.stdout.close()
        log.close()


if __name__ == "__main__":
    main()
