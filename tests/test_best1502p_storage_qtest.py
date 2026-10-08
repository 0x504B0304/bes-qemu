#!/usr/bin/env python3
"""reference NAND program and erase survive SIGKILL, with unchanged base images."""
import argparse
import hashlib
from pathlib import Path
import select
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--ap', required=True)
    parser.add_argument('--resource', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='soc-storage-test-') as directory:
        store = Path(directory) / 'flash.journal'
        machine = (f'miband-best1502p,resource-image={args.resource},flash-state={store},'
                   'nand-id=0xc8f4c8,nand-page-size=2048,nand-spare-size=64,'
                   'nand-page-count=262144,nand-pages-per-block=64')
        command = [args.qemu, '-M', machine, '-kernel', args.ap, '-display', 'none',
                   '-monitor', 'none', '-serial', 'none', '-serial', 'none',
                   '-accel', 'qtest', '-qtest', 'stdio']
        with (Path(directory) / 'stderr').open('w+') as error:
            def launch():
                return subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=error, text=True)
            process = launch()
            def cmd(text):
                process.stdin.write(text + '\n')
                process.stdin.flush()
                assert select.select([process.stdout], [], [], 10)[0], 'QTest timeout'
                answer = process.stdout.readline().strip()
                assert answer.startswith('OK'), (text, answer)
                return answer.split()[1:]
            def write(address, value):
                cmd(f'writel {address:#x} {value:#x}')
            try:
                row = 200000
                write(0x40500018, 2)
                write(0x40500008, 0x88776655)
                write(0x40500000, 0x32)
                write(0x40500000, (row << 8) | 0x10)
                process.kill(); process.wait()
                digest = hashlib.sha256(store.read_bytes()).hexdigest()
                process = launch()
                write(0x40500000, (row << 8) | 0x13)
                assert int(cmd('readl 0x30000000')[0], 0) == 0x88776655
                assert hashlib.sha256(store.read_bytes()).hexdigest() == digest
                write(0x40500000, (row << 8) | 0xd8)
                process.kill(); process.wait()
                process = launch()
                write(0x40500000, (row << 8) | 0x13)
                assert int(cmd('readl 0x30000000')[0], 0) == 0xffffffff
                print('PASS reference NAND program/replay/erase across SIGKILL and restart')
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()


if __name__ == '__main__':
    main()
