# CPU opponent behaviour — what `src/ai.c` transcribes (v2, 2026-08-23)

Companion to `ai-core.md` (the read-only transcription of 0x1C150–0x1F760).
This file says what the engine actually does with it, which tables it reads
from `wf.rom[]`, what is exact, what is `TODO EXACT`, and how the engine's
CPU compares with the oracle's.

Architecture: **virtual controller**. The 68k AI writes `+0x61` and jumps into
the move-prep dispatcher 0x1DFE2; the engine keeps every *decision* (tables,
rolls, thresholds, targets) and turns the result into joystick/button bits for
`core.c walk_logic`, so the move rules stay the human ones. The exceptions —
state written directly, exactly where the ROM writes it too — are listed in §7.

## 1. Per-frame entry (`eng_ai_inputs`, 0x1C15C/0x1C1D2)

| ROM | engine |
|---|---|
| 0x1C16E: CPU (+0x56 b7) or autopilot (+0x56 b6) men | `o->cpu`, or an autopilot man inside after a run-in (`ai_sub == 1`) — core.c hook |
| 0x1C228 range_linked: dx/dy, bucket 0x50/0xB0, opp right/below | `geometry()` |
| 0x1C1F4 state dispatch | `switch (state)`: 0 `stand`, 1 approach (+pseudo subs), 2 `run_tick`, 5 victim/stance, 8 clr +0xBD, 9 `perch`, 0xB `lockup`, 0xC cascade |
| 0x1108C pin intent (jsr'd by the ROM's knockdown enders) | rolled once per fresh knockdown of the opponent: `rng&0xFF < 0x110C8[id]` — a **WORD** table (0x110B0 `lsl #1`); the old byte indexing was wrong |

Button presses are latched for 2 frames (`press()`): the 0x514E digest only
keeps a press that lands on an odd frame, and the run steer 0xF34C reads the
stick on the tick after the state latch (runs hold 3 frames).

## 2. Stand / approach (0x1C2CC, 0x1C93E) — EXACT thresholds

- Stand entry clears `+0xB5 &= 0xCB, +0xB6 &= 0xF1, +0xB7 b1/b5` (0x1C2DC).
- 0x1C348 pin intent → cover (`queue_move 0x48`; ROM move 0x79, see §8).
- 0x1C370 run-attack pending (+0xB7 b0) → fire 0x72.
- 0x1C430 follow-up armed (+0xB5 b3): cleared when the opponent is not
  grapple-ready (0x1C44C); once per stand, back-to-the-ropes (x-zone <3 facing
  right / >=9 facing left) rolls `0x1F3BE` row 0 {20,80} → run away (0x1C462);
  0x1C4E4 once: policy2 `0x2307C[id][dx<0x68 ? 0 : dx<0xB0 ? 1 : 2]`, ctx 0
  resolves now, else the move is queued and a grapple-ready opponent makes us
  run at him; 0x1C5B6 fires the queued move when facing each other and
  `dx < +0xBC`.
- Approach NEAR (dx<0x50 && dy<0xC): opp standing/getting up → policy ctx 0;
  opp in a move: grapple-ready & facing us → policy2 ctx 0 if his move is one
  of 05/2A/2D/20/41 else counter 0x75 (B1); doing 0x4C → wait 0x3D; engaged →
  grab (B1). Opp react 0/0xA → ctx 0; lying 8/9 → top-rope eval (§4), else
  policy ctx 2/3; hp 0 → cover; react 1 → ctx 5 (facing) / 7 (same facing);
  any other react (still falling) → 0x3D wait with +0xB7 b6.
- Idle (policy 0xFF, +0xB9 b1): keep walking at the opponent while he stands /
  walks / is dizzy (0x1CAD6 → 0x1D01A). Against a motionless human this is the
  stock ±2 px jitter at his x (oracle f4994–4998); the harness scripts tap P1's
  stick every 100 frames to get past it.
- ALIGN (dy>=0xC) / mid range: 8-way diagonals (0x1CD6E); FAR aligned:
  bucket 1 & opp grapple-ready & facing → dx>=0x68: policy ctx 0 if he is in
  2D/20/41 under 0xB0, else dash 0x3C; dx<0x68: policy ctx 0 if 05/2A/2D/20/41
  else 0x75. Bucket 2 & opp grapple-ready → `0x1D030` {70,30}: 30 % run away.
  Opp dizzy at range → ctx 6. Else run-eval (§3).

Policy records (0x1DEA6): `[n][ids][w0][w1][w2]`, row by the **opponent's**
band, forced to row 2 under 10:00 (`st->clk_min < 0x10`). Result codes:
0xFF idle, 0xFE → 0x1DF1E (opp near the edge → rope-line walk sub 5, **TODO
EXACT**: treated as an 8-frame pause; else 0x3E), 0xFD nothing, 0x3E
pursue-reposition, 0x3C/0x3D pseudo-moves, 0x75 counter, anything else a real
move through `queue_move`.

## 3. Running (0x1F310, 0x1D7B6) — EXACT rolls, stand-in moves

- `run_eval`: dx>=0xE0 (inside only), once: `0x1F3BE[id]` → run AWAY (stick
  away + both buttons; +0x32 b2 mirrored as `ai_b6` b5 so the run rides the
  ropes untouched); 0xC8<=dx<0xE0, once: `0x1F3A6[id]` → run at him.
- Run phase 0 (0x1D94C): dy<8, once: policy ctx 8 → move id + gate from its
  prep handler (§6). Phase 1 (0x1D814): facing each other, dy<0xA, dx<0xA0,
  once: opp grapple-ready or doing move 3 → `0x23B26[id][min(stage,5)][band]`
  → 0x75 gate 0x50; else `0x238CE[...]` → 0x72 gate 0x48 (+0xB7 b0: fired from
  the next stand, 0x1C370). Fire when dx < gate.
- Break off (0x1D9F0/0x1D9F8 → state 3): opponent lying, engaged, +0x32 b1,
  doing 48/1A/23, or in a tie-up/hold → the stick is reversed (0xF42E skid).
- **TODO EXACT**: 0x72/0x75 out of a run are B2/B1 of the engine's cat 1/2
  row (the human matrix 0xE070), not the ROM's own 0x72 (0x19FC6) / 0x75
  (0x1A08C) records.

## 4. Top rope (0x1F1AC, sub 9 0x1D31E, state 9 0x1EEA0) — EXACT

Conditions: not the ringside scene, +0xB6 b0 clear, opponent band != 0, not
autopilot. Corner from his x/y by his facing (the 0x1F1D8/0x1F290 boxes,
transcribed literally), roll `0x1F2E0[id]` (first pair when his band low byte
== 1, else the second) → `+0x9A += 0x100` on him (direct write, 0x1F222; the
0x1F228 +0xAB bump is TODO EXACT — the engine meter counts presses), sub 9.
Sub 9 walks to `0x1D388[c]` = (1F8,1A0)(300,1A0)(1D0,120)(320,120); arrival
(|d|<8) with the corner free and him still down → corner claim + state 8
(direct, as 0x1D31E → 0xEECE does; the human 0xEDC0 gate is bypassed by the
ROM too). State 9: +0xBD to 8, then B1 — `0x1EF9E[id]` (standing) and
`0x1EF92[id]` (lying) are byte-for-byte the cat E/F B1 columns of 0xE4FE, so
the press reproduces the pick; ids 4/7/0xB roll `0x1EF90` {40,60} for move 1
(w4 = cat E B2; w7/wB have no column → B1, TODO EXACT).

## 5. Tie-up, hold, victim

- 0xB lockup (0x1EFAA): seed `0x20` vs a CPU partner, `0x1F060[stage]` vs a
  human, `+ rng&3`; countdown, reseed, unless +0x44 b14 → `0x1F05E` {50,50} →
  B1 (cat 8 knee 0x29).
- 0xC hold won (0x1F078): cascade `policy[id]` slot 1 → `[facing]` →
  `[x-zone 0/4/8]` → record, band row from the grapple partner (10-min rule),
  rescue mode → `0x1F150[id]`. 0x17 → B2 (whip), else B1 (facelock) and in the
  stance the column whose 0xE232-resolved throw row holds the cascade's move
  (unrouted cells skipped). x-zone +0x31: **TODO EXACT** (writer not found;
  0x20-px columns from x=0x1A0).
- Victim (0x1DA7E family, `victim_escape`): a CPU victim never mashes
  (0x10D04); once per hold it rolls the attacker-move table —
  cover 0x23D7E[stage], 10/46/0B 0x23F78, 09/22 0x23FC4, 11/0C/12/0F
  0x23DD0, 0x0A 0x23E76 (context counter +0xD2/+0xE5 = 0, TODO EXACT), the
  0x1DE54 list 0x23E16 — by own band; a win arms a 2/0x10 countdown, at 0 the
  engine's mash meter is written to 0x4000 and B1 pressed (the ROM enters
  0x38/0x78 directly). Engine guard: a lost roll escapes after 0xC0 frames
  anyway because the engine's hold loops have no 0x1254C timer.

## 6. Move prep handlers 0x1E018 (the spacing) — EXACT targets/gates

| rows | handler | engine |
|---|---|---|
| 0x3C | 0x1E42A (bit31: every frame) | `pseudo_3C`: keep closing while opp grapple-ready & facing, else stand. The 0x18028 record's own walk/y-seek is TODO EXACT (stick walk instead) |
| 0x3D | 0x1EBCE (every frame) | target 0x1F4CE (opp.x ∓ 0xA0 by his facing, y+0x10) via sub 0xA; clipped = arrived (0x1D6CA/0x1D728); then wait: +0xB7 b6 = until the falling man lands / stands (0x1EC8A), +0xB5 b4 = while he lies (0x1ED62) |
| 0x3E | 0x1DF7E + 0x1831A | ROM teleports x by ±(0xB0-dx+0x10) then walks; engine walks away until dx>=0xC0 or clipped — TODO EXACT |
| 0x75 | 0x1E25E | B1 now (engine cat 0 grab / cat 3-4 anti-run) |
| 0x00/0x21 | 0x1E2F6 | opp grapple-ready → gate 0x70 armed; else runs now |
| 01/03/07/27/2E/31/06 | 0x1E6FE–0x1E75C | gates 0x80 / 0x70(-8,-8 by opp id) / 0x70 / 0x60 → follow-up armed, fire at 0x1C5B6 |
| 2D/20/41/2A/40/04/05 | 0x1E4DA–0x1E568 | gates `0x1E58E[opp]`, `0x1E582[opp]`, 0x5C(+4 vs id 7), 0x60, 0x70, 0x78/0x80(-6 for id 8); −0x22 when the opponent is not running |
| 0x08 | 0x1E5F6 | room on the head side (0x260/0x290) → walk-to 0x1F4CE then B1 (engine box 1 pickup); else stomp spot |
| 14/46/10/13 | 0x1E62C | same with 0x250/0x2A0 |
| 0A/47/0B | 0x1E664 → 0x1F45A | beside him: x = opp.x ± 0x48 (0x30 if not face-up, y−0x18; pinned: ±0x20 / 0x1A variant, y toward 0x160) |
| 0x48 | 0x1E6A4 | illegal man → 0x0A; else 0x1F45A then B2 |
| 09/22 | 0x1E5AE | opp x in [0x200,0x300) → 0x1F51E (8 px head side) else 0x1F45A |

Sub 0xA walk-to (0x1D5A2/0x1F15C): 8-way steer until |dx|<8 && |dy|<8 (the
ROM's atan 0x20C8 is TODO EXACT), abort when the downed target is up
(0x1D702), boxed in → stomp spot (0x1D6CA). Engine guard: 0x100 frames.

## 7. Tag side: rescue and run-in

**PINS: see `docs/engine-specs/pin-partner.md`** — a cover arms BOTH partners
(0x133E2 `jsr 0x215B6` on the pinner, 0x133EA `jsr 0x21732` on the victim,
`+0xE6 = 1` on each), hands the pinner's pad to his own partner (0x21618) and
takes it back at 0x212D4; the run-in plays move **0x4E** (0x18AA0, 32 frames on
the ropes) instead of teleporting, and the break is reaction handler 9
(0x24BC2/0x24C0C) on hit record 0x1D, not a direct write. The notes below now
only describe the STANDING-hold rescue.

- Arming (0x2172C family, simplified to what the engine exposes): an
  autopilot apron man whose legal teammate is the `partner` of a state-0xC man
  who is NOT pinning arms with `0x2172C[band]` (hold, words 5D 7D BB); pin/hold
  gone → disarm (0x212A0/0x213A6). Pins arm explicitly from the cover instead.
- Countdown 0x1D526 (`eng_ai_rescue_tick`, called from `eng_apron_tick`): at 0 →
  autopilot (unless he is carrying a pad), disarm, usher grace `0x177` from
  `tag_rules.json`, `state 5 move 0x4E` — the ROM's own climb-in.
- Run-in 0x1D398 (`runin`): follow the teammate's y (|dy|>=8); a *pinning* man →
  walk to his front side and fire the stomp 0x0A (the real break); a *standing*
  holder within 0x40 → the old 0x213A6 stand-in direct write + B1;
  vs the teammate's standing opponent within (0x40,0x10), once: `0x1D4FC
  [stage]` 3-way: 0 strike, 1 behind grab (unrouted → B1), 2 nothing.
  After the grace expires `eng_tag_usher_tick` sets the recall (f34 b1) and
  tag.c's walk-out/0x4F climb takes him back to the apron (the referee usher
  walk 0x202BA is still TODO EXACT). The ROM's run-in also *runs* when
  |dx to teammate| >= 0xA0 (0x1CFC4) — the engine walks.

Core hook: an autopilot man inside is fed by the AI (and goes through
`walk_logic` instead of `eng_apron_tick`) for the whole usher grace — firing a
move clears `ai_sub`, so the gate is `ai_sub == 1 || ai_runin_t`.

## 8. RNG 0x21B4 and difficulty

- `eng_rng_fold(known)`: `s = $1C005C + known + $1C0080; ror.l s&7; store`.
  `$1C005C` starts at 0 (never seeded), `$1C0080` = `st->frame`
  (`eng_ai_frame`). Known registers folded: the 0x24CC retry counter D1
  (3..0). Everything else in D1–D7 is caller garbage → K=0 (rng-lockup.md
  §1b). `eng_rng()` = fold(0) for tieup.c.
- `st->stage` = `$1C0162` (word; the ladder bumps byte `$1C0163` at
  0x1A76/0x1AA4). Default 0, harness poke `WF_STAGE=n`. Read by: 0x1F060
  lockup seed, 0x1D8C2 run rolls (min 5), 0x1D4FC run-in roll, the victim
  escape tables, and tieup.c's 0xF878 + stage*8 bias rows. No DIP switch is
  read inside the AI: the DIP difficulty shapes the *ladder* (which stage you
  are on) and the tie-up bias rows, not the AI tables directly.
- Moves the ROM names differently: the pin-intent cover is 0x79 (0x1A1BE),
  routed here to the engine's 0x48 cover; 0x72/0x75 see §3.

## 9. Unrouted-cell guard

`routed()` holds the gap-map's unrouted ids (09 32 89 25 3B 1A 36 1C 1D 1E 39
23 2E 31 3F 45 49 54 64 6C 7A 8D 3C 3E 50 61 6B 6E 6F 7B 88 8E 7D 80–87).
`fire_move` resolves the category the way walk_logic will (`engine_cat`) and
withholds a press that would land on one of them (`ai: withheld`), so the AI
never produces `UNROUTED`. This is an engine restriction, not ROM behaviour.

## 10. Oracle comparison (tag.scn, CPU slots 4/5 from f3502)

`osum.py` over `wfport --scenario-file tools/scenarios/tag.scn --frames 5000`
(ids 9/8 as the CPU pair; engine: id 1 vs idle P1 id 0 —
different roster and RNG, so timings are compared, not frames).

| oracle | engine |
|---|---|
| f3503–3527: walk-in 24 f to dx 0x4F, then move 0 (grab) | f1–f23: 22 f to dx 0x4E, move 0 (policy ctx 0) |
| f3571 tie-up → 0x0C f3794 → whip 0x17 at f3843 (+0xBC 0x30 prep wait) | cascade → B2 → 0x17 the frame the hold window opens (the 0x30 prep wait lives in the engine's hold phase) |
| f3896 queued move 3 with +0xBC 0x68, fired at f3938 (0x1C5B6) | same gate 0x68 (0x1E70E: 0x70−8), fired when dx < gate |
| f4292 pin-intent cover 0x79 after the getup | `pin intent` roll at the knockdown, cover 0x48 queued at the next stand (ai_top.sh: 3 per 3000 f) |
| f4405 pickup 0x08 via sub 0xA (walk to 0x1D9 = opp.x−0xA0) | `target_1F4CE` → sub 0xA → B1 (box 1) |
| f4557 rescue 0x4E (partner 0x33D → 0x305), f4596 state 2 run, sub 0xA, 0x72 at f4738 | `rescue armed 5D/3E` → `run-in` (x+0x38) → walks to the pinner → `run-in break` + B1 → recall at +0x177 |
| f4898 move 0x3C, 63 frames, y 0x151→0x175 | `dash 3C`: stick walk while he is grapple-ready (no y-seek) |
| f4994–4998 ±2 px jitter at the opponent's x (idle) | same (`policy FF` → walk at him) |
| top rope not reached in this scenario | seeds 3/5/8 of `ai_top2.sh`: `toprope corner` → `climb` → `dive col` |

Deltas: the engine never teleports for 0x3E; the run-in walks instead of
running; 0x72/0x75 are engine-row stand-ins; the stock idle is unchanged.

## 11. Seeing it headlessly

```
# decisions, every ROM roll/pick logged as `ai: oN ...`
WF_CPU2=1 WF_DBGSEL=1 ./wfengine --headless --frames 1500 --drive script 2>&1 | grep '^ai:'
# run-eval: P1 starts at the left rope (far) — run-in / run-away / run policy / run 72
WF_X1=0x1E4 WF_CPU2=1 WF_DBGSEL=1 ./wfengine --headless --frames 400 --drive script 2>&1 | grep '^ai: o2 run'
# rescue run-in: P1 pins the idle P2, P2's partner (o3) comes in and breaks it
WF_DBGSEL=1 ./wfengine --headless --frames 2500 --drive pin 2>&1 | grep '^ai:'
# top rope: hurt P1 (band 1) under fuzz; corner -> climb -> dive
WF_SEED=8 WF_HP1=0x30 WF_CPU2=1 WF_DBGSEL=1 ./wfengine --headless --frames 3000 --drive fuzz 2>&1 | grep 'toprope\|climb\|dive'
# difficulty: stage 7 tables (lockup seed 0x1F060[7] = 4, run rolls row min(7,5), tie-up bias row 7)
WF_STAGE=7 WF_CPU2=1 WF_DBGSEL=1 ./wfengine --headless --frames 3000 --drive fuzz 2>&1 | grep '^ai:' | sort | uniq -c
# per-frame: WF_TRACE=4 now prints ai=<sub>/<mv>
```
Pokes added: `WF_STAGE`, `WF_X1`, `WF_Y1`, `WF_HP1`; `WF_CPU2` is re-applied
every frame (a decided match re-inits the state) and clears +0x33 b1.
