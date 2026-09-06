# Ring hardware (side ropes) — exact ROM semantics

Source: `../wrestlefest-decomp/reference/maincpu.asm` (all PCs below), data
decoded from `data/rom/31e14-0.ic18` (even) + `31e13-0.ic19` (odd) via the repo's own
`tools/thinker/native/libwfstream.so` (C port of the 0xD1FC sprite compiler). No repo
file was modified; nothing was compiled.

## 1. Who authors the ring-hardware sprites

The side ropes are **four ordinary game objects** (0x30 bytes each) in the aux object
area, ticked once per frame and pushed into the priority draw lists that `$2836`
drains into spriteram:

| Object | Record | State word (`+0x1C`) | Busy byte (`+0x1D`) | Role |
|---|---|---|---|---|
| left front  | `$1C1134` | `$1C1150` | `$1C1151` | left rope, near/lower half, draw list 1 |
| left back   | `$1C1164` | `$1C1180` | `$1C1181` | left rope, far/upper half, draw list 3 |
| right front | `$1C1194` | `$1C11B0` | `$1C11B1` | right rope, near/lower half, draw list 1, X-flipped |
| right back  | `$1C11C4` | `$1C11E0` | `$1C11E1` | right rope, far/upper half, draw list 3, X-flipped |

The "rope-shake byte pairs" of run-skid-turn.md §2c are simply these objects' state
words: `$1C1150 = $1C1134+0x1C` etc. "Busy" = low byte of the state word ≠ 0.

**Init — `0x1004A`** (rts `0x1011E`). Called from the match-scene init `0xC98` at
`0xCFA` (right after the extras init `0xFFD2`) and again from the ringside→ring
re-entry path at `0xFCC0`. It writes, per object:

```
+0x00 flags   = 0x8000            (live)
+0x1C state   = 0                 (idle)
+0x02 class   = 0x000E            (sprite-stream class 14 = "ring hardware"; also the
                                   colour-bank byte the 0xD1FC compiler runs through
                                   the $1C1610 bank remap -> bank 14 in spriteram)
+0x06 world X = 0x1C0 (left pair) / 0x330 (right pair)
+0x0A world Y = 0x178
+0x0E Z/lift  = 0x140             ($247C adds it to Y for the screen position)
+0x12 list    = 1 (front objects) / 3 (back objects)
+0x2E flip    = 0x0000 (left) / 0x8000 (right)   (copied into frame-word bit15)
```

**Per-frame runner — `0x10120`** (rts `0x10160`). Called from BOTH match frame lists:
`0x1000` (even list) and `0x10A4` (odd list) of the `0xF9A` frame loop — after the
wrestler tickers (`$F4C2`, `$FDEE`) and before the sprite-list drain `bsr $2836` at
`0x103A`. Guard: `btst #1,$1C0161` — bit1 set = ringside view active → whole routine
is a no-op (this is the gate `src/frame_10120.c` already ports as "skip-only").
Otherwise, for each of the 4 objects (pointer table `0x10162`):

```
handler = jumptable_0x10172[ state(+0x1C) & 0xF ]   ; 5 entries: 0x10186 0x101B2 0x10228 0x1029E 0x102F6
jsr handler ; jsr $247C (world->screen) ; jsr $27B8 (enqueue into draw list +0x12)
```

`$247C` (PC `0x247C`): `screenX(+0x14) = worldX(+0x6) − camX($1C17E6) ± sx8(+0x18)`,
`screenY(+0x16) = worldY(+0xA) + Z(+0xE) − camY($1C17EE) + sy8(+0x1A)` (the ±: the
byte adjust is negated when `+0x2E` bit7 set; both adjust bytes are 0 here).

`$27B8` (PC `0x27B8`): skips if flags bit7 clear or frame `(+0x4)&0x7FFF == 0x7FFF`;
for class byte `< 0x4E` first runs the screen-X clip `$2188` (carry → dropped); then
appends the OBJECT POINTER to draw list `+0x12` via the bucket table `0x2806`:

```
list 0: array $1C19D0, count $1C19B8      list 3: array $1C1B50, count $1C19C4
list 1: array $1C1A50, count $1C19BC      list 4: array $1C1BD0, count $1C19C8
list 2: array $1C1AD0, count $1C19C0      list 5: array $1C1C50, count $1C19CC
```

**Drain — `$2836`** compiles the lists into spriteram `$C2000` via `$D1FC` in order
**5, 4, 3, 2, 1, 0** (lists 5/4/2/0 Y-sorted by world Y via `$2948`; lists 3 and 1 —
the ring hardware — are NOT sorted). Later spriteram entries draw on top, so:
list 3 (back rope halves) sits UNDER the wrestlers' lists, list 1 (front rope halves)
sits OVER them, list 0 is frontmost. Wrestlers move between lists 0–4 per action
(e.g. rope-lean sets the wrestler to list 2 at `0x18382`).

## 2. The sprite data (class-14 stream)

`$D1FC` resolves object class `+0x2` through two ROM tables (PCs `0xD250–0xD280`):

- `0x38F14 + class*2` (word) → offset of the class frame-offset table.
  Class 14 word = `0x3710` → **frame table at ROM `0x3C624`**: 12 words
  `0000 0018 0026 003A 004E 0062 0076 008A 009E 00B2 00C6 FFFF` (11 frames).
- `0x38FB8 + class*4` (long) → **stream base = ROM `0x68BDC`** (class-14 stream is
  `0xDA` bytes, `0x68BDC..0x68CB5`; class 15 starts at `0x68CB6`).

Frame word `+0x4`: bit15 = X-flip (mirrors every per-sprite X offset and sets flipx),
low bits = frame index into the table above. Decoded frames (offsets are relative to
the object's screen origin `(+0x14, +0x16)`; x as added to screenX, y as added to the
compiled hardware Y word; `ch` = chain − N+1 16×16 tiles stacked vertically,
`tile+i`; all sprites bank/pal 14 via the `$1C1610` remap):

**Frames 2–6 — BACK half (far/upper tiers incl. the red top rope; idle = 2) — CORRECTED, the engine measured the y-offsets: 3 sprite columns each at x −17/−1/+15:**

| frame | col @ x−17 | col @ x−1 | col @ x+15 |
|---|---|---|---|
| 2 (idle) | y−23 tile 0xE744 ch1 | y−7 0xE750 ch3 | y+41 0xE760 ch3 |
| 3 | y−23 0xE7B0 ch4 | y+41 0xE7C0 ch2 | y+73 0xE7C4 ch1 |
| 4 | y−23 0xE7D8 ch3 | y+9 0xE7DC ch3 | y+57 0xE7E0 ch2 |
| 5 | y−23 0xE926 ch0 | y−23 0xE928 ch2 | y−7 0xE930 ch6 |
| 6 | y−23 0xE900 ch1 | y−23 0xE904 ch3 | y+9 0xE908 ch5 |

**Frames 7–10 — FRONT half (near/lower tiers; idle = 10) — CORRECTED: 3 sprite columns each at x −17/−1/+15:**

| frame | col @ x−17 | col @ x−1 | col @ x+15 |
|---|---|---|---|
| 7 | y−71 0xE7C8 ch6 | y+9 0xE7D0 ch3 | y+25 0xE7D4 ch2 |
| 8 | y−71 0xE7E8 ch5 | y−39 0xE7F0 ch5 | y+9 0xE7F8 ch3 |
| 9 | y−71 0xE910 ch2 | y−55 0xE918 ch4 | y−39 0xE920 ch5 |
| 10 (idle) | y−71 0xE770 ch3 | y−55 0xE780 ch5 | y−7 0xE790 ch4 |

For the right side the same frames are emitted with bit15 set: every x negated and
flipx=1. **3 segments (chained tile columns) per rope half, 6 per side.** Each rope
half is one ~48 px wide column stack; each sprite record costs one 16-byte spriteram
slot, so a full idle scene = 4 objects × 3 records = 12 records/frame.

**Frames 0–1 — ringside apron/post (not used in ring view):**

- frame 0: 4×3 slab of tiles 0xE740/41/42, cols x −25/−9/+7/+23, rows y +25/+41/+57
  — a plain ring-apron slice.
- frame 1: corner post + apron: 0xE700 ch4 @ (−57,−15), 0xE710/0xE720/0xE730 ch3 @
  (−41/−25/−9,+17), plus a 3×4 slab of 0xE740–0xE743 @ x +7/+23/+39, y +17..+65.

**Per-scene**: `0x1004A` runs for every match venue (`0xC98` is the common match
init, venue id `$1C0162`), so every in-ring scene has side ropes; the cage-match
overlay (venue 1) simply draws on top. Exceptions that borrow the slots:

- **Ringside view** (`$1C0161` bit1 set at `0xF9D6`, cleared at `0xF9F4` when
  re-entering): runner dead; instead `0xF8D8` (slot `$1C1134`) / `0xF8E4` (slot
  `$1C1164`) re-author the slot for each outside wrestler as class 14, list 0
  (frontmost), while the wrestler is put in list 1 (`0xF8EE`):
  - wrestler X in [0x290,0x3B0): frame 0 at (wrestlerX, Y=0x151, Z=0x13E) — apron
    slab tracks the wrestler;
  - X < 0x290: frame 1 at (0x269, 0x151, Z=0x146) — left corner post;
  - X ≥ 0x3B0: frame 0x8001 (frame 1 flipped) at (0x3C7, 0x151, Z=0x146) — right post.
  On re-entry `0xFCC0` calls `0x1004A` to restore the rope objects.
- **Venue-4 cinematic** (`cmpi.w #$4,$1C0162` at `0xB618`): block `0xB624–0xB80D`
  repurposes all four slots (+`$1C1194` as class 0x4E, `$1C1258` array) for intro
  graphics, re-activating `$1C1134/$1C1164` at `0xB8F0/0xB8F8`.

## 3. The rope-shake machines

The state written into the byte pairs selects the handler; writing it also wipes the
bit7 "already-initialised" latch in the state word's high byte, so a re-arm restarts
the sequence. Every handler's first tick queues **sound `0x2C`** (`jsr $2052`, at
`0x101BE / 0x10234 / 0x102AA / 0x10302`). Fields: `+0x22` word = countdown (low byte
loaded from table), `+0x24` = step index, `+0x05` = frame byte, `+0x04` high byte
re-written from `+0x2E` every step (keeps the flip bit). When the last step's timer
expires the whole state word is cleared → next tick handler 0 reloads the idle frame.

| state | handler | front table (list-1 objs) | back table (list-3 objs) | sequence (frame,ticks) |
|---|---|---|---|---|
| 0 idle | `0x10186` | — | — | one-shot: frame 2 (front) / frame 10 (back), from `+0x12` bit1 |
| 1 light | `0x101B2` | `0x10210` | `0x1021C` | front 3,4,5,6,2,4 ×6 ticks; back 7,8,10,9,10,8 ×6 (36 ticks) |
| 2 heavy | `0x10228` | `0x10286` | `0x10292` | front (3,16)(4,6)(2,6)(6,6)(2,6)(4,6); back (7,16)(8,6)(10,6)(9,6)(10,6)(8,6) (46 ticks) |
| 3 deep  | `0x1029E` | `0x102EE` (both sides use it) | n/a — never armed on back | (4,10)(5,10)(6,6) (26 ticks) |
| 4 blip  | `0x102F6` | `0x10346` | n/a | (6,14)(2,12) (26 ticks) |

"Busy" (`$1C1151` / `$1C11B1`) is just the state's low byte: nonzero from arm until
the sequence ends. Only the run-probe checks it; other arm sites overwrite freely.

Arm sites (all in `reference/maincpu.asm`):

- **State 1, busy-gated — run-into-rope probe `0x10FC6`** (run-skid-turn §2c): needs
  in-ring view (bit1 `$1C0161` clear), facing `+0x36` ∈ {1,5}; `+0x37` bit1 (left
  contact) → `0x10FF0/0x10FF8` write 1 to `$1C1150/$1C1180`; bit0 (right) →
  `0x11012/0x1101A` write `$1C11B0/$1C11E0`; carry returned = bounce. (At ringside
  the same probe instead plays sounds 0x28/0x32 at `0x1103C/0x11046` — no sprites.)
- **State 1, unconditional — `0x11058`** (`0x11060/0x11068`, `0x1107A/0x11082`): no
  caller appears in the disassembly; apparently unreferenced variant.
- **State 1, unconditional — `0x1B9AA/0x1B9B2` & `0x1B9BC/0x1B9C4`**: airborne/bump
  machine near `0x1B976` (facing 6, Z `+0xE` < 0x180, first pass plays 0x28+0x32) —
  body thrown into the ropes.
- **State 2 (heavy, one whole side) — helper `0x11D56`** (`0x11D5E/0x11D66` right,
  `0x11D70/0x11D78` left, by `+0x37` bit0), called at `0x11D0E/0x11D1A` from the
  Irish-whip/rebound machine (wrestler `+0x20` set to 8 / 6).
- **State 3 (front only) — `0x1839C`** (left if wrestler X < 0x280, else `0x183A6`
  right): rope-lean machine `0x1836A` — wrestler pressed deep into the ropes (sets
  wrestler anim `+0x12`=2, `+0x19`=0x18, recovery timer `+0x2A`=0x12).
- **State 4 (front only) — `0x19F8A`/`0x19F94`** (side by wrestler `+0x33` bit7):
  machine `0x19F1E`, re-armed on each of 6 repetitions (`+0x44`=6) while the
  animation loops — the stepping-through-the-ropes wiggle.

## 4. Other sprite scenery in the same system

- **Top/bottom ropes, posts, apron (in-ring view): NOT sprites** — they are part of
  the BG tilemaps. Only the left/right ropes (class-14 frames 2–10) are sprites,
  which is exactly why they vanish when the native engine renders tilemaps +
  wrestlers only.
- **Ringside apron/corner posts**: class-14 frames 0/1 authored per outside wrestler
  at `0xF8D8/0xF8E4` (see §2) — needed the moment the native engine renders the
  ringside camera.
- **Class-15 ringside extras**: two objects `$1C0F1C` / `$1C1028`, init `0xFFD2`
  (class 0x0F, X=0x40D / 0x330, Y=0x140 / 0x119, list 4), ticked by `0xFDEE` only
  when `$1C0161` bit1 is set (ringside). Stream: frame table `0x3C63C` (6 frames),
  base `0x68CB6`. Same emit pipeline.
- **Referee** (`$1C11F4`, machine `0x1F914`) and the venue-4 cinematic objects use
  the same `$247C/$27B8/$2836` pipeline but are separate concerns.

## 5. C sketch

```c
/* ring_hardware.c — native port of ROM 0x1004A + 0x10120 (+0x247C/0x27B8 tail).
 * ROM data to extract (add to manifest):
 *   0x38F14+0x1C  word   class-14 frame-table offset (0x3710)
 *   0x3C624       12 w   class-14 frame offsets (last 0xFFFF)
 *   0x38FB8+0x38  long   class-14 stream base (0x00068BDC)
 *   0x68BDC       0xDA b class-14 stream (frames 0..10)
 *   0x10210/0x1021C/0x10286/0x10292  6 byte-pairs each  shake tables st1/st2
 *   0x102EE 3 pairs (st3), 0x10346 2 pairs (st4)
 *   (already extracted elsewhere: bank remap $1C1610, sprite palettes ROM 0x2F22)
 */
enum { RH_LEFT_FRONT, RH_LEFT_BACK, RH_RIGHT_FRONT, RH_RIGHT_BACK };

typedef struct {            /* mirrors ROM object fields              */
    uint16_t state;         /* +0x1C: 0..4; low byte = "busy"         */
    int      latched;       /* state-word bit7 (first-tick latch)     */
    uint16_t frame;         /* +0x04 low bits (2..10, or 0/1 ringside)*/
    int      step, timer;   /* +0x24 / +0x22                          */
} RhObj;

static const struct { int16_t wx; uint8_t list; uint8_t flip; uint8_t idle; }
rh_def[4] = {               /* ROM 0x1004A */
    { 0x1C0, 1, 0, 2 }, { 0x1C0, 3, 0, 10 },
    { 0x330, 1, 1, 2 }, { 0x330, 3, 1, 10 },
};
#define RH_WY 0x178
#define RH_WZ 0x140

/* anim tables, {frame,ticks}: front variants (list 1) / back (list 3) */
static const uint8_t rh_st1_f[6][2]={{3,6},{4,6},{5,6},{6,6},{2,6},{4,6}};   /*0x10210*/
static const uint8_t rh_st1_b[6][2]={{7,6},{8,6},{10,6},{9,6},{10,6},{8,6}}; /*0x1021C*/
static const uint8_t rh_st2_f[6][2]={{3,16},{4,6},{2,6},{6,6},{2,6},{4,6}};  /*0x10286*/
static const uint8_t rh_st2_b[6][2]={{7,16},{8,6},{10,6},{9,6},{10,6},{8,6}};/*0x10292*/
static const uint8_t rh_st3[3][2]  ={{4,10},{5,10},{6,6}};                   /*0x102EE*/
static const uint8_t rh_st4[2][2]  ={{6,14},{2,12}};                         /*0x10346*/

void rh_arm(int side_right, int both, int st) {  /* ROM arm sites write state */
    RhObj *f = &rh[side_right ? RH_RIGHT_FRONT : RH_LEFT_FRONT];
    f->state = st; f->latched = 0;               /* move.w #st clears latch   */
    if (both) { RhObj *b = f + 1; b->state = st; b->latched = 0; }
}
int  rh_busy(int side_right) { return rh[side_right?2:0].state & 0xFF; }

/* one tick per 57.44Hz frame, ROM 0x10120; skip entirely at ringside
 * (mem[$1C0161] bit1) */
void rh_tick_and_emit(int16_t cam_x, int16_t cam_y) {
    for (int i = 0; i < 4; i++) {
        RhObj *o = &rh[i];
        const uint8_t (*tab)[2] = NULL; int n = 0;
        switch (o->state & 0xF) {
        case 0: if (!o->latched) { o->latched = 1; o->frame = rh_def[i].idle; }
                break;
        case 1: tab = rh_def[i].list==1 ? rh_st1_f : rh_st1_b; n = 6; goto anim;
        case 2: tab = rh_def[i].list==1 ? rh_st2_f : rh_st2_b; n = 6; goto anim;
        case 3: tab = rh_st3; n = 3; goto anim;
        case 4: tab = rh_st4; n = 2;
        anim:
            if (!o->latched) {                       /* handler first tick    */
                o->latched = 1; o->step = 0;
                wf_sound_cmd(0x2C);                  /* jsr $2052, D0=$2C     */
                o->frame = tab[0][0]; o->timer = tab[0][1];
            } else if (--o->timer == 0) {
                if (++o->step >= n) { o->state = 0; o->latched = 0; break; }
                o->frame = tab[o->step][0]; o->timer = tab[o->step][1];
            }
            break;
        }
        /* $247C + $27B8 + $2836/$D1FC tail: compile class-14 stream frame
         * into bank-14 spriteram records on draw layer rh_def[i].list.
         * screenX/Y then per-record offsets exactly as decoded in section 2;
         * clip $2188 applies (class 0x0E < 0x4E). */
        int16_t sx = rh_def[i].wx - cam_x;
        int16_t sy = (RH_WY + RH_WZ) - cam_y;
        wf_emit_class_stream(/*class*/14, o->frame | (rh_def[i].flip?0x8000:0),
                             sx, sy, /*layer*/rh_def[i].list, /*bank*/14);
    }
}
```

Notes for the implementation: (a) the emitted records must land between the wrestler
layers — back halves before, front halves after — matching the `$2836` drain order
5→0; (b) `wf_emit_class_stream` can reuse `src/stream_decode.c` (it already decodes
these exact streams — `wf_thinker_decode(14, 0x68BDC, off, flip, …)` reproduced every
frame above) or a baked table of the 9×3 records from §2; (c) the ringside frames 0/1
(`0xF8D8/0xF8E4` placement rules in §2) belong to the ringside camera phase, not
`rh_tick_and_emit`.
