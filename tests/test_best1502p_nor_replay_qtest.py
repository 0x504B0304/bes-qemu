#!/usr/bin/env python3
"""Synthetic NOR journal replay reaches all flash aliases and survives restart.

No vendor firmware, identity record, or model-specific flash location is used.
Run with --qemu PATH against a rebuilt core. Uses socket QMP and queued stdout
instead of select() on subprocess pipes, so the harness also runs on Windows.
"""
import argparse
import hashlib
import json
from pathlib import Path
import queue
import socket
import struct
import subprocess
import tempfile
import threading
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='bes-nor-replay-') as temporary:
        root = Path(temporary)
        ap, resource, journal = root / 'ap.bin', root / 'resource.bin', root / 'flash.journal'
        ap.write_bytes(bytes(256))
        resource.write_bytes(b'-rom1fs-' + bytes(2040))
        original = [hashlib.sha256(p.read_bytes()).digest() for p in (ap, resource)]
        flash = bytearray(b'\xff' * 0x1000000)
        flash[0xc0000:0xc0100] = ap.read_bytes()
        # Legacy core hashes 16 bytes, excluding the final '1'. Keep the
        # existing journal identity byte-for-byte; this test must do likewise.
        binding = hashlib.sha256(b'best1502p-nand-v1'[:16] + flash + resource.read_bytes()).digest()
        # Exercise both initial NOR snapshot restoration and appended NOR delta.
        offset = 0xeff040
        flash[offset:offset + 4] = b'BASE'
        header = bytearray(64); header[:8] = b'MBFLASH1'
        struct.pack_into('<4I', header, 8, len(flash), 2112, 262144, zlib.crc32(flash))
        header[32:64] = binding
        payload = b'TEST'
        rec = struct.pack('<3I', 1, offset + 8, len(payload))
        journal.write_bytes(header + flash + rec + struct.pack('<I', zlib.crc32(payload, zlib.crc32(rec))) + payload)
        for _ in range(2):
            with socket.socket() as reserve:
                reserve.bind(('127.0.0.1', 0)); port = reserve.getsockname()[1]
            command = [args.qemu, '-M',
                f'miband-best1502p,resource-image={resource},flash-state={journal},'
                'nand-id=0xc8f4c8,nand-page-size=2048,nand-spare-size=64,'
                'nand-page-count=262144,nand-pages-per-block=64',
                '-kernel', str(ap), '-display', 'none', '-monitor', 'none',
                '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio',
                '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off']
            with (root / 'stderr').open('w+') as errors:
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                           stderr=errors, text=True)
                lines = queue.Queue()
                def reader():
                    for line in process.stdout: lines.put(line)
                    lines.put('')
                thread = threading.Thread(target=reader, daemon=True); thread.start()
                def read(address):
                    process.stdin.write(f'readl {address:#x}\n'); process.stdin.flush()
                    answer = lines.get(timeout=15).strip().split()
                    if len(answer) != 2 or answer[0] != 'OK':
                        errors.flush()
                        raise AssertionError((answer, (root / 'stderr').read_text(errors='replace')[-2000:]))
                    return int(answer[1], 0)
                def verify():
                    for alias in (0x0c000000, 0x28000000, 0x2c000000):
                        assert read(alias + offset) == int.from_bytes(b'BASE', 'little')
                        assert read(alias + offset + 8) == int.from_bytes(b'TEST', 'little')
                        assert read(alias + offset + 12) == 0xffffffff
                try:
                    verify()
                    with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
                        stream = sock.makefile('rwb', buffering=0)
                        assert 'QMP' in json.loads(stream.readline())
                        for cmd in ('qmp_capabilities', 'system_reset'):
                            stream.write(json.dumps({'execute': cmd}).encode() + b'\n')
                            while True:
                                reply = json.loads(stream.readline())
                                assert 'error' not in reply, reply
                                if 'return' in reply: break
                        verify()
                        stream.close()
                finally:
                    if process.poll() is None: process.kill()
                    process.wait(timeout=10); thread.join(timeout=5)
                    process.stdin.close(); process.stdout.close()
        assert [hashlib.sha256(p.read_bytes()).digest() for p in (ap, resource)] == original
    print('PASS synthetic NOR seed and delta replay, all aliases, reset, process restart')


if __name__ == '__main__':
    main()
