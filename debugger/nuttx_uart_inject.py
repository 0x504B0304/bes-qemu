import gdb, json, os, struct
from pathlib import Path
cfg = json.loads(os.environ['MIBAND_NSH_CONFIG'])
blob = bytes.fromhex(os.environ['MIBAND_NSH_PAYLOAD'])
meta = json.loads(os.environ['MIBAND_NSH_LAYOUT'])
output = Path(os.environ['MIBAND_NSH_OUTPUT'])
safe = int(cfg['trigger'], 0)
f = {k: int(v, 0) for k, v in cfg['functions'].items()}
low, high = (int(v, 0) for v in cfg['heap_range'])
inf = gdb.selected_inferior()
def reg(n): return int(gdb.parse_and_eval('$' + n)) & 0xffffffff
def setreg(n, v): gdb.execute('set $%s=%#x' % (n, v), to_string=True)
def read(a, n): return bytes(inf.read_memory(a, n))
for guard in cfg['guards']:
    expected = bytes.fromhex(guard['expected'])
    if read(int(guard['address'], 0), len(expected)) != expected:
        raise RuntimeError('NSH firmware instruction guard mismatch')
if reg('pc') != safe or reg('xpsr') & 511 or not low + 64 <= reg('sp') < high or reg('sp') % 8:
    raise RuntimeError('not in the qualified NSH boot task')
names = ['r%d' % i for i in range(13)] + ['sp','lr','pc','xpsr','fpscr'] + ['s%d' % i for i in range(32)]
saved = {n: bytes(gdb.parse_and_eval('$' + n).bytes) for n in names}
sp = reg('sp')
scratch = read(sp - 64, 64)
trace = []
in_flight = False
def call(addr, args, call_sp=sp):
    global in_flight
    in_flight = True
    for i, value in enumerate(args): setreg('r%d' % i, value)
    setreg('sp', call_sp); setreg('lr', safe | 1); setreg('pc', addr | 1)
    bp = gdb.Breakpoint('*%#x' % safe, internal=True)
    bp.silent = True
    bp.condition = '$sp == %#x' % call_sp
    try: gdb.execute('continue', to_string=True)
    finally: bp.delete()
    if reg('pc') != safe or reg('sp') != call_sp or reg('xpsr') & 511:
        raise RuntimeError('unexpected stop in NSH native call: %#x' % addr)
    in_flight = False
    result = reg('r0')
    trace.append({'address': hex(addr), 'return': result})
    return result
allocation = None
try:
    allocation = call(f['malloc'], [1024])
    if allocation % 8 or not low <= allocation <= high - 1024:
        raise RuntimeError('invalid NSH guest allocation: %#x' % allocation)
    config, ops, path = allocation + 512, allocation + 640, allocation + 704
    for offset in meta['config_relocations']:
        blob = blob[:offset] + struct.pack('<I', config) + blob[offset+4:]
    inf.write_memory(allocation, blob)
    values = [int(cfg['uart_base'], 0)] + [f[k] | 1 for k in
        ('register','close','open','console','session','exit','sleep')]
    values += [path, ops] + [0] * 5 + [f['pm_stay'] | 1, f['pm_relax'] | 1,
        f['console_exit'] | 1, allocation + meta['exit'] + 1, cfg['console_exit_slot']]
    inf.write_memory(config, struct.pack('<20I', *values))
    # reference also checks readv/writev (words 9/10). Clear the whole reserved
    # table so absent callbacks cannot resolve to uninitialized heap bytes.
    inf.write_memory(ops, struct.pack('<16I', 0, 0,
        allocation + meta['read'] + 1, allocation + meta['write'] + 1, *([0] * 12)))
    inf.write_memory(path, b'/dev/qemu-nsh\0')
    args = bytearray(64)
    struct.pack_into('<I', args, 0, sp - 64 + 8) # empty copied argv
    struct.pack_into('<I', args, 16, cfg['priority'] << 8)
    struct.pack_into('<I', args, 28, cfg['stack_size'])
    inf.write_memory(sp - 64, args)
    pid = call(f['task_create'], [int(cfg['name'], 0),
        allocation + meta['entry'] + 1, 0, sp - 64 + 16], sp - 64)
    if not 0 < pid < 65536:
        raise RuntimeError('NSH task creation failed: %#x' % pid)
    # Registered callbacks and the task retain the allocation until reset.
    result = {'ok': True, 'pid': pid, 'allocation': hex(allocation),
              'config': hex(config), 'trace': trace}
except Exception as error:
    # An unexpected stop keeps its actual registers for diagnosis. Never
    # restore a caller over an in-flight syscall, assert or exception.
    output.with_suffix('.fault.json').write_text(json.dumps({
        'error': str(error), 'in_flight': in_flight,
        'registers': gdb.execute('info registers', to_string=True),
        'allocation': allocation, 'trace': trace}, indent=2))
    raise
finally:
    if not in_flight:
        inf.write_memory(sp - 64, scratch)
        for n, value in saved.items(): setreg(n, int.from_bytes(value, 'little'))
output.write_text(json.dumps(result, indent=2) + '\n')
