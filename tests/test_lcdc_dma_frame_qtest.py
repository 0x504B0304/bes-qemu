#!/usr/bin/env python3
"""Check requested transfers, partial DMA, and explicit panel GRAM visibility.

Sleep retention and deferred sleeping transfers are model policies, not proof
of the physical panel's behavior.
"""

import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import time
from pathlib import Path


DISPLAY_BYTES = 336 * 480 * 3
CHUNK_BYTES = 4032 * 4
CH7 = 0x401301E0
LCDC = 0x40200000
AP_RAM = 0x20000000
SCANOUT = 0x3C085000
DESCRIPTORS = 0x20080000
CONTROL = 0x8C489FC0
FRAME_COMPLETE = (1 << 21) | (1 << 27)


def qmp_command(stream, payload):
    stream.write(json.dumps(payload).encode("ascii") + b"\n")
    stream.flush()
    while True:
        result = json.loads(stream.readline())
        if "error" in result:
            raise RuntimeError(result["error"])
        if "return" in result:
            return result["return"]


def read_ppm_uniform(path):
    with path.open("rb") as image:
        assert image.readline() == b"P6\n"
        assert image.readline() == b"336 480\n"
        assert image.readline() == b"255\n"
        pixels = image.read()
        assert len(pixels) == DISPLAY_BYTES, (path, len(pixels))
        pixel = pixels[:3]
        assert pixels == pixel * (336 * 480), f"mixed or incomplete frame: {path}"
        return pixel


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qemu", type=Path)
    parser.add_argument("ap_image", type=Path)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--qmp-port", type=int, default=4593)
    parser.add_argument("--check-dump-pairing", action="store_true", default=True,
                        help="verify saved pairs and completion logs through the configured cap (enabled by default)")
    parser.add_argument("--display-dump-limit",
                        help="raw MIBAND_DISPLAY_DUMP_LIMIT value (unset by default)")
    parser.add_argument("--expected-dump-limit", type=int, default=64,
                        help="expected effective cap for the pairing check (default: 64)")
    parser.add_argument("--expect-dump-limit-warning", action="store_true",
                        help="expect exactly one invalid-limit warning and fallback")
    args = parser.parse_args()
    if not 0 <= args.expected_dump_limit <= 4096:
        parser.error("expected dump limit must be in 0..4096")
    if (args.display_dump_limit is not None or
            args.expected_dump_limit != 64 or args.expect_dump_limit_warning):
        if not args.check_dump_pairing:
            parser.error("dump-limit options require --check-dump-pairing")
    if args.run_dir.exists():
        parser.error(f"run directory already exists: {args.run_dir}")
    args.run_dir.mkdir(parents=True)

    environment = os.environ.copy()
    # The default case must exercise an unset limit, independent of the caller.
    environment.pop("MIBAND_DISPLAY_DUMP_LIMIT", None)
    if args.display_dump_limit is not None:
        environment["MIBAND_DISPLAY_DUMP_LIMIT"] = args.display_dump_limit
    dump_dir = args.run_dir / "display"
    if args.check_dump_pairing:
        dump_dir.mkdir()
        environment["MIBAND_DISPLAY_DUMP_DIR"] = str(dump_dir)

    qemu_log = (args.run_dir / "qemu.log").open("wb")
    qemu = subprocess.Popen(
        [str(args.qemu), "-M", "bes1503", "-kernel", str(args.ap_image),
         "-display", "none", "-monitor", "none", "-accel", "qtest",
         "-qtest", "stdio", "-qmp",
         f"tcp:127.0.0.1:{args.qmp_port},server=on,wait=off",
         "-serial", "none", "-serial", "none", "-d", "unimp,guest_errors",
         "-D", str(args.run_dir / "model.log")],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=qemu_log,
        text=True, env=environment,
    )

    def qtest(line):
        qemu.stdin.write(line + "\n")
        qemu.stdin.flush()
        response = qemu.stdout.readline().strip()
        if not response.startswith("OK"):
            raise RuntimeError(f"QTest {line!r}: {response!r}")
        return response.split()[-1] if len(response.split()) > 1 else None

    def write(address, value):
        qtest(f"writel {address:#x} {value:#x}")

    def panel_command(command):
        write(LCDC + 0x284, 0x02000000 | command << 8)
        write(LCDC + 0x280, 1)

    def start_dma(source, destination, chunks):
        write(CH7, source)
        write(CH7 + 4, destination)
        write(CH7 + 8, DESCRIPTORS if chunks > 1 else 0)
        write(CH7 + 12, CONTROL)
        for index in range(1, chunks):
            descriptor = DESCRIPTORS + (index - 1) * 16
            write(descriptor, source + index * CHUNK_BYTES)
            write(descriptor + 4, destination + index * CHUNK_BYTES)
            write(descriptor + 8,
                  DESCRIPTORS + index * 16 if index + 1 < chunks else 0)
            write(descriptor + 12, CONTROL)
        write(CH7 + 16, 0x8001)
        qtest("clock_step 1000000")
        assert int(qtest(f"readl {CH7 + 16:#x}"), 16) & 1 == 0

    try:
        # reference uses byte-wide copies; reference used word-wide copies. Width bits
        # select transfer units and must not force every descriptor to words.
        for width in (0, 1):
            source, target = AP_RAM + 0x100, SCANOUT + 0x100
            count = 15
            size = count << width
            qtest(f"memset {source:#x} {size + 4} 0x73")
            qtest(f"memset {target:#x} {size + 4} 0xa6")
            write(CH7, source)
            write(CH7 + 4, target)
            write(CH7 + 8, 0)
            write(CH7 + 12, 0x8c000000 | (width << 18) | (width << 21) | count)
            write(CH7 + 16, 0x8001)
            qtest("clock_step 1000000")
            assert int(qtest(f"readb {target + size - 1:#x}"), 16) == 0x73
            assert int(qtest(f"readb {target + size:#x}"), 16) == 0xa6
            assert int(qtest("readl 0x40130014"), 16) & 0x80
            assert int(qtest("readl 0x40130018"), 16) & 0x80 == 0
            write(0x40130008, 0x80)
        # Inconsistent widths have no verified packing semantics. Reject
        # without modifying the destination rather than silently truncating.
        qtest(f"memset {SCANOUT:#x} 64 0xa6")
        write(CH7, AP_RAM)
        write(CH7 + 4, SCANOUT)
        write(CH7 + 8, 0)
        write(CH7 + 12, 0x8c000000 | (1 << 21) | 16)
        write(CH7 + 16, 0x8001)
        qtest("clock_step 1000000")
        assert int(qtest(f"readb {SCANOUT:#x}"), 16) == 0xa6
        assert int(qtest("readl 0x40130018"), 16) & 0x80
        write(0x40130010, 0x80)
        qtest(f"memset {AP_RAM:#x} {DISPLAY_BYTES} 0x55")
        write(LCDC + 0x104, (480 << 16) | 336)
        write(LCDC + 0xFC, 336 * 3)
        write(LCDC + 0x264, 0x201)
        write(LCDC + 0xF4, SCANOUT)
        write(LCDC + 0x1C0, 1 << 21)
        start_dma(AP_RAM, SCANOUT, DISPLAY_BYTES // CHUNK_BYTES)
        assert int(qtest(f"readb {SCANOUT:#x}"), 16) == 0x55
        # A completed memory copy and timer tick cannot acknowledge a frame.
        qtest("clock_step 20000000")
        assert int(qtest(f"readl {LCDC + 0x1C4:#x}"), 16) & FRAME_COMPLETE == 0
        write(LCDC + 0x224, 1)

        qtest(f"memset {AP_RAM:#x} {CHUNK_BYTES} 0x11")
        start_dma(AP_RAM, SCANOUT, 1)
        assert int(qtest(f"readb {SCANOUT:#x}"), 16) == 0x11
        assert int(qtest(f"readb {SCANOUT + CHUNK_BYTES:#x}"), 16) == 0x55
        qtest("clock_step 20000000")

        for _ in range(50):
            try:
                sock = socket.create_connection(("127.0.0.1", args.qmp_port),
                                                timeout=1)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("QMP socket did not open")
        with sock, sock.makefile("rwb") as stream:
            assert "QMP" in json.loads(stream.readline())
            qmp_command(stream, {"execute": "qmp_capabilities"})
            def screenshot(name):
                path = args.run_dir / name
                qmp_command(stream, {
                    "execute": "human-monitor-command",
                    "arguments": {"command-line": f"screendump {path}"},
                })
                return read_ppm_uniform(path)

            pixel = screenshot("complete-before-next-dma.ppm")
            assert pixel == b"\x55\x55\x55", pixel.hex()

            write(LCDC + 0x1C4, 0)
            panel_command(0x28)
            assert screenshot("display-off-retains-gram.ppm") == b"\x00\x00\x00"
            panel_command(0x29)
            assert screenshot("display-on-restores-gram.ppm") == b"\x55\x55\x55"
            assert int(qtest(f"readl {LCDC + 0x1C4:#x}"), 16) & FRAME_COMPLETE == 0

            for command in (0x28, 0x10):
                panel_command(command)
            assert screenshot("panel-asleep.ppm") == b"\x00\x00\x00"

            qtest(f"memset {AP_RAM:#x} {DISPLAY_BYTES} 0x33")
            start_dma(AP_RAM, SCANOUT, DISPLAY_BYTES // CHUNK_BYTES)
            write(LCDC + 0x1C4, 0)
            write(LCDC + 0x224, 1)
            qtest("clock_step 20000000")
            assert int(qtest(f"readl {LCDC + 0x1C4:#x}"), 16) & FRAME_COMPLETE == 0
            assert screenshot("panel-asleep-with-request.ppm") == b"\x00\x00\x00"

            for command in (0x11, 0x29):
                panel_command(command)
            assert screenshot("wake-restores-previous-gram.ppm") == b"\x55\x55\x55"
            qtest("clock_step 20000000")
            assert screenshot("panel-awake-pending-request.ppm") == b"\x33\x33\x33"
            assert int(qtest(f"readl {LCDC + 0x224:#x}"), 16) & 1 == 0
            assert int(qtest(f"readl {LCDC + 0x1C4:#x}"), 16) & FRAME_COMPLETE == FRAME_COMPLETE

            # Copying into a second buffer alone cannot select it for display.
            second = SCANOUT + DISPLAY_BYTES
            qtest(f"memset {AP_RAM:#x} {DISPLAY_BYTES} 0x77")
            start_dma(AP_RAM, second, DISPLAY_BYTES // CHUNK_BYTES)
            qtest("clock_step 20000000")
            assert screenshot("second-buffer-unselected.ppm") == b"\x33\x33\x33"
            write(LCDC + 0xF4, second)
            qtest("clock_step 20000000")
            assert screenshot("second-buffer-no-request.ppm") == b"\x33\x33\x33"
            write(LCDC + 0x224, 1)
            qtest("clock_step 20000000")
            assert screenshot("second-buffer-request.ppm") == b"\x77\x77\x77"

            # Awake display-off hides the surface while a transfer updates GRAM.
            panel_command(0x28)
            qtest(f"memset {second:#x} {DISPLAY_BYTES} 0x66")
            write(LCDC + 0x1C4, 0)
            write(LCDC + 0x224, 1)
            qtest("clock_step 20000000")
            assert int(qtest(f"readl {LCDC + 0x1C4:#x}"), 16) & FRAME_COMPLETE == FRAME_COMPLETE
            assert screenshot("display-off-transfer-hidden.ppm") == b"\x00\x00\x00"
            panel_command(0x29)
            assert screenshot("display-on-transferred-gram.ppm") == b"\x66\x66\x66"

            # Repeating identical pixels still completes an explicit request.
            write(LCDC + 0x1C4, 0)
            write(LCDC + 0x224, 1)
            qtest("clock_step 20000000")
            assert int(qtest(f"readl {LCDC + 0x224:#x}"), 16) & 1 == 0
            assert int(qtest(f"readl {LCDC + 0x1C4:#x}"), 16) & FRAME_COMPLETE == FRAME_COMPLETE
            assert screenshot("same-hash-explicit-request.ppm") == b"\x66\x66\x66"

            qtest(f"memset {AP_RAM:#x} {DISPLAY_BYTES} 0x22")
            start_dma(AP_RAM, SCANOUT, DISPLAY_BYTES // CHUNK_BYTES)
            write(LCDC + 0xF4, SCANOUT)
            write(LCDC + 0x224, 1)
            qtest("clock_step 20000000")
            assert screenshot("panel-awake-after-dma.ppm") == b"\x22\x22\x22"

            # Software reset clears GRAM; wake alone cannot reconstruct it
            # from unchanged PSRAM. Only another request transfers those bytes.
            panel_command(0x01)
            for command in (0x11, 0x29):
                panel_command(command)
            qtest("clock_step 20000000")
            assert screenshot("reset-wake-empty-gram.ppm") == b"\x00\x00\x00"
            write(LCDC + 0x1C4, 0)
            write(LCDC + 0x224, 1)
            qtest("clock_step 20000000")
            assert int(qtest(f"readl {LCDC + 0x1C4:#x}"), 16) & FRAME_COMPLETE == FRAME_COMPLETE
            assert screenshot("reset-same-pixels-requested.ppm") == b"\x22\x22\x22"

            if args.check_dump_pairing:
                # Reach the diagnostic cap with distinct explicit transfers.
                # The visible model must keep transferring afterwards, while
                # all saved input/panel pairs remain the same evidence.
                def saved_hashes():
                    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in sorted(dump_dir.iterdir())}

                cap = args.expected_dump_limit
                # Bound attempts so an ignored cap fails instead of hanging.
                for _ in range(cap):
                    count = len(list(dump_dir.glob("lcdc-*.txt")))
                    if count >= cap:
                        break
                    value = (count + 1) & 0xFF
                    qtest(f"memset {SCANOUT:#x} {DISPLAY_BYTES} {value:#x}")
                    write(LCDC + 0x224, 1)
                    qtest("clock_step 20000000")
                assert len(list(dump_dir.glob("lcdc-*.txt"))) == cap

                # Frame 65+ must retain completion time, source and hash, not
                # only dump files whose chronological pairing is unverifiable.
                completions = {}
                pattern = (r"miband LCDC (?:complete|completed DMA) frame=(\d+) "
                           r"source=([0-9a-f]+) format=\d+ hash=([0-9a-f]+)"
                           r"[^\n]* virtual_ms=(\d+)")
                for frame, source, hash_, virtual_ms in re.findall(
                        pattern, (args.run_dir / "model.log").read_text()):
                    number = int(frame)
                    assert number not in completions, number
                    completions[number] = {"source": source, "hash": hash_,
                                           "virtual_ms": int(virtual_ms)}
                pairs = []
                for index in range(cap):
                    inputs = list(dump_dir.glob(f"lcdc-{index:03d}-*.txt"))
                    assert len(inputs) == 1, (index, inputs)
                    panel = dump_dir / f"panel-{index:03d}-output.txt"
                    input_meta = dict(item.split("=", 1)
                                      for item in inputs[0].read_text().split())
                    panel_meta = dict(item.split("=", 1)
                                      for item in panel.read_text().split())
                    assert int(input_meta["panel_frame"]) == index + 1
                    assert input_meta["panel_frame"] == panel_meta["frame"]
                    completion = completions[index + 1]
                    assert completion["source"] == input_meta["source"]
                    assert completion["hash"] == input_meta["hash"]
                    source = inputs[0].with_suffix(".bin").read_bytes()
                    gram = panel.with_suffix(".bin").read_bytes()
                    assert len(source) == DISPLAY_BYTES
                    assert len(gram) == 336 * 480 * 4
                    for component in range(3):
                        assert source[component::3] == gram[component::4], index
                    assert gram[3::4] == b"\xff" * (336 * 480), index
                    pairs.append({"index": index,
                                  "input_frame": int(input_meta["panel_frame"]),
                                  "panel_frame": int(panel_meta["frame"]),
                                  "completion": completion})

                before = saved_hashes()
                assert len(before) == cap * 4, len(before)
                for value in (0x99, 0xAA):
                    qtest(f"memset {SCANOUT:#x} {DISPLAY_BYTES} {value:#x}")
                    write(LCDC + 0x1C4, 0)
                    write(LCDC + 0x224, 1)
                    qtest("clock_step 20000000")
                    assert int(qtest(f"readl {LCDC + 0x1C4:#x}"), 16) & FRAME_COMPLETE == FRAME_COMPLETE
                    assert screenshot(f"past-dump-cap-{value:02x}.ppm") == bytes([value]) * 3
                after = saved_hashes()
                changed = sorted(name for name in before if before[name] != after.get(name))
                warning_count = (args.run_dir / "qemu.log").read_text().count(
                    "invalid MIBAND_DISPLAY_DUMP_LIMIT:")
                report = {"pairs": pairs, "cap": cap, "frames_after_cap": 2,
                          "environment_limit": args.display_dump_limit,
                          "invalid_limit_warning_count": warning_count,
                          "saved_hashes_before": before, "saved_hashes_after": after,
                          "changed_files": changed,
                          "extra_files": sorted(set(after) - set(before))}
                (args.run_dir / "dump-pairing.json").write_text(json.dumps(report, indent=2) + "\n")
                assert after == before, changed
                assert warning_count == int(args.expect_dump_limit_warning), warning_count
                print("LCDC_DUMP_PAIRING_CAP_OK")
        print("LCDC_REQUEST_DMA_SNAPSHOT_PANEL_GRAM_AND_VISIBILITY_OK")
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()
        qemu.stdin.close()
        qemu.stdout.close()
        qemu_log.close()


if __name__ == "__main__":
    main()
