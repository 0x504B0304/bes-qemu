#!/usr/bin/env python3
"""Exercise the BLE fixed-passkey LE Rand hook through the AP mailbox.

The AP image is stopped at its observed HCI send path, then a complete SMP
Pairing Request and an LE Rand command are placed in the same intersys
descriptor format used by the running firmware.  This verifies the QEMU
policy at the HCI boundary without fabricating a controller event or needing
an ESP32 board.
"""

import os
import pty
import signal
import subprocess
import sys
import time


def gdb_expressions(port: int) -> list[str]:
    # The scratch STR instruction is the same ARM Thumb store used by the
    # existing mailbox probes: GDB writes to the guest MMIO map do not invoke
    # QEMU's MemoryRegion callback, while an executed STR does.
    return [
        "set pagination off",
        "set confirm off",
        "set architecture arm",
        f"target remote 127.0.0.1:{port}",
        "break *0x0c2293a4",
        "continue",
        "set $saved_code = *(unsigned short *)0x20001000",
        # SMP Pairing Request: HCI ACL handle 1, L2CAP CID 0x0006.
        # The intersys head points to a descriptor; the descriptor points to
        # the complete 16-byte ACL packet below.
        "set {unsigned int}0x20010000 = 0x20010100",
        "set {unsigned int}0x20010100 = 0x00000000",
        "set {unsigned int}0x20010104 = 0x00000000",
        "set {unsigned int}0x20010108 = 0x00000010",
        "set {unsigned int}0x2001010c = 0x20010200",
        "set {unsigned int}0x20010110 = 0x00000001",
        # HCI ACL (5), L2CAP (4), SMP Pairing Request (7):
        # 02 01 00 0b 00 07 00 06 00 01 03 00 05 10 01 01
        "set {unsigned int}0x20010200 = 0x0b000102",
        "set {unsigned int}0x20010204 = 0x06000700",
        "set {unsigned int}0x20010208 = 0x00030100",
        "set {unsigned int}0x2001020c = 0x01011005",
        "set {unsigned int}0xc0004004 = 0x20010000",
        "set {unsigned short}0x20001000 = 0x6001",
        "set $r0 = 0x400000a0",
        "set $r1 = 4",
        "set $pc = 0x20001000",
        "stepi",
        # HCI LE Rand command (opcode 0x2018), reusing the descriptor.
        "set {unsigned int}0x20010120 = 0x00000000",
        "set {unsigned int}0x20010124 = 0x00000000",
        "set {unsigned int}0x20010128 = 4",
        "set {unsigned int}0x2001012c = 0x20010220",
        "set {unsigned int}0x20010130 = 0x00000001",
        "set {unsigned int}0x20010220 = 0x00201801",
        "set {unsigned int}0x20010000 = 0x20010120",
        "set {unsigned int}0xc0004004 = 0x20010000",
        "set $r0 = 0x400000a0",
        "set $r1 = 4",
        "set $pc = 0x20001000",
        "stepi",
        "set {unsigned short}0x20001000 = $saved_code",
        "detach",
        "quit",
    ]


def main() -> int:
    qemu = os.environ.get(
        "MIBAND_QEMU", "/tmp/miband-qemu-8.2/build/qemu-system-arm"
    )
    ap = os.environ.get("MIBAND_AP", "/mnt/e/HWRE/Miband/vela_ap.bin")
    resource = os.environ.get(
        "MIBAND_RESOURCE", "/mnt/e/HWRE/Miband/vela_resource.bin"
    )
    port = int(os.environ.get("MIBAND_GDB_PORT", "12362"))
    log = os.environ.get("MIBAND_BT_LE_LOG", "/tmp/miband-bt-le-passkey-qemu.log")

    master, slave = pty.openpty()
    serial_path = os.ttyname(slave)
    command = [
        qemu,
        "-M",
        f"bes1503,resource-image={resource},"
        f"bt-backend=serial:{serial_path}@115200,bt-le-passkey=123456",
        "-kernel",
        ap,
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "none",
        "-gdb",
        f"tcp::{port}",
        "-S",
        "-d",
        "unimp",
        "-D",
        log,
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.5)
        args = ["gdb-multiarch", "-q", "-nx", "-batch"]
        for expression in gdb_expressions(port):
            args.extend(["-ex", expression])
        result = subprocess.run(args, capture_output=True, text=True,
                                timeout=12)
        if result.returncode:
            if result.stdout:
                sys.stderr.write(result.stdout)
            if result.stderr:
                sys.stderr.write(result.stderr)
            return 1
    except (subprocess.TimeoutExpired, OSError) as error:
        print(f"GDB probe failed: {error}", file=sys.stderr)
        return 1
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)

    with open(log, "r", encoding="utf-8", errors="replace") as stream:
        qemu_log = stream.read()
    pairing = qemu_log.count("LE SMP pairing AP->BT code=01")
    seed = qemu_log.count("LE fixed passkey seed returned value=123456")
    action = qemu_log.count("opcode=2018 name=le_rand params=0 action=synthetic-le-passkey-rand")
    response = qemu_log.count(
        "BT->AP channel=0 len=15 bytes=04 0e 0c 01 18 20 00 "
        "40 e2 01 00 5a c3 17 a9"
    )
    print(f"le_pairing_seen={pairing} le_seed={seed} "
          f"le_rand_action={action} fixed_seed_response={response}")
    if pairing == 0 or seed == 0 or action == 0 or response == 0:
        print(f"QEMU log: {log}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
