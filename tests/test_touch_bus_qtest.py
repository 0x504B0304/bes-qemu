#!/usr/bin/env python3
"""QMP input must reach I2C reports, IRQ latches and wake acknowledgement."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


def main():
    with tempfile.TemporaryDirectory(prefix='bes-touch-qtest-') as temp:
        endpoint = Path(temp)/'qmp.sock'
        process = subprocess.Popen([os.environ['MIBAND_QEMU'], '-M',
            'bes1503,i2c5-probe-address=0x20,i2c5-probe-chip-id=0xe623',
            '-kernel', os.environ['MIBAND_AP'], '-display', 'none', '-monitor', 'none',
            '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio',
            '-qmp', f'unix:{endpoint},server=on,wait=off'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)
        def timeout(*_):
            raise TimeoutError('touch Qtest stalled')
        signal.signal(signal.SIGALRM, timeout)
        signal.alarm(30)
        client = socket.socket(socket.AF_UNIX)
        client.settimeout(5)
        try:
            for _ in range(100):
                try:
                    client.connect(str(endpoint))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if process.poll() is not None:
                        raise RuntimeError(process.stderr.read())
                    time.sleep(.02)
            else:
                raise TimeoutError('QMP startup')
            stream = client.makefile('rwb')
            json.loads(stream.readline())
            def qmp(name, arguments=None):
                message = {'execute': name}
                if arguments is not None:
                    message['arguments'] = arguments
                stream.write((json.dumps(message)+'\n').encode())
                stream.flush()
                while True:
                    reply = json.loads(stream.readline())
                    assert 'error' not in reply, reply
                    if 'return' in reply:
                        return reply['return']
            qmp('qmp_capabilities')
            def cmd(text):
                process.stdin.write(text+'\n')
                process.stdin.flush()
                reply = process.stdout.readline().strip().split()
                assert reply and reply[0] == 'OK', (text, reply)
                return int(reply[1], 0) if len(reply) > 1 else None
            def wr(address, value):
                cmd(f'writel {address:#x} {value:#x}')
            def rd(address):
                return cmd(f'readl {address:#x}')
            def report(register=0x8000, length=10):
                wr(0x40063004, 0x20)
                wr(0x40063010, register >> 8)
                wr(0x40063010, register & 255)
                for i in range(length):
                    wr(0x40063010, 0x100 | (0x200 if i == length-1 else 0))
                return bytes(rd(0x40063010) for _ in range(length))
            def event(x, y, down):
                qmp('input-send-event', {'events': [
                    {'type': 'abs', 'data': {'axis': 'x', 'value': x}},
                    {'type': 'abs', 'data': {'axis': 'y', 'value': y}},
                    {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
            assert report(0xcc, 2) == bytes.fromhex('23 e6')
            assert report(0x1c00, 2) == bytes.fromhex('49 5a')
            assert report() == bytes(10)
            event(0, 0, True)
            event(32767, 32767, True)
            event(32767, 32767, False)
            assert rd(0x4008a044) == 1 << 12
            assert report() == bytes.fromhex('01 00 00 00 00 00 00 00 00 02')
            wr(0x4008a04c, 1 << 12)
            assert rd(0x4008a044) == 0
            cmd('clock_step 10000000')
            assert rd(0x4008a044) == 1 << 12, 'next queued report must reassert IRQ'
            assert report() == bytes.fromhex('01 00 00 00 4f 01 df 01 00 01')
            assert report() == bytes.fromhex('01 00 00 00 4f 01 df 01 00 08')
            wr(0x4008a04c, 1 << 12)
            cmd('clock_step 100000000')
            assert rd(0x4008a044) == 0, 'empty queue must stop IRQ retries'
            assert report() == bytes(10)
            event(0, 0, True)
            assert report(0x2601, 1) == b'\x01'
            assert report(0x2601, 1) == b'\x00'
            assert report() == bytes(10), 'wake read must acknowledge queued reports'
            stream.close()
            print('TOUCH_BUS_OK endpoints=336x480 press-move-release=2/1/8 irq-retry=1 wake-ack=1')
        finally:
            signal.alarm(0)
            client.close()
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stdin.close()
            process.stdout.close()
            process.stderr.close()


if __name__ == '__main__':
    main()
