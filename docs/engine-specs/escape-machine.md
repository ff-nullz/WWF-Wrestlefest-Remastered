# Escape machine — victim-side mash / kick-out / spring-up / hold flip (read-only transcription)

Source: reference/maincpu.asm. Field names from engine/engine.h (`mash_aa`=+0xAA, `react_id`=+0x64 word
(+0x65 low byte), `state`=+0x20 (+0x21 low byte), `move_id`=+0x60 (+0x61 low byte), `partner`=+0x26,
`opp`=+0x7A, `grap44`=+0x44, `hold_t`=+0x46, `hold_ph`=+0x45, `hp`=+0x66, `hp_max`=+0x72, `band`=+0x70,
`btn_new`=+0xA2/+0xA3 low byte, `anim_sel`=+0x1C (bit15 init latch), `frame`=+0x24 (+0x25 low byte,
0xFE = finished), `result`=+0xFE).

---

## 1. +0x7A / "+0x7B" — there is NO byte field +0x7B

Every access in the ROM is `.l` on `($7a,Ax)`: +0x7A..+0x7D is one long = **pointer to the current
opponent object** (engine.h `opp`). "+0x7B" is merely byte 1 of that pointer. No `.b`/`.w` access to
+0x7A/+0x7B/+0x7C/+0x7D exists. The only near byte use is `0x0EA40: ori.b #$68,($7a,A0)` which is a
mis-disassembled data word inside a table (bytes `00 28 22 68 00 7A` = the start of `movea.l ($7a,A0),A1`
at 0x0EA42) — not code.

### Writers (all `.l`)
| PC | write | context |
|---|---|---|
| 0x0DF30 | `+0x82 = +0x7A` | human input walk 0xDE86: after 0xE958 finds a new target (carry clear)… |
| 0x0DF36 | `+0x7A = A1` | …opp := result of 0xE958, old opp saved in +0x82 (previous opponent) |
| 0x0F7CE | `A1->+0x7A = A0` | tie-up engage (0xF7C6-0xF7DC): mutual `partner` and `opp` links both ways, |
| 0x0F7D2 | `A0->+0x7A = A1` | `bset #6,+0x33` both |
| 0x1972E | `A3->+0x7A = A0->+0x26` | pin/setup at 0x196FA-0x19734: A2 := state 0xFF, A3 (= A0->+0x86 helper obj) → state 5 move 0x4D, A3->opp := A0's partner |
| 0x1C84C | `+0x7A = +0x7E` | CPU AI: rescue link +0x7E (tag save) becomes the opponent |
| 0x1C9EC | `+0x7A = A1->+0x86` | CPU AI: target from record A1's +0x86 |
| 0x1CA14 | `+0x7A = A1->+0x86` | same |
| 0x1CA68 | `+0x7A = A1->+0x86` | same |
| 0x1CCB8 | `+0x7A = A1->+0x86` | same, then state 5 move 0x3D |
| 0x1D4BE | `+0x7A = A1` | CPU AI after 0x24CC d100 == 0: opp := A1, then state 5 move 0x39 |
| 0x1E5E2 | `A1->+0x7A = A0` | CPU AI engage (0x1E5D4): `bset #6,+0x60` latch; opp's opp := self; `+0x26 = opp`; if opp's partner null → `opp->+0x26 = self` |

### Readers relevant to the escape machine
| PC | read | meaning |
|---|---|---|
| 0x0DEE2-0x0DEEE | `opp->+0x34 & 7` | input walk: opp "engaged/pinning" low bits gate target re-pick |
| 0x0EBF6-0x0EC00 | `btst #4,(opp->+0x34)` | **0xEBC4**: opp "wants to/is pinning" (pins-referee §1b) → skip down-victim arms |
| 0x1B3A6-0x1B3B0 | `btst #4,(opp->+0x34)` | lying reaction 8/9 per-frame: opponent pinning → stay down, no mash tick |
| 0x1880E-0x18818 | `opp->+0x9A = opp->+0xAA = 0x1000` | pin cell: freeze victim's mash (0x1000 = un-mashable) and down timer |

No +0x7A state machine exists: the field is a pointer, the "victim-side state" lives in `+0xAA`, `+0x65`,
`+0x20/+0x60`, `+0x44`, `+0x46/+0x45`.

---

## 2. 0xEBC4 — the action prefix (called only from 0x0DEB4 inside 0xDE86)

Caller gate (0xDE86-0xDEB8): `D3 = +0xA3 & 3` (new-press B1/B2) must be nonzero (else rts via 0xDFF8);
then `bsr 0xEBC4`; **carry set = press consumed, skip rest of attack selection** (0xDFF2). Mode gate at
0xDE8A: if `$1C0161` bit1 set and `+0x32` bit1 clear and `+0x33` bit2 clear → abort (TODO EXACT meaning of
$1C0161 bit1). State 0xFF objects never get here (dispatcher skips 0xFF at 0xF1C2).

Ladder, exact order (A0 = presser; `s` = +0x21, `m` = +0x61 bytes):

| PC | test | outcome |
|---|---|---|
| 0xEBC4 | `mash_aa != 0x4000` | → 0xEDB0 **fail** (carry clear, nothing written) |
| 0xEBCE-0xEBE4 | `s==5 && (m==0x4A \|\| m==0x51)` | 0xEBE6: `state=0x0005`, `move_id=0x004B` (kick-out) → 0xEDB6 |
| 0xEBF6-0xEC00 | `opp->+0x34 bit4` set (opp is pinning) | skip both down-victim arms → 0xEC7A |
| 0xEC04-0xEC12 | `s==4 && +0x65==8` | 0xEC14: `state=0x0005`, `move_id=0x0038` (**spring-up**) → 0xEDB6 |
| 0xEC24-0xEC32 | `s==4 && +0x65==9` | → roll-away arm 0xEC34, else → 0xEC7A |
| 0xEC34-0xEC5E | A2=`partner`; if `A2->s==5 && A2->m ∈ {0x35,0x0E,0x0F,0x23}` (incoming cover) | 0xEC60: if `A2->+0x60` bit4 (byte +0x60 = high byte → word bit 12, "cover cell 3 reached", set at 0x13DF2) **clear** → 0xEDB0 fail; set → 0xEC6A |
| 0xEC6A | (partner not covering, or covering with bit4 set) | `state=0x0005`, `move_id=0x0078` (**roll-away**) → 0xEDB6 |
| 0xEC7A-0xEDAE | `s==5 && m==X` → `move_id=Y`, `state=0x0005` | 0x5D→0x67 (0xEC8A), 0x5E→0x56 (0xECAA), 0x5F→0x5C (0xECCA), 0x60→0x5B (0xECEA), 0x61→0x59 (0xED0A), 0x62→0x5A (0xED2A), 0x63→0x55 (0xED48), 0x52→0x58 (0xED66), 0x53→0x57 (0xED84), 0x64→0x4B (0xEDA2) |
| 0xEDB0 | fail | `andi #$EE,CCR` (carry clear) → rts |
| 0xEDB6 | success | `clr.w mash_aa` (+0xAA=0); `ori #1,CCR` (carry set) → rts |

Notes:
- `move.w #5,+0x20` writes the WHOLE word → clears the bit15 "entered" latch → the new move's cell runs
  its init path next frame. Same for `move.w #N,+0x60` (clears +0x60 bit15/bit7-of-high-byte latches).
- **Only the presser is written** by 0xEBC4 itself (state, move_id, mash_aa). The attacker/partner is
  written by the *escape move's* handler on its first frame, e.g. move 0x4B (0x1820C): `bclr #0,
  (pinner +0x35)`, pinner → state 4 react 0x0F, `bclr #6,+0x33` both (pins-referee §Kick-out). Hold
  escapes 0x67/0x56/0x5C/0x5B/0x59/0x5A/0x55/0x58/0x57 likewise write the holder inside their own
  handlers — TODO EXACT per move (not transcribed here; grapple-move graph).
- There is no "hold reversal" in 0xEBC4: every arm is a fixed move substitution.
- The `s==4` arms are reached only when the opponent is NOT flagged pinning (+0x34 bit4); a lying victim
  under a CPU with pin intent cannot spring up/roll away even with the 0x4000 latch.

```c
/* 0xEBC4 — returns true if the press was consumed */
static bool escape_prefix(eng_obj *o) {
    if (o->mash_aa != 0x4000) return false;                         /* 0xEBC4 */
    int s = o->state & 0xFF, m = o->move_id & 0xFF;
    if (s == 5 && (m == 0x4A || m == 0x51)) return set_move(o, 0x4B);  /* 0xEBE6 */
    if (!(OBJ(o->opp)->f34 & 0x1000)) {                             /* 0xEBFA opp +0x34 bit4 */
        if (s == 4 && (o->react_id & 0xFF) == 8) return set_move(o, 0x38);   /* 0xEC14 */
        if (s == 4 && (o->react_id & 0xFF) == 9) {                  /* 0xEC24 */
            eng_obj *p = OBJ(o->partner);
            int pm = p->move_id & 0xFF;
            if ((p->state & 0xFF) == 5 &&
                (pm == 0x35 || pm == 0x0E || pm == 0x0F || pm == 0x23) &&
                !(p->move_id & 0x1000)) return false;              /* 0xEC60 cover not yet at cell 3 */
            return set_move(o, 0x78);                               /* 0xEC6A */
        }
    }
    if (s == 5) {                                                   /* 0xEC7A.. */
        static const uint8_t map[][2] = {{0x5D,0x67},{0x5E,0x56},{0x5F,0x5C},{0x60,0x5B},
            {0x61,0x59},{0x62,0x5A},{0x63,0x55},{0x52,0x58},{0x53,0x57},{0x64,0x4B}};
        for (int i = 0; i < 10; i++) if (m == map[i][0]) return set_move(o, map[i][1]);
    }
    return false;                                                   /* 0xEDB0 */
}
static bool set_move(eng_obj *o, int mv) {                          /* 0xEDB6 */
    o->state = 0x0005; o->move_id = mv; o->mash_aa = 0; return true;
}
```

---

## 3. Mash accounting

### 3a. Who writes +0xAA (complete list)
| PC | write | site |
|---|---|---|
| 0x06FA6 / 0x07038 | `= 0x4000` | match-start object setup for pinner/pinned pair (+0x56 cleared / `|= 0x40`): pre-latched TODO EXACT (mode setup, not in-match) |
| 0x08C74 / 0x08CA6 / 0x0983C | `= 1` | TODO EXACT (result/round setup) |
| 0x10C8A | `= D1` | 0x10C60 D0=0 seed |
| 0x10CA8-0x10CB4 | `+0xAB = tbl; &= 0xFF; -= 1` | 0x10C60 D0>=2 seed |
| 0x10CD8-0x10CDE | `+0xAB = tbl; &= 0xFF` | 0x10C60 D0=1 seed |
| 0x10D28 / 0x10D2E | `-= 1; ==0 → = 0x4000` | **0x10D04 down tick — the only press decrement** |
| 0x0EDB6 | `= 0` | 0xEBC4 success |
| 0x114DC | `= 0` | state 0 (stand) entry init 0x114B2 |
| 0x11E60 | `= 0` | state 7 get-up init |
| 0x1329A, 0x13F72, 0x15EFE, 0x15FC2, 0x178FA, 0x180D0, 0x18C2C, 0x1B1F2, 0x1BD7E, 0x1E292 | `= 0` | move/reaction init paths, each immediately followed by `jsr 0x10C60` (re-seed) |
| 0x19562..0x1962C (7 sites) | `= 0` + 0x10C60 | submission-hold victim move inits (0x5D-0x64 family) |
| 0x13518 | `+= 0x0C` (on A2=victim) | cover move cell 0x13500: adds 12 presses when the cover engages |
| 0x15D50 / 0x18818 | `= 0x1000` (victim) | pin cells: un-mashable (0x1000 ≠ 0x4000, never decremented to it within a count) |
| 0x17656 | `= D0` (victim) | walk-up seed `max(1, 0x176BA[min(+0xDC,5)] − hp) (+6 if move 0x23)` (pins-referee §1c) |

### 3b. 0x10C60 — seed (`D0` = mode). Exact:
```
0x10C64  if +0xAA < 0 (bit15)          → seed          ; negative = "re-seed me"
0x10C6A  else if +0xAA != 0            → return        ; already seeded / latched
0x10C6E  D0==0: (0x10C72) hp==0 → +0xAA = 0x100
                 else D1 = 0x2A - hp; if D1 < 0 → D1 = 1; +0xAA = D1        (0x10C7E-0x10C8A)
0x10C92  D0==1: (0x10CBA) quarter = hp_max >> 2          ; lsr.w #2 of +0x72
                 D1 = quarter; D2 = 3
                 loop: if D1 >= hp (unsigned cmp.w hp,D1; bcc) break     (0x10CC6)
                       D1 += quarter; D2--; while D2 != 0                (0x10CCC-0x10CD0)
                 +0xAB = byte 0x10D00[D2]; +0xAA &= 0xFF                 (0x10CD2-0x10CDE)
                 table 0x10D00 = {01,03,07,15}
                   → hp <= 1q : D2=3 → 0x15 (21 presses)
                     hp <= 2q : D2=2 → 7
                     hp <= 3q : D2=1 → 3
                     hp >  3q : D2=0 → 1
0x10C98  D0>=2: idx = (D0-1)*3 + band(+0x70);  +0xAB = 0x10CEA[idx]; +0xAA &= 0xFF; +0xAA -= 1
                 0x10CEA rows (3 bytes each by band 0..2):
                   row0 (unused, D0=1 takes the quarter path) 01 01 01
                   D0=2: 18 1C 20   D0=3: 08 0A 0C   D0=4: 0A 0D 10
                   D0=5: 18 1C 20   D0=6: 04 0C 18   D0=7: 04 0C 18   (then FF)
```
Note: D0=1 and D0>=2 write only the low byte and mask; no `-1` on the D0=1 path, `-1` on D0>=2.
Callers in the escape context use D0=1 (0x1B1FA, 0x1B2A8, 0x1BD86 — fall/bounce landings, i.e. the
lying reaction seeds) and D0=0 (hold/pin victim moves). `+0xAA` bit15 "re-seed" writers: none found in
the ROM (TODO EXACT — likely only reachable via the 0x1000 frozen value, which is positive, so never).

### 3c. 0x10D04 — down tick (per frame, called by lying/bounce/pinned/held handlers)
```
0x10D08  if (+0x56 & 0xC0) != 0 → return             ; in a pin record (b7 CPU, b6 pinned) TODO EXACT
0x10D12  bset #7, $1C167A                            ; HUD "mash!" flash request
0x10D1A  if +0xAA == 0x4000 → return                 ; already latched
0x10D22  if +0xA3 == 0      → return                 ; no fresh button edge this frame
0x10D28  --+0xAA; if != 0 → return
0x10D2E  +0xAA = 0x4000                              ; mashed-out latch
```
So +0xAA reaches 0x4000 **only** at 0x10D2E (needs `seed` fresh presses; 0x1000-frozen victims need
4096). It is consumed by 0xEBC4 on the *next* press (0x10D04 runs in the state handler, 0xEBC4 in the
input prefix; same press can't do both because 0x10D1A returns before decrement when already 0x4000 and
0xEBC4 ran earlier in the frame only if already 0x4000 — order: input prefix 0xDE86 runs before the state
dispatch, so the latching press and the escaping press are distinct).

```c
static void mash_seed(eng_obj *o, int mode) {                    /* 0x10C60 */
    if (!(o->mash_aa & 0x8000) && o->mash_aa) return;
    if (mode == 0) o->mash_aa = o->hp == 0 ? 0x100 : (0x2A - o->hp > 0 ? 0x2A - o->hp : 1);
    else if (mode == 1) {
        unsigned q = o->hp_max >> 2, d1 = q; int d2 = 3;
        while (!(d1 >= o->hp) && --d2) d1 += q;      /* exact loop shape: test, add, dec, loop */
        o->mash_aa = (uint8_t[]){1,3,7,0x15}[d2];
    } else o->mash_aa = tbl_10CEA[(mode-1)*3 + o->band] - 1;
}
static void down_tick(eng_obj *o) {                               /* 0x10D04 */
    if (o->f56 & 0xC0) return;
    hud_mash_flash = 1;                                           /* $1C167A bit7 */
    if (o->mash_aa == 0x4000 || o->btn_new_byte == 0) return;
    if (--o->mash_aa == 0) o->mash_aa = 0x4000;
}
```
NB the mode==1 loop above must be written exactly as the 68k: `for(d2=3; d2; ) { if (d1>=hp) break;
d1+=q; if(--d2==0) break; }` — d2 ends 0 when hp > 3q.

---

## 4. Move 0x38 — spring-up (table 0x12614[0x38] = 0x126F4 → cell 0x17AFE)

Cell 0x17AFE: `{code 0x17B12, mode 1 (one-shot), n=3, delays 6,6,6, sprites 0x6A,0x6B,0x6A}` → 18 ticks,
then `+0x25` parks at 0xFE.

| PC | when | write |
|---|---|---|
| 0x17B12 | `btst #7,(+0x1C)` clear = init | |
| 0x17B1A | init | `+0x01 = 1` (polar mover) |
| 0x17B20 | init | `+0x2C = 0x80` (angle: straight "toward" per facing TODO EXACT sign) |
| 0x17B26 | init | `+0x2A = 0x18` (speed) |
| 0x17B2C-0x17B36 | init, if `$1C17EE < 0x240` | `bchg #7,(+0x2D)` (flip angle 180°: keep inside some bound; $1C17EE = TODO EXACT, camera/ring x) |
| 0x17B3E | per frame | if `+0x25 != 0xFE` → rts |
| 0x17B46 | end | `state = 0x0007` (get-up) |
| 0x17B4C | end | `react_id = 0` (`clr.w +0x64`) |
| 0x17B50 | end | `bset #7,(+0x26)` = **bit31 of `partner` pointer** set (flag "rose from spring-up"; TODO EXACT consumer — state 7 handler 0x11E42 reads +0x1F/+0xFE only, so check who tests +0x26 bit31) |
| 0x17B56 | end | `+0x9C = 0` (secondary partner/link cleared) |

No writes to the opponent/partner object. No hp/dmg. Mash was already cleared by 0xEDB6.

```c
case 0x38:                                             /* 0x17B12 */
    if (!entered) { o->mover = 1; o->angle = 0x80; o->speed = 0x18;
                    if (ring_x_1C17EE < 0x240) o->angle ^= 0x8000; /* bchg #7 of +0x2D */ }
    else if (o->frame_lo == 0xFE) { o->state = 7; o->react_id = 0;
                    o->partner |= 0x80000000; o->link9c = 0; }
```

---

## 5. Held man (state 0xFF) and the standing hold 0x0C — no victim mash exists

State 0x0C cell = 0x124E2 `{code 0x124FA, mode 2 (loop), n=4, delays 0x10×4, sprites 0x39,0x3A,0x3F,0x3A}`
(state table 0x11478[0x0C] = 0x0114A8 → 0x124E2). **A0 in 0x124FA is the HOLDER** (the 0x2000 half who
draws both men); the held man is state 0xFF (skipped by all dispatchers at 0xF1C2, so never reaches
0xDE86/0xEBC4 and never calls 0x10D04). pins-referee §5 already states: "Held man (0xFF): no input, no
AI, no mash" — confirmed; there is no victim-side break in 0x124FA.

| PC | cond | write (A0 = holder, A2 = partner/held) |
|---|---|---|
| 0x124FA | init (`+0x1C` bit7 clear) | |
| 0x12502 | init | `+0x01 = 0` |
| 0x12506-0x12514 | init | `0x10B62(+0x18)` then `0x10B62(-0x18)` (clamped facing slide, net 0 but clips to ring) |
| 0x1251A/0x12520 | init | `bset #6,+0x33` on A0 and A2 (engaged) |
| 0x12526 | init | `hold_t = 0xE0` |
| 0x1252C | init | `hold_ph = 0` |
| 0x12530 | init | `+0xBC = 0` |
| 0x12534 | per frame, `+0xFE` bit15 (match over) | `state = 0x0005`, `move_id = 0x0050` → rts (0x1253C) |
| 0x1254C | per frame | `--hold_t` |
| 0x12550-0x1255E | `hold_t == 0xA0` | `hold_ph = 1` → 0x125A2 (human category-9 grapple window opens) |
| 0x12560 | `hold_t != 0` | → 0x125A2 |
| 0x12566 | **`hold_t == 0` — FLIP** | `state = 0x00FF` (holder becomes hidden) |
| 0x1256C-0x12570 | | A2 = `partner`; `A2->state = 0x000C` (held man becomes holder) |
| 0x12576 | | `+0xBC = 0` |
| 0x1257A | | `A2->grap44 = 0` (clr.w) — NB: the doc's "A1+0x44 = 0x2000" is the tie-up resolve at 0x12xxx (face-tieup §141-146), **not** here; here the new holder's +0x44 is 0 |
| 0x1257E-0x12584 | | `A2->spr = 0x0039`, then high byte := `A2->+0x2E` high byte (facing flip bit) |
| 0x1258A-0x12598 | | `exg; jsr 0x247C; jsr 0x27B8` — project + build sprite for A2 |
| 0x1259A | | `A0->spr = 0xFFFF` (hidden) |
| 0x125A2-0x125B8 | (timer running) if `grap44` bit5 (0x20) and `+0x25 == 0xFE` | `A1 = 0x12468` (play impact cell 0x1D2 once), `bclr #5,+0x44` |

The 0xFF "hidden record" 0x125C0 `{code 0x125CC, mode1, n=1, delay 0xFF00, spr 0xFFFF}`:
| PC | write |
|---|---|
| 0x125CC | `+0x01 = 0`; `spr = 0xFFFF` |
| 0x125D6-0x125EA | if `partner->partner == self` (24-bit compare) → rts (still held) |
| 0x125EE-0x1260C | else **release**: `state = 0`, `partner = 0`, `+0x9C = 0`, `+0x32 &= 0x87`, `+0x34 &= 0x300`, `+0xB4 = +0xB6 = 0` |

So the held man leaves 0xFF only when the holder stops pointing at him (holder executed a grapple move, or
the flip made *him* the holder). `grap44` bit15 (hidden-half, tie-up) / 0x2000 (draws both) are set by
the tie-up resolve (face-tieup.md §141-146), not by 0x124FA.

**What "mash-out of a standing hold" actually is:** after the holder's 0xE0-tick clock hits 0 the roles
flip (0x12566). Only *scripted* submission holds (state-5 move pairs, victim in 0x5D-0x64/0x52/0x53)
have a mash: their victim handlers call 0x10D04 each frame (callers 0x180F4, 0x18DBC, 0x18EE4,
0x194FA, 0x19600, 0x1965C, 0x1B218…), seeded by `clr +0xAA; 0x10C60(0)` at move init (0x19562-0x1962C),
and the escape is the 0xEBC4 state-5 ladder (§2 table). The +0x46 clock of 0x124FA does not interact
with +0xAA at all.

```c
static void hold_0c(eng_obj *h) {                          /* 0x124FA, h = holder */
    eng_obj *v = OBJ(h->partner);
    if (!entered) { h->mover = 0; slide_10B62(h, +0x18); slide_10B62(h, -0x18);
                    h->f33 |= 0x40; v->f33 |= 0x40; h->hold_t = 0xE0; h->hold_ph = 0; h->bc = 0; }
    if (h->result & 0x8000) { h->state = 5; h->move_id = 0x50; return; }
    if (--h->hold_t == 0xA0) { h->hold_ph = 1; }
    else if (h->hold_t == 0) {                             /* FLIP 0x12566 */
        h->state = 0x00FF; v->state = 0x000C; h->bc = 0; v->grap44 = 0;
        v->spr = 0x0039 | (v->facing & 0xFF00); project_247C(v); build_27B8(v);
        h->spr = 0xFFFF; return;
    }
    if ((h->grap44 & 0x20) && h->frame_lo == 0xFE) { play_cell_12468(h); h->grap44 &= ~0x20; }
}
```

---

## Open items (TODO EXACT)
- 0x17B50 `bset #7,(+0x26)` (partner pointer bit31) consumer.
- `$1C17EE` (0x17B2C) and `$1C0161` bit1 (0xDE8A) identities.
- +0x56 bits 6/7 semantics for the 0x10D04 gate (pins-referee says b7 CPU, b6 pinned/autopilot).
- Holder-side writes performed by each hold-escape move 0x67/0x56/0x5C/0x5B/0x59/0x5A/0x55/0x58/0x57
  on first entry (grapple-move graph; not transcribed).
