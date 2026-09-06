#!/bin/sh
# Trace gate for behaviour-neutral refactors (rename pass 2026-09-05).
#   tools/trace_gate.sh BIN OUTDIR   - run the fixed drives with full per-frame traces (WF_TRACE=0x1F)
#   Save a set from the binary BEFORE the change, another AFTER, then cmp the files:
#     cp wfengine /tmp/wf.ref; tools/trace_gate.sh /tmp/wf.ref /tmp/ref; ./build.sh; tools/trace_gate.sh ./wfengine /tmp/new
#     for f in /tmp/ref/*.txt; do cmp -s $f /tmp/new/$(basename $f) && echo SAME $f || echo DIFF $f; done
# Unlike make regress (a hash of the raw eng_state bytes) this survives struct layout changes.
B=$1; O=$2; mkdir -p "$O"; cd "$(dirname "$0")/.." || exit 2
S="0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10,100-101:10,110-111:10"
P=""; for f in $(seq 90 12 400); do P="$P,$f-$((f+1)):10"; done
WF_TRACE=0x1F $B --selftest --frames 1200 2>"$O/selftest.txt" >/dev/null
WF_TRACE=0x1F WF_CPU2=1 WF_X1=0x300 WF_Y1=0x197 WF_P1="0-120:4,300-1200:2" $B --headless --profile superstars --frames 1200 --drive script 2>"$O/topple_cpu.txt" >/dev/null
WF_TRACE=0x1F WF_MODRULES="extended_moves=1" WF_X1=0x300 WF_Y1=0x197 WF_P1="0-120:4,300-900:2" WF_P2="0-36:5${P}" $B --headless --frames 700 --drive script 2>"$O/topple_mat.txt" >/dev/null
WF_TRACE=0x1F WF_THROW=0x24 WF_CPU2=1 WF_P1="$S" $B --headless --frames 900 --drive script 2>"$O/throw_cpu.txt" >/dev/null
WF_TRACE=0x1F WF_RUMBLE=1 WF_CPU2=1 WF_P1="0-40:1,60-61:10,100-400:2,420-421:10" $B --headless --frames 1500 --drive script 2>"$O/rumble.txt" >/dev/null
WF_TRACE=0x1F WF_MODRULES="exit_ring=1" WF_X1=0x2E0 WF_Y1=0x190 WF_P1="0-200:1" $B --headless --frames 400 --drive script 2>"$O/exit_ring.txt" >/dev/null
WF_TRACE=0x1F WF_HP1=50 WF_THROW=0x24 WF_MODRULES="parasitic_pct=100,turbo=4" WF_P1="$S" $B --headless --frames 400 --drive script 2>"$O/parasitic_turbo.txt" >/dev/null
WF_TRACE=0x1F WF_CPU1=1 WF_CPU2=1 $B --headless --frames 2500 --drive script 2>"$O/cpu_vs_cpu.txt" >/dev/null
for f in "$O"/*.txt; do sed -i '/^tr /!d' "$f"; done   # trace lines only: log chatter (package loads, mod: lines) must not trip the gate
wc -l "$O"/*.txt | tail -1
