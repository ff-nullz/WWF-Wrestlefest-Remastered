# Palette-bank semantics of the 0xD1FC sprite compiler ($1C1610)

Sources: `../wrestlefest-decomp/reference/maincpu.asm` (all PCs below), verified
against the actual ROM bytes (`data/rom/31e14-0.ic18` even + `31e13-0.ic19` odd, interleaved;
verified by matching the opcode at $D1FC). Parser + raw dumps in this directory (`palex.py`).

## 1. Who writes $1C1610, and what it holds

$1C1610 is touched by exactly **two writers and four readers** in the whole ROM (grep of
maincpu.asm finds only these):

* **0x2AEA — allocator** ("walkout palette load").
  Input D0 = **global palette-resource id** (0..~0x3B).
  - If `$1C1610[D0]` bit 7 already set → return (already loaded; a mirror match shares the entry).
  - Else scan the free list `$1C1600[0..15]` for the first zero byte → hardware slot `n`
    (lowest-free-first); mark `$1C1600[n]=0xFF`; write **`$1C1610[D0] = 0x80 | n`**
    (move.b D1 then bset #7, 0x2B1C/0x2B20); copy the 16-colour source palette from
    `$2F22 + D0*32` into palette RAM at `$182000 + n*0x80` (clr first colour, 15 words).
* **0x2B58 — deallocator**: clears `$1C1610[D0]` and frees `$1C1600[slot]`.
* Readers: only the compiler's four `lea/move.l #$1c1610` at 0xD30C, 0xD644, 0xD722, 0xD8EA.

**Nothing rewrites $1C1610 per object or per frame. It is a single global map:
palette-resource id → (0x80 | hardware slot).** The emitted "bank byte" is the whole table
byte including bit 7, i.e. `0x80|n` (0xD378 `move.b (A3,D3.w),D3; move.w D3,(A2)+`).

Callers of 0x2AEA:
* **Wrestler object spawn** (the walkout): `move.w ($2,A0),D0; jsr $2aea` at 0x0E4A/0x589C/
  0x5F80/0x82AA/0x852A/0x8DEC→0x8DF6, i.e. **D0 = obj+2 = the wrestler's absolute character id
  (0..11)**. So a wrestler's table index IS his character id.
* Fixed-id loads for non-wrestler art: #$1E (0x8DC8 — referee-ish object also sets obj+2=0x1E),
  #$1F, #$2C, #$2D, #$34, #$35, #$37, #$3B (menus/HUD/effects). These sit at indices ≥ 0x1E and
  never collide with wrestler ids.
* Frees happen at object destruction / scene teardown (0x1D92, 0x887C, 0x8E56/0x8E60, 0xA77C,
  0xB186, 0xB850..., 0xC032). Never per-frame.

**During a live singles match** (chars A and B): `$1C1610[A] = 0x80|nA`, `$1C1610[B] = 0x80|nB`;
all other indices 0..11 (and unused 12..15) are **0**; ids ≥ 0x1E hold the referee/HUD slots.
The slot numbers nA/nB are **not fixed** — they are first-free-slot in allocation order at spawn
time (whatever the ring/referee/HUD already claimed comes first). The engine should replicate
the allocator (0x2AEA/0x2B58 + free list $1C1600), not hardcode n per index.

## 2. Where the emitted bank byte comes from, per emit path

First, the "D7" red herring: **D7 is not a palette bias.** It is a stack-displacement
compensator. 0xD1FC builds one 0x28-byte frame (0xD218) and clears D7 (0xD216); every nested
handler bumps D7 by exactly (bytes it pushed + 4 for the jsr return) — D2AE/D802 +0xA with 6
bytes pushed, DA12 +0x16 with 0x12 bytes, D43A/D540 +4 with nothing — so `(n,A7,D7.w)` always
addresses **frame+n of the single top-level frame**. Layout:

```
frame+0x00/02  X/Y origin  (obj+0x14 +0x10, obj+0x16 +0x90)
frame+0x04     nested sub-cell sprite count
frame+0x05/06  nested sub-cell dx / dy
frame+0x07     nested sub-cell dpal  <-- THE palette bias
frame+0x08     flags (bit4 = hflip, from obj+4 sign)
frame+0x0A     drawer's data-bank base = $38FB8[obj+2] (0xD27C)
frame+0x0E     partner's data-bank base (written by DA12, 0xDA6C)
frame+0x12/13  extra dx/dy word (cleared at 0xD21C; DA12 loads it per partner)
frame+0x14     partner object pointer = obj+0x26 (0xD23C)
```

Emit paths:

* **D2AE top loop (type-0 cell) and D802 top loop (type-1 packed cell)**: pal byte is used
  **raw, no bias** — 0xD36E→0xD378 and 0xD964→0xD972 index $1C1610 with the stream/channel
  byte directly.
* **D43A / D540 nested sub-cells → D71A (leaf) and D466/D63C (packed)**: pal index =
  **raw byte + frame+0x07** (0xD78E `add.b ($7,A7,D7.w),D3`; 0xD6D0 `add.b ($7,A7,D7.w),D1`),
  then $1C1610. frame+0x07 (dpal) is written **only** from the stream itself: the bit-9 prefix
  block (0xD2F4 / 0xD84C) or the bit-8 trailer block (0xD9D2 / 0xD3D8) reads it when the block's
  flag byte has bit 4 set, else it is cleared (0xD2C8 etc.). **Nothing sets it at compile entry
  per object, and DA12 never touches it.**
* **DA12 (type-2 interaction cell)** adds **no palette bias at all**. What it does:
  - reads W0 (partner frame index) and W1 (own-list offset) from the drawer's stream;
  - own-role list = **drawer's bank** + W1 (frame+0x0A);
  - partner list = **partner's bank**: partner id = `($2,partner_obj)` via the obj+0x26 link,
    list = `$38FB8[pid] + $38F14[pid].table[W0]` (0xDA30..0xDA70). If that table entry is
    0xFFFE the whole DA12 cell aborts (0xDA50) — nothing is drawn (this is why some holds are
    only drawable from one side);
  - per-partner dx/dy byte pair from the drawer stream table at cell+6 (indexed pid*2) goes
    into frame+0x12/13 for partner chunks (0xDBA0/0xDBEC), cleared for own chunks (0xDAF6...);
    this word is **position only** (added to X/Y at 0xD316/0xD334, 0xD650/0xD67A);
  - the interleaved chunks are ordinary D2AE/D802 cells (bit14 of each chunk header selects),
    with all the palette rules above.

**ROM ground truth** (dumped with palex.py): every character's data has his **absolute id baked
in**. Char N solo cells: constant pal channel = N (e.g. c0 → 0, c1 → 1, c2 → 2, c3 → 3, c4 → 4);
shared body sub-streams carry raw 4 with dpal = (N-4)&0xFF in each character's own wrapper
(c0: dpal=0xFC, c1: dpal=0xFD) so raw+dpal = N again. DA12 scan over all 75/78 grapple frames:
drawer 0 vs partner 1 → own parts index **0**, partner parts index **1**; drawer 1 vs partner 0
→ own parts index **1**, partner parts index **0**.

## 3. The rule

> **The index into $1C1610 is always the absolute character/palette-resource id of the
> character whose data bank the bytes were fetched from — never a role.**
> `emitted_bank = $1C1610[(raw_stream_byte + dpal) & 0xFF]`, where dpal is frame+0x07 for
> D43A/D540 sub-cells and 0 for the D2AE/D802 top loops; and `raw+dpal` always equals the
> owning character's id because each bank's wrappers bake it in.

Engine implementation, given (drawer id D, partner id P, raw byte r, path):

| path | index into $1C1610 |
|---|---|
| D2AE top / D802 packed top | `r` (data comes from the current sub-list's owning bank; = that character's id) |
| D43A/D540 → D71A / D466 | `(r + dpal) & 0xFF` |
| DA12 own-role chunks | decode drawer's bank list → indices come out = **D** |
| DA12 partner chunks | decode **partner's bank** list (frame index W0 via partner's $38F14 table) → indices come out = **P** |

Equivalently, if the engine keeps a role-relative decode for grapples: own role → bank of D,
partner role → bank of P; final bank byte = `0x80 | slot(id)` from the live allocator map.

Checking the four measured cases:
1. Hogan (0) solo: r=0 (or 4+0xFC) → index 0 → $1C1610[0] = Hogan's bank. ✓
2. Warrior (1) solo: r=1 (or 4+0xFD) → index 1 → Warrior's bank. ✓
3. Hold, Hogan's object draws: own list (Hogan bank) → 0; partner list (Warrior bank) → 1. ✓
4. Hold, Warrior's object draws: ROM says own → 1, partner → 0 — i.e. **the bytes still name
   Hogan 0 and Warrior 1**. The measured "0=own / 1=partner regardless of drawer" is a role
   *attribution* artifact, not a machine bias: for many holds only one direction's DA12 data
   exists (the other side's W0 entry is 0xFFFE and aborts — e.g. frame 21/23 abort for
   drawer 0 + partner 1), so the same character ends up drawing the pair whichever wrestler
   initiates, and with the pair Hogan(0)/Warrior(1) the emitted indices are 0 and 1 either way.
   No bias register or table rewrite exists to make raw 0 mean "drawer": if the engine emits
   drawer's bank for raw 0 when Warrior draws it will colour Warrior's body with Hogan's
   palette — the real machine never does that.

## 4. Tag matches

Identical mechanism — each of the 4 wrestler objects allocates `$1C1610[its char id] = 0x80|slot`
at spawn via 0x2AEA, and DA12 resolves partners through obj+0x26 → partner's obj+2 id exactly as
in singles; nothing about $1C1610 or the compiler differs in tag.
