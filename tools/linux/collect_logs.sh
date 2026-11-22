#!/usr/bin/env bash
# collect_logs.sh -- TpF2 Multiplayer log collector for Linux, the counterpart of
# tools/collect_logs.cmd.
#
#   ./collect_logs.sh [--out DIR]
#
# Writes tpf2mp-logs-<computer>-<time>.zip (.tar.gz when zip is missing) to your
# Downloads folder (else your home folder) and says where. Nothing is uploaded.
# Read-only on everything else: logs are read while a running game may still
# write them, and a file over 64 MB keeps only its last 64 MB.
#
# What goes in, for every kind of Steam found (native, Snap, Flatpak; see
# tpf2mp_paths.sh):
#   <kind>/data/          the mod's data folder: *.log *.txt *.cfg *.json *.jsonl, plugins/
#   <kind>/saved_runs/    the newest 3 runs the mod saved in tpf2mp/logs/
#   <kind>/netpunch/      the lobby's *.log *.json *.jsonl
#   <kind>/installed_files.txt   the installed libraries (size, date, sha256), the manifest
#   game<N>/              each game folder: run.sh and its backup, *.log, *.cfg, the game's
#                         build-id, whether the mod and the run.sh block are there
#   gameuser<N>/          each Steam account's game folder: stdout.txt, stdout_old.txt,
#                         settings.lua, the newest 5 crash dumps of the last 14 days
#   system.txt, manifest.txt
# Paths in the text files show the home folder as ~.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ -f "$HERE/tpf2mp_paths.sh" ] || { echo "error: tpf2mp_paths.sh must be beside collect_logs.sh" >&2; exit 1; }
# shellcheck source=tpf2mp_paths.sh
. "$HERE/tpf2mp_paths.sh"
shopt -s nullglob

MAX_FILE_BYTES=$((64 * 1024 * 1024))
MAX_DUMPS=5
DUMP_DAYS=14
OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT=${2:?--out needs a folder}; shift 2 ;;
    -h|--help) sed -n '2,/^set -uo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) tpf2mp_die "unknown option $1 (see --help)" ;;
  esac
done

HOST=$(hostname 2>/dev/null || uname -n)
ARCHIVE_NAME=tpf2mp-logs-$HOST-$(date +%Y%m%d-%H%M%S)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/tpf2mp-logs.XXXXXX") || tpf2mp_die "cannot create a temporary folder"
trap 'rm -rf "$WORK"' EXIT
STAGE=$WORK/$ARCHIVE_NAME
mkdir -p "$STAGE"
MANIFEST=$STAGE/manifest.txt
note() { printf '%s\n' "$*" >>"$MANIFEST"; }
say() { tpf2mp_say "$@"; }

# Copies one file; a long one keeps its tail (the end of a log is what matters).
copy_file() {   # src destdir [destname]
  local src=$1 dir=$2 name=${3:-} size
  [ -n "$name" ] || name=$(basename "$1")
  { [ -f "$src" ] && [ -r "$src" ]; } || return 1
  size=$(stat -c %s "$src" 2>/dev/null) || return 1
  mkdir -p "$dir"
  if [ "$size" -gt "$MAX_FILE_BYTES" ]; then
    tail -c "$MAX_FILE_BYTES" "$src" >"$dir/$name" 2>/dev/null || { note "  FAILED $(tpf2mp_tilde "$src")"; return 1; }
    note "  TRUNCATED to the last 64 MB of $((size / 1048576)) MB: $(tpf2mp_tilde "$src")"
  else
    cat "$src" >"$dir/$name" 2>/dev/null || { note "  FAILED $(tpf2mp_tilde "$src")"; return 1; }
    note "  $(tpf2mp_tilde "$src") ($size bytes)"
  fi
  touch -r "$src" "$dir/$name" 2>/dev/null
  return 0
}
# Copies the files of one folder that match the patterns; prints how many.
copy_matching() {   # dir destdir pattern...
  local dir=$1 dest=$2 n=0 p f
  shift 2
  [ -d "$dir" ] || { echo 0; return; }
  for p in "$@"; do
    for f in "$dir"/$p; do   # $p unquoted on purpose: it is a glob
      copy_file "$f" "$dest" && n=$((n + 1))
    done
  done
  echo "$n"
}
inventory_line() {   # file, shown relative to base
  local f=$1 base=$2 h
  h=$(sha256sum "$f" 2>/dev/null | cut -d' ' -f1)
  printf '%-40s %10s bytes  %s  %s\n' "${f#"$base"/}" "$(stat -c %s "$f")" "$(date -r "$f" '+%Y-%m-%d %H:%M')" "${h:-(unreadable)}"
}

say ""
say "Collecting Transport Fever 2 multiplayer logs..."
say ""

# ---- 1. the mod's folders ------------------------------------------------------------
mapfile -t ROOTS < <(tpf2mp_mod_roots)
if [ -n "${TPF2MP_DATADIR:-}" ] && [ -d "$TPF2MP_DATADIR" ]; then
  note "== TPF2MP_DATADIR: $(tpf2mp_tilde "$TPF2MP_DATADIR")"
  n=$(copy_matching "$TPF2MP_DATADIR" "$STAGE/custom/data" '*.log' '*.txt' '*.cfg' '*.json' '*.jsonl')
  say "  data folder from TPF2MP_DATADIR: $n file(s)"
fi
[ ${#ROOTS[@]} -gt 0 ] || { say "  mod folder:  not found (is TpF2 Multiplayer installed?)"; note "== mod folder: not found"; }
for row in "${ROOTS[@]:+${ROOTS[@]}}"; do
  IFS=$'\t' read -r kind root <<<"$row"
  dest=$STAGE/$kind
  note "== mod folder ($kind Steam): $(tpf2mp_tilde "$root")"
  n=$(copy_matching "$root/data" "$dest/data" '*.log' '*.txt' '*.cfg' '*.json' '*.jsonl')
  n=$((n + $(copy_matching "$root/data/plugins" "$dest/data/plugins" '*.cfg' '*.log' '*.txt')))
  runs=0
  while IFS= read -r r; do
    [ -n "$r" ] || continue
    note "== saved run: $r"
    n=$((n + $(copy_matching "$root/logs/$r" "$dest/saved_runs/$r" '*.log' '*.txt' '*.dmp')))
    runs=$((runs + 1))
  done < <(find "$root/logs" -mindepth 1 -maxdepth 1 -type d -regextype posix-extended \
             -regex '.*/[0-9]{8}-[0-9]{6}-[a-z0-9]+' -printf '%f\n' 2>/dev/null | sort -r | head -n 3)
  n=$((n + $(copy_matching "$root/netpunch" "$dest/netpunch" '*.log' '*.json' '*.jsonl')))
  {
    echo "mod folder: $(tpf2mp_tilde "$root") (the game under $kind Steam sees it as \$XDG_DATA_HOME/tpf2mp)"
    for f in "$root"/*.so "$root"/plugins/*.so "$root"/netpunch/netpunch; do
      [ -f "$f" ] && inventory_line "$f" "$root"
    done
    echo ""
    if [ -f "$root/tpf2mp_install.txt" ]; then
      echo "install manifest:"
      sed "s|$TPF2MP_REAL_HOME|~|g" "$root/tpf2mp_install.txt"
    else
      echo "no install manifest (not installed with install.sh)"
    fi
  } >"$dest/installed_files.txt" 2>/dev/null
  say "  mod folder ($kind Steam): $n file(s), $runs saved run(s)  ($(tpf2mp_tilde "$root"))"
done

# ---- 2. game folders -------------------------------------------------------------------
mapfile -t GAMES < <(tpf2mp_find_games)
[ ${#GAMES[@]} -gt 0 ] || { say "  game folder: NOT FOUND -- is Transport Fever 2 installed?"; note "== game folder: not found"; }
i=0
for row in "${GAMES[@]:+${GAMES[@]}}"; do
  IFS=$'\t' read -r kind steam g <<<"$row"
  i=$((i + 1)); dest=$STAGE/game$i
  note "== game folder $i ($kind Steam): $(tpf2mp_tilde "$g")"
  n=$(copy_matching "$g" "$dest" 'run.sh' 'run.sh.tpf2mp-orig' '*.log' '*.cfg' 'tpf2_menu_flags.txt')
  n=$((n + $(copy_matching "$g/netpunch" "$dest/netpunch" '*.log' '*.json' '*.jsonl')))
  bid=""
  if command -v readelf >/dev/null 2>&1; then
    bid=$(readelf -n "$g/TransportFever2" 2>/dev/null | sed -n 's/^[[:space:]]*Build ID: \([0-9a-f]*\).*/\1/p' | head -n 1)
  elif command -v file >/dev/null 2>&1; then
    bid=$(file -L "$g/TransportFever2" 2>/dev/null | sed -n 's/.*BuildID\[sha1\]=\([0-9a-f]*\).*/\1/p')
  fi
  {
    echo "game folder: $(tpf2mp_tilde "$g")"
    echo "steam: $kind, $(tpf2mp_tilde "$steam")"
    if [ -z "$bid" ]; then echo "TransportFever2 build-id: unreadable (no readelf or file)"
    elif [ "$bid" = "$TPF2MP_BUILD_ID" ]; then echo "TransportFever2 build-id: $bid -> Steam build 35924"
    else echo "TransportFever2 build-id: $bid -> unrecognised build"; fi
    echo "TransportFever2 size: $(stat -c %s "$g/TransportFever2") bytes, $(date -r "$g/TransportFever2" '+%Y-%m-%d %H:%M')"
    if grep -qxF "$TPF2MP_RUNSH_MARK" "$g/run.sh" 2>/dev/null; then echo "run.sh: preloads the loader (tpf2mp block present)"
    else echo "run.sh: no tpf2mp block (launch options, or not installed)"; fi
    if [ -f "$g/mods/mp_lockstep_1/mod.lua" ]; then
      echo "mods/mp_lockstep_1: present, $(find "$g/mods/mp_lockstep_1" -type f | wc -l) files"
    else
      echo "mods/mp_lockstep_1: missing"
    fi
    echo "mods folder: $(find "$g/mods" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort | paste -sd, -)"
  } >"$dest/installed_files.txt" 2>/dev/null
  say "  game folder $i:  $n file(s)  ($(tpf2mp_tilde "$g"))"
done

# ---- 3. game user folders: stdout, crash dumps, settings --------------------------------
j=0
while IFS= read -r steam; do
  for u in "$steam"/userdata/*/"$TPF2MP_APPID"/local; do
    [ -d "$u/crash_dump" ] || [ -f "$u/settings.lua" ] || continue
    j=$((j + 1)); dest=$STAGE/gameuser$j
    note "== game user folder $j: $(tpf2mp_tilde "$u")"
    n=$(copy_matching "$u/crash_dump" "$dest" 'stdout.txt' 'stdout_old.txt')
    n=$((n + $(copy_matching "$u" "$dest" 'settings.lua')))
    [ ! -f "$u/crash_dump/lockfile" ] || note "  crash_dump/lockfile is there: the game is running, or its last run ended without its own shutdown"
    all=0; taken=0
    while IFS= read -r d; do
      [ -n "$d" ] || continue
      if [ $taken -lt $MAX_DUMPS ]; then copy_file "$d" "$dest/crash_dump" && taken=$((taken + 1)); fi
    done < <(find "$u/crash_dump" -maxdepth 1 -type f -name '*.dmp' -mtime -"$DUMP_DAYS" -printf '%T@ %p\n' 2>/dev/null | sort -rn | cut -d' ' -f2-)
    all=$(find "$u/crash_dump" -maxdepth 1 -type f -name '*.dmp' 2>/dev/null | wc -l)
    note "  crash dumps: $all in the folder, the newest $taken of the last $DUMP_DAYS days copied"
    n=$((n + taken))
    mods=$(find "$u/mods" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort | paste -sd, -)
    [ -z "$mods" ] || echo "user mods folder: $mods" >"$dest/user_mods.txt"
    say "  game log folder $j: $n file(s)  ($(tpf2mp_tilde "$u"))"
  done
done < <(tpf2mp_steam_roots)
[ $j -gt 0 ] || { say "  game log folder: NOT FOUND (no crash_dump folder)"; note "== game user folder: not found"; }

# ---- 4. system summary -----------------------------------------------------------------
RUNNING=$(pgrep -xc TransportFever2 2>/dev/null || true)
{
  echo "collected: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "os:     $( (. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-?}") || echo '?')"
  echo "kernel: $(uname -srm)"
  echo "glibc:  $(getconf GNU_LIBC_VERSION 2>/dev/null || echo '?')"
  echo "desktop: ${XDG_CURRENT_DESKTOP:-?} (${XDG_SESSION_TYPE:-?})"
  echo "cpu:    $(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -n 1) ($(nproc) threads)"
  awk '/^MemTotal/ {t=$2} /^MemAvailable/ {a=$2} END {printf "ram:    %.1f GB total, %.1f GB available\n", t/1048576, a/1048576}' /proc/meminfo
  if command -v lspci >/dev/null 2>&1; then lspci 2>/dev/null | grep -Ei 'vga|3d controller|display' | sed 's/^/gpu:    /'; fi
  if command -v vulkaninfo >/dev/null 2>&1; then
    timeout 10 vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName|driverInfo|apiVersion' | sed 's/^[[:space:]]*/vulkan: /'
  fi
  echo "disk:   $(df -h "$HOME" 2>/dev/null | awk 'NR==2 {print $4 " free of " $2 " (home)"}')"
  while IFS= read -r s; do echo "steam:  $(tpf2mp_steam_kind "$s"), $(tpf2mp_tilde "$s")"; done < <(tpf2mp_steam_roots)
  if command -v snap >/dev/null 2>&1; then snap list steam 2>/dev/null | tail -n +2 | sed 's/^/snap:   /'; fi
  if command -v flatpak >/dev/null 2>&1; then flatpak info com.valvesoftware.Steam 2>/dev/null | grep -E 'Version|Commit' | sed 's/^[[:space:]]*/flatpak: /'; fi
  echo "game running while collecting: ${RUNNING:-0} instance(s)"
  echo "TPF2MP_DATADIR: ${TPF2MP_DATADIR:-}"
  echo "XDG_DATA_HOME (this shell): ${XDG_DATA_HOME:-}"
} 2>/dev/null | sed "s|$TPF2MP_REAL_HOME|~|g" >"$STAGE/system.txt"

# The lobby's message stream and state carry the invitation codes, and a code is
# enough to join: every string value of these keys is masked before packing
# (2026-09-26, as the in-game OPEN LOGS does).
MASK_KEYS='code|cross_code|steam|steam_code|steam_secret|password|pass|passcode|secret'
while IFS= read -r -d '' f; do
  sed -E -i "s/(\"($MASK_KEYS)\"[[:space:]]*:[[:space:]]*\")([^\"\\\\]|\\\\.)*\"/\\1<masked>\"/g" "$f" 2>/dev/null
done < <(find "$STAGE" -type f \( -name '*.json' -o -name '*.jsonl' -o -name '*.txt' \) -print0)
note "invitation codes in the lobby files are masked"

# ---- 5. the archive ----------------------------------------------------------------------
if [ -z "$OUT" ]; then
  OUT=$(xdg-user-dir DOWNLOAD 2>/dev/null || true)
  { [ -n "$OUT" ] && [ "$OUT" != "$HOME" ] && [ -d "$OUT" ]; } || OUT=$HOME/Downloads
  [ -d "$OUT" ] || OUT=$HOME
fi
mkdir -p "$OUT" || tpf2mp_die "cannot create $OUT"
if command -v zip >/dev/null 2>&1; then
  ARCHIVE=$OUT/$ARCHIVE_NAME.zip
  (cd "$WORK" && zip -qr "$ARCHIVE" "$ARCHIVE_NAME") || { say "Could not write $ARCHIVE"; trap - EXIT; say "The collected files are in $STAGE"; exit 1; }
else
  ARCHIVE=$OUT/$ARCHIVE_NAME.tar.gz
  tar -C "$WORK" -czf "$ARCHIVE" "$ARCHIVE_NAME" || { say "Could not write $ARCHIVE"; trap - EXIT; say "The collected files are in $STAGE"; exit 1; }
fi

say ""
say "Done: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
if [ "${RUNNING:-0}" -gt 0 ]; then
  say "Note: the game is still running, so its own log (stdout.txt) may be incomplete."
  say "      If you can, close the game and run this again. Starting the game wipes that"
  say "      log, but the mod saves the previous run's copy in tpf2mp/logs/ first."
fi
say ""
say "Send this file privately (DM, not a public channel): the lobby logs contain IP addresses."
