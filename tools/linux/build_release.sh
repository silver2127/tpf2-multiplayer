#!/usr/bin/env bash
# build_release.sh -- builds the Linux release of TpF2 Multiplayer.
#
#   tools/linux/build_release.sh [--build-dir DIR] [--out DIR] [--version V]
#                                [--netpunch PATH | --no-netpunch] [--without LIB]...
#                                [--bigmap-repo CHECKOUT_OR_WORKTREE]
#
# 1. Configures and builds native/linux in a build folder of its own (default
#    native/linux/out-release; native/linux/out is the developers' folder and is
#    refused), then checks each library's dynamic exports: the loader exports
#    clock and the gated Lua formatter interposer, the libraries it loads
#    nothing (exports_*.map). Every library
#    boot.cpp loads is required: libtpf2mp_boot.so tpf2_bridge_mp.so
#    tpf2_menu.so tpf2_slice.so tpf2_pluginhost.so. --without tpf2_slice.so or
#    --without tpf2_pluginhost.so (repeatable) leaves one out on purpose, and
#    BUILDINFO says so.
# 2. The lobby: tools/linux/build_netpunch.sh builds netpunch/dist-linux/netpunch,
#    which must then be newer than the start of that build. Without that script
#    --netpunch PATH (a file, or a folder holding netpunch) is required: an old
#    netpunch/dist-linux/netpunch is never taken on its own. --netpunch also
#    skips the build; --no-netpunch makes a release without the lobby. BUILDINFO
#    records where the lobby came from, its sha256 and its date.
# 3. Lays out <out>/tpf2mp-linux-<version>/:
#      install.sh uninstall.sh collect_logs.sh tpf2mp_paths.sh INSTALL.md
#      LICENSE THIRD_PARTY_NOTICES.md VERSION BUILDINFO SHA256SUMS
#      lib/libtpf2mp_boot.so lib/tpf2_*.so     (what boot.cpp loads)
#      lib/plugins/tpf2_previews.so           (optional native 3D previews)
#      mod/mp_lockstep_1/
#      netpunch/netpunch
#    and packs it as <out>/tpf2mp-linux-<version>.tar.gz (default out: dist/linux).
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=$REPO/native/linux/out-release
OUT=$REPO/dist/linux
VERSION="" NETPUNCH="" NO_NETPUNCH=0 HOST_BUILD=0 BIGMAP_REPO=""
WITHOUT=()
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --bigmap-repo) BIGMAP_REPO=$(realpath "${2:?}"); shift 2 ;;
    --build-dir) BUILD=${2:?}; shift 2 ;;
    --out) OUT=${2:?}; shift 2 ;;
    --version) VERSION=${2:?}; shift 2 ;;
    --netpunch) NETPUNCH=${2:?}; shift 2 ;;
    --no-netpunch) NO_NETPUNCH=1; shift ;;
    --host-build) HOST_BUILD=1; shift ;;
    --without) WITHOUT+=("${2:?--without needs a library name}"); shift 2 ;;
    -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
[ -n "$VERSION" ] || VERSION=$(tr -d '[:space:]' <"$REPO/installer/VERSION" 2>/dev/null || true)
[ -n "$VERSION" ] || die "no --version and no installer/VERSION"
[[ "$VERSION" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "version must contain only letters, digits, dot, underscore or hyphen"
BUILD=$(realpath -m "$BUILD"); OUT=$(realpath -m "$OUT")
[ "$BUILD" != "$(realpath -m "$REPO/native/linux/out")" ] || die "native/linux/out is the developers' build folder; pass another --build-dir"
[ -f "$REPO/docs/linux/INSTALL.md" ] || die "docs/linux/INSTALL.md is missing"

# What boot.cpp loads, and which of it a release may go without on purpose.
ALL_LIBS=(libtpf2mp_boot.so tpf2_bridge_mp.so tpf2_menu.so tpf2_slice.so tpf2_pluginhost.so)
OMITTABLE=" tpf2_slice.so tpf2_pluginhost.so "
LEFT_OUT=()
for w in "${WITHOUT[@]:+${WITHOUT[@]}}"; do
  case "$w" in *.so) ;; *) w=$w.so ;; esac
  case "$OMITTABLE" in *" $w "*) ;; *) die "--without $w: only tpf2_slice.so and tpf2_pluginhost.so can be left out" ;; esac
  case " ${LEFT_OUT[*]:-} " in *" $w "*) ;; *) LEFT_OUT+=("$w") ;; esac
done
if [ $NO_NETPUNCH = 1 ] && [ -n "$NETPUNCH" ]; then die "--netpunch and --no-netpunch exclude each other"; fi
if [ -n "$NETPUNCH" ]; then
  [ -e "$NETPUNCH" ] || die "--netpunch $NETPUNCH does not exist"
  NETPUNCH=$(realpath "$NETPUNCH")
fi
NAME=tpf2mp-linux-$VERSION
STAGE=$OUT/$NAME
# Staging replaces this whole folder. Never delete an explicitly selected
# build or lobby source, or recursively copy a lobby into its own subtree.
case "$BUILD/" in "$STAGE/"*) die "--build-dir must be outside the release staging folder $STAGE" ;; esac
if [ -n "$NETPUNCH" ]; then
  case "$NETPUNCH" in "$STAGE"|"$STAGE"/*) die "--netpunch must be outside the release staging folder $STAGE" ;; esac
  if [ -d "$NETPUNCH" ]; then
    case "$STAGE/" in "$NETPUNCH/"*) die "the release staging folder must be outside --netpunch" ;; esac
  fi
fi
if [ $NO_NETPUNCH = 0 ] && [ -z "$NETPUNCH" ] && [ ! -f "$REPO/tools/linux/build_netpunch.sh" ]; then
  die "tools/linux/build_netpunch.sh is missing, so the lobby cannot be built here: pass --netpunch PATH (a lobby built from this version) or --no-netpunch"
fi

# The Windows Lua sources are an explicit compatibility requirement.
python3 "$REPO/tools/linux/verify_lua_release.py"

# ---- 1. the libraries -------------------------------------------------------------------
say "== building native/linux in $BUILD"
GEN=()
if command -v ninja >/dev/null 2>&1 && [ ! -f "$BUILD/CMakeCache.txt" ]; then GEN=(-G Ninja); fi
if [ "$HOST_BUILD" = 1 ]; then
  cmake -S "$REPO/native/linux" -B "$BUILD" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" --parallel 4
  ctest --test-dir "$BUILD" --output-on-failure
else
  bash "$REPO/tools/linux/build_native.sh" --build-dir "$BUILD"
fi

LIBS=()
for l in "${ALL_LIBS[@]}"; do
  case " ${LEFT_OUT[*]:-} " in *" $l "*) say "note: $l is left out (--without)"; continue ;; esac
  [ -f "$BUILD/$l" ] || die "the build did not produce $l; pass --without $l to make a release without it on purpose"
  LIBS+=("$l")
done
PLUGINS=()
case " ${LEFT_OUT[*]:-} " in
  *" tpf2_pluginhost.so "*) ;;
  *)
    [ -f "$BUILD/tpf2_previews.so" ] || die "the build did not produce tpf2_previews.so"
    PLUGINS+=(tpf2_previews.so)
    ;;
esac
if [ -n "$BIGMAP_REPO" ]; then
  case " ${LEFT_OUT[*]:-} " in *" tpf2_pluginhost.so "*) die "Big Maps requires tpf2_pluginhost.so" ;; esac
  case "$BIGMAP_REPO/" in "$STAGE/"*) die "Big Maps source must be outside staging" ;; esac
  bash "$REPO/tools/linux/build_bigmap.sh" "$BIGMAP_REPO" "$BUILD/bigmap"
  install -m 0755 "$BUILD/bigmap/tpf2_bigmap.so" "$BUILD/tpf2_bigmap.so"
  PLUGINS+=(tpf2_bigmap.so)
fi
# A symbol the loader exports wins over the game's own for the whole process
# (it is in LD_PRELOAD); a library the loader dlopens needs to export nothing.
if command -v nm >/dev/null 2>&1; then
  for l in "${LIBS[@]}"; do
    exports=$(nm -D --defined-only "$BUILD/$l" | awk '{print $NF}' | sort | paste -sd' ' -)
    want=""; [ "$l" != libtpf2mp_boot.so ] || want="__sprintf_chk clock"
    [ "$exports" = "$want" ] || die "$l exports '${exports}', expected '${want}' (see native/linux/exports_*.map)"
  done
  for l in "${PLUGINS[@]}"; do
    exports=$(nm -D --defined-only "$BUILD/$l" | awk '{print $NF}' | sort | paste -sd' ' -)
    [ "$exports" = Tpf2mpPluginInit ] || die "$l exports '${exports}', expected 'Tpf2mpPluginInit'"
  done
else
  say "warning: nm is missing: the libraries' exports were not checked"
fi

# ---- 2. the lobby ----------------------------------------------------------------------------
NETPUNCH_FROM=""
if [ $NO_NETPUNCH = 1 ]; then
  :
elif [ -n "$NETPUNCH" ]; then
  NETPUNCH_FROM="given with --netpunch ($(basename "$NETPUNCH"))"
else
  say "== tools/linux/build_netpunch.sh"
  started=$(date +%s)
  bash "$REPO/tools/linux/build_netpunch.sh"
  NETPUNCH=$REPO/netpunch/dist-linux/netpunch
  [ -x "$NETPUNCH/netpunch" ] && [ -d "$NETPUNCH/_internal" ] || die "build_netpunch.sh did not produce the lobby and _internal directory"
  [ "$(stat -c %Y "$NETPUNCH/netpunch")" -ge "$started" ] || die "the lobby executable was not rebuilt by build_netpunch.sh"
  NETPUNCH_FROM="built by tools/linux/build_netpunch.sh"
fi

# ---- 3. the release folder -----------------------------------------------------------------
say "== laying out $STAGE"
rm -rf "$STAGE" "$OUT/$NAME.tar.gz"
mkdir -p "$STAGE/lib" "$STAGE/mod"
for l in "${LIBS[@]}"; do install -m 0755 "$BUILD/$l" "$STAGE/lib/$l"; done
if [ ${#PLUGINS[@]} -gt 0 ]; then
  mkdir -p "$STAGE/lib/plugins"
  for l in "${PLUGINS[@]}"; do install -m 0755 "$BUILD/$l" "$STAGE/lib/plugins/$l"; done
fi
if [ -n "$BIGMAP_REPO" ]; then
  install -m 0644 "$BIGMAP_REPO/linux/tpf2_bigmap.cfg" "$STAGE/lib/plugins/tpf2_bigmap.cfg"
fi
cp -R "$REPO/mod/mp_lockstep_1" "$STAGE/mod/"
python3 "$REPO/tools/linux/verify_lua_release.py" --mod-dir "$STAGE/mod/mp_lockstep_1"
NP_BIN=""   # the lobby executable taken, for its date in BUILDINFO
if [ -n "$NETPUNCH" ]; then
  mkdir -p "$STAGE/netpunch"
  if [ -d "$NETPUNCH" ]; then cp -R "$NETPUNCH/." "$STAGE/netpunch/"; NP_BIN=$NETPUNCH/netpunch
  else install -m 0755 "$NETPUNCH" "$STAGE/netpunch/netpunch"; NP_BIN=$NETPUNCH; fi
  [ -x "$STAGE/netpunch/netpunch" ] || die "the lobby folder has no executable netpunch"
fi
for s in install.sh uninstall.sh collect_logs.sh; do install -m 0755 "$REPO/tools/linux/$s" "$STAGE/$s"; done
install -m 0755 "$REPO/tools/linux/tpf2mp-launch" "$STAGE/tpf2mp-launch"
install -m 0644 "$REPO/tools/linux/tpf2mp_paths.sh" "$STAGE/tpf2mp_paths.sh"
install -m 0644 "$REPO/docs/linux/INSTALL.md" "$STAGE/INSTALL.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_0.5.6.md" "$STAGE/UPSTREAM_0.5.6.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_55e97a48.md" "$STAGE/UPSTREAM_dev_55e97a48.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_3edfbccd.md" "$STAGE/UPSTREAM_dev_3edfbccd.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_cae5d370.md" "$STAGE/UPSTREAM_dev_cae5d370.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_6cb03915.md" "$STAGE/UPSTREAM_dev_6cb03915.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_38432b5f.md" "$STAGE/UPSTREAM_dev_38432b5f.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_d3135a59.md" "$STAGE/UPSTREAM_dev_d3135a59.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_59bb258a.md" "$STAGE/UPSTREAM_dev_59bb258a.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_a658fc11.md" "$STAGE/UPSTREAM_dev_a658fc11.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_5fb7aea2.md" "$STAGE/UPSTREAM_dev_5fb7aea2.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_8e31f1e0.md" "$STAGE/UPSTREAM_dev_8e31f1e0.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_61578d27.md" "$STAGE/UPSTREAM_dev_61578d27.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_be3b86ae.md" "$STAGE/UPSTREAM_dev_be3b86ae.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_d6db920f.md" "$STAGE/UPSTREAM_dev_d6db920f.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_50d7588b.md" "$STAGE/UPSTREAM_dev_50d7588b.md"
install -m 0644 "$REPO/docs/linux/UPSTREAM_dev_66c870cf.md" "$STAGE/UPSTREAM_dev_66c870cf.md"
for f in LICENSE THIRD_PARTY_NOTICES.md; do [ ! -f "$REPO/$f" ] || install -m 0644 "$REPO/$f" "$STAGE/$f"; done
printf '%s\n' "$VERSION" >"$STAGE/VERSION"

COMMIT=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
if [ "$COMMIT" != unknown ] && [ -n "$(git -C "$REPO" status --porcelain 2>/dev/null | head -n 1)" ]; then COMMIT="$COMMIT (with uncommitted changes)"; fi
CXX=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt" | head -n 1)
{
  echo "TpF2 Multiplayer for Linux $VERSION"
  echo "built:    $(date -u '+%Y-%m-%d %H:%M UTC') from commit $COMMIT"
  if [ "$HOST_BUILD" = 1 ]; then
    echo "compiler: ${CXX:-?} ($("${CXX:-c++}" --version 2>/dev/null | head -n 1))"
  else
    echo "compiler: ${CXX:-?} inside soldier SDK (version below)"
  fi
  echo "game:     Transport Fever 2, Steam Linux build 35924 (build-id 3a0e156390b0e6f1e372051c24802c8493ae454a)"
  echo "Lua: dev 66c870cfb4595d361d5c65221516cd1dcb967284; all 28 files exact"
  if [ -n "$BIGMAP_REPO" ]; then echo "Big Maps: $BIGMAP_REPO $(git -C "$BIGMAP_REPO" rev-parse HEAD) (working tree built)"; fi
  echo "libraries: ${LIBS[*]}"
  echo "plugins: ${PLUGINS[*]:-none}"
  if [ "$HOST_BUILD" = 1 ]; then echo "native runtime: host build (development only)"
  else echo "native runtime: Steam Runtime soldier"; cat "$BUILD/NATIVE_BUILDINFO"; fi
  if [ ${#LEFT_OUT[@]} -gt 0 ]; then echo "left out: ${LEFT_OUT[*]} (--without)"; else echo "left out: none"; fi
  if [ -n "$NETPUNCH" ]; then
    echo "lobby:    netpunch/netpunch, $NETPUNCH_FROM"
    echo "lobby sha256:   $(sha256sum "$STAGE/netpunch/netpunch" | cut -d' ' -f1)"
    echo "lobby modified: $(date -u -r "$NP_BIN" '+%Y-%m-%d %H:%M:%S UTC')"
  else
    echo "lobby:    NOT INCLUDED (--no-netpunch)"
  fi
} >"$STAGE/BUILDINFO"
(cd "$STAGE" && find . -type f -printf '%P\n' | LC_ALL=C sort | xargs -d '\n' sha256sum) >"$OUT/.SHA256SUMS.$$"
mv "$OUT/.SHA256SUMS.$$" "$STAGE/SHA256SUMS"

# ---- 4. the tarball ------------------------------------------------------------------------------
tar -C "$OUT" --sort=name --owner=0 --group=0 --numeric-owner -czf "$OUT/$NAME.tar.gz" "$NAME"
cat "$REPO/tools/linux/self_extract.sh" "$OUT/$NAME.tar.gz" > "$OUT/$NAME.run"
chmod 0755 "$OUT/$NAME.run"
say "== done: $OUT/$NAME.tar.gz ($(du -h "$OUT/$NAME.tar.gz" | cut -f1))"
say "== installer: $OUT/$NAME.run"
