#!/bin/sh
# Editor walkthrough video: wfeditor on a private Xvfb display, ffmpeg x11grab,
# xdotool clicks through every tab and sub-tab.  -> docs/video/editor.mp4
# Must not run at the same time as tools/edshot.sh (both own display :99).
set -e
cd "$(dirname "$0")/.."
OUT=${1:-docs/video/editor.mp4}; mkdir -p "$(dirname "$OUT")"
export DISPLAY=:99
Xvfb :99 -screen 0 1920x1080x24 >/dev/null 2>&1 & XP=$!
sleep 1
WF_EDITOR_NAV=Profiles timeout 600 ./wfengine --editor --profile superstars >/dev/null 2>&1 & EP=$!
trap 'kill $EP $XP $FP 2>/dev/null' EXIT
W=""; i=0
while [ $i -lt 40 ]; do sleep 1; i=$((i+1)); W=$(xdotool search --name wfeditor 2>/dev/null | head -1); [ -n "$W" ] && break; done
[ -n "$W" ] || { echo "edvideo: no editor window" >&2; exit 1; }
sleep 2; xdotool windowfocus "$W"; xdotool mousemove 960 1000
ffmpeg -hide_banner -loglevel error -y -f x11grab -framerate 30 -video_size 1920x1080 -i :99 -vf scale=1600:900 -c:v libx264 -crf 24 -preset veryfast -pix_fmt yuv420p "$OUT" & FP=$!
sleep 2
click() { xdotool mousemove "$1" "$2"; sleep 0.4; xdotool mousemove $(($1+1)) $(($2+1)); sleep 0.2; xdotool mousedown 1; sleep 0.25; xdotool mouseup 1; sleep "${3:-2.5}"; }
# ---- the tour (top tabs y 22, sub-tabs y 60, left lists x 140) ----
sleep 3                                # Profiles
click 410 22 3                         # Rules: ringout_rules
click 90 168 3;  click 90 182 3        # game_rules, mode_rules
click 424 60 3;  click 498 60 3; click 578 60 3   # Rules sub-tabs: Tables, Arenas, Scenes
click 494 22 3                         # Wrestlers: Hogan
click 140 420 3; click 140 448 3       # DUGGAN, UNDERTAKER
click 574 22 3                         # Skins: duggan Artwork
click 330 60 2; click 474 60 3; click 570 60 3   # Info, Poses, AI Recipe
click 140 146 2; click 404 60 3; click 474 60 3  # undertaker: Artwork, Poses
click 650 22 3                         # Classes: 0 medium Info
click 396 60 3;  click 140 348 3; click 140 376 3   # Poses; 3 heavy; 4 giant
click 738 22 3                         # Weapons: steps
click 140 202 3                        # box
click 820 22 3                         # Arenas: wwf In-ring
click 424 60 3; click 502 60 3; click 578 60 3; click 664 60 3  # Ringside, Crowd, Ropes, AI recipe
click 140 168 3; click 334 60 3        # cage, In-ring
click 140 196 3                        # generic-color
click 902 22 3                         # Scenes: Walkout Preview
click 430 60 3; click 512 60 3         # Tilemap, Sounds
click 140 144 2; click 334 60 3        # Talk screen Preview
click 140 168 3; click 140 222 3       # Title-win card, Ending + credits
click 988 22 3                         # Calibrate
click 1076 22 3                        # Sounds
click 1150 22 3                        # Input
click 1220 22 3                        # Tools
click 336 22 3                         # back to Profiles
sleep 1
kill -INT $FP; wait $FP 2>/dev/null || true
echo "edvideo: $OUT"
