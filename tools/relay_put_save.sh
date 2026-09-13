#!/bin/sh
# Replace the world the dedicated relay holds (what a fresh leader is resumed into).
#   sh tools/relay_put_save.sh "<path to save>.sav" [root@76.13.109.115]
# Copies the .sav and its .sav.lua / .jpg sidecars to the relay's io dir as
# incoming_save.*; no restart needed (the relay reads the file when it resumes).
# A relay with no stored world asks its leader to press START GAME, which uploads
# the leader's most recent save; with one stored, it loads for everyone on arrival
# unless the leader says /new in chat before it is sent.
set -e
SAV="$1"; HOST="${2:-root@76.13.109.115}"
[ -f "$SAV" ] || { echo "no such save: $SAV" >&2; exit 2; }
STEM="${SAV%.sav}"
scp -q "$SAV" "$HOST:/tmp/incoming_save.sav"
[ -f "$STEM.sav.lua" ] && scp -q "$STEM.sav.lua" "$HOST:/tmp/incoming_save.sav.lua"
[ -f "$STEM.jpg" ] && scp -q "$STEM.jpg" "$HOST:/tmp/incoming_save.jpg"
ssh "$HOST" 'set -e; for f in /tmp/incoming_save.sav /tmp/incoming_save.sav.lua /tmp/incoming_save.jpg; do [ -f "$f" ] && install -o tpf2mp -g tpf2mp -m 0644 "$f" /var/lib/tpf2mp/relay/ && rm -f "$f"; done; ls -la --time-style=+%H:%M /var/lib/tpf2mp/relay/incoming_save.* | awk "{print \$5, \$6, \$7}"'
echo "relay world replaced"
