#!/usr/bin/env bash
# install.sh -- installs TpF2 Multiplayer for the native Linux version of
# Transport Fever 2 (Steam build 35924) from this release folder.
#
#   ./install.sh [--game DIR] [--data-home DIR] [--launch-options] [--force] [--dry-run]
#
# For the Steam that runs the game (tpf2mp_paths.sh explains native, Snap, Flatpak):
#   lib/*              -> <data home>/tpf2mp/          the loader and the libraries it loads
#   netpunch/*         -> <data home>/tpf2mp/netpunch/ the lobby
#   mod/mp_lockstep_1  -> <game>/mods/mp_lockstep_1    the game-script mod
# where <data home> is the XDG_DATA_HOME the game sees (Snap Steam:
# ~/snap/steam/common/.local/share). Then Steam's start of the game is made to
# preload the loader: the installer prints a durable wrapper line for Steam
# launch options. --patch-runsh instead adds a legacy preload block to run.sh
# and keeps the original beside it as run.sh.tpf2mp-orig; Steam updates may
# restore that file. Running the installer again updates in place, and
# removes the files an earlier install put into <data home>/tpf2mp that this
# release does not have. It stops while Transport Fever 2 is running.
# uninstall.sh undoes it from <data home>/tpf2mp/tpf2mp_install.txt; the data
# and logs folders are never touched.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ -f "$HERE/tpf2mp_paths.sh" ] || { echo "error: tpf2mp_paths.sh must be beside install.sh" >&2; exit 1; }
# shellcheck source=tpf2mp_paths.sh
. "$HERE/tpf2mp_paths.sh"

usage() {
  sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
  cat <<'EOF'
Options:
  --game DIR        the Transport Fever 2 folder (default: found in the Steam libraries)
  --data-home DIR   the XDG_DATA_HOME the game sees (default: from the kind of Steam)
  --launch-options  print the durable Steam launch wrapper line (default)
  --patch-runsh     patch run.sh instead (Steam updates may undo this)
  --release DIR     the release folder to install from (default: this script's folder)
  --force           install even for a game binary that is not build 35924, or
                    while Transport Fever 2 is running
  --dry-run         say what would be done, change nothing
EOF
}

GAME="" DATA_HOME="" LAUNCH=1 FORCE=0 RELEASE=$HERE
TPF2MP_DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --game) GAME=${2:?--game needs a folder}; shift 2 ;;
    --data-home) DATA_HOME=${2:?--data-home needs a folder}; shift 2 ;;
    --release) RELEASE=${2:?--release needs a folder}; shift 2 ;;
    --launch-options) LAUNCH=1; shift ;;
    --patch-runsh) LAUNCH=0; shift ;;
    --force) FORCE=1; shift ;;
    --dry-run) TPF2MP_DRY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) tpf2mp_die "unknown option $1 (see --help)" ;;
  esac
done
export TPF2MP_DRY

[ -f "$RELEASE/lib/libtpf2mp_boot.so" ] || tpf2mp_die "$RELEASE is not a release folder (no lib/libtpf2mp_boot.so)"
[ -f "$RELEASE/mod/mp_lockstep_1/mod.lua" ] || tpf2mp_die "$RELEASE has no mod/mp_lockstep_1/mod.lua"
[ -f "$RELEASE/tpf2mp-launch" ] || tpf2mp_die "$RELEASE has no launch wrapper"
if [ -f "$RELEASE/SHA256SUMS" ]; then
  (cd "$RELEASE" && sha256sum -c --quiet SHA256SUMS) || tpf2mp_die "release checksum verification failed"
fi

# ---- which game, which Steam ----------------------------------------------------
KIND="" STEAM=""
if [ -n "$GAME" ]; then
  [ -f "$GAME/TransportFever2" ] || tpf2mp_die "no TransportFever2 in $GAME"
  GAME=$(realpath "$GAME")
  while IFS=$'\t' read -r k s g; do
    if [ "$g" = "$GAME" ]; then KIND=$k; STEAM=$s; fi
  done < <(tpf2mp_find_games)
  [ -n "$KIND" ] || KIND=$(tpf2mp_steam_kind "$GAME")
else
  mapfile -t FOUND < <(tpf2mp_find_games)
  [ ${#FOUND[@]} -gt 0 ] || tpf2mp_die "Transport Fever 2 was not found in any Steam library; pass --game <folder>"
  best=-1
  for row in "${FOUND[@]}"; do
    IFS=$'\t' read -r k s g <<<"$row"
    t=$(tpf2mp_last_run "$s")
    if [ "$t" -gt "$best" ]; then best=$t; KIND=$k; STEAM=$s; GAME=$g; fi
  done
  if [ ${#FOUND[@]} -gt 1 ]; then
    tpf2mp_say "Transport Fever 2 is installed ${#FOUND[@]} times:"
    for row in "${FOUND[@]}"; do IFS=$'\t' read -r k s g <<<"$row"; tpf2mp_say "  $k Steam: $(tpf2mp_tilde "$g")"; done
    tpf2mp_say "Using the one whose Steam ran the game last. Pass --game to choose."
  fi
fi
DEFAULT_HOME=$(realpath -m "$(tpf2mp_game_data_home "$KIND")")
[ -n "$DATA_HOME" ] || DATA_HOME=$DEFAULT_HOME
case "$DATA_HOME" in /*) ;; *) tpf2mp_die "--data-home must be an absolute path" ;; esac
DATA_HOME=$(realpath -m "$DATA_HOME")
ROOT=$DATA_HOME/tpf2mp
BOOT=$ROOT/libtpf2mp_boot.so

tpf2mp_say "Game folder:  $(tpf2mp_tilde "$GAME") ($KIND Steam${STEAM:+, $(tpf2mp_tilde "$STEAM")})"
tpf2mp_say "Mod folder:   $(tpf2mp_tilde "$ROOT")"

# The libraries patch build 35924 only and stay off on anything else, so an
# install for another build would do nothing. A binary whose build-id cannot be
# read does not block the install: the loader checks it again at run time.
BID=""
if command -v readelf >/dev/null 2>&1; then
  BID=$(readelf -n "$GAME/TransportFever2" 2>/dev/null | sed -n 's/^[[:space:]]*Build ID: \([0-9a-f]*\).*/\1/p' | head -n 1) || BID=""
elif command -v file >/dev/null 2>&1; then
  BID=$(file -L "$GAME/TransportFever2" | sed -n 's/.*BuildID\[sha1\]=\([0-9a-f]*\).*/\1/p') || BID=""
fi
if [ -z "$BID" ]; then
  tpf2mp_warn "could not read the game's build-id (no readelf or file); the loader checks it when the game starts"
elif [ "$BID" != "$TPF2MP_BUILD_ID" ]; then
  msg="the game binary is not Steam build 35924 (build-id $BID): multiplayer would stay off. Verify the game files in Steam, or wait for a release for this build"
  if [ $FORCE = 1 ]; then tpf2mp_warn "$msg (--force: installing anyway)"; else tpf2mp_die "$msg"; fi
fi
if [ $LAUNCH = 0 ] && [ "$DATA_HOME" != "$DEFAULT_HOME" ]; then
  tpf2mp_warn "run.sh finds the loader under the game's own XDG_DATA_HOME ($(tpf2mp_tilde "$DEFAULT_HOME")); with --data-home elsewhere use --launch-options"
fi
case "$BOOT" in *[[:space:]:]*) tpf2mp_die "LD_PRELOAD cannot name a path with spaces or colons: $BOOT" ;; esac
# A running game keeps the libraries it has mapped (they are replaced by renames),
# but it reads the mod's scripts again whenever it loads a game or a save: a new
# lockstep.lua beside the old libraries can break that session.
if [ "$(tpf2mp_games_running)" -gt 0 ]; then
  msg="Transport Fever 2 is running: close it before installing"
  if [ "$TPF2MP_DRY" = 1 ]; then tpf2mp_warn "$msg (without --dry-run the install stops here unless --force)"
  elif [ $FORCE = 1 ]; then tpf2mp_warn "$msg (--force: installing anyway; restart the game before loading a game or a save)"
  else tpf2mp_die "$msg, or pass --force"; fi
fi

# Validate the loading change before replacing any installed library or script.
# A rejected run.sh must leave an existing installation usable.
TMPD=$(mktemp -d); trap 'rm -rf "$TMPD"' EXIT
RS=$GAME/run.sh ORIG=$GAME/run.sh.tpf2mp-orig
rc=1
if [ -f "$RS" ]; then
  rc=0; tpf2mp_runsh_strip "$RS" >"$TMPD/stock" || rc=$?
  case $rc in
    0|1) ;;
    *) tpf2mp_die "run.sh has a tpf2mp block edited by hand: restore run.sh.tpf2mp-orig (or verify the game files in Steam) and run install.sh again" ;;
  esac
elif [ $LAUNCH = 0 ]; then
  tpf2mp_die "no run.sh in the game folder: run again with --launch-options"
fi
if [ $LAUNCH = 0 ]; then
  tpf2mp_runsh_patch "$TMPD/stock" >"$TMPD/patched" || tpf2mp_die "run.sh does not start with #!: run again with --launch-options"
fi

# PyInstaller can ship relative shared-library aliases and directory links.
# They must resolve inside their own tree so the installed relative layout has
# the same meaning; reject broken or external links before changing any files.
for src in "$RELEASE/lib" "$RELEASE/netpunch"; do
  [ -d "$src" ] || continue
  src_real=$(realpath "$src")
  while IFS= read -r -d '' f; do
    link=$(readlink "$f")
    case "$link" in /*) tpf2mp_die "release contains an absolute symlink: $f" ;; esac
    target=$(realpath -e "$f") || tpf2mp_die "release contains a broken symlink: $f"
    case "$target" in "$src_real"|"$src_real"/*) ;; *) tpf2mp_die "release symlink leaves its package tree: $f" ;; esac
  done < <(find "$src" -type l -print0)
done

# ---- files ------------------------------------------------------------------------
MANIFEST=()
put_link() {
  local src=$1 dst=$2 tmp
  if [ "$TPF2MP_DRY" = 1 ]; then tpf2mp_say "  would link $(tpf2mp_tilde "$dst")"; return 0; fi
  mkdir -p "$(dirname "$dst")"
  tmp=$(mktemp -d "$(dirname "$dst")/.tpf2mp-link.XXXXXXXX")
  if ! ln -s -- "$(readlink "$src")" "$tmp/link" || ! mv -Tf -- "$tmp/link" "$dst"; then
    rm -rf "$tmp"
    return 1
  fi
  rmdir "$tmp"
}
put_tree() {   # release subfolder, destination folder
  local src=$1 dst=$2 f rel mode
  [ -d "$src" ] || return 0
  while IFS= read -r -d '' f; do
    rel=${f#"$src"/}
    if [ -L "$f" ]; then
      put_link "$f" "$dst/$rel"
    else
      mode=0644; [ -x "$f" ] && mode=0755
      tpf2mp_put "$f" "$dst/$rel" "$mode"
    fi
    MANIFEST+=("file"$'\t'"$dst/$rel")
  done < <(find "$src" \( -type f -o -type l \) -print0 | sort -z)
}
tpf2mp_say "Installing the libraries..."
put_tree "$RELEASE/lib" "$ROOT"
tpf2mp_put "$RELEASE/tpf2mp-launch" "$ROOT/tpf2mp-launch" 0755
MANIFEST+=("file"$'\t'"$ROOT/tpf2mp-launch")
if [ -d "$RELEASE/netpunch" ]; then
  tpf2mp_say "Installing the lobby..."
  put_tree "$RELEASE/netpunch" "$ROOT/netpunch"
else
  tpf2mp_warn "this release has no netpunch folder: HOST and JOIN cannot start the lobby"
fi

tpf2mp_say "Installing the mod..."
MOD=$GAME/mods/mp_lockstep_1
if [ "$TPF2MP_DRY" = 1 ]; then
  tpf2mp_say "  would replace $(tpf2mp_tilde "$MOD")"
else
  NEW=$GAME/mods/.mp_lockstep_1.tpf2mp-new OLD=$GAME/mods/.mp_lockstep_1.tpf2mp-old
  rm -rf "$NEW" "$OLD"
  mkdir -p "$GAME/mods"
  cp -R "$RELEASE/mod/mp_lockstep_1" "$NEW"
  [ -e "$MOD" ] && mv "$MOD" "$OLD"
  mv "$NEW" "$MOD"
  rm -rf "$OLD"
fi
MANIFEST+=("moddir"$'\t'"$MOD")

# ---- loading --------------------------------------------------------------------
if [ $LAUNCH = 1 ]; then
  tpf2mp_runsh_restore "$GAME" || tpf2mp_die "run.sh was left as it is"
  MANIFEST+=("loading"$'\t'"launch-options")
else
  # The backup is the file Steam shipped. A stock run.sh that differs from an old
  # backup came with a game update: it replaces the backup.
  if [ $rc = 1 ] && ! cmp -s "$RS" "$ORIG" 2>/dev/null; then
    tpf2mp_put "$RS" "$ORIG" keep
  elif [ $rc = 0 ] && [ ! -f "$ORIG" ]; then
    cp "$TMPD/stock" "$TMPD/orig" && chmod --reference="$RS" "$TMPD/orig"
    tpf2mp_put "$TMPD/orig" "$ORIG" keep
  fi
  if cmp -s "$TMPD/patched" "$RS"; then
    tpf2mp_say "run.sh already preloads the loader"
  else
    tpf2mp_put "$TMPD/patched" "$RS" keep
    tpf2mp_say "run.sh now preloads the loader (original: run.sh.tpf2mp-orig)"
  fi
  MANIFEST+=("loading"$'\t'"run.sh")
fi

# ---- files of the install before that this release does not have ------------------
# put_tree only adds and replaces, and boot.cpp loads every library of its list
# that it finds: a library an older release had must not stay beside newer ones.
OLDM=$ROOT/tpf2mp_install.txt
if [ -f "$OLDM" ]; then
  declare -A KEEP=()
  for m in "${MANIFEST[@]}"; do
    case "$m" in file$'\t'*) KEEP[${m#file$'\t'}]=1 ;; esac
  done
  GONE_DIRS=()
  while IFS=$'\t' read -r kind path; do
    [ "$kind" = file ] || continue
    case "$path" in "$ROOT"/*) ;; *) continue ;; esac
    case "/$path/" in */../*|*/./*) continue ;; esac
    [ -z "${KEEP[$path]:-}" ] || continue
    [ -e "$path" ] || [ -L "$path" ] || continue
    if [ "$TPF2MP_DRY" = 1 ]; then
      tpf2mp_say "  would remove $(tpf2mp_tilde "$path") (installed before, not in this release)"
    else
      rm -f "$path"
      tpf2mp_say "  removed $(tpf2mp_tilde "$path") (installed before, not in this release)"
      GONE_DIRS+=("$(dirname "$path")")
    fi
  done <"$OLDM"
  # and their folders (plugins/, netpunch/) when nothing else is left in them
  for d in "${GONE_DIRS[@]:+${GONE_DIRS[@]}}"; do
    while [ "$d" != "$ROOT" ] && case "$d" in "$ROOT"/*) true ;; *) false ;; esac; do
      rmdir "$d" 2>/dev/null || break
      d=$(dirname "$d")
    done
  done
fi

if [ "$TPF2MP_DRY" = 0 ]; then
  {
    printf '# TpF2 Multiplayer install manifest, read by uninstall.sh\n'
    printf 'version\t%s\n' "$(cat "$RELEASE/VERSION" 2>/dev/null || echo unknown)"
    printf 'installed\t%s\n' "$(date '+%Y-%m-%d %H:%M:%S %z')"
    printf 'game\t%s\n' "$GAME"
    printf '%s\n' "${MANIFEST[@]}"
  } >"$ROOT/tpf2mp_install.txt"
fi

tpf2mp_say ""
if [ $LAUNCH = 1 ]; then
  tpf2mp_say "Last step: in Steam, right-click Transport Fever 2 > Properties > Launch Options, and enter:"
  printf -v LAUNCH_LINE '%q %%command%%' "$ROOT/tpf2mp-launch"
  tpf2mp_say "  $LAUNCH_LINE"
else
  tpf2mp_say "Done. Start Transport Fever 2 from Steam; the title menu shows MULTIPLAYER."
fi
tpf2mp_say "Loader log: $(tpf2mp_tilde "$ROOT/data/tpf2_proxy.log")"
