#!/usr/bin/env python3
"""Test the firmware-observed paged PMU calendar interface without guest hooks."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time


ISPI_CONTROL = 0x40009004
ISPI_DATA = 0x40009008


def bcd(value):
    return ((value // 10) << 4) | value % 10


def calendar_seconds(fields):
    year, date, hour, minute_second = fields

    def decode(value):
        return (value >> 4) * 10 + (value & 15)

    return int(datetime(2000 + decode(year & 255),
                        decode((date >> 8) & 31), decode(date & 63),
                        decode(hour & 63), decode((minute_second >> 8) & 127),
                        decode(minute_second & 127),
                        tzinfo=timezone.utc).timestamp())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qemu", type=Path)
    parser.add_argument("ap_image", type=Path)
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args()
    if args.run_dir.exists():
        parser.error(f"run directory already exists: {args.run_dir}")
    args.run_dir.mkdir(parents=True)
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (args.qemu, args.ap_image, Path(__file__))}
    (args.run_dir / "input-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")

    def run(enabled, clock="vm"):
        label = str(enabled) if clock == "vm" else f"{enabled}-{clock}"
        command_line = [
            str(args.qemu), "-M",
            f"bes1503,pmu-rtc-model={'on' if enabled else 'off'},"
            "gpadc-model=on,pmu-status-model=on",
            "-kernel", str(args.ap_image), "-display", "none", "-monitor", "none",
            "-accel", "qtest", "-qtest", "stdio", "-serial", "none", "-serial", "none",
            "-rtc", f"base=2026-09-30T23:59:58,clock={clock}",
            "-d", "unimp,guest_errors", "-D", str(args.run_dir / f"model-{label}.log"),
        ]
        (args.run_dir / f"command-{label}.json").write_text(
            json.dumps(command_line, indent=2) + "\n")
        with (args.run_dir / f"qemu-{label}.log").open("w") as stderr:
            qemu = subprocess.Popen(command_line, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=stderr, text=True)

            def command(line):
                qemu.stdin.write(line + "\n")
                qemu.stdin.flush()
                reply = qemu.stdout.readline().strip()
                if not reply.startswith("OK"):
                    raise RuntimeError(f"QTest {line!r}: {reply!r}")
                return int(reply.split()[-1], 0) if len(reply.split()) > 1 else None

            def write(register, value):
                command(f"writel {ISPI_DATA:#x} {((register << 18) | (value << 2)):#x}")

            def read(register):
                command(f"writel {ISPI_DATA:#x} {((register << 18) | (1 << 26)):#x}")
                return command(f"readl {ISPI_DATA:#x}") >> 2

            def select(chip_select, page):
                command(f"writel {ISPI_CONTROL:#x} {(0x802 | (chip_select << 7)):#x}")
                write(0, 0xa000 + page * 16)

            def calendar():
                select(3, 1)
                return tuple(read(register) for register in (0x16, 0x15, 0x14, 0x13))

            def load(year, month, day, week, hour, minute, second):
                select(3, 1)
                write(0x12, bcd(year % 100))
                write(0x11, (week << 13) | (bcd(month) << 8) | bcd(day))
                write(0x10, bcd(hour))
                write(0x0f, (bcd(minute) << 8) | bcd(second))
                write(0x0e, 1)

            def sample():
                # Avoid treating a split register read over a rollover as a
                # clock-source failure. The hardware snapshot latch is not
                # modeled, so require two consecutive identical calendars.
                for _ in range(8):
                    before = time.monotonic()
                    first = calendar()
                    second = calendar()
                    after = time.monotonic()
                    if first == second:
                        return calendar_seconds(second), before, after
                raise AssertionError("calendar did not settle between samples")

            def elapsed_check(first, second, minimum=0):
                elapsed = second[0] - first[0]
                maximum = math.ceil(second[2] - first[1]) + 1
                assert minimum <= elapsed <= maximum, (clock, first, second)
                return {"rtc_seconds": elapsed,
                        "wall_seconds": second[2] - first[1]}

            def wall_clock_tests():
                evidence = {}
                before = sample()
                command("clock_step 120000000000")
                evidence["virtual_step_120s"] = elapsed_check(before, sample())

                before = sample()
                # Staging all fields must not change the running calendar.
                select(3, 1)
                write(0x12, 0x24)
                write(0x11, (4 << 13) | 0x0228)
                write(0x10, 0x12)
                write(0x0f, 0x3440)
                evidence["staged_write"] = elapsed_check(before, sample())

                write(0x0e, 1)
                assert read(0x0e) & 1 == 0
                before = sample()
                expected = int(datetime(2024, 2, 28, 12, 34, 40,
                                        tzinfo=timezone.utc).timestamp())
                assert 0 <= before[0] - expected <= 1, before
                assert calendar()[1] >> 13 == 4
                # The QTest CPU is stopped and virtual time is unchanged;
                # host and rt must nevertheless advance with elapsed time.
                time.sleep(2.2)
                evidence["wall_progress_after_load"] = elapsed_check(
                    before, sample(), minimum=2)

                before = sample()
                load(2025, 2, 29, 6, 0, 0, 0)
                evidence["invalid_load"] = elapsed_check(before, sample())
                before = sample()
                command("clock_step 120000000000")
                evidence["virtual_step_after_load_120s"] = elapsed_check(
                    before, sample())
                (args.run_dir / f"clock-{clock}-checks.json").write_text(
                    json.dumps(evidence, indent=2) + "\n")
                print(f"PMU_RTC_WALL_CLOCK_LOAD_OK clock={clock}")

            try:
                for chip_select in (0, 1, 3, 4):
                    for page in range(6):
                        select(chip_select, page)
                        write(0x15, 0x1000 + chip_select * 16 + page)
                    for page in range(6):
                        select(chip_select, page)
                        if enabled and chip_select == 3 and page == 1:
                            continue
                        assert read(0x15) == 0x1000 + chip_select * 16 + page
                select(3, 0)
                assert read(0) == 0x1530
                assert read(0x61) == int(enabled)
                select(3, 1)
                write(0x50, 0x1234)
                write(0x5e, 0x1200)
                assert read(0x50) == 0x1234
                assert read(0x5e) == 0x1200
                print(f"ISPI_PAGE_ISOLATION_OK rtc={enabled}")
                if not enabled:
                    select(3, 1)
                    assert read(0x15) == 0x1031
                    command("clock_step 2000000000")
                    assert read(0x15) == 0x1031
                    print("PMU_RTC_DISABLED_OK")
                    return

                if clock != "vm":
                    wall_clock_tests()
                    return

                select(3, 0)
                assert read(0x61) & 1 == 1
                assert calendar() == (0x26, 0x8930, 0x23, 0x5958), calendar()
                command("clock_step 500000000")
                assert calendar() == (0x26, 0x8930, 0x23, 0x5958), calendar()
                command("clock_step 500000000")
                assert calendar() == (0x26, 0x8930, 0x23, 0x5959), calendar()
                command("clock_step 1000000000")
                assert calendar() == (0x26, 0xb001, 0, 0), calendar()
                print("PMU_RTC_MIDNIGHT_OK")

                unchanged = calendar()
                write(0x12, 0x24)
                assert calendar() == unchanged
                load(2024, 2, 28, 4, 23, 59, 59)
                assert read(0x0e) & 1 == 0
                command("clock_step 1000000000")
                assert calendar() == (0x24, 0xa229, 0, 0), calendar()
                command("clock_step 86400000000000")
                assert calendar() == (0x24, 0xc301, 0, 0), calendar()
                print("PMU_RTC_LOAD_AND_LEAP_YEAR_OK")

                load(2026, 12, 31, 5, 23, 59, 59)
                command("clock_step 1000000000")
                assert calendar() == (0x27, 0xc101, 0, 0), calendar()
                print("PMU_RTC_YEAR_ROLLOVER_OK")

                unchanged = calendar()
                load(2025, 2, 29, 6, 0, 0, 0)
                assert calendar() == unchanged
                load(2026, 1, 1, 4, 25, 0, 0)
                assert calendar() == unchanged
                write(0x12, 0x2a)
                write(0x0e, 1)
                assert calendar() == unchanged
                load(2026, 1, 1, 0, 0, 0, 0)
                assert calendar() == unchanged
                command("clock_step 61000000000")
                assert calendar() == (0x27, 0xc101, 0, 0x0101), calendar()
                print("PMU_RTC_INVALID_LOAD_AND_PROGRESS_OK")

                load(2027, 1, 2, 7, 23, 59, 59)
                command("clock_step 1000000000")
                assert calendar() == (0x27, 0x2103, 0, 0), calendar()
                print("PMU_RTC_WEEK_ROLLOVER_OK")
            finally:
                qemu.terminate()
                try:
                    qemu.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    qemu.kill()
                    qemu.wait()
                qemu.stdin.close()
                qemu.stdout.close()

    run(False)
    run(True)
    run(True, "host")
    run(True, "rt")


if __name__ == "__main__":
    main()
