# RNG 0x21B4 / lockup 0x0B resolution / category 8 — exact transcription

Source: `reference/maincpu.asm` (read-only, 2026-08-22). Object stride 0x10C.
Engine field names from `engine/engine.h`: `o->state` (+0x20 word, bit15 =
entered latch, low byte = state id), `o->partner` (+0x26), `o->mash` (+0xBD),
`o->count` (+0x22), `o->frame` (+0x24/+0x25), `o->move_id` (+0x60),
`o->exch` (+0x48), `o->combo` (+0x52), `o->combo_t` (+0x54). `+0x44` is the
grapple flag word (bit15 hidden-half, bit14 exchange-lost, bit13 impact-cell
pending); the ROM mostly addresses it as the BYTE at +0x44 (= high byte), so
`btst #5/#6/#7,(+0x44)` == word bits 13/14/15.

---

## 1. RNG 0x21B4

### 1a. Routine, PC by PC

| PC | instruction | meaning |
|---|---|---|
| 0x21B4 | `movem.l D1,-(A7)` | only D1 is preserved; D0 is the return, D2-D7 untouched |
| 0x21B8 | `move.l $1C005C,D0` | D0 = 32-bit RNG state |
| 0x21BE-0x21CA | `add.l D1..D7,D0` | folds in **all seven** caller data registers (garbage included) |
| 0x21CC | `add.l $1C0080,D0` | + 32-bit frame counter |
| 0x21D2-0x21D4 | `move.w D0,D1; andi.w #7,D1` | rotate count = low 3 bits of the sum |
| 0x21D8 | `ror.l D1,D0` | rotate right by 0..7 (count 0 = no rotate, C cleared) |
| 0x21DA | `move.l D0,$1C005C` | state := rotated sum |
| 0x21E0-0x21E4 | `movem.l (A7)+,D1; rts` | return D0 = new state (full long); flags = those of `ror.l` (N/Z of result, C = last bit out); no caller reads them |

- **State** `$1C005C` (long). No writer other than 0x21DA exists in the ROM
  (grep). It is never seeded: initial value = whatever the boot RAM clear
  left (0) — TODO EXACT: confirm boot memclear covers $1C005C (no explicit
  `clr`/`move` to it anywhere).
- **Frame counter** `$1C0080` (long): `addq.l #1` at 0x10F2 in the main
  loop right after the vblank handshake (`$1C006F` bits 0/1), i.e. once per
  frame during play; cleared at mode transitions 0x942/0xBB6/0xE02/0xF94;
  other loops bump it at 0x5354/0x5996. Word `$1C0082`/byte `$1C0083` are
  its low word/byte (0xF70C uses `$1C0083` for the non-human-vs-CPU tie-up
  parity). Engine: `st->frame` (reset on the same transitions).

### 1b. C reproduction

```c
static uint32_t rng_state;                 /* $1C005C, never seeded */
uint32_t rng_21b4(uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d4,
                  uint32_t d5, uint32_t d6, uint32_t d7, uint32_t frame /*$1C0080*/)
{
    uint32_t s = rng_state + d1 + d2 + d3 + d4 + d5 + d6 + d7 + frame;
    unsigned n = s & 7;
    s = n ? ((s >> n) | (s << (32 - n))) : s;     /* ror.l D1,D0 */
    rng_state = s;
    return s;
}
```

Bit-exactness requires the exact D1-D7 of every caller. Verdict per caller
(everything not listed below as "known" is inherited from the dispatcher and
is effectively register garbage):

| caller PC | context | mask / use | D-register knowledge at the call |
|---|---|---|---|
| 0x6544, 0x655C | attract joystick synth (obj +0x5E/+0x56 timers, table 0x6584 → +0xA9) | `&0xF \| 4` vs +0x56; `&7` dir index | D1 = previous value; rest garbage. **Attract only** — not needed for match fidelity |
| 0x8122 | non-match object (+0x03 id 0x40/41/47/49 sprite flicker) | `&1` → +0x24 = 4 | title/attract object — ignore |
| 0x8E20 | non-match object blink (`count` reload) | `&0xF + 8` → +0x22 | title/attract — ignore |
| 0xAB78 | non-match (+0x25 toggle, `$1C0168`) | `&0xF` added to +0x22 | title/attract — ignore |
| 0xF6F2 | tie-up resolve, human(A0) vs CPU(A1) (face-tieup §2.4) | `&0xF` added to bias-table word | D4 = difficulty<<3 (known), D2 = table word (known), D0 = prior, D1/D3/D5-D7 = garbage from 0xF560 pass |
| 0x110A0 | 0x1108C CPU pin-intent roll (`$1C007C!=0`, CPU object) | `&0xFF` < `0x110C8[id]` → bset #4,+0x34 | garbage |
| 0x112EE | 0x112B8 family grapple-threshold (`0x113CA`/difficulty*3 + +0x70) | `&0xF` >= D1 threshold (bcc) | D1 = threshold byte (known); rest garbage |
| 0x11358 | 0x11322/0x11338/0x11340 (tables 0x113CD/0x113FF/0x11404, idx min(+0x48,4)) | `&0xF` > D2 (bhi) | D2 = threshold (known) |
| 0x1138C | 0x11368/0x11370 (0x1140F/0x11409/0x1140C by +0x70) | `&0xF` <= byte (bls → fail) | D2 = +0x70 (known) |
| 0x1EFD8, 0x1F008 | lockup mash seed (this doc §2) | `&3` added to `o->mash` | D6 = CPU-walk `dbra` counter (0x1C1EE, known: remaining objects), others garbage |
| 0x24D2 (bsr) | weighted pick 0x24CC, loop ×≤4 | `(&0xFF)>>1 < 0x64` | D1 = 3,2,1,0 retry counter (known); D2 = caller's |

**Documented simplification**: the sum of D1-D7 is unreproducible without a
full register trace, so the native engine cannot match ROM RNG *values*. The
gate already in use (drawn-parity canary: number of 0x21B4 calls per frame
must match MAME) is the right contract; treat the unknown register sum as a
per-call constant `K` (use 0) and keep the ror/state/frame structure so the
call-count and the 8-way rotation distribution stay ROM-shaped. Callers whose
*results* depend on the garbage: all of them except none — every call adds
D1-D7. The only calls where the garbage is plausibly constant are 0x24CC's
inner loop (D1 fixed, D2 = caller's pick index) and 0x1EFD8/0x1F008.

### 1c. Weighted pick 0x24CC (used by the lockup)

```
24D0  D1 = 3
24D2  loop: D0 = rng_21b4(); D0 = (D0 & 0xFF) >> 1          ; 0..127
24DE        if (D0 < 0x64) break;  dbra D1 → retry (≤4 draws)
24E8  else D0 = 0x32
24EC  if (D0 == 0) D0 = 1                                    ; roll ∈ 1..99
24F6  D1 = 0; D2 = 0; for(;;){ D2 += (u8)A6[D1]; if (D2 >= roll) return D1; D1++; }
```
With `0x1F05E = {0x32,0x32}`: returns 0 iff roll <= 50 (50/99), else 1.
Note 0x24CC consumes **1 to 4** RNG calls (parity canary!).

---

## 2. Lockup (state 0x0B) resolution

Dispatch: combat table `0x1C1F4[state]` via `0x1C1D2`, walked by `0x1C15C`
— **CPU-driven objects only** (pins-referee §3). Row 0x0B = `0x1EFAA`, row
0x0C = `0x1F06A`. The human side is the attack selector (§3). `+0x20` bit15
is set by the movement walker at 0xF4EC after the first dispatch of a state.

### 2a. 0x1EFAA — CPU mash seeding + countdown

| PC | instruction | meaning |
|---|---|---|
| 0x1EFAA | `btst #7,(+0x20); bne 1EFE6` | skip entry block once latched |
| 0x1EFB2 | `clr.w (+0xB6)` | AI scratch word := 0 |
| 0x1EFB6 | `bclr #1,(+0xB9)` | AI flag |
| 0x1EFBC | `move.b #0x20,(+0xBD)` | `o->mash = 0x20` |
| 0x1EFC2-0x1EFD4 | `A1=partner; btst #6,(A1+0x56); bne; btst #7; bne` | partner CPU (bit6 autopilot or bit7 CPU) → keep 0x20 |
| 0x1EFD6 | `bsr 0x1F04A` | partner is HUMAN: `o->mash = (u8)0x1F060[$1C0162]` — stage table `07 07 06 06 03 05 05 04 04 03` (stages 0-9; beyond = code bytes, TODO EXACT if stage ≥10 possible) |
| 0x1EFD8-0x1EFE2 | `jsr 21B4; andi.w #3; add.b D0,(+0xBD)` | `o->mash += rng & 3` |
| 0x1EFE6 | `subq.b #1,(+0xBD); bne rts` | per frame (entry frame included) countdown |
| 0x1EFEC-0x1F012 | reload exactly as 0x1EFBC-0x1EFE2 | `o->mash = seed + rng&3` |
| 0x1F016 | `btst #6,(+0x44); bne rts` | word bit14 set = partner already won this exchange → no pick |
| 0x1F01E-0x1F02E | `A6=0x1F05E; jsr 24CC; cmpi.b #0,D0; bne rts` | 50/99 pick; arm 1 = do nothing |
| 0x1F030 | `move.w #5,(+0x20)` | `o->state = 5` (bit15 clear → entry frame) |
| 0x1F036 | `move.w #0x29,(+0x60)` | `o->move_id = 0x29` — **the CPU shoves/knees: it WINS this exchange** |
| 0x1F03C-0x1F040 | `A1=partner; bset #6,(A1+0x44)` | partner word bit14 := 1 (locked out, marked loser) |

Note: the expiry frame does NOT return early — a CPU with mash=1 reloads and
rolls in the same frame. "Seed" semantics: vs human at stage 0 the CPU rolls
every 7..10 frames; CPU vs CPU every 0x20..0x23 frames.

### 2b. 0x1F06A — state 0x0C CPU row (hold winner)

`cmpi.b #0x30,(+0xBD); bcc rts; addq.b #1,(+0xBD); rts` — `o->mash` ramps
to 0x30 while holding; nothing else (the grapple pick cascade 0x1F078 is a
separate row — TODO EXACT which state row jumps to 0x1F078).

### 2c. Move 0x29 handler — table `0x12614[0x29] = 0x1656A`

Cell record 0x1656A: `{code 0x16582, mode 1 (one-shot, frame parks at 0xFE),
n=4, durations 0x14 0x08 0x0E 0x0E, cells 0x0F5 0x0F6 0x0F7 0x1D2}`. Total
≈ 21+9+15+15 = 60 ticks (d+1 each, anim.md).

| PC | instruction | meaning |
|---|---|---|
| 0x16582 | `btst #7,(+0x1C); bne 165A4` | first frame only (anim_sel latch) |
| 0x1658A | `clr.b (+0x01)` | `o->mover = 0` |
| 0x1658E | `bclr #7,(+0x44)` | self word bit15 := 0 → I am the drawn half |
| 0x16594 | `bset #7,(A2+0x44)` | partner (A2 = `o->partner`) hidden half |
| 0x1659A | `move.w #0xFF,(A2+0x20)` | **partner state := 0xFF** (frozen/carried, skipped by all 3 dispatchers; hidden cell 0x125C0) |
| 0x165A4-0x165B6 | `if (+0x25==1 && +0x22==0) sound 0x2A` | knee impact SFX on last tick of frame 1 |
| 0x165BC | `cmpi.b #0xFE,(+0x25); bne rts` | wait for anim end |
| 0x165C4 | `move.w #1,(A2+0x68)` | partner damage := 1 |
| 0x165CA | `addi.b #1,(A2+0xC7)` | partner times-hit stat |
| 0x165D0 | `move.w #0x100,(A2+0x54)` | `partner->combo_t = 0x100` |
| 0x165D6 | `addi.w #1,(A2+0x52)` | `partner->combo += 1` (times shoved in this lockup) |
| 0x165DC | `cmpi.w #2,(A2+0x52); bcc 165FC` | <2 → rematch, ≥2 → advantage |
| 0x165E4-0x165EA | `move.w #0xB,(+0x20); move.w #0xB,(A2+0x20)` | **rematch**: both back to lockup (entry frame → 0x1EFAA reseeds, 0x12474 re-runs) |
| 0x165F0 | `clr.w (+0x44)` | self = drawn half, no impact cell, bit14 cleared |
| 0x165F4 | `move.w #0x8000,(A2+0x44)` | partner hidden half, bit14 cleared |
| 0x165FC | `move.w #0xC,(+0x20)` | **advantage**: self → state 0x0C (hold won, 0xF760 shape) |
| 0x16602 | `move.w #0x2000,(+0x44)` | play impact cell first |
| 0x16608 | `move.w #0xFF,(A2+0x20)` | partner stays 0xFF (held victim) |
| 0x1660E | `clr.w (A2+0x44)` | partner flags := 0 |

Answers to the prompt's framing:
- **Who is the winner**: the one who performs 0x29 (CPU via 0x1F030 roll, or
  human via cat 8 press). The prompt's "loser → state 5 move 0x29 stagger"
  is inverted: 0x29 is the winner's knee/shove; the **loser goes to 0xFF**
  (drawn inside the winner's two-man cells F5-F7, then hidden).
- **Does the winner advance**: only on the partner's **second** shove in
  this lockup (`partner->combo >= 2`) → state 0x0C. First shove → both back
  to 0x0B (visual: knee, then re-lock). `+0x52` is `clr`'d at tie-up entry
  (0xF600 cleanup) so the count is per-lockup.
- **Unlinking**: 0x29 never clears `+0x26`; the pair stays linked through
  0x0B ↔ 0x29 ↔ 0x0C. (0x0C's flip at timer 0 swaps roles, face-tieup §4;
  the eventual unlink is in the grapple-move layer / 0xE1B0 cat 9 — out of
  scope here.)
- `+0xFE` bit15 (hit latch) during 0x0B bails both to state 5 anim 0x50
  (0x1249E in 0x12474) — TODO EXACT partner handling there.
- 0x12474 clears word bit13 (`bclr #5,+0x44` at 0x124DA) once the impact
  cell 0x1D2 has shown — this is what un-blocks the human's cat 8 press.

### 2d. C sketch

```c
static uint8_t mash_seed(eng_state *st, eng_obj *o) {        /* 0x1EFBC-0x1EFE2 */
    eng_obj *p = &st->obj[o->partner];
    uint8_t v = 0x20;
    if (!(p->f56 & 0xC0)) v = rom8(0x1F060 + st->stage);     /* partner human */
    return v + (rng_21b4(...) & 3);
}
void cpu_lockup_0b(eng_state *st, eng_obj *o) {              /* 0x1EFAA */
    if (!(o->state & 0x8000)) { o->b6 = 0; o->b9 &= ~2; o->mash = mash_seed(st, o); }
    if (--o->mash) return;
    o->mash = mash_seed(st, o);
    if (o->grap44 & 0x4000) return;
    if (pick_24cc(st, 0x1F05E) == 0) {                       /* 50/99 */
        o->state = 5; o->move_id = 0x29;
        st->obj[o->partner].grap44 |= 0x4000;
    }
}
void cpu_hold_0c(eng_obj *o) { if (o->mash < 0x30) o->mash++; }   /* 0x1F06A */

void move29_cell(eng_state *st, eng_obj *o) {                /* 0x16582 */
    eng_obj *p = &st->obj[o->partner];
    if (!(o->anim_sel & 0x8000)) {
        o->mover = 0; o->grap44 &= 0x7FFF; p->grap44 |= 0x8000; p->state = 0x00FF;
        return;
    }
    if ((o->frame & 0xFF) == 1 && o->count == 0) sfx(0x2A);
    if ((o->frame & 0xFF) != 0xFE) return;
    p->damage68 = 1; p->hits_c7++; p->combo_t = 0x100; p->combo++;
    if (p->combo < 2) { o->state = 0x000B; p->state = 0x000B; o->grap44 = 0; p->grap44 = 0x8000; }
    else             { o->state = 0x000C; o->grap44 = 0x2000; p->state = 0x00FF; p->grap44 = 0; }
}
```

---

## 3. Category 8 (map row `29 29 29`)

Produced only by test `0xE180`, 5th in the selector chain `0xDF3A-0xDF92`
(human pass-2 attack selector `0xDE86`, strikes.md §1). Exact path to it:

| PC | instruction | condition for reaching cat 8 |
|---|---|---|
| 0xDE8A-0xDEA2 | tag mode (`$1C0161` bit1) && `+0x32` bit1 clear && `+0x33` bit2 clear → exit | must be the legal man |
| 0xDEA6-0xDEAE | `D3 = +0xA3 & 3; beq exit` | a **new** B1/B2 press this frame (edge, not hold) |
| 0xDEB4 | `bsr 0xEBC4; bcs consumed` | grapple follow-up pre-consumer must not fire (it keys on state 0x0C / anim 0x15; not 0x0B — TODO EXACT) |
| 0xDEBC | `jsr 0xF0BA; bcs consumed` | tag corner-man attack must not fire |
| 0xDEC6 | `+0x21 == 4 → exit` | n/a (we are 0x0B) |
| 0xDED0 | `+0x32` bit0 set → short chain (0xE496/0xE4BA only) | run-in flag must be clear, else cat 8 unreachable |
| 0xDEDA | `bsr 0xEA42; bcs direct` | counter/join pre-consumer must not fire |
| 0xDF3A-0xDF52 | 0xE002 (weapon && state 0/1), 0xE02E (state 2), 0xE0B8/0xE110 (state 0/1) | all fail automatically in state 0x0B |
| **0xE180** | `A1 = +0x26` | partner |
| 0xE184 | `cmpi.b #0xB,(+0x21); bne fail` | **own state == 0x0B** |
| 0xE18E | `btst #5,(+0x44); bne fail` | word bit13 clear — impact cell not pending (blocks the lockup's first ≈0x10 ticks and every rematch's first ticks; cleared at 0x124DA) |
| 0xE198 | `btst #6,(+0x44); bne fail` | word bit14 clear — partner has not already won this exchange |
| 0xE1A2 | `bset #6,(A1+0x44)` | partner bit14 := 1 (lockout / loser mark) |
| 0xE1A8 | `D0 = 8; ori #1,CCR` | **cat 8** |
| 0xDF96-0xDFB8 | `A2 = *(0xE4FE + id*4); D0 = A2[8*3 + ((+0xA3&3)-1)]` | = 0x29 for every wrestler and every column (B1/B2/both) |
| 0xDFD6 | `bsr 0xE926` | tag-legality remap touches only move 0x48 — no effect |
| 0xDFDA | `bsr 0xEF9A` | proximity remap; TODO EXACT that 0x29 passes through unchanged (grapple-moves §1a says the net result is `+0x20=5, +0x60=0x29`) |
| 0xDFE8 | `+0x20 = 5; +0x60 = D0` | human performs 0x29 this frame |

So cat 8 arises **exclusively** from: human object, new B1 or B2 edge, own
state 0x0B, `+0x44` bits 13 and 14 clear, not run-in flagged, no earlier
pre-consumer. Either half of the lockup (hidden 0x8000 or drawn 0x2000) may
win — bit15 is not tested; 0x16582 re-assigns halves. Mashing does not touch
`o->mash`; each eligible press wins immediately, racing the CPU's 0x1EFAA
roll for the same exchange (whoever sets the other's bit14 first).

```c
int cat8_test(eng_state *st, eng_obj *o, int *cat) {         /* 0xE180 */
    eng_obj *p = &st->obj[o->partner];
    if ((o->state & 0xFF) != 0x0B) return 0;
    if (o->grap44 & 0x2000) return 0;                        /* bit13 */
    if (o->grap44 & 0x4000) return 0;                        /* bit14 */
    p->grap44 |= 0x4000;
    *cat = 8; return 1;                                       /* row → 0x29 */
}
```
