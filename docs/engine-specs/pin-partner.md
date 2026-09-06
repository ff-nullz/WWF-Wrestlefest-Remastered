# The pin run-in — BOTH partners enter, and the pad follows

Transcription of the tag machinery a cover starts: who is armed, who takes the pad, how the
extra men get into the ring, what ends the pin, and how long they are allowed to stay.
Source: `../wrestlefest-decomp/reference/maincpu.asm`. Companions:
`pins-referee.md` (the count), `pin-exact.md` (moves 0x48/0x4A/0x4B/0x51), `tag-mode.md`
(§1d usher/recall, §4 rescue), `ai-behaviour.md` §7.

Field names as in `src/engine.h`. `+0xNN` = ROM object field (stride 0x10C from `$1C05B0`).
`f33 b0` = legal man, `b1` = **human-flagged** (this object reads a pad), `b2` = out of ring,
`b6` = engaged. `f56 b6` = autopilot, `b7` = CPU. `f34 b0` = rescue armed, `b1` = usher
recall pending, `b2` = rescue target set. `+0xE6` = rescue countdown. `$1C1682` = usher grace.

---

## 1. A cover arms BOTH sides (0x133E2 / 0x133EA)

Move 0x48's catch block ends with two calls, and they are not the same routine:

```
0x133DC  move.w #$ffff,($4,A0)      ; the pinner is hidden behind the composite
0x133E2  jsr    $215b6              ; A0 = the PINNER   -> arms HIS OWN partner
0x133E8  exg    A0,A2
0x133EA  jsr    $21732              ; A0 = the VICTIM   -> arms the OTHER team's partner
```

So stock WrestleFest already sends **a man in from each corner** on a cover. The same pair
runs from the squash auto-chain (`0x18C88`: `0x215B6` on the attacker, `0x21732` on the
victim) and from the 0x0E/0x35 per-frame cover block (`0x13EAA` → `0x215B6`).

### 1a. `0x215B6` — arm my own partner (A0 = the pinner)

```
0x215B6  $1C007C == 0                -> rts                       ; match not live
0x215C4  $1C0161 b0 (rumble)         -> 0x216E8 (rumble variant)
0x215D0  A0 f32 b0 (I am outside)    -> rts
0x215D8  A0 f33 b0 clear (not legal) -> rts
0x215E2  A1 = A0->+0x86              ; MY OWN PARTNER
0x215E6  A1 f32 b0 set (on the apron)-> 0x2164E
         ---- partner already INSIDE ----
0x215EE  A1 f34 |= 1 ; f34 &= ~2
0x215FA  $1C1682 = 0x00FA
0x21602  A1 f33 b1 set               -> rts                       ; he already has a pad
0x2160A  A0 f33 b1 clear             -> 0x21630                   ; I am the CPU
0x21612  A1 f56 &= ~0x40             ; autopilot OFF
0x21618  A0 f33 &= ~2 ; A1 f33 |= 2  ; ***THE PAD MOVES TO THE PARTNER***
0x21624  A1 f35 |= 8 ; rts
0x21630  A1 +0x74 b7                 -> rts
0x21638  A1 +0xB5 |= 0x80            ; CPU rescue mode
0x21642  A1 +0x7E = (A0->+0x26)->+0x86   ; = the victim's partner (intercept him)
0x21648  A0 +0x7E = A1 ; rts
         ---- partner on the APRON (0x2164E) ----
0x2164E  A1 +0xE6 = 1
0x21654  A0 move lo in {0x48,0x23,0x1A,0x0E} -> 0x21688 (keep 1)
0x21674  else A1 +0xE6 = word 0x2172C[A1 +0x70]      ; 005D 007D 00BB
0x21688  A1 f34 |= 1 ; f34 &= ~2
0x21694  A1 f33 b1 set -> 0x216B8: A1 +0x7A = victim's partner ; A1 f34 |= 4 ; rts
0x2169C  A0 f33 b1 clear -> 0x216CC: A1 f34 |= 4 ; +0xB5 |= 0x80 ; +0x7E = victim's partner
0x216A4  else            A1 +0x7A = victim's partner ; A1 f34 |= 4 ; bra 0x21618
                                                       ; ***PAD MOVES*** (note the branch
                                                       ; lands at 0x21618, skipping the
                                                       ; f56 b6 clear at 0x21612)
```

**A cover is move 0x48, so `+0xE6 = 1`: the pinner's own partner comes in on the very next
frame.**

### 1b. `0x21732` — arm the victim's partner (A0 = the victim)

Same shape, no pad juggling on this side (nothing writes `f33 b1`):

```
0x2174C  A0 f33 b0 clear -> rts
0x21756  A1 = A0->+0x86 ; f34 |= 1 ; f34 &= ~2
0x21766  A1 f32 b0 set (apron) -> 0x2179C
0x2176E  $1C1682 = 0x00FA ; A1 f33 b1 -> rts ; A1 +0x74 b7 -> rts
0x21786  A1 +0xB5 |= 0x80 ; A1 +0x7E = A0->+0x26 (the pinner) ; A0 +0x7E = A1 ; rts
0x2179C  A1 +0xE6 = 1
0x217A2  A0 move lo in {0x4A,0x64,0x5E} -> keep 1
0x217BA  else A1 +0xE6 = word 0x217F8[A1 +0x70]      ; 003E 005D 007D
0x217CE  A1 f33 b1 -> A1 +0x7A = the pinner ; f34 |= 4 ; rts
0x217E4  else f34 |= 4 ; +0xB5 |= 0x80 ; +0x7E = the pinner ; rts
```

The pinned man is in move `0x4A`, so this side is `+0xE6 = 1` too.

Timer tables (already extracted): `gr_hold_timer_2110E` `3E 5D 7D`,
`gr_hold_timer_2172C` `5D 7D BB`, `gr_hold_timer_217F8` `3E 5D 7D` — all words, index
`+0x70` (energy band).

---

## 2. The run-in itself is a CLIMB, not a teleport (0x1D526 → move 0x4E)

`0x1D526`, run every apron-sub frame:

```
0x1D532  f34 b0 clear or +0xE6 == 0        -> C=0, done
0x1D540  --+0xE6 != 0                      -> C=0, done
0x1D546  f56 |= 0x40                       ; autopilot ON
0x1D54C  f34 &= ~3                          ; disarm + no recall pending
0x1D558  $1C1684/$1C1688[side] = self       ; the man the referee will usher
0x1D56E  +0xC6 b6 -> clr, f33 |= 0x20
0x1D582  $1C1682 = 0x0177                   ; <== ALLOWED-IN-RING GRACE
0x1D58A  state = 5 ; move = 0x4E ; C=1
```

Move **0x4E** (cell `0x18A90`, handler `0x18AA0`):

```
cell 0x18A90 = { code 0x00018AA0, mode 1, n 2, delays 0x10 0x10, spr 0x0066 0x0067 }
0x18AA8 init : +0x01 = 0 ; jsr 0x24EC2 (band) ; +0x12 = 2   ; rope-lean sprite list
0x18ABC frame lo == 0xFE:
        state = 0 ; +0xC6 b6 clr ; f34 &= ~0x10 ; +0xAE (sub) = 0
        +0x04 = +0x2E (pose = facing) ; +0x12 = 0
        f32 &= ~1                                  ; NOW INSIDE
        x += (f33 b7 ? -0x38 : +0x38)              ; 0x18AEA/0x18AF8
```

**32 frames on the ropes** (sprites 0x66/0x67 — the same straddle the climb-out 0x4F plays,
run the other way), and only then does he step over the rope line. He is state 5 for the
whole chain, so no press and no AI move can fire until it finishes. This replaces the old
engine behaviour, where `eng_ai_rescue_tick` wrote `x += ±0x38` in one frame (the reported
"they just teleport in" bug).

---

## 3. The control switch, and getting the pad back (0x21618 / 0x212D4)

The ROM never swaps `+0x8A` (the port pointer) here — that only happens at the tag SWAP
`0x188DE`. Which object reads a pad is decided by `f33 b1` plus `f56 b6` (the input pass
`0xF18A` skips autopilot men). The engine's `o->input` **is** that port pointer, so it
travels with `f33 b1`.

Handing over (`0x21612-0x21624`, reached from both `0x215B6` paths when the pinner is human
and his partner is not):

```
A1 f56 &= ~0x40      ; 0x21612 — inside-partner path only in the ROM
A0 f33 &= ~2         ; the pinner stops reading a pad
A1 f33 |= 2          ; his partner starts
A1 f35 |= 8
```

Taking it back — `0x212D4` (A0 must be a legal man), called by the kick-out `0x1825E` on both
men, by `0x212A0` (which runs it over the pair) and from the hit pipeline's reaction 7:

```
0x212D4  A0 f33 b0 clear -> rts
0x212DE  A1 = A0->+0x86
0x212E2  A0 f33 b1 set:  A1 f33 b1 set -> A1 f56 &= ~0x40      ; 2P team
                         else            A1 +0xB5 &= ~0x80     ; CPU partner leaves rescue
0x21302  A0 f56 b7 (CPU): A1 +0xB5 &= ~0x80
0x2130A  else  ; A0 is neither human-flagged nor CPU == he is the man who handed his pad over
         A0 f33 |= 2      ; ***PAD COMES HOME***
         A1 f33 &= ~2
         A1 f35 |= 0x20
         A1 f56 |= 0x40   ; ***THE CPU TAKES OVER THE PARTNER***
0x21322  $1C1697 |= 1
0x2132A  A1 f32 b0 (partner outside) -> A1 f34 &= ~1, &= ~4, +0xE6 = 0
```

`0x2131C`'s `bset #6,(+0x56)` is exactly the user's "the CPU takes over the tag partner for
the short time they are in the ring".

`0x213A6` (victim freed) additionally clears `f34 b0` on the pair's partner, `f34 b2` on the
teammate and `+0xB5 b7` on the CPU rescuers.

### Deviation (this is the requested feature)

Stock only clears `f56 b6` on the hand-over when the partner is **already inside** — the man
who *runs in* keeps autopilot (`0x1D546` sets it again), so in the arcade the human does not
actually drive him. The engine clears `f56 b6` for the runner-in **when he is the one
carrying the pad**, which is what makes "P1 controls his partner during the pin" work.
Everything else on the path is the ROM's.

---

## 4. What ends the pin, and the pad each time

| ender | ROM | engine |
|---|---|---|
| 3-count | referee visual 6 `0x20022`: `+0x25 == 6` stamps `+0xFE` 0x4000/0x4001 on both pairs, `== 7` → `$8009` win pose | `handler_hold` pin branch sees `result` → pinner state 7, victim lying; `eng_tag_pin_end` |
| kick-out (move 0x4B) | `0x18216` `bclr #0,(+0x35)` on the pinner, pinner state 4 react **0x0F**, `+0x9A = 0x50`, slide `0x10B9A(0x50,-1,0x10)`; `0x1825E` `jsr 0x21282` (near-fall `$0F1E` when `+0x109 ∈ {4,5}`, `clr +0x108`) then `0x212D4` on self and partner | `handler_hold` kick-out block + `eng_tag_pin_end` + `eng_ref_digit_wipe` |
| **break by a hit** | victim record **0x1D** → reaction handler **9** = `0x24BC2`/`0x24C0C` (below) | `eng_pin_break()` in `src/tag.c` |

The referee side needs nothing extra: SM6 `0x1F9D8` falls into SM1 `0x1F9E0` while `+0x25 < 6`,
and SM1 wipes the digits (`0x206FE`) the frame `+0x35 b0` disappears. At `+0x25 >= 6` SM6
`rts`'s — once "3" is up nothing aborts it.

### 4a. The real break — reaction handler 9 (`0x24BC2`)

Hit record **0x1D** (`data/romdata/hit_record.json` row 29: `flags 0x40, vbox1 0, result 0,
reaction 9`) is written as `+0x4C = 0x801D` at exactly one site, **`0x15F22`** — the ROM's own
pin-hold connect (which also does `bset #7,(+0x60)`, `bset #0,(+0x35)`, victim `state 5 move
0x64`, `jsr 0x215B6`). "Record 0x1D" therefore means *"I am covering someone; hitting me frees
us both"*.

```
0x24BC2  victim (=the covering man) +0x64 must be 0 or 0x0A, else 0x24C24
0x24BD4  bsr 0x24900 ; A1 state = 4 ; A1 +0x64 = 2
0x24BE4  jsr 0x24D7A (behind-hit remap)
0x24BEA  A2 = A1->+0x26                       ; the man he was covering
0x24BEE  A2 state = 4 ; A2 +0x64 = 9 ; A2 +0x9A = 8
0x24C00  A1 f33 &= ~0x40 ; A2 f33 &= ~0x40
0x24C0C  A1 f35 &= ~0x01                      ; ***the referee's cue dies***
0x24C14  exg A2,A0 ; jsr 0x21282 ; exg back   ; near-fall announce + clr +0x108
0x24C1C  A1 +0x26 |= 0x80000000               ; unlink
0x24C3C  A1 +0x44 = 0
```

The victim-eligibility gate `0x24126` is what decides whether a covering man can be reached
at all:

```
0x2412A  victim +0x4C == 0                     -> reject   (no record = no hurtbox)
0x24132  victim +0x20 b7 (word b15) clear      -> reject
0x2413C  victim f32 b6                         -> reject
0x24146  victim +0xFE != 0                     -> reject
0x2414C  victim f32 b2 and not $1C0161 b1      -> reject
0x24160  victim +0x04 == 0xFFFF                -> reject   ; HIDDEN
0x2416A  bsr 0x241B6: per-swing mask +0x8E     -> reject if already hit
0x24172  |dy| < 0x0C and |dx| < 0x50
```

Two of those explain why the *dive cover* 0x48 cannot be broken the way the pin-hold can:
`0x133DC` writes `spr = 0xFFFF` (rule `0x24160`), and `0x24090` clears `+0x4C` whenever
`+0x04` changes, so the hidden pinner also has no record. **The engine deliberately keeps the
covering man in the scan**: `handler_hold`'s pin branch writes `atk = 0x801D` every frame and
`eng_hit_scan` exempts `atk == 0x1D` from the 0x24160 hidden-victim rule. Marked
`TODO EXACT` in `src/hit.c`.

Two ways in, one body:

* **through the pipeline** — any strike with a real attacker record (jab record 7, …) lands on
  the covering man, `strike()` runs the result handler, then the victim record's reaction
  handler 9 calls `eng_pin_break()`. This is also what a *mistimed* stomp does: the 0xEF9A
  proximity remap degrades an out-of-range 0x0A to a jab at `0xF02E`, and the jab breaks it.
* **from the move handler** — `handler_stomp` (move 0x0A, `0x13744`) applies its damage by hand
  and never enters `0x24062`, so it calls `eng_pin_break()` directly when its linked target is
  a covering man.

`eng_pin_break()` writes the 0x24C0C body but gives the pinner **react 0x0F** (`0x1821C`, the
kick-out's "thrown off") with `+0x9A = 0x50` (`0x18246`) instead of the ROM's react 2 — the
one deliberate value change, in `tag_rules.json` so it can be put back. Everything else
(freed man react 9 + `+0x9A = 8`, both `f33 b6` cleared, `f35 b0` cleared, `+0x109` reset,
unlink, digit wipe, pads home) is the ROM's.

### 4b. Making the stomp selectable

The downed-target rows `0xE0B8`/`0xE110` require the target in state 4 with no `+0x26` link,
which a covering man never is. `src/core.c walk_logic` therefore offers the **face-down row
(cat 6)** against a man in the engine's pin state (`eng_pin_is_pinner`) within
`|dy| < 0x18, |dx| < 0x50`; for wrestler 0 that row's B1 column is `0A` = the stomp.
`src/ai.c engine_cat` mirrors it so the CPU rescuer picks the same move. The 0xEF9A remap is
untouched: prox box 3 (`0xE9BA+0x18` = dx `0x08..0x60`, dy `±0x1E`, mirrored by the target's
facing) still has to pass, so the run-in AI walks to the covering man's **front** side
(`x = pinner.x ∓ 0x20`) before pressing — `0x1E664`/`0x1F45A`'s "beside him" placement.
`TODO EXACT`: the ROM reaches this through its generic strike categories, not a special row.

---

## 5. How long they may stay — `$1C1682`

| value | written at | meaning |
|---|---|---|
| `0x00FA` (250 f) | `0x215FA`, `0x2176E`, `0x18B40`, `0x20F5C`, `0x21086`, `0x21174` | a rescue was armed while that partner was **already inside** |
| `0x0177` (375 f) | `0x1D582` | **the run-in fired — this is the "allowed in ring during a pin" time** |

Stock consumes it in the referee: SM0 `0x1F97E` decrements it and on reaching 0 goes
`+0x20 = $8002` with `+0x56 = $1C1684`; SM2 `0x1F9F8` escorts; usher visual 7 `0x202BA` sets
**`f34 b1` (recall)** and clears `f34 b0` on both `$1C1684` and `$1C1688`. The recall is
consumed by the state-0 AI `0x1C5EC → 0x1C5FA`: a non-legal man goes **state 1 sub 4**
(walk-out `0x11ABA`) and climbs out with move **0x4F** at the rope.

Engine: `eng_tag_usher_tick()` (`src/tag.c`, called from `eng_update` before the tie-up scan)
keeps `st->usher_t` as the global mirror and a per-object clock `ai_runin_t` seeded from the
same JSON value; when it expires the referee goes SM2 and walks the escort (below); the
pointed-out man gets `f34 |= 2`, his pad (if he had one) goes home, `eng_apron_tick`'s sub-4
branch walks him to the rope and `handler_climbout` (0x4F) puts him back on the apron.

### 5a. The escort itself (SM2 → SM7, transcribed 2026-08-24)

The referee's per-SM movement runs off the visual table `0x1FB1A` (index = SM low byte):
SM1/SM2/SM4 share the approach walker **`0x1FC22`**. Its dest is `target.x ∓ 0x50` **toward
ring centre 0x280** (`0x1FC92`; only SM4 narrows to `0x30` / `y-0x10`, `0x1FC5C`) — always a
reachable in-ring spot. Arrival (`0x2062C`: |dx|,|dy| < 6) with `+0x1D == 2` → `y = target.y
+ 1`, **SM7** (`0x1FD00`). SM7's visual **`0x202BA`** faces the target and on its FIRST frame
recalls BOTH usher slots: for `$1C1684` and `$1C1688`, `f34 |= 2` unless already outside
(f32 b0), and `f34 &= ~1` always (`0x202F4-0x20322`); poses 7/8 alternate until its logic
returns to idle. SM2's LOGIC head (`0x1F9F8`) re-runs the pin hunt **`0x20556`** every frame
— any `f35 b0` cue yanks the escort straight to SM1 (a **second cover during the escort is
always counted**), any watched hold to SM4 — and completes a target who is outside or
walking out (state 1 sub 4, `0x1FA08-0x1FA1E`) by advancing to `$1C1688` or idling
(`0x1FA20-0x1FA3E`).

Engine (`src/referee.c` case 2): same shape; the usher slots are found by scanning for
`ai_runin_t` intruders. **Bug fixed 2026-08-24 (V418/V419)**: the walk dest was `±0x30 AWAY
from centre` — with the intruder near a rope the dest lay outside the ropes, `ref_probe`
clipped forever and the referee stuck in SM2 for the rest of the match (no pin was ever
counted again — the user's tag-match "everything gets confused" after a second pin). The
head hunt was also missing, so a cover during an escort went uncounted.

### 5b. Repeated pins — the re-arm rules (the user's spec, all stock)

1. EVERY cover re-runs `0x215B6`/`0x21732`, wherever the partners are.
2. Partner already **inside** (brawling, or mid-recall walking out): `0x215EE`/`0x21756`
   re-arm him — `f34 |= 1`, **`f34 &= ~2` cancels a pending recall**, and `$1C1682 = 0xFA`
   (`0x215FA`/`0x2176E`) **RESTARTS the in-ring grace**. No `+0xE6` seed: he is already in.
3. Partner back on the **apron**: `+0xE6 = 1` (`0x2164E`/`0x2179C`) — he runs in again next
   frame, and the run-in fire rewrites `$1C1682 = 0x177` (`0x1D582`).
4. The pad hand-over (`0x21618`) runs again on the new cover and the kick-out/decision
   restore (`0x212D4`) brings it home again — symmetric across any number of pins.

Engine deviation fixed with 5a (V418): `ai_runin_t` doubles as the intruder marker the run-in
gate (`core.c`) and the escort scan key on; a recall zeroed it, so a second pin's inside
re-arm left the partner out of the run-in gate and he was walked out with the fresh grace
still running. The inside branches of `arm_own_partner`/`arm_victim_partner` now restore the
marker (`ai_runin_t = 1`) for a non-legal inside partner — stock needs no equivalent because
the `$1C1684`/`$1C1688` slots persist.

### 5c. The tag SWAP's pad gate (0x188DE, fixed V419)

`0x188BC`: the pad/port move (`+0x8A` swap + `f33 b1`, `0x188EE-0x18904`) only runs when the
outgoing man is NOT the CPU (`0x188DE btst #7,(+0x56)`) and the incoming man does not already
hold a pad (`0x188E6`); a CPU swap only flips the autopilot bits (`0x18918`). The engine's
`eng_tag_swap` set `f33 b1` unconditionally, so a CPU team's tag minted a phantom pad-reader
and that team's next cover "handed the pad" with no restore ever coming back.

---

## 6. `data/romdata/tag_rules.json`

Row order **is** the slot index (`TAG_*` in `src/engine.h`); the loader
(`eng_table_rows` → `eng_tag_rule`) flattens every numeric value in `rows`, so keep each `pc`
string starting with a letter and never reorder rows. The same ROM values are compiled in as
defaults, so deleting the file changes nothing.

| slot | key | value | ROM |
|---|---|---|---|
| 0 | `usher_timer_arm` | 250 (`0xFA`) | 0x215FA / 0x2176E / 0x18B40 / 0x20F5C / 0x21086 / 0x21174 |
| 1 | **`usher_timer_run_in`** | **375 (`0x177`)** | **0x1D582 — the allowed-in-ring time** |
| 2 | `pin_arm_delay` | 1 | 0x2164E / 0x2179C (`+0xE6` for a cover) |
| 3 | `enter_cell_ticks` | 16 (`0x10`) | 0x18A90 |
| 4 | `enter_cell_count` | 2 | 0x18A90 (2 × 0x10 = the 32-frame climb) |
| 5 | `enter_step_x` | 56 (`0x38`) | 0x18AEA / 0x18AF8 |
| 6 | `pin_break_react_pinner` | 15 (`0x0F`) | 0x1821C — **deviation**, ROM 0x24BD8 writes 2 |
| 7 | `pin_break_react_victim` | 9 | 0x24BF4 |
| 8 | `pin_break_down_t` | 8 | 0x24BFA |
| 9 | `pin_break_pinner_down_t` | 80 (`0x50`) | 0x18246 |

Slots 3/4 are informational (the cell record itself comes from ROM `0x18A90`); 5 is used.

---

## 7. Code map

| file | change |
|---|---|
| `src/tag.c` | `eng_tag_rule` (JSON), `eng_pin_is_pinner`, `eng_pin_break` (0x24C0C), `eng_tag_arm_pin` (0x215B6 + 0x21732), `eng_tag_restore_control` (0x212D4), `eng_tag_pin_end` (0x212A0 + 0x213A6), `eng_tag_rescue_live`, `eng_tag_usher_tick` ($1C1682 + 0x202BA) |
| `src/anim.c` | `handler_enterring` (move 0x4E, 0x18AA0) + `case 0x4E`; cover catch calls `eng_tag_arm_pin`; pin branch of `handler_hold` writes `atk = 0x801D` and calls `eng_tag_pin_end` on the kick-out / decision; `handler_stomp` breaks a cover |
| `src/ai.c` | `eng_ai_rescue_tick` fires **move 0x4E** instead of a teleport, seeds the grace from JSON, keeps `f56 b6` clear for the pad-carrier; `runin()` walks to the covering man's front and fires the stomp; `engine_cat` mirrors the pin-break category |
| `src/core.c` | run-in men stay AI-driven for the whole grace; pin-break category in `walk_logic`; `eng_tag_usher_tick` hook |
| `src/hit.c` | victim reaction handler 9 dispatch; record 0x1D exempt from the hidden-victim rule |
| `src/main.c` | `--drive pin` modes + extra trace fields (`in= f34= e6= rt= atk= f35=`) |
| `src/engine.h` | `TAG_*` slots, the new prototypes, `eng_state.usher_t` |

---

## 8. Repro

```sh
make
tools/pin_partner_check.sh three     # 3-count: pad returns at the fall
tools/pin_partner_check.sh kick      # victim mashes out (move 0x4B)
tools/pin_partner_check.sh stomp     # the CPU rescuer stomps the pile -> break
WF_PIN=partner WF_TRACE=0x3 ./wfengine --headless --frames 480 --drive pin 2>&1 \
  | grep -E "^tr f04[01][05] o[01] "     # P1's stick moves o1, not the pinner o0
```

`--drive pin` knocks P2 down with the demo jabs and injects the 0x48 cover at f300.
`WF_PIN` picks the ender: `three` (default) / `kick` (victim topped up at f299 and mashing
from f341) / `stomp` (the other team's rescuer is allowed in) / `partner` (P1 walks right
after f380 to prove the pad moved) / **`again[:frame2]`** (the user-spec TWO-PIN cycle: first
cover kicks out, both partners run in and brawl, a second cover is injected at `frame2`,
default 600 — use ~600 for partners-inside, ~1010 for partners-back-on-apron, ~838 for
mid-recall/climb-out; both graces, both escorts and the pad round trips are in the trace).
`three`, `kick` and `partner` hold obj3 on the apron with a harness poke so the intended
ender is the one that fires.

What the trace shows in every mode (frames from the default 2-human tag):

```
f0300   o0 st=8005 mv=48                     cover launched
f0331   cover: CATCH -> pin ; o0 st=000C pin=1 in=-1 atk=801D
        tag: pad -> partner (0x215B6)        ; o1 in=0
        tag: pin armed both sides (o1 e6=1 / o3 e6=1)
f0332   ai: o1/o3 run-in 0x4E 177            ; both st=8005 mv=4E, ap=1, rt=375
f0367   o1/o3 ap=0 x += 0x38                 ; 35 frames on the ropes, then inside
...     ender:
  three f0653 o0 st=0007, tag: pad <- partner (0x2130A)
  kick  f0349 o2 st=8005 mv=4B, o0 react 0F, tag: pad <- partner
  stomp f0407 o3 mv=0A -> pin: BREAK (0x24C0C) pinner o0 react 0F, freed o2
f0707   tag: usher recall o1 (0x1C1682 expired)  ; 375 frames after the run-in fired
f0742   o1 st=8005 mv=4F                     ; climbs back out to the apron
```

Verified clean (no `UNROUTED` / `stuck`) at 3000 frames on `--drive demo|pin|throw|fuzz` and
on all four `WF_PIN` modes. `--selftest` digest `bca1b27688cab1fa`.

Oracle: the CPU side is visible in `tools/scenarios/tag.scn` traces (objects `$1C05B0` stride
0x10C, `+0x8A`, `+0x56`, `$1C1682`/`$1C1684`/`$1C1688`; `tools/trace_tool.py`); the ring-out
scenario header documents how a `--fuzz-bot` seed was folded into a `.scn`. Not yet captured
for this path — `TODO EXACT`.

---

## 9. TODO EXACT

- The hidden-victim exemption for record 0x1D in `eng_hit_scan` (the ROM's 0x48 pinner really
  is unhittable; only its 0x23-family pin-hold carries the record).
- `pin_break_react_pinner` = 0x0F vs the ROM's 2 at `0x24BD8`.
- The usher escort walk is transcribed (§5a) but `$1C1684`/`$1C1688` are still not modelled —
  the SM2/SM7 target and the recall come from scanning `ai_runin_t` intruders.
- `+0x7E` (the rescue link the ROM writes at 0x21642/0x216DC/0x21786) has no engine field —
  the rescue AI finds its target by scanning instead. `+0x7A` writes are mapped to `o->opp`.
- The pin-break category row in `walk_logic`/`engine_cat` is an engine addition; the ROM
  reaches the break through its generic strike categories.
- `0x216E8` (the rumble variant of 0x215B6, with the `0x2172A` `{0x46,0x1E}` roll) and
  `0x217FE` (rumble 0x21732) are not implemented.
- Standing holds (state 0x0C, `pinning == 0`) still use the old `0x213A6` stand-in break in
  `runin()` — the holder carries no hit record in the engine. Pins no longer do.
