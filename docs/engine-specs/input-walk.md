# Joystick → walking: C transcription spec

Source: `reference/maincpu.asm` (all PCs are main-CPU ROM addresses).
Cross-checked against `docs/frame-loop.md`, `docs/ai-engine.md`,
`docs/native-rewrite-inventory/gameplay-tables.md`, `src/sincos.c`,
`src/frame_f18a.c`, `src/ported.c` (`ported_load_anim_byte`),
`data/romdata/mv_walk_angle.json`.

Scope: a **human** player's stick in a live match. The CPU pass (`0x1C150`)
skips the virtual controller entirely and steers by writing `+0x2D`/`+0x20`
directly (see `docs/ai-engine.md`); nothing here applies to it except the
shared mover/anim machinery of §6-§7.

## 0. Per-frame schedule (who runs what)

Match frame loop `0xF9A` (even list `0xFB0` / odd list `0x1054`, both run
these in this order — `docs/frame-loop.md`):

| pass | PC | role for walking |
|---|---|---|
| 1 | `0x514E` | input scan: hardware port → `+0xA8/+0xA9`, buttons → `+0xA2/+0xA4/+0xA6` |
| 2 | `0xF18A` | **human** decision pass: `0xDCDE` controller prefix (stand↔walk toggle, attacks, climbs) then state dispatch table `0xF1E4[+0x20 low byte]` — walk angle written here (`0xF2BC`) |
| 3 | `0x1C150` | **CPU** decision pass (skips humans; +0x56 bit6/bit7 gate) |
| 4 | `0xF4C2` | commit `+0x1C ← +0x20` (`0xF4E0-F4F0`), ring-bounds pushback (`0x280DC` + adds at `0xF52E-F542`), anim/state tick `0x1C03E` (arms mover + walk speed), then mover `0x2208` integrates `+0x2C`/`+0x2A` into position |

"Standing" and "walking" are therefore decided in pass 2 and *executed*
(anim + displacement) in pass 4 of the same frame.

## 1. Input plumbing in a live match — `0x514E`

`0x64E0` is the **menu/attract** builder only (called from the mode-select
loop at `0x5372`, and its `0x6534` tail synthesizes attract input). The
in-match equivalent is `0x514E`, called first in both frame lists (`0xFB0`,
`0x106C`).

```
0x514E  A2 = 0x51B4 (consume arm); if $1C0083 & 1 (frame-counter low bit,
        i.e. the ODD frame) A2 = 0x51A4 (accumulate arm)         ; 0x5154-0x5160
0x5166  loop objects via iterator 0x250E (carry = done)
0x5170    skip if +0x56 bit7 (CPU-controlled)                    ; humans only
0x5178    A1 = +0x8A                     ; pointer to the LIVE HW port:
                                         ; 0x140020 (P1) / 0x140022 (P2),
                                         ; installed at 0x6150/0x6280/0x629A
0x517C    D1 = *A1; not.b D1; and #0xFF  ; inputs are active-low
0x5184    move.b D1 -> +0xA9             ; full inverted byte (joy+buttons)
0x5188    andi.w #0xF, +0xA8             ; word +0xA8 &= 0xF  ⇒ after the pair:
                                         ;   +0xA8 (word) == +0xA9 (byte)
                                         ;   == joystick nibble 0..15, HELD level
0x518E    D1 >>= 4; &0xF                 ; button nibble (port bits 4-7)
0x5194    D2 = (D1<<5)&0x100             ; port bit7 -> button-word bit 8
0x519C    D1 = (D1&7) | D2               ; button word: bits0-2 = B1-B3, bit8 = B4
0x51A2    jmp (A2)
0x51A4    (odd frame)  +0xA4 |= D1; D1 = +0xA4          ; accumulate, keep
0x51B4    (even frame) D1 = +0xA4; +0xA4 = 0            ; consume + clear
0x51BC    +0xA6 = (+0xA6 & D1) ^ D1  = D1 & ~old        ; new & !held
0x51C4    +0xA2 = that              ; NEW-PRESS button word (low byte = +0xA3)
0x51CA    +0xA6 = D1                ; HELD button word    (low byte = +0xA7)
```

Resulting object fields, per frame:

| field | contents | held vs new-press |
|---|---|---|
| `+0xA8` (word) / `+0xA9` (byte) | joystick nibble, `~port & 0xF` | **held level** — no edge processing in a match |
| `+0xA4` (word) | button accumulator; OR-ed on odd frames, consumed+cleared on even frames (so a 1-frame tap is never lost across the 2-frame cadence) | latch |
| `+0xA6` (word) | held buttons | held |
| `+0xA2` (word) / `+0xA3` (byte) | newly-pressed buttons (`new & ~held`) | new-press |
| `+0x91` | **menu only.** `0x64E0` (menu) edge-latches the nibble: `+0xA9` is zeroed when equal to `+0x91` (`0x6502-0x6518`), so menu code sees a *new-press* nibble. `0x514E` never touches `+0x91`; in a match `+0xA9` stays a held level. | — |

Menu vs match difference in one line: **menu `0x64E0` = edge-detected nibble
via `+0x91`, buttons in `+0xA6/+0xA2`; match `0x514E` = level nibble, buttons
additionally staged through the `+0xA4` odd/even accumulator.**

Anomaly worth preserving: `0xF3DE` (`move.b $a3.w,D1; andi.b #3`) intends the
object's `+0xA3` (new-press B1/B2 — done correctly at `0xDD06`/`0xDEA6` as
`move.b ($a3,A0)`) but assembles as an **absolute read of ROM 0x0000A3**,
which is `0xFF` (vector-table filler). The test is therefore constant-true.
Transcribe as `if (1)` with a comment, or as `o->btn_new & 3` if you choose
to fix it behind a flag — bit-exactness requires constant-true.

## 2. Stand ↔ walk: the toggle lives in `0xDCDE`, not in the state handlers

Pass 2 (`0xF18A`) per object: skip apron/CPU (`+0x56` bit6/bit7, `0xF196-F1A4`);
if `+0x33` bit1 (human-controllable/legal man) and not `+0x34` bit2 →
`bsr 0xDCDE` (`0xF1A6-F1B6`); clear AI scratch `+0xB4/+0xB6` (`0xF1BA`); if
`+0x21 == 0xFF` skip; dispatch `0xF1E4[+0x20 & 0xFF]` (`0xF1CA-F1DE`).

`0xDCDE` (runs only when `+0x20` bit7 set = state committed, and `+0xFE == 0`
= no pending damage event):

1. `0xDD02` — tag-partner assist (reads `+0xA3` new-press B1/B2). Carry aborts.
2. `0xDE86` — attack selection on `+0xA3 & 3` (new-press B1/B2). Carry aborts.
3. `0xDDC8` — movement selector. Gates: `+0x32` bit0 clear (not running),
   `+0x34` bit1 clear (no incoming grapple). Then, by committed state `+0x21`:

```
0xDDE0  state 1 or state 0 -> 0xDE14
0xDDF2  state 5: anim +0x61==0x15 -> 0xDE4E ; ==0x16 -> 0xDE64 ; else nothing

0xDE14  tst.b +0xA9                   ; joystick nibble, HELD
        == 0 -> 0xDE3C
        != 0:  bsr 0xEDC0             ; corner-climb trigger (uses rope-zone
                                      ;   +0x36 from 0x280DC, corner XY windows,
                                      ;   starts state 8 climb at 0xEEF8);
                                      ;   carry = consumed
               bsr 0xEF0A             ; tag-match rope move ( $1C0161 bit1,
                                      ;   direction held 4 frames via +0xAC/+0xAD
                                      ;   repeat latch 0xEF6A ); carry = consumed
               state==1 -> done       ; already walking
0xDE34         +0x20 = 1              ; *** START WALKING (state 0 -> 1) ***
0xDE3C  (neutral) state==0 -> done
0xDE44         +0x20 = 0              ; *** STOP WALKING (state 1 -> 0) ***

0xDE4E  state5/anim0x15 + stick!=0 -> +0x20=5, +0x60=0x16   ; stance starts moving
0xDE64  state5/anim0x16 + stick==0 -> +0x20=5, +0x44=1, +0x60=0x15
```

So: **standing = `+0x1C`/`+0x20` state 0; walking = state 1 with substate
`+0xAE` (byte `+0xAF`) = 0.** The stick *level* toggles them every frame at
`0xDE34`/`0xDE44`. Releasing the stick is what resets the walk: state → 0,
whose first anim tick (`0x114B2`, first-run latch `bset #7,+0x1C` at
`0x114B2`) clears the mover type byte `+0x01` (`0x114BC`) — the wrestler
stops dead, no decay. `+0xAE` is not rewritten by the toggle; it is 0 in
normal play (cleared at state-machine exits: `0xF33C`, `0xFA50`, `0xFBF6`,
AI-side `0x1C2DC`).

## 3. Walk decision — state 1, substate 0 (`0xF244` → `0xF292`)

State-1 first-pass handler `0xF244` dispatches `0xF25A[+0xAE & 0x3F]`:
sub0 `0xF292` (walk), sub1 `0xF2F6` (run steer), sub2/3 `0xF338` rts,
sub4 `0xF33A` rts (lock-up entry), sub5-10 `0xF33C` (**exit to state 0**:
`+0x20=0`, `+0xAE=0`, `+0x26=0`), sub11-13 `0xF33A` rts.

```
0xF292  btst #1,+0x34                 ; opponent requested a grapple?
        set:  btst #0,+0x33 set -> bclr #1,+0x34 (refuse)      ; 0xF2CE
              else +0x20=1, +0xAE=4 (accept -> lock-up sub4)   ; 0xF2A2
        clear (normal walk):
0xF2B0  A1 = 0xF2D6                   ; walk-angle table
        D1 = +0xA8 (nibble) << 1
0xF2BC  +0x2C = table[nibble]         ; ANGLE word (only low byte +0x2D matters)
0xF2C2  jsr 0x10BE8                   ; auto-face the opponent (see §5)
0xF2C8  bsr 0x10E86                   ; tag-corner touch check (mode-gated:
                                      ;   $1C0161 bit0 clear, +0x33 bit0 set;
                                      ;   in own corner zone -> partner
                                      ;   +0x20=1,+0xAF=1/2 = run-in)
```

The second table read at `0xF498` is the state-**5** first-pass handler
`0xF462` (`0xF1E4[5]`): anims `0x3C/0x3D/0x3E` → `+0x20=0`; anim `0x16`
(the moving stance from `0xDE4E`) → same `+0x2C = 0xF2D6[+0xA8]` write +
`bsr 0x10E86`. I.e. anim 0x16 is "walk while in the 0x15/0x16 stance", same
angle law.

There is **no speed write and no exit-on-neutral here** — neutral never
reaches `0xF292` with the wrestler still walking because `0xDE44` has already
put it back in state 0 earlier in the same routine call chain (DCDE runs
before the `0xF1E4` dispatch in the same `0xF18A` iteration). If a
transcription runs the passes in another order, entry 0 of the table (angle
0x00 = full-speed UP) will leak through — keep the order.

## 4. Walk-angle table `0xF2D6` and the nibble map

16 u16 entries, index = joystick nibble (`+0xA8`), stride 2
(= `data/romdata/mv_walk_angle.json`):

`0000 0040 00C0 0000 0000 0020 00E0 0000 0080 0060 00A0 0000 0000 0000 0000 0000`

Angle unit: 256 = full circle. **0x00 = UP (away, +Y world), clockwise:
0x40 = RIGHT (+X), 0x80 = DOWN (toward camera, −Y), 0xC0 = LEFT.**
Port bits (after inversion): bit0 = RIGHT, bit1 = LEFT, bit2 = UP,
bit3 = DOWN — same decode as the select-screen nav tables
(`docs/native-rewrite-inventory/select-tables.md`) and consistent with the
state-2 handler (`0xF37C`: bit0 → angle 0x40 + face-right).

| nibble | stick | angle | | nibble | stick | angle |
|---|---|---|---|---|---|---|
| 0 | neutral | 0x00 (unreachable while walking — §3) | | 6 | U+L | 0xE0 |
| 1 | R | 0x40 | | 7 | U+R+L | 0x00 (impossible) |
| 2 | L | 0xC0 | | 8 | D | 0x80 |
| 3 | R+L | 0x00 (impossible) | | 9 | D+R | 0x60 |
| 4 | U | 0x00 | | A | D+L | 0xA0 |
| 5 | U+R | 0x20 | | B-F | contradictory | 0x00 |

All 8 real directions map exactly; the diagonals are true 45° table slots
(0x20/0x60/0xA0/0xE0), though the mover renders them anisotropically (§6).
Rows 0x0B-0x0F require opposing switches and are dead padding.

## 5. Facing `+0x2E` — never stick-driven while walking

`+0x2E` byte, **bit7 = horizontal flip** (the word is EOR-ed into the sprite
code word `+0x04` by the anim stepper at `0x1C12C`, so bit7 of the byte is
bit15 = hflip of the attribute word).

* Walking (and standing: `0xF236`; also every AI tick): `0x10BE8` auto-faces
  the opponent each frame. `D0` = opponent X (`+0x7A→+0x06`), plus a lead
  offset ±0x40/±0x10/±0x30 when the opponent is mid-run/whip (opp `+0x21`
  == 4 with `+0x65` 8/9, or `+0x21`==5 with `+0x61` 0x22/0x52, signed by opp
  facing) (`0x10BF4-0x10C44`); then `opponent_x >= own_x ? bset : bclr`
  bit7 (`0x10C46-0x10C58`). So facing flips freely mid-walk, purely from
  relative X; walking left while the opponent is to the right shows a
  back-pedal, which is the arcade behaviour.
* State 2 (`0xF34C`, approach/retreat walk — the "walk while target locked"
  state, `0xF1E4[2]`): the only place stick chooses facing. `D1 = +0xA8 & 3`
  (`0xF36A`): 1 (R) → `+0x2C=0x0040`, `bset #7,+0x2E`; 2 (L) → `+0x2C=0x00C0`,
  `bclr #7,+0x2E` (`0xF37C-0xF398`); 0 → keep facing, walk toward it:
  `+0x2D = ((facing&0x80) ^ 0x80) ? 0xC0 : 0x40` computed as
  `bchg #7 + addi #0x40` (`0xF39C-0xF3AC`). A facing change sets `+0x32` bit2
  (`0xF3B4-0xF3C2`). Entered only after commit (`bset #6,+0x20` latch at
  `0xF34C`).
* Run start (`0x117F6`): facing byte := `(+0x33 & 0x80) ^ 0x80` (side flag),
  once.

## 6. Speed `+0x2A` and the mover

**No immediate is written to `+0x2A` for normal walking.** Every anim-pass
tick of the walk (state 1 sub 0 motion function `0x116C6`, reached via
`0x1C03E` → descriptor `0x11478[1] = 0x115F2` → code ptr `0x11652` →
subtable `0x1167A[+0xAF]`) calls `0x1174C` (`ported_load_anim_byte` in
`src/ported.c`):

```
0x1174C  A3 = (+0x32 bit3) ? 0x116BA : 0x116AE
0x11762  D0 = +0x02                       ; wrestler id (= +0x56 & 0xF, set 0x745A)
0x11766  clr.w +0x2A ; +0x2B = A3[D0]     ; speed = per-wrestler byte
0x11770  CPU (+0x56 bit7) && +0x33 bit5 -> +0x2B += 6      ; AI hustle
0x11786  +0x74 bit7 (carrying weapon)   -> +0x2A word -= 8
```

* Normal walk speed table `0x116AE` (12 ids): `1B 1D 1C 19 1E 1B 18 1E 16 1B 1C 1B`
  — 0x16..0x1E, per wrestler. This is the value in `+0x2B` for a human walk.
* Alt table `0x116BA` (`+0x32` bit3 set — slow/carry variant): `0x0C` ×12.
* Other immediates seen on `+0x2A` are *other substates*: run `0x117B0`/`0x1193A`/
  `0x11A28` = `0x10`; ring-entry intro `0x1CC2` = `0x0C`; they are not the walk.

The same tick sets mover type `+0x01 = 1` on first run (`0x116CE`, guarded by
the `+0x1C` bit7 first-run latch) and selects the walk-cycle descriptor
(`0x11710`: normal `0x115F2` — 4 frames × 10 ticks, sprites 1-4; `+0x32` bit3
→ `0x160A` 4×12 sprites 5-8; weapon `+0x74` bit7 → `0x11622/0x1163A`).
Damage event (`+0xFE ≠ 0`) forces `+0x20 = 0` (`0x11702`).

**Mover** — pass 4 tail `0xF558` → `0x2208`, dispatch on `+0x01`
(0 = none, 1 = polar `0x222C`, 2 = velocity+gravity `0x2252`):

```
0x222C  D0 = +0x2C & 0xFF (angle byte +0x2D) ; D1 = +0x2A & 0xFF (speed +0x2B)
0x2244  bsr 0x22C0     ; quadrant polar: D2 = X delta, D3 = Y delta, 16.16
0x2248  X.l (+0x06) += D2 ; Y.l (+0x0A) += D3    ; positions are 16.16 longs
```

`0x22C0` (ported bit-exact in `src/sincos.c`): quadrant = angle>>6; tables
`0x2378` (X, 65×u16, `[0]=0`, `[64]=0x1400`) and `0x23FA` (Y, `[0]=0x1000`,
`[64]=0`); `mulu` by speed; Q1 negates Y, Q2 negates both, Q3 negates X, with
mirrored index `(0x40-a)<<1` in Q1/Q3. Note the **1.25:1 anisotropy**
(X peak 0x1400 vs Y peak 0x1000): id-3 walk (speed 0x19) = 2.0 px/frame
horizontal, 1.56 px/frame vertical. Angle 0 = +Y = up-screen/away (ring far
edge; the perspective clamp below narrows as Y grows).

**Bounds** — earlier in pass 4, `0x280DC` (in-ring handler `0x2818E`,
`src/floor_2818e.c`): Y clamped to [0x118, 0x198]; X clamped against the
perspective trapezoid `x_min = (Y<<8 + 0x40000)/0x2E0`,
`x_max = −((Y<<8 − 0xA3000)/0x2E0)` (outside 0x220..0x2E0 fast window); Z
floor 0x140. Violations write pushback into `+0x38/+0x3A/+0x3C` + flags
`+0x37`, rope-zone code `+0x36` (consumed by the corner-climb gate `0xEDC0`),
applied at `0xF52E-F542`. Walking never writes velocity fields; it moves only
through the polar mover.

## 7. State-machine context

* `+0x20` = requested state word (low byte `+0x21`); high-byte bit7 =
  "committed" latch (`0xF4EC`). `+0x1C` = committed state (low byte `+0x1D`),
  copied from `+0x20` at `0xF4E6` with previous state parked in `+0x1E`;
  its high-byte bit7 = "anim initialised" latch (set by each state's motion
  function on first run).
* `0xF1E4` (13 entries, human pass): 0 stand `0xF218`, 1 free-move `0xF244`,
  2 locked approach `0xF34C`, 3/4 rts, 5 scripted-anim `0xF462` (incl. the
  anim-0x16 walk), 6-8 rts, 9 `0xF4AA` (down: UP held → state 0xA get-up),
  10-12 rts.
* State 0 = **standing**: first-pass `0xF218` only accepts grapples
  (`+0x34` bit1 & `+0x33` bit0 clear → state 1 sub 4) or re-faces
  (`0x10BE8`). Its anim tick `0x114B2` zeroes the mover (`+0x01=0`), zeroes
  pushback scratch, and shows sprite frame 0 + facing (`0x11564`).
* State 1 sub 0 = **walking** (this spec). Ticked by: `0xDCDE` (enter/leave,
  §2), `0xF292` (angle, §3), `0x116C6` (speed/anim/mover arm, §6), `0x2208`
  (motion, §6). Sub 1 = run: angle is *not* stick-driven; `0xF2F6` only bends
  it vertically (`+0xA9` bit2 → `+0x2C=0x0008`, bit3 → `0x0088`, `not.b +0x2D`
  when `+0x33` bit7; else `+0x2C=0`), speed 0x10 (`0x117B0`).
* Walking into the opponent: pass-2 helper `0xF574` (from `0xF560`) —
  both in state 1, facing opposed (`0xF594`), |ΔX|<0x48, |ΔY|<0x10 →
  tie-up states 0x0B/0x0C/0xFF (`0xF726-0xF77E`). That is why walking is a
  distinct state at all.
* Reset on release: `0xDE44` (§2). Reset on damage: `+0xFE` event → state
  machine via `0x11702`/`0x115B6`. Reset on move end: sub5-10 → `0xF33C`.

## 8. C sketch

```c
/* Walk-angle by joystick nibble — ROM 0xF2D6 (mv_walk_angle.json). */
static const uint16_t mv_walk_angle[16] = {
    0x0000, 0x0040, 0x00C0, 0x0000, 0x0000, 0x0020, 0x00E0, 0x0000,
    0x0080, 0x0060, 0x00A0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};
/* Per-wrestler walk speed, id = +0x02 (= +0x56 & 0xF) — ROM 0x116AE/0x116BA. */
static const uint8_t walk_speed[12]      = { 0x1B,0x1D,0x1C,0x19,0x1E,0x1B,
                                             0x18,0x1E,0x16,0x1B,0x1C,0x1B };
static const uint8_t walk_speed_slow[12] = { 0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,
                                             0x0C,0x0C,0x0C,0x0C,0x0C,0x0C };

/* Pass 1 — ROM 0x514E..0x51D0, every frame, humans only (+0x56 bit7 clear).
 * odd_frame = $1C0083 & 1 (frame-counter low bit). port_bits = *(+0x8A),
 * i.e. the live word at 0x140020/0x140022 (active low). */
void input_scan(obj *o, unsigned port_bits, int odd_frame)
{
    unsigned inv = (~port_bits) & 0xFF;                 /* 0x517C-0x5180 */
    o->joy      = inv & 0x0F;                           /* +0xA8/+0xA9, HELD  0x5184-0x5188 */
    unsigned bt = ((inv >> 4) & 7) | ((inv & 0x80) << 1); /* b1-3 + bit8   0x518E-0x51A0 */
    unsigned lvl;
    if (odd_frame) { o->btn_acc |= bt; lvl = o->btn_acc; }   /* 0x51A4 */
    else           { lvl = o->btn_acc; o->btn_acc = 0; }     /* 0x51B4 */
    o->btn_new  = lvl & ~o->btn_held;                   /* +0xA2 (+0xA3)  0x51BC-0x51C4 */
    o->btn_held = lvl;                                  /* +0xA6          0x51CA */
    /* +0x91 untouched in-match: it is the 0x64E0 menu edge latch. */
}

/* Pass 2 — the walking part of 0xDCDE + 0xF1E4 dispatch, one human object.
 * Call only when +0x33 bit1 set, +0x34 bit2 clear (0xF1A6-F1B4), state
 * committed (+0x20 bit7) and no pending event (+0xFE == 0) (0xDCDE). */
void walk_input(obj *o, unsigned port_bits)
{
    (void)port_bits;              /* already digested by input_scan (pass 1) */

    /* ---- 0xDDC8/0xDE14: stand<->walk toggle on the HELD nibble ---- */
    if (!(o->f32 & 0x01) && !(o->f34 & 0x02)) {         /* not running, no grapple req */
        if (o->state == 0 || o->state == 1) {           /* 0xDDE0-0xDDEE */
            if (o->joy != 0) {                          /* 0xDE14 */
                if (!climb_check(o)   /* 0xEDC0: rope zone +0x36 & corner XY -> state 8 */
                 && !tag_rope_move(o) /* 0xEF0A: $1C0161 bit1, 4-frame hold via +0xAC */
                 && o->state == 0)
                    o->state_req = 1;                   /* START WALK  0xDE34 */
            } else if (o->state != 0) {
                o->state_req = 0;                       /* STOP WALK   0xDE44 */
            }
        }
        /* state 5 stance pair 0x15/0x16 handled at 0xDE4E/0xDE64 (same law) */
    }

    /* ---- 0xF1E4[1] -> 0xF244 -> sub 0 0xF292: the walk decision ---- */
    if (o->state == 1 && (o->substate & 0x3F) == 0) {
        if (o->f34 & 0x02) {                            /* grapple request  0xF292 */
            if (o->f33 & 0x01) o->f34 &= ~0x02;         /* refuse           0xF2CE */
            else { o->state_req = 1; o->substate = 4; } /* lock-up          0xF2A2 */
        } else {
            o->move_angle = mv_walk_angle[o->joy & 0xF];/* +0x2C            0xF2BC */
            face_opponent(o);                           /* 0x10BE8: bit7 of +0x2E =
                                                           (opp_x + lead >= own_x) */
            tag_corner_touch(o);                        /* 0x10E86 */
        }
    }
}

/* Pass 4 (excerpt) — commit + walk tick + mover, ROM 0xF4C2/0x1C03E/0x2208. */
void walk_commit_and_move(obj *o)
{
    if (!(o->state_committed)) { o->prev_state = o->state; o->state = o->state_req;
                                 o->state_committed = 1; }   /* 0xF4E0-F4F0 */
    ring_bounds(o);                                          /* 0x280DC + 0xF52E-F542 */

    if (o->state == 1 && (o->substate & 0x3F) == 0) {        /* 0x116C6 via 0x1C03E */
        if (!o->anim_latch) { o->mover = 1; o->anim_latch = 1; } /* 0x116CE */
        unsigned id = o->wrestler_id;                        /* +0x02 */
        unsigned spd = ((o->f32 & 0x08) ? walk_speed_slow
                                        : walk_speed)[id];   /* 0x1174C */
        if (o->cpu && (o->f33 & 0x20)) spd += 6;             /* 0x11780 */
        o->speed = spd;                                      /* +0x2A/+0x2B */
        if (o->f74 & 0x80) o->speed -= 8;                    /* weapon  0x1178E */
        walk_anim_select(o);                                 /* 0x11710 */
        if (o->event) o->state_req = 0;                      /* 0x11702 */
    } else if (o->state == 0) {
        if (!o->anim_latch) { o->mover = 0; o->anim_latch = 1; } /* 0x114B2/0x114BC */
    }

    if (o->mover == 1) {                                     /* 0x2208 -> 0x222C */
        int32_t dx, dy;
        sincos_22c0(o->move_angle & 0xFF, o->speed & 0xFF, &dx, &dy);
        o->x_fp += dx;   /* +0x06 16.16 */                   /* 0x2248 */
        o->y_fp += dy;   /* +0x0A 16.16; angle 0 = +Y = up/away */
    }
}
```

`sincos_22c0` is already transcribed bit-exact in `src/sincos.c` (tables
`0x2378`/`0x23FA`, quadrant negation).

## 9. Open labels (mechanics exact, names uncertain)

* Anim pair `0x15/0x16` in state 5 (the `0xDE4E/0xDE64/0xF484` stance) —
  behaves as a stance with a walking variant; exact move unidentified.
* `+0x33` bit0 (gates grapple-refusal `0xF2CE`, `0x10E86`) and `$1C0161`
  bit0/bit1 mode flags (bit0 set by LEFT at mode select per
  `docs/rom-tables.md`; `0x10E86` tag-touch runs when bit0 clear, `0xEF0A`
  when bit1 set) — semantics per observed gating only.
* State-1 subs 2/3 (`0xF338` rts / motion `0x1192C`,`0x11A20`) untraced here.
