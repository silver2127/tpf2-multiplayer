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
DUMPS=$(ls -d "$HOME"/.steam/steam/userdata/*/1066780/local/crash_dump 2>/dev/null | head -1)
started=0
launched_at=0
while :; do
    if pgrep -f 'TransportFever2.exe' >/dev/null 2>&1; then
        started=0
        # a crash leaves the engine's dialog up and the process alive. The game itself
        # prints __CRASHDB_DUMP__ into ITS OWN stdout.txt at the exception, so that line
        # in a stdout.txt newer than the launch is a crash of THIS run. A .dmp newer than
        # the launch is not: the engine's crash reporter writes the post-mortem of a
        # KILLED instance (tpf2server stop) 30-120 s later, after the next launch, and
        # the watchdog used to kill the healthy new game for it, again and again
        # (2026-09-18 18:18: a self-feeding "crash loop" after a plain update).
        if [ -n "$DUMPS" ] && [ "$launched_at" -gt 0 ] && [ -f "$DUMPS/stdout.txt" ]            && [ -n "$(find "$DUMPS" -maxdepth 1 -name stdout.txt -newermt "@$launched_at" 2>/dev/null)" ]; then
            newdump=$(grep -a -o '__CRASHDB_DUMP__ [0-9a-f-]*' "$DUMPS/stdout.txt" 2>/dev/null | head -1)
            if [ -n "$newdump" ]; then
                echo "$newdump in this run's stdout.txt; killing the game for a relaunch"
                pkill -f '^Z:.*TransportFever2' ; pkill -f '^C:.*TransportFever2'
                sleep 5
                pkill -9 -f '^Z:.*TransportFever2' ; pkill -9 -f '^C:.*TransportFever2'
                launched_at=0
                sleep 10
                continue
            fi
        fi
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
    launched_at=$started
    sleep 30
done
