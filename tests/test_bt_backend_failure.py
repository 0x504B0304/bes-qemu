#!/usr/bin/env python3
"""A requested physical backend must never fall back to synthetic success."""
import os
from pathlib import Path
import pty
import subprocess
import tempfile
import time


def main():
    qemu = os.environ['MIBAND_QEMU']
    resource = os.environ.get('MIBAND_RESOURCE', '/mnt/e/HWRE/Miband/vela_resource.bin')
    ap = os.environ.get('MIBAND_AP', '/mnt/e/HWRE/Miband/vela_ap.bin')
    run = Path(tempfile.mkdtemp(prefix='miband-bt-backend-failure-'))

    def command(backend):
        return [qemu, '-M', f'bes1503,resource-image={resource},bt-backend={backend}',
                '-kernel', ap, '-display', 'none', '-monitor', 'none', '-serial', 'none', '-S']

    invalid = [
        ('serial:/no-such-miband-controller@115200', b'refusing synthetic fallback'),
        ('serial:/dev/null@oops', b'invalid baud'),
        ('serial:/dev/null@', b'invalid baud'),
        ('serial:/dev/null@0', b'invalid baud'),
        ('serial:/dev/null@-1', b'invalid baud'),
        ('serial:/dev/null@4294967296', b'invalid baud'),
        ('serial:/dev/null@115200oops', b'invalid baud'),
        ('serial:', b'path is empty'),
        ('host-hci:', b'invalid host HCI device'),
        ('host-hci:bad', b'invalid host HCI device'),
        ('host-hci:-1', b'invalid host HCI device'),
        ('host-hci:65536', b'invalid host HCI device'),
        ('host-hci-extra', b'unknown backend'),
        ('invalid-backend', b'unknown backend'),
    ]
    for index, (backend, message) in enumerate(invalid):
        result = subprocess.run(command(backend), capture_output=True, timeout=5)
        (run / f'startup-{index}.stderr').write_bytes(result.stderr)
        assert result.returncode != 0, backend
        assert b'miband BT' in result.stderr and message in result.stderr, result.stderr
    master, slave = pty.openpty()
    process = None
    try:
        for prefix, suffix in [('serial', ''), ('serial-btdm', '@0x1c200')]:
            process = subprocess.Popen(command(f'{prefix}:{os.ttyname(slave)}{suffix}'),
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            time.sleep(0.3)
            assert process.poll() is None, 'Default/hex baud PTY should open'
            process.terminate()
            process.communicate(timeout=5)
        process = subprocess.Popen(command(f'serial:{os.ttyname(slave)}@115200'),
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        time.sleep(0.5)
        assert process.poll() is None, 'Valid PTY should open'
        os.close(master)
        master = None
        _, error = process.communicate(timeout=5)
        (run / 'disconnect.stderr').write_bytes(error)
        assert process.returncode != 0
        assert b'serial transport disconnected' in error, error
    finally:
        if master is not None:
            os.close(master)
        os.close(slave)
        if process is not None and process.poll() is None:
            process.terminate()
            process.communicate(timeout=5)
    print(f'PHYSICAL_BACKEND_FAILURE_CHECK_OK invalid={len(invalid)} valid=3 logs={run}')


if __name__ == '__main__':
    main()
