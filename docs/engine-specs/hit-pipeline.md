# Hit-detection pipeline — exact semantics from `reference/maincpu.asm`

Read-only exploration, 2026-08-22. Sources: `../wrestlefest-decomp/reference/maincpu.asm`,
`src/aabb_2425a.c`, `data/romdata/hit_*.json`, `docs/native-rewrite-inventory/gameplay-tables.md` §3,
`docs/sound-commands.md`.

## 0. Frame ordering (who calls what)

Main per-frame loop (`0xFBE..`):

```
0x0FE8  jsr 0xF4C2      ; per-object move-handler pass; its epilogue (0xF510) jsr 0x24090
0x0FEE  jsr 0x24062     ; THE HIT SCAN (0x24062 = clr.w D0, falls into 0x24064)
0x0FF4  jsr 0x24E58     ; damage drain / stagger decay (all 9 slots)
```

So each frame: (a) move handlers run and re-write `+0x4C` for the current anim frame,
(b) `0x24090` clears `+0x8E`/`+0x4C` for any object whose `+0x04` changed,
(c) the scan runs, (d) pending damage `+0x68` is drained into HP `+0x66`.

Object iterator `0x250E`: `D0` = index; `if (D0 >= 9) { C=1; return; }`
`A0 = *(long*)(0x5122 + 4*D0); D0++;` skip slot unless byte `(0,A0)` bit7 set (slot live);
returns C=0 with A0 = object. The table at ROM `0x5122` is 9 wrestler slots
`0x1C05B0, 0x1C06BC, ..., 0x1C0E10` (stride 0x10C) — matches
`data/romdata/hit_object_slot_ptrs.json`. (A second iterator `0x2540` walks 2 slots at
`0x5146`: `0x1C0F1C, 0x1C1028` — referee/managers — never used by the hit scan.)

## 1. Outer loop `0x24062/0x24064`, victim iteration `0x24106`, legality `0x24126`

### Attacker scan (`0x24062`)

```
24062  clr.w D0
24064  jsr 0x250E            ; next live object -> A0
2406A  bcs -> rts (2408E)
2406C  tst.w (0x4C,A0)  beq 24064   ; attack word == 0 -> not attacking
24072  btst #7,(0x20,A0) beq 24064  ; state WORD bit15 clear -> skip (byte +0x20 is the
                                    ;   high byte of the state word; 0x8005-style states set it)
2407A  bsr 0x240BC           ; A6 = hit record;  A2 = A6 (2407E)
24080  btst #7,(0,A2)   beq 24064   ; record flags bit7 clear -> record has no hitbox
24088  bsr 0x240D8           ; scan victims for this attacker
2408C  bra 24064
```

Record fetch `0x240BC`: `A6 = 0x24EF6 + ((word)(0x4C,A0) & 0xFF) * 7`.
**The attack id is the LOW byte (`+0x4D`) of the word at `+0x4C`; the high byte carries
modifier flags** (moves write `#$8014..$801F`, `#$401A` forms — those bits are tested by the
result handlers, see §3). 34 records = `data/romdata/hit_record.json` (7 bytes:
flags, victim_box1, victim_box2, attacker_box, damage, result_handler, reaction_handler —
the json's `pad` field at `+4` is really the **damage byte**, see `0x24424`).

### Victim iteration `0x24106` (called from wrapper `0x240D8`)

`0x240D8`: `clr.w D1`, then loop:

```
240DE  bsr 0x24106   ; next victim -> A1     (bcs -> done, restore, rts)
240E4  bsr 0x24126   ; legality              (bcs -> next victim)
240EA  bsr 0x241E8   ; box test              (bcs -> next victim)
240F0  bsr 0x2435A   ; result + reaction     (bcs -> next victim, NO bookkeeping)
240F6  bsr 0x24DEC   ; attacker bookkeeping
240FA  bsr 0x24E02   ; victim bookkeeping
240FE  bra 240DE     ; keep scanning further victims in the same swing/frame
```

`0x24106` itself: `exg D0,D1 / exg A0,A1 / jsr 0x250E / exg back`; if the returned object
`A1 == A0` (self) it loops for the next one; C=1 when the 9 slots are exhausted.
So D0/A0 stay the attacker iterator, D1/A1 the victim iterator.

### Legality `0x24126` — victim is hittable iff ALL of:

| PC | test | reject when |
|---|---|---|
| `2412A` | `tst.w (0x4C,A1)` | victim's attack word == 0 (victim must itself carry a record — it supplies the hurtboxes and the reaction handler) |
| `24132` | `btst #7,(0x20,A1)` | state word bit15 clear (freshly-hit victims are in state 4 → un-hittable until re-flagged) |
| `2413C` | `btst #6,(0x32,A1)` | **set** → invulnerable |
| `24146` | `tst.w (0xFE,A1)` | nonzero → latched (pin/grapple lock; set elsewhere as `0x8000/0x8001`, e.g. `0x11236..0x11250`) |
| `2414C` | `btst #2,(0x32,A1)` | if set, additionally require global byte `0x1C0161` bit1 **set** (`24154`), else reject |
| `24160` | `cmpi.w #-1,(0x4,A1)` | anim word == 0xFFFF |
| `2416A` | `bsr 0x241B6` | per-pair already-hit mask (below) |
| `24172..241A2` | reach prefilter | see below |

**Per-pair mask check `0x241B6`:** scan `A3 = 0x5122`; `D0 = 0x8000`, `D1 = 9`
(`dbra` → up to **10** compares; the 10th entry read is `0x5146`'s first long `0x1C0F1C`,
harmless since victims only ever come from the 9-slot iterator). On `cmpa.l (A3)+,A1` match:
bit = `0x8000 >> slot_index`; **reject if `(0x8E,A0) & bit`** (this attacker already hit this
victim during this swing). If A1 is not found in the table at all → reject (`241D0 → 241DE`).

**Reach prefilter (exact):**

```
24172  D0 = (word)(0x0A,A0)          ; attacker ring-depth
24176  D1 = (word)(0x0A,A1)          ; victim ring-depth
2417A  if ((byte)(0x4D,A1) == 0x14)  ; victim's attack id 0x14 (a lying pose record)
24182      D1 -= 8                   ; subq.w #8
24184  D1 -= D0; if borrow, D1 = -D1 ; |Δdepth|  (16-bit)
2418A  if (D1 >= 0x0C) reject       ; cmpi.w #$C / bcc  -> must be |Δdepth| < 12
24190  D0 = (word)(0x06,A0); D1 = (word)(0x06,A1)   ; X positions
24198  D1 -= D0; if borrow, D1 = -D1
2419E  if (D1 >= 0x50) reject       ; must be |Δx| < 0x50
```

All compares unsigned 16-bit after the absolute-value fold.

## 2. Box selection `0x241E8` and the AABB `0x2425A`

`0x241E8` (saves D0/A4-A5; **A2 = attacker record from the outer loop, A3 set here**):

```
241EC  exg A0,A1 ; bsr 0x240BC ; A3 = A6 ; exg back
       ; A3 = VICTIM's hit record (index = victim's +0x4D). The victim's own record
       ; supplies the hurtboxes AND (later) the reaction handler index.
241F6  A4 = 0x24FE4 + (u8)A2[3] * 4        ; attacker hitbox (hit_attacker_boxes.json)
2420A  if (!(A3[0] & 0x40)) -> MISS        ; victim record must enable box1 (bit6)
24212  A5 = 0x25010 + (u8)A3[1] * 4        ; victim hurtbox 1 (hit_victim_boxes.json)
24226  bsr 0x2425A ; if C clear -> HIT
2422A  else if (A3[0] & 0x20) {            ; bit5: second hurtbox
24232      A5 = 0x25010 + (u8)A3[2] * 4
24246      bsr 0x2425A ; C clear -> HIT else MISS
       } else MISS
```

So: attacker box index = **attacker record `+3`**; victim boxes = **victim record `+1`
(gated by victim flags bit6 — mandatory for any hit) then `+2` (bit5, only tried after box1
misses)**. Records with flags `0x80` (attacker-only, e.g. ids 15,17,22..25,28) can strike but
can never be struck through this path (bit6 clear ⇒ instant miss as victim).

### AABB `0x2425A` = X test `0x24264`, then (only if X overlaps) Y test `0x242DE`

`src/aabb_2425a.c` is the verified C form (cycle-charging wrapper); the math:

* Box bytes are **signed** `x0 y0 x1 y1` (byte 0/2 = X pair, byte 1/3 = Y pair).
* **X mirror**: each side independently — if byte `(0x04,obj)` bit7 (high byte of the anim
  word = current frame's h-flip) is set: `x0 = -x0; x1 = -x1; swap(x0,x1)` (`24270..2427C`
  attacker, `24292..2429E` victim). This is the **render flip**, not the logical facing byte
  `+0x2E` (which the handlers use, §3).
* Extend to 16-bit signed, add the object's **X position word `+0x06`**:
  `d0 = x0+ax, d1 = x1+ax` (attacker), `d2,d3` (victim).
* **Y test uses the height/altitude word `+0x0E`** — not the ring-depth `+0x0A` used by the
  prefilter (`+0x16` screen-Y is computed elsewhere as `+0x0A + +0x0E − camera`, `0x24B0`).
  No mirror on Y: `d0 = y0+(0x0E,A0)` etc.
* Overlap test (identical for both axes, **unsigned 16-bit compares** — sign only survives
  via wraparound, fine for on-screen coordinates):

```
overlap = (d2 >= d0 && d1 >= d2)   /* 242AC: victim lo inside attacker  */
       || (d3 >= d0 && d1 >= d3)   /* 242B4: victim hi inside attacker  */
       || (d0 >= d2 && d3 >= d0)   /* 242BC: attacker lo inside victim  */
       || (d1 >= d2 && d3 >= d1);  /* 242C4: attacker hi inside victim  */
```

  Hit ⇒ CCR C cleared (`andi #$EE,CCR`), miss ⇒ C set (`ori #1,CCR`).
* `0x2425A`: `bsr 24264; bcs rts; bsr 242DE; rts` — Y result is the final result.

## 3. Handlers `0x2435A` and the basic-punch pair

```
2435A  movem.l D0-D1/A0-A4,-(A7)
2435E  A4 = *(long*)(0x24398 + (u8)A2[5]*4)   ; RESULT handler <- ATTACKER record +5
24372  jsr (A4)
24374  bcs 24392                              ; result handler may VETO the hit (C set):
                                              ;   reaction + bookkeeping are all skipped
24378  A4 = *(long*)(0x243D4 + (u8)A3[6]*4)   ; REACTION handler <- VICTIM record +6
2438C  jsr (A4)
2438E  C = 0 ; restore ; rts
```

Result table `0x24398` (index → PC): 0=`0x2484C`, 1=`0x24408`, 2=`0x24468`, 3=`0x244E0`,
4=`0x2452A`, 5=`0x245F0`, 6=`0x24640`, 7=`0x2468A`, 8=`0x246EA`, 9=`0x24732`, 10=`0x24784`,
11=`0x247EC`, 12=`0x245BC`, (13=`0x245D6`, 14=`0x24784` — used by records 4 and 27).
Reaction table `0x243D4`: 0=`0x24852`, 1=`0x24868`, 2=`0x24924`, 3=`0x24984`, 4=`0x24A38`,
5=`0x24A90`, 6=`0x24AA8`, 7=`0x24B20`, 8=`0x24BA4`, 9=`0x24BC2`, 10=`0x24C42`, 11=`0x24CCA`,
12=`0x24D5E`.

### Which ids make up a "basic punch" hit

Record **1** (`flags 0x40, victim_box1 0, result 0, reaction 0`) is the **standing/neutral
victim pose**: it cannot attack (flags bit7 clear fails the `0x24080` gate) — it exists to
give an idle wrestler hurtbox 0 (x −16..32, y 0..96) and **reaction handler 0 =
`0x24852`**. Move handlers write `move.w #1,(0x4C,A0)` at dozens of sites (e.g. `0x1155A`,
`0x116E2`, `0x13092`...) to mean "hittable, standard pose".

The strike side of a jab/punch is a `0xC0/0x80` record whose result handler is **1
(`0x24408`, the generic strike handler)** — records **7** and **25** (`flags 0xC0/0x80,
attacker_box 6 = x −64..−1, y 0..96, damage 1, result 1, reaction 0`). Example writer
`0x1289E`: the move keeps `+0x4C = 0x21` (windup pose, victim-only record) and switches to
`+0x4C = 7` only when the anim frame counter `+0x24 == 3` (the active frame).

So a basic punch landing on a standing opponent runs **result `0x24408` (attacker record[5]=1)
then reaction `0x24852` (victim record[6]=0)**. ("Attack id 1" in the task brief is the
victim-side record of that pair.)

### Result handler 1 — generic strike `0x24408` (exact)

```
24408  btst #6,(0x4C,A1)   bne -> C=1, rts    ; victim attack-word bit14 ($40xx forms):
                                              ;   strike-immune, hit fully vetoed
24410  D0=0x2A jsr 0x2052                     ; sound: punch impact  (0x2052 = sound_write,
2441A  D0=0x32 jsr 0x2052                     ;   posts D0|0x3100 to latch 0x14000C)
                                              ; sound: crowd hit bed
24424  (byte)(0x69,A1) = A2[4]                ; DAMAGE: record byte +4 -> low byte of the
                                              ;   victim's pending-damage word +0x68
2442A  (word)(0x20,A1) = 4                    ; victim state = 4 (hit-reaction)
24430  (word)(0x64,A1) = 0                    ; reaction id = 0 (light flinch)
24436  btst #7,(0x4C,A1)   bne -> success     ; victim bit15 ($80xx forms): take the hit
                                              ;   but skip the escalation below
2443E  if (btst #4,(0x33,A1) != 0            ; "wounded/dazed" flag
24446      || (word)(0x52,A1) >= 3)          ; stagger count >= 3
2444E      (word)(0x64,A1) = 2               ; escalate reaction: knockdown
24454  bsr 0x24D7A                            ; behind-hit pose remap (below)
24458  bsr 0x24DDC                            ; +1 damage if victim +0x33 bit4
2445C  C=0, rts
```

There is **no knockback velocity write and no facing write in the basic-punch result
handler** — state 4 + reaction id `+0x64` drive the flinch through the HIT state machine
(`0xF18A` family); heavier handlers (e.g. `0x24468` clothesline-trade) do write extras like
`+0x9A = 0x50`. `0x24408` never touches `+0xFE` either — the latch is only *tested* by the
pipeline; it is set by the grapple/pin code (`0x11236..0x11250`).

**Behind-hit remap `0x24D7A`:** if victim's `+0x65` (low byte of the reaction word) `< 0x24`:
`D0 = ((0x2E,A0) >> 7) & 1` (attacker logical facing), `D1 = ((0x2E,A1) >> 7) & 1`;
if `D0 == D1` (both facing the same way ⇒ the blow lands on the victim's back), remap
`+0x65` through the 0x24-byte table `0x24DB8`
(`0B 0C 0B 0C 0D 05 06 07 08 09 0A 0B 0C 0D 0E 0F` then `0B`×18, `0C 0D`): front-flinch
variants 0..4 become back-flinch 0x0B..0x0D; 5..0x0F unchanged; 0x10..0x21 collapse to 0x0B.
This is where the **`+0x2E` facing byte** matters, vs the `+0x04` bit7 render flip used for
box mirroring. (Also called a second time from reaction `0x24852`; idempotent because the
table is stable on its own range.)

**Bonus damage `0x24DDC`:** `if (btst #4,(0x33,A1)) (word)(0x68,A1) += 1;`

### Reaction handler 0 — standing victim `0x24852` (exact)

```
24852  bset #7,(0x26,A0)     ; attacker: request anim/state re-dispatch
24858  bset #7,(0x26,A1)     ; victim: same
2485E  clr.w (0x44,A1)       ; kill victim's motion/script timer
24862  bsr 0x24D7A           ; behind-hit remap again
24866  rts
```

(`+0x26` doubles as the grapple-partner pointer long in hold states — reaction 1 `0x24868`
reads `movea.l (0x26,A1),A2` — but bit7 of its first byte is the ubiquitous
"refresh me" flag.)

### Damage application `0x24E58` / `0x24E72`, bands `0x24EC2`

`0x24E58` (called right after the scan, every frame, over all 9 slots via `0x250E`):

```
for each live slot A0:
  24E66  if ((word)(0x68,A0) != 0            ; pending damage
  24E6C      && (word)(0xFE,A0) == 0) {      ; and not latched
    24E72  D1 = (word)(0x66,A0) - (word)(0x68,A0)   ; HP -= damage
    24E7A  if (borrow) D1 = 0                        ; floor at 0
    24E7E  (word)(0x66,A0) = D1
    24E82  bsr 0x24EC2                               ; band update
    24E84  (word)(0x6A,A0) = -(word)(0x68,A0)        ; signed HP-delta for the HUD gauge
    24E8E  (word)(0x68,A0) = 0
  }
  24E92  if (--(word)(0x54,A0) borrows)              ; stagger decay timer
  24E98      { (0x54,A0)=0; (0x52,A0)=0; }           ; stagger count resets
  24EA0  if (btst #3,(0x32,A0) && (word)(0xD0,A0))   ; timed status flag
  24EAE      if (--(word)(0xD0,A0) == 0) bclr #3,(0x32,A0)
```

Band update `0x24EC2` (the "HP band" `+0x70` that drives dazed/critical behaviour):

```
D2 = (word)(0x72,A0)          ; max HP; if 0 -> return
D2 /= 3
if (HP <= 0x18)        band = 2   ; critical (fixed 24-point floor)
else if (HP <= 2*D2)   band = 1   ; below two-thirds
else                   band = 0
(word)(0x70,A0) = band
```

Victim bookkeeping (§4) turns band 2 into `bset #7,(0x64,A1)` — the critical-flavour
reaction bit.

## 4. `+0x8E` mask and the bookkeeping pair `0x24DEC` / `0x24E02`

**Set** (`0x24334`, called first thing by `0x24DEC`): same 10-iteration scan of `0x5122` as
`0x241B6`; on match `or.w (0x8000>>idx), (0x8E,A0)` — marks "attacker A0 has hit victim
slot idx". Not found ⇒ no-op.

**Attacker bookkeeping `0x24DEC`:**

```
24DEC  bsr 0x24334           ; set per-pair bit in (0x8E,A0)
24DF0  clr.w (0x44,A0)
24DF4  clr.w (0x46,A0)
24DF8  clr.w (0x4C,A0)       ; attack word cleared ON HIT
24DFC  (long)(0x92,A0) = A1  ; remember last victim
```

**Victim bookkeeping `0x24E02`:**

```
24E06  if (btst #7,(0x74,A1)) {              ; victim was holding someone
24E0E    clr.w (0x74,A1)
24E12    A2 = (long)(0x76,A1); (word)(0x1C,A2) = 2   ; release the held partner
24E1C    clr.l (0x76,A1); clr.w (0x44,A1)
       }
24E24  (word)(0xC6,A1) += 1                  ; hits-taken counter
24E2A  clr.w (0x4C,A1)                       ; victim's own attack cancelled
24E2E  clr.w (0x46,A1)
24E32  clr.w (0x18,A1); clr.w (0x1A,A1)      ; velocities zeroed
24E3A  bclr #4,(0x33,A1)                     ; consume the "wounded" flag
24E40  (long)(0x92,A1) = A0                  ; remember last attacker
24E44  if ((word)(0x70,A1) == 2)             ; critical band
24E4C      bset #7,(0x64,A1)                 ; critical-reaction bit
```

**Re-hit prevention, precisely:**
1. Within one frame, one attacker can hit several victims (the loop continues after
   bookkeeping using the cached record A2) but each victim only once — the mask bit set by
   `0x24334` fails the `0x241B6` legality check (belt-and-braces: `+0x4C` was also cleared,
   but the move handler re-writes it next frame while the anim continues).
2. Across the frames of one swing: the move handler re-arms `+0x4C` every frame, but the
   `+0x8E` bit persists, so the same victim can't be hit twice by one animation.
3. **The mask (and `+0x4C`) clear when the animation changes**: maintenance loop `0x24090`
   (called per frame from `0xF510`, before the scan) walks all 9 slots and, for any object
   whose anim word `+0x04` differs from its cache `+0x90`, does
   `clr.w (0x8E,A0); clr.w (0x4C,A0); (0x90,A0) = new +0x04`. A new swing = new anim word =
   fresh mask. Additionally the victim's own hit cancels its attack (`24E2A`), and a landed
   hit disarms the attacker until the next active frame (`24DF8`).

## 5. C sketch

New/confirmed object fields (byte offsets into the 0x10C-byte slot):

| offset | name | notes |
|---|---|---|
| +0x00 | `hdr` | bit7 = slot live (`0x250E`) |
| +0x04 | `anim` (u16) | high-byte bit7 = h-flip (box mirror); 0xFFFF = no-anim |
| +0x06 | `x` (s16) | X position |
| +0x0A | `depth` (s16) | ring depth — prefilter axis |
| +0x0E | `alt` (s16) | height — AABB Y axis |
| +0x18/+0x1A | `vel_x/vel_d` | zeroed on being hit |
| +0x20 | `state` (u16) | bit15 = in-move/hittable; 4 = hit-reaction |
| +0x26 | `link` (u32) | partner ptr in holds; byte0 bit7 = refresh request |
| +0x2E | `facing` (u8) | bit7 = logical facing |
| +0x32 | `flags32` | b6 invuln, b3 timed-status, b2 needs-global-bit1 |
| +0x33 | `flags33` | b4 wounded (+1 dmg, forces reaction 2) |
| +0x44/+0x46 | timers | cleared on hit |
| +0x4C | `attack` (u16) | low byte = record id (0..0x21); b15/b14 = modifier flags |
| +0x52/+0x54 | `stagger`, `stagger_t` | ≥3 escalates reaction; decays in drain |
| +0x64 | `reaction` (u16) | 0 flinch / 2 knockdown / 0xE ...; b15 critical; low byte remapped by 0x24DB8 |
| +0x66 | `hp` (u16) | |
| +0x68 | `pend_dmg` (u16) | low byte written from record[4] |
| +0x6A | `hp_delta` (s16) | −damage, HUD gauge |
| +0x70 | `band` | 0/1/2 from 0x24EC2 |
| +0x72 | `hp_max` (u16) | |
| +0x74/+0x76 | hold flag / held-obj ptr | released on hit |
| +0x8E | `hitmask` (u16) | bit `0x8000>>slot` per victim |
| +0x90 | `anim_cache` (u16) | for 0x24090 |
| +0x92 | `last_pair` (u32) | last victim (attacker) / last attacker (victim) |
| +0xC6 | `hits_taken` (u16) | |
| +0xD0 | status timer | with flags32 b3 |
| +0xFE | `latch` (u16) | 0x8000/0x8001 = pin/grapple lock; blocks hit + drain |

```c
/* tables straight from data/romdata */
typedef struct { u8 flags, vbox1, vbox2, abox, dmg, result, reaction; } hit_rec_t;   /* 0x24EF6, 34 */
extern const hit_rec_t hit_rec[34];
extern const s8 atk_box[11][4], vic_box[11][4];      /* 0x24FE4, 0x25010: x0 y0 x1 y1 */
extern obj_t *slot[9];                               /* 0x5122 */

static int slot_index(const obj_t *o) { for (int i=0;i<9;i++) if (slot[i]==o) return i; return -1; }

static int axis_overlap(s16 a0, s16 a1, s16 b0, s16 b1)   /* 0x24264/0x242DE core, unsigned cmps */
{
    u16 d0=a0, d1=a1, d2=b0, d3=b1;
    return (d2>=d0 && d1>=d2) || (d3>=d0 && d1>=d3) || (d0>=d2 && d3>=d0) || (d1>=d2 && d3>=d1);
}

static int box_test(const obj_t *a, const obj_t *v, const s8 *ab, const s8 *vb)   /* 0x2425A */
{
    s16 ax0=ab[0], ax1=ab[2], vx0=vb[0], vx1=vb[2];
    if (a->anim & 0x8000) { s16 t=ax0; ax0=(s8)-ab[2]; ax1=(s8)-t; }   /* +0x04 b15 mirror */
    if (v->anim & 0x8000) { s16 t=vx0; vx0=(s8)-vb[2]; vx1=(s8)-t; }
    if (!axis_overlap(a->x+ax0, a->x+ax1, v->x+vx0, v->x+vx1)) return 0;
    return axis_overlap(a->alt+ab[1], a->alt+ab[3], v->alt+vb[1], v->alt+vb[3]); /* +0x0E, no mirror */
}

static int victim_legal(const obj_t *a, const obj_t *v, wf_state_t *g)   /* 0x24126 */
{
    if (!v->attack || !(v->state & 0x8000)) return 0;
    if (v->flags32 & 0x40) return 0;
    if (v->latch) return 0;
    if ((v->flags32 & 0x04) && !(g->flag_1C0161 & 0x02)) return 0;
    if (v->anim == 0xFFFF) return 0;
    int i = slot_index(v);
    if (i < 0 || (a->hitmask & (0x8000u >> i))) return 0;              /* 0x241B6 */
    u16 dz = v->depth - (( (v->attack & 0xFF)==0x14) ? 8:0) - a->depth;
    if ((s16)dz < 0) dz = -dz;
    if (dz >= 0x0C) return 0;
    u16 dx = v->x - a->x; if ((s16)dx < 0) dx = -dx;
    return dx < 0x50;
}

void wf_hit_scan(wf_state_t *g)                     /* 0x24062 */
{
    for (int i=0;i<9;i++) {
        obj_t *a = slot[i];
        if (!a || !(a->hdr&0x80) || !a->attack || !(a->state & 0x8000)) continue;
        const hit_rec_t *ar = &hit_rec[a->attack & 0xFF];              /* 0x240BC */
        if (!(ar->flags & 0x80)) continue;                             /* 0x24080 */
        for (int j=0;j<9;j++) {                                        /* 0x240D8/0x24106 */
            obj_t *v = slot[j];
            if (!v || v==a || !(v->hdr&0x80)) continue;
            if (!victim_legal(a,v,g)) continue;
            const hit_rec_t *vr = &hit_rec[v->attack & 0xFF];          /* 0x241E8: A3 */
            if (!(vr->flags & 0x40)) continue;                         /* box1 mandatory */
            int hit = box_test(a,v, atk_box[ar->abox], vic_box[vr->vbox1]);
            if (!hit && (vr->flags & 0x20))
                hit = box_test(a,v, atk_box[ar->abox], vic_box[vr->vbox2]);
            if (!hit) continue;
            if (!hit_result[ar->result](a,v,ar,g)) continue;           /* 0x2435A; veto -> no bookkeeping */
            hit_reaction[vr->reaction](a,v,vr,g);
            /* attacker bookkeeping 0x24DEC */
            a->hitmask |= 0x8000u >> j;  a->t44 = a->t46 = 0;  a->attack = 0;  a->last_pair = v;
            /* victim bookkeeping 0x24E02 */
            if (v->hold_flag & 0x8000) { v->hold_flag=0; if (v->held){v->held->f1C=2; v->held=0;} v->t44=0; }
            v->hits_taken++; v->attack=0; v->t46=0; v->vel_x=v->vel_d=0;
            v->flags33 &= ~0x10; v->last_pair = a;
            if (v->band == 2) v->reaction |= 0x8000;
        }
    }
}

/* result handler 1 — basic strike (0x24408) */
static int res_strike(obj_t *a, obj_t *v, const hit_rec_t *r, wf_state_t *g)
{
    if (v->attack & 0x4000) return 0;                    /* strike-immune, veto */
    wf_sound(0x2A); wf_sound(0x32);                      /* 0x2052 */
    v->pend_dmg = (v->pend_dmg & 0xFF00) | r->dmg;       /* byte write to +0x69 */
    v->state = 4;  v->reaction = 0;
    if (!(v->attack & 0x8000)) {
        if ((v->flags33 & 0x10) || v->stagger >= 3) v->reaction = 2;
        remap_behind(a,v);                               /* 0x24D7A */
        if (v->flags33 & 0x10) v->pend_dmg++;            /* 0x24DDC */
    }
    return 1;
}

/* reaction handler 0 — standing victim (0x24852) */
static void rea_standing(obj_t *a, obj_t *v, const hit_rec_t *r, wf_state_t *g)
{
    a->link_flags |= 0x80;  v->link_flags |= 0x80;       /* bset #7,+0x26 */
    v->t44 = 0;
    remap_behind(a,v);
}

static void remap_behind(const obj_t *a, obj_t *v)       /* 0x24D7A + table 0x24DB8 */
{
    static const u8 tbl[0x24] = {0x0B,0x0C,0x0B,0x0C,0x0D,5,6,7,8,9,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
        0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0C,0x0D};
    u8 lo = v->reaction & 0xFF;
    if (lo < 0x24 && ((a->facing ^ v->facing) & 0x80) == 0)
        v->reaction = (v->reaction & 0xFF00) | tbl[lo];
}

void wf_damage_drain(void)                               /* 0x24E58, after the scan each frame */
{
    for (int i=0;i<9;i++) {
        obj_t *o = slot[i];
        if (!o || !(o->hdr&0x80)) continue;
        if (o->pend_dmg && !o->latch) {
            o->hp = (o->hp >= o->pend_dmg) ? o->hp - o->pend_dmg : 0;  /* 0x24E72 */
            band_update(o);                                            /* 0x24EC2 */
            o->hp_delta = -(s16)o->pend_dmg;  o->pend_dmg = 0;
        }
        if (o->stagger_t-- == 0) { o->stagger_t = 0; o->stagger = 0; } /* u16 borrow form */
        if ((o->flags32 & 0x08) && o->status_t && --o->status_t == 0) o->flags32 &= ~0x08;
    }
}

static void band_update(obj_t *o)                        /* 0x24EC2 */
{
    if (!o->hp_max) return;
    u16 third = o->hp_max / 3;
    o->band = (o->hp <= 0x18) ? 2 : (o->hp <= (u16)(third*2)) ? 1 : 0;
}

void wf_anim_change_maint(void)                          /* 0x24090, run before wf_hit_scan */
{
    for (int i=0;i<9;i++) {
        obj_t *o = slot[i];
        if (!o || !(o->hdr&0x80)) continue;
        if (o->anim != o->anim_cache) { o->hitmask = 0; o->attack = 0; o->anim_cache = o->anim; }
    }
}
```

Caveats / open items: the exact `+0x4C` value of the player's default punch move was traced
to the pattern "windup id (0x40-flag record) → active-frame id with result handler 1"
(`0x1289E/0x128AC` shows 0x21→7); other strike moves use ids 25/28 etc. — a per-move table
of `+0x4C` writers exists in the audit (`+0x4C` writers row, 44 sites) if the engine needs
them enumerated. `+0x52` (stagger) is incremented outside this pipeline (HIT state machine),
only consumed and decayed here.
