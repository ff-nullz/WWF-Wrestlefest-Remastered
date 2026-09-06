# The behind grab — moves 0x1C / 0x1D, victims 0x52 / 0x53 (transcribed 2026-08-23)

Source: `../wrestlefest-decomp/reference/maincpu.asm`. Object stride
0x10C; A0 = self, A2 = partner (+0x26). Field names as in submissions.md.
Engine: `src/anim.c` (`handler_behind1C/1D`, `handler_behindvictim`,
`handler_behindesc`, `handler_behindoff`, `handler_dtbrace`, `mash_tick`,
`behind_off`), `src/hit.c` (`behind_hit`), `src/tag.c`
(`eng_tag_arm_holder`, `eng_tag_arm_behind_holder/victim`), `src/ai.c`
(`held_move` + the 0x1E40A wait).

## 0. In one paragraph

Selector category 0x10 (0xE382: opponent state 4 react 1 = dizzy, same
facing, |dy| < 0x10, |dx| < 0x50, from state 0/1) links the pair (0xF178,
victim +0x52 = 0) and gives **B1 → move 0x1C** (the standing rear hold,
Earthquake's "sleeper") or **B2 → move 0x1D** (grab, spin him to face your
corner, hold him up for the partner). Both catch at the end of their frame 0
and write the victim into a self-drawn held move (**0x52 / 0x53**) seated at
the holder's `(x, y-1)` plus a per-class x offset; the held man mashes
(0x10C60 seed 3 / 2, 0x10D04 per frame) and the next press after 0x4000
substitutes the escape (**0x58 / 0x57**, 0xEBC4 ladder), which scripts the
holder into a throw-off (**0x6D / 0x76**, then down react 2). 0x1C loops
dealing 4 per 44 ticks with the 0x111C8 KO check (→ 0x6F / 0x6E); 0x1D deals
nothing — it only turns the victim toward the partner and arms him (0x20F04)
so the partner runs in and punches. A strike on the pair runs the reaction
handlers 1..4 (the double-team punch: held man flinches, holder braces in
0x65 / 0x73 / 0x74 for one cell and comes straight back with the once bits
kept — 0xC01C / 0xC01D).

## 1. Records

| move | record | handler | cells |
|---|---|---|---|
| 0x1C | 0x15668 | 0x1567C | mode 1, n 3, dur 0xC/0x16/0x16, spr 0x161/0x162/0x163 |
| 0x1D | 0x1585C | 0x15870 | mode 1, n 3, 0xC/0x16/0x16, spr 0x161/0x16C/0x16D |
| 0x52 | 0x18D12 (phase 1: 0x18D22) | 0x18D2E | n 2, 0x16/0x16, spr 0x168/0x169 (flinch: 8, 0x21B) |
| 0x53 | 0x18E3C (phase 1/2: 0x18E4C/0x18E58) | 0x18E64 | n 2, 0x16/0x16, spr 0x170/0x171 (flinch: 0xC, 0x4F / 0x51) |
| 0x58 | 0x191B4 | 0x191C8 | n 3, 0x18/0xC/0xC, spr 0x181-0x183 |
| 0x57 | 0x1913E | 0x1914E | n 2, 0x10/0x10, spr 0x186/0x187 |
| 0x6D | 0x19CF2 | 0x19D06 | n 3, 0x10 each, spr 0x163/0x163/0x10 |
| 0x76 | 0x1A0FE | 0x1A10E | n 2, 0x10/0x10, spr 0x16C/0x0F |
| 0x65 | 0x19664 | 0x19670 | n 1, 8, spr 0x219 |
| 0x73 / 0x74 | 0x1A014 / 0x1A050 | 0x1A020 / 0x1A05C | n 1, 0xC, spr 0x50 / 0x52 |

Tables (now registered): `behind_grab_class` 0x18DFA (12 bytes, per-wrestler
class 0..4: `0 0 1 3 0 0 3 2 4 1 1 1`), `behind_grab_off_hold` 0x18E06
(`D8 D8 E0 D4 D8 FF`, 0x52), `behind_grab_off_turn` 0x18F08 (`D8 DC D8 D4 D8
FF`, 0x53 and the 0x1D turn); `behind_arm_holder_partner` 0x21038
(`1 1 1`), `behind_arm_victim52_partner` 0x2110E (`3E 5D 7D`),
`behind_arm_victim53_partner` 0x2127C (`7D BB BB`). Offset = signed byte
(negative = in front), negated when the pair faces right.

## 2. Move 0x1C (0x1567C)

```
init (+0x1C b7 clear): clr.b +0x01; btst #7,+0x60; beq rts      ; 0x15688: a fresh
      ; entry (0x001C) does nothing — the tick loads frame 0 and latches;
      ; re-entry from 0x65 (0xC01C): bset +0x1C b7, clr +0x24/+0x22 (tick lands
      ; on frame 1), victim +0x60 = 0xC0xx, then the catch block 0x15730
0x156AA frame != 0 && !(+0x32 b0): +0x4C = 0x0A            ; hittable (record 0xA)
0x156BE frame 0, count 0:  |dy| < 0x10, |dx| < 0x50, facing bytes equal,
      0x11412 link, victim (state 4 && react 1) || state 1 || state 0
      -> 0x15730: v.state = 5, v.+0x61 = 0x52 (LOW BYTE), v.+0x44 = 0, v.facing = own;
         bset #7,+0x60 once: v.react &= 0xFF, f33 b6 both, +0x35 b1, snd 0x32,
         0x215B6 (arm own partner, band table 0x2172C), announcer (id, 0x1D)
      miss -> 0x1578C: poison; outside -> state 1 +0xAE = 1 spr = facing; else state 0
0x157BA FE: result b7 -> both state 0 (0x15800)
      else clr +0x24/+0x22; v.dmg = 4; 0x111C8 -> own 0x6F, victim 0x6E, 0x212A0
0x1580E fx: D1 = frame (+1 if count == 0): 0 -> 0x10D3A(0); 1 -> (1),(3); else (4)
```

## 3. Move 0x1D (0x15870)

```
init b7 clear: clr.b +0x01; v.+0x52 = 0; +0x44 = 0; rts (tick latches)
init b7 set (from 0x73/0x74, 0xC01D): +0x1C b7, clr +0x24/+0x22, v.+0x60 = 0x80xx, -> 0x158DA
0x158A8 frame != 0: +0x4C = 0x0D
0x158B4 frame 0 count 0: link && v.state 4 && v.react 1 -> 0x158DA: +0x60 b7, f33 b6
      both, v.state 5, v.+0x61 = 0x53, v.+0x44 = 0; else poison, state 0
0x15910 FE: result b7 -> both stand; else clr +0x24/+0x22;
      rumble ($1C0161 b0) -> 0x1596C EVERY loop; else bset #0,+0x45 ONCE:
        facing: toward partner (+0x86).x (bcc keeps b7 = right; no partner reads
        word $0006 = 0x654 -> right) / rumble: right iff x < 0x280
        v.facing = own; v.x = x; v.y = y-1; v.x += ±off_turn[class[v.id]]
        jsr 0x20F04 (arm own partner to work the victim; 0x21038 {1,1,1})
0x159C8 fx: D1 = frame (+1 if count == 0): 1 -> (8); != 0 -> (9)
```

## 4. Victims 0x52 (0x18D2E) / 0x53 (0x18E64)

Phase = `+0x44 & 1` (0x52) / `& 3` (0x53).

```
phase 0 init: clr.b +0x01; 0x10C60(3 / 2) (no clear first — a live count carries);
      x = h.x; y = h.y - 1; (0x53: facing = h.facing word); x += ±off[class];
      0x52: bset #7,+0x60 once -> 0x2103E (arm own partner), +0x35 b2, +0x52 = 0
phase 0 frame: +0x4C = 9 / 0xC; 0x10D04; at FE: bclr #7,+0x1C, +0x24 = 0, +0x22 = 1
      (the tick restarts frame 0 — the init does NOT re-run); 0x53: bset #7,+0x60
      once -> 0x21114. (0x52 computes fx 2/5 and never spawns them: nop at 0x18DF6.)
phase 1 (0x18E0C / 0x18F0E) / 2 (0x18F38): init +0x52 += 1, +0x54 = 0x40, A1 = the
      flinch record; FE: +0x44 = 0 (0x52 also state = 5 re-init)
```

Nothing transitions here: 0xEBC4 on the next press with +0xAA == 0x4000 →
0x52 → 0x58, 0x53 → 0x57 (submissions.md §2d). CPU: row 0x1E34E / 0x1E368
timers `0x1F576[0] {4D,4D,88}` / `[1] {C3,130,1AE}`; at 0 with +0x44 != 0
(mid-flinch) → wait one tick (0x1E40A).

## 5. Strikes on the pair — reaction handlers 1..4 (hit.c `behind_hit`)

Hit records: 9 (0x52 held man, vbox 8, react 1), 0xA (0x1C holder, react
2), 0xC (0x53 held man, vboxes 8+3, react 3), 0xD (0x1D holder, react 4).

| handler | side rule | light hit (react 0 / 0xA) | heavier |
|---|---|---|---|
| 1 0x24868 | striker must face the OTHER way (0x24874 else 0x8005) | f33 b6 / f35 cleared; +0x52 < 3 → held 0xC052 +0x44 = 1, holder 0xC065; else react 2 + holder 0x6F + 0x212A0 + poison | holder 0x6F, 0x212A0, poison |
| 2 0x24924 | striker must face the SAME way | held man state 0; holder react 0xB (0x24D7A), 0x212A0, poison | same |
| 3 0x24984 | other way | react 0: +0x52 < 3 → 0xC053 +0x44 = 1, holder 0xC073; react 0xA: +0x44 = 2, holder 0xC074; else react 2 + 0x6F | react 2, holder 0x6F |
| 4 0x24A38 | same way | held man back to state 4 react 1; holder react 2 → 0xB, +0x44 = 0, 0x212A0 | same |

0x24900: announcer 0x0F19 unless the striker is the hit man's own partner
(or rumble). The braces 0x65/0x73/0x74 play one cell (fx 0xC / 0xA / 0xB)
and return to 0xC01C / 0xC01D: the holder skips frame 0 and re-seats the
victim with the once bits kept.

## 6. Escapes and throw-offs

0x58 (0x191C8): init h → move 0x6D, +0x35 b2/b1 cleared, 0x212A0; frame 1 end
snd 0x2A + h.dmg = 3; FE state 0, f33 b6 cleared both, poison; fx 6/7 while
frames 0/1 show. 0x57 (0x1914E): init y = h.y-1, h → 0x76, 0x212A0; same
frame-1 / FE shape. 0x6D: fx 4 during frames 0/1, FE → react 2 (0xA when
outside). 0x76: fx 8 during frame 0, FE → react 2.

## 7. Engine notes / TODO EXACT

- The 0xEBC4 ladder arms for 0x52/0x53 are checked at the top of
  `handler_behindvictim` (before the 0x10D04 tick, as the ROM's input phase
  precedes the anim) because `core.c` is owned elsewhere; move them into
  `walk_logic`'s switch. One frame later than the ROM's ladder.
- `core.c`'s cat-0x10 branch sets only the presser's +0x26; the handlers add
  0xF178's second half (`v.partner = self` when he has none) at their init.
- 0x20F04 / 0x2103E / 0x21114 rumble branches (0x21010 / 0x210EE / 0x2125C,
  the 0x2187E helper pick) not transcribed; the weapon (+0x74 b7) gates
  (0x20F92 / 0x21096 / 0x21184 / 0x21630 / 0x2177E) are in tag.c since the
  ringside weapons landed (weapon.c).
- The 0x1D FE tick reaches the fx block with a stale D1 — fx 9 assumed.
- Cat 0x14 (0xE442: my opponent is held by my partner in 0x25/0x36/0x52/0x53
  — the double-team selector row) is core.c work; the CPU partner's run-in
  punches come from ai.c's rescue/run-in subs, whose 0x1F692 positioning vs
  a 0x1C/0x1D holder is still approximate (in the harness run it jabbed the
  holder from the front, which the ROM shrugs off too).
- The AI 0x1E800 quirk (a CPU entering 0x1D on a NON-legal opponent converts
  to 0x1C at his state-5 link row 0x1E830) is not modelled.
- Repro: `WF_DIZZY2=30 WF_DBGSEL=1 WF_P1="40-41:10" WF_TRACE=5 ./wfengine
  --headless --frames 260 --drive script` (B2 `40-41:20` → 0x1D; add a P2
  mash script over f80-300 for the escape; `WF_DMG2=0x66` for the KO;
  `WF_CPU2=1` for the CPU timer escape; `WF_DIZZY2=200 WF_P1="210-211:10"
  --frames 600` for the partner run-in and the double-team flinch).
