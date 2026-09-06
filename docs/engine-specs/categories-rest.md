# Attack-selector categories not yet in `walk_logic`: 4, 8, 0xE, 0xF, 0x11, 0x12, 0x13, 0x14

Read-only transcription, 2026-08-22. Source `reference/maincpu.asm` + raw ROM bytes
(`data/rom/31e14-0.ic18` even / `31e13-0.ic19` odd; misaligned handlers re-walked with
`tools/m68k_dasm_dump`). Chain order and the fetch at `0xDF96` are exactly as
`docs/engine-specs/strikes.md` §1d/§1e; nothing new was found in `0xE232-0xE4BA` beyond
the 21 categories already listed there (the chain is 0xE002, 0xE02E, 0xE0B8, 0xE110,
0xE180, 0xE1B0, 0xE1D4, 0xE27A, 0xE2DC, 0xE382, 0xE40C, 0xE442, [0xE496 run-in branch only],
0xE4BA).

**Premise correction (important):** cats 0xE/0xF are NOT "downed self". The test
`0xE27A` requires own `+0x21 == 9`, and state 9 is the **turnbuckle perch** (reactions.md
§2f: state 8 = climb, 9 = perched, 0xA = climb down). A mat-downed wrestler is state 4
(react 8/9) and matches nothing in the chain — there is no spring-up/roll selector; the
only get-up is the victim-side timer (reactions.md §2e). Cats 0xE/0xF are the top-rope
dives: 0xE onto a standing opponent, 0xF onto a lying/climbing one.

Field names: `o->state`(+0x20), `move_id`(+0x60, bit7 = "hit/confirmed" latch),
`react_id`(+0x64), `partner`(+0x26, bit7 = poison), `count`(+0x22), `frame`(+0x24/+0x25),
`mover`(+0x01), `vx/vy/vz/grav`(+0x58/5A/5C/5E), `facing`(+0x2E bit7), `f32..f37`, `atk`(+0x4C),
`dmg`(+0x68), `lasthit`(+0x92), `off_x/off_y`(+0x18/+0x1A), `grap44`(+0x44), `x/y/z`(+0x06/0A/0E),
`col` = B1:0, B2:1, both:2. `rom8(a)` = ROM byte.

## Map rows (ROM `*(0xE4FE + id*4) + cat*3 + col`, verified bytes)

| id | map | cat 4 | cat 8 | cat E | cat F | cat 11 | cat 12 | cat 13 | cat 14 |
|---|---|---|---|---|---|---|---|---|---|
| 0 | E52E | 21 21 FF | 29 29 29 | 1B 1B 1B | 0F 0F 0F | 1E 1E 1E | 0A 10 FF | 39 1C 1C | 00 72 FF |
| 1 | E56E | 2E 2E FF | 29 29 29 | 1B 1B 1B | 0C 0C 0C | 1E 1E 1E | 0A 10 FF | 39 1C 1C | 00 72 FF |
| 2 | E5AE | 03 03 FF | 29 29 29 | 0D 0D 0D | 0C 0C 0C | 1E 1E 1E | 0A 10 FF | 39 1C 1C | 00 72 FF |
| 3 | E5EE | 31 31 FF | 29 29 29 | 0D 0D 0D | 0F 0F 0F | 1E 1E 1E | 0A 10 FF | 39 1C 1C | 00 72 FF |
| 4 | E62E | 01 01 FF | 29 29 29 | 1B 01 04 | 0C 04 0C | 1E 1E 1E | 0A 46 FF | 39 1C 1C | 00 72 FF |
| 5 | E66E | 2E 2E FF | 29 29 29 | 1B 1B 1B | 0C 0C 0C | 1E 1E 1E | 0A 46 FF | 39 1C 1C | 00 72 FF |
| 6 | E6AE | 27 27 FF | 29 29 29 | 1B 1B 1B | 0F 0F 0F | 1E 1E 1E | 0A 0B FF | 39 1C 1C | 00 72 FF |
| 7 | E6EE | 07 07 FF | 29 29 29 | 1B 1B 1B | 0C 0C 0C | 1E 1E 1E | 0A 46 FF | 39 1C 1C | 00 72 FF |
| 8 | E72E | 21 21 FF | 29 29 29 | 0D 0D 0D | 0F 0F 0F | 1E 1E 1E | 0A 10 FF | 39 1C 1C | 00 72 FF |
| 9 | E76E | 2E 2E FF | 29 29 29 | 0D 0D 0D | 12 12 12 | 1E 1E 1E | 0A 46 FF | 39 1C 1C | 00 72 FF |
| 10 | E7AE | 27 27 FF | 29 29 29 | 1B 1B 1B | 0F 0F 0F | 1E 1E 1E | 0A 10 FF | 39 1C 1C | 00 72 FF |
| 11 | E7EE | 03 03 FF | 29 29 29 | 1B 1B 1B | 11 11 11 | 1E 1E 1E | 0A 47 FF | 39 1C 1C | 00 72 FF |

After the fetch (0xDFD6-0xDFE8): `0xE926` tag remap (only touches 0x48), `0xEF9A` proximity
remap (class byte `0xF070[id]`: 0x0A→03, 0x0B→04, 0x10→05, 0x0F→00, 0x46/47→05, 0x11/12/1B/
0D/0C/1E/39/1C(05)/27/2E/31/03/07→00 except 0x1C=05, 0x21/0x01/0x04=00), then the `0xE82E`
facing/zone remap only when D3==1 (cat 4 sets D3; none of the others here do), then
`state=5; move_id=entry`. `entry==0xFF` → `state=2; partner=-1` (run).

## Category conditions

### Cat 4 — anti-run matrix `0xE2DC`, value 4 (same test as cat 3, already in engine)
`f34` bit5 clear; opp `f33` bit4 set (opp running); own state 0/1; `alt62 ^= 0x80`;
`cat = rom8(0xE33A + bank*0x24 + opp->band*0xC + id)`; `D3=1`; partner link `0xF178`.
The `0xE33A` matrix yields 4 for bands 1-2 (rows 0x0C.. of each bank) and 3 for band 0,
with single 3s at bank0 row1 col2 (`0xE34E`) and bank1 row1 col1 (`0xE372`) — read it, do not
hard-code. Col 2 (both) is 0xFF everywhere → run.

### Cat 8 — `0xE180`: own state **0x0B** (front tie-up, from `0xF726` lock-up), partner
`+0x26` valid, own `+0x44` high byte bits 5 and 6 clear. Match: `bset #6, partner->+0x44`
(high byte), D0=8. Map "29 29 29" for every wrestler = the tie-up knee (grapple-moves.md
§1, handler `0x16582`). It arises **only inside the collar-and-elbow lock-up** (state 0x0B),
i.e. after `eng_tieup_scan` resolves 0x0B/0x0C; pressing any button there = knee. No
`0xF178`, no `D3`.

### Cat 0xE / 0xF — `0xE27A`: own state 9 (perched on the turnbuckle)
```
0xE27A  A1 = opp (+0x7A); own +0x21 != 9 -> no
0xE288  tag mode ($1C0161 bit1) -> bset #2,+0x33   (side effect, BEFORE the pick: the
                                                    diver becomes the illegal man)
0xE298  opp +0x21==4 && opp +0x65 in {8,9}  -> 0xF   (lying)
0xE2B2  opp +0x21==5 && opp +0x61==0x61     -> 0xF   (opp climbing the corner)
        else                               -> 0xE   (standing / anything else)
0xF178 partner link either way.
```
No distance test here — whiff handling is inside the dive handlers (`0x11152`/`0x11192`
boxes on landing). The press reaches here only because `0xDE86` runs for state 9 (state
9's own handler `0xF4AA` only watches DOWN → state 0xA). All four dive handlers start
with `bclr #1,+0x32` — bit1 is set by the climb state 8 (`0x11FBA/0x1208A/0x1212E`) and
means "launched from the top".

### Cat 0x11 — `0xE002`: `f74` bit7 (holding a weapon) && own state 0/1. No link, D3=0.
Map "1E 1E 1E" for all: weapon swing. Tested FIRST in the chain, so a weapon holder can
never run with both buttons.

### Cat 0x12 — `0xE40C`: own state 0/1; opp state 5 && opp `+0x61 == 0x61` (opp climbing).
`0xF178` link. Rows: B1 = 0x0A (stomp → via `0xF070[0x0A]=03` box), B2 = 0x10 / 0x46 /
0x47 / 0x0B (leap onto the climber), both = run.

### Cat 0x13 — `0xE496`, **run-in branch only** (`0xDED0`: `f32` bit0 set → `0xDF86`):
own state 1 && `+0xAF == 1` (tag run-in sub 1, set by `0x10E86`). `0xF178` link.
Rows "39 1C 1C": B1 = 0x39 (run-in strike), B2/both = 0x1C (behind grab,
move-handlers-1.md §2.9). Else the run-in branch falls to cat 0 (0xE4BA), which refuses
while `f32` bit0 is set → no pick.

### Cat 0x14 — `0xE442`: own state 0/1; opp state 5 && opp `+0x61` ∈ {0x25, 0x36, 0x53,
0x52} (opp mid-throw-hold / held). `0xF178` link. Rows "00 72 FF" = jab / kick / run for
everyone — it exists only to link `+0x26` so the jab/kick interrupts the hold.

## Handlers (cell = `L(0x12614+id*4)`; dur raw, ticks = d+1; all mode 1 hold-last)

Common helpers: `0x26AE(k)` homing launch (table `0x275C[k]` dx,dy; move-handlers-1 §2.12),
`0x258E(k)` **launch preset**: `vx = 0x25CA[k].vx (neg if facing bit7); vy=0; vz; grav; mover=2`
— entries used: 0:(0xC0,0x300,0x38) 9:(0x80,0x400,0x40) A:(-0x280,0x500,0x60)
1B:(0,0x800,0x60) 1C:(-0x300,0x700,0x60) 22:(-0x180,0x280,0x50) 23:(-0x200,0x400,0x58).
`0x1110E` thud sfx, `0x2052` sfx, `0x10BD0` add_pos_delta(dx,dy,dz; dx mirrored by facing),
`0x10B9A` carry partner, `0x11152` hit-downed box, `0x11192` hit-climber box, `0x11412`
mutual link, `0x10FC6` ropes probe, `0x1108C` CPU-roll, `0x110E0` teammate break-in,
`0x10D3A(k)` overlay effect. "MISS" = the shared faceplant: `state=4; react_id=5; spr=0x1F|facing;
dmg=3; sfx 0x32; $1C15D2=0x0F1A; partner|=0x80`. "FINISH" = `frame==0xFE` → `state=7` (rise),
`partner|=0x80`, spr 0x68|facing. "HIT DOWNED" = `victim.state=4; react&=0xFF; react_low = low-2
(8→6, 9→7); victim +0xC7 += 1`. "HIT CLIMBER" = `victim.state=5; victim.move_id=0x77`.
`+0x42` = flight floor bias (rope-fall.md §3). `$1C1800=0x0E` = screen shake.

### Cat 4 moves (anti-run swings; 0x21 already in move-handlers-1 §2.8)

**0x2E** cell 16A56 h 16A7A n7 dur 20 02 08 09 06 FF00 10 spr B6-BC (w1/w5/w9, flying tackle)
```
init:      mover=0; lasthit=0; off_x=0x40; off_y=0x50
bit7 clr & frame0: atk=5; if lasthit && lasthit.state==4 && lasthit.react_low==0x1F:
              partner=lasthit; f33 bit6 both; victim.state=0xFF (hidden); victim.partner=self;
              move_id|=0x80; count=0; off_x=0x30
           frame0 count==0 no hit: state=0; partner|=0x80; spr=0x68|facing; off_x|=0x8000; count=2
frame1 cnt0: off 0x40/0x50.  frame2 cnt0: 0x258E(9); f33 bit3; +0x3E=0x50; $1C15D2=(id,0x13)
frame5 landed(f37 b4): thud; mover=0; count=0; +0x3E=0; shake
FINISH: victim.facing=own; carry(0x80,-1,0); add(0x20,0,0); off_x|=0x8000; state=7; 0x1108C;
        victim.state=4 react=6 dmg=0x10; 0x110E0; victim+0xC7+=5; victim.spr=facing; victim.facing^=0x80;
        victim.react|=0x20; victim.f32|=8; sfx 0x32; partner|=0x80; clr f33 bit6 both
```
**0x31** cell 16FD8 h 17000 n8 dur 20 04 04 04 04 FF00 08 1C spr C2-C9 (w3, catch-and-slam)
```
init: mover=0; lasthit=0; off_y=0x20
frame0 (bit7 clr): atk=0x1B; confirm lasthit react 0x28 -> link both ways, f33 b6 both,
        victim hidden (state 0xFF), move_id|=0x80, count=0, frame+=1
frame1 cnt0 (no hit): off_x|=0x8000; state=0; spr=facing; partner|=0x80; count=2
frame4 cnt0: 0x258E(0x22).  landed: mover=0; count=0; shake
frame6 cnt0: thud; victim.state=4 react=0x2A |0x20, +0xC7+=5, dmg=0x16, f32 b3; 0x1108C; 0x110E0;
        carry(0x10,1,0); clr f33 b6 both
FINISH: off_x|=0x8000; state=7; spr 0x68; partner|=0x80
```
**0x03** cell 12AE6 h 12B16 n4 dur 08×4 spr 1AA 1AB 8100 0000; alt cell 12AFE spr 9D 9E 8100 0000
(w2/w11, flying shoulder/clothesline)
```
init: mover=0; lasthit=0
bit7 clr: atk=3; frame>=2: atk=0x12; confirm lasthit react 0x11 mutual -> move_id|=0x80; anim_sel=5
          (bit15 clear: restarts the alt cell); partner=lasthit
bit7 set & frame1 cnt0: victim.state=4; victim.react_low=0x19; victim.spr=0xB3|own facing; carry(-0x10,-1,0x28)
FINISH: state=0; partner|=0x80.   Every frame: bit7 set -> A1 = cell 0x12AFE
```
**0x07** cell 130E6 h 13116 n4 dur 04 08 14 04 spr BF C0 C1 C0; alt 130FE (dur 04 04 14 04) when id!=7
```
init: mover=0.  committed: atk=1; (D0=1/2 by id==7, dead); frame==1: atk=0x1C
FINISH: state=0; partner|=0x80.   id!=7 -> A1 = cell 0x130FE   (w7 only reaches it; TODO EXACT: w7 is id 7?)
```
**0x27** cell 16388 h 1639C n3 dur 0C 0C 20 spr BD BE 0000 (w6/w10)
```
init: mover=0; lasthit=0
bit7 clr: atk=0x10; confirm lasthit.state==4 && react_low==0x14 -> move_id|=0x80; frame=1; spr=0xFFFF; count=0xC
frame1 cnt0 (bit7 clr) or FINISH: state=0; partner|=0x80
```
**0x01** cell 128DE h 128FA n5 dur 04 08 FF00 FF00 10 spr 85 AB AC 17A 17A (w4: cat 4 and cat E B2 — dropkick)
```
init: if (f32 & 2) { f32&=~2; 0x258E(+0x1F==2 ? 0x1C : 0x1B); f33|=8 }        ; from the top
      else { vz=0x700; grav=0x60; 0x26AE(0); $1C1670=0; mover=2; +0x4A=0 }      ; ground dropkick
tick:  0x10FC6 ropes -> state=4 react=0x17 dmg=3 sfx 0x32 (fell out)
       atk=0 ; frame2: atk=0x18; if z <= (f33 b2 ? 0x110 : 0x150): count=0; add(-0x28,0,0)
       frame3 && landed: mover=0; count=0; thud
FINISH: state=7; partner|=0x80
```

### Cat 0xE dives onto a standing opponent

**0x1B / 0x0D** cells 15384 / 153A0, one handler `0x153BC`, n5 dur 08 18 18 FF00 10 (0x1B) /
08 10 18 FF00 10 (0x0D), spr 84 85 15B 15C 15C / 84 85 15D 15E 15E
```
init: mover=0; $1C1670=0; f32&=~2; vz=0x680; grav=0x48
      grap44 = (f34 & 8) ? 0 : (victim.state==5 && victim.move_low==0x53) ? 1 : 2
      if +0x45==2: facing = (own.x < victim.x) ? right : left
      d = (+0x45 ? +0x10 : -0x10), mirrored by facing; victim.x += d; 0x26AE(0); victim.x -= d
tick: frame0 cnt0: mover=2.  frame3: +0x42=-0x10; landed: thud; count=0; mover=0; +0x42=0;
      switch(grap44):
       0: f34&=~8; victim.state=4 react=0x1C dmg=0x12; $1C15D2=0x0F19; sfx 2B,32; 0x21424 (tag bookkeeping)
       1: victim still 0x53 && |dx|<0x18 && |dy|<0x14: victim.state=4 react=3; victim.partner->state=5
          move_id=0x6F; clr f33 b6 both; dmg=0x12
       2: !victim.f32 b0 && (tag? victim f33 b2 : 1) && victim.state in {0,1} or (4, react 1)
          && |dx|<0x30 && |dy|<0x30: sfx 2B,32; victim.state=4 react=0x20; victim.facing=own^0x80;
          dmg=0x12; +0xC7+=1; victim weapon (f74 b7) -> drop (weapon obj +0x1C=2, +0x76=0, f74=0, off_x|=0x8000);
          $1C15D2=(id, move==0x1B ? 0x0F : 0x0E)
FINISH: state=0; spr=facing; +0x4A=0; partner|=0x80.   No MISS reaction (lands standing).
```
`f34` bit3 meaning (set at `0x12C62` by move 0x04, `0x19704`, `0x1ABEA`) — TODO EXACT.

**0x04** cell 12BDA h 12C22 n4 dur 08 10 FF00 20 spr A4 A5 A6 17A (w4 cat E both / cat F B2,
missile-dropkick-catch). Very long handler; core:
```
init: lasthit=0; z+=0x30; f33|=8; f33&=~0x10; if !(f32&2) { 0x258E(0xA) } else
      { $1C1670=0; mover=2; f34|=8; f32&=~1; vx=0x400 vz=0x200 grav=0x38 vy=0x100, vx/vy negated if f33 b7 }
frame<2 (bit7 clr): atk=0; if z > (f33 b2?0x120:0x160): atk=0x17; confirm lasthit react 0x12 mutual ->
      mover=0; move_id|=0x80; 0x110E0; z=victim.z; anim_sel=5 (restart); if !(f34&8): 0x1108C, partner=lasthit
      else: pos=victim pos; off_x=0x10 off_y=0x40; partner=victim.partner
bit7 & frame1 cnt0: if f34 b3 clr: z+=0x40; off_x|=0x8000; victim.state=4 react=0x19; victim.f34|=8;
      victim.spr=0xB3|facing; carry(0x10,-1,0x10)   else (three-way, A3=victim.partner): +0x4A=0; ... 
      A3.state=5 move=0x8006; A3.facing^=0x80; victim.state=4 react=0x19 dmg=0x20 ... (0x12DAE-0x12E38)
frame2: mover=2; landed: thud; count=0; mover=0; f33&=~0x18; facing^=0x80; off_x=0xD0
frame<3: +0x3E=-0x10; 0x10FC6 ropes -> state=4 react=0x17 dmg=3
FINISH: off_x|=0x8000; state=7; spr 0x68; add(-0x30,0,0); partner|=0x80
bit7 set -> A1 = cell 0x12BF2 (or 0x12C0A when f34 b3)
```
TODO EXACT 0x12DAE-0x12E38 (the double-victim branch) before implementing.

### Cat 0xF dives onto a lying / climbing opponent

**0x0F** cell 13F78 h 13F90 n4 dur 0C 0C 14 FF00 spr 84 85 1A6 1A7 (w0/3/6/8/10 splash → pin)
```
init: mover=0; $1C1670=0; f32&=~2; vz=0x700 grav=0x48; 0x26AE(1)
frame0 cnt0: mover=2.  frame>=2: +0x42=-0x18; landed: thud; +0x42=0; mover=0; shake; +0x4A=0
   0x11152 hit downed: 0x11412 mutual? else MISS(+bchg facing, add(-0x30,0,0));
        state=5 move_id|=0x80; victim.state=5 move=0x51 (pinned under splash); f33 b6 both;
        facing=victim's; dmg=0xF; sfx 32,2B; $1C15D2=(id,0x15);
        CPU game && !f33 b2 && f33 b0 && victim f33 b0: f35|=1; pos=victim pos; add(0x40,-8,0)
   else victim is 0x54: state=4 react=0x1E; facing^=0x80
   else 0x11192 climber: state=5; 0x213A6 on victim (tag legal bookkeeping); move_id|=0x80;
        victim.state=5 move=0x51 dmg=0xF; sfx; f33 b6 both; f35|=1; facing=victim's;
        victim.partner->state=0; victim.partner=self; pos=victim pos; add(0x40,-8,0)
   else MISS (facing^=0x80, add(-0x30,0,0))
bit7 set -> A1 = hidden cell 0x125C0 (the pin cell draws both)
```
**0x0C** cell 13B98 h 13BC8 n4 dur 08 10 18 FF00 spr 84 85 1C1 1C2; alt 13BB0 (spr ...1C3) when facing bit7
(w1/2/5/7, w4 B1/both: top-rope elbow/splash, no pin)
```
init: mover=0; $1C1670=0; f32&=~2; vz=0x700 grav=0x48; 0x26AE(victim climbing 0x61 ? 5 : 2);
      facing = (vx & 0x8000) ^ 0x8000
frame0 cnt0: mover=2; f33|=8
landed (bit7 clr): shake; +0x4A=0; move_id|=0x80; 0x11152 -> HIT DOWNED; else 0x11192 -> HIT CLIMBER;
      (either) mover=0; count=0x10; y=victim.y-1; sfx 2B,32; victim.dmg=0xF; +0xC7+=1
      else MISS.   FINISH: state=7; partner|=0x80
```
**0x11** cell 14658 h 14670 n4 dur 08 08 28 FF00 spr 84 85 EF F0 (w11)
```
init: as 0x0C but 0x26AE(victim 0x61 ? 5 : 0xA).  frame0 cnt0: mover=2; f33|=8
frame1 cnt0: off_y=0x20; +0x42=0x20
frame3 landed: off_x|=0x8000; +0x42=0; thud; shake; +0x4A=0;
      0x11152: mover=0; count=0x18; y=victim.y-1; HIT DOWNED; victim.dmg=0xD
      0x11192: same, off_y=0xC; HIT CLIMBER; victim.dmg=0xD
      else MISS (dmg=4).   FINISH: state=7; partner|=0x80; spr 0x68|facing
```
**0x12** cell 147E6 h 14826 n6 dur 0C 0C 0C 0C 20 FF00 spr 8B 8C 8E 8F 15F 1B1; alt 14806 when +0x45 bit1
(w9 moonsault)
```
init: mover=0
frame1 cnt0 && human: sfx 0x31; frame=3 (skip the wind-up; CPU plays frames 1-2)
frame3 cnt0: mover=2; $1C1670=0; f32&=~2; f33|=8; add(0,0,0x20); vz=0x580 grav=0x48; 0x26AE(0x61?5:2)
frame5 landed: thud; mover=0; +0x4A=0; 0x11152: count=0x18; HIT DOWNED; y=victim.y-1; +0xD2+=1; dmg=0xA
      0x11192: count=0x18; HIT CLIMBER; y; off_y=0xC; dmg=0xA; +0xC7+=1
      else MISS (dmg=4; facing^=0x80; add(-0x30,0,0))
FINISH: state=7; partner|=0x80
```

### Cat 0x11 — weapon swing **0x1E** cell 159F8 h 15A20 n3 dur 08×3 spr 74 75 76; alt 15A0C spr 79 7A 7B
when `+0x75 != 0` (weapon type low byte)
```
init: mover=0; off_y=0x20; lasthit=0
frame1: atk=0xF (else atk=0); frame1 cnt0 && lasthit: state=0; count=2; spr=facing; off_x|=0x8000;
      DROP weapon: f74=0; W=+0x76; +0x76=0; W.anim_sel=2; W.x = x ± 0x60 (facing); W.y=y; W.z=z+0x40
FINISH (frame 0xFE) && f74 != 0: same drop but W.z=z+0x10; state=0; spr=facing; off_x|=0x8000; partner|=0x80
```
(Hit on the weapon swing makes you drop it — one use per pick-up.)

### Cat 0x12 leaps **0x46 / 0x47** cells 14424/14440 h 14458 (w4/5/7/9 B2 = 0x46; w11 B2 = 0x47)
n5 dur 08 0C 10 FF00 08 spr 84 85 1B0 1B1 1B1 / n4 dur 08 0C FF00 10 spr 84 85 1C1 1C2
```
init: mover=0; vz=0x600 grav=0x48; 0x26AE(victim 0x61 ? 5 : 2); facing=(vx&0x8000)^0x8000
frame0 cnt0: mover=2.  frame1 cnt0: off_x=0xF0 off_y=0x28 (0x47: F8/F0)
landed: off_x|=0x8000; thud; mover=0;
   0x11152: 0x11412 mutual else MISS; count=0; move_id|=0x80; y=victim.y-1; HIT DOWNED; dmg=0xA; sfx 2B,32;
            (0x46: victim +0xE0++ ; 0x47: +0xE2++)
   0x11192: count=0; move_id|=0x80; y; off_y=0xC; HIT CLIMBER; dmg=0xA; sfx; +0xC7++
   else MISS (facing^=0x80; 0x46 only: add(-0x30,0,0))
FINISH: off_x|=0x8000; state=7; spr 0x68; partner|=0x80; add(0x18,-1,0)
```
**0x0B** cell 13A1E h 13A2E n2 dur FF00 10 spr F1 F2 (w6 B2)
```
init: if victim react_low==9: facing=0; x = victim.x ± 0x38 (victim facing); facing=right if x>=old; add(0x38,-8,0)
      else: x=victim.x; y=victim.y-1; add(0x78,-1,0).   0x258E(0x23)
landed: count=0; mover=0; victim state 4 react 8/9: victim.state=4; react-2; +0xD4++; dmg=0xB; sfx 29,32
        else 0x11192: count=0; HIT CLIMBER; dmg=0xB; sfx 32,29; +0xC7++   else MISS
FINISH: state=7; partner|=0x80
```

### Cat 0x13 run-in strike **0x39** cell 17B5C h 17B70 n3 dur 06 04 0A spr 1E2 1E3 1E4
```
init: mover=0.  atk=0; frame==2: atk=0x19
overlay: k = frame + (count==0); k in {0,3}: 0x10D3A(0x15); k==1: 0x10D3A(0x16); else 0x10D3A(0x17)
FINISH: state=1; +0xAE=1 (back to the run); partner|=0x80; spr=facing
```

## C sketch — selector additions (engine names)

```c
/* in walk_logic, before the existing `if (o->btn_new & 3u)` block: */
if ((o->btn_new & 3u) && o->opp >= 0) {
    eng_obj *opp = &st->obj[o->opp];
    int cat = -1;
    if (o->f74 & 0x80u && state <= 1) cat = 0x11;                   /* 0xE002, first */
    else if ((state & 0xFF) == 0x0B && o->partner >= 0
             && !(o->grap44 & 0x6000u)) {                            /* 0xE180 cat 8 */
        st->obj[o->partner].grap44 |= 0x4000u; cat = 8;
    } else if ((state & 0xFF) == 9) {                                /* 0xE27A */
        if (st->tag_mode) o->f33 |= 4;
        cat = (((opp->state & 0xFF) == 4 && ((opp->react_id & 0xFF) == 8 || (opp->react_id & 0xFF) == 9))
            || ((opp->state & 0xFF) == 5 && (opp->move_id & 0xFF) == 0x61)) ? 0xF : 0xE;
        o->partner = o->opp;
    } else if (state <= 1 && (opp->state & 0xFF) == 5 && (opp->move_id & 0xFF) == 0x61) {
        cat = 0x12; o->partner = o->opp;                             /* 0xE40C */
    } else if (state <= 1 && (opp->state & 0xFF) == 5) {
        unsigned m = opp->move_id & 0xFF;
        if (m == 0x25 || m == 0x36 || m == 0x53 || m == 0x52) { cat = 0x14; o->partner = o->opp; }
    }
    /* run-in branch (f32 bit0): only 0xE496 then 0xE4BA */
    if (cat < 0 && (o->f32 & 1u) && state == 1 && (o->substate & 0xFF) == 1) { cat = 0x13; o->partner = o->opp; }
    /* then the existing fetch: entry = rom8(rom32c(0xE4FE+id*4) + cat*3 + col) ... */
}
```
Chain order to preserve: 0x11, 1/2, 5, 6/7, 8, 9, 0xA-D, 0xE/F, 3/4, 0x10, 0x12, 0x14, then 0
(run-in branch: 0x13, 0). Cat 4 needs no new selector code — only the map rows above and
the 0xE82E zone remap (D3=1) that cat 3 already shares.

## Open items (TODO EXACT)
* `f34` bit3 semantics (0x0D/0x1B `grap44=0` branch; set by 0x04 at 0x12C62, 0x19704, 0x1ABEA).
* Move 0x04 double-victim branch 0x12DAE-0x12E38 (writes a third object A3 = victim.partner).
* `0x213A6` / `0x21424` tag legal-man bookkeeping (used by 0x0F climber hit and 0x0D case 0).
* Victim-side scripted moves spawned here: 0x51 (pinned under splash), 0x77 (knocked off
  the corner), 0x6F, 0x8006 — cells not traced.
