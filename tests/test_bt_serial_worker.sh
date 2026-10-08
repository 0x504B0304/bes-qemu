#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$source_dir/.." && pwd)
test_dir=$(mktemp -d /tmp/miband-hci-worker-test.XXXXXX)
clang-23 -DMIBAND_SERIAL_STANDALONE -std=gnu11 -O1 -g \
    -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -pthread -I"$repo_dir/qemu/bridges/bluetooth/h4-serial" "$repo_dir/qemu/bridges/bluetooth/h4-serial/miband_bt_serial.c" \
    "$source_dir/test_bt_serial_worker.c" -lutil -o "$test_dir/test-worker"
ASAN_OPTIONS=detect_leaks=1 "$test_dir/test-worker" "$test_dir"
printf 'PTY test artifacts: %s\n' "$test_dir"
