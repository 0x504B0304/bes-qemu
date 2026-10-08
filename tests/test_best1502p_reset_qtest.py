#!/usr/bin/env python3
"""AON global reset vs latch-only peer bits, CLR, and host system_reset."""
import argparse
import json
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()

with tempfile.TemporaryDirectory(prefix='soc-reset-') as temp:
    address = str(Path(temp) / 'qmp')
    q = subprocess.Popen([a.qemu, '-M', 'miband-best1502p', '-kernel', a.ap_image,
        '-display', 'none', '-monitor', 'none', '-serial', 'none', '-serial', 'none',
        '-accel', 'qtest', '-qtest', 'stdio', '-qmp', f'unix:{address},server=on,wait=off'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    events = []
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                s.connect(address)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                assert time.monotonic() < deadline, 'QEMU did not listen'
                time.sleep(.01)
        s.settimeout(5)
        stream = s.makefile('rwb', buffering=0)
        json.loads(stream.readline())

        def qmp(command):
            stream.write(json.dumps({'execute': command}).encode() + b'\n')
            while True:
                reply = json.loads(stream.readline())
                if 'event' in reply:
                    events.append(reply)
                else:
                    assert 'return' in reply, reply
                    return reply['return']

        def cmd(line):
            q.stdin.write(line + '\n'); q.stdin.flush()
            assert select.select([q.stdout], [], [], 5)[0], 'QTest timeout'
            reply = q.stdout.readline().strip()
            assert reply.startswith('OK'), (line, reply)
            return int(reply.split()[1], 0) if len(reply.split()) > 1 else None

        qmp('qmp_capabilities')
        # Unmodeled peer-domain bits store state only; never reset the AP.
        cmd('writel 0x400800c8 1')
        assert cmd('readl 0x400800c8') == 1
        cmd('writel 0x400800cc 0x80')
        cmd('writel 0x400800c4 0')
        qmp('query-status')
        assert not events
        cmd('writel 0x400800cc 1')
        assert cmd('readl 0x400800c8') == 0

        for port in (0x400800c8, 0x400800c4, None):
            events.clear()
            cmd('writel 0x20001000 0xdeadbeef')
            cmd('writel 0x4000c038 0x10')
            if port is None:
                qmp('system_reset')
            else:
                cmd(f'writel {port:#x} 0x80')
            deadline = time.monotonic() + 5
            while not any(e['event'] == 'RESET' for e in events):
                assert time.monotonic() < deadline, 'missing QMP RESET event'
                qmp('query-status')
            event = next(e for e in events if e['event'] == 'RESET')
            assert event['data']['guest'] == (port is not None), event
            assert cmd('readl 0x400800c8') == 0, 'reset latch not cleared'
            assert cmd('readl 0x4000c038') == 0, 'UART runtime state not reset'
            assert cmd('readl 0x20001000') == 0xdeadbeef, 'reset must not erase RAM'
        print('PASS reference AON SET/PULSE reset, CLR/no-op, UART reset and RAM retention')
        stream.close()
    finally:
        s.close()
        q.terminate(); q.wait(timeout=5)
        q.stdin.close(); q.stdout.close()
