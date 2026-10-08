"""QEMU microphone voice shutdown ordering (none backend by default)."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.qmp_client import Qmp

def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu','ap','shim'): p.add_argument('--'+name, required=True)
    p.add_argument('--audiodev', default='none,id=hostmic')
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='bes-audio-close-') as temp:
        path = Path(temp)/'qmp.sock'
        with tempfile.TemporaryFile() as log:
            proc = subprocess.Popen([a.qemu,'-M',
                f'bes1503,audio-mode=firmware,audio-source=mic,audio-device=hostmic,audio-shim={a.shim}',
                '-audiodev',a.audiodev,'-kernel',a.ap,'-S','-display','none',
                '-monitor','none','-serial','none','-serial','none',
                '-qmp',f'unix:{path},server=on,wait=off'],stdout=log,stderr=log)
            qmp = None
            try:
                qmp = Qmp(path,proc)
                qmp.call('quit')
                rc = proc.wait(timeout=5)
                log.seek(0)
                assert rc == 0, (rc,log.read().decode())
                print('PASS: paused microphone shutdown does not touch destroyed QEMU audio state')
            finally:
                if qmp: qmp.close()
                if proc.poll() is None: proc.kill(); proc.wait()

if __name__ == '__main__': main()
