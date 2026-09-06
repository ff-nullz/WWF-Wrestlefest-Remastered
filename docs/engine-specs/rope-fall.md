# Rope-side knockdown: exact transcription (fall 0x1B134 -> bounce 0x1B1D0 -> lying 0x1B318)

Source: `../wrestlefest-decomp/reference/maincpu.asm` (read-only pass, 2026-08-22),
cross-checked against the verified C in `../wrestlefest-decomp/src/floor_2818e.c`
and `docs/engine-specs/reactions.md` / `apply-motion.md`. All PCs are ROM addresses.
Field conventions below use the engine names from `engine/engine.h`:
`x/y/z` 16.16 longs at +0x06/+0x0A/+0x0E (int word first), `vx/vy/vz` 8.8 at
+0x58/+0x5A/+0x5C, `grav` +0x5E, `mover` +0x01, `facing` +0x2E bit15,
`zone` +0x36, `clip` +0x37, pushbacks +0x38/+0x3A/+0x3C, probe hints
+0x3E (X look-ahead) / +0x40 (`clip_h`, Y bias) / +0x42 (`floor42`, Z bias).

---

## 0. Frame-loop order (why "previous frame" matters)

Per object, per frame, the object pass `0xF4C2` runs `0xF518`:

```
0F518: clr.w  (+0x36,A0)            ; zone+clip cleared FIRST
0F51C: tst.b  (+0x01,A0); beq skip  ; probe only while a mover is active
0F522: jsr    0x280DC               ; bounds/floor probe (uses +0x3E/+0x40/+0x42)
0F528: tst.b  (+0x37,A0); beq skip
0F52E: x_int += (+0x38)             ; move.w ($38,A0),D1 / add.w D1,($6,A0)
0F536: y_int += (+0x3A)
0F53E: z_int += (+0x3C)
0F546: jsr    0x1C03E               ; STATE HANDLER (0x1B134 / 0x1B1D0 / 0x1B318 ...)
0F54C: jsr    0x247C  ; world->screen
0F552: jsr    0x27B8  ; sprite build
0F558: jsr    0x2208  ; MOTION: mover 2 adds vel<<8 to the 16.16 positions
```

So each frame: **probe -> apply pushback -> run handler -> apply velocities.**
The probe sees the position left by the *previous* frame's `0x2208` motion, and it
consumes the hint fields (+0x3E/+0x40/+0x42) the handler left set *last* frame.
Corrections therefore lag motion by exactly one frame; the handler reads
fresh `clip` bits (bit4 = landed) the same frame they are produced.

Motion `0x2208` mover 2 (`0x2252`): `x += vx<<8` (0x2266), `y += vy<<8` (0x227C),
`z += vz<<8` (0x2294), then gravity `vz -= grav` (0x22A6) with a provably-dead
clamp at 0x22B0-0x22B8. Axis law: **larger z = higher; mat plane z=0x140;
positive vz = rising.**

---

## 1. `0x10F9C` — edge-of-ring correction (fall init, D0=0x40)

Full body, 0x10F9C-0x10FC4:

```
10F9C: move.w ($a,A0),D1        ; D1 = y int
10FA0: addi.w #$20,D1
10FA4: cmpi.w #$198,D1
10FA8: bcs    $10fb2            ; unsigned: skip if y+0x20 < 0x198
10FAA: neg.w  D0
10FAC: move.w D0,($5a,A0)       ; vy = -D0
10FB0: bra    $10fc4
10FB2: move.w ($a,A0),D1
10FB6: subi.w #$20,D1
10FBA: cmpi.w #$118,D1
10FBE: bcc    $10fc4            ; unsigned: skip if y-0x20 >= 0x118
10FC0: move.w D0,($5a,A0)       ; vy = +D0
10FC4: rts
```

- **The only field it writes is `+0x5A` (vy).** Nothing stronger — no position
  write, no clamp, no clip fields. Conditions exactly as the earlier pass had
  them: `y+0x20 >= 0x198` -> `vy = -D0`; else `y-0x20 < 0x118` -> `vy = +D0`;
  else untouched. Comparisons are **unsigned** (bcs/bcc) — irrelevant for
  in-band y but that is the letter of it.
- Units: vy is 8.8 px/frame, applied by mover 2 (`0x227C: y += vy<<8`). With
  D0=0x40 that is **0.25 px/frame drift toward the ring center**, persisting
  as long as the mover runs (nothing else writes +0x5A during the fall;
  `0x258E` at fall init clears it *before* 0x10F9C runs — order matters).
- Trigger bands: fires for y >= 0x178 (bottom) or y < 0x138 (top). Between
  0x138..0x177 vy stays 0.

**MAME datapoint reconciled:** fall from y=0x193: 0x193+0x20=0x1B3 >= 0x198 ->
vy = 0xFFC0 written once at init and never touched again — hence "vy=0xFFC0
the whole flight". Rest at 0x18B = 8 px inward = ~32 frames of mover-2 motion
at 0.25 px/f between fall init and lying init (lying sets mover=0). The drift
stops accruing on y only in the sense that bounce init re-arms the launcher —
see below: **`0x258E` clears vy (0x25A6)**, so the bounce phase has vy=0 and
the 8 px is accrued during the fall arc alone.

## 2. `0x10B62` — the "clipped facing-relative slide" (bounce D0=0x50, cover D0=0)

Full body, 0x10B62-0x10B98:

```
10B62: movem.l D0-D1/A0-A1,-(A7)
10B66: move.w D0,($3e,A0)       ; +0x3E = D0 (X look-ahead hint)
10B6A: move.w ($36,A0),D0       ; D0 = OLD zone:clip word (pre-probe)
10B6E: jsr    $280dc            ; run the bounds probe NOW, in-line
10B74: move.w ($36,A0),D1       ; NEW zone:clip word
10B78: andi.w #$3,D1            ; isolate +0x37 bits 0-1 (X trapezoid edges)
10B7C: tst.w  D1
10B7E: beq    $10b84
10B80: clr.w  ($58,A0)          ; probe point crossed an X edge -> vx = 0
10B84: move.w D0,($36,A0)       ; RESTORE the pre-probe zone:clip word
10B88: move.w ($38,A0),D0
10B8C: add.w  D0,($6,A0)        ; x_int += X pushback  (THE ONLY POSITION WRITE)
10B90: clr.w  ($3e,A0)          ; clear the hint
10B94: movem.l (A7)+,D0-D1/A0-A1
10B98: rts
```

Exact semantics:

- **Yes, it runs `0x280DC`** — a second, in-handler probe of the point
  `(x + face_sign*D0, y + clip_h, z + floor42)` (`0x280DC` negates +0x3E in
  place at 0x28112 when facing bit7 is set — direction convention: **+0x3E
  probes ahead of facing**; positive D0 = in front of the wrestler).
- **It applies ONLY the X pushback `+0x38`** (0x10B8C). The Y (+0x3A) and Z
  (+0x3C) pushbacks the inner probe computed are *discarded* — they sit in the
  fields but the next frame's `0x280DC` clears all three on entry
  (0x280E0-0x280E8) before recomputing, so they never land.
- **"Ring-clipped" means:** in the in-ring floor handler `0x2818E`, `+0x38` is
  only written when the probe X is outside the perspective trapezoid at row
  `y+clip_h` **and** the probe z (`z+floor42`) is **< 0x180**
  (floor_2818e.c:287-312 / 341-363). At z >= 0x180 the crossing only sets
  zone=2 and *no pushback* (high flight may cross the ropes). Trapezoid law:
  `xmin(y) = ((y<<8)+0x40000)/0x2E0` guarded by `x < 0x220`;
  `xmax(y) = -(((y<<8)-0xA3000)/0x2E0)` guarded by `x > 0x2E0`.
  The pushback is `boundary - probe_x`, i.e. the object moves so the
  look-ahead point sits exactly on the boundary — always inward.
- **`+0x36` (zone) IS preserved:** the pre-probe zone:clip word is saved in D0
  (0x10B6A) and restored after (0x10B84), so callers and the same-frame logic
  keep seeing the *frame-start* probe's zone/clip, not the temp probe's.
- **vx zeroing (0x10B80)** keys on the temp probe's X-edge *flags* (+0x37 bits
  0/1), which `0x2818E` sets even in the z>=0x180 no-pushback case — so vx is
  killed whenever the look-ahead point is outside the trapezoid at any height,
  while the position moves only when z < 0x180.

**Why the MAME bounce showed NO x displacement:** two independent reasons.
(a) The bounce launcher `0x258E(3)` sets vx=0 (table id 3 = {0, 0x300, 0x38}).
(b) `0x10B62(0x50)`'s slide is zero unless the point 0x50 px *in front of
facing* pokes outside the X trapezoid — mid-ring that never happens. It moves
the body **only** when the wrestler lands within 0x50 px of the left/right
trapezoid edge *facing it* (post-landing z=0x140 < 0x180, so the pushback is
live), and then only in X. **It never touches Y** — the 0x118/0x198 rope
lines are entirely out of 0x10B62's reach.

## 3. How `0x280DC` / `0x2818E` consume `+0x3E` / `+0x40` / `+0x42`

`0x280DC` (0x280DC-0x28122), the dispatcher:

```
280E0: clr.w (+0x38); clr.w (+0x3A); clr.w (+0x3C)   ; pushbacks zeroed every probe
280EC: btst #5,(+0x32); bne rts                      ; bit5 = probe-exempt object
280F6: D0 = x_int
280FA: D1 = y_int
280FE: D2 = z_int
28102: D1 += (+0x40)          ; clip_h biases the Y test point
28106: D2 += (+0x42)          ; floor42 biases the Z test point
2810A: if facing bit7: neg.w (+0x3E)   ; IN PLACE (0x28112)
28116: D0 += (+0x3E)          ; X look-ahead toward facing
2811A: bsr 0x28124            ; scene dispatch: table 0x28154, index
                              ;  ($1C007E&7)<<3 + (+0x32&4); scene 0 slot -> 0x2818E
```

Note the in-place negate: a facing-left object flips the sign of +0x3E every
probe. Harmless in stock only because every user either rewrites it each frame
(fall, 0x1B182) or zeroes it immediately after one probe (0x10B62, lying init).

`0x2818E` (in-ring; verified line-exact in `src/floor_2818e.c`, run_2818e):

- Gate: `+0x32` bit0 must be **clear** or the whole handler is skipped
  (floor_2818e.c:183).
- **Y low** (asm 0x2819C-0x281B8): if `y+0x40h < 0x118`: clip bit3,
  `+0x3A = 0x118 - (y+0x40h)`, zone=1. Applied next at 0xF536 this snaps
  **y to 0x118 - clip_h**.
- **Y high** (0x281BA-0x281D6): if `y+0x40h > 0x198`: clip bit2,
  `+0x3A = 0x198 - (y+0x40h)` (negative), zone=1 -> snaps y to
  `0x198 - clip_h`. **No z gate — Y containment applies at any height.**
  Geometric effect of `+0x40`: it shifts the whole legal Y window to
  `[0x118 - clip_h, 0x198 - clip_h]`.
- **Z floor** (0x281D8-0x281F2, floor_2818e.c:239-261): if `z+0x42h < 0x140`:
  clip bit4 ("landed"), `+0x3C = (0x140 - (z+0x42h)) + 0x42h = 0x140 - z`.
  Geometric effect of `+0x42`: a **negative** value (throw handlers use -0x10/
  -0x18/-0x20, e.g. 0x1B5C6/0x13FEC/0x15476) makes contact trigger while the
  body is still `|0x42h|` px above the mat, **but the snap target is always
  exactly z=0x140** — the bias raises the trigger, not the rest height.
  vz is *not* zeroed by the probe; the state transition re-arms it.
- **X trapezoid** (0x281F6-0x28286): as in §2 — outer guards 0x220/0x2E0,
  `divs.w #$2E0` slopes, clip bit1 (xmin) / bit0 (xmax) always set on a
  crossing; pushback `+0x38` and zone=1 only when `z_probe < 0x180`, else
  zone=2 and no pushback. Note the X row uses the **biased** y (D1 already has
  +0x40 added) and the **biased** z, so `clip_h`/`floor42` bend the X clip too.

Hint values by state in this sequence (all verified):

| state | +0x3E | +0x40 | +0x42 | set at |
|---|---|---|---|---|
| fall 0x1B134 flight | 0x40 (rewritten every frame) | 0 | whatever the throw left (0 for plain punch knockdowns; throws use -0x10/-0x18/-0x20 then clear) | 0x1B182 |
| fall on landing | 0 | 0 | — | 0x1B190 |
| bounce 0x1B1D0 | 0 (0x10B62 cleans up) | 0 | unchanged | 0x10B90 |
| lying id 8 init | 0x50 for one 0x10B62 probe | 0 | unchanged | 0x1B356 |
| lying id 9 init | 0x30 | **-0x18** | unchanged | 0x1B362/0x1B368, cleared 0x1B384/0x1B388 |

Lying id 9's `+0x40 = -0x18` shifts the Y window to **[0x130, 0x1B0]** for its
one manual probe: an anchor within 0x18 of the top rope line is pushed down to
0x130; the bottom limit relaxes to 0x1B0 (unreachable, since flight clamped the
anchor to <= 0x198). The face-down sprite hangs up-screen from its anchor, so
the shifted window is what keeps *that* body art off the top rope.

## 4. The complete stock sequence, write by write

### Phase A — fall (state 4, react 2/3/4, cell 0x1B114 -> handler 0x1B134)

Init frame (+0x1C bit7 clear):

| PC | write |
|---|---|
| 0x1B13C | `$1C1697 = 1` (global event byte) |
| 0x1B146-0x1B14E | `jsr 0x258E(D0 = react_id - 2)`: 0x25A2 `vx = tab[id].w0`, 0x25A6 **`vy = 0`**, 0x25AA `vz = tab[id].w1`, 0x25AE `grav = tab[id].w2`, 0x25B2 **`mover = 2`**, 0x25C0 `vx = -vx` if facing bit7. Table 0x25CA+6*id: id0 {0xC0,0x300,0x38}, id1 {0x180,0x300,0x38}, id2 {0x200,0x300,0x38} |
| 0x1B154-0x1B158 | `jsr 0x10F9C(D0=0x40)` -> `vy = ±0x40 / unchanged` (§1) |
| 0xF558->0x2266/0x227C/0x2294/0x22A6 | same frame, after the handler: x/y/z += vel<<8; vz -= grav |

Every flight frame (bit7 set):

| PC | write |
|---|---|
| 0xF518-0xF544 | clear zone/clip; probe (hints +0x3E=0x40, +0x40=0, +0x42 as-is); **x/y/z += pushbacks** — this is the flight-time Y clamp to [0x118,0x198] and the eventual z snap to 0x140 |
| 0x1B160 | `jsr 0x10FC6`: if `$1C0161` bit1 clear and zone==1 (or 5, cage): clip bit1 -> `$1C1150=$1C1180=1` (0x10FF0), clip bit0 -> `$1C11B0=$1C11E0=1` (0x11012) — the rope-shake arm; if zone==3 (out-of-ring floor, rumble handler 0x28288): `state=4` (0x1102C), `+0x64=0x17` (0x11032), sounds 0x28+0x32 |
| 0x1B172-0x1B17C | **one-shot** on the tick both `+0x24==0` and `+0x22==0` (frame-0 art expiry): `jsr 0x10BD0(-0x10, 0, 0x10)` -> 0x10BDA `x_int -= 0x10` facing-relative, 0x10BE2 `z_int += 0x10` |
| 0x1B182 | `+0x3E = 0x40` (re-arm the look-ahead for next frame's probe) |
| 0x1B188 landed (clip bit4) | 0x1B190 `+0x3E = 0`; 0x1B194 `state = 4`; 0x1B19A `react_id = 5`; 0x1B1A0 `jsr 0x1110E` (sound 0x29, 0x2D if +0x33 bit2) |
| 0x1B1A8-0x1B1B0 | not landed and +0x64 bit15: cell = 0x1B124 (critical art) |
| 0x2208 | velocities still applied on the landing frame (vz now negative) — z dips below 0x140 once more; next frame's probe re-snaps it before the bounce handler runs |

### Phase B — bounce (react 5, cell 0x1B1B8 -> handler 0x1B1D0)

Init:

| PC | write |
|---|---|
| 0x1B1D8 | `bclr #4, +0x33` |
| 0x1B1DE-0x1B1E2 | `jsr 0x10B62(D0=0x50)` — §2: temp probe at x±0x50; `vx=0` if it crossed an X edge (0x10B80); `x_int += +0x38` (0x10B8C, zero mid-ring); zone/clip restored |
| 0x1B1E8-0x1B1EC | `jsr 0x258E(3)`: `vx=0, vy=0, vz=0x300, grav=0x38, mover=2` — the straight-up pop |
| 0x1B1F2 | `+0xAA = 0`; 0x1B1F6-0x1B1FA `jsr 0x10C60(1)`: seed mash count from hp quarter (table 0x10D00 = 01 03 07 15, code 0x10CBA-0x10CDE) |
| 0x1B200-0x1B210 | if react != 0x2A and `+0x71 == 2`: `bset #7, +0x64` (face-down) |
| 0x1B258-0x1B260 | bit15 +0x64: cell = 0x1B1C4 (critical art) |

Per frame: 0xF518 probe/pushback (hints now 0/0/+0x42); 0x1B218 `jsr 0x10D04`
(referee "body down" bset #7 $1C167A + mash decrement); on clip bit4
(0x1B21E): 0x1B226 sound; 0x1B22C `state = 4`; react -> **8** (0x1B242, bit15
clear) / **9** (0x1B24A, bit15 set) / 8 for 0x2A (0x1B252).

### Phase C — lying (react 8/9, cells 0x1B300/0x1B30C -> handler 0x1B318)

Init:

| PC | write |
|---|---|
| 0x1B320 | **`mover (+0x01) = 0`** — motion *and* the per-frame probe stop; rest position is final after this init |
| 0x1B324-0x1B346 | `bclr #4/#6/#3 +0x33`, `bclr #7 +0x60`, `clr +0x52/+0x54/+0xD0` |
| 0x1B348 | `jsr 0x10F56`: if `+0x9A == 0` (0x10F56) seed forced-down time from `+0x71`: 2 -> 0xE0 (0x10F7C), 1 -> 0x80 (0x10F8C), else 0x30 (0x10F94); gates 0x10F5C-0x10F72 force the 0x30 path |
| id 9 (0x1B34E beq): 0x1B362 | `+0x3E = 0x30` |
| 0x1B368 | `+0x40 = -0x18` |
| 0x1B36E | `jsr 0x280DC` (manual probe: point (x±0x30, y-0x18, z+0x42)) |
| 0x1B374-0x1B378 | `x_int += +0x38` |
| 0x1B37C-0x1B380 | **`y_int += +0x3A`** — the only place in the chain that applies a Y pushback by hand; snaps the anchor into [0x130, 0x1B0] |
| 0x1B384-0x1B388 | `+0x3E = 0`, `+0x40 = 0` |
| id 8: 0x1B356-0x1B35A | `jsr 0x10B62(0x50)` — X-only slide, Y untouched |

Per frame (0x1B390+): no probe, no motion; timers/pin checks only; RISE at
0x1B3C4: `state = 7`, `bclr #7, +0x64`.

### Where the per-frame pushback matters for the rest position

1. **Flight Y clamp.** The fall is the only phase with vy and an active mover;
   `0xF536` applies `+0x3A` every flight frame, so the anchor can never end a
   frame outside [0x118, 0x198] no matter what the throw arc did. Combined
   with 0x10F9C's vy the anchor lands strictly *inside* the band.
2. **Landing snap.** `0xF53E` puts z exactly at 0x140 on the landing frame
   (biased trigger via +0x42, unbiased target), and again one frame later
   after the transition frame's leftover vz dip.
3. **After lying init the probe is off** (mover=0 fails the 0xF51C gate), so
   the lying-init writes are literally the last position writes — replaying
   them exactly reproduces the rest position bit for bit.

### Pressed INTO a rope line (y clamped at 0x118/0x198 during flight)

If the throw releases the victim beyond the band (or the arc carries it out),
the next frame's probe flags clip bit2/bit3 and `0xF536` snaps y to the line
**in one frame** (pushback = full overshoot, not gradual). From then on the
only y motion is vy: 0x10F9C fired at fall init (the release point is well
inside the 0x178/0x138 trigger bands), so the body pinned against the line
**glides 0.25 px/frame toward the ring center for the rest of the flight** —
that is the visible "slides away from the ropes while falling". The rope
shake the player sees is `0x10FC6` arming `$1C1150/$1C11B0` on the X-edge
clip bits. At touchdown the anchor is ~(flight_frames_remaining)/4 px inside
the line; the bounce adds nothing in y (vy cleared by 0x258E(3), 0x10B62 is
X-only); lying id 9 finally shifts a top-rope body down to >= 0x130. A
face-up (id 8) body at the bottom line keeps whatever the drift bought — in
the capture, 0x18B, 13 px inside — and its art hangs down-screen from the
anchor, inside the ring.

## 5. C sketches (engine style, `eng_obj` fields)

```c
/* 0x10F9C — engine/anim.c edge_arc() is already exact (unsigned vs signed
 * compare is moot for ring-band y). Kept for reference: */
static void edge_arc(eng_obj *o, int16_t d0)             /* 0x10F9C */
{
    int16_t yi = (int16_t)(o->y >> 16);
    if ((uint16_t)(yi + 0x20) >= 0x198u)       o->vy = (int16_t)-d0; /* 0x10FAC */
    else if ((uint16_t)(yi - 0x20) < 0x118u)   o->vy = d0;           /* 0x10FC0 */
}

/* 0x2818E trapezoid rows (divs.w #$2E0; C division truncates toward zero
 * exactly like divs) */
static int16_t trap_xmin(int16_t y)      /* valid when x < 0x220 */
{ return (int16_t)((((int32_t)y << 8) + 0x40000) / 0x2E0); }
static int16_t trap_xmax(int16_t y)      /* valid when x > 0x2E0 */
{ return (int16_t)(-((((int32_t)y << 8) - 0xA3000) / 0x2E0)); }

/* 0x10B62 — look-ahead X containment ("clipped facing-relative slide").
 * Probes (x + face*d0, y + clip_h, z + floor42) through the scene floor law,
 * applies ONLY the X pushback, kills vx on any X-edge crossing, and leaves
 * zone/clip exactly as the frame-start bounds pass wrote them. */
static void slide_clip(eng_obj *o, int16_t d0)           /* 0x10B62 */
{
    int16_t px = (int16_t)(o->x >> 16)
               + ((o->facing & 0x8000u) ? (int16_t)-d0 : d0); /* 0x28112/0x28116 */
    int16_t py = (int16_t)(o->y >> 16) + o->clip_h;           /* 0x28102 */
    int16_t pz = (int16_t)(o->z >> 16) + o->floor42;          /* 0x28106 */
    int16_t push = 0;
    int     edge = 0;

    if (px < 0x220 && px < trap_xmin(py)) {          /* 0x281F6/0x2820E */
        edge = 1;                                    /* +0x37 bit1, always */
        if (pz < 0x180) push = (int16_t)(trap_xmin(py) - px); /* 0x28224 */
    } else if (px > 0x2E0 && px > trap_xmax(py)) {   /* 0x2823C/0x28256 */
        edge = 1;                                    /* +0x37 bit0 */
        if (pz < 0x180) push = (int16_t)(trap_xmax(py) - px); /* 0x2826A */
    }
    if (edge) o->vx = 0;                             /* 0x10B80: on the FLAG */
    o->x += (int32_t)push << 16;                     /* 0x10B8C: X only */
    /* zone/clip untouched == the ROM's save/restore (0x10B6A/0x10B84) */
}

/* eng_ring_bounds() corrections (motion.c) — the 0x2818E law with the hints:
 *   Y:  window is [0x118 - clip_h, 0x198 - clip_h]; applies at ANY height.
 *   Z:  landed when (z + floor42) < 0x140; snap to 0x140 EXACTLY (0x281DE:
 *       push = 0x140 - z, independent of floor42). Do NOT zero vz here —
 *       stock leaves it and lets the next state re-arm (one visible frame
 *       of overshoot re-snapped next frame).
 *   X:  crossing sets the clip bit at any height; the POSITION pushback and
 *       zone=1 only when (z + floor42) < 0x180, else zone=2 and no clamp
 *       (bodies may fly over the ropes). */

/* 0x1B134 fall */
static uint32_t handler_fall(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {                  /* init, 0x1B13C */
        o->clip_h = 0;                               /* +0x40 is 0 in flight */
        knockback(o, (o->react_id & 0xFFu) - 2u);    /* 0x258E: vy=0, mover=2 */
        edge_arc(o, 0x40);                           /* 0x10F9C AFTER 0x258E */
    } else {
        /* 0x10FC6: rope shake on this frame's X-edge clip bits */
        if (o->zone == 1 && (o->clip & 0x03u))
            eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);
        if (o->frame == 0 && o->count == 0)          /* one-shot, 0x1B16C */
            add_pos_delta(o, -0x10, 0, 0x10);        /* 0x10BD0 */
        o->lookahead = 0x40;                         /* +0x3E for next probe */
        if (o->landed) {                             /* +0x37 bit4 */
            o->lookahead = 0;                        /* 0x1B190 */
            eng_sound((o->f33 & 0x04u) ? 0x2D : 0x29); /* 0x1110E */
            o->state = 4; o->react_id = 5;           /* 0x1B194/0x1B19A */
        }
    }
    return cell;   /* +0x64 bit15 -> cell 0x1B124 (critical art) */
}

/* 0x1B1D0 bounce */
static uint32_t handler_bounce(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {                  /* init, 0x1B1D8 */
        o->clip_h = 0;
        slide_clip(o, 0x50);                         /* 0x10B62(0x50) */
        knockback(o, 3);                             /* vx=0 vz=0x300 g=0x38 */
        o->mash_aa = mash_seed(o);                   /* 0x10C60(1) */
        if ((o->react_id & 0xFFu) != 0x2Au && o->band == 2)
            o->react_id |= 0x8000u;                  /* 0x1B210 face-down */
    } else if (o->landed) {                          /* 0x1B21E */
        eng_sound(0x29);                             /* 0x1110E */
        o->state = 4;                                /* 0x1B22C */
        o->react_id = ((o->react_id & 0xFFu) == 0x2Au) ? 8
                    : (o->react_id & 0x8000u) ? 9 : 8;  /* 0x1B242-0x1B252 */
    }
    return cell;
}

/* 0x1B318 lying */
static uint32_t handler_lying(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {                  /* init, 0x1B320 */
        o->mover = 0;                                /* motion + probe OFF */
        if (o->down_t == 0)                          /* 0x10F56 gate */
            o->down_t = (o->band == 2) ? 0xE0 : (o->band == 1) ? 0x80 : 0x30;
        if ((o->react_id & 0xFFu) == 9) {            /* 0x1B34E */
            /* one manual probe, X AND Y applied: 0x1B362-0x1B388 */
            o->clip_h = -0x18;
            slide_clip(o, 0x30);                     /* X part (+0x38) */
            {   int16_t yi = (int16_t)(o->y >> 16) - 0x18;  /* +0x40 bias */
                if (yi < 0x118)      o->y += (int32_t)(0x118 - yi) << 16;
                else if (yi > 0x198) o->y += (int32_t)(0x198 - yi) << 16;
            }                                        /* window [0x130,0x1B0] */
            o->clip_h = 0;                           /* 0x1B388 */
        } else {
            slide_clip(o, 0x50);                     /* id 8: 0x10B62(0x50) */
        }
    } else { /* timers / forced rise as currently implemented (0x1B390+) */ }
    return cell;
}
```

## 6. Engine deltas found while transcribing (why it still differs)

All in `../wrestlefest-decomp/engine/`:

1. **`anim.c` sets `clip_h = 0x18` (positive, every frame) in fall/bounce/
   lying.** Stock has +0x40 = **0** through fall and bounce, and **-0x18**
   for exactly one probe in lying-id-9 init. A standing +0x18 bias shrinks
   the window to [0x100, 0x180] — the body is *allowed 0x18 px past the
   bottom line* and clamped 0x18 early at the top: precisely "lands with the
   body across the bottom ropes". (`motion.c` currently `(void)`s clip_h, so
   today the bias is inert — but the plumbing plan is inverted vs stock.)
2. **`motion.c` floor uses `z < 0x140 + floor42`, snapping to
   `0x140 + floor42`.** Stock: trigger `z + floor42 < 0x140` (i.e. -0x20
   biases the trigger *up*), snap **always to z = 0x140** (pushback
   `0x140 - z`, 0x281DE-0x281F2). Engine's sign is flipped and the rest
   height is wrong whenever floor42 != 0.
3. **`motion.c` clamps X at any height.** Stock suppresses the X pushback at
   `z_probe >= 0x180` (zone=2) — high throws must cross the ropes.
4. **`motion.c` zeroes vz on landing.** Stock does not; the dip-and-resnap
   frame is part of the look.
5. **`anim.c` bounce lands to react 8 unconditionally** — stock picks 9 when
   +0x64 bit15 (set at bounce init for band 2), which is the only route into
   the lying-id-9 Y snap.
6. **Missing pieces now specified exactly:** `slide_clip` (0x10B62) for bounce
   and lying-8, the lying-9 init probe, the fall's one-shot -0x10/+0x10 hop
   (0x1B172), and the per-frame `+0x3E = 0x40` look-ahead that keeps a low
   fall inside the trapezoid.
7. `anim.c handler_lying` down-time uses hp thresholds; stock `0x10F56` keys
   on the energy **band** `+0x71` (2 -> 0xE0, 1 -> 0x80, else 0x30) and skips
   seeding when `+0x9A` was pre-set by a move.
