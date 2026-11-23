#!/usr/bin/env bash
# Linux/WSL host; the output is Android/Bionic, not GNU/Linux.
# ANDROID_ABI selects arm64-v8a (default), armeabi-v7a, x86_64 or x86;
# BES_ANDROID_WORK must be a directory dedicated to that one ABI.
set -euo pipefail
overlay=$(cd -- "$(dirname -- "$0")/.." && pwd)
: "${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to a Linux Android NDK r28 or newer}"
: "${BES_ANDROID_WORK:?Set BES_ANDROID_WORK to a dedicated Android build directory}"
: "${MIBAND_QEMU_SRC:?Set MIBAND_QEMU_SRC to a clean QEMU f671ce11 checkout}"
# Android 8.0 (API 26) is the minimum; newer bionic symbols must not be linked.
api=${ANDROID_API:-26}
export ANDROID_API=$api
abi=${ANDROID_ABI:-arm64-v8a}
export ANDROID_ABI=$abi
# ABI -> clang target, binutils-style prefix, QEMU host CPU, Termux architecture.
case $abi in
 arm64-v8a)   target=aarch64-linux-android;     cross=aarch64-linux-android-;     cpu=aarch64; termux=aarch64;;
 armeabi-v7a) target=armv7a-linux-androideabi;  cross=arm-linux-androideabi-;     cpu=arm;     termux=arm;;
 x86_64)      target=x86_64-linux-android;      cross=x86_64-linux-android-;      cpu=x86_64;  termux=x86_64;;
 x86)         target=i686-linux-android;        cross=i686-linux-android-;        cpu=i386;    termux=i686;;
 *) echo "Unsupported ANDROID_ABI: $abi" >&2; exit 2;;
esac
toolchain=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64
prefix=$BES_ANDROID_WORK/sysroot/data/data/com.termux/files/usr
mkdir -p "$BES_ANDROID_WORK"
if [[ -f $BES_ANDROID_WORK/sysroot/dependency-lock.json ]] &&
   ! grep -q "\"architecture\": \"$termux\"" "$BES_ANDROID_WORK/sysroot/dependency-lock.json"; then
 echo "$BES_ANDROID_WORK holds another ABI's sysroot; use one work directory per ABI" >&2; exit 2
fi
python3 "$overlay/android/dependencies.py" --arch "$termux" \
 --cache "${BES_ANDROID_CACHE:-$BES_ANDROID_WORK/packages/cache}" --sysroot "$BES_ANDROID_WORK/sysroot"
python3 "$overlay/tools/integrate_qemu.py" "$MIBAND_QEMU_SRC"
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$BES_ANDROID_WORK/sysroot
export PKG_CONFIG=$(command -v pkg-config)
export PATH=$toolchain/bin:$PATH
export AR=$toolchain/bin/llvm-ar NM=$toolchain/bin/llvm-nm RANLIB=$toolchain/bin/llvm-ranlib STRIP=$toolchain/bin/llvm-strip
python3 "$overlay/android/build_slirp.py" --work "$BES_ANDROID_WORK" --ndk "$ANDROID_NDK_ROOT" --abi "$abi"
build=$BES_ANDROID_WORK/qemu-build
mkdir -p "$build"
cd "$build"
if [[ ! -f build.ninja ]]; then
 "$MIBAND_QEMU_SRC/configure" --target-list=arm-softmmu --cpu=$cpu \
  --cross-prefix=$cross --cc="$toolchain/bin/${target}${api}-clang" \
  --cxx="$toolchain/bin/${target}${api}-clang++" --host-cc=cc \
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
abi=os.environ['ANDROID_ABI']
platform={'arm64-v8a':'android-arm64','armeabi-v7a':'android-arm','x86_64':'android-x86_64','x86':'android-x86'}[abi]
record=dict(schema='bes.core-build.v1',commit=git('rev-parse','HEAD'),
 dirty=bool(git('status','--porcelain','--untracked-files=all','--','qemu','tools','android')),
 binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),platform=platform,abi=abi,
 minimum_api=int(os.environ['ANDROID_API']))
(binary.parent/'bes-core-build.json').write_text(json.dumps(record,sort_keys=True)+'\n')
PY
echo "Built Android $abi: $build/qemu-system-arm"
