# Royal Rumble mode — 68k transcription notes (2026-08-23, read-only pass)

Source: `reference/maincpu.asm`. Mode flag `$1C0161` b0 (engine `st->g161 & 1`,
armed by the game select, gameselect.c). 121 `btst #0,$1C0161` arms in the ROM;
the ones that shape the match are listed here. Everything not cited is TODO EXACT.

## 1. State

| RAM | meaning | engine |
|---|---|---|
| `$1C05B0` 9 slots × 0x10C | slots 0-3 humans (by player), 4-8 CPUs (`$1C09E0` = slot 4) | `obj[0..8]` |
| `$1C0380` words | the 6 ids at the bell (`0x10982`: humans' picks then random unpicked) | local to init |
| `$1C16A8[6]` / `$1C16B4[6]` | "picked" flags ids 0-5 / 6-11 (`0x1099E`, `0xBCD2`) | `rumble_picked[12]` |
| `$1C16A7` byte | eliminations pending an entrant (`0x1A518` ++, `0xBB74` --) | `rumble_pending` |
| `$1C1694` word | entrant timer, 0x126 frames (`0xBB38`) | `rumble_t` |
| `$1C1586[9]` words | per-slot HUD/mini-select markers: 0x8000 = re-entry menu open (humans), 0x4000 = CPU entrant queued (`0xBCC4`), 0 = none | TODO (HUD) |
| `$1C0156[4]` words | 0x3001 = this player was eliminated (`0x2021C`, HUD "continue?") | TODO |
| `$1C16C5` bits | b0 no human left (`0x20244`), b1 all markers clear (`0x201B4`), b2 a human won (`0x20254`); `== 3` → 0x1AFC ceremony; `$1C16C4 = 1` at 0x1AFCA (king) | `rumble_phase` |
| `+0x32` b4 | eliminated (victim of the out-launch, `0x19xxx`/`0x2020x` tests) | `f32 & 0x10` |
| `+0x32` b15 | queued entrant, skipped by the object pass (`0xF4CE`) and the AI (`0x1C17E`); shown as portrait row id+0x10 (`0xBC98`) | — |

## 2. Match init — `0x10902..0x10AC6` (rumble branch of 0x10504)

* Humans: slots 0-3 by seated player, `+0x56` = 0x40|slot, human bit, HP table
  `0x10830[slot]` (`0x10782`), positions `0x10ACC[6]` = (0x240,0x120) (0x2D0,0x120)
  (0x230,0x150) (0x2C0,0x150) (0x240,0x180) (0x2B0,0x180) z 0x140, facing
  `0x10AE0[6]` = R L R L R L (`0x10A52`), palettes `0x2AEA`, state 0 sub 0.
* `0x10982`: `$1C0380` filled — picks of the humans, then `6 - humans` CPU ids
  drawn by the d100 walker `0x24CC` over `0x10B5C` (4 tries) among unpicked
  `$1C16A8`/`$1C16B4`, else first free (`0x10AF0`/`0x10B26`).
* `0x10A36`: CPU slots `$1C09E0+`: `+0x32 = 1`, `+0x56` = slot byte | b7 CPU | b5 for
  id ≥ 6, `+0xB6` b7, palette, state 0, HP **0x87** (`0x1081A`).
* `0x10AC0`: `0x10718` placement.

## 3. Elimination — 0x68 rumble branch `0x198F0`

Landed outside at FE: `mover 0, state 5, move 0x7A, +0x44 = 4, $1C15D2 = id,
$1C15D3 = 0x29` ("eliminated"). Out-launchers also mark `+0x32 b4` and credit
the thrower `+0xC4++` (out-of-ring.md §?).

### move 0x7A `0x1A348` (phases on +0x44)
* phase 4 `0x1A464`: latch, mover 0, `+0x4A = 3` (camera excludes him), turn
  (`bchg +0x2E`), pose `0x1D3|facing`, `+0x22 = 0x28` wait; clears `+0x34` b6/b7 on
  self and `+0x7A`; at 0 → phase 5, turn again. Cell `0x11DCA`.
* phase 5 `0x1A4CA`: walk (`0x1174C` speed) to `(+0xBE,+0xC0)` = (0x140 | 0x3C0 by
  facing, 0xB8); arrive `0x1F15C` → `0x7744` (HUD), `0x2B58(row)` palette free,
  **slot cleared** (`clr.b (A0)`, 0x10C-byte wipe), `$1C16A7++`.
* phases 1-3 (`0x1A38E`, `0x1A3E8`, `0x1A430`): out-walk variants (+0x44 = 1..3) —
  TODO EXACT users.

## 4. Entrant spawner — `0xBB14` (frame list, when no `$1C1586` marker is 0x8000)

`$1C16A7 != 0` → `$1C1694++`; at 0x126: reset; live slots < 6 → `$1C16A7--`, pick the
side with fewer live men (`+0x56` b5 = ids 6-11), a random unpicked id (`0x24CC` on
`0xBCF8`, 4 tries, then `0x10AF0`/`0x10B26` first free), `0xBC6A`: first free slot
of `$1C09E0+`: wipe, active, `+0x56 = id (|0x20 side)`, `+0x02 = id + 0x10`
(portrait row while queued), b7 CPU, `+0x32 = 0x8005`, HP `0x1079E` (0x87),
`$1C158E[slot] = 0x4000`, picked flag set.

### Arrival — `0x7430` (HUD strip, when the portrait slide `+0x04 == 0xFF`)
`$1C1586[+0x20] = 0`, row := id, `0x2AEA`, side bit, free the portrait palette
`0x2B58(0x1C)`, **state 1 sub 0xB**, `+0x2C = 0`, `(x,y,z) = (0x350, 0x20, 0x100)`,
`bclr #7,+0x32`; human: `+0x32 = 7`; `+0x2E = 0`, mover, **`+0x56` b6 autopilot**.
Sub 0xB = `0x1D74A` walkway walk (angle 0xC0; x < 0x279 → x 0x279, angle 0;
y ≥ 0x100 → y 0x100, CPU: `+0xB6` b7; autopilot off; `+0x4A = 0`; move 0x69 climb in).
The clamps are EXACT — the climb poses 0x20-0x26 bake the whole 3-rope front
section (tiles 0x2544-46, bank 14, 7 columns at rel y +25/+41/+57), which only
overlays the real rope sprites at (0x279, 0x100). The engine snaps to the target
on pre-walk arrival (walk_to's 8px tolerance left it at (0x280, 0xF9) — the
"climb-in out of whack with the ropes" playtest bug, fixed V401).

## 5. Re-targeting — `0x20AA6` (per frame)

Every live CPU (`+0x56` b7) without pin intent, standing (state 0, `+0xB5` b3 clear)
or walking sub 0 with `+0xB6` b7: clear b7/b6, `0x20BDA` nearest live man within
0x400 (`$1C1692`/`$1C168C`) → `+0x7A`; none → `+0xB6` b6. Then every slot's `+0x7A`
top bit cleared. Humans: TODO EXACT (engine: nearest live man each frame).

## 6. Controller — `0x2017A` (per frame, match live)

b0 clear: if no `$1C1586` word is set: count live men not eliminated: humans D3,
CPUs D4; eliminated humans → `$1C0156[slot] = 0x3001`. D3 == 0 → **b0** (+ clr
`$1C169E`); D3 == 1 && D4 == 0 → **b2**: `0x206FE` wipe, `$1C1214 = 0x8009`,
`$1C0092` = winner, winner state 1 sub 0xC (walk to the king spot), first free
`$1C09E0+` slot → move 0x8E (announcer id 0x1F at (0x278,0x199,0x14E)).
b0 set: when all `$1C0156` words are 0 → **b1**. `$1C16C5 == 3` → 0x1AFC ceremony.
Re-entry: `0x18E4` — an inactive human slot with a credit (`0x55A(5)`) and START
opens `$1C1586[i] = 0x8000` → `0x9132` mini-select (TODO).

## 7. Other arms
* Referee SM5 rumble: fixed idle point (0x268, 0x158) (`0x1F9B0`).
* AI approach `0x1C958`: rumble: `bclr #6,+0xB6` set → if not `+0xB5` b7: `+0xB6` b7,
  state 0 (re-decide); else opp must be live, inside, not eliminated → approach;
  else re-decide.
* Tie-up bias table `0xF8D0` (rumble) vs `0xF878`; selector `0xEAEE` cat entry 0x49
  (rumble double cover?) — TODO.
* HP regen / result: `0x1120A` rumble branch `+0xC4++`, `0x21358`.
* HUD: rumble arms 0x7366/0x7788/0x787E/0x7C1C/0x7E8A/0x7F22/0x803A/0x8076,
  0x8BB2/0x8C4E/0x8ECC (portrait strip) — TODO.

## 7b. Pins / submissions eliminate (user-confirmed 2026-08-23)
* Referee count at 3, rumble (`0x200D4`): `0x21358(pinner)`, pinner `+0xC4++`, the
  pinned man (`+0x26`) gets `+0x32 b4`; no result words; `0x2014A` digit wipe,
  referee `$8005` idle. The victim's 0x4A handler (`0x1811C`): b4 → announcer
  (id, 0x29), pinner state 7 / cue off / divorce, victim lying 0x20 (`0x18184`).
* Submission KO rumble (`0x1120A` → `0x11284`): `+0xC4++`, `0x21358`, no result;
  the KO handler (`0x1AC96`) announces 0x29 and sets the victim's b4.
* Stand entry `0x1152A`: `+0x32 b4` → state 5 move 0x7A (+0x44 as found, 0 → phase
  table slot 0 = 0x1A464 turn/hold → phase 1 `0x1A38E` walk to the near rope
  (+0x32 = 0x1000, angle 0xC0 / 0x40 by x ≥ 0x280, hits `+0x37`) → phase 2 `0x1A3E8`
  climb out (outside bit, list 2, cell 0x1A354 sprs 0x66/0x67, FE: -0x20 step) →
  phase 3 `0x1A430` drop (list 4, launcher 0x1F, landed) → phase 4 → 5.
* Also `0x19DDE`: 0x69 climb-in rumble arm → 0x7A with b4 (TODO EXACT trigger).

## 8. Engine stage A (2026-08-23)
Init (6 men), elimination → 0x7A walk-off → slot free, entrant timer/spawn/arrival
(walkway walk → 0x69), nearest re-target, AI rumble arm, controller b0/b2 → match
result. Pins refused in rumble (no pinfalls). HUD/mini-select/ceremony TODO.
