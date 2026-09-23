#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
task_dir=$(mktemp -d /tmp/tpf2-slice-commands.XXXXXX)
trap 'rm -rf -- "$task_dir"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -fPIC -shared \
  "$task_root/tools/linux/test_slice_foreign_unwind.cpp" -o "$task_dir/foreign.so"
"${CXX:-g++}" -std=c++17 -O2 -fno-omit-frame-pointer -Wall -Wextra -Werror \
  -fvisibility=hidden -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL \
  "$task_root/tools/linux/test_slice_commands.cpp" -pthread -ldl -o "$task_dir/test"
"$task_dir/test" "$task_dir/foreign.so" "$task_dir/"
"${LUA:-lua}" "$task_root/tools/linux/test_slice_line_wire.lua" "$task_root"
