# Victim side — state 4 reactions, knockdown, energy, hitstun invulnerability

Source: `../wrestlefest-decomp/reference/maincpu.asm` (read-only pass, 2026-08-22),
bytes verified against an interleaved flat image of `data/rom/31e14-0.ic18` (even) +
`31e13-0.ic19` (odd). Cross-checked with `data/romdata/hit_record.json`,
`hit_reaction_handlers.json`, `mv_reaction_cell_ptrs.json`, `mv_state_cell_ptrs.json`,
`docs/engine-specs/anim.md`, `apply-motion.md`, `run-skid-turn.md`, `input-walk.md`,
`docs/referee-1f914.md`. Existing C shims (cycle oracles, "not PORTED"):
`src/frame_24062.c` (hit pipeline), `src/frame_24e58.c` (drain), `src/floor_2818e.c`.

Three premise corrections up front (all proven below):

1. **`+0xFE` is NOT a per-hit event latch.** It is the per-object **match-result word**
   (0 while fighting; `$4000/$4001` pinfall, `$8000/$8001` KO-by-energy,
   `$8002/$8003/$8005` count-out, referee also `bset #7` on the byte). It is written
   ONLY by match-end code (`0x11236-0x11250`, `0x1FE76-0x1FF1C`, `0x200FC-0x20110`,
   `0x20472-0x20490`) and never cleared mid-match (object init clears it). Everything
   that "consumes" it (`0x24126`, `0x24E58`, `0xDCDE`, `0x1B39E`, `0x11E7A`) is a
   match-over freeze/unwind, not per-hit bookkeeping. The real hitstun latch is
   **`+0x4C` (hurt-record id) being cleared** — see §4.
2. **Basic-punch reaction id is `+0x64 = 0`**, written by the **result handler**
   (attacker-record byte 5 → table `0x24398`), not by the `0x243D4` handler. `0x243D4`
   (victim-record byte 6) runs *after* it and only acknowledges / handles special victim
   situations (record 1 → index 0 → `0x24852`, a 3-instruction ack).
3. **States 9/0xA are NOT mat lying/get-up.** They are the **turnbuckle perch /
   climb-down** pair (state 8 = climb up, entered from the corner-climb gate `0xEEF8`).
   Mat knockdown/lying/get-up is entirely `state 4` reactions `2/3/4 → 5 → 8/9`
   followed by **state 7** (rise). input-walk.md's "state 9 = down" note is a mislabel.

---

## 0. Pipeline overview (even frames only)

Frame loop `0xFA4`: `$1C0083` bit0 = frame parity. The **even** path (`0xFB0-0x1050`)
calls, after the object pass `0xF4C2`: `jsr 0x24062` (hit detection + reaction dispatch)
then `jsr 0x24E58` (HP drain) at `0xFEE/0xFF4`. The **odd** path (`0x1054-0x10E0`) calls
neither. So hits connect and HP applies on a strict 2-frame cadence; animation/motion
run every frame.

`0x24062`: `clr.w D0`, then loop `0x24064`: for each live object A0 (iterator `0x250E`
over `0x5122`): if `+0x4C != 0` and `+0x20` bit7 set (state latched): A2 =
`0x24EF6 + (+0x4C & 0xFF)*7` (7-byte hit record, `0x240BC`); if record byte0 **bit7**
set (this record attacks) → `bsr 0x240D8`.

`0x240D8`: for each *other* live object A1 (victim): legality `0x24126` → hitbox
`0x241E8` → dispatch `0x2435A` → on hit `0x24DEC` (attacker epilogue) + `0x24E02`
(victim epilogue).

**Hit record** (`0x24EF6 + id*7`, = `hit_record.json`):
byte0 flags (b7 attacks, b6/b5 test victim box1/box2), byte1/2 victim hurtbox idx
(→ `0x25010`), byte3 attacker hitbox idx (→ `0x24FE4`), byte4 **damage**, byte5
**result-handler idx** (→ `0x24398`), byte6 **reaction-handler idx** (→ `0x243D4`).

`0x2435A`: `jsr 0x24398[attacker_rec.byte5]` (writes the victim's reaction — carry set
= no hit) then `jsr 0x243D4[victim_rec.byte6]` (victim-situation ack).

`0x24090` (called at loop end, `0xF510`): per object, when the sprite word `+0x04`
differs from the shadow `+0x90`: `clr.w +0x8E` (per-attack already-hit slot mask),
`clr.w +0x4C`, `+0x90 = +0x04`. **`+0x4C` is therefore a per-sprite-frame assertion**:
every handler that wants to attack or be hittable must (re)write it.

### The basic punch chain (attacker side, for reference)

Standing attack table: default situation checker `0xE4BA` → D0=0; move id =
`perWrestler(0xE4FE)[situation*3 + button-1]`; wrestler 0 row 0 = `00 72 FF` → **B1 =
move 0x00 (punch)**, B2 = 0x72 (kick). Punch cell `0x12850` {handler `0x12888`, mode 1,
n=5, dur A,6,4,A,2, spr 0,92,93,94,93}. Handler `0x12888`: init nudges X −0x18
(`0x10B62`); per frame `+0x4C = 0x21` (idle/whiff record), **on frame index 3 (extended
fist, spr 0x94): `+0x4C = 7`** (`0x128A4-0x128B0`); on `+0x25==0xFE` → state 0,
`bset #7,(+0x26)`.

hit_record[7] = `{flags C0, box1 0, box2 0, atkbox 6, damage 1, result 1, reaction 0}`
→ punch resolves through **result handler 1 = `0x24408`**. (hit_record[1], flags `40`,
is the **standing/walking hurt record** set every frame by the stand/walk handlers
`0x114B2`/`0x116C6` — its byte6 = 0 selects ack `0x24852`.)

---

## 1. State 4 dispatch and the punch reaction

### 1a. Dispatcher

State table `0xF1E4` (13 entries): entry 4 = **`0xF460` = `rts`** (`0xF1F4`). Nobody in
the state-machine pass moves victims. All victim logic lives in the **anim cell
handlers**: pick `0x1C03E` with `+0x1D == 4` → `A1 = *(0x1AFD4 + (+0x65)*4)`; handler =
cell+0, called every frame with A0=object, A1=cell, A2=partner(+0x26 or $7F000) —
before the tick, may substitute A1 (anim.md §1b). Victims *move* because their handlers
arm the mover: `+0x01=2` + `+0x58/5A/5C/5E` (velocity+gravity mode of `0x2208`).

`0x1AFD4` has **43 entries** (0x1AFD4..0x1B080). `+0x64` is a word: low byte `+0x65` =
cell index; high-byte bits are flags: **bit7 (word bit15) = "critical/band-2 art"**
(set by `0x24E4C`, consumed by handlers as an A1-substitution), **bit5 (word bit13) =
"rise dizzy"** (set by heavy move handlers, e.g. `0x150E8...`, consumed by state 7).

### 1b. Result handler 1 — `0x24408` (punch/jab class)

```
0x24408  if victim +0x4C high-byte bit6 set -> carry, no hit        ; ($4c,A1) btst #6
0x24410  sound 0x2A, sound 0x32 (jsr 0x2052)
0x24424  victim +0x69 = attacker_rec.byte4        ; pending damage (low byte of +0x68) = 1
0x2442A  victim +0x20 = 4                         ; state 4 (bit15 clear => anim restart)
0x24430  victim +0x64 = 0                         ; BASIC FLINCH
0x24436  if !(victim +0x4C high byte bit7):       ; not a "already-special" record
0x2443E    if victim +0x33 bit4 (RUNNING, set by run init 0x11C90)
0x24446    or victim +0x52 >= 3 (combo count)     ; third recent hit
0x2444E      victim +0x64 = 2                     ; ESCALATE TO KNOCKDOWN
0x24454  bsr 0x24D7A                              ; behind-hit remap (below)
0x24458  bsr 0x24DDC                              ; running victim: +0x68 += 1 extra dmg
         carry clear = hit
```

`0x24D7A` (front/back): if `+0x65 < 0x24` and attacker facing == victim facing (hit
from behind, `+0x2E` bit7 compare `0x24D88-0x24DA0`): `+0x65 =
byteTable_0x24DB8[+0x65]`. Table: `0B 0C 0B 0C 0D 05 06 07 08 09 0A 0B 0C 0D 0E 0F`
then `0B`×18, `0C 0D`. So flinch 0→0x0B, dizzy 1→0x0C, falls 2/3/4→0x0B/0x0C/0x0D,
5..0xF unchanged, 0x10-0x21→0x0B, 0x22→0x0C, 0x23→0x0D.

`0x243D4[hit_record[1].byte6 = 0]` = **`0x24852`**: `bset #7,(+0x26,A0)` and
`bset #7,(+0x26,A1)` (MSB flag on both partner pointers = "hit connected", consumed by
the partner-validation 24-bit compares, e.g. `0x115D2`/`0x125D6`), `clr.w (+0x44,A1)`,
`bsr 0x24D7A` (again — idempotent for already-remapped ids), rts. Indices 1-12 of
`0x243D4` (`0x24868/0x24924/...`) are for victims whose *current record* marks a
special situation (in tie-up, holding, being held): they break the grapple, retarget,
and often escalate to `+0x64=2` — out of scope here.

Epilogues (both, on every landed hit):
- `0x24DEC` (attacker): `bsr 0x24334` — set bit `0x8000>>victimSlot` in attacker
  `+0x8E` (can't hit the same victim twice with one attack frame; mask cleared by
  `0x24090` when the attack sprite changes); clear attacker `+0x44/+0x46/+0x4C`;
  attacker `+0x92 = victim` (last-hit pointer).
- `0x24E02` (victim): drop carried weapon (`+0x74` bit7: weapon obj `+0x76` gets
  `+0x1C=2`, clear `+0x74/+0x76/+0x44`); `+0xC6 += 1` (times-hit stat);
  **`clr.w +0x4C`** (victim now unhittable, §4); clear `+0x46/+0x18/+0x1A`;
  `bclr #4,(+0x33)` (stop "running"); `+0x92 = attacker`; **if `+0x70 == 2` (energy
  band 2) → `bset #7,(+0x64)`** (`0x24E4C`) = critical-art flag (word bit15).

### 1c. Reaction 0 — basic punch flinch

Cell `0x1B080` (id 0): `{handler 0x1B090, mode 1 (hold-last), n=2, dur 6,6,
spr 0x0F,0x10}` (sprite xor `+0x2E` flip at load).

Handler `0x1B090`:
```
first frame (+0x1C bit7 clear):            ; 0x1B090-0x1B0A8
    +0x01 = 0                              ; mover OFF - no knockback motion at all
    +0x52 += 1                             ; combo hit counter
    +0x54 = 0x40                           ; combo decay timer (even-frame ticks)
later frames:                              ; 0x1B0AA-0x1B0B8
    if +0x25 == 0xFE:  +0x20 = 0           ; anim finished -> state 0 (stand)
```
Hitstun length: dur 6+6 ⇒ 7+7 ticks + init tick = `+0x25` reaches `0xFE` on tick 15;
the handler (which runs before the tick) writes state 0 on call 15, the stand state
latches at the next frame top (`0xF4E0`). ≈ **15-16 frames**, no displacement, no
velocity writes. The victim never asserts `+0x4C`, so it is unhittable for the whole
flinch (§4). Combo bookkeeping is the only side effect: 3 hits inside the `+0x54`
window (0x40 even-frame decrements ≈ 128 frames, decay in `0x24E92-0x24E9C`)
escalates the *next* result-handler-1 hit to a knockdown.

Reaction 0x0A (`0x1B3D2`, `{0x1B3DE, mode 1, n=1, dur 0x10, spr 0x10}`) is the second
flinch flavor (used by result handlers `0x246EA` etc.): same +0x52/+0x54 init; on
`0xFE`: id 0x20 → `+0x20=4, +0x64=2` (chains into knockdown); else if `+0x32` bit0 →
state 1 sub 1, else state 0 (`0x1B3FE-0x1B438`).

Reaction 1 (`0x1B0BC` `{0x1B0CC, mode 2 loop, n=2, dur 0x10,0x10, spr 0x18,0x19}`) =
**dizzy stagger**: init `+0x01=0`, `+0x46=0x80` (duration), and if `+0x52==0`:
`+0x52=4, +0x54=0x40` — primed so ANY hit while dizzy knocks down. Per frame:
`jsr 0x115D2`, **`+0x4C = 1`** (hittable, standing record), `+0x46 -= 1`; at 0 →
state 0, `$1C1697 = 1`.

---

## 2. Knockdown chain and get-up

### 2a. Which `+0x64` ids knock down

- **2 / 3 / 4** — airborne backward fall, shared cell `0x1B114`
  `{handler 0x1B134, mode 1, n=2, dur 0x10,0xFF00, spr 0x11,0x14}` (0x11 = flying,
  0x14 = flat). Critical variant substitutes cell `0x1B124` (same handler, spr
  0x11,**0x16** face-down art) via `+0x64` bit15 (`0x1B1A8-0x1B1B4`).
  Writers: result handler 1 escalation (`0x2444E`), heavy classes `0x245F0`
  (`+0x64=2`, running victim → 3, `0x24618-0x2462A`), `0x24640` (`+0x64=3`, running →
  4), most `0x243D4` situation handlers (`+0x64=2`), throw results, etc.
- **0x0B / 0x0C / 0x0D** — the same falls when hit **from behind**: cell `0x1B43A`
  `{handler 0x1B440, mode 0}`; handler: `bchg #7,(+0x2E)` (flip 180°), `+0x20 = 4`,
  `+0x65 = +0x65 - 0x0B + 2` (0x0B→2, 0x0C→3, 0x0D→4), `+0x04 = 0xFFFF` (hidden this
  frame), rts. So a punch from behind = flip + knockdown.
- Others in the family: 0x17 whip-rebound fall (`0x1BA82`), 0x14/0x1D clothesline
  falls (`0x1B78A`), 0x0F high fall (`0x1B66C`), 0x10/0x11/0x12 "victim hidden during
  slam" (`0x1B706`: spr 0xFFFF, `+0x44=0x20` countdown → state 0), 0x0E grabbed
  (`0x1B460`).

### 2b. Fall handler `0x1B134` (ids 2/3/4)

```
init (+0x1C bit7 clear):                                ; 0x1B13C-0x1B15E
    $1C1697 = 1                                         ; global event byte
    D0 = +0x65 - 2 ; jsr 0x258E                         ; ARM KNOCKBACK (below)
    D0 = 0x40      ; jsr 0x10F9C                        ; edge-of-ring Y correction
per frame:                                              ; 0x1B160-0x1B1B4
    jsr 0x10FC6                                         ; rope-bump event check
    if +0x24 == 0 && +0x22 == 0:                        ; exact tick frame 0 expires
        (D0,D1,D2)=(-0x10,0,0x10); jsr 0x10BD0          ; facing-relative X-0x10, Z+0x10
    +0x3E = 0x40                                        ; X clip width hint for floor pass
    if +0x37 bit4 (Z floor contact, from bounds 0x280DC):
        +0x3E = 0; +0x20 = 4; +0x65 = 5                 ; -> BOUNCE reaction
        jsr 0x1110E                                     ; thud: sound 0x29 (0x2D if +0x33 bit2)
    else if +0x64 bit15: A1 = 0x1B124                   ; critical art substitution
```

**`0x258E` = knockback launcher** (also used by many throw handlers): entry
`0x25CA + D0*6` = 3 words `{vx, vz, grav}`:
`0: C0 300 38` `1: 180 300 38` `2: 200 300 38` `3: 0 300 38`.
Writes `+0x58 = vx`, `+0x5A = 0`, `+0x5C = vz`, `+0x5E = grav`, **`+0x01 = 2`**
(velocity mover), and `neg.w +0x58` when facing right (`+0x2E` bit7) — i.e. always
knocked *backward*. Units are 8.8 px/frame (`0x2252` adds `vel<<8` to the 16.16
position; gravity subtracts from vz each frame — apply-motion.md). So id 2 = 0.75 px/f
backward, id 3 = 1.5, id 4 = 2.0, all with a 3 px/f pop-up and 0x38/0x100 px/f²
gravity. `0x10F9C(D0=0x40)`: if `Y+0x20 >= 0x198` → `+0x5A = -D0`; else if
`Y-0x20 < 0x118` → `+0x5A = +D0` (push the arc back inside the ring band).
`0x10BD0`: facing-relative position add (negates D0 when facing right) to X/Y/Z.

### 2c. Bounce — id 5 (and 0x2A), cell `0x1B1B8` `{0x1B1D0, mode 1, n=1, dur 0xFF00, spr 0x1F}`

```
init:  bclr #4,(+0x33)                     ; clear running
       jsr 0x10B62(D0=0x50)                ; clipped facing-relative X slide (below)
       jsr 0x258E(D0=3)                    ; vx=0, vz=0x300: straight-up bounce
       clr.w +0xAA; jsr 0x10C60(D0=1)      ; seed mash-out counter (below)
       if +0x65 != 0x2A and +0x71 == 2: bset #7,(+0x64)   ; band-2 -> face-down art
per frame:
       jsr 0x10D04                         ; down bookkeeping (referee flag + mash)
       if +0x37 bit4 (landed again):
           jsr 0x1110E                     ; thud
           +0x20 = 4
           +0x65 = (id 0x2A -> 8) else (bit15 ? 9 : 8)    ; LYING face-up/face-down
       if +0x64 bit15: A1 = 0x1B1C4        ; critical art {0x1B1D0, mode1, n1, FF00, spr 0x16}
```

### 2d. Lying — ids 8 (face-up, spr 0x12) / 9 (face-down, spr 0x13)

Cells `0x1B300`/`0x1B30C` `{handler 0x1B318, mode 1, n=1, dur 0xFF00}` (hold forever;
sprite never changes, so `0x24090` never re-clears state — but `+0x4C` is never
asserted either: a mat-lying victim is NOT hittable through the strike pipeline;
ground attacks are selected as *moves* against a downed opponent by `0xDE86`
(`0xDF06-0xDF3A`: opponent state 4, `+0x65` 8/9) and apply their own damage).

```
init:   +0x01 = 0
        bclr #4,#6,#3,(+0x33); bclr #7,(+0x60); clr +0x52/+0x54/+0xD0
        jsr 0x10F56                        ; seed forced-down time +0x9A (unless a move
                                           ;  already set it): band2 0xE0, band1 0x80,
                                           ;  else 0x30 (CPU with +0x33 bit5/2: 0x30)
        id 9: +0x3E=0x30, +0x40=-0x18, jsr 0x280DC, X += +0x38, Y += +0x3A
        id 8: jsr 0x10B62(0x50)            ; body slide, ring-clipped
per frame:
        if +0x32 bit4 -> RISE              ; forced (opponent picks you up etc.)
        jsr 0x115D2                        ; partner back-pointer sanity
        if +0xFE bit7 -> RISE              ; match over: everyone stands
        A3 = +0x7A; if A3->+0x34 bit4: rts ; OPPONENT IS PINNING: stay down
        jsr 0x10D04                        ; bset #7,$1C167A (referee "body down");
                                           ;  human (+0x56&0xC0==0): each new button
                                           ;  press (+0xA3) decrements +0xAA; at 0 ->
                                           ;  +0xAA = 0x4000 ("mashed out")
        if +0x9A == 0 -> RISE
        else --+0x9A; at 0 -> RISE
RISE:   +0x20 = 7; bclr #7,(+0x64)                        ; 0x1B3C4-0x1B3CE
```
Mash-out seeding `0x10C60`: D0=1 (from bounce): thresholds by quarter of `+0x72`:
hp≤¼→0x15, ≤½→7, ≤¾→3, else 1 press (`byteTable 0x10D00 = 01 03 07 15`,
`0x10CBA-0x10CDE`). D0=0 (hold/pin moves `0x180D6/0x18C32/...`): `+0xAA = 0x100` if
`+0x66==0`, else `0x2A - +0x66` (min 1). `+0xAA == 0x4000` gates the action prefix
`0xEBC4`: a downed player who has mashed out may press attack → state 5 move `0x38`
(spring-up attack) when `+0x65==8` (`0xEC04-0xEC1E`).

### 2e. Get-up — **state 7**, cell `0x11E36` `{handler 0x11E42, mode 1, n=1, dur 0x10, spr 0x68}`

```
init:   +0x01 = 0; bclr #3,#4,#6,(+0x33); clr +0xAA; clr +0x9A
on +0x25 == 0xFE (17 ticks):
        $1C1697 = 1
        if +0xFE != 0 or +0x1F == 5:  +0x20 = 0          ; match over / rose out of a
                                                          ;  move-owned anim: stand
        else bclr #5,(+0x64); if was set:                ; "rise dizzy" flag from heavy
             +0x20 = 4; +0x64 = 1                        ;  moves -> dizzy reaction 1
        else +0x20 = 0
```

### 2f. States 8/9/0xA — the turnbuckle set (for the record; NOT mat get-up)

- State 8 (climb up), cell `0x11EA6` `{0x11F3A, mode 1, n=7, dur 7×0xC, spr
  66,67,31,32,33,34,35}`; sub-handlers `0x12078/0x12122` teleport to the corner from
  tables `0x121C8`/`0x121D8` (rumble), Z=0x180/0x140, `+0x44` = corner index (+0x45
  bits = side/variant), voice `0x17`; on `0xFE` → `+0x20 = 9`
  (`0x1206A/0x12112/0x121BE`).
- State 9 (perched), dispatcher `0xF4AA`: joystick nibble bit3 held (DOWN per the
  `0xF2D6` angle map) → `+0x20 = 0xA`. Cell `0x121E0` `{0x121E6, mode 0}`: pins the
  object at corner table `0x12256` = (484,400)(784,400)(464,280)(796,280), Z=0x180,
  sprite `0x1227E[+0x44&3]` = 30/30/36/36 ^ facing; `+0x46 = 0x80` timeout or `+0xFE`
  bit7 → `+0x20 = 0xA`; `clr.l +0x26` every frame.
- State 0xA (climb down), cell `0x12282` `{0x122E6, mode 1, n=7, dur 7×0x10, spr
  35,34,33,32,31,66,67}` — reverse ladder, repositions at the corner, X∓0x18, layer
  `+0x12 = 4`.

---

## 3. Energy / HP

- `+0x66` current HP, `+0x72` max HP: seeded at wrestler setup `0xE94/0xE98`
  (match-config values), refills `+0x66 = +0x72` at `0x8C3E/0x8C64` (also
  `+0x6A = +0x72` HUD), tag-swap copies `0xD30/0xD96`. `+0x68` = **pending damage
  word** (result handlers write the attack's byte4 into `+0x69`; `0x24DDC` adds a word
  +1 for a running victim; grapple handlers `addi` it directly e.g. `0x2459C` +3).
  `+0x6A` = HUD delta (gauge smear), `+0x70` = energy band word (low byte `+0x71`).

- **Drain `0x24E58`** (even frames only, from `0xFF4`): per live object:
  ```
  if +0x68 != 0 and +0xFE == 0:              ; damage pending, match not decided
      +0x66 = max(0, +0x66 - +0x68)          ; 0x24E72-0x24E7E
      bsr 0x24EC2                            ; recompute band -> +0x70
      +0x6A = -(+0x68); +0x68 = 0            ; HUD gauge animation delta
  +0x54 -= 1; on borrow: +0x54 = 0, +0x52 = 0        ; combo decay (0x24E92-0x24E9C)
  if +0x32 bit3 and +0xD0 != 0: --+0xD0; at 0: bclr #3,(+0x32)   ; post-whip slow-walk
  ```

- **Band calc `0x24EC2`**: if `+0x72 == 0` skip. `D2 = +0x72/3`; band = **2 if
  `+0x66 <= 0x18`**, else **1 if `+0x66 <= 2*(+0x72/3)`**, else **0**; stored to
  `+0x70` (`0x24ED4-0x24EEC`). HUD redraws call it too (`0x188BE`, `0x18AAC`).

- **Band consumers**: band 2 → `0x24E4C` sets `+0x64` bit15 on every further hit
  (critical/face-down art, longer lying); `0x1B208` same at bounce; `0x10F56` forced
  -down time 0xE0 (band2) / 0x80 (band1) / 0x30.

- **At `+0x66 == 0`**: there is no dedicated "down state"; instead
  (a) mash-out seed `0x10C60(D0=0)` = `+0xAA = 0x100` (256 presses — pin escape
  effectively impossible; nonzero HP gives `0x2A - hp`);
  (b) heavy grapple moves call **`0x111C8`** (callers `0x136A8, 0x157D8, 0x15C54,
  0x161E4, 0x179C6, 0x17ED4, ...`): if engaged opponent's `+0x66 == 0` (`0x111F8`) →
  `bset #3,(+0x60)` once; singles (`$1C0161` bit0 clear): `$1C1214 = $8009`, sounds,
  **`+0xFE = $8000` (winner+partner), `$8001` (loser+partner)**, `+0x4A = 4`, victory
  jingle `0xF2A`; rumble path: `+0xC4 += 1`, `jsr 0x21358` (elimination). The pinfall
  itself is the referee's: visual 6 `0x20022` stamps `+0xFE = $4000/$4001` at count 3
  (referee-1f914.md).

---

## 4. Hitstun invulnerability — what actually gates re-hits

Legality `0x24126` (A0 = attacker, A1 = victim; carry set = cannot hit):

1. `tst.w (+0x4C,A1)` — **victim must currently assert a hurt record.** This is the
   real "hitstun latch": `0x24E02` clears it the instant a hit lands, `0x24090`
   re-clears it whenever the victim's sprite changes, and the flinch/fall/bounce/lying
   handlers never write it — so a reacting victim is unhittable for the entire
   reaction. Hittability is opt-in per frame: stand `0x114B2`/walk `0x116C6` → 1,
   dizzy `0x1B0F8` → 1, move-owned "laid on mat" `0x15C2C` → `0x8014` (record 0x14,
   which gets the Y−8 allowance at `0x2417A-0x24182`), attack anims → their own ids.
2. `+0x20` bit7 set — state must be latched (a state written this frame gives one
   frame of grace until pass-4 `0xF4EC` sets bit15).
3. `+0x32` bit6 clear — hard invulnerability flag (set only by the special sequence at
   `0x1AE70-0x1AE76` with bit5, teleport-out logic; cleared by the `0x243D4` situation
   handlers, e.g. `0x24878`).
4. `tst.w (+0xFE,A1) == 0` — match-result word (see correction #1): once a fall/KO/
   count-out is stamped nobody can be hit again. Lifecycle: cleared by object init at
   match setup; written exactly at `0x11236-0x11250` (energy-KO), `0x200FC-0x20110`
   (pinfall count 3), `0x1FE76-0x1FF1C` (count-out), `0x20472-0x20490` (referee
   win-pose `bset #7`); consumed by `0x24126` (no hits), `0x24E58` (no drain),
   `0xDCDE` gate (no player actions), `0x1B39E`/`0x11E7A` (downed men stand up),
   `0x121E6` variants (leave corner).
5. `+0x32` bit2 (facing-changed) only legal when `$1C0161` bit1 set (mode gate).
6. `+0x04 != 0xFFFF` — hidden victims (reactions 0x10-0x12, back-flip frame of
   `0x1B440`) are unhittable.
7. `0x241B6`: attacker's `+0x8E` slot mask — one hit per victim per attack sprite
   frame (set by `0x24334`, cleared with the sprite by `0x24090`).
8. Plus proximity (|ΔY| < 0xC, with −8 for record 0x14; |ΔX| < 0x50) and the
   box-vs-box test `0x241E8` (attacker box `0x24FE4[rec.byte3]` vs victim boxes
   `0x25010[rec.byte1/2]` per flags bits 6/5).

---

## 5. C sketch

New object fields (68k offsets kept for oracle diffing) beyond anim.md's set:

```c
u16 hurt_rec;    /* +0x4C  hit/hurt record id; 0 = unhittable; re-asserted per frame */
u16 combo;       /* +0x52  recent-hit count (>=3 with result 1 => knockdown)         */
u16 combo_t;     /* +0x54  combo decay, even-frame ticks (0x40 per hit)              */
s16 vx, vy, vz;  /* +0x58/+0x5A/+0x5C  8.8 px/f, mover mode 2                        */
u16 grav;        /* +0x5E                                                            */
u16 move_id;     /* +0x60  (bit7 of low byte cleared by lying init)                  */
u16 reaction;    /* +0x64  bit15 critical art, bit13 rise-dizzy, low byte cell idx   */
u16 hp;          /* +0x66 */  u16 dmg_pend; /* +0x68 */  s16 hud_delta; /* +0x6A */
u16 band;        /* +0x70  0/1/2 from 0x24EC2                                        */
u16 hp_max;      /* +0x72 */
u16 hit_mask;    /* +0x8E  per-attack-frame victim slots (attacker side)             */
u16 spr_shadow;  /* +0x90  last +0x04, drives +0x8E/+0x4C auto-clear (0x24090)       */
obj *last_hit;   /* +0x92  set both ways by the epilogues                            */
u16 down_forced; /* +0x9A  forced-down frames (0x30/0x80/0xE0 by band; moves: 8/0x50)*/
u16 mash;        /* +0xAA  presses to escape; 0x4000 = escaped; 0x100 at hp==0       */
u8  mash_goal;   /* +0xAB  (written by 0x10C60 D0=1 path)                            */
u16 hits_taken;  /* +0xC6 */  u16 slowwalk_t; /* +0xD0 */
u16 result;      /* +0xFE  0 fighting; 4000/4001 pin, 8000/8001 KO, 8002/3/5 countout*/
/* flags used: f32 bit4 forced-rise, bit6 invuln; f33 bit4 running; f37 bit4 z-floor */
```

```c
/* ROM 0x24408 — result handler 1 (punch class); A=attacker, V=victim, rec=A's record */
static bool result_punch(Obj*A, Obj*V, const u8*rec){
    if (V->hurt_rec & 0x4000) return false;              /* high-byte bit6 */
    sound(0x2A); sound(0x32);
    V->dmg_pend = (V->dmg_pend & 0xFF00) | rec[4];       /* move.b ->+0x69 */
    V->state = 4; V->reaction = 0;                       /* flinch */
    if (!(V->hurt_rec & 0x8000) &&
        ((V->f33 & BIT4) || V->combo >= 3)) V->reaction = 2;   /* knockdown */
    back_remap(A, V);                                    /* 0x24D7A: same facing ->
                                                            reaction = tbl24DB8[id] */
    if (V->f33 & BIT4) V->dmg_pend += 1;                 /* 0x24DDC */
    return true;
}

/* ROM 0x1B090 — reaction 0 cell handler (called every frame before the tick) */
static void rc_flinch(Obj*o){
    if (!(o->anim_sel & 0x8000)) { o->mode = 0; o->combo++; o->combo_t = 0x40; }
    else if ((o->frame & 0xFF) == 0xFE) o->state = 0;    /* 15 ticks later */
}

/* ROM 0x258E — knockback launcher; tbl25CA[4] = {{0xC0,0x300,0x38},{0x180,..},
   {0x200,..},{0,0x300,0x38}} */
static void launch(Obj*o, int i){
    o->vx = tbl25CA[i][0]; o->vy = 0; o->vz = tbl25CA[i][1]; o->grav = tbl25CA[i][2];
    o->mode = 2; if (o->flip & 0x80) o->vx = -o->vx;
}

/* ROM 0x1B134 — reactions 2/3/4 (airborne fall), cell 0x1B114 / crit 0x1B124 */
static const u8* rc_fall(Obj*o, const u8*cell){
    if (!(o->anim_sel & 0x8000)) {
        g_event_1c1697 = 1;
        launch(o, (o->reaction & 0xFF) - 2);
        edge_y_fix(o, 0x40);                             /* 0x10F9C */
    } else {
        rope_bump_events(o);                             /* 0x10FC6 */
        if (o->frame == 0 && o->count == 0) pos_add_facing(o, -0x10, 0, 0x10);
        o->clip_x_req = 0x40;                            /* +0x3E, floor pass input */
        if (o->push & BIT4) {                            /* landed */
            o->clip_x_req = 0; o->state = 4;
            o->reaction = (o->reaction & 0xFF00) | 5;    /* bounce */
            thud_sound(o);                               /* 0x1110E: 0x29/0x2D */
        } else if (o->reaction & 0x8000) return CELL_1B124;
    }
    return cell;
}

/* ROM 0x1B1D0 — reaction 5 bounce; ROM 0x1B318 — reactions 8/9 lying */
static const u8* rc_bounce(Obj*o, const u8*cell){
    if (!(o->anim_sel & 0x8000)) {
        o->f33 &= ~BIT4; nudge_clipped(o, 0x50);         /* 0x10B62 */
        launch(o, 3); o->mash = 0; seed_mash_quartile(o);/* 0x10C60(1): 1/3/7/0x15 */
        if ((o->reaction&0xFF)!=0x2A && o->band==2) o->reaction |= 0x8000;
    } else {
        down_tick(o);                                    /* 0x10D04 */
        if (o->push & BIT4) {
            thud_sound(o); o->state = 4;
            o->reaction = (o->reaction & 0xFF00) |
                (((o->reaction&0xFF)==0x2A) ? 8 : (o->reaction&0x8000 ? 9 : 8));
        } else if (o->reaction & 0x8000) return CELL_1B1C4;
    }
    return cell;
}

static void rc_lying(Obj*o){
    if (!(o->anim_sel & 0x8000)) {
        o->mode=0; o->f33 &= ~(BIT3|BIT4|BIT6); o->move_id &= ~0x80;
        o->combo=o->combo_t=o->slowwalk_t=0;
        if (!o->down_forced) o->down_forced = down_time_by_band(o); /* 0x10F56 */
        slide_body(o);                                   /* id9: (0x30,-0x18) clip;
                                                            id8: nudge 0x50 */
        return;
    }
    if (!(o->f32 & BIT4)) {                              /* not force-risen */
        partner_check(o);                                /* 0x115D2 */
        if (!(o->result & 0x8000)) {
            Obj*opp = o->opponent;                       /* +0x7A */
            if (opp->f34 & BIT4) return;                 /* being pinned: stay */
            down_tick(o);                                /* referee flag + mash */
            if (o->down_forced && --o->down_forced) return;
        }
    }
    o->state = 7; o->reaction &= ~0x8000;                /* 0x1B3C4 */
}

/* ROM 0x11E42 — state 7 rise (cell 0x11E36: 1 frame, 17 ticks, spr 0x68) */
static void st_getup(Obj*o){
    if (!(o->anim_sel & 0x8000)) {
        o->mode=0; o->f33 &= ~(BIT3|BIT4|BIT6); o->mash=0; o->down_forced=0;
    } else if ((o->frame & 0xFF) == 0xFE) {
        g_event_1c1697 = 1;
        if (o->result || (o->prev_sel & 0xFF) == 5) o->state = 0;
        else if (o->reaction & 0x2000) { o->reaction &= ~0x2000;
                                         o->state = 4; o->reaction = 1; } /* dizzy */
        else o->state = 0;
    }
}
```

Porting notes: reaction/state writes must go through the `+0x20` word (bit15 clear) so
the pass-4 latch restarts the cell; the A1-substitution (critical art) must stay
one-frame-scoped exactly like `0x1C03E`; hit detection and `0x24E58` belong on the
even-frame list only; and `+0x4C`/`+0x8E` auto-clear on sprite change (`0x24090`) is
load-bearing for both attack cadence and hitstun invulnerability.
