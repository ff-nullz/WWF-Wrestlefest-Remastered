# ATTRACT placeholder, coins and START (ROM 0x654 / 0x6FC / 0x434 / 0x978)

Engine: `src/attract.c` (scene `ENG_SCENE_ATTRACT`, the default boot
scene), `src/credit.c` / `src/credit.h` (coin chutes, credit counter,
START seat, CREDIT-line gate). Headless:
`./wfengine --headless --drive attract --frames N --shot out.ppm`
(`WF_P1="a-b:bits,..."`: bits 1 R 2 L 4 U 8 D 10 B1 20 B2 **40 START
80 COIN**; `WF_CREDITS=n` preloads credits; `WF_DBGSEL=1` logs coins,
seats and phase changes). GUI: the window boots into it; `5`/`6` are
the coin chutes, `1`/`2` P1/P2 START (the MAME keys). `--no-front` /
`WF_NOFRONT=1` still go straight to the match; `--drive gameselect` or
`WF_FRONT=gameselect` start at the GAME SELECT as before.

Flow: **ATTRACT → (coin, START) → GAME SELECT → character select → …**

## 1. What stock does (reset path)

| PC     | what |
|--------|------|
| 0x654  | reset: SR, USP, registers cleared; `$184000` = 0; `$1C0076` = 0; `$1C007A` = -1 |
| 0x6BE  | 0x1FC0 FG0 fill; 0x6C2 sound 0x3100; 0x1F1E clear work RAM `$1C0060..$1C3AD4`; 0x1E1E dips → `$1C0066`; 0x2006; 0x400 coinage → `$1C004B/$1C004D` from table 0x42C[dip&3]; 0x6D82 high-score names |
| 0x6FC  | attract rebuild (also the post-match entry): clr `$1C007C/$1C00B2/$1C0076`, 0x1F52, bset 7,`$1C0076`, sound 0x3100 |
| 0x724  | `btst 7,$1C0067` (dip) → skip the FBI card to 0x790 when set |
| 0x72E  | 0x1FC0, 0x1F6C, 0x1FDE, 0x1F9E (FG0/tilemaps/sprites cleared, credit redraw forced), 0x2988 palette VRAM cleared, clr `$1C0166` |
| 0x748  | `$1C007E` = 3, `$1C15F4` = `$1C15F8` = 3, jsr 0x2A06 palette load (`$1C15FC` = 0) |
| 0x764  | jsr 0x26772: snapshot palette banks 0x180000/0x182000/0x186000/0x188000 (16 lines × 16 words, stride 0x80) to `$1C1CD4..`, then zero them; 2 vblanks |
| 0x76A  | scroll `$1C1804` = 0x280, `$1C1806` = 0x600; jsr 0x26E66 compose (scene 3 window = the FBI seal and text) |
| 0x780  | jsr 0x26844 **fade-in** |
| 0x786  | bsr 0x1E6C **hold** 0x80 vblanks |
| 0x78A  | jsr 0x264E2 **fade-out** |
| 0x790  | trademark card: clears, 0x2A06, 0x26772, blit id 4 (0x2503C), 0x26844, 0x1E6C, 0x264E2 |
| 0x7C6  | Technos / Tecmo card: `$1C15FC` = 3, jsr 0xA0AE, scroll (0x3C0, 0x600), compose, fade-in/hold/fade-out |
| 0x830  | bra 0x91C: sound 0x3102, palettes 0, 0x81BA roster/title scroll (scene 3 at (0x280, 0x400), "INSERT COIN" blits 2/0x53/0x54), then 0xAC0 demo match … back to 0x6FC |

### 1.1 Fade routines

* **0x26844 fade-in**: `bclr 7,$1C0076` (IRQ3 work off). For step D0 =
  0..7: map the snapshot through 0x26642 into `$1C24D4..`, wait vblank,
  copy to the four banks (0x26752), then three more vblank waits → **4
  frames per step, 32 frames**. `bset 7,$1C0076` on exit.
* **0x264E2 fade-out**: clear scratch, `bclr 7,$1C0076`, wait vblank,
  snapshot (0x26732), wait; for D0 = 7 down to 0: map, wait, copy, three
  waits (4 frames per step; step 7 is the unchanged picture). Restores
  `$1C0076` from D7 at 0x26636.
* **0x26642 map**: per colour word `w`, the three low nibbles (bits
  8-11, 4-7, 0-3) become `rom[0x266B2 + nibble*8 + step]`; the top
  nibble is dropped. Table 0x266B2 is 16 rows × 8 steps (column 0 is
  all zeros = black, column 7 is the identity).
* **0x1E6C hold**: 0x80 iterations of `bset 0,$1C006F` / wait bit 1 —
  one vblank each. IRQ3 (0x834) is live here: 0x900 → 0x1E92 credit
  line every vblank, 0x978 START scan.

### 1.2 Oracle timings (`boot.scn`, `--68k --video`)

| oracle frame | what |
|---|---|
| f0-15 | black (reset, compose) |
| f16 + 4k | fade-in step k visible (f20 first non-black, f44 step 7 = full) |
| f47 | CREDIT line appears (IRQ3 back on) |
| f47-176 | hold |
| f181 + 4k | fade-out step 6-k (f205 = black) |
| f209-216 | trademark card prep (palette reload, 0x26772) |
| f217 | trademark card fade-in starts |

## 2. Engine placeholder

`attract.c` plays the FBI card and **loops it**:

```
PRELUDE 16 frames black (oracle f0-15)
FADEIN  8 steps x 4 frames, IRQ3 off           (0x26844)
HOLD    0x80 frames, IRQ3 on: credit line, START (0x1E6C)
FADEOUT 3-frame lead + 8 steps x 4 frames, IRQ3 off (0x264E2)
GAP     5 frames black, IRQ3 on (stock: next card's prep)  -> FADEIN
```

Pixel-exact vs the oracle at f15, f20, f30, f44, f47, f100, f181, f197,
f205 (`--drive attract --frames N`, engine frame N = oracle frame N).
The fade is applied in `draw()` after `wf_palette_latch()`: the
snapshot is taken from the loaded palette the first time the scene is
drawn (0x26772) and the current step is rewritten to the four banks every
frame (same picture as the ROM's in-place step writes).

**TODO EXACT (the loop stands in for all of this):** the trademark
card (0x790, blit 4), the Technos/Tecmo card (0x7C6, 0xA0AE), the roster
scroll 0x81BA with INSERT COIN, the demo match 0xAC0 and the return
through 0x6FC; the `$1C0067` bit 7 dip that skips the FBI card; the
GAP length is the oracle's card-to-card spacing, not a ROM constant.

## 3. Coins (IRQ2 0x434, every frame on every screen)

`eng_coin_tick()` runs at the top of `eng_update()` — the interrupt is
not scene-gated. Per chute (0x48C; port bit active-low, `pressed` =
engine input bit 7):

```
pressed                -> held++                                 (0x4EE)
released, held == 0    -> if gap < 10: gap++                     (0x4B0)
released, gap < 4      -> held = 0   (too soon after a coin)     (0x4AC)
released, held >= 0x360-> held = 0   (stuck coin)                (0x556)
released               -> COIN ; gap = 0 ; held = 0              (0x4A8)
```

COIN (0x4F2): `$1C004E`++ (coins banked), `$1C0052`++, sound **0x312A**
(`eng_sound(0x312A)`; the audio layer keys on 0x2A); if `$1C004B`
(coins per credit) > banked → done; else credits `$1C004F` +=
`$1C004D` (credits per coin), capped at **0x63** (cap also zeroes the
bank), `$1C0050` += same, bank -= coins per credit.

Coinage table 0x42C (`$1C0067 & 3`): (1,1) (1,2) (2,1) (3,1). The
engine uses row 0 — the dip word is not modelled (TODO EXACT). Service
coin 0x4BC (`$140020` bit 2, `$1C0056` debounce, +1 credit after 0x1E
frames) has no key (TODO EXACT).

## 4. START (IRQ3 0x978 while `$1C007C` bit 7 is clear)

4-slot path (dip `$1C0066 & 0x800 != 0x800`): for slot 0..3, if START
is pressed (`btst 7,(1,A0)` clear): `jsr 0x55A` with D0 = 0 → 0x5CE:
`$1C004F == 0` → carry clear, next slot (**START with no credit does
nothing**); else `$1C004F`--, carry set → clr `$1C0076`, bset 7
`$1C007C`, 0x1F38 object clear, slot +0 = 0x8000, +0x8A = port address;
0x8AE resets the stacks, 0x1F6C/0x1FDE/0x1F9E, `jmp 0x52BE` GAME SELECT.

`eng_start_scan()` returns the seated player; `attract.c` switches to
`ENG_SCENE_GAMESELECT` on the same frame. It runs only while the scene's
IRQ3 flag is on (hold and gap phases), like the ROM's `$1C0076` bit 7
gating. TODO EXACT: the 2-slot dip path 0x9E6, P2 START seating during
the game select / walk-in (0x6E3E), the mode 2/3/4 credit costs of
0x55A (0x582..0x648).

## 5. CREDIT line (0x1E92 gate)

`eng_credit_line()`: redraw when `$1C0072` (last drawn) differs from the
word at `$1C004E` (coins << 8 | credits) or bit 7 of `$1C0072` is set
(0x1F9E forces it on every screen entry); the body is
`eng_credit_draw()` in gameselect.c (blit 0 + number 0, §5 of
scene-gameselect.md). Called from the attract hold (IRQ3 0x900) and from
the game select loop (0x546A), so coins dropped on either screen update
the count. With one coin per credit the banked byte is always 0, so the
line tracks `$1C004F`.

## 6. Exact vs TODO EXACT

Exact: FBI card tilemap/scroll/palette (scene 3 at (0x280, 0x600),
arena 3, text set 0), fade table and step timing, hold length, IRQ3
gating of START and the credit line, coin debounce and credit
arithmetic, START consuming one credit and entering the game select,
START ignored at zero credits, CREDIT line gate.

TODO EXACT: everything after the FBI card (§2), dips (coinage, 2-slot
cabinet, FBI skip), service coin, free play, P2 seating outside the
attract, the 0x1F38 object clear (the engine re-inits at match start).
