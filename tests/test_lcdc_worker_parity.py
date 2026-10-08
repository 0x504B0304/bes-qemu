#!/usr/bin/env python3
"""Compare LCDC sync/worker panel bytes and visible pixels in isolated QTest.

Run under WSL. This never attaches to an existing QEMU process. Both instances
use the same executable and AP image and differ only in lcdc-worker=off/on.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import time


LCDC = 0x40200000
FB = 0x3C085000
WIDTH, HEIGHT = 336, 480
PIXELS = WIDTH * HEIGHT
DONE = (1 << 21) | (1 << 27)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pattern(pixel_bytes):
    """Make asymmetric channel/position patterns and reference RGB/BGRA."""
    active = bytearray()
    rgb = bytearray()
    panel = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r = (37 * x + 11 * y + 31 * (x // 17)) & 255
            g = (7 * x + 43 * y + 19 * (y // 13)) & 255
            b = (53 * x + 3 * y + (x ^ y)) & 255
            if pixel_bytes == 2:
                r5, g6, b5 = r >> 3, g >> 2, b >> 3
                active.extend(((r5 << 11) | (g6 << 5) | b5).to_bytes(2, "little"))
                r, g, b = r5 * 8 + r5 // 4, g6 * 4 + g6 // 16, b5 * 8 + b5 // 4
            else:
                active.extend((b, g, r))
                if pixel_bytes == 4:
                    # An independently varying X byte must not become alpha.
                    active.append((29 * x + 61 * y) & 255)
            rgb.extend((r, g, b))
            panel.extend((b, g, r, 255))
    return bytes(active), bytes(rgb), bytes(panel)


def padded(active, pixel_bytes, padding, salt):
    row_size = WIDTH * pixel_bytes
    return b"".join(
        active[y * row_size:(y + 1) * row_size]
        + bytes((salt + y * 23 + i * 47) & 255 for i in range(padding))
        for y in range(HEIGHT)
    )


def cases():
    for name, pixel_bytes, config, padding in (
        ("bgr888", 3, 0x201, 13),
        ("bgrx8888", 4, 0x401, 17),
        ("rgb565", 2, 0x001, 7),
    ):
        active, rgb, panel = pattern(pixel_bytes)
        for suffix, row_padding, salt, write_input in (
            ("tight", 0, 0, True),
            ("padded", padding, 0x35, True),
            ("repeat-identical-frame", padding, 0x35, False),
            ("padding-only-change", padding, 0xC9, True),
        ):
            yield {
                "name": f"{name}-{suffix}", "format": name,
                "pixel_bytes": pixel_bytes, "config": config,
                "stride": WIDTH * pixel_bytes + row_padding,
                "padding_bytes_per_row": row_padding,
                "write_input": write_input, "active": active,
                "input": padded(active, pixel_bytes, row_padding, salt),
                "expected_rgb": rgb, "expected_panel": panel,
            }


class Instance:
    def __init__(self, qemu, ap, run, mode, port):
        self.mode, self.port = mode, port
        self.run = run / mode
        self.run.mkdir()
        self.dump = self.run / "dump"
        self.dump.mkdir()
        self.name = f"lcdc-parity-{mode}-{os.getpid()}"
        self.log = (self.run / "stderr.log").open("w")
        self.sock = self.stream = None
        self.command_line = [
            str(qemu), "-name", self.name, "-M",
            f"bes1503,lcdc-worker={'on' if mode == 'async' else 'off'},framebuffer-bpp=16",
            "-kernel", str(ap), "-display", "none", "-monitor", "none",
            "-serial", "none", "-serial", "none", "-accel", "qtest",
            "-qtest", "stdio", "-qtest-log", "/dev/null", "-qmp",
            f"tcp:127.0.0.1:{port},server=on,wait=off", "-d",
            "unimp,guest_errors", "-D", str(self.run / "model.log"),
        ]
        env = dict(os.environ, MIBAND_DISPLAY_DUMP_DIR=str(self.dump))
        env.pop("MIBAND_LCDC_TEST_WORKER_DELAY_US", None)
        self.proc = subprocess.Popen(
            self.command_line, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.log, text=True, env=env,
        )

    def __enter__(self):
        try:
            for _ in range(150):
                try:
                    self.sock = socket.create_connection(("127.0.0.1", self.port), 0.2)
                    break
                except OSError:
                    if self.proc.poll() is not None:
                        raise RuntimeError((self.run / "stderr.log").read_text())
                    time.sleep(0.02)
            else:
                raise RuntimeError(f"{self.mode} isolated QMP listener did not start")
            self.sock.settimeout(5)
            self.stream = self.sock.makefile("rwb")
            assert "QMP" in json.loads(self.stream.readline())
            self.qmp("qmp_capabilities")
            assert self.qmp("query-name")["name"] == self.name
            self.write(LCDC + 0x104, (HEIGHT << 16) | WIDTH)
            self.write(LCDC + 0x1C0, DONE)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self.stream:
            self.stream.close()
        if self.sock:
            self.sock.close()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.proc.stdin.close()
        self.proc.stdout.close()
        self.log.close()

    def qmp(self, execute, arguments=None):
        request = {"execute": execute}
        if arguments:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request).encode() + b"\n")
        self.stream.flush()
        while True:
            response = json.loads(self.stream.readline())
            assert "error" not in response, response
            if "return" in response:
                return response["return"]

    def command(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline().strip()
        assert reply.startswith("OK"), (self.mode, line[:100], reply)
        return reply.split()[-1]

    def write(self, address, value):
        self.command(f"writel {address:#x} {value:#x}")

    def read(self, address):
        return int(self.command(f"readl {address:#x}"), 16)

    def frame(self, case, index):
        if case["write_input"]:
            self.command(f"write {FB:#x} {len(case['input'])} 0x" + case["input"].hex())
        self.write(LCDC + 0xFC, case["stride"])
        self.write(LCDC + 0x264, case["config"])
        self.write(LCDC + 0xF4, FB)
        self.write(LCDC + 0x1C4, 0)
        assert self.read(LCDC + 0x1C4) & DONE == 0
        self.write(LCDC + 0x224, 1)
        self.command("clock_step 20000000")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            status = self.read(LCDC + 0x1C4)
            if status & DONE == DONE:
                break
            time.sleep(0.002)
        else:
            raise AssertionError((self.mode, case["name"], "frame completion timed out"))
        assert self.read(LCDC + 0x224) & 1 == 0
        ppm = self.run / f"{index:02d}-{case['name']}.ppm"
        self.qmp("human-monitor-command", {"command-line": f"screendump {ppm}"})
        with ppm.open("rb") as image:
            assert image.readline() == b"P6\n"
            assert image.readline() == b"336 480\n"
            assert image.readline() == b"255\n"
            rgb = image.read()
        assert len(rgb) == PIXELS * 3
        panel_path = self.dump / f"panel-{index:03d}-output.bin"
        panel = panel_path.read_bytes()
        assert len(panel) == PIXELS * 4
        panel_meta = panel_path.with_suffix(".txt").read_text()
        assert int(re.search(r"\bframe=(\d+)", panel_meta)[1]) == index + 1
        input_path, = self.dump.glob(f"lcdc-{index:03d}-*.bin")
        assert input_path.read_bytes() == case["input"], (self.mode, case["name"], "input capture differs")
        return rgb, panel, {
            "rgb_sha256": sha256(rgb), "panel_bgra_sha256": sha256(panel),
            "ppm_file_sha256": file_sha256(ppm), "frame_id": index + 1,
            "completion_status": f"0x{status:08x}", "frame_start_cleared": True,
            "ppm": str(ppm), "panel_dump": str(panel_path),
            "input_dump": str(input_path), "captured_input_equal": True,
        }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qemu", type=Path)
    parser.add_argument("ap", type=Path)
    parser.add_argument("run", type=Path)
    parser.add_argument("--build-log", type=Path, default=Path("/tmp/miband-parallel-build-final.log"))
    parser.add_argument("--sync-port", type=int, default=4713)
    parser.add_argument("--async-port", type=int, default=4714)
    args = parser.parse_args()
    assert os.name == "posix" and "microsoft" in Path("/proc/version").read_text().lower(), "Run this test under WSL"
    build_text = args.build_log.read_text()
    assert f"Built: {args.qemu}" in build_text and "Linking target qemu-system-arm" in build_text
    assert args.sync_port != args.async_port
    # Refuse to attach if either requested local port is already in use.
    for port in (args.sync_port, args.async_port):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", port))
    args.run.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(args.build_log, args.run / "build.log")
    results = {
        "passed": False, "qemu": str(args.qemu), "qemu_sha256": file_sha256(args.qemu),
        "ap": str(args.ap), "ap_sha256": file_sha256(args.ap),
        "test_sha256": file_sha256(Path(__file__)),
        "build_log": str(args.build_log), "build_log_sha256": file_sha256(args.build_log),
        "width": WIDTH, "height": HEIGHT, "pixels_per_frame": PIXELS,
        "comparison": "all 645120 panel BGRA bytes and all 483840 visible RGB bytes, plus independent expected pixels",
        "instances": {}, "cases": [],
    }

    def timed_out(*_):
        raise TimeoutError("LCDC sync/async pixel parity test stalled")

    signal.signal(signal.SIGALRM, timed_out)
    signal.alarm(90)
    instances = []
    try:
        with ExitStack() as stack:
            for mode, port in (("sync", args.sync_port), ("async", args.async_port)):
                instance = stack.enter_context(Instance(args.qemu, args.ap, args.run, mode, port))
                instances.append(instance)
                results["instances"][mode] = {
                    "pid": instance.proc.pid, "qmp_port": port,
                    "command": instance.command_line,
                    "environment": {"MIBAND_DISPLAY_DUMP_DIR": str(instance.dump)},
                }
            for index, case in enumerate(cases()):
                outputs = [instance.frame(case, index) for instance in instances]
                rgb_equal = outputs[0][0] == outputs[1][0] == case["expected_rgb"]
                panel_equal = outputs[0][1] == outputs[1][1] == case["expected_panel"]
                entry = {key: case[key] for key in (
                    "name", "format", "pixel_bytes", "config", "stride",
                    "padding_bytes_per_row", "write_input",
                )}
                entry.update({
                    "source_bytes": len(case["input"]),
                    "source_sha256": sha256(case["input"]),
                    "active_input_sha256": sha256(case["active"]),
                    "expected_rgb_sha256": sha256(case["expected_rgb"]),
                    "expected_panel_bgra_sha256": sha256(case["expected_panel"]),
                    "sync": outputs[0][2], "async": outputs[1][2],
                    "sync_async_expected_rgb_equal": rgb_equal,
                    "sync_async_expected_panel_equal": panel_equal,
                })
                results["cases"].append(entry)
                assert rgb_equal and panel_equal, (case["name"], "pixel mismatch; inspect saved dumps")
                print(f"PASS {case['name']} panel={entry['expected_panel_bgra_sha256']} rgb={entry['expected_rgb_sha256']}", flush=True)
            results["passed"] = True
            results["case_count"] = len(results["cases"])
    except BaseException as error:
        results["error"] = repr(error)
        raise
    finally:
        signal.alarm(0)
        for instance in instances:
            results["instances"][instance.mode]["terminated"] = instance.proc.poll() is not None
            results["instances"][instance.mode]["returncode"] = instance.proc.returncode
        (args.run / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
