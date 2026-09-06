#!/bin/bash
# Ring-out repro (docs/engine-specs/ringout-impl.md): P1 = wrestler 5 (gorilla press 0x2F
# gives react 0x16), B1 tap ladder into the stance, drag P2 to the left ropes, 5-frame
# drag back to flip facing, throw at f443 -> exit -> scene 2 at f584.
#   tools/ringout_demo.sh FRAMES [ENV=VAL ...] [-- extra wfengine args]
#   tools/ringout_demo.sh 2400                                # count-out: 1 at f663, 20 at f2183
#   tools/ringout_demo.sh 2400 WF_P2="2095-2182:1"            # loser walking at the 20-count (0x11702)
#   tools/ringout_demo.sh 1500 WF_P2="720-880:1,881-960:4"    # human climbs back in (zone 4 + stick +y)
#   tools/ringout_demo.sh 1500 WF_CPU2AT=600                  # CPU walks home (0x1C6DC)
cd "$(dirname "$0")/.."
FR=${1:-2600}; shift
ENVS=(); ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in --) shift; ARGS=("$@"); break;; *=*) export "$1"; ENVS+=("$1");; *) ARGS+=("$1");; esac
  shift
done
# TAPS_TO: the B1 tap ladder must STOP as soon as the tie-up is won (f95 with the
# 0xF7E8/0xF574 contact tie-up), or the first tap inside the hold fires the throw
# at f101 in mid-ring and there is no ring-out. 94 = last tap on the winning frame.
TAPS_TO=${TAPS_TO:-94}; DRAG0=${DRAG0:-252}; DRAG1=${DRAG1:-430}; DRAGBITS=${DRAGBITS:-2}
DRAG2A=${DRAG2A:-433}; DRAG2B=${DRAG2B:-438}; DRAG2BITS=${DRAG2BITS:-1}; THROWF=${THROWF:-442}
S="0-30:1"
for ((f=34; f<=TAPS_TO; f+=6)); do S="$S,$f-$((f+1)):10"; done
S="$S,${DRAG0}-${DRAG1}:${DRAGBITS},${DRAG2A}-${DRAG2B}:${DRAG2BITS},${THROWF}-$((THROWF+1)):10${P1_EXTRA:+,$P1_EXTRA}"
env "${ENVS[@]}" WF_W1=5 WF_THROW=0x2F WF_DBGSEL=1 WF_TRACE=${WF_TRACE:-0x15} WF_P1="$S" \
  ./wfengine --headless --frames "$FR" --drive script "${ARGS[@]}"
