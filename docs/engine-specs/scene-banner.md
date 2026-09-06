# Match-card "VS" banner — ROM 0x98BA / 0x9A42 (src/banner.c)

The card drawn over the ring at the start of the ring intro: both team-1
names across the top rows, "VS" in the middle, both team-2 names across the
bottom rows. Oracle: `tools/scenarios/tag.scn` f1994..f2121 (first visible
frame f1995, gone at f2122).

## Where it sits in the flow

| ROM PC | what | engine |
|---|---|---|
| 0xC66 | `jsr 0xA654` ring intro, after 0xC98 match init | `eng_intro_begin` (walkin.c) |
| 0xA670 | `jsr 0x98BA` — draw the card before step 0 | `eng_banner_draw(st)` from `eng_intro_begin` |
| 0xA9E2 step 0 | wait `0xAA66[0]` = 0x80 frames | walkin.c `step_wait` |
| 0xAA4E | step 0 -> 1: `jsr 0x9A42` wipe FG0 rows 1..29 | `eng_banner_clear()` |
| 0xA782 | intro skipped / ended: `jsr 0x9A42` | `eng_banner_clear()` from `eng_intro_end` |
| 0x7BC8 / 0xC6C | text palette set 0 during the intro, 1 after | render.c publishes set 0 while `eng_banner_active()` |

Timing (trace, announcer object $1C14CE +0x22): +0x22 = 1 at f1994, the
card is drawn the same frame; 0xAA46 steps at +0x22 = 0x81 -> f2122 wipes
it. 128 frames on screen.

## 0x98BA draw

Skipped entirely in Royal Rumble (`btst #0,$1C0161`, engine `eng_gs_rumble()`).

For every active object $1C05B0 + n*0x10C (engine slots 0..3; only the slot
parity and side bit +0x33 b7 matter, so the engine's team-2-at-2/3 layout
and the ROM's $1C09E0 CPU pair give the same result):

- `id = +0x56 & 0xF` (engine `wrestler & 0xF`).
- 0x993A: `pos = (slot & 1) | (side ? 2 : 0)`; palette nibble
  `d7 = (pos & 2 ? pos + 0xC : pos + 6) << 4` — lines 6/7 for team 1,
  0xE/0xF for team 2; FG0 address = 0xC0000 + word `0x9980[id*8 + pos*2]`
  (ids 0..9 only — 10 rows of 4 words, the table ends at 0x99D0 where the
  next routine starts).
- ids >= 0xA: 0x99D0 stamps the fixed 8-row x 38-cell bitmap at 0x9D36
  (row stride 0x26) to FG0 0x504 (team 1) / 0x1504 (team 2), byte +3 =
  `d7 + 9` (the +9 lands in the tile-high nibble: tiles 0x9xx).
- otherwise 0x9ACA glyph run, record `0x9B26[id]` = `{first tile (word),
  count-1 (word), then (delta (word), count-1 (word))... , 0}`; each cell
  gets the next consecutive tile, byte +1 = tile low, byte +3 = tile high
  | d7; `delta` is added unsigned (`andi.l #$ffff`) — 0x100 = next row same
  column, 0xFC = next row one column left, 0x104 = one right.
- 0x9A78: 16 palette words from `0x9E66[id]` (long) to
  0x180300 (team 1) / 0x180700 (team 2), +0x80 when the slot is odd — the
  FG0 palette lines 6/7/E/F. Written through the bus (aliased palette RAM,
  pal_load.c).
- "VS": A3 = 0xC0E40, or for ids 4/5 0xC1040 when on team 1, then
  0x100 is subtracted for ids 4/5 either way (0x98E2..0x9902); the LAST
  active object's value wins. After the loop: 0x9ACA record 0xC (0x9D24,
  4 rows x 8 cells from tile 0x72F) at A3, palette 0.

Table facts read back from ROM at runtime (nothing is hand-copied):
- 0x9980 rows: id0 `0704 0750 1704 1750`, id6 `0328 0370 1428 1470`,
  id8 `0514 0560 1614 1660`, id9 `0324 0374 1424 1474`, ...
- 0x9B26 records: 0x9BC6 (id 0, 3 rows of 19 from 0x529), ..., 0x9D24 = VS,
  0x9B5A = 0xA496 (id 0xD: the small WWF logo used by 0xB29E).

## Engine specifics

- `eng_banner_draw` records `{id, pos, d7, palette dst}` per slot and the
  VS address; `eng_banner_refresh()` replays the cell + palette writes every
  frame while the card is up. Reason: `eng_init` (which begins the intro)
  runs before `eng_render_init` loads the ROM / VRAM, and the scene palette
  reload (`wf_palette_latch` -> `wf_palette_load`) runs after it in
  `eng_render_frame`; the replay is byte-identical to the one-shot ROM
  writes. The ROM 0x9E66 read happens at refresh time for the same reason.
- `eng_banner_clear` = 0x9A42: `memset(fg0 + 0x100, 0, 0x1D00)`.
- render.c: `eng_banner_refresh()` sits right after `wf_palette_latch()`;
  the recompose condition also keys on the text set so the set-0 -> set-1
  switch after the intro reloads the text palette (0xC74).

## Verified

- `WF_INTRO=1 ./wfengine --headless --frames 6 --drive script --shot x.ppm`
  vs oracle f2000: same glyph positions, colours (red HULK HOGAN, gold
  logo, team-2 names), VS position. Scratchpad `cmp6.png` / `final.png`.
- Card gone at intro frame 128 (chain shots 2580 vs 2590 = oracle
  f2120 / f2140).

## TODO EXACT

- Ids 0xA/0xB bitmap path (0x99D0) is transcribed but only eyeballed
  (DEMOLITION renders); no oracle frame with those ids was dumped.
- 0xA65E..0xA676 and the rest of the intro are walkin.c's.
