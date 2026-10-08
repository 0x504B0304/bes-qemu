#!/usr/bin/env python3
"""Exercise actual NOR/NAND MMIO, kill QEMU, and verify disk replay."""
import argparse
import hashlib
from pathlib import Path
import signal
import subprocess
import tempfile
p=argparse.ArgumentParser();p.add_argument('--qemu',required=True);p.add_argument('--ap',required=True)
a=p.parse_args()
def timeout(*_):raise TimeoutError('qtest stalled')
signal.signal(signal.SIGALRM,timeout);signal.alarm(45)
with tempfile.TemporaryDirectory(prefix='miband-flash-qtest-') as d:
    d=Path(d);store=d/'flash.journal'
    command=[a.qemu,'-M',f'bes1503,flash-state={store}',
             '-kernel',a.ap,'-display','none','-monitor','none','-serial','none',
             '-serial','none','-accel','qtest','-qtest','stdio']
    with (d/'stderr').open('w') as err:
        def launch():return subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True)
        proc=launch()
        def cmd(text):
            proc.stdin.write(text+'\n');proc.stdin.flush()
            answer=proc.stdout.readline().strip();assert answer.startswith('OK'),(text,answer)
            return answer.split()[1:]
        def w(addr,value):cmd(f'writel {addr:#x} {value:#x}')
        try:
            # NOR filesystem only, never AP instructions.
            w(0x40140000,0xfc000020);w(0x40140018,2)
            w(0x40140000,0xfc000032);w(0x40140008,0x44332211);w(0x40140018,3)
            assert int(cmd('readl 0x0cfc0000')[0],0)==0x44332211
            row=200000
            w(0x40500018,2);w(0x40500008,0x88776655);w(0x40500000,0x32)
            w(0x40500000,(row<<8)|0x10)
            proc.kill();proc.wait()  # No graceful quit/save hook.
            # Preserve compatibility with the historical MBFLASH1 AP-only base.
            assert store.read_bytes()[32:64] == hashlib.sha256(Path(a.ap).read_bytes()).digest()
            before=hashlib.sha256(store.read_bytes()).hexdigest()
            proc=launch()
            assert int(cmd('readl 0x0cfc0000')[0],0)==0x44332211
            w(0x40500000,(row<<8)|0x13)
            # NAND cache noncached alias from the board definition.
            assert int(cmd('readl 0x30000000')[0],0)==0x88776655
            assert hashlib.sha256(store.read_bytes()).hexdigest()==before
            w(0x40500000,(row<<8)|0xd8)
            proc.kill();proc.wait();proc=launch()
            w(0x40500000,(row<<8)|0x13)
            assert int(cmd('readl 0x30000000')[0],0)==0xffffffff
            print('PASS: real QEMU NOR+NAND MMIO persisted through SIGKILL and restart; erase retained')
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
