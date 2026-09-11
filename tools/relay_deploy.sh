#!/bin/sh
# Deploy the dedicated relay (lobby.py --relay-only) to the VPS as a systemd service.
#   sh tools/relay_deploy.sh [root@76.13.109.115] [lobby name] [udp port]
# Installs netpunch/*.py to /opt/tpf2mp/netpunch, the tpf2mp-relay service (runs
# as user tpf2mp, io dir /var/lib/tpf2mp/relay), opens the UDP port in ufw, and
# announces the lobby to the master server (public, always on) as "Dedicated Test Server All Welcome". Re-run to
# update the code or the settings; the service restarts and keeps its code (the
# relay keeps its secret in its io dir). It announces to the master server on the
# same machine; set MASTER_URL for one elsewhere.
set -e
HOST="${1:-root@76.13.109.115}"
LOBBY="${2:-Dedicated Test Server All Welcome}"
PORT="${3:-29471}"
MASTER="${MASTER_URL:-http://127.0.0.1:8471}"
# Never restart the relay under a live session: the restart drops every player's
# transport and each game carries on alone (2026-09-10, 10:12). FORCE=1 overrides.
if [ "${FORCE:-0}" != "1" ]; then
  N=$(ssh "$HOST" 'python3 -c "import json;d=json.load(open(\"/var/lib/tpf2mp/relay/lobby_state.json\"));print(len(d.get(\"players\",[])))" 2>/dev/null' || echo 0)
  if [ "${N:-0}" -gt 0 ]; then echo "relay has $N player(s) connected -- not restarting (FORCE=1 to override)"; exit 3; fi
fi
tar -C netpunch -cf - lobby.py punch.py seal.py connect.py mesh.py observe.py modshare.py desynclogs.py 2>/dev/null \
  | ssh "$HOST" 'mkdir -p /opt/tpf2mp/netpunch && tar -C /opt/tpf2mp/netpunch -xf -'
ssh "$HOST" "set -e
python3 -c 'import stun' 2>/dev/null || pip3 install --quiet --break-system-packages pystun3 || apt-get install -y -qq python3-pip && pip3 install --quiet --break-system-packages pystun3
id -u tpf2mp >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin tpf2mp
mkdir -p /var/lib/tpf2mp/relay /etc/tpf2mp
chown -R tpf2mp:tpf2mp /var/lib/tpf2mp
cat > /etc/tpf2mp/relay.env <<EOF
LOBBY_NAME=$LOBBY
RELAY_PORT=$PORT
MASTER_URL=$MASTER
EOF
cat > /etc/systemd/system/tpf2mp-relay.service <<'EOF'
[Unit]
Description=tpf2mp dedicated relay (lobby.py --relay-only)
After=network-online.target
Wants=network-online.target

[Service]
User=tpf2mp
EnvironmentFile=/etc/tpf2mp/relay.env
WorkingDirectory=/var/lib/tpf2mp/relay
ExecStart=/usr/bin/python3 /opt/tpf2mp/netpunch/lobby.py host --relay-only --name relay \\
  --lobby-name \"\${LOBBY_NAME}\" --local-port \${RELAY_PORT} \\
  --publish \${MASTER_URL} --public \\
  --io-dir /var/lib/tpf2mp/relay
Restart=always
RestartSec=5
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/var/lib/tpf2mp/relay

[Install]
WantedBy=multi-user.target
EOF
if command -v ufw >/dev/null 2>&1; then ufw allow $PORT/udp >/dev/null && echo \"ufw: udp/$PORT open\"; fi
systemctl daemon-reload
systemctl enable --now tpf2mp-relay
systemctl restart tpf2mp-relay
sleep 6
systemctl is-active tpf2mp-relay
journalctl -u tpf2mp-relay -n 12 --no-pager | cut -c1-160
curl -s $MASTER/list | python3 -c 'import json,sys; [print(\"listed:\", r[\"name\"], \"|\", r[\"game\"], \"|\", r[\"players\"], \"players\") for r in json.load(sys.stdin)[\"servers\"]]'"
