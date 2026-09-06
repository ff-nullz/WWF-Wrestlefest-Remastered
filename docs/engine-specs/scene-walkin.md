# Ring intro / walk-in before the live match (0xA654)

Engine: `src/walkin.c` (`eng_intro_begin` / `eng_intro_tick` / `eng_intro_end`),
hooks in `src/core.c` (`eng_init` tail, top of `eng_update`), announcer sprite in
`src/sprite.c`. Oracle: `tools/scenarios/tag.scn --frames 3600` — scene word
`$1C007E` 3→4 at f1582 (aisle), →0 at f1989; men placed f1994; live at f3502.
Engine frame 0 == oracle f1994.

## 0. Match-start sequencer (who calls what)

```
0xAC0  next-match entry: clears $1C0092.., 0xD1C, 0x2988, jsr table 0xB14[$1C007C?0:4]
0xBD2  first match: $1C0166=0, 0x8EB6 (snd 0x3100/0x3120), stages 4/9 → 0xAE20 arena
0xC02  jsr 0x7B70      AISLE walk-in, tilemap scene 4 (writer 0xAE56), sprite rows 0x40+id
                       — NOT transcribed (TODO EXACT): needs the aisle tilemap + streams.
0xC08  tag only: $1C0066 b3 → slots 1/3 ($1C06B4/$1C07C0..) forced active
0xC60  jsr 0xC98       MATCH INIT: 0xD1C, scene byte 0xD12[stage] → $1C007F, 0x2988,
                       0x1FC0 clear_screen (FG0), 0x1F6C, camera $1C1804/6 = (0x1E0,0x230)
                       [0xCC4], tilemap 0x26E66, 0x1086C objects (tag setup 0x10504,
                       positions 0x10708), 0xFFD2/0x1004A ringside props (ids 0xF/0xE at
                       (0x40D,0x140) (0x330,0x119) (0x1C0,0x178) (0x330,0x178) — slots 8..11,
                       NOT wrestlers), 0x2A06
0xC66  jsr 0xA654      RING INTRO (this file)
0xC6C  $1C15FC=1, 0x2A06, BGM 0xDEE[stage] via 0x2052 (stage 0 → 0x3104)
0xB08  jsr 0x8AE6      $1C0072=-1, 0x1E92; per human slot 0x782C + 0x7506 (HUD init);
                       0xC1EA; $1C169E = 0x8000 (clock runs, 0x8B30)
0xB0E  jmp 0xF8C       live frame loop ($1C0076 b7, 0x514E input, ...)
```

`0x2052` sound post: while `$1C007C != 0` every command is OR'd with 0x3100 and
written 16-bit to `$14000C` (so 0x32 → 0x3132). The engine's `snd()` in walkin.c does
the same.

## 1. 0xA654 — the ceremony

Entry (0xA654..0xA67A): stage 0 only → referee `+0x06 += 0x80` (0xA65E — seat
(0x280,0x198) → (0x300,0x198), matches the trace); tag → 0x98BA sprite banks; BGM
0x3132 (0xA676).

Per frame (0xA680..0xA774), rumble (`$1C0161` b0) skips the script:

1. Announcer object `$1C14CE`: handler `0xA7B4[+0x1D]` (step byte), then
   `+0x1D >= 0xB → +0x04 |= 0x8000` (0xA6AA, face left), stage != 0 → `+0x04 = 0xFFFF`
   (hidden); 0x247C screen pos; 0x27B8 sprite.
2. Each of the 8 wrestler slots `$1C05B0 + n*0x10C` with +0x00 b7: 0xACB6 (below),
   0x247C, 0x27B8.
3. Referee `$1C11F4`: 0x247C + 0x27B8 only (no machine; SM0 0x1F97E / `$1C1682` belongs
   to the live loop — the trace shows `$1C1682 == 0` throughout the intro).
4. 0x2836, wait vblank (`$1C006F`), then exit test (0xA732): any player's raw button
   bits 0x30 (B1/B2) → exit; else continue until `+0x1D >= 0x18` (`$1C14EB`).

Exit (0xA778..0xA7B2): 0x2B58 #0x1F (free the announcer's palette slot), 0x9A42
(clear sprite RAM), 0x7506 (HUD), `clr.w $1C14CE` (announcer off), `$1C15FC = 1`,
0x2A06, 0x8EB6 (snd 0x3100, 0x3120), snd 0x312F.

### 1a. Step table 0xA7B4 (24 entries, index = +0x1D)

| step | PC | what | duration |
|---|---|---|---|
| 0 | 0xA9E2 | spawn announcer (0x2AEA #0x1F; +0x00=0x8001, id 0x1F, (0x280,0x178,z 0x140)), pose 2, wait | 0xAA66[0]=0x80 (+1) |
| 1 | 0xAA96 | pose 2 for 0x10, pose 3 for 0x20 | 0x32 |
| 2 | 0xAAEA | phrase 0xAC56[2]=0x7B "this contest…" | 0xAC6E[2]=0xA0 |
| 3 | 0xA9E2 | wait (re-seats the announcer) | 0xAA66[3]=0x20 |
| 4 | 0xAAEA | name of pair 0 (0xABD8) | 0xAC9E[id] |
| 5 | 0xA814 | point (cell 0xC), pair-0 man's pose cycle, sting 0x3132/0x3131 (id>=5) | 0x40 |
| 6 | 0xAAEA | 0x7C "and his partner" | 0x14 |
| 7 | 0xA8C0 | advance | 0 |
| 8 | 0xAAEA | name of pair 1 | 0xAC9E[id] |
| 9 | 0xA814 | point, pair-1 pose | 0x40 |
| A | 0xA8C0 | advance | 0 |
| B | 0xAAEA | 0x7D "versus" (announcer now faces left) | 0x28 |
| C | 0xA8C0 | advance | 0 |
| D | 0xAAEA | name of pair 2 | 0xAC9E[id] |
| E | 0xA814 | point, pair-2 pose | 0x40 |
| F | 0xAAEA | 0x7C | 0x14 |
| 10 | 0xA8C0 | advance | 0 |
| 11 | 0xAAEA | name of pair 3 | 0xAC9E[id] |
| 12 | 0xA814 | point, pair-3 pose | 0x40 |
| 13 | 0xA9E2 | wait | 0xAA66[0x13]=0x20 |
| 14 | 0xAA96 | pose 2/3 bob | 0x32 |
| 15 | 0xA8EC | walk out: +0x2B=8 (0.5 px/f), +0x2D=0 (+y), cells 8..B every 0xC, until y >= 0x1A7 | ~94 |
| 16 | 0xA952 | cells 4..7, 0xC each | 0x30 |
| 17 | 0xA99A | cells 0xD..0x10, 0xC each | 0x30 |
| 18 | — | exit | |

Tables (all read from `wf.rom[]`): waits 0xAA66 (words), phrase bytes 0xAC56, step
durations 0xAC6E (0 = by wrestler: 0xAC9E[id] words), name phrase per id 0xAC4A
(0x6D,0x6E,0x6F,0x74,0x73,0x73,0x71,0x70,0x72,0x75,0x73,0x73), pairs 0xA8CC
({$1C05B0,$1C09E0},{$1C06BC,$1C0AEC},{$1C07C8,$1C09E0},{$1C08D4,$1C0AEC}).

Speech step 0xAAEA detail: entry clears +0x22/+0x24/+0x1E; phrase byte 0/0xFF means
"say the name of the slot picked by 0xABD8" (step 4 → pair 0, 8 → pair 1, 0xD → pair 2,
else pair 3; a pair's second entry is used when the first slot is inactive). After a
name, `$1C0168 = (byte == 0x73) ? 0x8000 : 0` — 0x73 is a shared team name, and 0xA814
then skips 4 steps (0xA8B2: partner's "and his partner"/name/pose). Mouth flap: `+0x05`
(low byte of the cell word) = +0x25, which toggles every 6 + (0x21B4 rng & 0xF) frames.

### 1b. Wrestler pose driver 0xACB6

`+0x1D == 0`: cell = side1 ? 0 : 0x8000 (0xACE8: face each other), +0x22/+0x24 = 0.
`+0x1D != 0` (set by 0xA814; also the NEXT slot when the man's id is 0xA, 0xA878): list
= `0xAD6C[id]` (n = `0xAD9C[id]` cell words then n duration words); cell = list[+0x24];
++`+0x22` > dur[+0x24] → next cell; past the last → `clr.w +0x1C` (+0x1D = 0).
E.g. id 0: 5B,5A,59 ×3, 5C, 5D with 6,…,6,0x20 frames.

Rumble (0xACC6): `+0x22` counts to 0x40 then `$1C14EA = 0x18` + snd 0x312F — a 64-frame
hold, no ceremony (match.scn is a rumble: live at f842 = 0x40 after placement).

## 2. Engine mapping

- `eng_init` → `eng_intro_begin`: FG0 cleared (0x1FC0), `hud_inited = 0`, ref x += 0x80,
  snd 0x3132. `WF_NOINTRO=1` (auto-set for `--selftest` and every headless `--drive`
  other than `demo`; `WF_INTRO=1` overrides) calls `eng_intro_end` immediately, which
  is exactly the old frame-0 live start.
- `eng_update` returns after `eng_intro_tick` while `st->intro`; only the announcer
  driver (`eng_announce_tick`) runs alongside, like 0xA0E8 in IRQ.
- `eng_intro_end`: announcer off, ref back to 0x280, snd 0x3100/0x3120/0x312F,
  stage BGM 0xDEE[0], HUD init armed (0x7506), `sig169e |= 0x80` (0x8B30), and the
  opening bell `eng_announce(0x0F, 0x2A)` (0x11270) — both moved here from `eng_init`.
- Announcer = `st->ann` (`eng_ann`), drawn as sprite row 0x1F in the depth-sorted list.
- Headless drives zero their inputs while `st->intro` so scripted button taps don't
  skip the ceremony; GUI B1/B2 skip it like the cabinet.
- Debug: `WF_INTROLOG=1` prints each step change; `WF_SNDLOG=1` the sound posts.

Timeline check (engine vs oracle, offset 1994): step 1 f128 (oracle ~2122), step 2
f178 (2172), step 3 f339 (2333), walk-out f1251 (3245), exit f1440 — the oracle exits at
f3502 because its wrestlers have longer name durations (0xAC9E; the engine's fixed ids
0/2/1/3 come from the select-screen TODO in core.c).

## 3. Exact vs TODO EXACT

Exact (PC-cited, table-driven): step table and all 8 handlers, speech/name/duration
tables, pair selection 0xABD8, team-name skip, wrestler pose cycle 0xACB6, rumble hold,
button skip, walk-out motion (0x2208 case 1 via `eng_sincos_step`), exit sound posts,
HUD/clock/bell ordering.

TODO EXACT:
- 0x7B70 aisle scene (tilemap 4, rows 0x40+id) is not played at all.
- 0x2AEA/0x2B58 palette-slot allocator: the announcer's body palette (0x2F22 + 0x1F*32)
  is parked in sprite bank 0xD (`eng_sprite_extra_bank`).
- Referee intro seat: `+0x06 += 0x80` at 0xA65E is transcribed; the stock 0x10718 seat is
  restored at exit by assignment (the ROM's live ref walks from 0x300 — see trace f3525).
- 1P layout: ROM CPU team lives in slots 4/5 ($1C09E0/$1C0AEC); the engine folds the
  pair fallback onto slots 2/3.
- Stage != 0 paths (0xAA5E skip, 0xA6BA hidden announcer) and 0x9A42 sprite-RAM clear
  are no-ops here.
- 0x8AE6's per-human 0x782C and 0xC1EA are not transcribed (HUD init covers 0x7506).
