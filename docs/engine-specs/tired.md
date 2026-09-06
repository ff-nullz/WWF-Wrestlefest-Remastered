# Tired / puffed-out behaviour — 68k transcription (read-only, no compile)

Source: `reference/maincpu.asm`; bytes cross-checked against the interleaved
program ROM (`31e14-0.ic18` even / `31e13-0.ic19` odd). Field names as engine.

## 0. Headline corrections to the playtest model

1. **The stomach-hold slow walk is NOT driven by the energy band.** It is a
   per-object flag, `+0x32` BYTE bit3 (= word `f32` bit 11), set **on the
   victim by 21 move handlers** (list §2) at the moment they are thrown, and it
   lasts **0xD0 ticks of standing/walking** after the first stand
   (`+0xD0` countdown in the per-frame damage loop `0x24EA0`). Running clears
   it at once (`0x117A4`). No reader of `+0x70/+0x71` exists anywhere in the
   stand/walk/run handlers `0x114B2..0x11B6C`.
2. **The standing "puffed out" state is reaction 1 (dizzy) and is entered only
   via `+0x64` bit5 (word bit13) at get-up.** Band does not gate it at
   `0x11E42`. Band 2 instead (a) sets `+0x64` bit7 (word bit15) on every hit
   (`0x24E44`) = critical art + face-down lying, and (b) lengthens the
   forced-down time `+0x9A` (`0x10F56`: 0x30 / 0x80 / 0xE0 by band).
3. The "choke" is not a separate category. A dizzy victim loses the front
   tie-up **instantly** in the HIT machine (`0xF698/0xF6AA` -> `0xF742/0xF760`),
   giving the attacker state 0xC (hold) straight away; the hold then offers
   cat 9 moves 0x15/0x17. From behind, cat 0x10 (`0xE382`) offers 0x1C/0x1D.

## 1. Walk / stand variants

### 1a. Cell records (bytes from ROM)

```
114AC  stand   00 01 14 B2 | 00 00                         handler 0x114B2, mode 0 (spr = +0x2E)
115F2  walk    00 01 16 52 | 00 02 | 00 04 | 0A 0A 0A 0A | spr 01 02 03 04    normal, 4x10 ticks
1160A  walk-T  00 01 16 52 | 00 02 | 00 04 | 0C 0C 0C 0C | spr 05 06 07 08    TIRED (f32 byte bit3), 4x12
11622  walk-W  00 01 16 52 | 00 02 | 00 04 | 14 10 14 10 | spr 7C 7D 7E 7F    weapon, +0x75 bit0 clear
1163A  walk-W2 00 01 16 52 | 00 02 | 00 04 | 14 10 14 10 | spr 80 81 82 83    weapon, +0x75 bit0 set
```
Sprites 5..8 are the stomach-hold walk. **There is no tired stand cell**: stand
`0x11564` always writes `spr = +0x2E` (sprite 0 | facing); weapon stand is
`0x7E`/`0x82` (`0x11586/0x1158C`). A tired wrestler standing still shows the
normal stance.

### 1b. Speed bytes

```
116AE normal:  1B 1D 1C 19 1E 1B 18 1E 16 1B 1C 1B    (per wrestler id +0x02)
116BA tired:   0C 0C 0C 0C 0C 0C 0C 0C 0C 0C 0C 0C    (all 12)
```

### 1c. Walk dispatcher `0x11652` and plain walk `0x116C6`

```
11652  if !(0x1C007C bit7):  bclr #3,(+0x32)       ; tired flag only lives while
                                                    ;   global 0x1C007C b7 set (TODO EXACT:
                                                    ;   set 0x9B2/0xAA4 on start accept,
                                                    ;   cleared 0x6FC/0x924)
11662  jsr 0x1167A[(+0xAE & 0x3F)*4]               ; sub 0 = 0x116C6 plain walk
116C6  if !(anim_sel b15): mover=1; bclr #6,(+0x33)
116DA  bsr 0x1174C                                  ; speed (below)
116DE  bsr 0x115D2                                  ; stale link clear
116E2  atk(+0x4C) = 1
116E8  if +0xAF < 5 && +0x74 b7: atk = 0xE          ; weapon stance
116FE  bsr 0x11710                                  ; cell select (below)
11702  if +0xFE != 0: state = 0
```

### 1d. Cell select `0x11710` (returns A1 = cell)

```
11710  if +0x74 b7 (weapon): +0x1B = 0x10; A1 = (+0x75 b0) ? 0x1163A : 0x11622; rts
11736  A1 = 0x115F2
1173C  if +0x32 byte b3 (tired): A1 = 0x1160A
1174A  rts
```

### 1e. Speed `0x1174C`

```
1174C  A3 = (+0x32 byte b3) ? 0x116BA : 0x116AE
11762  D0 = +0x02 (wrestler id); +0x2A = 0; +0x2B = A3[D0]      (speed word = 00xx)
11770  if +0x56 b7 (human) && +0x33 b5 (comeback, §4c): +0x2B += 6
11786  if +0x74 b7 (weapon): +0x2A -= 8
11794  rts
```
So tired walk = speed 0x0C (vs 0x16..0x1E), 12-tick frames (vs 10), sprites 5-8.

### 1f. Stand handler tired-related lines `0x114B2`

```
114B2  first frame (bset #7,+0x1C was clear): mover=0; bclr b3,b4,b6 (+0x33); bclr b2 (+0x32)
       clr +0x12,+0xAA,+0x44,+0x46,+0x24,+0x22,+0x18,+0x1A,+0x3E,+0x40,+0x42
11504  if +0x32 byte b3 && !(0x1C007C b7): bclr #3,(+0x32)        ; same gate as 0x11652
1151E  if +0xD0 == 0: +0xD0 = 0xD0                                ; arm the slow-walk timer
1152A  if +0x32 b4: state=5, move_id=0x7A                         ; (unrelated)
```

### 1g. Slow-walk expiry — damage loop `0x24E58` (runs every object every frame)

```
24E92  if --combo_t(+0x54) < 0: combo_t = 0; combo(+0x52) = 0
24EA0  if +0x32 byte b3 && +0xD0 != 0: if --+0xD0 == 0: bclr #3,(+0x32)
```
=> tired walk lasts **0xD0 = 208 frames** counted from the first stand after the
throw (timer armed at `0x11524`, decremented only while the flag is set).
`0x1B330` (lying entry) clears `+0xD0` so the count restarts after each knockdown.

### 1h. Other writers of `+0x32` byte bit3

```
set  (on VICTIM A2): 21 move handlers, §2b list (always paired with bset #5,+0x64 except
                     0x1518C and the 0x16D6C.. group)
clr  0x11516 stand gate, 0x1165C walk gate, 0x117A4 RUN ENTRY (run cancels tired walk),
     0x187D4 (move 0x4C handler 0x1878A), 0x18A46 (move 0x4D handler 0x18948), 0x24EB4 expiry
```
**Can a tired wrestler run? Yes** — cat row 0xFF -> state 2 at `0xDFC8` has no
flag test, and `0x117A4` simply clears the tired flag.

## 2. Get-up `0x11E42` (state 7, cell `0x11E36` = `00 01 1E 42 | 00 01 | 00 01 | 00 10 | 00 68`)

```
11E42  if !(anim_sel b15):                          ; first frame
         mover=0; bclr b3,b4,b6 (+0x33); clr +0xAA (mash); clr +0x9A (down_t); rts
11E6A  if +0x25 != 0xFE: rts                        ; wait for the 17-tick frame to end
11E72  0x1C1697 = 1                                  ; sprite-order refresh
11E7A  if +0xFE != 0              -> state 0         ; out-of-ring / match-over: never dizzy
11E80  if +0x1F == 5              -> state 0         ; prev-state low byte == 5 (TODO EXACT: +0x1E
                                                     ;   semantics) -> straight to stand
11E88  if bclr #5,(+0x64) was SET -> state 4, react_id = 1     ; *** the ONLY dizzy path ***
       else                        -> state 0
```
**Band is not consulted.** Only react word bit13 ("rise dizzy"). Note `0x1B3CA`
(lying -> state 7) clears bit7/bit15 but leaves bit13.

### 2b. Every writer of `bset #5,(+0x64)` (react bit13), all on the VICTIM (A2)

| PC | move_id | handler | condition at the write |
|---|---|---|---|
| `0x150E8` | 0x19 falling press slam | `0x15006` | over-the-ropes variant (frame 3, X clipped); react=0x25 |
| `0x1515A` | 0x19 | `0x15006` | normal release end of frame 5; react=0x21 |
| `0x152FA` | 0x1A | `0x15228` | +0x60 b7 variant path; victim state5 move 0x5E, dmg 0x16 (+bit3) |
| `0x15376` | 0x1A | `0x15228` | normal release (+bit3) |
| `0x1621E` | 0x25 | `0x1612E` | +0x60 b7 clear path; victim state5 move 0x60 (+bit3) |
| `0x16326` | 0x26 | `0x16258` | react=5, dmg 0x16, sound 0x32 (+bit3) |
| `0x16502` | 0x28 | `0x16436` | react=5, dmg 0x12 (+bit3) |
| `0x167B6` | 0x2B | `0x1669A` | +0x60 b7 clear; react=0x25, dmg 0x18 (+bit3) |
| `0x16848` | 0x2B | `0x1669A` | +0x60 b7 clear alt; react=0x24, dmg 0x18 (+bit3) |
| `0x16C28` | 0x2E | `0x16A7A` | react=6, dmg 0x10 (+bit3) |

No other `bset #5,(+0x64)` exists (grep of `08E. 0005 0064` / `08EA 0005 0064`).
The remaining bit3-only (tired walk, no dizzy) writers: `0x1518C` (0x19),
`0x16D6C/0x16DA8` (0x2F), `0x16F7A` (0x30), `0x17112` (0x31), `0x17376/0x17404`
(0x33), `0x1754E` (0x34), `0x17A0E` (0x36), `0x17C80` (0x3A), `0x17F72` (0x3B),
`0x1870A` (0x44).

### 2c. Band-driven writes that LOOK like "rise puffed out"

```
24E44  (victim setup 0x24E0x): if band(+0x70) == 2: bset #7,(+0x64)   ; word b15 = critical art
1B208  (bounce react 5 handler 0x1B1D0): if +0x71 == 2 && react != 0x2A: bset #7,(+0x64)
1BDB8  (react handler 0x1BD..): +0x65 = (+0x71 == 2) ? 9 : 8         ; face-down lying when band 2
1BB92  (react handler 0x1BB6C): spr = (+0x71 == 2) ? 0x1F : 0x14
10F56  forced-down seed (jsr from 0x15F82, 0x1789C, 0x18CD6, 0x1B348 lying entry, 0x1E2A0):
         if +0x9A != 0: rts                              ; a move already seeded it
         if human(+0x56 b7) && (+0x33 b5 || +0x33 b2): +0x9A = 0x30   ; comeback/"hurry"
         elif +0x71 == 2: +0x9A = 0xE0
         elif +0x71 == 1: +0x9A = 0x80
         else:            +0x9A = 0x30
1B3B8  lying handler: if +0x9A: --+0x9A, at 0 -> state 7 (get-up), bclr #7,(+0x64)
```
So a band-2 wrestler lies down 0xE0 frames (vs 0x30 healthy) — the
"can't get up" part of puffed-out — then rises normally unless bit13 was set.
Band calc `0x24EC2`: `+0x70 = 2 if HP(+0x66) <= 0x18, 1 if HP < 2*(max/3)..`
(loop: D0=0x18, then D0=(max/3)<<1; band = 2,1,0) — matches hit-pipeline.md.

## 3. Dizzy reaction 1 — `0x1B0CC` (cell `0x1B0BC` = `00 01 B0 CC | 00 02 | 00 02 | 10 10 | spr 18 19`)

```
1B0CC  if !(anim_sel b15):                     ; first frame
         mover(+0x01) = 0
         hold_t(+0x46) = 0x80                  ; CONSTANT — no band, no wrestler table
         if combo(+0x52) == 0: combo = 4; combo_t(+0x54) = 0x40
         rts
1B0F2  jsr 0x115D2                             ; drop stale partner link
1B0F8  atk(+0x4C) = 1                          ; standing hurtbox -> fully hittable
1B0FE  if --hold_t != 0: rts
1B104  state = 0; 0x1C1697 = 1                 ; 128 frames later: stand
```
- **Input is never read**: the selector `0xDF.. ` only matches own state
  0/1/2/5/9/0xB/0xC; state 4 matches nothing, so buttons/stick do nothing.
  No mash shortens it.
- **Cut short only by being hit** (victim re-entry `0x2442A` writes state 4 / a
  new react) or grabbed (cat 0x10 / tie-up below).
- `combo=4/combo_t=0x40` pre-loads the combo counter so the first strike on a
  dizzy victim already counts as the 4th+ hit (knockdown tier); `combo_t`
  decays in `0x24E92` every frame (0x40 -> clears combo 64 frames later, i.e.
  well before hold_t expires; the next hit within the window keeps it).

### 3b. Opponent's options against a dizzy victim

**From behind — cat 0x10 `0xE382`** (exact):
```
E386  own +0x34 b5 clear; own state 0, or state 1 with +0xAF == 0
E3AA  opp +0x26 == 0; opp state 4; opp +0x65 == 1 (react low byte = dizzy)
E3C6  |dy| < 0x10; |dx| < 0x50; own +0x2E == opp +0x2E (same facing = behind)
E3FC  D0 = 0x10; bsr 0xF178 (link); clr.w opp +0x52 (combo reset); -> row fetch
```
Map rows `0xE4FE` (cat 0x10 = bytes [0x30..0x32]; cols B1 / B2 / both):
```
all 12 wrestlers:  1C 1D FF      -> B1 = move 0x1C behind grab, B2 = 0x1D behind lift /
                                     atomic-drop hold, both = FF (run)
```
Handlers: 0x1C cell `0x15668` {`0x1567C`, mode1, n3, dur 0C 16 16, spr 161 162 163};
0x1D cell `0x1585C` {`0x15870`, n3, dur 0C 16 16, spr 161 16C 16D}. Both re-check
`victim state 4 && +0x65 == 1` at catch (`0x15716`, `0x158D2`), victim -> state 5
scripted move 0x52/0x53.

**From the front — no category.** Cat 0 row (`00 72 FF`) is the normal strike /
grapple. The grapple enters the HIT machine `0xF18A`, whose tie-up resolution
short-circuits on a dizzy side:
```
F690  if A0 (attacker) state 4 && +0x65 == 1 -> 0xF742: A0 state = 0xFF (held), A1 state = 0xC,
                                                 A1 +0xBC = 0, A0 +0x44 = 0, A1 +0x44 = 0x2000
F6A2  if A1 (defender) state 4 && +0x65 == 1 -> 0xF760: A0 state = 0xC (HOLD), A0 +0xBC = 0,
                                                 A1 state = 0xFF, A0 +0x44 = 0x2000, A1 +0x44 = 0
F77E  both X,Y = midpoint; +0x48 ++ ...
```
i.e. **the dizzy wrestler never gets the collar-and-elbow mash; the other side
takes the hold (state 0xC) immediately** — that is the "choke you can apply
directly". From the hold, **cat 9 `0xE1B0`** (state 0xC, `+0x45 == 1`):
```
all 12 wrestlers:  15 17 15      -> B1/both = move 0x15 front facelock stance
                                     (cell 0x14CE0, 1 dmg per wrench cycle),
                                     B2 = 0x17 whip (cell 0x14E2C)
```
then the stance matrix cats 0xA-0xD (`0xE232[opp band*0x18 + ...]`) for throws.

Other dizzy-sensitive rows for completeness (`0xE4FE` per wrestler 0..B):
```
cat 0x05 (lying front B1/B2/both): 08 48 22 x2, 08 48 09 (w2), 08 48 22 x3, 08 48 09 (w7),
                                   08 48 22, 08 48 09 (w9), 08 48 22 x2
cat 0x06: 0A 48 0A (all)      cat 0x07: 35/0E/0B/0E/14/46/10/47/23/13/47/10, 48, FF
cat 0x12: 0A 10 FF (w0-3,8,A), 0A 46 FF (w4,5,7,9), 0A 0B FF (w6), 0A 47 FF (wB)
cat 0x14: 00 72 FF (all)
```
`0xF842`, `0x1C662/0x1CAFC/0x1CC78` also test `+0x65 == 1` (HIT machine / AI
target checks) — not transcribed, TODO EXACT if AI needs them.

## 4. Other low-energy behaviour

### 4a. Run — allowed when tired (§1h); **running regenerates HP**
```
11858  (run sub-handler 0x11796, every tick): bsr 0x11900;
       if HP(+0x66) < max(+0x72): HP += 1; +0x6A = 1   (HUD smear +1)
119A8  (run sub-handler 0x1192C): identical
```
One HP per frame while in state 2. No regen while standing/walking (no other
`+0x66` add inside `0x114B2..0x11B6C`). Other adders `0x8F38`, `0x9102`,
`0x10810` are outside the match loop (round reset / HUD) — TODO EXACT.

### 4b. Tie-up resolution bias by band
None in the HIT machine beyond the dizzy short-circuit above; `0xF6B4..`
branches on human/CPU (`+0x56 b7`) and difficulty `0x1C0161 b0` (tables
`0xF8D0` / `0xF878`), not on `+0x70`. Band does index the AI/selector
matrices: `0xE070` (running cat, opp band rows), `0xE232` (stance throws),
`0xE33A` (anti-run), and AI tables at `0x1E944` (`0x1E9FA + band*2`),
`0x1F20A`, `0x1F55C` (`0x1F576`) — AI only.

### 4c. The "hurry"/comeback cue — `+0x33` bit5 (per-frame scan `0xFD00`)
```
FD00  for 6 objects at 0x1C05B0 stride 0x10C:
FD08   if +0x33 b5:  if +0xC8: --+0xC8, !=0 -> next
                     else: +0xC8 = 0; +0xC6 (hits-taken) = 0; bclr #5,(+0x33); jsr 0x8962
FD36   elif CPU (+0x56 b7 clear):
           if !(+0x32 b0): +0xC8 = 0
           elif +0xC8 >= 0xFDB4[+0x57*2]: bset #6,(+0xC6); +0xC8 = 0x3A9
FD72   else (human): if +0xC7 >= 0xFDCC[+0x57]:
                     +0xC8 = 0xFDD8[difficulty 0x1C0162 *2]; bset #5,(+0x33)
FDB4 (CPU, words): 0930 0846 0B7E 0A1C 0B08 08BC 0A90 0A1C 09A6 0B08 0930 0B08
FDCC (human hits-taken threshold, per +0x57): 13 11 18 15 13 10 15 15 14 18 12 17
FDD8 (boost duration by difficulty): 01D6 024C 024C 02C2
```
While `+0x33` b5: human walk speed +6 (`0x11780`), forced-down fixed 0x30
(`0x10F64`), and `0x10F3A` also sets it when the partner is in state 5 move
0x71 (TODO EXACT: that routine's purpose). This is the only "hurry" cue found;
there is no band-driven announcer/flash in the stand/walk path.

## 5. Engine write tables (PC order, engine names)

```
walk  0x11652  if !g_start_accepted: f32 &= ~BIT11(tired)
      0x116CE  first: mover=1; f33 &= ~b6
      0x1174C  speed = (f32&BIT11 ? 0x0C : walk_tab[id]) + (human && f33&b5 ? 6 : 0) - (weapon ? 8 : 0)
      0x116E2  atk = 1 (0xE weapon)
      0x11710  cell = weapon ? (0x1163A|0x11622) : (f32&BIT11 ? 0x1160A : 0x115F2)
stand 0x11516  if f32&BIT11 && !g_start_accepted: clear
      0x11524  if slowwalk_t(+0xD0)==0: slowwalk_t = 0xD0
      0x11564  spr = facing (no tired stance)
dmg   0x24EA0  if f32&BIT11 && slowwalk_t: if --slowwalk_t==0: f32 &= ~BIT11
run   0x117A4  f32 &= ~BIT11;   0x11866 hp = min(hp+1, hp_max), smear=1 (per tick)
getup 0x11E42  first: mover=0; f33 &= ~(b3|b4|b6); mash=0; down_t=0
      0x11E7A  end: ring_out ? stand : prev==5 ? stand : (react_id&0x2000 ? {react_id&=~0x2000;
               state=4; react_id=1} : stand)
dizzy 0x1B0CC  first: mover=0; hold_t=0x80; if !combo {combo=4; combo_t=0x40}
      0x1B0F8  atk=1; if --hold_t==0: state=0
victim 0x24E4C  if band==2: react_id |= 0x8000
lying 0x1B348  down_t = (human && f33&(b5|b2)) ? 0x30 : band==2 ? 0xE0 : band==1 ? 0x80 : 0x30
```

## 6. C sketch

```c
/* eng_obj: f32 is the +0x32 WORD; the ROM's "bset #3,(+0x32)" is the HIGH byte,
 * i.e. word bit 11.  (engine/anim.c header comment "bit3 run" is wrong: run-in is
 * byte +0x32 bit0 = word bit 8.)  */
#define F32_TIRED   0x0800u          /* +0x32 byte b3 */
#define F33_HURRY   0x0020u          /* +0x33 byte b5 (low byte of the same word) */

static uint32_t handler_stand(eng_obj *o, uint32_t cell)
{
    ...existing...
    if ((o->f32 & F32_TIRED) && !cur_st->start_accepted) o->f32 &= ~F32_TIRED; /* 0x11504 */
    if (o->slowwalk_t == 0) o->slowwalk_t = 0xD0;                               /* 0x1151E */
    o->spr = o->facing;  o->atk = 1;
    return cell;
}

static uint32_t handler_walk(eng_obj *o, uint32_t cell)
{
    if (!cur_st->start_accepted) o->f32 &= ~F32_TIRED;                 /* 0x11652 */
    if (!(o->anim_sel & 0x8000u)) o->mover = 1;                        /* 0x116CE */
    /* 0x1174C */
    o->speed = (o->f32 & F32_TIRED) ? 0x0C : (uint16_t)speed_tab[o->wrestler];
    if (o->human && (o->f32 & F33_HURRY)) o->speed += 6;
    if (o->weapon) o->speed -= 8;
    o->atk = (o->weapon && o->sub < 5) ? 0xE : 1;                      /* 0x116E2/0x116F8 */
    /* 0x11710 */
    if (o->weapon) return (o->f75 & 1) ? 0x1163Au : 0x11622u;
    return (o->f32 & F32_TIRED) ? 0x1160Au : 0x115F2u;
}

/* damage loop 0x24E92/0x24EA0 — per object per frame, in the existing hit pipeline */
static void slowwalk_tick(eng_obj *o)
{
    if (o->combo_t-- == 0) { o->combo_t = 0; o->combo = 0; }
    if ((o->f32 & F32_TIRED) && o->slowwalk_t && --o->slowwalk_t == 0)
        o->f32 &= ~F32_TIRED;
}

static uint32_t handler_run(eng_obj *o, uint32_t cell)       /* additions */
{
    if (!(o->anim_sel & 0x8000u)) o->f32 &= ~F32_TIRED;     /* 0x117A4 */
    if (o->hp < o->hp_max) { o->hp++; o->hp_smear = 1; }    /* 0x11866 / 0x119A8 */
    ...
}

static uint32_t handler_getup(eng_obj *o, uint32_t cell)
{
    o->clip_h = 0;
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; o->down_t = 0; o->mash = 0;
        o->f33 &= ~(0x08|0x10|0x40); return cell; }
    if ((o->frame & 0xFFu) != 0xFEu) return cell;
    if (o->ring_out || (o->prev_state & 0xFF) == 5) { o->state = 0; return cell; }   /* 0x11E7A/0x11E80 */
    if (o->react_id & 0x2000u) { o->react_id = 1; o->state = 4; }                     /* 0x11E88 -> 0x11E98 */
    else o->state = 0;
    return cell;
}

/* handler_dizzy: engine version already matches 0x1B0CC exactly (0x80 const, combo 4/0x40,
 * atk=1, no input).  Add the 0x115D2 link-drop call if links are modelled. */

/* Move handlers listed in §2b: on throw release
 *     v->react_id |= 0x2000;  v->f32 |= F32_TIRED;     (both, at the PCs listed)
 * and the bit3-only group:  v->f32 |= F32_TIRED;
 * Victim setup 0x24E4C:     if (v->band == 2) v->react_id |= 0x8000;
 */

/* core.c selector additions */
/* cat 0x10 (0xE382): own state 0 or (1 && sub==0), own f34 b5 clear, opp.partner==0,
 *   opp.state==4 && (opp.react_id&0xFF)==1, |dy|<0x10, |dx|<0x50, same facing;
 *   link(); opp->combo = 0; row = map[id][0x10*3 + col] -> 0x1C / 0x1D / 0xFF(run) */
/* HIT machine tie-up (0xF690): if either side is state 4 react 1, the OTHER side takes
 *   state 0xC with grap44 = 0x2000, the dizzy side state 0xFF; skip the mash entirely.
 *   Then cat 9 (state 0xC, hold_t low byte == 1): 0x15 / 0x17 / 0x15. */
```

## 7. TODO EXACT
- `0x1C007C` bit7 meaning (set `0x9B2/0xAA4`, cleared `0x6FC/0x924`); assumed
  "a start was accepted / match live". If wrong, the tired-walk gate inverts.
- `+0x1E/+0x1F` at `0x11E80` — assumed previous-state word.
- `0x10F3A` `+0x33` b5 setter (partner state5 move 0x71) and `0x8962` on expiry.
- `0xF842`, `0x1C662`, `0x1CAFC`, `0x1CC78` dizzy tests (AI side).
- `0x8F38/0x9102/0x10810` HP adds — confirm out-of-match.
