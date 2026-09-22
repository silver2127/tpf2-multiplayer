#!/usr/bin/env bash
# Build and test the native libraries inside Valve's pinned soldier SDK.
# Needs bubblewrap, curl, tar and sha256sum. Downloads the SDK once (~947 MiB).
# Usage: build_native.sh [--build-dir DIR] [--sdk-root DIR]
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=$repo/native/linux/out-soldier
sdk_version=2.0.20260805.254767
sdk_sha256=ffc654ace090b74445d19f5c43b740ffeebb7280aa758a451c21e71821cfd791
cache=${XDG_CACHE_HOME:-$HOME/.cache}/tpf2mp/soldier-$sdk_version
sdk=$cache/root
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) build=${2:?}; shift 2 ;;
    --sdk-root) sdk=${2:?}; shift 2 ;;
    -h|--help) sed -n '2,5p' "$0" | sed 's/^# //'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done
for tool in bwrap curl tar sha256sum; do command -v "$tool" >/dev/null; done
if [ ! -f "$sdk/.tpf2mp-sdk-ready" ] && [ "$sdk" = "$cache/root" ]; then
  mkdir -p "$cache"
  archive=$cache/sdk.tar.gz
  if [ ! -f "$archive" ]; then
    curl -fL --retry 3 --connect-timeout 20 -o "$archive.part" \
      "https://repo.steampowered.com/steamrt-images-soldier/snapshots/$sdk_version/com.valvesoftware.SteamRuntime.Sdk-amd64,i386-soldier-sysroot.tar.gz"
    printf '%s  %s\n' "$sdk_sha256" "$archive.part" | sha256sum -c
    mv "$archive.part" "$archive"
  fi
  printf '%s  %s\n' "$sdk_sha256" "$archive" | sha256sum -c
  mkdir -p "$sdk"
  tar --no-same-owner -xzf "$archive" -C "$sdk"
  touch "$sdk/.tpf2mp-sdk-ready"
fi
[ -x "$sdk/usr/bin/cmake" ] || { echo "SDK lacks usr/bin/cmake: $sdk" >&2; exit 1; }
mkdir -p "$sdk/dev" "$sdk/work" "$sdk/build"
mkdir -p "$build"
build=$(realpath "$build"); sdk=$(realpath "$sdk")
if [ -f "$build/CMakeCache.txt" ] && ! grep -q '^CMAKE_HOME_DIRECTORY:INTERNAL=/work/native/linux$' "$build/CMakeCache.txt"; then
  echo "Use a separate build directory for soldier: $build contains another build." >&2
  exit 1
fi
# Source is read-only. The only persistent writable mount is the build folder.
bwrap --die-with-parent --unshare-pid --unshare-net \
  --ro-bind "$sdk" / --proc /proc --dev /dev --tmpfs /tmp \
  --ro-bind "$repo" /work --bind "$build" /build --chdir /work \
  --setenv TPF2MP_BUILD_JOBS "${TPF2MP_BUILD_JOBS:-4}" \
  --setenv PATH /usr/bin:/bin --unsetenv LD_PRELOAD --unsetenv LD_LIBRARY_PATH \
  /bin/bash -ec '
    cmake -S native/linux -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build /build --parallel "${TPF2MP_BUILD_JOBS:-4}"
    cd /build
    ctest --output-on-failure
    { cat /etc/os-release; c++ --version; } > NATIVE_BUILDINFO
  '
for lib in "$build"/*.so; do
  newest=$(objdump -T "$lib" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -Vu | tail -n 1 || true)
  [ -z "$newest" ] || [ "$(printf '2.31\n%s\n' "$newest" | sort -V | tail -n 1)" = 2.31 ] || {
    echo "$lib requires glibc $newest, newer than the 2.31 baseline" >&2; exit 1;
  }
done
printf 'Native libraries built and tested in soldier: %s\n' "$build"
