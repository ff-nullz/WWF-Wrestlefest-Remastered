#!/bin/sh
# Run the coverage drive set headless with rendering on and print every
# UNMAPPED data-layer read (deduped by page+site). Usage: tools/cov_drives.sh [outfile]
# WF_TBL_RECORD=path additionally records the touched ROM ranges (transition).
cd "$(dirname "$0")/.." || exit 1
OUT=${1:-/tmp/wf-unmapped.txt}
export WF_RENDER_ALL=1 WF_DATA=rom
: > "$OUT"
run() { "$@" 2>&1 >/dev/null | grep "UNMAPPED\|OUT-OF-RANGE" >> "$OUT"; }
run ./wfengine --headless --frames 3000 --drive fuzz
run ./wfengine --headless --frames 1500 --drive pin
run ./wfengine --headless --frames 1500 --drive throw
run ./wfengine --headless --frames 1000 --drive solorun
run ./wfengine --headless --frames 6000 --drive attract
run ./wfengine --headless --frames 1200 --drive gameselect
run ./wfengine --headless --frames 1500 --drive charselect
run env WF_FRONT=1 ./wfengine --headless --frames 5000 --drive script
run env WF_RUMBLE=1 WF_CPU2=1 ./wfengine --headless --frames 5000 --drive fuzz
for w in 0 1 2 3 4 5 6 7 8 9 10 11; do
  run env WF_W1=$w WF_W2=$(( (w+5)%12 )) WF_CPU2=1 ./wfengine --headless --frames 2500 --drive fuzz
done
run env WF_CPU1=1 WF_CPU2=1 ./wfengine --headless --frames 6000 --drive script
run env WF_OUT2=300 ./wfengine --headless --frames 1500 --drive fuzz
run env WF_INTRO=1 ./wfengine --headless --frames 1200 --drive script
run env WF_FRONT=1 WF_CREDITS=9 ./wfengine --headless --frames 20000 --drive ladder
run env WF_FRONT=1 WF_CREDITS=1 WF_LADDER=lose ./wfengine --headless --frames 4000 --drive ladder
run ./wfengine --selftest --frames 1200
sort -u "$OUT" -o "$OUT"
echo "$(wc -l < "$OUT") unmapped/out-of-range sites -> $OUT"
