import base64, json, os, hashlib
from pathlib import Path
import gdb

profile = json.loads(os.environ['MIBAND_QUERY_PROFILE'])
cfg = profile['native_query']
output = Path(os.environ['MIBAND_QUERY_OUTPUT'])
kind = os.environ['MIBAND_QUERY_KIND']
operation = os.environ.get('MIBAND_QUERY_OPERATION', 'query')
safe = int(cfg['safe_context'], 0)
functions = {k: int(v, 0) for k, v in cfg['functions'].items()}
low, high = (int(v, 0) for v in cfg['heap_range'])
diagnostic = {}
install_events = []
install_bp = None
uninstalled = []

class ResourceRejected(RuntimeError): pass

def reg(n): return int(gdb.parse_and_eval('$' + n)) & 0xffffffff
def setreg(n, v): gdb.execute('set $%s = %#x' % (n, v), to_string=True)
def read(a, n): return bytes(gdb.selected_inferior().read_memory(a, n))
def write(a, b): gdb.selected_inferior().write_memory(a, b)
def u32(a): return int.from_bytes(read(int(a, 0), 4), 'little')
def raw(n): return bytes(gdb.parse_and_eval('$' + n).bytes)

def return_breakpoint(address, sp):
    # Several firmware threads execute the same uv_run code. Only the saved
    # caller stack identifies our injected return, not the PC alone.
    bp = gdb.Breakpoint('*%#x' % address, internal=True)
    bp.silent = True
    bp.condition = '$sp == %#x' % sp
    return bp

def run():
    for guard in cfg['guards']:
        expected = bytes.fromhex(guard['expected'])
        if read(int(guard['address'], 0), len(expected)) != expected:
            raise RuntimeError('native JSON instruction guard mismatch')
    if (reg('pc') != safe or reg('xpsr') & 511 or
            not u32(cfg['event_loop_guard']) or
            reg('r4') != u32(cfg['event_loop_guard']) or
            not u32(cfg['config_ready_guard'])):
        raise RuntimeError('not in the initialized resource event loop')
    names = ['r%d' % i for i in range(13)] + ['sp', 'lr', 'pc', 'xpsr', 'fpscr']
    names += ['s%d' % i for i in range(32)]
    saved = {n: raw(n) for n in names}
    diagnostic['savedRegisters'] = {n: v.hex() for n, v in saved.items()}
    sp = reg('sp')
    allocated = []
    files = {}
    trace = []
    call_in_flight = False
    runtime = None
    watchface_result = {}

    def call(name, *args):
        nonlocal call_in_flight
        call_in_flight = True
        for i, value in enumerate(args): setreg('r%d' % i, value)
        setreg('lr', safe | 1); setreg('pc', functions[name] | 1)
        bp = return_breakpoint(safe, sp)
        try: gdb.execute('continue', to_string=True)
        finally: bp.delete()
        if reg('pc') != safe or reg('sp') != sp or reg('xpsr') & 511:
            diagnostic['registers'] = gdb.execute('info registers', to_string=True)
            diagnostic['allocations'] = list(allocated)
            raise RuntimeError('unexpected native stop in ' + name)
        call_in_flight = False
        value = reg('r0')
        trace.append({'function': name, 'return': value})
        return value

    def query_files():
        global uninstalled
        files = {}
        for path in cfg['files'][kind]:
            encoded = path.encode() + b'\0'
            if len(encoded) > 512: raise RuntimeError('index path too long')
            write(pathbuf, encoded)
            pointer = call('read_text', pathbuf)
            if not pointer and kind == 'quickapp' and cfg.get('quickapp_snapshot'):
                # A fresh device can have no apps.json yet. Ask the native
                # PackageManager to serialize its actual registry, including
                # an empty registry; never manufacture an empty host index.
                engine = u32(cfg['quickapp_snapshot']['engine_global'])
                if not low <= engine <= high - 112:
                    raise RuntimeError('native quickapp registry unavailable')
                call('save_registry', engine + 16)
                pointer = call('read_text', pathbuf)
            if not low <= pointer < high:
                raise RuntimeError('firmware could not read required index: ' + path)
            allocated.append(pointer)
            data = bytearray()
            limit = int(cfg.get('max_index_bytes', 262144))
            while len(data) < limit:
                count = min(64, limit - len(data), high - pointer - len(data))
                if count <= 0: raise RuntimeError('index crossed guest heap range')
                block = read(pointer + len(data), count)
                if b'\0' in block:
                    data.extend(block.split(b'\0', 1)[0]); break
                data.extend(block)
            else: raise RuntimeError('index exceeds limit; refusing partial result')
            json.loads(data)  # Malformed/incomplete indexes must not look empty.
            files[path] = base64.b64encode(data).decode('ascii')
            call('free', pointer); allocated.remove(pointer)
        if kind == 'quickapp' and cfg.get('quickapp_registry'):
            catalog = json.loads(base64.b64decode(files[cfg['files']['quickapp'][0]]))
            uninstalled = []
            for item in catalog['InstalledApps']:
                name = item['package']
                if not isinstance(name, str) or len(name.encode()) > 96:
                    raise RuntimeError('invalid installed package name')
                write(pathbuf, name.encode() + b'\0')
                hidden = call('is_uninstalled', pathbuf)
                if hidden not in (0, 1): raise RuntimeError('invalid uninstall state')
                if hidden: uninstalled.append(name)
        return files

    def selected(files, resource_id, require_current=False):
        catalog = json.loads(base64.b64decode(files[cfg['files']['watchface'][0]]))
        for item in catalog['watchface_list']:
            if (str(item.get('id')) == resource_id and str(item.get('type')) != '2'
                    and str(item.get('is_delete', '0')) != '1'):
                return not require_current or str(item.get('in_use')) == '1'
        return False

    def quickapp_runtime():
        runtime_cfg = cfg.get('quickapp_launch')
        if not runtime_cfg: return None
        context = u32(runtime_cfg['context_global'])
        if not context: return None
        if not low <= context <= high - 64:
            raise RuntimeError('invalid native quickapp context')
        pointer = int.from_bytes(read(context + runtime_cfg['package_offset'], 4), 'little')
        handle = int.from_bytes(read(context + runtime_cfg['vm_offset'], 4), 'little')
        state = read(context + runtime_cfg['state_offset'], 1)[0]
        if not pointer or not handle: return None
        if not low <= pointer <= high - 97 or not low <= handle < high:
            raise RuntimeError('invalid native quickapp VM/package pointer')
        name = read(pointer, 97).split(b'\0', 1)[0].decode('ascii')
        installed = json.loads(base64.b64decode(files[cfg['files']['quickapp'][0]]))['InstalledApps']
        if name not in {a['package'] for a in installed}:
            raise RuntimeError('running package absent from native installed registry')
        return {'package': name, 'state': state, 'vm': handle,
                'foreground': state in runtime_cfg['foreground_states']}

    try:
        pathbuf = call('malloc', 512)
        if not low <= pathbuf <= high - 512:
            raise RuntimeError('guest malloc failed or returned invalid address')
        allocated.append(pathbuf)
        files = query_files()
        if operation == 'set':
            resource_id = os.environ['MIBAND_QUERY_ID']
            if kind != 'watchface' or not selected(files, resource_id):
                raise ResourceRejected('not an installed normal watchface')
            write(pathbuf, resource_id.encode('ascii') + b'\0')
            if call('set_current', pathbuf) != 0:
                raise RuntimeError('firmware set_current failed')
            call('refresh', 1)
            renderer = u32(cfg['watchface_set']['renderer_global'])
            if not low <= renderer < high:
                raise RuntimeError('native renderer unavailable after switch')
            if call('refresh_data', renderer) != 0:
                raise RuntimeError('native watchface data refresh failed')
            files = query_files()
            if not selected(files, resource_id, True):
                raise RuntimeError('native index did not confirm current watchface')
        elif operation == 'launch':
            resource_id = os.environ['MIBAND_QUERY_ID']
            installed = json.loads(base64.b64decode(files[cfg['files']['quickapp'][0]]))['InstalledApps']
            if kind != 'quickapp' or resource_id in uninstalled or resource_id not in {a['package'] for a in installed}:
                raise ResourceRejected('quickapp is not installed')
            write(pathbuf, resource_id.encode('ascii') + b'\0')
            admitted = call('launch', pathbuf, 0)
            # Some navigators return their context, not a status code. The
            # adapter still requires a real VM in stable foreground afterward.
            if cfg['quickapp_launch'].get('abi', 'status_zero') == 'status_zero' and admitted != 0:
                raise ResourceRejected('firmware rejected quickapp launch')
        elif operation == 'install' and kind == 'watchface':
            install = cfg['watchface_install']
            payload = Path(os.environ['MIBAND_QUERY_PACKAGE']).read_bytes()
            if payload[:4] != bytes.fromhex('5aa53412') or not 0 < len(payload) <= install['max_package_bytes']:
                raise RuntimeError('invalid watchface package')
            buffer = call('malloc', len(payload))
            if not low <= buffer <= high - len(payload):
                raise RuntimeError('guest watchface allocation failed')
            allocated.append(buffer)
            write(buffer, payload)
            staged = os.environ['MIBAND_QUERY_STAGE']
            write(pathbuf, staged.encode() + b'\0')
            if call('write_file', pathbuf, 32, buffer, len(payload)) != 0:
                raise RuntimeError('firmware watchface staging failed')
            call('free', buffer); allocated.remove(buffer)
            base = call('base_get', pathbuf)
            if not low <= base <= high - 192:
                call('unlink', pathbuf)
                raise ResourceRejected('firmware rejected watchface resource')
            try:
                resource_id = read(base + 64, 64).split(b'\0', 1)[0].decode('ascii')
                if not resource_id.isdigit() or not 9 <= len(resource_id) <= 24:
                    raise RuntimeError('invalid firmware watchface ID')
                version = int.from_bytes(read(base + 128, 4), 'little')
            finally:
                call('base_release', base)
            catalog = json.loads(base64.b64decode(files[cfg['files']['watchface'][0]]))['watchface_list']
            if any(str(item.get('id')) == resource_id and str(item.get('type')) == '2' for item in catalog):
                call('unlink', pathbuf)
                raise ResourceRejected('AOD watchface is not a normal resource')
            responses = []
            class WatchfaceResponse(gdb.Breakpoint):
                def stop(self):
                    packet = read(reg('r0'), 24)
                    if packet[:3] == bytes((4, 4, 6)):
                        responses.append({'phase': 'prepare', 'status': packet[16]})
                    elif packet[:3] == bytes((4, 5, 6)):
                        responses.append({'phase': 'complete', 'status': packet[20], 'complete': packet[21]})
                    return False
            response_bp = WatchfaceResponse('*' + cfg['functions']['send_message'], internal=True)
            try:
                # The same decoded Prepare payload consumed by the firmware's
                # normal handler. It owns version/count/space checks and UI.
                write(pathbuf + 256, b'\0' * 128)
                write(pathbuf + 256, resource_id.encode() + b'\0')
                write(pathbuf + 320, bytes((4, 4)) + b'\0' * 62)
                write(pathbuf + 336, (pathbuf + 256).to_bytes(4, 'little'))
                write(pathbuf + 344, b'\x01')
                write(pathbuf + 352, version.to_bytes(4, 'little'))
                write(pathbuf + 384, b'\0' * 32)
                write(pathbuf + 408, (pathbuf + 320).to_bytes(4, 'little'))
                call('prepare_watchface', pathbuf + 384)
                if not responses or responses[-1] != {'phase': 'prepare', 'status': 0}:
                    call('unlink', pathbuf)
                    raise ResourceRejected('firmware watchface Prepare rejected: ' + json.dumps(responses))
                # Native transport completion, not a claim of BLE/MASS ACK.
                # The callback owns loader validation, move, index and result.
                digest = hashlib.md5(payload).digest()
                write(pathbuf + 384, b'\x10' + digest + b'\0' * 15)
                call('complete_watchface', pathbuf + 384)
            finally:
                response_bp.delete()
            # A builtin restore can retain the transport file; a market
            # install moves it. In both cases cleanup goes through the guest.
            write(pathbuf, staged.encode() + b'\0')
            if call('access', pathbuf, 0) == 0 and call('unlink', pathbuf) != 0:
                raise RuntimeError('firmware watchface staging cleanup failed')
            files = query_files()
            if (not any(e['phase'] == 'complete' and e['status'] == 3 and e['complete'] == 1 for e in responses)
                    or not selected(files, resource_id, True)):
                raise ResourceRejected('firmware did not confirm watchface installation: ' + json.dumps(responses))
            installed = next(item for item in json.loads(base64.b64decode(files[cfg['files']['watchface'][0]]))['watchface_list']
                             if str(item.get('id')) == resource_id and str(item.get('is_delete', '0')) != '1')
            root = install['builtin_root'] if str(installed['type']) == '0' else install['market_root']
            installed_path = root + resource_id + '/resource.bin'
            write(pathbuf, installed_path.encode() + b'\0')
            pointer = call('read_text', pathbuf)
            if not low <= pointer <= high - len(payload) - 1:
                raise RuntimeError('installed watchface readback unavailable')
            allocated.append(pointer)
            if read(pointer, len(payload) + 1) != payload + b'\0':
                raise RuntimeError('installed watchface readback mismatch')
            watchface_result = {'watchfaceId': resource_id, 'current': True,
                                'size': len(payload), 'sha256': hashlib.sha256(payload).hexdigest(),
                                'installEvents': responses}
        elif operation == 'delete' and kind == 'watchface':
            resource_id = os.environ['MIBAND_QUERY_ID']
            if not selected(files, resource_id):
                raise ResourceRejected('not an installed normal watchface')
            write(pathbuf, resource_id.encode() + b'\0')
            if call('delete_watchface', pathbuf, 1) != 0:
                raise ResourceRejected('firmware watchface delete failed')
            call('refresh', 1)
            files = query_files()
            if selected(files, resource_id):
                raise RuntimeError('firmware index did not confirm watchface deletion')
            watchface_result = {'watchfaceId': resource_id, 'deleted': True}
        elif operation == 'install':
            global install_bp
            install = cfg['quickapp_install']
            engine = u32(install['engine_global'])
            if not low <= engine <= high - 64 or read(engine + install['busy_offset'], 1)[0] == 2:
                raise RuntimeError('native installer unavailable or busy')
            payload = Path(os.environ['MIBAND_QUERY_PACKAGE']).read_bytes()
            if not 0 < len(payload) <= install['max_package_bytes']:
                raise RuntimeError('invalid package size')
            buffer = call('malloc', len(payload))
            if not low <= buffer <= high - len(payload):
                raise RuntimeError('guest package allocation failed')
            allocated.append(buffer)
            write(buffer, payload)
            write(pathbuf, os.environ['MIBAND_QUERY_STAGE'].encode() + b'\0')
            if call('write_file', pathbuf, 32, buffer, len(payload)) != 0:
                raise RuntimeError('firmware package staging failed')
            call('free', buffer); allocated.remove(buffer)
            class Completion(gdb.Breakpoint):
                def stop(self):
                    status = reg('r1')
                    event = {'status': status if status < 0x80000000 else status - 0x100000000}
                    if status == 0:
                        pointer = int.from_bytes(read(reg('r2'), 4), 'little')
                        event['package'] = read(pointer, 97).split(b'\0', 1)[0].decode('ascii')
                    install_events.append(event)
                    return False
            install_bp = Completion('*' + install['callback'], internal=True)
            write(pathbuf + 384, pathbuf.to_bytes(4, 'little'))
            if install.get('abi') == 'loop_path':
                admitted = call('install_local', u32(cfg['event_loop_guard']), pathbuf)
                accepted = 0
            else:
                admitted = call('install_local', pathbuf + 384, 1, 0)
                accepted = 0
            if admitted != accepted:
                raise RuntimeError('native installer rejected admission')
        elif operation == 'install_verify':
            # The worker finished and we reacquired the main event loop with
            # a fresh register snapshot. Cleanup only our temporary package.
            write(pathbuf, os.environ['MIBAND_QUERY_STAGE'].encode() + b'\0')
            cleaned_by_callback = (cfg['quickapp_install'].get('abi') == 'loop_path' and
                                   any(e['status'] == 0 for e in install_events))
            if not cleaned_by_callback and call('unlink', pathbuf) != 0:
                raise RuntimeError('native package cleanup failed')
            resource_id = os.environ['MIBAND_QUERY_ID']
            if (resource_id in uninstalled and
                    any(e.get('package') == resource_id and e['status'] == 0 for e in install_events)):
                write(pathbuf, resource_id.encode() + b'\0')
                write(pathbuf + 384, b'\0' * 64)
                write(pathbuf + 392, pathbuf.to_bytes(4, 'little'))
                call('restore_app', pathbuf + 384)
                # Native install callback skipped launcher registration while
                # the recoverable preset was hidden. Rebuild it from native
                # InstalledAppInfo after the verified restore callback.
                if call('lookup_info', pathbuf, pathbuf + 384) != 0:
                    raise RuntimeError('installed app metadata unavailable')
                appid = call('lookup_appid', pathbuf + 384)
                if call('register_app', appid, pathbuf + 384) != 0:
                    raise RuntimeError('native restored app registration failed')
                files = query_files()
        elif operation == 'delete':
            resource_id = os.environ['MIBAND_QUERY_ID']
            installed = json.loads(base64.b64decode(files[cfg['files']['quickapp'][0]]))['InstalledApps']
            if resource_id not in {a['package'] for a in installed} or resource_id in uninstalled:
                raise ResourceRejected('quickapp is not installed')
            write(pathbuf, resource_id.encode() + b'\0')
            call('delete_app', pathbuf, 0)  # completion is asynchronous, not this return
        elif operation != 'query':
            raise RuntimeError('unsupported resource operation')
        if kind == 'quickapp': runtime = quickapp_runtime()
    finally:
        # An interrupted native call may hold locks. Preserve that frame for
        # diagnosis rather than free memory or blindly resume a saved context.
        if not call_in_flight:
            for pointer in allocated: call('free', pointer)
            for name, value in saved.items():
                gdb.set_convenience_variable('miband_restore',
                    gdb.Value(value, gdb.parse_and_eval('$' + name).type))
                gdb.execute('set $%s = $miband_restore' % name, to_string=True)
            gdb.set_convenience_variable('miband_restore', None)
            if any(raw(n) != v for n, v in saved.items()):
                raise RuntimeError('native register restoration failed')
    return dict({'ok': True, 'files': files, 'nativeTrace': trace, 'quickappRuntime': runtime,
            'quickappUninstalled': uninstalled if kind == 'quickapp' else []}, **watchface_result)

try:
    result = run()
    if operation == 'install' and kind == 'quickapp':
        class CompletedBoundary(gdb.Breakpoint):
            def stop(self):
                terminal = any(e['status'] <= 0 for e in install_events)
                return (terminal and reg('r4') == u32(cfg['event_loop_guard'])
                        and reg('xpsr') & 511 == 0)
        boundary = CompletedBoundary('*%#x' % safe, internal=True)
        try: gdb.execute('continue', to_string=True)
        finally: boundary.delete()
        if reg('pc') != safe or not install_events:
            raise RuntimeError('unexpected stop waiting for native installation')
        install_bp.delete(); install_bp = None
        operation = 'install_verify'
        result = run()
        resource_id = os.environ['MIBAND_QUERY_ID']
        result['installEvents'] = install_events
        installed = json.loads(base64.b64decode(result['files'][cfg['files']['quickapp'][0]]))['InstalledApps']
        if (not any(e.get('package') == resource_id and e['status'] == 0 for e in install_events)
                or resource_id not in {a['package'] for a in installed} or resource_id in uninstalled):
            result['operationError'] = '固件未确认目标快应用安装完成: ' + json.dumps(install_events)
except ResourceRejected as exc: result = {'ok': True, 'operationError': str(exc)}
except Exception as exc: result = {'ok': False, 'error': str(exc), 'diagnostic': diagnostic}
finally:
    if install_bp is not None: install_bp.delete()
output.write_text(json.dumps(result, ensure_ascii=False))
