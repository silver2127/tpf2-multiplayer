#!/bin/sh
# setup_vps.sh -- prepare a Linux box (Ubuntu 24.04+) to run a TpF2 Multiplayer
# dedicated server: the Windows game under Proton inside a Linux Steam client, on a
# virtual display. Idempotent; run as root. Installs nothing the relay or master
# server use, and touches no other user.
#
#   sh tools/server/setup_vps.sh            (on the box, from a checkout or a copy of tools/server)
#
# After it: `sudo -iu tpf2server sh /opt/tpf2mp/server/steam_login.sh` (the one
# interactive step), then `tpf2server install`, `tpf2server configure`, `systemctl
# start tpf2mp-game`. tools/server/README.md is the runbook.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
USER_NAME=tpf2server
HOME_DIR=/home/$USER_NAME
DEST=/opt/tpf2mp/server
DISPLAY_NUM=${TPF2_SERVER_DISPLAY:-9}

echo "== packages"
dpkg --add-architecture i386
apt-get update -qq
# steam-installer is Debian/Ubuntu's packaging of Valve's client (it downloads the
# client itself on first start); the i386 libraries are what that client needs.
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  steam-installer xvfb xdotool x11-utils mesa-vulkan-drivers mesa-vulkan-drivers:i386 \
  libvulkan1 libvulkan1:i386 libgl1-mesa-dri:i386 libsdl2-2.0-0 python3 curl unzip zstd \
  >/dev/null

echo "== user $USER_NAME"
id -u $USER_NAME >/dev/null 2>&1 || useradd --create-home --shell /bin/bash $USER_NAME
mkdir -p $DEST
install -m 755 "$HERE/tpf2server" /usr/local/bin/tpf2server
install -m 755 "$HERE/steam_login.sh" $DEST/steam_login.sh
install -m 755 "$HERE/steam_bootstrap.sh" $DEST/steam_bootstrap.sh
install -m 644 "$HERE/steam_compat.py" $DEST/steam_compat.py
install -m 755 "$HERE/game_watchdog.sh" $DEST/game_watchdog.sh
install -m 644 "$HERE/native_watchdog.py" $DEST/native_watchdog.py
install -m 644 "$HERE/server.env.example" $DEST/server.env.example
[ -f /etc/tpf2mp/server.env ] || { mkdir -p /etc/tpf2mp; install -m 644 "$HERE/server.env.example" /etc/tpf2mp/server.env; }
sed -i "s/^DISPLAY=.*/DISPLAY=:$DISPLAY_NUM/" /etc/tpf2mp/server.env

echo "== systemd units"
cat > /etc/systemd/system/tpf2mp-xvfb.service <<EOF
[Unit]
Description=tpf2mp dedicated server: virtual display :$DISPLAY_NUM
After=network.target

[Service]
User=$USER_NAME
ExecStart=/usr/bin/Xvfb :$DISPLAY_NUM -screen 0 1280x720x24 -nolisten tcp -noreset
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/tpf2mp-steam.service <<EOF
[Unit]
Description=tpf2mp dedicated server: Steam client (offline mode) on :$DISPLAY_NUM
After=tpf2mp-xvfb.service network-online.target
Requires=tpf2mp-xvfb.service

[Service]
User=$USER_NAME
EnvironmentFile=/etc/tpf2mp/server.env
Environment=HOME=$HOME_DIR
WorkingDirectory=$HOME_DIR
# -silent: no window on start; -no-browser: no CEF overlay/store, which is what
# a Steam client under Xvfb spends its time crashing in
ExecStart=/usr/games/steam -silent -no-browser -nofriendsui
Restart=always
RestartSec=10
# Steam forks: the unit follows the main pid it leaves behind
KillMode=control-group

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/tpf2mp-game.service <<EOF
[Unit]
Description=tpf2mp dedicated server: Transport Fever 2 (watchdog)
After=tpf2mp-steam.service
Requires=tpf2mp-steam.service

[Service]
User=$USER_NAME
EnvironmentFile=/etc/tpf2mp/server.env
Environment=HOME=$HOME_DIR
WorkingDirectory=$HOME_DIR
ExecStart=/bin/sh $DEST/game_watchdog.sh
Restart=always
RestartSec=15

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now tpf2mp-xvfb >/dev/null
echo "== firewall"
if command -v ufw >/dev/null 2>&1; then
  # the host lobby: UDP for the session, TCP for save transfers (same port; the relay uses 29471)
  PORT=$(sed -n 's/^LOBBY_PORT=//p' /etc/tpf2mp/server.env)
  [ -n "$PORT" ] && ufw allow "$PORT"/udp >/dev/null && ufw allow "$PORT"/tcp >/dev/null && echo "ufw: $PORT udp+tcp open"
fi
echo "== steam client bootstrap (the wrapper's licence dialog has no place on a headless box)"
sudo -iu $USER_NAME sh $DEST/steam_bootstrap.sh
echo "setup done. Next: sudo -iu $USER_NAME sh $DEST/steam_login.sh"
