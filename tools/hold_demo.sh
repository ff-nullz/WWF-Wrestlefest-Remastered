#!/bin/sh
# docs/engine-specs/hold-timeout.md repros. Run from the repo root.
#   tools/hold_demo.sh stance   -- 0x0C -> 0x15, no press: reverse at +0x80
#   tools/hold_demo.sh drag     -- ... then the stick held (move 0x16)
#   tools/hold_demo.sh mash N   -- the victim (P2) taps B: the one-shot roll
#   tools/hold_demo.sh cpu      -- CPU victim (WF_CPU2=1): no roll, the clock
#   tools/hold_demo.sh cpuhold  -- CPU holder: the clock is 0xBB, not 0x80
set -e
GRAB="0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10"
FILT='/^tr f/{f=$2;o=$3;for(i=1;i<=NF;i++){if($i~/^st=/)s=$i;if($i~/^mv=/)m=$i;if($i~/^\(/)x=$i};c=s" "m" "x;if(p[o]!=c){print f,o,c;p[o]=c};next}{print}'
run() { env WF_W1=2 WF_DBGSEL=1 WF_TRACE=5 "$@" ./wfengine --headless --frames "${FR:-400}" --drive script 2>&1 \
        | awk "$FILT" | grep -Ev '^(package|banner|video):'; }

case "$1" in
stance)  run WF_P1="$GRAB" ;;
drag)    run WF_P1="$GRAB,100-400:1" ;;
mash)    n=${2:-1}; run WF_P1="$GRAB" WF_P2="$((100 + n * 2))-$((101 + n * 2)):10,140-141:10,180-181:10" ;;
cpu)     run WF_CPU2=1 WF_P1="0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10" ;;
cpuhold) FR=1200; run WF_CPU2=1 WF_P1="0-0:0" ;;
*)       echo "usage: $0 stance|drag|mash N|cpu|cpuhold" >&2; exit 2 ;;
esac
