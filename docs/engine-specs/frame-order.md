# WWF WrestleFest — live-match per-frame update ORDER (C main-loop spec)

Sources: `../wrestlefest-decomp/reference/maincpu.asm`,
`docs/frame-loop.md`, `docs/frame-list-c.md`, `docs/c-owned-machine.md`.
All PCs verified against the asm this session. Scope: ORDER and gating only;
routine internals (0x2208, 0x247C, anim tick, walk input) are other agents'.

## 0. Timing frame

57.44 Hz, 272 lines, 768 cyc/line. IRQ2 every 16 lines (raster/coin), IRQ3 at
line 248 (vblank). The main loop at **0xF9A** does one work list per frame
(lines 0..247, effectively), sets `$1C006F` bit0 ("work done"), then spins at
**0x10E8** until IRQ3 sets `$1C006F` bit1. `$1C0080` is the 32-bit frame
counter; its low bit (`$1C0083` bit0) selects the even/odd list.

## 1. The three concurrent strands

### 1a. IRQ2 — handler 0x434 (every 16 lines, NOT gameplay)

Coin/credit only. `clr.w $140016`; `bsr $4BC` service coin (`$140020` bit2 →
`$1C004F`); two calls to `0x48C` (D1 = `$140020` bit0/bit1) debouncing coin
slots via counters `$1C0054`/`$1C0078` + bytes `$1C0010`/`$1C0011`, coin pulse
`0x4F2` adds credit; `clr.w $140002` (ack); `rte`. **Gameplay input is NOT
read here** — pads are polled in the frame list (0x514E). A C port can run
this once per frame (or per coin event); nothing in the match depends on the
16-line cadence except coin debounce speed.

### 1b. Main loop 0xF9A — the gameplay tick (one even OR odd list per frame)

Head, every frame:

```
0F9A  clr.w $1C006E              ; per-frame scratch
0FA0  bsr $21B4                  ; rng_mix — RNG folds D1-D7 + frame ctr into $1C005C
0FA4  btst #0,$1C0083            ; frame parity
```

**EVEN list (0xFB0), in exact call order:**

| # | PC | what |
|---|----|------|
| 1 | `0x514E` | input poll: per live slot, skip CPU (`+0x56` bit7), read pad via `+0x8A` (slot0=port0), edge-detect into `+0xA2/+0xA4/+0xA6` |
| 2 | `0x8B3A` | START buttons / continue / player join (early-out if `$1C16C4`) |
| 3 | `0x2088E` | per live object: floor/zone byte `+0x30` from table `0x208EC` by world X/Y |
| 4 | `0x20958` | per live in-ring object (`+0x21` in {0,1,9}): **CPU AI / approach** (`0x20E00` rumble / `0x20CCC` tag) |
| 5 | `0x26936` | camera follow: average in-ring targets, Δ clamp ±4 → `$1C17E6/EA/EE/F2` |
| 6 | `0x2983C` | camera clamp: scene table `0x298B4[$1C007E]` clamps `$1C17E6`/`$1C17EA` |
| 7 | `0xF560` | **hit apply wrapper**: `0xF574` pair-collide loop (`0xF7E8` AABB per pair, apply `0xF5E4`) → `0xF98C` → `0xFD00` → `0xA0E0` announcer latch (`$1C15D2/D3`) |
| 8 | `0xF18A` | **HIT / grapple SM, humans**: per live non-apron non-CPU object, clear `+0xB4/+0xB6`, `jsr` table **`0xF1E4`** indexed by `+0x20` (state dispatch) |
| 9 | `0x1C150` | **main object SM, CPU/apron side**: 9 slots from `$1C05B0` (see §2), `jsr` table **`0x1C1F4`** by `+0x20` |
| 10 | `0x1F914` | **referee** SM at `$1C11F4`: table `0x1F952` by `+0x21`, nests `0x1FAF0`/`0x247C`/`0x27B8` (pin counts, count-out) |
| 11 | `0xF4C2` | **pose/motion pass** per live object (see §3 for internal order): state latch, floor probe `0x280DC`, anim script `0x1C03E`, screen pos `0x247C`, draw-list enqueue `0x27B8`, apply_motion `0x2208`; tail `0x24090` anim-cell-change tracker |
| 12 | `0x24062` | *(even only)* **hit detect side SM**: loop head `0x24064` = `next_live_object`; objects with `+0x4C` set & `+0x20` bit7 → `0x240BC`/`0x240D8` (AABB `0x241E8`, miss `0x2435A`) |
| 13 | `0x24E58` | *(even only)* HP drain `+0x68`→`+0x66`, stun `+0x54` decay |
| 14 | `0xFDEE` | extra objects `$1C0F1C`/`$1C1028` (if `$1C0161` bit1) via table `0xFE22` |
| 15 | `0x10120` | complement: 4 objects `$1C1134/1164/1194/11C4` (if bit1 clear) |
| 16 | `0x10E6A` | clear_flag_row: `bclr #7` on 12 stride-`$2A` slots from `$1C1258` |
| 17 | `0x7A00` | 3 stride-`$2A` objects at `$1C1450` (cage etc.) |
| 18 | `0x88A2` | *(even)* POWER-UP FG0 flash blit |
| 19 | `0x8710` | 4 extra objects at `$1C14CE` (spawn/tick, nests `0x2AEA`) |
| 20 | `0x262D2` | *(even)* **match clock**: BCD `$1C16A0/A2`, divider `$1C16A4`, HUD digits `0x2641E`, TIME UP → `0x21E6` + `$1C16C5=3` |
| 21 | `0xBB14` | *(even)* rumble next-body seat scan (`$1C1586`) |
| 22 | `0x7366` | rumble walk-in / elimination SM (`$1C0161` bit0 else RTS) |
| 23 | `0x8CF8` | match live → RTS; else YOU WON / result chrome (`0x2503C`) |
| 24 | `0x9052` | *(even)* low-HP sound sting (`jsr $2052 #$310B`) |
| 25 | `0x899C` | *(even)* "HURRY" FG0 flash at `$C1008` |
| 26 | `0x8ECC` | *(even)* rumble slow HP regen |
| 27 | `0x2836` | **sprite compile pass** → live spriteram `$C2000` (see §4) |
| 28 | `0x18C4` | *(even)* match rules seating: rumble picker / tag seat — NOT pin/win/time-up |
| 29 | `0x298EC` | scene-object flag queue drain (`$1C1808` list, table `0x299F4`) |

Then `bset #0,$1C006F` (0x1048) and spin at 0x10E8.

**ODD list (0x1054)** — same routines, different set and different head order:

`0x285DA` (scene overlay pass), `0x26A42` (arm incremental tilemap row blit →
`$1C16D2/$1C16D6`, consumed by IRQ3's `0x26DAC`), `0x26936`, `0x2983C`,
`0x514E`, `0x8B3A`, `0x2088E`, `0x20958`, `0xF560`, `0xF18A`, `0x1C150`,
`0x1F914`, `0xF4C2`, `0xFDEE`, `0x10120`, `0x10E6A`, `0x7548` (energy-meter
HUD smear `+0x6C`→`+0x6E` + FG0 blit), `0x8710`, `0x7A00`, `0x7366`, `0x8CF8`,
`0xBCFE` (rumble countdown / palette pulse), `0xC04A` ("HURRY" trigger),
`0xC710` (buy-in prompt), `0x2836`, `0x298EC`, then `bset #0` at 0x10E0.

Note the parity skew: **odd frames run camera (26936/2983C) BEFORE input**,
even frames after AI. Core spine identical on both parities:
input → AI → camera → hit apply → human SM → CPU SM → referee → motion/screen
→ extras/HUD → **sprite compile last**.

After the wake: `addq.l #1,$1C0080`; if `$1C007C` live → rumble `0x2017A` /
tag last-man checks, `jmp $F9A`; else attract-timeout exit.

### 1c. IRQ3 — handler 0x834 (line 248, the render publish)

```
0834  wait $140020 bit1 (hw vblank pin)
0842  btst #7,$1C0076  disabled → rte
084E  bset #7,$1C006F  re-entrancy guard
085A  btst #0,$1C006F  work NOT done → skip the eight (frame steal)
0866  move.w #0,$140008          ; SPRBUF latch: live $C2000 → buffered copy
086E  jsr $B4B8   ; flash-palette publish: shadow $1C0F2C → palette $180300 (2×16 words), gated $1C16C2 bit6 + /8 divider
0874  jsr $B53A   ; 96-frame vblank counter on extra object $1C0F1C (gated $1C006F bit4)
087A  jsr $27012  ; camera shake Y: table $27048[$1C1800] += $1C17EE/$1C17F2, count down
0880  jsr $27066  ; camera shake X: same via $1C1802 into $1C17E6/$1C17EA
0886  jsr $26E1A  ; SCROLL publish: camera → hw regs $100000/2/4/6 (y = -(cam-0x800)); skipped if $1C006F bit4
088C  jsr $2946A  ; BG tilemap queued-cell writer: $1C19AC count, bytes $1C194A, scene table $295A4 → VRAM $80000
0892  jsr $26DAC  ; BG tilemap row blit: if $1C16D2 bit7, 16 ptr-pairs from $1C16D6 (armed by odd 0x26A42)
0898  jsr $264AA  ; clock-digit blink palette $18071C/E (gated $1C169E bit5, last-minute $1C16A5)
089E  match dead (`$1C007C` bit7 clear) + 0x978 → attract restart jmp $52BE
08F8  bset #1,$1C006F            ; THE WAKE
0900  bsr $1E92                  ; post-wake HUD: credits/coin FG0 text
0904  bclr #7 / 0910 clr $140000 (ack) / rte
```

If the work list overran, bit0 is clear at 085A: the eight are **skipped**,
no wake, the loop finishes and waits for the *next* IRQ3 — a stolen frame,
not a crash. A C port reproduces this by simply letting a long tick eat a
vsync.

MAME draws at line-248 start from the **buffered** spriteram, so what is on
screen is the sprite list compiled one frame earlier (SPRBUF one-frame lag).

## 2. Per-object vs once-per-frame; slot order

**Slot iteration**: `next_live_object` `0x250E` walks pointer table
**`0x5122`**: `$1C05B0, $1C06BC, $1C07C8, $1C08D4, $1C09E0, $1C0AEC,
$1C0BF8, $1C0D04, $1C0E10` — 9 slots, stride `0x10C`, **always ascending
index 0→8**, skipping non-live (`+0x00` bit7 clear). Slot 0 = P1 (port 0);
slots 0–3 the four wrestlers; 4–8 extras/managers. `0x1C150` iterates the
same 9 inline (`lea $1C05B0 / moveq #8 / adda #$10C`). The referee is a
separate object at `$1C11F4` (not in the 9), ticked only by `0x1F914`.
The tenth pointer `$1C0F1C` is the extra object, ticked by `0xFDEE`/`0xB53A`.

**Per-object (loop over 0x5122 order):** `0x514E`, `0x2088E`, `0x20958`,
`0xF574` (pairwise N²), `0xF18A`, `0x1C150`, `0xF4C2` (+ its tail `0x24090`),
`0x24062`, `0x24E58`.

**Once per frame:** RNG `0x21B4`, `0x8B3A`, camera `0x26936`/`0x2983C`,
referee `0x1F914`, clock `0x262D2`, all HUD/FG0 routines, extras blocks
(fixed small sets), sprite compile `0x2836`, rules `0x18C4`, `0x298EC`,
everything in IRQ2/IRQ3.

**Inter-object ordering that matters:**
- Ascending slot order everywhere ⇒ **P1 (slot 0) is always updated before
  P2/CPU** within any given pass; a state P1's handler writes this frame is
  visible to slot 1's handler the same frame.
- **Human state machines (`0xF18A`→table `0xF1E4`) run BEFORE the main/CPU
  dispatch (`0x1C150`→table `0x1C1F4`)**; `0xF18A` skips `+0x56` bit6
  (apron/elim) and bit7 (CPU) objects, `0x1C150` handles the rest — two
  disjoint dispatches over the same slots, human side first.
- Hit application (`0xF560`) runs **before** both state dispatches: this
  frame's collisions feed this frame's state transitions.
- AI (`0x20958`) runs before hit apply — AI decides on last frame's world.
- Referee (`0x1F914`) runs after both wrestler SMs, before the motion pass.

## 3. Gameplay tick vs render publish boundary

Within `0xF4C2`'s per-object helper `0xF518` the **verified asm order** is:

```
0x280DC floor probe (if +0x01 moving) → push deltas +0x38/3A/3C into world
→ 0x1C03E anim script select (table by +0x1D: 0x12614/0x1AFD4/0x11478)
→ 0x247C  screen pos: world(t) → +0x14/+0x16
→ 0x27B8  ENQUEUE into per-priority draw list (table 0x2806 by +0x12 →
          lists $1C19D0..$1C1C50 + counters $1C19B8..$1C19CC)
→ 0x2208  apply_motion: world(t) + velocity → world(t+1)
```

i.e. `0x247C` converts the **pre-motion** world; the docs' sketch
"0x2208 → 0x247C → 0xD1FC" is the same pipeline phase-shifted one frame
(this frame's `0x2208` output is next frame's `0x247C` input). State handlers
(`0xF1E4`/`0x1C1F4`) may also call `0x2208`/`0x247C` on their own arms; the
`0xF4C2` pass is the canonical per-frame one.

**Publish boundary, in order:**
1. During the work list: `0x2836` drains the six priority lists back-to-front
   (`$1C1C50`→`1BD0`→`1B50`→`1AD0`→`1A50`→`19D0`; `0x2948` sorts 1C50/1BD0/
   1AD0/19D0), calling **`0xD1FC` per record**, which emits 16-byte sprite
   records **directly into live spriteram `$C2000`** (A2 cursor); then clears
   stale leftover records (`clr.b (3,A2)` loop at `0x28DE`,
   count bookkeeping `$1C19B2/$1C19B4`). FG0 text (`$C0000`–`$C1FFF`, e.g.
   `$C1008`) is also written directly from the work list (HUD routines).
2. IRQ3 `0x866` writes `$140008` → hardware copies live `$C2000` into the
   buffered spriteram (SPRBUF).
3. IRQ3's eight publish palette (`$180300`, `$18071C`), scroll regs
   (`$100000`–`$100006`) and BG tilemap VRAM (`$80000`).
4. Hardware draws at next line-248 from the buffer → screen lags the compile
   by one frame.

So: **gameplay tick = everything before `0x2836` in the list; `0x2836` is the
in-tick render *compile*; IRQ3 is the render *publish*** (latch + palette +
scroll + tilemap). Camera shake (`0x27012`/`0x27066`) is the one gameplay-RAM
write that lives on the publish side — it perturbs `$1C17E6..F2` *after* the
tick, so the shaken camera is what `0x26E1A` publishes and what next frame's
`0x247C` reads.

## 4. C sketch — `void match_frame(state *st)`

```c
/* One live-match frame. ROM order preserved; PCs cited.
 * IRQ2 (0x434) is coin-only and folded to once per frame here. */
void match_frame(state *st)
{
    st->scratch_1C006E = 0;                    /* 0xF9A  clr.w $1C006E   */
    rng_mix(st);                               /* 0x21B4 every frame     */
    bool even = !(st->frame & 1);              /* $1C0083 bit0           */

    if (!even) {                               /* odd-only head          */
        scene_overlay_pass(st);                /* 0x285DA                */
        tilemap_row_arm(st);                   /* 0x26A42 → $1C16D2/D6   */
        camera_follow(st);                     /* 0x26936                */
        camera_clamp(st);                      /* 0x2983C                */
    }
    input_poll(st);                            /* 0x514E  slots 0..8 asc */
    start_continue_join(st);                   /* 0x8B3A                 */
    floor_zone_update(st);                     /* 0x2088E per object     */
    cpu_ai(st);                                /* 0x20958 per object     */
    if (even) {
        camera_follow(st);                     /* 0x26936                */
        camera_clamp(st);                      /* 0x2983C                */
    }
    hit_apply(st);                             /* 0xF560: F574/F7E8 pair-
                                                  collide, F98C, FD00,
                                                  A0E0 announcer latch   */
    human_state_dispatch(st);                  /* 0xF18A → table 0xF1E4,
                                                  skips CPU+apron        */
    main_state_dispatch(st);                   /* 0x1C150 → table 0x1C1F4,
                                                  9 slots, CPU/apron side*/
    referee_tick(st);                          /* 0x1F914 ($1C11F4)      */
    pose_motion_pass(st);                      /* 0xF4C2: per object
                                                  280DC → 1C03E anim →
                                                  247C screen → 27B8
                                                  enqueue → 2208 motion;
                                                  tail 0x24090           */
    if (even) {
        hit_detect_side_sm(st);                /* 0x24062 (loop 0x24064) */
        hp_drain_stun(st);                     /* 0x24E58                */
    }
    extra_objects_a(st);                       /* 0xFDEE / 0x10120       */
    clear_flag_row(st);                        /* 0x10E6A                */
    if (even) {
        cage_objects(st);                      /* 0x7A00 (even position) */
        powerup_flash(st);                     /* 0x88A2                 */
    } else {
        energy_meter_hud(st);                  /* 0x7548 (odd)           */
    }
    extra_objects_b(st);                       /* 0x8710                 */
    if (!even) cage_objects(st);               /* 0x7A00 (odd position)  */
    if (even) {
        match_clock(st);                       /* 0x262D2 (TIME UP path) */
        rumble_seat_scan(st);                  /* 0xBB14                 */
    }
    rumble_walkin_elim(st);                    /* 0x7366                 */
    result_chrome(st);                         /* 0x8CF8 (RTS if live)   */
    if (even) {
        low_hp_sting(st);                      /* 0x9052 → snd 0x2052    */
        hurry_flash(st);                       /* 0x899C                 */
        rumble_hp_regen(st);                   /* 0x8ECC                 */
    } else {
        rumble_countdown(st);                  /* 0xBCFE                 */
        hurry_trigger(st);                     /* 0xC04A                 */
        buyin_prompt(st);                      /* 0xC710                 */
    }
    sprite_compile(st);                        /* 0x2836 → 0x28FA →
                                                  0xD1FC per record →
                                                  live spriteram $C2000  */
    if (even) match_rules_seating(st);         /* 0x18C4                 */
    scene_flag_queue(st);                      /* 0x298EC                */
    /* ---- work done: $1C006F bit0 (0x1048/0x10E0) ---- */

    coin_service_tick(st);                     /* IRQ2 0x434 folded      */

    /* ---- IRQ3 0x834: render publish ---- */
    spriteram_latch(st);                       /* 0x866 $140008: $C2000 →
                                                  buffered (drawn NEXT
                                                  frame)                 */
    flash_palette_publish(st);                 /* 0xB4B8 → $180300       */
    vblank_96_counter(st);                     /* 0xB53A                 */
    camera_shake_y(st);                        /* 0x27012 → $1C17EE/F2   */
    camera_shake_x(st);                        /* 0x27066 → $1C17E6/EA   */
    scroll_publish(st);                        /* 0x26E1A → $100000..06  */
    tilemap_cell_queue(st);                    /* 0x2946A → VRAM $80000  */
    tilemap_row_blit(st);                      /* 0x26DAC (from 0x26A42) */
    clock_blink_palette(st);                   /* 0x264AA → $18071C/1E   */
    post_wake_hud(st);                         /* 0x1E92 credits FG0     */

    /* ---- 0x10F2 after the wake ---- */
    st->frame++;                               /* addq.l #1,$1C0080      */
    match_alive_checks(st);                    /* $1C007C, rumble 0x2017A,
                                                  tag last-man; attract
                                                  timeout exit if dead   */
}
```

Fidelity notes for engine/core.c:
- Keep the ascending slot order (P1 first) — it is load-bearing for
  same-frame state visibility between objects.
- Keep `0x247C`-before-`0x2208` inside the pose pass (screen from pre-motion
  world) or you shift every sprite by one frame of velocity.
- Keep sprite compile at end-of-tick and the one-frame SPRBUF lag if
  parity with the oracle matters; a pure C engine may collapse the lag, but
  that is a visible timing change.
- The overrun rule (work not done at vblank ⇒ skip publish, steal a frame)
  is what makes long frames degrade gracefully; model it as "miss the vsync".
