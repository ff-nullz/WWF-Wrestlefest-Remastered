# Strikes: B1/B2 press → punch/kick → landing-ready — exact semantics

Source: `../wrestlefest-decomp/reference/maincpu.asm` (read-only pass, 2026-08-22).
Companion to `docs/engine-specs/input-walk.md` (§2 controller prefix, field names reused),
`docs/engine-specs/run-skid-turn.md` (§2a run entry), `docs/engine-specs/anim.md`
(cell/handler contract, `+0x22/+0x24/+0x25` semantics),
`docs/engine-specs/frame-order.md` (pass order; `+0x30` zone byte).

Correction to the task premise up front: the attack selector's context categories are
**0..0x14 (21 rows)**, not 0..7, and the per-wrestler maps at `0xE4FE` are 21×3+1 = **0x40
bytes** each. The row/column law `D0 = cat*3 + ((+0xA3 & 3) − 1)` is exactly as stated.

---

## 0. Call chain and gates (`0xDCDE`, pass 2 `0xF18A`)

`0xF18A` per live object (skip `+0x56` bit6/bit7; require `+0x33` bit1 set and `+0x34`
bit2 clear) → `bsr 0xDCDE`:

```
0xDCDE  btst #7,(+0x20) ; beq rts     ; state word bit15 ("state committed") must be SET
0xDCE8  tst.w (+0xFE)   ; bne rts     ; no pending event (hit/grapple result outstanding)
0xDCF0  bsr 0xDD02      ; tag-partner double-team assist; carry = consumed, rts
0xDCF6  bsr 0xDE86      ; *** ATTACK SELECTOR ***       ; carry = consumed, rts
0xDCFC  bsr 0xDDC8      ; movement selector (input-walk §2)
```

So two global gates sit in front of every attack: **`+0x20` bit15 set** (a state written
after last frame's commit — e.g. by the hit pipeline — parks the pad until `0xF4E0`
re-latches) and **`+0xFE == 0`** (`+0xFE` is the async event word, written as
`0x8000+code` / `0x4000+code` by grapple/throw resolution, e.g. `0x11236`, `0x1FE76`,
`0x200FC`; consumed by the victim machinery).

`0xDD02` (before attacks, only when `$1C0161 & 3 == 0`): if new-press B1/B2 (`+0xA3 & 3`),
own `+0x32` bit0 clear, teammate `+0x86` is in state 1 with `+0xAE & 0x80FF == 0x8002`
(run-in sub 2), and own XY is in the own-corner window (side `+0x33` bit7: left needs
X<0x220 && Y≥0x186 at `0xDD4E`; right needs X≥0x2F0 && Y<0x12E at `0xDD60`): own
state 0/1 → `+0x20=5, +0x60=0x4C`; own state-5 stance 0x15/0x16 → `+0x20=5, +0x60=0x66`
(`0xDD9E/0xDDAC`). Carry set = press consumed, no attack.

---

## 1. The attack selector `0xDE86` … `0xDF96`

### 1a. Entry gates (`0xDE86-0xDED6`)

```
0xDE8A  tag mode ($1C0161 bit1) && +0x32 bit1 clear && +0x33 bit2 clear
        -> exit (not the legal man: buttons dead)                    ; 0xDEA2 -> 0xDFFC
0xDEA6  D3 = +0xA3 & 3            ; NEW-PRESS B1/B2 (the +0xA4 odd/even accumulator
        == 0 -> exit carry-clear  ;  makes a 1-frame-apart pair read as 3 = "both")
0xDEB2  clr.l D3                  ; D3 reused: "remap-eligible" flag (see §2)
0xDEB4  bsr 0xEBC4  ; carry -> consumed   ; grapple-advantage follow-ups (§1b)
0xDEBC  jsr 0xF0BA  ; carry -> consumed   ; tag-mode corner-man attack (§1b)
0xDEC6  +0x21 == 4 (victim state) -> exit carry-clear ; you can't attack while reacting
0xDED0  +0x32 bit0 set (run-in) -> 0xDF86 (short category chain: only 0xE496/0xE4BA)
0xDEDA  bsr 0xEA42  ; carry -> 0xDFE8 DIRECT: +0x20=5, +0x60=D0 (counters/joins, §1b)
```

### 1b. The three pre-category consumers

* **`0xEBC4` — follow-ups while holding grapple advantage.** Runs only when word
  `+0xAA == 0x4000` (`+0xAA` is the tie-up mash meter at `0x10C8A/0x10D28`; `0x10D2E`
  writes `0x4000` when the opponent's counter hits 0 = "you won the lock-up"). Then a
  ladder of `own (+0x21,+0x61)/(+0x65)` states picks the follow-up and **writes
  `+0x20=5, +0x60=move` itself**: state5 move 0x4A/0x51→0x4B; victim 4/anim 8→0x38;
  victim 4/anim 9 (with partner-move filter 0x35/0x0E/0x0F/0x23 + `+0x60` bit4 at
  `0xEC38-0xEC66`)→0x78; state5 0x5D→0x67, 0x5E→0x56, 0x5F→0x5C, 0x60→0x5B, 0x61→0x59,
  0x62→0x5A, 0x63→0x55, 0x52→0x58, 0x53→0x57, 0x64→0x4B. Every hit path also
  `clr.w +0xAA` (`0xEDB6`) and returns carry.
* **`0xF0BA` — tag corner-man.** Tag mode bit1, no weapon, `+0x33` bit2, state 0/1;
  `0xF106` scans the two corner objects `$1C0F1C/$1C1028` (`+0x76==0`, their `+0x1D==3`,
  |ΔX|<0x20, |ΔY|<0x20 → mutual `+0x76` link) → `+0x20=5, +0x60=0x70`, carry.
* **`0xEA42` — press-while-the-opponent-is-mid-move.** No weapon. Own state 0/1 branch
  (`0xEA64`): opponent (A1=`+0x7A`) in state 5, `D1 = +0x60 & 0x80FF`:
  `0x8009/0x801A` → prox box 3 (`0xE958`, §1e) → move **0x0A**; `0x800F/0x800E` → box 0
  → **0x0A** (all via `0xF178` partner-link); `0x8022/0x803B` → **0x72** (kick counter,
  `0xF178`); `0x8048` → join a running 2-man grapple (partner-of-partner A2 checks,
  boxes 0xE/0xF/0x10, result 0x0A or 0x49 in rumble via `+0xA3==1` test `0xEAF8`,
  with `+0x26/+0x9C` link writes `0xEB2E-0xEB3A`); `0x8049` → box 0xD → **0x0A** +
  links. Non-0/1 branch (`0xEB68`): own `+0x20 == 0x8006` (committed TURN) with
  `+0x44 != 0` and `+0x25 == 0` → chance roll `0x1129E(D1=2)` → move **0x3F**; own
  state5 move 0x7B → `bset #7,+0x60` once + roll(0) → **0x7C**. Carry → `0xDFE8`
  writes `+0x20=5, +0x60=D0` directly (no remap passes).

### 1c. Target switch (`0xDEE2-0xDF36`)

A1 = primary opponent `+0x7A`. If `A1->+0x34 & 7` ≠ 0 → skip. Mainline (own state 0, or
state 1 with `+0xAF == 0`): load the secondary opponent A1 = `+0x82`; if he is a downed
victim (`+0x21==4`, `+0x65` == 8, or 9 → skip his anim test) and inside prox box 0 of
`0xE958` → **swap: `+0x82 = old +0x7A; +0x7A = A1`** (attack the downed man instead).
Quirk kept for fidelity: from states other than {0,1 sub0} the flow reaches `0xDF24`
with A1 still = `+0x7A`, making the "swap" a self-assignment.

### 1d. Context-category chain (`0xDF3A-0xDF92` → tests `0xE002…0xE4BA`)

Each test returns carry+`D0=category` on match (`0xE028`) or carry-clear (`0xE022`);
first match wins, order exactly:

| PC | cat | condition (all exact) | extras on match |
|---|---|---|---|
| `0xE002` | 0x11 | weapon held (`+0x74` bit7) && state 0/1 | — |
| `0xE02E` | 1 or 2 | own state **2 (running)**, `+0x34` bit1 clear, `+0x44==0` | `bchg #7,+0x62` picks bank; `D0 = 0xE070[bank*0x24 + opp(+0x7A)->+0x70 * 0xC + own id +0x02]` |
| `0xE0B8` | 5 | state 0/1; opp `+0x26==0`, opp victim `+0x21==4` && `+0x65` ∈ {8,6}; `+0x34` bit1 clear | `D4=1`; `0xF178` link |
| `0xE110` | 6 / 7 | same but opp `+0x65` ∈ {9,7} | opp `+0x70==0` → 6 else 7; `0xF178` |
| `0xE180` | 8 | own state **0x0B** (front tie-up), `+0x44` high-byte bits5,6 clear | `bset #6` in partner(`+0x26`)->`+0x44` high byte |
| `0xE1B0` | 9 | own state **0x0C**, `+0x45 == 1` (timer low byte) | `clr.w +0x44` |
| `0xE1D4` | 0x0A-0x0D | own **state 5 anim 0x15/0x16** (the lock-up stance) | `clr +0x44,+0x46`; **`D3=1`** (arms §2); `bchg (opp +0x70), +0x62` picks column; `D0 = 0xE232[opp_heat*0x18 + (bit?0xC:0) + id]` |
| `0xE27A` | 0x0E / 0x0F | own state **9 (downed)** | tag mode → `bset #2,+0x33`; opp victim 8/9 or opp move 0x61 → 0x0F else 0x0E; `0xF178` |
| `0xE2DC` | 3 / 4 | opp running (`opp +0x33` bit4), own state 0/1, `+0x34` bit5 clear | **`D3=1`**; `bchg #7,+0x62`; `D0 = 0xE33A[bank*0x24 + opp_heat*0xC + id]`; `0xF178` |
| `0xE382` | 0x10 | own state 0 or 1-sub0; opp `+0x26==0`, opp victim 4/`+0x65==1`; \|ΔY\|<0x10, \|ΔX\|<0x50, same facing byte (`eor +0x2E`) | `0xF178`; `clr.w opp->+0x52` |
| `0xE40C` | 0x12 | state 0/1; opp state5 `+0x61 == 0x61` | `0xF178` |
| `0xE442` | 0x14 | state 0/1; opp state5 `+0x61` ∈ {0x25,0x36,0x53,0x52} | `0xF178` |
| `0xE496` | 0x13 | (run-in branch only) own state 1 && `+0xAF == 1` | `0xF178` |
| `0xE4BA` | 0 | fallback, both branches: `+0x34` bit1 clear, `+0x32` bit0 clear (tested twice in tag mode), state 0 or 1 | — |

No match → `0xDF92 → 0xDFF8` carry-clear exit (this is why **you cannot buffer or chain
an attack mid-move**: plain state-5 matches nothing). `0xF178` = partner link:
`+0x26 = A1; if (A1->+0x26 == 0) A1->+0x26 = A0` — the *category pick itself* links you
to the target for grapple-class rows; cat 0/1/2/8/9/0xA-0xD/0x11 do not call it (8/9
already have `+0x26`).

Category matrices (raw): `0xE070` (running) values 1/2; `0xE232` (stance) values
0x0A..0x0D; `0xE33A` (anti-run) values 3/4 — each 0x48 bytes = 2 alternation banks/cols
× opp-heat rows × 12 wrestler ids; `+0x62` is a per-object **alternation latch**
(`bchg` = strict A/B/A/B so repeat attacks from the same context vary).

### 1e. Row fetch and dispatch (`0xDF96-0xDFF6`)

```
0xDF96  A2 = *(0xE4FE + (+0x02 wrestler id)*4)      ; 12 pointers, maps stride 0x40
0xDFA6  D0 = cat*3
0xDFAC  D1 = (+0xA3 & 3) - 1                        ; col: B1=0, B2=1, B1+B2=2
0xDFB8  D0 = byte A2[cat*3 + col] & 0xFF
0xDFC2  D0 == 0xFF:  +0x20 = 2 (word, bit15 clear); clr.l +0x26   ; *** RUN ***
                      -> carry-set exit                            ; 0xDFC8-0xDFD2
        else:
0xDFD6    bsr 0xE926   ; tag-legality remap: unless (opp +0x33 bit0 && own +0x33 bit0
                       ;   && own +0x33 bit2 clear), move 0x48 -> 0x0A
0xDFDA    bsr 0xEF9A   ; proximity remap (§1f); carry -> exit (it wrote the state)
0xDFE0    if D3 != 0: bsr 0xE82E   ; facing/zone remap (§2) — stance & anti-run only
0xDFE8    +0x20 = 5 (word, bit15 clear)             ; *** SCRIPTED MOVE ***
0xDFEE    +0x60 = D0 (word)                         ; move id, low byte +0x61
0xDFF2    carry-set exit
```

**Exact writes of a successful pick: `move.w #5,(+0x20)` and `move.w D0,(+0x60)` —
nothing else.** `+0x62` is only ever `bchg`'d inside the category pickers; `+0x4C` is
NOT touched by the selector (it is the per-frame hit-record id owned by cell handlers,
§3). Wrestler 0 map `0xE52E` (21 rows × B1,B2,both):

```
cat:  0        1        2        3        4        5        6        7
     00 72 FF|2D 2D 2D|05 05 05|06 06 FF|21 21 FF|08 48 22|0A 48 0A|35 48 FF|
cat:  8        9        A        B        C        D        E        F
     29 29 29|15 17 15|18 18 18|24 24 24|30 30 30|19 19 19|1B 1B 1B|0F 0F 0F|
cat:  10       11       12       13       14      pad
     1C 1D FF|1E 1E 1E|0A 10 FF|39 1C 1C|00 72 FF|FF
```

So **B1 standing = move 0x00 (jab), B2 standing = 0x72 (kick), both = 0xFF = run**, and
0xFF anywhere in a table means "run" (categories 3, 4, 5(w1 col2), 7, 0x10, 0x12 cols).

### 1f. Proximity remap `0xEF9A` (whiff prevention)

`D0` = chosen move. Moves ≥ 0x4A: never remapped. Else `t = 0xF070[D0]` (0x4A bytes):

```
F070: idx 08:81 09:82 0A:03 0B:04 0E:06 10:05 13:05 22:88 23:06 35:0B 46:05 47:05
      48:07 49:07 — all others 00
```

* `t == 0`: keep the move (jab 0x00 and kick 0x72 land here — strikes always come out).
* `t` bit7 clear: `t` is an `0xE958` box profile; opponent inside → keep; outside →
  **`0xF02E`**: `+0xA3==3` → `+0x20=2` (run); `==1` → `+0x20=5,+0x60=0` (jab); else
  `+0x20=5,+0x60=0x72` (kick) — carry-set (grapple move degraded to a strike).
* `t` bit7 set (0x81/0x82/0x88): tiered: box (D0==8?1:0xC): inside → D0==8 keeps, others
  remap through per-wrestler byte `0xEFFE[id]` = `10 10 10 10 47 47 0B 46 10 46 10 47`;
  else box (D0==8?3:2): inside → D0==8 becomes 0x0A, others keep; else → the `0xF02E`
  strike/run fallback.

`0xE958` (D0 = profile): 8-byte records at `0xE9BA` (x_lo,x_hi,y_lo,y_hi words, x pair
`exg`+negated when the opponent faces right `+0x2E` bit7); inside = own `+0x06/+0x0A`
within `[opp+lo, opp+hi]`; carry = inside. Profile 0 = x∈[-0x20,+0x80], y∈[-0x38,+0x28].

---

## 2. The facing/zone remap `0xE82E` (table `0xE8AE`, load at `0xE852`)

**When:** only when the category test set `D3=1` — i.e. attacks chosen from the
**lock-up stance matrix (`0xE1D4`, cats 0x0A-0x0D)** or the **anti-running matrix
(`0xE2DC`, cats 3/4)**. Jab/kick (cat 0) never pass through it.

```
0xE838  side = 0xE917[ word +0x30 ]     ; +0x30 = floor/zone byte, written each frame
                                        ;   by pass 3 0x2088E from table 0x208EC (X/Y)
        0xE917 = 00 00 00 FF FF FF FF FF FF 01 01 01 00 FF   (zones 0..0xD)
        side == 0xFF -> no remap (exit)
0xE846  A1 = *(0xE8AE + id*4)           ; per-wrestler triple list, 0xFF-terminated
0xE856  for each triple (f, m, r):      ; bytes in THIS order: flag, move, replacement
          if m != D0: continue          ; first matching triple ends the scan either way
          facing = +0x2E bit7           ; 0 = left, 1 = right
          replace (D0 = r) iff  f == (side==0 ? facing : !facing)
          else keep D0
```

I.e. each triple is a **mirrored-variant pair rule**: `f` says which facing the
replacement is for, and `side` (which lateral zone band of the arena you stand in:
zones 0-2 and 0xC are class 0, zones 9-0xB class 1, everything else exempt) flips the
sense, so the animation plays toward the correct side. Lists:

```
w0 0xE8DE: (00,30,19) (01,19,30) (01,18,24)      w6 0xE902: —
w1 0xE8E8: (01,18,24)                            w7 0xE903: (00,2B,1A)(01,1A,2B)(01,18,02)
w2 0xE8EC: (01,33,24) (01,18,24)                 w8 0xE90D: —
w3 0xE8F3: (01,33,24) (01,18,24)                 w9 0xE90E: (01,19,2C)
w4 0xE8FA: (01,2B,2C)                            wA 0xE912: (01,19,2C)
w5 0xE8FE: (00,30,24)                            wB 0xE916: —
```

---

## 3. State 5 machinery: the jab and the kick

### 3a. Dispatch

* Pass-2 `0xF1E4[5] = 0xF462`: anims 0x3C/0x3D/0x3E → `+0x20=0`; anim 0x16 (moving
  stance) → `+0x2C = 0xF2D6[+0xA8]` + `bsr 0x10E86`; **everything else — including a
  jab/kick in flight — does nothing**. All strike logic lives in the anim-cell handler.
* Anim core `0x1C03E`: `+0x1D == 5` → cell = `*(0x12614 + (+0x61)*4)`; handler runs
  every frame *before* the tick (anim.md §1b). `0x12614[0] = 0x12850` (jab),
  `0x12614[0x72] = 0x19FC6` (kick).

### 3b. Jab — move 0x00, cell `0x12850`

```
cell 0x12850 = { handler 0x12888, mode 1 (hold-last), n 5,
                 dur  000A 0006 0004 000A 0002        ; displayed d+1 = 11,7,5,11,3 ticks
                 spr  0000 0092 0093 0094 0093 }      ; frame 3 (spr 0x94) = the punch
alt  0x1286C = { handler 0x12888, mode 1, n 5, dur 5,5,5,9,2, same sprites }
```

Handler `0x12888` (every frame):

```
first frame (+0x1C bit15 clear):
  0x12890  clr.b +0x01                 ; mover OFF — a jab never moves you
  0x12894  D0 = -0x18; bsr 0x10B62    ; forward-space probe: +0x3E = D0 (facing-
           ; mirrored inside 0x280DC at 0x28112: probes the point 0x18px IN FRONT),
           ; one floor pass, X += pushback +0x38, +0x36 preserved, +0x58 cleared if
           ; the probe clipped an X bound — i.e. you get nudged off the ropes so the
           ; punch stays in bounds
committed frames:
  0x1289E  +0x4C = 0x21                ; hit-record id: windup/recovery
  0x128A4  if word +0x24 == 3: +0x4C = 0x07     ; ACTIVE frame (sprite 0x94)
  0x128B2  if byte +0x25 == 0xFE:               ; mode-1 finished sentinel
             +0x20 = 0                          ; *** EXIT to state 0 ***
             bset #7,(+0x26)                    ; poison partner link (§3e)
tail (both branches):
  0x128C6  if +0x56 bit7 (CPU) && +0x33 bit5 (hustle): A1 = 0x1286C
           ; per-frame cell substitution -> AI jabs are 11 ticks faster
```

### 3c. Kick — move 0x72, cell `0x19FC6`

```
cell 0x19FC6 = { handler 0x19FDA, mode 1, n 3,
                 dur 000A 000A 000A                    ; 11,11,11 ticks
                 spr 0000 01CD 01CE }                  ; frame 2 (spr 0x1CE) = the kick
```

Handler `0x19FDA`:

```
first frame:  clr.b +0x01 only (no space probe, no cell substitution)
committed:    +0x4C = 0x01                    ; same id as plain standing
              if +0x24 == 2: +0x4C = 0x11 ; ACTIVE (and skips the exit test that tick)
              elif +0x25 == 0xFE: +0x20 = 0 ; bset #7,(+0x26)   ; exit as the jab
```

Both moves show sprite 0 (neutral stand) for the first 11 ticks — the windup — and both
**exit by `move.w #0,(+0x20)`** on the handler call after the tick that set
`+0x24 = 0xFE`; the pass-4 latch `0xF4E0` then re-enters state 0 (stand cell `0x114AC`
zeroes mover/velocity block and resumes `+0x4C = 1`).

### 3d. When the strike actually connects — `+0x4C` and the even-frame hit SM

`+0x4C` (word) is the object's **hit/hurt record id**, re-written every frame by
whatever handler runs, and cleared by:

* `0x24090` (tail of pass 11 `0xF4C2`): whenever the displayed sprite `+0x04` differs
  from the shadow `+0x90` → `clr +0x8E` (per-victim once-mask), `clr +0x4C`,
  `+0x90 = +0x04`. Ordering matters: handler writes `+0x4C` → `0x24090` may clear it →
  pass 12 reads it. **So the tick on which a new sprite frame loads never hits**; the
  active window of the jab is ticks 2..11 of frame 3 (10 chances), kick ticks 2..11 of
  frame 2.
* `0x24DEC/0x24E02` after a successful application (below).

`0x24062` (**even frames only**): for every object with `+0x4C != 0` and `+0x20` bit15
set: record `A2 = 0x24EF6 + (+0x4C & 0xFF)*7` (`0x240BC`). Record byte0 bit7 set → scan
all other live objects as victims (`0x240D8`):

7-byte attack record `{flags, hurtbox1, hurtbox2, attackbox, damage, atk_apply, vic_apply}`:

```
id 0x01 (kick windup, stand, skid): 40 00 00 00 00 00 00   ; hittable, hurtbox 0
id 0x21 (jab windup):               40 00 00 00 00 00 00
id 0x07 (JAB ACTIVE):               C0 00 00 06 01 01 00   ; active + hurtbox 0
id 0x11 (KICK ACTIVE):              80 00 00 05 02 08 00   ; active, NO hurtbox:
                                                           ;   the kicker is unhittable
                                                           ;   on his active frames
id 0x03 (running, for context):     C0 00 00 09 00 02 00
```

flags: bit7 = "is an active attack" (attacker gate `0x24080`); bit6/bit5 = "this object,
as a victim, exposes hurtbox byte1 / byte2" (`0x2420A/0x2422A` — both clear ⇒ invulnerable).

Victim filter `0x24126`: victim `+0x4C != 0`, `+0x20` bit15 set, `+0x32` bit6 clear,
`+0xFE == 0`, (`+0x32` bit2 ⇒ tag mode only), sprite ≠ 0xFFFF, attacker's `+0x8E`
slot-bit for this victim clear (`0x241B6`, once per victim per sprite frame), coarse
|ΔY| < 0x0C (−8 on victim if his `+0x4D == 0x14`), |ΔX| < 0x50.

Fine test `0x241E8`: attack box = `0x24FE4[att_rec[3]]`, hurt boxes =
`0x25010[vic_rec[1]/[2]]`, each 4 bytes `(x_lo, z_lo, x_hi, z_hi)`; X pair negated+
swapped by the object's own sprite flip `+0x04` bit7, applied around `+0x06`; Z pair
around `+0x0E`. Jab box 6 = `C0 00 FF 60` → x∈[front+1, front+0x40], z∈[0,0x60]; kick
box 5 = `C8 00 FF 60` → reach 0x38. Hurtbox 0 = `F0 00 20 60`.

Apply `0x2435A`: `jsr 0x24398[att_rec[5]]`; carry clear → `jsr 0x243D4[vic_rec[6]]`,
then `0x24DEC` + `0x24E02`:

* **Jab apply `0x24408`** (idx 1): victim `+0x4C` bit14 set → no effect (bit14 = strike-
  proof record, e.g. the `0x401A` writer at `0x1AC0A`). Else: sounds 0x2A + 0x32
  (`0x2052`); **victim `+0x69` = rec[4] = 1 (damage)**; victim `+0x20 = 4`,
  `+0x64 = 0` (flinch); if victim `+0x4C` bit15 clear: victim running (`+0x33` bit4) or
  **combo `+0x52 ≥ 3` → `+0x64 = 2` (knockdown)**; `0x24D7A`; `0x24DDC`.
* **Kick apply `0x246EA`** (idx 8): sound 0x2B; **victim `+0x69` = 2**; `+0x20 = 4`,
  `+0x64 = 0x0A` (kick flinch); same bit15 / running / `+0x52 ≥ 3` → `+0x64 = 2`;
  `0x24D7A`; `0x24DDC`.
* `0x24D7A`: if victim `+0x65 < 0x24` and attacker facing == victim facing (**hit from
  behind**): `+0x65 = 0x24DB8[+0x65]` (0→0x0B, 1→0x0C, 2→0x0B, 3→0x0C, 4→0x0D,
  5..0xF identity, 0x10+→0x0B…) — back-reaction anims.
* `0x24DDC`: victim running → `+0x68 += 1` (bonus damage on a runner).
* Victim-side `0x243D4[0] = 0x24852`: `bset #7,(+0x26)` on BOTH objects, victim
  `+0x44 = 0`, `0x24D7A` again (idempotent).
* `0x24DEC` (attacker post): set `+0x8E` victim bit (`0x24334`); attacker `+0x44 = 0`,
  `+0x46 = 0`, **`+0x4C = 0`**, `+0x92 = victim` (last-hit link). One landed hit per
  swing: the handler re-writes `+0x4C=7` next tick but the `+0x8E` bit blocks that
  victim until the sprite changes — and the next sprite frame writes the inactive id.
* `0x24E02` (victim post): drop weapon (`+0x74/+0x76/+0x44`), `+0xC6 += 1` (times-hit),
  `+0x4C = 0`, `+0x46 = 0`, `+0x18/+0x1A = 0`, `bclr #4,+0x33`, `+0x92 = attacker`,
  `+0x70 == 2 → bset #7,+0x64` (band-2 victim cell, anim.md).

**Damage lands later**: `0x24E58` (even frames): if `+0x68 != 0 && +0xFE == 0`:
`+0x66 (HP) −= +0x68` (floor 0), `0x24EC2` HUD arm, `+0x6A = −(+0x68)` (meter smear for
`0x7548`), `clr +0x68`. Then stun decay: `+0x54 −= 1`, borrow → `+0x54 = 0`,
**`+0x52 = 0`**. The victim flinch cell handlers (`0x18E14/0x18F16/0x18F40`) do
`+0x52 += 1; +0x54 = 0x40` — so three flinches inside the ~64-even-frame window
escalate the next strike into a knockdown.

### 3e. The `bset #7,(+0x26)` at move end / on hit

Sets bit31 of the partner pointer long. Consumers tolerate it: the anim core masks to
24 bits via the bus; `0x115D2` (stand/walk sanity, every frame) sees `+0x26 != 0`,
dereferences (24-bit wrap), and the 32-bit back-pointer compare `A0 == partner->+0x26`
now fails (either side poisoned) → `clr.l +0x26; clr.l +0x9C` on the next frame — i.e.
**bit31 is a lazy "divorce this pair" mark**. If `+0x26` was 0 it becomes `0x80000000`,
`0x115D2` reads ROM `0x000026` as the back-pointer, compare fails, cleared — same net
effect. A C port needs the same tolerance (mask + treat-nonzero-as-stale).

---

## 4. What gates a new attack

* **`+0x20` bit15 must be set** (`0xDCDE` gate): any pending un-committed state write
  (hit pipeline, AI) freezes the pad for that frame.
* **`+0xFE == 0`** (`0xDCDE`): outstanding grapple/damage event = no input.
* **New-press only**: `+0xA3` is `new & ~held` (`0x51BC`); holding B1 does nothing;
  there is no buffering — a press during a move is simply lost.
* **No attack during an attack**: mid-move (state 5, anim not 0x15/0x16) every category
  test fails, so `0xDE86` exits carry-clear. The `+0xFE` latch is *not* what blocks it —
  it is the category chain. Exceptions that *do* read buttons mid-state-5: the stance
  pair 0x15/0x16 (cats 0x0A-0x0D), move 0x7B (`0xEA42` → 0x7C), moves 0x4A/0x51/0x5D-
  0x64/0x52/0x53 under `+0xAA == 0x4000` (`0xEBC4` follow-ups).
* **Not while a victim** (`+0x21 == 4`, `0xDEC6`) and **not while a run-in is active**
  except cats 0x13/0 (`+0x32` bit0 branch `0xDF86`).
* **Tag legal-man**: `$1C0161` bit1 set and `+0x33` bit2 clear (and `+0x32` bit1 clear)
  → buttons dead (`0xDE94-0xDEA2`).
* Cat 0 itself needs `+0x34` bit1 clear (no incoming grapple request) — otherwise the
  press falls through and the grapple proceeds.
* On the *receiving* side, `+0x4C` gates hittability every frame: id with flags bit6/5
  clear (kick active 0x11) = invulnerable; `+0x4C == 0` (first tick of any sprite
  frame, or just-hit) = untargetable; `+0x4C` bit14 = strike-proof; bit15 (ids
  0x8014/0x8015, whip context) = no knockdown escalation and no behind-remap/run-bonus.

---

## 5. C sketch

New object fields beyond input-walk/run-skid specs (68k offsets):

```c
uint16_t move_id;     /* +0x60 word (bit15 = "variant used" latch); +0x61 = index  */
uint8_t  alt62;       /* +0x62: alternation latch, bchg'd by category pickers      */
uint16_t adv_aa;      /* +0xAA: grapple-advantage word; 0x4000 = follow-up window  */
uint16_t zone30;      /* +0x30: floor/zone byte from 0x2088E/0x208EC (pass 3)      */
uint16_t hit_id;      /* +0x4C: hit record id; bit14 strike-proof, bit15 no-combo  */
uint16_t hitmask8e;   /* +0x8E: per-victim already-hit bits; +0x90 = last sprite   */
uint16_t dmg68, hp66, smear6a;      /* +0x68 pending damage -> +0x66 HP, +0x6A HUD */
uint16_t combo52, stun54;           /* +0x52 flinch count, +0x54 its decay timer   */
uint16_t probe3e, push38;           /* +0x3E probe X offset, +0x38 X pushback      */
uint32_t partner26;   /* +0x26: bit31 = stale mark; +0x9C reverse link             */
uint32_t lasthit92;   /* +0x92 */   uint16_t timeshit_c6;   /* +0xC6 */
```

```c
/* ---- 0xDE86: attack selector (human pass 2, after dcde gates). ---- */
int attack_select(obj *o)                       /* returns 1 = press consumed */
{
    if (tag_mode_bit1() && !(o->f32 & 2) && !(o->f33 & 4)) return 0; /* 0xDE8A */
    unsigned btn = o->btn_new & 3;                                   /* 0xDEA6 */
    if (!btn) return 0;
    int remap_ok = 0;                                                /* D3 */
    if (adv_followup(o))  return 1;   /* 0xEBC4: +0xAA==0x4000 ladder, writes state */
    if (tag_cornerman(o)) return 1;   /* 0xF0BA: state5 move 0x70                   */
    if (o->state == 4) return 0;                                     /* 0xDEC6 */
    unsigned cat, mv;
    if (o->f32 & 1) {                                                /* run-in 0xDF86 */
        if (!cat_runin(o,&cat) && !cat_base(o,&cat)) return 0;  /* 0xE496 / 0xE4BA */
    } else {
        if (counter_vs_move(o)) return 1;      /* 0xEA42 -> state5 +0x60=D0, no remap */
        retarget_downed(o);                    /* 0xDEE2-0xDF36: +0x7A/+0x82 swap    */
        if (!category(o, &cat, &remap_ok)) return 0;   /* the 0xDF3A chain, table §1d */
    }
    mv = attack_map[o->wrestler_id][cat*3 + (btn-1)];                /* 0xDF96-0xDFBE */
    if (mv == 0xFF) { o->state_req = 2; o->partner26 = 0; return 1; }/* RUN  0xDFC8 */
    mv = tag_048_remap(o, mv);                                       /* 0xE926 */
    if (prox_remap(o, &mv))            /* 0xEF9A: wrote run/jab/kick itself  0xDFDA */
        return 1;
    if (remap_ok) mv = zone_face_remap(o, mv);                       /* 0xE82E */
    o->state_req = 5; o->move_id = mv;                               /* 0xDFE8-0xDFEE */
    return 1;
}

/* ---- 0xE82E: zone/facing move remap (stance + anti-run picks only). ---- */
unsigned zone_face_remap(obj *o, unsigned mv)
{
    static const uint8_t side_tbl[14] =                              /* 0xE917 */
        {0,0,0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,1,1,1,0,0xFF};
    unsigned side = side_tbl[o->zone30];
    if (side == 0xFF) return mv;
    for (const uint8_t *t = remap_list[o->wrestler_id]; t[0] != 0xFF; t += 3) {
        if (t[1] != mv) continue;                 /* (flag, move, replacement) */
        unsigned facing = (o->facing >> 7) & 1;
        return (t[0] == (side ? !facing : facing)) ? t[2] : mv;      /* 0xE868-0xE89E */
    }
    return mv;
}

/* ---- 0x12888: jab cell handler (cell 0x12850; AI-fast cell 0x1286C). ---- */
const cell *jab_tick(obj *o, const cell *c)
{
    if (!o->anim_latched) {
        o->mover = 0;                                                /* 0x12890 */
        forward_space_probe(o, -0x18);   /* 0x10B62: +0x3E=-0x18 (facing-mirrored in
                                            0x280DC), x += pushback, keeps +0x36 */
    } else {
        o->hit_id = 0x21;                                            /* 0x1289E */
        if (o->frame == 3) o->hit_id = 0x07;    /* ACTIVE: box 6, dmg 1  0x128A4 */
        if ((o->frame & 0xFF) == 0xFE) {                             /* 0x128B2 */
            o->state_req = 0;                                        /* 0x128BA */
            o->partner26 |= 0x80000000u;         /* lazy unlink       0x128C0 */
        }
    }
    return (o->cpu && (o->f33 & 0x20)) ? cell_1286C : c;             /* 0x128C6 */
}

/* ---- 0x19FDA: kick cell handler (cell 0x19FC6). ---- */
const cell *kick_tick(obj *o, const cell *c)
{
    if (!o->anim_latched) { o->mover = 0; return c; }                /* 0x19FE2 */
    o->hit_id = 0x01;                                                /* 0x19FE8 */
    if (o->frame == 2) o->hit_id = 0x11;        /* ACTIVE: box 5, dmg 2, no hurtbox */
    else if ((o->frame & 0xFF) == 0xFE) {                            /* 0x19FFE */
        o->state_req = 0; o->partner26 |= 0x80000000u;               /* 0x1A006 */
    }
    return c;
}

/* Hit side (even frames, 0x24062 after 0x24090's sprite-change clear of +0x4C/+0x8E):
   record = atk_rec[o->hit_id & 0xFF]; jab: {C0,0,0,6,1,1,0} kick: {80,0,0,5,2,8,0};
   apply: victim +0x69 = dmg; +0x20 = 4; +0x64 = jab?0:0x0A, ->2 if victim running or
   victim->combo52 >= 3; behind-hit remap 0x24DB8; +1 dmg if running; then attacker
   +0x4C=0, +0x8E |= victim bit, victim +0x4C=0, +0xC6++, both +0x26 |= bit31.
   0x24E58 drains +0x68 into +0x66 and decays +0x54/+0x52. */
```

### Open labels

* Move-id names beyond jab/kick are unconfirmed (0x2D/0x05 running attacks, 0x1B/0x0F
  down attacks, etc.) — mechanics and table cells above are exact, names are not.
* `+0x52 ≥ 3` uses the *victim's* counter; the writers found (`0x18E14/0x18F16/0x18F40`)
  are flinch handlers — other writers may exist in the victim-cell bank.
* `+0x32` bit2's tag-mode victim filter (`0x2414C-0x2415C`) — observed gating only.
* `0xE070/0xE232/0xE33A` heat rows use opp `+0x70` values 0..2 ("grapple meter" per
  run-skid spec); semantics of `+0x70 == 2` beyond the band-2 victim flag untraced.
