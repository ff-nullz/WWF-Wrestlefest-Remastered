# Tag match — transcription of the tag-team machinery (reference/maincpu.asm)

Read-only transcription. PCs are ROM addresses. Field names: x/y/z = +0x06/+0x0A/+0x0E,
state = +0x20 (word; low byte +0x21 = state, bit15 = entry flag), sub = +0xAE (word; low byte
+0xAF, bit7 = "arrived"), move_id = +0x60, f32/f33/f34/f35 = +0x32..+0x35, f56 = +0x56,
teammate = +0x86, partner (grapple) = +0x26, opp = +0x7A, alt = +0x7E, facing = +0x2E (b7),
walk-dir = +0x2D (angle byte: 0x00 = +y/down, 0x40 = +x/right, 0x80 = up, 0xC0 = left),
walk-speed = +0x2A/+0x2B, rescue-timer = +0xE6, side = f33 b7 (0 = P1 team, 1 = P2/CPU team).

## 0. Mode word — CORRECTION to the other specs

`$1C0161` b0 = rumble, **b1 = ringside/weapons scene (NOT tag)**. **Tag is `$1C0161 & 3 == 0`.**
Evidence: human tag request `0xDD12` requires `($1C0161 & 3) == 0`; `0x1F3D6` tag-want eval
bails when b1 set; `0xF0BA` / `0x1C70C` (b1 gated) look for *weapon objects* (slots 9/10 at
`$1C0F1C`/`$1C1028`, `+0x1D == 3`, holder `+0x76`) → move 0x70 = pick-up. So the prompt's
"0xF0BA tag corner-man attack" is wrong: 0xF0BA is the weapon pick-up pre-consumer, irrelevant
to tag. categories-rest.md "tag mode ($1C0161 b1) → f33 b2" is really "ringside scene → diver
lands outside".

Tag globals: `$1C1682` referee usher timer (0xFA = 250 f / 0x177 = 375 f); `$1C1684`/`$1C1688`
"man to usher out" ptr for side 0 / side 1; `$1C1697` re-sync flag (written at 0x10778, 0x11550,
0x11E72, 0x18A50, 0x19AAC, 0x1B10A, 0x1B13C, 0x1C3F2, 0x1C8D0, 0x1CA24, 0x1D634, 0x1D9F0,
0x1EBC4, 0x1EDBA, 0x1F5FA, 0x212FA/0x21322, 0x213E0 — **no reader found in the asm, TODO EXACT**;
treat as write-only/dead). `$1C1670` corner-occupied bits.

## 1. Match setup

### 1a. 0x10400 (1P-tag path, after opponent pick 0x1034A) — CPU pair
```
10400-10454  D0/D1 = $1C0380[pick0], [pick1]; either 10/11 -> (10,11); stage $1C0163 == 4|9 -> (4,5)
10454-10466  $1C0A36=D0 $1C0B42=D1 (ids), $1C09E2 / $1C0AEE = ids (slot4 = $1C09E0, slot5 = $1C0AEC)
1046C/10474  slot4.+0x00 = slot5.+0x00 = 0x8000 (live)
10480-104A0  faced-list $1C0598[0..9]: first -1 entry <- (D0,D1)
104A4 loop D7=1..0 over slot4, slot5 (live only):
   f32 word = 0; f56 byte = 0x80 (CPU)
   teammate: slot4.+0x86 = slot5 ; slot5.+0x86 = slot4 ; slot5 ALSO f56 |= 0x40 (autopilot/apron)   ; 0x104D2
   jsr 0x2AEA (load wrestler +0x02);  id >= 6 -> f56 |= 0x20
```
### 1b. 0x10504 — human slots 0..3
```
10508 for slot 0..3 live: if !(+0xA1 b0 "player present") -> f56 = 0x40 (autopilot)
      +0x02 = f56 & 0xFF (id); f32 word = 0; if !(f56 b6) -> f33 |= 2 (human)
      jsr 0x2AEA; slots >= 2 (>= $1C07C8) -> f33 |= 0x80 (side 1)
1055E for D0=0..3 live: bsr 0x10782 (hp init); (x,y) = 0x10708[D0]; z = 0x140; D0>=2 -> f33 b7
      0x10708: (0x240,0x150) (0x220,0x160) (0x2C0,0x150) (0x2E0,0x160)        ; = trace start positions
105AC CPU live: D1 = slot0 live ? 8 : 0 ; slot4 (x,y) = 0x10708[2 + D1/4] , slot5 = next  ; (0x2C0,0x150),(0x2E0,0x160)
      z = 0x140 both; 0x10782 hp for both; if slot0 live: $1C0A13 |= 0x80, $1C0B1F |= 0x80 (side bit on CPU f33)
10622 for the 3 PARTNER slots (1,3,5; stride 0x218 from $1C06BC): live -> f34 |= 2  (TAG RECALL PENDING)
10644 (again) slot0 live -> CPU f33 b7 set
1065E for the 3 LEADER slots (0,2,4; stride 0x218 from $1C05B0), live:
      f33 |= 1 (LEGAL);  if !CPU: f33 |= 2 (human), f56 &= ~0x40
      A1 = leader+0x10C (partner): f56 |= 0x40 (AUTOPILOT)
      if partner !CPU: leader +0xA1 b0 clear -> partner f33 &= ~2
                       else partner +0xA1 b0 set -> partner f56 &= ~0x40, f33 |= 2   ; 2-player team: both human
106CE jsr 0x10718 ; $1C008C = 0
```
Human teammate links (+0x86 for slots 0..3) are NOT written here — they come from the
slot allocation (0x6FB8/0x7042 object-link writers, 0xD54/0xDC4/0xE64 templates). TODO EXACT 0xE64.

### 1c. 0x10718 — referee object seed + re-sync
```
10718 A0=$1C11F4 (referee): +0x00=0x8000 +0x02=0x0C (id) x=0x280 y=0x198 z=0x140; jsr 0x2AEA
      +0x20=0x8000 (state 0, entry); +0x34=0; loads 7, 0xE, 0x1D, 1 via 0x2AEA (sprite banks)
10778 $1C1697 = 1
```
### 1d. Walk-out to the apron (first frames)
Partner slots start with **f34 b1** (recall) and **f56 b6** (autopilot) → they are AI-driven
(0x1C16E: the AI pass accepts f56 b6 OR (f56 b7 && !f32 b7)). State-0 AI `0x1C5EC`: f34 b1 →
`0x1C5FA`: not legal → **state = 1, sub = 4** (walk-out, §2a). Legal man with f34 b1 (set by
the usher) → b1 moved to teammate, `$1C1684/8 = teammate`.

Intro face-off (common state-1 sub 0xC, `0x11B6C`, reached by the intro script — TODO EXACT
caller): `f34 = 4; 0x1174C speed; dest (+0xBE,+0xC0) = (legal ? 0x254 : 0x29C, 0x160)`
(rumble 0x2C0); walk via 0x1F15C; arrived → `sub |= 0x80, +0x22 = -1, snap x/y`; then
facing = toward teammate, and when teammate is also in state 1 sub 0x800C → both get
`state 5 move 0x8B` (team pose). Not tag-specific beyond the legal/non-legal x.

## 2. The apron partner

Common (non-AI) state-1 sub table `0x1167A` (dispatch 0x11662, index = sub & 0x3F):
sub0 0x116C6 walk, **sub1 0x11796 apron follow**, **sub2 0x1192C apron corner idle**,
**sub3 0x11A20 apron walk-to-middle**, **sub4 0x11ABA walk-out**, sub5..0xB 0x116C6, sub0xC 0x11B6C.
The AI sub table 0x1C908 runs *in addition* for CPU/autopilot men (sub1 = 0x1D398 §4).

### 2a. sub 4 — walk out of the ring `0x11ABA`
```
entry: +0x18=+0x1A=0; +0x01=1; f34 &= ~0x10; f34 b7/b6 cleared (and opp's); 0x1174C (speed by id)
       facing = side; +0x2D = side ? 0xC0 : 0x40 (walk toward own side rope); +0x2C=0
each:  f34 b0 (rescue armed) -> f34 &= ~2, state 0, sub 0            ; abort: go save partner instead
       else 0x115D2; rope edge hit (side1: +0x37 b0 right rope / side0: +0x37 b1 left rope)
             -> state 5 move 0x4F (climb out, handler 0x18B0E)
```
Move 0x4F `0x18B0E`: entry `+0x12=2; facing = (f32 & 0x80)<<8`; if f34 b0 → `state 0,
$1C1682 = 0xFA, +0x12 = 0` (rescue overrides); at frame 0xFE → `state 1 sub 1; facing ^= 0x80;
f34 &= ~2; +0x12 = 4; x += side ? +0x38 : -0x38` (lands on the apron outside the rope).
Note f32 b0 (outside) is set by sub 1 entry, not here.

### 2b. sub 1 — apron follow `0x11796` (entry when state-1 entry flag clear)
```
1179E f32 |= 1 (OUTSIDE) ; f32 &= ~8 ; f33 &= ~0x40 ; +0x2A = 0x10 ; +0x12 = 4 ; +0x44 = 1 ; +0x46 = 0
117C6 0x11900: +0x44 = 0x94 (table 0x11914, all ids 0x94)   ; regen tick
117CA APRON LINE: side0: x = (y + 0x580) >> 2 ; side1: x = (0xE48 - y) >> 2     ; slanted aprons!
      (y=0x192 -> x=0x1C4 ; y=0x120 -> x=0x34A) — the partners live on the LEFT/RIGHT side aprons
per frame (117F6):
      facing = !side
      +0xFE b7 (match decided) -> state 5 move 0x4E (enter ring)
      jsr 0x1D526 (rescue countdown, §4) ; carry -> done
      0x115D2 (drop stale grapple link) ; !f33 b5 -> +0xC8++ (apron boredom counter, §2e)
      +0xC6 b6 -> state 1 sub 3 (walk to apron middle, §2d)
      --+0x44 == 0 -> +0x44 = 0x94 ; hp +0x66 < max +0x72 -> +0x66++ , +0x6A = 1   ; APRON REGEN 1 hp / 148 f
      f34 b5 (hurt-on-apron) : +0x46 = 0x20 countdown, pose 0x1D3|facing, +0x22 = 2 ; at 0 clr b5
      else +0x2D != 0 -> walking anim (+0x01=1) ; +0x2D in [0x40,0xC0) (upward) -> y = max(y,0x120)
                          else y = min(y,0x192)                    ; APRON Y RANGE 0x120..0x192
           +0x2D == 0 -> standing (+0x01=0), pose = facing, anim 0x125C0
```
Who moves +0x2D: the AI sub1 0x1D398 (§4a) for CPU/autopilot; a human-controlled partner is
never reached by input (0xF18A-0xF1A4 skips f56 b6 and f56 b7 objects) → **the human's partner
cannot be moved and has no tag button; it is pure autopilot**. Only the legal man has input.

### 2c. sub 2 — walk to the corner post and wait `0x1192C`
```
entry: +0x01=1; +0x2A=0x10; +0x44=1; sub &= ~0x80; facing=!side; +0x2C = side ? 0x0078 : 0x0008
       (+0x2D = 0x08 = down-right along the left apron / 0x78 = up-right along the right apron)
each:  !f33 b5 -> +0xC8++ ; +0xC6 b6 -> clr, f33 |= 0x20 ; regen as 2b ; +0xFE b7 -> move 0x4E
       0x1D526 ; if !(sub b7): side0: y > 0x182 -> y = 0x182, arrive ; side1: y < 0x130 -> y = 0x130, arrive
       arrive: sub |= 0x80 (sub = 0x8002 = "AT CORNER, TAG-READY"), +0x01=0, pose=facing, +0x22=2
```
Corner post spots on the apron: side0 (0x1C4..0x1C8, 0x182..0x192), side1 (0x34C, 0x120..0x130);
the 0x4D/0x4E handlers snap to `0x18A88[side] = (0x1C8,0x192) / (0x34C,0x120)`.

### 2d. sub 3 — walk to the apron middle `0x11A20` (after +0xC6 b6 "bored")
```
entry: +0x2A=0x10 ; side1: +0x2C=0x00F8, side0: +0x2C=0x0008 ; y >= 0x158 -> +0x2D ^= 0x80 ; +0x01=1
each:  0x1D526 ; crossed y = 0x158 -> (+0xC6 b6 ? state 5 move 0x71 : state 1 sub 1)
```
Move 0x71 = "wants a tag" gesture at the apron middle (handler not transcribed, TODO EXACT
0x12614+0x71*4). It is what the legal AI man reacts to (§3a).

### 2e. Partner following the legal man — `0x10E86` (bsr from the human walker 0xF2C8 / move 0x16 0xF49E)
```
10E8A not rumble ; self legal ; self inside (f33 b2 clear) ; teammate outside (f32 b0)
10EB8 own-corner zone: side0: x < 0x230 && y > 0x170 ; side1: x > 0x300 && y < 0x150
  NOT in corner (10EE6): teammate state 1 sub 2 -> teammate state 1, sub = 1   ; follow me along the apron
  IN corner (10F0C):     teammate state 1 sub 1 -> teammate state 1, sub = 2   ; go wait at the post
                         teammate state 5 move 0x71 -> +0xC6 b6 clr, f33 |= 0x20, state 1 sub 2
```
Boredom: `0xFD44-0xFD70` (per-frame) `+0xC8 >= 0xFDB4[id]` → `+0xC6 |= 0x40, +0xC8 = 0x3A9`.
(0xFDB4 16-word table: TODO EXACT dump.)

## 3. THE TAG

### 3a. Trigger
Human legal man `0xDD02` (pre-consumer in the human selector, returns carry):
```
DD06 (+0xA3 & 3) != 0 (A or B pressed) ; ($1C0161 & 3) == 0 ; self inside (f32 b0 clear)
DD2A teammate state == 1 && (teammate sub & 0x80FF) == 0x8002   ; partner waiting AT THE POST
DD46 corner zone: side0: x < 0x220 && y >= 0x186 ; side1: x >= 0x2F0 && y < 0x12E
DD70 state 1 or 0 -> state 5 move 0x4C (tag)  ; state 5 move 0x15/0x16 (turnbuckle) -> move 0x66 (tag from the buckle)
```
CPU legal man: state-0 AI `0x1C640`: +0xB5 b6 (wants tag) && opp hp != 0 && !(opp lying react 1):
teammate state 1 sub 1|2 → **state 1 sub 8** (tag-corner walk 0x1D206); teammate in move 0x71 →
clear +0xC6 b6, f33 |= 0x20, sub 8. (`0x1C7B0 0x23ED0[id][stage]` = approach chance, 0x1EC72
also enters sub 8 when the teammate is outside and opp hp != 0.) +0xB5 b6 itself: `0x1F3D6` sets
it when legal && !outside && !autopilot && (lying react 1 → roll 0x1F458 {0x1E,0x46}; else
own hp + 0xC < teammate hp) else clears.
Sub 8 `0x1D206`: once: teammate → state 1 sub 2; dest (+0xBE,+0xC0) = side0 (0x200,0x190) /
side1 (0x308,0x128); walk 0x1F15C; arrived → `+0x01=0, +0x23=0x60`; teammate sub b7 →
`+0xB5 &= ~0x40, state 5 move 0x4C` (0x1D316). While walking it still reacts (0x1D244-0x1D2EA:
opp standing within +0xB2 → 0x1DE66 walk-to / 0x75 / 0x3C).

### 3b. Move 0x4C — tag (handler 0x1878A, entry 0x18772)
```
entry: +0x01=0 ; (x,y) = 0x188B4[side] = (0x218,0x193) / (0x2F6,0x121) ; z=0x140 ; facing = side
       teammate: state 5, move 0x4D (receive) ; f32 &= ~8
frame +0x25 == 1, +0x23 < 2:
   jsr 0x1F760 -> D0 = 0 iff opp is LYING (state 4 react 8) INSIDE MY CORNER ZONE:
        side1: opp.y < 0x160 && (opp facing L ? 0x2D0 <= opp.x < 0x300 : opp.x >= 0x298)
        side0: opp.y >= 0x170 && (opp facing L ? opp.x < 0x26A : 0x1F8 <= opp.x < 0x248)
   D0 == 0 (double-team window, 0x187FA): bsr 0x188BC SWAP ; jsr 0x21978 ; f32 &= ~1
        opp +0x9A = 0x1000, +0xAA = 0x1000 ; jsr 0x214C0 (§3d) ; D0 != 0 ->
        teammate state 1 sub 2, teammate f32 |= 1 (stays outside)        ; partner refused
   D0 != 0 (normal, 0x18840): +0x12 = 2 ; 0x10BD0 add (-0x20,0,0) (facing-relative) ; sfx (+0x03, 0x18)
frame +0x25 == 0xFE (0x18864): A3 = teammate ; jsr 0x21978 ; bsr 0x188BC SWAP
       state 1 sub 3 ; +0x26 |= 0x80 ; facing ^= 0x80 ; pose = facing ; +0x12 = 4
       teammate +0x22 = 0 ; +0x2C = 0 ; x = teammate.x
```
**SWAP `0x188BC`** (A0 = outgoing, A3 = incoming):
```
188BE jsr 0x24EC2 on A3 (band +0x70 = hp/(max/3) tier recompute)
188C6 A0 f33 &= ~1 ; A3 f33 |= 1            ; LEGAL FLAG SWAP
188D2 A0 f32 |= 1  ; A3 f32 &= ~1           ; outside / inside
188DE A0 !CPU: A3 !human -> swap +0x8A (input block ptr) ; A0 f33 &= ~2, A3 f33 |= 2 ; A0 f56 |= 0x40, A3 f56 &= ~0x40
               A3 human (2P team) -> A0 f56 |= 0x40, A3 f56 &= ~0x40 only
      A0 CPU : A3 f56 &= ~0x40, A0 f56 |= 0x40
18924 jsr 0xC1EA : HUD recolour: per live human slot, palette 0xC264 (legal) / 0xC294 (apron)
```
`0x21978` (A0 = outgoing, A3 = incoming): `A0.f34 |= A3.f34 & 7 ; A0.+0xE6 = A3.+0xE6 ;
A3.f34 &= ~7 ; A3.+0xE6 = 0 ; $1C1684/8[side] = A0` (pending rescue/recall duties move to the
man now on the apron; the outgoing man is the one the referee ushers).

### 3c. Move 0x4D — receive the tag (handler 0x18948, entry 0x1892C)
```
entry: +0x01=0 ; (x,y) = 0x18A88[side] = (0x1C8,0x192) / (0x34C,0x120) ; z=0x140 ; +0x24 = 0
frames: +0x25==0 && +0x22==0 -> sfx 0x30 (0x2052)
  f34 b3 (illegal-man variant, set by 0x12C62 dive/ 0x21xx): frame 1 && +0x22==0 -> +0x22=2 +0x12=4 sub=0
        facing ^= 0x80 ; state 8, +0x44 = side ? 7 : 4          ; (state 8 = rush in, TODO EXACT)
  else frame 2 && +0x22==0 -> +0x12 = 2
  frame 0xFE: +0xC6 b6 clr ; pose=facing ; sub=0 ; (x,y) = 0x188B4[side] (inside corner)
        f34 b3 was clear -> state 0, f32 &= ~8, +0x12 = 0, $1C1697 = 1   ; NOW INSIDE, LEGAL
        f34 b3 was set   -> state 5 (move continues)
  +0x24 == 0 -> sfx 0x13 ; == 1 -> 0x14 (0x10D3A)
```
### 3d. Post-tag double-team `0x214C0` (called with A0 = outgoing man)
```
214CE (not rumble) A1 = teammate ; A0 f56 |= 0x40 ; A1 f56 &= ~0x40 ; $1C1684/8[side] = A0
2150C A0 f34 |= 1 (armed), &= ~2 ; +0x20 high byte = 0 ; +0xB5 b4 clr ; move = 0x37 (HOLD-FOR-PARTNER)
21528 A1 human -> return 1
      else roll 0x23378[A1 id][A0.partner band] (0x24CC): 0 -> A1 +0x44 = side ? 7 : 4 ; A1 opp = A0 opp ;
           A1 state = 8 ; return 0          ; partner rushes in to hit the held man
           else A1 +0xB5 |= 0x80 (rescue mode), A1 +0x7E = A0 opp ; return 1
```
Move 0x37 `0x17A58` (entry 0x17A40): partner obj A2 = opp: `A2 state 5 move 0x61 (held)`;
`x/y/z/facing = opp's; f33 b6 both (engaged); teammate !f32 b1 -> teammate state 5 move 0x4E`
(enter); `+0xC6 b6 -> clr, f33 |= 0x20`; frame 0xFE / count → release (0x17AEC+). See
submissions.md "0x37 helper move 0x4E".

### 3e. Move 0x4E — enter over the ropes (handler 0x18AA0, entry 0x18A90)
```
entry: +0x01=0 ; jsr 0x24EC2 (band) ; +0x12 = 2
0xFE : state 0 ; +0xC6 b6 clr ; f34 &= ~0x10 ; sub 0 ; pose=facing ; +0x12=0 ; f32 &= ~1 (INSIDE)
       x += side ? -0x38 : +0x38
```
Writers: apron subs on +0xFE b7 (0x11814/0x119C0), rescue timer 0x1D590, 0x37 helper 0x17AB0.
Move 0x69 (climb in from ringside, 0xEF56 human / AI 0x1C6EC etc.) is the generic ringside
climb, not the apron entry.

### 3f. After the tag — usher / "5 count"
Referee idle `0x1F97E`: `$1C1682 != 0 → --; reaching 0 → ref state 2 (0x8002), ref.+0x56 =
$1C1684`. Ref state 2 `0x1F9F8`: target +0xFE → state 5; `0x20556` hunt; target outside or
(state 1 sub 4) → (target == $1C1688 ? state 5 : target = $1C1688, restart); else keep ushering.
Usher visual 7 `0x202BA`: facing flip, `+0x23 = 0xA, pose +0x05 = 7`, face the target;
**for $1C1684 and $1C1688: not outside → f34 |= 2 (RECALL); always f34 &= ~1**; then wag 7/8
every 0xA/8 frames. Recall is consumed by the AI `0x1C5EC → 0x1C5FA` (non-legal → state 1 sub 4
walk-out §2a; legal → hand b1 to teammate). Also `0x1DE6C`: non-legal CPU whose opp is outside →
f34 |= 2, state 0 (go home). `0x1FA82` (20-count) clears $1C1682 only when `$1C0161 b1`.
`$1C1682 = 0xFA` written by every rescue-arm site (0x20F5C, 0x21086, 0x21174, 0x215FA,
0x2176E, 0x18B40), `= 0x177` by 0x1D582 (run-in fired). There is no explicit 5-count; the
"count" is this 250/375-frame grace before the referee walks over.

Illegal man: `0x12C5C-0x12C68` (dive launch): `+0x01=2; f34 |= 8 (illegal-man variant); f32 &= ~1`
— a diver becomes "in the ring illegally"; 0x4D's f34 b3 branch handles his climb (§3c).
`0x1CFC4` (AI tag check): |dx to teammate| >= 0xA0 → state 2 (run) via 0x1D7A0; else
`f34 &= ~4; human → f56 &= ~0x40, teammate !human → f35 |= 8; state 0`.

## 4. Rescue / run-in

### 4a. Arming (the 0x20F04..0x217FE family, called from hold/pin/grapple handlers)
All follow one template (A0 = attacker, A1 = victim's teammate, A2 = victim):
```
if A0 legal:  A1 = victim.teammate ; victim.teammate not outside (f32 b0 clear) ->
      A1.f34 |= 1 (armed), &= ~2 ; $1C1682 = 0xFA ;
      A1 human ? nothing : A0 human ? (A1 f56 &= ~0x40 ; A0 f33 &= ~2 ; A1 f33 |= 2 ; A1 f35 |= 8)   ; 0x20F74: control jumps to the partner
                            : !(A1 +0x74 b7) -> A1 +0xB5 |= 0x80, A1 +0x7E = A0.partner ; A0 +0x7E = A1
   teammate outside -> |dx(victim, A1)| < 0x50 -> same as above (0x20F8C path)
                       else A1 +0xE6 = table[A1 band] ; A1 f34 |= 1, &= ~2 ;
                            A1 human -> A1 opp = A0.partner, f34 |= 4
                            else A0 human -> A1 opp = A0.partner, f34 |= 4 (+ control swap 0x20F7A)
                            else f34 |= 4, +0xB5 |= 0x80, +0x7E = A0.partner
```
Timer tables by band: `0x21038 = 01 01 01`, `0x2110E = 3E 5D 7D`, `0x2127C = 7D BB BB`,
`0x214BA = 1F 1F 1F`, `0x2172C = 5D 7D BB`, `0x217F8 = 3E 5D 7D`, `0x2172A = {0x46,0x1E}` (roll).
Variants: 0x21040 (pins: 0x2110E), 0x21114 (+ |dx| < 0x50 test, 0x2127C), 0x211EC (always
0x2127C), 0x21424 (submission: both men → $1C1684/8, +0xE6 = 0x1F, engaged bits, victim f32 |= 4),
0x215B6 (hold moves 0x48/0x23/0x1A/0x0E → +0xE6 = 1 immediate, else 0x2172C), 0x21732
(pin chain: 0x4A/0x64/0x5E → 1, else 0x217F8). `0x2187E` = rumble variant (other objects).
Disarm `0x212A0`/`0x212D4` (hold released): legal self → teammate: human/human → f56 &= ~0x40;
CPU → +0xB5 &= ~0x80; else (self !human !CPU) self f33 |= 2, teammate f33 &= ~2, f35 |= 0x20,
f56 |= 0x40 ; then if teammate outside: `f34 &= ~5, +0xE6 = 0`. `0x213A6` (victim freed):
`partner.teammate f34 &= ~1 / +0xB5 &= ~0x80 ...; own teammate f34 &= ~4`.

### 4b. Countdown `0x1D526` (every apron sub frame)
```
f34 b0 && +0xE6 != 0 : --+0xE6 == 0 ->
   f56 |= 0x40 ; f34 &= ~3 ; $1C1684/8[side] = self ; +0xC6 b6 -> clr, f33 |= 0x20
   $1C1682 = 0x177 ; state 5 move 0x4E (ENTER) ; carry set
```
### 4c. In-ring run-in AI — state-1 sub 1 `0x1D398` (also the apron-follow mover)
```
1D3A2 D1=0x08 D2=0x88 ; dy = teammate.y - y ; teammate above: +0x2D = side ? 0x78 : 0x88
                                               below:         +0x2D = side ? 0xF8 : 0x08
1D3DE |dy| < 8 -> +0x2D = 0 ; then !f34 b0 && teammate.partner in state 5 move 0x53 && |dx| < 0x40
      -> state 5 move 0x39 (strike from the apron)
1D428 (match live) A1 = teammate.opp ; standing (0/1) or lying react 0/1/0xA ; |dy| < 0x10 && |dx| < 0x40
      -> once (+0xB7 b1): roll 0x1D4FC[stage] (3-way, rows 19 0F 3C / 1E 14 32 x2 / 23 19 28 / 28 1E 1E x5 / 2D 23 14)
         0 -> opp = A1, state 5 move 0x39 (run-in strike)   1 -> A1 unpartnered: +0xB5 b4, move 0x1C (behind grab)
```
(cat 0x13 run-in strike/grab per categories-rest.md; `0x21358` = rumble retarget.)
Walk-to helper `0x1F15C`: |x-+0xBE| < 8 && |y-+0xC0| < 8 → 0; else `$1C15E4 = self,
$1C15E8/EA = dest, jsr 0x20C8 (angle), +0x2D = result, return 1`.

## 5. Match end in tag
Pins: only legal men (pins-referee.md; `0x204FA` = first two live slots with f33 b0).
Referee action point `0x204B2` = midpoint of the two legal men. Count-out: `0x1FA82`.
Decision flags: win pose `0x1AD24`: self & teammate `+0xA0 = 0x40`, opp & its teammate `0x80`;
loser `0x1AD7C`: `+0xFF == 5` (tag) → all four `0x80`. HUD `0x11B6` reads slot0/slot2 `+0xA0`
(`($1C0650 | $1C0868) & 0xC000`). `+0xFE = 0x8000/0x8001` pairs (hud-rules.md §3). While +0xFE
b7 the apron men run 0x4E into the ring (2b/2c).

## 6. C sketches

```c
/* 1. tag setup (0x10504-0x106CE) */
static const s16 start_xy[4][2] = {{0x240,0x150},{0x220,0x160},{0x2C0,0x150},{0x2E0,0x160}};
void tag_setup(Engine *st){
  for (int i=0;i<4;i++){ Obj *o=&st->obj[i]; if(!o->live) continue;
    if(!(o->a1&1)) o->f56=0x40; o->id=o->f56&0xFF; o->f32w=0; if(!(o->f56&0x40)) o->f33|=2;
    load_wrestler(o); if(i>=2) o->f33|=0x80; o->x=start_xy[i][0]; o->y=start_xy[i][1]; o->z=0x140; hp_init(o); }
  Obj *c0=&st->obj[4],*c1=&st->obj[5]; int k = st->obj[0].live?2:0;       /* 0x105AC */
  c0->x=start_xy[k][0]; c0->y=start_xy[k][1]; c1->x=start_xy[k+1][0]; c1->y=start_xy[k+1][1]; c0->z=c1->z=0x140;
  c0->f56=0x80; c1->f56=0xC0; c0->teammate=c1; c1->teammate=c0;             /* 0x104B6-0x104D8 */
  if(st->obj[0].live){ c0->f33|=0x80; c1->f33|=0x80; }
  for (int i=1;i<6;i+=2) if(st->obj[i].live) st->obj[i].f34|=2;            /* partners: recall */
  for (int i=0;i<6;i+=2){ Obj *L=&st->obj[i],*P=&st->obj[i+1]; if(!L->live) continue;
    L->f33|=1; if(!(L->f56&0x80)){ L->f33|=2; L->f56&=~0x40; } P->f56|=0x40;
    if(!(P->f56&0x80)){ if(!(L->a1&1)) P->f33&=~2; else if(P->a1&1){ P->f56&=~0x40; P->f33|=2; } } }
  referee_seed(); st->resync=1;
}

/* 2. apron follow/idle (0x11796 / 0x1192C) — per frame, state 1 sub 1/2 */
void apron_sub(Obj *o, int sub){
  int side = o->f33>>7;
  if (entry(o)){ o->f32|=1; o->f32&=~8; o->f33&=~0x40; o->spd=0x10; o->plane=4; o->t44=1;
    if(sub==1){ o->x = side ? (0xE48-o->y)>>2 : (o->y+0x580)>>2; }
    else { o->sub&=~0x80; o->dir = side?0x78:0x08; } }
  o->facing = side?0:0x80;
  if (o->fe&0x8000){ set_move(o,0x4E); return; }
  if (rescue_tick(o)) return;                                   /* 0x1D526 */
  if(!(o->f33&0x20)) o->c8++;
  if(sub==1 && (o->c6&0x40)){ o->state=1; o->sub=3; return; }
  if(--o->t44==0){ o->t44=0x94; if(o->hp<o->hpmax){ o->hp++; o->hpflash=1; } }
  if(sub==2){ if(!(o->sub&0x80)){ if(!side && o->y>0x182){o->y=0x182;o->sub|=0x80;o->cnt=2;}
                                   if( side && o->y<0x130){o->y=0x130;o->sub|=0x80;o->cnt=2;} } return; }
  if(o->dir){ o->anim=1; if(o->dir>=0x40&&o->dir<0xC0){ if(o->y<0x120)o->y=0x120; } else if(o->y>0x192)o->y=0x192; }
  else { o->anim=0; o->pose=o->facing; }
}

/* 3. tag exchange (0x188BC + 0x21978) */
void tag_swap(Obj *out, Obj *in){
  recompute_band(in);
  out->f33&=~1; in->f33|=1; out->f32|=1; in->f32&=~1;
  if(!(out->f56&0x80)){ if(!(in->f33&2)){ swap(out->input,in->input); out->f33&=~2; in->f33|=2; }
                        out->f56|=0x40; in->f56&=~0x40; }
  else { in->f56&=~0x40; out->f56|=0x40; }
  hud_recolour();                                                /* 0xC1EA */
  out->f34 |= in->f34&7; out->e6=in->e6; in->f34&=~7; in->e6=0;  /* 0x21978 */
  st->usher[out->f33>>7]=out;
}

/* 4. run-in (0x1D526) */
int rescue_tick(Obj *o){
  if(!(o->f34&1)||!o->e6||--o->e6) return 0;
  o->f56|=0x40; o->f34&=~3; st->usher[o->f33>>7]=o;
  if(o->c6&0x40){ o->c6&=~0x40; o->f33|=0x20; }
  st->usher_timer=0x177; set_move(o,0x4E); return 1;
}

/* 5. referee usher (0x1F97E / 0x202BA) */
void ref_idle(Ref *r){ if(st->usher_timer && --st->usher_timer==0){ r->state=2; r->target=st->usher[0]; } }
void ref_usher_visual(Ref *r){
  if(entry(r)){ r->pose=7; r->t23=0xA; face(r,r->target);
    for(int s=0;s<2;s++){ Obj *m=st->usher[s]; if(!(m->f32&1)) m->f34|=2; m->f34&=~1; } }
  if(--r->t23==0){ r->pose ^= 7^8; r->t23 = r->pose==8?8:0xA; }
}
```

## TODO EXACT
- `$1C1697` reader (none found); `0xE64` human teammate link template; intro sub 0xC caller.
- Move 0x71 / 0x70 / 0x66 / 0x8B handlers; state 8 (`+0x44` = 4/7) semantics; `0xFDB4` table.
- `0x18B94` (move 0x50, 2-man setup, x ±0x20, both +0x26 |= 0x80) and 0x51 (0x18C24) = in-ring
  double-team grapples (writers 0x124A4/0x12542/0x14D6A/0x14DCE, 0x13E3E/0x1403A/0x1411C).
- Trace partner x 0x2C0→0x226→0x1FC does not fit the slanted-apron formula unless y also moved;
  re-check the trace y (expected x = (y+0x580)/4 on side 0).
