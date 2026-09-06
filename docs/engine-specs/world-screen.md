# ROM 0x247C `sprite_screen_pos` — C transcription spec (world → screen)

Source of truth: `../wrestlefest-decomp/reference/maincpu.asm`
(0x247C body at asm lines 2344–2354). Cross-checked against
`docs/c-owned-machine.md` (pipeline + `wf_pose_sync_screen`),
`docs/frame-loop.md`, `src/pose.c`, `src/frame_10120.c` `helper_247c`,
`src/video.c` (rasteriser). The rough C attempts (`src/pose.c:66`
`wf_pose_sync_screen`, `helper_247c` in `frame_10120.c`/`frame_7a00.c`/
`frame_1f914.c`/`frame_8710.c`) were verified against the asm and are
**correct** — same formula derived independently below.

---

## 1. The routine itself — ROM 0x247C, exact semantics

```
00247C  movem.l D0-D2/A0,-(A7)
002480  move.w  ($18,A0),D1        ; word at +0x18
002484  andi.w  #$ff,D1            ; keep LOW byte (= memory byte +0x19, big-endian)
002488  ext.w   D1                 ; sign-extend → hotspot_x (int8)
00248A  move.w  ($1A,A0),D2
00248E  andi.w  #$ff,D2            ; low byte = memory byte +0x1B
002492  ext.w   D2                 ; hotspot_y (int8)
002494  move.w  ($6,A0),D0         ; world X
002498  sub.w   $1C17E6.l,D0       ; - camera X
00249E  move.w  D0,($14,A0)        ; screen X (raw)
0024A2  btst    #7,($2E,A0)        ; facing flag
0024A8  beq     $24AC
0024AA  neg.w   D1                 ; facing-left: mirror hotspot X
0024AC  add.w   D1,($14,A0)        ; screen X += hotspot_x
0024B0  move.w  ($A,A0),D0         ; world Y (depth axis)
0024B4  add.w   ($E,A0),D0         ; + world Z (height axis)
0024B8  sub.w   $1C17EE.l,D0       ; - camera Y
0024BE  move.w  D0,($16,A0)        ; screen Y (raw)
0024C2  add.w   D2,($16,A0)        ; screen Y += hotspot_y   (last CCR-setting op)
0024C6  movem.l (A7)+,D0-D2/A0
0024CA  rts
```

### Inputs (A0 = object base, all 16-bit big-endian words)

| field | meaning |
|---|---|
| `+0x06` | world X, pixels (integer part; `+0x08` is the .16 fraction, unused here) |
| `+0x0A` | world Y — **depth** axis (`+0x0C` fraction unused) |
| `+0x0E` | world Z — **height** axis (`+0x10` fraction unused). Ring-mat floor level is `Z = 0x140` (leaf constants of ring clip 0x2818E, see `src/floor_2818e.c` header: "Y 0x118/0x198, Z 0x140, X 0x220/0x2E0") |
| `+0x18` word, low byte only | per-anim-frame hotspot X offset, **signed int8**. Because the 68k reads a *word* at +0x18 and masks `#$FF`, the byte actually used lives at address **+0x19** |
| `+0x1A` word, low byte only | hotspot Y offset, signed int8 (byte at **+0x1B**) |
| `+0x2E` bit 7 | facing-left flag: negates hotspot X only |
| `$1C17E6` | camera X (word) |
| `$1C17EE` | camera Y (word) |

`$1C17E6`/`$1C17EE` is the object camera — **established**: 0x247C reads
exactly these two. `$1C1804`/`$1C1806` are *not* read here; they are the
scripted camera **target** used only by camera-follow mode 4 (§4). There
are no per-scene offsets inside 0x247C; per-scene behaviour enters only
through the camera value.

### Outputs

```
obj[+0x14] (screen_x) = obj[+0x06] - $1C17E6  + (bit7(+0x2E) ? -hx : +hx)
obj[+0x16] (screen_y) = obj[+0x0A] + obj[+0x0E] - $1C17EE + hy
    hx = (int8)lowbyte(word +0x18), hy = (int8)lowbyte(word +0x1A)
```

- Pure mod-65536 16-bit arithmetic. **No clamping, no wrap-masking, no
  min/max of any kind inside 0x247C.** Off-screen values stay as-is;
  culling happens downstream (0x27B8 clip via 0x2188, and per-tile in
  the 0xD1FC compiler).
- CCR: the final flag-setting instruction is `add.w D2,($16,A0)`
  (movem restore does not touch CCR), so a caller can observe N/Z/V/C/X
  of the screen-Y + hotspot add. `helper_247c` models this
  (`ccr_add16(f, y, d2)`).
- Cycle cost (per the ported helpers): 298, or 300 when the facing
  negate is taken.

### Z contribution and the Y convention

- The z/height contribution is a plain `add.w`: screen_y = y + z − camY.
  Depth (+0x0A) and height (+0x0E) collapse onto one screen axis,
  classic belt-scroller style. **Larger screen_y = higher on screen.**
- **There is no y-inversion in 0x247C and none in the 0xD1FC sprite
  compiler.** The compiler stores screen y *directly* into the sprite
  record (word 0 low byte + attr bit 1 as bit 8; the odd-looking
  `moveq #0/not.b/sub.w D2,D4` at 0xD352–0xD356 computes `0xFF − y`
  only to harvest its borrow (X flag) as y-bit-8 via `roxr.b #1,D3`
  at 0xD358; the high byte of that subtraction also lands in the high
  byte of record word 3).
- The inversion is a **property of the sprite hardware/rasteriser**:
  MAME `ddragon3_v.cpp` / `src/video.c:draw_native_sprite_command`:
  `ypos = (256 - compiled_y) & 0x1FF; ypos -= 16;` then each 16×16
  chain tile draws at `ypos - 16*count - 8 + py`. So there is no
  "spriteram y = 256 − …" computed in software anywhere; visible
  screen_y range is roughly −8..240 measured **up from the screen
  bottom** (feet of a ring-standing wrestler sit around screen_y = 0x50,
  see §4 equilibrium).

### What 0xD1FC does with +0x14/+0x16 (for completeness)

At 0xD220–0xD236 the compiler seeds its per-object frame:
`base_x = obj[+0x14] + 0x10`, `base_y = obj[+0x16] + 0x90` (guard-band
bias). Per tile (0xD314–0xD348): `tx = base_x + int8(list) (+adj, negated
on flip)`; cull if `(unsigned)tx > 0x15F`; `ty = base_y + int8(list)
(+adj)`; cull if `(unsigned)ty > 0x187`; then `tx -= 0x10; ty -= 0x90`
and emit. So the guard window is x ∈ [−16, 335], y ∈ [−144, 247] in
true screen coords.

---

## 2. Which objects, which callers, frame order

Objects live in 9 slots pointed to by the ROM table at **0x5122**:
`$1C05B0 $1C06BC $1C07C8 $1C08D4 $1C09E0 $1C0AEC $1C0BF8 $1C0D04
$1C0E10` (stride 0x10C). Iterator **0x250E** returns the next slot whose
byte +0x00 has bit 7 set (active), carry set after index 8.

**Main per-frame caller — PC 0xF54C**, inside helper 0xF518, called per
active object from the frame-list entry **0xF4C2** (both even and odd
lists). Per-object order at 0xF518:

```
0xF522  jsr 0x280DC     ; floor probe (only if +0x01 != 0)
0xF52E+ add +0x38/+0x3A/+0x3C into +0x06/+0x0A/+0x0E   ; pushback/clip deltas (if +0x37)
0xF546  jsr 0x1C03E     ; anim table pick + tick (sets sprite frame & hotspots)
0xF54C  jsr 0x247C      ; ← world → screen
0xF552  jsr 0x27B8      ; insert into priority draw list (clips via virtual sx/sy)
0xF558  jsr 0x2208      ; apply_motion — velocity integrates AFTER 0x247C
```

So relative order per object: **0x247C runs after this frame's anim tick
and pushback but before 0x2208**; motion applied by 0x2208 becomes
visible in the *next* frame's 0x247C. The sprite compiler pass **0x2836
(→ 0xD1FC per record)** sits near the end of both frame lists
(`docs/frame-loop.md` even/odd lists), i.e. camera update (0x26936,
0x2983C — early in the list) → object pass 0xF4C2 (0x247C) → compiler
0x2836/0xD1FC. All in the same frame, so 0x247C always sees the
already-updated, already-clamped camera.

Other callers of 0x247C (all secondary):

- 0x1F914 referee state machine ($1C11F4 object) — nested per
  `docs/frame-loop.md`.
- 0xFDEE — extra objects `$1C0F1C`/`$1C1028` (table 0x5146, iterator
  0x2540), nests 0x2208/0x280DC/0x247C.
- Scene/one-shot spawners: bsr at 0x1D0C, 0x7B4C; jsr at 0x5CFA,
  0x7CAA, 0x7DC8, 0x7E38, 0x7EFC, 0x8838, 0xA6C0, 0xA6E8, 0xA708.

---

## 3. Camera update — who writes $1C17E6/$1C17EE

Both frame lists run, early and in this order (every frame):

1. **0x26936 — camera follow** (even list position 5, odd position 3)
2. **0x2983C — camera clamp to scene limits**, table 0x298B4

plus event-driven **screen shake** 0x27012/0x27064.

### 3a. Camera follow — ROM 0x26936, exact rule

Not a midpoint snap and not a lerp: it is a **centroid-error servo with
a ±4 px/frame slew limit**.

```
D3 = $1C17E6 + 0xA0          ; camX + 160 = world X currently at screen centre
D4 = $1C17EE - 0xF0          ; camY − 240
D1 = D2 = 0; D5 = 0          ; error sums, count
for each active object (iterator 0x250E over table 0x5122):
    mode = obj[+0x4B]:
      2 → rts                       ; camera frozen this frame
      3 → skip object
      4 → D1 = $1C1804 - $1C17E6;   ; scripted target mode
          D2 = $1C1806 - $1C17EE; goto STEP (no divide)
      1 → SOLO
      0 → if !(obj[+0x33] & 0x01) skip      ; not camera-relevant
          D5++
          D1 += obj[+0x06] - D3
          D2 += obj[+0x0A] - D4             ; NOTE: no z here
          if ($1C0161 bit1) && (obj[+0x33] & 0x04) → SOLO  ; focused object
SOLO:                                        ; (0x269AE)
    D7 = ($1C0161 bit1) ? 0x100 : 0x140
    D5 = 1
    D1 = obj[+0x06] - D3
    D2 = obj[+0x0A] + obj[+0x0E] - D7 - D4   ; z included in solo mode
    fall through to DONE
DONE (0x269D4):
    if D5 == 0 → rts
    D1 = (int32)D1 / D5   (divs)             ; signed average error
    D2 = (int32)D2 / D5
STEP (0x269FC):
    clamp D1 to [-4, +4];  $1C17E6 += D1;  $1C17EA += D1
    clamp D2 to [-4, +4];  $1C17EE += D2;  $1C17F2 += D2
    rts
```

Equilibria (what the constants mean):

- X: settles when avg(obj X) = camX + 160 → the wrestlers' centroid sits
  at screen centre (screen_x = 160).
- Y (normal): settles when avg(obj Y-depth) = camY − 240. With the ring
  mat at Z = 0x140, a standing wrestler's screen_y = y + z − camY =
  0x140 − 0xF0 = **0x50** (feet ~80 px up from the bottom).
- Y (solo): settles at screen_y(incl. z) = D7 − 0xF0 = 0x10 (game-type
  flag $1C0161 bit1 set) or 0x50 (clear).

`$1C17EA`/`$1C17F2` are the second scroll-layer copy of the camera
(pair select by `$1C17FE` bit 7 at 0x26A7A → staging `$1C17F6/$1C17FA`
→ `>>5` tile scroll at 0x26ADA; camX is also written to the hardware
scroll register at 0x26E28 `move.w $1C17E6,$100000`). The follow and
clamp routines always mirror the same delta/limit into both pairs.

### 3b. Camera clamp — ROM 0x2983C, table 0x298B4

```
0x2983C  lea 0x298B4,A0
0x29842  D0 = $1C007E (scene index) << 3      ; 8-byte entries
         entry: { Xmin, Ymin, Xmax, Ymax }    ; words, in that order
         if camX < Xmin (unsigned):  camX = Xmin;  $1C17EA = Xmin
         else if camX >= Xmax:       camX = Xmax;  $1C17EA = Xmax
         if camY < Ymin (unsigned):  camY = Ymin;  $1C17F2 = Ymin
         else if camY >= Ymax:       camY = Ymax;  $1C17F2 = Ymax
```

Table 0x298B4, 7 entries (index = `$1C007E`):

| scene | Xmin | Ymin | Xmax | Ymax |
|---|---|---|---|---|
| 0 | 0x0140 | 0x0200 | 0x0280 | 0x02C0 |
| 1 | 0x0140 | 0x0200 | 0x0280 | 0x02C0 |
| 2 | 0x0140 | 0x0200 | 0x03C0 | 0x0200 |
| 3 | 0x0140 | 0x0200 | 0x0780 | 0x0700 |
| 4 | 0x0140 | 0x0200 | 0x0780 | 0x0700 |
| 5 | 0x0140 | 0x0200 | 0x0280 | 0x02C0 |
| 6 | 0x0140 | 0x0200 | 0x03C0 | 0x0200 |

### 3c. Screen shake — 0x27012 (Y) / 0x27064 (X)

While countdown `$1C1800` (Y) / `$1C1802` (X) is nonzero, add
`shake_tab[count]` to camY(+$1C17F2) / camX(+$1C17EA) and decrement.
Signed word table at **0x27048** indexed by the *remaining* count:
`{ +1,-2,+3,-4,+5,-6,+7,-8,+9,-10,+12,-13,+14,-15,+16 }` (index 1..15;
index 0 never used since count==0 exits). Alternating signs make it net
out as the counter runs down.

### 3d. Scripted target `$1C1804`/`$1C1806`

Set as per-scene constants by scene setup code (e.g. 0x76A: 0x280/0x600;
0xCC4: 0x1E0/0x230; 0x126E: 0x140/0x600; 0x5300: 0x140/0x200; …,
16 sites). Consumed only by follow mode 4 above (still slew-limited to
±4/frame and still clamped by 0x2983C).

---

## 4. C sketches (integer-only)

```c
/* ROM 0x247C — world → screen. obj = object base in work RAM.
 * rd16/wr16 big-endian words; CAM_X=$1C17E6, CAM_Y=$1C17EE. */
void sprite_screen_pos(uint32_t obj)
{
    int16_t hx = (int8_t)(rd16(obj + 0x18) & 0xFF);  /* byte +0x19 */
    int16_t hy = (int8_t)(rd16(obj + 0x1A) & 0xFF);  /* byte +0x1B */
    uint16_t sx = (uint16_t)(rd16(obj + 0x06) - rd16(CAM_X));
    uint16_t sy;

    if (rd8(obj + 0x2E) & 0x80)                       /* facing left */
        hx = (int16_t)-hx;
    wr16(obj + 0x14, (uint16_t)(sx + (uint16_t)hx));

    sy = (uint16_t)(rd16(obj + 0x0A) + rd16(obj + 0x0E) - rd16(CAM_Y));
    wr16(obj + 0x16, (uint16_t)(sy + (uint16_t)hy));
    /* CCR (if a caller cares) = flags of the final 16-bit add. */
}

/* ROM 0x26936 + 0x2983C — per-frame camera update (run both, in order,
 * every frame, before the object pass). */
static int16_t clamp4(int32_t v)
{
    return (int16_t)(v > 4 ? 4 : (v < -4 ? -4 : v));
}

void camera_follow_26936(void)             /* ROM 0x26936 */
{
    int32_t ex = 0, ey = 0;                /* D1, D2 */
    int cnt = 0, target = 0;               /* D5, mode-4 flag */
    uint16_t ax = (uint16_t)(rd16(CAM_X) + 0xA0);   /* D3 */
    uint16_t ay = (uint16_t)(rd16(CAM_Y) - 0xF0);   /* D4 */
    int focus_flag = (rd8(W(0x1C0161)) >> 1) & 1;

    for (int i = 0; i < 9; i++) {          /* iterator 0x250E */
        uint32_t o = SLOT_TABLE_5122[i];   /* $1C05B0 .. $1C0E10 */
        if (!(rd8(o + 0x00) & 0x80)) continue;
        uint8_t mode = rd8(o + 0x4B);
        if (mode == 2) return;             /* camera frozen */
        if (mode == 3) continue;
        if (mode == 4) {                   /* scripted target */
            ex = (int16_t)(rd16(W(0x1C1804)) - rd16(CAM_X));
            ey = (int16_t)(rd16(W(0x1C1806)) - rd16(CAM_Y));
            target = 1; break;             /* bra STEP: no divide */
        }
        if (mode == 1 ||
            ((rd8(o + 0x33) & 0x01) && focus_flag &&
             (rd8(o + 0x33) & 0x04))) {    /* SOLO (0x269AE) */
            int16_t d7 = focus_flag ? 0x100 : 0x140;
            cnt = 1;
            ex = (int16_t)(rd16(o + 0x06) - ax);
            ey = (int16_t)(rd16(o + 0x0A) + rd16(o + 0x0E) - d7 - ay);
            break;                          /* falls to DONE */
        }
        if (!(rd8(o + 0x33) & 0x01)) continue;   /* mode 0 */
        cnt++;
        ex += (int16_t)(rd16(o + 0x06) - ax);
        ey += (int16_t)(rd16(o + 0x0A) - ay);    /* no z in group mode */
    }
    if (!target) {
        if (cnt == 0) return;
        ex /= cnt;  ey /= cnt;             /* divs.w */
    }
    /* STEP 0x269FC: slew-limit ±4, mirror into both layer pairs */
    int16_t dx = clamp4(ex), dy = clamp4(ey);
    wr16(CAM_X,  (uint16_t)(rd16(CAM_X)  + dx));
    wr16(CAM_XB, (uint16_t)(rd16(CAM_XB) + dx));   /* $1C17EA */
    wr16(CAM_Y,  (uint16_t)(rd16(CAM_Y)  + dy));
    wr16(CAM_YB, (uint16_t)(rd16(CAM_YB) + dy));   /* $1C17F2 */
}

void camera_clamp_2983c(void)              /* ROM 0x2983C, table 0x298B4 */
{
    static const uint16_t lim[7][4] = {    /* Xmin, Ymin, Xmax, Ymax */
        {0x140,0x200,0x280,0x2C0}, {0x140,0x200,0x280,0x2C0},
        {0x140,0x200,0x3C0,0x200}, {0x140,0x200,0x780,0x700},
        {0x140,0x200,0x780,0x700}, {0x140,0x200,0x280,0x2C0},
        {0x140,0x200,0x3C0,0x200},
    };
    const uint16_t *e = lim[rd16(W(0x1C007E))];
    uint16_t cx = rd16(CAM_X), cy = rd16(CAM_Y);
    if (cx < e[0])       { wr16(CAM_X, e[0]); wr16(CAM_XB, e[0]); }
    else if (cx >= e[2]) { wr16(CAM_X, e[2]); wr16(CAM_XB, e[2]); }
    if (cy < e[1])       { wr16(CAM_Y, e[1]); wr16(CAM_YB, e[1]); }
    else if (cy >= e[3]) { wr16(CAM_Y, e[3]); wr16(CAM_YB, e[3]); }
}
```

Faithfulness notes:
- All comparisons in the clamp are **unsigned** (`bcc`/`bcs` after
  `cmp.w`), matching the asm.
- The follow routine's iterator order is slot 0..8; the SOLO/target
  breaks abandon the rest of the list exactly as the ROM does.
- Mode 4's error is applied **without** dividing (bra 0x269FC skips
  the divs), still slew-limited to ±4.
- Do not add wrapping/masking to `sprite_screen_pos` — downstream code
  depends on full 16-bit values (0xD1FC re-biases by +0x10/+0x90 and
  culls per tile; the 9-bit truncation happens only in the emitted
  sprite record: y = word0 low byte + attr bit1, x = word5 low byte +
  attr bit2).
