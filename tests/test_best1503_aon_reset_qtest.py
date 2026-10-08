"""Exercise BEST1503 AON CMU global reset command semantics."""

import argparse
from pathlib import Path
import subprocess
import tempfile

from tools.qmp_client import Qmp


AON_CMU = 0x40080000
GBL_RESET_PULSE = AON_CMU + 0x0C4
GBL_RESET_SET = AON_CMU + 0x0C8
GBL_RESET_CLR = AON_CMU + 0x0CC


def run(qemu: Path, ap: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="bes1503-aon-reset-") as directory:
        qmp_path = Path(directory) / "qmp.sock"
        process = subprocess.Popen(
            [str(qemu), "-M", "bes1503,bt-backend=synthetic",
             "-kernel", str(ap), "-accel", "qtest", "-qtest", "stdio",
             "-qmp", f"unix:{qmp_path},server=on,wait=off", "-display", "none",
             "-monitor", "none", "-serial", "none", "-serial", "none"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
        )
        qmp = None

        def command(line: str) -> int:
            assert process.stdin is not None and process.stdout is not None
            process.stdin.write(line + "\n")
            process.stdin.flush()
            reply = process.stdout.readline().strip()
            if not reply.startswith("OK"):
                raise AssertionError(f"QTest {line!r}: {reply!r}")
            fields = reply.split()
            return int(fields[-1], 0) if len(fields) > 1 else 0

        try:
            qmp = Qmp(qmp_path, process)

            command(f"writel {GBL_RESET_SET:#x} 0x1")
            assert command(f"readl {GBL_RESET_SET:#x}") == 0x1

            # A non-global bit is latched without resetting the machine.
            command(f"writel {GBL_RESET_SET:#x} 0x40")
            qmp.call("query-status")
            assert command(f"readl {GBL_RESET_SET:#x}") == 0x41

            command(f"writel {GBL_RESET_CLR:#x} 0x1")
            assert command(f"readl {GBL_RESET_SET:#x}") == 0x40

            # PULSE commands are edge-triggered and do not remain latched.
            command(f"writel {GBL_RESET_PULSE:#x} 0x1")
            assert command(f"readl {GBL_RESET_SET:#x}") == 0x40

            # GLOBAL_RESETN (bit 7) requests a machine reset.  The reset
            # callback is asynchronous, so a QMP round trip flushes it before
            # checking that the SoC reset state returned to its defaults.
            command(f"writel {GBL_RESET_SET:#x} 0x80")
            qmp.call("query-status")
            assert command(f"readl {GBL_RESET_SET:#x}") == 0

            command(f"writel {GBL_RESET_PULSE:#x} 0x80")
            qmp.call("query-status")
            assert command(f"readl {GBL_RESET_SET:#x}") == 0
            assert process.poll() is None
        finally:
            if qmp is not None:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            if process.stdin is not None:
                process.stdin.close()
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("qemu", type=Path)
    parser.add_argument("ap_image", type=Path)
    args = parser.parse_args()
    run(args.qemu, args.ap_image)
    print("BEST1503_AON_RESET_OK set/clear/pulse/global-reset")


if __name__ == "__main__":
    main()
