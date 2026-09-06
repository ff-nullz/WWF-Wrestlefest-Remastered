# GAME SELECT screen (ROM 0x52BE – 0x54DE)

Engine: `src/gameselect.c` (screen), `src/scene.h` / `src/scene.c`
(front-end dispatcher). Run it headless with
`./wfengine --headless --drive gameselect --frames N --shot out.ppm`
(`WF_P1="a-b:bits,..."` drives P1; bits 1 R 2 L 4 U 8 D 10 B1 20 B2);
in the GUI with `./wfengine --front` (or `WF_FRONT=1`).

## 1. Entry (reset path 0x8AE → 0x8F2)

After coin + start the vblank handler's start-wait (0x89E/0x978) resets
the stacks and runs, in order:

| PC      | what                                                        |
|---------|-------------------------------------------------------------|
| 0x1F6C  | clear $1C16D2..$1C19AE scroll scratch, clear FG/BG tilemaps |
| 0x1FDE  | clear $1C19B2..$1C1CD0, clear spriteram $C2000 (0x1000 B)   |
| 0x1F9E  | clear FG0 $C0000 (0x2000 B); bset 7,$1C0072 (credit redraw) |
| 0x52BE  | `jmp` — the screen itself                                   |

0x52BE:

| PC      | what                                                                |
|---------|---------------------------------------------------------------------|
| 0x52BE  | $1C15FC = 1 (fg0 text palette set)                                  |
| 0x52C6  | jsr 0x2A06 palette load (arena $1C15F4 still 0 here)                |
| 0x52CC  | sound 0x3100, 0x3120, 0x3103 via 0x2052 (it ORs 0x3100 while $1C007C != 0) — the menu music. **Engine**: `eng_sound()` with the same three words. |
| 0x52EA  | bclr 4,$1C006F; clr.l $1C0080; bset 7,$1C0076                       |
| 0x5300  | $1C1804 = 0x140, $1C1806 = 0x200 (scroll)                           |
| 0x5310  | $1C007E = 3 (scene word)                                            |
| 0x5318  | jsr 0x26E66 compose (scene_map.c, native)                           |
| 0x531E  | jsr 0x1F9E (FG0 clear + credit redraw flag)                         |
| 0x5324  | $1C15F4 = 3, $1C15F8 = 3                                            |
| 0x5334  | jsr 0x2A06 palette load again, now with arena 3                     |
| 0x533C  | clr $1C015C (timeout)                                               |

The engine publishes scene 3 / cam (0x140,0x200) / arena 3 / textset 1
and lets `render.c` compose + palette-load on the change it sees
(`eng_scene_vals()` feeds `$1C15F4`/`$1C15FC` to pal_load.c).

## 2. Per-frame loop 0x5342 – 0x5470

Four slots $1C05B0 + n·0x10C; only slots with +0 bit 7 (seated) run.
One seated slot = two vblank waits per iteration = one frame. Only P1
is modelled (TODO EXACT: a seated P2 would run the same code on its own
input in the same frame).

```
$1C0080++ ; clr $1C006E
$1C015C++ ; if $1C015C >= 0x200 -> 0x5474 (leave, current mode)      0x5360
jsr 0x64E0                                                            0x5372
   +0xA8.w = ~port & 0xFF ; +0xA9 &= 0xF  (word +0xA8 == dir nibble)
   D1 = (raw >> 4) & 3  (buttons)  ; +0xA6 held ; +0xA2.w = new & ~held
if +0xA3 (low byte of +0xA2: ANY new button press) -> 0x5474 (leave)  0x5378
D0 = +0xA8.w (direction nibble)
if !($1C0161 bit0):                                                   0x5384
    if rom[0x54EC + D0]    (LEFT):  bset 0,$1C0161 ; wait vbl ;
                            0x54CA(0x5518 -> pal 0x188602) ; clr +6,+A
    else                   0x5484(0x5518 -> pal 0x188602)   (tag card flash)
else:                                                                 0x53FA
    if rom[0x54E0 + D0]    (RIGHT): bclr 0,$1C0161 ; wait vbl ;
                            0x54CA(0x55D8 -> pal 0x188482) ; clr +6,+A
    else                   0x5484(0x55D8 -> pal 0x188482)   (rumble flash)
jsr 0x1E92 (credit line)                                              0x546A
```

Tables (read from ROM at runtime):

| addr   | contents                                                       |
|--------|----------------------------------------------------------------|
| 0x54E0 | 12 bytes, nonzero at dir 1/5/9 (RIGHT with any U/D)            |
| 0x54EC | 12 bytes, nonzero at dir 2/6/10 (LEFT with any U/D)            |
| 0x550E | flash sequence, 10 bytes: 1 2 3 4 5 4 3 2 1 0                  |
| 0x5518 | tag card palette, 6 sets × 16 words (15 copied)                |
| 0x55D8 | rumble card palette, 6 sets × 16 words                         |

### 2.1 Flash 0x5484

```
+0xA++ ; if +0xA != 5 return ; +0xA = 0
set = rom[0x550E + +6] ; +6++ ; if +6 == 10: +6 = 0
copy 15 words rom[table + set*32 ..] -> palette dst (moveq #$e / dbra)
```
0x54CA is the same copy with set 0. Destinations are in the fg-tile
bank (0x188000): 0x188602 = line 0xC pen 1 (Saturday Night's Main Event
logo), 0x188482 = line 9 pen 1 (Royal Rumble logo). The engine keeps the
current set per card and rewrites the 15 words every frame from
`gs_draw()` (after `wf_palette_latch()` in render.c) — same picture, the
write just repeats.

Oracle-measured timing (match.scn trace): loop iteration 1 is oracle
frame 109; the first step (set 1) is sampled at f113, then every 5
frames. Engine frame count N ↔ oracle frame 108+N. Pixel-exact against
`match.scn` f150 (engine `--frames 42`) and f250 with LEFT at engine
frames 92-94 (`--frames 142`); f153 / f251 (next step) differ, which is
the expected boundary.

## 3. Input / confirm

* Port byte (0x64E0, MAME wwfwfest map): bits 0-3 R/L/U/D, 4 B1, 5 B2,
  7 start. Engine bit 6 (start) is moved to 7 by `port_byte()`.
* LEFT arms Rumble, RIGHT disarms it. Up/down alone do nothing.
* **Confirm = one new press of B1 or B2** (tst.b +0xA3). The scenario
  comment "two button presses" counts the character-select's press as
  well. Oracle: b1 pressed at f260 is sampled at f261 and the loop leaves
  that frame; engine leaves on the frame it sees the press.
* Timeout: $1C015C reaches 0x200 → leave with the current mode (0x5474).

## 4. Leaving (0x5474 → 0x5698 → 0x57E0 → 0x58B2)

* 0x5474 clr $1C015C/$1C015E.
* 0x5698: a "vs" sub-select for two seated players, gated on
  `$1C0066 & 0x800` (dip) and P2 seated. Not modelled — TODO EXACT.
  (Oracle PC at f202/f240 with only P1 seated is 0x5420/0x5428, the
  flash wait, so it is never entered in the stock scenarios.)
* 0x57E0: seats the character-select objects (+2 = 0xD, +6/+A from
  0x646C/0x647C, +0xE = 0x140, sprite from 0x6484/0x6494 ...) — the
  character select's business.
* 0x58B2: scene 3 recomposed at scroll (0x140, **0x300**) for tag or
  (0x140, **0x500**) for rumble, 0x1F9E, arena 3, 0x2A06 again. The
  engine leaves `st->cam_x/cam_y` at that pair and returns
  `ENG_SCENE_CHARSELECT`; while no character select is registered the
  dispatcher starts the match (tag; rumble mode TODO EXACT).

## 5. CREDIT line (0x1E92) and number draw (0x26122)

Forced by bit 7 of $1C0072 (set by 0x1F9E): $1C0063 = BCD($1C004F)
(0x2790), blit id 0 (0x2503C, "CREDIT:" mode 0 at FG0 0x1E6C — `eng_blit`),
number id 0 (0x26122 record 0x262B0: mode 0 pal 0 at FG0 0x1E88, value
from $1C0063, two digits with leading-zero blank), and with the high
byte of $1C004E zero the three cells at $C1E94 are cleared (0x1F06).
`eng_num_draw()` transcribes all three 0x26122 modes (0x261A2 8×8,
0x261CE 8×16, 0x2620E 16×16). Not modelled: free play `$1C004B == 1`
(0x1ED4) and the `$1C004E` high-byte branch (0x1EE6) — TODO EXACT.
Credits are 0 after the start consumed the coin; the engine draws 0.

## 6. Exact vs TODO EXACT

Exact (oracle pixel-identical): tilemap/scroll/priority (scene 3 via
scene_map.c), palettes (arena 3 + fg0 set 1), both card flashes and
their phase, LEFT/RIGHT arming, confirm on a new press, timeout value,
CREDIT line.

TODO EXACT: P2 seated slot; the dip-gated 2P sub-select 0x5698; free
play credit text; palette write one frame later than the ROM's in-vblank
store is not modelled (the visible result matches frame for frame);
sound words 0x3100/0x3120/0x3103 are passed to `eng_sound()` as-is — the
audio layer keys on the low byte.
