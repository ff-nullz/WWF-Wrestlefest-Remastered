# Ringside weapons — the ring STEPS and the BOX (2026-08-23)

Transcribed from `reference/maincpu.asm`. Engine home: `src/weapon.c`
(objects + machine), `src/anim.c` (moves 0x70/0x1E/0x1F), `src/core.c`
(press consumer + selector row), `src/ai.c` (CPU arm), `src/hit.c`
(result 6 + the victim drop), `src/tag.c` (rescue-arm gates).

## 1. The objects

Two work-RAM object slots, stride 0x10C: **$1C0F1C** (slot 9, type 0 =
the ring STEPS) and **$1C1028** (slot 10, type 1 = the BOX/crate).
Spawned at match init `0xCF4 -> 0xFFD2`:

| field | steps | box |
|---|---|---|
| +0x00 | 0x8000 | 0x8000 |
| +0x74 type | 0 | 1 |
| +0x02/+0x03 | 0x000F (sprite sheet 0x0F) | 0x000F |
| x,y (0xFFEE/0x10022) | 0x40D, 0x140 | 0x330, 0x119 |
| z | 0x100 | 0x100 |
| +0x1C | 0 (hidden) | 0 |

Sheet 0x0F poses: `type*3` rest, `+1/+2` tumble frames (so 0..2 steps,
3..5 box). Verified visually (`WF_POSETEST=15,0` / `15,3`).

The ring-out scene enter (`0xFA14/0xFA26`) sets both `z = 0x100,
+0x1C = 3` — they appear on the ringside floor; the machine `0xFDEE`
(frame list 0xFFA/0x109E) runs only while `$1C0161` b1 is set.

## 2. The machine (0xFDEE, jump table 0xFE22 on +0x1C & 0xF)

- **0 hidden** (0xFE36): sprite 0xFFFF.
- **1 carried** (0xFE3E): sprite hidden — the carry is baked into the
  HOLDER's own sheet poses; tracks the holder: facing copied, x/y his,
  z + 0x50, x ± 0x20 toward the facing.
- **2 tossed** (0xFE82): init = launcher `0x258E` **class 0x10**
  ({vx -64, vz 256, grav 72} of `knockback_launch` 0x25CA), pose
  `type*3`, list 0, +0x76 cleared; flight integrated by 0x2208 with the
  ±0x20 bounds probes (0xFEBC/0xFECE) whose banked push lands it
  (+0x37 b4). Engine simplifies to velocity/gravity + the z = 0x100
  floor plane (TODO EXACT: the wall probes).
- **3 resting** (0xFF1E): pose `type*3 | facing`, list 2 (+0x12 = 2),
  **+0x76 cleared every tick** (pickup reservations are frame-local).
- **4 tumble** (0xFF54): list 2; every 0x10 ticks +0x24 += 1, pose
  alternates `type*3+1 / +2` (+0x25 b0); at +0x24 == 4 -> state 3.

## 3. Man-side state

`+0x74/+0x75` word (engine `eng_obj.f74`): byte +0x74 b7 = holding, low
byte +0x75 = the weapon type. `+0x76` (engine `wobj`) = the weapon link.
Carrying changes the man everywhere the ROM tests +0x74 b7:

- stand `0x1156A`: stance record +0x4C = **0xE**, pose 0x7E / 0x82.
- walk `0x116F0/0x11710/0x11786`: record 0xE (sub < 5), cells 0x11622
  (sprites 0x7C-0x7F) / 0x1163A (0x80-0x83), speed -8.
- tie-up ineligible (`0xF7E8`), no 0x7B escape roll (`0xEA46`), not
  rescue-armed by the pin/behind arms (`0x20F92/0x21096/0x21184/
  0x21630/0x2177E`, tag.c).

## 4. Pickup — move 0x70 (cells 0x19E80/0x19E90, handler 0x19EA0)

HUMAN: press consumer `0xDEBC -> 0xF0BA` (before the 0xDF3A category
chain): ringside scene + not holding + OUTSIDE (+0x33 b2) + state 0/1;
probe `0xF106/0xF12E`: weapon +0x76 == 0, +0x1D == 3, |dy| < 0x20,
|dx| < 0x20 -> both sides linked (0xF164/0xF168), move 0x70 (0xF0F4).

CPU: `0x1C6B6` arm (after the wants-tag eval): ringside scene, not
holding — `0x1C70C` scans both weapons (alive, state 3, unreserved),
takes the nearer by |dx| (0x1C77A) if closer than the opponent
(0x1C796 vs +0xB0); roll `0x23ED0[id] -> [stage] pair` (0x1C79E,
jsr 0x24CC), 0 = go: +0x76 = weapon, walk-to sub 0xA to it, move 0x70
on arrival (0x1C7C6 — set directly, not selector-routed).

Handler: snaps the man to the weapon spot and its facing, weapon
+0x1C = 1; at FE stands with +0x18 b7 and y -= 0x10.

## 5. Swing — moves 0x1E/0x1F (cells 0x159F8/0x15A0C, handler 0x15A20)

Selector row `0xE002` (FIRST of the chain): weapon held + state 0/1 ->
**cat 0x11**; every ws_move_map row maps cat 0x11 to 0x1E. CPU close
combat: `0x1CABA/0x1D032` — dx < 0x68 swings 0x1E at once (0x1CD4E,
not vs a lying man), 0x68..0x90 rolls the 0x1D066 {50,50} pair for the
0x1F throw (0x1D058). Both ids share the handler.

Cell 1 arms **+0x4C = 0xF** (hit record 0xF: flags 0x80, abox 6,
damage 0x14, result handler **6**). Result 6 (`0x24640`): announce
$0F28, sounds 0x2E+0x32, react 3 (4 if the victim ran, +1 damage
0x24DDC), behind remap 0x24D7A. On the connect (+0x92 pairing) the
weapon is tossed forward — x ± 0x60, z + 0x40, +0x1C = 2
(0x15A5A-0x15AAE); a whiff releases it at FE with z + 0x10
(0x15AB0-0x15B12). One swing per pickup, hit or miss.

## 6. Drops

- Climb back in (`0x19A66` in move 0x69's ringside init): weapon
  +0x1C = 2, +0x74 cleared — you cannot bring it into the ring.
- Struck while carrying (`0x24E02-0x24E20`): dropped in place, +0x44
  cleared.
- Both scene loops (`0xFA58` enter / `0xFC0A` leave) clear +0x74.

## 7. Engine notes / simplifications (TODO EXACT)

- Tossed flight: floor plane only, no ±0x20 wall probes (§2).
- The pickup |dy| test is a word compare (ROM `cmpi.b` compares the
  difference's low byte — same result in the ringside y range).
- CPU arm: engine gates on f33 b2 (outside) — stock filters the inside
  legal man off through the 0x1C6D4 go-home branch instead.
- Scalars live in the synthetic `rules/weapon_rules` table (spawn
  spots, pickup window, carry offsets, tumble timing).

## 8. Repro

    WF_NOINTRO=1 WF_DBGSEL=1 WF_OUT2=20 WF_WOUT1=30 \
      WF_P1="40-41:10,100-101:10" ./wfengine --headless --no-front \
      --frames 160 --drive script
    -> "wpn: P1 picks up weapon 1 (type 1)", "sel: P1 cat=11 entry=1E",
       "wpn: weapon 1 lands ..., tumbles"; a close-range press logs
       "hit: P1 rec 0F res 6 -> Pn react 03". WF_P1="40-41:10,50-250:4"
       climbs in instead: "wpn: P1 drops weapon 1 at (...)".
