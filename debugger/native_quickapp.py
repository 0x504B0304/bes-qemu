import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import time
import gdb

profile = json.loads(getattr(gdb, "miband_context", os.environ)["MIBAND_QUICKAPP_PROFILE"])
operation = getattr(gdb, "miband_context", os.environ)["MIBAND_QUICKAPP_OPERATION"]
output = Path(getattr(gdb, "miband_context", os.environ)["MIBAND_QUICKAPP_OUTPUT"])
functions = {key: int(value, 0) for key, value in profile["functions"].items()}
quickapp = profile["quickapp"]
qfunctions = {key: int(value, 0) for key, value in quickapp["functions"].items()}
cfg = profile["gdb"]
SCRATCH = int(cfg["scratch"], 0)
PATH = int(cfg["path"], 0)
MODE = int(cfg["mode"], 0)
PAYLOAD = int(cfg["payload"], 0)
TRANSFER_STATE = int(cfg["transfer_state"], 0)
CHUNK = int(cfg["chunk"])
SAVED = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc", "xpsr"]
stage = "attach"

def reg(name): return int(gdb.parse_and_eval("$" + name)) & 0xffffffff
def setreg(name, value): gdb.execute(f"set ${name} = {int(value) & 0xffffffff:#x}", to_string=True)
def read(address, count): return bytes(gdb.selected_inferior().read_memory(address, count))
def write(address, data): gdb.selected_inferior().write_memory(address, data)

def continue_until_stop(timeout):
    # Synchronous continue returns only after a breakpoint or signal stops the
    # remote target. continue& can return before GDB has updated thread state,
    # allowing following register/memory reads to race a running guest.
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
    setreg("lr", SCRATCH | 1); setreg("pc", qfunctions.get(name, functions.get(name, 0)) | 1)
    if not reg("pc"): raise RuntimeError("missing quickapp entry: " + name)
    bp = gdb.Breakpoint(f"*{SCRATCH:#x}", internal=True); bp.silent = True
    try:
        if not continue_until_stop(int(cfg.get("call_timeout_sec", 30))):
            raise TimeoutError(f"{name} timed out in the firmware")
    finally:
        try: bp.delete()
        except gdb.error: pass
    if reg("pc") != SCRATCH: raise RuntimeError(f"{name} stopped at {reg('pc'):#x}")
    return reg("r0")

def read_file(path, limit=1024 * 1024):
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
    if not stream: raise RuntimeError("firmware fopen quick-app staging path failed")
    try:
        for offset in range(0, len(payload), CHUNK):
            chunk = payload[offset:offset + CHUNK]; write(PAYLOAD, chunk)
            if call("fwrite", PAYLOAD, 1, len(chunk), stream) != len(chunk):
                raise RuntimeError(f"firmware fwrite short at {offset}")
    finally:
        if call("fclose", stream) != 0: raise RuntimeError("firmware fclose quick-app staging path failed")

def query_files():
    files = {}
    for path in profile["quickapp"]["files"]:
        data = read_file(path)
        if data is not None: files[path] = base64.b64encode(data).decode("ascii")
    return files

def run():
    global stage
    stage = "halt target"
    ensure_stopped()
    stage = "validate function guards"
    for spec in profile["quickapp"].get("operations", {}).values():
        expected = bytes.fromhex(spec["guard"]["expected"])
        if read(int(spec["guard"]["address"], 0), len(expected)) != expected:
            raise RuntimeError("firmware quick-app instruction guard mismatch")
    safe_context = int(cfg["safe_context"], 0)
    stage = "read event-loop context"
    if reg("pc") != safe_context:
        bp = gdb.Breakpoint(f"*{safe_context:#x}", internal=True)
        bp.condition = (f"$r4 == *(unsigned int *){int(cfg['event_loop_guard'], 0):#x} && "
                        f"*(unsigned int *){int(cfg['config_ready_guard'], 0):#x} != 0")
        bp.silent = True
        try:
            stage = "wait for event loop"
            if not continue_until_stop(int(cfg.get("safe_context_timeout_sec", 30))):
                raise TimeoutError("timed out waiting for quick-app event loop")
        finally:
            try: bp.delete()
            except gdb.error: pass
    if reg("pc") != safe_context:
        raise RuntimeError("not at quick-app event loop")
    if reg("xpsr") & 0x1ff:
        raise RuntimeError("refusing native call in exception context")
    if not int.from_bytes(read(int(cfg["config_ready_guard"], 0), 4), "little"):
        raise RuntimeError("quick-app config is not initialized")
    stage = "save guest state"
    saved = {name: reg(name) for name in SAVED}; backup = read(SCRATCH, int(cfg["scratch_size"]))
    try:
        if operation == "query":
            stage = "query firmware files"
            result = {"ok": True, "files": query_files()}
        elif operation == "install":
            stage = "install firmware package"
            package = Path(getattr(gdb, "miband_context", os.environ)["MIBAND_QUICKAPP_PACKAGE"]).read_bytes()
            if not package or len(package) > 64 * 1024 * 1024:
                raise RuntimeError("unsupported quick-app package")
            write_file(profile["quickapp"]["staging"], package)
            # install_from_path is the firmware callback after MASS has
            # reached 100%.  We stage the bytes through the firmware file
            # API, then inject that completed-transfer state so the native
            # scanner accepts the same path without a host-side catalogue.
            write(TRANSFER_STATE, b"\x02")
            # This is the firmware's local-RPK entry point.  It starts the
            # asynchronous file/install state machine, which owns extraction,
            # registration, and rpk_info.json updates.  The older
            # scan_and_install symbol only initializes the scanner task and
            # must not be called for every package.
            write(PATH, quickapp["staging"].encode() + b"\0")
            if call("install_from_path", PATH) != 0:
                raise RuntimeError("firmware quickapp install_from_path rejected the completed transfer")
            # Do not read the index while the firmware transaction is queued;
            # release QEMU and poll the firmware-owned index afterwards.
            result = {"ok": True, "size": len(package),
                      "sha256": hashlib.sha256(package).hexdigest(),
                      "files": {}}
        elif operation == "launch":
            stage = "launch firmware package"
            name = getattr(gdb, "miband_context", os.environ)["MIBAND_QUICKAPP_NAME"].encode() + b"\0"; write(PATH, name)
            if call("launch_app", PATH) != 0: raise RuntimeError("firmware quickapp_launch_app failed")
            result = {"ok": True, "packageName": getattr(gdb, "miband_context", os.environ)["MIBAND_QUICKAPP_NAME"],
                      "files": query_files()}
        elif operation == "delete":
            stage = "delete firmware package"
            name = getattr(gdb, "miband_context", os.environ)["MIBAND_QUICKAPP_NAME"].encode() + b"\0"
            write(PATH, name)
            state = SCRATCH + 0x2000
            write(state, b"\0" * 16)
            # Follow the native uninstall handler's configuration update path
            # without invoking its modal UI callback.
            if call("quickapp_delete_state", 0, state):
                context = call("quickapp_delete_context_base") + 96
                write(state, struct.pack("<I", context))
            write(state + 4, b"\x01")
            if call("quickapp_delete_package", PATH, state) != 0:
                raise RuntimeError("firmware quickapp config removal failed")
            result = {"ok": True, "packageName": getattr(gdb, "miband_context", os.environ)["MIBAND_QUICKAPP_NAME"],
                      "deleted": True, "files": query_files()}
        else: raise RuntimeError("unknown native quick-app operation")
    finally:
        write(SCRATCH, backup)
        for name, value in saved.items(): setreg(name, value)
        if read(SCRATCH, len(backup)) != backup or any(reg(n) != v for n, v in saved.items()):
            raise RuntimeError("native register/scratch restoration failed")
    output.write_text(json.dumps(result, ensure_ascii=False) + "\n", encoding="utf-8")
    print("NATIVE_QUICKAPP_RESULT " + json.dumps(result, ensure_ascii=False), flush=True)

try: run()
except Exception as exc:
    result = {"ok": False, "error": f"{stage}: {type(exc).__name__}: {exc}"}
    output.write_text(json.dumps(result, ensure_ascii=False) + "\n", encoding="utf-8")
    print("NATIVE_QUICKAPP_RESULT " + json.dumps(result, ensure_ascii=False), flush=True)
