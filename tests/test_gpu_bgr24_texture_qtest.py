#!/usr/bin/env python3
"""Isolated GPU regression with captured firmware commands and CPU pixel oracles.

No guest CPU/GDB hook runs. This tests GPU target bytes, not LCDC/panel output.
POINT transparency outside the source is a candidate model policy, not a
measurement of the physical GPU. Firmware replay uses only in-bounds samples.
"""
import argparse
import hashlib
import json
import os
import shlex
import struct
import subprocess
from pathlib import Path

GPU = 0x40220000
COMMAND = 0x38100000
TARGET = 0x20001000
# 0x38100000 aliases 0x3c100000, so keep fixtures outside that command page.
SOURCE_A = 0x3C200000
SOURCE_B = 0x3C210000
PSRAM_END = 0x3E000000
SENTINEL = bytes((0xD3, 0x65, 0xA7))
REPO = Path(__file__).resolve().parents[3]
CAPTURE = REPO / "evidence/runs/watchface-menu-coldboot-20260930/run/gpu"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def word(value):
    return struct.pack("<I", value)


def fp(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def state(register, value):
    return struct.pack("<II", 0x30010000 | register, value)


def path_rectangle(left, top, right, bottom):
    # Same FP32 MOVE/LINE/CLOSE payload layout as captured submit20/21.
    values = [2, fp(left), fp(top), 4, fp(right), fp(top),
              4, fp(right), fp(bottom), 4, fp(left), fp(bottom), 1, 0]
    payload = struct.pack("<14I", *values)
    return struct.pack("<II", 0x40000007, 0) + payload


def pattern(width, height, source, source_width=None, source_height=None,
            register_stride=None, source_format=0x21000100, border_color=0,
            offset_x=0.5, offset_y=0.5, clip=None, rectangle=None):
    sw, sh = source_width or width, source_height or height
    clip = clip or (0, 0, width, height)
    rectangle = rectangle or (0, 0, width, height)
    x, y, cw, ch = clip
    values = [
        (0xA10, 0x109), (0xA11, TARGET), (0xA12, width * 4),
        (0xA13, width | height << 16), (0xA00, 0x10001102),
        (0xA02, 0), (0xA34, 0x03300013),
        (0xA3B, fp(1)), (0xA3C, 0),
        (0xA40, fp(1)), (0xA41, 0), (0xA42, 0),
        (0xA43, 0), (0xA44, fp(1)), (0xA45, 0),
        (0xACD, 0), (0xACE, 0), (0xA1B, 0x11000), (0xA3D, 0x3A9),
        (0xA39, x | y << 16), (0xA3A, cw | ch << 16),
        (0xA18, fp(offset_x / sw)), (0xA19, fp(offset_y / sh)),
        (0xA1A, fp(1)), (0xA1C, fp(1 / sw)), (0xA1D, 0), (0xA1E, 0),
        (0xA1F, 1), (0xA20, 0), (0xA21, fp(1 / sh)), (0xA22, 0),
        (0xA25, source_format), (0xA27, border_color),
        (0xA29, source), (0xA2B, register_stride if register_stride is not None else sw * 4),
        (0xA2D, 0), (0xA2F, sw | sh << 16), (0xAD1, 0),
    ]
    return b"".join(state(reg, val) for reg, val in values) + path_rectangle(*rectangle)


def colorful(width, height, variant=0):
    # Adjacent channels and rows differ, exposing channel swaps, wrong stride,
    # unintended alpha reads, and averaging at the firmware's half-pixel center.
    return bytes(channel for y in range(height) for x in range(width)
                 for channel in ((53*x + 7*y + variant) % 256,
                                 (19*x + 61*y + 37 + variant) % 256,
                                 (97*x + 29*y + 113 + variant) % 256))


def ppm(path, pixels, width, height):
    rgb = bytearray(len(pixels))
    rgb[0::3], rgb[1::3], rgb[2::3] = pixels[2::3], pixels[1::3], pixels[0::3]
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("ap_image", type=Path)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--qmp-port", type=int, default=5351)
    parser.add_argument("--capture-dir", type=Path, default=CAPTURE)
    parser.add_argument('--synthetic-only', action='store_true',
                        help='Run independent synthetic oracles; explicitly skip remote capture replays.')
    args = parser.parse_args()
    if args.run_dir.exists():
        parser.error(f"run directory already exists: {args.run_dir}")
    args.run_dir = args.run_dir.resolve()
    args.run_dir.mkdir(parents=True)
    dump_dir = args.run_dir / "gpu"
    dump_dir.mkdir()
    env = os.environ.copy()
    for key in ["MIBAND_AGGRESSIVE_DUMP_ARM_FILE", "MIBAND_AGGRESSIVE_DUMP_ON_TOUCH_RELEASE"]:
        env.pop(key, None)
    env.update(MIBAND_AGGRESSIVE_DUMP_DIR=str(dump_dir),
               MIBAND_AGGRESSIVE_DUMP_LIMIT="128", MIBAND_GPU_PATH_DIAG="1")
    command = [str(args.qemu.resolve()), "-M",
               "bes1503,gpu-render-index8=on,gpu-ack-no-render=on,gpu-scanout-bridge=off",
               "-kernel", str(args.ap_image.resolve()), "-display", "none", "-monitor", "none",
               "-accel", "qtest", "-qtest", "stdio", "-qmp",
               f"tcp:127.0.0.1:{args.qmp_port},server=on,wait=off",
               "-serial", "none", "-serial", "none", "-d", "unimp,guest_errors",
               "-D", str(args.run_dir / "model.log")]
    (args.run_dir / "launch-command.txt").write_text(shlex.join(command) + "\n")
    manifest = {"qemu": {"path": str(args.qemu.resolve()), "sha256": sha(args.qemu.read_bytes())},
                "ap": {"path": str(args.ap_image.resolve()), "sha256": sha(args.ap_image.read_bytes())},
                "test_sha256": sha(Path(__file__).read_bytes()), "capture_inputs": {}}
    results = []
    stderr = (args.run_dir / "qemu-stderr.log").open("wb")
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=stderr, text=True, env=env)
    submit_index = 0

    def qtest(text):
        proc.stdin.write(text + "\n")
        proc.stdin.flush()
        response = proc.stdout.readline().strip()
        if not response.startswith("OK"):
            raise RuntimeError(f"QTest {text[:100]!r}: {response!r}")
        return response[2:].strip()

    def write_memory(address, data):
        for offset in range(0, len(data), 4096):
            chunk = data[offset:offset + 4096]
            qtest(f"write {address + offset:#x} {len(chunk)} 0x{chunk.hex()}")

    def read_memory(address, size):
        return b"".join(bytes.fromhex(qtest(f"read {address + offset:#x} {min(4096, size - offset)}").removeprefix("0x"))
                        for offset in range(0, size, 4096))

    def submit(data):
        nonlocal submit_index
        if data[-8:] != b"\0" * 8:
            data += b"\0" * 8
        assert len(data) % 8 == 0
        write_memory(COMMAND, data)
        qtest(f"writel {GPU + 0x14:#x} 1")
        qtest(f"writel {GPU + 0x500:#x} {COMMAND:#x}")
        qtest(f"writel {GPU + 0x504:#x} {len(data) // 8}")
        irq = int(qtest(f"readl {GPU + 0x10:#x}"), 0)
        index = submit_index
        submit_index += 1
        meta_path = dump_dir / f"submit-{index:03d}.txt"
        metadata = dict(field.split("=", 1) for field in meta_path.read_text().split())
        return {"index": index, "irq": irq, "metadata": metadata}

    def verify(name, expected, width, height, submission, rendering_complete=True,
               address=TARGET, note=""):
        actual = read_memory(address, len(expected))
        changed = [i // 3 for i in range(0, len(expected), 3) if actual[i:i+3] != expected[i:i+3]]
        xs, ys = [i % width for i in changed], [i // width for i in changed]
        metadata = submission["metadata"]
        expected_skip = 0 if rendering_complete else 1
        metadata_ok = (metadata.get("rendering_complete") == str(int(rendering_complete)) and
                       int(metadata.get("paths_skipped", "-1")) >= expected_skip and
                       (not rendering_complete or metadata.get("paths_skipped") == "0"))
        guard_ok = True
        if address == TARGET:
            guard_ok = (read_memory(address - 16, 16) == b"\x9d" * 16 and
                        read_memory(address + len(expected), 16) == b"\x9d" * 16)
        record = {"case": name, "layer": "GPU target memory only", "note": note,
                  "size": [width, height], "target_address": hex(address),
                  "expected_sha256": sha(expected), "actual_sha256": sha(actual),
                  "different_pixels": len(changed),
                  "difference_bbox_exclusive": [min(xs), min(ys), max(xs)+1, max(ys)+1] if changed else None,
                  "target_guards_unchanged": guard_ok, "submission": submission,
                  "rendering_metadata_ok": metadata_ok,
                  "pass": not changed and guard_ok and metadata_ok}
        results.append(record)
        (args.run_dir / f"{name}-expected.bin").write_bytes(expected)
        (args.run_dir / f"{name}-actual.bin").write_bytes(actual)
        ppm(args.run_dir / f"{name}-expected.ppm", expected, width, height)
        ppm(args.run_dir / f"{name}-actual.ppm", actual, width, height)
        print(f"{name}: {'PASS' if record['pass'] else 'FAIL'} pixels={len(changed)} metadata={metadata_ok}", flush=True)

    def initialize_target(width, height):
        seed = SENTINEL * (width * height)
        write_memory(TARGET - 16, b"\x9d" * 16 + seed + b"\x9d" * 16)
        return seed

    try:
        w, h = 7, 5
        source = colorful(w, h)
        # Three padding pixels per source row; no alpha byte per pixel.
        padded = b"".join(source[y*w*3:(y+1)*w*3] + b"\xf1\x02\xe3" * 3 for y in range(h))
        write_memory(SOURCE_A, padded)
        for phase in (0.25, 0.5, 0.75):
            initialize_target(w, h)
            result = submit(pattern(w, h, SOURCE_A, register_stride=40,
                                    offset_x=phase, offset_y=phase))
            verify(f"point-padded-phase-{str(phase).replace('.', '_')}", source, w, h, result,
                   note="CPU oracle removes row padding; every source pixel must copy exactly, including final row/pixel.")

        # A two-pixel upward page movement, followed by incoming page rows.
        w, h, shift, half = 7, 6, 2, 3
        first, second = colorful(w, h), colorful(w, h, 131)
        write_memory(SOURCE_A, first)
        write_memory(SOURCE_B, second)
        seed = initialize_target(w, h)
        upper = submit(pattern(w, h, SOURCE_A, offset_y=shift+0.5,
                               clip=(0, 0, w, half), rectangle=(0, 0, w, h-shift)))
        expected = first[shift*w*3:(shift+half)*w*3] + seed[half*w*3:]
        verify("moving-pages-upper-half", expected, w, h, upper)
        lower = submit(pattern(w, h, SOURCE_A, offset_y=shift+0.5,
                               clip=(0, half, w, h-half), rectangle=(0, 0, w, h-shift)) +
                       pattern(w, h, SOURCE_B, offset_y=0.5-(h-shift),
                               clip=(0, half, w, h-half), rectangle=(0, h-shift, w, h)))
        expected = first[shift*w*3:] + second[:shift*w*3]
        verify("moving-pages-complete", expected, w, h, lower,
               note="Independent row concatenation oracle; second submission must retain the already drawn upper half.")

        w, h = 7, 5
        source = colorful(w, h)
        write_memory(SOURCE_A, source)
        for name, dx, dy in [("negative-left", -1, 0), ("negative-top", 0, -1),
                             ("right-bottom", 1, 1)]:
            seed = initialize_target(w, h)
            expected = bytearray(seed)
            for y in range(h):
                for x in range(w):
                    sx, sy = x + dx, y + dy
                    if 0 <= sx < w and 0 <= sy < h:
                        expected[(y*w+x)*3:(y*w+x+1)*3] = source[(sy*w+sx)*3:(sy*w+sx+1)*3]
            result = submit(pattern(w, h, SOURCE_A, offset_x=dx+0.75, offset_y=dy+0.75))
            verify(f"candidate-border-{name}", bytes(expected), w, h, result,
                   note="Candidate COLOR a27=0 policy: out-of-bounds POINT sample is transparent, not edge-clamped. No hardware claim.")
        seed = initialize_target(w, h)
        result = submit(pattern(w, h, SOURCE_A, offset_x=float("nan")))
        verify("reject-nonfinite-coordinate", seed, w, h, result, rendering_complete=False,
               note="Reject nonfinite affine state explicitly and preserve the target; this supersedes the old transparent-success fallback.")

        # Exact final mapped source byte is valid; one byte beyond is rejected.
        w, h = 7, 1
        source = colorful(w, h)
        write_memory(PSRAM_END - len(source), source)
        initialize_target(w, h)
        result = submit(pattern(w, h, PSRAM_END - len(source)))
        verify("last-mapped-source-byte", source, w, h, result)
        rejected = [
            ("unknown-filter", {"source_format": 0x21010100}),
            ("unknown-rgb-order", {"source_format": 0x21000120}),
            ("unknown-layout", {"source_format": 0x61000100}),
            ("unverified-border-color", {"border_color": 0xFF123456}),
            ("unaligned-register-stride", {"register_stride": 27}),
            ("undersized-register-stride", {"register_stride": 24}),
            ("source-crosses-map-end", {"source": PSRAM_END - len(source) + 1}),
        ]
        for name, options in rejected:
            seed = initialize_target(w, h)
            options = {"source": SOURCE_A, **options}
            result = submit(pattern(w, h, **options))
            verify(f"reject-{name}", seed, w, h, result, rendering_complete=False,
                   note="Rejected draw must preserve target and report paths_skipped>0/rendering_complete=0 even if the stream is accepted.")

        # The firmware source states below encode POINT. The previous legacy
        # test preserved the incorrect bilinear fallback; it is superseded by
        # this explicit four-corner oracle at (.5,.5) pixel centres.
        w, h = 2, 2
        corners = [bytes((0, 0, 255)), bytes((0, 255, 0)),
                   bytes((255, 0, 0)), bytes((255, 255, 255))]
        legacy_expected = bytes((0,0,255, 0,255,0, 255,0,0, 255,255,255))
        write_memory(SOURCE_A, b"".join(pixel + b"\xff" for pixel in corners))
        initialize_target(w, h)
        result = submit(pattern(w, h, SOURCE_A, source_format=0x01000107,
                                register_stride=8))
        verify("bgra-point", legacy_expected, w, h, result,
               note="Captured BGRA POINT format: fixed red/green/blue/white oracle; no neighbour averaging.")
        write_memory(SOURCE_A, bytes((0, 1, 2, 3)))
        palette = b"".join(state(0xB00 + i, int.from_bytes(pixel + b"\xff", "little"))
                           for i, pixel in enumerate(corners))
        initialize_target(w, h)
        result = submit(palette + pattern(w, h, SOURCE_A, source_format=0x01000900,
                                          register_stride=2))
        verify("index8-point", legacy_expected, w, h, result,
               note="Captured INDEX8 POINT format uses the same fixed four-color oracle.")

        if args.synthetic_only:
            manifest['skipped_captured_replays'] = 2
            manifest['synthetic_only'] = True
            if any(not case['pass'] for case in results):
                raise SystemExit(1)
            return 0

        # Unmodified firmware commands and source bytes; no host recreation of
        # GPU transforms. Oracle is only literal source-row concatenation.
        captured = {}
        for name in ["submit-017-target.bin", "submit-019-target.bin",
                     "submit-020-command.bin", "submit-021-command.bin"]:
            data = (args.capture_dir / name).read_bytes()
            captured[name] = data
            manifest["capture_inputs"][name] = {"path": str((args.capture_dir/name).resolve()),
                                                "bytes": len(data), "sha256": sha(data)}
        first, second = captured["submit-017-target.bin"], captured["submit-019-target.bin"]
        assert len(first) == len(second) == 336 * 480 * 3
        write_memory(0x3C5692C0, first)
        write_memory(0x3C5DF500, second)
        seed = SENTINEL * (336 * 480)
        write_memory(0x20000000, seed)
        upper = submit(captured["submit-020-command.bin"])
        verify("firmware-submit020-upper", first[:336*240*3] + seed[336*240*3:],
               336, 480, upper, address=0x20000000)
        lower = submit(captured["submit-021-command.bin"])
        verify("firmware-submit021-composite", first[:336*452*3] + second[:336*28*3],
               336, 480, lower, address=0x20000000,
               note="Original matrix centers select source17 rows0..451 and source19 rows0..27 at target rows452..479. Both original paths/scissors and two submissions are replayed unchanged.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        stderr.close()
        report = {"scope": "GPU target regression, no display-model or physical-SoC correctness claim",
                  "manifest": manifest, "cases": results,
                  "passed": sum(x["pass"] for x in results),
                  "failed": sum(not x["pass"] for x in results)}
        (args.run_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 1 if any(not x["pass"] for x in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
