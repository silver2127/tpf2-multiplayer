#!/usr/bin/env bash
# Build a selected native Big Maps checkout/worktree without modifying it.
set -euo pipefail
source_repo=$(realpath "${1:?Big Maps checkout/worktree required}")
build=$(realpath -m "${2:?output build directory required}")
sdk=${TPF2MP_SDK_ROOT:-$HOME/.cache/tpf2mp/soldier-2.0.20260805.254767/root}
[ -f "$source_repo/linux/CMakeLists.txt" ] || { echo 'Big Maps checkout lacks linux/CMakeLists.txt' >&2; exit 1; }
[ -x "$sdk/usr/bin/cmake" ] || { echo 'soldier SDK missing' >&2; exit 1; }
mkdir -p "$build"
printf '{ global: Tpf2mpPluginInit; local: *; };\n' > "$build/exports.map"
bwrap --die-with-parent --unshare-pid --unshare-net --ro-bind "$sdk" / --proc /proc --dev /dev --tmpfs /tmp \
  --ro-bind "$(dirname "$source_repo")" /work --bind "$build" /build --chdir "/work/$(basename "$source_repo")" \
  --setenv PATH /usr/bin:/bin --unsetenv LD_PRELOAD --unsetenv LD_LIBRARY_PATH \
  /bin/bash -ec 'cmake -S linux -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MODULE_LINKER_FLAGS=-Wl,--version-script=/build/exports.map; cmake --build /build --parallel 4; cd /build; ctest --output-on-failure'
[ -f "$build/tpf2_bigmap.so" ]
exports=$(nm -D --defined-only "$build/tpf2_bigmap.so" | awk '{print $NF}' | sort | paste -sd' ' -)
[ "$exports" = Tpf2mpPluginInit ] || { echo "unexpected Big Maps exports: $exports" >&2; exit 1; }
newest=$(objdump -T "$build/tpf2_bigmap.so" | sed -n 's/.*GLIBC_\([0-9.]*\).*/\1/p' | sort -Vu | tail -n 1)
[ "$(printf '2.31\n%s\n' "$newest" | sort -V | tail -n 1)" = 2.31 ]
