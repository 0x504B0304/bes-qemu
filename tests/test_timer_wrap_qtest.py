#!/usr/bin/env python3
"""Check AP timer rates and the MCU free-running counter across a wrap."""

import argparse
import subprocess


TIMER_VALUE = "0x40003004"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qemu", help="clang-23-built qemu-system-arm path")
    parser.add_argument("ap_image", help="unaltered vela_ap.bin path")
    args = parser.parse_args()

    qemu = subprocess.Popen(
        [args.qemu, "-M", "bes1503", "-kernel", args.ap_image,
         "-display", "none", "-monitor", "none", "-accel", "qtest",
         "-qtest", "stdio", "-serial", "none", "-serial", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True,
    )

    def command(line):
        qemu.stdin.write(line + "\n")
        qemu.stdin.flush()
        reply = qemu.stdout.readline().strip()
        if not reply.startswith("OK"):
            raise RuntimeError(f"QTest {line!r}: {reply!r}")
        return reply.split()[-1] if len(reply.split()) > 1 else None

    try:
        # The AP's clock_gettime lower half reads -AON0.VALUE and converts
        # count * 1000000 >> 14. A 16000 Hz model made each minute take
        # 61.44 virtual seconds, although the PMU RTC itself was correct.
        command("writel 0x40084008 0x82")
        aon0_initial = int(command("readl 0x40084004"), 16)
        command("clock_step 1000000000")
        aon0_second = int(command("readl 0x40084004"), 16)
        command("clock_step 59000000000")
        aon0_minute = int(command("readl 0x40084004"), 16)
        second_ticks = (aon0_initial - aon0_second) & 0xffffffff
        minute_ticks = (aon0_initial - aon0_minute) & 0xffffffff
        assert second_ticks == 16384, second_ticks
        assert minute_ticks == 60 * 16384, minute_ticks
        before_us = ((-aon0_initial) & 0xffffffff) * 1000000 >> 14
        after_us = ((-aon0_minute) & 0xffffffff) * 1000000 >> 14
        assert after_us - before_us == 60000000, (before_us, after_us)
        print("AON0_CLOCK_RATE_OK", second_ticks, minute_ticks,
              "guest_elapsed_us", after_us - before_us)

        # The AP independently arms AON1 with 16 ticks per millisecond.
        # Verify count and expiry, without assuming it shares AON0's rate.
        command("writel 0x40084020 16000")
        command("writel 0x40084028 0xa3")
        command("clock_step 500000000")
        assert int(command("readl 0x40084024"), 16) == 8000
        command("clock_step 499999999")
        assert int(command("readl 0x40084030"), 16) == 0
        command("clock_step 1")
        assert int(command("readl 0x40084024"), 16) == 0
        assert int(command("readl 0x40084030"), 16) == 1
        assert int(command("readl 0x40084034"), 16) == 1
        command("writel 0x4008402c 1")
        assert int(command("readl 0x40084030"), 16) == 0
        command("writel 0x40084028 0")
        print("AON1_ALARM_RATE_OK 16000 ticks expired at 1000000000 ns")

        command("writel 0x40003008 0x82")
        mcu_initial = int(command(f"readl {TIMER_VALUE}"), 16)
        command("clock_step 1000000000")
        mcu_second = int(command(f"readl {TIMER_VALUE}"), 16)
        assert (mcu_initial - mcu_second) & 0xffffffff == 6000000
        print("MCU_CLOCK_RATE_OK 6000000 ticks per second")
        command("writel 0x40003008 0")
        command("writel 0x40003000 0xffffffff")
        command("writel 0x40003008 0x82")
        initial = int(command(f"readl {TIMER_VALUE}"), 16)
        command("clock_step 715827882000")
        before_wrap = int(command(f"readl {TIMER_VALUE}"), 16)
        command("clock_step 1000000")
        after_wrap = int(command(f"readl {TIMER_VALUE}"), 16)
        command("clock_step 715827882000")
        second_wrap = int(command(f"readl {TIMER_VALUE}"), 16)
        command("clock_step 3600000000000")
        after_hour = int(command(f"readl {TIMER_VALUE}"), 16)

        assert initial == 0xffffffff, hex(initial)
        assert 0 < before_wrap < 10, hex(before_wrap)
        assert after_wrap > 0xff000000, hex(after_wrap)
        assert second_wrap > 0xff000000, hex(second_wrap)
        assert after_hour != 0, hex(after_hour)
        print("MCU_TIMER_WRAP_OK", *(f"{v:08x}" for v in
              (initial, before_wrap, after_wrap, second_wrap, after_hour)))
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()
        qemu.stdin.close()
        qemu.stdout.close()


if __name__ == "__main__":
    main()
