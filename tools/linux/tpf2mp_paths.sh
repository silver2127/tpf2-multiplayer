# tpf2mp_paths.sh -- sourced by install.sh, uninstall.sh and collect_logs.sh:
# where Steam, the game and the mod's folders are on this machine, and the
# run.sh block that makes the game preload the mod's loader.
#
# Linux has three kinds of Steam, and each gives the game its own $HOME:
#   native   ~/.local/share/Steam (also behind ~/.steam/root, ~/.steam/steam,
#            ~/.steam/debian-installation). The game gets the session's HOME and
#            XDG_DATA_HOME.
#   snap     ~/snap/steam/common/.local/share/Steam. The snap runs Steam, and the
#            game under it, with HOME=~/snap/steam/common and
#            XDG_DATA_HOME=~/snap/steam/common/.local/share (read from the
#            environment of a running Snap Steam client, snap revision 271).
#   flatpak  ~/.var/app/com.valvesoftware.Steam/.local/share/Steam. INFERRED, not
#            tested: Flatpak sets XDG_DATA_HOME=~/.var/app/<app id>/data.
# The mod's folder is $XDG_DATA_HOME/tpf2mp AS THE GAME SEES IT (datadir_linux.h,
# lockstep.lua), so it follows the kind of Steam that runs the game, not the
# environment of the shell running these scripts.
# shellcheck shell=bash

TPF2MP_APPID=1066780
TPF2MP_REAL_HOME=${SNAP_REAL_HOME:-$HOME}
# shellcheck disable=SC2034  # read by the scripts that source this file
TPF2MP_BUILD_ID=3a0e156390b0e6f1e372051c24802c8493ae454a   # GNU build-id of Steam Linux build 35924

tpf2mp_say() { printf '%s\n' "$*"; }
tpf2mp_warn() { printf 'warning: %s\n' "$*" >&2; }
tpf2mp_die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ~ instead of the home folder, for text that may be shared.
tpf2mp_tilde() { local p=$1; case "$p" in "$TPF2MP_REAL_HOME"/*) p="~${p#"$TPF2MP_REAL_HOME"}" ;; esac; printf '%s\n' "$p"; }

# snap | flatpak | native, from the real path of a Steam folder.
tpf2mp_steam_kind() {
  case "$1/" in
    "$(realpath -m "$TPF2MP_REAL_HOME/snap/steam")"/*) echo snap ;;
    "$(realpath -m "$TPF2MP_REAL_HOME/.var/app/com.valvesoftware.Steam")"/*) echo flatpak ;;
    *) echo native ;;
  esac
}

# Every Steam folder on this machine, as real paths, one per line.
tpf2mp_steam_roots() {
  local c r seen=$'\n' xdg=""
  case "${XDG_DATA_HOME:-}" in /*) [ -z "${SNAP_REAL_HOME:-}" ] && xdg=$XDG_DATA_HOME/Steam ;; esac
  for c in "$TPF2MP_REAL_HOME/.local/share/Steam" "$xdg" \
           "$TPF2MP_REAL_HOME/.steam/root" "$TPF2MP_REAL_HOME/.steam/steam" \
           "$TPF2MP_REAL_HOME/.steam/debian-installation" \
           "$TPF2MP_REAL_HOME/snap/steam/common/.local/share/Steam" \
           "$TPF2MP_REAL_HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
           "$TPF2MP_REAL_HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
    [ -n "$c" ] || continue
    [ -d "$c/steamapps" ] || [ -d "$c/userdata" ] || continue
    r=$(realpath "$c") || continue
    case "$seen" in *$'\n'"$r"$'\n'*) continue ;; esac
    seen=$seen$r$'\n'
    printf '%s\n' "$r"
  done
}

# The Steam folder itself and every library its libraryfolders.vdf lists.
tpf2mp_library_paths() {
  local vdf
  printf '%s\n' "$1"
  for vdf in "$1/steamapps/libraryfolders.vdf" "$1/config/libraryfolders.vdf"; do
    [ -f "$vdf" ] || continue
    sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | sed 's/\\\\/\\/g'
  done
}

# "kind<TAB>steam folder<TAB>game folder" for every Transport Fever 2 install.
tpf2mp_find_games() {
  local root lib inst acf dir seen=$'\n'
  while IFS= read -r root; do
    while IFS= read -r lib; do
      [ -d "$lib/steamapps" ] || continue
      # A library without the app has no manifest. The callers run under
      # set -e -o pipefail, where a failing read here would end the whole scan.
      inst="" acf=$lib/steamapps/appmanifest_$TPF2MP_APPID.acf
      if [ -f "$acf" ]; then
        inst=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$acf" | head -n 1) || inst=""
      fi
      dir=$lib/steamapps/common/${inst:-Transport Fever 2}
      [ -f "$dir/TransportFever2" ] || continue
      dir=$(realpath "$dir") || continue
      case "$seen" in *$'\n'"$dir"$'\n'*) continue ;; esac
      seen=$seen$dir$'\n'
      printf '%s\t%s\t%s\n' "$(tpf2mp_steam_kind "$root")" "$root" "$dir"
    done < <(tpf2mp_library_paths "$root")
  done < <(tpf2mp_steam_roots)
}

# Modification time of a Steam folder's newest crash_dump/stdout.txt (0: none):
# the Steam that ran the game last.
tpf2mp_last_run() {
  local f t best=0
  for f in "$1"/userdata/*/"$TPF2MP_APPID"/local/crash_dump/stdout.txt; do
    [ -f "$f" ] || continue
    t=$(stat -c %Y "$f") && [ "$t" -gt "$best" ] && best=$t
  done
  echo "$best"
}

# The XDG_DATA_HOME the game resolves its folders with, for a kind of Steam.
tpf2mp_game_data_home() {
  case "$1" in
    snap) printf '%s\n' "$TPF2MP_REAL_HOME/snap/steam/common/.local/share" ;;
    flatpak) printf '%s\n' "$TPF2MP_REAL_HOME/.var/app/com.valvesoftware.Steam/data" ;;
    *) case "${XDG_DATA_HOME:-}" in
         /*) if [ -z "${SNAP_REAL_HOME:-}" ]; then printf '%s\n' "$XDG_DATA_HOME"; return; fi ;;
       esac
       printf '%s\n' "$TPF2MP_REAL_HOME/.local/share" ;;
  esac
}

# Every tpf2mp folder that exists, as "label<TAB>path".
tpf2mp_mod_roots() {
  local k p seen=$'\n'
  for k in native snap flatpak; do
    p=$(tpf2mp_game_data_home "$k")/tpf2mp
    [ -d "$p" ] || continue
    p=$(realpath "$p") || continue
    case "$seen" in *$'\n'"$p"$'\n'*) continue ;; esac
    seen=$seen$p$'\n'
    printf '%s\t%s\n' "$k" "$p"
  done
}

# How many TransportFever2 processes run (0: none), whoever runs them: a shared
# Steam library is one game folder. pgrep matches the 15-character process name
# exactly; without procps, /proc/<pid>/comm (readable for every process) is read.
tpf2mp_games_running() {
  local n=0 c
  if command -v pgrep >/dev/null 2>&1; then
    n=$(pgrep -x TransportFever2 2>/dev/null | wc -l) || n=0
  else
    for c in /proc/[0-9]*/comm; do
      if [ "$(cat "$c" 2>/dev/null)" = TransportFever2 ]; then n=$((n + 1)); fi
    done
  fi
  echo $((n))
}

# Replaces dst with a copy of src by a rename inside dst's folder: a running game
# keeps the libraries it has mapped (a copy over a mapped .so crashes it) and a
# running run.sh keeps reading the old file. mode: octal, or "keep" (dst's own).
# With TPF2MP_DRY=1 it only says what it would do.
tpf2mp_put() {
  local src=$1 dst=$2 mode=$3 tmp
  if [ "${TPF2MP_DRY:-0}" = 1 ]; then tpf2mp_say "  would write $(tpf2mp_tilde "$dst")"; return 0; fi
  mkdir -p "$(dirname "$dst")" || return 1
  tmp=$(dirname "$dst")/.$(basename "$dst").tpf2mp-new.$$
  cp -f "$src" "$tmp" || { rm -f "$tmp"; return 1; }
  if [ "$mode" != keep ]; then chmod "$mode" "$tmp"
  elif [ -e "$dst" ]; then chmod --reference="$dst" "$tmp"; fi
  mv -f "$tmp" "$dst"
}

# ---- run.sh ----------------------------------------------------------------
# Steam starts the game through run.sh in the game folder. The block below goes
# after its first lines and preloads the loader when it is installed; the loader
# resolves everything else itself (boot.cpp). The path follows datadir_linux.h:
# $XDG_DATA_HOME when it is absolute, else $HOME/.local/share. The first,
# hand-made patch (TPF2MP_RUNSH_LEGACY) used $HOME only; it is recognised and
# replaced. LD_PRELOAD splits on spaces and colons, so the installer refuses a
# folder whose path has either.
TPF2MP_RUNSH_MARK='# tpf2mp (Linux port): preload the multiplayer loader when it is installed.'
# shellcheck disable=SC2016  # the $ are for run.sh, not for this shell
TPF2MP_RUNSH_BLOCK='# tpf2mp (Linux port): preload the multiplayer loader when it is installed.
# Undo: run uninstall.sh, or restore run.sh.tpf2mp-orig beside this file (or verify the game files in Steam).
TPF2MP_BOOT="$HOME/.local/share/tpf2mp/libtpf2mp_boot.so"
case "$XDG_DATA_HOME" in /*) TPF2MP_BOOT="$XDG_DATA_HOME/tpf2mp/libtpf2mp_boot.so" ;; esac
if [ -f "$TPF2MP_BOOT" ]; then
  export LD_PRELOAD="$TPF2MP_BOOT${LD_PRELOAD:+:$LD_PRELOAD}"
fi
# end of the tpf2mp block'
# shellcheck disable=SC2016
TPF2MP_RUNSH_LEGACY='# tpf2mp (Linux port): preload the multiplayer loader when it is installed.
# Undo: restore run.sh.tpf2mp-orig beside this file (or verify the game files in Steam).
TPF2MP_BOOT="$HOME/.local/share/tpf2mp/libtpf2mp_boot.so"
if [ -f "$TPF2MP_BOOT" ]; then
  export LD_PRELOAD="$TPF2MP_BOOT${LD_PRELOAD:+:$LD_PRELOAD}"
fi'

# 1 when the file $1 is not empty and its last byte is not a newline, else 0.
# awk ends every line it prints with one; strip and patch leave it off again.
tpf2mp_no_final_newline() {
  if [ -s "$1" ] && [ "$(tail -c 1 "$1" | od -An -tx1 | tr -d ' \n')" != 0a ]; then echo 1; else echo 0; fi
}

# Prints run.sh without our block and the blank line after it: byte for byte
# the file tpf2mp_runsh_patch was given, in every layout.
# Returns 0 when a block was removed, 1 when there was none, 2 when the marker
# is there but the lines around it are not one of our blocks (edited by hand;
# nothing is printed then).
tpf2mp_runsh_strip() {
  awk -v mark="$TPF2MP_RUNSH_MARK" -v cur="$TPF2MP_RUNSH_BLOCK" -v old="$TPF2MP_RUNSH_LEGACY" \
      -v nonl="$(tpf2mp_no_final_newline "$1")" '
    { line[NR] = $0 }
    function matches(start, text,   b, n, i) {
      n = split(text, b, "\n")
      for (i = 1; i <= n; i++) if ((start + i - 1) > NR || line[start + i - 1] != b[i]) return 0
      return n
    }
    END {
      found = 0; m = 0
      for (i = 1; i <= NR; i++) {
        if (line[i] == mark) {
          k = matches(i, cur); if (!k) k = matches(i, old)
          if (!k) exit 2
          i += k - 1
          if (i + 1 <= NR && line[i + 1] == "") i++
          found = 1
          continue
        }
        out[++m] = line[i]
      }
      for (i = 1; i <= m; i++) { sep = (i < m || nonl != 1) ? "\n" : ""; printf "%s%s", out[i], sep }
      exit found ? 0 : 1
    }' "$1"
}

# Prints the stock run.sh given as $1 with our block and one blank line inserted
# after its shebang line, or after the blank line that follows the shebang when
# there is one (Steam's run.sh). Only those lines are added, so
# tpf2mp_runsh_strip gives the stock bytes back. Returns 1 without a shebang.
tpf2mp_runsh_patch() {
  awk -v block="$TPF2MP_RUNSH_BLOCK" -v nonl="$(tpf2mp_no_final_newline "$1")" '
    NR == 1 { if (substr($0, 1, 2) != "#!") { bad = 1; exit 1 } out[++m] = $0; next }
    NR == 2 { if ($0 == "") { out[++m] = $0; out[++m] = block; out[++m] = ""; next }
              out[++m] = block; out[++m] = "" }
    { out[++m] = $0 }
    END {
      if (bad || NR == 0) exit 1
      if (NR == 1) out[++m] = block
      for (i = 1; i <= m; i++) { sep = (i < m || nonl != 1) ? "\n" : ""; printf "%s%s", out[i], sep }
    }' "$1"
}

# Takes our block out of <game>/run.sh again: back to run.sh.tpf2mp-orig when
# that is exactly the file the block went into, else run.sh without the block (a
# backup that differs is left in place and named). Returns 1, changing nothing,
# for a block edited by hand.
tpf2mp_runsh_restore() {
  local rs=$1/run.sh orig=$1/run.sh.tpf2mp-orig tmp rc=0
  [ -f "$rs" ] || return 0
  tmp=$(mktemp) || return 1
  tpf2mp_runsh_strip "$rs" >"$tmp" || rc=$?
  case $rc in
    0)
      if [ -f "$orig" ] && cmp -s "$tmp" "$orig"; then
        if [ "${TPF2MP_DRY:-0}" = 1 ]; then tpf2mp_say "  would restore run.sh from run.sh.tpf2mp-orig"
        else mv -f "$orig" "$rs" && tpf2mp_say "run.sh restored from run.sh.tpf2mp-orig"; fi
      else
        tpf2mp_put "$tmp" "$rs" keep && tpf2mp_say "run.sh: the tpf2mp block is removed"
        [ ! -f "$orig" ] || tpf2mp_warn "run.sh.tpf2mp-orig differs from run.sh without the block; it was left in place"
      fi ;;
    1)
      if [ -f "$orig" ] && cmp -s "$rs" "$orig"; then
        if [ "${TPF2MP_DRY:-0}" = 1 ]; then tpf2mp_say "  would remove the unused run.sh.tpf2mp-orig"; else rm -f "$orig"; fi
      elif [ -f "$orig" ]; then
        tpf2mp_warn "run.sh has no tpf2mp block but run.sh.tpf2mp-orig differs from it; the backup was left in place"
      fi ;;
    *)
      rm -f "$tmp"
      tpf2mp_warn "run.sh has a tpf2mp block edited by hand: restore run.sh.tpf2mp-orig yourself, or verify the game files in Steam"
      return 1 ;;
  esac
  rm -f "$tmp"
}
