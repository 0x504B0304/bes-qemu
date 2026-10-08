#!/usr/bin/env python3
"""Exercise PSRAM mode-register transactions and ordinary memory reads."""
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu')
    parser.add_argument('ap_image')
    args = parser.parse_args()
    proc = subprocess.Popen(
        [args.qemu, '-M', 'bes1503', '-kernel', args.ap_image,
         '-display', 'none', '-monitor', 'none', '-accel', 'qtest',
         '-qtest', 'stdio', '-serial', 'none', '-serial', 'none'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)

    def command(line):
        proc.stdin.write(line+'\n')
        proc.stdin.flush()
        reply = proc.stdout.readline().strip().split()
        if not reply or reply[0] != 'OK':
            raise RuntimeError((line, reply))
        return int(reply[1], 16) if len(reply) > 1 else None

    def wr(address, value):
        command(f'writel {address:#x} {value:#x}')

    def rd(address):
        return command(f'readl {address:#x}')

    def transaction(op, address, count=1):
        wr(0x40160008, address)
        wr(0x4016000c, count)
        wr(0x40160004, op)

    try:
        transaction(6, 1)
        assert rd(0x40160018) & 0x1f == 0x0d
        assert rd(0x40160018) == 0, 'one MR word must drain the transfer'
        for reg, value in [(0, 0x16161616), (2, 0x20202020), (4, 0x08080808), (8, 0x41414141)]:
            wr(0x40160014, value)
            transaction(5, reg)
            transaction(6, reg)
            assert rd(0x40160018) == value, (reg, value)
        wr(0x40160014, 0)
        transaction(5, 1)
        transaction(6, 1)
        assert rd(0x40160018) & 0x1f == 0x0d, 'identity is read-only'
        print('PSRAM_MODE_REGISTERS_OK')

        # Calibration writes through the NC alias and uses MC command 2.
        wr(0x38000000, 0xff00ff00)
        wr(0x3c000004, 0x00112233)
        assert rd(0x18000000) == 0xff00ff00
        assert rd(0x1c000004) == 0x00112233
        transaction(2, 0, 2)
        assert rd(0x40160018) == 0xff00ff00
        assert rd(0x40160018) == 0x00112233
        assert rd(0x40160018) == 0
        transaction(2, 0x02000000)
        assert rd(0x40160018) == 0, 'out-of-range transfer must not read RAM'
        # A memory RX transaction must not overwrite the last TX FIFO word.
        wr(0x40160014, 0x70707070)
        transaction(2, 0)
        assert rd(0x40160018) == 0xff00ff00
        transaction(5, 4)
        transaction(6, 4)
        assert rd(0x40160018) == 0x70707070, 'RX must not overwrite TX'
        transaction(6, 1)
        assert rd(0x40160018) & 0x1f == 0x0d
        assert rd(0x40160404) & 1 == 0, 'synchronous command is complete'
        print('PSRAM_MEMORY_AND_ALIASES_OK')
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == '__main__':
    main()
