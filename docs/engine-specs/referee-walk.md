# Referee movement — exact transcription (reference/maincpu.asm)

Referee object = `$1C11F4` (NOT one of the 9 `$1C05B0+n*0x10C` wrestler slots, so no
wrestler collision/hit scan ever reaches it — see §4). Sprite table id `+0x02 = 0x0C`
(stream row 12). Field names below are engine names: x `+0x06` (16.16, int word at +0x06),
y `+0x0A`, z `+0x0E`, mover `+0x01`, facing `+0x2E` (bit7 = face left), speed `+0x2A`
(word; magnitude in low byte `+0x2B`), angle `+0x2C` (word; low byte `+0x2D`), state
`+0x20`, SM index `+0x21`, visual latch `+0x1C`/`+0x1D`, tick `+0x23`, cell count `+0x25`,
pose `+0x05` (rendered facing byte `+0x04`), probe out `+0x36` (byte hit flag) / `+0x37`
(rope bits) / `+0x38`,`+0x3A`,`+0x3C` (pushback), f35 `+0x35`, target ptr `+0x56`,
idle target x/y `+0x30`/`+0x1E`, approach dest `+0x60`/`+0x62`, legal-man ptrs `+0x4C`/`+0x50`.

Angle convention (mover 1 = `0x222C` → `0x22C0` quadrant jump, tables `0x2378` cos /
`0x23FA` sin, entry 0 = `0000`/`1000`): **0x00 = +y (down screen), 0x40 = +x (right),
0x80 = -y (up), 0xC0 = -x (left)**; per frame delta = table[angle] * speed (16.16, table
full-scale 0x1000) → speed 0x16 = 1.375 px/frame, 0x1C = 1.75, 0x14 = 1.25.

## 0. Spawn / resets

| PC | writes |
|---|---|
| `0x10718` | `+0x00=$8000` live, `+0x02=0x0C` (row 12), `x=0x280`, `y=0x198`, `z=0x140`, `+0x20=$8000` (SM0 intro), `+0x34.w=0` (clears f34 AND f35) |
| `0xFCC6` | (between-fall reset) `x=0x280`, `y=0x198`, `+0x20=$8000`, `+0x34.w=0`, `+0x12=0` |
| `0xFB94` | (ring-out count scene) `x=0x340`, `y=0x160`, `+0x20=$8003` |

Ring floor box used by the probe (`0x2818E`, stage type 0, `+0x32` bit2 clear):
y ∈ [0x118, 0x198], z ≥ 0x140, x ≥ 0x220 and x ≥ (y<<8 + 0x40000)/0x2E0 (left rope,
slanted), x ≤ 0x2E0 and x ≤ −((y<<8 − 0xA3000)/0x2E0) (right rope). Pushback written to
`+0x38` (x) / `+0x3A` (y), `+0x36=1` (or 2 when z ≥ 0x180: airborne, no pushback),
`+0x37` bits: 0 = right rope, 1 = left rope, 2 = bottom rope (y > 0x198), 3 = top rope
(y < 0x118), 4 = under floor (z < 0x140).

## 1. Idle — SM5 `0x1F99E` + visual 0 `0x1FB46`

### SM5 `0x1F99E` (runs every frame while `+0x21==5`; never moves the ref itself)

```
1F99E  btst #0,$1C0161 (rumble)     bne → 1F9B0
1F9A8  jsr 0x204B2                  ; tag/singles: +0x30/+0x1E = action point (below)
1F9B0  (rumble) +0x30=0x268, +0x1E=0x158
1F9BC  jsr 0x2052E                  ; f35 &= 0xE7; if +0x30 < 0x270: f35 |= 0x08; if +0x1E < 0x160: f35 |= 0x10
1F9C2  if +0x56==0 or target->+0xFE==0: bsr 0x20556 (hunt: any slot f35 bit0 → +0x20=$8001, +0x56=slot;
                                         else f35 bit1 && +0x33 bit0 → $8004)
```

`0x204B2` action point: `bsr 0x204FA` → `+0x4C`,`+0x50` = first two live slots with
`+0x33` bit0 (legal men) (scan 9 slots from `$1C05B0`, stride 0x10C); A2=`+0x4C`,
A3=`+0x50`. If A2 `+0x33` bit6 set → `+0x30 = A2.x`, `+0x1E = A2.y` (A2 alone).
Else `+0x30 = min(A2.x,A3.x) + |A2.x−A3.x|/2`, `+0x1E = min(A2.y,A3.y) + |dy|/2`
(= midpoint of the two legal men).

### Visual 0 `0x1FB46` (tag/singles path; rumble path `0x1FC06` = static pose, no motion)

```
1FB52  first entry (bset #7,+0x1C was clear): +0x23=8, +0x25=4, +0x05=0, +0x04=+0x2E, +0x01=1 (mover 1)
1FB78  jsr 0x205AC                  ; heading (below) — writes +0x2D,+0x2B or clears +0x2A
1FB7E  if +0x2B == 0 → rts          ; STOPPED: no motion, no animation (pose frozen)
1FB86  jsr 0x2208                   ; x += cos[+0x2D]*+0x2B, y += sin[+0x2D]*+0x2B
1FB8C  +0x36=0 ; jsr 0x280DC        ; floor/rope probe
1FB96  if +0x36 != 0:
1FB9C     x += +0x38 ; y += +0x3A   ; pushback onto the rope line
1FBAC     f35 &= 0xF9               ; clear rope-memory bits 1,2
1FBB2     if +0x37 bit0 (right rope): f35 |= 0x06
1FBC2     elif +0x37 bit1 (left rope): f35 |= 0x02
1FBD2     elif +0x37 bit3 (top rope):  f35 |= 0x04
          (bottom rope: bits stay cleared)
          NOTE: bits 1,2 are only rewritten on a probe-hit frame → they are a MEMORY of the
          last rope touched, persisting while he walks in open floor.
1FBE0  if --+0x23 != 0 → rts
1FBE6  +0x23=8 ; if --+0x25 == 0: +0x05=0, +0x25=4 ; else +0x05++    ; 4 walk poses, 8 frames each
```

### Heading `0x205AC` (the 0x2060C walk)

```
205AC  D0 = (+0x34.w & 0x1E) >> 1            ; = f35 bits1..4 → idx bits 0..3:
                                             ;   idx&1  = left-rope memory  (f35 bit1)
                                             ;   idx&2  = top-rope memory   (f35 bit2)  (right rope sets both)
                                             ;   idx&4  = action x < 0x270  (f35 bit3)
                                             ;   idx&8  = action y < 0x160  (f35 bit4)
205B8  D1 = +0x30 − x  (unsigned word)
205C0  if D1 < 8:                            ; action x ∈ [x, x+7]  (only when action is at/just right of him)
205C6     D1 = D0 & 0x0A                     ; (top-rope memory, action-upper-half)
205CC     if D1 != 0 && D1 != 0x0A:          ; exactly ONE of the two set
205D6        +0x2A.w = 0 ; rts               ; STAND (speed 0, heading untouched)
205DC  if +0x30 < x:                         ; action is left of him
205E6     D0 |= 0x10                         ; idx bit4
205EA     bclr #7,+0x2E ; nop ; bset #7,+0x2E  ; net: facing = LEFT (patched-out branch)
         (action ≥ x: facing NOT touched)
205F8  +0x2D = byte 0x2060C[D0]
20604  +0x2B = 0x16                          ; speed 1.375 px/f
```

Table `0x2060C` (32 bytes, ROM):

```
2060C: C0 80 40 80 40 80 40 80 40 00 C0 00 40 00 40 00   ; idx 0..15  (action x >= ref x)
2061C: 40 80 C0 80 40 80 C0 80 C0 00 40 00 C0 00 40 00   ; idx 16..31 (action x <  ref x)
```

Decoded (L=left 0xC0, R=right 0x40, U=up 0x80, D=down 0x00), idx = ropeL | ropeT<<1 | actLeftHalf<<2 | actUpperHalf<<3 (+16 if action left of ref):

| idx | mem none | memL | memT | memL+T(right rope) |
|---|---|---|---|---|
| action lower-right half, right of ref (0-3) | L | U | R | U |
| action lower-left half, right of ref (4-7) | R | U | R | U |
| action upper-right, right of ref (8-11) | R | D | L | D |
| action upper-left, right of ref (12-15) | R | D | R | D |
| action lower-right, left of ref (16-19) | R | U | L | U |
| action lower-left, left of ref (20-23) | R | U | L | U |
| action upper-right, left of ref (24-27) | L | D | R | D |
| action upper-left, left of ref (28-31) | L | D | R | D |

Reading: with no rope memory he walks horizontally AWAY from the action's half (x-wise,
toward the far side rope); after touching a side rope (memL / right-rope=L+T) he walks
vertically to the OPPOSITE y-half from the action (up if action is lower, down if upper);
after touching the top rope (memT) he walks horizontally again. The only standing
condition is the deadband at `0x205C0`: ref x within 8 px left of the action x, and
(top-rope memory XOR action-in-upper-half) — i.e. he parks **x-aligned with the midpoint
on the opposite y-half**, above them when they are low, below them when they are high.
There is no minimum-distance rule and no step-away-on-approach rule; separation comes
purely from the opposite-y-half parking and the rope walk. TODO EXACT `0x205C0`: confirm
in MAME that the unsigned `< 8` really is one-sided (he can overshoot leftwards and then
walks per the table until the next rope flips memory).

Facing in idle: rendered `+0x04` is copied from `+0x2E` ONLY at visual-0 entry
(`0x1FB6C`); `0x205EA` sets `+0x2E` bit7 (face left) when action is left, never clears
it. So he does not visibly turn while idle. TODO EXACT: verify on MAME (possible that
`0x27B8`/renderer reads `+0x2E` elsewhere — it reads `+0x04` only).

Walk anim idle: poses `+0x05` = 0,1,2,3 looping, 8 frames each (`0x1FBE0`); frozen when
stopped. (Rumble idle `0x1FC06`: `+0x23=8`, `+0x04=+0x2E`, mover 1, no pose change.)

### Patrol / leave — visual 5 & A `0x1FF52`

First entry: `+0x2B=0x16`, `+0x05=0`, `+0x25=4`, `+0x23=1`; visual A → `+0x2D=0xC0`;
visual 5 → `+0x2D = 0x2001E[(f35 & 6)>>1]` with `0x2001E: C0 80 40` (memL→U, memT→R,
none→L). Per frame: `0x2208` move, probe; on hit: visual A → `+0x2A=0` (stop, stays);
visual 5 → pushback, then `(word(+0x36) & mask) == mask` where the word's low byte is
`+0x37` (rope bits) and mask = `0x2001A[(f35&6)>>1]` = `04 02 08 01` (none→bottom rope,
memL→left rope, memT→top rope, both→right rope) → `+0x20=$8000`. Anim: 4 poses, 6 frames
each (`0x1FFFA`). f35 rope-memory is NOT rewritten in visual 5 (heading fixed at entry).

State plumbing (`0x1FAF0`): `+0x20=$800N` latches both SM `+0x21=N` and visual `+0x1D=N`.
So every in-match `$8005` (pin abort `0x1F9F0`, win `0x203EE`, escort/ring-out fallbacks)
runs SM5 + **visual 5** (fixed-heading walk to the rope named by mask) → `$8000` → SM0
(`$1C1682==0` → falls straight into the SM5 body) + **visual 0** (table walk) until a hunt
fires. Visual 0 is therefore the steady-state idle; visual 5 is the transition out of the
count spot. TODO EXACT `0x1FF82`: with f35 memory = none the heading is 0xC0 (left) but
the exit mask is 0x04 (bottom rope, y > 0x198) — on a purely horizontal walk from
y=pinner.y+1 this only fires if he is already below 0x198; otherwise he pushes into the
left rope until a hunt fires. Needs a MAME trace of the post-abort frames.

## 2. Approach for the count — visual 1/2/4 `0x1FC22`

```
1FC2C  first entry: +0x2E=0 (face right), +0x23=1, +0x25=4, +0x05=0, +0x2B=0x1C (1.75 px/f)
1FC48  A1=+0x56 (pinner) ; +0x60=A1.x ; +0x62=A1.y ; D1=0x50
1FC5C  if SM==4 (ring-out): D1=0x30, +0x62 -= 0x10;
          if A1 in state 5: move 0x22 → +0x62 -= 0x10 more; move 0x09 → D1=0x50
1FC92  if A1.x > 0x280: +0x60 -= D1  else +0x60 += D1      ; stand on the ring-CENTER side of the pinner
1FCA6  facing: +0x60 < x → +0x2E bit7 set (left) else clear ; +0x04=+0x2E
1FCC4  every frame: $1C15E4=A0, $1C15E8=+0x60, $1C15EA=+0x62 ; jsr 0x20C8 (angle to dest) → +0x2D
1FCE8  jsr 0x2208 (move) ; bsr 0x2062C (arrive test: probe hit counts as arrived after pushback;
       else |x−+0x60| < 6 && |y−+0x62| < 6)
1FCF4  on arrive: y = A1.y + 1 (draw just in front of the pair); visual 1 → +0x24=0, +0x20=$8006;
       visual 2 → $8007; visual 4 → $8008
1FD38  anim: 4 poses, 8 frames each
```
Count position = (pinner.x ∓ 0x50 toward x=0x280, pinner.y + 1). No offset table beyond
the constants above.

## 3. Sprite cells

Pose index `+0x05`: idle/approach walk 0..3; count 4/5/6; usher 7/8; 20-count 9..0xC;
ring-out 0xD/0xE; win 0xF/0x10/0x11. `+0x04` high byte = facing (bit7 left), `+0x04.w ==
0x7FFF` = hidden. Cell graphics resolved by the renderer from table id `+0x02 = 0xC` (row
12) — cell addresses per pose: TODO EXACT (sprite stream row 12 decode in `0x2AEA`/
`0x2836`; not part of this walk).

## 4. Hits / ropes

No code path damages the referee: every wrestler attack/collision scan walks the 9 slots
at `$1C05B0` (e.g. hunt-style loops `0x204FA`, `0x20556`), and `$1C11F4` is referenced
only by: sprite/setup `0x5A00..0x5CAA`, `0x5FCC/0x601E` (slot bookkeeping), `0xA702`
(render), `0xFB94/0xFCC6/0x10718` (resets), `0x1F914` (its own routine). He is never
knocked down and has no reaction state. Rope interaction is only the probe pushback above
(`0x1FB9C`, `0x1FFBC`, `0x2063C`); wrestlers do not push him.

## 5. C sketch of the idle walk (visual 0 + SM5)

```c
/* angle: 0x00=+y 0x40=+x 0x80=-y 0xC0=-x ; sin/cos tables 0x2378/0x23FA, scale 0x1000 */
static const uint8_t HEAD[32] = { /* ROM 0x2060C */
 0xC0,0x80,0x40,0x80,0x40,0x80,0x40,0x80,0x40,0x00,0xC0,0x00,0x40,0x00,0x40,0x00,
 0x40,0x80,0xC0,0x80,0x40,0x80,0xC0,0x80,0xC0,0x00,0x40,0x00,0xC0,0x00,0x40,0x00 };

static void ref_sm5(eng_state *st, eng_ref *r)             /* 0x1F99E */
{
    eng_obj *a = legal[0], *b = legal[1];                 /* 0x204FA: first two live slots with +0x33 bit0 */
    if (a->f33 & 0x40) { r->tx = a->x>>16; r->ty = a->y>>16; }
    else { r->tx = min(ax,bx) + abs(ax-bx)/2; r->ty = min(ay,by) + abs(ay-by)/2; }   /* 0x204B2 */
    r->f35 &= 0xE7;                                        /* 0x2052E */
    if (r->tx < 0x270) r->f35 |= 0x08;
    if (r->ty < 0x160) r->f35 |= 0x10;
    if (!r->target || target->fFE == 0) hunt(st);          /* 0x20556 */
}

static void ref_vis0(eng_state *st, eng_ref *r)            /* 0x1FB46 */
{
    if (!r->latched) { r->latched=1; r->t23=8; r->c25=4; r->pose=0; r->face4=r->facing; r->mover=1; }
    /* 0x205AC heading */
    unsigned idx = (r->f35 & 0x1E) >> 1;
    uint16_t d = (uint16_t)(r->tx - (r->x>>16));
    if (d < 8) { unsigned m = idx & 0x0A; if (m && m != 0x0A) { r->speed = 0; goto moved; } }
    if (r->tx < (r->x>>16)) { idx |= 0x10; r->facing |= 0x80; }
    r->angle = HEAD[idx]; r->speed = 0x16;
    /* 0x1FB7E */
    if (r->speed == 0) return;                             /* stand, pose frozen */
    r->x += cos16[r->angle] * r->speed; r->y += sin16[r->angle] * r->speed;   /* 0x2208 */
    probe(r);                                              /* 0x280DC */
    if (r->hit36) {
        r->x += r->push38<<16; r->y += r->push3A<<16;
        r->f35 &= 0xF9;
        if (r->ropes37 & 0x01) r->f35 |= 0x06;             /* right rope */
        else if (r->ropes37 & 0x02) r->f35 |= 0x02;        /* left rope */
        else if (r->ropes37 & 0x08) r->f35 |= 0x04;        /* top rope */
    }
    if (--r->t23) return;
    r->t23 = 8; if (--r->c25 == 0) { r->pose = 0; r->c25 = 4; } else r->pose++;
moved:;
}
```
Note for the engine: the current `referee.c` parks at a far-lane quarter with a y-drift to
0x11C; stock instead (a) computes the legal-men midpoint each frame, (b) walks horizontally
away to the far side rope, then vertically to the opposite y-half, (c) parks only when
x-aligned (within 8 px) with the midpoint on the opposite y-half, and (d) in-match idle
actually alternates visual 5 (`$8005`, fixed heading to a rope) → `$8000` visual 0 (table
walk). Y range is clamped by the probe to 0x118..0x198 so "above" = y≈0x118 (top rope
line), "below" = y≈0x198 — never between the wrestlers in screen-y unless they straddle.
