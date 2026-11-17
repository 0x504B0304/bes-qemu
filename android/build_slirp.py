#!/usr/bin/env python3
"""Pinned slirp build with an app-private resolv.conf, never a Termux path."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

SOURCE = {
    'version': '4.9.7',
    'url': 'https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.7/libslirp-v4.9.7.tar.gz',
    'sha256': 'd2567fa06a6357c7755d61c67293b2c0bf069b3f2173bde6b76cf768529a1938',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--ndk', type=Path, required=True)
    args = parser.parse_args()
    cache = args.work/'packages/cache'; cache.mkdir(parents=True, exist_ok=True)
    archive = cache/'libslirp-v4.9.7.tar.gz'
    if not archive.is_file(): urllib.request.urlretrieve(SOURCE['url'], archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SOURCE['sha256']:
        raise ValueError('libslirp source digest mismatch')
    directory = args.work/'slirp-source'; directory.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as stream: stream.extractall(directory, filter='data')
    source = directory/'libslirp-v4.9.7'
    path = source/'src/slirp.c'
    text = path.read_text()
    before = '#define RESOLV_CONF_PATH "/etc/resolv.conf"'
    after = '''/* Android has no /etc/resolv.conf. The application owns this file.
 * Read it on every cache validation so a network change can update DNS. */
static const char *mbs_resolv_conf_path(void)
{
    const char *path = getenv("MIBAND_SLIRP_RESOLV_CONF");
    return path && path[0] == '/' ? path : "/etc/resolv.conf";
}
#define RESOLV_CONF_PATH mbs_resolv_conf_path()'''
    if text.count(before) != 1: raise ValueError('libslirp resolver patch no longer applies')
    path.write_text(text.replace(before, after))
    prefix = args.work/'sysroot/data/data/com.termux/files/usr'
    toolchain = args.ndk/'toolchains/llvm/prebuilt/linux-x86_64'
    cross = args.work/'slirp-cross.ini'
    cross.write_text(f'''[binaries]
c = '{toolchain}/bin/aarch64-linux-android31-clang'
ar = '{toolchain}/bin/llvm-ar'
strip = '{toolchain}/bin/llvm-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[built-in options]
c_args = ['-O2', '-ffile-prefix-map={source}=libslirp']
c_link_args = ['-L{prefix}/lib', '-Wl,-rpath-link,{prefix}/lib', '-Wl,-z,max-page-size=16384']
''')
    env = dict(os.environ, PKG_CONFIG_LIBDIR=str(prefix/'lib/pkgconfig'),
               PKG_CONFIG_SYSROOT_DIR=str(args.work/'sysroot'))
    build = args.work/'slirp-build'
    command = ['meson', 'setup', str(build), str(source), '--cross-file', str(cross),
               '--buildtype=release', '--default-library=shared', '--prefix=/data/data/com.termux/files/usr']
    if (build/'build.ninja').is_file(): command.append('--reconfigure')
    subprocess.run(command, env=env, check=True)
    subprocess.run(['ninja', '-C', str(build), 'libslirp.so'], env=env, check=True)
    binary = build/'libslirp.so'
    target = (prefix/'lib/libslirp.so').resolve(strict=True)
    shutil.copy2(binary, target)
    notices = prefix/'share/doc/libslirp'; notices.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source/'COPYRIGHT', notices/'COPYRIGHT.android-source')
    record = dict(source=SOURCE, patch='app-private-resolv-conf-v1',
                  patched_source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    (args.work/'sysroot/android-native-overrides.json').write_text(json.dumps({'libslirp': record}, indent=2)+'\n')
    print('Android slirp resolver built')


if __name__ == '__main__': main()
