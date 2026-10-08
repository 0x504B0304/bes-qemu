#!/usr/bin/env python3
"""GPU LINEAR/PAD regression using captured opaque gradient and fixed oracles.

LINEAR here is the captured single-row linear-paint state, not BI_LINEAR.
The candidate interpolation uses the submitted u directly (no extra -0.5).
Real source alpha is always 255; this test does not validate translucent GPUs.
"""
import argparse
import json
import math
import os
import shlex
import struct
import subprocess
from pathlib import Path

from tests.test_gpu_bgr24_texture_qtest import (
    COMMAND, GPU, SOURCE_A, TARGET, fp, pattern, sha, state,
)

REPO = Path(__file__).resolve().parents[3]
CAPTURE = REPO / "evidence/runs/gpu-gradient-source-20260930/run/gpu"
COLORS = [bytes((20, 40, 60, 255)), bytes((100, 120, 140, 255)),
          bytes((220, 200, 180, 255))]
SEED_PIXEL = bytes((0xD3, 0x65, 0xA7))


def draw(width, height, sw=3, sh=1, source_format=0x01011107,
         stride=None, overrides=None, rectangle=False, clip=None):
    data = pattern(width, height, SOURCE_A, source_width=sw, source_height=sh,
                   register_stride=stride if stride is not None else sw*4,
                   source_format=source_format, clip=clip)
    header, path = data[:-64], data[-64:]
    changes = dict(overrides or {})
    if rectangle:
        changes[0xA00] = 0x10001181
        path = struct.pack("<II4H", 0x40000001, 0, 0, 0, width, height)
    return header + b"".join(state(k, v) for k, v in changes.items()) + path


def save_ppm(path, data, width, height, stride, pixel_bytes):
    rgb = bytearray()
    for y in range(height):
        for x in range(width):
            offset = y*stride + x*pixel_bytes
            rgb.extend(data[offset:offset+3][::-1])
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("ap_image", type=Path)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--qmp-port", type=int, default=5355)
    parser.add_argument("--capture-dir", type=Path, default=CAPTURE)
    parser.add_argument('--synthetic-only', action='store_true',
                        help='Run independent synthetic oracles; explicitly skip remote capture replays.')
    args = parser.parse_args()
    if args.run_dir.exists():
        parser.error(f"run directory already exists: {args.run_dir}")
    args.run_dir = args.run_dir.resolve()
    args.run_dir.mkdir(parents=True)
    dump = args.run_dir / "gpu"
    dump.mkdir()
    environment = os.environ.copy()
    for name in ["MIBAND_AGGRESSIVE_DUMP_ARM_FILE", "MIBAND_AGGRESSIVE_DUMP_ON_TOUCH_RELEASE"]:
        environment.pop(name, None)
    environment.update(MIBAND_AGGRESSIVE_DUMP_DIR=str(dump),
                       MIBAND_AGGRESSIVE_DUMP_LIMIT="128", MIBAND_GPU_PATH_DIAG="1")
    command = [str(args.qemu.resolve()), "-M",
               "bes1503,gpu-render-index8=on,gpu-ack-no-render=on,gpu-scanout-bridge=off",
               "-kernel", str(args.ap_image.resolve()), "-display", "none", "-monitor", "none",
               "-accel", "qtest", "-qtest", "stdio", "-qmp",
               f"tcp:127.0.0.1:{args.qmp_port},server=on,wait=off", "-serial", "none",
               "-serial", "none", "-d", "unimp,guest_errors", "-D", str(args.run_dir / "model.log")]
    (args.run_dir / "launch-command.txt").write_text(shlex.join(command) + "\n")
    manifest = {"qemu_sha256": sha(args.qemu.read_bytes()),
                "ap_sha256": sha(args.ap_image.read_bytes()),
                "test_sha256": sha(Path(__file__).read_bytes()),
                "helper_sha256": sha(Path(__file__).with_name("test_gpu_bgr24_texture_qtest.py").read_bytes()),
                "capture_inputs": {}}
    results, submit_index = [], 0
    log = (args.run_dir / "qemu-stderr.log").open("wb")
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=log, env=environment, text=True)

    def qtest(command):
        process.stdin.write(command + "\n")
        process.stdin.flush()
        response = process.stdout.readline().strip()
        if not response.startswith("OK"):
            raise RuntimeError((command[:80], response))
        return response[2:].strip()

    def write(address, data):
        for offset in range(0, len(data), 4096):
            chunk = data[offset:offset+4096]
            qtest(f"write {address+offset:#x} {len(chunk)} 0x{chunk.hex()}")

    def read(address, length):
        return b"".join(bytes.fromhex(qtest(f"read {address+offset:#x} {min(4096,length-offset)}").removeprefix("0x"))
                        for offset in range(0, length, 4096))

    def submit(data):
        nonlocal submit_index
        if data[-8:] != b"\0"*8:
            data += b"\0"*8
        assert len(data) % 8 == 0
        write(COMMAND, data)
        qtest(f"writel {GPU+0x14:#x} 1")
        qtest(f"writel {GPU+0x500:#x} {COMMAND:#x}")
        qtest(f"writel {GPU+0x504:#x} {len(data)//8}")
        irq = int(qtest(f"readl {GPU+0x10:#x}"), 0)
        metadata = dict(item.split("=", 1) for item in
                        (dump/f"submit-{submit_index:03d}.txt").read_text().split())
        result = {"index": submit_index, "irq": irq, "metadata": metadata}
        submit_index += 1
        return result

    def seed(width, height):
        value = SEED_PIXEL*(width*height)
        write(TARGET-16, b"\x9d"*16 + value + b"\x9d"*16)
        return value

    def check(name, expected, width, height, submission, complete=True,
              address=TARGET, pixel_bytes=3, stride=None, selected=None, note="",
              numeric_intervals=None):
        stride = stride or width*pixel_bytes
        actual = read(address, len(expected))
        selected = selected or [(x, y) for y in range(height) for x in range(width)]
        nominal_differences = [(x,y) for x,y in selected
                               if actual[y*stride+x*pixel_bytes:y*stride+(x+1)*pixel_bytes] !=
                               expected[y*stride+x*pixel_bytes:y*stride+(x+1)*pixel_bytes]]
        numeric_intervals = numeric_intervals or {}
        differences, numeric_matches = [], []
        for x,y in nominal_differences:
            intervals = numeric_intervals.get((x,y))
            pixel = actual[y*stride+x*pixel_bytes:y*stride+(x+1)*pixel_bytes]
            if intervals and all(low <= value <= high for value,(low,high) in zip(pixel,intervals)):
                numeric_matches.append((x,y))
            else:
                differences.append((x,y))
        metadata = submission["metadata"]
        metadata_ok = (metadata.get("rendering_complete") == str(int(complete)) and
                       (metadata.get("paths_skipped") == "0" if complete else
                        int(metadata.get("paths_skipped", "0")) > 0))
        guards_ok = address != TARGET or (
            read(address-16,16) == b"\x9d"*16 and read(address+len(expected),16) == b"\x9d"*16)
        result = {"case": name, "note": note, "layer": "GPU target memory",
                  "checked_pixels": len(selected), "different_pixels": len(differences),
                  "nominal_reference_different_pixels": len(nominal_differences),
                  "numeric_ambiguous_pixels": len(numeric_intervals),
                  "differences_within_derived_numeric_interval": numeric_matches,
                  "first_differences": differences[:12], "expected_sha256": sha(expected),
                  "actual_sha256": sha(actual), "submission": submission,
                  "metadata_ok": metadata_ok, "guards_ok": guards_ok,
                  "pass": not differences and metadata_ok and guards_ok}
        results.append(result)
        for label, data in [("expected", expected), ("actual", actual)]:
            (args.run_dir/f"{name}-{label}.bin").write_bytes(data)
            save_ppm(args.run_dir/f"{name}-{label}.ppm", data, width, height, stride, pixel_bytes)
        print(name, "PASS" if result["pass"] else "FAIL", "pixels", len(differences),
              "metadata", metadata_ok, flush=True)

    try:
        write(SOURCE_A, b"".join(COLORS))
        seed(8, 1)
        # Submitted u is direct linear-paint coordinate. Explicit values prove
        # that u=.5 averages endpoints, rather than silently subtracting .5.
        positions = [-2, 0, .25, .5, 1, 1.5, 2, 99]
        expected = bytes((20,40,60, 20,40,60, 40,60,80, 60,80,100,
                          100,120,140, 160,160,160, 220,200,180, 220,200,180))
        commands = b"".join(draw(8, 1, clip=(x,0,1,1), overrides={
            0xA18: fp(u/3), 0xA1C: 0, 0xA20: 0}) for x,u in enumerate(positions))
        check("linear-pad-explicit-u", expected, 8, 1, submit(commands),
              note="Fixed independent color list; no half-texel subtraction. Candidate single-row linear-paint semantics.")
        for rectangle in [False, True]:
            seed(3,3)
            commands = draw(3,3, overrides={0xA18: 0, 0xA19: fp(-.5),
                0xA1C: 0, 0xA1D: fp(-1), 0xA20: fp(1/3), 0xA21: 0}, rectangle=rectangle)
            expected = b"".join(color[:3]*3 for color in COLORS)
            check("linear-vertical-"+("rectangle" if rectangle else "path"), expected,
                  3,3,submit(commands),note="u=y, v=-.5-x; row colors are explicit and all columns must agree.")

        # Legacy coordinates deliberately hit integer texels: their existing
        # bilinear phase stays untouched while off-diagonal mapping changes.
        corners = [bytes((0,0,255,255)), bytes((0,255,0,255)),
                   bytes((255,0,0,255)), bytes((255,255,255,255))]
        rotated = b"".join(corners[i][:3] for i in [2,0,3,1])
        for indexed in [False, True]:
            write(SOURCE_A, bytes((0,1,2,3)) if indexed else b"".join(corners))
            palette = b"".join(state(0xB00+i,int.from_bytes(c,"little"))
                                for i,c in enumerate(corners)) if indexed else b""
            for rectangle in [False, True]:
                seed(2,2)
                commands = palette + draw(2,2,sw=2,sh=2,
                    source_format=0x01000900 if indexed else 0x01000107,
                    stride=2 if indexed else 8, rectangle=rectangle,
                    overrides={0xA18:0,0xA19:fp(.5),0xA1C:0,
                               0xA1D:fp(-.5),0xA20:fp(.5),0xA21:0})
                check("legacy-rotated-"+("index8" if indexed else "bgra")+
                      ("-rectangle" if rectangle else "-path"), rotated,2,2,submit(commands),
                      note="Fixed [blue,red;white,green], u=y/v=1-x; existing integer sampling phase unchanged.")

        rejected = [
            ("translucent-unsampled-texel", {"source": b"".join(COLORS[:2])+COLORS[2][:3]+b"\x80"}),
            ("multiple-rows", {"source": b"".join(COLORS)*2, "sh":2}),
            ("different-filter", {"source_format":0x01021107}),
            ("nonzero-border-state", {"overrides":{0xA27:1}}),
            ("nonfinite-affine", {"overrides":{0xA18:fp(float("nan"))}}),
            ("projective-state", {"overrides":{0xA1E:fp(.125)}}),
            ("undersized-stride", {"stride":8}),
        ]
        for name, options in rejected:
            options = dict(options)
            write(SOURCE_A, options.pop("source", b"".join(COLORS)))
            before = seed(1,1)
            check("reject-"+name,before,1,1,submit(draw(1,1,**options)),complete=False,
                  note="Outside the captured opaque single-row finite-affine format scope; target must be preserved.")

        if args.synthetic_only:
            manifest['skipped_captured_replays'] = 1
            manifest['synthetic_only'] = True
            if any(not case['pass'] for case in results):
                raise SystemExit(1)
            return 0

        # Reproduce the captured first gradient draw, preserving its states,
        # original command bytes, path, source and target-before bytes.
        capture = args.capture_dir / "gradient-source"
        inputs = {}
        for name in ["source.bin","states-le.bin","path.bin","target-before.bin"]:
            inputs[name] = (capture/name).read_bytes()
            manifest["capture_inputs"][name] = {"path":str((capture/name).resolve()),
                "sha256":sha(inputs[name]),"bytes":len(inputs[name])}
        original = (args.capture_dir/"submit-002-command.bin").read_bytes()
        manifest["capture_inputs"]["submit-002-command.bin"] = {
            "path":str((args.capture_dir/"submit-002-command.bin").resolve()),
            "sha256":sha(original),"bytes":len(original)}
        states = struct.unpack("<4096I",inputs["states-le.bin"])
        sw, sh = states[0xA2F]&0xFFFF, states[0xA2F]>>16
        assert (sw,sh) == (1024,1) and set(inputs["source.bin"][3::4]) == {255}
        offset = original.index(inputs["path.bin"])
        draw_header = struct.unpack_from("<II",original,offset-8)
        assert draw_header == (0x40000000 | (len(inputs["path.bin"])//8),0)
        prefix_end = offset+len(inputs["path.bin"])
        prefix = original[:prefix_end] + b"\0"*8
        (args.run_dir/"captured-command-prefix.bin").write_bytes(prefix)
        # All captured state values are restored without rendering. This
        # supplies inherited registers absent from the original prefix.
        setup = b"".join(state(register,value) for register,value in enumerate(states))
        submit(setup)
        write(states[0xA29],inputs["source.bin"])
        write(states[0xA11],inputs["target-before.bin"])
        captured_submit = submit(prefix)
        c0, du_dx, du_dy = [struct.unpack("<f",struct.pack("<I",states[r]))[0]
                           for r in [0xA18,0xA1C,0xA20]]
        width,height,stride = states[0xA13]&0xFFFF,states[0xA13]>>16,states[0xA12]
        expected = bytearray(inputs["target-before.bin"])
        # The captured rounded path transforms to [0,326]x[0,210], radius36.
        # A strict interior strip x=48..277 avoids all curved-edge raster rules.
        selected = [(x,y) for y in range(height) for x in range(48,278)]
        source = inputs["source.bin"]
        numeric_intervals, numeric_details = {}, []
        unit_roundoff = 2**-24
        gamma6 = 6*unit_roundoff/(1-6*unit_roundoff)
        gamma4 = 4*unit_roundoff/(1-4*unit_roundoff)

        def paint_value(position, channel):
            position = min(sw-1,max(0,position))
            left,right = math.floor(position),min(sw-1,math.floor(position)+1)
            fraction = position-left
            return source[left*4+channel]*(1-fraction)+source[right*4+channel]*fraction

        for x,y in selected:
            position = min(sw-1,max(0,(c0 + du_dx*x + du_dy*y)*sw))
            left,right = math.floor(position),min(sw-1,math.floor(position)+1)
            weight = position-left
            offset = y*stride+x*4
            for channel in range(3):
                value = source[left*4+channel]*(1-weight)+source[right*4+channel]*weight
                expected[offset+channel] = math.floor(value+.5)
            expected[offset+3] = 255
            # A local IEEE-754 forward-error envelope, not a global pixel
            # tolerance. Register coefficients are binary32, while this
            # independent matrix/paint oracle uses Python binary64. Bound
            # products, additions and interpolation before integer rounding;
            # only a boundary straddling N+.5 may admit two channel values.
            coordinate_error = gamma6*(abs(c0)+abs(du_dx*x)+abs(du_dy*y))*sw
            low_u,high_u = position-coordinate_error,position+coordinate_error
            samples = [low_u,high_u]
            samples += list(range(math.ceil(low_u),math.floor(high_u)+1))
            intervals = []
            for channel in range(3):
                values = [paint_value(u,channel) for u in samples]
                paint_error = gamma4*max(1,max(abs(value) for value in values))
                low = max(0,math.floor(min(values)-paint_error+.5))
                high = min(255,math.floor(max(values)+paint_error+.5))
                intervals.append((low,high))
            intervals.append((255,255))
            if any(low != high for low,high in intervals):
                numeric_intervals[(x,y)] = intervals
                numeric_details.append({"x":x,"y":y,"nominal_u":position,
                    "coordinate_error_bound":coordinate_error,"channel_integer_intervals":intervals})
        (args.run_dir/"captured-gradient-numeric-envelope.json").write_text(json.dumps({
            "unit_roundoff":unit_roundoff,"gamma6_coordinate":gamma6,"gamma4_paint":gamma4,
            "method":"IEEE-754 forward-error bound on matrix and 1D paint arithmetic; only final integer rounding boundaries receive per-channel intervals. No global plus/minus-one allowance.",
            "pixels":numeric_details},indent=2)+"\n")
        check("captured-gradient-safe-interior",bytes(expected),width,height,captured_submit,
              address=states[0xA11],pixel_bytes=4,stride=stride,selected=selected,
              numeric_intervals=numeric_intervals,
              note="Original prefix through first gradient DRAW plus END; captured states prime inherited registers. 48300 guaranteed interior pixels checked without copying curved-edge rasterization. Opaque source only; nominal double-reference differences remain reported, and only derived local floating-point rounding intervals are accepted.")
        actual = read(states[0xA11],len(expected))
        padding = [y*stride+i for y in range(height) for i in range(width*4,stride)]
        results[-1]["target_row_padding_unchanged"] = all(actual[i]==inputs["target-before.bin"][i] for i in padding)
        results[-1]["pass"] &= results[-1]["target_row_padding_unchanged"]
        manifest["replay_prefix"] = {"original_bytes_retained":prefix_end,
            "appended_end_bytes":8,"sha256":sha(prefix),
            "inherited_states_restored":True,"gradient_source_alpha_values":[255]}
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        log.close()
        report = {"scope":"GPU targets only; trace-informed LINEAR semantics, not a physical GPU pixel oracle",
                  "manifest":manifest,"cases":results,
                  "passed":sum(x["pass"] for x in results),
                  "failed":sum(not x["pass"] for x in results)}
        (args.run_dir/"report.json").write_text(json.dumps(report,indent=2)+"\n")
    return int(any(not x["pass"] for x in results))


if __name__ == "__main__":
    raise SystemExit(main())
