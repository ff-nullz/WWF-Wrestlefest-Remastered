# Cat 7 (face-down opponent) drops — rope-run family transcription

Source: `reference/maincpu.asm`. Read-only; nothing compiled. Field names per
`engine/engine.h`. "victim" = A2 = `partner` (+0x26). `react_lo` = react_id & 0xFF.
Sounds via `0x2052(D0)`; `$1C15D2/D3` = announce (wrestler byte +0x03, name id);
`$1C1800 = 0xE` = camera shake; `$1C0161` bit1 = cage/no-ropes mode flag.

## 0. Which handlers actually run to the ropes

Only THREE of the eight use the run-to-ropes machinery, and they dispatch on
`grap44` (+0x44), which state-5 entry zeroes (anim.md): phase 0 run / 1 turn /
2 run back / 3 drop. The cover 0x48 (`0x132F4`) does NOT share them: its 84/85
frames are an ordinary `0x26AE` homing dive (engine `handler_cover` is right).
Sprites 84/85 are simply the "crouch-and-launch" pose reused by every dive.

| handler | move | uses run phases? | drop phase PC |
|---|---|---|---|
| `0x175C4` | 0x35 leg drop (w0) | yes, `+0x44 & 3` | `0x1778E` |
| `0x13D46` | 0x0E (w1/w3) | yes, `+0x44 & 3` | `0x13D7E` |
| `0x15D08` | 0x23 (w8) | yes, `+0x44 & 0xF`: 0 lift, 1-4 hops, 5/6/7 run/turn/run-back, 8 drop | `0x15E42` |
| `0x14B64` | 0x14 (w4) | no — direct dive | `0x14B64` |
| `0x14458` | 0x46/0x47 (w5/w7/w10) | no — direct dive | `0x14458` |
| `0x13A2E` | 0x0B (w2) | no — knockback preset 0x23 | `0x13A2E` |
| `0x149F2` | 0x13 (w9) | no — direct dive | `0x149F2` |
| `0x135EC` | 0x09 (cat5 B1+B2, w2/w7/w9) | no — a hold, not a dive | `0x135EC` |

Dispatch prologue (`0x175C4` / `0x13D46`, identical):
```
btst #1,$1C0161 ; beq +      ; cage: no ropes to run to
  move.w #3,+0x44            ; -> straight to the drop phase (EVERY tick)
jsr tbl[(+0x44 & 3)]         ; 0x175FC / 0x176C4 / 0x1770C / <drop>
```
`0x15D08`: `jsr tbl[(+0x44 & 0xF)]`, table `0x15D20` = {0x15D44, 0x15DB8 x4,
0x175FC, 0x176C4, 0x1770C, 0x15E42}.

Each phase ends with `state = 5; +0x44 += 1` — re-entering state 5 clears the
anim block (count/frame/anim_sel latch) but `+0x44` survives, so the handler
re-inits into the next phase. The victim is held flat the whole time by
`victim.down_t = 0x200` written once in phase 0 (and cleared on hit/miss).

## 1. Shared helpers

### 1a. `0x175FC` — phase 0: run to the FAR ropes
```
175FC  if anim_sel b15 clear (init):
17604    mover = 1
1760A    speed = 0 ; speed_lo(+0x2B) = run_tab 0x11D4A[wrestler]      ; = handler_run's speed
1761E    victim.down_t = 0x200                                        ; hold flat
17624    victim.mash_aa = max(1, tbl0x176BA[min(victim.+0xDC,5)] - victim.hp)
           (+6 if own move_id lo == 0x23)                            ; escape seed
           tbl0x176BA words: 36 1E 16 10 0A (index 5 reads CODE 0x0828 — ROM quirk,
           TODO EXACT 0x17634: clamp is to 5 not 4)
1765A    facing = 0 (bclr b7 +0x2E) ; angle = 0xC0                    ; default: run LEFT
17666    if x < 0x270 (ring mid-line): facing = 0x8000 ; angle = 0x40 ; run RIGHT
1767A    -> 176AC
1767C  else (committed):
1767C    atk = 0
17680    rope = facing b15 ? (clip & 1 /*xmax*/) : (clip & 2 /*xmin*/) ; only the side you run INTO
         if rope:
1769C      0x10FC6 (rope shake; cage wall -> react 0x17, ignored here since cage skips this phase)
176A0      state = 5 ; +0x44 += 1 ; rts                                ; -> phase 1
176AC  cell = 0x11C62 (state-2 run cell, loop 9..E) ; 0x1112E footstep ; rts
```
No zone test: `0x17680` uses the x-clip bits only (rope_contact() in engine =
`zone==1 && clip&3`; use the facing-selected bit instead).

### 1b. `0x176C4` — phase 1: rebound turn
```
176C4  init: facing ^= 0x8000 ; angle ^= 0x80 (0x40<->0xC0) ; mover = 0 ; speed = 0x0C
176E4  else: if frame == 1: mover = 1                                 ; moves during 2nd turn frame
176F2        if frame == 0xFE: state = 5 ; +0x44 += 1                  ; -> phase 2
17704  cell = 0x11DD0 (state-6 turn cell: n2, dur 8/12, spr 0x69) ; rts
```
Note: `+0x44` is NOT decremented (unlike state-2's budget); here it is a phase
counter. `speed` stays 0x0C through the turn; phase 2 re-loads run speed.

### 1c. `0x1770C` — phase 2: run back to the victim
```
1770C  init: mover = 1 ; speed = run_tab[wrestler]
1772E        tgt(+0xBE) = victim.x + (victim.facing b15 ? -0x38 : +0x38)
                                 + (own.facing b15 ? -0x60 : +0x60)   ; stop 0x60 short of the drop spot
17758        -> 17780
1775A  else: facing b15 clear (running left) : reached = (x <= tgt)
             facing b15 set   (running right): reached = (x >  tgt)   ; 0x1776E bcs
17776        reached: state = 5 ; +0x44 += 1                            ; -> drop phase
17780  cell = 0x11C62 ; 0x1112E footstep ; rts
```
x-only target, no y homing, no rope test on the way back. The drop phase's
`0x26AE` homing closes the remaining 0x60 and the y gap.

### 1d. Other helpers used below
| PC | meaning |
|---|---|
| `0x26AE(k)` | `homing_launch(o,v,k)`, table `0x275C`: k2=(0x38,-8) k3=(0x58,-8) k4=(0x18,-8) k5=(0x40,-1). dx mirrored by VICTIM facing. |
| `0x258E(k)` | knockback preset (vx,vz,grav); k=0x23 used by 0x0B, k=3 by 0x0E-hit. TODO EXACT values of 0x25CA[0x23], [3]. |
| `0x11152` | hit-downed prox box → engine `eng_prox_box(o,v,0x0A)`. `0x11192` = hit-climber box. |
| `0x11412` | mutual link (partner->partner == self). |
| `0x1110E` | landing thud 0x29. `0x10F56` = re-seed `down_t` by band (E0/80/30) if it is 0. |
| `0x10D3A(k)` | spawn overlay fx k (0xF = leg-drop dust). `0x111C8` = KO check. `0x215B6` = score/pin bookkeeping. |
| MISS | `state=4 react 5 dmg 3 ; snd 0x32 ; announce 0x0F1A ; partner |= 0x80 ; spr 0x1F|facing` (+ per-move offset/flip) |
| FINISH | `frame==0xFE -> state 7 ; partner |= 0x80` (+ spr 0x68|facing on some) |

`0x1C1800=0xE` shake, `bset #7,+0x18` (off_x |= 0x8000) and `bset #4,+0x60`
(move_id bit4 "airborne") are cosmetic/collision hints; `bset #7,+0x60` is the
hit latch the engine already uses (`move_id |= 0x8000`).

## 2. `0x175C4` — move 0x35 LEG DROP (w0 Hogan). Rec 0x175A8 {8,14,10,FF00,10; 84 85 E6 E7 E8}

Phases 0-2 = §1a-c. Phase 3 `0x1778E`:
```
1778E  if move_id b15 set -> 178A4 (post-hit tail)
17798  init (anim_sel b15 clear):
177A0    mover = 0 ; vz = 0x600 ; grav = 0x48
177B0    k = (facing byte == victim.facing byte) ? 3 : 4 ; 0x26AE(k)     ; vx/vy set, mover=2 by 0x26AE
         (TODO EXACT 0x177B4: 0x26AE sets mover=2; then 0x177DA re-sets it — harmless)
177CE  frame==0 && count==0: mover = 2                                  ; flight starts after frame 0 (8 ticks)
177E4  frame==1 && count==0: off_y = 0x28
177F8  if vz >= 0 (rising): move_id |= 0x10 ; floor42 = 0x20             ; (+0x42 = +0x20, sign literal)
1780A  if !landed -> 178A4
17814  landed: floor42 = 0 ; off_x |= 0x8000
1781E    if !prox_box(0x11152) -> MISS 178CC ; if !mutual(0x11412) -> MISS
17832    HIT: mover = 0 ; count = 0 (releases FF00 hold) ; shake ; move_id |= 0x8000
17848         y = victim.y - 1 (16.16: -(1<<16)) ; victim.state = 4 ; victim.react_id = 0x1A
1785E         move_id &= ~0x10 ; victim.dmg = 0x12 ; victim.+0xDC += 1 ; victim.+0xC7 += 1
17874         snd 0x2B, 0x32 ; announce (self, 0x12)
17898         victim.down_t = 0 ; 0x10F56(victim) (re-seed E0/80/30 by band)
178A4  tail: if frame >= 4: 0x10D3A(0xF) (dust fx, each tick of frame 4)
178B6        frame == 0xFE: state = 7 ; partner |= 0x80
178CC  MISS: state = 4 ; react_id = 5 ; dmg = 3 ; snd 0x32 ; announce 0x0F1A ; partner |= 0x80
              victim.down_t = 0 ; victim.mash_aa = 0 ; cell = 0x125C0 ; spr = 0x1F | facing
```
Victim react 0x1A = the leg-drop-taken reaction (TODO EXACT: cell for react 0x1A
not read here; engine may map it to lying-hit 7 like the stomp).

### C sketch
```c
/* 0x175FC / 0x176C4 / 0x1770C — shared rope-run phases, dispatched on grap44. */
static uint32_t phase_run_to_ropes(eng_obj *o, eng_obj *v, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 1; o->speed = run_speed(o);
        if (v) { v->down_t = 0x200; v->mash_aa = escape_seed(o, v); } /* 0x17624 */
        if ((o->x >> 16) < 0x270) { o->facing = 0x8000; o->angle = 0x40; }
        else                      { o->facing = 0;      o->angle = 0xC0; }
        return 0x11C62u;
    }
    o->atk = 0;
    if ((o->facing & 0x8000u) ? (o->clip & 1u) : (o->clip & 2u)) {
        eng_ropes_arm((o->clip & 1u) ? 1 : 0, 1, 1);          /* 0x10FC6 */
        o->state = 5; o->grap44++;
        return cell;
    }
    footstep(o);                                              /* 0x1112E */
    return 0x11C62u;
}
static uint32_t phase_rebound_turn(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->facing ^= 0x8000u; o->angle ^= 0x80u; o->mover = 0; o->speed = 0x0C;
    } else if (o->frame == 1) o->mover = 1;
    else if ((o->frame & 0xFFu) == 0xFEu) { o->state = 5; o->grap44++; return cell; }
    return 0x11DD0u;
}
static uint32_t phase_run_back(eng_obj *o, eng_obj *v, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 1; o->speed = run_speed(o);
        o->run_tgt = (v->x >> 16) + ((v->facing & 0x8000u) ? -0x38 : 0x38)
                                  + ((o->facing & 0x8000u) ? -0x60 : 0x60);   /* +0xBE */
        return 0x11C62u;
    }
    int xi = o->x >> 16;
    if ((o->facing & 0x8000u) ? (xi > o->run_tgt) : (xi <= o->run_tgt)) {
        o->state = 5; o->grap44++; return cell;
    }
    footstep(o);
    return 0x11C62u;
}
/* 0x175C4 — move 0x35 leg drop. */
static uint32_t handler_legdrop(eng_obj *o, uint32_t cell)
{
    eng_obj *v = partner_of(o);
    if (!v) { o->state = 0; return cell; }
    if (cage_mode()) o->grap44 = 3;                           /* $1C0161 b1 */
    switch (o->grap44 & 3u) {
    case 0: return phase_run_to_ropes(o, v, cell);
    case 1: return phase_rebound_turn(o, cell);
    case 2: return phase_run_back(o, v, cell);
    }
    /* phase 3: 0x1778E */
    if (o->move_id & 0x8000u) goto tail;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->vz = 0x600; o->grav = 0x48;
        homing_launch(o, v, ((o->facing ^ v->facing) & 0x8000u) ? 4 : 3);
        return cell;
    }
    if (o->frame == 0 && o->count == 0) o->mover = 2;
    if (o->frame == 1 && o->count == 0) o->off_y = 0x28;
    if (o->vz >= 0) { o->move_id |= 0x10u; o->floor42 = 0x20; }
    if (!o->landed) goto tail;
    o->floor42 = 0; o->off_x |= 0x8000u;
    if (eng_prox_box(o, v, 0x0A) && v->partner == self_idx(o)) {
        o->mover = 0; o->count = 0; o->move_id |= 0x8000u; o->move_id &= ~0x10u;
        o->y = v->y - (1 << 16);
        v->state = 4; v->react_id = 0x1A; v->dmg = 0x12; v->downs_dc++; v->c7++;
        eng_sound(0x2B); eng_sound(0x32);
        v->down_t = 0; seed_down_t(v);                        /* 0x10F56 */
    } else {                                                  /* MISS 0x178CC */
        o->state = 4; o->react_id = 5; o->dmg = 3; eng_sound(0x32);
        o->partner = -1; v->down_t = 0; v->mash_aa = 0;
        return 0x125C0u;
    }
tail:
    if ((o->frame & 0xFFu) >= 4) spawn_fx(o, 0xF);
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = 7; o->partner = -1; }
    return cell;
}
```

## 3. `0x13D46` — move 0x0E (w1/w3). Rec 0x13D26 {8,10,10,FF00,FF00,FF00; 84 85 EA EB EB EC}

Phases 0-2 shared. Phase 3 `0x13D7E` — a dive that lands in a PIN-like hold:
```
13D7E  init: mover = 0 ; vz = 0x600 ; grav = 0x48 ; 0x26AE(2)
13DA0        facing = (vx < 0) ? 0 : 0x8000                            ; from vx sign
13DB4  frame==1 && count==0: off_y = 0x20
13DC8  if move_id b15 set -> 13EE8
13DD2  frame==0 && count==0: mover = 2
13DE8  if frame != 3 -> 13EE8
13DF2  frame 3: move_id |= 0x10 ; if !landed -> 13EE8
13E02    landed: off_x |= 0x8000 ; mover = 0
13E0C    !prox(0x11152) -> 13F1A ; !mutual -> 13F1A
13E20    HIT: count = 0 ; f33 |= 0x40 both (engaged) ; shake
13E38         victim.state = 5 ; victim.move_id = 0x51 ; victim.dmg = 0x11    ; victim SCRIPTED move 0x51
13E4A         move_id &= ~0x10 ; victim.+0xDA += 1 ; victim.+0xC7 += 5
13E5A         snd 0x32, 0x2B ; announce (self, 0x20)
13E7E         0x258E(3) knockback preset ; x = victim.x ; y = victim.y - 1
13E98         x += victim.facing b15 ? -0x30 : +0x30
13EAA         singles ($1C007C) && !(f33 b2) && f33 b0 (legal) && victim f33 b0:
13ED2             f35 |= 1 ; move_id |= 0x8000 ; 0x215B6 (pin start)
13EE8  else-path: if frame == 0xFE: rts (never self-ends once hit)
13EF2         if landed: mover = 0 ; count = 0 ; shake ; if move_id b15 clear: state = 7
13F1A  MISS: victim.state==5 && victim.move_id==0x54 ? (state=4 react 0x1E)
              : state=4 react 5 spr 0x1F|facing ; facing ^= 0x8000 ; dmg 3 ; snd 0x32
                partner |= 0x80 ; victim.down_t = 0 ; victim.mash_aa = 0
```
TODO EXACT: victim move 0x51 (hold/pin victim script) and `0x215B6` are not
transcribed; the 0x13EE8 tail means the attacker stays in state 5 until the
victim's 0x51 script or the pin machinery releases it. For the engine, a
SIMPLIFIED end (like handler_cover) = treat as hit-downed + pin state 0x0C.

## 4. `0x15D08` — move 0x23 (w8). Recs 0x15CE0 {FF00,10; 1D6 1D7} and 0x15CF0 {4,18,C,FF00; 84 85 F3 F4}

`0x15D08` is the handler of BOTH records; `+0x44 & 0xF` picks the phase.
Phase 0 `0x15D44` (lift, cell swapped to 0x1A24A {n3, dur 0x28,0x18,4; spr 0x59 0x5A ...}):
```
15D44  init: mover = 0 ; victim.mash_aa = 0x1000 ; victim.down_t = 0x1000 (frozen, un-mashable)
15D5C        snd 0x31 ; facing = (victim.x + (victim.facing b15 ? -0x38 : 0x38) >= x) ? 0x8000 : 0
15D8C  frame==0xFE: state = 5 ; +0x44 += 1 ; if cage ($1C0161 b1): +0x44 = 8  ; skip hops+run
15DB0  cell = 0x1A24A
```
Phases 1-4 `0x15DB8` (hops carrying the victim toward the ropes):
```
15DB8  init: k = (facing byte != victim.facing byte) ? 5 + hold_ph(+0x45) : 0xA - hold_ph
             (+0x45 = low byte of +0x44 = hop number 1..4)             ; 0x275C[k]: TODO EXACT k 6..9
15DE2        vz = 0x500 ; grav = 0x48 ; 0x26AE(k) ; mover = 2
15DFE  landed: thud ; mover = 0 ; count = 0 ; shake
15E1C  frame==0xFE: state = 5 ; zone(+0x36)==1 ? +0x44 = 5 : +0x44 += 1   ; rope contact -> run phases
```
(TODO EXACT 0x15DC2: the victim is NOT repositioned here — check the 0x1A24A
cell / victim move for the carry; this phase is outside the cat-7 question.)
Phases 5/6/7 = §1a-c (phase 5 also seeds mash via 0x17624 with the +6 for 0x23).
Phase 8 `0x15E42` (the drop, uses rec 0x15CF0 frames 84 85 F3 F4):
```
15E42  if move_id b15 set -> 15FEC
15E4C  init: mover = 0 ; vz = 0x600 ; grav = 0x48 ; 0x26AE(2)
15E72  frame==0 && count==0: mover = 2
15E88  vz >= 0: move_id |= 0x10
15E94  frame==2 && count==0: off_y = 0x20 ; floor42 = 0x20
15EAE  !landed -> 15FD4
15EB8  landed: off_x |= 0x8000 ; floor42 = 0 ; thud
15EC8    !mutual -> MISS 15F8E ; !prox(0x11152) -> MISS
15EDC    HIT: mover = 0 ; announce (self, 0x25) ; shake ; move_id &= ~0x10
15EFE         victim.mash_aa = 0 ; victim.down_t = 0 ; victim.+0xDE += 1
15F0A         legal pin (f33 b0 both, !f33 b2): atk = 0x801D ; move_id |= 0x8000 ; f35 |= 1
15F34             victim.state = 5 ; victim.move_id = 0x64 ; victim.dmg = 0x14
15F46             carry_at(victim, self, 0x30, -1, 0) [A0/A2 swapped: VICTIM placed rel. to self] ; 0x215B6
15F64         else: count = 0x14 ; victim.state = 4 ; victim.react_id = 7 ; victim.down_t = 0
15F7C               victim.dmg = 0x14 ; 0x10F56(victim)
15F8E  MISS: state 4 react 5 ; facing ^= 0x8000 ; partner |= 0x80 ; VICTIM.dmg = 3 (A2! literal)
              snd 0x32 ; victim.down_t = 0 ; victim.mash_aa = 0 ; spr 0x1F|facing
              (0x15FA0 loads D0=-0x20 for add_pos_delta but never calls it — dead)
15FD4  frame==0xFE: state = 7 ; partner |= 0x80
15FEC  post-hit: victim f32 b4 (forced rise) ? announce(victim,0x29) : (result b7 clear ? rts)
1600E         state = 7 ; victim.state = 4 ; victim.react_id = 9 ; f35 &= ~1 ; add_pos_delta(-0x30,0,0) ...
```

## 5. `0x14B64` — move 0x14 (w4). Rec 0x14B48 {8,10,8,FF00,8; 84 85 191 192 192}. NO rope run.
```
14B64  if move_id b15 set -> 14CCA
14B6E  init: mover = 0
14B7C        tx = victim.x + (victim.facing b15 ? -0x38 : 0x38)
14B8E        facing = (tx >= x) ? 0x8000 : 0
14BA0        vz = 0x500 ; grav = 0x48 ; x = tx ; add_pos_delta(0x48, -1, 0)
14BC0        0x26AE(facing == victim.facing ? 3 : 4)
14BDE  frame==0 && count==0: mover = 2 ; f33 |= 0x08
14BF6  frame==1 && count==0: off_y = 0x20
14C0A  frame != 3 -> 14CCA
14C14  frame 3: floor42 = 0x20 ; !landed -> rts
14C24    landed: off_x |= 0x8000 ; floor42 = 0 ; thud ; mover = 0 ; count = 0
14C3C    !prox(0x11152) -> MISS 14C80
14C46    HIT: move_id |= 0x8000 ; victim.state = 4 ; victim.react_id = 7 ; victim.+0xD8 += 1 ; victim.+0xC7 += 1
14C62         snd 0x32, 0x2A ; victim.dmg = 0x0B
14C80  MISS: state 4 react 5 spr 0x1F|facing ; partner |= 0x80 ; facing ^= 0x8000 ; add_pos_delta(-0x20,0,0)
              VICTIM.dmg = 3 (A2 — literal, TODO EXACT 0x14CB2) ; snd 0x32 ; announce 0x0F1A
14CCA  frame==0xFE: state = 7 ; partner |= 0x80
```
Victim react 7 = lying-hit (face-down variant) — same id the stomp produces from 9.
Note: the victim's lying hold is never re-armed here (no down_t write) — the
lying handler's own 0x10F56 seed applies on the react-7 → lying transition.

## 6. `0x14458` — moves 0x46 / 0x47 (w5, w7/w10). Recs 0x14424 / 0x14440. NO rope run.
```
14458  if move_id b15 set -> 145CC
14462  init: mover = 0 ; vz = 0x600 ; grav = 0x48
1447A        k = 2 ; if victim.state==5 && victim.move_id==0x61 (climbing): k = 5 ; 0x26AE(k)
14498        facing = (vx < 0) ? 0 : 0x8000
144AC  frame==0 && count==0: mover = 2
144C2  frame==1 && count==0: off_x = -0x10 ; off_y = 0x28 ; if move 0x47: off_x = -8 ; off_y = -0x10
144F0  !landed -> 145CC
144FA  landed: off_x |= 0x8000 ; thud ; mover = 0
1450A    prox(0x11152): !mutual -> MISS 14606
1451E      HIT DOWNED: count = 0 ; move_id |= 0x8000 ; y = victim.y - 1
14532        victim.state = 4 ; victim.react_id = react_lo - 2 (8->6, 9->7) ; victim.+0xC7 += 1
14548        victim.dmg = 0x0A ; snd 0x2B, 0x32 ; (0x46 ? victim.+0xE0 : victim.+0xE2) += 1
14578    else prox(0x11192) climber: count = 0 ; move_id |= 0x8000 ; y = victim.y - 1 ; off_y = 0x0C
1459C        victim.state = 5 ; victim.move_id = 0x77 ; victim.dmg = 0x0A ; snd ; victim.+0xC7 += 1
14606  MISS: state 4 react 5 dmg 3 snd 0x32 announce 0x0F1A partner|=0x80 facing^=0x8000 spr 0x1F|facing
              if move 0x46: add_pos_delta(-0x30, 0, 0)
145CC  frame==0xFE: off_x |= 0x8000 ; state = 7 ; spr = 0x68|facing ; partner |= 0x80 ; add_pos_delta(0x18,-1,0)
```
This is structurally handler_leap (0x10) with dmg 0x0A and per-move offsets.

## 7. `0x13A2E` — move 0x0B (w2). Rec 0x13A1E {FF00,10; F1 F2}. NO rope run, NO prox test.
```
13A2E  init: if victim.react_lo != 9:  x = victim.x ; y = victim.y - 1 ; add_pos_delta(0x78, -1, 0)
13A62        else (face-down): facing = 0 ; tx = victim.x + (victim.facing b15 ? -0x38 : 0x38)
13A7A             if tx >= x: facing = 0x8000 ; x = tx ; add_pos_delta(0x38, -8, 0)
13A9A        0x258E(0x23) knockback preset (TODO EXACT 0x25CA[0x23] vx/vz/grav)
13AA8  !landed: frame==0xFE -> state 7 ; partner |= 0x80 ; rts
13AB2  landed: count = 0 (release FF00) ; mover = 0
13ABA    victim.state==4 && react_lo in {8,9}: victim.state = 4 ; react_id = react_lo - 2
13AEA        victim.+0xD4 += 1 ; victim.dmg = 0x0B ; snd 0x29, 0x32            ; unconditional hit
13B0C    else prox(0x11192) climber: count = 0 ; victim.state = 5 ; victim.move_id = 0x77 ; victim.dmg = 0x0B
13B2C        snd 0x32, 0x29 ; victim.+0xC7 += 1 ; (bra 0x14422 = rts)
13B4A    else MISS: state 4 react 5 spr 0x1F|facing dmg 3 snd 0x32 announce 0x0F1A partner |= 0x80
```
Note the hit path does NOT set move_id b15 and does not re-arm anything; the
animation just continues to frame 1 (F2, 0x10 ticks) and ends via 0x13B82.
TODO EXACT 0x13AD2: `+0xC7` is not bumped on the downed hit (only on the
climber hit) — literal.

## 8. `0x149F2` — move 0x13 (w9). Rec 0x149E2 {20,FF00; ED EE}. NO rope run.
```
149F2  init: mover = 2 ; facing = 0 ; tx = victim.x + (victim.facing b15 ? -0x38 : 0x38)
14A1A        if tx >= x: facing = 0x8000 ; x = tx ; y = victim.y ; z = victim.z
14A36        add_pos_delta(0x10, 0, 0x20) ; vz = 0x300 ; grav = 0x30 ; 0x26AE(2)
14A60  move_id b15 clear && frame == 1 && landed:
14A7A        thud ; !prox(0x11152) -> MISS 14AFC
14A8A        HIT: victim.state = 4 ; victim.react_id = 7 ; victim.dmg = 0x0A ; move_id |= 0x8000
14AA2             mover = 0 ; count = 0x18 (hold released into a 0x18-tick tail)
14AAC             victim.+0xD6 += 1 ; victim.+0xC7 += 1 ; snd 0x2A, 0x32
14ACC  frame==0xFE: state = 7 ; spr = 0x68|facing ; add_pos_delta(0x40, 0, 0) ; partner |= 0x80
14AFC  MISS: state 4 react 5 ; facing ^= 0x8000 ; spr 0x1F|facing ; add_pos_delta(-0x40, 0, 0)
              dmg = 3 ; snd 0x32 ; announce 0x0F1A ; partner |= 0x80
```
Victim react 7 regardless of 8/9 (cat 7 only arrives with 9 anyway).

## 9. `0x135EC` — move 0x09 (cat 5 both buttons, w2/w7/w9). Rec 0x135C8 {7 frames; 18B D8..DD}
Not a dive: a ground hold on a FACE-UP (react 8) victim, mash loop frames 5-6.
```
135EC  init: mover = 0 ; x = victim.x ; y = victim.y ; facing byte = victim.facing byte
1360E  frame==0 && count==0:
1361A    victim.state==4 && react_lo==8 && mutual:
13632      f33 |= 0x40 both ; f35 |= 2 (self) ; victim.f35 |= 4 ; victim.+0xC7 += 1
13650      announce (self, 9) ; victim.state = 0xFF (hidden, held)
1366A    else: state = 0 ; spr = facing ; partner |= 0x80 ; count = 1            ; abort
13686  frame==0xFE:
13690    if result b7 set -> 136E2 release
13698    frame = 4 ; count = 0 (loop 5-6) ; victim.dmg = 3 ; 0x111C8 KO check
136AE      no KO (cc): 0x1370E: if move_id b15 was clear: move_id |= 0x8000
13716            victim.state = 5 ; victim.move_id = 0x5D ; 0x215B6 ; snd 0x32
136B0      KO: if $1C0161 b0: announce (victim, 0x29) ; victim.f32 |= 0x10 ; partner |= 0x80 ; spr 0x68|facing
136E2      release: state = 7 ; victim.state = 4 ; victim.react_id &= 0xFF
136F4               f33 &= ~0x40 both ; f35 &= ~2 ; victim.f35 &= ~4
```
Same shape as handler_mount (0x22); the difference is react 8 gate, +3/loop,
and the victim scripted move 0x5D on the first loop. Already covered by
move-handlers-1 §2.2; include here only because the caller listed it.

## 10. Summary for the engine

1. Add `phase_run_to_ropes / phase_rebound_turn / phase_run_back` (§1) keyed on
   `grap44`; they return the run (0x11C62) and turn (0x11DD0) cells, so the
   existing state-2/6 art plays. Needs one new field: run target `+0xBE`
   (int16 x), plus cage flag check (`$1C0161` b1).
2. Hook 0x35 and 0x0E through the 4-way dispatch; 0x23 through the 9-way.
3. 0x14 / 0x46 / 0x47 / 0x13 are handler_leap clones with different presets
   (table in §5/6/8) — no run phase, so they already nearly work.
4. 0x0B has no prox test: it hits if the victim is still lying.
5. Victim hold-down during the run: `victim.down_t = 0x200` once at phase 0
   init; cleared on hit (then 0x10F56 re-seed) and on miss. Victims of the
   0x14/0x46/0x47/0x13 direct dives get no down_t write at all.
6. The victim's lying-state escape seed `mash_aa` is written in phase 0 and
   zeroed on miss; for 0x35 it is left alone on hit (the react 0x1A cell
   presumably re-seeds) — TODO EXACT.
