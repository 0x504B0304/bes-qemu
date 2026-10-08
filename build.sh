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

if [[ ! -f "$qemu_src/build/build.ninja" ]]; then
    (
        cd -- "$qemu_src"
        CC="$bes_cc" ./configure \
            --target-list=arm-softmmu \
            --disable-docs --disable-tools --disable-user \
            --disable-guest-agent --disable-slirp \
            --disable-sdl --disable-gtk --disable-curses \
            --disable-opengl --disable-virglrenderer --disable-spice \
            --disable-capstone --disable-libnfs --disable-libiscsi \
            --disable-rbd --disable-xen --disable-kvm \
            --disable-tcg-interpreter --disable-werror
    )
fi

# QEMU 8.2's configure writes werror=true for Git checkouts even with
# --disable-werror; Meson reconfigure reads that native-file default again.
if grep -q '^werror = true$' "$qemu_src/build/config-meson.cross"; then
    sed -i 's/^werror = true$/werror = false/' "$qemu_src/build/config-meson.cross"
    meson configure "$qemu_src/build" -Dwerror=false
fi
ninja -C "$qemu_src/build" qemu-system-arm
"$qemu_src/build/qemu-system-arm" -M help | grep bes1503
printf 'Built: %s/build/qemu-system-arm\n' "$qemu_src"
