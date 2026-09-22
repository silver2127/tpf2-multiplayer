#!/bin/sh
# xclick.sh -- click at x,y in the game's window on the virtual display and take a
# screenshot: the operator's hand for the dialogs a headless first run meets (the
# news popup, the graphics warning). Xvfb has no window manager, so the window is
# focused explicitly first (an unfocused xdotool click never reaches SDL/Wine).
#
#   xclick.sh X Y [/tmp/after.png]        as the tpf2server user, DISPLAY set
#   xclick.sh shot [/tmp/shot.png]        screenshot only
set -u
: "${DISPLAY:=:9}"
export DISPLAY
out=${3:-/tmp/vps_screen.png}
if [ "${1:-}" = shot ]; then import -window root "${2:-/tmp/vps_screen.png}"; exit 0; fi
x=$1; y=$2
win=$(xwininfo -root -tree | grep -F '"Transport Fever 2"' | grep -o '0x[0-9a-f]*' | head -1)
[ -n "$win" ] || { echo "no Transport Fever 2 window on $DISPLAY"; xwininfo -root -tree | grep -F '":' | grep -v 'has no name' | head -5; exit 1; }
xdotool windowfocus "$win" 2>/dev/null
xdotool mousemove --sync "$x" "$y"
sleep 0.4
xdotool mousedown 1; sleep 0.1; xdotool mouseup 1
sleep "${XCLICK_WAIT:-4}"
import -window root "$out"
echo "clicked $x,$y in $win -> $out"
