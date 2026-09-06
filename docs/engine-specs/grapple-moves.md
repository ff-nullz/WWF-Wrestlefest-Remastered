# Grapple moves 0x29 / 0x15 / 0x17 — and the throw layer they open

Source: `../wrestlefest-decomp/reference/maincpu.asm` (read-only pass,
2026-08-22), bytes cross-checked against an interleaved flat image of
`data/rom/31e14-0.ic18` + `31e13-0.ic19`. DA12 classification verified with the
prebuilt `tools/thinker/native/libwfstream.so` decoder + raw stream headers.
Companions: anim.md (cell contract), face-tieup.md (states 0x0B/0x0C),
strikes.md (§1d selector), reactions.md (state-4 machinery, `0x258E`),
run-skid-turn.md (whip rebound).

## 0. Premise corrections (all proven below)

1. **Moves 0x15/0x17 are not "THE throws" — they are the layer that opens
   them.** Move 0x15 is the **front-facelock stance** (a looping wrench hold,
   1 HP per anim cycle). The category test `0xE1D4` (strikes.md §1d, cats
   0x0A-0x0D) matches *own state 5, anim 0x15 or 0x16* — i.e. a NEW B1/B2 press
   **inside move 0x15** picks the real throws through the stance matrix
   `0xE232`: w0 gets **0x18 (hip toss), 0x24 (suplex/slam fwd), 0x30
   (backdrop), 0x19 (falling press slam)**. Those four are transcribed here
   too (task item 3 is unanswerable without them — nothing in 0x15/0x17/0x29
   ever goes airborne).
2. **All 12 wrestler maps share these rows** (dumped from `0xE4FE` maps at
   `0xE52E..0xE7EE`): every map has cat 8 = `29 29 29`, cat 9 = `15 17 15`.
   Per-wrestler variety enters only at the cats-0xA-0xD layer (matrix +
   per-wrestler map rows + `0xE82E` zone/facing remap; w0 remap triples
   `(00,30,19)(01,19,30)(01,18,24)` swap 0x19↔0x30 and 0x18→0x24 by side).
3. **face-tieup.md §4 mislabel**: the lockup mash pick at `0x1F030` does NOT
   send the wrestler to "state 5 anim 0x29 (stagger)" — it performs **move
   0x29, the tie-up knee** (`0x1F030-0x1F040`: `+0x20=5, +0x60=0x29`,
   `bset #6,(partner+0x44)`) — the identical writes to the human cat-8 pick.
   The lockup auto-resolves by kneeing.

Support routine conventions used throughout (verified at the PCs shown):

* `0x10BD0(D0,D1,D2)` — facing-relative position add to SELF: `neg.w D0` when
  `+0x2E` bit7 (facing right), then X+=D0, Y+=D1, Z+=D2. **Positive D0 =
  backward** (opposite facing); negative = forward.
* `0x10B9A(D0,D1,D2)` (`0x10B9A-0x10BCE`) — **carry/place partner**: partner
  (`+0x26`) X/Y/Z := own X/Y/Z, then the same facing-signed add on the
  partner. D1/D2 are absolute (Y, Z-height).
* `0x10B62(D0)` — ring-clipped self nudge: `+0x3E=D0`, bounds pass `0x280DC`
  (which facing-mirrors the probe), `+0x58` cleared if X clipped
  (`+0x36&3`), X += pushback `+0x38`. Negative = forward.
* `0x258E(D0)` (`0x258E-0x25C8`) — ballistic launcher: entry
  `0x25CA + D0*6` = `{vx, vz, grav}` (8.8 px/frame); writes `+0x58=vx,
  +0x5A=0, +0x5C=vz, +0x5E=grav, +0x01=2`, and `neg.w +0x58` when facing
  right — **positive table vx = backward, negative = forward** (relative to
  the victim's own facing at launch time).
* `0x1110E` — thud sound 0x29 (0x2D outside, `+0x33` bit2).
* `$1C15D2/$1C15D3` — announcer/voice trigger pair (byte wrestler-id `+0x03`,
  byte phrase#); the reversal family writes the word `0x0F1A` (jingle).
* `0x2052(D0)` — sound queue.

---

## 1. Move 0x29 — the tie-up knee (cat 8, from lockup 0x0B)

### 1a. Entry — cat 8 test `0xE180` (exact bits)

```
0xE180  A1 = +0x26 (partner)
0xE184  +0x21 == 0x0B                       ; own state = lockup
0xE18E  byte(+0x44) bit5 clear              ; word bit13: impact cell pending
0xE198  byte(+0x44) bit6 clear              ; word bit14: partner already kneeing me
0xE1A2  bset #6, byte(partner+0x44)         ; word bit14 on PARTNER
0xE1A8  D0 = 8 -> carry (row 8 -> move 0x29 for every wrestler)
```

Partner-bit14 semantics: **mutual exclusion + victim marker.** The partner's
own cat-8 test now fails at `0xE198`, and the lockup auto-timer skips its
pick while bit14 is set (`0x1F016 btst #6,(+0x44) -> bne rts`). Bit15
(hidden-half) is NOT tested — either half of the lockup may knee; move 0x29's
init fixes the halves up. `+0x44` is rewritten wholesale at resolution, which
clears bit14 again.

AI/auto path `0x1EFAA` (state-0x0B combat row): `+0xBD` countdown seeded
0x20 (+`rng&3`; vs a human opponent re-seeded from difficulty table
`0x1F060`); on expiry, if own bit14 clear: weighted 50/50 pick (`0x24CC` over
`0x1F05E = {0x32,0x32}`); a 0 → the same three writes as cat 8
(`0x1F030-0x1F040`). So a lockup that nobody mashes out of resolves itself by
somebody kneeing.

### 1b. Cell `0x1656A` (table `0x12614[0x29] @ 0x126B8`)

```
{ handler 0x16582, mode 1 (hold-last), n 4,
  dur  0014 0008 000E 000E            ; 21,9,15,15 ticks (d+1)
  spr  00F5 00F6 00F7 01D2 }          ; F5-F7 two-man knee art, 1D2 impact flash
```

All four sprites are **DA12 interaction poses** (§6) — the victim's body is
drawn inside the attacker's frames for the whole move.

### 1c. Handler `0x16582` — full transcription

```
first frame (+0x1C bit7 clear):                    ; 0x16582-0x165A2
  0x1658A  clr.b +0x01                             ; mover off
  0x1658E  bclr #7, byte(+0x44)                    ; own word bit15 clear: I am
                                                   ;   now the DRAWN half
  0x16594  bset #7, byte(partner+0x44)             ; partner bit15: hidden half
  0x1659A  partner +0x20 = 0x00FF                  ; freeze victim; his next
           ; anim pick is the hidden cell 0x125C0 -> +0x04 = 0xFFFF, +0x01 = 0
           ; (anim.md: +0x1D==0xFF path, NOT the +0x21 freeze)
committed frames:
  0x165A4  if +0x25 == 1 && +0x22 == 0:            ; last tick of frame 1
  0x165B2      sound 0x2A                          ; knee impact
  0x165BC  if +0x25 == 0xFE:                       ; anim finished — RESOLVE
  0x165C4      partner +0x68 = 1                   ; 1 damage (word write)
  0x165CA      partner +0xC7 += 1                  ; times-hit stat
  0x165D0      partner +0x54 = 0x100               ; long combo-decay window
  0x165D6      partner +0x52 += 1                  ; combo counter
  0x165DC      if partner +0x52 < 2:               ; ---- back to lockup ----
  0x165E4          own +0x20 = 0x0B; partner +0x20 = 0x0B
  0x165F0          own +0x44 = 0                   ; drawn half, no impact cell
  0x165F4          partner +0x44 = 0x8000          ; hidden half
               else:                               ; ---- ADVANTAGE WON ----
  0x165FC          own +0x20 = 0x0C                ; hold (rear waist-lock)
  0x16602          own +0x44 = 0x2000              ; play impact cell first
  0x16608          partner +0x20 = 0x00FF          ; held victim (frozen)
  0x1660E          partner +0x44 = 0
```

So: **each knee = 1 damage; the second knee inside the `+0x54 = 0x100`
combo window (the victim's `+0x52` also counts prior strikes) breaks the
lockup into the hold 0x0C with the kneer holding.** Fewer than two → both
back into the mash contest. No position writes, no launcher, no divorce —
the pair pointer survives both exits. Victim hidden for the entire move.

---

## 2. Cat 9 — from the hold 0x0C: `0x15` (B1/both) and `0x17` (B2)

### 2a. Entry — cat 9 test `0xE1B0`

```
0xE1B4  +0x21 == 0x0C                 ; holding
0xE1BE  +0x45 == 1                    ; hold phase 1: timer +0x46 has passed
                                      ;   0xA0 (face-tieup §4: 0xE0 -> 0xA0
                                      ;   takes 0x40 frames; at 0 the hold FLIPS)
0xE1C8  D0 = 9; clr.w +0x44           ; clears phase + flags
```

Row 9 = `15 17 15` for all wrestlers. The held victim is in state 0xFF
(frozen+hidden) since the tie-up resolution (`0xF742/0xF760`).

### 2b. Move 0x15 — front-facelock stance / wrench (cell `0x14CE0`)

```
{ handler 0x14CF4, mode 1, n 3,
  dur 000A 000E 0010                  ; 11,15,17 ticks
  spr 00F8 00F9 00FA }                ; all three DA12 two-man wrench art
```

Handler `0x14CF4`:

```
first frame:
  0x14CFC  clr.b +0x01
  0x14D00  if +0x44 == 1:                          ; RE-ENTRY from stance-walk
  0x14D1E      bset #7,(+0x1C); clr +0x24/+0x22    ;   (0xDE64 writes it): mark
                                                   ;   anim inited, no nudge —
                                                   ;   resumes loop seamlessly
           else:
  0x14D08      if +0x1F == 0x0C:                   ; came from the hold state
  0x14D10          0x10BD0(0x20,0,0)               ; step 0x20 BACKWARD
  0x14D2C  if +0x44 != 3:                          ; 3 = reversal re-entry (§5):
                                                   ;   victim is already the 0x7B man
  0x14D36      partner +0x20 = 5; +0x60 = 0x7B     ; victim -> held-victim move
committed frames (0x14D44):
  0x14D44  partner X = own X; partner Y = own Y    ; CARRY, zero offset
  0x14D50  partner +0x2E = own +0x2E; bchg #7      ; face each other
  0x14D5C  if +0xFE bit7 (match over):
  0x14D64      own +0x20 = 5, +0x60 = 0x50         ; bail move (splits the pair,
                                                   ;   partner -> reaction 1 dizzy,
                                                   ;   X ±0x20 apart, both +0x26
                                                   ;   poisoned — 0x18BA0-0x18BF2)
  0x14D72  elif +0x25 == 0xFE:                     ; wrench cycle complete
  0x14D7A      clr +0x24; clr +0x22                ; loop (resumes at frame 1)
  0x14D82      partner +0x68 = 1                   ; *** 1 damage per cycle ***
```

No sound, no divorce, no exit of its own: the stance persists until (a) a new
B1/B2 press matches cats 0xA-0xD (the throws, §4), (b) the movement selector
toggles it to the walk (§2c), (c) the victim's 0x7B escape fires (§5), or
(d) match over.

### 2c. Stance-walk toggle (movement selector `0xDDC8`) and move 0x16

`0xDDC8` (runs when no attack consumed the frame): own state 5 +
`+0x61 == 0x15` + stick held (`+0xA9 != 0`) → `+0x20=5, +0x60=0x16`
(`0xDE4E-0xDE60`). `+0x61 == 0x16` + stick released → `+0x20=5, +0x44=1,
+0x60=0x15, clr +0x2C` (`0xDE64-0xDE7C`) — the `+0x44==1` no-init re-entry
above.

Move 0x16 cell `0x14D8A` = `{0x14DA2, mode 2 loop, n 4, dur 4×0x0A, spr
0x43-0x46 (all DA12)}`: init `+0x01=1` + walk-speed table `0x1174C` then
**`+0x2B >>= 1` (half speed while dragging)**; per frame: match-over → bail
0x50; angle `+0x2C` not 0/0x80 → own facing := angle bit7, partner := opposite
(`0x14DE6-0x14DF6`); `0x10B62(-0x28)` forward clip; **partner X/Y/Z = own
X/Y/Z** (`0x14E02-0x14E12`); own sprite flip bit maintained manually
(`0x14E14-0x14E28`). The victim is dragged around the ring at the holder's
exact position. Being in 0x16 also satisfies the cats-0xA-0xD test.

### 2d. Move 0x17 — the whip (cell `0x14E2C`)

```
{ handler 0x14E40, mode 1, n 3,
  dur 0008 000A 0010                  ; 9,11,17 ticks
  spr 00FE 00FF 0100 }                ; FE,FF = DA12 two-man; 100 = single (DC3C)
```

Handler `0x14E40`:

```
first frame:
  0x14E48  clr.b +0x01
  0x14E4C  partner +0x20 = 0x00FF               ; freeze+hide victim
  0x14E52  0x10B62(-0x18)                       ; clip-nudge forward 0x18
  0x14E5A  partner X/Y = own X/Y                ; snap under the art
release — last tick of frame 1 (+0x24==1 && +0x22==0, 0x14E6A):
  0x14E78  partner +0x20 = 2                    ; *** victim -> RUN state ***
  0x14E7E  bset #7, partner +0x26               ; lazy divorce, victim side
  0x14E84  bset #7, own +0x34                   ; "I whipped him" flag
  0x14E8A  bset #6, partner +0x34               ; "I was whipped" flag
           ; consumers found: AI retarget gate 0x20980 skips objects with
           ;   +0x34 bit6/7 set; cleared at 0xF630 (whip-resolution pass)
           ;   and 0x11AD6/0x11ADE (walk sub-handlers)
  0x14E90  bclr #6, own +0x33; bclr #6, partner +0x33   ; disengaged
  0x14E9C  partner +0x44 = 2                    ; *** rope-bounce budget = 2 ***
           ; run-skid-turn.md §"rebound": each rope edge -> state 6 turn +
           ;   rope-shake + +0x44 -= 1; zone-5 contact with +0x44 != 0 ->
           ;   reaction 0x17 fall with +0x68 = 8 (0x11CFA-0x11D12); this write
           ;   answers that doc's open question "who arms +0x44 for a run"
  0x14EA2  partner +0x2E = own +0x2E; bchg #7   ; victim faces AWAY
  0x14EAE  partner +0x2D = 0xC0; if partner faces right -> 0x40
           ; run angle = victim's own facing: he runs away from the thrower
  0x14EC2  0x10B9A(0x50, -2, 0)                 ; placed 0x50 BEHIND the
                                                ; thrower, Y-2 (rear waist-lock
                                                ; hurls him past/behind)
exit (+0x25 == 0xFE, 0x14ED4):
  0x14EDC  own +0x20 = 0
  0x14EE2  bchg #7, own +0x2E                   ; turn around to face the runner
  0x14EE8  bset #7, own +0x26                   ; lazy divorce, own side
```

No damage and no sound in the whip itself; the run/ring machinery owns the
sequel (rope shake `0x11D56`, hard-contact falls reaction 0x17 with `+0x68`
8/0x0A, and the anti-run categories 3/4 / counter contexts for catching the
rebound). Frame 2 (single-man art) plays after the victim is already running.

---

## 3. The throw layer (cats 0xA-0xD from stance 0x15/0x16) — w0's four

Cat test `0xE1D4`: `clr +0x44, +0x46`, `D3=1` (arms the `0xE82E` remap),
`bchg` alternation, `D0 = 0xE232[opp_heat*0x18 + bank*0xC + own id]`; matrix
bytes (0x48, dumped): rows by opponent energy band 0..2, values 0x0A-0x0D.
w0 map rows: A=`18 18 18`, B=`24 24 24`, C=`30 30 30`, D=`19 19 19`.

Common shape of all four: victim frozen at `+0x20=0xFF` (hidden) on frame 0;
DA12 two-man art while carried; at the release tick the victim is written
`+0x20=4, +0x64=<air reaction>` + damage `+0x68` + `bclr #6` both `+0x33`;
the attacker finishes his follow-through on single-man art and exits with
`bset #7,(+0x26)` (lazy divorce `0x115D2`). Voice trigger at start.

### 3a. Move 0x18 — hip toss (cell `0x14EF0`)

```
{ 0x14F08, mode 1, n 4, dur 000C 0008 0008 000C, spr 0101 0102 0103 0104 }
                                    ; 101-103 DA12, 104 single
init:    clr +0x01; partner +0x20 = 0xFF; voice(id, 2); +0x19 = 0x10 (X off)
release (end of frame 2, 0x14F34):
         partner +0x20 = 4, +0x64 = 0x22        ; air reaction 0x22
         partner +0xC7 += 1; partner +0x68 = 0x0C          ; *** 12 damage ***
         sound 0x32
         partner +0x2E = own, bchg #7           ; faces away
         bclr #6 both +0x33
         0x10B9A(0x50, 1, 0x50)                 ; released 0x50 BEHIND,
                                                ;   0x50 IN THE AIR (over the head)
exit (+0x25==0xFE, 0x14F8E): bset #7 +0x18 (offset clear); own +0x20 = 0;
         bchg #7 +0x2E (turn to face the thrown man); +0x04 = +0x2E (frame 0);
         bset #7, +0x26
```

### 3b. Move 0x24 — forward suplex/slam (cell `0x1603A`)

```
{ 0x16056, mode 1, n 5, dur 0006 000A 0010 0006 0012, spr 0107..010B }
                                    ; 107-109 DA12, 10A/10B single
init:    clr +0x01; 0x10B62(-0x18); partner = 0xFF;
         voice(id, 1) — or (id, 0x27) when own id == 8 (0x16078-0x16094)
release (end of frame 2, 0x16098):
         partner +0x20 = 4, +0x64 = 0x16        ; air reaction 0x16
         partner +0xC7 += 1; partner Y -= 1
         partner +0x68 = 0x0E                   ; *** 14 damage ***
         sound 0x32; partner facing: bchg #7 only (kept relative)
         0x10B9A(-0x10, 0, 0x40)                ; 0x10 IN FRONT, 0x40 up
exit:    own +0x20 = 0; +0x04 = +0x2E; 0x10BD0(0x18,0,0) step back; bset #7,+0x26
```

### 3c. Move 0x30 — backdrop / back suplex (cell `0x16E28`)

```
{ 0x16E48, mode 1, n 6, dur 0010 0008 0006 0012 0016 000E,
  spr 010D 010E 010F 01F0 01F1 01F2 }        ; 10D..1F1 DA12, 1F2 single
init:    clr +0x01; +0x1B = 0x20 (Y off)
         0x11412 link check (partner->+0x26 == self; movem/cmpa at
             0x11412-0x11430) — stale -> bset #7 +0x26, own +0x20 = 0,
             A1 = hidden cell 0x125C0 for this frame (abort, 0x16EE0)
         partner +0x20 = 0xFF
         roll 0x1129E(D1=1, D2=+0xEA, +0xEA += 1)          ; fumble check (§7)
             fail -> own +0x20=5, +0x60=0x7E (REVERSAL, §5) and stop
         if +0x46 != 0 (AI/behind-take writers; cat test cleared it):
             bset #7 +0x1C, clr +0x24/+0x22; partner facing := own;
             bchg own +0x2E (turn); own X := partner X; 0x10BD0(-0x18);
             0x10B62(-0x38)                                 ; step behind him
         voice(id, 4)
frame 2 end:  +0x19 = 0xE0 (X off -0x20)
frame 3 end:  sound 0x2B (swing)
release (end of frame 4, 0x16F22):
         +0x19 = 0
         partner +0x20 = 4, +0x64 = 0x13        ; air reaction 0x13
         partner +0xC7 += 5
         0x110E0                                 ; singles: victim's tag partner
                                                 ;   on apron (state1/+0xAF==1)
                                                 ;   gets +0x34 bit5 = run-in
         partner +0x68 = 0x11                   ; *** 17 damage ***
         sound 0x32
         if own id == 0 or 5: partner +0x68 += 2 ; *** 19 for Hogan/Warrior ***
         bset #5, byte(partner+0x64)            ; word bit13 = rise-dizzy
         bset #3, partner +0x32                 ; slow-walk after rising
         0x10B9A(0x50, 0, 0x10)                 ; 0x50 behind, 0x10 up
         bclr #6 both +0x33
exit:    own +0x20 = 0; 0x10BD0(-0x38) forward; 0x1108C (CPU pin-intent roll:
         rng&0xFF < word 0x110C8[id] -> bset #4 +0x34; w0 row 0x88 ≈ 53%);
         bchg #7 +0x2E; +0x04 = +0x2E; bset #7 +0x18; bset #7 +0x26
```

### 3d. Move 0x19 — falling press slam (cell `0x14FB6` + variant `0x14FDE`)

```
0x14FB6: { 0x15006, mode 1, n 8, dur 000C 0008 0004 000A FF00 000C 0010 0010,
           spr 01F7 01F8 01F9 01FA 01FB 01FC 01FD 01FD }   ; 1F7-1FC DA12
0x14FDE: same handler/durs, spr 01F7 01F8 01F9 01FA 01FD 01FD 01FD 01FD
         ; the +0x60 bit7 variant: post-release frames single-man
tail every frame (0x151FC): if +0x60 bit7 set -> A1 = 0x14FDE
```

Handler `0x15006`:

```
init:    clr +0x01; partner +0x20 = 0xFF; +0x19 = 0x2E, +0x1B = 0x4E
         roll 0x1129E(D1=1, D2=+0xF4, +0xF4 += 1)
             fail -> own +0x20=5, +0x60=0x83 (BOTCH, §5) and stop
         0x10B9A(0xF0, 0, 0)                    ; park hidden victim clear of play
         voice(id, 7)
frame 2 end (0x1507C):  0x258E(0x16)            ; *** ATTACKER goes ballistic:
                                                ;   vx=+0x100 (backward 1.0 px/f),
                                                ;   vz=0x300 up, grav 0x48 ***
frame 3 end (0x15068):  +0x19 = 0x7F
frame 3 end, singles only (0x15094-0x15118): edge test: +0x3E=0x80 probe via
         0x280DC; if X clipped (+0x36&3):       ; *** OVER-THE-ROPES VARIANT ***
             bset #7 +0x60 (variant cell); +0x04 = 0x1FD|flip now
             partner +0x20 = 4, +0x64 = 0x25; bset #5 (bit13 rise-dizzy)
             clr partner +0x44; bchg partner +0x2E
             0x10B9A(0x80, 1, 0x39)             ; 0x80 behind, 0x39 up
             0x11058                            ; shake the ropes on that side
             bclr #6 both +0x33                 ; (damage comes at 0x15186 below)
landing (any frame, +0x37 bit4, 0x1511A): 0x1110E thud; clr +0x01;
         clr +0x22 (releases the FF00 hold on frame 4); $1C1800 = 0xE (shake)
release (end of frame 5, 0x15138), if bit7 clear (normal path):
             partner +0x20 = 4, +0x64 = 0x21; bset #5 (rise-dizzy)
             partner +0x2E = own, bchg; 0x10B9A(0xF0, 1, 0)   ; dropped 0xF0 behind
             bclr #6 both
         both paths (0x15186):
             partner +0xC7 += 5; bset #3 partner +0x32 (slow-walk)
             0x110E0 (tag run-in); 0x1108C (pin-intent roll)
             partner +0x68 = 0x14               ; *** 20 damage ***
             sound 0x32
frame 6 end: bset #7 +0x18; 0x10BD0(0x30,0,0) step back
exit (+0x25==0xFE): own +0x20 = 7               ; *** attacker RISES from the
                                                ;   mat (state 7 get-up) ***
         0x1108C again; +0x04 = 0x68|facing; bset #7 +0x26
```

---

## 4. Victim side — the air reactions and the landing chain

The throws do NOT use the strike knockdown ids 2/3/4. Each has a dedicated
air reaction; **every one funnels into bounce (5) → lying (8/9) → state 7
rise → optional dizzy (reaction 1) via `+0x64` bit13** (reactions.md §2).
Launch table rows (`0x25CA + i*6`, {vx, vz, grav}, 8.8 px/f; vx sign:
positive = backward, negative = forward, relative to the victim's facing at
launch — `0x258E` negates on facing-right):

```
[0x06] vx=-0x100 vz=+0x300 grav=0x38    ; reaction 0x13 (backdrop)
[0x0C] vx=-0x100 vz= 0     grav=0x28    ; reaction 0x16 in-ring (suplex, launched
                                        ;   from Z=0x40 carry height)
[0x16] vx=+0x100 vz=+0x300 grav=0x48    ; move 0x19 ATTACKER's leap
[0x18] vx=-0x180 vz=-0x100 grav=0x58    ; reaction 0x22 (hip toss: released at
                                        ;   Z=0x50 moving away and DOWN)
[0x20] vx=-0x480 vz=+0x400 grav=0x58    ; reaction 0x16 thrown OUT of the ring
[0x25] vx=-0x140 vz=+0x200 grav=0x40    ; reaction 0x25 (press slam over ropes)
[0x03] vx= 0     vz=+0x300 grav=0x38    ; reaction 0x21 (press slam drop: straight
                                        ;   pop-up where he was dropped)
```

* **0x13** (`0x1B72E` `{0x1B73E, mode1, n2, dur 0x10,FF00, spr 0x11,0x14}`):
  init `0x258E(6)` + `0x10F9C(0x30)` ring-band Y fix; per frame `0x10FC6`
  rope events; on `+0x37` bit4 → thud, `+0x20=4, +0x65=5` (bounce), sprite
  `0x1F|flip` immediately (`0x1B75C-0x1B786`).
* **0x16** (`0x1B8BA` `{0x1B8CA, n2, dur 6,FF00, spr 0xB2,0xB3}`): init runs
  a ring-corner diagonal test (`0x1B8F0-0x1B930`, `(Y<<8 ± 0x9C000/0x48000)
  / 0x2E0` vs X by facing) — beyond the line (singles, arena byte
  `$1C007F != 1`) → `0x258E(0x20)` + `bset #2,+0x33` (**thrown out of the
  ring**; tag: also `bset #4,+0x32` forced-rise, thrower `+0xC4 += 1`); else
  `0x258E(0x0C)` + `0x10F9C(0x30)`. Crossing the ropes (`+0x36==6`, once via
  `+0x45` bit0): sounds 0x28+0x32, rope-shake globals (`0x1B976-0x1B9CA`).
  Landing in-ring → bounce 5 + `0x10BD0(-0x38)` + thud (`0x1BA1A`); landing
  outside (singles) → `+0x20=5, +0x60=0x68` (outside-landing move) + sound
  0x28 (`0x1BA48`).
* **0x21** (`0x1BD66` **mode 0**, handler `0x1BD6C`): init `0x258E(3)`,
  `clr +0xAA`, mash seed `0x10C60(1)`; then **substitutes the bounce cell
  `0x1B1B8`** for the tick (spr 0x1F). Per frame `0x10D04` down bookkeeping +
  `0x10B62(0x50)` clip; landing → thud, `+0x65 = (+0x71==2 ? 9 : 8)` —
  straight to LYING, face-down in energy band 2 (`0x1BDB2-0x1BDCE`).
* **0x22** (`0x1BDD8` `{0x1BDE8, n2, dur 8,FF00, spr 0xB2,0xB3}`): init
  `0x258E(0x18)` + `0x10F9C(0x30)`; end of frame 0: `Z -= 0x10`; landing →
  thud, `clr +0x42`, bounce 5, sprite 0x1F, `0x10BD0(-0x38)` forward
  (`0x1BE3A-0x1BE68`).
* **0x25** (`0x1BF08` `{0x1BF1C, n3, dur 8,C,FF00, spr B3,B2,B3}`): init
  `0x258E(0x25)`; `+0x44 != 0` → `neg +0x58` (direction override hook);
  `bchg #7,+0x2E` (flip); `0x10F9C(0x30)`; landing → thud, bounce 5,
  `0x10BD0(-0x38)` (`0x1BF4E-0x1BF92`).

Chain confirmation: bounce 5 (`0x1B1B8/0x1B1D0`) re-launches `0x258E(3)`,
seeds pin-mash (`0x10C60`), lands into lying 8/9; lying → state 7 rise;
rise pops reaction 1 (dizzy) when word bit13 was set by the thrower
(reactions.md §2c-2e). `+0x42 = 0xFFE0/0xFFF0` writes during flight are the
float/hover block (cleared on landing); recorded as-is.

---

## 5. The reversal/escape web (what ends a hold that nobody throws)

* **Move 0x7B — held victim** (cell `0x1A532`, **mode 0**, handler `0x1A538`;
  no art of its own): first frame `+0x04 = 0xFFFF` (self-hide); holder
  enraged (`+0x33` bit5) → `bset #7,+0x60` (**escape attempt disabled** — the
  same bit `0xEA42` sets after the one allowed mash-roll); unless `+0x44==1`
  (re-entry) or previous anim was a state-5 move: seed **`+0x44 = 0x80`
  escape countdown, or 0xBB when the holder is CPU** (`0x1A554-0x1A578`).
  Per frame `--+0x44`; at 0: own `+0x20=5, +0x60=0x7C`, **holder → state
  0xFF** (`0x1A57A-0x1A590`). Player escape: `0xEA42` (strikes §1b) rolls
  once on a press while in 0x7B → 0x7C early.
* **Move 0x7C — break free** (cell `0x1A594` `{0x1A5A0, mode1, n1, dur 8,
  spr 0x221 (DA12)}`): init clears both movers, `bchg` own facing, partner
  frozen+hidden (`0x1A5B6-0x1A5C0`); on 0xFE: **own `+0x20=5, +0x60=0x17` —
  the escapee whips the former holder**, partner stays 0xFF, `bchg` facing
  back, `0x10BD0(0x20,0,0)` (`0x1A5CC-0x1A5F0`). So the wrench hold always
  ends in someone getting whipped.
* **Move 0x7E — fumbled backdrop** (cell `0x1A674` `{0x1A688, mode1, n3,
  dur 0x10,0x1C,0x0C, spr 0x10D-0x10F}` — the backdrop's own first sprites):
  init `+0x44=2`; each 0xFE decrements and replays; at 0: jingle `0x0F1A`,
  **roles swap** — own `+0x60 = 0xC07B` (held victim, escape pre-disabled
  bits), own `+0x44=1`, own sprite 0xFFFF; partner `+0x20=5, +0x60=0x15`
  with **partner `+0x44=3`** (the §2b path that skips re-entering 0x7B —
  A0 already is the 0x7B man), partner sprite `0x39|flip`, offsets cleared
  (`0x1A69E-0x1A700`). Moves 0x7D/0x7F/0x81/0x82 (`0x1A604/0x1A718/0x1A7B4/
  0x1A832`) are the same reversal with different struggle art/timers
  (0x7D: timer 0x50, spr 0x14E/0x14F — pose absent for w0).
* **Move 0x83 — fumbled press slam** (cell `0x1A8A6` `{0x1A8B6, mode2, n2,
  dur 0x18,0x10, spr 0x1F7,0x1F8}` — the lift art straining): timer
  `+0x44=0x50`; at 0: jingle 0x0F1A; **own `+0x20=4, +0x64=0x0E` (grabbed)
  with own `+0x68=8` — the attacker eats 8 damage**; partner `+0x20=5,
  +0x60=0xC006` (**the victim performs move 0x06 on him**); own sprite 0x2B,
  own Z += 0x48; partner Y += 1, sprite 0x60; `+0x92` cross-links; `bclr #6`
  both (`0x1A8CA-0x1A93C`).

## 6. DA12 interaction art — per-frame classification (wrestler-0 streams)

Pose type read from the stream header (`0x38F14` row-offset table →
`0x38FB8[row]` stream; header bits 15-14: 0=D2AE body, 1=D802 packed,
**2=DA12 interaction**, 3=DC3C slices; nested-DA12 checked via
`libwfstream.so` decode). Result:

| cell | DA12 frames | single frames | victim object state there |
|---|---|---|---|
| mv 0x29 `1656A` | spr F5,F6,F7,1D2 — ALL | — | 0xFF (frozen+hidden) whole move |
| mv 0x15 `14CE0` | F8,F9,FA — ALL | — | state5 move 0x7B, self-hidden 0xFFFF |
| mv 0x16 `14D8A` | 43,44,45,46 — ALL | — | still 0x7B, carried at own X/Y/Z |
| mv 0x17 `14E2C` | FE,FF (frames 0-1) | 100 (frame 2) | released to state 2 at end of frame 1 |
| mv 0x18 `14EF0` | 101-103 (0-2) | 104 (frame 3) | released (react 0x22) end of frame 2 |
| mv 0x24 `1603A` | 107-109 (0-2) | 10A,10B (3-4) | released (0x16) end of frame 2 |
| mv 0x30 `16E28` | 10D-10F,1F0,1F1 (0-4) | 1F2 (frame 5) | released (0x13) end of frame 4 |
| mv 0x19 `14FB6` | 1F7-1FC (0-5) | 1FD (6-7) | released (0x21) end of frame 5 |
| mv 0x19 variant `14FDE` | 1F7-1FA (0-3) | 1FD (4-7) | released (0x25) at frame-3 edge check |
| mv 0x7C `1A594` | 221 | — | 0xFF |
| lockup 0x0B / hold 0x0C | 39,3A,3B,3F,1D2 — ALL | — | other half hidden via +0x44 bit15 / 0xFF |

**Rule for the engine**: the release tick and the art switch coincide exactly
— arm the partner row for every frame in which the victim is at `+0x20==0xFF`
(or lockup-half `+0x44` bit15); disarm from the release write onward, where
the victim draws himself as a state-4 reaction. Move 0x19's `+0x60` bit7
variant exists precisely to swap the tail frames to single-man art when the
victim leaves early. The victim-side art poses (0x11,0x14,0x1F,0xB2,0xB3,
0x12,0x13,0x68) are all single-body (D802/DC3C). (Bank/palette: grapple DA12
streams carry each figure's real id — palette-banks.md.)

## 7. The fumble roll `0x1129E` (D1=1) and friends

```
0x11302 (D1=1; D2 = per-move attempt counter, +0xF4 for 0x19, +0xEA for 0x30):
  own CPU (+0x56 bit7)         -> carry SET (CPU never fumbles here)
  vs human victim              -> tbl 0x113FF = 10 0E 0A 08 07
  vs CPU victim, singles       -> tbl 0x113CD + difficulty*5:
       diff0: 10 0F 0C 09 07 ... diff9: 10 0B 07 06 05   (10 rows dumped)
  vs CPU victim, tag           -> tbl 0x11404 = 10 0F 0D 0B 08
  t = tbl[min(D2,4)]; success iff (rng 0x21B4 & 0xF) <= t  (carry set)
```

First attempt t=0x10 → never fails; the counters **never reset**, so the 2nd,
3rd… press-slam/backdrop of the match gets fumble-prone (floor ≈ 50-70%).
Fumble → moves 0x83 / 0x7E (§5). Other helpers: `0x11412` mutual-link check
(carry set = stale → abort move 0x30); `0x1108C` CPU pin-intent
(`rng&0xFF < word 0x110C8[id]` → `bset #4,+0x34`; row words
88 80 44 40 78 78 50 80 80 85 80 40); `0x110E0` victim's-tag-partner run-in
(singles only); `0x11058` rope-shake side pick by `+0x37` bits 0/1.

## 8. C sketches (engine handler style; fields per docs/engine-specs template)

```c
/* ---- ROM 0x16582: move 0x29 tie-up knee (cell 0x1656A) ---- */
static const u8 *mv29_knee(Obj *o, const u8 *cell, Obj *p)
{
    if (!(o->anim_sel & 0x8000)) {                    /* 0x16582 */
        o->mover = 0;                                 /* +0x01 */
        o->grap44 &= ~0x8000; p->grap44 |= 0x8000;    /* swap drawn/hidden */
        p->state = 0x00FF;                            /* freeze: hidden cell */
    } else {
        if (o->frame == 1 && o->count == 0) sound(0x2A);
        if ((o->frame & 0xFF) == 0xFE) {              /* resolve 0x165BC */
            p->dmg_pend = 1; p->hits_c7++;            /* +0x68 / +0xC7 */
            p->combo_t = 0x100;
            if (++p->combo < 2) {                     /* back to lockup */
                o->state = 0x0B; p->state = 0x0B;
                o->grap44 = 0; p->grap44 = 0x8000;
            } else {                                  /* hold won */
                o->state = 0x0C; o->grap44 = 0x2000;
                p->state = 0x00FF; p->grap44 = 0;
            }
        }
    }
    return cell;    /* no divorce: pair survives both exits */
}

/* ---- ROM 0x14CF4: move 0x15 facelock stance (cell 0x14CE0) ---- */
static const u8 *mv15_stance(Obj *o, const u8 *cell, Obj *p)
{
    if (!(o->anim_sel & 0x8000)) {
        o->mover = 0;
        if (o->grap44 == 1) {                         /* stance-walk re-entry */
            o->anim_sel |= 0x8000; o->frame = 0; o->count = 0;
        } else if ((o->prev_sel & 0xFF) == 0x0C)
            pos_add_facing(o, +0x20, 0, 0);           /* 0x10BD0: step back */
        if (o->grap44 != 3) { p->state = 5; p->move_id = 0x7B; }
    } else {
        p->x = o->x; p->y = o->y;                     /* carry */
        p->flip = o->flip ^ 0x8000;                   /* face each other */
        if (o->result & 0x8000)      { o->state = 5; o->move_id = 0x50; }
        else if ((o->frame&0xFF) == 0xFE) {
            o->frame = 0; o->count = 0;               /* loop (resumes fr.1) */
            p->dmg_pend = 1;                          /* 1 HP per wrench */
        }
    }
    return cell;
}

/* ---- ROM 0x14E40: move 0x17 whip (cell 0x14E2C) ---- */
static const u8 *mv17_whip(Obj *o, const u8 *cell, Obj *p)
{
    if (!(o->anim_sel & 0x8000)) {
        o->mover = 0; p->state = 0x00FF;
        nudge_clipped(o, -0x18);                      /* 0x10B62 */
        p->x = o->x; p->y = o->y;
    } else if (o->frame == 1 && o->count == 0) {      /* RELEASE 0x14E6A */
        p->state = 2;                                 /* run */
        p->partner26 |= 0x80000000u;                  /* lazy divorce */
        o->f34 |= BIT7; p->f34 |= BIT6;               /* whip markers */
        o->f33 &= ~BIT6; p->f33 &= ~BIT6;
        p->grap44 = 2;                                /* rope-bounce budget */
        p->flip = o->flip ^ 0x8000;
        p->angle = (p->flip & 0x8000) ? 0x40 : 0xC0;  /* run his facing */
        carry_place(o, p, +0x50, -2, 0);              /* 0x10B9A: behind */
    } else if ((o->frame & 0xFF) == 0xFE) {
        o->state = 0; o->flip ^= 0x8000;              /* turn to watch */
        o->partner26 |= 0x80000000u;
    }
    return cell;
}

/* ---- ROM 0x15006: move 0x19 press slam (cells 0x14FB6 / 0x14FDE) ---- */
static const u8 *mv19_slam(Obj *o, const u8 *cell, Obj *p)
{
    if (!(o->anim_sel & 0x8000)) {
        o->mover = 0; p->state = 0x00FF;
        o->off_x = 0x2E; o->off_y = 0x4E;
        if (!roll_1129e(o, 1, o->cnt_f4++))           /* fumble */
            { o->state = 5; o->move_id = 0x83; return cell; }
        carry_place(o, p, 0xF0, 0, 0);
        voice(o->wid, 7);
    } else {
        if (o->frame == 3 && o->count == 0) o->off_x = 0x7F;
        if (o->frame == 2 && o->count == 0) launch(o, 0x16); /* attacker leaps:
                                            vx +0x100 back, vz 0x300, g 0x48 */
        if (o->frame == 3 && o->count == 0 && !tag_mode()
            && probe_clipped(o, 0x80)) {              /* at the ropes: OUT */
            o->move_id |= 0x80;                       /* variant cell */
            o->spr = 0x1FD | o->flip;
            p->state = 4; p->reaction = 0x25 | 0x2000;/* + rise-dizzy bit13 */
            p->grap44 = 0; p->flip ^= 0x8000;
            carry_place(o, p, 0x80, 1, 0x39);
            shake_ropes(o);                           /* 0x11058 */
            o->f33 &= ~BIT6; p->f33 &= ~BIT6;
        }
        if (o->push37 & BIT4) {                       /* attacker lands */
            thud(o); o->mover = 0; o->count = 0;      /* release FF00 hold */
            g_shake_1c1800 = 0x0E;
        }
        if (o->frame == 5 && o->count == 0) {         /* release (normal) */
            if (!(o->move_id & 0x80)) {
                p->state = 4; p->reaction = 0x21 | 0x2000;
                p->flip = o->flip ^ 0x8000;
                carry_place(o, p, 0xF0, 1, 0);
                o->f33 &= ~BIT6; p->f33 &= ~BIT6;
            }
            p->hits_c7 += 5; p->f32 |= BIT3;          /* slow walk */
            tag_runin_check(p);                       /* 0x110E0 */
            cpu_pin_roll(o);                          /* 0x1108C */
            p->dmg_pend = 0x14; sound(0x32);
        }
        if (o->frame == 6 && o->count == 0)
            { o->off_x |= 0x8000; pos_add_facing(o, 0x30, 0, 0); }
        if ((o->frame & 0xFF) == 0xFE) {
            o->state = 7;                             /* attacker gets up */
            cpu_pin_roll(o);
            o->spr = 0x68 | o->flip; o->partner26 |= 0x80000000u;
        }
    }
    return (o->move_id & 0x80) ? CELL_14FDE : cell;   /* 0x151FC */
}

/* Victim reaction 0x22 (hip toss), ROM 0x1BDE8 — pattern for 0x13/16/21/25 */
static const u8 *rc22_air(Obj *o, const u8 *cell)
{
    if (!(o->anim_sel & 0x8000)) {
        launch(o, 0x18);                              /* vx -0x180 fwd, vz -0x100
                                                         down, grav 0x58 */
        edge_y_fix(o, 0x30);                          /* 0x10F9C */
    } else {
        if (o->frame == 0 && o->count == 0) o->z -= 0x10;
        rope_bump_events(o);                          /* 0x10FC6 */
        if (o->frame == 1) o->float42 = 0xFFE0;
        if (o->push37 & BIT4) {                       /* landed */
            thud(o); o->float42 = 0;
            o->state = 4; o->reaction = (o->reaction & 0xFF00) | 5; /* bounce */
            o->spr = 0x1F | o->flip;
            pos_add_facing(o, -0x38, 0, 0);
        }
    }
    return cell;
}
```

## 9. Open labels / follow-ups

* `+0x42` (written 0xFFE0/0xFFF0 mid-flight, cleared on landing) — float
  block semantics untraced; keep the writes.
* Move 0x06 (the 0x83 counter move the ex-victim performs) and move 0x68
  (outside-landing) not transcribed; move ids ORed with 0xC000/0x80 use the
  low byte for the cell index — the high bits are handler-owned flags.
* The `+0x46 != 0` behind-take entry of move 0x30: writers are on AI paths
  (cat test clears it); untraced.
* Move 0x50 first two instructions were clipped by the dump but the body is:
  split pair X ±0x20, partner → reaction 1 (dizzy, spr 0x16), both `+0x26`
  poisoned, `bclr #6 +0x33` (0x18BA0-0x18BF2).
* `+0x34` bit6/bit7 whip markers: consumers found at `0x20980` (AI retarget
  skip) and clears at `0xF630`/`0x11AD6`; a full whip-resolution trace
  belongs to the run/rebound phase.
