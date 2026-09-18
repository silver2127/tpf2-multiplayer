#!/bin/sh
# game_watchdog.sh -- keep Transport Fever 2 running under the Steam client (systemd
# unit tpf2mp-game). Steam launches the game itself (Proton, the compat data, the
# overlay), so the game is never our child: ask Steam to start it, then watch for
# the process and ask again when it is gone. The mod's dedicated mode does the rest
# (host, load, autosave). Logs go to the journal.
set -u
APPID=1066780
: "${DISPLAY:=:9}"
export DISPLAY
STEAM=/usr/games/steam
started=0
while :; do
    if pgrep -f 'TransportFever2.exe' >/dev/null 2>&1; then
        started=0
        sleep 20
        continue
    fi
    if ! pgrep -x steam >/dev/null 2>&1; then
        echo "steam client is not running; waiting"
        sleep 15
        continue
    fi
    if [ "$started" -gt 0 ] && [ $(( $(date +%s) - started )) -lt 240 ]; then
        sleep 10          # a launch takes a while (Proton, then a 300 MB save); do not stack launches
        continue
    fi
    echo "asking Steam to launch app $APPID"
    "$STEAM" -applaunch "$APPID" >/dev/null 2>&1 &
    started=$(date +%s)
    sleep 30
done
