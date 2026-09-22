#!/bin/sh
# steam_login.sh -- the one interactive step: log the Steam client in on the virtual
# display, then switch it to offline mode so the same account can play online elsewhere.
# Run as the tpf2server user over ssh:  sudo -iu tpf2server sh /opt/tpf2mp/server/steam_login.sh
#
# The password and the Steam Guard code are typed by you at this terminal and pushed
# into the client's login window with xdotool; nothing is written anywhere. Steam
# keeps its own refresh token afterwards (config/loginusers.vdf), as on any PC.
set -eu
: "${DISPLAY:=:9}"
export DISPLAY
STEAM=/usr/games/steam
CFG="$HOME/.steam/steam/config/loginusers.vdf"

if ! xdpyinfo >/dev/null 2>&1; then echo "no X display at $DISPLAY (systemctl start tpf2mp-xvfb)"; exit 1; fi
if systemctl is-active --quiet tpf2mp-steam 2>/dev/null; then echo "stop the client first: sudo systemctl stop tpf2mp-steam"; exit 1; fi
pkill -x steam 2>/dev/null || true

if [ "${TPF2_OFFLINE_ONLY:-0}" = 1 ]; then USERNAME=$(python3 -c "import re,sys;t=open(sys.argv[1]).read();m=re.search(r'\"AccountName\"\s*\"([^\"]+)\"',t);print(m.group(1) if m else '')" "$CFG"); [ -n "$USERNAME" ] || { echo "no account in $CFG"; exit 1; }; pkill -x steam 2>/dev/null || true; sleep 3; SKIP_LOGIN=1; fi
if [ "${SKIP_LOGIN:-0}" != 1 ]; then
printf 'Steam account name: '; read -r USERNAME
printf 'Password (not echoed): '; stty -echo; read -r PASSWORD; stty echo; echo
echo "starting the client on $DISPLAY (the first start downloads the client itself, a few minutes)..."
"$STEAM" -login "$USERNAME" "$PASSWORD" >/tmp/steam_login.out 2>&1 &
unset PASSWORD
for i in $(seq 1 90); do
    sleep 4
    if grep -q '"RememberPassword"' "$CFG" 2>/dev/null && grep -q "\"AccountName\"\s*\"$USERNAME\"" "$CFG" 2>/dev/null; then break; fi
    # Steam Guard: the client opens a code window; type what the operator types here
    if xdotool search --name 'Steam Guard' >/dev/null 2>&1 || xdotool search --name 'Sign in' >/dev/null 2>&1; then
        if [ "${ASKED:-0}" = 0 ]; then
            printf 'Steam Guard code (from the app / email), or Enter to keep waiting: '; read -r CODE
            if [ -n "$CODE" ]; then
                win=$(xdotool search --name 'Steam Guard' 2>/dev/null | head -1 || true)
                [ -z "$win" ] && win=$(xdotool search --name 'Sign in' 2>/dev/null | head -1 || true)
                [ -n "$win" ] && xdotool windowactivate --sync "$win" 2>/dev/null || true
                xdotool type --delay 80 "$CODE"; xdotool key Return
                ASKED=1
            fi
        fi
    fi
    [ $((i % 5)) -eq 0 ] && echo "  waiting for the login to complete ($((i*4)) s)..."
done
if ! grep -q "\"AccountName\"\s*\"$USERNAME\"" "$CFG" 2>/dev/null; then
    echo "the client did not record a login for $USERNAME; see /tmp/steam_login.out and try again"; exit 1
fi
echo "logged in as $USERNAME"
fi
if [ "${TPF2_STAY_ONLINE:-0}" = 1 ]; then echo "TPF2_STAY_ONLINE=1: the client stays online (install the game now); mark offline later with: sudo systemctl stop tpf2mp-steam; TPF2_OFFLINE_ONLY=1 sh $0"; exit 0; fi
# Let the client finish its first-run work (library, Proton listing), then stop it and
# mark the account for offline mode: WantsOfflineMode makes the next start offline
# without the dialog; the same account is then free to play online elsewhere.
sleep 60
pkill -x steam 2>/dev/null || true
sleep 5
python3 - "$CFG" "$USERNAME" <<'EOF'
import re, sys
p, user = sys.argv[1], sys.argv[2]
t = open(p, encoding="utf-8").read()
def setkey(block, key, val):
    if re.search(r'"%s"\s*"[^"]*"' % key, block):
        return re.sub(r'("%s"\s*")[^"]*(")' % key, lambda m: m.group(1) + val + m.group(2), block)
    return block.rstrip().rstrip("}").rstrip() + '\n\t\t"%s"\t\t"%s"\n\t}' % (key, val)
# the account's block: from its AccountName back to the enclosing "{"
m = re.search(r'\{[^{}]*"AccountName"\s*"%s"[^{}]*\}' % re.escape(user), t)
if not m:
    sys.exit("account block not found in " + p)
b = m.group(0)
for k, v in (("WantsOfflineMode", "1"), ("SkipOfflineModeWarning", "1"), ("RememberPassword", "1"), ("MostRecent", "1"), ("AllowAutoLogin", "1")):
    b = setkey(b, k, v)
open(p, "w", encoding="utf-8").write(t[:m.start()] + b + t[m.end():])
print("offline mode set for", user)
EOF
echo "done. Start the client for good: sudo systemctl enable --now tpf2mp-steam; then install the game (tools/server/README.md)."
