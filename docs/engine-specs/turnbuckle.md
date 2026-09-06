# Turnbuckle: climb / perch / dives — read-only transcription (reference/maincpu.asm)

Engine names: state `+0x20` (low `+0x21`), latch `+0x1C` b7, anim_sel/mover `+0x01`, frame `+0x25`,
count `+0x22`, spr `+0x04/05`, x `+0x06` y `+0x0A` z `+0x0E`, list `+0x12`, facing `+0x2E` (b7 = left),
f32/f33/f34/f35, grap44 `+0x44` (word; **low byte is `+0x45`**), move_id `+0x60` (lo `+0x61`),
react_id `+0x64` (lo `+0x65`), vx `+0x58` vy `+0x5A` vz `+0x5C` grav `+0x5E`, off_x `+0x18` off_y `+0x1A`,
partner `+0x26`, opp `+0x7A`, joy nibble `+0xA9` (b0 R, b1 L, b2 UP, b3 DOWN), `+0xAC/+0xAD` hold-latch.
Globals: `$1C1670` corner-occupied bits (long), `$1C0161` b0 rumble b1 tag/cage, `$1C15D2` voice pair.

## 0. RESOLUTION of the 0x61 question (asked in the brief)

`0x12614[0x61]` = `0x194CA` {code 0x194D0, mode 0}. `0x12614[0x37]` = `0x17A40` {0x17A58, mode 1, n4,
dur 08 08 08 08, spr 18B 18C 18D 1CF}; at **0x17A64-0x17A6A** the 0x37 holder writes
`victim.state=5; victim.move_id=0x61`. 0x61 handler (0x194D0): unlatched & bit7 clear →
`0x10C60(5)` escape seed, `0x211EC`, `+0x44=0xAA` (timer); latched: `0x10D04` mash; bit7 clear →
`spr=0x12|facing`; bit7 set → `spr=0xFFFF` (hidden), `--+0x44`, at 0 → `state=5 move=0x59` (escape).
**So submissions.md is right: 0x61 = VICTIM HELD IN STANDING HOLD 0x37 (bearhug-type). It is NOT
"climbing the corner".** Climbing is not a move at all — it is states 8/9/0xA (below).

Corollary: `0x12614[0x77]` = `0x1A148` {code 0x1A154, mode 1, n1, dur 0x18, spr 0x1DF}:
```
1A154 unlatched:  A2(partner = the 0x37 holder).state = 0xFF   ; holder HIDDEN during the flinch
1A164 frame==FE:  state=5 move_id=0x8061 (back to being held, latched, hidden)
                  A2.state=5 A2.move_id=0x8037 (holder re-latched)
```
**0x77 = "held man takes a third-party hit, then the hold resumes"** (25 ticks), not "knocked off
the turnbuckle". Every "HIT CLIMBER"/`0x11192` site in categories-rest/move-handlers-1/rope-run-drops
is really "hit the man held in 0x37 (tag/rumble third man)". `0x11192` (0x11192-0x111C6):
`partner.state==5 && partner.+0x61==0x61 && box 0x0A` → carry. Cat 0x12 (0xE40C) = "opp is being held
by someone else (state5/0x61), own state 0/1" — a teammate-assist category, tag/rumble only (in 1v1 the
only possible holder is yourself, who is then state 5 and can't select).
Cat 0xF second branch (0xE2B2) = dive onto the held man. **No "knock a climber off the post" path
exists** in the ROM: a man in state 8/9/0xA is never a hit-test target (all box tests require victim
state 0/1/4/5-with-move); `+0x4A` (set to 1 on the climb) has **no reader anywhere** in the wrestler
code (grep `($4a,A` → only unrelated tables) — TODO EXACT: confirm with hit-pipeline state filter.

## 1. HUMAN climb entry

### 1a. Where the test runs
`0xF18A` human pass: `f33 b1 && !f34 b2` → `bsr 0xDCDE` (controller prefix) → `0xDDE0`: state 0 or 1 →
`0xDE14`: `tst.b +0xA9` (nibble HELD) != 0 → `bsr 0xEDC0` (climb gate), carry = consumed → else
`bsr 0xEF0A` (tag rope move) → else state 0 → `+0x20 = 1` walk. (input-walk.md §2 confirmed.)
State dispatch `0xF1E4[state]`: 8 → `0xF4A8` **rts** (no input while climbing), 9 → `0xF4AA`,
0xA → `0xF4BA` rts.

### 1b. Climb gate `0xEDC0` (A0 = self; returns carry = climb started)
```
EDC0  f34 b0 set            -> no        (TODO EXACT meaning of f34 b0 here; running/ropes flag)
EDCA  f33 b2 set            -> no        (tag: already the illegal man)
EDD4  (+0x36 & 3) == 0      -> no        ; rope zone: must be pressed against a rope this frame
                                          ;  (zone 1 = rope pushback / 2 high / 3 outside, rope-fall.md)
EDE0  $1C1670 != 0          -> no        ; ANY corner occupied by ANYONE blocks every climb
EDEA  $1C0161 b1 (tag/cage) -> EE96
 ---- 1v1 / rumble: four corner windows (x +0x06, y +0x0A), each needs the stick held 4 frames ----
EDF6  x <  0x1C5 && y <  0x11A : D0=8 (DOWN) 0xEF6A ok -> 0xEECE(corner 2)  ; near-left  post
EE1E  x >= 0x32B && y <  0x11A : D0=8 (DOWN)          -> 0xEECE(corner 3)  ; near-right post
EE46  x <  0x1F1 && y >= 0x197 : D0=4 (UP)            -> 0xEECE(corner 0)  ; far-left   post
EE6E  x >= 0x2FC && y >= 0x197 : D0=4 (UP)            -> 0xEECE(corner 1)  ; far-right  post
      (y small = near the camera; UP = away = +y. You push TOWARD the post.)
 ---- tag / cage: two side positions, NO direction needed ----
EE96  x >= 0x3CF : 0xEECE(9) ;  EEAC  x < 0x271 : 0xEECE(8) ; else no
EEC2 carry clear / EEC8 carry set
```
`0xEF6A(D0=mask)`: `D0 & +0xA9 == 0` → `clr.w +0xAC`, fail; else if `+0xAC` b7 clear → `+0xAC = 0x8004`;
`--+0xAD` (byte); reaches 0 → `clr.w +0xAC`, **carry set** (direction held 4 consecutive frames).

### 1c. Corner claim `0xEECE(D0 = corner index)` — shared with the AI (0x1D374)
```
EECE  D1 = $1C1670; btst D0,D1 set -> carry set (corner taken), no change
EED8  if +0xAC == 0: +0xAC = 0x8004            ; re-arm the hold latch
EEE4  tst.w +0xA8 (flags discarded - dead)
EEE8  bset D0,D1 ; $1C1670 = D1                ; *** CORNER-FREE BIT SET ***
EEF0  grap44 +0x44 = D0 (word: +0x44=0, +0x45=index)
EEF4  +0x4C = 0
EEF8  state +0x20 = 8                           ; *** CLIMB ***
```

### 1d. State 8 — climb up. Cell selector `0x11F3A`: `jsr 0x11F5E[+0x44]` then `A1 = 0x11F86[+0x44]`
```
+0x44   handler   cell     cell record (code 0x11F3A, mode 1 hold-last, dur raw 0x0C = 13 ticks/frame)
0,1     11FAE     11F0A    n5 dur 0C×5  spr 66 67 2D 2E 2F          (far corners, 65 ticks)
2,3     11FAE     11EA6    n7 dur 0C×7  spr 66 67 31 32 33 34 35    (near corners, 91 ticks)
4,5     12078     11F26    n3 dur 0C×3  spr 2D 2E 2F                (move-driven, mid-ladder start)
6,7     12078     11ECA    n5 dur 0C×5  spr 31 32 33 34 35
8,9     12122     11EE6    n7 dur 0C×7  spr 66 67 31 32 33 34 37    (tag/cage side)
```
Position tables (x,y words): `0x121C8[0..3]` = (0x1DC,0x193) (0x314,0x193) (0x1BD,0x120) (0x338,0x120);
`0x121D8[0..1]` (for +0x44 8,9) = (0x278,0x160) (0x3C8,0x160).

`0x11FAE` (corners 0-3, the human/AI path):
```
unlatched (+0x1C b7 clear):
  11FB6  mover +0x01 = 0
  11FBA  f32 |= 2                                ; "on the turnbuckle" (dives bclr it)
  11FCC  x,y = 0x121C8[+0x44]                    ; teleport to the post base
  11FD8  z = 0x140                               ; rope height
  11FEA  facing = (+0x45 & 1) << 7               ; right-side corners (1,3) face LEFT; else right
  11FEE  list +0x12 = 2                          ; rope-lean draw list
  11FF4  $1C15D2 = (id, 0x17)  voice ; 12004 sfx 0x32 (0x2052)
  12018  if !rumble ($1C0161 b0): +0x4A = 1      ; (write-only flag, see §0)
latched:
  12020  frame==1 && count==0:                   ; = tick 14, top of the first rung
         x += (+0x45 b0 ? +0x18 : -0x18)         ; shift OUT onto the post (right corners +x)
         z = 0x180                               ; PERCH HEIGHT
         facing ^= 0x80                          ; turn to face the ring
         list = 4 ; if +0x45 b1 (near corners 2,3): list = 0
  12062  frame==0xFE: state = 9 ; list = 0
```
`0x12078` (+0x44 4..7, entered from moves 0x4D @0x189CE/0x189DE [+0x44=4 / 7 if f33 b7] and
0x88 @0x1AB7C [+0x44=6 / 5 if f33 b7; 0x88 first walks +0x2A=0x10 at angle 0x88/0xF8 until
y<0x120 / y>=0x190, sets +0x4A=1]; TODO EXACT which cats/whips yield 0x4D/0x88):
unlatched: mover=0; `+0xAE=0`; f32|=2; c=+0x44&3; list=4; x,y=0x121C8[c]; z=0x180;
x += (c b0 ? +0x18 : -0x18); facing = ((c&1)<<7) ^ 0x80; if !(c b1) list=0; voice 0x17, sfx 0x32.
latched: frame 0xFE → state=9, list=0. (No +0x4A write, no $1C1670 bit — TODO EXACT: these climbs
never claim the corner bit.)
`0x12122` (+0x44 8,9 tag): unlatched: mover=0; f32|=2; x,y=0x121D8[+0x44-8]; z=0x140;
facing=(+0x45&1)<<7; voice/sfx. latched frame1 cnt0: x += (b0? +0x18:-0x18); z=0x180; facing^=0x80.
frame FE → state 9 (list untouched).

### 1e. State 9 — perched. Cell `0x121E0` {code 0x121E6, mode 0 code-only}
```
unlatched: 121EE bset #7,+0x1C ; mover=0 ; list=0 ; +0x46 = 0x80 (perch timer, 128 ticks)
           12202 x,y = 0x12256[+0x44]  (full index 0..9, stride 4)
           1221A z = 0x180
           12220 spr +0x05 = 0x1227E[+0x44 & 3] ; +0x04 = facing byte   (spr word = facing<<8 | n)
latched:   1223C if !(+0xFE b7): --+0x46 ; ==0 -> state = 0xA      ; timeout climb-down
                 else (match over flag) -> state = 0xA
every tick 12250 clr.l +0x26 (partner poisoned/none)
```
`0x12256` perch positions: idx0 (0x1E4,0x190) 1 (0x310,0x190) 2 (0x1D0,0x118) 3 (0x31C,0x118),
idx4-7 = same four again, idx8 (0x268,0x158) idx9 (0x3D8,0x158). `0x1227E` sprites: 30 30 36 36
(far corners spr 0x30, near corners 0x36; idx 8/9 → 0x30 via &3).
**No off_x/off_y (+0x18/+0x1A) writes anywhere in states 8/9/0xA** — posts are drawn by x/y/z only.
Human input on the perch: `0xF4AA`: `+0xA9 b3 (DOWN held)` → `state = 0xA`. Buttons: `0xDE86`
(attack selector, runs for every state) → `0xDEA6` new-press B1/B2 → chain → cat 0xE/0xF (§2).
Tag note: `0xDE8A` skips the legal-man refusal when `f32 b1` is set, so an illegal man on the post
can still dive; `0xE292` then `bset #2,+0x33` (becomes/stays illegal). Turning on the perch: none.

## 2. The dives (cat 0xE standing / 0xF lying-or-held)

Selector `0xE27A`: `+0x21 != 9` → no. Opp (+0x7A) state 4 && react_lo ∈{8,9} → cat 0xF; opp state 5
&& move_lo == 0x61 (held in 0x37) → cat 0xF; else → cat 0xE. `0xF178` links partners both ways.
Rows from `0xE4FE[id]` maps (stride 0x40, row = cat*3 = B1 B2 both), dumped:
```
id:     0      1      2      3      4      5      6      7      8      9      A      B
cat E:  1B1B1B 1B1B1B 0D0D0D 0D0D0D 1B0104 1B1B1B 1B1B1B 1B1B1B 0D0D0D 0D0D0D 1B1B1B 1B1B1B
cat F:  0F0F0F 0C0C0C 0C0C0C 0F0F0F 0C040C 0C0C0C 0F0F0F 0C0C0C 0F0F0F 121212 0F0F0F 111111
cat 12: 0A10FF 0A10FF 0A10FF 0A10FF 0A46FF 0A46FF 0A0BFF 0A46FF 0A10FF 0A46FF 0A10FF 0A47FF
```
AI picks (0x1EEA0 state 9, §2d): `0x1EF9E[id]` = 1B 1B 0D 0D 1B 1B 1B 1B 0D 0D 1B 1B (= cat E B1),
`0x1EF92[id]` = 0F 0C 0C 0F 0C 0C 0F 0C 0F 12 0F 11 (= cat F B1). `0x1EF90` = {0x28,0x3C}.

### 2a. Homing launch `0x26AE(k)` exact (0x26AE-0x2756), A2 = victim
```
dx,dy = 0x275C[k] (signed words); if victim facing b7: dx = -dx
T = ((vz / grav) & 0xFFFF) << 1          ; unsigned divu; T==0 -> no change
vx = ((victim.x + dx - x) << 8) / T   (divs), clamped to [-0x200, +0x200]
vy = ((victim.y + dy - y) << 8) / T   (divs), clamped to [-0xC0, +0xC0]
0x275C: k0 (0,0)  k1 (0x30,-0x20)  k2 (0x38,-8)  k5 (0x40,-1)  kA (0x40,-0xC)
```
Box 0x0A `0xE9BA+0x50`: x -8..+0x68 on the victim's facing side (mirrored+swapped if victim faces
left), y -0x48..+0x18 (0xE958).

### 2b. Cat 0xE: 0x1B / 0x0D (ids 0,1,4,5,6,7,A,B / 2,3,8,9) — one handler `0x153BC`
Cells: 0x1B `0x15384` n5 dur 08 18 18 FF00 10 spr 84 85 15B 15C 15C; 0x0D `0x153A0` n5
dur 08 10 18 FF00 10 spr 84 85 15D 15E 15E (mode 1).
```
unlatched 153C6: mover=0 ; 153CA clr.l $1C1670 (*** CORNER FREED — the whole long ***)
  153D0 f32 &= ~2 ; vz=0x680 ; grav=0x48
  153E2 grap44 = f34 b3 ? 0 : (victim.state==5 && victim.move_lo==0x53) ? 1 : 2
  15410 if +0x45==2: facing = (x < victim.x) ? right : left   (TODO EXACT: +0x45 here is grap44 low byte
                                                                just written = 2 -> always taken for case 2)
  1542C d = (+0x45 ? +0x10 : -0x10), negated if own facing b7 ; victim.x += d ; 0x26AE(0) ; victim.x -= d
latched 1545A: frame0 && count0 -> mover=2 (velocity flight)
  1546C frame==3: +0x42 = -0x10 (floor bias) ; if !(f37 b4 landed) rts
  15486 landed: thud 0x1110E ; count=0 ; mover=0 ; +0x42=0 ; jmp 0x154AA[grap44]:
   case0 154B6: f34 &= ~8 ; victim.state=4 react=0x1C dmg(+0x68)=0x12 ; $1C15D2=0x0F19 ; sfx 2B,32 ; 0x21424
   case1 154F4: victim still state5/0x53 && |dx|<0x18 && |dy|<0x14: victim.state=4 react=3 ;
                victim.partner.state=5 move=0x6F ; f33 b6 clr both ; victim.dmg=0x12
   case2 15562: !(victim f32 b0) && (tag ? victim f33 b2 : 1) && victim state∈{0,1} or (4 && react_lo==1)
                && |dx|<0x30 && |dy|<0x30: sfx 2B,32 ; victim.state=4 react=0x20 ; victim.facing = own^0x80 ;
                victim.dmg=0x12 ; victim.+0xC7++ ; weapon drop (f74 b7: f74=0, W.+0x1C=2, +0x76=0, W.off_x|=0x8000) ;
                $1C15D2 = (id, move==0x1B ? 0x0F : 0x0E)
  15648 frame==0xFE: state=0 ; spr=facing word ; +0x4A=0 ; partner|=0x80.   NO faceplant on a miss.
```
### 2c. Cat 0xF: 0x0F / 0x0C / 0x11 / 0x12 (and w4 0x04/0x01, cat E B2/both for w4)
Inits verified at the PCs; bodies per categories-rest.md (not re-walked here):
- **0x0F** (ids 0,3,6,8,A) cell 0x13F78 h 0x13F90 n4 dur 0C 0C 14 FF00 spr 84 85 1A6 1A7:
  13FA2 mover=0; **13FA6 clr.l $1C1670**; f32&=~2; vz=0x700 grav=0x48; 0x26AE(1). Landing: 0x11152 hit
  downed → state=5 move|=0x80, victim.state=5 move=0x51 (pinned under splash), dmg 0xF, f33 b6 both,
  $1C15D2=(id,0x15); victim 0x54 → react 0x1E; 0x11192 held-man → victim move 0x51, holder.state=0,
  victim.partner=self; else MISS faceplant (state=4 react=5 dmg=3 sfx 0x32, facing^=0x80, add(-0x30,0,0)).
- **0x0C** (ids 1,2,5,7; w4 B1/both) cell 0x13B98 h 0x13BC8 n4 dur 08 10 18 FF00 spr 84 85 1C1 1C2:
  13BD0 mover=0; **13BD4 clr.l $1C1670**; f32&=~2; vz=0x700 grav=0x48; 0x26AE(victim 0x61 ? 5 : 2);
  13C0A facing = (vx & 0x8000) ^ 0x8000. frame0 cnt0 → mover=2, f33|=8. landed 13C50: shake
  $1C1800=0xE; +0x4A=0; move|=0x80; 0x11152 → HIT DOWNED (victim.state=4, react_lo 8→6/9→7, +0xC7++)
  else 0x11192 → victim.state=5 move=0x77; both: mover=0 count=0x10 y=victim.y-1 sfx 2B,32 dmg 0xF;
  else MISS. FINISH state=7.
- **0x11** (id B) cell 0x14658 h 0x14670 n4 dur 08 08 28 FF00 spr 84 85 EF F0: **1467C clr.l $1C1670**;
  f32&=~2; vz=0x700 grav=0x48; 0x26AE(victim 0x61 ? 5 : 0xA). frame1 cnt0: off_y=0x20, +0x42=0x20;
  frame3 landed: off_x|=0x8000; dmg 0xD (hit) / MISS dmg 4; FINISH state=7 spr 0x68.
- **0x12** (id 9 moonsault) cell 0x147E6 h 0x14826 n6 dur 0C 0C 0C 0C 20 FF00 spr 8B 8C 8E 8F 15F 1B1:
  unlatched only mover=0. frame1 cnt0 && human (!+0x56 b7): sfx 0x31, frame(+0x24)=3 (skip wind-up).
  frame3 cnt0 14864: mover=2; **14870 clr.l $1C1670**; f32&=~2; f33|=8; add(0,0,0x20); vz=0x580 grav=0x48;
  0x26AE(0x61?5:2). frame5 landed: hit dmg 0xA / MISS dmg 4; FINISH state=7.
- **0x04** (id 4) h 0x12C22: from the top (f32 b2 set): **12C56 clr.l $1C1670**; mover=2; f34|=8; f32&=~1;
  vx=0x400 vz=0x200 grav=0x38 vy=0x100 (negated by f33 b7). **0x01** (id 4 cat E B2) h 0x128FA:
  f32 b2 → f32&=~2, 0x258E(+0x1F==2 ? 0x1C : 0x1B) launch preset, f33|=8; else **1293E clr.l $1C1670**.
  TODO EXACT 0x12DAE-0x12E38 (0x04 double-victim branch) per categories-rest.

### 2d. AI side (ai-core §4d, re-read)
`0x1F1AC`: tag → no; `+0xB6 b0` → no; opp `+0x70`==0 → no; `+0x56 b6` → no. Corner from opp x/y
(opp facing right: x<0x210 → y>=0x170 ? c0 : (x<0x200 && y<0x150) c2; x>=0x260 → y>=0x170 ? c1 :
(x>=0x268 && y<0x150) c3; facing left mirrored with 0x288/0x278/0x2D8/0x2E0). Roll `0x24CC` on
`0x1F2E0[id*4 + (opp +0x71==1 ? 0 : 2)]` (rows: 0A5A 1E46 | 1450 283C | 0064 1450 | 1450 2D37 | 1E46 3C28
| 0A5A 2341 | 0064 0F55 | 0A5A 194B | 0F55 2341 | 194B 372D | 0F55 372D | 0A5A 283C); D0==0 → opp
`+0x9A += 0x100`, opp `+0xAB += 0x14`, state 1 sub `+0xAE=9`. Sub 9 `0x1D31E`: `0x10BE8`; first entry
`+0xBE/+0xC0 = 0x1D388[+0xBC]` ((0x1F8,0x1A0)(0x300,0x1A0)(0x1D0,0x120)(0x320,0x120)); each tick:
corner bit set in `$1C1670` or opp not state 4 → state 0; `0x1F15C` arrived → `jsr 0xEECE(+0xBC)`
(claims the bit; carry ignored), `+0x44 = +0xBC`, state 8. State 8 AI `0x1EE98`: `+0xBD=0`. State 9 AI
`0x1EEA0`: `++ +0xBD` until 8 (8 ticks on the perch); tag partner (+0x86) state5/0x89 → state 5 move 4;
else `+0x61 = (opp state5/0x61 or state4 react 8/9) ? 0x1EF92[id] : 0x1EF9E[id]`; then if opp state 5 and
not move 0x7B and id ∈{4,7,0xB}: roll 0x1EF90 → 0 → `+0x61=1` (dropkick), `+0xB5 b4`; commit: `+0x60 lo=0`,
state 5, `+0xB6 b0`, → 0x1DFE2. The AI never climbs down voluntarily (timer 0x80 only).

## 3. "Knocking a climber off" — does not exist (see §0)
Cat 0x12 (`0xE40C`: own 0/1, opp state5/0x61) is the teammate stomp/leap on a man held by a third
wrestler: B1 0x0A stomp (box `0xF070[0x0A]=03`), B2 0x10/0x46/0x47/0x0B (handlers per
move-handlers-1 §2.12 / categories-rest cat 0x12), both = run (0xFF). Effect on the held man:
`victim.state=5; victim.move_id=0x77` + dmg (0x0A/0x0B), then 0x77 returns him to 0x8061 / holder to
0x8037 (§0). A wrestler on the post (states 8/9/0xA) cannot be targeted or displaced.

## 4. Climbing down / abort / corner-free bookkeeping
State 0xA handler `0x122E6` (cell selected at 0x12420):
```
unlatched: !tag: x,y = 0x121C8[+0x44] ; tag: bclr #2,+0x33 ; x,y = 0x121D8[+0x44-8]
  12326 z = 0x180 ; x += (+0x45 b0 ? +0x18 : -0x18) ; list=4 ; if +0x45 b1: list=0
  12420 cell: !tag: +0x45 b1 ? 0x12282 {0x122E6, mode1, n7, dur 10×7, spr 35 34 33 32 31 66 67}
                           : 0x122A6 {0x122E6, mode1, n5, dur 10×5, spr 2F 2E 2D 66 67}
              tag: 0x122C2 {0x122E6, mode1, n7, dur 10×7, spr 37 34 33 32 31 66 67} ; list=0
  (dur raw 0x10 = 17 ticks/frame: near 119, far 85, tag 119 ticks)
latched !tag: (near: frame==4 cnt0 | far: frame==2 cnt0) -> list=2 ; z=0x140   ; back on the rope step
  12396 frame==0xFE: state=0 ; spr=facing ; 123AC clr.l $1C1670 ; +0x4A=0 ; f32 &= ~2 ;
        x += (+0x45 b0 ? -0x28 : +0x28)                                       ; step into the ring
latched tag: frame==4 cnt0 -> z=0x140 ; frame==0xFE: state=0 ; spr=facing word ; 123F8 clr.l $1C1670 ;
        bclr #2,+0x33 ; +0x4A=0 ; f32 &= ~2 ; x += (b0 ? -0x18 : +0x18)
```
Entry to 0xA: human DOWN held (0xF4AA), perch timer `+0x46` 0x80 → 0 (0x12244), or `+0xFE b7`
(match-over) at 0x1223C. There is no abort during state 8 (no input, no timeout).
`$1C1670` writers: **set** 0xEEE8 (bit = corner, via 0xEECE from 0xEDC0 human / 0x1D374 AI);
**cleared (whole long)** 0xF998 (round/match reset, under `$1C1678 b7`, with `$1C1674`), 0x123AC /
0x123F8 (climb-down finish), and every dive init: 0x1293E (0x01), 0x12C56 (0x04), 0x13BD4 (0x0C),
0x13FA6 (0x0F), 0x1467C (0x11), 0x14870 (0x12), 0x153CA (0x1B/0x0D). Readers: 0xEDE0 (`!= 0` blocks
all human climbs), 0xEED4 and 0x1D34A (`btst corner`). Single-occupant semantics: only one man on
any post at a time; moves 0x4D/0x88 → state 8 bypass the bit (TODO EXACT).

## 5. World coordinates summary
```
corner  post base 0x121C8  climb x-shift  perch 0x12256   perch spr  climb cell / down cell   lists
0 FL    (0x1DC,0x193)      -0x18          (0x1E4,0x190)   0x30       11F0A / 122A6 (5 fr)     2→4→0
1 FR    (0x314,0x193)      +0x18          (0x310,0x190)   0x30       11F0A / 122A6            2→4→0
2 NL    (0x1BD,0x120)      -0x18          (0x1D0,0x118)   0x36       11EA6 / 12282 (7 fr)     2→0→0
3 NR    (0x338,0x120)      +0x18          (0x31C,0x118)   0x36       11EA6 / 12282            2→0→0
8 tagL  (0x278,0x160)      -0x18          (0x268,0x158)   0x30       11EE6 / 122C2            (unchanged)→0
9 tagR  (0x3C8,0x160)      +0x18          (0x3D8,0x158)   0x30       11EE6 / 122C2
```
z: rope step 0x140 (climb start, first 13 ticks; climb-down last frames), perch 0x180. Facing:
corners 1,3,9 face left during the first rung then flip to face right... i.e. facing at perch =
corner&1 ? RIGHT : LEFT (faces into the ring). No sprite offsets.
Ticks: climb far 65 / near 91 / tag 91; perch max 128; down far 85 / near 119 / tag 119.

## 6. C sketches
```c
/* ---- 0xEDC0: human climb gate, called from walk prefix when nibble != 0, state 0/1 ---- */
static bool climb_gate(eng_state *st, eng_obj *o) {
    if (o->f34 & 1) return false;  if (o->f33 & 4) return false;
    if ((o->zone & 3) == 0) return false;  if (st->corner_bits) return false;
    if (st->flags161 & 2) {                     /* tag / cage */
        if (o->x >= 0x3CF) return corner_claim(st, o, 9);
        if (o->x <  0x271) return corner_claim(st, o, 8);
        return false;
    }
    struct { int c; bool xlo; s16 xlim, ylim; bool ylo; u8 mask; } W[4] = {
        {2,true,0x1C5,0x11A,true,8},{3,false,0x32B,0x11A,true,8},
        {0,true,0x1F1,0x197,false,4},{1,false,0x2FC,0x197,false,4}};
    for (int i=0;i<4;i++) {
        bool xin = W[i].xlo ? o->x < W[i].xlim : o->x >= W[i].xlim;
        bool yin = W[i].ylo ? o->y < W[i].ylim : o->y >= W[i].ylim;
        if (xin && yin && hold4(o, W[i].mask) && corner_claim(st, o, W[i].c)) return true;
    }
    return false;
}
static bool hold4(eng_obj *o, u8 mask) {           /* 0xEF6A */
    if (!(o->joy_held & mask)) { o->ac = 0; return false; }
    if (!(o->ac & 0x8000)) o->ac = 0x8004;
    if (--o->ad_lo) return false;  o->ac = 0; return true;
}
static bool corner_claim(eng_state *st, eng_obj *o, int c) {   /* 0xEECE */
    if (st->corner_bits & (1u<<c)) return false;
    if (o->ac == 0) o->ac = 0x8004;
    st->corner_bits |= 1u<<c; o->grap44 = c; o->f4c = 0; o->state = 8; return true;
}
/* ---- state 8 handler 0x11FAE (corners 0-3) ---- */
static void climb_tick(eng_state *st, eng_obj *o) {
    int c = o->grap44 & 0xFF;
    if (!(o->latch & 0x80)) {          /* anim stepper latches; cell = c<2 ? 0x11F0A : 0x11EA6 */
        o->mover = 0; o->f32 |= 2; o->x = post[c].x; o->y = post[c].y; o->z = 0x140;
        o->facing = (c & 1) << 7; o->list = 2; voice(o->id, 0x17); sfx(0x32);
        if (!(st->flags161 & 1)) o->f4a = 1;
    } else if (o->frame == 1 && o->count == 0) {
        o->x += (c & 1) ? 0x18 : -0x18; o->z = 0x180; o->facing ^= 0x80; o->list = (c & 2) ? 0 : 4;
    } else if (o->frame == 0xFE) { o->state = 9; o->list = 0; }
}
/* ---- state 9 handler 0x121E6 (code-only cell) ---- */
static void perch_tick(eng_state *st, eng_obj *o) {
    int i = o->grap44 & 0xFF;
    if (!(o->latch & 0x80)) {
        o->latch |= 0x80; o->mover = 0; o->list = 0; o->timer46 = 0x80;
        o->x = perch[i].x; o->y = perch[i].y; o->z = 0x180;
        o->spr = (o->facing << 8) | perch_spr[i & 3];            /* 30 30 36 36 */
    } else if ((o->fe & 0x80) || --o->timer46 == 0) o->state = 0xA;
    o->partner = 0;
    /* human: 0xF4AA  if (joy_held & 8) state = 0xA;  buttons -> selector cat 0xE/0xF */
}
/* ---- dive init common (0x153CA / 0x13BD4 / 0x13FA6 / 0x1467C / 0x14870) ---- */
static void dive_init(eng_state *st, eng_obj *o, eng_obj *v, s16 vz, s16 grav, int k) {
    o->mover = 0; st->corner_bits = 0; o->f32 &= ~2; o->vz = vz; o->grav = grav;
    homing_launch(o, v, k);               /* 0x26AE: vx,vy toward v + tbl[k], clamp 0x200/0xC0 */
}
/* ---- 0x77: held man flinches (what "HIT CLIMBER" really does) ---- */
static void held_flinch_tick(eng_obj *o, eng_obj *holder) {
    if (!(o->latch & 0x80)) holder->state = 0xFF;                      /* hide the holder */
    else if (o->frame == 0xFE) { o->state = 5; o->move_id = 0x8061; holder->state = 5; holder->move_id = 0x8037; }
}
```
