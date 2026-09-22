#!/bin/sh
# game_watchdog.sh -- keep Transport Fever 2 running under the Steam client (systemd
# unit tpf2mp-game). Steam launches the game itself (Proton, the compat data, the
# overlay), so the game is never our child: ask Steam to start it, then watch for
# the process and ask again when it is gone. The mod's dedicated mode does the rest
# (host, load, autosave). Logs go to the journal.
#
# A CRASHED GAME IS A STUCK GAME, not a dump file. The engine's crash reporter
# leaves the process alive in its dialog, and it also runs for a game WE killed:
# ~15 s after a kill it writes a .dmp and appends __CRASHDB_DUMP__ to stdout.txt --
# the NEXT run's stdout.txt, same path. Judging by dumps or markers killed the
# healthy new game twice over (2026-09-18 18:18 and 19:04, both self-feeding
# loops). What a crashed game stops doing is presenting: the menu DLL writes
# tpf2_menu.log every few seconds while it presents (title menu, loading screen,
# world alike). A game process whose menu log has not moved for STALL_SECONDS is
# stuck: it is killed -9 (no crash handler, no stray dump) and launched again.
set -u
case "${RUNTIME:-proton}" in
    native) exec python3 "$(dirname "$0")/native_watchdog.py" ;;
    proton) ;;
    *) echo "RUNTIME must be native or proton" >&2; exit 2 ;;
esac
APPID=1066780
: "${DISPLAY:=:9}"
export DISPLAY
STEAM=/usr/games/steam
GAME_DIR=$(ls -d "$HOME"/.steam/steam/steamapps/common/"Transport Fever 2" 2>/dev/null | head -1)
MENU_LOG="$GAME_DIR/tpf2_menu.log"
STALL_SECONDS=${STALL_SECONDS:-180}
started=0
launched_at=0
while :; do
    if pgrep -f 'TransportFever2[.]exe' >/dev/null 2>&1; then
        started=0
        if [ "$launched_at" -gt 0 ] && [ -f "$MENU_LOG" ]; then
            now=$(date +%s)
            mod=$(stat -c %Y "$MENU_LOG" 2>/dev/null || echo "$now")
            # the log must have moved since THIS launch before its silence counts (the
            # first seconds of a launch, before the DLL loads, are not a stall)
            if [ "$mod" -gt "$launched_at" ] && [ $((now - mod)) -ge "$STALL_SECONDS" ]; then
                echo "the game has not presented for $((now - mod)) s (tpf2_menu.log still); killing it -9 for a relaunch"
                pkill -9 -f '^[A-Za-z]:.*TransportFever2[.]exe'
                sleep 5
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
