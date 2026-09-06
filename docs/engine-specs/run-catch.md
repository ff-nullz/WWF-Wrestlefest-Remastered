# Anti-run catch (move 0x06 / result 3 / reaction 0x0E) and whip 0x17 direction

Source: `reference/maincpu.asm`, read-only, 2026-08-22. Byte dumps via a parse of the
asm hex column. Field names as requested (state +0x20, move_id +0x60, react_id +0x64,
partner +0x26, grap44 +0x44, facing +0x2E (bit7 of the byte = bit15 of the word = faces
RIGHT), angle +0x2C/+0x2D, x/y/z +0x06/+0x0A/+0x0E, vx/vy/vz +0x58/5A/5C, grav +0x5E,
mover +0x01, count +0x22, frame +0x24 (low byte +0x25), dmg +0x68 (low byte +0x69),
last_pair +0x92, atk +0x4C).

Headline: **there is no separate "backdrop on a runner" move.** The duck IS move 0x06
(cell 0x13020, sprites 0xAF/0xB0). The catcher never throws anybody: the *victim's*
reaction 0x0E (cell 0x1B460) teleports him onto the catcher's head (z+0x48) and fires
launcher 4 (`vx=-0x180` forward *in the runner's own facing*, `vz=0x600`, `grav=0x38`),
so he sails over the catcher and lands with reaction 5 on the far side. The catcher just
finishes his own three-frame anim and returns to state 0 with his facing unchanged.

---

## 1. Attack selector — standing player vs an incoming runner

Gates in front (strikes.md §0/§1a): `+0x20` bit15 set, `+0xFE==0`, new-press `+0xA3&3`,
own `+0x21` != 4. Category chain reaches `0xE2DC` only after cats 0x11,1/2,5,6/7,8,9,
0xA-D,0xE/F fail (none match for a standing man facing a runner).

```
0xE2DC  A1 = +0x7A (primary opponent)
0xE2E0  btst #5,(+0x34,A0)          ; bne -> no match (0xE022)
0xE2EA  btst #4,(+0x33,A1)          ; opponent RUNNING flag; beq -> no match
0xE2F4  own +0x21 == 0 or 1          ; else no match
0xE308  D3 = 1                        ; arms the facing/zone remap 0xE82E
0xE30C  A2 = 0xE33A
0xE312  bchg #7,(+0x62,A0)           ; alternation latch; was set -> A2 += 0x24
0xE320  D0 = (+0x70,A1) * 0xC + (+0x02,A0)   ; opp HP band (0/1/2) * 12 + own id
0xE32E  D0 = byte A2[D0]             ; category 3 or 4
0xE332  bsr 0xF178                    ; partner link: +0x26 = A1; if A1->+0x26==0, A1->+0x26 = A0
0xE336  bra 0xE028                    ; carry + D0 = cat
```

Matrix `0xE33A` (2 banks x 3 bands x 12 ids):

```
bank0 band0: 03 03 03 03 03 03 03 03 03 03 03 03
bank0 band1: 04 04 04 04 04 04 04 04 04 03 04 04
bank0 band2: 04 04 04 04 04 04 04 04 04 04 04 04
bank1 band0: 03 03 03 03 03 03 03 03 03 03 03 03
bank1 band1: 04 04 04 04 04 04 04 04 03 04 04 04
bank1 band2: 04 04 04 04 04 04 04 04 04 04 04 04
```

So **cat 3 whenever the runner is in HP band 0 (healthy)**, cat 4 when he is hurt
(bands 1/2), with two id-specific band-1 exceptions (id 9 bank0, id 8 bank1). The
alternation latch is nearly irrelevant.

Row fetch `0xDF96`: `A2 = *(0xE4FE + id*4)`; byte `A2[cat*3 + col]`, col = B1 0 / B2 1 /
both 2. Per-wrestler bytes at map+9 (cat 3: B1,B2,both) and map+12 (cat 4):

```
id map     cat3       cat4
0  E52E    06 06 FF   21 21 FF
1  E56E    06 06 FF   2E 2E FF
2  E5AE    06 06 FF   03 03 FF
3  E5EE    06 06 FF   31 31 FF
4  E62E    06 06 FF   01 01 FF
5  E66E    06 06 FF   2E 2E FF
6  E6AE    06 06 FF   27 27 FF
7  E6EE    06 06 FF   07 07 FF
8  E72E    07 07 FF   21 21 FF
9  E76E    03 03 FF   2E 2E FF
A  E7AE    06 06 FF   27 27 FF
B  E7EE    06 06 FF   03 03 FF
```

B1 or B2 vs a healthy runner = **move 0x06** for ids 0-7, 0xA, 0xB (id 8 -> 0x07, id 9
-> 0x03). Both buttons = 0xFF = run (`0xDFC2`: `+0x20=2`, `clr.l +0x26`).

Remaps on the way out:
* `0xE926` tag remap: only touches 0x48. No effect.
* `0xEF9A` proximity remap: `0xF070[0x06] == 0` (listed entries are 08,09,0A,0B,0E,10,13,
  22,23,35,46-49 only) -> **keep; never degraded to jab/kick/run**. Same for 0x21/0x2E/
  0x03/0x31/0x01/0x27/0x07 (all `t==0`).
* `0xE82E` facing/zone remap (D3=1): per-wrestler triple lists (`0xE8AE`) only contain
  moves 30/19/18/33/2B/1A -> no triple matches 0x06 -> **keep**.
* `0xDFE8`: `+0x20 = 5`, `+0x60 = 0x0006`. Nothing else. `+0x26` already links to the
  runner via `0xF178`.

---

## 2. Hit-record 4 and result 3 `0x244E0`

Records (7 bytes, `flags hurt1 hurt2 atkbox dmg result reaction`), base `0x24EF6`:

```
rec 03 @24F0B: C0 00 00 09 00 02 00   ; the RUNNER (active! box 9 = FA 00 FF 60: x[front+1..front+6], z[0..0x60])
rec 04 @24F12: C0 09 00 04 0E 03 0C   ; the CATCHER: hurtbox 9 = E4 00 FF 60 (front 0x1C px),
                                      ;   attack box 4 = D0 00 FF 60 (x[front+1..front+0x30], z[0..0x60]),
                                      ;   damage 0x0E, result 3 (0x244E0), reaction 0x0C (0x24D5E)
rec 08 @24F2E: C0 00 00 04 10 0D 00   ; move 0x21 swing (cat 4 alternative): box 4, dmg 0x10, result 0x0D
```

Result 3, attacker A0 = catcher (record 4 live), victim A1 = runner, A2 = record 4:

```
244E0  tst.b (+0x4C,A1)          ; victim atk high byte != 0 -> VETO (C=1)
244E8  btst #4,(+0x33,A1)        ; victim must be RUNNING, else VETO
244F0  $1C15D2 = (+0x03,A0)      ; "last move" log: attacker slot byte
244F8  $1C15D3 = 3               ;   ... move class 3
24500  D0 = (+0x2E,A0); D1 = (+0x2E,A1); eor.b -> equal facings -> VETO
                                 ; (same facing = he is running AWAY: no catch)
2450C  victim +0x20 = 4
24512  victim +0x64 = 0x000E     ; reaction 0x0E
24518  victim +0x69 = rec[4] = 0x0E  ; damage 14 (low byte of +0x68)
2451E  C = 0 ; rts
```

**Writes nothing on the attacker** — no state 5, no move id, no positions. Then
(`0x2435A`) reaction handler = victim record[6] = rec3[6] = 0 -> `0x24852`: `bset #7,
(+0x26,A0)`, `bset #7,(+0x26,A1)`, `clr.w (+0x44,A1)`, `bsr 0x24D7A` (remap is a no-op:
facings differ). Then bookkeeping: `0x24DEC` attacker `clr +0x44,+0x46,+0x4C`,
**`+0x92 = A1`**; `0x24E02` victim `+0xC6++`, `clr +0x4C,+0x46,+0x18,+0x1A`,
`bclr #4,+0x33` (running flag consumed), **`+0x92 = A0`**, band 2 -> `bset #7,+0x64`.

The "0x13034 catch conversion" of the old spec is simply the catcher's handler noticing
`+0x92 != 0` next call (§3). There is no move-id rewrite on the attacker.

Ordering hazard (TODO EXACT PC 0x24062 slot order): the runner carries an *active*
record 3 (result 2 = `0x24468`) with box 9 against the catcher's hurtbox 9. If the runner's
slot is scanned first and box 9 (6 px) reaches, `0x24468`/`0x244AC` fires instead:
runner `+0x20=4,+0x64=0x18`, catcher `+0x20=4,+0x64=2,+0x9A=0x50` — a collision, not a
catch. Box 4 (0x30 px) reaches first by distance on an approach of >6 px/tick, so in
practice the catch wins on the frame the runner enters 0x30 px.

---

## 3. Move 0x06 handler `0x13034` — the catcher (full)

Cell `0x13020 = { 0x13034, mode 1, n 3, dur 0x20 0x18 0x20, spr 0xAF 0xB0 0x0000 }`
(ticks d+1 = 33, 25, 33).

```
13034  btst #7,(+0x1C)  bne 13088           ; committed -> per-frame part
-- first call --
1303C  clr.b +0x01                           ; mover OFF
13040  bclr #3,(+0x34)  bne 13058            ; +0x34 bit3 (re-entry flag, TODO EXACT writer)
13048  btst #7,(+0x60)  bne 1306A            ; move id 0x8006 (fresh "caught" entry, used by
                                            ;   the 0x7E fumble path with +0x60=0xC006)
13050  clr.l +0x92                           ; last_pair = 0 -> arms the test at 130A4
13054  rts
13058  bset #7,+0x1C ; clr.w +0x24 ; clr.w +0x22 ; rts      ; restart anim at frame 0
1306A  bset #7,+0x1C ; clr.w +0x24 ; +0x04 = 0x0060 ; +0x04.b = +0x2E ; +0x22 = 0x0C ; rts
                                            ; sprite 0x60|facing, 13 ticks
-- per frame --
13088  btst #7,(+0x60)  bne 130CC            ; already caught -> only the exit test
13092  +0x4C = 1                             ; standard hittable pose
13098  tst.b +0x25  bne 130BC                ; only during FRAME 0:
1309E    +0x4C = 4                           ;   record 4 ARMED (every tick of frame 0)
130A4    tst.l +0x92  beq 130BC              ;   pipeline set last_pair last frame?
130AA    clr.l +0x4C                         ;   yes: CAUGHT. +0x4C/+0x4E = 0
130AE    +0x22 = 0x0C                        ;   shorten frame 0 remainder to 13 ticks
130B4    bset #7,+0x60                       ;   move id -> 0x8006 (no more records)
130BA    rts
130BC  +0x25 == 0 && (byte +0x23) < 2 -> 130D4     ; WHIFF: end of frame 0 -> exit
       else
130CC  +0x25 == 0xFE -> 130D4 else rts       ; caught path: play frames 1,2 then exit
130D4  +0x20 = 0 ; bset #7,+0x26 ; clr.w +0x4C ; rts
```

Catcher write table (catch case), PC order:

| tick | PC | write |
|---|---|---|
| entry | 1303C/13050 | mover=0; last_pair=0 |
| frame 0 every tick | 1309E | atk=4 |
| frame 0, tick after the hit | 130AA/130AE/130B4 | atk=0 (long); count=0x0C; move_id\|=0x8000 |
| frame 2 end (+0x25==FE) | 130D4 | state=0; partner bit7 set; atk=0 |

Whiff: exits at end of frame 0 (sprite 0xAF, 33 ticks). Caught: 0xAF (13 more ticks),
0xB0 (25), sprite 0 (33), then state 0. **Facing never written.** No damage, no
launcher, no partner positions on the catcher side.

Alternative cat-4 answer, move 0x21 `0x15B24 = { 0x15B34, mode 1, n 2, dur 0x0E 0x18,
spr 0xB4 0xB5 }`: `+0x4C = 8` on frame 1 only, else 1; FE -> state 0, `bset #7,+0x26`,
`+0x04 = +0x2E` word. Record 8 result 0x0D = `0x245D6` -> logs class 0x0C -> falls into
`0x245F0` heavy: sounds 0x2B,0x32, dmg 0x10, victim state 4 react 2 (3 if running: `0x2461E`),
behind remap, +1 dmg if running (`0x24DDC`). That is the plain "knock the runner down".

---

## 4. Reaction 0x0E — `0x1AFD4[0x0E] = 0x1B460`, handler `0x1B478` (full)

Cell `0x1B460 = { 0x1B478, mode 1, n 4, dur 0x04 0x10 0x10 0xFF00, spr 0xB1 0xB1 0xB2
0xB3 }` (ticks 5, 17, 17, hold).

```
-- init (+0x1C bit7 clear) --
1B482  clr.w +0x44
1B486  A2 = +0x92                          ; last attacker = the catcher
1B48A  +0x26 = A2                          ; partner link (long)
1B48E  x = A2.x ; y = A2.y - 1 ; z = A2.z + 0x48      ; on the catcher's head
1B4AA  D0 = -0x10 ; if A2 faces right (bit7 +0x2E) D0 = +0x10 ; x += D0
                                           ; 0x10 px in FRONT of the catcher (his facing)
1B4BC  if $1C007F == 1 -> 1B516            ; (mode byte, TODO EXACT meaning)
1B4C8  if $1C0161 bit1 (tag/2-ring) -> 1B516
1B4D4  if own facing right:  D3 = -((y<<8 - 0x96000) / 0x2E0) ; if D3 >= x -> 1B516 else 1B532
       else:                 D3 =  ((y<<8 + 0x4B000) / 0x2E0) ; if D3 <  x -> 1B516 else 1B532
                                           ; perspective rope-edge test in the victim's
                                           ; forward direction: still inside -> normal arc
1B516  jsr 0x258E(4)                       ; LAUNCHER 4: vx=-0x180 (fwd), vz=+0x600, grav=0x38
1B520  jsr 0x10F9C(0x30)                   ; ring-band Y fix
1B52A  clr.b +0x01                         ; mover OFF again (0x258E set 2) -> held on the head
1B52E  rts
1B532  jsr 0x258E(0x0E)                    ; LAUNCHER 0x0E: vx=-0x300, vz=+0x780, grav=0x48
1B53C  clr.b +0x01
1B540  bset #2,+0x33                       ; "outside the ring" (over-the-top)
1B546  if $1C0161 bit0 (rumble): bset #4,+0x32 ; A2->+0xC4 += 1   ; elimination credit
1B55E  rts
-- per frame --
1B562  if +0x36 == 6 (crossed ropes): bset #0,+0x45 once -> sounds 0x28,0x32; clr.w +0x58; $1C1802 = 0x0E (shake)
1B592  if +0x36 == 5: jsr 0x11D56 (rope shake)
1B5A0  jsr 0x10FC6                         ; rope bump events
1B5A6  if +0x25 == 0 && +0x22 == 0: +0x01 = 2 ; rts     ; END OF FRAME 0 (tick 5): flight starts
1B5BC  if +0x25 != 3: rts                  ; frames 1,2: just fly
1B5C6  +0x42 = -0x20                       ; floor probe bias while waiting to land
1B5CC  if +0x37 bit4 clear (no floor):
         if +0x36 == 3 (out-of-ring floor): tag bit1 -> sound 0x28 only ; else -> 1B632
         else rts
       else (landed):
         if +0x33 bit2 && !tag bit1 -> 1B632
1B604    +0x20 = 4 ; +0x65.b = 5           ; REACTION 5 (bounce) — byte write, +0x64 high byte kept
1B610    +0x04 = 0x1F ; +0x04.b = +0x2E    ; sprite 0x1F|facing
1B61C    jsr 0x10BD0(-0x38, 0, 0)          ; step 0x38 FORWARD (own facing)
1B62A    jsr 0x1110E                       ; thud
         -> 1B666 clr.w +0x42 ; rts
1B632  +0x20 = 5 ; +0x60 = 0x68 ; clr.w +0x44 ; +0x04 = 0x16|facing ; 0x10BD0(+0x10,0,0) ; sound 0x28 ; clr.w +0x42
                                           ; landed outside: move 0x68 (cell 0x1981E, TODO EXACT)
```

Victim write table, PC order:

| tick | PC | write |
|---|---|---|
| hit frame | 2450C-24518 | state=4; react=0x0E; dmg.lo=0x0E |
| hit frame | 2485E/24E02 | grap44=0; atk=0; vx/vy (+0x18/+0x1A)=0; f33&=~bit4; last_pair=catcher |
| react init | 1B482-1B4B8 | grap44=0; partner=catcher; x=c.x±0x10(c facing); y=c.y-1; z=c.z+0x48 |
| react init | 25A2-25C0 (via 1B516) | vx=-0x180 (neg if own facing right), vy=0, vz=0x600, grav=0x38, mover=2 |
| react init | 1B52A | mover=0 (held on the head) |
| frame 0 end (tick 5) | 1B5B2 | mover=2 (flight) |
| frame 3, airborne | 1B5C6 | floor42=-0x20 |
| landing | 1B604-1B62A | state=4; react.lo=5; spr=0x1F\|facing; x += 0x38 fwd; floor42=0; thud |

Facing: **never written** on the victim in 0x244E0 or 0x1B478 — he keeps the facing he ran
in with, so "forward" (`vx<0`, negated when facing right) carries him THROUGH the catcher
to the other side. Landing is the normal reaction-5 bounce -> lying 8/9.

Launcher table `0x25CA` (6 bytes: vx, vz, grav; vx negated when facing right; mover=2):

```
[4]  FE80 0600 0038    backdrop-on-runner (this path)
[6]  FF00 0300 0038    reaction 0x13 backdrop (grapple move 0x30)
[E]  FD00 0780 0048    over-the-top-rope (rumble elimination arc)
```

---

## 5. Attacker-side move = move 0x06 itself

See §3. Handler PC `0x13034`, cells/durations above. No release writes, no launcher id, no
damage (damage comes from record 4 byte 4 = 0x0E via `0x24518`), no deltas, **no facing
change**. After the catch the runner is on the far side and the catcher still faces the
way he did; the *player* must turn. (The grapple backdrop 0x30/`0x16E48` is a different
move entirely — `grapple-moves.md §3c`, launcher [6], reaction 0x13 — and is not what the
anti-run press produces.)

---

## 6. Whip 0x17 direction — `0x12614[0x17] = 0x14E2C`, handler `0x14E40`

Cell `0x14E2C = { 0x14E40, mode 1, n 3, dur 8 0xA 0x10, spr 0xFE 0xFF 0x100 }`.
A2 = partner (victim) on entry.

```
-- init --
14E48  clr.b +0x01 (mover off)
14E4C  victim +0x20 = 0x00FF               ; frozen
14E52  0x10B62(-0x18)                       ; forward space probe
14E5A  victim x = own x ; victim y = own y  ; carried at zero offset
-- end of frame 1 (+0x24==1 && +0x22==0) --
14E78  victim +0x20 = 2                     ; RUN
14E7E  bset #7, victim +0x26                ; re-dispatch flag (link kept)
14E84  bset #7, own +0x34 ; bset #6, victim +0x34      ; whip markers
14E90  bclr #6, own +0x33 ; bclr #6, victim +0x33
14E9C  victim +0x44 = 2                     ; rope-bounce budget
14EA2  victim +0x2E.w = own +0x2E.w
14EA8  bchg #7, victim +0x2E               ; *** victim facing = OPPOSITE of attacker ***
14EAE  victim +0x2D = 0xC0                  ; angle low byte
14EB4  if victim faces right (bit7 set): victim +0x2D = 0x40
                                            ; i.e. attacker faces LEFT -> victim 0x40 (runs right)
                                            ;      attacker faces RIGHT -> victim 0xC0 (runs left)
14EC2  0x10B9A(0x50, -2, 0)                 ; victim pos = own pos + (facing-relative +0x50, -2, 0)
                                            ; +D0 is BEHIND (0x10B9A negates D0 when the
                                            ; ATTACKER faces right; -0x18 at 14E52 is "front")
-- frame 2 end (+0x25 == 0xFE) --
14EDC  own +0x20 = 0
14EE2  bchg #7, own +0x2E                   ; *** ATTACKER FLIPS to face the victim's run ***
14EE8  bset #7, own +0x26
```

Victim write table: `state=2; partner bit7; f34|=bit6; f33&=~bit6; grap44=2;
facing = attacker.facing ^ 0x8000; angle.lo = faces_right ? 0x40 : 0xC0;
x = a.x + (a.faces_right ? -0x50 : +0x50); y = a.y - 2; z = a.z`.
Attacker: `f34|=bit7; f33&=~bit6`; at FE: `state=0; facing ^= 0x8000; partner bit7`.

So the stock whip sends the opponent **behind the whipper, running away from the
whipper's back, facing opposite**, and the whipper turns around on the last frame to
watch him go. The engine's `handler_whip` (engine/anim.c:894) does the opposite on all
three counts: `v->facing = o->facing` (should be `^0x8000`), `v->x = o->x ∓ 0x20` in
front (should be `±0x50` behind), angle 0x40 when the *attacker* faces right (stock: 0x40
when the *victim* faces right, i.e. attacker faces left), and it never flips the
attacker at FE. That is exactly "attacker whips one way, opponent runs the other" as
reported — the sprite facing and the run direction disagree, and the whipper ends up
with his back to the returning runner, which also breaks §2's facing-differ test.

---

## C sketch

```c
/* hit.c result 3 — 0x244E0 */
case 3:
    if ((v->atk >> 8) || !(v->f33 & 0x10u)
        || !((a->facing ^ v->facing) & 0x8000u)) return VETO;   /* C=1: no hit bookkeeping */
    v->state = 4; v->react_id = 0x0E;
    v->dmg = (v->dmg & 0xFF00u) | arec[4];                        /* 0x0E */
    break;                     /* then reaction rec3[6]=0: partner-bit7 both, v->grap44=0 */
/* bookkeeping already does a->last_pair = v, v->last_pair = a, v->f33 &= ~0x10 */

/* anim.c move 0x06 — 0x13034 */
static uint32_t handler_catch(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (o->f34 & 0x08u) { o->f34 &= ~0x08u; o->anim_sel |= 0x8000u; o->frame = o->count = 0; }
        else if (o->move_id & 0x8000u) { o->anim_sel |= 0x8000u; o->frame = 0;
                                         o->spr = 0x60 | (o->facing & 0x8000u); o->count = 0x0C; }
        else o->last_pair = -1;
        return cell;
    }
    if (!(o->move_id & 0x8000u)) {
        o->atk = 1;
        if ((o->frame & 0xFF) == 0) {
            o->atk = 4;
            if (o->last_pair >= 0) {                /* caught (set by the pipeline) */
                o->atk = 0; o->count = 0x0C; o->move_id |= 0x8000u; return cell;
            }
            if ((o->count & 0xFF) < 2) goto done;   /* whiff */
        }
    }
    if ((o->frame & 0xFF) == 0xFE) { done: o->state = 0; o->partner = -1; o->atk = 0; }
    return cell;
}

/* anim.c reaction 0x0E — 0x1B478 (cell 0x1B460: 5/17/17/hold, spr B1 B1 B2 B3) */
static uint32_t handler_grabbed(eng_obj *o, uint32_t cell)
{
    eng_obj *c = &obj[o->last_pair];                       /* the catcher */
    if (!(o->anim_sel & 0x8000u)) {
        o->grap44 = 0; o->partner = o->last_pair;
        o->x = c->x + (((c->facing & 0x8000u) ? 0x10 : -0x10) << 16);
        o->y = c->y - (1 << 16); o->z = c->z + (0x48 << 16);
        int yi = o->y >> 16, xi = o->x >> 16, over;
        if (o->facing & 0x8000u) over = !(-(((yi << 8) - 0x96000) / 0x2E0) >= xi);
        else                     over = !((((yi << 8) + 0x4B000) / 0x2E0) < xi);
        if (mode7F == 1 || (mode161 & 2)) over = 0;
        knockback(o, over ? 0x0E : 4); edge_arc(o, 0x30);   /* 0x258E, 0x10F9C(0x30) */
        o->mover = 0;                                        /* held on the head */
        if (over) { o->f33 |= 0x04u; if (mode161 & 1) { o->f32 |= 0x10u; c->elims++; } }
        return cell;
    }
    rope_bump_events(o);                                     /* 0x10FC6 (+zone 6/5 sounds) */
    if (o->frame == 0 && o->count == 0) { o->mover = 2; return cell; }   /* tick 5: fly */
    if ((o->frame & 0xFF) != 3) return cell;
    o->floor42 = -0x20;
    if (o->landed) {
        if ((o->f33 & 0x04u) && !(mode161 & 2)) goto outside;
        o->state = 4; o->react_id = (o->react_id & 0xFF00u) | 5;
        o->spr = 0x1F | (o->facing & 0x8000u);
        add_pos_delta(o, -0x38, 0, 0); eng_sound(0x29); o->floor42 = 0;
    } else if (o->zone == 3 && !(mode161 & 2)) {
    outside: o->state = 5; o->move_id = 0x68; o->grap44 = 0;
        o->spr = 0x16 | (o->facing & 0x8000u); add_pos_delta(o, 0x10, 0, 0);
        eng_sound(0x28); o->floor42 = 0;
    }
    return cell;
}

/* anim.c whip 0x17 — fix, end of frame 1 */
v->state = 2; v->grap44 = 2;
v->facing = o->facing ^ 0x8000u;
v->angle  = (v->angle & 0xFF00u) | ((v->facing & 0x8000u) ? 0x40 : 0xC0);
v->x = o->x + (((o->facing & 0x8000u) ? -0x50 : 0x50) << 16);   /* BEHIND the whipper */
v->y = o->y - (2 << 16); v->z = o->z;
v->f34 |= 0x40u; o->f34 |= 0x80u; v->f33 &= ~0x40u; o->f33 &= ~0x40u;
/* FE: */ o->state = 0; o->facing ^= 0x8000u; o->partner = -1;
```

## Open items (TODO EXACT)

* `0x24062` slot order vs the runner's own active record 3 (§2 hazard).
* `$1C007F == 1` meaning at `0x1B4BC` (forces the in-ring arc).
* `+0x34` bit3 writer (restart path `0x13058`).
* Move 0x68 (`0x1981E`, outside landing) not transcribed.
* Reaction 0x0C `0x24D5E` (catcher hit while catching): only acts if victim `+0x65 == 0x0A`
  (-> state 4 react 2); otherwise nothing. Not on this path.
