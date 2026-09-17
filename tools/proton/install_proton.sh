#!/usr/bin/env bash
# TpF2 Multiplayer -- install into the Windows game under Steam Proton (Linux, Steam Deck).
#
# The same install as tools/proton/install.py, without Python: finds Steam, the game
# and the Proton prefix, checks the game is Steam build 35924, downloads the release's
# TpF2Multiplayer-files.zip and checks it against SHA256SUMS.txt, keeps the game's own
# alut.dll as alut_real.dll, installs the files, links the prefix's Steam folders to the
# real ones, and writes a manifest. Replaced files go to .tpf2mp-proton-backups/.
#
#   sh install_proton.sh                 install the release this copy came from (or the latest)
#   sh install_proton.sh --version 0.6   a specific release
#   sh install_proton.sh --files-zip TpF2Multiplayer-files.zip   from a downloaded archive
#   sh install_proton.sh --dry-run       show the plan, change nothing
#   sh install_proton.sh --uninstall     remove the mod, put the game's own alut.dll back
#   --steam-root DIR, --game-dir DIR, --prefix DIR override the detection; --no-links skips
#   the prefix links (when you manage the prefix yourself).
#
# Needs: bash, curl, sha256sum, od, and one of unzip / bsdtar / python3 (to extract).
set -eu

REPO="silver2127/tpf2-multiplayer"
APP_ID="1066780"
GAME_FOLDER="Transport Fever 2"
FILES_ASSET="TpF2Multiplayer-files.zip"
SUMS_ASSET="SHA256SUMS.txt"
DEFAULT_VERSION=""            # stamped with the release version on the copy attached to a release
BUILD_TIMESTAMP=0x675abcc6    # Steam build 35924: PE TimeDateStamp
BUILD_IMAGE_SIZE=0x46ce000    # and SizeOfImage (compared as printf %#x prints them)
STOCK_ALUT_SHA256="${TPF2MP_STOCK_ALUT_SHA256:-3df103ae3d94a6b90c4d2a6d75dcb388cd835f5e3af9962b22c20d4473cfc035}"
MOD="mods/mp_lockstep_1"
KEEP_IF_PRESENT="tpf2_slice.cfg tpf2mp.cfg"
MANIFEST=".tpf2mp-proton-manifest.json"
BACKUPS=".tpf2mp-proton-backups"

version="$DEFAULT_VERSION"; files_zip=""; steam_root="${STEAM_ROOT:-}"; game_dir=""; prefix=""
dry_run=0; uninstall=0; no_links=0
while [ $# -gt 0 ]; do
  case "$1" in
    --version) version="$2"; shift 2 ;;
    --files-zip) files_zip="$2"; shift 2 ;;
    --steam-root) steam_root="$2"; shift 2 ;;
    --game-dir) game_dir="$2"; shift 2 ;;
    --prefix) prefix="$2"; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    --uninstall) uninstall=1; shift ;;
    --no-links) no_links=1; shift ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
done

say()  { printf '%s\n' "$*"; }
fail() { printf 'install_proton: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "'$1' is needed and not installed"; }
need sha256sum; need od
sha() { sha256sum "$1" | sed 's/^\\//' | cut -c1-64; }   # GNU prefixes a '\' when the path has a backslash
u32() { od -An -tu4 -j "$2" -N4 "$1" | tr -d ' '; }     # little-endian u32 at offset
u16() { od -An -tu2 -j "$2" -N2 "$1" | tr -d ' '; }

# ---- find Steam, the library, the game, the prefix --------------------------------
find_steam() {
  for r in "$steam_root" "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
           "$HOME/snap/steam/common/.local/share/Steam" \
           "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
           "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
    [ -n "$r" ] && [ -d "$r/steamapps" ] && { printf '%s\n' "$r"; return 0; }
  done
  return 1
}
library_of() {               # the Steam library that holds the game
  local s="$1" lib
  { printf '%s\n' "$s"; grep -o '"path"[[:space:]]*"[^"]*"' "$s/steamapps/libraryfolders.vdf" 2>/dev/null \
      | sed 's/.*"path"[[:space:]]*"\(.*\)"/\1/; s/\\\\/\\/g'; } | while IFS= read -r lib; do
    [ -f "$lib/steamapps/appmanifest_$APP_ID.acf" ] && { printf '%s\n' "$lib"; break; }
  done
}
if [ -n "$steam_root" ]; then [ -d "$steam_root/steamapps" ] || fail "--steam-root has no steamapps folder: $steam_root"; fi
steam="$(find_steam || true)"
library=""; [ -n "$steam" ] && library="$(library_of "$steam")"
if [ -z "$game_dir" ]; then
  [ -n "$library" ] || fail "Transport Fever 2 was not found in any Steam library. Install it in Steam, or pass --game-dir."
  game_dir="$library/steamapps/common/$GAME_FOLDER"
fi
[ -n "$steam" ] || fail "Steam was not found. Pass --steam-root (the folder with steamapps and userdata)."
if [ -z "$prefix" ]; then
  if [ -n "$library" ] && [ "$game_dir" = "$library/steamapps/common/$GAME_FOLDER" ]; then prefix="$library/steamapps/compatdata/$APP_ID/pfx"
  else prefix="$(dirname "$(dirname "$game_dir")")/compatdata/$APP_ID/pfx"; fi
fi
game_dir="${game_dir%/}"; prefix="${prefix%/}"; steam="${steam%/}"
say "Steam:  $steam"; say "Game:   $game_dir"; say "Prefix: $prefix"

# ---- the game: Windows build 35924, not running ----------------------------------
exe="$game_dir/TransportFever2.exe"
if [ ! -f "$exe" ]; then
  [ -f "$game_dir/TransportFever2" ] && fail "$game_dir holds the native Linux game, not the Windows one. In Steam: Properties > Compatibility > force Proton, let Steam download the Windows build, then run this again."
  fail "TransportFever2.exe is missing in $game_dir"
fi
[ "$(u16 "$exe" 0)" = "23117" ] || fail "$exe is not a Windows executable"           # 'MZ'
pe=$(u32 "$exe" 60)
[ "$(u32 "$exe" "$pe")" = "17744" ] || fail "$exe has no PE header"                     # 'PE\0\0'
[ "$(u16 "$exe" $((pe + 4)))" = "34404" ] || fail "$exe is not an x64 executable"       # 0x8664
stamp=$(u32 "$exe" $((pe + 8))); image=$(u32 "$exe" $((pe + 80)))
[ "$(printf '%#x' "$stamp")" = "$BUILD_TIMESTAMP" ] && [ "$(printf '%#x' "$image")" = "$BUILD_IMAGE_SIZE" ] \
  || fail "$exe is not Steam build 35924 (stamp $(printf '%#x' "$stamp"), image $(printf '%#x' "$image")); the mod supports that build only"
if command -v pgrep >/dev/null 2>&1 && pgrep -f 'TransportFever2\.exe' >/dev/null 2>&1; then
  fail "Transport Fever 2 is running. Close it and run this again; nothing was changed"
fi

# ---- uninstall ----------------------------------------------------------------------
if [ "$uninstall" = 1 ]; then
  [ -f "$game_dir/$MANIFEST" ] || fail "no $MANIFEST in $game_dir: nothing this script installed"
  say "Uninstalling TpF2 Multiplayer from $game_dir"
  if [ -f "$game_dir/alut_real.dll" ]; then
    [ "$(sha "$game_dir/alut_real.dll")" = "$STOCK_ALUT_SHA256" ] || fail "alut_real.dll is not the game's own alut.dll; verify the game files in Steam instead"
    [ "$dry_run" = 1 ] || mv -f "$game_dir/alut_real.dll" "$game_dir/alut.dll"
    say "  the game's own alut.dll is back"
  fi
  grep -o '"[^"]*"[[:space:]]*:[[:space:]]*"[0-9a-f]\{64\}"' "$game_dir/$MANIFEST" | sed 's/^"\([^"]*\)".*/\1/' | while IFS= read -r rel; do
    [ "$rel" = "alut.dll" ] && continue
    if [ -f "$game_dir/$rel" ]; then say "  remove $rel"; [ "$dry_run" = 1 ] || rm -f "$game_dir/$rel"; fi
  done
  if [ "$dry_run" != 1 ]; then
    rm -rf "$game_dir/$MOD"; rmdir "$game_dir/netpunch" 2>/dev/null || true; rm -f "$game_dir/$MANIFEST"
  fi
  say "Done. The prefix links and $BACKUPS/ are left in place."
  exit 0
fi

# ---- the game's own alut.dll -------------------------------------------------------
alut_mode="rename"
if [ -f "$game_dir/alut_real.dll" ]; then
  [ "$(sha "$game_dir/alut_real.dll")" = "$STOCK_ALUT_SHA256" ] || fail "$game_dir/alut_real.dll is not the game's own alut.dll. Verify the game files in Steam, delete it, and run this again"
  alut_mode="keep"
else
  [ -f "$game_dir/alut.dll" ] || fail "$game_dir/alut.dll is missing; verify the game files in Steam"
  [ "$(sha "$game_dir/alut.dll")" = "$STOCK_ALUT_SHA256" ] || fail "$game_dir/alut.dll is not the game's own file: another mod or tool has replaced it, and two replacements cannot both work. Remove that mod (or verify the game files in Steam), then run this again"
fi

# ---- the payload ---------------------------------------------------------------------
work="$(mktemp -d "${TMPDIR:-/tmp}/tpf2mp-install.XXXXXX")"
trap 'rm -rf "$work"' EXIT
if [ -z "$files_zip" ]; then
  need curl
  if [ -n "$version" ]; then base="https://github.com/$REPO/releases/download/v$version"; tag="v$version"
  else base="https://github.com/$REPO/releases/latest/download"; tag="latest"; fi
  cache="${XDG_CACHE_HOME:-$HOME/.cache}/tpf2mp/$tag"; mkdir -p "$cache"
  say "Downloading $FILES_ASSET ($tag)..."
  curl -fsSL --retry 3 -o "$cache/$SUMS_ASSET" "$base/$SUMS_ASSET" || fail "could not download $base/$SUMS_ASSET (no such release, or no network)"
  if [ -t 1 ]; then progress="--progress-bar"; else progress="-sS"; fi     # a bar on a terminal, silence in a log
  curl -fL --retry 3 $progress -o "$cache/$FILES_ASSET" "$base/$FILES_ASSET" || fail "could not download $base/$FILES_ASSET"
  want="$(grep " $FILES_ASSET\$" "$cache/$SUMS_ASSET" | cut -c1-64)"
  [ -n "$want" ] || fail "$SUMS_ASSET does not list $FILES_ASSET"
  [ "$(sha "$cache/$FILES_ASSET")" = "$want" ] || fail "$FILES_ASSET does not match $SUMS_ASSET (a broken download?); delete $cache and try again"
  say "  checksum ok"
  files_zip="$cache/$FILES_ASSET"
else
  [ -f "$files_zip" ] || fail "no such file: $files_zip"
  tag="$(basename "$files_zip")"
fi
payload="$work/payload"; mkdir -p "$payload"
if command -v unzip >/dev/null 2>&1; then unzip -q "$files_zip" -d "$payload"
elif command -v bsdtar >/dev/null 2>&1; then bsdtar -xf "$files_zip" -C "$payload"
elif command -v python3 >/dev/null 2>&1; then python3 -m zipfile -e "$files_zip" "$payload"
elif command -v python >/dev/null 2>&1; then python -m zipfile -e "$files_zip" "$payload"
else fail "nothing to extract a zip with: install unzip (or bsdtar)"; fi
for r in alut.dll tpf2_pluginhost.dll tpf2_bridge_mp.dll tpf2_menu.dll tpf2_slice.dll netpunch/netpunch.exe \
         "$MOD/mod.lua" "$MOD/res/config/game_script/lockstep.lua"; do
  [ -f "$payload/$r" ] || fail "the archive lacks $r: not a TpF2 Multiplayer files archive"
done
shipped_version="$(tr -d '\r\n' < "$payload/tpf2mp_version.txt" 2>/dev/null || echo "?")"
say "Payload: TpF2 Multiplayer $shipped_version"

# ---- the plan --------------------------------------------------------------------------
copy=(); remove=()
while IFS= read -r rel; do
  keep=0; for k in $KEEP_IF_PRESENT; do [ "$rel" = "$k" ] && [ -e "$game_dir/$rel" ] && keep=1; done
  [ "$keep" = 1 ] && continue
  if [ -L "$game_dir/$rel" ] || { [ -e "$game_dir/$rel" ] && [ ! -f "$game_dir/$rel" ]; }; then fail "$game_dir/$rel is not an ordinary file"; fi
  if [ ! -f "$game_dir/$rel" ] || [ "$(sha "$game_dir/$rel")" != "$(sha "$payload/$rel")" ]; then copy+=("$rel"); fi
done < <(cd "$payload" && find . -type f | sed 's|^\./||' | grep -v '^alut\.dll$' | sort; echo alut.dll)   # the proxy last
if [ -d "$game_dir/$MOD" ]; then
  while IFS= read -r rel; do [ -e "$payload/$rel" ] || remove+=("$rel"); done \
    < <(cd "$game_dir" && find "$MOD" -type f -o -type l | sort)
fi
links=()
if [ "$no_links" != 1 ]; then
  winsteam="$prefix/drive_c/Program Files (x86)/Steam"
  for pair in "$winsteam/userdata|$steam/userdata" "$winsteam/steamapps/common|$(dirname "$game_dir")" \
              "$winsteam/steamapps/workshop|$(dirname "$(dirname "$game_dir")")/workshop"; do
    link="${pair%%|*}"; target="${pair#*|}"
    if [ -L "$link" ]; then
      [ "$(readlink "$link")" = "$target" ] || fail "Proton prefix: $link points at $(readlink "$link"), not $target. Move it aside and run this again"
    elif [ -e "$link" ]; then
      [ -d "$link" ] && [ -z "$(ls -A "$link")" ] || fail "Proton prefix: $link exists and is not an empty folder. Move it aside and run this again"
      links+=("$pair")
    else links+=("$pair"); fi
  done
fi
say "Plan: copy ${#copy[@]} file(s), remove ${#remove[@]} stale file(s), ${#links[@]} prefix link(s)$( [ "$alut_mode" = rename ] && printf '%s' "; the game's alut.dll is kept as alut_real.dll")"
for rel in "${copy[@]}";   do say "  copy   $rel"; done
for rel in "${remove[@]}"; do say "  remove $rel"; done
for pair in "${links[@]}"; do say "  link   ${pair%%|*} -> ${pair#*|}"; done
if [ "$dry_run" = 1 ]; then say "Dry run: nothing changed."; exit 0; fi
if [ "${#copy[@]}" = 0 ] && [ "${#remove[@]}" = 0 ] && [ "${#links[@]}" = 0 ]; then say "Already installed and current."; exit 0; fi

# ---- apply -------------------------------------------------------------------------------
backup=""
backup_of() {   # keep a replaced file
  local rel="$1"
  [ "$rel" = "alut.dll" ] && [ "$alut_mode" = "rename" ] && return 0
  [ -e "$game_dir/$rel" ] || return 0
  if [ -z "$backup" ]; then backup="$game_dir/$BACKUPS/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$backup"; fi
  mkdir -p "$backup/$(dirname "$rel")"; cp -p "$game_dir/$rel" "$backup/$rel"
}
if [ "$alut_mode" = "rename" ]; then cp -p "$game_dir/alut.dll" "$game_dir/alut_real.dll.tmp" && mv -f "$game_dir/alut_real.dll.tmp" "$game_dir/alut_real.dll"; fi
for rel in "${remove[@]}"; do backup_of "$rel"; rm -f "$game_dir/$rel"; done
for rel in "${copy[@]}"; do
  backup_of "$rel"; mkdir -p "$game_dir/$(dirname "$rel")"
  cp -p "$payload/$rel" "$game_dir/$rel.tmp" && mv -f "$game_dir/$rel.tmp" "$game_dir/$rel"
done
for pair in "${links[@]}"; do
  link="${pair%%|*}"; target="${pair#*|}"
  mkdir -p "$(dirname "$link")"; [ -d "$link" ] && rmdir "$link"; ln -s "$target" "$link"
done
{
  printf '{\n  "product": "TpF2 Multiplayer",\n  "version": "%s",\n  "release": "%s",\n  "installed": "%s",\n  "installer": "install_proton.sh",\n  "files": {\n' \
    "$shipped_version" "$tag" "$(date +%Y-%m-%dT%H:%M:%S)"
  first=1
  while IFS= read -r rel; do
    [ -f "$game_dir/$rel" ] || continue
    [ "$first" = 1 ] || printf ',\n'; first=0
    printf '    "%s": "%s"' "$rel" "$(sha "$game_dir/$rel")"
  done < <(cd "$payload" && find . -type f | sed 's|^\./||' | sort)
  printf '\n  },\n  "alut_real": "%s"\n}\n' "$(sha "$game_dir/alut_real.dll")"
} > "$game_dir/$MANIFEST.tmp" && mv -f "$game_dir/$MANIFEST.tmp" "$game_dir/$MANIFEST"
say "Installed TpF2 Multiplayer $shipped_version into $game_dir$( [ -n "$backup" ] && printf '%s' " (replaced files kept in $backup)")"
say "Start the game: Main menu > Multiplayer. Everyone in a session needs the same version."
