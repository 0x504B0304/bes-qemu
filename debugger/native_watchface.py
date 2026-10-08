import hashlib
import json
import os
from pathlib import Path
import time
import gdb

profile = json.loads(getattr(gdb, "miband_context", os.environ)["MIBAND_WATCHFACE_PROFILE"])
operation = getattr(gdb, "miband_context", os.environ)["MIBAND_WATCHFACE_OPERATION"]
output = Path(getattr(gdb, "miband_context", os.environ)["MIBAND_WATCHFACE_OUTPUT"])
functions = {key: int(value, 0) for key, value in profile["functions"].items()}
cfg = profile["gdb"]
SCRATCH = int(cfg["scratch"], 0)
PATH = int(cfg["path"], 0)
MODE = int(cfg["mode"], 0)
PAYLOAD = int(cfg["payload"], 0)
CHUNK = int(cfg["chunk"])
SAVED = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc", "xpsr"]

def reg(name): return int(gdb.parse_and_eval("$" + name)) & 0xffffffff
def setreg(name, value): gdb.execute(f"set ${name} = {int(value) & 0xffffffff:#x}", to_string=True)
def read(address, count): return bytes(gdb.selected_inferior().read_memory(address, count))
def write(address, data): gdb.selected_inferior().write_memory(address, data)

def continue_until_stop(timeout):
    # Keep register and memory access serialized with the remote target. GDB's
    # asynchronous continue can return before its thread state is updated.
    gdb.execute("continue", to_string=True)
    return True

def ensure_stopped():
    inferior = gdb.selected_inferior()
    try:
        gdb.execute("interrupt", to_string=True)
    except gdb.error:
        if not any(thread.is_running() for thread in inferior.threads()):
            return
        raise
    deadline = time.monotonic() + 3
    while any(thread.is_running() for thread in inferior.threads()):
        if time.monotonic() >= deadline:
            raise TimeoutError("GDB could not halt the guest")
        time.sleep(0.01)

def call(name, *args):
    for index, value in enumerate(args): setreg("r" + str(index), value)
    setreg("lr", SCRATCH | 1); setreg("pc", functions[name] | 1)
    bp = gdb.Breakpoint(f"*{SCRATCH:#x}", internal=True); bp.silent = True
    try:
        if not continue_until_stop(int(cfg.get("call_timeout_sec", 30))):
            raise TimeoutError(f"{name} timed out in the firmware")
    finally:
        try: bp.delete()
        except gdb.error: pass
    if reg("pc") != SCRATCH: raise RuntimeError(f"{name} stopped at {reg('pc'):#x}")
    return reg("r0")

def read_file(path, limit=256 * 1024):
    write(PATH, path.encode() + b"\0"); write(MODE, b"rb\0")
    stream = call("fopen", PATH, MODE)
    if not stream: return None
    data = bytearray()
    try:
        while len(data) < limit:
            request = min(CHUNK, limit - len(data)); got = call("fread", PAYLOAD, 1, request, stream)
            if got <= 0: break
            data.extend(read(PAYLOAD, got))
            if got < request: break
    finally: call("fclose", stream)
    return bytes(data)

def write_file(path, payload):
    write(PATH, path.encode() + b"\0"); write(MODE, b"wb\0")
    stream = call("fopen", PATH, MODE)
    if not stream: raise RuntimeError("firmware fopen staging path failed")
    try:
        for offset in range(0, len(payload), CHUNK):
            chunk = payload[offset:offset + CHUNK]; write(PAYLOAD, chunk)
            if call("fwrite", PAYLOAD, 1, len(chunk), stream) != len(chunk):
                raise RuntimeError(f"firmware fwrite short at {offset}")
    finally:
        if call("fclose", stream) != 0: raise RuntimeError("firmware fclose staging path failed")

def refresh(force=1):
    # 3.101.043 returns the renderer refresh status (6 for a normal rebuild),
    # rather than a C-style success value.  The following renderer/data calls
    # are the acceptance checks; keep the native return for diagnostics.
    call("refresh_watchface", force)
    renderer = call("get_renderer")
    if not renderer: raise RuntimeError("firmware returned no watchface renderer")
    if call("refresh_data", renderer) != 0: raise RuntimeError("firmware watchface data refresh failed")

def parse_id(base):
    value = read(base + 0x40, 64).split(b"\0", 1)[0]
    if not value or not all(48 <= c <= 57 for c in value): raise RuntimeError(f"invalid watchface id: {value!r}")
    return value.decode("ascii")

def resource_base(watchface_id):
    """Load a watchface base from firmware-owned resource locations.

    Market watchfaces live under /data, while the factory catalog is packed
    into the read-only /resource image (AOD has its own subdirectory).
    Returning both the base pointer and the path keeps selection in the
    firmware's native loader instead of making the host copy resource files.
    """
    templates = profile["files"].get("resource_roots", [
        "/data/app/watchface/market/{id}/resource.bin",
        "/resource/watchface/{id}/resource.bin",
        "/resource/watchface/aod/{id}/resource.bin",
    ])
    for template in templates:
        path = template.format(id=watchface_id)
        write(PATH, path.encode() + b"\0")
        base = call("base_get", PATH)
        if base:
            return base, path
    return 0, None

def query_files():
    import base64
    files = {}
    for path in profile["files"]["list"] + [profile["files"]["database"]]:
        data = read_file(path)
        if data is not None: files[path] = base64.b64encode(data).decode("ascii")
    return files

def run():
    ensure_stopped()
    for spec in profile['operations'].values():
        guard = spec['guard']
        expected = bytes.fromhex(guard['expected'])
        if read(int(guard['address'], 0), len(expected)) != expected:
            raise RuntimeError('firmware native function instruction guard mismatch')
    safe_context = int(cfg["safe_context"], 0)
    if reg("pc") != safe_context:
        bp = gdb.Breakpoint(f"*{safe_context:#x}", internal=True)
        bp.condition = (f"$r4 == *(unsigned int *){int(cfg['event_loop_guard'], 0):#x} && "
                        f"*(unsigned int *){int(cfg['config_ready_guard'], 0):#x} != 0")
        bp.silent = True
        try:
            if not continue_until_stop(int(cfg.get("safe_context_timeout_sec", 30))):
                raise TimeoutError("timed out waiting for watchface event loop")
        finally:
            try: bp.delete()
            except gdb.error: pass
    if reg("pc") != safe_context: raise RuntimeError("not at watchface event loop")
    if reg("xpsr") & 0x1ff: raise RuntimeError("refusing native call in exception context")
    if not int.from_bytes(read(int(cfg["config_ready_guard"], 0), 4), "little"): raise RuntimeError("watchface config is not initialized")
    saved = {name: reg(name) for name in SAVED}; backup = read(SCRATCH, int(cfg["scratch_size"]))
    try:
        if operation == "query":
            result = {"ok": True, "files": query_files()}
        elif operation == "install":
            package = Path(getattr(gdb, "miband_context", os.environ)["MIBAND_WATCHFACE_PACKAGE"]).read_bytes()
            if not package or len(package) > 4 * 1024 * 1024 or package[:4] != bytes.fromhex("5aa53412"): raise RuntimeError("unsupported watchface package")
            staging = profile["files"]["staging"]; write_file(staging, package)
            write(PATH, staging.encode() + b"\0"); base = call("base_get", PATH)
            if not 0x3c000000 <= base < 0x3e000000: raise RuntimeError(f"base_get returned {base:#x}")
            watchface_id = parse_id(base)
            if call("add_new_watchface", base + 0x40, PATH) != 0: raise RuntimeError("firmware add_new_watchface failed")
            if call("set_current", base + 0x40) != 0: raise RuntimeError("firmware set_current failed")
            refresh(1); installed_path = f"/data/app/watchface/market/{watchface_id}/resource.bin"
            installed = read_file(installed_path, len(package) + 1)
            if installed != package: raise RuntimeError("firmware installed resource readback mismatch")
            call("base_release", base)
            result = {"ok": True, "watchfaceId": watchface_id, "size": len(package), "sha256": hashlib.sha256(package).hexdigest(), "current": True, "files": query_files()}
        elif operation == "set":
            watchface_id = getattr(gdb, "miband_context", os.environ)["MIBAND_WATCHFACE_ID"]
            base, path = resource_base(watchface_id)
            if not base: raise RuntimeError("base_get failed for watchface resource")
            try:
                if call("set_current", base + 0x40) != 0: raise RuntimeError("firmware set_current failed")
                refresh(1)
            finally: call("base_release", base)
            result = {"ok": True, "watchfaceId": watchface_id, "current": True, "files": query_files()}
        elif operation == "delete":
            watchface_id = getattr(gdb, "miband_context", os.environ)["MIBAND_WATCHFACE_ID"]; write(PATH, watchface_id.encode() + b"\0")
            if call("delete_watchface", PATH) != 0: raise RuntimeError("firmware delete_watchface failed")
            refresh(1); result = {"ok": True, "watchfaceId": watchface_id, "deleted": True, "files": query_files()}
        else: raise RuntimeError("unknown native operation")
    finally:
        write(SCRATCH, backup)
        for name, value in saved.items(): setreg(name, value)
        if read(SCRATCH, len(backup)) != backup or any(reg(n) != v for n, v in saved.items()):
            raise RuntimeError("native register/scratch restoration failed")
    output.write_text(json.dumps(result, ensure_ascii=False) + "\n", encoding="utf-8")
    print("NATIVE_WATCHFACE_RESULT " + json.dumps(result, ensure_ascii=False), flush=True)

try: run()
except Exception as exc:
    result = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
    output.write_text(json.dumps(result, ensure_ascii=False) + "\n", encoding="utf-8")
    print("NATIVE_WATCHFACE_RESULT " + json.dumps(result, ensure_ascii=False), flush=True)
