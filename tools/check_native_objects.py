# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile integrated C units first so native portability errors fail promptly."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tools.source_layout import sources


def main(build):
    expected={'hw_arm_'+path.name+'.obj' for path in sources().values() if path.suffix=='.c'}
    output=subprocess.check_output(['ninja','-C',str(build),'-t','targets','all'],text=True)
    targets=[line.split(':',1)[0] for line in output.splitlines()
             if Path(line.split(':',1)[0]).name in expected]
    if not targets:raise RuntimeError('native object targets not found')
    subprocess.run(['ninja','-C',str(build),*targets],check=True)


if __name__=='__main__':main(Path(sys.argv[1]))
