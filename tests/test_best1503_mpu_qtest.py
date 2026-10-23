# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the BES1503 AP reports the SoC's eight unified MPU regions.

ARMv8-M leaves the number of MPU regions IMPLEMENTATION DEFINED and has the
core report it through MPU_TYPE.DREGION.  This board left mpu-ns-regions and
mpu-s-regions unset, so the guest saw the Cortex-M33 model default of 16
regions while the SoC integrates eight; firmware that sizes its MPU setup from
MPU_TYPE therefore read the wrong value.  An unconfigured machine still reads
0x1000 here.

This checks the guest-visible register rather than the QOM property, so it
also covers the machine property -> ARMv7M -> CPU -> NVIC path.  MPU_TYPE is
not banked: the ARMv7M device forwards one region count to the CPU and QEMU
rejects a machine whose Secure and NonSecure counts differ.  QMP uses a TCP
port because the Windows Python builds lack socket.AF_UNIX.
"""

import argparse
from pathlib import Path
import socket
import subprocess

from tools.qmp_client import Qmp


MPU_TYPE = 0xE000ED90
EXPECTED_REGIONS = 8
EXPECTED_MPU_TYPE = EXPECTED_REGIONS << 8


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run(qemu: Path, ap: Path) -> None:
    endpoint = ("127.0.0.1", free_port())
    process = subprocess.Popen(
        [str(qemu), "-M", "bes1503,bt-backend=synthetic",
         "-kernel", str(ap), "-accel", "qtest", "-qtest", "stdio",
         "-qmp", f"tcp:{endpoint[0]}:{endpoint[1]},server=on,wait=off",
         "-display", "none", "-monitor", "none",
         "-serial", "none", "-serial", "none"],
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
        # Reaching a QMP reply proves the machine realized; a Secure/
        # NonSecure region mismatch would already have aborted it.
        qmp = Qmp(endpoint, process)
        qmp.call("query-status")

        # MPU_TYPE.DREGION is the number of data regions and SEPARATE stays
        # clear for the unified MPU.  An unconfigured machine reads 0x1000
        # (16 regions) from the Cortex-M33 default.
        mpu_type = command(f"readl {MPU_TYPE:#x}")
        assert mpu_type == EXPECTED_MPU_TYPE, (
            f"MPU_TYPE {mpu_type:#010x}: {mpu_type >> 8 & 0xff} regions, "
            f"expected {EXPECTED_REGIONS}")

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
    print(f"BEST1503_MPU_REGIONS_OK mpu_type={EXPECTED_MPU_TYPE:#x} dregion={EXPECTED_REGIONS}")


if __name__ == "__main__":
    main()
