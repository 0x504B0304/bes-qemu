"""Submit H4 through the BEST1503 mailbox without executing AP instructions."""
import os
import select
import struct
import subprocess
import tempfile


def request(packet):
    return request_many([packet])[0]


def request_many(packets):
    qemu = os.environ['MIBAND_QEMU']
    ap = os.environ['MIBAND_AP']
    with tempfile.TemporaryFile() as stderr:
        proc = subprocess.Popen([qemu, '-M', 'bes1503,bt-backend=synthetic',
            '-kernel', ap, '-display', 'none', '-monitor', 'none', '-serial', 'none',
            '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr)
        def command(text):
            proc.stdin.write((text+'\n').encode())
            proc.stdin.flush()
            if not select.select([proc.stdout], [], [], 5)[0]:
                raise TimeoutError('Qtest mailbox reply timed out')
            reply = proc.stdout.readline().decode().strip().split()
            if not reply or reply[0] != 'OK':
                stderr.seek(0)
                raise RuntimeError((text, reply, stderr.read().decode()))
            return reply[1] if len(reply) > 1 else None
        def write(address, data):
            command(f'write {address:#x} {len(data)} 0x{data.hex()}')
        def word(address, value):
            command(f'writel {address:#x} {value:#x}')
        try:
            responses = []
            for packet in packets:
                word(0xc0004004, 0x20010000)
                word(0x20010000, 0x20010100)
                write(0x20010100, struct.pack('<5I', 0, 0, len(packet), 0x20010200, 1))
                write(0x20010200, packet)
                word(0x400000a0, 0x8004)
                command('clock_step 10000000')
                node = int(command('readl 0xc0004000'), 0)
                assert node, 'controller did not publish a reply descriptor'
                length = int(command(f'readl {node+8:#x}'), 0)
                address = int(command(f'readl {node+12:#x}'), 0)
                assert 0 < length <= 2048, length
                responses.append(bytes.fromhex(command(f'read {address:#x} {length}')[2:]))
                word(0x400000a4, 1)
                word(0xd0330054, 4)
            return responses
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            proc.stdin.close()
            proc.stdout.close()
