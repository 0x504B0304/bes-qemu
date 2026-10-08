#!/usr/bin/env python3
"""Smoke-test the QEMU H4 endpoint with a pseudo-terminal controller.

This does not require an ESP32.  It verifies that the emulator opens a serial
backend, emits complete H4 commands, and accepts a matching Command Complete
event from the other side of the UART.
"""

import os
import errno
import pty
import select
import signal
import subprocess
import sys
import time


def main() -> int:
    qemu = os.environ.get(
        "MIBAND_QEMU",
        "/tmp/miband-qemu-8.2/build/qemu-system-arm",
    )
    ap = os.environ.get("MIBAND_AP", "/mnt/e/HWRE/Miband/vela_ap.bin")
    resource = os.environ.get(
        "MIBAND_RESOURCE", "/mnt/e/HWRE/Miband/vela_resource.bin"
    )
    log = "/tmp/miband-bt-serial-smoke-qemu.log"

    master, slave = pty.openpty()
    serial_path = os.ttyname(slave)
    command = [
        qemu,
        "-M",
        f"bes1503,resource-image={resource},"
        f"bt-backend=serial:{serial_path}@115200,"
        "bt-smp-just-works=on,bt-passkey=123456",
        "-kernel",
        ap,
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "none",
        "-d",
        "unimp",
        "-D",
        log,
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
    commands = 0
    responses = 0
    acl_injected = 0
    passkey_event_injected = 0
    passkey_reply = 0
    address_command = 0
    buffer = bytearray()
    deadline = time.monotonic() + 6
    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.2)
            if not ready:
                continue
            try:
                data = os.read(master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            buffer.extend(data)
            while len(buffer) >= 4:
                if buffer[0] != 0x01:
                    del buffer[0]
                    continue
                length = 4 + buffer[3]
                if len(buffer) < length:
                    break
                packet = bytes(buffer[:length])
                del buffer[:length]
                opcode = packet[1] | (packet[2] << 8)
                commands += 1
                if (opcode == 0x2005 and packet[3] == 6 and
                        len(packet) == 10):
                    address_command += 1
                if opcode == 0x0c2e:
                    if packet[3] == 10 and packet[10:14] == bytes((0x40, 0xe2, 0x01, 0x00)):
                        passkey_reply += 1
                event = bytes((0x04, 0x0E, 0x04, 0x01,
                               opcode & 0xFF, opcode >> 8, 0x00))
                # Controllers are allowed to split an H4 event at any byte
                # boundary.  Exercise the reassembly path rather than only
                # the convenient single-write case.
                os.write(master, event[:2])
                time.sleep(0.01)
                os.write(master, event[2:5])
                time.sleep(0.01)
                os.write(master, event[5:])
                responses += 1
                if passkey_event_injected == 0:
                    # Classic HCI User Passkey Request: the controller gives
                    # the host a peer address and waits for 0x0c2e.  QEMU's
                    # explicit bt-passkey option should answer it while still
                    # forwarding the event through the AP mailbox.
                    user_passkey = bytes((0x04, 0x16, 0x06,
                                          0x01, 0x02, 0x03, 0x04,
                                          0x05, 0x06))
                    os.write(master, user_passkey[:3])
                    time.sleep(0.01)
                    os.write(master, user_passkey[3:])
                    passkey_event_injected = 1
                if acl_injected == 0:
                    # An unsolicited ACL packet may follow a command event;
                    # it must remain queued until the AP consumes the event.
                    # This is a complete L2CAP SMP Pairing Request, so the
                    # optional just-works policy can be checked as well.
                    acl = bytes((0x02, 0x01, 0x00, 0x0b, 0x00,
                                 0x07, 0x00, 0x06, 0x00,
                                 0x01, 0x03, 0x00, 0x00, 0x10, 0x01, 0x01))
                    os.write(master, acl[:4])
                    time.sleep(0.01)
                    os.write(master, acl[4:])
                    acl_injected = 1
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
    smp_rewrite = qemu_log.count("SMP just-works rewrite")
    passkey_log = qemu_log.count("fixed passkey reply injected value=123456")
    address_forward = qemu_log.count("internal HCI command opcode=2005")
    address_parse = qemu_log.count("LE address type=1 value=be:99:34:45:56:67")
    print(f"serial_commands={commands} serial_responses={responses} "
          f"acl_injected={acl_injected} smp_rewrite={smp_rewrite} "
          f"passkey_event={passkey_event_injected} passkey_reply={passkey_reply} "
          f"address_forward={address_forward} address_parse={address_parse} "
          f"address_command={address_command}")
    if (commands < 3 or responses != commands or smp_rewrite == 0 or
            passkey_event_injected == 0 or passkey_reply == 0 or
            passkey_log == 0 or address_forward == 0 or address_parse == 0 or
            address_command == 0):
        print(f"QEMU log: {log}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
