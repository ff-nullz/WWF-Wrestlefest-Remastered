# CPU opponent core — transcription of 0x1C150–0x1F760 (reference/maincpu.asm)

Read-only transcription. Scope: what is needed to drive one CPU wrestler in a
singles match. Tag / rumble / attract arms are noted where they gate a branch
but not expanded. Corrections to `docs/ai-engine.md`: the weight tables ARE
conditioned (on stage `$1C0162`, own band `+0x70`, opponent band, wrestler id,
x-zone, clock), and the AI does NOT only write `+0x20/+0x60` — it also writes
`+0x61` (move id) and then runs the same `0x1DFE2` move-prep dispatcher the
human path uses (see §3).

## 0. Field glossary (engine names where known, else ROM offset)

| field | meaning (as used by the AI) |
|---|---|
| `+0x20` state word (`+0x21` low) | bit7 = "latched" (state entry done). 0 stand, 1 walk, 2 run, 3 ?, 4 react, 5 move, 6 hold, 7 getup, 8 climb, 0xB lockup, 0xC hold-won |
| `+0x60/+0x61` | move word / move id; `+0x60` bit6 = move-prep done (`0x1E25E`) |
| `+0x65` | reaction sub-id (state 4): 1 = ?, 8/9 = lying (face up / face down), 0/0xA = ? |
| `+0x2D` | walk direction byte (angle/8, 0x00 right, 0x40 up, 0x80 left, 0xC0 down) |
| `+0x2E` bit7 | facing right |
| `+0x31` | x-zone column (`<3` left edge, `>=9` right edge) TODO EXACT |
| `+0x33` b0 tag-legal?, b1 human, b2 outside ring, b4 grapple-ready, b6 engaged/lockup |
| `+0x34` b1 tag-recall pending, b2 ?, b4 **pin intent** |
| `+0x35` b0 pinned, b1 ?, b3 ? |
| `+0x44/+0x45` | timer word / low byte; in state 2 = run phase 0/1/2 |
| `+0x56` b7 CPU object, b6 autopilot (human being puppeteered), b5 side |
| `+0x57` | wrestler id (0..11) |
| `+0x66` hp, `+0x72` hp_max, `+0x70` band word (0 healthy, 1 hurt, 2 desperate), `+0x71` low byte |
| `+0x74` b7 holds weapon; `+0x76` held/ref link |
| `+0x7A` opp pointer; `+0x7E` rescue/alt target; `+0x86` partner; `+0x26` grapple partner |
| `+0x9A` down timer (victim), `+0xAA/+0xAB` mash meter (victim) |
| `+0xAE` | state-1 sub-state (index into `0x1C908`) |
| `+0xB0/+0xB2` |dx|, |dy| to opp; `+0xB4` range bucket 0/1/2; `+0xB5` b0 opp right, b1 opp below |
| `+0xB5` b2 dir latch, b3 "follow-up armed (dx<+0xBC gate)", b4 move committed, b5 walking to (+0xBE,+0xC0), b6 wants tag, b7 rescue mode |
| `+0xB6` b0 pinned-partner guard, b1/b2 run-roll once-latches, b3 x-run latch, b4 rope-move pending (0x6C), b5 ?, b6 rumble, b7 "re-decide" |
| `+0xB7` b0 run-attack pending (0x72), b1 once-latch for a roll, b2/b3 follow-up latches, b4 use +0xBC as move, b5 corner walk, b6 pickup pending (0x3D), b7 pursuit-wait |
| `+0xB9` b0 wait-for-opp-engaged, b1 policy returned idle |
| `+0xBA` | move timer (from `0x1F576`) |
| `+0xBC` | scratch: distance gate / countdown / corner index / move id |
| `+0xBD` | mash counter (state 0xB) / kick-out count |
| `+0xBE/+0xC0` | walk target x/y |
| `$1C0161` b0 rumble, b1 tag/cage; `$1C007C` match live; `$1C0162` stage (0..9, = difficulty); `$1C16A0` clock minutes BCD; `$1C1697` "re-sync" flag; `$1C1670` corner-occupied bits |

Band (`0x24EC2`): `band = hp <= 0x18 ? 2 : hp <= 2*hp_max/3 ? 1 : 0`.

RNG `0x21B4` and d100 walker `0x24CC` as in docs/ai-engine.md. Every table below
is {p, 100-p} pairs unless said otherwise; "roll==0" means the first bucket hit.

## 1. Walker 0x1C15C / dispatcher 0x1C1D2 / table 0x1C1F4

```
1C150  if $1C16C5 bit0 (time-up lock) -> rts
1C15C  A0 = $1C05B0, 9 slots (D6=8), stride 0x10C
1C164  skip unless +0x00 bit7 (live)
1C16E  if +0x56 bit6 (autopilot) -> qualifies
       else need +0x56 bit7 (CPU) and NOT +0x32 bit7
1C186  skip if +0x21 == 0xFF (hold victim)   ; carried by holder
1C18E  skip if +0xFE != 0                     ; result latch
1C194  skip if +0x32 bit4
1C19C  bsr 0x1C228  range_linked (fills +0xB0/+0xB2/+0xB4/+0xB5 b0-1)
1C1A0  if $1C007C == 0 (attract): +0xD2=3,+0xD4=2,+0xD6=3,+0xD8=2,+0xDA=3,+0xDE=3,+0xE4=4 (context counters)
1C1D2  jsr 0x1C1F4[(+0x20 & 0xFF)*4]
```

| state | handler | role |
|---|---|---|
| 0 | 0x1C2CC | stand: decide |
| 1 | 0x1C8F4 | walk: sub-dispatch by +0xAE via 0x1C908 |
| 2 | 0x1D7B6 | run |
| 3 | 0x1DA66 | (rope/skid?) only `subq #2,+0x2A` when +0xB7 b0 |
| 4 | 0x1DA7E | reaction / lying |
| 5 | 0x1DFE2 | move-prep dispatch by +0x61 via 0x1E018 (shared with human path) |
| 6 | 0x1EDC4 | hold applied: keep/release roll |
| 7 | 0x1EE96 | rts |
| 8 | 0x1EE98 | climb: clr +0xBD |
| 9 | 0x1EEA0 | top-rope wait: +0xBD++ to 8 then pick dive |
| 0xA | 0x1EE9E | rts |
| 0xB | 0x1EFAA | lockup mash contest |
| 0xC | 0x1F06A | hold won: +0xBD++ (cap 0x30) |

State-1 sub table 0x1C908 (+0xAE): 0 0x1C93E approach, 1 0x1D398 tag run-in,
2/3 rts, 4 0x1D1F0 tag recall, 5 0x1D078 walk-to-rope-line, 6 0x1D106 walk
sideways, 7 0x1D16A walk-off, 8 0x1D206 tag-corner walk, 9 0x1D31E corner
climb, 0xA 0x1D5A2 walk to (+0xBE,+0xC0), 0xB 0x1D74A weapon exit, 0xC rts.

## 2. State 0 — 0x1C2CC (stand)

```
1C2CC  if !(+0x20 b7) rts                       ; wait for state entry
1C2D6  jsr 0x10BE8 face_opponent                 ; +0x2E b7 := opp.x (+lead) >= x
1C2DC  +0xAE=0, +0x44=0, +0x46=0, +0x32 &= 0xFBA7, +0xB5 &= 0xCB, +0xB6 &= 0xF1FD, +0xB7 b5=0
1C300  [rumble arm, skipped in singles]
1C322  if +0xB6 b4 && $1C0161 b1: clr b4, state=5 move=0x6C (tag rope move) rts
1C348  if +0x34 b4 (pin intent) && !(+0xB5 b3): state=5 move=0x79 (COVER) rts
1C366  if +0x34 b2 -> 1C7E2 (rescue/exit arm)
1C370  if bclr +0xB7 b0: state=5 (move already in +0x60) rts       ; run-attack pending
1C380  if bclr +0xB7 b4: +0x60 = +0xBC; state=5 rts                ; queued move
1C390  if +0xB7 b7 (pursuit-wait):
         roll 0x1C8F2 {50,50}; 0 -> bsr 0x1DF1E (walk to opp / stand)
         else bsr 0x1DF7E pursue-reposition; if it returned 0: bclr b7, state=5 move=0x3E
1C3CC  if +0xB5 b7 -> 1C7EC rescue
1C3D6  [tag: partner engaged -> $1C1697=1 rts]
1C3FC  if +0xB9 b0: if !(opp +0x33 b6) bclr b0 else bset +0xB6 b7 rts
1C420  if opp +0x34 b4: bset +0xB6 b7 rts                          ; opp is covering
1C430  if +0xB5 b3 (follow-up armed):
         1C44C  if !(opp +0x33 b4): bclr b3 -> 1C7E2
         1C462  live && !(+0x33 b2) && bset +0xB7 b2 was clear:
                  x-zone +0x31 <3 facing right or >=9 facing left (back to ropes):
                  roll 0x1F3BE {20,80} (wrestler-0 row, no id offset); 0 ->
                  bclr b3, state=2 RUN, 0x1D7A0 set dir, bset +0x32 b2, flip +0x2D b7 and +0x2E b7 (run AWAY), bclr +0xB7 b1, rts
         1C4E4  if !(+0xB7 b3): 0x1F7F0 demo script (live: carry set) ->
                  1C548  if facing == opp facing -> rts
                         bset +0xB7 b3; A2=0x2307C; D2 = 0 if dx<0x68 else 1 if dx<0xB0 else 2
                         dx<0x68: bclr b3, 0x1DEA6 policy pick (ctx D2) -> 1C5B6
                         else: bclr +0x33 b4, 0x1DEA6, clr +0x60, state=0; if +0x33 b4: state=2 run + 0x1D7A0
         1C5B6  if facing != opp facing and dx < +0xBC: bclr b3, clr +0x60, bset +0xB5 b4, state=5 -> 0x1DFE2
1C5EC  if +0x34 b1 -> 0x1C5FA tag recall (singles: state=1 +0xAE=4)
1C640  if +0xB5 b6 (wants tag) ... else bsr 0x1F3D6 (tag-want eval; singles: clears b6)
1C6B6  [tag/weapon arm $1C0161 b1]
1C7E2  if +0xB5 b7: 1C7EC rescue ... (singles: falls to)
1C880  opp in state 4 sub 6/7/8/9 keeps +0xB6 b0 else clr
1C8B2  [rumble] else: $1C1697=1, +0x60=0, state=1, +0xAE=0   ; -> WALK/approach
```

So in singles a standing CPU with nothing queued goes to state 1 sub 0 every
frame it is idle; the real decision lives in the approach sub-state.

### 2a. State 1 sub 0 — 0x1C93E (approach)

```
1C93E  bclr +0xB7 b7; jsr 0x10BE8 face_opponent; A1=opp
1C94E  [rumble arm] ; singles:
1C99E  [tag arms on +0x33 b2 outside] ; singles with both inside:
1CA82  bclr +0xB6 b5
1CA8C  if opp +0x32 b1 -> 1CCF0 (opp in a grapple-phase: walk to 0x1D068[opp +0x44] target, move=0x3D, sub=0xA, state cleared, +0xB7 b5/b6, +0xB5 b5)
1CA96  if +0xB4 != 0 (dx >= 0x50) -> 1CD5C (far)
1CA9E  if dy >= 0xC -> 1CDD0 (align)
--- NEAR (dx<0x50, dy<0xC) ---
1CAA8  if bclr +0x34 b2 && +0x33 b1 -> 1CFFA
1CABA  if +0x74 b7 (weapon) -> 1CD2C (weapon attack: move 0x1E, or 0x3D if opp lying)
1CAC4  if +0xB5 b7 -> 0x1F5B2 (counter-move table, §3c)
1CACE  if +0xB9 b1 (policy said idle): if opp in state 0/1(sub0)/4 sub1 -> 1CFB4 (tag check) else bclr b1 rts
1CB2C  D2=0
       opp state 0 or 1 -> 0x1DE66 policy pick ctx 0      ; NEUTRAL
       opp state 5:
         opp +0x33 b4 (grapple-ready): if facing same -> rts
           A2=0x2307C: opp move 5/2A/2D/20/41 -> 0x1DEA6 (policy2 ctx0)  else clr +0x01, state=5 move=0x75 (counter-grab)
         else opp move 0x4C -> retarget +0x7A=opp partner, +0xB7 b6, state=5 move 0x3D
              opp +0x33 b6 (engaged) -> [rumble] else bclr +0xB5 b4, state=5 move=0 -> 0x1DFE2
       opp state 7 -> policy ctx 0
       opp state 4: sub 0/0xA -> policy ctx 0
                    sub 8 -> ctx 2 ; sub 9 -> ctx 3 :  (lying)
                       1CC2C demo script; live: [tag] ; if opp hp(+0x66)!=0 -> 0x1F1AC (top-rope eval, §4d)
                              else move=0x48 (cover/pin), state=5, bclr +0xB5 b4 -> 0x1DFE2
                    sub 1 : facing != opp -> ctx 5 ; else script/ (live) ctx 7
                    other -> +0xB7 b6, state=5 move 0x3D (pickup)
       opp other state with +0x33 b4: facing same -> 1CFB4 else 0x1DEA6 policy2 ctx 0
--- FAR (dx >= 0x50) --- 1CD5C
1CD5C  if dy < 0xC -> 1CE38
1CD66  if +0xB4==1 (0x50<=dx<0xB0) -> 1CDD0 align
       opp +0x33 b4 -> 1D01A walk straight (dir 0x40/0xC0 toward opp by +0xB5 b0) [bug: +0x2D 0x40=up?]  TODO EXACT dir table
       else diagonal: +0xB5 b1 (below) ? {dx<0xC: 0x00 ; else 0xE0/0x20} : {dx<0xC: 0x80 ; else 0xA0/0x60}
1CDD0  (align, dy>=0xC or mid range)
       opp state5 move 0x4C -> pickup 0x3D
       [rumble] else -> 1CD6E diagonal walk as above
1CE38  (dy<0xC, dx>=0x50)
       +0x34 b2 -> 1CFC4 ; +0x74 b7 weapon -> 1D032 (dx<0x68 -> weapon attack; <0x90 roll 0x1D066{50,50} -> move 0x1F)
       +0xB4==1 -> 1CEFC
       opp +0x33 b4 -> 1CE96: facing != opp; $1C007E!=1; !(+0x33 b2): roll 0x1D030 {70,30}; !=0 -> RUN AWAY (state 2, flip both facing bits)
       opp state4 sub1 -> facing!=opp: ctx 6 policy ; else 1CFB4
       else if !(opp +0x32 b0): jsr 0x1F310 run-eval (§4a); ==0 -> 0x1D7B6 (already state 2)
1CEFC  opp +0x33 b4 && facing != opp -> 1CF12 else 1CFB4
1CF12  dx>=0x68: dx<0xB0 && opp state5 move 2D/20/41 -> policy ctx 0 ; else state=5 move=0x3C (dash-in?)  ; dx>=0xB0 -> move 0x3C
       dx<0x68: opp state5 moves 5/2A/2D/20/41 -> 0x1DEA6 ctx0 ; else move 0x75
1CFB4  +0x34 b1 -> tag recall ; else 1D01A walk toward opp: +0x2D = +0xB5 b0 ? 0x40 : 0xC0
```

Walk direction bytes (`+0x2D`) issued by the AI: 0x00/0x20/0x40/0x60/0x80/0xA0/
0xC0/0xE0, i.e. 8 compass points with the engine's own angle convention; the
walk state's mover uses `+0x2D` with `+0x2E` facing from `0x10BE8`. There is no
"circle" behaviour; lateral motion only appears as the dy-alignment diagonals.

Key thresholds: near = dx<0x50 && dy<0xC; policy distance ctx at 0x1C564: 0x68 / 0xB0.

## 3. Policy lookup — 0x1DE66 / 0x1DEA6 (and 0x1F0AA, 0x1F616)

```
1DE66  if D2 == 0:                                   ; entry with ctx in D2
1DE6C    !(+0x33 b0) && opp +0x32 b0 -> bset +0x34 b1, state=0 rts   ; tag recall
1DE8C  live or rumble: A2 = 0x219B4 (ai_move_policy) ; else 1DEF8 idle
1DEA6  rec = *( *(A2 + id*4) + ctx*4 )               ; id=+0x57, ctx=D2 (0..10)
1DEBA  n = rec[0]; ids = rec+1; w = rec+1+n
1DEC0  band = opp->+0x70
1DEC8  if $1C16A0 (minutes BCD) < 0x10: band = 2     ; under 10 min: desperate row
1DED6  if band: w += n << (band>>1)                   ; band1 -> +n, band2 -> +2n
1DEE4  k = 0x24CC(w)  (d100 over the n weights)
1DEEA  +0x61 = ids[k]
1DEF0  0xFF -> state=1, +0xB9 b1, +0xAE=0, +0x60=0 (idle)
1DF0E  0xFE -> 0x1DF1E: opp.x past 0x270 (facing right) / before 0x2A0 -> bclr +0xB5 b5, state=1 sub 5 (walk to rope line) ; else state=5 move 0x3E
1DF62  0xFD -> clr +0x60 rts
1DF70  0x3E -> 0x1DF7E pursue-reposition: x += ±(0xB0-dx+0x10) then 0x280DC clamp; if clipped (+0x37) -> state=1 sub5
1DFCC  else clr +0x60 (low), state=5, bclr +0xB5 b4, bclr +0xB7 b6 -> 0x1DFE2
```

Record format: `[n][n move ids][n weights band0][n weights band1][n weights
band2]` — weights are plain percentages summed by the walker. Rows are
selected by the **opponent's** band; the self band is not used here. The
`0x2307C` variant (ai_move_policy2, A2 pre-loaded at the caller) has the same
format, ctx always 0, and is the "opponent is mid-grapple-move" policy.

Contexts passed as D2 (from §2a): 0 neutral/opp standing, 2 opp lying face-up
(sub 8), 3 lying face-down (sub 9), 4 (0x1D26E tag-corner / 0x1D5E8 walk-to
arrival vs grapple-ready opp), 5 opp in react 1 facing away, 6 opp react 1 at
range, 7 opp react 1 same facing, 8 run start (0x1D966), 9 top-rope arrival
(0x1E9CC → 0x1F084 path), 1 lockup result (0x1F078).

0x1F078 (hold cascade, called from state 0xB win/other): D2=1; demo script;
live: if +0xB5 b7 (rescue) → `+0x61 = ws_ai_fallback_move[id]` (0x1F150: 24 ×7,
18, 24, 2C, 24, 24); else deeper index `*( *( *(0x219B4+id*4) + 4 ) + facing*4 )`
then `+ (zone<3 ? 0 : zone>=9 ? 8 : 4)` → record → band row (same 10-min
rule, band from `+0x26` grapple partner) → `+0x61`; then state=5, +0xB5 b4,
+0xB7 b1, clr +0x44/+0x46 → 0x1DFE2.

0x1DFE2 (state 5 / move-prep): bclr +0xB9 b1; jsr 0x1E018[+0x61*4]; bit31 of
the pointer = "also run when state 5 already latched"; otherwise only runs on
first entry. The rows are the per-move prep handlers (timers `0x1F55A`
→ `+0xBA = 0x1F576[ctx][+0x71]`, target positions, +0xBC gates). This is the
same dispatcher the human input path reaches, so the AI does **not** bypass the
move rules — it bypasses the *controller* (never touches +0xA3/+0xA9/+0xAA).

### 3c. Counter table 0x1F5B2 (rescue mode, +0xB5 b7)
opp state 5 move 0x4A → (rumble) move 0x49 → 0x1F45A. Else scan `0x1F720`
(16 ids: 53 61 09 22 3B 36 1C 25 1D 48 0F 0E 1A 23 77 37) for opp move; hit k →
record `*( *(0x233F0 + id*4) + k*4 )` same format, band = opp band (no clock
rule) → `+0x61`; 0x3E → reposition ±0xB8 + clamp. Miss → $1C1697=1, +0xB6 b7,
bclr +0xB5 b7, state 0.

## 4. Other rows

### 4a. Running — when (0x1F310) and state 2 (0x1D7B6)
```
1F310  D0 = id*2
1F318  dx >= 0xE0: if +0x33 b2 fail; once (+0xB6 b2): roll 0x1F3BE[id]; 0 -> RUN AWAY (flip facing) ret 0
       dx in [0xC8,0xE0): once (+0xB6 b1): roll 0x1F3A6[id]; 0 -> RUN toward (state 2, 0x1D7A0 sets +0x2D = facing? 0x40:0xC0) ret 0
       else ret 1
0x1F3A6 (mid)  08 0E 05 07 10 0A 03 0C 07 08 0A 0C   (% by id)
0x1F3BE (far)  14 1E 0F 14 23 19 0A 19 0C 12 19 16
```
State 2 row `0x1D7B6` by run phase `+0x45`:
- 0 (0x1D92A): `+0x34 b2`→rescue scan; `+0x32 b2` (running away) rts; opp `+0x32 b0` → $1C1697=1, state 3; dy<8: once (+0xB7 b1) and !(+0xB5 b3): policy ctx 8 → `+0x20 = 0x8002` (stay running, latched). Then if opp not in react 1/0/0xA and not +0xB5 b7: if dx < +0xBC → bclr b3, state 5 → 0x1DFE2 (running attack); else opp engaged/+0x32 b1 or move 48/1A/23 → state 3 else rts.
- 2 (0x1D7D0): tag: dx>=0xC0 → state 3; else clr +0xBC, bclr +0xB7 b0.
- 1 (0x1D7F0): live: facing==opp facing → bclr +0xB7 b1 → 1D8FC. Else dy<0xA && dx<0xA0, once (+0xB7 b1): clr +0xBC; opp grapple-ready or opp move 3 → roll `0x23B26[id][min(stage,5)][band]` (0x1D8C2) → 0 → +0x32 b2, move 0x75, +0xBC=0x50; else [rumble: 0x23856[id] by own band] / `0x238CE[id][stage][band]` → 0 → move 0x72 (running strike), +0xB7 b0, +0xBC=0x48. 1D8FC: if +0xBC && +0xBC >= dx: +0xB7 b0 ? state 3 : (bclr b1, state 5).

### 4b. State 4 — 0x1DA7E (reaction / lying)
```
1DA7E  not latched: +0xAE=0,+0xBC=0, +0xB5&=0xC7, +0xB7&=0x7C, bclr +0x34 b2, [tag] rts
1DAB0  +0x65==1: bclr +0xB9 b1, +0xB5&=0x3F, +0xB7=0x02 once -> 0x1F3D6 tag eval
1DAD6  +0x65==8 (face up) -> 1DAEA ; ==9 (face down) -> 1DC48 ; else rts
1DAEA  !live && !rumble: [demo: +0x9A = 0x1000 unless opp id 9 script 7/8] rts
1DB28  A1 = +0x26 (the man on top); must be state 5:
       move 0x08 (cover): once (+0xB7 b1): roll (rumble 0x1DE4E : 0x23D7E[stage])[own band] ; 0 -> move 0x38 (kick-out/roll), state 5 -> 0x1DFE2
       move 10/46/0B: A3=0x23F78 ; move 09/22: A3=0x23FC4 ; else 1DC48
         once: roll (rumble 0x1DE48 : A3[stage])[own band]; 0 -> +0xBC = 2 (or 0x10 unless move 9/22), +0x60 = 0x38 (sub8) / 0x78 (sub9), +0xB5 b4 -> 1DE08
1DC48  face-down: (rumble|live) A1=+0x26 state 5:
       moves 11/0C/12/0F -> once: roll 0x23DD0[stage][own band]; 0 -> +0xB5 b4, move 0x54 if 0x0F&&!(A1 +0x35 b0) (+0xBC=0x20) else +0xBC=0x10, 0x38/0x78
       moves in 0x1DE54 list (10 0B 13 14 0E 35 23) idx k -> once: roll 0x23E16[k][min(+0xD2[k],5)] (context counter); 0 -> 0x54 (0x0E & !b0) else 0x38/0x78, +0xBC=0x10, +0xB5 b4
       move 0x0A -> once: ctr=+0xE5 <5: roll 0x23E76[stage][ctr]; 0 -> 0x38/0x78, +0xBC=2, +0xB5 b4
1DE08  if +0xB5 b4 && A1 state 5: A1 move 23/35/0E -> wait for A1 +0x60 b4 then state 5 ; else --+0xBC == 0 -> state 5
```
So the CPU "mash-out" is **not** a press count: it is a one-shot weighted roll
per hold/pin move, then a `+0xBC` countdown, then `state=5 move 0x38/0x78`
(escape move) — `0x10D04` returns early for `+0x56 & 0xC0`, so `+0xAA` is never
decremented for a CPU victim. Pin kick-out count is separate (`0x1E48C`: by hp
→ `+0xBD` = 8/0/1/2/3/4/5, see pins-referee.md).

### 4c. Downed opponent: cover vs stomp vs pickup
- Pin intent `0x1108C` (jsr'd by ~20 knockdown enders): live && !(+0x33 b1):
  `rng&0xFF < 0x110C8[+0x02 id]` (88 80 44 40 78 78 50 80 80 85 80 40 /256) → `bset #4,+0x34`.
  Consumed at 0x1C348 in stand: → move 0x79 (cover) unless +0xB5 b3.
- Otherwise, approach sub0 with opp lying (state 4 sub 8/9): 0x1CC2C → live:
  opp hp!=0 → 0x1F1AC top-rope evaluation (§4d); hp==0 → move 0x48 pin.
  Pickup `0x3D` is chosen when opp is in a react sub other than 0/1/8/9/0xA, or
  opp is doing 0x4C, or the weapon arm. Stomp/drop moves come from policy ctx
  2/3 (ids in `ai_move_policy.json` rows for ctx 2,3 — cite, not re-dumped).
- Pickup-after-rescue 0x1D498 (tag run-in near downed man): roll 3-way
  `0x1D4FC[stage]` (rows 19 0F 3C / 1E 14 32 / 1E 14 32 / 23 19 28 / 28 1E 1E ×5 / 2D 23 14):
  0 → move 0x39 on that target; 1 → move 0x1C (if target free); 2 → nothing.

### 4d. Top-rope / corner (0x1F1AC, sub 9 0x1D31E, state 9 0x1EEA0)
0x1F1AC: not tag, !(+0xB6 b0), opp band != 0, !(+0x56 b6): pick corner
`+0xBC` 0..3 from opp x/y vs (0x210/0x260/0x268/0x200, 0x170/0x150) by opp
facing; roll `0x1F2E0[id]` pair (first pair if opp `+0x71`==1 else second):
0 → opp `+0x9A += 0x100`, opp `+0xAB += 0x14` (hold him down longer), state 1
sub 9. 0x1F2E0 rows (p for band1 / other): 0A/1E 14/28 00/14 14/2D 1E/3C 0A/23
00/0F 0A/19 0F/23 19/37 0F/37 0A/28.
Sub 9 walks to `0x1D388[+0xBC]` ((1F8,1A0)(300,1A0)(1D0,120)(320,120)); on
arrival, corner free ($1C1670 bit) and opp state 4 → `jsr 0xEECE`, `+0x44=+0xBC`,
state 8. State 9 (0x1EEA0): +0xBD++ until 8; partner state5 move 0x89 → move 4;
else `+0x61 = 0x1EF9E[id]` (1B/0D) or `0x1EF92[id]` (ws_ai_move_id) if opp lying /
doing 0x61; ids 4/7/0xB may roll 0x1EF90 {40,60} → move 1; state 5, +0xB6 b0.

### 4e. Holds: state 6 (0x1EDC4), 0xB lockup (0x1EFAA), 0xC (0x1F06A)
- 0x1EDC4: live, +0x45!=0: unlatched: roll `0x1EE38[stage][own band]` (stage0: 14 0F 0A … stage9: 1E 1E 1E) → 0 → +0xB5 b4 else clr +0xBC/+0xB7 b1. Latched: if b4 → clr +0x44, state 5 move 0x3F (release/throw) — TODO EXACT which move 0x3F is.
- 0x1EFAA lockup: `+0xBD = (CPU: 0x1F060[stage] = 32 32 07 07 06 06 03 05 05 04 04 03, human 0x20) + rng&3`; per frame `--+0xBD`, on 0 reseed and unless `+0x44 b6`: roll 0x1F05E {50,50} → 0 → state 5 move 0x29 (lose), partner `+0x44 b6`. The winner goes to 0x1F078 (§3).
- 0x1F06A: `if +0xBD < 0x30: +0xBD++` (hold strength feed; the hold timer itself is in anim code 0x1254C).

### 4f. Walk-to-target sub 0xA (0x1D5A2) — used by 0x3D pickup, corner, weapon
`0x1F15C` steers: |x-+0xBE|<8 && |y-+0xC0|<8 → arrived (0); else `$1C15E4/E8/EA` → `jsr 0x20C8` (atan-ish over a 0x2184 octant table) → `+0x2D`. On arrival: +0xB5 b5 path with opp grapple-ready → ctx 4 policy; move 0x1C/0x1D pending and opp not lying → abort (state 0, +0xB6 b7, $1C1697); else `state=5, +0xB5 b4, clr +0x60 → 0x1DFE2`. dx < +0xBC also triggers the move early (0x1D65A).

## 5. Stage ($1C0162) influence — every indexed site
| PC | table | index | effect |
|---|---|---|---|
| 0x1C7B0 | 0x23ED0 ai_approach_chance[id] | +stage*2 | (tag) approach-corner chance |
| 0x1C820 | 0x24010 ai_disengage_chance[stage] | +band*2 | rescue mode drop |
| 0x1D4A0 | 0x1D4FC | stage*4 (3-way) | pickup/throw choice near downed target |
| 0x1D8CE | 0x23B26 / 0x238CE [id][min(stage,5)] | +band*2 | running 0x75 / 0x72 chance |
| 0x1DB66 | 0x23D7E ai_move38_chance[stage] | +band*2 | escape from cover |
| 0x1DBEC | 0x23F78 / 0x23FC4 ai_stage_band_a/b[stage] | +band*2 | escape from hold moves |
| 0x1DCDE | 0x23E76 ai_move38_78[stage] | +ctr*2 | escape from move 0x0A |
| 0x1DDAA | 0x23DD0 ai_corner_rope_chance[stage] | +band*2 | escape from 11/0C/12/0F |
| 0x1E930 | 0x1E9D6/0x1E9FA + 0x1EA1E[stage] (00 06 0C 12 18 1E…) | +band*2 | move 0x16 after 0x1E83A countdown |
| 0x1EDE4 | 0x1EE38 ai_state11_release[stage] | +band*2 | keep/convert hold |
| 0x1F04A | 0x1F060 ai_script_seed[stage] | — | lockup mash seed |
| 0x1C47C/0x1F0D6 | — | zone +0x31 | not stage |
Plus the 10-min clock rule (0x1DEC8, 0x1E8C0, 0x1F0FC) forcing the opp-band-2
policy row. No DIP is read inside the AI; `$1C0162` is the campaign stage.

## 6. C sketch of a CPU tick (singles)

```c
void cpu_tick(Obj *o) {                                   /* 0x1C15C */
    if (!(o->f56 & 0x80) && !(o->f56 & 0x40)) return;
    if (o->state == 0xFF || o->fe || (o->f32 & 0x10)) return;
    range_linked(o);                                      /* 0x1C228 */
    switch (o->state & 0xFF) {
    case 0:  cpu_stand(o); break;      case 1:  cpu_walk_sub[o->ae](o); break;
    case 2:  cpu_run(o); break;        case 4:  cpu_react(o); break;
    case 5:  move_prep(o); break;      case 6:  cpu_hold(o); break;
    case 9:  cpu_toprope(o); break;    case 0xB: cpu_lockup(o); break;
    case 0xC: if (o->bd < 0x30) o->bd++; break;
    }
}
void cpu_stand(Obj *o) {                                  /* 0x1C2CC */
    if (!(o->state & 0x8000)) return;
    face_opponent(o); o->ae = 0; o->t44 = 0; o->t46 = 0; /* +clear flag bits */
    if ((o->f34 & 0x10) && !(o->b5 & 8)) { set_move(o, 0x79); return; }   /* cover */
    if (clr(o->b7, 0))   { o->state = 5; return; }
    if (clr(o->b7, 4))   { o->move = o->bc; o->state = 5; return; }
    if (o->b7 & 0x80)    { pursuit_wait(o); return; }                    /* 0x1C390 */
    if (o->b5 & 8)       { followup_arm(o); return; }                     /* 0x1C430 */
    o->move = 0; o->state = 1; o->ae = 0;                                /* approach */
}
void cpu_approach(Obj *o) {                               /* 0x1C93E */
    Obj *p = o->opp; face_opponent(o);
    if (p->f32 & 2) { walk_to(o, tgt_1D068[p->t44]); o->move = 0x3D; o->ae = 0xA; return; }
    if (o->b4 == 0 && o->dy < 0xC) {                      /* near */
        if (o->b9 & 2) { ... idle check ...; return; }
        int ctx = classify(p);                            /* 0x1CB2C: 0 neutral, 2/3 lying, 5/6/7 react1 */
        if (ctx == LYING) { if (p->hp) toprope_eval(o); else set_move(o, 0x48); return; }
        if (p->f33 & 0x10) { if (same_facing) return; policy2(o) or set_move(o, 0x75); return; }
        policy_pick(o, ctx); return;                      /* 0x1DE66 */
    }
    if (o->dy >= 0xC || o->b4 == 1) { o->dir = diag_toward(o); return; }   /* 0x1CD6E */
    /* far, aligned */
    if (p->f33 & 0x10 && roll(tbl_1D030)) { run_away(o); return; }
    if (!(p->f32 & 1) && run_eval(o) == 0) return;        /* 0x1F310 */
    o->dir = (o->b5 & 1) ? 0x40 : 0xC0;                    /* 0x1D01A */
}
void policy_pick(Obj *o, int ctx) {                       /* 0x1DEA6 */
    const u8 *rec = policy[o->id][ctx];
    int n = rec[0]; const u8 *ids = rec + 1, *w = rec + 1 + n;
    int band = o->opp->band; if (clock_min_bcd < 0x10) band = 2;
    if (band) w += n << (band >> 1);
    u8 mv = ids[d100_walk(w)];
    if (mv == 0xFF) { o->state = 1; o->b9 |= 2; o->ae = 0; o->move = 0; return; }
    if (mv == 0xFE) { rope_or_pursue(o); return; }   if (mv == 0xFD) { o->move = 0; return; }
    if (mv == 0x3E && pursue_reposition(o)) return;
    o->move_lo = 0; o->state = 5; o->b5 &= ~0x10; o->b7 &= ~0x40; o->move_id = mv; move_prep(o);
}
void cpu_react(Obj *o) {                                  /* 0x1DA7E, lying */
    Obj *a = o->grap; if (!a || a->state != 5) return;
    if (!(o->b7 & 2)) { o->b7 |= 2;
        const u8 *w = escape_table_for(a->move_id, stage) + o->band * 2;
        if (d100_walk(w) == 0) { o->bc = gate(a->move_id); o->move = (o->sub == 8 ? 0x38 : 0x78); o->b5 |= 0x10; } }
    if ((o->b5 & 0x10) && --o->bc == 0) o->state = 5;
}
```

TODO EXACT: meaning of `+0x31` zone columns and of `+0x2D` angle mapping
(0x40 = toward +y or -y) — 0x1D01A/0x1CD8C; the `+0x34 b2` rescue arm
(0x1C7E2/0x1CFC4) is tag-only and not transcribed; move names for 0x3C, 0x3D,
0x3E, 0x3F, 0x48, 0x72, 0x75, 0x79 are inferred from context (0x79 cover,
0x48 pin, 0x3D pickup, 0x3E pursue, 0x72 running strike, 0x75 counter-grab).
