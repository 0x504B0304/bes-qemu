import json, os, gdb
from pathlib import Path
cfg = json.loads(os.environ['MIBAND_INPUT_CONFIG'])
value = json.loads(os.environ['MIBAND_INPUT_VALUE'])
safe = int(cfg['safe_context'], 0)
functions = {k: int(v, 0) for k, v in cfg['functions'].items()}
diagnostic = {}
def reg(n): return int(gdb.parse_and_eval('$' + n)) & 0xffffffff
def raw(n): return bytes(gdb.parse_and_eval('$' + n).bytes)
def read(a, n): return bytes(gdb.selected_inferior().read_memory(a, n))
def write(a, b): gdb.selected_inferior().write_memory(a, b)
def u32(a): return int.from_bytes(read(int(a, 0), 4), 'little')
def setreg(n, v): gdb.execute('set $%s = %#x' % (n, v), to_string=True)
def run():
    for guard in cfg['guards']:
        b = bytes.fromhex(guard['expected'])
        if read(int(guard['address'], 0), len(b)) != b: raise RuntimeError('input instruction guard mismatch')
    if (reg('pc') != safe or reg('xpsr') & 511 or not u32(cfg['config_ready_guard']) or
            reg('r4') != u32(cfg['event_loop_guard'])):
        raise RuntimeError('input requires initialized main event loop')
    names = ['r%d' % i for i in range(13)] + ['sp', 'lr', 'pc', 'xpsr', 'fpscr'] + ['s%d' % i for i in range(32)]
    saved = {n: raw(n) for n in names}
    diagnostic['savedRegisters'] = {n: v.hex() for n, v in saved.items()}
    sp = reg('sp'); pointer = None; fd = None; active = False; trace = []; tick = None
    def call(name, *args):
        nonlocal active
        active = True
        for i, v in enumerate(args): setreg('r%d' % i, v)
        setreg('lr', safe | 1); setreg('pc', functions[name] | 1)
        bp = gdb.Breakpoint('*%#x' % safe, internal=True)
        bp.silent = True; bp.condition = '$sp == %#x' % sp
        try: gdb.execute('continue', to_string=True)
        finally: bp.delete()
        if reg('pc') != safe or reg('sp') != sp or reg('xpsr') & 511:
            diagnostic['registers'] = gdb.execute('info registers', to_string=True)
            diagnostic['allocation'] = pointer; diagnostic['fd'] = fd
            raise RuntimeError('unexpected input stop in ' + name)
        active = False
        result = reg('r0'); trace.append({'function': name, 'return': result})
        return result
    try:
        if 'clock' in value:
            tick = call('clock')
            return {'ok': True, 'tick': tick, 'trace': trace}
        pointer = call('malloc', 128)
        low, high = [int(v, 0) for v in cfg['heap_range']]
        if not low <= pointer <= high - 128:
            pointer = None; raise RuntimeError('input allocation failed')
        if 'button' in value:
            write(pointer, cfg['button_device'].encode() + b'\0')
            fd = call('open', pointer, cfg['write_flags'])
            if fd & 0x80000000: fd = None; raise RuntimeError('native button open failed')
            write(pointer + 64, value['button'].to_bytes(4, 'little'))
            if call('write', fd, pointer + 64, 4) != 4: raise RuntimeError('native button write failed')
        else:
            if value['event'] == cfg['rotation_start']:
                call('display_activity', 2, 0)
                call('crown_activity', 0)
            write(pointer, value['event'].to_bytes(4, 'little') + value['delta'].to_bytes(4, 'little', signed=True))
            # Synchronous dispatch: the event bus borrows, rather than copies,
            # the payload. It must finish all callbacks before we free it.
            if call('event', cfg['event_topic'], pointer, 1) != 0: raise RuntimeError('native input dispatch failed')
    finally:
        if not active:
            if fd is not None: call('close', fd)
            if pointer is not None: call('free', pointer)
            for n, v in saved.items():
                gdb.set_convenience_variable('miband_restore', gdb.Value(v, gdb.parse_and_eval('$' + n).type))
                gdb.execute('set $%s = $miband_restore' % n, to_string=True)
            gdb.set_convenience_variable('miband_restore', None)
            if any(raw(n) != v for n, v in saved.items()): raise RuntimeError('input register restoration failed')
    return {'ok': True, 'trace': trace}
try: result = run()
except Exception as exc: result = {'ok': False, 'error': str(exc), 'diagnostic': diagnostic}
Path(os.environ['MIBAND_INPUT_OUTPUT']).write_text(json.dumps(result))
