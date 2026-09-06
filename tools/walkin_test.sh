#!/bin/sh
# Headless WALK-IN test (user 2026-08-28): 1P alone picks the two right-most
# clone cells (top, then bottom) on the extended select of PROFILE, the
# ring intro runs (WF_INTRO=1), and every sound post is logged. Prints the
# picks + the intro's speech posts: 31xx = board phrase, "wav" = library.
#   tools/walkin_test.sh [PROFILE] [COLS]   (COLS = right pulses, default 6)
set -e
PROF=${1:-superstars}; COLS=${2:-6}
P1=""; f=30
i=0; while [ $i -lt $COLS ]; do P1="$P1${P1:+,}$f-$((f+1)):1"; f=$((f+15)); i=$((i+1)); done
P1="$P1,$((f+10))-$((f+11)):10,$((f+40))-$((f+41)):8,$((f+70))-$((f+71)):10"
WF_INTRO=1 WF_SEATED=1 WF_P1="$P1" WF_SNDLOG=1 timeout 300 ./wfengine --headless --profile "$PROF" --drive charselect --frames 4000 2>&1 \
  | grep "cs: picks\|snd: 31[4-7]\|snd: wav\|announce:" | head -20
