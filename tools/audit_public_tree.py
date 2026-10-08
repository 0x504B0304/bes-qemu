"""Audit current publishable files, excluding deleted paths and Git-ignored state.

Historical disclosure cannot be revoked. License/source notices are findings
to preserve, not permission to delete attribution. This is a release-tree check.
"""
import json
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
PRODUCT=re.compile(r'band(?:10|11|9)[\w-]*|watchs5|redmi|小米|\b(?:p67|p65|q66|o66tc|p62lte|n67)\b',re.I)


def audit():
    paths=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT)
    findings=[]
    count=0
    for name in sorted(set(paths.decode().split('\0'))- {''}):
        p=ROOT/name
        if not p.is_file() or name=='tools/audit_public_tree.py': continue
        count+=1
        if name.startswith('devices/') or p.suffix in ('.bin','.elf','.yaffs'):
            findings.append(dict(path=name,kind='unexpected product/firmware material'))
        try: text=p.read_text(encoding='utf-8')
        except (UnicodeError,OSError): continue
        for number,line in enumerate(text.splitlines(),1):
            if PRODUCT.search(line) or PRODUCT.search(name):
                protected=bool(re.search('copyright|SPDX|https?://|author',line,re.I))
                findings.append(dict(path=name,line=number,kind='preserve source/license notice' if protected else 'explicit product clue'))
    return dict(schema='bes.public-tree-audit.v1',files_checked=count,findings=findings,
                scope='Current non-ignored source tree; historical/public-review disclosure remains.')


if __name__=='__main__':
    result=audit()
    print(json.dumps(result,indent=2))
    raise SystemExit(any(f['kind']!='preserve source/license notice' for f in result['findings']))
