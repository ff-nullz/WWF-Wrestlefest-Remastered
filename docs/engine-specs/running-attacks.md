# Running attacks — moves 0x05, 0x04, 0x2A, 0x20, 0x40, 0x41 (+0x2D cross-check)

Source: `reference/maincpu.asm`, read-only. Move table `0x12614[id]` → cell → handler PC.
Field names per `engine/engine.h`; `atk` = +0x4C, `f33/f34` = +0x33/+0x34 byte flags,
`off_x/off_y` low bytes = +0x19/+0x1B, `last_hit` = +0x92 (set by hit bookkeeping
`0x24DEC/0x24E02`: attacker.+0x92 = victim, victim.+0x92 = attacker).

## Shared helpers (PCs referenced below)

| PC | meaning |
|---|---|
| `0x258E` (D0=i) | `knockback(o,i)` — table `0x25CA[i]` {vx, vz, grav}, vy=0, mover=2, vx negated if facing bit15. Table vx **positive = backward**. Entries used: 0xA {-640, 0x500, 0x60}, 0xB {-704, 0x600, 0x68}, 0x13 {+768, 0, 0x58}, 0x24 (victim alt). |
| `0x10FC6` | rope-contact probe: C=1 if `$1C0161` bit1 clear, `zone` ∈ {1,5}, `clip` bit1 (→ shake `$1C1150/1180`) or bit0 (→ `$1C11B0/11E0`). If `zone == 3` (cage wall): self → `state=4, react_id=0x17`, sounds 0x28/0x32, C=0. Engine form: `o->zone==1 && (o->clip&3)`. |
| `0x10B9A` (D0,D1,D2) | `carry_at(o,v,dx,dy,dz)` — v.pos = o.pos + facing-mirrored (dx,dy,dz). |
| `0x10BD0` (D0,D1,D2) | `add_pos_delta(o,dx,dy,dz)` facing-mirrored. |
| `0x11412` | mutual link: C=0 iff `partner->partner == self`. |
| `0x1108C` | singles-only (`$1C007C != 0`, self f33 bit1 clear): RNG `0x21B4` & 0xFF < `0x110C8[wrestler]` → `bset #4,f34` (CPU "wounded" roll). |
| `0x110E0` | singles only: victim's teammate (+0x86) in state 1 sub 1 → `bset #5,(+0x34)` on the teammate. Cosmetic, TODO EXACT if tag matters. |
| `0x1110E` | landing thud: sound 0x29 (0x2D if f33 bit2). |
| `0x1112E` | footstep sound 0x33 when `count==0 && (frame==1 \|\| frame==4)`. |
| `0x24D7A` | hit-from-behind remap: if react_id < 0x24 and attacker.facing bit15 == victim.facing bit15 → `react_id = tbl_0x24DB8[react_id]` (0→0B, 1→0C, 2→0B, 3→0C, 4→0D, 5..F identity, 10/11→0B, ≥12→0B). |
| `0x24DDC` | victim running (f33 bit4) → `dmg += 1`. |
| `$1C15D2/D3` | move-name announce (wrestler byte +0x03, name id). |

Attack records consumed (from `data/romdata/hit_record.json`, result/reaction = hit-pipeline tables):

| id | flags | vbox1 | abox | dmg | result → effect on victim (A1) | reaction |
|---|---|---|---|---|---|---|
| 0x03 | C0 | 0 | 9 | 0 | **2** `0x24468`: veto if victim +0x4C high byte ≠ 0. Sound 0x2B. Victim `state=4, react 2, +0x9A=0x50`; attacker `state=4, react 2 (+0x9A=0x50)` if victim was state 2 (running), else attacker `react 0x18`. `0x24D7A`. `bset #7,+0x26` (attacker partner poisoned). | 0 |
| 0x06 | C0 | 6 | 2 | 10 | **4** `0x2452A`: sounds 2B/32, announce 5, `victim.dmg_lo(+0x69)=10`, **attacker atk=0**, victim `state=4, react 0x10`; if attacker angle(+0x2D) bit7 == victim facing bit7: victim faces attacker (`facing = attacker.facing ^ 0x8000`), `0x24DDC`, wrestler 0/5 → `victim.dmg += 3`; else victim `react 0x0B`. | 0 |
| 0x0B | C0 | 0 | 10 | 7 | **5** `0x245F0`: sounds 2B/32, dmg, victim `state=4, react 2` (3 if victim running), `0x24D7A`, `0x24DDC`. | 0 |
| 0x10 | C0 | 1 | 2 | 12 | **7** `0x2468A`: sounds, announce 0xD, dmg, victim `state=4, react 0x1D` **iff attacker move low byte == 0x41**, else `react 0x14`; `0x24D7A`, `0x24DDC`. | 12 |
| 0x13 | C0 | 0 | 2 | 9 | **12** `0x245BC`: announce 0x1B then result-5 body (react 2/3). | 0 |
| 0x16 | 80 | – | 0 | 12 | **5** (as 0x0B, dmg 12). Attacker-only flags: cannot be hit through it. | 0 |
| 0x17 | 80 | – | 1 | 14 | **11** `0x247EC`: veto if victim +0x4C bit15. Sounds, announce 0x16, **attacker atk=0**, dmg, victim `state=4, react 0x12`; `0x24D7A/0x24DDC` unless victim atk id == 0x1A. | 0 |

Victim cells: react 0x10 and 0x12 share `0x1B700` = {handler `0x1B706`, mode 0}: `count=0xFF00`;
first entry `+0x44=0x20`; `--+0x44 == 0 → state 0`. Sprite untouched (freeze in last pose) —
a 32-tick "caught" placeholder the attacker overwrites. React 0x19 = `0x1BB40` {mode 1, n 1,
dur FF00, spr 0xB3}, handler `0x1BB4C`: init `knockback(0x13)` (or 0x24 if f34 bit3, cleared);
then `0x10FC6`; landed → thud, `state=4 react 5` (bounce), spr 0x14 (0x1F if +0x71==2) |
facing, `add_pos_delta(-0x40,0,0)`. React 0x1D = `0x1B78A` {mode 1, n 1, dur 0xC, spr 0x195},
handler `0x1B7A2`: init `facing ^= 0x8000; partner = last_hit; pos = partner pos (y−1); mover=0`;
at 0xFE: prev_sel==2 → `state 4 react 0x1B, facing ^=, add_pos_delta(8,-1,0x40)`, else
`state 4 react 2, facing ^=, add_pos_delta(8,-1,0x10), spr = 0x11|facing`. Cell swap to
`0x1B796` unless react 0x14.

---

## Move 0x05 — running clothesline/forearm (w0..w11 cat2 mostly, w1/w4 cat1)

Cell `0x12F08` = {handler `0x12F40`, mode 1, n 6, dur 6,6,6,6,4,0xC, spr 96,97,98,99,9A,9B}
Hit-latched alt cell `0x12F28` = {same handler, mode 1, n 4, dur 4,4,6,6, spr 9F,A0,A1,9B}

| PC | when | write |
|---|---|---|
| `12F48` | init (anim_sel b15 clear) | `mover = 1` (keeps polar run; speed/angle untouched from state 2) |
| `12F4E` | init | `last_hit = 0` |
| `12F56` | every committed frame | `0x1112E` footstep |
| `12F64` | move_id b15 clear | `atk = 3` (run-collision record) |
| `12F72` | … and `frame < 4` | `atk = 6` (the strike; frames 0-3 = 28 ticks) |
| `12F78-12F90` | … and `last_hit && victim.react_id lo == 0x10 && victim.last_hit == self` | HIT: |
| `12F92` | | `partner = victim` |
| `12F98` | | `anim_sel = 5` (b15 clear ⇒ tick re-inits frame 0 on the **alt cell**) |
| `12F9E` | | `move_id \|= 0x8000` |
| `12FA4` | | `0x1108C` |
| `12FAA` | | `atk = 0` |
| `12FB0` | no hit | `0x10FC6` C → `state = 6` (rope turn) |
| `12FBE-12FD2` | move_id b15 set, `frame==2 && count==0` | victim `state=4`, `react_id lo = 0x19`; `carry_at(0x10,-1,0x38)`; `0x110E0` |
| `12FF8` | `frame == 0xFE` (both cells) | `state = 3` (skid: decelerates +0x2A by 5/frame → state 0); `partner \|= b31` (poison) |
| `13018` | return | cell = `0x12F28` if move_id b15 else own |

```c
/* 0x12F40 — move 0x05 running strike: record 6 for the first 4 frames,
 * run-collision 3 after. Keeps running; a caught victim (react 0x10 from
 * result 4) is carried and dumped at frame 2 of the alt cell. */
static uint32_t handler_runstrike(eng_obj *o, uint32_t cell)
{
    eng_obj *v = last_hit_obj(o);                      /* +0x92 */
    if (!(o->anim_sel & 0x8000u)) { o->mover = 1; o->last_hit = -1; return cell; }
    footstep_1112E(o);
    if (!(o->move_id & 0x8000u)) {
        o->atk = (o->frame < 4) ? 6 : 3;
        if (v && (v->react_id & 0xFF) == 0x10 && v->last_hit == self(o)) {
            o->partner = o->last_hit; o->anim_sel = 5; o->move_id |= 0x8000u;
            wounded_roll_1108C(o); o->atk = 0; return ALT_0x12F28;
        }
        if (rope_contact_10FC6(o)) { o->state = 6; return cell; }
    } else if (o->frame == 2 && o->count == 0 && v) {
        v->state = 4; v->react_id = (v->react_id & 0xFF00) | 0x19;
        carry_at(o, v, 0x10, -1, 0x38);
    }
    if ((o->frame & 0xFF) == 0xFE) { o->state = 3; o->partner = -1; }
    return (o->move_id & 0x8000u) ? ALT_0x12F28 : cell;
}
```
Note the 0xFE exit in the unlatched branch is reached only when neither hit nor rope fired
(`12FBE` falls to `12FF8` when b15 clear).

---

## Move 0x04 — running dropkick (w1/w2/w4 cat2)

Cell `0x12BDA` = {handler `0x12C22`, mode 1, n 4, dur 8,0x10,FF00,0x20, spr A4,A5,A6,17A}
Alt cells (hit-latched): `0x12BF2` {dur 2,4,FF00,0x20, spr A2,A3,A6,17A};
`0x12C0A` (f34 bit3) {dur 4,8,FF00,0x20, spr 1CB,1CC,A6,17A}.

| PC | when | write |
|---|---|---|
| `12C2A` | init | `last_hit = 0` |
| `12C2E` | init | `z += 0x30` (word +0x0E; floor is 0x140, up = +) |
| `12C34/3A` | init | `f33 \|= 0x08` (airborne), `f33 &= ~0x10` (not running) |
| `12C40` | init, `f32` bit1 was clear | `knockback(0xA)` → vx fwd 2.5px/f, vz 0x500, grav 0x60, mover=2 |
| `12C56-12C98` | init, `f32` bit1 was set (cleared) | TODO EXACT alt launch: `$1C1670=0`, `mover=2`, `f34 \|= 8`, `f32 &= ~1`, `vx=0x400 vz=0x200 grav=0x38 vy=0x100`; if f33 bit7: `vy=-vy, vx=-vx` (a rope/turnbuckle variant, not the matrix path) |
| `12CB0` | move b15 clear, `frame < 2` | `atk = 0` |
| `12CB4-12CCC` | … and `z > 0x160` (0x120 if f33 bit2) unsigned | `atk = 0x17` |
| `12CD2-12CEA` | … and `last_hit && victim.react lo == 0x12 && victim.last_hit == self` | HIT: |
| `12CEC` | | `mover = 0` |
| `12CF0` | | `move_id \|= 0x8000` |
| `12CFA` | | `0x110E0`; `z = victim.z` (`12CFE`) |
| `12D04` | | `anim_sel = 5` (restart on alt cell) |
| `12D12-18` | f34 bit3 clear | `0x1108C`; `partner = last_hit` |
| `12D22-46` | f34 bit3 set (alt) | `x,y = victim.x,y`; `off_x lo = 0x10`, `off_y lo = 0x40`; `partner = victim.partner` |
| `12D4A-62` | move b15 set, `frame==1 && count==0` | |
| `12D66` | | `f34 &= ~8`; if it was set → `12DAE` alt double-team block (TODO EXACT: victim.partner A3 → `state 5 move 0x8006`, facing flip, both `f33 &= ~0x40`, carries) |
| `12D6E-12DA4` | normal | `z += 0x40`; `off_x \|= 0x8000`; victim `state=4, react_id=0x19, f34 \|= 8, spr = 0xB3 \| facing`; `carry_at(0x10,-1,0x10)` |
| `12E3A-42` | `frame == 2` | `mover = 2` |
| `12E48-74` | `frame == 2 && landed` | `0x1110E`; `count = 0` (break hold); `mover = 0`; `f33 &= ~0x18`; `facing ^= 0x8000`; `off_x lo = 0xD0` |
| `12E7A-82` | `frame < 3` | `lookahead(+0x3E) = -0x10` |
| `12E88` | … | `0x10FC6` C → `state=4, react_id=0x17, dmg=3`, sound 0x32, `lookahead=0` (rope crash) |
| `12EB0-E2` | `frame == 0xFE` | `off_x \|= 0x8000`; `state = 7`; `spr = 0x68 \| facing`; `lookahead=0`; `add_pos_delta(-0x30,0,0)`; `partner` poisoned |
| `12EEA-F00` | return | cell = `0x12BF2` if move b15 (or `0x12C0A` if f34 bit3) |

```c
/* 0x12C22 — move 0x04 running dropkick. Launcher 0xA, record 0x17 only
 * while z > 0x160 on frames 0-1. Hit (victim react 0x12) -> hang in the
 * air with the victim carried, drop at frame 1 of the alt cell. Landing
 * at frame 2 flips facing; end -> state 7 (get up). */
static uint32_t handler_dropkick(eng_obj *o, uint32_t cell)
{
    eng_obj *v = last_hit_obj(o);
    if (!(o->anim_sel & 0x8000u)) {
        o->last_hit = -1; o->z += 0x30 << 16;
        o->f33 = (o->f33 | 0x08) & ~0x10;
        knockback(o, 0xA);                             /* f32 bit1 alt: TODO EXACT 0x12C56 */
        return cell;
    }
    if (!(o->move_id & 0x8000u) && o->frame < 2) {
        uint16_t lim = (o->f33 & 0x04) ? 0x120 : 0x160;
        o->atk = 0;
        if ((uint16_t)(o->z >> 16) > lim) {
            o->atk = 0x17;
            if (v && (v->react_id & 0xFF) == 0x12 && v->last_hit == self(o)) {
                o->mover = 0; o->move_id |= 0x8000u; o->z = v->z;
                o->anim_sel = 5; wounded_roll_1108C(o); o->partner = o->last_hit;
                return ALT_0x12BF2;
            }
        }
    } else if ((o->move_id & 0x8000u) && o->frame == 1 && o->count == 0 && v) {
        o->z += 0x40 << 16; o->off_x |= 0x8000u;
        v->state = 4; v->react_id = 0x19; v->f34 |= 0x08;
        v->spr = 0x00B3 | (v->facing & 0x8000u);
        carry_at(o, v, 0x10, -1, 0x10);
    }
    if (o->frame == 2) {
        o->mover = 2;
        if (o->landed) {
            eng_sound(0x29); o->count = 0; o->mover = 0; o->f33 &= ~0x18;
            o->facing ^= 0x8000u; o->off_x = (o->off_x & 0xFF00) | 0xD0;
        }
    }
    if (o->frame < 3) {
        o->lookahead = -0x10;
        if (rope_contact_10FC6(o)) {                   /* 0x12E88 */
            o->state = 4; o->react_id = 0x17; o->dmg = 3; eng_sound(0x32);
            o->lookahead = 0; return cell;
        }
    } else if ((o->frame & 0xFF) == 0xFE) {
        o->off_x |= 0x8000u; o->state = 7; o->spr = 0x0068 | (o->facing & 0x8000u);
        o->lookahead = 0; add_pos_delta(o, -0x30, 0, 0); o->partner = -1;
    }
    return (o->move_id & 0x8000u) ? ALT_0x12BF2 : cell;
}
```
Note: rope branch at `12E88` is only evaluated on frames 0-2 (`bcc 12EB0` at `12E80`).

---

## Move 0x2A — running shoulder/elbow (w2, w9 cat1)

Cell `0x16614` = {handler `0x1662C`, mode 1, n 4, dur 4,6,4,FF00, spr A7,A8,A9,AA}

| PC | when | write |
|---|---|---|
| `1662C-34` | init | **nothing** (mover/speed/angle carried from run state 2; `bra 16678`) |
| `16636` | every frame | `atk = 3` |
| `16642` | `frame != 0` | `atk = 0x13` (active; record 0x13 res 12: announce 0x1B, victim react 2/3, dmg 9) |
| `16648` | | `0x10FC6` C → `state = 6`, rts |
| `16656-62` | `frame == 3` (FF00 hold, spr AA) | `speed -= 3` |
| `16664-72` | … `speed < 0` | `mover = 0`; `speed = 0`; `state = 0`; `partner` poisoned |

No hit branch, no cell swap, no facing change. The hit is entirely result 12's doing.

```c
/* 0x1662C — move 0x2A running shoulder: record 3 on frame 0, 0x13 from
 * frame 1; keeps running, decays -3/frame on the held last frame. */
static uint32_t handler_runshoulder(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) return cell;
    o->atk = (o->frame == 0) ? 3 : 0x13;
    if (rope_contact_10FC6(o)) { o->state = 6; return cell; }
    if ((o->frame & 0xFF) == 3) {
        o->speed -= 3;
        if ((int16_t)o->speed < 0) { o->mover = 0; o->speed = 0; o->state = 0; o->partner = -1; }
    }
    return cell;
}
```

---

## Move 0x20 — running strike (w3, w8 cat1) — and 0x2D cross-check

Cell `0x16A06` = {handler `0x16A16`, mode 1, n 2, dur 6,4, spr AD,AE}.
Cell `0x169FA` (move 0x2D) = {handler `0x16A16`, mode 1, n 1, dur 0xA, spr 95}.
**Same handler for both** — only the art differs.

| PC | when | write |
|---|---|---|
| `16A16` | | `btst #7,(+0x1C)` followed by `nop` — result unused (no init branch) |
| `16A1E` | every frame | `0x10FC6` C → `state = 6`, rts |
| `16A2C` | | `atk = 0x0B` (res 5: dmg 7, victim react 2/3) |
| `16A32` | | `speed -= 2` |
| `16A3A-4E` | `speed < 0` | `state = 0`; `add_pos_delta(8,0,0)`; `partner` poisoned |

`engine/anim.c handler_runatk` matches exactly; register it for move 0x20 as well
(`case 0x20: cell = handler_runatk(o, cell)`). The anim end (`0xFE`) is never tested —
the move ends purely on speed decay; sprite holds on the last frame (mode 1).

---

## Move 0x40 — running jump strike (w2 cat2)

Cell `0x183EC` = {handler `0x18404`, mode 1, n 4, dur 4,4,FF00,8, spr 84,85,1AF,84}

| PC | when | write |
|---|---|---|
| `1840C-10` | init | `knockback(0xB)` → vx fwd 2.75px/f, vz 0x600, grav 0x68, **then** |
| `18416` | init | `mover = 0` (launch armed, not yet applied) |
| `1841A` | init | `f33 \|= 0x08` |
| `18422-3A` | `frame == 0 && count == 0` | `mover = 2` (take off at end of frame 0); `f33 \|= 0x08` |
| `1843C-44` | `frame == 2` (FF00 hold, airborne) | `0x10FC6` C → `state = 4, react_id = 0x17` (rope crash), rts |
| `1845A` | `frame == 2` | `atk = 0x16` (attacker-only record, res 5, dmg 12) |
| `18460-78` | `frame == 2 && landed` | `0x1110E`; `f33 &= ~0x08`; `mover = 0`; `count = 0` (release hold → frame 3) |
| `1847C-84` | `frame == 0xFE` | `state = 0` |

No last_hit check, no partner write, no cell swap, facing unchanged. `atk` is not written on
frames 0/1/3 — `0x24090` leaves it 0 after each sprite change (effectively untargetable;
TODO EXACT if the engine's bookkeeping differs).

```c
/* 0x18404 — move 0x40 running jump strike: launcher 0xB fired at the end
 * of frame 0, record 0x16 while airborne on frame 2, land -> frame 3. */
static uint32_t handler_runjump(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 0xB); o->mover = 0; o->f33 |= 0x08; return cell;
    }
    if (o->frame == 0 && o->count == 0) { o->mover = 2; o->f33 |= 0x08; return cell; }
    if (o->frame == 2) {
        if (rope_contact_10FC6(o)) { o->state = 4; o->react_id = 0x17; return cell; }
        o->atk = 0x16;
        if (o->landed) { eng_sound(0x29); o->f33 &= ~0x08; o->mover = 0; o->count = 0; }
    }
    if ((o->frame & 0xFF) == 0xFE) o->state = 0;
    return cell;
}
```

---

## Move 0x41 — running catch/slam (w3, w9 cat2)

Cell `0x1848C` = {handler `0x18498`, mode 1, n 1, dur 0xE, spr 1A4}

| PC | when | write |
|---|---|---|
| `184A0` | init | `last_hit = 0` (mover/speed untouched → keeps running) |
| `184A8` | every frame | `0x10FC6` C → `state = 6`, rts |
| `184C2` | move b15 clear | `atk = 0x10` (res 7 → victim react **0x1D** because move lo == 0x41; dmg 12) |
| `184C8-E0` | … and `last_hit && victim.state lo == 4 && victim.react lo == 0x1D` | HIT: |
| `184E2` | | `move_id \|= 0x8000` |
| `184E8` | | `spr = 0xFFFF` (attacker hidden; the victim's react-0x1D cell draws the pair) |
| `184EE` | | `count = 0x0C` (hold 12 ticks, matches react 0x1D dur) |
| `184F4` | | `mover = 0` |
| `184F8-FC` | | `victim.partner = self`; `partner = victim` |
| `18502-10` | `frame == 0xFE` (unlatched whiff, or latched after the 0xC hold) | `state = 0`; `partner` poisoned |

Victim side (react 0x1D, `0x1B7A2`): flips facing, snaps to attacker's position, then at 0xFE
goes to react 0x1B (if arrived from state 2) or react 2 with an `add_pos_delta` throw-off.

```c
/* 0x18498 — move 0x41 running catch: record 0x10; a victim already put in
 * react 0x1D by result 7 is latched, the attacker hides for 12 ticks. */
static uint32_t handler_runcatch(eng_obj *o, uint32_t cell)
{
    eng_obj *v = last_hit_obj(o);
    if (!(o->anim_sel & 0x8000u)) { o->last_hit = -1; return cell; }
    if (rope_contact_10FC6(o)) { o->state = 6; return cell; }
    if (!(o->move_id & 0x8000u)) {
        o->atk = 0x10;
        if (v && (v->state & 0xFF) == 4 && (v->react_id & 0xFF) == 0x1D) {
            o->move_id |= 0x8000u; o->spr = 0xFFFF; o->count = 0x0C; o->mover = 0;
            v->partner = self(o); o->partner = o->last_hit; return cell;
        }
    }
    if ((o->frame & 0xFF) == 0xFE) { o->state = 0; o->partner = -1; }
    return cell;
}
```

---

## Engine prerequisites / open items

- `last_hit` (+0x92) must be populated by `eng_hit_scan` bookkeeping (attacker ← victim at
  `0x24DFC`, victim ← attacker at `0x24E40`) — moves 0x04/0x05/0x41 gate their latch on it.
- Result handlers 2, 4, 7, 11, 12 and reactions 0x10/0x12 (`0x1B706`), 0x19 (`0x1BB4C`),
  0x1D (`0x1B7A2`) are needed for these moves to do anything beyond a whiff.
- `0x12C40` f32-bit1 alt launch and the `0x12DAE` double-team block: TODO EXACT (tag only).
- `0x24784` (result 10) compares `D1` not `D0` against the victim facing (`0x24798`) —
  not used by these six moves, noted for whoever ports record 5.
- `0x1C15D2` announce writes are HUD-only; omit or route to the move-name overlay.
