#!/usr/bin/env bash
set -euo pipefail

qemu_commit=f671ce11d2a15a58ab752d7cf0b2890c9022c7ba
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
qemu_src=${MIBAND_QEMU_SRC:-/tmp/miband-qemu-8.2}

bes_cc=${BES_CC:-clang-23}
command -v "$bes_cc" >/dev/null
command -v meson >/dev/null
command -v ninja >/dev/null
pkg-config --exists glib-2.0 pixman-1

if [[ ! -d "$qemu_src/.git" ]]; then
    mkdir -p -- "$qemu_src"
    git -C "$qemu_src" init
    git -C "$qemu_src" remote add origin https://github.com/qemu/qemu.git
    git -C "$qemu_src" fetch --depth 1 origin "$qemu_commit"
    git -C "$qemu_src" checkout --detach FETCH_HEAD
fi

actual_commit=$(git -C "$qemu_src" rev-parse HEAD)
if [[ "$actual_commit" != "$qemu_commit" ]]; then
    printf 'QEMU checkout mismatch: %s (expected %s)\n' "$actual_commit" "$qemu_commit" >&2
    exit 1
fi

python3 "$script_dir/tools/integrate_qemu.py" "$qemu_src"
case "$(uname -s)" in
    MINGW*|MSYS*) python3 "$script_dir/tools/prepare_windows_build.py" "$qemu_src" ;;
esac

# Studio's network bridge needs QEMU user-mode networking (slirp).  Keep an
# explicit opt-out for environments that only build the old offline core.
slirp_arg=--enable-slirp
slirp_option=enabled
if [[ "${MIBAND_SLIRP:-1}" == 0 ]]; then
    slirp_arg=--disable-slirp
    slirp_option=disabled
fi
if [[ "$slirp_arg" == --enable-slirp ]] && ! pkg-config --exists slirp; then
    printf 'slirp development package is required (libslirp-dev / libslirp); '
    printf 'set MIBAND_SLIRP=0 only for an offline core build\n' >&2
    exit 1
fi

if [[ ! -f "$qemu_src/build/build.ninja" || ! -f "$qemu_src/build/meson-private/build.dat" ]]; then
    (
        cd -- "$qemu_src"
        CC="$bes_cc" ./configure \
            --target-list=arm-softmmu \
            --disable-docs --disable-tools --disable-user \
            --disable-guest-agent "$slirp_arg" \
            --disable-sdl --disable-gtk --disable-curses \
            --disable-opengl --disable-virglrenderer --disable-spice \
            --disable-gnutls \
            --disable-capstone --disable-libnfs --disable-libiscsi \
            --disable-rbd --disable-xen --disable-kvm \
            --disable-tcg-interpreter --disable-werror
    )
else
    # Existing build trees retain their Meson options. Keep the requested
    # network backend in sync even when reusing a cached build directory.
    bes_meson=$(sed -n 's/^MESON=//p' "$qemu_src/build/config-host.mak")
    "$bes_meson" configure "$qemu_src/build" "-Dslirp=$slirp_option"
fi

# QEMU 8.2's configure writes werror=true for Git checkouts even with
# --disable-werror; Meson reconfigure reads that native-file default again.
if grep -q '^werror = true$' "$qemu_src/build/config-meson.cross"; then
    python3 - "$qemu_src/build/config-meson.cross" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding='utf-8')
updated = text.replace('werror = true', 'werror = false')
if updated == text:
    raise SystemExit('expected werror setting was not found')
path.write_text(updated, encoding='utf-8')
PY
    bes_meson=$(sed -n 's/^MESON=//p' "$qemu_src/build/config-host.mak")
    "$bes_meson" configure "$qemu_src/build" -Dwerror=false
fi
bes_binary=qemu-system-arm
if [[ "${BES_RELEASE_BUILD:-0}" == 1 ]]; then
    # Preserve relative source locations for diagnostics, not host checkout paths.
    bes_source_map=$qemu_src
    bes_overlay_map=$script_dir
    bes_release_prefix=
    case "$(uname -s)" in
        MINGW*|MSYS*)
            bes_source_map=$(cygpath -m "$qemu_src")
            bes_overlay_map=$(cygpath -m "$script_dir")
            # Meson's Windows default is the build Python's venv prefix.
            # Use a neutral relocatable installation prefix instead.
            bes_release_prefix=C:/MiBandStudio
            ;;
    esac
    bes_meson=$(sed -n 's/^MESON=//p' "$qemu_src/build/config-host.mak")
    if [[ -n "$bes_release_prefix" ]]; then
        "$bes_meson" configure "$qemu_src/build" "-Dprefix=$bes_release_prefix"
    fi
    "$bes_meson" configure "$qemu_src/build" -Ddebug=false -Dstrip=true \
        "-Dc_args=['-ffile-prefix-map=$bes_source_map=.','-ffile-prefix-map=$bes_overlay_map=bes-qemu']"
fi
case "$(uname -s)" in
    MINGW*|MSYS*)
        bes_binary=qemu-system-arm.exe
        python3 "$script_dir/tools/check_native_objects.py" "$qemu_src/build"
        ;;
esac
ninja -C "$qemu_src/build" "$bes_binary"
"$qemu_src/build/$bes_binary" -M help | grep bes1503
# Record the overlay actually used by this binary. Studio's release lock may
# describe a different checkout, especially for local development builds.
python3 - "$script_dir" "$qemu_src/build/$bes_binary" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

overlay, binary = map(Path, sys.argv[1:])
def git(*args):
    return subprocess.run(['git', '-C', str(overlay), *args], text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
root = git('rev-parse', '--show-toplevel')
root_path = root.stdout.strip()
if os.name == 'nt' and root_path.startswith('/'):
    root_path = subprocess.check_output(['cygpath', '-m', root_path], text=True).strip()
if root.returncode == 0 and Path(root_path).resolve() == overlay.resolve():
    commit = git('rev-parse', 'HEAD')
    status = git('status', '--porcelain', '--untracked-files=all', '--', 'build.sh', 'qemu', 'tools')
    revision = commit.stdout.strip() if commit.returncode == 0 else None
    dirty = bool(status.stdout) if status.returncode == 0 else None
else:
    revision = None
    dirty = None
with binary.open('rb') as stream:
    digest = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        digest.update(block)
record = dict(schema='bes.core-build.v1', commit=revision, dirty=dirty,
              binary_sha256=digest.hexdigest())
marker = binary.parent / 'bes-core-build.json'
temporary = marker.with_suffix('.tmp')
temporary.write_text(json.dumps(record, sort_keys=True) + '\n', encoding='utf-8')
temporary.replace(marker)
PY
printf 'Built: %s/build/%s\n' "$qemu_src" "$bes_binary"
