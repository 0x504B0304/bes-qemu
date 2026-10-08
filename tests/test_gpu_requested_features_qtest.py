#!/usr/bin/env python3
"""Independent small color oracles and captured firmware GPU input replays.

QTest runs no guest CPU and has no GDB hooks. This checks GPU target bytes;
it does not claim physical GPU, LCDC, panel GRAM or QMP window equivalence.
"""
import argparse
import json
import math
import os
import shlex
import struct
import subprocess
from pathlib import Path

from tests.test_gpu_bgr24_texture_qtest import (
    COMMAND, GPU, SOURCE_A, TARGET, fp, pattern, sha, state,
)

REPO = Path(__file__).resolve().parents[3]
CAPTURE = REPO/'evidence/runs/gpu-requested-inputs-20260930/run/gpu/draw-inputs'
PREVIEW = REPO/'evidence/runs/gpu-preview-inputs-20260930/run/gpu/draw-inputs'
DEST = bytes((100,80,60))


def round_byte(value):
    return max(0,min(255,math.floor(value+0.5)))


def over(source, destination):
    # Premultiplied components: source is not multiplied by alpha again.
    result = [min(255,s+round_byte(d*(255-source[3])/255))
              for s,d in zip(source[:3],destination[:3])]
    if len(destination)==4:
        result.append(min(255,source[3]+round_byte(destination[3]*(255-source[3])/255)))
    return bytes(result)


def draw(fmt, width=1, height=1, sw=2, sh=2, stride=None, overrides=None,
         rectangle=False, clip=None):
    data = pattern(width,height,SOURCE_A,source_width=sw,source_height=sh,
                   register_stride=stride or sw*4,source_format=fmt,clip=clip)
    changes = dict(overrides or {})
    # Constant coordinates isolate the filter from geometry and interpolation
    # gradients. Test fixtures override this explicitly for rotation/replay.
    changes = {0xA1C:0,0xA1D:0,0xA20:0,0xA21:0,**changes}
    payload = data[-64:]
    if rectangle:
        changes[0xA00] = 0x10001181
        payload = struct.pack('<II4H',0x40000001,0,0,0,width,height)
    return data[:-64]+b''.join(state(r,v) for r,v in changes.items())+payload


def save_ppm(path, data, width, height, pixel_bytes, stride):
    rgb = bytearray()
    for y in range(height):
        for x in range(width):
            offset = y*stride+x*pixel_bytes
            rgb.extend(data[offset:offset+3][::-1])
    path.write_bytes(f'P6\n{width} {height}\n255\n'.encode()+rgb)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu',type=Path)
    parser.add_argument('ap_image',type=Path)
    parser.add_argument('run_dir',type=Path)
    parser.add_argument('--qmp-port',type=int,default=5362)
    parser.add_argument('--synthetic-only', action='store_true',
                        help='Run independent synthetic oracles; explicitly skip remote capture replays.')
    args = parser.parse_args()
    if args.run_dir.exists():
        parser.error('run directory already exists')
    args.run_dir = args.run_dir.resolve()
    args.run_dir.mkdir(parents=True)
    dump = args.run_dir/'gpu'
    dump.mkdir()
    environment = os.environ.copy()
    for key in list(environment):
        if key.startswith('MIBAND_GPU_') or key.startswith('MIBAND_AGGRESSIVE_DUMP_'):
            environment.pop(key)
    environment.update(MIBAND_AGGRESSIVE_DUMP_DIR=str(dump),
                       MIBAND_AGGRESSIVE_DUMP_LIMIT='128',MIBAND_GPU_PATH_DIAG='1')
    command = [str(args.qemu.resolve()),'-M',
               'bes1503,gpu-render-index8=on,gpu-ack-no-render=on,gpu-scanout-bridge=off',
               '-kernel',str(args.ap_image.resolve()),'-display','none','-monitor','none',
               '-accel','qtest','-qtest','stdio','-qmp',
               f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off','-serial','none',
               '-serial','none','-d','unimp,guest_errors','-D',str(args.run_dir/'model.log')]
    (args.run_dir/'launch-command.txt').write_text(shlex.join(command)+'\n')
    manifest = {'qemu_sha256':sha(args.qemu.read_bytes()),
                'ap_sha256':sha(args.ap_image.read_bytes()),
                'test_sha256':sha(Path(__file__).read_bytes()),
                'helper_sha256':sha(Path(__file__).with_name('test_gpu_bgr24_texture_qtest.py').read_bytes()),
                'capture_inputs':{}}
    results, index = [],0
    log = (args.run_dir/'qemu-stderr.log').open('wb')
    proc = subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                            stderr=log,env=environment,text=True)

    def qtest(text):
        proc.stdin.write(text+'\n');proc.stdin.flush()
        response = proc.stdout.readline().strip()
        if not response.startswith('OK'):
            raise RuntimeError((text[:80],response))
        return response[2:].strip()

    def write(address,data):
        for offset in range(0,len(data),4096):
            chunk=data[offset:offset+4096]
            qtest(f'write {address+offset:#x} {len(chunk)} 0x{chunk.hex()}')

    def read(address,size):
        return b''.join(bytes.fromhex(qtest(f'read {address+offset:#x} {min(4096,size-offset)}').removeprefix('0x'))
                        for offset in range(0,size,4096))

    def submit(data):
        nonlocal index
        # Payloads can end in END path words: always append command END.
        data += b'\0'*8
        assert len(data)%8==0
        write(COMMAND,data)
        qtest(f'writel {GPU+0x14:#x} 1')
        qtest(f'writel {GPU+0x500:#x} {COMMAND:#x}')
        qtest(f'writel {GPU+0x504:#x} {len(data)//8}')
        irq=int(qtest(f'readl {GPU+0x10:#x}'),0)
        metadata=dict(item.split('=',1) for item in (dump/f'submit-{index:03d}.txt').read_text().split())
        result={'index':index,'irq':irq,'metadata':metadata}
        index+=1
        return result

    def seed(width=1,height=1,pixel=DEST):
        data=pixel*(width*height)
        write(TARGET-16,b'\x9d'*16+data+b'\x9d'*16)
        return data

    def check(name,expected,width,height,submission,address=TARGET,pixel_bytes=3,
              stride=None,selected=None,complete=True,note='',details=None):
        stride=stride or width*pixel_bytes
        actual=read(address,len(expected))
        selected=selected if selected is not None else [(x,y) for y in range(height) for x in range(width)]
        different=[]
        for x,y in selected:
            at=y*stride+x*pixel_bytes
            if expected[at:at+pixel_bytes]!=actual[at:at+pixel_bytes]:
                different.append({'x':x,'y':y,'expected':list(expected[at:at+pixel_bytes]),
                                  'actual':list(actual[at:at+pixel_bytes])})
        metadata=submission['metadata']
        metadata_ok=(metadata.get('rendering_complete')==str(int(complete)) and
                     (metadata.get('paths_skipped')=='0' if complete else int(metadata.get('paths_skipped','0'))>0))
        guards_ok=address!=TARGET or (read(address-16,16)==b'\x9d'*16 and
                                     read(address+len(expected),16)==b'\x9d'*16)
        record={'case':name,'layer':'GPU target memory','note':note,'details':details,
                'size':[width,height],'checked_pixels':len(selected),'different_pixels':len(different),
                'first_differences':different[:16],'expected_sha256':sha(expected),'actual_sha256':sha(actual),
                'submission':submission,'metadata_ok':metadata_ok,'guards_ok':guards_ok,
                'pass':not different and bool(selected) and metadata_ok and guards_ok}
        results.append(record)
        for label,data in [('expected',expected),('actual',actual)]:
            (args.run_dir/f'{name}-{label}.bin').write_bytes(data)
            save_ppm(args.run_dir/f'{name}-{label}.ppm',data,width,height,pixel_bytes,stride)
        print(name,'PASS' if record['pass'] else 'FAIL','pixels',len(different),'metadata',metadata_ok,flush=True)

    def captured(directory):
        inputs={p.name:p.read_bytes() for p in directory.iterdir() if p.is_file()}
        for name,data in inputs.items():
            manifest['capture_inputs'][str((directory/name).relative_to(REPO))]=sha(data)
        regs=list(struct.unpack('<4096I',inputs['states-le.bin']))
        width,height=regs[0xA13]&65535,regs[0xA13]>>16
        pixel_bytes=3 if regs[0xA10]==0x109 else 4
        stride=regs[0xA12]//4*3 if pixel_bytes==3 else regs[0xA12]
        # BGR24's register pitch uses four-byte slots, memory packs 3 bytes.
        source=inputs.get('source.bin',b'')
        logical_stride=regs[0xA2B]//4*3 if regs[0xA25]&0x20000000 else regs[0xA2B]
        logical_bytes=logical_stride*(regs[0xA2F]>>16)
        if source:
            assert len(source)>=logical_bytes
            write(regs[0xA29],source[:logical_bytes])
        write(regs[0xA11],inputs['target-before.bin'])
        # Restore all captured state, including CLUT; no fabricated inherited
        # state, changed matrices, paths, addresses or target contents.
        commands=b''.join(state(r,v) for r,v in enumerate(regs))
        payload=inputs['draw.bin']
        assert len(payload)%8==0
        commands+=struct.pack('<II',0x40000000|len(payload)//8,0)+payload
        detail={'capture':str(directory.relative_to(REPO)),'saved_source_bytes':len(source),
                'logical_source_bytes_replayed':logical_bytes if source else 0,
                'source_tail_excluded':len(source)-logical_bytes if source else 0,
                'registers':{hex(r):hex(regs[r]) for r in [0xA00,0xA02,0xA10,0xA11,0xA12,0xA13,
                                                           0xA25,0xA29,0xA2B,0xA2F,0xA34,0xA39,0xA3A]}}
        return regs,inputs,source[:logical_bytes],commands,width,height,stride,pixel_bytes,detail

    try:
        # A 2x2 square with unequal channels, plus one padding pixel per row.
        # At submitted (.5,.5), POINT is top-left, BILINEAR is their mean.
        corners=[bytes((0,0,252,255)),bytes((0,252,0,255)),
                 bytes((252,0,0,255)),bytes((252,252,252,255))]
        for layout,point,bilinear in [('index8',0x01000900,0x01020900),
                                      ('bgra',0x01000107,0x01020107),
                                      ('bgr24',0x21000100,0x21020100)]:
            if layout=='index8':
                source=bytes((0,1,254,2,3,254));pitch=3
                palette=b''.join(state(0xB00+i,int.from_bytes(c,'little')) for i,c in enumerate(corners))
            else:
                bpp=4 if layout=='bgra' else 3
                source=b''.join(b''.join(c[:bpp] for c in corners[y*2:y*2+2])+b'\xe7'*bpp for y in range(2))
                pitch=12;palette=b''
            write(SOURCE_A,source)
            for filter_name,fmt,expected in [('point',point,bytes((0,0,252))),
                                            ('bilinear',bilinear,bytes((126,126,126)))]:
                for rectangle in [False,True]:
                    seed()
                    check(f'{layout}-{filter_name}-'+('rect' if rectangle else 'path'),expected,1,1,
                          submit(palette+draw(fmt,stride=pitch,rectangle=rectangle)),
                          note='Fixed hand-calculated colors at (.5,.5); padding must not enter either sample.')

        write(SOURCE_A,bytes((20,40,60,255,100,120,140,255,220,200,180,255)))
        for name,u,expected in [('middle',0.5,(60,80,100)),('left',-4,(20,40,60)),('right',99,(220,200,180))]:
            seed()
            check('linear-pad-'+name,bytes(expected),1,1,
                  submit(draw(0x01011107,sw=3,sh=1,overrides={0xA18:fp(u/3)})))

        # Premultiplied inputs distinguish S + D*(1-A) from double-alpha.
        translucent=bytes((20,40,60,128))
        write(SOURCE_A,translucent)
        for fmt in [0x01000107,0x01020107,0x01000900,0x01020900]:
            indexed=fmt&255==0
            write(SOURCE_A,b'\0' if indexed else translucent)
            prefix=state(0xB00,int.from_bytes(translucent,'little')) if indexed else b''
            seed()
            check(f'premult-source-{fmt:08x}',bytes((70,80,90)),1,1,
                  submit(prefix+draw(fmt,sw=1,sh=1,stride=1 if indexed else 4,
                                     overrides={0xA18:0,0xA19:0})))

        seed()
        check('premult-solid-src-over',bytes((70,80,90)),1,1,
              submit(draw(0x01000107,overrides={0xA00:0x10000102,0xA02:0x803c2814})))
        seed(pixel=bytes((100,80,60,200)))
        check('premult-target-alpha',bytes((70,80,90,228)),1,1,
              submit(draw(0x01000107,overrides={0xA00:0x10000102,0xA02:0x803c2814,
                                                0xA10:0x103})),pixel_bytes=4)

        write(SOURCE_A,bytes((40,80,120)))
        for color,expected in [(0xffffffff,(40,80,120)),(0x80808080,(70,80,90)),
                               (0x80ff4080,(70,60,150)),(0,(100,80,60))]:
            seed()
            check(f'multiply-tint-{color:08x}',bytes(expected),1,1,
                  submit(draw(0x21000100,sw=1,sh=1,overrides={0xA00:0x10002102,
                              0xA02:color,0xA18:0,0xA19:0})),
                  note='RGBA source*color/255, rounded once, then premultiplied SRC_OVER; fixed channel arithmetic.')

        # DST_IN must scale destination, including alpha; color RGB irrelevant.
        for alpha,expected in [(0,(0,0,0,0)),(128,(50,40,30,100)),(255,(100,80,60,200))]:
            seed(pixel=bytes((100,80,60,200)))
            check(f'dst-in-alpha-{alpha}',bytes(expected),1,1,
                  submit(draw(0x01000107,overrides={0xA00:0x10000402,
                              0xA02:alpha<<24,0xA10:0x103})),pixel_bytes=4)

        # COLOR border differs from PAD. Every out-of-bounds BI_LINEAR tap
        # contributes transparent zero, producing 1/4 of the one valid texel.
        write(SOURCE_A,bytes((40,80,120,255)))
        seed()
        check('bilinear-transparent-border',bytes((85,80,75)),1,1,
              submit(draw(0x01020107,sw=1,sh=1,overrides={0xA18:fp(-.5),0xA19:fp(-.5)})))
        expected=seed()
        check('reject-unobserved-color-border',expected,1,1,
              submit(draw(0x01000107,sw=1,sh=1,overrides={0xA18:fp(-2),0xA27:0xff46321e})),
              complete=False,note='All captured a27 values are zero. Nonzero COLOR is outside validated scope and must be rejected explicitly.')

        # A zero-alpha tint and a second draw must preserve prior submissions.
        seed(2,1)
        first=draw(0x01000107,width=2,overrides={0xA00:0x10000102,0xA02:0xff1e140a},clip=(0,0,1,1))
        check('accumulation-first',bytes((10,20,30,100,80,60)),2,1,submit(first))
        second=draw(0x01000107,width=2,overrides={0xA00:0x10000102,0xA02:0xff3c3228},clip=(1,0,1,1))
        check('accumulation-second',bytes((10,20,30,40,50,60)),2,1,submit(second))

        # Unknown image/blend modes must not silently enter the solid branch.
        for mode in [0x10003102,0x10000202]:
            expected=seed()
            check(f'reject-mode-{mode:08x}',expected,1,1,
                  submit(draw(0x01000107,overrides={0xA00:mode})),complete=False)

        if args.synthetic_only:
            manifest['skipped_captured_replays'] = 12
            manifest['synthetic_only'] = True
            if any(not case['pass'] for case in results):
                raise SystemExit(1)
            return 0

        # Actual MULTIPLY + BGR24 BILINEAR requests, with untouched source,
        # target-before, state and path. Safe regular-grid points are far from
        # clipping/rounded path boundaries. This is independent of QEMU's
        # path rasterizer and does not assert its AA sampling pattern.
        captures=[CAPTURE/f'draw-{i:03d}' for i in range(22,31)]+[PREVIEW/'draw-023']
        for directory in captures:
            regs,inputs,source,commands,w,h,stride,bpp,detail=captured(directory)
            expected=bytearray(inputs['target-before.bin'])
            sw,sh=regs[0xA2F]&65535,regs[0xA2F]>>16
            packed=regs[0xA25]&0x20000000!=0
            sbpp=3 if packed else 4
            ss=regs[0xA2B]//4*3 if packed else regs[0xA2B]
            matrix={r:struct.unpack('<f',struct.pack('<I',regs[r]))[0]
                    for r in [0xA18,0xA19,0xA1C,0xA1D,0xA20,0xA21]}
            color=[(regs[0xA02]>>shift)&255 for shift in (0,8,16,24)]
            selected=[];ambiguous=0;max_bound=0
            for y in range(64,208,8):
                for x in range(72,264,8):
                    u=(matrix[0xA18]+matrix[0xA1C]*x+matrix[0xA20]*y)*sw
                    v=(matrix[0xA19]+matrix[0xA1D]*x+matrix[0xA21]*y)*sh
                    ix,iy=math.floor(u),math.floor(v)
                    assert 0<=ix<sw-1 and 0<=iy<sh-1
                    fx,fy=u-ix,v-iy
                    weights=[(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy]
                    taps=[]
                    for dx,dy in [(0,0),(1,0),(0,1),(1,1)]:
                        at=(iy+dy)*ss+(ix+dx)*sbpp
                        taps.append(list(source[at:at+sbpp])+([255] if packed else []))
                    components=[sum(t[c]*weight for t,weight in zip(taps,weights)) for c in range(4)]
                    # Each affine coordinate uses <=6 binary32 operations.
                    # gamma_6*sum(abs(terms))*size bounds coordinate error;
                    # bilinear uint8 channels are <=255-Lipschitz per axis.
                    gamma=6*2**-24/(1-6*2**-24)
                    eu=gamma*(abs(matrix[0xA18])+abs(matrix[0xA1C]*x)+abs(matrix[0xA20]*y))*sw
                    ev=gamma*(abs(matrix[0xA19])+abs(matrix[0xA1D]*x)+abs(matrix[0xA21]*y))*sh
                    bound=255*(eu+ev)+0.001
                    max_bound=max(max_bound,bound)
                    if any(abs(c-(math.floor(c)+.5))<=bound for c in components):
                        ambiguous+=1;continue
                    sample=[round_byte(c) for c in components]
                    if regs[0xA00]==0x10002102:
                        sample=[round_byte(c*t/255) for c,t in zip(sample,color)]
                    at=y*stride+x*bpp
                    expected[at:at+bpp]=over(sample,expected[at:at+bpp])
                    selected.append((x,y))
            detail.update(checked_coordinates=selected,excluded_quantization_ambiguous=ambiguous,
                          max_sample_rounding_error_bound=max_bound,
                          numeric_policy='Check exact bytes only where derived binary32 error cannot cross a rounding threshold; no per-pixel tolerance.')
            name=('captured-preview-bgra-bilinear' if directory.parent==PREVIEW else 'captured-multiply-'+directory.name)
            check(name,bytes(expected),w,h,submit(commands),address=regs[0xA11],pixel_bytes=bpp,
                  stride=stride,selected=selected,details=detail,
                  note='Only listed safe interior pixels are oracle-checked; full actual target is preserved, other expected-image pixels retain target-before.')

        regs,inputs,source,commands,w,h,stride,bpp,detail=captured(PREVIEW/'draw-017')
        expected=bytearray(inputs['target-before.bin'])
        # Captured path is roundrect (radius48) XOR bottom48px rectangle,
        # translated y=-432. The center belongs to both contours and is kept;
        # extreme bottom corners belong only to the rectangle and are erased.
        retained=[(x,y) for y in range(8,40,4) for x in range(64,272,8)]
        erased=[(x,y) for y in range(44,48) for x in list(range(4))+list(range(332,336))]
        changed_before=0
        for x,y in erased:
            at=y*stride+x*bpp
            changed_before+=int(any(expected[at:at+bpp]))
            expected[at:at+bpp]=b'\0'*bpp
        detail.update(retained_coordinates=retained,erased_coordinates=erased,
                      erased_pixels_nonzero_before=changed_before)
        check('captured-preview-dst-in',bytes(expected),w,h,submit(commands),address=regs[0xA11],
              pixel_bytes=bpp,stride=stride,selected=retained+erased,details=detail,
              note='Analytic even-odd contour classification, well away from curves; target-before is untouched replay input.')
        # The actual captured erased corners may already be zero. Keep the
        # exact replay, then separately exercise that same real path against a
        # marked destination. This is a derived GPU unit fixture, not firmware
        # display evidence. It must expose SRC_OVER incorrectly replacing DST_IN.
        derived_before=bytes((100,80,60,200))*(w*h)
        write(regs[0xA11],derived_before)
        derived_expected=bytearray(derived_before)
        for x,y in erased:
            at=y*stride+x*bpp
            derived_expected[at:at+bpp]=b'\0'*bpp
        check('captured-dst-in-path-marked-target',bytes(derived_expected),w,h,submit(commands),
              address=regs[0xA11],pixel_bytes=bpp,stride=stride,selected=retained+erased,
              note='Derived fixture: captured state/path unchanged, target deliberately seeded with BGRA(100,80,60,200). Expected 32 certain corner pixels clear and 208 central pixels persist.')
    finally:
        try:
            proc.terminate();proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill();proc.wait(timeout=10)
        log.close()
        (args.run_dir/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        (args.run_dir/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(f'{sum(r["pass"] for r in results)}/{len(results)} passed')
    return 0 if all(r['pass'] for r in results) else 1


if __name__=='__main__':
    raise SystemExit(main())
