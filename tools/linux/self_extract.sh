#!/usr/bin/env bash
# Self-extracting Linux installer; build_release.sh appends the release tarball.
set -euo pipefail
line=$(awk '/^__TPF2MP_ARCHIVE_BELOW__$/ { print NR + 1; exit }' "$0")
[ -n "$line" ] || { echo 'Installer payload is missing.' >&2; exit 1; }
temp=$(mktemp -d "${TMPDIR:-/tmp}/tpf2mp-install.XXXXXXXX")
trap 'rm -rf "$temp"' EXIT
tail -n "+$line" "$0" | tar -xz -C "$temp"
shopt -s nullglob
release=("$temp"/tpf2mp-linux-*)
[ ${#release[@]} -eq 1 ] && [ -f "${release[0]}/install.sh" ] || {
  echo 'Installer payload has an invalid layout.' >&2; exit 1;
}
(cd "${release[0]}" && sha256sum -c --quiet SHA256SUMS)
if [ "${1:-}" = --extract ]; then
  [ $# -eq 2 ] || { echo 'Usage: installer.run --extract NEW_DIRECTORY' >&2; exit 2; }
  [ ! -e "$2" ] || { echo "Extraction target already exists: $2" >&2; exit 1; }
  mkdir -p -- "$2"
  cp -a "${release[0]}/." "$2/"
  printf 'Extracted installer to %s\n' "$2"
else
  bash "${release[0]}/install.sh" "$@"
fi
exit 0
__TPF2MP_ARCHIVE_BELOW__
