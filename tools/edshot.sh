#!/bin/sh
# Headless editor screenshot: runs wfeditor on a private Xvfb display so
# nothing touches the user's screen (user 2026-08-28).
#
#   tools/edshot.sh OUT.png [TAB] [PROFILE] [click X Y]...
#
# TAB = WF_EDITOR_NAV name (Rules, Skins, ...), default Rules; PROFILE
# default superstars; every "click X Y" is sent in order (window-relative,
# the window sits at 0,0 on the virtual display) before the shot;
# "rclick X Y" = right button.
# Clicks need the slow press below: a one-frame press/release is dropped.
set -e
OUT=$1; TAB=${2:-Rules}; PROF=${3-superstars}; shift 3 2>/dev/null || shift $#
DISP=:99
export DISPLAY=$DISP
Xvfb $DISP -screen 0 1920x1080x24 >/dev/null 2>&1 & XP=$!
sleep 1
WF_EDITOR_NAV=$TAB timeout 120 ./wfengine --editor ${PROF:+--profile "$PROF"} >/dev/null 2>&1 & EP=$!
trap 'kill $EP $XP 2>/dev/null' EXIT
W=""
i=0
while [ $i -lt 40 ]; do
    sleep 1; i=$((i + 1))
    W=$(xdotool search --name wfeditor 2>/dev/null | head -1)
    [ -n "$W" ] && break
done
[ -n "$W" ] || { echo "edshot: no editor window" >&2; exit 1; }
sleep 2
xdotool windowfocus "$W"
while [ $# -ge 2 ]; do
    if [ "$1" = type ]; then xdotool type --delay 60 "$2"; sleep 0.8; shift 2; continue; fi
    if [ "$1" = wait ]; then sleep "$2"; shift 2; continue; fi
    [ $# -ge 3 ] || break
    case "$1" in click) B=1;; rclick) B=3;; *) echo "edshot: expected 'click|rclick X Y' or 'type TEXT'" >&2; exit 1;; esac
    xdotool mousemove "$2" "$3"; sleep 0.5
    xdotool mousemove $(($2 + 1)) $(($3 + 1)); sleep 0.3
    xdotool mousedown $B; sleep 0.25; xdotool mouseup $B
    sleep 1.5
    shift 3
done
import -window root "$OUT"
echo "edshot: $OUT"
