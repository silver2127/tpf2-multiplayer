#!/bin/sh
# steam_bootstrap.sh -- what Debian's /usr/games/steam does after its licence dialog
# (which has no place on a headless box): fetch Valve's launcher tarball, check it,
# and unpack the client bootstrap into ~/.steam/debian-installation. Run as the
# tpf2server user; idempotent. setup_vps.sh installs it beside steam_login.sh.
set -eu
S=$HOME/.steam/debian-installation
# the version and checksum the installed wrapper expects (it re-checks them on every start)
V=$(sed -n 's/^version="\(.*\)"/\1/p' /usr/games/steam | head -1)
SHA=$(sed -n 's/^sha256="\(.*\)"/\1/p' /usr/games/steam | head -1)
[ -n "$V" ] && [ -n "$SHA" ] || { echo "cannot read version/sha256 from /usr/games/steam"; exit 1; }
if [ "$(cat "$S/deb-installer/version" 2>/dev/null || true)" = "$V" ] && [ -x "$S/ubuntu12_32/steam" ]; then
    echo "bootstrap $V already in place"; exit 0
fi
mkdir -p "$S/deb-installer"
tgz="$S/deb-installer/steam_$V.tar.gz"
curl -fsS -o "$tgz" "https://repo.steampowered.com/steam/archive/beta/steam_$V.tar.gz"
got=$(sha256sum -b "$tgz" | cut -d' ' -f1)
[ "$got" = "$SHA" ] || { echo "sha256 mismatch: $got"; rm -f "$tgz"; exit 1; }
tar -C "$S/deb-installer" -zxf "$tgz" steam-launcher/bootstraplinux_ubuntu12_32.tar.xz
mv "$S/deb-installer/steam-launcher/bootstraplinux_ubuntu12_32.tar.xz" "$S/bootstrap.tar.xz"
tar -C "$S" -xf "$S/bootstrap.tar.xz"
echo "$V" > "$S/deb-installer/version"
rm -f "$tgz"
mkdir -p "$HOME/.steam"
ln -fns "$S" "$HOME/.steam/root"
ln -fns "$S" "$HOME/.steam/steam"
echo "bootstrap $V unpacked into $S"
