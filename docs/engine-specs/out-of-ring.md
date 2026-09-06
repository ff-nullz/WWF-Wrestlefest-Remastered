# Out-of-ring: scene floors, the exit, landing outside, re-entry, count-out

Source `../wrestlefest-decomp/reference/maincpu.asm` (read-only, 2026-08-22).
Field names from `engine/engine.h`; conventions as in `docs/engine-specs/rope-fall.md`
(x/y/z int words at +0x06/+0x0A/+0x0E; `zone` +0x36, `clip` +0x37, pushbacks
+0x38/+0x3A/+0x3C; D0/D1/D2 in the floor handlers = probe x / y+clip_h / z+floor42).

Two flags that the task text conflates — they are different bytes:

| flag | meaning | set | cleared |
|---|---|---|---|
| **`+0x33` bit2** | **"I am outside the ring"** — selects the odd scene-dispatch slot (see §1) | 0x1B954 (react 0x16 launch out), 0x1B540 (react 0x0E launch out), 0x19C54 (climb-out move, cell 0x19C28), 0xFADE (ring-out scene init, non-legal man), 0xE292 (tag-partner apron, only when $1C0161 bit1) | 0x19A90 (climb-in move 0x69, frame 4 start), 0x12304/0x123FE (cage climb cell 0x122A6) |
| `+0x32` bit2 | "just turned around" latch (state-6 turn / AI run-turn). NOT out-of-ring. | 0xF3BC, 0x1C4CA, 0x1C52E | 0x114CC (state 0 init), 0x11E04 |

`0x28124` does `move.w (+0x32),D4; andi.l #4` — a **word** read, so the bit tested is
bit2 of the LOW byte = **`+0x33` bit2**.

Globals: `$1C007E` scene word (`$1C007F` low byte); `$1C0161` bit0 = rumble mode,
bit1 = **ringside scene is showing** (set 0xF9D6 on the ring-out scene switch, cleared
0xF9F4 on return; not "cage" — TODO EXACT whether cage also sets it elsewhere);
`$1C1678` ring-out scene trigger; `$1C169A/B` count-out counter; `$1C1802` HUD shake;
`$1C1804/6` scroll target; `$1C1808[]/$1C1828` tile-event queue.

---

## 1. Scene dispatch 0x28124 / table 0x28154

```
28128  A1 = 0x28154
2812E  D3 = $1C007E & 7 ; D3 <<= 3           ; scene*8
2813C  D4 = word(+0x32) & 4                  ; +0x33 bit2 ? 4 : 0
28146  A1 = table[D3 + D4] ; jsr (A1)
```

| scene | inside (+0x33 b2 = 0) | outside (b2 = 1) |
|---|---|---|
| 0 | 0x2818E ring floor | 0x28288 ringside floor (zone 3) |
| 1 | 0x2831C cage floor (zones 1/5/6) | 0x2818C rts (no floor) |
| 2 | 0x28480 ringside-scene apron (zone 1/4) | 0x2851E ringside-scene floor (zones 1/3/4) |
| 3 | rts | rts |
| 4 | rts | rts |
| 5 | 0x2818E | 0x28288 |
| 6 | 0x28480 | 0x2851E |

Scene 2/6 = the ring-out camera scene. Stage -> ring-out scene byte: table 0xFA00
`[02 06 00 06 02 06 02 02 06 02]` indexed by `$1C0162`; return table 0xFA0A
`[00 05 00 05 00 05 00 00 05 00]` (0xF9C2-0xF9FC). Scene 1 is a distinct floor with
zone 5 (low X crossing, clamp) / zone 6 (high X crossing z>=0x180, clamp with the
wider 0x38000/0xAC000 rows) — rope-fall.md calls it cage; 0x1B976 treats zone 6 as
"went over the ropes" (sounds+shake). TODO EXACT: which match mode writes
`$1C007E = 1`.

## 2. Out-of-ring floor 0x28288 (scene 0/5, +0x33 bit2 set) — write table

Inputs D0 = x+lookahead, D1 = y+clip_h, D2 = z+floor42. No +0x32 bit0 gate (unlike
0x2818E). **No Y law at all** — y is free outside.

| PC | condition | writes |
|---|---|---|
| 0x2828C-0x282A6 | `D2 < 0x100` | clip bit4 (landed); `+0x3C = (0x100 - D2) + floor42` = `0x100 - z` → **floor level outside is z = 0x100** (0x40 below the mat plane 0x140) |
| 0x282AA-0x282DC | `D0 < 0x1C7` and `D0 < ((D1<<8)+0x28000)/0x2E0` | `+0x38 = xmin - D0` (push right), clip bit1, **zone = 3** |
| 0x282E0-0x28312 | `D0 > 0x360` and `D0 > -(((D1<<8)-0xBA000)/0x2E0)` | `+0x38 = xmax - D0` (push left), clip bit0, **zone = 3** |
| else | | zone stays 0 (cleared at 0xF518), no writes |

So zone 3 means **"touching the barrier"** (the outer trapezoid, rows 0x28000/0xBA000
vs the ring's 0x40000/0xA3000 — about 0x50..0x80 px wider per side), applied at any
height (no z gate). There is no y clamp and no apron: the mat edge is not a wall from
outside.

Scene-2 variants (same structure, constants only):

- 0x28480 (inside flag clear): `y != 0x160` → clip bit2, `+0x3A = 0x160 - y`, zone 1
  (y pinned to 0x160); `z < 0x140` → clip **bit1** (sic, not bit4), `+0x3C = 0x140 - z`;
  `x < 0x270` → push to 0x270 clip bit1 zone 1; `x > 0x3D0` → push to 0x3D0 clip bit0
  zone 1; else `0x2B0 <= x < 0x390` → **zone 4** (the "in front of the ring" band).
- 0x2851E (outside flag set): `y < 0x110` → push to 0x110 clip bit3 zone 1;
  `y > 0x140` → push to 0x140 clip bit2 zone 1; `0x294 <= x < 0x38C` → zone 4;
  `z < 0x100` → clip bit4, `+0x3C = 0x100 - z`; `x < 0x1C8` → push clip bit1 **zone 3**;
  `x > 0x470` → push clip bit0 zone 3.

```c
static void floor_out_28288(eng_obj *o, int16_t px, int16_t py, int16_t pz)
{
    if (pz < 0x100) { o->clip |= 0x10; o->push_z = (int16_t)(0x100 - pz) + o->floor42; }
    if (px < 0x1C7) {
        int16_t xmin = (int16_t)((((int32_t)py << 8) + 0x28000) / 0x2E0);
        if (px < xmin) { o->push_x = xmin - px; o->clip |= 0x02; o->zone = 3; return; }
    }
    if (px > 0x360) {
        int16_t xmax = (int16_t)(-((((int32_t)py << 8) - 0xBA000) / 0x2E0));
        if (px > xmax) { o->push_x = xmax - px; o->clip |= 0x01; o->zone = 3; }
    }
}
```

## 3. Leaving the ring

### 3a. The high crossing (zone 2) is NOT an exit by itself

0x2818E at `z_probe >= 0x180` sets zone=2 + the X clip bit and **no pushback**; nobody
tests zone==2 (grep: no `cmpi.b #2,(+0x36)` in the reaction family). A zone-2 body keeps
flying with +0x33 bit2 clear, so next frame it is still probed by 0x2818E and, once it
descends below 0x180, is pushed back inside. The exit is therefore decided **at launch**,
not by the crossing: the thrower's reaction handler runs a corner-diagonal test and sets
`+0x33` bit2 up front, which switches the probe to 0x28288 for the whole flight.

Exit launchers (all: `0x258E(id)` from the table at 0x25CA, `bset #2,+0x33`, rumble →
`bset #4,+0x32` + thrower `+0xC4 += 1`; both refuse when `$1C007F == 1` or `$1C0161` bit1):

| reaction | cell | test (facing R / L) | launch row |
|---|---|---|---|
| 0x16 (thrown) | 0x1B8BA → 0x1B8CA | R: `x > -((y<<8 - 0x9C000)/0x2E0)` (0x1B8F8); L: `x <= ((y<<8)+0x48000)/0x2E0` (0x1B916) | 0x20 = {vx -0x480, vz +0x400, g 0x58} (0x1B94A) |
| 0x0E (lifted & thrown) | 0x1B460 → 0x1B478 | R: `x > -((y<<8 - 0x96000)/0x2E0)` (0x1B4DC); L: `x <= ((y<<8)+0x4B000)/0x2E0` (0x1B4FA) | 0x0E = {vx -0x300, vz +0x780, g 0x48} (0x1B532), mover stays 0 until frame-0 expiry (0x1B5B2 `mover=2`) |
| 0x25 (press slam over ropes) | 0x1BF08 | set by the attacker, §5 | 0x25 = {vx -0x140, vz +0x200, g 0x40} |
| climb-out move (cell 0x19C28) | — | voluntary | 0x0F = {0, +0x100, 0x38} at frame 3 (0x19CA0) |

Launch table 0x25CA (6 bytes/row {vx, vz, grav}, vx negated on facing-right by 0x258E):
row 0x0D {+0x40, +0x100, 0x38}, 0x0E {-0x300, +0x780, 0x48}, 0x0F {0, +0x100, 0x38},
0x20 {-0x480, +0x400, 0x58}, 0x25 {-0x140, +0x200, 0x40}.

Writers: `+0x64 = 0x16` at 0x160AC / 0x16D9C (throw enders); `+0x64 = 0x0E` at
0x1A8DE/0x1A978/0x1AA1A/0x1AAD6/0x24512.

### 3b. Flight outside (reaction 0x16 per-frame, 0x1B976-0x1BA80; 0x0E is the same shape at 0x1B562-0x1B66A)

| PC | writes |
|---|---|
| 0x1B976-0x1B994 | zone==6 once (`bset #0,+0x45` latch): sounds 0x28, 0x32 (0x0E also `vx=0`, `$1C1802=0xE`) |
| 0x1B99A-0x1B9CA | zone==6 and z<0x180: X clip bit1 → `$1C1150=$1C1180=1`, else `$1C11B0=$1C11E0=1` (rope shake) |
| 0x1B9CC | only on frame `+0x25 == 1` (0x0E: frame 3): |
| 0x1B9D6 | `+0x42 = -0x20` (floor42: trigger contact 0x20 px early, snap unbiased) |
| 0x1B9DC | `jsr 0x10FC6` (below) |
| 0x1B9E2 not landed | zone==3 (barrier hit in flight): `$1C0161` bit1 clear → **land outside** 0x1BA48; bit1 set → sound 0x28 only (0x1BA72) |
| 0x1B9E2 landed (clip bit4) | `+0x33` bit2 set and bit1 clear → **land outside** 0x1BA48; else in-ring bounce 0x1BA1A: `state=4, +0x65=5`, spr `0x1F|facing`, `0x10BD0(-0x38,0,0)`, thud 0x1110E |
| 0x1BA48 land outside | `state = 5`, **`move_id = 0x68`**, `+0x44 = 0`, spr `0x16|facing`, `0x10BD0(+0x10,0,0)`, sound 0x28, `+0x42 = 0` |

`0x10FC6` (0x10FC6-0x11056), the rope-bump check every air reaction calls:

```
10FC6  if $1C0161 bit1 (ringside scene): goto 11024
10FD0  if zone==1 or zone==5: clip b1 → $1C1150=$1C1180=1 (unless $1C1151 busy)
                               clip b0 → $1C11B0=$1C11E0=1 (unless $1C11B1 busy); C=1 rts
11024  elif zone==3: state=4, +0x64=0x17, sounds 0x28, 0x32          ; barrier slam
1104C  C=0 rts
```

### 3c. Reaction 0x17 — barrier / hard-contact fall (cell 0x1BA82, handler 0x1BA92)

Cell: `{0x1BA92, mode 1, n 2, dur 0x10, 0xFF00, spr 0x11, 0x14}` (flying, flat — same art
as the punch knockdown). Also entered by the run state on an X clip with zone 3
(0x11CE0: `+0x68 = 0x0A`, sound 0x28 → 0x11D2A `state=4, +0x64=0x17, +0x44=0`) and by a
whip rebound with zone 5 and `+0x44 != 0` (0x11CFA: `+0x68 = 8`, 0x11D56 shake).

| PC | writes |
|---|---|
| 0x1BA9A init | `bclr #4,+0x33` |
| 0x1BAA0 | `0x258E(0x0D)`: vx=+0x40 (back), vy=0, vz=+0x100, grav=0x38, mover=2 |
| 0x1BAAA | `$1C1802 = 0xE` (HUD shake) |
| 0x1BAB2 | `$1C007F == 1` → sound 0x28 |
| 0x1BAC8 per frame | landed (clip b4): thud 0x1110E (0x29 / 0x2D if +0x33 b2), `state=4, +0x65=5` (bounce), spr `0x1F|facing` |

No rope shake, no 0x10FC6, no floor42 — a short hop then the normal bounce(5) →
lying(8/9) chain, on whichever floor the scene flag selects (outside: z=0x100).

### 3d. Lying outside and getting back in — move 0x68 (cell 0x1981E, handler 0x19832)

Cell `{0x19832, mode 1, n 3, dur FF00, 0x40, 0x10, spr 0x16, 0x13, 0x68}`; handler
dispatches on `+0x44 & 3` through 0x19848:

| phase | PC | writes |
|---|---|---|
| 0 land | 0x19858 init | `mover=2`, `vx=vy=0`, `+0x19 = 0xD0` |
| | 0x1987E landed | `0x1112E` (sound 0x33 on frames 1/4); not rumble and legal (+0x33 b0): **`$1C1678 = 0x8000`, `$1C1679 = facing`, `state=5, move=0x6A`** (ring-out scene); else `mover=0`, `bclr #3,+0x33`, `+0x22 = 0` (release the FF00 hold) |
| | 0x198E6 FE | not rumble: `state=5, +0x44=1` (same move, phase 1); rumble: `mover=0, state=5, move=0x7A, +0x44=4`, `$1C15D2 = id, $1C15D3 = 0x29` (eliminated) |
| 1 walk to corner | 0x19934 | `mover=1`, `0x1174C` (walk speed by id), `bchg #7,+0x2E`; target `(+0xBE,+0xC0) = (0x3B0,0xE0)` if x>=0x280 else `(0x142,0xE0)`; `0x11710`+`0x1F15C` walk (arrive when \|dx\|<8 and \|dy\|<8) → `state=5, +0x44=2` |
| 2 walk to centre | 0x19992 | target `(0x279, 0xE0)` → `+0x44=3` |
| 3 walk up | 0x199C4 | `mover=1, angle=0` (+y), until `y >= 0x100`: `mover=0, y=0x100`, **`state=5, move=0x69`** |

Move 0x69 — climb in through the ropes (cell 0x199FC `{0x19A20, mode 1, n 7, dur 0xC×7,
spr 0x20..0x26}`):

| PC | writes |
|---|---|
| 0x19A28 init | `mover=0`, `+0x1B = 0x57`; not ringside scene: queue byte 1 into `$1C1808[$1C1828++]` (rope-part tile event, consumed by 0x298F4 → tile table 0x299F4); ringside scene: `y=0x150, z=0xF0`, `+0x74` bit7 → partner `+0x1C = 2` |
| 0x19A82 frame 4 start | **`bclr #2,+0x33`** — back inside; probe returns to 0x2818E |
| 0x19A96 FE | `bset #7,+0x18`, `state=0`, `$1C1697=1`, `+0xAE=0`, spr=facing; not ringside: **`z=0x140, y=0x118`** (stands on the top rope line), queue byte 2; ringside: `z=0x140, y=0x160`, then scan: both legal men out/down → `$1C1678 = 0xC000` |

Forced return at count 10: AI 0x1C6DC / 0x1EBF4 (`$1C169B >= 0x0A`, legal, `+0x56` b5
clear): `move=0x69, (+0xBE,+0xC0)=(0x310,0x138), state=1, +0xAE=0x0A` — walks then
climbs. 0x1D74A-0x1D798 (AI ringside): clamp x to 0x279 / y to 0x100 then move 0x69.

Moves 0x6A/0x6B (ringside scene): 0x6A (cell 0x19B3C, spr 0x13,0x68,0x00, dur 0x40,0x10,8):
init `mover=0, +0x19=0xD0, z=0x100`, teleport to `(0x1F0,0x168)` if x<0x280 else
`(0x438,0x168)`; FE → `move=0x6B`, `bchg #7,+0x2E`. 0x6B (mode 0, 0x19BC0): `mover=1`,
walk to `(0x42C,0x120)` / `(0x210,0x120)` by side → `state=0`, `bclr #5,+0x32`.
Ring-out scene init 0xFA78-0xFB56: the man in move 0x6A with +0x33 b2: `$1C1806=0x200`,
`$1C1804 = 0x350/0x190` by `$1C1679` b7, `bset #5,+0x32` (probe-exempt), `+0x68 = 8`
(8 damage for the fall); the non-legal man: `+0x32 &= 0xC3`, `bset #2,+0x33`,
`bset #5,+0x32`, `move=0x6B`, `y=0x164, z=0x100`, `x=0x200` or mirrored; others
`x = 0x2A0/0x3A0`.

## 4. Sprite-stream row 0x20

`data/romdata/mv_sprite_stream_ptrs.json` row 32 = stream **0x06957A** (rows 12..80 are
non-wrestler streams: referee, managers, effects). No `move.w #$20,(+0x02)` writer exists;
the row is not selected by any of the out-of-ring code above — the wrestler's own stream
is used throughout (poses 0x16 lying-outside, 0x13, 0x68, 0x20..0x26 climb-in, 0x27..0x2B
climb-out). TODO EXACT: the object that uses row 0x20 (effects spawner, likely via
0x299F4/0x29A2C tile tables or a computed `+0x02`).

## 5. Press slam over the ropes (move 0x19, handler 0x15006, 0x15094-0x15116)

Entry: end of frame 3 (`+0x24==3 && +0x22==0`), **not** ringside scene (`$1C0161` b1
clear), then `+0x3E = 0x80; jsr 0x280DC; +0x3E = 0` and **`+0x36 & 3 != 0`** — i.e. the
probe 0x80 px ahead of facing crossed an X edge (zone 1 low, or zone 2 high, both carry
the clip bits... note: `andi.b #3,(+0x36)` masks the ZONE byte, so the test is zone ∈
{1,2,3}; TODO EXACT whether that is intended vs +0x37). Differences from the normal drop:

| PC | writes |
|---|---|
| 0x150CA | `bset #7,+0x60` (variant cell 0x14FDE) |
| 0x150D0 | own spr = `0x1FD \| facing` |
| 0x150DC-0x150F2 | partner `state=4`, **`+0x64 = 0x25`**, `bset #5,+0x64`, `+0x44 = 0`, `bchg #7,+0x2E` |
| 0x150F8 | `0x10B9A(0x80, 1, 0x39)` — partner 0x80 behind, z 0x39 up |
| 0x1510A | `0x11058` rope shake on the clipped side |
| 0x1510E | `bclr #6,+0x33` both |

Reaction 0x25 (0x1BF08): `0x258E(0x25)`, facing flip, `0x10F9C(0x30)`, and it **never sets
`+0x33` bit2** — the victim stays on the in-ring floor law, so it lands on the mat
(bounce 5 at 0x1BF4E+), not outside. The normal path (0x1514E) is reaction 0x21.

## 6. Count-out and the referee

- Trigger: `$1C1678 = 0x8000` (+ `$1C1679` = faller facing) at 0x198A0 when a **legal**
  man lands outside in a non-rumble match; `0xC000` at 0x19B2E when both are out/down.
- 0xF98C: bit7 → scene switch: `$1C007F = 0xFA00[stage]`, `bset #1,$1C0161`, all objects
  re-placed (0xFA3A loop), then 0xFB5A: vblank, `0x26E66` scene compose, referee teleport
  `(0x340,0x160)`, **referee `+0x20 = 0x8003`**, `bset #7,$1C0076`, `clr $1C1678`, YM 0x80.
  bit6 (return): `$1C007F = 0xFA0A[stage]`, `bclr #1,$1C0161`, `$1C1804/6 = 0x1E0/0x230`.
- Referee visual 3 (0x1FD5E): init `clr $1C169A`; x clamped to [0x298,0x390]; tick 0x1FDF4:
  `+0x24++`; at **0x50 frames** `$1C169A++`; at 20 (0x1FE0A) if neither legal man has
  `+0x33` b2 → rts (count parks); blit digit; `>= 0x11` → YM warnings; `== 0x14` → 0x1FE5A:
  both out → all four `+0xFE = 0x8005`; one out → out pair `0x8002`, in pair `0x8003`
  (0x1FEB2/0x1FF02), `0x90D6`, `clr $1C169E`, YM 0x3108 / jingle 0x20156.
- Wrestlers react at 10 (§3d). Count is per frame of the referee object; nothing else
  writes `$1C169A` except 0x2002C/0x20082 (pin count).

```c
/* 0x10FC6 */
static int rope_bump(eng_state *st, eng_obj *o)
{
    if (!(st->g1c0161 & 2)) {
        if (o->zone == 1 || o->zone == 5) {
            if (o->clip & 0x02) { eng_ropes_arm(0, 1, 1); return 1; }   /* 0x10FF0 */
            if (o->clip & 0x01) { eng_ropes_arm(1, 1, 1); return 1; }   /* 0x11012 */
        }
    } else if (o->zone == 3) {                                           /* 0x11024 */
        o->state = 4; o->react_id = 0x17; eng_sound(0x28); eng_sound(0x32);
    }
    return 0;
}

/* 0x1BA92 reaction 0x17 */
static void react_barrier_fall(eng_obj *o)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->f33 &= ~0x10u; knockback(o, 0x0D);  /* vx +0x40, vz +0x100, g 0x38 */
        hud_shake = 0xE; if (scene_lo == 1) eng_sound(0x28);
    } else if (o->landed) {
        thud(o); o->state = 4; o->react_id = 5; o->spr = 0x1F | o->facing;
    }
}

/* 0x1B8CA reaction 0x16 — launch side */
static void react_thrown_init(eng_obj *o, eng_obj *thrower)
{
    int out = 0;
    if (scene_lo != 1 && !(g1c0161 & 2)) {
        o->grap44 = 0;
        int16_t y = o->y >> 16, x = o->x >> 16;
        if (thrower->facing & 0x8000u) out = x > (int16_t)-((((int32_t)y<<8) - 0x9C000) / 0x2E0);
        else                            out = x <= (int16_t)(((((int32_t)y<<8) + 0x48000) / 0x2E0));
    }
    if (out) { knockback(o, 0x20); o->f33 |= 4;                    /* 0x1B94A */
               if (rumble) { o->f32 |= 0x10; thrower->c4++; } }
    else     { knockback(o, 0x0C); edge_arc(o, 0x30); }            /* 0x1B932 */
}
/* landing outside (0x1BA48): state=5, move_id=0x68, grap44=0, spr=0x16|facing,
 * add_pos_delta(o,+0x10,0,0), sound 0x28, floor42=0.  Re-entry: move 0x68 phases
 * 1..3 walk to (0x279,0x100) then move 0x69: f33 &= ~4 at frame 4, exit at
 * (y=0x118, z=0x140) state 0. */
```
