#!/usr/bin/env bash
# uninstall.sh -- removes TpF2 Multiplayer (Linux) again.
#
#   ./uninstall.sh [--game DIR] [--data-home DIR] [--purge] [--force] [--dry-run]
#
# Reads the manifest install.sh wrote (<data home>/tpf2mp/tpf2mp_install.txt,
# for every kind of Steam that has one), removes the files it lists and the mod
# folder, and gives run.sh back its original (run.sh.tpf2mp-orig). The data
# folder, the saved logs and the lobby's own files stay, as the Windows
# uninstaller leaves them, unless --purge removes the whole tpf2mp folder.
# The known library names go too, listed or not (an install from before the
# manifest), and run.sh is restored in every game folder whose run.sh has the
# block. It stops while Transport Fever 2 is running, unless --force: that game
# would miss the mod's scripts the next time it loads a save.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ -f "$HERE/tpf2mp_paths.sh" ] || { echo "error: tpf2mp_paths.sh must be beside uninstall.sh" >&2; exit 1; }
# shellcheck source=tpf2mp_paths.sh
. "$HERE/tpf2mp_paths.sh"

GAME="" DATA_HOME="" PURGE=0 FORCE=0
TPF2MP_DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --game) GAME=${2:?--game needs a folder}; shift 2 ;;
    --data-home) DATA_HOME=${2:?--data-home needs a folder}; shift 2 ;;
    --purge) PURGE=1; shift ;;
    --force) FORCE=1; shift ;;
    --dry-run) TPF2MP_DRY=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) tpf2mp_die "unknown option $1 (see --help)" ;;
  esac
done
export TPF2MP_DRY

if [ -n "$GAME" ]; then
  GAME=$(realpath "$GAME")
  [ -n "$DATA_HOME" ] || DATA_HOME=$(tpf2mp_game_data_home "$(tpf2mp_steam_kind "$GAME")")
fi
if [ -n "$DATA_HOME" ]; then
  case "$DATA_HOME" in /*) ;; *) tpf2mp_die "--data-home must be an absolute path" ;; esac
  DATA_HOME=$(realpath -m "$DATA_HOME")
fi

if [ "$(tpf2mp_games_running)" -gt 0 ]; then
  msg="Transport Fever 2 is running: close it before uninstalling"
  if [ "$TPF2MP_DRY" = 1 ]; then tpf2mp_warn "$msg (without --dry-run the uninstall stops here unless --force)"
  elif [ $FORCE = 1 ]; then tpf2mp_warn "$msg (--force: uninstalling anyway)"
  else tpf2mp_die "$msg, or pass --force"; fi
fi

do_rm() { if [ "$TPF2MP_DRY" = 1 ]; then tpf2mp_say "  would remove $(tpf2mp_tilde "$1")"; else rm -f "$1"; fi; }
do_rmtree() { if [ "$TPF2MP_DRY" = 1 ]; then tpf2mp_say "  would remove $(tpf2mp_tilde "$1")/"; else rm -rf "$1"; fi; }

ROOTS=()
if [ -n "$DATA_HOME" ]; then
  ROOTS+=("$DATA_HOME/tpf2mp")
else
  for k in native snap flatpak; do
    r=$(realpath -m "$(tpf2mp_game_data_home "$k")")/tpf2mp
    [ -f "$r/tpf2mp_install.txt" ] || [ -f "$r/libtpf2mp_boot.so" ] || continue
    case " ${ROOTS[*]:-} " in *" $r "*) continue ;; esac
    ROOTS+=("$r")
  done
fi

GAMES=()
[ -z "$GAME" ] || GAMES+=("$(realpath "$GAME")")
LAUNCH_OPTS=0
for ROOT in "${ROOTS[@]:+${ROOTS[@]}}"; do
  tpf2mp_say "Removing $(tpf2mp_tilde "$ROOT")..."
  M=$ROOT/tpf2mp_install.txt
  declare -A LISTED=()
  if [ -f "$M" ]; then
    while IFS=$'\t' read -r kind path; do
      case "$kind" in
        file)   # only what lies inside this tpf2mp folder
          case "/$path/" in */../*|*/./*) tpf2mp_warn "ignored invalid manifest path: $path"; continue ;; esac
          case "$path" in
            "$ROOT"/*) LISTED[$path]=1; do_rm "$path" ;;
            *) tpf2mp_warn "ignored manifest entry outside $ROOT: $path" ;;
          esac ;;
        moddir)
          if [ -f "$path/mod.lua" ] && [ "$(basename "$path")" = mp_lockstep_1 ]; then do_rmtree "$path"; fi ;;
        game) if [ -z "$GAME" ] || [ "$path" = "$GAME" ]; then GAMES+=("$path"); fi ;;
        loading) [ "$path" != launch-options ] || LAUNCH_OPTS=1 ;;
      esac
    done <"$M"
    do_rm "$M"
  fi
  # and the known names a manifest does not list: boot.cpp would load a leftover
  for n in libtpf2mp_boot.so tpf2_bridge_mp.so tpf2_menu.so tpf2_slice.so tpf2_pluginhost.so plugins/tpf2_previews.so tpf2mp-launch netpunch/netpunch; do
    [ -z "${LISTED[$ROOT/$n]:-}" ] || continue
    [ ! -e "$ROOT/$n" ] || do_rm "$ROOT/$n"
  done
  if [ "$TPF2MP_DRY" = 0 ]; then
    rmdir "$ROOT/netpunch" "$ROOT/plugins" 2>/dev/null || true
  fi
  if [ $PURGE = 1 ]; then do_rmtree "$ROOT"; fi
done

# run.sh: the games named above, and any other game whose run.sh has the block
if [ -z "$GAME" ] && [ -z "$DATA_HOME" ]; then
  while IFS=$'\t' read -r _ _ g; do
    if grep -qxF "$TPF2MP_RUNSH_MARK" "$g/run.sh" 2>/dev/null; then GAMES+=("$g"); fi
  done < <(tpf2mp_find_games)
fi
DONE=$'\n'
for g in "${GAMES[@]:+${GAMES[@]}}"; do
  case "$DONE" in *$'\n'"$g"$'\n'*) continue ;; esac
  DONE=$DONE$g$'\n'
  tpf2mp_say "Game folder $(tpf2mp_tilde "$g"):"
  if [ -n "$GAME" ] && [ "$g" = "$(realpath "$GAME")" ] && [ -f "$g/mods/mp_lockstep_1/mod.lua" ]; then
    do_rmtree "$g/mods/mp_lockstep_1"
  fi
  tpf2mp_runsh_restore "$g" || true
done

[ ${#ROOTS[@]} -gt 0 ] || [ ${#GAMES[@]} -gt 0 ] || tpf2mp_say "Nothing of TpF2 Multiplayer was found."
if [ $LAUNCH_OPTS = 1 ]; then
  tpf2mp_say "Also clear the tpf2mp-launch (or older LD_PRELOAD) entry from the game's Steam launch options."
fi
tpf2mp_say "Done."
