# Run / skid / turn — exact semantics (state 2 / 3 / 6)

Source: `../wrestlefest-decomp/reference/maincpu.asm` (read-only pass, 2026-08-22).
Companion to `docs/engine-specs/input-walk.md` (field conventions reused verbatim) and
`docs/engine-specs/anim.md` (cell/handler contract, `+0x18`/`+0x24` semantics).
Cross-checked against `src/floor_2818e.c` for the `+0x36`/`+0x37` codes.

State→cell table `0x11478` (state pass of `0x1C03E`): `[0]=0x114AC` stand,
`[1]=0x115F2` walk, `[2]=0x11C62` **run**, `[3]=0x11D82` **skid**, `[4]=[5]=0x11DCA`
(handler `0x12612`, mode 0), `[6]=0x11DD0` **turn**, `[7]=0x11E36`, …

Pass-2 (human input) table `0xF1E4`: `[2]=0xF34C` run steer+reversal, `[3]=0xF45E` **rts**,
`[6]=0xF4A4` **rts** — skid and turn have no input handler; all their logic lives in the
anim-cell handlers below, which run for humans *and* CPUs.

A correction to the task premise: the reversal detector `0xF42E` never sees a *walking*
object — it is the tail of the **state-2 (run)** pass-2 handler. Walk (state 1) dispatches
through `0xF244`, which never reaches `0xF42E`.

---

## 1. Reversal detector `0xF42E-0xF45A` (inside `0xF34C`, state 2 only)

Runs on the **committed** branch of `0xF34C`: `0xF34C` opens with
`bset #6,(+0x20,A0)` — that is bit6 of the *high byte*, i.e. **bit14 of the state word**,
a "steering initialized" latch inside state 2. First tick (bit14 was clear) does the
direction pick (§2b) and returns; every later tick goes to `0xF3C6` and flows into the
detector. So it runs every frame of an established run, for a human object in state 2
(the CPU pass `0x1C150` never calls `0xF34C`).

Order of the three skid triggers in the committed branch:

```
0xF3C6  btst #1,(+0x34)          ; opponent requested a grapple
0xF3CE    && +0x44 == 0 (tst.w)  ; and no rope/whip budget pending
0xF3D4    -> move.w #3,(+0x20)   ; SKID, done
0xF3DE  move.b $A3.w,D1; andi.b #3  ; *** ANOMALY: absolute ROM read of 0x0000A3
                                 ; (=0xFF, vector filler), NOT (+0xA3,A0).
                                 ; Constant-true gate — keep it constant-true.
0xF3E8  btst #0,$1C0161  == 0 -> 0xF42E   ; mode flag (bit0, armed by LEFT at title)
0xF3F2  +0x45 != 1       -> 0xF42E        ; +0x45 = LOW BYTE of timer word +0x44
0xF3FA  A2 = (+0x7A) opponent; |own_x(+0x06) - opp_x| >= 0x60 -> 0xF42E
0xF410  bset #0,(+0x46) ; already set -> 0xF42E   ; once-per-run latch (byte +0x46)
0xF418  D1 = 3; jsr 0x1129E ; bcc -> 0xF42E       ; chance roll, see below
0xF426  -> move.w #3,(+0x20)   ; SKID (random stumble near the opponent), done

0xF42E  tst.w (+0x44) ; bne rts          ; timer must be 0 — a whip/rope budget
                                         ; suppresses reversal entirely
0xF434  btst #7,(+0x2D)                  ; run angle byte: 0xC0(left) bit7=1,
                                         ;                 0x40(right) bit7=0
  set (running LEFT):
0xF43C    btst #0,(+0xA9) ; beq rts      ; RIGHT held  (bit0 = R, HELD level —
                                         ; 0x514E stores the level nibble, no edge)
0xF444    move.w #3,(+0x20)              ; SKID
0xF44A    beq $f45c                      ; DEAD branch: move.w #3 cleared Z, never
                                         ; taken — control FALLS THROUGH to 0xF44C
  clear (running RIGHT), and fallthrough from above:
0xF44C    btst #1,(+0xA9) ; beq rts      ; LEFT held (bit1 = L)
0xF454    move.w #3,(+0x20)              ; SKID (idempotent re-write on fallthrough)
0xF45A  nop
0xF45C  rts
```

**Fields compared:** direction = `+0x2D` bit7 (run angle byte, 0x40 right / 0xC0 left)
against the **held** stick bits `+0xA9` bit0 (RIGHT) / bit1 (LEFT). These are levels,
not edges — in a match `0x514E` writes the raw inverted nibble every frame
(input-walk.md §1); the "edge" naming in older notes is wrong for the match path.

**The write:** exactly `move.w #3,(+0x20)` — nothing else. No substate (`+0xAE`
untouched), no speed/angle/mover change. Bit15-clear write ⇒ next frame's commit
(`0xF4E0`) re-latches `+0x1C=3` and the anim core swaps in skid cell `0x11D82`.
Because `+0x01` (mover=polar), `+0x2A/+0x2B` (run speed) and `+0x2D` (angle) all
carry over from the run, the object *slides on* in its old direction — the skid
handler then decays that speed (§3).

The `0x1129E` roll, case `D1=3` (`0x11368`): `A1 = 0x1140F` (3-byte chance table),
`D2 = word +0x70` (grapple-meter/heat counter), `rand16()&0xF <= A1[D2]` ⇒ carry ⇒
stumble. (`0x1129E` is a 4-case chance dispatcher `0x112B0[D1]`; `0x21B4` = RNG.)

---

## 2. Entering RUN, and what ends it

### 2a. Entry — both attack buttons, NOT a double-tap

There is no direction double-tap anywhere on the human path. The trigger is the
**B1+B2 new-press pair**:

* `0xDCDE` (controller prefix, pass 2) → attack selector `0xDE86`. At `0xDF96`:
  `A2 = 0xE4FE[wrestler id +0x02]` (per-wrestler attack table),
  `D0 = context_category*3 + ((+0xA3 & 3) - 1)` — column 0 = B1, 1 = B2, 2 = both
  (`+0xA3` = new-press button byte; the `+0xA4` odd/even accumulator makes a
  1-frame-apart pair register together).
  `0xDFC2`: **entry byte == 0xFF ⇒ `0xDFC8: move.w #2,(+0x20); clr.l (+0x26)` = RUN**
  (partner link cleared). Wrestler 0's table `0xE52E` row 0 = `00 72 FF`: B1 jab,
  B2 kick, both = run; categories 3 (`06 06 FF`) and 4 (`21 21 FF`) likewise.
  So run works from **state 0 and state 1 alike** — a direct walk→run exists and is
  the same button chord; no state-1 machinery is involved.
* Secondary: `0xEF9A` (attack remap near an opponent, called at `0xDFDA`) ends at
  `0xF02E`: `+0xA3 == 3` (both new-pressed) ⇒ `0xF036: +0x20 = 2` (run); `== 1` ⇒
  state 5 anim 0 else state 5 anim 0x72.
* AI: e.g. `0x1C4C0/0x1C524/0x1C5A4`: `+0x20=2`, `jsr 0x1D7A0`
  (`+0x2D := facing bit7 ? 0x40 : 0xC0`), plus their own `bset #2,+0x32` /
  `bchg #7,+0x2D`/`+0x2E` games.
* Turn completion re-enters run: see §2c.

### 2b. First-tick steer (`0xF34C-0xF3C2`, once per run via the bit14 latch)

```
0xF34C  bset #6,(+0x20)  (bit14) ; bne -> committed branch 0xF3C6
0xF354  if +0x1D == 6 -> rts     ; arrived from a TURN: direction/facing already
                                 ; flipped by 0x11DE8, do not re-steer
0xF35E  if +0x44 != 0 -> rts     ; whip/rope budget pending: keep direction
0xF366  D2 = old +0x2E ; D1 = +0xA8 & 3     ; held stick L/R
  D1==1 (R):   +0x2C = 0x0040 ; bset #7,+0x2E  (face right)      ; 0xF37C
  D1==2,3 (L): +0x2C = 0x00C0 ; bclr #7,+0x2E  (face left)       ; 0xF38C
  D1==0:       +0x2D = (facing byte &0x80, bchg #7, +0x40)       ; 0xF39C
               = facing right ? 0x40 : 0xC0   ; run the way you face, keep facing;
               rts (skips the change check)
0xF3B4  if +0x2E changed: bset #2,(+0x32)   ; "facing changed" flag
```

### 2c. `0x10FC6` — the ropes probe, and its carry

Called from the **skid** handler (`0x11DA4`). Meaning of carry: **set = "you are
pressed into the ropes"** (and the rope-shake was triggered); clear = nothing there.

```
0x10FC6  btst #1,$1C0161 ; bne 0x11024        ; alt/tag mode -> bounce check only
0x10FD0  +0x36 (rope-zone code from the bounds pass) must be 1 or 5, else -> 0x11024
0x10FE0  btst #1,(+0x37)                      ; pushed at LEFT X bound this frame
           set: if $1C1151 busy -> carry=1
                else $1C1150=1, $1C1180=1 -> carry=1     ; left rope-shake pair, light
0x11002  btst #0,(+0x37)                      ; pushed at RIGHT X bound
           clear -> carry=0
           set:  if $1C11B1 busy -> carry=1
                 else $1C11B0=1, $1C11E0=1 -> carry=1    ; right rope-shake pair
0x11024  (alt mode, or zone not 1/5): if +0x36 == 3:
             +0x20 = 4 ; +0x64 = 0x17        ; WHIP-REBOUND state (victim anim 0x17)
             sounds 0x28 and 0x32 (jsr 0x2052)
         carry = 0 in every 0x11024 path (0x1104C andi #$EE,CCR)
```

`+0x36`/`+0x37` come from the per-frame bounds pass `0x280DC` (`src/floor_2818e.c`):
`+0x37` bit0 = pushed at X max (right), bit1 = X min (left), bit2 = Y high 0x198,
bit3 = Y low 0x118, bit4 = Z floor. `+0x36` zone codes: in-ring handler writes 1 at
any rope contact (X edge with Z<0x180, or Y clamp), 2 = X edge while high (Z≥0x180);
the other floor handlers in the same file write 3, 5, 6 for their X edges
(apron/outside/aisle variants).

### 2d. Turn state 6 — cell `0x11DD0` {handler `0x11DE0`, mode 1, n 2, dur 8/12(+1),
sprites 0x69,0x69}

```
0x11DE0  btst #7,(+0x1C) ; bne 0x11E0C     ; first frame (anim not yet latched):
0x11DE8    bchg #7,(+0x2E)                 ; flip facing
0x11DEE    bchg #7,(+0x2D)                 ; flip run angle (0x40 <-> 0xC0)
0x11DF4    clr.b (+0x01)                   ; mover OFF while turning
0x11DF8    +0x2A = 0x000C                  ; speed pre-armed = 12
0x11DFE    +0x19 = 0x20                    ; sprite X offset low byte = +32px
                                           ;   (+0x18 word, negated when facing)
0x11E04    bclr #2,(+0x32)                 ; clear "facing changed"
         committed frames:
0x11E0C    if +0x25 == 1:   +0x01 = 1      ; frame index 1: mover back ON — you
                                           ;   drift the NEW way at speed 0x0C
0x11E1A    if +0x25 == 0xFE:               ; cell finished (sentinel, anim.md §1c)
0x11E22      +0x20 = 2                     ; *** RESUME RUN ***
0x11E28      bset #7,(+0x18)               ; word bit15 = "core clears the +0x18/+0x1A
                                           ;   sprite offsets after this anim pass"
0x11E2E    +0x4C = 0x20                    ; hit/hurt record id while turning
```

**Exact completion write: `+0x20 = 2` and `+0x18 |= 0x8000`. Nothing else.** The new
run's `0xF34C` first tick then sees `+0x1D == 6` and *skips* steering (§2b), so the
turn's `bchg` pair alone decides the new direction; the run handler `0x11C82`
re-arms per-wrestler speed on its own first tick (mode-1 cell, `+0x25` stays 0xFE,
`+0x22`=0 — see anim.md hold-last).

### 2e. What ends a run

Release does **nothing**: neutral stick was only read on the first tick (and maps to
"run the way you face"). A run in progress ends by:

| trigger | where | result |
|---|---|---|
| opposite direction held | `0xF42E` | state 3 skid |
| opponent grapple request `+0x34` bit1 (`+0x44`==0) | `0xF3C6` | state 3 skid |
| random stumble roll near opponent (gates §1) | `0xF3F2` | state 3 skid |
| damage event byte `+0xFE` bit7 | `0x11CAE` (run handler) | state 3 skid |
| running into the bound you face, zone 3 | `0x11CE0` | state 4, `+0x64`=0x17, `+0x68`=0x0A, sound 0x28 (hard rope rebound) |
| … zone 5 with `+0x44` ≠ 0 | `0x11CFA` | state 4, `+0x64`=0x17, `+0x68`=8, rope-shake value 2 (`0x11D56`) |
| … any other zone (incl. in-ring rope zone 1) | `0x11D14` | **state 6 turn** + rope-shake 2 + `+0x44 -= 1` (floored at 0, `0x11D1E-11D26`) — turn then resumes run, which *is* the visible rebound |
| any other state writer (moves, victim anims) | various | whatever they write |

The edge test (`0x11CC4-0x11CDE`): facing right (`+0x2E` bit7 set) → `+0x37` bit0
(right bound), facing left → bit1 (left bound); only the bound you are running into
counts. `+0x44` is thus the **bounce budget** (whip/AI writers arm it; each edge-turn
consumes one; while nonzero it also suppresses reversal and first-tick steering).

### 2f. Run anim handler `0x11C82` (cell `0x11C62`, mode 2 loop, n 6, dur 6×6(+1),
sprites 9..0xE) — full body

```
0x11C82  btst #7,(+0x1C)  ; first frame:
0x11C8A    +0x01 = 1                        ; polar mover
0x11C90    bset #4,(+0x33)                  ; "is running" flag
0x11C96    A2 = 0x11D4A ; D0 = +0x02        ; per-wrestler run speed table
0x11CA0    clr.w +0x2A ; +0x2B = A2[id]     ; speeds: 2F 35 31 2D 38 30 2C 38 28 2F 30 2F
0x11CAA    -> 0x11D3C
         committed:
0x11CAE    +0xFE bit7 -> +0x20 = 3 ; rts    ; event abort -> skid
0x11CC0    bsr 0x115D2                      ; drop one-sided partner link (+0x26/+0x9C)
0x11CC4    edge test (above); no edge -> 0x11D3C
0x11CE0    zone dispatch (table in §2e)
0x11D2A    state-4 exits also do +0x64=0x17, clr.w +0x44
0x11D3C  every non-exiting tick: +0x4C = 3 ; jsr 0x1112E
                                           ; 0x1112E: footsteps — sound 0x33 when
                                           ; +0x22==0 and frame index +0x25 is 1 or 4
```

The run is horizontal-only: `+0x2C` high byte 0, `+0x2D` ∈ {0x40, 0xC0}; there is no
vertical steer in state 2 (that is the run-in, §4).

---

## 3. Skid handler — cell `0x11D82` {handler `0x11D8E`, mode 1, n 1, dur 0xFF00
(hold forever), sprite 0x0A}

Full body `0x11D8E-0x11DC8`:

```
0x11D8E  btst #7,(+0x1C) ; bne 0x11D9E
0x11D96    bclr #4,(+0x33) ; rts            ; first frame: clear the running flag,
                                            ;   nothing else (mover/speed/angle kept)
0x11D9E  +0x4C = 1                          ; hurt-record id while skidding
0x11DA4  bsr 0x10FC6                        ; ropes probe (§2c)
0x11DA8  bcs? no: bcc 0x11DB2 — carry SET:
0x11DAA    +0x20 = 6 ; rts                  ; skidded into the ropes -> TURN
         carry clear:
0x11DB2  subi.w #5,(+0x2A)                  ; DECEL: speed word -= 5 per frame
0x11DB8  bpl rts                            ; still >= 0: keep sliding (mover +0x01
                                            ;   is still 1, angle +0x2D unchanged)
0x11DBA  +0x20 = 0                          ; went negative -> STAND
0x11DC0  clr.b (+0x01)                      ; mover off
0x11DC4  clr.w (+0x2A)                      ; speed = 0
0x11DC8  rts
```

From run speed 0x28..0x38 the slide lasts 9..12 frames (`0x2A -= 5` until minus).
Note `0x10FC6`'s `+0x36==3` side effect (state 4 + anim 0x17 + sounds) can also fire
from inside the skid — the ROM then *additionally* returns carry-clear and keeps
decelerating that frame, but the state-4 write has already retargeted the anim.

---

## 4. Which run is which — `0xF2F6` bend, 0x10 vs 0x11D4A, `+0x33` bit7

Two unrelated "runs":

| | in-match run | entrance/tag **run-in** |
|---|---|---|
| state | **2** | **1 sub 1** (`+0xAF`=1; sub 2 similar, `0x1192C`) |
| pass-2 handler | `0xF34C` | `0xF244` → `0xF25A[1]` = **`0xF2F6`** |
| motion fn (pass 4) | cell handler `0x11C82` | `0x11796` via `0x1167A[+0xAF]` |
| speed | `+0x2B = 0x11D4A[+0x02]` (0x28..0x38) | **`move.w #0x10,(+0x2A)` at `0x117B0`** (`0x1193A` for sub 2) |
| direction | horizontal, angle 0x40/0xC0 | near-vertical along the aisle |

`0xF2F6` (state 1 sub 1 pass-2, every frame) — confirmed exactly:

```
0xF2F6  btst #2,(+0xA9)        ; UP held
        set -> 0xF308: +0x2C = 0x0008
               btst #7,(+0x33) ; set -> not.b (+0x2D)  => 0xF7
0xF2FE  btst #3,(+0xA9)        ; DOWN held
        set -> 0xF31C: +0x2C = 0x0088
               btst #7,(+0x33) ; set -> not.b (+0x2D)  => 0x77
0xF306  neither -> 0xF330: clr.w (+0x2C)   ; angle 0
0xF336  rts
```

So UP runs at angle 0x08 (nearly straight up-screen, 8/256 bent toward +X) and DOWN
at 0x88 (nearly straight down, bent −X); with `+0x33` bit7 the bend mirrors
(0xF7/0x77). Neutral gives angle 0 — and the motion fn `0x11796` treats
`+0x2D == 0` as **stop** (`0x118B2/0x118EE`: mover off, sprite := facing), so
releasing the stick halts a run-in, unlike state 2. `0x11796` also: first frame sets
`bset #0,+0x32` (running gate for `0xDDC8`), speed 0x10, `+0x44=1` then `0x11900`
re-arms `+0x44 = 0x11914[id] = 0x94` for all 12, and **teleports** onto the aisle:
`X = (Y+0x580)>>2` (left side) or `X = (0xE48−Y)>>2` (right side) by `+0x33` bit7
(`0x117CA-0x117F2`); later frames set facing once: `+0x2E = (+0x33 & 0x80) ^ 0x80`
(`0x117F6`), clamp Y into the aisle band 0x120..0x192 by angle half
(`0x118BE-0x118EC`).

**`+0x33` bit7 = team/side flag** (0 = P1 team / left corner, 1 = P2 team / right).
Set once at match init `0x104FE-0x1055A`: objects at base ≥ `0x1C07C8` (slots 2,3)
get `bset #7,(+0x33)` (`0x1054E`), and again in the spawn loop for slot index ≥ 2
(`0x10596`, spawn XY from `0x10708`). It decides run-in geometry, run-in facing, the
`0xF2F6` mirror, and nothing about the in-match state-2 run.

The claim "speed 0x10 at 0x117B0" therefore applies **only to state 1 sub 1/2
(run-in)**; the state-2 run always uses the `0x11D4A` per-wrestler byte.

---

## 5. C sketch

Field names follow input-walk.md §8 (`o->state_req` = `+0x20` word writer with bit15
clear, `o->joy` = `+0xA8/+0xA9` held nibble, `o->facing` = `+0x2E` byte, `o->angle` =
`+0x2D` byte with `+0x2C` word writes, `o->speed_w` = `+0x2A` word / `o->speed` =
`+0x2B` byte, `o->mover` = `+0x01`, `o->f32/f33/f34` = `+0x32/33/34`).

**New object fields needed** (offset, width):

```c
uint16_t timer44;      /* +0x44 word: bounce/whip budget; low byte = +0x45      */
uint8_t  once46;       /* +0x46 byte: bit0 = stumble-roll once latch            */
uint16_t hit_id;       /* +0x4C: hit/hurt record id (run 3, skid 1, turn 0x20)  */
uint8_t  anim_frame;   /* +0x25: cell frame index; 0xFE = finished (anim core)  */
uint16_t spr_xoff;     /* +0x18: sprite X offset word; low byte +0x19 = pixels, */
                       /*        bit15 = "core clears +0x18/+0x1A after pass"   */
uint16_t victim_id;    /* +0x64: victim-anim id (0x17 = whip-rebound run)       */
uint16_t impact68;     /* +0x68: rebound impact param (0x0A / 8)                */
uint16_t heat70;       /* +0x70: meter indexing chance table 0x1140F            */
uint16_t event_fe;     /* +0xFE: event word; bit15 aborts a run into skid       */
uint8_t  rope_zone;    /* +0x36 and  */ uint8_t push_flags; /* +0x37, from 0x280DC */
/* +0x33 bit4 = running flag, bit7 = team side; +0x32 bit0 = run-in gate,
   bit2 = facing changed.  Globals: mode161 ($1C0161 bits 0/1), rope-shake slots
   $1C1150/$1C1180 + busy $1C1151, $1C11B0/$1C11E0 + busy $1C11B1. */
static const uint8_t run_speed[12] =   /* ROM 0x11D4A, index +0x02 */
    { 0x2F,0x35,0x31,0x2D,0x38,0x30,0x2C,0x38,0x28,0x2F,0x30,0x2F };
```

```c
/* ---- 0x10FC6: ropes probe. Returns 1 = carry (pressed into ropes). ---- */
static int ropes_check(obj *o)
{
    if (!(mode161 & 2)) {                                   /* 0x10FC6 */
        if (o->rope_zone == 1 || o->rope_zone == 5) {       /* 0x10FD0-0x10FDE */
            if (o->push_flags & 0x02) {                     /* left bound 0x10FE0 */
                if (!ropeshake_l_busy()) ropeshake_l(1);    /* $1C1150/$1C1180=1 */
                return 1;                                   /* 0x11052 carry set */
            }
            if (o->push_flags & 0x01) {                     /* right bound 0x11002 */
                if (!ropeshake_r_busy()) ropeshake_r(1);    /* $1C11B0/$1C11E0=1 */
                return 1;
            }
            return 0;                                       /* 0x1104C */
        }
    }
    if (o->rope_zone == 3) {                                /* 0x11024 */
        o->state_req = 4; o->victim_id = 0x17;              /* 0x1102C-0x11032 */
        sound(0x28); sound(0x32);                           /* jsr 0x2052 x2 */
    }
    return 0;                                               /* carry clear */
}

/* ---- 0xF3C6-0xF45A: committed-tick portion of the state-2 pass-2 handler.
 *      (First tick = steer, §2b; this runs every tick after it.) ---- */
void reversal_check(obj *o)
{
    if ((o->f34 & 0x02) && o->timer44 == 0) {               /* 0xF3C6-0xF3D2 */
        o->state_req = 3; return;                           /* 0xF3D4 grapple -> skid */
    }
    if (1 /* ROM bug: reads ROM[0xA3]=0xFF, not (+0xA3) — constant true 0xF3DE */
        && (mode161 & 1)                                    /* 0xF3E8 */
        && (o->timer44 & 0xFF) == 1                         /* +0x45   0xF3F2 */
        && abs16(o->x - o->opp->x) < 0x60                   /* 0xF3FA-0xF40E */
        && !(o->once46 & 1)) {                              /* 0xF410 test-and-set */
        o->once46 |= 1;
        if (chance_1129e(o, 3)) {                           /* 0xF418: rand&0xF <=
                                                               tbl_1140F[o->heat70] */
            o->state_req = 3; return;                       /* 0xF426 stumble */
        }
    }
    if (o->timer44 != 0) return;                            /* 0xF42E */
    if (o->angle & 0x80) {                                  /* 0xF434 running left */
        if (o->joy & 1) o->state_req = 3;                   /* RIGHT held 0xF43C/F444 */
        /* ROM falls through (dead beq at 0xF44A) into the next test — same result */
    }
    if (!(o->angle & 0x80) || (o->joy & 1)) {               /* fallthrough fidelity */
        if (o->joy & 2) o->state_req = 3;                   /* LEFT held 0xF44C/F454 */
    }
}                                                           /* write is state 3 ONLY */

/* ---- 0x11C82: run anim-cell handler (cell 0x11C62), humans AND CPUs ---- */
void run_tick(obj *o)
{
    if (!o->anim_latched) {                                 /* 0x11C82 first frame */
        o->mover = 1;                                       /* 0x11C8A */
        o->f33 |= 0x10;                                     /* running flag 0x11C90 */
        o->speed_w = run_speed[o->wrestler_id];             /* 0x11C96-0x11CA4 */
    } else {
        if (o->event_fe & 0x8000) { o->state_req = 3; return; }  /* 0x11CAE */
        drop_stale_partner(o);                              /* 0x115D2  0x11CC0 */
        int at_edge = (o->facing & 0x80) ? (o->push_flags & 1)   /* 0x11CC4 */
                                         : (o->push_flags & 2);
        if (at_edge) {
            if (o->rope_zone == 3) {                        /* 0x11CE0 */
                o->impact68 = 0x0A; sound(0x28);            /* 0x11CE8-0x11CF2 */
                o->state_req = 4; o->victim_id = 0x17; o->timer44 = 0; /* 0x11D2A */
            } else if (o->rope_zone == 5 && o->timer44) {   /* 0x11CFA-0x11D06 */
                o->impact68 = 8; ropeshake_facing(o, 2);    /* 0x11D08/0x11D56 */
                o->state_req = 4; o->victim_id = 0x17; o->timer44 = 0; /* 0x11D2A */
            } else {
                o->state_req = 6;                           /* TURN     0x11D14 */
                ropeshake_facing(o, 2);                     /* 0x11D56 */
                if (o->timer44) o->timer44--;               /* 0x11D1E-0x11D26 */
            }
            return;
        }
    }
    o->hit_id = 3;                                          /* 0x11D3C */
    footsteps(o);       /* 0x1112E: sound 0x33 when +0x22==0 && frame is 1 or 4 */
}

/* ---- 0x11D8E: skid anim-cell handler (cell 0x11D82, sprite 0x0A held) ---- */
void skid_tick(obj *o)
{
    if (!o->anim_latched) { o->f33 &= ~0x10; return; }      /* 0x11D8E-0x11D9C */
    o->hit_id = 1;                                          /* 0x11D9E */
    if (ropes_check(o)) { o->state_req = 6; return; }       /* 0x11DA4-0x11DB0 */
    o->speed_w -= 5;                                        /* DECEL    0x11DB2 */
    if ((int16_t)o->speed_w < 0) {                          /* 0x11DB8 bpl */
        o->state_req = 0;                                   /* 0x11DBA stand */
        o->mover = 0;                                       /* 0x11DC0 */
        o->speed_w = 0;                                     /* 0x11DC4 */
    }
}

/* ---- 0x11DE0: turn anim-cell handler (cell 0x11DD0: 2 frames, dur 8/12,
 *      sprite 0x69 both) ---- */
void turn_tick(obj *o)
{
    if (!o->anim_latched) {                                 /* first frame */
        o->facing ^= 0x80;                                  /* 0x11DE8 */
        o->angle  ^= 0x80;                                  /* 0x11DEE 0x40<->0xC0 */
        o->mover   = 0;                                     /* 0x11DF4 */
        o->speed_w = 0x000C;                                /* 0x11DF8 */
        o->spr_xoff = (o->spr_xoff & 0xFF00) | 0x20;        /* +0x19    0x11DFE */
        o->f32 &= ~0x04;                                    /* 0x11E04 */
    } else {
        if (o->anim_frame == 1) o->mover = 1;               /* 0x11E0C-0x11E14 */
        if (o->anim_frame == 0xFE) {                        /* cell done 0x11E1A */
            o->state_req = 2;                               /* RESUME RUN 0x11E22 */
            o->spr_xoff |= 0x8000;    /* core clears +0x18/+0x1A post-pass 0x11E28 */
        }
    }
    o->hit_id = 0x20;                                       /* 0x11E2E */
}
```

Run entry (`0xDFC8`/`0xF036`) belongs in the attack-selector transcription, not here:
`if attack_table[id][cat*3+2] == 0xFF && (o->btn_new & 3) == 3 → state_req = 2,
partner = NULL`.

### Open labels

* `+0x36 == 3 / 5` — which physical floors those zone codes belong to (they come from
  the non-in-ring handlers in `0x280DC`; the in-ring rope zone is 1/2).
* `+0x68` semantics inside state 4 anim 0x17 (impact/rebound parameter, values 8/0x0A).
* Who arms `+0x44` for a state-2 run (whip and AI writers; the human button entry
  leaves it 0, so a voluntary run always turns at the ropes rather than bouncing).
