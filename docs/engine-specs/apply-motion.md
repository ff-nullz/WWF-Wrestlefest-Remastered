# apply_motion (ROM 0x2208) — C transcription spec

Source: `../wrestlefest-decomp/reference/maincpu.asm` (all PCs below are ROM
addresses in that listing). Object layout cross-checked against
`../wrestlefest-decomp/docs/memory-catalog.csv` rows 62-66 and
`docs/native-rewrite-inventory/gameplay-tables.md` §2.

Object pointer is always in **A0**. Object stride 0x10C; slot pointer table of 11
longwords at ROM **0x5122**: `1C05B0 1C06BC 1C07C8 1C08D4 1C09E0 1C0AEC 1C0BF8
1C0D04 1C0E10 1C0F1C 1C1028` (last two are aux objects, see §3).

---

## 1. 0x2208 walk-through — it is a 3-way mode dispatcher

```
002208  movem.l D0-D3/A1,-(A7)          ; saves D0-D3,A1 (mask F040); A0 untouched
00220C  moveq   #0,D0
00220E  move.b  (0x01,A0),D0            ; motion MODE byte at +0x01
002212  asl.w   #1,D0
002214  lea     0x2224,A1
00221A  jsr     (A1,D0.w)               ; bra.s table, 2 bytes/entry
00221E  movem.l (A7)+,D0-D3/A1
002222  rts
002224  bra 0x222A   ; mode 0 -> rts (no motion)
002226  bra 0x222C   ; mode 1 -> polar (angle+speed)
002228  bra 0x2252   ; mode 2 -> per-axis velocity + gravity
00222A  rts
```

**The ONLY gate inside 0x2208 itself is the mode byte at +0x01** (`object_move_mode`,
catalog row 62: 0 idle / 1 polar / 2 velocity; P1 walking uses mode 1).
There is **no** test of +0x20/+0x32/+0x33 inside 0x2208 — those bits gate it from the
*caller* side (see §3). Mode 3 would land on the rts at 0x222A (no-op); modes >= 4 are
out of table (mode 4 would alias mode 1, mode 5 jumps mid-instruction) and never occur.

### Position representation (critical)

x, y, z are each a **32-bit 16.16 fixed-point long**:
integer pixels in the *high* word at +0x06 / +0x0A / +0x0E, fraction in the *low* word
at **+0x08 / +0x0C / +0x10** (catalog row 65, confirmed). Both movers use `add.l` onto
these longs. (Other engine code, e.g. the push-back in 0xF518 below, does `add.w`
directly on the integer word and leaves the fraction alone.)

### Mode 1 — polar (0x222C), the walk mover

```
00222C  move.w  (0x2C,A0),D0            ; D0 = angle word          (read 1)
002230  move.w  (0x2A,A0),D1            ; D1 = speed word          (read 2)
002234  mulu.w  #1,D1                   ; speed *= 1 (patch hook; TURBO mod point)
002238  andi.l  #0xFF,D0                ; angle := low byte, 0..255
00223E  andi.l  #0xFF,D1                ; speed := low byte, 0..255
002244  bsr     0x22C0                  ; -> D2 = dx (s32), D3 = dy (s32)
002248  add.l   D2,(0x06,A0)            ; x16.16 += dx             (write 1)
00224C  add.l   D3,(0x0A,A0)            ; y16.16 += dy             (write 2)
002250  rts
```

Mode 1 never touches z, never applies gravity, has no accumulator other than the
16.16 position longs themselves. Reads exactly +0x2C then +0x2A; writes exactly the
two position longs, in x-then-y order.

### Mode 2 — per-axis velocity + gravity (0x2252), the airborne/thrown mover

Velocities are **signed 8.8 pixels/frame** words: vx +0x58, vy +0x5A, vz +0x5C,
gravity +0x5E (catalog row 66). Per axis the delta is built identically
(x at 0x2252-0x2266, y at 0x226A-0x227C, z at 0x2280-0x2294):

```
        moveq   #0,D0                   ; (x only; redundant, mulu rewrites all 32 bits)
        move.w  (0x58,A0),D0            ; u16 load of velocity
        mulu.w  #1,D0                   ; *1 patch hook; result zero-extends to 32 bits
        andi.l  #0xFFFF,D0              ; redundant
        swap    D0                      ; D0 = vel << 16
        asr.l   #8,D0                   ; ARITHMETIC >> 8  =>  D0 = sign16(vel) << 8
        add.l   D0,(0x06,A0)            ; position16.16 += vel*256
```

i.e. exact C: `pos32 += ((int32_t)(int16_t)vel) << 8;` — sub-pixel precision comes
from the low 8 bits of vel landing in the fraction word. Order: x (0x2266), y (0x227C),
z (0x2294), **then** gravity is applied to vz:

```
002298  move.w  (0x5E,A0),D0            ; gravity term (u16 via mulu#1/andi.l#FFFF)
0022A6  sub.w   D0,(0x5C,A0)            ; vz -= grav        (16-bit, wraps)
0022AA  tst.w   (0x5C,A0)
0022AE  bpl     0x22BE                  ; vz >= 0 -> done
0022B0  cmpi.w  #0x2000,(0x5C,A0)
0022B6  bcc     0x22BE                  ; branch if (u16)vz >= 0x2000 (unsigned!)
0022B8  move.w  #0x2000,(0x5C,A0)       ; terminal clamp -- DEAD CODE
0022BE  rts
```

**The clamp at 0x22B8 is unreachable**: it requires vz < 0 signed (so (u16)vz >=
0x8000) *and* (u16)vz < 0x2000 — contradictory. Presumably a bcs/#-0x2000 typo in the
original. Transcribe faithfully (i.e. the clamp never fires); vz simply keeps wrapping.
Note gravity is *subtracted*; jump code elsewhere loads +0x5C/+0x5E with the signs it
needs (z adds into screen-y in 0x247C, so smaller z = higher on screen).

---

## 2. Trig core 0x22C0 / quadrant table 0x22D0 — exact integer math

Input: D0 = angle 0..255, D1 = speed 0..255. Output: D2 = dx, D3 = dy (signed 32-bit
16.16 deltas). Clobbers A1.

```
0022C0  move.l  D0,D2
0022C2  lsr.w   #6,D2                   ; quadrant = angle >> 6   (0..3)
0022C4  asl.w   #2,D2
0022C6  lea     0x22D0,A1
0022CC  jmp     (A1,D2.w)               ; bra.w table, 4 bytes/entry
0022D0  bra 0x22E0 / 0x22FE / 0x2328 / 0x234E   ; Q0 Q1 Q2 Q3
```

Two **65-entry u16 tables** (index 0..64 inclusive — the 65th entry exists precisely
because the mirror quadrants index `0x40 - n` which reaches 64):

- `SIN5120` at ROM **0x2378**: `round(5120 * sin(i*pi/128))`, i = 0..64
  (0x0000 ... 0x1400). Used for the **x** component. 5120 = 0x1400 = 1.25 * 4096:
  x deltas are aspect-scaled 1.25x relative to y.
- `COS4096` at ROM **0x23FA**: `round(4096 * cos(i*pi/128))`, i = 0..64
  (0x1000 ... 0x0000). Used for the **y** component.

Per quadrant (all multiplies are `mulu.w` — **unsigned 16x16 -> full 32-bit** result;
sign is applied afterwards with `neg.l`; index is doubled with `asl.w #1` for the word
tables):

| Q | angle | PC | index i | dx (D2) | dy (D3) |
|---|---|---|---|---|---|
| 0 | 0x00-0x3F | 0x22E0 | `i = a`            | `+SIN5120[i]*spd` | `+COS4096[i]*spd` |
| 1 | 0x40-0x7F | 0x22FE | `i = 0x40-(a-0x40)` (1..64) | `+SIN5120[i]*spd` | `-COS4096[i]*spd` (`neg.l D3` @0x2324) |
| 2 | 0x80-0xBF | 0x2328 | `i = a-0x80`       | `-SIN5120[i]*spd` (`neg.l D2` @0x2348) | `-COS4096[i]*spd` (`neg.l D3` @0x234A) |
| 3 | 0xC0-0xFF | 0x234E | `i = 0x40-(a-0xC0)` (1..64) | `-SIN5120[i]*spd` (`neg.l D2` @0x2374) | `+COS4096[i]*spd` |

Details worth reproducing exactly:
- Q1/Q3 mirror: `subi.b #$40/#$C0, D0` (byte op), then `move.l #0x40,D3; sub.w D0,D3`
  so i runs 0x40 down to 1 (never 0) — continuity across quadrant seams is exact:
  a=0x3F -> Q0 i=63; a=0x40 -> Q1 i=64 (SIN peak 0x1400, COS 0); a=0x7F -> Q1 i=1.
- Result: `dx = sin256(a) * 5120 * spd`, `dy = cos256(a) * 4096 * spd` where
  `sin256/cos256` use the compass convention **angle 0 = +y (down-screen), 0x40 = +x
  (right), 0x80 = -y, 0xC0 = -x** (verified: walk table 0xF2D6 maps joystick codes to
  0x00/0x40/0xC0/0x20/0xE0/0x80/0x60/0xA0 -> +0x2C).
- **No shift is applied to the product.** The scaling falls out of adding the raw
  32-bit product to the 16.16 position: pixels/frame = product / 65536.

Table contents (transcribe verbatim into C):

```
SIN5120[65] = {  /* ROM 0x2378..0x23F9 */
0x0000,0x007E,0x00FB,0x0179,0x01F6,0x0273,0x02EF,0x036B,0x03E7,0x0462,
0x04DC,0x0556,0x05CE,0x0646,0x06BD,0x0733,0x07A7,0x081B,0x088D,0x08FE,
0x096E,0x09DC,0x0A48,0x0AB3,0x0B1D,0x0B84,0x0BEA,0x0C4E,0x0CB0,0x0D10,
0x0D6E,0x0DCA,0x0E24,0x0E7C,0x0ED2,0x0F25,0x0F76,0x0FC4,0x1010,0x105A,
0x10A1,0x10E6,0x1128,0x1167,0x11A3,0x11DD,0x1214,0x1249,0x127A,0x12A9,
0x12D5,0x12FE,0x1324,0x1347,0x1367,0x1384,0x139E,0x13B5,0x13C9,0x13D9,
0x13E7,0x13F2,0x13FA,0x13FE,0x1400 };

COS4096[65] = {  /* ROM 0x23FA..0x247B */
0x1000,0x0FFF,0x0FFB,0x0FF5,0x0FEC,0x0FE1,0x0FD4,0x0FC4,0x0FB1,0x0F9C,
0x0F85,0x0F6C,0x0F50,0x0F31,0x0F11,0x0EEE,0x0EC8,0x0EA1,0x0E77,0x0E4B,
0x0E1C,0x0DEC,0x0DB9,0x0D85,0x0D4E,0x0D15,0x0CDA,0x0C9B,0x0C5E,0x0C1E,
0x0BDB,0x0B97,0x0B50,0x0B08,0x0ABF,0x0A73,0x0A26,0x09D8,0x0988,0x0937,
0x08E4,0x088F,0x083A,0x07E3,0x078B,0x0732,0x06D7,0x067C,0x061F,0x05C2,
0x0564,0x0505,0x04A5,0x0444,0x03E3,0x0381,0x031F,0x02BC,0x0259,0x01F5,
0x0191,0x012D,0x00C9,0x0065,0x0000 };
```

Spot checks: SIN5120[32]=0x0E24=3620≈5120·sin45°; COS4096[32]=0x0B50=2896≈4096·cos45°.

---

## 3. Callers — who runs apply_motion per frame, for which objects

All `jsr $2208` sites: 0x1CF8 (bsr), 0x8068, 0xA908, **0xF558**, 0xFEB6, 0x1FB86,
0x1FCE8, 0x1FD88, 0x1FF98.

### 3a. Main per-object loop — loop head **0xF4C2**

Called once per game frame from the main frame loop at **0x0FE8** (and the alternate
frame path at **0x1098**). Iteration rule:

```
00F4C2  clr.w   D0                      ; slot index = 0
00F4C4  jsr     0x250E                  ; iterator: next ACTIVE object
00F4CA  bcs     0xF510                  ; carry set = slots exhausted -> tail rts
00F4CE  btst    #7,(0x32,A0)            ; *** +0x32 bit7 = SKIP this object entirely
00F4D4  bne     0xF50E                  ;     (no state latch, no motion, no draw)
00F4D8  btst    #7,(0x20,A0)            ; +0x20 bit7 = state-latch consumed flag
00F4DE  bne     0xF4F2
00F4E0  move.w  (0x1C,A0),(0x1E,A0)     ; prev state <- cur
00F4E6  move.w  (0x20,A0),(0x1C,A0)     ; cur state  <- requested
00F4EC  bset    #7,(0x20,A0)            ; mark request consumed
00F4F2  bsr     0xF518                  ; per-object update (ends in jsr 0x2208)
00F4F6  btst    #1,$1C0161              ; global mode flag (tag/2-ring mode)
00F4FE  beq     0xF50E
00F500  btst    #2,(0x33,A0)            ; +0x33 bit2 gates aux-sprite spawn 0xF8D8
00F506  bne     0xF50E                  ;     (rope-hold shadow obj @1C1134; NOT motion)
00F50E  bra     0xF4C4
```

Iterator **0x250E**: `if (D0 >= 9) {carry=1; return}` else `A0 = *(long*)(0x5122 +
D0*4); D0++; if (!(obj->status[0] & 0x80)) retry; carry=0` — i.e. the **first 9 slots**
of the 0x5122 table, active = **bit 7 of the byte at +0x00** (bit 15 of the status
word; spawns write 0x8001 to +0x00).

Per-object body **0xF518** (order matters):

```
00F518  clr.w   (0x36,A0)               ; clear collision-result word
00F51C  tst.b   (0x01,A0)               ; mode 0 -> skip collision/push-back
00F520  beq     0xF546
00F522  jsr     0x280DC                 ; bounds/collision test (sets +0x37 flags)
00F528  tst.b   (0x37,A0)
00F52C  beq     0xF546
00F52E-0xF542   add.w (0x38,A0)->x_int(+0x06), (0x3A)->y_int(+0x0A), (0x3C)->z_int(+0x0E)
                                        ; push-back: WORD adds to integer words only
00F546  jsr     0x1C03E                 ; anim-cell state machine
00F54C  jsr     0x247C                  ; world->screen: +0x14 = x_int - cam($1C17E6)
                                        ;   ± sext8(+0x18) (negated if +0x2E bit7 set);
                                        ;   +0x16 = y_int + z_int - cam($1C17EE) + sext8(+0x1A)
00F552  jsr     0x27B8                  ; sprite emit
00F558  jsr     0x2208                  ; <<< apply_motion — every active, non-frozen slot
00F55E  rts
```

So the **effective per-frame gates** on motion for a normal object are:
1. +0x00 bit7 (byte) — slot active (iterator 0x250E).
2. +0x32 bit7 — object frozen/suspended (0xF4CE) — skips everything.
3. +0x01 mode byte — 0 means no movement (inside 0x2208).
(+0x20 bit7 only controls the state latch; +0x33 bit2 only gates the auxiliary
0xF8D8 sprite, not motion.)

### 3b. Other call sites (extra invocations, same semantics)

- **0x1FB86 / 0x1FCE8 / 0x1FD88 / 0x1FF98** — inside the *referee* state machine
  (fixed object 0x1C11F4; dispatcher 0x1F914, jump table 0x1F952 indexed by state byte
  +0x21; called from the frame loop at 0x0FE2/0x1092). The referee is NOT in the
  9-slot loop, so its states call 0x2208 themselves (e.g. 0x1FB72 sets mode 1 and
  walks; 0x1FCE8 path steers +0x2C toward a target x at +0x60 then moves).
- **0xFEB6** — aux objects at 0x1C0F1C / 0x1C1028 (slots 9/10 of the 0x5122 table),
  handler 0xFDEE (frame loop 0x0FFA/0x109E), state table 0xFE22 indexed by +0x1C & 0xF;
  only runs when $1C0161 bit1 set (tag-team extras: valets/managers).
- **0x8068** — ring-entrance sequence (sets mode via +0x2A=8, +0x2C=0x80 at 0x801C,
  wrestler walks down the aisle).
- **0xA908** — an effect handler (sets +0x2A=8, +0x2C=0, animates +0x22/+0x24 while
  drifting).
- **0x1CF8** — attract/title screen: loop 0x1CF2 iterates the same 9 slots via 0x250E
  and flies 6 demo objects (mode set at 0x1CBE-0x1CC7: +0x2C=0, +0x2A=0xC).

---

## 4. Units — what speed 0x18 means

Mode 1: pixels/frame = `trig * speed / 65536`.
- Along **y** (angle 0/0x80): `4096 * speed / 65536` = **speed/16 px/frame**.
  speed 0x18 (24) -> **1.5 px/frame**.
- Along **x** (angle 0x40/0xC0): `5120 * speed / 65536` = **speed * 1.25 / 16**.
  speed 0x18 -> **1.875 px/frame** (deliberate 1.25 aspect boost on x).
- Diagonals interpolate exactly per the tables (elliptical, not normalized).
So +0x2A speed is effectively **4.4 fixed-point pixels/frame in y-units**. Observed P1
peak 0x2F ≈ 2.94 px/f y, 3.67 px/f x (catalog row 63).

Mode 2: pixels/frame = `(int16)vel / 256` per axis (8.8 fixed). Gravity: vz decreases
by (u16)grav each frame (terminal clamp is dead code, see §1).

---

## 5. C sketch (integer-exact)

```c
/* Object fields used (offsets from object base, big-endian 68k):
   +0x01 u8  mode; +0x06/+0x0A/+0x0E s32 x,y,z as 16.16 (frac at +0x08/+0x0C/+0x10);
   +0x2A u16 speed (low byte used); +0x2C u16 angle (low byte used);
   +0x58/+0x5A/+0x5C s16 vx,vy,vz (8.8 px/f); +0x5E u16 grav. */

static const uint16_t SIN5120[65] = { /* ROM 0x2378, see §2 */ };
static const uint16_t COS4096[65] = { /* ROM 0x23FA, see §2 */ };

/* ROM 0x22C0-0x2376: quadrant dispatch + lookup */
static void sincos_step(uint32_t angle, uint32_t speed, int32_t *dx, int32_t *dy)
{
    switch ((angle >> 6) & 3) {                     /* 0x22C2 lsr.w #6 */
    case 0: {                                       /* 0x22E0 */
        uint32_t i = angle;                         /* 0..63 */
        *dx =  (int32_t)(SIN5120[i] * speed);       /* mulu.w: u16*u16 -> u32 */
        *dy =  (int32_t)(COS4096[i] * speed);
        break; }
    case 1: {                                       /* 0x22FE */
        uint32_t i = 0x40u - (angle - 0x40u);       /* 1..64 mirror */
        *dx =  (int32_t)(SIN5120[i] * speed);
        *dy = -(int32_t)(COS4096[i] * speed);       /* neg.l @0x2324 */
        break; }
    case 2: {                                       /* 0x2328 */
        uint32_t i = angle - 0x80u;                 /* 0..63 */
        *dx = -(int32_t)(SIN5120[i] * speed);       /* neg.l @0x2348 */
        *dy = -(int32_t)(COS4096[i] * speed);       /* neg.l @0x234A */
        break; }
    default: {                                      /* 0x234E */
        uint32_t i = 0x40u - (angle - 0xC0u);       /* 1..64 mirror */
        *dx = -(int32_t)(SIN5120[i] * speed);       /* neg.l @0x2374 */
        *dy =  (int32_t)(COS4096[i] * speed);
        break; }
    }
}

/* ROM 0x2208 */
void apply_motion(obj *o)
{
    switch (o->mode /* +0x01 */) {                  /* 0x220E-0x221A jump table 0x2224 */
    case 0:
    default:                                        /* mode 0 (and 3): no motion */
        return;

    case 1: {                                       /* 0x222C polar */
        uint32_t angle = o->angle & 0xFF;           /* 0x2238 andi.l #$ff (+0x2C) */
        uint32_t speed = (o->speed * 1) & 0xFF;     /* 0x2234 mulu#1, 0x223E mask (+0x2A) */
        int32_t dx, dy;
        sincos_step(angle, speed, &dx, &dy);        /* 0x2244 bsr 0x22C0 */
        o->x += dx;                                 /* 0x2248 add.l  (16.16 @ +0x06) */
        o->y += dy;                                 /* 0x224C add.l  (16.16 @ +0x0A) */
        return; }

    case 2: {                                       /* 0x2252 velocity + gravity */
        o->x += ((int32_t)o->vx) << 8;              /* 0x2266  (+0x58, 8.8 px/f) */
        o->y += ((int32_t)o->vy) << 8;              /* 0x227C  (+0x5A) */
        o->z += ((int32_t)o->vz) << 8;              /* 0x2294  (+0x5C) */
        o->vz = (int16_t)((uint16_t)o->vz - o->grav); /* 0x22A6 sub.w (+0x5E), wraps */
        if (o->vz < 0 && (uint16_t)o->vz < 0x2000)  /* 0x22AA-0x22B6: provably false */
            o->vz = 0x2000;                         /* 0x22B8 dead code, kept verbatim */
        return; }
    }
}
```

Caller contract to reproduce (per frame, ROM 0xF4C2): for slot 0..8 of table 0x5122,
if `(status_byte0 & 0x80)` and `!(field32 & 0x80)`, run the 0xF518 sequence
(clear +0x36; if mode: collide 0x280DC, apply +0x38/3A/3C word push-back to integer
position words when +0x37 set; anim 0x1C03E; screen calc 0x247C; sprite 0x27B8;
then `apply_motion`). Referee (0x1C11F4) and tag-aux objects (0x1C0F1C/0x1C1028) call
`apply_motion` from their own state handlers instead.
