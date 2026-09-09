#!/bin/sh
# Deploy netpunch/masterserver.py to the VPS as a systemd service behind nginx.
#   sh tools/masterserver_deploy.sh root@76.13.109.115
# Installs /opt/tpf2mp/masterserver.py, the tpf2mp-master service on
# 127.0.0.1:8471, and a  location /tpf2mp/  block in the matrix vhost so the
# list is reachable at https://srv1306562.hstgr.cloud/tpf2mp/list . Re-run to
# update the script; the service restarts.
set -e
HOST="${1:-root@76.13.109.115}"
scp -q netpunch/masterserver.py "$HOST:/tmp/masterserver.py"
ssh "$HOST" 'set -e
mkdir -p /opt/tpf2mp
install -m 0644 /tmp/masterserver.py /opt/tpf2mp/masterserver.py
id -u tpf2mp >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin tpf2mp
cat > /etc/systemd/system/tpf2mp-master.service <<EOF
[Unit]
Description=tpf2mp master server (public game list)
After=network.target

[Service]
User=tpf2mp
ExecStart=/usr/bin/python3 /opt/tpf2mp/masterserver.py 8471
Restart=always
RestartSec=3
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now tpf2mp-master
systemctl restart tpf2mp-master
if ! grep -q "location /tpf2mp/" /etc/nginx/sites-enabled/matrix; then
  python3 - <<PY
import re
p="/etc/nginx/sites-enabled/matrix"; s=open(p).read()
blk="""    location /tpf2mp/ {
        proxy_pass http://127.0.0.1:8471/;
        proxy_set_header X-Real-IP \$remote_addr;
        client_max_body_size 8k;
    }
"""
# insert into every server block, right after its first server_name line
out=re.sub(r"(server_name srv1306562\.hstgr\.cloud;\n)", lambda m: m.group(1)+blk, s)
open(p,"w").write(out)
PY
  nginx -t && systemctl reload nginx
fi
sleep 1
curl -s http://127.0.0.1:8471/health; echo
curl -s https://srv1306562.hstgr.cloud/tpf2mp/health; echo
systemctl is-active tpf2mp-master'
