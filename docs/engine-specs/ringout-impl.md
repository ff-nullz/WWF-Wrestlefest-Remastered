# Ring-out area — implementation notes (2026-08-23)

Spec: `ringout-scene.md` (0xF98C switch, count-out, floors, moves 0x69/0x6A/0x6B),
`out-of-ring.md` (exit test, 0x68, scene dispatch), `referee-walk.md` / `pins-referee.md`
(SM3 / visual 3, digit blits). Code: `src/ringout.c` (new), `src/referee.c` (SM3),
`src/motion.c` (scene-2 floors), `src/camera.c` (solo branch), `src/anim.c` out-of-ring
block (exit test, 0x68 trigger, 0x6A/0x6B, 0x69 ringside variants), `src/core.c` (hooks,
human climb-in gate), `src/render.c` (palette selector), `src/ringhw.c` (rope objects off).

## Oracle ground truth

No stock scenario produced a ring-out, so one was hunted with the port's fuzz-bot: seed 2
over `tag.scn` throws the CPU legal man over the left ropes at f2448 (move 0x06 catch ->
react 0x0E, barrier hit -> 0x68 at f2519), fires `$1C1678 = 0x8000` at f2523, shows scene
2 from f2524 and returns to scene 0 at f2880 (count reached 4). The per-frame input bits of
that run were folded into **`tools/scenarios/ringout.scn`** (296 presses) — it replays
deterministically:

```
cd ../wrestlefest-decomp && ./wfport --roms ../wrestlefest/stock --68k --video --no-sdl --no-sound \
  --scenario-file ./tools/scenarios/ringout.scn --frames 2950 --trace X.wfo \
  --dump-frames 2524,2560,2700,2860,2880 --dump-ppm DIR
```
`scan_ringout.py`-style check on the trace: `$1C007F` = 2 from f2524, `$1C1678` = 0x8000 at
f2523 / 0xC000 at f2879, `$1C169A` 1..4 at f2608/2688/2768/2848 (80 frames apart).

Facts read off that trace (all matched by the engine, see "Verified"):

| frame | stock |
|---|---|
| f2524 | scene 2; other legal man -> (0x2A0,0x160,0x140) state 0; non-legal men -> (0x200,0x164,0x100)/(0x428,0x164,0x100) move 0x6B with +0x33 b2 set; faller keeps move 0x6A (re-init); cam (0x190,0x200) |
| f2529 | referee at (0x33F,0x160) `+0x20 = $8003`, pose 9; faller re-seated at (0x1F0,0x168,0x100) |
| f2529-2544 | cam x 0x190 -> 0x150 (= faller x - 0xA0), 4 px/frame; y pinned 0x200 |
| f2536..2624 | referee poses 9,A,B,C every 8 frames, 12 on C (`+0x22` reload 8 / 0xC) |
| f2608 | `$1C169A` = 1 (0x50 frames after the $8003 entry), then +1 every 0x50 |
| f2622 | faller 0x6A -> 0x6B (0x40+0x10+0x08 frames), walks to (0x210,0x120) |
| f2668 | CPU faller: state 1 sub 0x0A walk to (0x310,0x138) with `+0x60 = 0x69` queued |
| f2788 | climb 0x69 starts at (0x309,0x150,0xF0) |
| f2879 | 0x69 ends at (0x309,0x160,0x140); scan 0x19B04 -> `$1C1678 = 0xC000` |
| f2880 | scene 0; legal men at (0x240/0x2C0, 0x150, 0x140) state 0; partners state 1 at y 0x160 with +0x33 b2 clear, re-pinned to the apron x by f2885; referee (0x280,0x198) `$8000`; cam (0x1E0,0x230) |

## Oracle to the 20-count — `tools/scenarios/ringout20.scn` (2026-08-23)

`ringout.scn` only reaches count 4: the man it throws out is the **CPU** legal man (slot 4,
`+0x56` b7 set), and 0x1D74A/0x1C6DC send him home long before 20. To watch the whole count
and the resolve, the man left outside has to be a human. `--fuzz-bot 19` over `tag.scn`
does that (ringout-impl "Turnbuckle" section): P1 (slot 0) dives out of the ring-out view
and is standing on the walkway at f2728 with `+0x33 = 0x07` (legal + outside).

`ringout20.scn` = that run's recorded per-frame input bits (`Trace.input_bits` -> button
names) up to **f2738**, and nothing after, so nobody presses the zone-4 climb-in gate again:

```
cd ../wrestlefest-decomp && ./wfport --roms ../wrestlefest/stock --68k --video \
  --no-sdl --no-sound --scenario-file ./tools/scenarios/ringout20.scn \
  --frames 4260 --dump-frames 3504,4064,4144,4230 --dump-ppm DIR --trace R20.wfo
```

| stock frame | `$1C169A` | what |
|---|---|---|
| f2540 | 0 | scene 2, `$1C1678 = 0x8000`; slot 4 (CPU) is the faller |
| f2624 | 1 | first tick, then +1 every 0x50 |
| f2728 | 3 | slot 0 (human P1) lands outside too — both legal men out |
| f2704..f3424 | 2..11 | slot 4 climbs back in (0x69) and stays in; slot 0 stays out |
| f3504 / f4064 / f4144 | 12 / 19 / 20 | the digit windows read **12**, **19**, **20** |
| f4144 | 20 | resolve: slot 0 `+0xFE = $8003` -> move **0x8C**, its partner slot 1 `$8003`; slot 4/5 `$8002`. "RING OUT" chrome (blit 0x52), human loser -> YM $3108 |

## Engine repro (headless)

`tools/ringout_demo.sh` drives P1 (wrestler 5, gorilla press 0x2F -> react 0x16) through
tie-up -> hold -> stance, drags P2 to the left ropes, flips facing with a 5-frame drag back,
and throws him out at f443 (he lands outside a few frames later):

```
tools/ringout_demo.sh 2400                        # count-out: P2 human, stays out, count 1..20, P2 lose pose at f2184
tools/ringout_demo.sh 2400 WF_P2="2095-2182:1"     # loser WALKING when the 20-count lands (0x11702)
tools/ringout_demo.sh 1500 WF_P2="720-880:1,881-960:4"   # human return: walk right, hold +y on the ring front (zone 4) -> 0x69 -> scene 0
tools/ringout_demo.sh 1500 WF_CPU2AT=600           # CPU return: 0x1C6DC walk to (0x310,0x138) -> 0x69 -> scene 0
tools/ringout_demo.sh 1200 --shot out.ppm          # frame dump (the scene shows from f584)
```
**`TAPS_TO` (2026-08-23):** the default was 250, i.e. the B1 tap ladder kept firing after the
tie-up was won. Since the contact tie-up landed on main (0xF7E8/0xF574) the tie-up starts at
f28 and is won at f95, so the f100 tap fired the gorilla press in mid-ring and the script
stopped producing a ring-out at all. The default is now **94** (the last tap is the winning
one); `TAPS_TO=250` reproduces the broken run.

`WF_TRACE=0x15` prints the referee line with `scene/g161/trig/count/cam`. Engine timeline
(count-out run, at a1dd2ca): tie-up f28, hold 0x15 f95, drag 0x16 f252, throw 0x2F f443,
exit -> barrier hit -> 0x68, scene 2 + referee (0x33E,0x160) SM3 f584, 0x6A -> 0x6B, arrive
(0x209,0x126), **count 1 at f663 and +1 per 80 frames, 20 at f2183** -> `+0xFE = $8003` on
the outside pair / `$8002` inside, P2 lose pose 0x8C on the next frame, match-over stand-in
restarts. (The exact frames drift by a few when the tie-up/hold timing on main changes; the
80-frame spacing and the 20th count are the invariants.)

## Bugs fixed 2026-08-23

**1 & 2 — "only displays up to 11", "when ring out appears the number updates to 21".**
One bug: `digit_blit` read 0x206E6-0x206F6 as "second pass, glyph table entry 1 at dest+8",
so every value 10..19 drew `tens-of-glyph[n]` + a literal "1" (= **11**) and 20 drew
"2" + "1" (= **21**, the frame the RING OUT chrome appears). The ROM's `bra $206BA` re-enters
the row loop with A4/D3 untouched, so the second pass is bytes 8..15 of the **same** glyph —
table entries 0x0A..0x14 are 16-byte, two-digit glyphs (0x207DE = "1"+"0" … 0x2087E = "2"+"0").
Fixed in `digit_glyph(dest, glyph_ptr)` / `digit_blit` (src/referee.c). Side-by-side against
`ringout20.scn` at counts 12/19/20 and the resolve: identical digits in the identical FG0
cells (scratchpad `ringout_cmp.png`, `count_zoom.png`).

**3 — "when my character loses, he slides left to the end of the area".** `0x11702`, the two
instructions at the tail of the walk handler 0x116C6, were not transcribed:
`tst.w (+0xFE,A0) ; bne -> move.w #0,(+0x20,A0)`. Once a result is stamped the pad is dead
(0xDCE8 / core.c `if (o->result) return`), but the polar mover keeps the walker's last
angle+speed, so a man walking at the 20-count glided until the ringside barrier (x 0x1C8)
and, never returning to STAND, never reached the 0x11598 hook or the lose pose. Added to
`handler_walk` in src/anim.c, excluding the apron follow (0x11796 is a separate sub with no
such test). Repro: `tools/ringout_demo.sh 2400 WF_P2="2095-2182:1"` — before, P2 slid from
0x2D1 to the barrier and stayed in state 1 forever; after, he stops at 0x2D1 and takes move
0x8C on the next frame, matching stock (`ringout20.scn` f4144: slot 0 -> 0x8C on the spot).

**4 — `data/romdata/ringout_rules.json`.** Frames per count, warning count, resolve count,
the referee x clamp and their siblings now come from `eng_table_rows("ringout_rules")` via
`ringout_rule()` (src/ringout.c), with the ROM values as compiled-in defaults. Keys/PCs are
listed in the file's `fields` array and in the `RO_*` enum in src/engine.h.

Frames for eyeballing against the oracle (scratchpad `ro_cmp4.png` / `ro_cmp5.png`): same
tilemap, same palette set (2), count digit at the same FG0 cell ($C0748, above the near
post), referee on the apron line, inside man on the far mat, faller on the walkway.

## Exact vs TODO EXACT

Exact (PC-cited in the code):
- 0xF98C both directions: scene bytes 0xFA00/0xFA0A by stage, `$1C0161` b1, object
  re-seating (faller 0x6A re-init + 8 damage + probe-exempt; non-legal 0x6B at 0x200/0x428
  facing the ring; other legal man 0x2A0/0x3A0 by the faller's facing; return positions
  0x240/0x2C0 y 0x150 z 0x140), scroll targets (0x190|0x350,0x200) / (0x1E0,0x230),
  referee teleports, digit wipe, YM 0x80 / 0x20, `$1C15D4` clear.
- `$1C1678`: 0x8000 + facing from 0x68 phase 0 on a legal landing (0x198A0, non-rumble);
  0xC000 from 0x69's ringside end after the 0x19B04 scan (b0 && (f32 b1 || !b2)), 2 men.
- Floors 0x28480 / 0x2851E (every constant, clip/zone bits, the 0x28544 zone-4-only-when-
  pushed quirk), dispatched on `$1C007E` in {2,6}.
- Camera: solo servo on the outside legal man (0x26936), ±4/frame, clamp row 2 (y 0x200).
- Referee SM3 0x1FA82 (midpoint walk, 0x20 jitter / 0x28 stand, speed 0x14) + visual 3
  0x1FD5E (init, x clamp [0x298,0x390], poses 9..C with the 8/0xC reload, tick 0x1FDF4:
  +1 per 0x50, park-at-20 when nobody is out, digit blit 0x2067C with the $C0744 tens window
  and the 0x206E6 second half of the SAME 16-byte glyph, YM $3165.. from 17,
  resolve 0x1FE5A/0x1FEB2/0x1FF02: outside
  pair $8003, inside pair $8002, both $8005 + blit 0x52 + $3108, jingle 0x20156 when the
  loser is CPU, `$1C169E` cleared).
- Loser handling: 0x11598 (`+0xFE` b15 -> `+0xFF` b0 = lose pose 0x8C at 0x115C4, b1 = no
  pose, neither = the winner walk) and **0x11702** (a stamped result drops a walker back to
  STAND so the hook can run) — the apron follow 0x11796 has no such test.
- Timing constants: `data/romdata/ringout_rules.json` (frames per count 0x50, warn 0x11,
  resolve 0x14, ref clamp [0x298,0x390], entry (0x340,0x160), poses 9..C 8/+4, YM $3165,
  freeze 0x1000, faller damage 8, windows $C0744/$C0748, results $8003/$8002/$8005).
- Moves: 0x6A (0x19B58 seat, +0x18 b7 at frame-0 expiry, bchg facing -> 0x6B), 0x6B
  (targets by legality/side, state 0 + f32 b5 clear on arrival), 0x69 ringside init
  (y 0x150, z 0xF0) and end (y 0x160, z 0x140, scan), frame-4 `bclr #2,+0x33`.
- Exit test 0x1B4D4/0x1B4FA (victim's own facing, re-seated x): thrower ender `bchg`
  leaves the victim facing the side he flies to; the engine's reaction init already holds
  the re-seated x.
- Human climb-in 0xEF0A/0xEF6A: legal, ringside scene, zone 4, stick +y (nibble 4) held 4
  frames -> 0x69.
- CPU return 0x1C6DC: `+0xB5` b5 latch, target (0x310,0x138), state-1 sub 0x0A walk then
  0x69 (carried by 0x69's grap44-b7 pre-walk phase in anim.c).

TODO EXACT / simplifications:
- 0x1B8F0 reads `(+0x2E,A2)` with A2 inherited from the reaction dispatcher; treated as the
  victim (A0) like 0x1B4D4. Spec sketch said "thrower facing" — wrong on the oracle.
- Non-legal men at ringside only walk 0x6B to the corner spot and stand (oracle f2694: the
  stock partners brawl at ringside, state 0x0C/0xFF). Valet objects $1C0F1C/$1C1028 absent.
- CPU ringside walker 0x1D74A (issues the return before count 10) is folded into "a standing
  CPU legal man outside goes home at once"; the count-10 rule 0x1C6DC is the documented gate.
- Inside man holding stick -y (nibble 8) on the ring front -> move 0x6C (climb OUT to
  ringside) is untranscribed; the press is ignored.
- `0x90D6` leftover-energy scoring on the count-out result not called.
- 0x1FD88: the referee's `+0x22` at SM3 entry is whatever the previous visual left (oracle:
  5); the engine seeds 8 when it is <= 0.
- Scene-leave keeps the stock positions but re-pins the partners through tag.c's apron
  follow (`apron = 1, sub = 1`); `+0x33` b2 is cleared on them (oracle f2880 shows it clear).
- Stage is always 0 (`st->stage`), so the ring-out scene is 2 and the return scene 0; stages
  1/3/5/8 would pick 6 -> 5 through the same tables.
- Ring hardware objects are not re-created (0x1004A): the engine's rope objects are static
  and are merely not drawn while the scene is 2/6.
- The rumble branch of 0x1987E (`+0x3A`/0x7A elimination) untouched.
- `$1C1682` (0x1FA8C clr) has no engine counterpart.
- `$1C169A` is shared with the pin count in the ROM; the engine keeps the count-out in
  `st->count_out` and the pin half-count in `ref.cell` (both blit through the same 0x2067C).
- The camera's playtest smoothing (camera.c) is bypassed in solo mode (stock ±4).

## Harness additions
- `WF_CPU2AT=frame` hands P2 to the CPU at that frame (main.c).
- `WF_TRACE` object line now shows `fc=` facing, `f33=`, `zn=`; the referee line shows
  scene / g161 / trig / count / cam.
- `baseline.txt` regenerated (eng_state grew: ringout fields, ref t22/t24, obj tgt_y/ai_b5).

## Turnbuckle from inside the ring-out view (2026-08-23)

Oracle: `--fuzz-bot 19` over `tag.scn` — ring-out at f2540, P1 (slot 0, the inside legal
man) pushed on the left wall (x 0x26E, zone 1, stick left) climbs at f2572 (`$1C1670` =
0x100, `+0x44` = 8), perches at f2665, dives 0x1B at f2728 onto the outside man, lands on the
walkway floor (z 0x100) at f2798, stands outside at f2816, climbs back in through the zone-4
gate at f3122 and the scene returns at f3215. Frames dumped at 2590/2700/2740/2800 (scratchpad
`cl_cmp_climb/perch/dive/land.png`, engine on the left).

| stock (seed 19) | PC | engine |
|---|---|---|
| gate: ringside scene, no corner bit, zone&3, any stick — x < 0x271 -> corner 8, x >= 0x3CF -> corner 9; no 4-frame hold | 0xEDEA -> 0xEE96, claim 0xEECE | core.c walk_logic (before the 1v1 windows) |
| climb 0x12122: (0x278,0x160) / (0x3C8,0x160), z 0x140, facing = (corner&1)<<7 (corner 8 faces LEFT = the post/outside), cell 0x11EE6 (66 67 31..37, 7 x 0xC); frame-1 expiry: x -/+ 0x18, z 0x180, facing flipped (faces INTO the ring); `+0x12` untouched | 0x12122-0x121C4 | anim.c handler_climb (`rs` branch) |
| perch 0x121E6: (0x268,0x158) / (0x3D8,0x158), z 0x180, spr 0x30 (0x1227E[c&3]), timer 0x80 -> 0xA | 0x12202-0x1224A | handler_perch |
| climb-down ringside: cell 0x122C2, `bclr #2,+0x33` at entry and end, z 0x140 at frame 4, end x +/- 0x18, list 0, corner long cleared | 0x12304, 0x123D0-0x1241C, 0x12442 | handler_climbdown (`rs` branch) |
| dive select 0xE27A: ringside -> `bset #2,+0x33` on the DIVER (he is an outside man from now on); opp lying 8/9 or held 0x61 -> cat F, else cat E | 0xE288-0xE2D8 | core.c state-9 selector |
| dive 0x1B/0x0D 0x153BC: grap44 = f34 b3 ? 0 : victim 0x53 ? 1 : 2; case 2 faces the victim by x (bit7 = right); homing 0x26AE on victim.x +/- 0x10 ahead of own facing; lands on whichever floor `+0x33` b2 selects — the walkway z 0x100 in the ring-out view; hit case 2 needs the victim outside when the scene shows (0x15562); FE -> state 0 OUTSIDE, no 0x68 | 0x153C6-0x15456, 0x15562, 0x15648 | handler_topdive |
| AI top-rope intent | 0x1F1AC first test `btst #1,$1C0161; bne` -> never climbs while the ring-out view shows | nothing to transcribe |

Engine timeline (`tools/ringout_demo.sh 1300 P1_EXTRA="600-700:2,780-781:10"`): P1 walks
left from f600, wall at 0x26E f623 -> state 8 at (0x278,0x160) f624 facing L, (0x260, z 0x180)
facing R from the second rung, perch (0x268,0x158) f717, B1 at f780 -> `sel: cat=E entry=1B`,
f33 0x47, dive facing L toward P2 (0x209), lands (0x1FF,0x113,0x100) f850 -> P2 react 0x20,
P1 stands outside f870; both legal men are now outside so the count runs to a double
count-out unless one climbs back in (zone-4 gate). Oracle ref: perch 0x268/0x158 fc 0x80,
dive vx -0x3D vy -0xC0 vz 0x680, landing z 0x100 at f2798 — same shape.

TODO EXACT (turnbuckle in the view): the perch sprite facing is stock (into the ring) — the
user's "faces outward" is what the first climb rung shows and what the dive init does when
the victim is on the outside; 0x26AE's `+0x45` temp offset applied through the victim's x
like the ROM; climb-down frame-4 z drop taken from 0x123D0 but the tag cell's `+0x12` clear
(0x12448) is applied at entry (one tick early).
