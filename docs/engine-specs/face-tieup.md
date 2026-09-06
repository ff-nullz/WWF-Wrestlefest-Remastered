# Facing + tie-up entry — exact semantics from reference/maincpu.asm

Source: `../wrestlefest-decomp/reference/maincpu.asm`. Object stride is
0x10C, wrestler slot pointer table at `0x5122` (9 slots). Word at `+0x20` =
state: **low byte (`+0x21`) = state number**, high byte = flags (bit7 = "state
latched", set by pass-1 `0xF4EC` after the first frame in a state; bit6 set by
the state-2 handler `0xF34C`). State dispatch `0xF1CA-0xF1DE`:
`jmp 0xF1E4[state & 0xFF]`, state 0xFF is skipped entirely (`0xF1C2`).

---

## 1. `face_opponent` — 0x10BE8

```
010BE8  movem.l D0-D1/A1,-(A7)
010BEC  A1 = *(A0+0x7A)              ; OPPONENT POINTER: long at +0x7A, no slot scan
010BF0  D0 = A1->x (+0x06)           ; opponent world X
        ; ---- lead selection (D1), default = NO lead ----
010BF4  if (A1+0x21)==4 {            ; opp state 4 (action/anim +0x64/65 machine)
010BFC     D1=0x40; if (A1+0x65)==8  goto apply     ; running anim 8 -> lead 0x40
010C08     D1=0x10; if (A1+0x65)==9  goto apply     ; running anim 9 -> lead 0x10
           else goto compare (no lead)
010C16  } else if (A1+0x21)==5 {     ; opp state 5 (reaction/anim +0x60/61 machine)
010C1E     if (A1+0x61)==0x22 { D1=0x30; goto apply }   ; whip/stagger anim 0x22
010C2C     if (A1+0x61)==0x52 { D1=0x10; goto apply }   ; anim 0x52
           else goto compare
        } else goto compare          ; any other opp state: lead = 0
        ; ---- apply lead in the OPPONENT'S movement direction ----
010C38  if (A1+0x2E) bit7 set: D0 -= D1  else D0 += D1   ; opp facing-right => opp
                                                         ; moving right, lead ahead
        ; ---- compare & write ----
010C46  cmp.w (A0+0x06),D0           ; D0 - own_x, UNSIGNED
010C4A  if (D0 >= own_x)  bset #7,(A0+0x2E)   ; opponent right of me -> face right
        else              bclr #7,(A0+0x2E)
010C5A  rts
```

- **Opponent found via the long pointer at `+0x7A`** (current opponent). It is
  written at match setup (`0x2149A`, `0xDF36`, `0x1E5E2`), by AI (`0x1D4BE`),
  and re-written by the tie-up entry tail (`0xF7CE/0xF7D2`). `+0x26` is the
  *engaged-grapple partner* (NULL when free); `+0x86` is the *tag partner* —
  neither is used here.
- **Writes bit7 of the byte at `+0x2E` only** (word `+0x2E` is the sprite
  flip mask 0x0000/0x8000 — anim.md). It does **not** touch `+0x2C/+0x2D`
  (movement angle): on the walk path the angle was already written from
  table `0xF2D6[+0xA8*2]` immediately before the call (`0xF2B0-0xF2C0`).
- **No hysteresis**: plain unsigned `>=` every frame; equal X ⇒ face right.
  The "lead" exists only while the opponent is in state 4 anim 8/9 or state 5
  anim 0x22/0x52 (run/whip), and is signed by the *opponent's* facing so you
  turn toward where a runner is about to be.
- **Callers**: state 0 stand handler `0xF236` (inside `0xF218`), walk handler
  `0xF2C2` (state 1 sub-state 0, `0xF292`); plus the CPU/AI paths
  `0x1C2D6, 0x1C856, 0x1C944, 0x1C9F6, 0x1CA1E, 0x1D088, 0x1D116`.
  On both stand and walk paths the call is skipped when `+0x34` bit1 is set
  (tag-corner recall request: state:=1/+0xAE:=4 if `+0x33` bit0 clear, else
  just clears the bit — `0xF218-0xF242`, `0xF292-0xF2D4`).
- The AI has a *separate near-clone* `0x1C228` that computes distance (not
  facing): same opponent leads but 0x40/0x28 (state 4 anim 8/9 only), stores
  `|dx|` to `+0xB0` and sets `+0xB5` bit0 when the opponent is to the right.

## 2. Pass-2 tie-up scan — 0xF560 → 0xF574

`0xF560` (pass 2) runs `0xF574` (tie-up scan), then `0xF98C` (round/fall
manager), `0xFD00` (enrage-meter tick), `jsr 0xA0E0`.

### 2.1 Pair iteration (0xF574)

```
F574  D0 = 0
F576  loop: jsr 0x250E          ; next active object: scans slot table 0x5122[D0],
                                ; D0=0..8, returns A0 with byte +0x00 bit7 set,
                                ; D0=index+1; carry set = list exhausted -> rts
F580  bsr 0xF7E8 (A0)           ; eligibility self;  carry set -> next object
F586  A1 = *(A0+0x7A)           ; the pair is (object, ITS OPPONENT +0x7A) — not all pairs
F58A  bsr 0xF7E8 (A1)           ; eligibility opponent (via exg A0/A1); carry -> next
```

Each engaged pair would be visited from both sides, but the first success sets
`+0x26` on both, which makes `0xF7E8` fail on the second visit.

### 2.2 Eligibility — 0xF7E8 ("pair_collide_ok", per object; carry clear = OK)

In order, ALL must hold (any failure ⇒ `ori #1,CCR`, carry set):

1. `+0x74` bit7 clear — not holding a weapon (`0xF7E8`)
2. word `+0xFE` == 0 — no hit/damage latch pending (`0xF7F2`)
3. `+0x32` bit4 clear (`0xF7F8`)
4. `+0xB7` bit7 clear (`0xF800`)
5. byte `+0x20` bit7 SET — state latched ≥ 1 frame (not the entry frame) (`0xF808`)
6. `+0x32` bit0 clear (`0xF810`)
7. if global `0x1C0161` bit1 set (outside-ring brawl phase): `+0x33` bit2 must
   be SET (`0xF818-0xF828`)
8. state gate (`0xF82A-0xF864`): `+0x21` ∈ {0 (stand), 1 (walk)}, or
   (state 4 AND `+0x65` ∈ {0,1}), or (state 5 AND `+0x61` == 0x72)
9. long `+0x26` == 0 — not already engaged with someone (`0xF866`)

### 2.3 Pair gates (0xF594-0xF5DC), in order

1. **Facing opposed**: `(+0x2E byte of A0) XOR (+0x2E byte of A1) != 0`
   (`0xF594-0xF5A0`; whole byte XOR, in practice only bit7 differs).
   Equal facing ⇒ no tie-up ⇒ **you walk through an opponent's back**.
2. **Same side of the ring**: bit2 of `+0x33` equal on both (`0xF5A2-0xF5B4`).
3. `|A1->x - A0->x| < 0x48` — unsigned abs of word `+0x06` delta, `bcs`-style
   `cmpi #0x48 / bcc fail` (`0xF5B6-0xF5C8`).
4. `|A1->y - A0->y| < 0x10` — word `+0x0A` (depth axis) (`0xF5CA-0xF5DC`).

All four pass ⇒ `bsr 0xF5E4` (resolve), then the scan CONTINUES with the next
slot (multiple pairs can engage in one frame).

### 2.4 Resolution — 0xF5E4

**Attract mode** (`0x1C007C == 0`; bit15 of that word is set when a human
joins, `0x9B2`, cleared back to attract `0x924`): instant, no lockup —
`if (A0+0x66 >= A1+0x66)` (unsigned, `0xF5F0-0xF5FC`) A0 takes the hold
(→ `0xF760`) else A1 does (→ `0xF742`).

**Live game** (`0x1C007C != 0`), `0xF600-0xF724`:

- Cleanup on BOTH: `+0x52 := 0`; bclr bit2 `+0x34`; if `+0x33` bit1 (human)
  → bclr bit6 `+0x56` (autopilot off); bclr bits7,6 of `+0x34`.
- **Enrage auto-win** (`+0x33` bit5 = "on fire", set by the meter code
  `0xFDA2`), priority order: human A0 (bit7 `+0x56` clear) enraged → A0 wins;
  human A1 enraged → A1 wins; CPU A0 enraged → A0 wins; CPU A1 enraged →
  A1 wins (`0xF648-0xF68C`).
- **Runner loses**: A0 in state 4 anim 1 → A1 wins; A1 in state 4 anim 1 →
  A0 wins (`0xF690-0xF6B0`).
- **Human(A0) vs CPU(A1)** (`+0x56` bit7 clear/set, `0xF6B4-0xF70A`):
  `D2 = table[(A0+0x48)*2]` where table = `0xF8D0` if `0x1C0161` bit0 set,
  else `0xF878 + difficulty(0x1C0162)*8`; `D2 += rng(0x21B4) & 0xF`;
  `D2 >= 0x14` (signed bpl) → **A0 wins**; `D2 < 0xF` (bmi) → **A1 wins**;
  0xF..0x13 → **lockup 0x0B**. (`0xF878` rows per difficulty, 4 words each:
  {14,0C,11,00},{14,00,10,0C},{14,0B,00,0E},{0E,14,0B,00},… `0xF8D0` =
  {14,0E,00,09}.) `+0x48` is the per-object exchange counter.
- **Every other pairing** (`0xF70C-0xF724`): `D4 = (byte 0x1C0083 >> 1) & 7`
  (frame counter parity): `D4 >= 5` → A0 wins; `D4 < 2` → A1 wins; 2..4 →
  lockup 0x0B.

### 2.5 Writes on success

- **Lockup** `0xF726`: `A0->state20 := 0x000B`, `A1->state20 := 0x000B`
  (word write ⇒ latch bit cleared ⇒ entry frame), `A0+0x44 := 0x2000`,
  `A1+0x44 := 0x8000`.
- **A1 wins** `0xF742`: `A0->state20 := 0x00FF` (held victim),
  `A1->state20 := 0x000C`, `A1+0xBC := 0`, `A0+0x44 := 0`, `A1+0x44 := 0x2000`.
- **A0 wins** `0xF760`: mirror — `A0 := 0x000C`, `A0+0xBC := 0`,
  `A1 := 0x00FF`, `A0+0x44 := 0x2000`, `A1+0x44 := 0`.
- **Common tail** `0xF77E-0xF7E0` (all three outcomes):
  - **snap BOTH to the midpoint**: `x := (x0+x1)>>1` into both `+0x06`,
    `y := (y0+y1)>>1` into both `+0x0A` (no facing writes, no Z writes);
  - `+0x48 := (+0x48 + 1) & mod 4` on each (advances the bias-table row);
  - partner pointers: `A1+0x26 := A0`, `A0+0x26 := A1`, and `+0x7A` of each
    set to the other;
  - `bset #6, +0x33` on both ("engaged" flag).

`+0x44` flag word (per the state 0x0B/0x0C anim callbacks): bit15 (0x8000) =
"hidden half" — sprite forced 0xFFFF; bit13 (0x2000) = "play impact cell
first"; bit14 (0x4000) = set on the PARTNER when this wrestler loses a mash
tick (`0x1F040`).

## 3. Body overlap while walking — there is NONE

There is **no wrestler-vs-wrestler separation or pushback anywhere**:

- The movement integrator `0x2208` (angle `+0x2D` × speed via `0x22C0`
  sin/cos, or ballistic `+0x58/5A/5C`) adds velocity with no body test.
- The clamp machinery `0x280DC/0x28124` dispatches per-arena **ring geometry**
  handlers only (table `0x28154`, selected by arena `0x1C007E` and `+0x32`
  bit2), e.g. `0x2818E`: Y clamped to 0x118..0x198, X/Z ring bounds —
  writing pushback deltas into `+0x38/3A/3C`. No opponent term.
- `0x10E86` (called after face_opponent on the walk path) is not a pushback
  either: it is the **tag-corner trigger** — via tag partner `+0x86`, it wakes
  the apron partner (state/sub `+0xAF` 1↔2) when you cross X 0x230/0x300 or
  Y 0x170/0x150 thresholds.
- `0xF7E8`/`0xF574` is purely the tie-up *detector*; it moves nobody until a
  tie-up fires (then it snaps both to the midpoint).

So: walking into an opposed-facing opponent within (|dx|<0x48, |dy|<0x10)
⇒ tie-up; walking into his back, or any gate failing ⇒ **sprites simply
overlap and pass through**. The engine needs no separation rule.

## 4. States 0x0B / 0x0C (short — full machine is a later phase)

Three tables per state: movement `0xF1E4` (rows 0x0B=`0xF4BC`, 0x0C=`0xF4BE`
are both plain `rts` — no movement), anim `0x11478` (dispatcher `0x1C06A`,
indexed by latched state `+0x1C`; state 0xFF uses hidden record `0x125C0`),
combat/input `0x1C1F4` (dispatcher `0x1C1D2`; rows 0x0B=`0x1EFAA`,
0x0C=`0x1F06A`).

**Anim record layout** (`0x1C0C8/0x1C12C`): `+0` code ptr, `+4` mode (0 none,
1 one-shot [frame idx parks at 0xFE], 2 loop), `+6` frame count n, `+8` n
delay words, `+8+2n` n cell words (each stored to `+0x04` XOR word `+0x2E`).

- **State 0x0B — collar-and-elbow lockup (mash contest)**, record `0x12450`:
  code `0x12474`, loop, 4 frames, delay 0x10 each, **cells 0x39,0x3A,0x39,0x3B**
  (two-man pose drawn by the 0x2000-half; the 0x8000-half's sprite is 0xFFFF).
  Entry (`0x12474`): nudge X by +0x18 via the clamped mover (`0x10B62`), set
  bit6 `+0x33` both. `+0x44` bit13 ⇒ first show one-shot record `0x12468`
  (**impact cell 0x1D2**, delay 0x10) then the loop. `+0xFE` bit15 ⇒ bail to
  state 5 anim 0x50. Mash (`0x1EFAA`): countdown `+0xBD` seeded 0x20 (human)
  or `0x1F060[difficulty]` (CPU: 32 32 07 07 06 06 03 05 05 04 04 03) plus
  `rng&3`; on expiry a weighted pick `0x24CC` over `0x1F05E`={0x32,0x32}
  (~50/50) — a 0 sends this wrestler to state 5 anim 0x29 (stagger) and sets
  the partner's `+0x44` bit14 (he won). `0x1F06A` bumps `+0xBD` (cap 0x30).
- **State 0x0C — grapple hold won (rear waist-lock)**, record `0x124E2`:
  code `0x124FA`, loop, 4 frames, delay 0x10, **cells 0x39,0x3A,0x3F,0x3A**.
  Entry: X nudges +0x18 then -0x18 via `0x10B62`, `+0x46 := 0xE0` hold timer,
  `+0x45 := 0`, `+0xBC := 0`. Timer (`0x1254C`): at 0xA0 set `+0x45 := 1`;
  at 0 the hold FLIPS — self := 0xFF, partner (`+0x26`) := 0x0C, partner cell
  0x39^facing. The victim (0xFF) is skipped by all three dispatchers and gets
  carried by the holder (`0x10B9A`: copy pos + facing-signed offset).
- **Mash/grapple threshold helpers — `0x11322` family** (called from the
  grapple-move machinery, jump-vector data around `0x112B8`): all compare
  `rng(0x21B4) & 0xF` against a byte threshold, success = carry clear when
  rng > threshold (`0x11362`). `0x11322`: table `0x113CD + difficulty*5`,
  index `min(D2,4)` (D2 = exchange counter `+0x48`); `0x11338`: table
  `0x113FF`; `0x11340`: `0x11404`; `0x11368/0x11370`: tables
  `0x1140F`/`0x11409` (or `0x1140C` when the `+0x7A` opponent is human),
  indexed by `+0x70`.

## 5. C sketch

```c
/* new object fields (offsets are ROM object offsets, stride 0x10C) */
struct wf_obj {
    u16 state20;       /* +0x20: hi=flags (b7 latched, b6), lo=state (+0x21) */
    u16 x, y, z;       /* +0x06, +0x0A, +0x0E */
    u16 flip;          /* +0x2E: 0x0000/0x8000, bit15 = facing right */
    struct wf_obj *opp;     /* +0x7A  current opponent (facing target)   */
    struct wf_obj *partner; /* +0x26  engaged grapple partner, NULL free */
    u16 grap_flags;    /* +0x44: b15 hidden-half, b14 lost-tick, b13 impact-cell */
    u16 exch;          /* +0x48: tie-up exchange counter, mod 4 */
    u8  fl33, fl34;    /* +0x33 (b0 tag,b1 human,b2 outside,b5 enraged,b6 engaged),
                          +0x34 */
    u8  fl32, fl56;    /* +0x32 (b0,b4 gates), +0x56 (b7 CPU, b6 autopilot) */
    u8  fl74b7, flB7b7;/* +0x74 b7 weapon, +0xB7 b7 */
    u16 hit_latch;     /* +0xFE */
    u16 meter66;       /* +0x66 (attract tie-break) */
    u16 hold_t;  u8 hold_ph;  u16 bc;  u8 mash_bd;  /* +0x46,+0x45,+0xBC,+0xBD */
};

/* 0x10BE8 — run in stand (0xF218/0xF236) and walk (0xF292/0xF2C2) handlers,
   after the walk angle table write, unless fl34 bit1 pended a tag recall. */
void face_opponent(struct wf_obj *o)
{
    struct wf_obj *p = o->opp;                    /* 10BEC */
    int lead = 0;                                 /* default: none */
    u8 st = p->state20 & 0xFF;
    if (st == 4) {                                /* 10BF4 */
        if (p->anim65 == 8) lead = 0x40; else if (p->anim65 == 9) lead = 0x10;
    } else if (st == 5) {                         /* 10C16 */
        if (p->anim61 == 0x22) lead = 0x30; else if (p->anim61 == 0x52) lead = 0x10;
    }
    u16 px = p->x + ((p->flip & 0x8000) ? -lead : lead);   /* 10C38-10C44 */
    if (px >= o->x) o->flip |= 0x8000; else o->flip &= ~0x8000; /* 10C46-10C5A, unsigned */
}

/* 0xF574 — pass 2, after all per-object updates */
void tieup_scan(void)
{
    for (int i = 0; i < 9; i++) {                          /* 250E slot walk  */
        struct wf_obj *a = slot[i];
        if (!a || !(a->active & 0x80)) continue;
        struct wf_obj *b = a->opp;                         /* F586 */
        if (!eligible(a) || !eligible(b)) continue;        /* F7E8 x2, §2.2  */
        if (!((a->flip ^ b->flip) & 0xFF00)) continue;     /* F594 same facing */
        if ((a->fl33 ^ b->fl33) & 4) continue;             /* F5A2 diff side  */
        if (absu16(b->x - a->x) >= 0x48) continue;         /* F5B6 */
        if (absu16(b->y - a->y) >= 0x10) continue;         /* F5CA */
        resolve(a, b);                                     /* F5E4, §2.4-2.5 */
    }
}

static void engage_tail(struct wf_obj *a, struct wf_obj *b)   /* F77E */
{
    u16 mx = (u16)(a->x + b->x) >> 1, my = (u16)(a->y + b->y) >> 1;
    a->x = b->x = mx;  a->y = b->y = my;
    a->exch = (a->exch + 1) & 3;  b->exch = (b->exch + 1) & 3; /* mod-4 via cmp/clr */
    a->partner = b; b->partner = a; a->opp = b; b->opp = a;    /* F7C6-F7D2 */
    a->fl33 |= 0x40; b->fl33 |= 0x40;                          /* F7D6 */
}
```

Outcome writes (§2.5): lockup — both `state20 = 0x000B`, `grap_flags` 0x2000
(visible) / 0x8000 (hidden); win — winner `state20 = 0x000C`, `bc = 0`,
`grap_flags = 0x2000`; loser `state20 = 0x00FF`, `grap_flags = 0`. RNG is the
shared `0x21B4` (drawn-parity canary: call counts must match the ROM).
