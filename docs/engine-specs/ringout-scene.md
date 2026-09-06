# Ring-out camera scene: switch 0xF98C, count-out, floors, moves 0x69/0x6A/0x6B

Source `reference/maincpu.asm` (read-only, 2026-08-22). Engine names: x/y/z = +0x06/+0x0A/+0x0E
(16.16, word part quoted), `state` +0x20, `move_id` +0x60, `react_id` +0x64, `f32` +0x32,
`f33` +0x33, `mover` +0x01, `zone` +0x36, `clip` +0x37, pushbacks +0x38/+0x3A/+0x3C,
`floor42` +0x42, `scene` = `$1C007E` (low byte `$1C007F`), `cam_x/cam_y` = `$1C17E6/$1C17EE`,
scroll target `$1C1804/$1C1806`, stage = `$1C0162`.

Caller: `0xF564 bsr 0xF98C` inside the pass-2 wrapper `0xF560` (after the pair-collide loop,
before `0xFD00`) — once per frame, every frame (frame-order.md row 7).

---

## 0. `$1C1678` trigger word

| value | writer | meaning |
|---|---|---|
| `0x8000` + `$1C1679 = faller +0x2E` (facing byte, bit7 = left) | 0x198A0 (move 0x68 phase 0, landed, not rumble, legal `+0x33` b0) | one legal man landed outside → **enter** ring-out scene |
| `0xC000` | 0x19B2E (end of climb-in 0x69 while ringside scene showing, when 2 legal men are "in": scan 0x19B04 counts slots with b0 legal AND (`+0x32` b1 set OR `+0x33` b2 clear)) | both legal men back inside → **leave** ring-out scene |
| `0` | 0xFBB4 (after entry), 0xFBC8 (on exit) | idle |

0xF98C tests bit7 only as the gate, then bit6 picks the direction. So `0xC000` = "bit7 fire +
bit6 return"; `0x8000` = enter. No "both men out" encoding lives here — "both out" is decided
by the referee SM3 from `+0x33` b2 of the two legal men (§2). TODO EXACT 0x19B14: `+0x32` b1
counts as "in" — bit meaning not identified (probably "partner/apron"?).

## 1. 0xF98C — scene switch, PC by PC

```
F98C  if !($1C1678 b7) → rts (0xFCFE)
F998  clr.l $1C1670 ; clr.l $1C1674                    ; TODO EXACT (sprite-list/HUD latches)
F9A4  bclr #7,$1C0076                                   ; frame IRQ body disabled (0x842 → rte)
F9AC  jsr 0x1F6C   ; clear tilemap work RAM $1C16D2..$1C19AE, zero FG/BG VRAM $80000/$82000 (0x400 longs each)
F9B2  jsr 0x1FDE   ; clear $1C19B2..$1C1CD0 and $C2000 (0x400 longs), clr.w $140008
F9B8  if $1C1678 b6 → F9E0 (return path)
--- enter ---
F9C2  $1C007F = 0xFA00[$1C0162]      ; scene byte by stage
F9D6  bset #1,$1C0161                ; "ringside scene showing"
F9DE  bra FA14
--- leave ---
F9E0  $1C007F = 0xFA0A[$1C0162]
F9F4  bclr #1,$1C0161
F9FC  bra FBC8
```

Tables (bytes, index = stage 0..9):

```
FA00  02 06 00 06 02 06 02 02 06 02     ; ring-out scene per stage  (scene 2 or 6; stage 2 → 0 !)
FA0A  00 05 00 05 00 05 00 00 05 00     ; return scene per stage    (0 or 5)
```
(Stages whose return is 5 use ring-out 6; stages returning to 0 use 2. Stage 2 maps 0→0 — a
ring-out on stage 2 just re-composes scene 0; TODO EXACT whether stage 2 is the cage.)

### 1a. Enter: objects (0xFA14-0xFB56)

```
FA14  $1C0F1C: z=0x100, +0x1C=3        ; extra object A (manager/valet slot, table 0x5146)
FA26  $1C1028: z=0x100, +0x1C=3        ; extra object B
FA38  D0=0; loop 0x250E over wrestler slots (carry = end → FB5A):
FA44    +0x18=0 +0x1A=0 +0x12=0 +0xAE=0 +0x34=0 +0x74=0 ; +0x26.l=0 ; +0x9C.l=0 ; +0x4C=0 +0x4A=0
FA6C    if +0x56 b7 (CPU) : +0xB4=0
FA78    if f33 b2 && move_id==0x6A:                       ; THE FALLER (already in 0x6A from 0x198B0)
FA8A        $1C1806=0x200 ; $1C1804 = ($1C1679 b7) ? 0x350 : 0x190   ; scene-start camera
FAAE        state=5, move_id=0x6A (re-init: +0x1C b7 was cleared at FA44 so 0x6A runs its init again)
FABA        bset #5,f32                                    ; probe-exempt
FAC0        +0x68 = 8                                      ; 8 damage for the fall
        elif !(f33 b0) (NON-LEGAL man):                    ; FACA
FAD2        f32 &= 0x00C3 (word!) ; bclr #6,+0xC6 ; bset #2,f33 (outside) ; bset #5,f32
FAEA        state=5, move_id=0x6B, y=0x164, z=0x100
FB02        f33 b7 clear → x=0x200, bset #7,+0x2E (face left)  else x=0x428, bclr #7,+0x2E (face right)
        else (the OTHER LEGAL man, still inside):          ; FB2A
FB2A        f32 &= 0x00C3 ; state=0 ; y=0x160
FB3C        x = ($1C1679 b7) ? 0x3A0 : 0x2A0               ; stands on the faller's side of the ring
```
Note `f33 b7` (FB02) = team side bit (TODO EXACT name; same bit used at 0xFC4A/0x19BDC).
Positions in the ring-out scene are in the same world units but the ring is drawn differently
(§3): mat y-line 0x160 (FB36, FC70), floor z = 0x100, ringside y band 0x110..0x140.

### 1b. Enter: rebuild (0xFB5A-0xFBC4)

```
FB5A  wait $140026 b2 set (vblank)
FB64  jsr 0x26E66                      ; full tilemap compose for scene $1C007E (cam regs := $1C1804/6)
FB6A  $1C15F4 = $1C15F8 = $1C007E       ; palette-set selectors
FB7E  jsr 0x2A06                        ; palette load: 0x2A98[$1C15F4]→$188000, 0x2AB4[..]→$186000, 0x2B82+$1C15FC*64
FB84  jsr 0xC1EA                        ; tag palette copy → $180100 (16 words)
FB8A  wait $140026 b2 clear
FB94  referee $1C11F4: x=0x340, y=0x160, state(+0x20)=0x8003   ; SM3 + visual 3 latch (0x1FAF0)
FBAC  bset #7,$1C0076 ; clr.w $1C1678
FBBA  0x2052(0x80)                      ; YM command 0x80 (music change)
FBC4  rts
```

### 1c. Leave (0xFBC8-0xFCFE)

```
FBC8  clr.w $1C1678 ; $1C1804=0x1E0 ; $1C1806=0x230     ; scene-start camera for the ring scene
FBDE  D0=0; loop 0x250E (carry → FC80):
FBEA    +0x18=+0x1A=+0x12=+0xAE=0 ; f32 &= 0x00C3 ; bclr #6,+0xC6 ; +0x34=+0x74=0 ; +0x26.l=+0x9C.l=0 ; +0x4C=+0x4A=0
FC1E    CPU (+0x56 b7): +0xB4=0
FC2A    legal (f33 b0): state=0, +0xAE=0, y=0x150, z=0x140, x = (f33 b7) ? 0x2C0 : 0x240
        non-legal:       state=1, +0xAE=1, y=0x160, z=0x140 (x untouched — he is still at his 0x6B corner spot; TODO EXACT whether state 1 = walk-to-corner apron return)
FC80  wait vblank ; jsr 0x26E66 ; $1C15F4=$1C15F8=$1C007E ; jsr 0x2A06 ; jsr 0xC1EA ; wait !vblank
FCBA  jsr 0x206FE                       ; wipe the FG0 count digits ($C0744.. 4 rows)
FCC0  jsr 0x1004A                       ; re-create ring hardware objects (ropes/posts)
FCC6  referee: x=0x280, y=0x198, state=0x8000, +0x34=0, +0x12=0
FCE6  0x2052(0x20)                      ; YM 0x20 (music back)
FCF0  clr.w $1C15D4 ; bset #7,$1C0076
FCFE  rts
```
So positions are NOT restored: both legal men are re-placed at (0x240|0x2C0, 0x150, z 0x140)
(in-ring, just inside the bottom rope line 0x198? no — y 0x150 is mid-ring), referee to the
bottom-rope centre (0x280,0x198) in SM0.

### 1d. Camera in the ring-out scene

- Compose 0x26E66 copies `$1C1804/$1C1806` into the cam staging (`$1C17F6/FA`, 0x26EA6) — the
  scene starts at cam (0x350|0x190, 0x200).
- Per frame the servo 0x26936 still runs. In group mode (`+0x4B == 0`) an object with
  `$1C0161 b1 && f33 b2` (the outside man) forces SOLO with D7=0x100:
  `err_x = x - cam_x - 0xA0`, `err_y = y + z - 0x100 - cam_y + 0xF0`, ±4/frame. Nobody sets
  `+0x4B = 4` for this scene (grep: only 0x26936 reads +0x4B). So the camera follows the
  faller, not the scroll target, after the first frame.
- Clamp 0x2983C row 2 / row 6 (identical): `Xmin 0x140, Ymin 0x200, Xmax 0x3C0, Ymax 0x200`
  → **y is pinned at 0x200**; x pans 0x140..0x3C0. The engine's `ref_camera_limits` table
  already carries rows 0-6, so `engine/camera.c` needs only the solo branch (missing today)
  and `st->scene` = 2/6.

## 2. Count-out machine

### 2a. Referee SM3 / visual 3 — 0x1FD5E (runs from 0x1F914 each frame while `+0x20 = $8003`)

```
1FD5E  first entry (bset #7,+0x1C was clear):
         clr.w $1C169A ; +0x24=0 ; +0x54=0x3165 (next YM warning id) ; +0x23=8 ; +0x05=9 (pose) ; +0x04=+0x2E
1FD88  jsr 0x2208 (apply_motion; mover as left by previous state — TODO EXACT, likely 0 after the teleport)
1FD8E  x clamped to [0x298, 0x390]
1FDAC  if --+0x22 != 0 → 1FDE6          ; +0x22 is the pose timer (initial value: whatever it was; 0x1000 set at 1FE5A freezes it)
1FDB4    +0x23=8 ; pose: if +0x05==0xC → +0x05=9 else +0x05++ ; if it became 0xC → +0x23 += 4 ; +0x04=+0x2E
1FDE6  if $1C169B != 0x14 → bsr 0x1FDF4 (tick) ; rts
```
Poses 9..0xC cycle (the "count with the arm" loop, 9→A→B→C, extra 4 frames on C).
TODO EXACT 0x1FDAC: the timer decremented is `+0x22` (word) but reloaded via `+0x23` (byte
of the same word) — i.e. reload writes the low byte only; 8 or 12 frames per pose.

Tick 0x1FDF4:
```
1FDF4  +0x24++ ; if +0x24 < 0x50 → rts                  ; 80 frames per count (~1.39 s @57.44)
1FE00  +0x24=0 ; $1C169A++ (word; $1C169B = count 1..20)
1FE0A  if count == 20:
1FE14     0x204FA → A2,A3 = first two live slots with f33 b0 (legal men)
1FE18     if neither has f33 b2 → rts                  ; NOBODY OUTSIDE: park at 20, no blit, no result
1FE2A  0x2067C(D0 = $1C169A)                             ; digit blit (see 2b) — D0 bit15 is NEVER set here
1FE36  if count >= 0x11: 0x2052(+0x54) ; +0x54++         ; YM 0x3165,0x3166,0x3167,0x3168 at 17,18,19,20
1FE4E  if count != 20 → rts
--- result (1FE5A) ---
1FE5A  +0x22 = 0x1000                                     ; pose timer effectively frozen
1FE60  0x204FA → A2 (first legal), A3 (second legal)
1FE64  A2 out && A3 out  → FE76: +0xFE = 0x8005 on A2, A2->partner(+0x86), A3, A3->partner
                                  0x2052(0x3108) ; clr $1C169E ; 0x2503C(0x52) ; rts     ; DOUBLE COUNT-OUT / draw chrome 0x52
1FE64  A2 out, A3 in    → FEB2: A2 pair +0xFE=0x8002, A3 pair +0xFE=0x8003 ; 0x90D6 ; clr $1C169E
                                  A3 CPU (+0x56 b7)? 0x20156 jingle : 0x2052(0x3108)+0x2503C(0x52) ; rts
1FE64  A2 in,  A3 out   → FF02: A3 pair 0x8002, A2 pair 0x8003 ; 0x90D6 ; clr $1C169E ; A2 CPU? jingle : YM 3108 + blit 0x52
```
**Corrected 2026-08-23** (the A2/A3 roles above were transposed): 0x1FE64 `btst #2,(0x33,A2)`
branches to 0x1FEB2 when A2 is **inside**, so at 0x1FEB2 A2 = inside gets **0x8002** and
A3 = outside gets **0x8003**; 0x1FF02 is the mirror (A3 inside 0x8002, A2 outside 0x8003).
The **counted-out pair is always 0x8003** — `+0xFF` b0 set, which 0x11598 turns into the
lose pose 0x8C at 0x115C4; 0x8002 (b1) makes 0x115AE return, so the surviving pair just
stands. This agrees with hud-rules.md. The jingle test `btst #7,(0x56,A3)` at 0x1FEDE (and
`(0x56,A2)` at 0x1FF2E) is on the **outside/losing** man: CPU loser -> jingle 0x20156,
human loser -> YM 0x3108 + chrome blit 0x52. Verified on the oracle (`ringout20.scn`
f4144: slot 0 outside `+0xFE=8003` -> move 0x8C, slot 4 inside `+0xFE=8002`).

Once `$1C169B == 20` and the result is stamped, 0x1FDE6 stops calling the tick: the referee
stays in SM3 counting pose forever (the match-end machinery takes over via `+0xFE`).

### 2b. Digit blit 0x2067C (D0 = value)

```
20680  if D0 b15: bclr, dest A2=$C1188 (attract/other caller 0x142C only)
20692  else dest = (D0 < 10) ? $C0748 : $C0744            ; FG0 text layer, one row per line, stride 0x100
206A4  D1 = D0*4 ; A4 = (0x2073A)[D1] ; D3 = 0            ; glyph pointer
206BA  4 rows x 2 cells: cell = {byte (A4,D3)++, attr 0xEA} at +0,+4 ; A3 += 0x100 per row   (D3 ends at 8)
206E6  if D0 >= 10: A3 = A2 + 8 ; moveq #1,D0 ; bra 206BA  ; RE-ENTERS THE ROW LOOP
```
**The second pass is NOT the digit "1".** `bra $206BA` jumps back *past* the 0x206B4 table
lookup, so A4 still points at glyph[D0] and D3 is still 8 — it blits **bytes 8..15 of the
same glyph** two cells to the right. `moveq #1,D0` only makes the 0x206E6 `cmpi.b #$a,D0`
fail so the routine ends after the second pass. Consistent with the table: entries
0x2073A[0..9] are 8 bytes apart (one-digit glyphs, 4 rows x 2 cells) while entries
0x2073A[0x0A..0x14] are **16** bytes apart (two-digit glyphs, 4 rows x 4 cells: low 8 bytes
= the tens pair of cells, high 8 = the ones pair). e.g. 0x207DE (value 10) = `69 6A 79 7A
89 8A 99 9A` ("1") + `67 68 77 78 87 88 97 98` ("0"); 0x2087E (value 20) = "2" + "0".

Reading the second pass as "blit glyph 1" makes every value 10..19 render as **11** and 20
render as **21** — exactly the two symptoms reported against the engine on 2026-08-23.
Count-out uses the plain `$1C169A` word → same destination as the pin count ($C0748/$C0744),
not $C1188. Digit tile bytes: `data/romdata/ref_count_digit_tiles.json` (pins-referee.md).
0x206FE wipes 4 rows × 16 bytes at $C0744.

### 2c. Cancel / getting back in

- A man re-entering runs 0x68 phases 1-3 → 0x69 (cell 0x199FC). At frame 4 (0x19A90) `bclr #2,f33`;
  at FE (0x19AF0, ringside scene): `z=0x140, y=0x160`, then the 0x19B04 scan; 2 "in" legal men →
  `$1C1678 = 0xC000` → next 0xF98C leaves the scene (§1c), and the referee's `+0x20=$8000`
  resets him (`$1C169A` is next cleared by the next SM3 entry at 0x1FD66 or a pin 0x2002C).
- Nothing else clears `$1C169A` mid-count: while the scene is showing the count keeps ticking
  regardless of who is in. At 20, if both legal men now have b2 clear the tick just returns
  (park) — the result only fires if someone is still outside at the 20th tick.
- AI forced return at 10: 0x1C6DC (`$1C169B >= 0x0A`): `bset #5,+0xB5; move_id=0x69;
  (+0xBE,+0xC0)=(0x310,0x138); state=1; +0xAE=0x0A`. 0x1D74A (AI ringside, y >= 0xE0): angle
  +0x2D=0xC0; if x < 0x279 → x=0x279, angle 0; if y >= 0x100 → y=0x100, CPU: bset #7,+0xB6;
  bclr #6,+0x56; +0x4A=0; state=5, move_id=0x69.

## 3. Floor handlers, scene 2/6 (dispatch 0x28154 slot [scene*8 + (f33 b2 ? 4 : 0)])

Inputs D0 = x+lookahead, D1 = y+clip_h, D2 = z+floor42. All pushbacks banked in +0x38/+0x3A/+0x3C
and applied next pass (0xF52E), like 0x2818E.

### 3a. 0x28480 — "inside" (f33 b2 clear) = the apron/mat line in the ring-out view

| PC | condition | writes |
|---|---|---|
| 28484-2849C | `D1 != 0x160` | `+0x3A = 0x160 - D1`, zone=1, clip b2 (**y pinned to exactly 0x160** — a 1-D rail, both directions) |
| 284A2-284BC | `D2 < 0x140` | clip **b1** (sic), `+0x3C = (0x140 - D2) + floor42` (mat z 0x140) |
| 284C0-284E0 | `D0 < 0x270` | `+0x38 = 0x270 - D0`, clip b1, zone=1 ; rts |
| 284E4-28502 | `D0 > 0x3D0` | `+0x38 = 0x3D0 - D0`, clip b0, zone=1 ; rts |
| 28506-28512 | `0x2B0 <= D0 < 0x390` | zone=4 ("in front of the ring" band) |

No perspective rows: the ring in this view is the x box [0x270, 0x3D0] on the single line y=0x160.
Note no landed bit (b4) is ever set by this handler — an inside man in this scene never reads
"landed"; TODO EXACT whether any state in scene 2 relies on clip b4.

### 3b. 0x2851E — "outside" (f33 b2 set) = ringside floor in the ring-out view

| PC | condition | writes |
|---|---|---|
| 28522-2853A | `D1 < 0x110` | `+0x3A = 0x110 - D1`, clip b3, zone=1 (top of the walkway) |
| 28540-28558 | `D1 > 0x140` | `+0x3A = 0x140 - D1`, clip b2, zone=1 (ring-skirt line) |
| 2855E-2856A | `0x294 <= D0 < 0x38C` | zone=4 (in front of the ring) — only evaluated when D1 > 0x140 path taken?  **No**: 28544 `bls 28570` skips the zone-4 test when D1 <= 0x140; i.e. zone 4 is set only while being pushed up from below 0x140. TODO EXACT intent |
| 28570-2858A | `D2 < 0x100` | clip b4 (landed), `+0x3C = (0x100 - D2) + floor42` (floor z 0x100) |
| 2858E-285AE | `D0 < 0x1C8` | `+0x38 = 0x1C8 - D0`, clip b1, **zone=3** (barrier) ; rts |
| 285B2-285D0 | `D0 > 0x470` | `+0x38 = 0x470 - D0`, clip b0, **zone=3** ; rts |

Geometry of the ring-out view: walkway y ∈ [0x110,0x140], floor z=0x100, barriers at x=0x1C8
and x=0x470 (straight walls, no trapezoid), ring front spans x 0x294..0x38C (zone 4).

## 4. Moves

### 4a. 0x6A — ringside land (cell 0x19B3C: `{0x19B50, mode 1, n 3, dur 0x40,0x10,0x08, spr 0x13,0x68,0x00}`)

| PC | writes |
|---|---|
| 0x19B58 init | `mover=0`, `+0x19 = 0xD0` (sprite x-offset), `z = 0x100`; `x < 0x280` → `(x,y)=(0x1F0,0x168)` else `(0x438,0x168)` |
| 0x19B8C | frame 0 expiry (`+0x25==0 && +0x23==0`): `bset #7,+0x18` (hold flag — TODO EXACT) |
| 0x19B9E FE | `state=5, move_id=0x6B`, `bchg #7,+0x2E` (turn around) |

Entered twice: at 0x198B0 (landing, before the scene exists — x/y already the lying spot) and
re-initialised by 0xFAAE after the scene compose (the +0x1C b7 latch was cleared at 0xFA44, so
0x19B58 runs again and teleports him to (0x1F0|0x438, 0x168, z 0x100) in the new view).

### 4b. 0x6B — walk to the corner spot (cell 0x19BBA: `{0x19BC0, mode 0}`)

| PC | writes |
|---|---|
| 0x19BC8 init | `mover=1`, `0x1174C` (walk speed by id) |
| 0x19BD4 | target: legal (f33 b0) → by x (`x >= 0x280` → `(0x42C,0x120)` else `(0x210,0x120)`); non-legal → by team bit f33 b7 (set → 0x42C side, clear → 0x210) |
| 0x19C0A each frame | `0x11710` (heading to +0xBE/+0xC0) + `0x1F15C` (arrive test, |dx|<8 && |dy|<8) |
| 0x19C1A arrive | `state=0`, `bclr #5,f32` (probe-exempt off) |

### 4c. 0x69 — climb in, differences while `$1C0161 b1`

| PC | not ringside | ringside scene |
|---|---|---|
| 0x19A28 init | `mover=0`, `+0x1B=0x57`, queue byte 1 → `$1C1808[$1C1828++]` (rope-part tile) | `mover=0`, `+0x1B=0x57`, **`y=0x150, z=0xF0`**; if `+0x74` b7: partner(+0x76) `+0x1C = 2`, `+0x74=0` |
| 0x19A82 frame 4 | `bclr #2,f33` | same |
| 0x19A96 FE | `bset #7,+0x18; state=0; $1C1697=1; +0xAE=0; +0x04=+0x2E; z=0x140, y=0x118`; queue byte 2 | same prefix; **`z=0x140, y=0x160`**; scan → `$1C1678=0xC000` if 2 legal men in |

## 5. What the scene draws

- Tilemap compose 0x26E66 is fully table-driven: FG block table `0x2709C[scene]` (scene 2 →
  0x274DC, scene 6 → 0x27CDC), BG `0x270BC[scene]` (2 → 0x275DC, 6 → 0x27DDC), priority byte
  `0x26E9E[scene]` = `7C 7C 78 7C 7C FF 7C 7C`? — dump: `7C 7C 7C 78 7C 7C FF ..` TODO EXACT
  ordering (the listing shows words 7C7C 7C78 7C7C 7CFF: scenes 0..7 = 7C,7C,7C,78,7C,7C,7C,FF).
  `src/scene_map.c` indexes these tables by `$1C007E` with no scene restriction, so **the
  composer already supports scenes 2 and 6**; `engine/scenery.c` likewise reads placement
  `0x2867A[scene]` (2/6 → 0x2875A), scripts `0x28C66[scene]` (2/6 → 0x28F9A), patch pairs
  `0x295A4[scene]` FG (2/6 → {0x296EC, 0x2CD2C}) and `0x295DC[scene]` BG (2/6 → {0x2979A,
  0x2E2EC}) generically.
- Palettes: `0x2A06` with `$1C15F4 = scene` (0x2A98/0x2AB4 tables) then `0xC1EA`.
- Extra objects `$1C0F1C/$1C1028` get `+0x1C=3` and z=0x100 (valets stand on the floor).
- Ring hardware objects (ropes/posts from 0x1004A) are NOT re-created on entry, only on exit
  (0xFCC0) — the ring-out view has no rope objects; the ring is tilemap art.
- The count digits are FG0 text at $C0748/$C0744 (same place as the pin count).

## 6. C sketch

```c
/* 0xF98C, once per frame in pass 2 */
void ringout_scene_switch(eng_state *st)
{
    if (!(st->ringout_trig & 0x8000)) return;
    irq_body_enable(0);                                    /* bclr #7,$1C0076 */
    tilemap_work_clear(); vram_clear();                    /* 0x1F6C, 0x1FDE */
    if (!(st->ringout_trig & 0x4000)) {                    /* ENTER */
        st->scene = RINGOUT_SCENE[st->stage];              /* FA00 */
        st->g161 |= 2;
        extra[0].z = extra[1].z = 0x100; extra[0].f1c = extra[1].f1c = 3;
        for each wrestler o (0x250E) {
            o->f18 = o->f1a = o->f12 = o->ae = o->f34 = o->f74 = 0; o->l26 = o->l9c = 0; o->f4c = o->f4a = 0;
            if (o->cpu) o->b4 = 0;
            if ((o->f33 & 4) && o->move_id == 0x6A) {      /* the faller */
                st->scroll_ty = 0x200; st->scroll_tx = (st->ringout_face & 0x80) ? 0x350 : 0x190;
                o->state = 5; o->move_id = 0x6A; o->f32 |= 0x20; o->dmg68 = 8;
            } else if (!(o->f33 & 1)) {                    /* non-legal */
                o->f32 &= 0xC3; o->c6 &= ~0x40; o->f33 |= 4; o->f32 |= 0x20;
                o->state = 5; o->move_id = 0x6B; o->y = 0x164; o->z = 0x100;
                if (o->f33 & 0x80) { o->x = 0x428; o->facing &= ~0x80; } else { o->x = 0x200; o->facing |= 0x80; }
            } else {                                       /* other legal man */
                o->f32 &= 0xC3; o->state = 0; o->y = 0x160;
                o->x = (st->ringout_face & 0x80) ? 0x3A0 : 0x2A0;
            }
        }
        wait_vblank(); scene_compose_26E66(); pal_sel = st->scene; pal_load_2A06(); pal_tag_C1EA(); wait_not_vblank();
        ref->x = 0x340; ref->y = 0x160; ref->state = 0x8003;
        irq_body_enable(1); st->ringout_trig = 0; ym(0x80);
    } else {                                               /* LEAVE */
        st->scene = RETURN_SCENE[st->stage];               /* FA0A */
        st->g161 &= ~2; st->ringout_trig = 0; st->scroll_tx = 0x1E0; st->scroll_ty = 0x230;
        for each wrestler o {
            ...same clears...; o->f32 &= 0xC3; o->c6 &= ~0x40;
            if (o->f33 & 1) { o->state = 0; o->ae = 0; o->y = 0x150; o->z = 0x140; o->x = (o->f33 & 0x80) ? 0x2C0 : 0x240; }
            else            { o->state = 1; o->ae = 1; o->y = 0x160; o->z = 0x140; }
        }
        wait_vblank(); scene_compose_26E66(); pal...; wait_not_vblank();
        digits_wipe_206FE(); ring_hardware_init_1004A();
        ref->x = 0x280; ref->y = 0x198; ref->state = 0x8000; ref->f34 = 0; ref->f12 = 0;
        ym(0x20); st->g15d4 = 0; irq_body_enable(1);
    }
}

/* referee SM3 / visual 3 — 0x1FD5E, per frame while +0x20 == 0x8003 */
void ref_sm3_countout(eng_state *st, eng_ref *r)
{
    if (!r->latched) { r->latched = 1; st->count = 0; r->t24 = 0; r->ym54 = 0x3165; r->t23 = 8; r->pose = 9; r->face4 = r->facing; }
    eng_apply_motion(r);
    if (r->x < 0x298) r->x = 0x298; else if (r->x >= 0x390) r->x = 0x390;
    if (--r->t22 == 0) {                                   /* pose timer (word; reload via low byte +0x23) */
        r->t23 = 8;
        if (r->pose == 0xC) r->pose = 9; else if (++r->pose == 0xC) r->t23 += 4;
        r->face4 = r->facing;
    }
    if ((st->count & 0xFF) == 20) return;
    /* tick 0x1FDF4 */
    if (++r->t24 < 0x50) return;
    r->t24 = 0; st->count++;
    eng_obj *a, *b; legal_pair_204FA(&a, &b);
    if ((st->count & 0xFF) == 20 && !(a->f33 & 4) && !(b->f33 & 4)) return;   /* park at 20 */
    digits_blit_2067C(st->count);
    if ((st->count & 0xFF) >= 0x11) ym(r->ym54++);
    if ((st->count & 0xFF) != 20) return;
    r->t22 = 0x1000;
    if ((a->f33 & 4) && (b->f33 & 4)) { stamp(a, 0x8005); stamp(a->partner, 0x8005); stamp(b, 0x8005); stamp(b->partner, 0x8005);
                                        ym(0x3108); st->g169e = 0; chrome_blit_2503C(0x52); return; }
    eng_obj *out = (a->f33 & 4) ? a : b, *in = (a->f33 & 4) ? b : a;
    stamp(out, 0x8003); stamp(out->partner, 0x8003); stamp(in, 0x8002); stamp(in->partner, 0x8002);
    regain_90D6(); st->g169e = 0;
    if (out->cpu) jingle_20156(); else { ym(0x3108); chrome_blit_2503C(0x52); }
}
```

TODO EXACT summary: 0x19B14 (`+0x32` b1 meaning in the "in" scan); stage-2 ring-out = scene 0;
0x1FDAC t22/t23 word-vs-byte reload; 0x28544
zone-4 only on the y>0x140 path; 0x26E9E per-scene priority byte order; `+0x18` b7 hold flag.
