#!/bin/sh
# Deploy netpunch/masterserver.py to the VPS as a systemd service behind nginx.
#   sh tools/masterserver_deploy.sh [ssh target] [hostname]
# Installs /opt/tpf2mp/masterserver.py, the tpf2mp-master service on
# 127.0.0.1:8471, and an nginx site that serves it at https://<hostname>/tpf2mp/
# with its own Let's Encrypt certificate (certbot webroot; each renewal reloads
# nginx). The default hostname is the VPS's reverse-DNS name, so the list needs
# no domain of its own. Other nginx sites are left alone. Re-run to update the
# script; the service restarts.
set -e
HOST="${1:-root@76.13.109.115}"
NAME="${2:-srv1306562.hstgr.cloud}"
scp -q netpunch/masterserver.py "$HOST:/tmp/masterserver.py"
ssh "$HOST" "NAME='$NAME' sh -s" <<'REMOTE'
set -e
mkdir -p /opt/tpf2mp
install -m 0644 /tmp/masterserver.py /opt/tpf2mp/masterserver.py
id -u tpf2mp >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin tpf2mp
# desync reports (POST /tpf2mp/desync) are kept here; the service may write nowhere else
mkdir -p /var/lib/tpf2mp/desync
chown tpf2mp:tpf2mp /var/lib/tpf2mp/desync
chmod 0750 /var/lib/tpf2mp/desync
cat > /etc/systemd/system/tpf2mp-master.service <<EOF
[Unit]
Description=tpf2mp master server (public game list, desync reports)
After=network.target

[Service]
User=tpf2mp
ExecStart=/usr/bin/python3 /opt/tpf2mp/masterserver.py 8471 --desync-dir /var/lib/tpf2mp/desync
Restart=always
RestartSec=3
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/var/lib/tpf2mp/desync
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now tpf2mp-master
systemctl restart tpf2mp-master

# One nginx site for $NAME: port 80 answers the ACME challenge and redirects the
# rest; 443 proxies /tpf2mp/ to the service. The 443 listen names this machine's
# public IPv4 (the default route's source address), so it joins a site already
# listening on that address. Not getent: /etc/hosts maps the machine's own name
# to 127.0.1.1, and a listen there serves nobody outside.
IP=$(ip -4 route get 1.1.1.1 | awk '{ for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit } }')
[ -n "$IP" ] || { echo "cannot find this machine's public IPv4"; exit 2; }
SITE=/etc/nginx/sites-available/tpf2mp
WEBROOT=/var/www/tpf2mp-acme
mkdir -p "$WEBROOT"
write_site() {
  {
    cat <<EOF
# written by tools/masterserver_deploy.sh
server {
    listen 80;
    server_name $NAME;
    location /.well-known/acme-challenge/ { root $WEBROOT; }
    location / { return 301 https://\$host\$request_uri; }
}
EOF
    if [ "$1" = tls ]; then
      cat <<EOF
server {
    listen $IP:443 ssl;
    server_name $NAME;
    ssl_certificate /etc/letsencrypt/live/$NAME/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/$NAME/privkey.pem;
EOF
      [ -f /etc/letsencrypt/options-ssl-nginx.conf ] && echo "    include /etc/letsencrypt/options-ssl-nginx.conf;"
      cat <<EOF
    location /tpf2mp/ {
        proxy_pass http://127.0.0.1:8471/;
        proxy_set_header X-Real-IP \$remote_addr;
        client_max_body_size 8k;
    }
    # a desync report is a zip of scrubbed logs (netpunch/desynclogs.py); the
    # service refuses more than 16 MB as well
    location = /tpf2mp/desync {
        proxy_pass http://127.0.0.1:8471/desync;
        proxy_set_header X-Real-IP \$remote_addr;
        client_max_body_size 16m;
        proxy_read_timeout 120s;
    }
    location / { return 404; }
}
EOF
    fi
  } > "$SITE"
  ln -sf "$SITE" /etc/nginx/sites-enabled/tpf2mp
  if ! nginx -t 2>/dev/null; then
    rm -f /etc/nginx/sites-enabled/tpf2mp
    nginx -t || true
    echo "nginx rejected the tpf2mp site; it has been disabled"
    exit 1
  fi
  systemctl reload nginx
}
if [ ! -f "/etc/letsencrypt/live/$NAME/fullchain.pem" ]; then
  write_site http
  certbot certonly --webroot -w "$WEBROOT" -d "$NAME" --non-interactive --agree-tos \
    --keep-until-expiring --deploy-hook "systemctl reload nginx"
fi
write_site tls
sleep 1
curl -fsS http://127.0.0.1:8471/health; echo
# --resolve: on this machine the name is 127.0.1.1, where nothing listens on 443
curl -fsS --resolve "$NAME:443:$IP" "https://$NAME/tpf2mp/health"; echo
systemctl is-active tpf2mp-master
REMOTE
