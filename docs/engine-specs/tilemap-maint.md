# Per-frame tilemap maintenance — exact semantics (reference/maincpu.asm)

Everything below is read straight from `../wrestlefest-decomp/reference/maincpu.asm`.
All camera/tile values are for FG ("layer A", VRAM `$80000`, row pitch `0x80` bytes = 64 entries);
the BG layer (`$82000`, pitch `0x40`) is the mirror path noted where it differs.

**Headline answer:** the maintenance blit uses *exactly* the same source math and tables as the
full compose — **no clamp, no alternate block table**. The 357-cell diff vs stock comes from a
**third writer**: a per-scene, camera-gated **tile-animation overlay** (`0x285DA` → queue →
IRQ3 `0x2946A`) that stamps prebuilt 8×4-tile patches (crowd/ramp animation frames) over the
raw-map content at fixed, map-anchored VRAM offsets. The raw map's ring-corner/hook tiles at
those cells are placeholders the stock game always covers within 1 frame of them entering view.

---

## 0. Coordinate model (needed for everything else)

- Camera regs: FG camX `$1C17E6`, FG camY `$1C17EE` (BG pair `$1C17EA`/`$1C17F2`; in matches
  the camera-follow `0x26936` adds the same clamped ±4 delta to both, so FG==BG cam).
- **Unit coords**: `unitX = camX >> 5`, `unitY = camY >> 5` (`0x26ADA` → `$1C17D6/$1C17D8`).
  One unit = 4 FG tiles wide × 2 FG tile rows tall (VRAM step 8 bytes horizontally, `0x100`
  bytes = 2 rows vertically). BG unit = 2×2 BG tiles (steps 4 bytes / `0x80` bytes).
- **Y is flipped in map space**: source row index is `0x40 - unitY` (see `0x26CA8`); scroll-reg
  write `0x26E1A` does `scrollY = -(camY - 0x800)` into `$100002` (BG: `$100006`), FG scrollX
  `$1C17E6`→`$100000`, gated off while `$1C006F` bit4 is set.
- **VRAM is a 16×16-unit torus**: dest addr (`0x26D56`) =
  `base + ((unitX + 0x10) & 0xF) * 8 + ((0x40 - unitY) & 0xF) * 0x100`
  (BG: `*4` and `<<7`). So map unit *u* always lands in VRAM column `u & 15` regardless of the
  camera — the map→VRAM mapping is a **fixed aliasing mod 16 units**, camera-independent.
- **Source** (`0x26CA8`, the `unit_src` both compose and maintenance share):
  `mx = unitX + xoff`, `my = 0x40 - (unitY + yoff)`;
  block pointer `A2 = sceneBlockTable[(mx / 10) * 4 + (my >> 3) * 32]` where the per-scene
  block-pointer table is `$2709C[scene]` for FG, `$270BC[scene]` for BG (`0x26C1C`);
  in-block offset FG = `(mx % 10) * 8 + (my & 7) * 0xA0` (BG: `*4` / `*0x50`).
  Blocks are 10 units wide × 8 units tall. **There is no clamp, no bounds test, no wrap mask on
  `mx`/`my` anywhere in this routine** — identical for compose and maintenance.

## 1. What runs each frame

Frame-loop order (main loop at `0x1054`, matches frame-order.md):

```
jsr $285DA   ; tile-ANIMATION driver: cull by camera, tick scripts, ENQUEUE cell bytes
jsr $26A42   ; edge-strip builder: detect unit-coord change, build 16 src/dst pointer pairs
jsr $26936   ; camera follow (±4 clamp) — note: runs AFTER the two above
...
IRQ3: jsr $2946A  ; stamp queued animation cells into VRAM
      jsr $26DAC  ; blit the pending edge strip into VRAM
```

### 1a. Edge-strip maintenance (`0x26A42` → IRQ3 `0x26DAC`)

- `0x26A42`: early-out if `$1C16D2 != 0` (a strip is still pending — this is the W_DIRTY word:
  bit7 = FG strip armed, bit6 = BG strip armed). Otherwise runs the body twice, toggling
  `$1C17FE` bit7 (clear = FG, set = BG).
- Per layer (`0x26A60`):
  1. `0x26A7A`: load working cam `$1C17F6/$1C17FA` from that layer's cam regs, and load the
     layer's **tracked unit coords** into `$1C17E2/$1C17E4` (FG tracks in `$1C17DA/$1C17DC`,
     BG in `$1C17DE/$1C17E0`).
  2. `0x26ADA`: compute new `unitX/unitY`; compare to tracked. Direction code → `$1C16D4`:
     `1` = X grew, `3` = X shrank, `0` = Y grew, `2` = Y shrank, `$FF`+carry = no change (stop).
     **X takes priority; at most ONE axis, ONE unit step per layer per frame** (big jumps are
     caught up one unit per frame because step 4 advances the tracked coord by exactly 1).
  3. `0x26B4A`: arm `$1C16D2` bit7 (FG) / bit6 (BG).
  4. `0x26B68`: pick tables (`0x26C1C` as above; dest base + pair buffer `$1C16D6` FG /
     `$1C1756` BG) and the **strip offset list** (`0x26C6E`): `$26F82[dir]` →
     - dir 0 (Y grew): 16 words `(xoff,yoff)` = X −3…+12, Y **+5** (incoming top row)
     - dir 1 (X grew): X **+13**, Y +4…−11 (incoming right column)
     - dir 2 (Y shrank): X −3…+12, Y **−12** (incoming bottom row)
     - dir 3 (X shrank): X **−4**, Y +4…−11 (incoming left column)
     then `0x26C8A` builds 16 `(src,dst)` pairs via `0x26CA8`/`0x26D56` into the pair buffer.
     (`xoff` = high byte, `yoff` = low byte, both sign-extended.)
  5. `0x26B76`: advance the layer's tracked unit coord by 1 in the moved direction.
- IRQ3 `0x26DAC`: if bit7 set, copy 16 FG pairs — per pair 4 longs: src `+0,+4` → dst `+0,+4`
  and src `+0x50,+0x54` → dst `+0x80,+0x84` (one unit = 4 tiles × 2 rows; source row pitch
  `0x50`·2 = `0xA0` matches `0x26CA8`). If bit6 set, 16 BG pairs as words
  (`+0,+2,+0x28,+0x2A` → `+0,+2,+0x40,+0x42`). Clears the bits.

**Because source math and tables are identical to the full compose, the strip content is always
byte-for-byte the raw map. Panning can never, by itself, make stock VRAM differ from a full
recompose at the same camera.** (Window cells other than the incoming strip are simply never
rewritten — but they were written from the same map when they entered, so no drift.)

### 1b. Full compose (`0x26E66`, scene start only)

Runs both layers (toggling `$1C17FE` bit7) through `0x26EA6`:
sets **all four cam regs from the scene-start camera `$1C1804/$1C1806`** (so yes — at scene
start the window is the scene-start camera's window, not camX=0x280's; the maintenance then
migrates it strip-by-strip, producing identical bytes to a recompose at the new camera),
sets tracked units, writes scroll regs, then window = X `unitX−3 … unitX+12`,
Y `unitY+4 … unitY−11` — 16×16 units = the whole 64×32 VRAM page — one row per `0x26DAC`
call, immediate. Afterwards it also:
- `move.b #$FF, $1C185A` — **clears the active-animation list**, forcing every visible
  animation to re-stamp its cell on the next `0x285DA` tick (this is why stock never shows the
  raw placeholder tiles for more than a frame), and
- writes `$26E9E[scene]` to `$140010`.

## 2. The `0x2946A` body and its feeders — the actual cause of the 357-cell diff

### IRQ3 stamper `0x2946A`

- FG: `count = $1C19AC` queued cell bytes at `$1C194A`. Per scene `$295A4[scene]` holds a pair
  `{offsetTable, patchData}` (scene 0: `$29614`, `$29A2C`). For each queued byte `c`:
  `dst = $80000 + offsetTable[c]` (word), `src = patchData + c*64`, copy 16 longs as **4 VRAM
  rows × 8 tiles** (dst rows `+0,+0x80,+0x100,+0x180`, 16 src bytes per row). Clears count.
- BG: count `$1C19AA`, bytes `$1C197A`, pair table `$295DC`, base `$82000`, `c*32`, 8 longs as
  4 rows × 4 tiles (dst rows `+0,+0x40,+0x80,+0xC0`).
- **The VRAM offsets are constants per cell byte** — they are the *torus addresses of fixed map
  cells*. Verified for scene 0: placement cell `(cX,cY)` sits at map unit `(2cX+2, 2cY+2)` and
  `offsetTable` entries equal `((0x40-(2cY+2)) & 15)*0x100 + ((2cX+2) & 15)*8`, including the
  mod-16 wrap (e.g. cX=7 → unit 16 → column 0, offset `$0C00`; cY=6 → `$02xx`). So the stamps
  are **map-anchored patches drawn through the same aliasing as the map blit** — consistent at
  any camera.

### Frame driver `0x285DA` (runs every frame, before camera update)

1. `0x28608` — **camera cull**: bounds from the FG *tracked* unit coords
   `uX=$1C17DA`, `uY=$1C17DC` (i.e., last frame's camera):
   `xlo=((uX+1)>>1)-2`, `xhi=((uX-1)>>1)+6`, `yhi=((uY+1)>>1)+2`, `ylo=yhi-8`
   (unsigned byte compares). Walks the per-scene **placement list `$2867A[scene]`** of triples
   `(cellX, cellY, animId)` terminated by a negative byte; every triple with
   `xlo<=cellX<xhi && ylo<=cellY<yhi` gets its `animId` appended to the desired list `$1C182A`
   (FF-terminated). These bounds are ~8×8 cells = the whole 16×16-unit VRAM window, so
   "active" ≈ "currently mapped into VRAM".
   Scene 0 list (`$28696`): 32 anims — cellY=9: cX 4..0xD (ids 0..9); cellY=8: cX 4..0xD
   (ids 0xA..0x13); cellY=7: cX 4..0xD (ids 0x14..0x1F order varies); cellY=6: cX 4 and 0xD.
   That's the crowd/ramp band; at camX=0x280 (uX=0x14) the active X range is cells 8..14,
   exactly the "right columns" where the user measured the diff (~20 cells × 32 tiles ≈ 357
   differing tiles after identical-tile overlap).
2. `0x2884E` — deactivate: ids in active list `$1C185A` no longer desired → free their
   4-byte slot in `$1C188A` (0x2F slots: `[0]=id|bit7 active|bit6 initialized`, `[1]=frame`,
   `[2]=countdown`, `[3]=sync cell byte`).
3. `0x28808` — activate: desired ids not yet active → allocate a slot.
4. Copy desired → active (`$1C182A` → `$1C185A`, 0x30 bytes).
5. `0x28896` → per-slot handler (dispatch `$28916[$1C007F]`, nearly all → `0x28B46`),
   ticking already-initialized slots first, then first-ticking new ones:
   - **Script** = `$28C66[scene][id]` (scene 0 table `$28C82`): layout
     `[0]=lastFrameIndex, [1]=syncGroup, then pairs [2+2f]=cellByte, [3+2f]=duration`.
     E.g. id 0 (`$28D12`): `07 01, (00,4)(01,4)(00,4)(01,4)(00,4)(01,4)(02,4)(01,4)` — a
     3-cell crowd cycle at 4 ticks/step.
   - **First tick** (bit6 was clear): if `syncGroup != 0`, scan other slots for the same
     `slot[3]&0x3F` and copy their `frame/countdown` (keeps all crowd sections in phase);
     otherwise `frame=0, countdown=script[3]`. Then **enqueue `script[2+frame*2]`
     immediately** — a cell entering the window is stamped the same frame it activates.
   - **Every later tick**: `countdown--`; on borrow `frame++`, reset to 0 when
     `frame > script[0]`, `countdown=script[3+frame*2]`, enqueue `script[2+frame*2]`.
   - Enqueue target: cell byte bit7 clear → FG queue `$1C194A/$1C19AC`, set → BG queue.

So per active animation one patch stamp every `duration` frames (typically 4), plus an
immediate stamp on activation (scene compose resets `$1C185A`, so also right after compose).

## 3. Q&A verdicts

1. **Maintenance** = (a) edge-strip blit: one incoming 16-unit row/column per axis change,
   armed by `0x26ADA`'s unit-coord comparison (`$1C16D2` is the dirty word, `$1C16D4` the
   direction), published by IRQ3 `0x26DAC`; (b) animation overlay: `0x285DA` camera-culls the
   per-scene placement list, ticks scripts, enqueues cell bytes; IRQ3 `0x2946A` stamps them.
2. **No clamp / no different table** in the maintenance source path — `0x26CA8` is shared
   verbatim. The stock-vs-raw-map difference is entirely the **post-blit stamp pass**
   (`0x2946A` patches). The raw map's ring-corner/hook tiles at those cells are static
   placeholder art that stock always overwrites.
3. **Scene start** composes at the scene-start camera `$1C1804/$1C1806`; the strip maintenance
   then reproduces exactly what a full recompose at any later camera would produce (same math),
   so there is no "lucky stale map content". The only persistent-but-unmaintained content is
   the last stamped animation frame of a cell that scrolled out of the cull window — it stays
   in VRAM until the edge strip overwrites it on re-entry, and is re-stamped the same frame the
   cell re-enters (activation), so it is never visible; it only matters for byte-exact
   comparisons of off-window VRAM.
4. **Engine rule**: keep the full recompose per camera change (it equals the strip-maintained
   map content). Then add the missing overlay pass, run **every frame after compose and before
   publish**, using the *previous* frame's camera units (the stock driver runs before camera
   follow):
   - Load per scene: placement triples `$2867A[scene]`, scripts `$28C66[scene]`, patch pair
     tables `$295A4`/`$295DC` (FG/BG offset+data).
   - Maintain the slot state machine of §2.5 exactly (activation on entering the cull bounds,
     sync-group phase copy, per-frame countdown, cycle reset when `frame > script[0]`).
   - On every enqueue, stamp `patchData + cell*64` (FG, 8×4 tiles) at
     `layerBase + offsetTable[cell]` — or equivalently at map unit `(2cX+2, 2cY+2)` through the
     same torus mapping the compose uses. Because a full recompose wipes previously stamped
     cells, after each recompose also re-stamp the **current frame of every active slot**
     (stock gets this for free since it never wipes; the `$1C185A=FF` reset in `0x26E66` is the
     stock analogue). That makes the engine's VRAM equal stock's everywhere the window is
     visible; for byte-exact full-VRAM parity also keep last-stamped frames of deactivated
     cells until the map blit path overwrites them.

Key addresses recap: strip lists `$26F92/$26FB2/$26FD2/$26FF2`; block tables `$2709C/$270BC`;
patch pair tables `$295A4/$295DC` (scene 0 FG: offsets `$29614`, data `$29A2C`); placement
lists `$2867A` (scene 0: `$28696`); script tables `$28C66` (scene 0: `$28C82`); state
`$1C16D2` (dirty), `$1C16D4` (dir), `$1C17DA-$1C17E0` (tracked units), `$1C182A/$1C185A`
(desired/active anims), `$1C188A` (slots), `$1C194A+$1C19AC` / `$1C197A+$1C19AA` (queues).
