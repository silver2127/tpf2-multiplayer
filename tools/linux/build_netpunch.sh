#!/usr/bin/env bash
# build_netpunch.sh -- freeze the lobby (netpunch/lobby.py) into one folder,
# netpunch/dist-linux/netpunch/: the executable netpunch plus _internal/. It is
# the program the game's Multiplayer panel starts on Linux, as netpunch.exe is
# on Windows. Deploy the folder's contents together: the executable does not
# start without _internal/ beside it.
#
#   tools/linux/build_netpunch.sh [--build-dir DIR] [--dist-dir DIR] [--python PYTHON] [--test]
#
#   --build-dir DIR   the downloaded Python, the venv and PyInstaller's work
#                     files (default netpunch/build-linux; BUILD_DIR=DIR too)
#   --dist-dir DIR    where the netpunch/ folder is written (default
#                     netpunch/dist-linux; DIST_DIR=DIR too)
#   --python PYTHON   build with this interpreter instead of the pinned
#                     python-build-standalone CPython (see THE GLIBC FLOOR)
#   --test            run the lobby self-tests with the built executable
#
# THE GLIBC FLOOR. Steam runs the game in its soldier container with whichever
# glibc is newer, the host's or soldier's 2.31, so the lobby has to start on
# 2.31. A distribution's Python brings libpython, OpenSSL and extension modules
# linked against that distribution's glibc (2.38 and newer from Ubuntu 24.04 on),
# and they do not load on Ubuntu 22.04 or Debian 12. So by default the script
# downloads a pinned python-build-standalone CPython (glibc 2.17 floor, OpenSSL
# built in, checked against its sha256; PBS_RELEASE, PBS_VERSION and PBS_SHA256
# change the pin) and installs miniupnpc and zstandard as manylinux wheels:
# nothing is compiled. After freezing, every ELF file in the output is checked,
# and the build fails when one needs a glibc newer than GLIBC_MAX (default 2.31).
#
# Needs curl or wget, sha256sum, tar and objdump (binutils). With --python and
# no miniupnpc wheel for that Python, miniupnpc is built from source, which needs
# a C compiler and that Python's headers; without python3-dev the script fetches
# libpython3.X-dev with apt-get download (no root) and unpacks it into the build
# dir. ALLOW_NO_MINIUPNPC=1 builds without UPnP instead of failing.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
np="$repo/netpunch"
build_dir="${BUILD_DIR:-$np/build-linux}"
dist_dir="${DIST_DIR:-$np/dist-linux}"
python=""                       # empty: the pinned python-build-standalone CPython
pyinstaller_version="${PYINSTALLER_VERSION:-6.22.3}"
glibc_max="${GLIBC_MAX:-2.31}"
# https://github.com/astral-sh/python-build-standalone -- 3.12 is the series
# netpunch/README.md builds netpunch.exe with, and miniupnpc has wheels for it
pbs_release="${PBS_RELEASE:-20260901}"
pbs_version="${PBS_VERSION:-3.12.14}"
pbs_sha256="${PBS_SHA256:-72748da13197c1fb161e3afeef20a6a385ff24f2165e6e2758e47008e7faba4c}"
run_tests=0

say() { printf '[build_netpunch] %s\n' "$*"; }
die() { printf '[build_netpunch] ERROR: %s\n' "$*" >&2; exit 1; }
usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }
newer() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]; }   # version $1 > $2
fetch() {   # URL FILE
    if command -v curl >/dev/null; then curl -fsSL --retry 3 --connect-timeout 20 -o "$2" "$1"
    elif command -v wget >/dev/null; then wget -q -O "$2" "$1"
    else die "curl or wget is needed to download Python"; fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) [ $# -ge 2 ] || die "--build-dir needs a value"; build_dir="$2"; shift 2 ;;
        --dist-dir)  [ $# -ge 2 ] || die "--dist-dir needs a value"; dist_dir="$2"; shift 2 ;;
        --python)    [ $# -ge 2 ] || die "--python needs a value"; python="$2"; shift 2 ;;
        --test)      run_tests=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           usage >&2; die "unknown option: $1" ;;
    esac
done

command -v objdump >/dev/null || die "objdump is needed for the glibc check (Debian/Ubuntu: apt install binutils)"
mkdir -p "$build_dir" "$dist_dir"
build_dir=$(cd "$build_dir" && pwd)
dist_dir=$(cd "$dist_dir" && pwd)

# ---- 1. the interpreter -----------------------------------------------------------
if [ -z "$python" ]; then
    [ "$(uname -m)" = x86_64 ] || die "the pinned Python is for x86_64, this is $(uname -m): pass --python"
    pbs_name="cpython-$pbs_version+$pbs_release-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
    pbs_dir="$build_dir/cpython-$pbs_version+$pbs_release"
    python="$pbs_dir/python/bin/python3"
    if [ ! -x "$python" ]; then
        say "downloading python-build-standalone CPython $pbs_version ($pbs_release)"
        part="$build_dir/$pbs_name.part"
        fetch "https://github.com/astral-sh/python-build-standalone/releases/download/$pbs_release/${pbs_name//+/%2B}" "$part" \
            || die "could not download $pbs_name"
        echo "$pbs_sha256  $part" | sha256sum -c --status \
            || die "$pbs_name does not match its sha256 ($pbs_sha256)"
        rm -rf "$pbs_dir" "$pbs_dir.part"
        mkdir -p "$pbs_dir.part"
        tar -xzf "$part" -C "$pbs_dir.part"
        mv "$pbs_dir.part" "$pbs_dir"
        rm -f "$part"
        [ -x "$python" ] || die "$pbs_name has no python/bin/python3"
    fi
else
    python=$(command -v "$python") || die "no $python"
fi

# ---- 2. the venv --------------------------------------------------------------------
venv="$build_dir/venv"
vpy="$venv/bin/python"
base=$("$python" -c 'import sys; print(sys.executable)')
# one venv per interpreter: a venv made from another Python is made again
if [ ! -x "$vpy" ] || [ "$(cat "$venv/.netpunch-base" 2>/dev/null || true)" != "$base" ]; then
    say "venv $venv from $base ($("$python" -c 'import sys; print(sys.version.split()[0])'))"
    rm -rf "$venv"
    "$python" -m venv "$venv" || die "cannot make a venv with $python (Debian/Ubuntu: apt install python3-venv)"
    printf '%s\n' "$base" > "$venv/.netpunch-base"
fi
pyver=$("$vpy" -c 'import sys; print("%d.%d" % sys.version_info[:2])')
"$vpy" -m pip install --disable-pip-version-check -q --upgrade pip

# ---- 3. the packages: requirements.txt, miniupnpc on its own ----------------------
grep -v -i -E '^[[:space:]]*miniupnpc' "$np/requirements.txt" > "$build_dir/requirements-pure.txt"
# --prefer-binary: zstandard's C backend comes as its manylinux wheel
"$vpy" -m pip install --disable-pip-version-check -q --prefer-binary -r "$build_dir/requirements-pure.txt" \
    "pyinstaller==$pyinstaller_version"

if ! "$vpy" -c 'import miniupnpc' 2>/dev/null; then
    if "$vpy" -m pip install --disable-pip-version-check -q --only-binary=:all: miniupnpc \
            > "$build_dir/miniupnpc-wheel.log" 2>&1; then
        say "miniupnpc: manylinux wheel"
    else
        say "miniupnpc: no wheel for Python $pyver -- building it from source"
        cflags="${CFLAGS:-}"
        inc=$("$vpy" -c 'import sysconfig; print(sysconfig.get_paths()["include"])')
        if [ ! -f "$inc/Python.h" ] && command -v apt-get >/dev/null && command -v dpkg-deb >/dev/null; then
            pkg="libpython$pyver-dev"
            say "no $inc/Python.h: fetching $pkg into the build dir (apt-get download, no root)"
            rm -rf "$build_dir/debs" "$build_dir/pydev"
            mkdir -p "$build_dir/debs" "$build_dir/pydev"
            if (cd "$build_dir/debs" && apt-get download "$pkg"); then
                for deb in "$build_dir"/debs/*.deb; do dpkg-deb -x "$deb" "$build_dir/pydev"; done
                multiarch=$("$vpy" -c 'import sysconfig; print(sysconfig.get_config_var("MULTIARCH") or "")')
                root="$build_dir/pydev/usr/include"
                # Debian's python3.X/pyconfig.h includes <MULTIARCH/python3.X/pyconfig.h>,
                # so the bare include folder has to be on the path as well
                cflags="$cflags -I$root -I$root/python$pyver"
                [ -z "$multiarch" ] || cflags="$cflags -I$root/$multiarch/python$pyver"
            else
                say "apt-get download $pkg failed"
            fi
        fi
        if ! CFLAGS="$cflags" "$vpy" -m pip install --disable-pip-version-check -q --no-cache-dir miniupnpc \
                > "$build_dir/miniupnpc-build.log" 2>&1; then
            tail -15 "$build_dir/miniupnpc-build.log" >&2
            [ "${ALLOW_NO_MINIUPNPC:-0}" = 1 ] \
                || die "miniupnpc did not build (C compiler and python$pyver headers needed; ALLOW_NO_MINIUPNPC=1 builds without UPnP)"
            say "WARNING: building WITHOUT miniupnpc -- this lobby will not try UPnP"
        fi
    fi
fi

# ---- 4. freeze --------------------------------------------------------------------------
out="$dist_dir/netpunch"
exe="$out/netpunch"
say "PyInstaller $pyinstaller_version, Python $pyver -> $out/"
rm -rf "$out"           # an earlier onefile build left a FILE here
# PYTHONDONTWRITEBYTECODE: no __pycache__ left beside the sources in netpunch/
PYTHONDONTWRITEBYTECODE=1 "$vpy" -m PyInstaller --noconfirm --clean --log-level WARN \
    --distpath "$dist_dir" --workpath "$build_dir/work" "$np/netpunch-linux.spec"
[ -x "$exe" ] && [ -d "$out/_internal" ] || die "PyInstaller produced no $exe with _internal/"

# ---- 5. what went in ----------------------------------------------------------------
listing=$("$venv/bin/pyi-archive_viewer" --list --recursive --brief "$exe" 2>/dev/null) \
    || die "pyi-archive_viewer could not read $exe"
missing=""
for m in lobby punch connect observe mesh seal modshare desynclogs linuxpaths stun zstandard; do
    grep -q -E "(^|[[:space:]'/])$m([.'[:space:]]|$)" <<<"$listing" || missing="$missing $m"
done
[ -n "$(find "$out/_internal" -path '*/zstandard/backend_c*.so' -print -quit)" ] || missing="$missing zstandard.backend_c"
[ -z "$missing" ] || die "missing from the build:$missing"
if [ -n "$(find "$out/_internal" -name 'miniupnpc*.so' -print -quit)" ]; then
    say "bundled: the lobby modules, pystun3, zstandard (C backend), miniupnpc"
else
    [ "${ALLOW_NO_MINIUPNPC:-0}" = 1 ] || die "miniupnpc is missing from the build"
    say "bundled: the lobby modules, pystun3, zstandard (C backend) -- NO miniupnpc"
fi
# ssl: urllib handles https:// only when the ssl module loads, and a closed
# loopback port answers at once. This also shows that the executable starts.
answer=$(cd "$build_dir" && "$exe" --print-public-list https://127.0.0.1:9 2>&1 || true)
case "$answer" in
    ""|*"[PYI-"*|*Traceback*|*"unknown url type"*) die "the executable failed its HTTPS probe: $answer" ;;
esac
say "https probe: $answer"

# ---- 6. the glibc floor ---------------------------------------------------------------
newest="" newest_file="" too_new=""
while IFS= read -r -d '' f; do
    v=$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -u -V | tail -1 || true)
    [ -n "$v" ] || continue
    rel=${f#"$out"/}
    if [ -z "$newest" ] || newer "$v" "$newest"; then newest=$v; newest_file=$rel; fi
    if newer "$v" "$glibc_max"; then too_new="$too_new $rel (GLIBC_$v)"; fi
done < <(find "$out" -type f \( -name netpunch -o -name '*.so' -o -name '*.so.*' \) -print0)
[ -n "$newest" ] || die "objdump found no glibc symbol versions under $out"
[ -z "$too_new" ] \
    || die "these need a glibc newer than $glibc_max (GLIBC_MAX), so the lobby would not start everywhere the game does:$too_new"
say "needs glibc >= $newest ($newest_file); the limit is $glibc_max"

# ---- 7. self-tests with the executable ------------------------------------------------
if [ "$run_tests" = 1 ]; then
    work=$(mktemp -d "${TMPDIR:-/tmp}/netpunch-selftest.XXXXXX")
    failed=""
    # one at a time: the tests use fixed loopback ports
    for t in --selftest --selftest-relay --selftest-mesh --selftest-mods --selftest-transfer; do
        log="$work/${t#--}.log"
        start=$(date +%s)
        if (cd "$work" && timeout 900 "$exe" "$t" > "$log" 2>&1); then
            say "$t: $(grep -E 'PASS' "$log" | tail -1) ($(( $(date +%s) - start )) s)"
        else
            failed="$failed $t"
            say "$t: FAILED ($(( $(date +%s) - start )) s), log $log"
            tail -20 "$log"
        fi
    done
    [ -z "$failed" ] || die "self-tests failed:$failed"
    rm -rf "$work"
fi

say "done: $out/ ($(du -sh "$out" | cut -f1)) -- deploy netpunch and _internal/ together"
