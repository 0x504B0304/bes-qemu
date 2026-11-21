#!/usr/bin/env bash
# Linux/WSL host; the output is Android/Bionic ARM64, not GNU/Linux ARM64.
set -euo pipefail
overlay=$(cd -- "$(dirname -- "$0")/.." && pwd)
: "${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to a Linux Android NDK r28 or newer}"
: "${BES_ANDROID_WORK:?Set BES_ANDROID_WORK to a dedicated Android build directory}"
: "${MIBAND_QEMU_SRC:?Set MIBAND_QEMU_SRC to a clean QEMU f671ce11 checkout}"
# Android 10 (API 29) is the minimum; newer bionic symbols must not be linked.
api=${ANDROID_API:-29}
export ANDROID_API=$api
toolchain=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64
prefix=$BES_ANDROID_WORK/sysroot/data/data/com.termux/files/usr
mkdir -p "$BES_ANDROID_WORK"
python3 "$overlay/android/dependencies.py" --cache "$BES_ANDROID_WORK/packages/cache" --sysroot "$BES_ANDROID_WORK/sysroot"
python3 "$overlay/tools/integrate_qemu.py" "$MIBAND_QEMU_SRC"
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$BES_ANDROID_WORK/sysroot
export PKG_CONFIG=$(command -v pkg-config)
export PATH=$toolchain/bin:$PATH
export AR=$toolchain/bin/llvm-ar NM=$toolchain/bin/llvm-nm RANLIB=$toolchain/bin/llvm-ranlib STRIP=$toolchain/bin/llvm-strip
python3 "$overlay/android/build_slirp.py" --work "$BES_ANDROID_WORK" --ndk "$ANDROID_NDK_ROOT"
build=$BES_ANDROID_WORK/qemu-build
mkdir -p "$build"
cd "$build"
if [[ ! -f build.ninja ]]; then
 "$MIBAND_QEMU_SRC/configure" --target-list=arm-softmmu --cpu=aarch64 \
  --cross-prefix=aarch64-linux-android- --cc="$toolchain/bin/aarch64-linux-android${api}-clang" \
  --cxx="$toolchain/bin/aarch64-linux-android${api}-clang++" --host-cc=cc \
  --disable-docs --disable-tools --disable-user \
  --disable-guest-agent --enable-slirp --enable-vnc --disable-sdl --disable-gtk \
  --disable-curses --disable-opengl --disable-virglrenderer --disable-spice \
  --disable-gnutls --disable-capstone --disable-libnfs --disable-libiscsi \
  --disable-rbd --disable-xen --disable-kvm --disable-virtfs --disable-linux-aio --disable-linux-io-uring \
  --disable-vhost-net --disable-vhost-user --disable-vhost-vdpa \
  --disable-tcg-interpreter --disable-werror --disable-debug-info \
  --extra-cflags="-I$prefix/include -ffile-prefix-map=$MIBAND_QEMU_SRC=. -ffile-prefix-map=$overlay=bes-qemu" \
  --extra-ldflags="-L$prefix/lib -Wl,-rpath-link,$prefix/lib -Wl,-z,max-page-size=16384 -Wl,-rpath,\$ORIGIN"
fi
ninja -j "${BES_BUILD_JOBS:-8}" qemu-system-arm
python3 - "$overlay" "$build/qemu-system-arm" <<'PY'
import hashlib,json,os,subprocess,sys
from pathlib import Path
root,binary=map(Path,sys.argv[1:])
def git(*args):return subprocess.check_output(['git','-C',str(root),*args],text=True).strip()
record=dict(schema='bes.core-build.v1',commit=git('rev-parse','HEAD'),
 dirty=bool(git('status','--porcelain','--untracked-files=all','--','qemu','tools','android')),
 binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),platform='android-arm64',
 minimum_api=int(os.environ['ANDROID_API']))
(binary.parent/'bes-core-build.json').write_text(json.dumps(record,sort_keys=True)+'\n')
PY
echo "Built Android ARM64: $build/qemu-system-arm"
