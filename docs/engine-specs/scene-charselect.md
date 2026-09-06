# SELECT PLAYERS — the character select (0x58B2..0x5EA8)

Engine: `src/charselect.c` (scene `ENG_SCENE_CHARSELECT`, registered from
`eng_scene_init`), hooks in `src/scene.c` (hands `eng_cs_picks()` to
`eng_init_picks`), `src/core.c` (`eng_init_picks`), `src/render.c`
(`eng_bg_cam_x`: one plane scrolls alone) and `src/scene_map.c`
(`wf_tilemap_shadow_adopt`). Oracle: `tools/scenarios/tag.scn` — window
(0x140, 0x300) written f201, loop f223, timeout f1245, P1 auto-pick f1333,
partner auto-pick f1471, flash ends f1547, walk-in (scene 4) f1582.

Reached from the game select's confirm (0x5474 -> 0x5698 -> 0x58B2). Leaves
by `jmp $AC0` (0x5EA8) into the walk-in / match init; the engine's dispatcher
turns that into `ENG_SCENE_MATCH` with the roster applied.

## 1. Entry (0x58B2..0x5994)

| PC | what | engine |
|---|---|---|
| 0x58BC | teammate pointers `+0x86`: slot0<->slot1, slot2<->slot3 | `si ^ 1` |
| 0x58EE/0x58F6 | window `$1C1804/6` = (0x140, 0x300); rumble 0x5934/0x593C (0x140, 0x500) | `cam_x/cam_y` |
| 0x58FE | scene word 3, `jsr $26E66` compose, `jsr $1F9E` fg0 clear, `$1C15F4/8` = 3, `jsr $2A06` | begin() |
| 0x57E0 | seat every active slot: `+0x02` = 0xD (cursor row), `+0x22` = 0x647C[slot], `+0x06/+0x0A` = 0x646C[slot], `+0x0E` = 0x140, `+0x04/+0xAE` = 0x6484[slot] (pose, colour), `+0xA1` b0 seated, `+0x58/+0x5C` = 0, `jsr $2AEA` palette | only slot 0 is active from the engine's game select (TODO EXACT: P2 seated there) |
| 0x5978 | 16 vblanks (`0x21E6`) | phase 0 |
| 0x5980 | 10 roster words `$1C0598..` = 0xFFFF; `$1C015C` = 0 | |

`$1C11F4[pos]` (cursor occupancy) and `$1C1214[pos]` (picked) are word
tables by grid position. Occupancy of the seat is already 1 at f223
(set by the seat path).

## 2. Grid

Ten positions, `0x64A4[pos]` = world (x, y): x 0x180/0x1E0/0x240/0x2A0/0x300,
y 0x268 for positions 0-4 (top row) and 0x208 for 5-9 (bottom row; rumble
adds 0x200, 0x5CF4). `0x64CC[pos]` = wrestler id: 0 7 2 8 6 / 9 1 3 A B
(Hogan, Perfect, Jake, Earthquake, Slaughter / DiBiase, Warrior, Boss Man,
Smash, Crush). Ids 4/5 (LOD) are CPU-only. Screen position is 0x247C:
`x - $1C17E6`, `y + 0x140 - $1C17EE` — the nP frame pose carries the offset
that puts it over the portrait.

Only three columns fit: the window x `$1C17E6` is 0x140 (columns 0-2,
"left page") or 0x210 (columns 2-4, "right page"), flag `$1C015E` b1. The
page flips whenever a cursor lands on column 2 (positions 2/7) from a side
column (0x5C16..0x5CC8); the flip is requested with b0|b3, the page bit is
toggled at 0x5D3C and 0x658C then slides `$1C17E6` by 8 px per frame to the
page's rest x, clearing b3 on arrival. `$1C17EA` (the other plane's camera)
never moves: the SELECT PLAYERS banner stays put while the portraits slide.
While b3 is set the loop skips input and `$1C015C` (0x59A2/0x59E6).

Move tables (`[pos * 11 + joystick nibble]` -> pos): 0x62BC left page (8
rows), 0x6314 right page, 0x6382 right page rumble. An occupied target is
re-routed through `0x63F0[prev]` (ten 10-byte tables at 0x6418..); tag
refuses position 9 (Crush alone, 0x5BB6) and an occupied alternative falls
back to `prev`. After a page flip 0x600E re-seats cursors that are not on
column 2 to the first free entry of 0x62AA (left) / 0x62B6 (right, tag) /
0x62B0 (right, rumble), scanning index 5 down to 0.

## 3. Per frame (0x5996..0x5D9C)

```
$1C0080++
b3 scrolling ? 0x658C step : ($1C015C++ >= 0x400 -> b4 auto-pilot)
for slot 0..3 (active, not picked):
    b3 ? position only (0x5CD0)
    0x64E0 input  (b4: 0x6534 auto-pilot)
    +0xA3 press ? 0x5A00 pick : 0x5B3E move
    0x5CD0 +0x06/+0x0A = 0x64A4[pos]; 0x247C screen; 0x27B8 sprite
0x2836 flush; vblank ($1C006F)
0x60A2 join: new START on an unseated port -> seat + 0x5F28 spawn (clears $1C015C)
b0 ? clear b0, toggle b1 page, 0x600E re-seat
for slot 0..3 active: not picked -> busy; picked -> ++$58 < 0x4C ? 0x5EAE flash, busy
busy -> loop; b5 && !b6 -> loop (2P-vs sub-select, TODO EXACT)
```

**Input 0x64E0**: `+0xA8` = `~port & 0xFF`, nibble in `+0xA9`; the same
nibble as last frame (`+0x91`) reads as 0 — the cursor moves on joystick
*edges*. Buttons: `+0xA2` = new presses of B1/B2, `+0xA6` level, `+0xA3` is
the press byte. **Auto-pilot 0x6534** (after 0x400 frames): `+0x5E++`; below
0x40 it bumps `+0x56` and, when `(rng & 0xF) | 4 < +0x56`, writes a random
nibble from 0x6584 into `+0xA9` (held, so the cursor runs to the edge); at
0x40 it rearms to 0x20 and sets `+0xA3` = 1 (a press every 32 frames).

**Pick 0x5A00**: refused on a picked position (auto-pilot clears `+0xA2`,
0x5B08). Otherwise picked[pos] = 1, occupancy cleared. Rumble: done. Tag:
position 8 (Smash) takes the teammate too — if the teammate is already
picked nothing happens (Demolition only as a pair), else the teammate is
activated/picked at pos+1 (0x5A36..0x5A94). Any other position: own
picked; if the teammate object is inactive and not human (`+0xA1` b0) the
port and button state are copied to it and 0x5F28 spawns the partner cursor
at the same position with the *same* nP identity (0x62A6[D6] is the own
slot index) — one human picks both men of the team.

**Flash 0x5EAE**: pen 15 of fg palette bank `0x5F14[pos]` (rumble 0x5F1E)
at `0x188000 + bank*0x80 + 0x1E` cycles 0x5F0A (cyan, magenta, 0x00EF,
white, magenta) one word per frame for 0x4C frames; frame 0x4B writes the
player colour (`+0xAE`, 0x6484: 0x00F0 green for 1P) which stays.

## 4. Exit (0x5DA0..0x5EA8)

0x5DAC: per active slot, `roster[n++] = 0x64CC[pos]`, `+0x56` and `+0x02`
= id. 0x5DF0: slot 0 active but not human swaps with slot 1 (`+0x56`,
`+0x02`, roster pair); the slot 2/3 swap at 0x5E48 tests ROM 0x64CC through
a stale A2 and never runs. Clear `$1C0156/015A/015C/015E/1698`, 32 vblanks,
`jmp $AC0`.

CPU team (match placement 0x10370..0x104A4, `docs/select-contract.md`):
list the ids not in the roster and not 4/5 at `$1C0380`, two weighted draws
0x24CC over `0x106E0[stage]` (equal draws step the second by ±1), either
id A/B forces the pair A,B, `$1C0163` 4/9 forces 4,5; the ids land in the
CPU objects and in the first two 0xFFFF roster words. The engine does this
in `finish()` with `eng_rng()` — **TODO EXACT**: the ROM rng stream.

`eng_cs_picks()` = roster[0..3] = {P1, partner, CPU1, CPU2};
`eng_init_picks(st, picks)` seats them in slots 0..3 (tag-mode.md §1).

## 5. Engine notes / TODO EXACT

- 2P-vs sub-select (`$1C0066 & 0x800`, 0x610A..0x62A2, tables 0x6494) and
  the credit test on join (0x55A) are not modelled; START on ports 1..3
  joins unconditionally.
- The page scroll needs the two planes on different cameras: `eng_bg_cam_x`
  composes the `$1C17EA` plane at the still x and the renderer adopts the
  composed VRAM as its tilemap shadow (`wf_tilemap_shadow_adopt`), which
  also fixes the same-scene window change from the game select.
- Picked cursors are not drawn: 0x27B8 skips them and 0x2836 disables every record past this frame's count (0x28C8..0x28E6), so the frame vanishes on the pick and nothing trails the moving partner cursor. No cursors during the exit wait (oracle f1560).
- Rumble tables are wired (move/seat/bank) but the rumble exit is the tag
  path.

## 6. Harness

```
./wfengine --headless --drive charselect --frames N --shot out.ppm   # straight in
WF_P1="30-31:1,60-61:10" ...                                         # R edge, B1
./wfengine --headless --front --drive script --frames 700            # game select -> select -> match
WF_DBGSEL=1  prints cs: lines (timeout, flags, page x, per-slot pos/picked, picks)
```
Oracle frames: `wfport ... --dump-frames 240,1400,1546,1560` (see HANDOFF).
Side-by-side PNGs from this session are in the scratchpad (`cmp_*.png`).
