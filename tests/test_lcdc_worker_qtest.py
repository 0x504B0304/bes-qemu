#!/usr/bin/env python3
"""Exercise asynchronous LCDC ownership, ordered completion and DMA cancellation.

Starts a new isolated QTest process; never connects to a production instance.
The worker delay is QTest-only and makes races deterministic.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

LCDC = 0x40200000
DMA = 0x40130000
CH7 = DMA + 0x1E0
RAM = 0x20000000
FB = 0x3C085000
PIXELS = 336 * 480
SIZE = PIXELS * 3
DONE = (1 << 21) | (1 << 27)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("qemu", type=Path)
    p.add_argument("ap", type=Path)
    p.add_argument("run", type=Path)
    p.add_argument("--qmp-port", type=int, default=4698)
    a = p.parse_args()
    a.run.mkdir(parents=True, exist_ok=False)
    log = (a.run / "stderr.log").open("w")
    q = subprocess.Popen([
        str(a.qemu), "-M", "bes1503,lcdc-worker=on",
        "-kernel", str(a.ap), "-display", "none", "-monitor", "none",
        "-serial", "none", "-serial", "none", "-accel", "qtest",
        "-qtest", "stdio", "-qmp",
        f"tcp:127.0.0.1:{a.qmp_port},server=on,wait=off",
        "-d", "unimp,guest_errors", "-D", str(a.run / "model.log"),
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True,
        env=dict(os.environ, MIBAND_LCDC_TEST_WORKER_DELAY_US="40000"))
    results = []

    def timed_out(*_):
        raise TimeoutError("asynchronous LCDC regression stalled")

    signal.signal(signal.SIGALRM, timed_out)
    signal.alarm(45)

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

    def fill(address, value, size=SIZE):
        command(f"memset {address:#x} {size} {value:#x}")

    def tick():
        command("clock_step 20000000")

    def panel(value):
        write(LCDC + 0x284, 0x02000000 | (value << 8))
        write(LCDC + 0x280, 1)

    def start(address=FB):
        write(LCDC + 0xF4, address)
        write(LCDC + 0x1C4, 0)
        write(LCDC + 0x224, 1)

    def await_done():
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if read(LCDC + 0x1C4) & DONE == DONE:
                return
            time.sleep(0.002)
        raise AssertionError("LCDC did not complete its accepted request")

    def mark(name):
        results.append(name)
        print(name, flush=True)

    try:
        for _ in range(100):
            try:
                sock = socket.create_connection(("127.0.0.1", a.qmp_port), 0.2)
                break
            except OSError:
                if q.poll() is not None:
                    raise RuntimeError((a.run / "stderr.log").read_text())
                time.sleep(0.02)
        else:
            raise RuntimeError("isolated QMP listener did not start")
        sock.settimeout(5)
        with sock, sock.makefile("rwb") as stream:
            assert "QMP" in json.loads(stream.readline())

            def qmp(execute, arguments=None):
                value = {"execute": execute}
                if arguments:
                    value["arguments"] = arguments
                stream.write(json.dumps(value).encode() + b"\n")
                stream.flush()
                while True:
                    result = json.loads(stream.readline())
                    assert "error" not in result, result
                    if "return" in result:
                        return result["return"]

            qmp("qmp_capabilities")

            def screenshot(name, expected):
                path = a.run / (name + ".ppm")
                qmp("human-monitor-command", {
                    "command-line": f"screendump {path}"})
                with path.open("rb") as image:
                    assert image.readline() == b"P6\n"
                    assert image.readline() == b"336 480\n"
                    assert image.readline() == b"255\n"
                    data = image.read()
                assert data == expected * PIXELS, (name, data[:12].hex())

            write(LCDC + 0x104, (480 << 16) | 336)
            write(LCDC + 0xFC, 336 * 3)
            write(LCDC + 0x264, 0x201)
            write(LCDC + 0x1C0, 1 << 21)

            fill(FB, 0x33)
            start()
            tick()
            assert read(LCDC + 0x1C4) & DONE == 0
            assert read(LCDC + 0x224) & 1
            fill(FB, 0x77)
            await_done()
            screenshot("immutable-input", b"\x33" * 3)
            mark("LCDC_WORKER_IMMUTABLE_INPUT_NO_EARLY_IRQ_OK")

            # Two accepted register configurations survive future reprogramming.
            second = FB + SIZE
            fill(FB, 0x11)
            fill(second, 0x66)
            start(FB)
            start(second)
            tick()
            await_done()
            screenshot("fifo-first", b"\x11" * 3)
            assert read(LCDC + 0x224) & 1
            write(LCDC + 0x1C4, 0)
            tick()
            await_done()
            screenshot("fifo-second", b"\x66" * 3)
            assert read(LCDC + 0x224) & 1 == 0
            mark("LCDC_WORKER_FIFO_CONFIG_AND_BOTH_COMPLETIONS_OK")

            fill(second, 0x55)
            start(second)
            tick()
            panel(0x10)
            time.sleep(0.08)
            assert read(LCDC + 0x1C4) & DONE == 0
            screenshot("sleep-retains-hidden", b"\x00" * 3)
            panel(0x11)
            screenshot("wake-old-gram", b"\x66" * 3)
            tick()
            await_done()
            screenshot("wake-accepts-result", b"\x55" * 3)
            mark("LCDC_WORKER_SLEEP_DEFERS_RESULT_AND_IRQ_OK")

            panel(0x28)
            fill(second, 0x44)
            start(second)
            tick()
            await_done()
            screenshot("off-transfer-hidden", b"\x00" * 3)
            panel(0x29)
            screenshot("off-transfer-gram", b"\x44" * 3)
            mark("LCDC_WORKER_OFF_UPDATES_GRAM_WITHOUT_SHOWING_OK")

            fill(second, 0x22)
            start(second)
            tick()
            panel(0x01)
            panel(0x11)
            panel(0x29)
            time.sleep(0.08)
            tick()
            assert read(LCDC + 0x1C4) & DONE == 0
            assert read(LCDC + 0x224) & 1 == 0
            screenshot("reset-late-result", b"\x00" * 3)
            start(second)
            tick()
            await_done()
            screenshot("reset-new-generation", b"\x22" * 3)
            mark("LCDC_WORKER_RESET_CANCELS_LATE_RESULT_OK")

            # Verify non-gray channel ordering in the board's BGR888 path.
            command(f"write {FB:#x} {SIZE} 0x" + "123456" * PIXELS)
            start(FB)
            tick()
            await_done()
            screenshot("bgr-channel-order", bytes.fromhex("563412"))
            mark("LCDC_WORKER_BGR_CHANNEL_ORDER_OK")

            # Each accepted frame keeps its own completed-DMA snapshot even
            # when another complete copy replaces the same physical buffer.
            def dma_frame(value):
                fill(RAM, value)
                chunk = 4032 * 4
                descriptors = RAM + 0x100000
                chunks = SIZE // chunk
                for index in range(1, chunks):
                    descriptor = descriptors + (index - 1) * 16
                    write(descriptor, RAM + index * chunk)
                    write(descriptor + 4, FB + index * chunk)
                    write(descriptor + 8,
                          descriptors + index * 16 if index + 1 < chunks else 0)
                    write(descriptor + 12, 0x8C489FC0)
                write(CH7, RAM)
                write(CH7 + 4, FB)
                write(CH7 + 8, descriptors)
                write(CH7 + 12, 0x8C489FC0)
                write(CH7 + 16, 0x8001)
                command("clock_step 1000000")
                assert read(CH7 + 16) & 1 == 0

            dma_frame(0x88)
            start(FB)
            dma_frame(0x99)
            start(FB)
            tick()
            await_done()
            screenshot("dma-snapshot-first", b"\x88" * 3)
            write(LCDC + 0x1C4, 0)
            tick()
            await_done()
            screenshot("dma-snapshot-second", b"\x99" * 3)
            mark("LCDC_WORKER_TWO_DMA_GENERATIONS_SAME_BUFFER_OK")

            # DMA start registers latch at the enable edge. Cancelling the
            # channel before its timer cannot produce writes or a completion.
            fill(RAM, 0xA5, 64)
            fill(RAM + 128, 0x5A, 64)
            dst = FB + SIZE * 2
            fill(dst, 0, 256)
            write(DMA + 0x08, 0x80)
            write(CH7, RAM)
            write(CH7 + 4, dst)
            write(CH7 + 8, 0)
            write(CH7 + 12, 0x8C480010)
            write(CH7 + 16, 0x8001)
            write(CH7 + 16, 0)
            command("clock_step 1000000")
            assert read(dst) == 0
            assert read(DMA + 0x14) & 0x80 == 0
            write(CH7 + 16, 0x8001)
            write(CH7, RAM + 128)
            write(CH7 + 4, dst + 128)
            command("clock_step 1000000")
            assert read(dst) == 0xA5A5A5A5
            assert read(dst + 128) == 0
            assert read(CH7 + 16) & 1 == 0
            assert read(DMA + 0x14) & 0x80
            mark("DMA_DISABLE_CANCEL_AND_ENABLE_REGISTER_LATCH_OK")

        (a.run / "results.json").write_text(json.dumps({
            "passed": results, "qemu": str(a.qemu), "qmp_port": a.qmp_port,
            "worker_delay_us": 40000,
        }, indent=2) + "\n")
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
