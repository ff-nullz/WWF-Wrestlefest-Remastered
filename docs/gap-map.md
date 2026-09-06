# wfengine gap map — what stock WrestleFest does that the engine does not

Surveyed 2026-08-22 by four read-only explorers against HEAD `f004865`
(ROM: `reference/maincpu.asm`). Each row
cites the ROM PC. "FREEZE" rows are now caught by the generic soft-lock
guard in `src/anim.c` (unrouted record on a FF00 hold / mode-0 / loop
bails after 0x40 frames) — they whiff instead of locking, but they are
still wrong until transcribed.

## 1. Moves (table 0x12614, ids 0x00..0x8E — 0x8F is NOT a valid id)

Routed: 57 ids (`case 0xNN:` in `eng_anim_tick` + 0x00/0x3D/0x72 via the
cell-handler switch). Unrouted: 85. Ones reachable from a live 0xE4FE
selector cell (a real button press) first:

| id | record | handler | what | player-reachable from |
|---|---|---|---|---|
| ~~01~~ | 128DE | 128FA | **DONE** `handler_move01` (0x1C launch class when +0x1F==2 TODO) | cat1 w4 B2, w7; cat2 w4 B2, w11; cat4 w4; catE w4 B2 |
| ~~02~~ | 129F0 | 12A08 | **DONE** `handler_throw02` | catB w4, w7 (all cols) |
| ~~11~~ | 14658 | 14670 | **DONE** `handler_topdrop11` | catF w11 |
| ~~12~~ | 147E6 | 14826 | **DONE** `handler_moonsault` | catF w9 |
| ~~23~~ | 15CE0 | 15D08 | **DONE** `handler_quake23` (Earthquake's rope-run sit-down splash → cover, victim move 0x64 `handler_pinned64`) | cat7 w8 B1 |
| ~~26~~ | 1623C | 16258 | **DONE** piledriver (`handler_piledriver`) | catA w5 B2; catC w1, w9; catD w3 |
| ~~28~~ | 1641E | 16436 | **DONE** lifting slam (`handler_liftslam`) | catC w11; catD w4 B2 |
| ~~2B~~ | 1667A | 1669A | **DONE** `handler_throw2B` | catC w7; catD w4 |
| ~~2E~~ | 16A56 | 16A7A | **DONE** `handler_tackle` + result 0x0A (hit.c) | cat4 w1, w5, w9 |
| ~~31~~ | 16FD8 | 17000 | **DONE** `handler_catchslam` + result 0x0E (hit.c) | cat4 w3 |
| ~~43~~ | 18518 | 1853C | **DONE** `handler_throw43` | catC w8, w10 |

Whiffs (mode 1, no FF00 — play, do nothing, recover): ~~cat3 w8 `07`, w9
`03`; cat4 w2/w11 `03`, w6/w10 `27`, w7 `07`~~ DONE; ~~cat5 both-buttons w2/w7/w9
`09`~~ DONE (`hold_holder`); catA w3 `32`, ~~w9 `2C`~~ DONE; catB w5 `89`, ~~w6
`32`, w8 `42`~~ DONE, ~~w10 `2C`~~; catC ~~w4 `2C`~~, w5 `25`/`89`, ~~w6 `3A`~~ DONE; catD ~~w1/w5
`2F`~~ DONE, w6 `3B`, ~~w7 `1A`~~ DONE (`handler_plex`),
~~w8 `25`~~ DONE (`handler_bearhug`), ~~w9 `36`~~ DONE (`handler_mdd`), ~~w11 `44`~~ DONE. **All-roster gaps:** ~~cat 0x10 behind grab →
`1C`/`1D`~~ DONE 2026-08-23 (docs/engine-specs/behind-grab.md: holders 0x1C/0x1D, victims 0x52/0x53, escapes 0x58/0x57, throw-offs 0x6D/0x76, double-team braces 0x65/0x73/0x74, hit reactions 1-4 in hit.c `behind_hit`, CPU victim timers; open: the 0x20F04/0x2103E/0x21114 rumble arms, the cat-0x14 double-team selector row in core.c, the 0xEBC4 ladder arms live in the victim handler until core.c takes them); cat 0x11 weapon swing → `1E`; cat 0x13 run-in strike →
`39`/`1C`.

**Hold/pin subsystem** — transcribed 2026-08-23: holders 0x09/0x22/0x25/0x36/0x1A/0x23, victim moves 0x5D/0x5E/0x5F/0x60/0x62/0x63/0x64, escapes 0x67/0x5C/0x5B/0x5A/0x55/0x56/0x4B (0xEBC4 ladder in core.c), KO pair 0x6E/0x6F (+0x8A), the hold-break reaction handlers 6/7/0xA/0xB (hit.c `hold_hit`), CPU escape timers 0x1F576 (ai.c). Still open: 0x3B (w6, needs victim 0x8D + walk phases), 0x89 (w5), 0x37/0x61 standing hold, the 0x1129E entry-1 roll on 0x1A (fail → 0x87), rumble branches. Original note: moves 0x25 (w5/w8 Perfect-Plex pin), 0x36 (w9 →
victim 0x63), 0x89 (w5 B2 grapple hold), 0x09 (cat 5 ground hold), 0x1C/
0x1D behind grabs, 0x3B (w6 3-phase), 0x1A (w7 slam-into-cover), 0x23
(w8 cradle) all hang off it — transcribe the subsystem once, then these.

Other unrouted hard-hold ids (not on a selector cell — reached by
scripts, AI or other handlers): 3F, 45, 49 (double cover), 54, 64, 6C,
7A, 8D (FF00); 3C, 3E, 50, 61, 6B, 6E, 6F, 7B (held victim!), 88, 8E
(mode 0); 7D, 80–87 (reversal/fumble family, mode-2 loops; 0x84 is the
fail branch of move 0x33's 0x1129E roll, 0x86 of 0x34's).

## 2. Victim reactions (table 0x1AFD4, 43 entries 0x00..0x2A)

Routed: 31. Unrouted (12) — each freezes/whiffs the victim:

| id | record | handler | set by | shape |
|---|---|---|---|---|
| ~~0F~~ DONE | 1B66C | 1B684 | stomp 0x0A (0x1381A/0x139D4), kick-out 0x4B (0x18222/0x18290) | FF00 |
| ~~15~~ DONE | 1B84E | 1B85E | moves 0x2C (0x1696C), 0x3A (0x17C74) | FF00 |
| ~~18~~ DONE | 1BAF0 | 1BB08 | hit pipeline 0x244B2 | FE (special-cased → stand) |
| 1B | 1BC22 | 1BC36 | caught41 handler at 0x1B7EC | FF00 |
| ~~**1C**~~ DONE | 1BC9E | 1BCAA | **top-rope dive 0x0D/0x1B onto a standing man (0x154C2)** — sibling of the fixed 0x20 | FE |
| ~~1E~~ DONE | 1BCF6 | 1BD0E | dive 0x0E (0x13F30), splash 0x0F (0x140E6) | FE |
| 1F/28 | 1BD60 | 12612 (rts) | runner-vs-runner collision (0x247C6/0x247CE) | stock no-op; real effect unknown |
| ~~24~~ DONE | 1BECC | 1BED8 | move 0x2B (0x16842) | FE |
| ~~26~~ DONE | 1BF96 | 1BFA6 | move 0x2F (0x16D5A) | FF00 |
| 27 | 1BFFE | 1C004 | escape 0x5C (0x1947A) | mode 0 (+0x22 raw loop counter) |
| ~~29~~ DONE | 1BAFC | 1BB08 | move 0x02 (0x12A6A) | FE |

Ids 0x2B–0x2F do not exist; `rom16()` has no bounds check (latent OOB).

## 3. Referee (0x1F914, object $1C11F4) — SM table 0x1F952 / visual 0x1FB1A

| N | SM / visual | what | engine |
|---|---|---|---|
| 0 | 1F97E / 1FB46 | intro hold $1C1682 (250f) → $8002 | absent (spawns in SM5) |
| 1 | 1F9E0 / 1FC22 | approach the pin | done (mirror + probe fixed 32ce52f) |
| 2/7 | 1F9F8 / 1FC22, 202BA | follow +0x56 | absent |
| 3 | 1FA82 / 1FD5E | stand between legal men; **ring-out 20-count** | **done** |
| 4/8 | 1FA40 / 1FC22, 20354 | scan +0x35 b0/b1, approach, pose | repurposed as hold-watch |
| 5 | 1F99E / 1FB46 | idle midpoint + hunt | done (tag arm; rumble arm absent) |
| 6 | 1F9D8 / 20022 | 1-2-3 count | done, exact-shaped |
| 9 | 1FAEC / 203EE | win pose; $1C15D2 announcer; 0x20156 stage jingle | partial (bell/jingle added 23fce3f) |
| A | 1FAEC / 1FF52 | leave / patrol | absent |

Approach motion is a fixed 0x18000/frame axis step, not 0x20C8+0x2208
(SIMPLIFIED). Legal-man pair 0x204FA picks the first two f33 b0 slots.

## 4. Ring-out area

| item | ROM | engine |
|---|---|---|
| floor z 0x100 / no Y law / barrier trapezoid zone 3 | 0x28288–0x28312 | done (constants exact) |
| barrier reaction 0x17 | 0x11024 → 0x1BA92 | only from the run handler |
| moves 0x68 (outside), 0x69 (climb in), escapes 0x5C–0x67 | 0x19832, 0x19A20, 0x1978A… | done (0x27 stands in for react 6, TODO EXACT) |
| **getting thrown out** (`exit_test`, react 0x16/0x0E launch) | 0x1B4D4, 0x1B8CA, 0x1B94A | done (victim's own facing, re-seated x; ringout-impl.md) |
| landing outside → 0x68 → 0x6A + `$1C1678` | 0x1BA48, 0x1B976, 0x198A0 | done |
| ring-out 20-count + display | 0x1FD5E/0x1FDF4/0x1FE5A, digits 0x2067C ($C0744 for ≥10) | done (referee.c SM3; 0x90D6 TODO) |
| ring-out camera scene ($1C1678, 0xF98C, ref teleport (0x340,0x160)) | 0x198A0, 0x19B2E, 0xFB5A | done (ringout.c; partner brawl via the 0x1C99E arms 2026-08-23; valets TODO) |
| in-ring men behind the near ropes while the scene shows (list 1 + row-14 rope object 0xF8D8/0xF8E4) | 0xF4F6, 0xF8D8, 0x1F94A | **done** (sprite.c, 2026-08-23) |
| 0x10B62 slide runs the scene law 0x280DC (was the in-ring trapezoid) | 0x10B62, 0x28124 | **done** (motion.c law_run) |
| wrestler reacts at count 10 (force 0x69) | 0x1C6DC, 0x1EBF4 | done for CPU (any count); human climb-in 0xEF0A done, climb-out 0x6C TODO |
| scene-2 ringside variants (zone 4, crowd wall x<0x1C8 / >0x470) | 0x28480, 0x2851E | done (motion.c) |
| weapons / objects at ringside (+0x74 gate) | 0xFFD2 spawn, 0xFDEE machine, 0xF0BA pickup, 0x19EA0 move 0x70, 0x15A20 moves 0x1E/0x1F, 0x24640 result 6, 0x1C70C CPU arm | done 2026-08-23 (weapon.c, docs/engine-specs/weapons.md) |

## 5. Match flow

| item | ROM | engine |
|---|---|---|
| intro walk-in / entrance ($1C1682 = 0xFA sites, ref SM0, sub 0xC 0x11B6C face-off, team pose 0x8B) | 0x18B40, 0x20F5C, 0x21086, 0x21174, 0x215FA, 0x2176E | absent (`eng_init` teleports to 0x10708) — agent building it |
| opening bell ×3 + roar (phrase 0x2A) | 0x11270 | done (23fce3f, at eng_init) |
| match timer 30:01 countdown | 0x262D2/0x26300/0x26370 | enabled (32ce52f); ≤5:00 flash 0x264AA TODO |
| TIME UP overlay | 0x263D8 | done; +0xA0 = 0x80 byte missing |
| pin 1-2-3, half-count stamps | 0x20022, 0x2073A | done exact |
| match end / win pose / ladder bump | 0x203EE, 0x11B6 → 0x1396 → 0x19BA | SIMPLIFIED (over_t waits, eng_init restart); poses 0x1AD24/0x1AD7C, +0xA0 winner byte, 0x90D6 energy regain absent |
| game-mode select (scene 3) | 0x52BE–0x54DE | **done** (src/gameselect.c, pixel-exact; `--front`) |
| char select, aisle walk-in 0x7B70, VS card 0x98BA, ring intro 0xA654, attract/credits | 0x57E0.., 0x7B70, 0x98BA, 0xA654, 0x6FC.. | **done** (charselect.c, aisle.c, banner.c, walkin.c, attract.c, credit.c) |

## 6. HUD

| element | ROM | engine |
|---|---|---|
| energy gauges, init, odd-frame runtime, portraits | 0x75EE/0x76AA/0x76F8, 0x7506, 0x7548, 0x77D8 | done |
| 1P–4P labels | 0x776C | done (live human slots only, tile by PORT 0x77D0) |
| clear HUD block | 0x7744 | absent |
| HUD block prompts INSERT COIN / BUY-IN / PUSH n BUTTON / REGAIN POWER | 0xC710 (0x9ACA ids 0x18-0x26, 0xC404 credit test, +0xC2/+0xC3 cache) | done for the 2-player cabinet (dip 0xC00 = 0x800); 0x400 / 4-player default and the mid-match JOIN of an empty seat (IRQ3 0x978 → 0x1F38) TODO EXACT |
| START buy-in (refill) | 0x8B3A/0x8BA2, 0x55A D0=2 | done (general +0xA2 b8 path; dip 0x800 co-op port path TODO EXACT; $1C0166 attract index untouched) |
| get-up mash overlay | 0x899C ($1C167A b7 from 0x10D04) | done |
| POWER-UP flash | 0x88A2 (+0x33 b5 on fire) | drawn, but dormant: the comeback scan 0xFD00 (+0xC7 hits-taken, +0xC8 timer, f33 b5) is absent (tired.md §4c) |
| timer plate + digits | 0x26300/0x2641E/0x2647C | done (now live) |
| pin-count digits | 0x2067C | partial: always $C0748; ROM uses $C0744 for ≥10, $C1188 with b15 |
| ring-out count display | 0x1FE32 → 0x2067C | absent |
| result plates (TIME UP 0x13, GIVE UP 0x26, 0x50/0x51, DQ 0x52) | 0x2503C / 0x25204 | modes 0/2 exact, 1/3 approximated; DQ never called |
| tag prompt indicator | unknown | absent |
| winner/loser +0xA0 flags | — | absent |

## 7. Sound (all WAVs already in data/sounds + data/music; only issuing logic missing)

Engine issues 14 distinct ids; ROM gameplay issues ~80 via $2052 (175
call sites). Missing, by impact:

1. **Announcer speech** — driver 0xA0E8/0xA0FE, tables 0xA1B6 (names) /
   0xA216 (37 phrases) — **driver now in `src/announce.c` (23fce3f)**;
   wired: bells 0x2A/0x2C, hip toss 0x02, suplex 0x01, backdrop 0x04,
   press slam + 0x33 → 0x07, 0x34 → 0x1C, tag 0x18, top rope 0x17.
   Unwired request sites (51 in ROM): flying mare 0x14F22, shoulder throw
   0x244F8, clothesline 0x2454E/0x24756, MDD 0x1795C, figure-four
   0x13658, Boston crab 0x15BF8, stomp 0x137BC, bigfoot 0x245E6, knee
   smasher 0x246AE, elbow 0x1563C/0x245CC, axe handle 0x1562C,
   backbreaker 0x16912/0x186AC, leg drop 0x17890, powerslam 0x16B7A,
   piledriver 0x162A4, splash 0x14074/0x13E76, flying clothesline
   0x24812, dives 0x1A (17 sites), DDT 0x174E8 (done), sleeper 0x15780,
   two-count 0x21294, gut wrench 0x17C32, Perfect Plex 0x1527C, cobra
   clutch 0x17E6A, gorilla press 0x16CE6, "OH NO" 0x24640/0x24BAC,
   disqualified 0x29 (9 sites), rumble king 0x1AF9C/0x1AFB0.
2. Win jingle music 0x08 — done (referee win).
3. Music beyond stage-0 BGM: attract 0x01/02/03, menu 0x07, post-match
   0x05/0x09 (0x20172), ending 0x0D/0x0E, continue 0x10, boot 0x11, 0x1F,
   stop 0x00; stage-change / resume BGM (0x90CE via table 0xDEE).
4. Big crowd pop 0x31 instead of 0x32 at 0x14850, 0x15D60, 0x17E76,
   0x1A2B2 (+0xA872 strength branch) — engine always emits 0x32.
5. 0x20 stop-all on scene transitions (0x52DA, 0x8EC4, 0xFCEA); 0x80
   crowd (0x7FCA, 0xFBBE); 0x2E (0x2464C); ring-intro speech 0x7B/7C/7D
   (0xAB22, table 0xAC56); pre-match taunts 0x6A/6B/78/79 (0xB11A, 0xB14A);
   0x2C only via one shared ringhw path (ROM has 4 handlers).
6. **Oracle sound board now real** (decomp main: Z80 + OKI + YM bus, `wfport --68k --sound`,
   `WF_SNDTRACE`, `make sndtest`, docs/sound-board.md). Mismatches it exposed in the
   engine's WAV path (TODO): cmd 0x53 is a TWO-phrase sequence (50 then 51 — the
   extractor assumed one phrase per command); per-command OKI attenuation vol 0-7
   is ignored (punch 0x2A = vol 5, names vol 3); music commands also drive OKI
   percussion from the Z80 sequencer (the MAME music renders already include it).
   YM2151 FM synthesis is still the MAME renders (no FM core offline).
7. Headless runs never call `audio_init`, so `eng_sound` is silent there;
   `WF_SNDLOG=1` logs the latch writes instead.

## 8. Known simplifications still marked in code

See `grep -n "TODO EXACT\|SIMPLIFIED\|PLACEHOLDER" src/*.c` — notably the
0x1129E success roll on every throw (no fumble/reversal 0x83–0x87), the
announcer's 0xA404 legal-man veto, +0xC7 victim counter, $1C1800 crowd
index, 0x1108C pin-chance bit, 0x110E0 rescue arm.

## 9. Companion sprites — 0x10D3A / $1C1258 (added 2026-08-23)

`0x10D3A(D0)` fills the first free of 11 slots ($1C1258, stride 0x2A) from
table `0x10DDA[D0] = {dy, pose, list}`: pose from the OWNER's row, placed at
(x + hotspot, y+dy, z-dy), enqueued for this frame; `0x10E6A` (frame list
0x1006/0x10AA) frees every slot each frame, so handlers re-spawn per tick.
Engine: `eng_fx` slots in `eng_state`, `fx_spawn()` in anim.c, drawn by
`eng_sprite_emit` (list 0 depth sort / lists 2,4).

| site | D0 → pose | owner | engine |
|---|---|---|---|
| 0x178AC | 0xF → 0xE9 | leg drop 0x35 frame ≥4 (the leg across the victim) | **done** |
| 0x1BC0A/0x1BC16 | 0x11/0x10 → 0x1D5/0x1D4 | react 0x1A (leg-dropped victim, airborne) | **done** |
| 0x15820/34/3E/4C | 0,1,3,4 → 0x164/0x165/0x166/0x167 | behind grab 0x1C (0x1567C) | **done** |
| 0x159DA/EC | 8,9 → 0x16E/0x16F | behind grab 0x1D (0x15870) | **done** |
| 0x17BCA/DE/EC | 0x15,0x16,0x17 → 0x1E5/0x1E6/0x1E7 | 0x17Bxx (before 0x3A) | TODO |
| 0x183B0 | 0x12 → 0x1D? | 0x183xx | TODO |
| 0x18A68/7C | 0x13,0x14 | 0x18Axx | TODO |
| 0x19244/58 | 6,7 → 0x184/0x185 | escape 0x58 (0x191C8) | **done** |
| 0x1967E | 0xC → 0x21A | double-team brace 0x65 (0x19670) | **done** |
| 0x19D58/6C | 4,4 → 0x167 | throw-off 0x6D (0x19D06) | **done** |
| 0x19FAE/BA | 0xD,0xE → 0x57/0x58 | 0x19Fxx | TODO |
| 0x1A044/80 | 0xA,0xB → 0x53/0x54 | double-team braces 0x73/0x74 (0x1A020/0x1A05C) | **done** |
| 0x1A13C | 8 → 0x16E | throw-off 0x76 (0x1A10E) | **done** |

Also noted: `+0x60` bit 4 (engine `move_id & 0x1000`) = "attacker airborne"
set by 0x13DF2 (dive 0x0E), 0x15E8E, 0x177FE (leg drop); read by the
downed-man selector 0xEC60 (no roll-away 0x78 while the 0x0F/0x23 man is up)
and AI 0x1DE3E. Leg drop sets/clears it now; 0xEC60/0x1DE3E consumers TODO.

## 10. Data layer (ADR-001) — 2026-08-23

| item | state |
|---|---|
| ROM tables → `data/tables/**.json` (164, verified round trip) → `build/base.pak` | **done**; game never opens `rom/` |
| wrestler packages → `build/wrestlers/NN.pak`; tile sets → `build/gfx.pak` (`--verify-gfx` identical to chips) | **done** |
| engine scalars as synthetic tables (`rules`: hold/tag/ringout) | done for those three; the rest of the hard-coded scalars (user: "expose as many as possible") open |
| per-id slicing into packages (rule 7), roster manifest / ids > 11 (rule 8) | open |
| mods: path overlay in the packer (`mods/order.txt`) | done; no in-game mod manager |
| wfeditor (tkinter deep-dive editor) | next (user 2026-08-23) |

