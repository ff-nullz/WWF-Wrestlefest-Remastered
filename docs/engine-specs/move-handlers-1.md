# Move-cell handlers, batch 1: downed / running / anti-run / behind / counter moves

Source: `../wrestlefest-decomp/reference/maincpu.asm` + raw program ROM
(`data/rom/31e14-0.ic18` even bytes, `31e13-0.ic19` odd — verified: `L(0x12614) == 0x00012850`).
Misaligned regions re-disassembled with `tools/m68k_dasm_dump` (every-alignment sweep).
Read-only pass, 2026-08-22. Companion: `docs/engine-specs/strikes.md` (§1f, §3 template),
`anim.md` (cell contract), `reactions.md`, `hit-pipeline.md`, `run-skid-turn.md` (§2c ropes probe).

Engine field mapping used in the C sketches (from `engine/engine.h`): `mover`(+0x01),
`spr`(+0x04), `x/y/z`(+0x06/0A/0E), `off_x/off_y`(+0x18/1A), `anim_sel`(+0x1C),
`state`(+0x20), `count`(+0x22), `frame`(+0x24, low byte +0x25), `partner`(+0x26),
`speed/angle`(+0x2A/2C), `facing`(+0x2E), `f32/f33/f34/f35/f37`(+0x32..+0x37),
`atk`(+0x4C), `vx/vy/vz/grav`(+0x58..5E), `move_id`(+0x60), `react_id`(+0x64),
`hp`(+0x66), `dmg`(+0x68), `band`(+0x70), `down_t`(+0x9A), `mash`(+0xAA here = the
ROM word +0xAA, NOT engine `mash`+0xBD), `lasthit`(+0x92), `revlink`(+0x9C).
"poison partner" = `bset #7,(+0x26)` (lazy divorce, strikes.md §3e). "A2" in a handler =
the partner object (+0x26, dummy 0x7F000 when 0).

---

## 0. Cell records (table `0x12614 + id*4`)

| move | cell | handler | mode | n | durations (raw; ticks = d+1) | sprites |
|---|---|---|---|---|---|---|
| 0x08 | 0x1316C | 0x13180 | 1 | 3 | 0006 000C 0008 | 00CB 00CC 00CD |
| 0x09 | 0x135C8 | 0x135EC | 1 | 7 | 0010 0010 000C 0016 000A 0010 0010 | 018B 00D8 00D9 00DA 00DB 00DC 00DD |
| 0x48 | 0x132D8 | 0x132F4 | 1 | 5 | 0006 ×5 | 0084 0085 001A 001B 001C |
| 0x22 | 0x15B6C | 0x15B8C | 1 | 6 | 0010 0010 000A 000A 0008 0020 | 018B 00CE 00CF 00D0 00D1 00D2 |
| 0x2D | 0x169FA | 0x16A16 | 1 | 1 | 000A | 0095 |
| 0x05 | 0x12F08 | 0x12F40 | 1 | 6 | 0006 0006 0006 0006 0004 000C | 0096 0097 0098 0099 009A 009B |
| — alt | 0x12F28 | 0x12F40 | 1 | 4 | 0004 0004 0006 0006 | 009F 00A0 00A1 009B |
| 0x06 | 0x13020 | 0x13034 | 1 | 3 | 0020 0018 0020 | 00AF 00B0 0000 |
| 0x21 | 0x15B24 | 0x15B34 | 1 | 2 | 000E 0018 | 00B4 00B5 |
| 0x1C | 0x15668 | 0x1567C | 1 | 3 | 000C 0016 0016 | 0161 0162 0163 |
| 0x1D | 0x1585C | 0x15870 | 1 | 3 | 000C 0016 0016 | 0161 016C 016D |
| 0x0A | 0x13734 | 0x13744 | 1 | 2 | 0014 0010 | 00E4 00E5 |
| 0x10 | 0x141D8 | 0x141EC | 1 | 3 | 0014 FF00 0008 | 00EF 00F0 00F0 |
| 0x35 | 0x175A8 | 0x175C4 | 1 | 5 | 0008 0014 0010 FF00 0010 | 0084 0085 00E6 00E7 00E8 |

Hidden cell `0x125C0` = {handler 0x125CC, mode 1, n 1, dur FF00, spr FFFF} — several
handlers substitute it per-frame (`movea.l #$125C0,A1`) to stop drawing/ticking.

---

## 1. Proximity remap `0xEF9A` — verified against code, complete for this batch

Disassembly at `0xEF9A-0xF06E` confirms strikes.md §1f exactly. Precise algorithm:

```
if (D0 >= 0x4A) return keep;                     /* 0xEF9E */
t = byte 0xF070[D0]; if (t == 0) return keep;    /* 0xEFAC-0xEFB0 */
if (!(t & 0x80)) {                               /* plain class: one box */
    inside_box(t)   -> keep;                     /* 0xEFBC, E958 with A1=target */
    else            -> F02E fallback;
} else {                                         /* tiered class 0x81/0x82/0x88 */
    b1 = (D0 == 8) ? 1 : 0xC;                    /* 0xEFCA-0xEFD4 */
    if (inside_box(b1)) {
        D0 == 8 -> keep;                         /* pin cover only in its far window */
        else    -> D0 = byte 0xEFFE[wrestler id]; /* per-wrestler follow-up */
    } else {
        b2 = (D0 == 8) ? 3 : 2;                  /* 0xF00A-0xF014 */
        if (inside_box(b2)) { D0 == 8 -> D0 = 0x0A; else keep; }
        else -> F02E fallback;
    }
}
F02E fallback (carry-set exit, remap CONSUMES the press):
    +0xA3 == 3 -> state = 2 (run)                /* exact compare, both buttons */
    +0xA3 == 1 -> state = 5, move_id = 0        /* jab  */
    else       -> state = 5, move_id = 0x72     /* kick */
```

`0xF070` full table (0x4A bytes, raw):

```
00: 00 00 00 00 00 00 00 00 81 82 03 04 00 00 06 00
10: 05 00 00 05 00 00 00 00 00 00 00 00 00 00 00 00
20: 00 00 88 06 00 00 00 00 00 00 00 00 00 00 00 00
30: 00 00 00 00 00 0B 00 00 00 00 00 00 00 00 00 00
40: 00 00 00 00 00 00 05 05 07 07
```

`0xEFFE` per-wrestler follow-up byte: `10 10 10 10 47 47 0B 46 10 46 10 47`.

`0xE958` box test (verified): profile record = `0xE9BA + D0*8` = 4 signed words
`x_lo, x_hi, y_lo, y_hi`. **X pair is exchanged AND both negated when the OPPONENT
(A1) faces right** (`btst #7,(0x2E,A1)`, `0xE96C-0xE978`) — i.e. the box is expressed
in the opponent's facing frame; +x = the side the opponent faces. Inside (carry set)
iff `opp.x + x_lo < own.x <= opp.x + x_hi` AND `opp.y + y_lo < own.y <= opp.y + y_hi`
(unsigned word compares: bcc on lo means lo-bound must be strictly below own).

`0xE9BA` box profiles (signed px):

| prof | x | y | | prof | x | y |
|---|---|---|---|---|---|---|
| 0 | [-32,+128] | [-56,+40] | | 8 | [-48,+32] | [-24,+24] |
| 1 | [+80,+160] | [-24,+24] | | 9 | [+32,+112] | [-40,+16] |
| 2 | [-48,+56] | [-24,+24] | | A | [-8,+104] | [-72,+24] |
| 3 | [+8,+96] | [-30,+30] | | B | [-32,+96] | [-40,+24] |
| 4 | [-24,+96] | [-48,+24] | | C | [+56,+160] | [-56,+32] |
| 5 | [-64,+192] | [-56,+8] | | D | [-32,+64] | [-52,+32] |
| 6 | [-32,+96] | [-56,+24] | | E | [-64,+80] | [-56,+16] |
| 7 | [-64,+112] | [-56,+24] | | F/10 | [-32,+64] | [-40,+40] |

Whiff-prevention outcome per move in this batch:

| move | class | effect |
|---|---|---|
| 0x08 | 0x81 | inside box **1** (x +80..+160 on the downed victim's facing side — the "one end of the body" window) → keep the pin cover; else inside box **3** → becomes 0x0A (stomp); else strike/run fallback |
| 0x09 | 0x82 | inside box **C** (x +56..+160) → per-wrestler `0xEFFE` follow-up (w0→0x10 leap, w4/w5/wB→0x47, w6→0x0B, w7/w9→0x46); else inside box **2** (x -48..+56, tight) → keep the ground hold; else fallback |
| 0x22 | 0x88 | same tiering as 0x09: box C → `0xEFFE` follow-up, box 2 → keep mount punches, else fallback |
| 0x48 | 0x07 | inside box **7** (x -64..+112, y -56..+24) → keep pick-up; else fallback |
| 0x0A | 0x03 | inside box **3** (x +8..+96) → keep stomp; else fallback |
| 0x10 | 0x05 | inside box **5** (x -64..+192, y -56..+8 — very wide, leap can travel) → keep; else fallback |
| 0x35 | 0x0B | inside box **B** (x -32..+96, y -40..+24) → keep; else fallback |
| 0x2D 0x05 0x06 0x21 0x1C 0x1D | 0x00 | never remapped — always come out |

Tag remap `0xE926` (verified, runs before 0xEF9A): `D0 == 0x48` becomes `0x0A` unless
(opp(+0x7A)->f33 bit0 && own f33 bit0 && own f33 bit2 clear) — both legal men and not
the resting tag partner.

---

## 2. Handler transcriptions

Convention: "first frame" = `+0x1C` bit15 clear (anim not yet latched; the latch is set
by the tick after the handler returns). "committed" = bit15 set. Handler runs EVERY
frame before the tick; `frame` here = word +0x24 (low byte +0x25); `count` = +0x22.
`+0x25 == 0xFE` = mode-1 finished sentinel. Mutual-link check `0x11412`:
carry CLEAR iff `partner->partner == self` (else the pair is stale).

### 2.1 Move 0x08 — pin cover (cat 5 B1) — `0x13180`

Interaction cell: the victim is HIDDEN mid-move (see §5). No `+0x4C` record is ever
armed (untargetable except leftover 0); instead the referee word `+0x4A` is used.

```
first frame (0x13188-0x131B0):
  mover = 0
  x = victim.x; y = victim.y; z = victim.z; facing = victim.facing (byte copy)
  off_x = 0x0050                      ; drawn 0x50 px toward screen +x
  +0x4A = 2                           ; referee: "cover in progress"
end of frame 0 (+0x25==0 && +0x22==0, 0x131C0):
  if link intact (0x11412 cc) && victim.state==4 && victim.react_id low==8:
      victim.state = 0x00FF           ; *** HIDE VICTIM (state FF = not drawn) ***
      f33 |= bit6 on BOTH             ; interaction lock
      facing = victim.facing          ; re-copied
  else ABORT (0x131F4):
      state = 0; off_x |= 0x8000 (clear-after-pass); count = 2; +0x4A = 0
      poison partner; spr = facing|0x0000
      x += (facing bit7 ? -0x80 : +0x80)          ; step off the body
finish (+0x25 == 0xFE, 0x1322C):
  decide who covers (0x13236):
      if own +0x56 bit7 (CPU) && victim +0x56 bit7 (CPU) && victim f33 bit5 (hustle):
          REVERSED: victim.state = 0x0C, victim +0x44 = 0x2000;
                    own state = 0x00FF (hidden), own +0x44 = 0
      else NORMAL: own state = 0x0C (pin), own +0x44 = 0x2000;
                    victim.state = 0x00FF, victim +0x44 = 0
  common (0x13284): off_x |= 0x8000; +0x4A = 0
      victim.down_t = 0; victim.combo_t = 0; victim.combo = 0; victim.mashAA = 0
      victim.facing = own.facing ^ 0x8000         ; head-to-toe
      own.x += (facing bit7 ? -0x80 : +0x80)      ; lie across the body
      victim.x = own.x; victim.y = own.y
      nudge_clipped(self, 0)  ; 0x10B62 D0=0 — ring-clamp both, +0x36 preserved
      nudge_clipped(victim, 0)                     ; via exg A0,A2
```

The pin proper is state 0x0C's machinery (tieup.c side); restore of the hidden half is
done by that machinery, not here. Sprites 0xCC/0xCD (both bodies) are interaction art.

```c
static void mv08_pin_cover(eng_obj *o, eng_obj *v)     /* ROM 0x13180 */
{
    if (first_frame(o)) {
        o->mover = 0; o->x = v->x; o->y = v->y; o->z = v->z;
        o->facing = v->facing; o->off_x = 0x50; o->ref4A = 2; return;
    }
    if ((o->frame & 0xFF) == 0 && o->count == 0) {
        if (link_ok(o, v) && (v->state & 0xFF) == 4 && (v->react_id & 0xFF) == 8) {
            v->state = 0x00FF; o->f33 |= B6; v->f33 |= B6; o->facing = v->facing;
        } else {   /* abort */
            o->state = 0; o->off_x |= 0x8000; o->count = 2; o->ref4A = 0;
            poison_partner(o); o->spr = o->facing;
            o->x += (o->facing & 0x8000) ? -0x80 : 0x80;
        }
        return;
    }
    if ((o->frame & 0xFF) == 0xFE) {
        int reversed = (o->cpu && v->cpu && (v->f33 & B5));
        eng_obj *cov = reversed ? v : o, *hid = reversed ? o : v;
        cov->state = 0x0C; cov->grap44 = 0x2000; hid->state = 0x00FF; hid->grap44 = 0;
        o->off_x |= 0x8000; o->ref4A = 0;
        v->down_t = v->combo_t = v->combo = v->mashAA = 0;
        v->facing = o->facing ^ 0x8000;
        o->x += (o->facing & 0x8000) ? -0x80 : 0x80;
        v->x = o->x; v->y = o->y;
        nudge_clipped(o, 0); nudge_clipped(v, 0);          /* 0x10B62 */
    }
}
```

### 2.2 Move 0x09 — ground hold / submission (cat 5 alt B1: w2/w7/w9; `0xEFFE` reachable) — `0x135EC`

Structurally the sibling of 0x22 (below). Victim hidden for the duration; DA12
interaction frames 0x18B, 0xD8-0xDD.

```
first frame: mover=0; x/y = victim x/y; facing = victim.facing.
end of frame 0 (0x1361A): victim.state==4 && react==8 && link intact ->
    f33 bit6 both; own f35 bit1, victim f35 bit2 (hold roles);
    victim +0xC7 += 1 (times-held); announcer $1C15D2 = (own id, 0x09);
    victim.state = 0x00FF                     ; *** HIDE ***
  else abort: state=0, spr=facing, poison partner, count=1.
finish (+0x25==0xFE, 0x13686):
  if own +0xFE bit7 (match event): release now (0x136E2, below)
  else: frame = 4, count = 0                  ; *** LOOP frames 5-6 ***
        victim.dmg = 3                        ; +3 per loop
        jsr 0x111C8 (KO check):
          carry clear (no KO) -> once (bset#7 move_id, 0x1370E):
              victim.state=5, victim.move_id=0x5D  ; victim's scripted struggle
              jsr 0x215B6 (score); sound 0x32
          carry set (KO fired): if rumble ($1C0161 bit0):
              announcer (victim id, 0x29); victim f32 |= bit4 (FORCED RISE)
              poison own partner; spr = 0x68|facing
          then release (0x136E2): own state=7 (rise); victim.state=4,
              react_id &= 0xFF; clear f33 bit6 both, own f35 bit1, victim f35 bit2
```

NOTE: `victim.state=5/move 0x5D` and the hidden state are exclusive — the 0x5D write
happens on the first loop only, and re-shows the victim through their own cell.

### 2.3 Move 0x48 — pick-up of a downed opponent (cats 5/6/7 B2 column) — `0x132F4`

This is the "grab them off the mat" move (tag-remapped to 0x0A when not legal-man,
`0xE926`). The attacker leaps onto the body, lifts, and the pair converts to a carry:
victim runs scripted move **0x4A** (its cell 0x180AC draws the interaction), the
**attacker hides himself** (`spr = 0xFFFF` + hidden-cell substitution).

```
every frame first (0x132F4): if move_id bit7 (carry latched):
    A1 = 0x125C0 (hidden cell) ; rts        ; stops drawing/ticking own anim
first frame (0x13306):
  mover = 0 (then armed below); vz = 0x480; grav = 0x48
  jsr 0x26AE(D0=1)     ; homing launch: land at victim.x + (victim faces right? -0x30:+0x30),
                       ;   victim.y - 0x20; T = 2*(vz/grav) = 32 ticks;
                       ;   vx clamped ±0x200, vy clamped ±0xC0
  facing = (vx high byte & 0x80) ^ 0x80     ; face along the flight
  if victim.move_id bit7 set -> rts         ; already taken
  victim.state = 5; victim.move_id = 0x4A   ; victim enters "being lifted" script
committed (0x1334A):
  at frame 0 first tick (frame==0 && count==0... exact: both words zero): mover = 2 (velocity)
  if +0x25 < 4: rts                          ; airborne frames
  +0x42 = 0xFFE0                             ; flight floor bias while falling
  if !(f37 bit4, landed): rts
  +0x42 = 0
  CATCH test (0x1337A): victim.state==5 && victim.move_id low==0x4A && link intact
    -> (0x13392): mover=0; state = 5 re-written (re-inits own anim);
       move_id |= 0x8000 (latch -> hidden cell from now on);
       f35 |= bit0 (carrier role); f33 bit6 both; facing = victim.facing;
       x/y/z = victim x/y/z; add_pos_delta(0x40, -8, 0)      ; 0x10BD0, X facing-mirrored
       spr = 0xFFFF                                          ; *** HIDE SELF ***
       jsr 0x215B6 (score, $1C007C-gated); victim-side jsr 0x21732 (exg A0,A2 around)
       victim.state = 5 (re-written); victim.move_id |= 0x8000  ; 0x4A phase 2 (lift)
  MISS (0x13400): own state = 4; react_id = 5 (faceplant);
       spr = 0x001F | facing<<8...  (word 0x1F then facing byte into the high byte);
       dmg = 3 (self-damage); poison own partner; victim.revlink(+0x9C) = 0
```

```c
static void mv48_pickup(eng_obj *o, eng_obj *v)        /* ROM 0x132F4 */
{
    if (o->move_id & 0x8000) { use_hidden_cell(o); return; }
    if (first_frame(o)) {
        o->vz = 0x480; o->grav = 0x48;
        homing_launch(o, v, /*tbl 0x275C idx*/1);      /* 0x26AE: dx +0x30, dy -0x20 */
        o->facing = ((o->vx & 0x8000) ^ 0x8000);
        if (!(v->move_id & 0x8000)) { v->state = 5; v->move_id = 0x4A; }
        return;
    }
    if (o->frame == 0 && o->count == 0) o->mover = 2;
    if ((o->frame & 0xFF) < 4) return;
    o->floor42 = 0xFFE0;
    if (!o->landed) return;
    o->floor42 = 0;
    if ((v->state & 0xFF) == 5 && (v->move_id & 0xFF) == 0x4A && link_ok(o, v)) {
        o->mover = 0; o->state = 5; o->move_id |= 0x8000; o->f35 |= B0;
        o->f33 |= B6; v->f33 |= B6; o->facing = v->facing;
        o->x = v->x; o->y = v->y; o->z = v->z;
        add_pos_delta(o, 0x40, -8, 0);                 /* 0x10BD0 */
        o->spr = 0xFFFF;                               /* attacker hidden */
        score_215b6(o); score_21732(v);
        v->state = 5; v->move_id |= 0x8000;            /* 0x4A lift phase */
    } else {
        o->state = 4; o->react_id = 5; o->spr = (o->facing & 0xFF00) | 0x1F;
        o->dmg = 3; poison_partner(o); v->revlink = 0;
    }
}
```

### 2.4 Move 0x22 — mounted punches (cat 5 both-buttons) — `0x15B8C`

Interaction cell (frames 0x18B, 0xCE-0xD2 draw both bodies); victim hidden.

```
first frame: mover=0; x/y = victim x/y; facing = victim.facing.
end of frame 0 (0x15BAE): victim.state==4 && react==8 && link intact ->
    f33 bit6 both; own f35 bit1; victim f35 bit2;
    victim.state = 0x00FF                       ; *** HIDE ***
    announcer $1C15D2 = (own id byte +0x03, code 0x0A)
  else abort: state=0; spr=facing; poison partner; count=1.
committed frames (0x15C20): atk = 0; if move_id bit7: atk = 0x8014
    ; record 0x14 = {40 04 00 00 00 00 06}: passive, hurtbox 4 (x +0x10..+0x30
    ; = his exposed back), victim-apply 6; bit15 = no-combo-escalation flag.
    ; i.e. during the loop the attacker can be hit off the mount with a special
    ; break reaction; before the first loop he is untargetable (atk 0).
finish (+0x25==0xFE, 0x15C32):
  if own +0xFE bit7: release (0x15C8E below)
  else: frame = 3; count = 0                    ; *** LOOP frames 4-5 (42 ticks) ***
        victim.dmg = 3
        jsr 0x111C8:
          no KO -> once (0x15CBA, bset#7 move_id):
              victim.state=5, victim.move_id=0x5F; 0x215B6; sound 0x32
          KO -> if rumble: announcer (victim id, 0x29); victim f32 |= bit4 (rise);
                poison own partner; spr = 0x68|facing
              release (0x15C8E): own state=7; victim.state=4, react &= 0xFF;
                clear own f35 bit1, victim f35 bit2, f33 bit6 both
```

### 2.5 Move 0x2D — running attack (cat 1, all columns) — `0x16A16`

Tiny. Note the disassembly quirk: `btst #7,(0x1C,A0)` at entry is followed by **NOP**
(0x16A1C) — the first-frame branch was patched out; the body runs identically every
frame. The polar mover armed by run state 2 is left running (handler never clears
`mover`), so the attacker keeps sliding.

```
every frame:
  jsr 0x10FC6 (ropes probe) ; carry (pressed into ropes) -> state = 6 (turn), rts
  atk = 0x0B                ; record 0x0B = {C0 00 00 0A 07 05 00}:
                            ;   active, attack box A (x front+0x10..front+0xFF... raw
                            ;   F0 00 FF 60 -> x -0x10..+0xFF, z 0..0x60), dmg 7,
                            ;   result handler 5 (0x245F0), no hurtbox -> unhittable
  speed -= 2 ; if speed went negative:
      state = 0; add_pos_delta(8, 0, 0) (0x10BD0); poison partner
```

```c
static void mv2d_run_attack(eng_obj *o)                /* ROM 0x16A16 */
{
    if (ropes_probe(o)) { o->state = 6; return; }      /* 0x10FC6 carry */
    o->atk = 0x0B;
    o->speed -= 2;
    if ((int16_t)o->speed < 0) {
        o->state = 0; add_pos_delta(o, 8, 0, 0); poison_partner(o);
    }
}
```

### 2.6 Move 0x05 — running grab/carry (cat 2, all columns) — `0x12F40`

```
first frame: mover = 1 (polar — keeps the run vector); lasthit(+0x92) = 0.
every committed frame: jsr 0x1112E (footstep sound 0x33 on frames 1/4 when count==0).
while move_id bit7 clear (0x12F64):
  atk = 3 (running stance); frames 0-3: atk = 6
      ; record 6 = {C0 06 00 02 0A 04 00}: active, own hurtbox 6, attack box 2
      ;   (x -0x20..front 0xFF, z 0..0x60), dmg byte 0x0A, result handler 4 (0x2452A)
  if lasthit != 0 && lasthit->state==4? no: exact test (0x12F78):
     A2 = lasthit; if A2->react low(+0x65) == 0x10 && A2->lasthit == self:
        partner = lasthit (adopt victim)
        anim_sel = 0x0005 (bit15 CLEAR -> re-init this cell; sub cell 0x12F28 below)
        move_id |= 0x8000 (caught latch); jsr 0x1108C (CPU wounded roll, sets own
            f34 bit4 when rand byte < 0x110C8[id]); atk = 0
  else: jsr 0x10FC6; carry -> state = 6 (turn), rts
caught (move_id bit7 set, 0x12FBE):
  at frame 2 first tick (+0x25==2 && count==0):
      victim.state = 4; victim.react low(+0x65) = 0x19 (carried reaction)
      carry_at(0x10, -1, 0x38)      ; 0x10B9A: victim pos = own pos, then
                                    ;   x += ±0x10 (own facing), y += -1, z += 0x38
      jsr 0x110E0                   ; singles only: victim's teammate in walk-in
                                    ;   sub 1 -> teammate f34 |= bit5 (alert)
finish (+0x25==0xFE): state = 3 (SKID), poison partner.
tail every frame: if move_id bit7: A1 = cell 0x12F28   ; caught-variant frames
                                                        ; (0x9F 0xA0 0xA1 0x9B, faster)
```

The hit that sets the victim's `+0x65 = 0x10` comes from the pipeline (record 6 →
result 4), which also writes `lasthit` both ways; the handler's `+0x65==0x10 &&
victim.lasthit==self` test is how it detects "my grab connected".

### 2.7 Move 0x06 — anti-run catch (cat 3 B1/B2) — `0x13034`

```
first frame (0x1303C): mover = 0
  f34 bit3 test-and-clear (bclr): was set (pipeline result-3 wrote it: "collision
      resolved into a grapple") -> RESTART anim: anim_sel |= 0x8000, frame=0, count=0
  elif move_id bit7 (caught latch): RESTART with pose: anim_sel |= 0x8000, frame=0,
      spr = 0x60|facing (hold pose), count = 0x0C
  else: lasthit = 0
committed, bit7 clear (0x13088):
  atk = 1; frame 0 -> atk = 4
      ; record 4 = {C0 09 00 04 0E 03 0C}: active, hurtbox 9, attack box 4
      ;   (x -0x30..front, z 0..0x60), byte4 0x0E, result 3 (0x244E0),
      ;   victim-reaction 0x0C (0x24D5E)
  if lasthit != 0 (caught): atk = 0 AND +0x4E = 0 (clr.l), count = 0x0C,
      move_id |= 0x8000
  frame 0 && count low byte >= 2 -> wait; count < 2 -> EXIT (state 0, poison, atk 0)
bit7 set or frames > 0: +0x25==0xFE -> EXIT (state 0, poison, atk = 0)
```

So the catch window is frame 0 only (33 ticks, sprite 0xAF); on a whiff it exits at
the END of frame 0 without ever showing frames 1-2. The runner's reaction and the
follow-up grapple state are armed by pipeline result 3 / reaction 0x0C, which also
re-enters this cell via the f34-bit3 restart.

### 2.8 Move 0x21 — anti-run swing (cat 4 B1/B2) — `0x15B34`

```
first frame: mover = 0.
frame 1: atk = 8
    ; record 8 = {C0 00 00 04 10 0D 00}: active, attack box 4, dmg byte 0x10,
    ;   result handler 0x0D (0x245D6) — the back-drop-the-runner result.
    ; frames 0 and 0xFE leave atk = whatever 0x24090 cleared -> 0 (untargetable).
finish: state = 0; poison partner; spr = facing|0x0000.
```

```c
static void mv21_antirun_swing(eng_obj *o)             /* ROM 0x15B34 */
{
    if (first_frame(o)) { o->mover = 0; return; }
    if (o->frame == 1) o->atk = 8;
    if ((o->frame & 0xFF) == 0xFE) {
        o->state = 0; poison_partner(o); o->spr = o->facing;
    }
}
```

### 2.9 Move 0x1C — behind grab (cat 0x10 B1) — `0x1567C`

Not an interaction-hide cell: the victim stays visible in scripted move 0x52.

```
first frame: mover = 0
  if move_id bit7 (re-entry): anim_sel |= 0x8000, frame=0, count=0;
      victim +0x60 HIGH BYTE = 0xC0 (victim move flags bits15+14) ; jump to CATCH
  else rts.
committed:
  frames > 0 && !(f32 bit0 run-in): atk = 0x0A
      ; record 0x0A = {40 00 00 00 00 00 02}: passive, NO hurtbox bytes ->
      ;   invulnerable, but victim-apply 2 (0x24924) if somehow hit as victim
  end of frame 0 (+0x25==0 && count==0, 0x156D0):
      GATE: |own.y - victim.y| < 0x10  &&  |own.x - victim.x| < 0x50
            && facing bytes equal (eor) && link intact (0x11412 cc)
            && victim in {state 4 react-low 1 (standing dazed), state 1, state 0}
      CATCH (0x15730): victim.state = 5; victim +0x61 = 0x52 (byte write);
          victim +0x44 = 0; victim.facing = own.facing;
          once (bset#7 own move_id; skip if set):
              victim.react &= 0xFF; f33 bit6 both; own f35 bit1;
              sound 0x32; jsr 0x215B6; announcer = (own id, 0x1D)
      WHIFF (0x1578C): poison partner;
          if f32 bit0 (run-in): state = 1, +0xAE = 1, spr = facing
          else state = 0
  finish (+0x25==0xFE, 0x157BA):
      if own +0xFE bit7: own state = 0; victim.state = 0  (release both)
      else: frame = 0; count = 0            ; *** LOOP whole cell (46 ticks) ***
            victim.dmg = 4
            jsr 0x111C8: KO -> own move: state=5, move_id=0x6F;
                               victim: state=5, move_id=0x6E; jsr 0x212A0
tail (every committed tick, 0x1580E): D1 = +0x25 (+1 if count==0):
      D1==0 -> spawn_fx(0)          ; 0x10D3A id 0: spr 0x164 at own pos
      D1==1 -> spawn_fx(1); spawn_fx(3)      ; sprs 0x165, 0x166
      else  -> spawn_fx(4)          ; spr 0x167 (struggle flashes)
```

`0x10D3A(id)`: allocates one of 11 overlay slots at $1C1258 (0x2A stride); 6-byte
table `0x10DDA[id]` = {dYZ, sprite, +0x12}: slot sprite = tbl.spr | own facing,
x = own.x ± (+0x19 byte, facing-mirrored), y = own.y + dYZ, z = own.z − dYZ.
Table: id0 spr 0x164, id1 0x165, id3 0x166 (dYZ −4), id4 0x167, id8 0x16E, id9 0x16F
(all others dYZ −2, +0x12 = 0).

### 2.10 Move 0x1D — behind lift / atomic-drop hold (cat 0x10 B2) — `0x15870`

```
first frame: mover = 0
  if move_id bit7 clear: victim.combo = 0; own +0x44 = 0; rts
  else (re-entry): anim_sel |= 0x8000; frame=0; count=0;
      victim +0x60 HIGH BYTE = 0x80; jump to CATCH
committed:
  frames > 0: atk = 0x0D    ; record 0x0D = {40 00 00 00 00 00 04}: passive,
                            ;   invulnerable, victim-apply 4
  end of frame 0 (0x158B4): link intact && victim.state==4 && react-low==1 ->
      CATCH (0x158DA): move_id |= 0x8000; f33 bit6 both;
          victim.state = 5; victim +0x61 = 0x53; victim +0x44 = 0
      else: poison partner; state = 0
  frames > 0 / after: if own +0xFE bit7: own state = 0; victim.state = 0
  finish (+0x25==0xFE, 0x15928): frame = 0; count = 0        ; *** LOOP, no damage ***
      re-face (once per loop):
        singles ($1C0161 bit0 clear): once (+0x45 bit0 latch):
            face toward own teammate(+0x86): facing = (teammate.x >= own.x) ? right : left
        rumble (bit0 set): facing = (own.x < 0x280) ? right : left  (face ring center)
      victim.facing = own.facing; victim.x = own.x; victim.y = own.y - 1
      victim.x += ±(width table: 0x18F08[0x18DFA[victim id]] byte, victim-facing-
                    mirrored)                       ; per-wrestler carried offset
      jsr 0x20F04 ($1C007C-gated scoring tail)
tail: D1 = +0x25 (+1 if count==0): D1==1 -> spawn_fx(8) (spr 0x16E);
      D1!=0 -> spawn_fx(9) (spr 0x16F).   [68k quirk: the loop path reaches the
      D1 test with a stale D1 — harmless, only picks which flash sprite]
```

The victim escapes via their own move-0x53 script / the `0xEBC4` follow-up ladder
(state5 0x53 → attacker move 0x57), not via this handler.

### 2.11 Move 0x0A — stomp & universal hold-breaker (cat 6, many remaps) — `0x13744`

Damage is applied by the HANDLER, not the hit pipeline (`atk` stays 1 = plain
hittable). Action point = end of frame 0 (21 ticks in).

```
first frame: mover = 0.
committed: atk = 1
  end of frame 0 (+0x25==0 && count==0) && victim +0xFE == 0:
    A3 = victim.partner
    victim.state == 4:
        react-low 8 or 9 -> HIT: victim.state = 4 (rewritten);
            victim.react_low = react_low - 8 + 6   (8->6, 9->7: lying-hit bands)
            victim.dmg = 2; sound 0x32; poison own partner;
            victim +0xE4 += 1; $1C15D2 = 0x0F0B (announcer); sound 0x2A
        else: nothing
    victim.state == 5 (breaking up someone's move; D1 = victim.move_id & 0x80FF):
      0x8048 (carrier mid-pickup, 0x137EA):
          if victim.revlink is a live move-0x49 object A4 (2-man rumble lift):
              own partner = A4; treat as the 0x8049 case below
          else: victim.react_id = 0x0F (dropped); victim.facing ^= 0x8000;
              [with A0=A3 carried] carry_at(0x50, -1, 0x20)   ; 0x10B9A: reposition
                                                    ; the pair for the drop
          merge 0x1385A: jsr 0x24900 (announcer 0x0F19 if the pair isn't own
              teammate, singles only); victim.state = 4;
              victim f35 bit0 clear; f33 bit6 clear on victim AND A3;
              jsr 0x21282 on A3 (combo-announce tail); A3.state = 7 (rise);
              poison own partner; victim.revlink = 0; poison victim.partner;
              sound 0x2A
      0x800E (over-shoulder carry): victim.react_id = 0x10F;
          [A0=A3] carry_at(0x14, -1, 0x20); merge 0x1385A as above
      0x801A (0x138B2): victim.state=4, react=6; victim f35 bit0 clear;
          [A0=A3] add_pos_delta(-0x60, -1, 0), jsr 0x21282;
          victim.off_x |= 0x8000; A3.state = 7; 0x24900; f33 bit6 clear both;
          poison own AND victim partners; sound 0x2A
      0x8009 (ground hold above, 0x13922): victim.state=4, react=6;
          victim f35 bit1 clear; victim.facing ^= 0x8000;
          [A0=victim] add_pos_delta(-0x30, -1, 0);
          A3.state = 7; A3.facing ^= 0x8000; A3 f35 bit2 clear;
          jsr 0x212A0 on A3; 0x24900; f33 bit6 clear victim+A3;
          poison own and victim partners; sound 0x2A
      0x8061 (climbing the corner): 0x24900; victim.state = 5,
          victim.move_id = 0x77 (knocked off the turnbuckle); sound 0x2A
      0x8049 (second lifter, 0x139CE): victim.state = 4, react = 0x0F;
          victim f35 bit0 clear; A3 +0x60 bit5 clear (2-man link flag);
          victim f33 bit6 clear; poison own partner; A3.revlink = 0; sound 0x2A
finish (+0x25==0xFE): state = 0; poison partner.
```

### 2.12 Move 0x10 — leap attack (cat 0x12 B2; `0xEFFE` follow-up for w0/w6/wB) — `0x141EC`

Two entry contexts: partner climbing the corner (move 0x61) or partner downed. Damage
handler-applied on landing; no `atk` write at all (untargetable in flight after each
sprite change).

```
first frame (move_id bit7 clear, 0x141F6):
  if victim.state==5 && victim.move_id low==0x61 (climber):
      vz = 0x300; grav = 0x48; homing 0x26AE(5)   ; dx +0x40, dy -1; T = 20 ticks
      facing = (vx & 0x8000) ^ 0x8000
  else (downed victim):
      x = victim.x + (victim faces right ? -0x38 : +0x38)   ; body-end position
      facing = (new x < old own x) ? left : right            ; face back the way
      y = victim.y; z = victim.z; add_pos_delta(0x10, 0, 0x10)
      vz = 0x300; grav = 0x48
      homing 0x26AE(victim react-low==8 ? 0x0A : 0x0B)  ; dx +0x40 dy -12 / dx +0x38 dy -1
  common: mover = 2 (velocity); z += 0x40; off_y low = 0x10
committed (0x142B6):
  frame 0 first tick: off_x |= 0x8000
  frame 1 (the FF00 hold = airborne, 0x142C8):
      +0x42 = 0xFFE0 (flight floor bias)
      if !(f37 bit4 landed): rts
      +0x42 = 0; jsr 0x1110E (thud: sound 0x29, or 0x2D if f33 bit2); mover = 0
      jsr 0x11152: carry iff partner is downed (state4 react 8/9) AND own inside
                   box 0x0A of 0xE958 relative to the victim (x -8..+104 on the
                   victim's facing side, y -72..+24)
        HIT DOWNED (0x142FA): count = 0 (release the hold-forever frame);
            move_id |= 0x8000; y = victim.y - 1; off_y low = 0x0C
            victim.state = 4; victim.react &= 0xFF;
            victim.react_low = react_low - 8 + 6 (8->6/9->7); victim +0xC7 += 1;
            victim.dmg = 0x0A; victim +0xD2 += 1; sounds 0x2B + 0x32
        else jsr 0x11192: carry iff partner is state5 move 0x61 AND inside box 0x0A
        HIT CLIMBER (0x14354): count = 0; move_id |= 0x8000; y = victim.y - 1;
            off_y low = 0x0C; victim.state = 5; victim.move_id = 0x77 (knocked off);
            victim.dmg = 0x0A; sound 0x32; victim +0xC7 += 1
        MISS (0x143D6): own state = 4; own react_id = 5 (faceplant);
            add_pos_delta(0x50, -1, 0); facing ^= 0x8000;
            spr = 0x1F | facing high byte; own dmg = 3 (self-damage); sound 0x32;
            $1C15D2 = 0x0F1A; poison partner
finish (move_id bit7 set path, +0x25==0xFE, 0x1439E): off_x |= 0x8000;
    state = 7 (rise); spr = 0x68|facing; add_pos_delta(0x18, -1, 0); poison partner.
```

`0x26AE` (homing launch, exact): table `0x275C[D0]` = (dx, dy) signed words, dx negated
if the PARTNER faces right; flight ticks T = 2 * floor(+0x5C / +0x5E);
`vx = ((partner.x + dx − own.x) << 8) / T` clamped to ±0x200;
`vy = ((partner.y + dy − own.y) << 8) / T` clamped to ±0xC0. Entries used here:
idx1 (+0x30,−0x20), idx3 (+0x58,−8), idx4 (+0x18,−8), idx5 (+0x40,−1),
idx0xA (+0x40,−12), idx0xB (+0x38,−1).

### 2.13 Move 0x35 — ropes-walk body splash (cat 7 B1, wrestler 0) — `0x175C4`

**See §6 for the premise correction: this is NOT the pick-up.** Four-phase move; the
top-level handler dispatches on `+0x44 & 3` through table `0x175EC` =
{0x175FC, 0x176C4, 0x1770C, 0x1778E}. Tag mode ($1C0161 bit1) forces `+0x44 = 3`
(skips the walk phases; direct leap).

```
phase 0 — 0x175FC: walk to the far ropes.
  first frame: mover = 1; speed low byte = walk table 0x11D4A[id];
    victim.down_t = 0x200 (keep them down);
    victim.mashAA = tbl0x176BA[min(victim +0xDC, 5)] − victim.hp (floor 1)
        ; 0x176BA words: 16 1E 16 0A 00 0A (escape-press seeds; +0xDC = times
        ;   already splashed) ... +6 if own +0x61 == 0x23 (code shared with move 0x23)
    facing/angle: own.x >= 0x270 -> face left, angle = 0xC0;
                  else face right, angle = 0x40
  committed: atk = 0 (untargetable);
    when pressed into the X bound being walked toward (f37 bit0 if facing right,
    bit1 if facing left): jsr 0x10FC6 (rope shake); state = 5 re-written (re-init);
    +0x44 += 1  -> phase 1
    else: A1 = walk cell 0x11C62 (substituted: draws the walk anim); 0x1112E footsteps
phase 1 — 0x176C4: turn at the ropes.  facing ^= 0x8000; angle low ^= 0x80;
  mover 0 then 1 at frame 1; speed = 0x0C; A1 = turn cell 0x11DD0;
  +0x25==0xFE -> state=5 rewrite, +0x44 += 1 -> phase 2
phase 2 — 0x1770C: walk back to the body.
  first frame: mover = 1, walk speed; target +0xBE =
      victim.x + (victim faces right ? -0x38 : +0x38)      ; one END of the body
                + (own facing right ? -0x60 : +0x60)       ; approach overshoot
  committed: walk cell 0x11C62 + footsteps until own.x crosses +0xBE in the walk
      direction -> state=5 rewrite, +0x44 += 1 -> phase 3
phase 3 — 0x1778E: the splash (cell frames now play: 0x84 0x85 0xE6 0xE7-hold 0xE8).
  first frame: mover = 0; vz = 0x600; grav = 0x48;
      homing 0x26AE(same facing as victim ? 3 : 4)     ; dx +0x58 / +0x18, dy -8
  frame 0 first tick: mover = 2
  frame 1 first tick: off_y low = 0x28
  while vz >= 0: move_id bit4 set... (0x177F8: at apex latch +0x60 bit4);
      +0x42 = 0x20 while rising
  landed (f37 bit4): +0x42 = 0; off_x |= 0x8000
      jsr 0x11152 (downed victim + box 0x0A) && link intact (0x11412):
        HIT (0x17832): mover=0; count=0; $1C1800 = 0x0E (screen shake);
            move_id |= 0x8000; y = victim.y − 1; victim.state = 4;
            victim.react_id = 0x1A (squashed flat); move_id bit4 cleared;
            victim.dmg = 0x12; victim +0xDC += 1; victim +0xC7 += 1;
            sounds 0x2B + 0x32; announcer = (own id, 0x12);
            victim.down_t = 0; jsr 0x10F56 (re-seed forced-down time)
        MISS (0x178CC): state = 4; react_id = 5; own dmg = 3; sound 0x32;
            $1C15D2 = 0x0F1A; poison partner; victim.down_t = 0; victim.mashAA = 0;
            A1 = hidden cell 0x125C0; spr = 0x1F | facing byte
  latched (move_id bit7, 0x178A4): frames >= 4 -> spawn_fx(0x0F) per tick;
      +0x25==0xFE -> state = 7 (rise); poison partner
```

---

## 3. Shared helpers referenced (one-liners, all verified in this pass)

| PC | role |
|---|---|
| `0x10B62` | clipped facing-relative X slide: `+0x3E = D0`, one bounds pass, `x += +0x38` pushback (strikes.md §3b) |
| `0x10B9A` | carry partner: partner pos = own pos; then partner.x += ±D0 (own facing), y += D1, z += D2 |
| `0x10BD0` | `add_pos_delta`: own x/y/z += D0/D1/D2, D0 negated when facing bit7 (already generated) |
| `0x26AE` | homing launch — see §2.12; uses PARTNER's facing to mirror dx |
| `0x10D3A` | spawn overlay effect from $1C1258 pool; table 0x10DDA (see §2.9) |
| `0x10FC6` | ropes probe; carry = pressed into ropes; side-effect rope shake (run-skid-turn.md §2c) |
| `0x11412` | mutual-link check: carry CLEAR iff partner->partner == self |
| `0x11152` | carry iff partner is downed victim (state 4, react 8/9) AND own inside E958 box 0x0A of partner |
| `0x11192` | carry iff partner is state5 move 0x61 (climber) AND inside box 0x0A |
| `0x111C8` | KO check: CPU-game only ($1C007C), both legal, victim hp==0; fires match-end events (+0xFE = 0x8000/0x8001 pairs, $1C1214 = 0x8009, +0x4A = 4, jingle) or rumble elimination (+0xC4+=1, 0x21358); carry = fired; once-latch = bset#3 of +0x60 high byte |
| `0x1108C` | CPU-game roll: rand byte < 0x110C8[id] (88 80 44 40 78 78 50 80 80 85 80 40) -> own f34 bit4 |
| `0x110E0` | singles only: victim's teammate in walk-in sub1 -> teammate f34 bit5 |
| `0x1112E` | footsteps: sound 0x33 when count==0 && frame 1 or 4 |
| `0x1110E` | thud: sound 0x29 (0x2D if f33 bit2) |
| `0x10F56` | seed forced-down time +0x9A by band (reactions.md) |
| `0x2052` | sound submit (ids here: 0x2A punch-bed, 0x2B heavy hit, 0x31, 0x32 crowd, 0x33 step) |
| `0x215B6` `0x21732` `0x212A0` `0x21282` `0x20F04` | scoring/announce family, ALL gated `tst.w $1C007C; beq rts` — no-ops in a human-vs-human game. 0x21282 extra: if +0x109 in {4,5} -> $1C15D2 = 0x0F1E, clr +0x108. 0x215B6 exists as `src/generated/re_0215b6.c` — call the generated body |
| `0x24900` | if singles && A1's teammate != A0: $1C15D2 = 0x0F19 ("broke up the move") |

`+0x4A` (word): referee-context word, writers only: 1 (0x12018, 0x1AB82), 2 (pin cover
0x131AA), 3 (0x73CE, 0x1A470), 4 (KO 0x11262, referee 0x20142). Not in memory-catalog
yet; consumed by the referee module.

Attack records armed by this batch (`0x24EF6 + id*7`, {flags, hurt1, hurt2, atkbox,
dmg, result, reaction}): id 3 = `C0 00 00 09 00 02 00`, 4 = `C0 09 00 04 0E 03 0C`,
6 = `C0 06 00 02 0A 04 00`, 8 = `C0 00 00 04 10 0D 00`, 0x0A = `40 00 00 00 00 00 02`,
0x0B = `C0 00 00 0A 07 05 00`, 0x0D = `40 00 00 00 00 00 04`, 0x14 = `40 04 00 00 00
00 06`, 1 = `40 00 00 00 00 00 00`. Result/reaction handler PCs are in
hit-pipeline.md ("Result table 0x24398 / Reaction table 0x243D4").

---

## 4. Announcer/event writes used ($1C15D2 word; byte pair = (wrestler id, code))

(own id, 0x09) move 09 catch · (own id, 0x0A) move 22 catch · (own id, 0x1D) move 1C
catch · (own id, 0x12) move 35 hit · (victim id, 0x29) rumble KO (09/22) · word
0x0F0B stomp-a-downed · 0x0F19 broke-up (0x24900) · 0x0F1A splash/leap miss ·
0x0F1E combo tail (21282) · 0x0F2A victory (111C8). `$1C1800 = 0x0E` = screen shake
(moves 0x35 hit, and 0x2D's sibling handler).

---

## 5. Interaction cells (DA12) — who is hidden, and restore

| move | hidden object | how hidden | drawn by | restored |
|---|---|---|---|---|
| 0x08 | victim at end of frame 0 (`state = 0x00FF`); at finish the NON-covering half gets `state = 0x00FF`, the coverer enters state 0x0C | state FF (anim wholly skipped) | attacker frames 0xCC/0xCD, then the state-0x0C pin cell | by the pin/state-0x0C machinery |
| 0x09 | victim (`state = 0x00FF`) at catch | state FF | attacker frames 0x18B, 0xD8-0xDD (DA12 rows) | first loop re-arms victim as move 0x5D; release path writes victim state 4 react &= 0xFF |
| 0x22 | victim (`state = 0x00FF`) at catch | state FF | attacker frames 0x18B, 0xCE-0xD2 | release: victim state 4, react &= 0xFF; or victim scripted 0x5F |
| 0x48 | ATTACKER (`spr = 0xFFFF` + per-frame hidden-cell 0x125C0 substitution once move_id bit7) | sprite FFFF + hidden cell | victim's move-0x4A cell (0x180AC) | by the 0x4A script chain when the slam resolves |
| 0x1C/0x1D | nobody — both visible (attacker hold frames + victim scripted 0x52/0x53) | — | — | — |
| 0x05/0x06/0x21/0x2D/0x0A/0x10/0x35 | nobody (0x35's MISS path substitutes own hidden cell + splat sprite) | — | — | — |

The DA12/packed distinction per sprite word lives in the frame display programs
(gfx-findings.md opcode map), not in these handlers; the engine-side contract is
exactly the hide/restore writes above plus `+0x26` partner rows for the drawer.

---

## 6. Coordinator addendum — move 0x35 and "the pick-up at the head"

(a) is §2.13 above. Findings against the stated premise:

* **Move 0x35 is NOT a pick-up.** It is the auto-walk ropes splash: walk to the far
  rope, turn, walk back to a point `victim.x ± 0x38` (victim-facing-mirrored) `± 0x60`,
  leap, splash for 0x12 damage; the victim STAYS DOWN (react 0x1A, down-time
  re-seeded by 0x10F56). In tag mode (bit1) the walk phases are skipped entirely.
* (b) The positional gates that exist for 0x35: selection remap class `0xF070[0x35] =
  0x0B` → must be inside E958 box **B** (x −32..+96 on the victim's facing side,
  y −40..+24) or the press degrades to jab/kick/run; landing applies only if inside
  box **A** (x −8..+104, y −72..+24) of the victim (0x11152) with the link intact.
  The "one end of the body" feel comes from these boxes being mirrored by the DOWNED
  VICTIM's `+0x2E` facing — they cover the side the victim faces (head end for the
  lying frames; verify the art orientation against MAME before naming it "head" in code).
* **The actual pick-up of a downed opponent is move 0x48** (B2 column of ALL downed
  cats 5/6/7, §2.3): position window = class 7 box **7** (x −64..+112, y −56..+24),
  tag-legality remap 0xE926 (0x48→0x0A for the illegal man). At completion the victim
  goes into scripted move **0x4A** (lift/carry chain — cell 0x180AC), NOT into tie-up
  0x0B nor pin 0x0C; links stay as armed by the category's 0xF178 (`+0x26` both ways),
  and the attacker hides (spr 0xFFFF). Slam/hold resolution is the 0x4A script's.
* (c) **Forced rise `f32 |= bit4` confirmed** (consumed at reactions.md §2d "if +0x32
  bit4 → RISE"), but its only two writers in the whole ROM are the **rumble-KO paths**
  of move 0x09 (`0x136CA`) and move 0x22 (`0x15C76`) — no pick-up move uses it; 0x48
  stands the victim up by scripting them into move 0x4A instead.
* Per-wrestler cat-7 B1 alternatives (w1/w3=0x0E, w2=0x0B, w4=0x14, w5/wA=0x46/0x47,
  w6/wB=0x10, w8=0x23, w9=0x13): 0x0E/0x14/0x46/0x47 all open with the same lift
  sprites 0x84/0x85 — those are the per-wrestler "pick them up into a throw" family,
  cells at 0x13D26 / 0x14B48 / 0x14424 / 0x14440 (0x46 and 0x47 share handler
  0x14458), not yet transcribed (out of this batch's scope).
