#!/usr/bin/env python3
"""GPU coverage and geometry regression, in a new qtest-only instance.

This validates the stated software 1/4/16 sample candidate, not physical GPU
sample positions. Images are GPU target memory, never LCDC or panel evidence.
Actual path payloads and matrices are replayed with white paint on black so
coverage can be measured independently of source sampling and composition.
"""
import argparse
import bisect
import json
import math
import os
import shlex
import struct
import subprocess
from pathlib import Path

from tests.test_gpu_bgr24_texture_qtest import COMMAND, GPU, fp, ppm, sha, state

ROOT = Path(__file__).resolve().parents[3]
CAPTURE = ROOT / "evidence/runs/gpu-path-aa-audit-20260930/captured-paths.json"
TARGET = 0x3C400000
SOURCE = 0x3C200000


def payload(contours, s16=False):
    result = bytearray()
    for contour in contours:
        for i, point in enumerate(contour):
            result.extend(struct.pack("<h2h" if s16 else "<I2f",
                                      2 if i == 0 else 4, *point))
        result.extend(struct.pack("<h" if s16 else "<I", 1))
    result.extend(struct.pack("<h" if s16 else "<I", 0))
    result.extend(b"\0" * (-len(result) % 8))
    return bytes(result)


def command(raw, width, height, quality=3, evenodd=False, s16=False,
            color=0xFFFFFFFF, matrix=(1, 0, 0, 0, 1, 0), clip=None,
            pattern=False):
    x, y, w, h = clip or (0, 0, width, height)
    states = {0xA00: 0x10001102 if pattern else 0x10000102,
              0xA02: 0 if pattern else color, 0xA10: 0x109,
              0xA11: TARGET, 0xA12: width * 4,
              0xA13: width | height << 16,
              0xA34: (0x03100000 if s16 else 0x03300000) |
                      quality | (0x10 if evenodd else 0),
              0xA39: x | y << 16, 0xA3A: w | h << 16,
              0xAD1: 0}
    states.update({0xA40 + i: fp(v) for i, v in enumerate(matrix)})
    if pattern:
        states.update({0xA18: fp(.5), 0xA19: fp(.5), 0xA1A: fp(1),
                       0xA1C: 0, 0xA1D: 0, 0xA1E: 0,
                       0xA20: 0, 0xA21: 0, 0xA22: 0,
                       0xA25: 0x01000107, 0xA27: 0,
                       0xA29: SOURCE, 0xA2B: 4, 0xA2D: 0,
                       0xA2F: 0x10001})
    return (b"".join(state(r, v) for r, v in states.items()) +
            struct.pack("<II", 0x40000000 | (len(raw) // 8), 0) + raw)


def captured_curves(record):
    raw = bytes.fromhex(record["path_hex"])
    size = 4 if ((record["control"] >> 20) & 3) == 3 else 2
    matrix = [struct.unpack("<f", struct.pack("<I", record["states"][hex(r)]))[0]
              for r in range(0xA40, 0xA46)]
    curves, current, start, offset = [], None, None, 0

    def transform(point):
        x, y = point
        return (matrix[0] * x + matrix[1] * y + matrix[2],
                matrix[3] * x + matrix[4] * y + matrix[5])

    def close():
        if current is not None and current != start:
            curves.append([transform(current), transform(start)])

    while offset + size <= len(raw):
        op = raw[offset]
        offset += size
        if op in (0, 1):
            close()
            current = None
            if op == 0:
                break
            continue
        count = {2: 2, 4: 2, 6: 4, 8: 6}[op]
        values = struct.unpack_from("<" + ("f" if size == 4 else "h") * count,
                                    raw, offset)
        offset += size * count
        points = list(zip(values[::2], values[1::2]))
        if op == 2:
            close()
            current = start = points[0]
        else:
            curves.append([transform(p) for p in [current] + points])
            current = points[-1]
    return curves, matrix


def bezier(points, t):
    values = list(points)
    while len(values) > 1:
        values = [(a[0] * (1 - t) + b[0] * t,
                   a[1] * (1 - t) + b[1] * t)
                  for a, b in zip(values, values[1:])]
    return values[0]


def monotonic_segments(points):
    """Split exactly at y derivative roots, independent of QEMU flattening."""
    roots = []
    y = [p[1] for p in points]
    if len(y) == 3:
        denominator = y[0] - 2 * y[1] + y[2]
        if denominator:
            roots = [(y[0] - y[1]) / denominator]
    elif len(y) == 4:
        a = -y[0] + 3 * y[1] - 3 * y[2] + y[3]
        b = 2 * (y[0] - 2 * y[1] + y[2])
        c = y[1] - y[0]
        if abs(a) < 1e-14:
            roots = [-c / b] if b else []
        elif b * b - 4 * a * c >= 0:
            d = math.sqrt(b * b - 4 * a * c)
            roots = [(-b - d) / (2 * a), (-b + d) / (2 * a)]
    splits = [0] + sorted(t for t in roots if 0 < t < 1) + [1]
    return [(points, a, b, bezier(points, a)[1], bezier(points, b)[1])
            for a, b in zip(splits, splits[1:])]


def exact_curve_coverage(curves, width, height, grid, evenodd, clip):
    segments = [segment for curve in curves for segment in monotonic_segments(curve)]
    result = bytearray(width * height * 3)
    cx, cy, cw, ch = clip
    for y in range(max(0, cy), min(height, cy + ch)):
        counts = [0] * width
        for sy in range(grid):
            sample_y = y + (sy + .5) / grid
            intersections = []
            for points, start, end, ya, yb in segments:
                if not min(ya, yb) <= sample_y < max(ya, yb):
                    continue
                low, high = start, end
                for _ in range(40):
                    middle = (low + high) / 2
                    value = bezier(points, middle)[1]
                    if (value < sample_y) == (yb > ya):
                        low = middle
                    else:
                        high = middle
                intersections.append((bezier(points, (low + high) / 2)[0],
                                      1 if yb > ya else -1))
            intersections.sort()
            positions = [x for x, _ in intersections]
            cumulative = [0]
            for _, direction in intersections:
                cumulative.append(cumulative[-1] + direction)
            for x in range(max(0, cx), min(width, cx + cw)):
                for sx in range(grid):
                    index = bisect.bisect_right(positions, x + (sx + .5) / grid)
                    counts[x] += bool(index % 2 if evenodd else cumulative[index])
        for x, count in enumerate(counts):
            coverage = (count * 255 + grid * grid // 2) // (grid * grid)
            result[(y * width + x) * 3:(y * width + x + 1) * 3] = bytes([coverage]) * 3
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("ap_image", type=Path)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--qmp-port", type=int, default=5481)
    parser.add_argument('--synthetic-only', action='store_true',
                        help='Run independent synthetic oracles; explicitly skip remote capture replays.')
    args = parser.parse_args()
    if args.run_dir.exists():
        parser.error("run directory exists")
    run = args.run_dir.resolve()
    run.mkdir(parents=True)
    (run / "gpu").mkdir()
    env = os.environ.copy()
    for key in ["MIBAND_AGGRESSIVE_DUMP_ARM_FILE", "MIBAND_AGGRESSIVE_DUMP_ON_TOUCH_RELEASE"]:
        env.pop(key, None)
    env.update(MIBAND_AGGRESSIVE_DUMP_DIR=str(run / "gpu"),
               MIBAND_AGGRESSIVE_DUMP_LIMIT="128", MIBAND_GPU_PATH_DIAG="1")
    launch = [str(args.qemu.resolve()), "-M",
              "bes1503,gpu-render-index8=on,gpu-ack-no-render=on,gpu-scanout-bridge=off",
              "-kernel", str(args.ap_image.resolve()), "-display", "none", "-monitor", "none",
              "-accel", "qtest", "-qtest", "stdio", "-qmp",
              f"tcp:127.0.0.1:{args.qmp_port},server=on,wait=off",
              "-serial", "none", "-serial", "none", "-d", "unimp,guest_errors",
              "-D", str(run / "model.log")]
    (run / "launch-command.txt").write_text(shlex.join(launch) + "\n")
    helper = Path(__file__).with_name("test_gpu_bgr24_texture_qtest.py")
    for source in [Path(__file__), helper]:
        (run / source.name).write_bytes(source.read_bytes())
    manifest = {"qemu_sha256": sha(args.qemu.read_bytes()),
                "ap_sha256": sha(args.ap_image.read_bytes()),
                "test_sha256": sha(Path(__file__).read_bytes()),
                "helper_sha256": sha(helper.read_bytes()),
                "captured_geometry_sha256": None if args.synthetic_only else sha(CAPTURE.read_bytes()),
                "layer": "GPU target memory only", "gdb_hooks": [],
                "sample_positions": "candidate centred square grids; no physical GPU validation"}
    results, index = [], 0
    log = (run / "qemu-stderr.log").open("wb")
    process = subprocess.Popen(launch, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=log, env=env, text=True)

    def qtest(text):
        process.stdin.write(text + "\n")
        process.stdin.flush()
        response = process.stdout.readline().strip()
        if not response.startswith("OK"):
            raise RuntimeError((text[:80], response))
        return response[2:].strip()

    def write(address, data):
        for offset in range(0, len(data), 4096):
            part = data[offset:offset + 4096]
            qtest(f"write {address + offset:#x} {len(part)} 0x{part.hex()}")

    def read(address, length):
        return b"".join(bytes.fromhex(qtest(
            f"read {address + offset:#x} {min(4096, length - offset)}").removeprefix("0x"))
            for offset in range(0, length, 4096))

    def check(name, commands, expected, width, height, note, complete=True):
        nonlocal index
        write(TARGET - 16, b"\x9d" * 16 + b"\0" * len(expected) + b"\x9d" * 16)
        write(COMMAND, commands + b"\0" * 8)
        qtest(f"writel {GPU + 0x14:#x} 1")
        qtest(f"writel {GPU + 0x500:#x} {COMMAND:#x}")
        qtest(f"writel {GPU + 0x504:#x} {len(commands) // 8 + 1}")
        irq = int(qtest(f"readl {GPU + 0x10:#x}"), 0)
        actual = read(TARGET, len(expected))
        metadata = dict(item.split("=", 1) for item in
                        (run / "gpu" / f"submit-{index:03d}.txt").read_text().split())
        differences = [(i // 3 % width, i // 3 // width, actual[i], expected[i])
                       for i in range(0, len(expected), 3) if actual[i:i + 3] != expected[i:i + 3]]
        guards = read(TARGET - 16, 16) == b"\x9d" * 16 and read(TARGET + len(expected), 16) == b"\x9d" * 16
        metadata_ok = metadata["rendering_complete"] == str(int(complete))
        result = {"case": name, "note": note, "irq": irq, "metadata": metadata,
                  "different_pixels": len(differences), "first_differences": differences[:20],
                  "expected_sha256": sha(expected), "actual_sha256": sha(actual),
                  "guards_ok": guards, "pass": not differences and guards and metadata_ok}
        results.append(result)
        for label, data in [("expected", expected), ("actual", actual)]:
            (run / f"{name}-{label}.bin").write_bytes(data)
            ppm(run / f"{name}-{label}.ppm", data, width, height)
        (run / f"{name}-command.bin").write_bytes(commands + b"\0" * 8)
        print(name, "PASS" if result["pass"] else "FAIL", len(differences), flush=True)
        index += 1

    rect = [(0, 0), (2, 0), (2, 2), (0, 2)]
    gray = lambda values: b"".join(bytes([value]) * 3 for value in values)
    try:
        check("high-s16-quarter-area", command(payload([rect], True), 3, 3,
              s16=True, matrix=(1, 0, .25, 0, 1, .25)),
              gray([143, 191, 48, 191, 255, 64, 48, 64, 16]), 3, 3,
              "Independent exact rectangle areas: outer product (.75,1,.25); 16-sample grid agrees exactly before 8-bit quantization.")
        medium = gray([64, 128, 64, 128, 255, 128, 64, 128, 64])
        check("medium-fp32-half-area", command(payload([rect]), 3, 3,
              quality=1, matrix=(1, 0, .5, 0, 1, .5)), medium, 3, 3,
              "Independent exact rectangle areas: outer product (.5,1,.5); 4-sample grid agrees.")
        check("low-centre-binary", command(payload([rect]), 3, 3,
              quality=0, matrix=(1, 0, .5, 0, 1, .5)),
              gray([255, 255, 0, 255, 255, 0, 0, 0, 0]), 3, 3,
              "LOW remains binary at one centre per pixel.")
        adjacent = [[(0, 0), (1.5, 0), (1.5, 3), (0, 3)],
                    [(1.5, 0), (3, 0), (3, 3), (1.5, 3)]]
        for evenodd in [False, True]:
            check("adjacent-once-" + str(evenodd), command(payload(adjacent), 3, 3,
                  evenodd=evenodd, color=0x80808080), bytes([128]) * 27, 3, 3,
                  "Premultiplied 50% white shared edge receives one pixel blend, including pixels straddling both contours.")
        overlapping = [[(0, 0), (2, 0), (2, 3), (0, 3)],
                       [(1, 0), (3, 0), (3, 3), (1, 3)]]
        for evenodd in [False, True]:
            check("overlap-fill-" + str(evenodd), command(payload(overlapping), 3, 3,
                  evenodd=evenodd, color=0x80808080),
                  gray(([128, 0, 128] if evenodd else [128] * 3) * 3), 3, 3,
                  "Same winding overlapping contours: nonzero union blends once; evenodd preserves the overlap hole.")
        commands = b"".join(command(payload([rect]), 3, 3, quality=1,
                              matrix=(1, 0, .5, 0, 1, .5), clip=clip)
                             for clip in [(0, 0, 3, 1), (0, 1, 3, 2)])
        check("split-clip-accumulation", commands, medium, 3, 3,
              "Two draws in one submission update disjoint upper/lower rows and preserve their accumulated pixels.")
        write(SOURCE, b"\xff" * 4)
        check("texture-coverage", command(payload([rect]), 3, 3, quality=1,
              matrix=(1, 0, .5, 0, 1, .5), pattern=True), medium, 3, 3,
              "Opaque constant BGRA source uses the same path coverage as solid paint.")
        check("reject-unobserved-upper", command(payload([rect]), 3, 3,
              quality=2), bytes(27), 3, 3,
              "Unobserved quality 2 remains explicitly incomplete, preserving target.", complete=False)
        # Keep curve coverage independently testable without importing the
        # remote firmware captures. The oracle solves Bezier intersections;
        # it does not reproduce the C renderer's polygon subdivision.
        for name, points in (
            # y(t)=1+14t; all sample/curve horizontal separations exceed
            # 0.0092 px, above the 0.0026 px bound derived from the stated
            # 1/512 px flatness tolerance and this curve's maximum slope.
            ("synthetic-quadratic", [(1, 1), (7, 8), (1, 15)]),
            ("synthetic-cubic", [(1.2, 1.4), (2.7, 13.1), (14.3, -.2), (13.2, 9.6)]),
        ):
            # Round the input once to its actual FP32 command representation.
            points = [tuple(struct.unpack('<2f', struct.pack('<2f', *p))) for p in points]
            raw = struct.pack('<I2f', 2, *points[0])
            values = [coordinate for p in points[1:] for coordinate in p]
            raw += struct.pack('<I' + 'f' * len(values), 6 if len(points) == 3 else 8, *values)
            raw += struct.pack('<II', 1, 0)
            raw += bytes(-len(raw) % 8)
            expected = exact_curve_coverage([points, [points[-1], points[0]]],
                                            16, 16, 4, False, (0, 0, 16, 16))
            check(name, command(raw, 16, 16), expected, 16, 16,
                  'Synthetic FP32 curve; exact Bezier-root coverage oracle, no captured image or C flattening reuse.')
        if args.synthetic_only:
            manifest['skipped_captured_replays'] = 3
            manifest['synthetic_only'] = True
            if any(not case['pass'] for case in results):
                raise SystemExit(1)
            return 0

        records = json.loads(CAPTURE.read_text())
        chosen = []
        for control in [0x03100003, 0x03300001, 0x03300013]:
            for record in records:
                if record["control"] != control:
                    continue
                curves, matrix = captured_curves(record)
                if any(len(c) > 2 for c in curves):
                    chosen.append((record, curves, matrix))
                    break
        for record, curves, matrix in chosen:
            regs = record["states"]
            size = regs["0xa13"]
            width, height = size & 0xFFFF, size >> 16
            window, extent = regs["0xa39"], regs["0xa3a"]
            clip = (window & 0xFFFF, window >> 16, extent & 0xFFFF, extent >> 16)
            quality = record["control"] & 3
            grid = 4 if quality == 3 else 2
            evenodd = bool(record["control"] & 0x10)
            expected = exact_curve_coverage(curves, width, height, grid, evenodd, clip)
            check(f"captured-geometry-{record['control']:08x}", command(
                  bytes.fromhex(record["path_hex"]), width, height,
                  quality=quality, evenodd=evenodd,
                  s16=((record["control"] >> 20) & 3) == 1,
                  matrix=matrix, clip=clip), expected, width, height,
                  f"Actual payload/matrix from {record['file']} +{record['offset']:#x}; target/color replaced for coverage measurement. Exact Bezier roots, independent of QEMU curve flattening.")
    finally:
        process.terminate()
        process.wait(timeout=10)
        log.close()
        (run / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        (run / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    if not results or not all(result["pass"] for result in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
