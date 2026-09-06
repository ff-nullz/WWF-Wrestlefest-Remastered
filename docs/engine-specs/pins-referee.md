# Match end: pins, the referee, and the mash contests — exact semantics from reference/maincpu.asm

Read-only exploration, 2026-08-22. Everything below was verified instruction-by-instruction in
`../wrestlefest-decomp/reference/maincpu.asm` unless marked *(doc)* (taken from
`docs/referee-1f914.md` / `docs/engine-specs/strikes.md` / `docs/engine-specs/reactions.md` /
`docs/engine-specs/face-tieup.md`, which this pass re-confirmed and in three places corrects).

Conventions: `+0xNN` = object field (stride 0x10C, slots at `$1C05B0`), word fields unless
".b". "pinner"/"victim" as at cover time. `+0x66` = **remaining energy** (verified by
`0x1E48C`: `+0x66==0` → CPU never kicks out; `>=0x80` → kicks instantly). `+0x33` bit0 =
legal man, bit2 = out of ring, bit6 = engaged-in-pin/tie-up. RNG = `0x21B4`, d100 walker =
`0x24CC` over `{p,100-p}` byte pairs.

---

## 1. PIN INITIATION

### 1a. Selecting a pin vs a downed opponent (selector `0xDE86`, downed-target block `0xDF06-0xDF3A`)

`0xDEE2-0xDF36`: A1 = `+0x7A`; only if `A1->+0x34 & 7 == 0` (not in a grapple phase). From
state 0 (or 1 with `+0xAF==0`): A1 = secondary opponent `+0x82`; if he is a mat victim
(`+0x21==4` and `+0x65 == 8` (face-up) — id 9 (face-down) skips the anim test at `0xDF1A`) and
inside prox box 0 of `0xE958` (x∈[-0x20,+0x80], y∈[-0x38,+0x28]) → **swap `+0x7A`/`+0x82`**
(`0xDF30-0xDF36`): the downed man becomes the primary target.

Then the category chain (`0xDF3A`): vs a downed opponent the hits are
- `0xE0B8` cat **5**: opp `+0x26==0`, opp `+0x21==4 && +0x65 ∈ {8,6}` (face-up / seated) — sets `D4=1`, links `0xF178`;
- `0xE110` cat **6/7**: same but `+0x65 ∈ {9,7}` (face-down); opp `+0x70==0` → 6 else 7.

Row fetch `0xDF96`: per-wrestler map `0xE4FE[id]` (stride 0x40), byte `[cat*3 + col]`,
col = `(+0xA3 & 3) - 1` (B1/B2/both). Wrestler 0: cat5 = `08 48 22`, cat6 = `0A 48 0A`,
cat7 = `35 48 FF`. So **B2 near any downed opponent = move `0x48` = the generic cover** for
every wrestler; B1/both give per-wrestler ground attacks or the special pins. Proximity remap
`0xEF9A` (`0xF070` table: `0x22:88`, `0x23:06`, `0x0E:06`, `0x35:0B`) degrades an
out-of-range pick to jab/kick/run (`0xF02E`). A successful pick writes exactly
`+0x20=0x0005` and `+0x60=move` (`0xDFE8-0xDFEE`).

**The pin-capable attacker moves** (this is the exact filter list the engine itself uses at
`0xEC40-0xEC5E` and `0x18104-0x1811A`): `0x48` (dive-cover, cell `0x132D8`, handler
`0x132F4`), `0x0E` (cell `0x13D26` → dispatcher `0x13D46`/`0x13D6E`), `0x0F` (cell `0x13F78`,
handler `0x13F90`), `0x35` (cell `0x175A8` → dispatcher `0x175C4`/`0x175EC`), `0x23`
(grapple cradle, cell `0x15CE0`, handler `0x15D08` → dispatcher `0x15D20`; victim gets
`+0xAA=0x1000` at `0x15D50` — effectively un-mashable). Move cell table = `0x12614[+0x61]*4`.

### 1b. CPU pin intent — `+0x34` bit4

`+0x34` bit4 = "wants to / is pinning". Only two setters in the ROM:
- `0x1108C` (jsr'd from ~20 knockdown-move enders, e.g. `0x12AE0`, `0x12D12`, `0x151E2`…):
  if `$1C007C` and own `+0x33` bit1 clear: `rng&0xFF < word 0x110C8[id]` → `bset #4,(+0x34)`.
  Per-wrestler chance table `0x110C8` (12 words): `88 80 44 40 78 78 50 80 80 85 80 40`.
- `0x1F84A` (AI script stepper): script byte with bit7 → `bset #4,(+0x34)` and masks 0x7F
  into `+0x101` (script tables `0x1F866` / `0x1F87A` when `$1C0167`).

Consumed at `0x1C348` (AI cascade): bit4 set (and `+0xB5` bit3 clear) → `+0x20=5`,
`+0x60=0x79` (walk-to-cover move). Also read at `0x1B3AA` (a lying victim whose `+0x7A`
opponent has bit4 **stays down** — reactions.md §2d), `0xEBFA` (suppresses the victim's
spring-up follow-up while the opponent intends to pin), and `0x1C420` (partner AI: opponent
pinning my partner → `bset #7,+0xB6` = "go save"). Cleared at `0x115A0`, `0x11AD0`,
`0x18AD0`, `0x1A2F2`, `0x1E282` (move/interrupt cleanup sites).

### 1c. What the cover writes on both objects (move 0x0E phase-3 handler `0x13D7E`, `0x13DF2-0x13E92`; move 0x0F at `0x14016+`; 0x48/0x35/0x23 identical pattern)

Approach phases (moves 0x0E/0x35 share `+0x44&3` phases `0x175FC`/`0x176C4`/`0x1770C`):
phase 0 (`0x175FC`) *while still walking to the body*: walk speed from `0x11D4A[id]`,
**victim `+0x9A = 0x200`** (forced-down timer — holds the body down during the walk-up), and
seeds the victim's mash counter `+0xAA = max(1, word 0x176BA[min(+0xDC,5)] − victim +0x66)`,
`+6` if own `+0x61==0x23`. Table `0x176BA` = `36 1E 16 10 0A` (5 words; the clamp writes
index 5, which overreads the word `0x0828` at `0x176C4` — 6th+ cover of the same victim is
effectively un-mashable during the walk-up). `+0xDC` = per-victim cover count, incremented
only at `0x1786A` (move 0x35's cover phase). This walk-up seed only matters *before* the
cover connects (lying victim mashes via `0x10D04`; reaching `+0xAA==0x4000` then enables
`0xEBC4` escapes: `+0x65==8` → spring-up move `0x38` (`0xEC14`), `+0x65==9` with attacker in
{0x35,0x0E,0x0F,0x23} and attacker `+0x60` bit4 (set at cell 3, `0x13DF2`) → roll-away move
`0x78` (`0xEC6A`)). It is **clobbered at cover time** (below).

The cover itself (`0x13DF8`: cell 3 + floor contact `+0x37` bit4), after target-still-down
check `0x11152` (opp `+0x26` in state 4 anim 8/9 within box 0xA) and back-pointer check
`0x11412` (`opp->+0x26 == self`), else the attacker takes reaction 5/0x1E bounce-off
(`0x13F1A-0x13F74`):

```
attacker: bset #6,(+0x33)                 victim: bset #6,(+0x33)
          bclr #4,(+0x60)                         +0x20 = 0x0005, +0x60 = 0x0051
          snap X/Y to victim (±0x30 by facing)    +0x68 = 0x11
          launcher 0x258E(D0=3)                   +0xDA += 1, +0xC7 += 5
$1C1800 = 0x0E; YM 0x32 + 0x2B
$1C15D2 = attacker +0x03 (slot id); $1C15D3 = 0x20   ; announcer "cover" how-byte
```

Then per frame (`0x13EAA-0x13EE4`), **while `$1C007C != 0` and own `+0x33` bit2 clear and
both `+0x33` bit0 set**: `bset #0,(+0x35)` **on the attacker**, `bset #7,(+0x60)`
(engaged latch), `jsr 0x215B6` (tag maintenance: marks own partner `+0x34` bit0, reloads
`$1C1682 = 0xFA`, legal-bit juggling `0x21612`).

Victim's forced move `0x51` (cell `0x18BF4`, handler `0x18C24`): first entry
`clr +0xAA; jsr 0x10C60(D0=0)` → **`+0xAA = 0x100` if `+0x66==0`, else `max(1, 0x2A − +0x66)`**
(this is the real 3-count mash count; it replaces the walk-up seed), then launcher
`0x258E(3)` (vz=0x300, grav 0x38 — the squash bounce). On landing the FF00 hold is broken
(`clr +0x22` at `0x18C5A`), the cell chain finishes (`+0x25==0xFE`) and — if match live and
both legal — **auto-chains** (`0x18C88`): victim → `+0x20=$8005... +0x60=$C04A` (sustained
struggle, cell `0x180AC/0x180B8` spr 0x1D/0x1E) and attacker (unless in move 0x0E) →
`+0x60=$C048` (sustained cover); `jsr 0x21732` = **tag-save hook**: victim's partner gets
`+0x34` bit0, `$1C1682=0xFA` run-in window, CPU partner gets `+0xB5` bit7 + `+0x7E` rescue
link, seated partner gets break-up timer `+0xE6` from `0x217F8` = `3E 5D 7D` by `+0x70` band.

### 1d. How the referee learns — NOT `$1C167A`

`0x10D04` ("down tick", called by lying/bounce/pinned handlers): if `+0x56 & 0xC0 == 0`
(a legal, controlled body): `bset #7,$1C167A`; if `+0xAA != 0x4000` and `+0xA3 != 0` (fresh
button edge): `--+0xAA`, at 0 → `+0xAA = 0x4000` (mashed-out latch). **`$1C167A` bit7 is
consumed only at `0x89AC-0x8A72`** — it drives the FG0 palette flash at `$C1008`
(`0xFF00FF00` strobing = the "mash!" HUD flash), with bit6 latch and previous-frame copy
`$1C167B`. The referee never reads it. **The referee's cue is `+0x35` bit0 on the attacker**,
found by the hunt `0x20556` scanning the 9 slots.

---

## 2. THE REFEREE — object `$1C11F4`, wrapper `0x1F914`

Wrapper (16 insns): `+0x21` ×4 → SM table `0x1F952` (11 longs), `jsr (A3)`; then
`0x1FAF0` visual dispatch (`bclr #7,+0x20` → latch `+0x20`→`+0x1C`; `+0x1D` ×4 → table
`0x1FB1A`), `0x247C`, `0x27B8`, and `0xF8E4` when `$1C0161` bit1 (cage puppet). Every
in-match SM write is `move.w #$800N,(+0x20)` so SM and visual stay in lockstep. Both tables
are extracted as `data/romdata/ref_sm_dispatch.json` / `ref_visual_dispatch.json`.

### SM table `0x1F952` (exact bodies)

| N | PC | body |
|---|---|---|
| 0 intro | `0x1F97E` | `$1C1682` ≠ 0: `--`; on hitting 0 → `+0x20=$8002`, `+0x56 = $1C1684.l`. Else **falls into 5**. |
| 1 attend pin | `0x1F9E0` | A1=`+0x56`; **`+0x35` bit0 still set → rts**; else digit wipe `0x206FE`, `+0x20=$8005`. |
| 2/7 escort | `0x1F9F8` | target `+0xFE`≠0 → `$8005`. Hunt `0x20556` (found → return). Else if target `+0x32` bit0 (seated) or (state 1, `+0xAF==4`): retarget `$1C1688`, `+0x20=$8002`. |
| 3 20-count walk | `0x1FA82` | cage bit → `clr $1C1682`. `0x204FA` → A2/A3 legal men; walk to X-midpoint: `+0x2D=0xC0/0x40` by side; `|dx|<0x20` → `bchg #7,+0x2D` (oscillate) ; `<0x28` → `clr +0x2A` (stop); else `+0x2B=0x14`. Never writes `+0x20`. |
| 4/8 attend ring-out | `0x1FA40` | target `+0xFE`≠0 or `+0x35` bit1 clear → `$8005`. Else scan 9 slots for `+0x35` bit0 → **a pin pre-empts the ring-out**: `+0x56=slot`, `+0x20=$8001`. |
| 5 idle | `0x1F99E` | tag: `0x204B2` midpoint→`+0x30/+0x1E`; rumble: `+0x30=0x268,+0x1E=0x158`. `0x2052E` zone bits (`+0x35` bit3 if `+0x30<0x270`, bit4 if `+0x1E<0x160`). If no target or target `+0xFE==0`: hunt `0x20556`. |
| 6 pin count | `0x1F9D8` | **`+0x25 >= 6 → rts` (point of no return — at "three" the count cannot be aborted)**; else falls into 1 (abort to `$8005` the frame `+0x35` bit0 disappears, wiping the digits). |
| 9 | `0x1FAEC` | rts (visual 9 does the work). |
| A | `0x1FAEC` | rts. |

Hunt `0x20556`: `$1C007C==0` → not-found. 9 slots: live + `+0x35` **bit0** → `+0x20=$8001`
(pin); else `+0x35` **bit1** + `+0x33` bit0 → `+0x20=$8004` (ring-out); `+0x56 = slot`.
Returns Z=1 on found (D5=0/1).

### Visual table `0x1FB1A`

| N | PC | body |
|---|---|---|
| 0 | `0x1FB46` | tag: walk cycle (tick `+0x23`=8, 4 poses), heading `0x205AC` (table `0x2060C[32]` by `+0x34` bits1-4 + far-side bit), `0x2208` motion, `0x280DC` floor probe with pushback and `+0x35` push bits. Rumble: static pose. |
| 1/2/4 | `0x1FC22` | approach `+0x56`: dest = target X ± D1 (ring-center side; D1=0x50, or 0x30 with `+0x62−=0x10` when SM==4; target in move 0x22 → extra −0x10, move 0x09 → 0x50), walk `0x20C8`+`0x2208`, close test `0x2062C` (\|dx\|<6 && \|dy\|<6, floor-bump counts as arrived). On close: own Y = targetY+1; visual 1 → `clr +0x24`, `+0x20=$8006` (**the only in-match `$8006` writer**); visual 2 → `$8007`; visual 4 → `$8008`. |
| 3 | `0x1FD5E` | **20-count** (see below). |
| 5/A | `0x1FF52` | leave/patrol: heading `0x2001E[(+0x34&6)>>1]` = `C0 80 40` (A: fixed 0xC0); floor probe; visual A stops on bump; visual 5: on bump matching mask `0x2001A[(+0x34&6)>>1]` = `04 02 08 01` → `+0x20=$8000`. |
| 6 | `0x20022` | **1-2-3 pinfall** (see below). |
| 7 | `0x202BA` | usher: wag poses 7/8; pokes both sides `$1C1684`/`$1C1688`: not-seated → `bset #1,+0x34`; always `bclr #0,+0x34`. |
| 8 | `0x20354` | ring-out pose: frames 0xD/0xE (target seated: 7/8), face target. **Does not count** — see below. |
| 9 | `0x203EE` | win pose F→10→11: at 10: `0x20156` (YM `$3109` on stages 4/9 else `$3105`), `$1C15D2 = $0F2A` (human winner) / `$0F2C` + `clr $1C15D4` (CPU winner). At 11: (tag) CPU winner also YM `$3108`; **`bset #7,(+0xFE)` on winner, winner's partner, loser, loser's partner** (`$4000/$4001` → `$C000/$C001` = final), `+0x20=$8005`. Rumble: `$800A`. |

### Visual 6 — the 1-2-3 (`0x20022`) exact

First entry: `clr $1C169A`, `clr +0x24`, `+0x54=$3162` (YM "ONE"), copy target facing,
**`+0x23 = 0x14`**, `+0x05 = 4`. Per frame: A1 = `+0x56` (pinner), A2 = `A1->+0x26` (victim);
`--+0x23`; on zero:

- `+0x25 += 1` (own cell = **half-count**) and **victim `+0x109 += 1`** (per-victim tally
  read by the CPU kick-out scheduler and the near-fall announcer).
- `+0x25 == 7` → the FALL aftermath: tag → `+0x20=$8009` (win pose), digit wipe `0x206FE`,
  pinner `+0x4A = 4`; rumble → wipe, `$8005`.
- `+0x25` even → `$1C169A += 1` (displayed digit), blit `0x2067C(D0 = word(+0x24)>>1` = 1,2,3`)`,
  YM `+0x54` then `+0x54 += 1` (3162/3163/3164 = ONE/TWO/THREE).
- pose cadence: `+0x05` toggles 5/6 with `+0x23 = 0x20` / `0x38` (so first half-count after
  0x14 frames, then alternating 0x20/0x38 — the doc's "0x14 per half-count" is only the seed).
- `+0x25 == 6` (the moment "3" goes up): `+0x23 += 0x10`; **tag: stamps the result**:
  pinner pair `+0xFE = $4000`, victim pair `+0xFE = $4001` (all four objects via `+0x86`),
  `clr $1C169E` (the match-decided signal the frame-loop mode driver `0x6FC` acts on),
  `jsr 0x90D6` (leftover-energy scoring). Rumble instead: `0x21358` (credit chain via
  `+0x7E`), pinner `+0xC4 += 1`, victim `bset #4,(+0x32)` (elimination-rise), rts.

*(Correction to `docs/referee-1f914.md` §4: the `+0xFE` stamp happens at `+0x25==6`, not 7;
`+0x25==7` only does `$8009`/`$8005` and the wipe.)*

The referee **never touches `$1C007C`** — the resolves clear `$1C169E`; the mode driver
(`0x6FC` / `0x924`) later clears `$1C007C`, and `0x8CF8` paints YOU WON when it is clear.

### Kick-out — how victim mash interrupts the count

The count handler itself never reads `+0xAA`. The interrupt chain is entirely victim-side:

1. Victim in move `0xC04A` (handler `0x180C4`): per frame, **only while `+0x66 != 0` and the
   pinner's `+0x60` bit7 is set**, `jsr 0x10D04` → each fresh button edge (`+0xA3`)
   decrements `+0xAA` (seeded `max(1, 0x2A − energy)`, `0x100` at zero energy — *zero-energy
   victims cannot mash at all*); at 0 → `+0xAA = 0x4000`.
2. Next button press: action prefix `0xEBC4` (`0xDEB4`) sees `+0xAA == 0x4000` and own state
   5 move `0x4A`/`0x51` → **`+0x20=5, +0x60=0x4B`** (kick-out move) and `clr +0xAA`.
3. Move `0x4B` (cell `0x181EC`/`0x181FE` spr 6E/6F/70/71, handler `0x1820C`): first entry
   **`bclr #0,(+0x35)` on the pinner** — the referee flag dies here — pinner → state 4
   reaction `0x0F` (thrown off, `+0x9A=0x50`), `bclr #6,(+0x33)` both, slide `0x10B9A`,
   `jsr 0x21282`: if victim `+0x109` ∈ {4,5} → `$1C15D2 = $0F1E` (near-fall announcer), then
   `clr +0x108` (tally reset). A third man in move `0x49` (double-cover) is released the
   same way (`0x18286-0x182C6`).
4. Referee: SM6 falls into SM1 while `+0x25 < 6`; the frame `+0x35` bit0 is gone it wipes
   the digits (`0x206FE`) and goes `$8005` → idle → hunt. **At `+0x25 >= 6` SM6 rts's** —
   once "3" is up the fall always completes.

The pin also ends without mashing via `0x180FA-0x18184`: if the pinner leaves
{0x48,0x0E,0x0F} state-5, or own `+0xFE` bit7 (match over), or `+0x32` bit4 (rumble
elimination / forced rise: `$1C15D2=own id`, `$1C15D3=0x29` announcer): pinner → state 7
get-up + `bclr #0,(+0x35)`, victim → state 4 lying (`+0x9A=0x20`). A hit landing on the
pinner (`0x24C0C` in the hit pipeline) also clears `+0x35` bit0.

CPU victims never mash: AI `0x1E452` computes once (latch `bset #6,+0x60`) a target
half-count from energy (`0x1E48C`: `+0x66==0`→8 (never), `>=0x80`→0, `>=0x65`→1, `>=0x50`→2,
`>=0x40`→3, `>=0x25`→4, else 5) into `+0xBD`, then when victim `+0x109 == +0xBD` writes
`+0x60 = 0x4B` (moves 0x4A/0x64) or `0x56` (holds) directly.

### FG0 digit blits — `0x2067C`, tables `0x2073A`, wipe `0x206FE`

`0x2067C(D0=n)`: window = `$C0748` (single digit), `$C0744` when `n >= 10` (tens at +0,
ones at +8), `$C1188` when D0 bit15 (alternate window; no in-match caller passes it).
Glyph pointer table `0x2073A` — **21 longs, n = 0..20** (reader `0x206B4`) → 8 bytes each
(rows from `0x2078E`): 4 rows × 2 tile bytes, written as `tile.w` + attr `0x00EA`, row
stride `0x100`. Extracted: `data/romdata/ref_count_digit_tiles.json`. `0x206FE` clears
4 longs × 4 rows at `$C0744`.

### Count-out (visual 3 `0x1FD5E`, tick `0x1FDF4`) — and when it actually runs

**In a normal match nothing ever writes `$8003`**: ring-outs get SM4 → visual 4 approach →
`$8008` visual 8, which only poses. The full 20-count runs when a **legal** man lands on the
outside floor (`0x1987E-0x198A8`, non-rumble: `$1C1678 = $8000`, `$1C1679 =` faller's
facing; `0x19B28`: both men down/out → `$C000`): the frame-loop match-in dispatcher
(`0xF98C`) sees bit7 and takes `0xFB5A-0xFBC4`: waits vblank `$140026`, `jsr 0x26E66`,
**teleports the referee to ringside `(0x340,0x160)`, `+0x20 = $8003`**, `bset #7,$1C0076`,
`clr $1C1678`, YM `0x80`.

Visual 3 exact: init `clr $1C169A`, `+0x54=$3165`, poses 9..0xC; per frame `0x2208` then
clamp own X to `[0x298,0x390]`; while `$1C169B != 0x14` tick `0x1FDF4`: `+0x24 += 1`; at
**`0x50` frames**: `clr +0x24`, `$1C169A += 1`; if the new count is 20 and **neither** legal
man still has `+0x33` bit2 → rts (count parks, no resolve); blit `0x2067C(count)`; count
`>= 0x11` → YM `+0x54++` (3165/66/67 warnings); `== 0x14` → resolve `0x1FE5A`:
`+0x22 = 0x1000` (pose hold), `0x204FA` → A2/A3, by `+0x33` bit2:
- both out → all four `+0xFE = $8005` (double KO), YM `$3108`, `clr $1C169E`, FG0 big-blit
  `0x2503C(D0=0x52)`;
- one out → out pair `+0xFE = $8002`, in pair `+0xFE = $8003`, `0x90D6`, `clr $1C169E`;
  winner human (`+0x56` bit7 clear) → YM `$3108` + blit 0x52, winner CPU → `0x20156` jingle.

Wrestlers react to a live count at 10: `0x1C6DC` / `0x1EBF4` (`cmpi.b #$a,$1C169B` → forced
return move if legal) *(doc: tag-extract.md)*.

### Instant KO — the `+0xFE = $8000/$8001` form

`0x111C8` (called from finisher-move enders, e.g. `0x17ED4`): match live, own `+0x33` bit2
clear, both bit0 set, and **victim `+0x66 == 0`** → once (`bset #3,+0x60` latch):
**`$1C1214 = $8009`** (that is referee `+0x20` — the only external write into the referee's
state word: straight to win pose, no count), `clr $1C169E`, YM `0x32`×2, winner pair
`+0xFE = $8000`, loser pair `$8001`, `0x90D6`. (Rumble branch at `0x11284` differs.)

### Referee motion

Visual 0/1/2/3/5/A call `0x2208` (apply_motion) directly with `+0x2A/+0x2B/+0x2D`; the
approach also uses `0x20C8` (motion_toward via `$1C15E4/E8/EA`); floor probe `0x280DC`
with manual pushback (`+0x38/+0x3A`). SM3 and `0x205AC` only steer heading/speed.

---

## 3. LOCKUP MASH — `0x1EFAA` exact (state 0x0B row of the **CPU** table `0x1C1F4`)

Critical framing: table `0x1C1F4` (13 rows, indexed `+0x21`) is dispatched by `0x1C1D2`
from the walker at `0x1C15C`, which **only visits CPU-driven objects** (`+0x56` bit6, or
bit7 with `+0x32` bit7 clear; skips state 0xFF, `+0xFE`≠0, `+0x32` bit4). Match AI writes
`+0x20`/`+0x60` directly and never synthesizes button input *(doc: ai-port-map.md)*. So
`0x1EFAA` is the **CPU's** side of the contest; the human's side is the ordinary attack
selector.

**Human**: a fresh B1/B2 edge in state 0x0B hits category 8 (`0xE180`): requires own
`+0x44` byte bits 5 AND 6 clear (word bits 13/14 — bit13 = impact-cell pending, bit14 =
"opponent already won this exchange"); then `bset #6,(partner +0x44)` (lock the partner
out) and cat 8 → map row (`29 29 29` for every column) → **move `0x29` immediately**.
Mashing does *not* touch `+0xBD` and does *not* feed `0x24CC` — **each press instantly wins
the current exchange** unless the CPU's roll already landed this exchange (then bit14
blocks cat 8 and the press falls through to nothing).

**CPU** (`0x1EFAA`): on state entry (runs once — `+0x20` bit15 is set by the movement
walker at `0xF4EC` after first dispatch): `clr +0xB6`, `bclr #1,+0xB9`, `+0xBD = 0x20`;
if the partner is a human (`+0x26->+0x56` bits6,7 both clear) → `+0xBD = byte
0x1F060[$1C0162]` (stage word; table = `07 07 06 06 03 05 05 04 04 03` for stages 0-9);
then `+0xBD += rng&3`. Per frame: `--+0xBD`; on expiry: reload the same way
(`0x1EFEC-0x1F012`), then **if own `+0x44` bit14 clear**: `0x24CC` over `0x1F05E =
{0x32,0x32}` (50/50); arm 0 → **the CPU wins the exchange**: `+0x20=5, +0x60=0x29`,
`bset #6,(partner +0x44)`. So vs a human the CPU contests every 3-7(+0..3) frames at 50%
(stage 0-1: every 7+, alone in CPU-vs-CPU: every 0x20+rng&3), and the human must simply
press first. `0x1F04A` is the seed helper; note `0x1F05E` is the 2-byte `0x24CC` pair and
`0x1F060` the 10-byte stage table (face-tieup.md prints them merged as one 12-byte table).

**Exchange resolution — move `0x29`** (cell `0x1656A`: 4 cells dur 14/8/E/E, spr F5 F6 F7 +
impact 1D2, handler `0x16582`): init: winner `bclr #7,+0x44`, loser (`A2 = +0x26`)
`bset #7,+0x44` and **`+0x20 = 0x00FF`** (frozen/carried, skipped by all dispatchers); YM
`0x2A` at cell 1. On `+0x25==0xFE`: loser `+0x68=1`, `+0xC7 += 1`, `+0x54=0x100`,
**loser `+0x52 += 1`**; if loser's `+0x52 < 2`: **both back to state 0x0B** (winner
`+0x44=0`, loser `+0x44=$8000` hidden-half) — another round; else winner → **state 0x0C**
(`+0x44=$2000` impact-cell bit13), loser stays 0x00FF. I.e. the lockup is a race to shove
the same opponent twice; `+0x44` bit14 is only the intra-exchange lockout, and the task
prompt's "loser goes to 0x29" inverts it — **move 0x29 is the winner's shove**; the loser
is the one frozen at 0xFF. (`+0xFE` bit15 during the tie-up bails both to state 5 anim 0x50
— `0x12534`.)

---

## 4. HOLD 0x0C — `0x1F06A` and the escape

State 0x0C = the **holder** of the rear waist-lock; the held man is state 0xFF (frozen,
carried by `0x10B9A`) and has **no input path and no AI row — his mashing does nothing
here**. The hold's own clock (anim code `0x124FA`, on the holder): `+0x46 = 0xE0` at entry,
`−1`/frame; at `0xA0` → `+0x45 = 1` (grapple window opens); at `0` → **the hold flips**
(`0x12566`): holder → 0x00FF, victim (`+0x26`) → state 0x0C with `+0x44=0`, cell 0x39 —
that flip is the only "escape", and no button press accelerates `+0x46`.

- **Human holder**: press with `+0x45 == 1` → category 9 (`0xE1B0`): `clr +0x44`, cat 9 →
  per-wrestler grapple moves (wrestler 0: `15 17 15`).
- **CPU holder** (`0x1F06A`, the entire row is 12 bytes): `cmpi.b #$30,(+0xBD); bcc → fall
  through to 0x1F078; addq.b #1,(+0xBD); rts`. So `+0xBD` counts frames held, and at
  **0x30** the row falls into the state-independent CPU move picker `0x1F078` (script step
  `0x1F7F0(D2=1)`; on `+0xB5` bit7 → `+0x61 = 0x1F150[+0x57]`, else nested pick tables at
  `0x219B4` by script id / facing / zone `+0x31` / opponent `+0x70`, deeper arms nest
  `0x24CC` and `bra 0x1DFE2`) — the CPU executes a grapple move ~0x30 frames in, well
  before the 0xE0-frame flip. (ai-port-map's "script_counter" name and its 12-byte size
  refer to exactly these three instructions; the nested description belongs to `0x1F078`.)

**Scripted submission holds are a different mechanism** (they are state-5 *moves*, victim in
moves `0x5D-0x64`/`0x52`/`0x53`): those victim handlers DO call `0x10D04` per frame
(callers `0x180F4, 0x18DBC, 0x18EE4, 0x194FA, 0x19600, 0x1965C, 0x1B218…`), so the victim
mashes `+0xAA` (seeded by `0x10C60(0)`: `0x100` at zero energy else `max(1,0x2A−energy)`)
down to the `0x4000` latch, and the next press runs the **`0xEBC4` ladder**: state-5 own
move → escape move, exactly `0x4A/0x51→0x4B, 0x5D→0x67, 0x5E→0x56, 0x5F→0x5C, 0x60→0x5B,
0x61→0x59, 0x62→0x5A, 0x63→0x55, 0x52→0x58, 0x53→0x57, 0x64→0x4B`, plus the down-victim
arms (state 4 anim 8 → `0x38` spring-up; anim 9 vs incoming cover {0x35,0x0E,0x0F,0x23}
with `+0x60` bit4 → `0x78` roll-away); every hit `clr +0xAA` (`0xEDB6`) and returns carry
(consumes the press before the category chain). The holder side of those scripted holds
picks follow-ups through the same `0xEBC4` when *he* holds the `0x4000` latch (won a
tie-up) — that full grapple-move graph is the later task.

---

## 5. C SKETCHES

```c
/* ---- pin_start: the cover connect (0x13DF8-0x13E92 shape, all pin moves) ---- */
static bool pin_cover_connect(Obj *a /*attacker*/) {
    Obj *v = a->partner;                              /* +0x26 */
    if (!victim_still_down_in_box(a))   return bounce_off(a);   /* 0x11152: st4 anim 8/9, box 0xA */
    if (v->partner != a)                return bounce_off(a);   /* 0x11412 */
    a->f33 |= BIT6;  v->f33 |= BIT6;                  /* engaged */
    g_1c1800 = 0x0E;
    v->state = 5; v->move = 0x51; v->dmg68 = 0x11;    /* squash move */
    v->times_covered_da++; v->c7 += 5;
    ym(0x32); ym(0x2B);
    g_announce_id  = a->slot_id;                      /* $1C15D2 */
    g_announce_how = 0x20;                            /* $1C15D3 */
    launch(a, 3);  a->x = v->x + (v->facing_left ? -0x30 : 0x30); a->y = v->y - 1;
    return true;
}
/* per frame while covering (0x13EAA): */
static void pin_hold_frame(Obj *a) {
    if (!g_match_live || (a->f33 & BIT2) || !(a->f33 & BIT0) || !(a->partner->f33 & BIT0)) return;
    a->f35 |= BIT0;                                   /* the referee's cue */
    a->move_w |= 0x8000;                              /* engaged latch (+0x60 bit7... word bit15) */
    tag_maint_215b6(a);
}
/* victim move 0x51 first entry: v->mash_aa = (v->energy==0)?0x100:max(1,0x2A-v->energy);
   on landing auto-chain: v->move=0xC04A (struggle), a->move=0xC048 (unless a in 0x0E);
   tag_save_21732(v).  Kick-out: mash → +0xAA==0x4000 → press → move 0x4B →
   pinner->f35 &= ~BIT0; near-fall announce if v->halfcounts(+0x109) in {4,5}. */

/* ---- referee_frame (wrapper 0x1F914) ---- */
void referee_frame(Ref *r) {
    switch (r->sm /* +0x21 */) {
    case 0: if (g_1c1682 && !--g_1c1682) { set(r,0x8002); r->target = g_1c1684; break; }
            /* fallthrough */                                      /* 0x1F97E */
    case 5: idle_midpoint_or_rumble_park(r); zone_bits(r);         /* 0x1F99E/204B2/2052E */
            if (!r->target || !r->target->result) hunt(r); break;  /* 0x20556: bit0→8001, bit1→8004 */
    case 6: if (r->cell >= 6) break;                               /* 0x1F9D8 point of no return */
            /* fallthrough */
    case 1: if (!(r->target->f35 & BIT0)) { digits_wipe(); set(r,0x8005); } break;  /* 0x1F9E0 */
    case 2: case 7: escort(r); break;                              /* 0x1F9F8 */
    case 4: case 8: attend_ringout_or_preempt_pin(r); break;       /* 0x1FA40 */
    case 3: walk_between_men(r); break;                            /* 0x1FA82 (never writes +0x20) */
    case 9: case 0xA: break;                                       /* 0x1FAEC */
    }
    visual_dispatch(r);        /* 0x1FAF0: latch, then 0x1FB1A[vis] */
    sprite_screen_pos(r); draw_list_insert(r);
    if (g_1c0161 & BIT1) cage_puppet();                            /* 0xF8E4 */
}
/* visual 6 (0x20022): if(--r->t23) return;  r->cell++; victim->halfcount109++;
   if (r->cell==7) { tag? set(r,0x8009), wipe, pinner->hit4a=4 : (wipe, set(r,0x8005)); return; }
   if (!(r->cell&1)) { g_count++; blit_digit(r->cell>>1); ym(r->ym54++); }
   r->t23 = (r->pose==5)?0x38:0x20; toggle pose 5/6;               /* seed was 0x14 */
   if (r->cell==6) { r->t23+=0x10; tag? stamp 4000/4001 on both pairs, g_169e=0, energy_score()
                                 : rumble_credit(); } */

/* ---- lockup_mash (0x1EFAA, CPU only; human = cat 8 press → move 0x29) ---- */
void ai_lockup_0b(Obj *o) {
    if (!(o->state_w & 0x8000)) {                       /* entry (walker sets bit15 after) */
        o->b6 = 0; o->b9 &= ~BIT1; o->bd = seed_bd(o);  /* 0x20, or 0x1F060[stage] vs human */
        return;                                          /* +rng&3, 0x1EFB2-0x1EFE2 */
    }
    if (--o->bd) return;
    o->bd = seed_bd(o);
    if (o->grap44 & 0x4000) return;                      /* opponent already won this round */
    if (d100_pick(tbl_1F05E) == 0) {                     /* {50,50} */
        o->state = 5; o->move = 0x29;                    /* I shove = I win the exchange */
        o->partner->grap44 |= 0x4000;
    }
}
/* move 0x29 end (0x165BC): loser->shoved52++;  <2 → both state 0x0B (rematch);
                            >=2 → winner state 0x0C (+0x44=0x2000), loser 0x00FF. */

/* ---- hold_mash (state 0x0C) ---- */
void ai_hold_0c(Obj *o) {                                /* 0x1F06A — CPU holder only */
    if (o->bd < 0x30) { o->bd++; return; }
    cpu_pick_grapple_move(o);                            /* falls into 0x1F078 cascade */
}
/* holder clock 0x124FA: t46: 0xE0→0; at 0xA0 phase45=1 (human cat-9 window);
   at 0 FLIP: holder→0xFF, victim→0x0C.  Held man (0xFF): no input, no AI, no mash. */
```

---

## 6. TABLE MANIFEST (for extraction)

| addr | size/shape | contents |
|---|---|---|
| `0x1F952` | 11 longs | referee SM handlers (extracted: `ref_sm_dispatch`) |
| `0x1FB1A` | 11 longs | referee visual handlers (extracted: `ref_visual_dispatch`) |
| `0x2073A` | 21 longs → 8 bytes each (rows at `0x2078E+`) | count digit glyphs 0..20, 2×4 FG0 tiles, attr `0x00EA` (extracted: `ref_count_digit_tiles`) |
| `0x2001A` | 4 bytes `04 02 08 01` | visual-5 leave bump masks (extracted in `ref_heading_*`) |
| `0x2001E` | 3(4) bytes `C0 80 40` | visual-5 leave headings by `+0x34` bits1-2 |
| `0x2060C` | 32 bytes | idle-walk heading by `+0x34` bits1-4 (+16 = target left) |
| `0x110C8` | 12 words `88 80 44 40 78 78 50 80 80 85 80 40` | CPU pin-intent chance per wrestler (rng&0xFF <) |
| `0x176BA` | 5 words `36 1E 16 10 0A` (idx 5 overreads `0x0828`) | walk-up mash seed by `min(+0xDC,5)`, − victim `+0x66`, min 1, +6 if move 0x23 |
| `0x10D00` | 4 bytes `01 03 07 15` | bounce mash seed by energy quartile (`0x10C60` D0=1) |
| `0x10CEA` | 3×N bytes | `0x10C60` D0≥2 seed rows (D0-1)*3 + `+0x70` |
| — (code `0x10C7E`) | — | pin/hold mash seed: `0x100` if energy 0 else `0x2A −` energy (D0=0) |
| `0x1F05E` | 2 bytes `32 32` | lockup expiry 50/50 `0x24CC` pair |
| `0x1F060` | 10 bytes `07 07 06 06 03 05 05 04 04 03` | CPU lockup countdown seed by stage (`$1C0162`) |
| `0x1E48C` | code bands | CPU kick-out half-count by energy: 0→8, ≥80→0, ≥65→1, ≥50→2, ≥40→3, ≥25→4, else 5 |
| `0x12614` | 144 longs | state-5 move cell table (`+0x61`) |
| `0x13D26`/`0x13F78`/`0x175A8`/`0x15CE0`/`0x1269C…` | cells | pin moves 0x0E / 0x0F / 0x35 / 0x23 / 0x22 |
| `0x13D6E`, `0x175EC` | 4 longs each | pin `+0x44` phase dispatchers (`175FC 176C4 1770C` + `13D7E`/`1778E`) |
| `0x18BF4`/`0x18C04`/`0x18C14` | cells | victim move 0x51 (squash) — spr 15/17/16 variants |
| `0x180AC`/`0x180B8` | cells | victim move 0x4A struggle (spr 1D/1E, FF00) |
| `0x181EC`/`0x181FE` | cells | kick-out move 0x4B (spr 6E 6F / 70 71) |
| `0x132D8` | cell | pinner move 0x48 cover |
| `0x1656A`/`0x16614` | cells | lockup shove move 0x29 (spr F5 F6 F7 1D2) |
| `0x1C1F4` | 13 longs | CPU per-state AI rows (0x0B=`0x1EFAA`, 0x0C=`0x1F06A`) |
| `0x1F866`/`0x1F87A` (+`0x1F150`, `0x219B4`) | ptr lists | CPU move scripts (bit7 = set pin intent) |
| `0x217F8` / `0x2172A` / `0x2127C` | 3 words each | tag break-up / rescue timers `3E 5D 7D` … by `+0x70` |
| windows | — | digits: `$C0748` ones, `$C0744` tens, `$C1188` alt; mash-flash palette `$C1008`; flags `$1C167A/B` |

Key globals: `$1C169A/B` count, `$1C169E` decided-signal, `$1C1682` intro/run-in timer,
`$1C1684`/`$1C1688` sides, `$1C15D2/3/4` announcer, `$1C1214` = referee `+0x20`,
`$1C1678/9` ring-out scene trigger, `$1C1800` HUD event, `+0xFE` forms: `$4000/$4001` pin
(→ `$C000/$C001` after win pose), `$8002/$8003` count-out lose/win, `$8005` double KO,
`$8000/$8001` instant KO (`0x111C8`).
