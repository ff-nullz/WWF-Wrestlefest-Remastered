#!/bin/sh
# Mod-effect gate: every gameplay rule with a headless drive and the effect it
# must produce. tools/trace_gate.sh proves nothing CHANGED; this proves the
# mods still DO something after the engine under them moves.
#   tools/mod_gate.sh          -> PASS/FAIL per rule, exit 1 on any FAIL
# Drives avoid tap timing where a rule changes animation speed (turbo) and
# use the CPU or a thrown/perched man instead. WF_MODRULES overrides the
# rules without a profile; WF_HP1 lowers P1 so a gain is visible.
cd "$(dirname "$0")/.." || exit 2
B=./wfengine; fails=0
S="0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10,100-101:10,110-111:10"
P=""; for f in $(seq 90 12 400); do P="$P,$f-$((f+1)):10"; done
run() { # name, expectation (grep -E on stderr), then the command
  n=$1; e=$2; shift 2
  if "$@" 2>&1 | grep -Eq "$e"; then echo "PASS  $n"; else echo "FAIL  $n  (wanted /$e/)"; fails=$((fails+1)); fi
}
# parasitic_pct: the thrower takes what the bodyslam took (14) -> hp 64
run parasitic_pct 'parasitic - o0 takes 14 from o2 \(hp 64/' env WF_HP1=50 WF_THROW=0x24 WF_MODRULES="parasitic_pct=100" WF_DBGSEL=1 WF_P1="$S" $B --headless --frames 300 --drive script
# human_hit_mult: a human's tie-up knee does 3 (stock 1) -> the CPU victim shows hp 103 after one knee... use the throw: 14 x 3 = 42 -> 64
run human_hit_mult 'lying: P3 react=8 hp=64 ' env WF_THROW=0x24 WF_MODRULES="human_hit_mult=3" WF_DBGSEL=1 WF_P1="$S" $B --headless --frames 300 --drive script
# ko_dq: x9 damage on the slam (126 > 106) knocks the victim out -> his team loses
run ko_dq 'KNOCKED OUT - his team loses' env WF_THROW=0x24 WF_MODRULES="human_hit_mult=9,ko_dq=1" WF_DBGSEL=1 WF_P1="$S" $B --headless --frames 300 --drive script
# turbo: the punch cell chain runs in fewer than 30 frames (stock 38)
t=$(env WF_MODRULES="turbo=5" WF_X1=0x1C0 WF_Y1=0x150 WF_TRACE=1 WF_P1="2-3:10" $B --headless --frames 80 --drive script 2>&1 | grep -c 'st=8005')
if [ "$t" -gt 0 ] && [ "$t" -lt 30 ]; then echo "PASS  turbo (punch $t frames)"; else echo "FAIL  turbo (punch $t frames, wanted < 30)"; fails=$((fails+1)); fi
# extended_moves: a scripted swing topples the perched man, he lands on the mat and gets up (state 7 at z 0x140)
run extended_moves 'down from the buckle - top-rope bit cleared' env WF_MODRULES="extended_moves=1" WF_X1=0x300 WF_Y1=0x197 WF_DBGSEL=1 WF_P1="0-120:4" WF_P2="0-36:5${P}" $B --headless --frames 400 --drive script
run extended_moves_getup 'tr f0[0-9]+ o0 \([0-9A-F]+,[0-9A-F]+,140\) st=8007' env WF_MODRULES="extended_moves=1" WF_X1=0x300 WF_Y1=0x197 WF_TRACE=1 WF_P1="0-120:4" WF_P2="0-36:5${P}" $B --headless --frames 400 --drive script
# toprope_out: the toppled man lands OUTSIDE (move 0x68 lying outside)
run toprope_out 'tr f0[0-9]+ o0 .* mv=68 ' env WF_MODRULES="extended_moves=1,toprope_out=1" WF_X1=0x300 WF_Y1=0x197 WF_TRACE=1 WF_P1="0-120:4" WF_P2="0-36:5${P}" $B --headless --frames 400 --drive script
# exit_ring: hold into the near rope -> the hop, then the outside walk (move 0x6A)
run exit_ring 'P1 hops out of the ring' env WF_MODRULES="exit_ring=1" WF_X1=0x2E0 WF_Y1=0x190 WF_DBGSEL=1 WF_P1="0-200:1" $B --headless --frames 300 --drive script
run exit_ring_lands_outside 'tr f0[0-9]+ o0 .* mv=6A ' env WF_MODRULES="exit_ring=1" WF_X1=0x2E0 WF_Y1=0x190 WF_TRACE=1 WF_P1="0-200:1" $B --headless --frames 300 --drive script
# exit_ring_climb: the climb-out (move 0x7A) instead of the hop
run exit_ring_climb 'P1 climbs out of the ring' env WF_MODRULES="exit_ring=1,exit_ring_climb=1" WF_X1=0x2E0 WF_Y1=0x190 WF_DBGSEL=1 WF_P1="0-200:1" $B --headless --frames 300 --drive script
# throw_out: the slam's victim rides the arc over the ropes (W1=2: Hogan's 0x24 is his suplex, excluded)
run throw_out 'throw_out - o2 over the ropes' env WF_W1=2 WF_THROW=0x24 WF_MODRULES="throw_out=1" WF_DBGSEL=1 WF_P1="$S" $B --headless --frames 400 --drive script
# backup run-ins: a wrestler runs in within ~25 s
run backup_runin 'BACKUP w[0-9]+ runs in' env WF_MODRULES="backup_avg_secs=2" WF_CPU2=1 WF_DBGSEL=1 $B --headless --frames 1500 --drive script
# ref_knockdown: P1 punches the referee walking past (WF_REFDBG=1 prints the per-frame distance)
run ref_knockdown 'ref: KNOCKED DOWN' env WF_MODRULES="ref_knockdown=1" WF_X1=0x240 WF_Y1=0x198 WF_DBGSEL=1 WF_P1="2-3:10,14-15:10,26-27:10" $B --headless --frames 60 --drive script
# stacking: the vampire-turbo profile carries BOTH rules (CPU vs CPU: parasitic lines; and a fast punch)
run stack_parasitic 'parasitic - o0 takes' env WF_HP1=50 WF_CPU1=1 WF_CPU2=1 WF_DBGSEL=1 $B --headless --profile vampire-turbo --frames 1500 --drive script
t=$(env WF_X1=0x1C0 WF_Y1=0x150 WF_TRACE=1 WF_P1="2-3:10" $B --headless --profile vampire-turbo --frames 80 --drive script 2>&1 | grep -c 'st=8005')
if [ "$t" -gt 0 ] && [ "$t" -lt 34 ]; then echo "PASS  stack_turbo (punch $t frames)"; else echo "FAIL  stack_turbo (punch $t frames)"; fails=$((fails+1)); fi
# stock rumble DOUBLE PIN (user 2026-09-05 "the double pin doesn't work"): WF_PINAT=3 imposes a
# cover (o4 on o5, the victim nearly done) 0x30 px beside P1 parked at (300,190), away from the bell
# seats; B2 at f9 JOINS the cover and P1 lands on the pile as the react-5 faller. (Was a pin the CPU
# sim happened to form beside P1 - it died with the first AI change.)
run double_pin_join 'rumble: P1 joins the cover \(double pin\)' env WF_RUMBLE=1 WF_CPU2=1 WF_X1=0x300 WF_Y1=0x190 WF_PINAT=3 WF_DBGSEL=1 WF_P1="9-10:20" $B --headless --frames 90 --drive script
run double_pin_lands 'tr f00[2-8][0-9] o0 .* st=8004 .* rc=05 ' env WF_RUMBLE=1 WF_CPU2=1 WF_X1=0x300 WF_Y1=0x190 WF_PINAT=3 WF_TRACE=1 WF_P1="9-10:20" $B --headless --frames 90 --drive script
# cage escape (mode cage_escape_win, cagematch profile): hold DOWN into the bottom wall ~2 s ->
# the front-wall climb (pose 277 hand over hand, 37/33/35 over the top), the drop, the WIN
run cage_climb 'mod: P1 climbs the cage' env WF_STAGE=2 WF_X1=0x280 WF_Y1=0x120 WF_DBGSEL=1 WF_P1="0-140:8" $B --headless --profile cagematch --frames 200 --drive script
run cage_escape 'ESCAPES over the front of the cage' env WF_STAGE=2 WF_X1=0x280 WF_Y1=0x120 WF_DBGSEL=1 WF_P1="0-140:8" $B --headless --profile cagematch --frames 420 --drive script
echo "mod-gate: $fails failure(s)"; [ $fails -eq 0 ]
