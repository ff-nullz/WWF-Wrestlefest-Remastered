# Draw order — ROM 0x27B8 (enqueue) / 0x2948 (sort) / 0x28FA+0x2836 (drain)

Source: `reference/maincpu.asm` lines 2582–2700 (PC 0x27B8–0x2986), plus the
per-state `+0x12` writers listed in §3. Read-only transcription; nothing compiled.

## TL;DR — the one rule

Every drawable object carries a **draw-list index in word `+0x12` (0..5)**.
`0x27B8` appends the object *pointer* to that list in call order. `0x2836` drains
the lists **5,4,3,2,1,0** into spriteram from index 0 upward, and the rasteriser
paints ascending spriteram index with plain overwrite — so **list 0 is frontmost
and, inside a list, the record emitted later wins**. Lists **5,4,2,0 are bubble-sorted
descending by world depth `+0x0A` (NOT screen y `+0x16`)**, stable on ties, so the
smallest `+0x0A` (nearest the viewer) is emitted last and draws on top. Lists 3 and 1
(rope halves) are not sorted. Referee, wrestlers and effects all live in list 0 by
default; ties in `+0x0A` resolve by enqueue order = referee first (0x1F914 runs before
0xF4C2 in both frame lists), then wrestler slots 0..8.

**The engine's bug is the sort key.** `eng_sprite_emit` sorts by `sy` (= `+0x16` =
y + z − camY + per-frame hotspot_y). The ROM never looks at `+0x16` for ordering; it
sorts on raw `+0x0A`. After the leg drop Hogan's lying-pose hotspot_y differs from
Warrior's, so `+0x16` order flips even though Hogan's `+0x0A` is smaller (nearer).
Same mechanism for the referee ("wrestlers over the ref even when the ref is in
front"): hotspot/z differences leak into the key.

## 1. 0x27B8 — enqueue one object (called per object after 0x247C)

```
0027B8  movem.l D0/A1-A3,-(A7)
0027BC  btst  #7,(0,A0)            ; object inactive → skip
0027C4  D0 = (+0x04) & 0x7FFF
0027CC  cmpi.w #$7FFF,D0 ; beq     ; hidden cell (pose 0x7FFF) → skip
0027D2  cmpi.b #$4E,(+0x03) ; bcc $27E0   ; class/row byte >= 0x4E → no clip test
0027DA  bsr $2188 ; bcs skip       ; clip: skip if sx>=0x17F | sx<-0x3F | sy>=0x13F | sy<-0x3F
                                   ;       (sx = +0x14, sy = +0x16, signed compares)
0027E0  lea $2806,A1
0027E6  D0 = (+0x12) << 3          ; list index * 8, NO range check
0027EC  A2 = table[D0+0]           ; pointer array for that list
0027F0  A3 = table[D0+4]           ; count word for that list
0027F4  A2[(*A3)*4] = A0           ; append OBJECT POINTER (not a record)
0027FC  addq.w #1,(A3)             ; count++  (no overflow check; arrays are 0x80 B = 32 ptrs)
002800  rts
```

Table 0x2806 (`{array, count}` per list):

| list | array   | count   |
|---|---|---|
| 0 | $1C19D0 | $1C19B8 |
| 1 | $1C1A50 | $1C19BC |
| 2 | $1C1AD0 | $1C19C0 |
| 3 | $1C1B50 | $1C19C4 |
| 4 | $1C1BD0 | $1C19C8 |
| 5 | $1C1C50 | $1C19CC |

There is **no per-object priority byte** beyond `+0x12`, and no insertion-by-y: the
array is strictly FIFO in call order. Nothing in 0x27B8 reads `+0x33`, `+0x44`,
`+0x21` or `+0x0A`. Visibility gates are only: flags bit7, pose==0x7FFF, and the
clip (rows < 0x4E). `+0x44` bit15 (hidden lockup half) is **not** tested anywhere
in 0x27B8/0x2836/0xD1FC (grep of 0xD1FC..0xDCA0 finds no `+0x44` read); the hidden
half disappears because its pose is driven to 0x7FFF / state 0xFF by the anim path
— TODO EXACT (anim `+0x1D==0xFF` path per grapple-moves.md:104; not re-derived here).

## 2. 0x2836 — drain (once per frame, near the end of both frame lists)

```
002836  clr.w $1C19B2                 ; new record count
002840  A2 = $C2000                   ; spriteram write cursor, ascending
002846  A1=$1C1C50 A6=$1C19CC ; bsr $2948 (sort) ; bsr $28FA (emit)   ; list 5
00285A  A1=$1C1BD0 A6=$1C19C8 ; sort ; emit                            ; list 4
00286E  A1=$1C1B50 A6=$1C19C4 ;        emit                            ; list 3  (NO sort)
00287E  A1=$1C1AD0 A6=$1C19C0 ; sort ; emit                            ; list 2
002892  A1=$1C1A50 A6=$1C19BC ;        emit                            ; list 1  (NO sort)
0028A2  A1=$1C19D0 A6=$1C19B8 ; sort ; emit                            ; list 0
0028B6  $1C19B2 = (A2-$C2000)>>4      ; records written this frame
0028C8  if old count $1C19B4 > new: clr.b (+3,A2) for each stale record (disable bit0)
0028EA  $1C19B4 = $1C19B2
```

### 0x2948 — per-list sort (lists 5,4,2,0 only)

```
002948  D0 = count-2 ; bcs exit       ; <2 entries: nothing
002954  pass: A4 = array, D2 = 0
00295A    A2 = A4[0], A3 = A4[4]
002962    D1 = A2->(+0x0A) ; cmp.w A3->(+0x0A),D1 ; bge keep
00296C    swap A4[0]<->A4[4] ; D2 = 1          ; swap only when A2.y < A3.y  (strict)
002976    A4 += 4 ; dbra D0
00297E  if D2: repeat pass
```

Result: array ordered by **`+0x0A` descending** (largest world-Y / farthest first),
**stable** for equal `+0x0A` (strict `<` swap). Key is world depth alone — no z
(`+0x0E`), no hotspot, no `+0x16`.

### 0x28FA — emit one list

```
0028FA  if count==0 return ; D1 = count-1 ; count = 0
002906  A0 = *(A1)+                         ; ascending array order
002908  if (+0x21)==0xFF skip               ; state 0xFF objects are never compiled
002912  if ((+0x04)&0x7FFF)==0x7FFF skip    ; hidden cell (re-checked)
002926  jsr $D1FC                           ; compile records at A2, A2 advances
002930  if (+0x26) bit7 && (+0xFE)==0: clr.l (+0x26)   ; consume one-shot override
002942  dbra D1
```

### Resulting on-screen priority

`src/video.c:911 draw_sprites()` walks `s = 0 .. WF_SPRRAM_SIZE` step 16 and
`draw_native_sprite_command` overwrites pixels (`video.c:672`), identical to MAME
`ddragon3_v.cpp`. So **higher spriteram index draws on top**; therefore list 0 over
list 1 over ... over list 5, and inside a sorted list the smallest `+0x0A` is on top.

With wrestlers/referee/effects in list 0 and ropes in 3 (back) / 1 (front) the
stack, bottom→top, is: list 5 · list 4 (wrestler outside/far states) · **back rope
halves** · list 2 (rope-lean / apron states) · **front rope halves** · list 0
(referee, wrestlers, effects, sorted by `+0x0A`).

## 3. Who writes `+0x12` (all `move.w #n,(+0x12,A0)` / `clr.w` hits, A0 = object)

Default / reset to **list 0**: 0xFA4C, 0xFBF2 (match-start slot init after 0x250E
walk), 0x114D8 (state reset), 0x121F8, 0x12448, 0x183E0, 0x18A4C, 0x18AE0, 0x18B48,
0x19C5A (`+0x33` bit2 set + y=0x152: outside, near side → list 0), 0x120E6, 0x12070,
0x12118, 0x1234E (return from outside), 0x73CA (non-match attract), 0xFEAC
(ring-hardware/extra object reset).

**List 2** (between rope halves: behind front ropes, in front of back ropes):
0x18382 (rope lean, `+0x2A=0x12`, `+0x19=0x18`), 0x11FEE (z=0x140, pos from table by
`+0x44&3`), 0x1238A (z=0x140), 0x18840, 0x189FC, 0x18AB2, 0x18B1A, 0x1A3FA
(`+0x33` bit2 set = outside flag), 0xFF3C/0xFF6A (ring-hardware object variants).

**List 4** (behind the back ropes): 0x117B6 (`+0x44=1`, `+0x2A=0x10`), 0x1204C and
0x1233E (z=0x180, x±0x18, facing flipped — far-side outside/apron; both immediately
override to **list 0 if `+0x45` bit1** at 0x12058/0x12344 — near side), 0x120A0
(pos table 0x121C8 by `+0x44&3`), 0x1889A, 0x189B6, 0x18B74, 0x1A438.
TODO EXACT: the exact meaning of `+0x45` bit1 (near/far side latch) — only bit0 is
documented (out-of-ring.md:128); PCs 0x12052, 0x12344, 0x120DE.

**List 1 / 3**: only the ring hardware: 0x10074/0x100DC (front halves = 1),
0x100A8/0x10112 (back halves = 3). 0xF8EE (outside state): wrestler → **list 1**,
rope object `$1C1134`/`$1C1164` (A1) → list 0 — i.e. when a man is outside on the
near side the rope half is re-authored to list 0 over him (ring-hardware.md:127).

**List 5**: no writer found in the match code (grep) — TODO EXACT whether anything
other than attract/intro uses it. 0xB6DA (`+0x12 = slot+1`, fixed screen `+0x16=0x7E`,
class 0x4D) is an attract/roster screen, not a match.

## 4. Special cases

**Referee `$1C11F4`.** Only writer: 0xFCE2 (`clr.w`, between-fall reset) → list 0,
never changed (0x1F914 writes no `+0x12`). It is enqueued by 0x1F914, which precedes
0xF4C2 in both frame lists (frame-loop.md:41,51), so on an exact `+0x0A` tie the ref
is *behind* the wrestlers; otherwise pure `+0x0A` sort decides. The engine's "ref
first, then stable sort" matches this — only its key (`sy`) is wrong.

**Wrestlers.** 0xF4C2 walks slots 0..8 via 0x250E (table 0x5122) and 0xF518 calls
0x27B8 at 0xF552 per slot, so within-list FIFO is slot order; the 0x2948 sort then
reorders by `+0x0A`.

**Pinned / held composite (DA12 two-man art).** The held man is frozen in state
0xFF (pin-exact.md:22, pins-referee.md:308) → dropped at 0x2908, never compiled.
The composite is compiled by the holder's object alone, in the holder's list, at the
holder's `+0x0A`. The hidden lockup half (`+0x44` bit15) likewise contributes no
records (pose 0x7FFF / state 0xFF via the anim path; 0x27B8 itself does not test
`+0x44`).

**Effects / dust — 0x10D3A(k).** Allocates one of 11 objects at `$1C1258` (stride
0x2A); table 0x10DDA, 6 bytes per id `{dy.w, spr.w, list.w}`: copies the spawner's
class `+0x02` and x/y/z, then `+0x0A += dy; +0x0E -= dy` (screen y unchanged, depth
nudged nearer: dy = −2/−4/−1, some 0), `+0x04 = spr | facing`, `+0x12 = list` —
**every table entry's list word is 0** (0x10DDA..0x10E34). Calls 0x247C/0x27B8 at
0x10DC6/0x10DCC at spawn time (during the state handler, i.e. before the referee and
wrestler enqueues that frame); subsequent frames re-enqueue via the effect ticker
(0x10E6A — TODO EXACT, not read). Net: effects draw over the wrestler they spawn on
because their `+0x0A` is smaller (or tie → FIFO).

**Ropes.** Class-14 objects `$1C1134/$1164/$1194/$11C4`, list 1 (front halves) /
3 (back halves), enqueued by 0x10120 after 0xF4C2 (frame-loop order), unsorted.
Post-0x2836 stack is exactly ring-hardware.md:67–72.

## 5. C sketch for eng_sprite_emit (matches the ROM)

```c
/* list[k] = FIFO of object refs; k = obj->list (+0x12), default 0.
 * Enqueue order per frame: referee (0x1F914), wrestler slots 0..8 (0xF4C2),
 * effects (0x10E6A tick; spawn-frame effects go in even earlier), ring hw (0x10120). */
struct ent { int wy /* +0x0A */; unsigned row; uint16_t spr; int sx, sy; int prow; };
struct ent L[6][32]; int n[6] = {0};

#define PUSH(k, e) do { if (n[k] < 32) L[k][n[k]++] = (e); } while (0)

if (ref.active && !hidden(ref))        PUSH(ref.list /*0*/, ref_ent);
for (slot = 0; slot < 9; slot++)       /* 0x250E order */
    if (obj[slot].active && !hidden(obj[slot])) PUSH(obj[slot].list, ent(slot));
for each live effect (spawn order)     PUSH(0, fx_ent);
for each rope half                     PUSH(half.list /*1 or 3*/, rope_ent);

/* hidden(o): (o->spr & 0x7FFF) == 0x7FFF || o->state == 0xFF || clipped(o) (0x2188,
 * only rows < 0x4E).  +0x44 bit15 is NOT a ROM test — keep only if the anim path
 * cannot be trusted to have set pose 0x7FFF. */

static void sort_far_to_near(struct ent *a, int cnt)     /* 0x2948 */
{
    for (int swapped = 1; swapped && cnt >= 2; ) {       /* stable bubble, desc +0x0A */
        swapped = 0;
        for (int i = 0; i + 1 < cnt; i++)
            if (a[i].wy < a[i + 1].wy) { swap(a[i], a[i + 1]); swapped = 1; }
    }
}

static const int drain[6] = {5, 4, 3, 2, 1, 0};          /* 0x2836 */
for (int d = 0; d < 6; d++) {
    int k = drain[d];
    if (k != 3 && k != 1) sort_far_to_near(L[k], n[k]);  /* ropes stay FIFO */
    for (int i = 0; i < n[k]; i++)                       /* 0x28FA: ascending index */
        eng_sprite_emit_pose(L[k][i].row, L[k][i].spr, L[k][i].sx, L[k][i].sy,
                             L[k][i].prow, &slot);       /* later record = on top */
}
```

Minimal diff to the current engine: (1) sort key `o->y` (world `+0x0A`) instead of
`o->sy`; (2) carry `+0x12` per object (default 0; rope-lean=2, outside far=4, outside
near=0, 0xF8EE outside=1) and bucket before draining 5→0 with ropes at 3 and 1;
(3) effects into list 0 with their −dy depth nudge. Items (2)/(3) only matter once
those states exist in the engine; (1) alone fixes both playtest reports.
