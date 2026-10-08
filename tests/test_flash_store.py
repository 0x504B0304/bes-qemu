#!/usr/bin/env python3
import os
from tools.source_layout import source_file, include_flags
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

repo=Path(__file__).resolve().parent
compiler=os.environ.get('BES_CC', 'gcc' if os.name=='nt' else 'clang-23')
print(subprocess.check_output([compiler,'--version'],text=True).splitlines()[0])
with tempfile.TemporaryDirectory(prefix='miband-store-test-') as tmp:
    tmp=Path(tmp);exe=tmp/('test.exe' if os.name=='nt' else 'test');state=tmp/'flash'
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0','zlib'],text=True))
    subprocess.run([compiler,'-Wall','-Wextra','-Wno-unused-parameter','-Werror',
                    str(repo/'test_flash_store.c'),str(source_file('miband_store.c')),*include_flags(),*flags,'-o',str(exe)],check=True)
    def run(mode,good=True,geometry=None):
        p=subprocess.run([str(exe),mode,str(state)] + ([geometry] if geometry else []),capture_output=True,text=True)
        assert (p.returncode==0)==good,(mode,p.stdout,p.stderr)
        return p
    run('write');run('read');run('badbase',False)
    hold=subprocess.Popen([str(exe),'hold',str(state)],stdout=subprocess.PIPE,text=True)
    try:
        assert hold.stdout.readline().strip()=='LOCKED'
        run('read',False)
    finally:hold.terminate();hold.wait()
    original=state.read_bytes()
    with state.open('ab') as f:f.write(b'partial')
    run('read');assert state.read_bytes()==original
    corrupt=bytearray(original);corrupt[-1]^=1;state.write_bytes(corrupt)
    run('read',False)
    state.write_bytes(original)
    state=tmp/'large-spare-flash'
    run('write',geometry='large-spare');run('read',geometry='large-spare')
    header=state.read_bytes()[:64]
    assert header[:8]==b'MBFLASH1'
    assert struct.unpack_from('<III',header,8)==(0x4000,2176,131072)
    run('read',False)  # Reject opening the new layout as the legacy geometry.
    run('badnor',False,'large-spare');run('badpage',False,'large-spare')
    original=state.read_bytes()
    with state.open('ab') as f:f.write(b'partial')
    run('read',geometry='large-spare');assert state.read_bytes()==original
    corrupt=bytearray(original);corrupt[-1]^=1;state.write_bytes(corrupt)
    run('read',False,'large-spare')
    print('PASS: durable NOR/NAND, erase ordering, base guard, lock, torn-tail recovery, corruption rejection')
