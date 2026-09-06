# Aisle walk-in — ROM 0x7B70 / 0x7F0E (src/aisle.c, ENG_SCENE_AISLE)

The entrance scene between the character select and the ring intro: the
human team walks up the aisle (scene word 4, the tunnel tilemap) under their
name plates, the screen fades to black, then 0xC98 sets the ring up and the
intro (walkin.c) draws the VS card (scene-banner.md). Oracle
`tools/scenarios/tag.scn`: scene word 3 -> 4 at f1582, 4 -> 0 at f1989.

## Flow (0xBD2 first match)

| ROM | what | engine |
|---|---|---|
| 0xC02 `jsr 0x7B70` | this scene | scene.c routes every front-end -> MATCH hand-off through ENG_SCENE_AISLE |
| 0x7B9A/0x7BB2 | sound 0x310E (stage 0) / 0x310D (stage 4, 9) | `eng_sound(0x310E)` (TODO EXACT: stage) |
| 0x7BC2 `0x1FC0` | FG0 filled with the solid tile: cells `FF C5 FF 04` | `fg0_fill(0xC5, 0x04)` |
| 0x7BC8..0x7BF0 | text set 0, scene 4, arena 4/4, window (0x140, 0x200) | `st->scene/cam`, `eng_scene_publish(4, 0)` |
| 0x7BF8 `0x26E66` + 0x7BFE | compose; priority byte 0x7B to $140011 | render.c compose; `wf.priority = 0x7B` each draw |
| 0x7C0C `0x285DA` | crowd stamps every frame | `eng_scenery_tick` in the scene draw |
| 0x7C16 `0x2AEA #$34` | sprite palette bank for the scene | not modelled (sprite.c maps by the stream bank byte) |
| 0x7C28..0x7C62 | id swap: a Hogan (id 0) leader trades +0x02 with the partner, undone if that puts id 9 in the leader slot | walk order only (`order_swap`) — TODO EXACT: the ROM swaps the object ids |
| 0x7C90 loop | per man 0x7F0E, 0x247C, 0x27B8; then 0x7E86, 0x285DA, 0x2836, vblank; until $1C0080 != 0 | `aisle_update` phases |
| 0x7CF0..0x7E0C | second team ($1C07C8 seated) walks the same way | only with `WF_AISLE_2P` (front end is the 1P flow) — TODO EXACT |
| 0x7E0E `0x1FC0` | black curtain | `fg0_fill`, then 6 frames (f1988..f1993, TODO EXACT: 0xC98 compose waits) -> MATCH |

Trace timing: scene 4 at f1582; leader +0x1C b7 (entered) at f1587; fade-in
0x8FD6 f1587..f1610 ($1C0080 1..8); walk from f1611 (y 0x130 - 0.5/frame);
y < 0x90 at ~f1930; fade-out 0x8F50 f1931..f1954; 0x21E6 wait 0x20 to f1986;
black f1988..f1993; ring + card f1994/5. Engine: 5 curtain frames (TODO
EXACT: the vblank waits in 0x2052/0x26E66/0x2A06), then identical counts —
`WF_DBGSEL=1` prints `aisle fN walk y=... sy=...` and the values match the
trace frame for frame (f1620 y=0x12A sy=0x2A <-> engine walk+9).

## 0x7F0E — one man

First entry (`bset #7,+0x1C`):
- leader only (A0 not one of $1C06BC/$1C08D4/$1C0AEC): 0x8FD6 fade-in —
  8 steps x 3 frames, FG0 filled with cell `FF (C5-step) FF 04`; 0x1F9E
  clears FG0 and sets $1C0072 b7 (CREDIT line redraw, 0x1E92 —
  `eng_credit_draw`); name plates through 0x2503C: `0x8186[id*4]` for the
  leader, `0x8186[partner id*4 + 2]` for the partner (mode-1 8x16 plates
  0x2B..0x3D; hud.c `eng_blit` gained the 0x250DC mode-1 path: tile
  `(c-0x10)*2 + 0x30` at the cell and +1 at the cell + 0x100); 0xB29E
  bottom bar (row 0x1D cols 2..29, tiles 0x4FA then 0x4FB, pal 1) + the
  small WWF logo run 0x9B26[0xD] at 0xC1A7C pal 1 (`eng_banner_runs`).
- rumble (0x7F9A): single plate from 0x816E — not modelled.
- 0x7FC6 both: sound 0x3180; 0x2AEA banks from 0x813E[id]; +0x22/+0x24 = 0;
  +0x02 += 0x40 (sprite row; 0x4B -> 0x4A); +0x00 = 0x8001 (polar mover);
  speed 8, angle 0x80; (x, y, z) = (0x1D8, 0x130, 0x100); tag: x = 0x1C0,
  partner +0x30.

Every frame (0x8068): 0x2208 motion (`eng_apply_motion`, -0.5 px/frame in
y); `y < 0x90` -> leader: 0x8F50 fade-out (8 x 3 frames, cell
`FF (BE+step) FF 04`) then 0x21E6 wait 0x20 frames, $1C0080 = 0x8000;
partner: just the flag. Otherwise the pose: cell = `0x8136[+0x24 & 0xFF]`
(identity 0..7); row 0x41 (Mr. Perfect) sets +0x1B = 0x30 for cells >= 4
(`off_y`); +0x22 counts to 0xC then +0x24 += 0x101; when the high byte
reaches 4: +0x24 = 0, and rows 0x40/0x41/0x47/0x49 take cells 4..7 on
`rng 0x21B4 & 1 == 0` (`eng_rng`).

Screen position 0x247C with the scene window: sx = x - 0x140, sy = y +
0x100 - 0x200; 0x27B8 clips rows < 0x4E at sy < -0x3F (sprite.c does the
same), so the men vanish at y < 0xC1 (~f1834) before the fade.

## 0x7E86 — the $1C14CE object

Re-seated every frame: id 0x4C, x 0x1D8, z 0x100, y = last walker's y + 1,
cell 0 for three frames then 0xFFFF (hidden) for two (+0x22 limit 2/1,
+0x24 toggles). The oracle's sprite RAM never contains a record for it even
while active with cell 0 (f1700..f1702); its pose decodes to tiles 0/2/4..
which are blank in the stock sprite ROM but not in data/gfx-edit, so the
engine keeps the machine and only emits it with `WF_AISLE_FLASH` —
TODO EXACT.

## Renderer fixes this scene needed (shared files, minimal)

- scenery.c: the FG/BG queue choice is bit 7 of the script's SYNC byte
  (0x28B74 copies script[1] to slot +0x03, 0x28C16 tests it), not of the
  cell byte. Scene 4 scripts (0x29362..) carry sync 0x81: BG. Also the
  stamps now mirror into the renderer's tilemap shadow
  (`wf_tilemap_shadow_write`) the way the IRQ3 stamper's bus stores do.
- render.c: the shadow is primed right after a recompose so a one-shot
  `--shot` keeps that frame's stamps; the recompose condition includes the
  text set.
- After these the engine FG/BG tilemaps and tile palettes are byte-identical
  to the oracle at f1700 (checked against `tag.wfo`).

## Verified

`WF_INTRO=1 ./wfengine --headless --front --drive gameselect --frames N
--shot` (game select times out at f511, character select at f2011, aisle
f2044..f2454, card f2455..f2582): scratchpad `final.png` pairs oracle
f1600/f1620/f1800/f1950/f1988/f2000/f2120/f2140 with engine
2060/2110/2250/2400/2450/2460/2580/2590 — curtain, fade-in, walk with
crowd + plates + bar + logo, fade-out pattern, black, card, card gone.

## TODO EXACT (summary)

- 5-frame curtain-in and 6-frame curtain-out are measured, not transcribed.
- Stage 4/9 sound 0x310D; rumble walker; 2P second team; 0x2AEA #$34 bank;
  the +0x02 id swap persisting into the match; the row-0x4C object.
