# WrestleFest animation system — C transcription spec

Source: `../wrestlefest-decomp/reference/maincpu.asm` (read-only pass, 2026-08-22).
Routines: cell pick `0x1C03E`/`0x1C090`, tick `0x1C0C8`, frame load `0x1C12C`.
Sole caller: `0xF546` inside the per-object update `0xF518`, driven by the object loop `0xF4C2`
(iterator `0x250E` over the 9 slot pointers at `0x5122`; live = `+0x00` bit7).

All word fields are big-endian 68k words; "low byte of word +0xNN" is the byte at +0xNN+1.

---

## 0. Object fields used by the anim core

| offset | size | role |
|---|---|---|
| `+0x01` | b | motion mode for `0x2208`: 0 none, 1 polar (+0x2A speed, +0x2C angle), 2 velocity (+0x58/5A/5C). Written by cell handlers, not by the anim core. |
| `+0x02` | w | wrestler id (index into per-wrestler byte tables) |
| `+0x04` | w | **current sprite word**. bit15 = h-flip (byte +0x04 bit7; also mirrors hitboxes, cf. `0x24FE4` doc). Low byte (`+0x05`) = frame number. `value & 0x7FFF == 0x7FFF` ⇒ not drawn (`0x27C4-0x27D0`). |
| `+0x18` | w | sprite X offset, signed low byte (`ext.w` at `0x2488`), negated when facing (`0x24A2-0x24AA`); **bit15 = "clear after this anim pass"** (see §1 tail) |
| `+0x1A` | w | sprite Y offset, signed low byte (`0x248A-0x2492`, applied `0x24C2`) |
| `+0x1C` | w | **anim selector**: bit15 = "anim initialized", low byte (`+0x1D`) = source id (copied from state, below). Value $FF ⇒ hidden cell. |
| `+0x1E` | w | previous anim selector (history copy, `0xF4E0`) |
| `+0x20` | w | **state word**: bit15 = "state entered", low byte (`+0x21`) = state id. `+0x21 == $FF` ⇒ anim wholly skipped. |
| `+0x22` | w | **frame countdown** (duration counter; `0xFF00` = hold forever) |
| `+0x24` | w | **frame index**; low byte `+0x25` is what game logic polls; `0xFE` = "sequence finished" sentinel (e.g. `0x125B0`, `0x11E1A`) |
| `+0x26` | l | grapple/interaction **partner object pointer** (mutual: partner->+0x26 must point back, cf. `0x115D2`); passed to cell handlers in A2, `0` ⇒ dummy `$7F000` |
| `+0x2A` | w | walk/run **speed**; low byte `+0x2B` = per-wrestler speed byte (see §2b — it is NOT a sprite selector) |
| `+0x2C` | w | walk **angle**; effective value is `word & 0xFF` (low byte `+0x2D`), 256-step circle |
| `+0x2E` | w | **facing/flip mask**, xor'ed into every loaded sprite word. Engine uses `0x0000` / `0x8000` (bit7 of byte `+0x2E` = word bit15). |
| `+0x60` | w | move id; low byte `+0x61` indexes the move cell table when `+0x1D == 5` |
| `+0x64` | w | victim/reaction id; low byte `+0x65` indexes the victim cell table when `+0x1D == 4` (bit7 of the id set by `0x24E4C` for band 2) |

Handler-side extras referenced below: `+0x4C` hit/hurt record id, `+0xAE` walk sub-state,
`+0x74/+0x75` weapon flags, `+0x32` bit3 run flag / bit2 "facing changed", `+0xFE/+0xFF`
input words, `+0xA8` input code, `+0xA9` input edges.

## Cell record layout (confirmed against 0x115F2, 0x1160A, 0x11C62, 0x11D82, 0x11DD0, 0x125C0)

```
+0  long  handler PC          (always present)
+4  word  mode: 0 = tick does NOTHING (handler-driven cell)
                2 = loop; any other nonzero (engine uses 1) = hold-last
+6  word  n                   (only if mode != 0)
+8  n words  durations        (0xFF00 = hold forever on that frame)
+8+2n n words sprite words    (xor'ed with +0x2E at load)
```
Mode-0 cells are truncated after +4 — handler code often starts at +6 (e.g. stand cell
`0x114AC` = {handler `0x114B2`, mode 0} and the code IS `0x114B2`).

---

## 1. Per-frame flow — EXACT semantics

### 1a. State→anim latch (`0xF4D8-0xF4F0`) — the only thing that "re-picks" a sequence

Every frame, per object, before anything else:
```
if !(+0x20 bit15):            ; a state writer stored a new state (bit15 clear)
    +0x1E = +0x1C             ; save previous anim selector
    +0x1C = +0x20             ; anim selector := state  (bit15 clear ⇒ anim re-inits)
    +0x20 |= 0x8000           ; mark state entered
```
So **`+0x1D` changes only via `move.w #state,(+0x20)` writers**; there is no other writer of
`+0x1C` on the wrestler path. Clearing bit15 of `+0x1C` is what restarts a sequence.

### 1b. `0x1C03E` — pick + handler + tick (runs EVERY frame, cell ptr is never cached)

```
save D0/A1/A4
if +0x1D == 5:            A1 = table 0x12614;  D0 = word +0x60     ; 0x1C042-54  MOVE
elif +0x1D == 4:          A1 = table 0x1AFD4;  D0 = word +0x64     ; 0x1C056-68  VICTIM
else:                     A1 = table 0x11478;  D0 = word +0x1C     ; 0x1C06A-70  STATE
      if (D0 & 0xFF)==0xFF: A1 = cell 0x125C0; goto have           ; 0x1C074-80  HIDDEN
if byte +0x21 == 0xFF: restore, rts                                ; 0x1C082 guard (table paths only)
A1 = *(A1 + (D0 & 0xFF)*4)                                         ; 0x1C08A-90  ← the pick
have:
A2 = long +0x26; if A2 == 0: A2 = 0x7F000                          ; 0x1C094-A0  partner or ROM bit-bucket
A4 = *(long*)(A1+0); jsr (A4)         ; HANDLER — A0=obj, A1=cell, A2=partner   ; 0x1C0A6-AA
jsr 0x1C0C8                           ; TICK, using A1 **as the handler left it**; 0x1C0AC
if word +0x18 bit15: +0x18 = 0; +0x1A = 0                          ; 0x1C0B2-C0
restore, rts
```
Exact priority: **move (5) > victim (4) > state**, tested in that order on `+0x1D`;
`+0x1D==0xFF` is a sub-case of the state branch and bypasses the `+0x21` guard.

**The handler** (cell +0):
- called **every frame**, **before** the tick, with A0 = object, A1 = its own cell record,
  A2 = partner object (or the ROM address `$7F000` as a harmless write sink);
- is where all per-state game logic lives (state transitions, speed, +0x4C, +0x01, …);
- **may replace A1**; the tick then animates the substituted cell (this is how walk swaps in
  the run/weapon cells, §4). Because 0x1C03E's `movem` restores A1 only after the tick,
  the substitution is scoped to this one frame.

### 1c. `0x1C0C8` — tick (A0 = obj, A1 = cell)

```
if word cell+4 == 0: rts                          ; 0x1C0CC  mode 0: fully handler-driven
if !(+0x1C bit15):                                ; 0x1C0D4  first tick of a sequence
    +0x24 = 0; load(0x1C12C); +0x1C |= 0x8000; rts
if +0x22 == 0xFF00: rts                           ; 0x1C0EC  hold-forever duration
+0x22 -= 1; if no borrow (old value != 0): rts    ; 0x1C0F4-F8  (subq/bcc: advance ONLY when it was 0)
+0x24 += 1
if +0x24 < cell.n: load(0x1C12C); rts             ; 0x1C0FE-106
+0x24 = 0xFE                                      ; 0x1C10A  finished sentinel (logic reads +0x25)
if cell.mode == 2: +0x24 = 0; load(0x1C12C); rts  ; 0x1C110/11E/122  LOOP
else:              +0x22 = 0; rts                 ; 0x1C118  HOLD-LAST: no reload, +0x04 keeps last sprite
```
- A frame with duration `d` is displayed for **d+1 tick calls** (loaded with `+0x22=d`,
  advanced on the tick that decrements 0→0xFFFF).
- **Mode 1 (hold-last)**: `+0x04` is simply never rewritten; `+0x24` stays `0xFE` (each
  further tick re-enters the end branch: 0→borrow→0xFF→≥n→0xFE, `+0x22=0`). `+0x25==0xFE`
  is the universal "anim done" test in move handlers.
- **Mode 2 (loop)**: frame wraps to 0 and reloads; the transient `0xFE` is overwritten.
- **Mode 0**: tick returns before even the init, so the handler must set `+0x1C` bit15,
  `+0x04`, `+0x22`, `+0x24` itself (the stand handler `0x114B2` opens with a test-and-set
  `bset #7,(+0x1C)` for exactly this).

### 1d. `0x1C12C` — frame load (A0 = obj, A1 = cell, uses `+0x24`)

```
f = +0x24
+0x22 = word at cell+8 + 2*f                      ; duration → countdown
spr   = word at cell+8 + 2*cell.n + 2*f           ; sprite word
+0x04 = spr ^ word +0x2E                          ; 0x1C144-4A  FULL 16-bit xor
```
**The entire +0x2E word is xor'ed**, not just one bit; since the engine only ever stores
`0x0000`/`0x8000` there (`bset/bclr/bchg #7` on byte +0x2E, or `move.w #$8000`, e.g.
`0xF382/0xF392/0x100E2/0x1BFCC`), the practical effect is toggling sprite bit15 = h-flip.
Handlers that set `+0x04` manually do the same by `or`/`eor`/`move` of `+0x2E`
(`0x11564`, `0x11590`, `0x11C1C`, `0x1C012-1C01A`, `0x108A4`, …).

---

## 2. Sprite word and the +0x2B question

### 2a. `+0x04`
Composed exactly as in §1d. Consumers:
- `0x247C`: screen position = `X(+0x06) − scrollX($1C17E6) + sext(lowbyte +0x18)`
  (offset negated when `+0x2E` bit7 set — sprite offsets are facing-mirrored),
  `Y = (+0x0A) + Z(+0x0E) − scrollY($1C17EE) + sext(lowbyte +0x1A)`.
- `0x27B8`: enqueue object into the per-layer (`+0x12`) display lists at `$1C19B8+` unless
  `(+0x04 & 0x7FFF) == 0x7FFF` — i.e. the hidden cell's `0xFFFF` sprite is culled here.
- Hit code mirrors hitbox X by `+0x04` bit7 (byte), i.e. the same flip bit.

### 2b. `+0x2B` is a per-wrestler WALK/RUN SPEED, not a sprite selector (doc correction)
Every writer/reader is movement-related:
- `0x1174C` (called from walk handler `0x116C6`): `clr.w +0x2A; +0x2B = table[+0x02]` with
  table `0x116AE` = `1B 1D 1C 19 1E 1B 18 1E 16 1B 1C 1B` (normal walk) or `0x116BA` = all
  `0C` when `+0x32` bit3 (post-whip/slow walk); `+0x2B += 6` if `+0x56` bit7 && `+0x33` bit5
  (`0x11780`); `+0x2A -= 8` (word) when carrying a weapon (`0x1178E`).
- `0x11C96-0x11CA4` (run state init): `+0x2B = table 0x11D4A[+0x02]` =
  `2F 35 31 2D 38 30 2C 38 28 2F 30 2F` — per-wrestler **run speed** (same table reused by
  the whip paths `0x17618`/`0x17728`). Fixed speeds for managers/referee at
  `0x1FAE4/0x1FC42/0x1FF5A/0x20604`.
- Consumed only by the polar mover `0x222C-0x2250` (motion mode `+0x01 == 1`):
  `D0 = +0x2C & 0xFF` (angle), `D1 = +0x2A & 0xFF` (speed); quadrant dispatch `0x22C0`
  over sin/cos tables `0x2378`/`0x23FA`; products added to the 32-bit X (`+0x06`) and
  Y (`+0x0A`) fixed-point positions.

So `+0x2B` never touches cell or sprite selection. Per-wrestler artwork differences are
resolved downstream of `+0x04` in the sprite-stream decoder (`0xD25C`/`0xD278` over
`0x38FB8`/`0x38F14`), outside these routines. The gameplay-tables rows calling `0x116AE`/
`0x11D4A` "anim byte" tables should be relabeled "walk/run speed".

---

## 3. When a new cell is (re)picked

- The pick runs **every frame**; nothing caches the cell pointer in the object. What makes a
  *different* cell appear is a change of `+0x1D` / `+0x61` / `+0x65`, and what makes a
  sequence *restart* is bit15 of `+0x1C` being cleared.
- Both normally happen together via the latch `0xF4E0-0xF4F0` (§1a): any code doing
  `move.w #N,(+0x20)` (bit15 clear) causes, at the next frame top:
  `+0x1E = +0x1C; +0x1C = N` (bit15 clear ⇒ tick re-inits at frame 0).
  - `N == 5` + `move.w #move,(+0x60)` ⇒ move cell `0x12614[+0x61]` (144 entries; ids like
    `$8014` still index by the low byte — high bits are flags).
  - `N == 4` + `move.w #reaction,(+0x64)` ⇒ victim cell `0x1AFD4[+0x65]` (45 entries).
  - other `N` ⇒ state cell `0x11478[N]` (13 entries, `data/romdata/mv_state_cell_ptrs.json`).
    Entries 4 and 5 are the shared placeholder `0x11DCA` = {handler `0x12612` = `rts`,
    mode 0} — unreachable in practice because `+0x1D` 4/5 short-circuit above.
  - `N == $FF` (as `+0x1D`) ⇒ single hidden cell `0x125C0` = {handler `0x125CC`, mode 1,
    n 1, dur `0xFF00`, sprite `0xFFFF`}: handler forces `+0x04 = 0xFFFF`, `+0x01 = 0`, and
    validates the partner back-pointer (24-bit compare `0x125D6-0x125EA`), resetting the
    object to state 0 and clearing `+0x26/+0x9C/+0xB4/+0xB6` and flag bits if stale.
  - `+0x21 == $FF` (state word low byte) freezes everything: no handler, no tick, `+0x04`
    keeps its last value (`0x1C082`). Distinct from the `+0x1D == $FF` hidden cell.
- A handler can also restart its own cell by clearing `+0x1C` bit15, or hard-select a cell
  for one frame by swapping A1 (§1b), or bump `+0x24`/`+0x22` directly (mode-0 cells,
  e.g. `0x1C004` drives `+0x22` as a raw loop counter and toggles `+0x2E` bit7 each pass).

---

## 4. The walk animation

State cells involved (all in `0x11478`): 0 stand `0x114AC`, 1 walk `0x115F2`,
2 run `0x11C62`, 3 skid `0x11D82`, 6 turn `0x11DD0`.

- **Standing pose (state 0)** — cell `0x114AC`, **mode 0**, handler `0x114B2`.
  First frame (test-and-set `bset #7,(+0x1C)` at `0x114B2`): clears `+0x01` (no motion) and
  the whole anim/velocity block (`+0x22/+0x24/+0x18/+0x1A/+0x3E..+0x42/+0x44/+0x46/+0x12/+0xAA`).
  Every later frame: `+0x4C = 1`; partner sanity check `0x115D2`; **sprite = frame 0**:
  `move.w (+0x2E),(+0x04)` at `0x11564` (sprite word 0 + flip). Weapon held (`+0x74` bit7):
  `+0x4C=$E`, sprite `$7E` or `$82` by `+0x75` bit0, OR `+0x2E` (`0x11572-0x11594`).
  Input (`+0xFE` bit7 gate): `+0xFF` bit0 ⇒ state 5 move `$8C`; bit1 ⇒ nothing;
  otherwise ⇒ **state 1** with `+0xAE = $C` (`0x11598-0x115D0`).

- **Walk cycle (state 1)** — cell `0x115F2`: handler `0x11652`, **mode 2 loop, n=4,
  durations 10,10,10,10 (=11 ticks/step), sprite words 1,2,3,4**.
  Handler `0x11652`: in attract clears run flag; dispatches sub-state `+0xAE & 0x3F` via
  table `0x1167A` (0 = plain walk `0x116C6`; 1 = `0x11796` (turnbuckle mount: `+0x2A=$10`,
  `+0x44=1`…); 2/3/4 = `0x1192C/0x11A20/0x11ABA`; $C = `0x11B6C`; the rest alias `0x116C6`).
  Plain walk `0x116C6`: on first frame `+0x01 = 1` (polar motion); every frame
  `bsr 0x1174C` (speed, §2b), `+0x4C = 1` (or `$E` with weapon), and **`bsr 0x11710` which
  replaces A1** so the tick animates:
    - weapon held (`+0x74` bit7): cell `0x11622` (sprites `7C-7F`, dur 20,16,20,16) or
      `0x1163A` (sprites `80-83`) selected by `+0x75` bit0 (`0x11718-0x11734`);
    - else run flag `+0x32` bit3: cell `0x1160A` (sprites `5-8`, dur 12,12,12,12);
    - else the walk cell `0x115F2` itself.
  Since all variants share n=4, the frame index carries across a swap seamlessly.
  `+0xFE != 0` ⇒ back to state 0 (`0x11702`).

- **Direction & facing**: movement direction is the angle `+0x2C` from input-code table
  `0xF2D6` (`0,40,C0,0,0,20,E0,0,80,60,A0…`, readers `0xF2BC`, `0xF498`), speed `+0x2A`.
  Facing is maintained by `0xF34C` (and the mirror pair `0xFB10/0xFB20`): horizontal input
  `+0xA8 & 3` == 1 ⇒ angle `$40`, `bset #7,(+0x2E)` (`0xF37C-0xF388`); == 2 ⇒ angle `$C0`,
  `bclr #7,(+0x2E)` (`0xF38C-0xF398`); vertical-only ⇒ angle recomputed from current facing
  (`0xF39C-0xF3AC`), facing untouched. A facing change sets `+0x32` bit2 (`0xF3B4-0xF3C2`).
  **Frames are never direction-selected** — one 4-frame cycle serves both directions and is
  mirrored purely by the `^ +0x2E` at load (`0x1C144`) flipping sprite bit15.

- **Reversal/stop**: direction-reversal detection (`0xF42E-0xF45A`, `+0x2D` bit7 vs `+0xA9`
  edge bits) enters state 3 — cell `0x11D82` = {mode 1, n=1, dur `0xFF00` hold, sprite `$0A`
  (skid pose)}; its handler decelerates `+0x2A -= 5` until negative ⇒ state 0
  (`0x11DB2-0x11DC4`), or via `0x10FC6`-carry ⇒ state 6. State 6 (turn) — cell `0x11DD0` =
  {mode 1, n=2, dur 8,12, sprites `$69,$69`}; handler `0x11DE0` on entry does
  `bchg #7,(+0x2E)` **and** `bchg #7,(+0x2D)` (flip facing and angle, `0x11DE8-0x11DEE`),
  restarts motion at `+0x25 == 1`, and on `+0x25 == 0xFE` ⇒ state 2 with `+0x18` bit15 set.

- **Run (state 2)** — cell `0x11C62` = {handler `0x11C82`, mode 2, n=6, dur 6×6, sprites
  `9,A,B,C,D,E`}; init: `+0x01 = 1`, run speed `+0x2B = 0x11D4A[+0x02]`, `bset #4,(+0x33)`.

---

## 5. C sketch (integer-only)

```c
/* ROM cells are raw big-endian bytes; keep them as const u8* into the ROM image. */
#define CELL_MODE(c)  be16((c)+4)      /* 0 none, 2 loop, else hold-last */
#define CELL_N(c)     be16((c)+6)
#define CELL_DUR(c,f) be16((c)+8+2*(f))
#define CELL_SPR(c,f) be16((c)+8+2*CELL_N(c)+2*(f))

/* Fields the engine object struct must carry (68k offsets kept for oracle diffing):
   u16 spr;        // +0x04  sprite word (bit15 flip; 0xFFFF hidden)
   u16 off_x;      // +0x18  low byte = signed X offset, bit15 = clear-after-anim
   u16 off_y;      // +0x1A
   u16 anim_sel;   // +0x1C  bit15 init, low byte = source id
   u16 prev_sel;   // +0x1E
   u16 state;      // +0x20  bit15 entered, low byte = state id ($FF = frozen)
   u16 count;      // +0x22  countdown (0xFF00 hold)
   u16 frame;      // +0x24  frame idx (0xFE finished)
   obj *partner;   // +0x26
   u16 speed;      // +0x2A  (low byte written from per-wrestler tables)
   u16 angle;      // +0x2C  (low byte effective)
   u16 flip;       // +0x2E  0x0000 / 0x8000
   u16 move_id;    // +0x60  (idx = &0xFF)
   u16 victim_id;  // +0x64  (idx = &0xFF)
   plus handler-owned: u8 motion(+0x01), u16 wid(+0x02), u16 hitrec(+0x4C),
   u16 walk_sub(+0xAE), flags +0x32/+0x33, weapon +0x74/+0x75, input +0xFE/+0xA8/+0xA9. */

/* handlers: called every frame BEFORE the tick; may substitute the cell. */
typedef const u8 *(*cell_handler)(obj *o, const u8 *cell, obj *partner);

static void anim_load(obj *o, const u8 *c)         /* 0x1C12C */
{
    o->count = CELL_DUR(c, o->frame);
    o->spr   = CELL_SPR(c, o->frame) ^ o->flip;    /* full 16-bit xor of +0x2E */
}

static void anim_seq_tick(obj *o, const u8 *c)     /* 0x1C0C8 */
{
    if (CELL_MODE(c) == 0) return;                              /* handler-driven   */
    if (!(o->anim_sel & 0x8000)) {                              /* first tick       */
        o->frame = 0; anim_load(o, c); o->anim_sel |= 0x8000; return;
    }
    if (o->count == 0xFF00) return;                             /* hold forever     */
    if (o->count-- != 0) return;                                /* subq/bcc 0x1C0F4:
                                                                   advance only on 0→0xFFFF */
    if (++o->frame < CELL_N(c)) { anim_load(o, c); return; }
    o->frame = 0xFE;                                            /* finished sentinel */
    if (CELL_MODE(c) == 2) { o->frame = 0; anim_load(o, c); }   /* loop             */
    else o->count = 0;                                          /* hold-last: no load */
}

void anim_tick(obj *o)                              /* 0x1C03E — call once per frame */
{
    const u8 *c; u16 idx;
    u8 sel = o->anim_sel & 0xFF;                                /* +0x1D */
    if      (sel == 5)    { c = rom(0x12614); idx = o->move_id  & 0xFF; }
    else if (sel == 4)    { c = rom(0x1AFD4); idx = o->victim_id & 0xFF; }
    else if (sel == 0xFF) { c = rom(0x125C0); goto have; }      /* bypasses +0x21 guard */
    else                  { c = rom(0x11478); idx = sel; }
    if ((o->state & 0xFF) == 0xFF) return;                      /* 0x1C082 frozen    */
    c = rom(be32(c + idx * 4));                                 /* 0x1C090 the pick  */
have:
    {
        obj *a2 = o->partner ? o->partner : &dummy_obj;         /* $7F000 bit bucket */
        c = ((cell_handler)lookup(be32(c)))(o, c, a2);          /* 0x1C0AA           */
        anim_seq_tick(o, c);                                    /* 0x1C0AC           */
        if (o->off_x & 0x8000) { o->off_x = 0; o->off_y = 0; }  /* 0x1C0B2-C0        */
    }
}

void anim_set(obj *o)   /* the state→anim latch, 0xF4E0-0xF4F0; run at frame top,
                           BEFORE anim_tick, for every live object not held (+0x32 b7) */
{
    if (!(o->state & 0x8000)) {
        o->prev_sel = o->anim_sel;      /* +0x1E */
        o->anim_sel = o->state;         /* bit15 clear ⇒ sequence restarts at frame 0 */
        o->state   |= 0x8000;
    }
}
/* Frame order per object (0xF4C2/0xF518): anim_set → gravity/impulse(+0x01,0x280DC)
   → anim_tick(0x1C03E) → screen xform(0x247C) → draw enqueue(0x27B8, culls
   spr&0x7FFF==0x7FFF) → motion(0x2208 by +0x01); then hit pipeline 0x24090. */
```

Porting notes:
- In the native port, cell handlers must be transcribed as C functions keyed by ROM PC
  (the cell records embed handler PCs); the walk handler chain must preserve the
  "handler returns the cell to animate" contract (`0x11710` swap).
- The `$7F000` dummy: reads from it are ROM garbage the original code tolerates
  (only `0x125CC` reads `partner->+0x26` and compares — never equal to a RAM address);
  writes are discarded. A zeroed dummy object reproduces both properties safely.
- Timing: one tick == one game frame at 57.44 Hz (IRQ-driven main loop; ddragon3 h/w).
