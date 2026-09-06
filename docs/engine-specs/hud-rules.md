# Stock in-match HUD, clock, match rules, ladder — transcription from reference/maincpu.asm

Read-only. Line refs are `reference/maincpu.asm` (the in-repo disassembly).
Nothing compiled, nothing edited.

## 0. Conventions used below

- **FG0 cell** = 4 bytes at `0xC0000 + row*0x100 + col*4` (64 cols × 32 rows, pitch 0x100).
  Every HUD writer stores only bytes `+1` and `+3`: `+1 = tile & 0xFF`, `+3 = (pal<<4) | (tile>>8)`.
  Written as `cell(row,col) = tile:pal` here. (Clock writes a long `(tile&0xFF)<<16 | pal<<4|tile>>8`
  via `swap`, same two bytes land in +1/+3.)
- Object row: `$1C05B0 + slot*0x10C`, slots 0..3 = human row (0=P1, 1=P1 partner, 2=P2, 3=P2 partner,
  pair pointers `0xF7C`: `$1C06BC,$1C05B0,$1C08D4,$1C07C8`). CPU row `$1C09E0 + n*0x10C`.
  `+0x00` bit7 live; `+0x56` bit7 CPU, bit6 apron/eliminated; `+0x57` wrestler id.
- Energy fields: `+0x66` hp (current), `+0x6A` pending HUD delta (signed, written `-dmg` at `0x24E84`),
  `+0x6C` delta accumulator, `+0x6E` displayed hp, `+0x70` band 0..2 (from `0x24EEC`), `+0x72` hp max.
  (`+0x6A` is NOT max; max is `+0x72`. Catalog rows "p1_maximum_energy 0x1C0622 = +0x72" agree.)
- Frame loop `0xF9A`: even frames (`$1C0083` bit0 clear) run `0xFB0..0x1050` incl. clock `jsr $262D2`
  (`0x1018`, L1141); odd frames `0x1054..` run meters `bsr $7548` (`0x10B0`, L1169). YOU WON `0x8CF8` at
  `0x10C0` (odd list) — both lists call it (doc match-rules-18c4 §1).

---

## 1. Energy gauge / name plate / portrait

### 1.1 Tables

| ROM | bytes | meaning |
|---|---|---|
| `0x76A2` | `0104 0128 0154 0178` | HUD base byte-offset per human slot 0..3 → cell(1,1), cell(1,10), cell(1,21), cell(1,30) |
| `0x77D0` | `0B0F 0B18 0B21 0B2A` | "nP" label tile base, indexed by `(+0x8A input ptr) & 0xF` = port 0/2/4/6 → 3 tiles each (pal 1) |
| `0x7814` | `21D0 31D9 41EB 51E2 D1F4 71FD 8206 920F A218 B221 C22A D233` | portrait word per wrestler id 0..11: `tile = w & 0xFFF` (3×3, +1 per tile), `pal = w>>12` (written raw: +1=lo, +3=hi byte, so pal nibble = high nibble) |
| gauge tiles | consts in `0x76AA` | empty cell top `0xB33`, bottom `0xB3A` (top+7); slots 0/1 fill base `0xAD9` (bottom = top+0xB); slots 2/3 fill base `0xAF2` (bottom = top+0xE); all pal **1** |

### 1.2 `0x7720` slot→HUD index
Scans `$1C05B0 + i*0x10C` for `A0`; returns D0 = i (0..3) with C=1 if found, C=0 (D0=4) if A0 is not a human-row object. CPU objects therefore never get a gauge (stock).

### 1.3 `0x7680` base pointer
`A1 = 0xC0000 + tbl_76A2[slot]`.

### 1.4 `0x76E8` cell write (A1, D4=tile): `(A1+1)=D4&0xFF; (A1+3)=(D4>>8)|0x10` → pal 1 always.

### 1.5 `0x76AA` one gauge cell (A1=cell, D0=slot, D1=fill 0..8)
```
D2=7, D3=0xB33                         ; empty
if D1!=0: D2=0xB, D3=0xAD9             ; slot 0/1 colour
          if (D0 & 2): D2=0xE, D3=0xAF2 ; slot 2/3 colour
D3 += D1
write A1       tile D3        (top row)
write A1+0x100 tile D3+D2     (bottom row)
```
So fill 1..7 = partial, 8 = full, 0 = empty. Cell is 2 tiles tall.

### 1.6 `0x75EE` draw gauge (A0=object)
PC-ordered writes (slot s, base b=row1,col c0 from `0x76A2`):
1. `0x75F6` A1 = b + 0x20C → cell(3, c0+3). Loop `0x7600` ×6 (`D2=5`): `76AA(D1=0)` → 6 empty cells at cols c0+3..c0+8, rows 3 and 4.
2. `0x7610` D1 = byte `+0x6F` (= low byte of displayed `+0x6E`). If 0 → done (empty gauge).
3. `0x761E` `divu #16`: q = D1 (full cells), r = remainder.
4. `0x7624` A1 = b+0x20C again. D3=1. If q==0 → step 6. If q>=6 → q=6, D3=0 (no partial).
   Loop `0x7648` q times: `76AA(D1=8)` full cell, A1+=4.
5. `0x7660` if D3==0 (capped) → done.
6. `0x7664` r &= 0xFF; if r==0 → done; if r odd r--; r>>=1 → `76AA(D1=r/2)` partial at the next cell (r/2 ∈ 1..7).

Gauge width = 6 cells × 16 hp = **96 hp displayed max**; hp 0x60+ is full. (Hulk max 110 shows full until < 96.)
No colour band by hp, no `+0x70` use, **no low-energy flash** in `0x7506/0x7548/0x75EE` (the only in-match
"hurry" cue is YM `0x0B` from `0x9052` when energy < 0x18 — sound-commands.md). TODO EXACT: palette `$180700`
animation elsewhere, not found.

### 1.7 `0x776C` "nP" labels (all live human slots)
For each live human slot: `D1 = (+0x8A).w & 0xF` (port byte offset 0/2/4/6) → `D2 = tbl_77D0[D1/2]`;
A1 = base; 3 cells `cell(1,c0..c0+2) = D2, D2+1, D2+2 : pal 1`. (`btst #0,$1C0161` at `0x7788` result unused — nop'd.)

### 1.8 `0x77D8` portrait 3×3 (A0 = cell ptr, D0 = wrestler id)
`w = tbl_7814[id]`; rows 2..4 (A0 starts at base+0x100), cols c0..c0+2: `cell = w, w+1, w+2` per row,
next row `w+3..`, raw `+1 = w&0xFF, +3 = w>>8` (tile low 12 bits, pal = top nibble: 2,3,4,5,D,7,8,9,A,B,C,D).

### 1.9 `0x782C` portrait invoke (A0=object)
Match live, object live, not CPU, `0x7720` found, `id = (+0x56)&0xF`; **id 5 skipped** (`0x7862` — Animal, used by the LOD override); A0 = base+0x100 → `0x77D8`.

### 1.10 `0x7744` clear HUD block (A0=object)
If `0x7720` found: from base, 4 rows × 9 longs `clr.l` (cols c0..c0+8, rows 1..4; `adda #0xDC` = 0x100-0x24).

### 1.11 `0x7506` HUD init (called from match setup `0x8B16`, `0x9480`, `0xA788`)
For slot 0..3 live & human: `+0x6E = +0x66`; `0x75EE` gauge; `0x782C` portrait; `0x776C` labels.

### 1.12 `0x7548` HUD runtime (odd frames)
```
if $1C16C5 != 0 (time-up lock) or $1C007C == 0: rts
0x776C labels (every odd frame)
for slot 0..3 live & human:
  +0x6C += +0x6A; +0x6A = 0                    ; take pending delta
  if +0x6C == 0: next
  if +0x6C < 0:  +0x6E -= (6C == -1 ? 1 : 2); +0x6C += same   ; drain 2/odd-frame
  else:          +0x6E += (6C ==  1 ? 1 : 2); +0x6C -= same
  0x76F8: clamp +0x6E to [0, +0x72]; on clamp +0x6C = 0
  0x75EE redraw gauge
```
Gauge moves 2 hp per odd frame (1 hp/frame average). Regain (`0x90D6`) adds to both `+0x66` and `+0x6A`
so the bar animates up.

### 1.13 Layout summary (per human slot s, c0 = 1/10/21/30)
| rows | cols | content |
|---|---|---|
| 1 | c0..c0+2 | "1P/2P/3P/4P" (tiles 0xB0F/0xB18/0xB21/0xB2A +0..2, pal 1) |
| 2..4 | c0..c0+2 | portrait 3×3 |
| 3..4 | c0+3..c0+8 | gauge 6 cells (top/bottom tile pair) |
Singles (1P vs CPU): only slot 0 drawn. 1P tag: slots 0,1. 2P tag: 0..3. CPU never (stock).

---

## 2. Match clock

Globals: `$1C16A0` minutes (BCD word, low byte `$1C16A1`), `$1C16A2` seconds (BCD, low byte `$1C16A3`),
`$1C16A4` frame divider (low byte `$1C16A5`), `$1C169E` match-signal bits, `$1C16C5` lock byte.

### 2.1 `0x262D2` wrapper (even frames only, L1141) — disassembly misaligned at 0x262D0; real insns:
```
0262D2 tst.w $1C007C      beq rts        ; match live
0262DC btst #4,$1C169E    bne rts        ; already time-up
0262E8 btst #7,$1C169E    beq rts        ; clock enabled (bit7 set by match start; TODO EXACT writer PC)
0262F4 bset #6,$1C169E    bne 26370      ; first call → init
```
Init (`0x26300`): 6 cells at `$C1D04` = cell(29,1..6): top `0x49C+i : pal E`, bottom row 30 `0x4A2+i : pal E`
("TIME" plate, 2 rows); colon `$C1D24`/`$C1E24` = cell(29,9) & cell(30,9) = `0x4B2 : pal E`.
`0x26348`: `$1C16A4=1; $1C16A2=0x0001; $1C16A0=0x0030` (**30:01**); palette `$18071C=0x0FF, $18071E=0x00F`.

Tick (`0x26370`): `--$1C16A4; if !=0 rts; $1C16A4=4;` → one tick per 4 calls = **every 8 frames**
(even-only caller) = 7.18 ticks/s at 57.44 Hz. 30:00 BCD = 1800 ticks ≈ 250.7 s real.
```
sec = sbcd(sec,1); if no borrow: store, goto draw
sec = 0x59; min = sbcd(min,1); if borrow → TIME UP (0x263D8)
store min; if min == 0x05: bset #5,$1C169E; YM 0x0B via 0x2052 (hurry music)
```
Draw (`0x2641E`) via `0x2647C(D0=digit, D1=column)`: A0 = `$C1D1C + D1*4` → cell(29, 7+D1);
top `0x4A8+d : pal E`, bottom cell(30,7+D1) `0x4B3+d : pal E`. Columns: min tens D1=0, min ones 1, (colon 2),
sec tens 3, sec ones 4 → cols 7,8,(9),10,11.
Flash (`0x264AA`, vblank side): if `$1C169E` bit5 (≤5:00): `$1C16A5` bit0 ? palette (0x00F,0x003) : (0x0FF,0x00F) — clock digits flash on the divider parity.
AI reads `$1C16A0 < 0x10` (L30019/30684/31211) to change behaviour under 10 minutes.

### 2.2 TIME UP `0x263D8`
```
bset #4,$1C169E; $1C16A0 = $1C16A2 = 0
0x2503C(D0=0x13)                      ; big FG0 blit "TIME UP"
$1C16C5 = 3                           ; lock: kills 0x18C4 seating and 0x7548 HUD updates
0x21E6(0x40)                          ; wait 64 vblanks
for every live object (0x250E walk): +0xA0 = 0x80 (byte), +0xFE = 0x8005
```
Then frame loop sees `$1C16C5==3` at `0x1166` → `0x1AFC`: `$1C15FC=1; 0x2A06; 0x2503C(0x50); wait 0x60;
$1C0092[] = live human ptrs; jsr 0x65C0; jmp 0x6FC`. **Time-up = draw → no stage advance; game over path.**
DIPs do not set the time (fixed 0x30). Difficulty dip `$1C0066 & 0x300` only affects CPU energy (§4).

---

## 3. Match end / result

### 3.1 `+0xFE` result word (per object; bit7 of low byte = final)
| value | writer | meaning |
|---|---|---|
| `0x4000/0x4001` | referee `0x200FC..0x2011A` at half-count 6 | pin winner pair / loser pair; `→0xC000/0xC001` after win pose |
| `0x8000/0x8001` | `0x111C8` | instant KO winner / loser (hp 0) |
| `0x8002/0x8003` | `0x1FEB2..0x1FF1C` | count-out pairs. Loser-handler `0x1AD7C` treats `+0xFF==3` as **lost** and `0x90D6` skips bit0 set, so 8003 = counted-out side (pins-referee.md labels are swapped — TODO EXACT which A2/A3 is in-ring at `0x1FEB2`) |
| `0x8005` | `0x1FE76..0x1FE90`, `0x26416` | double KO / time-up (draw) |
| bit0 set | — | loser (no regain in `0x90D6`) |

### 3.2 Instant KO `0x111C8` (A0=attacker; callers: move enders `0x136A8 0x157D8 0x15C54 0x161E4 0x179C6 0x17ED4`)
```
if !$1C007C: C=0
A2 = +0x26 (victim). if (+0x33 bit2) or !(+0x33 bit0) or !(A2+0x33 bit0) or A2+0x66 != 0: C=0
bset #3,+0x60 (latch; already set → C=0)
if !rumble ($1C0161 bit0 clear):
   $1C1214 = 0x8009         ; referee +0x20 → win-pose state, no count
   clr $1C169E; YM 0x32 twice
   self & partner(+0x86) +0xFE = 0x8000; victim & its partner +0xFE = 0x8001
   jsr 0x90D6 (regain); clr $1C169E; +0x4A = 4
   if attacker human: $1C15D2 = 0x0F2A (announcer latch); 0x2503C(0x26) (blit)
   else (CPU): +0xC4++ ; jsr 0x21358
else rumble: +0xC4++; jsr 0x21358
C=1
```
Hp reaching 0 on its own does **not** end the match; only this KO check (on the finishing move) or a pin does.

### 3.3 Post-fall regain `0x90D6` (Clear Stage Power Up dip)
For human slots 0..3 with `+0xFF` bit0 clear: `add = tbl_912A[($1C0066 & 0x6000)>>12]`,
`tbl_912A = 0018 0020 000C 0000` (24/32/12/0 hp); `+0x66 += add; +0x6A += add; +0x66 = min(+0x66, +0x72)`.

### 3.4 Winner/loser flags `+0xA0` (byte: 0x40 = won, 0x80 = lost)
- Win-pose anim handler `0x1AD24` (table entry `0x1AD10`): at `+0x25 == 0xFE` (anim end): self+partner `+0xA0=0x40`, opponent(`+0x7A`)+its partner `0x80`. Also YM `0x30` at pose start.
- Loser handler `0x1AD7C` (entry `0x1AD76`): pose `0x1D3|facing`, `+0x22=0x40` countdown; at 0: `+0xFF==3` → self pair 0x80, other pair 0x40; `+0xFF==5` → tag: all four 0x80; rumble: every CPU object 0x80.
- Time-up: all live 0x80 (`0x26410`).

### 3.5 Frame-loop end detection (`0x10F8..0x1194`, after vblank spin)
```
if $1C007C == 0: attract timeout tbl_119A[$1C0166] vs $1C0080 … (not match)
rumble: jsr 0x2017A; $1C16C4 → 0x1B4E ceremony; $1C16C5==3 → 0x1AFC; else loop 0xF9A
tag:    if ($1C0650 | $1C0868) & 0xC000  → 0x11B6      ; slot0/slot2 +0xA0 (team leaders) got 0x40/0x80
        else loop 0xF9A
```
`tbl_119A` (14 words, mode index `$1C0166`): `0350 02E0 0410 03A0 0290 03A0 04B0 03B0 0650 0480 0320 0330 0340 0360`.

### 3.6 Match-over sequence `0x11B6` → `0x1396` → `0x19BA`
```
0x11B6 clr $1C0092..$1C009E; wait 0x20; 0x2503C(0x8050); wait 0x20
       D0=0x51; scan human slots for +0xA0 bit6 (a winner): found → stage==9 ? skip : 0x2503C(0x51)
       none → 0x2503C(0x50)                         ; result banner blits (YOU WON / YOU LOST)
       wait 0x40; clr $1C016C,$1C008C; wait 0x40
       if slot0 or slot2 +0xA0 bit7 (a human lost): 0x1256 continue-screen chrome
            (0x206FE wipe, YM 0x3107, scene 3, 0x26E66, $1C15FC=2, tag-flag $1C0160 bit6 rebuild,
             per-pair loser code → $1C0168/$1C016A, 0x15F4/0x139C continue countdown with 0x2503C 0x8018/801D/801E…)
       0x1396 jmp 0x19BA
0x19BA jsr 0x8EB6 (YM 3100/3120 stop)
       if no human slot has +0xA0 bit6 or +0x104 != 0: jsr 0x65C0; jmp 0x6FC   ; game over → attract
       tag checks ($1C0160 bit6, $1C06B4/$1C08CC pair words, $1C0650/$1C0868 bit6) → 0x1AF0 jmp 0xAC0 (rematch same stage, no bump)
       if $1C0163 == 9: → 0x1B4E championship ceremony (YM 0x11, scene 4, 0x26E66, then 0x6FC)
       jsr 0xB608 (stage-4 title screen, only when $1C0162==4); jsr 0xBDA6 (title-belt scene)
       $1C0163++ ; $1C0165++
       if $1C0066 bit7 (Championship Game dip = "4th"): stage 3 or 8 → $1C0163++ again (skips a defence)
       if $1C0163 == 5: $1C0598[0..9] = 0xFFFF; then for live humans: $1C0598 += {0x0000, id}  ; reset faced-list
       jmp 0xAC0                                                           ; next match setup
```
YOU WON chrome `0x8CF8` (when `$1C007C==0`): `$1C0167==0` → announcer `0x2503C(0x29)` + banner `0x9ACA(0x15)` at `$C131C`;
`==1` → `0x2A`/`0x16` at `$C1604`; ≥2 → `0x2503C(6)`, `(mode-2)+7`, `(mode-2)+0x40`, then sprite object `$1C14CE`
(pose table `0x8E9A`) and `0x8006/0x8007+mode/0x8040+mode` after `$1C0082 >= 0x200`. TODO EXACT: `$1C0167` writer (no store found by grep; likely a byte alias).
`0x6FC`: `clr $1C007C,$1C00B2,$1C0076; jsr 0x1F52` (object wipe) → attract rebuild `0x790`/`0x52BE`.
Match start sets `$1C007C` bit7 at `0x9B2`/`0xAA4`.

### 3.7 Count-out, pins — already in docs/engine-specs/pins-referee.md (count 20 at 0x50 frames/step, half-count 6 stamps `+0xFE`). Not re-transcribed.

---

## 4. Single-player ladder

- Stage byte **`$1C0163`** (word reads of `$1C0162` see it as the low byte; `0xE0E` zeroes the long at game start). `$1C0165` = matches played (mirror, never reset on skip).
- Scene per stage `0xD12` (10 bytes → `$1C007F`): `00 05 01 05 00 05 00 01 05 00` (1 = cage, at stages 2 and 7).
- Stage BGM `0xDEE` (10 words): `3104 310A 310C 310F 3106 3104 310A 310C 310F 3106`.
- CPU energy by stage `0x10848` (10 words): `0073 0091 0096 009B 00B9 00A0 00A5 00A5 00AA 00C8`
  (last CPU slot `$1C09E0` gets full value; other CPUs −15; `0x107EC`). Rumble CPUs fixed `0x87`.
- Handicap by difficulty dip `0x1085C` (4 s16, index `($1C0066&0x300)>>8`): `0000 FFF1 000F 001E` (+0/−15/+15/+30).
- Human start/max hp by id `0x10830` (12 words): `006E 006A 0062 0066 01F4 01F4 006A 0068 0064 0060 0068 0060`
  (ids 4/5 = LOD 500 hp). `0x10782`: on a non-first stage with `+0x104` bit7 clear the human **keeps** hp (skips re-init → `0x1082E`).
- **Opponent pick `0x1034A`** (1P tag only; 2P-vs-2P skips at `0x1035C`):
  1. `$1C0598` faced-list (10 words; `0x7C` sentinel → reset). Candidates = ids 0..11 not in the list and not 4/5 → `$1C0380[]`.
  2. Weight table ptr `0x106E0[stage % 5]` = `0x106F4, 0x106FC, 0x10702, 0x10706, 0x106F4`:
     `0D0D0D0D 0C0C0C0C` (8), `1111 1111 1010` (6), `19191919` (4), `3232` (2), (stage 4 unused). Two draws via `0x24CC`; equal → neighbour.
  3. Either pick is 10/11 → pair forced `(10,11)` Demolition. **Stage 4 or 9 → `(4,5)` Legion of Doom.**
  4. Stored `$1C0A36/$1C0B42` (+0x56 of CPU slots 0/1) and `$1C09E2/$1C0AEE` (+0x02); slots marked live; pair appended to `$1C0598`.
- Title path: stages 0–3 challenges, 4 = title vs LOD (`0xB608` title card at `$1C0162==4`), 5–8 defences (faced-list reset at 5), 9 = LOD again → ceremony `0x1B4E`. Dip "Championship Game 4th" skips one defence at 3 and 8.
- Rematch: loss → continue screen; on continue `0x19BA` sees `+0xA0` bit6 / `+0x104` and `jmp 0xAC0` **without** bumping `$1C0163` → same stage, same opponents (faced-list already holds them; the pick reruns only if `$1C008C==0`, `0x10360`). TODO EXACT: `+0x104` writer (continue credit).

---

## 5. C sketch

```c
#define FG0_CELL(r,c) (0xC0000 + (r)*0x100 + (c)*4)
static void fg0_put(uint32_t a, uint16_t tile, uint8_t pal){ w8(a+1, tile); w8(a+3, (pal<<4)|(tile>>8)); }

static const uint16_t hud_base[4] = {0x104,0x128,0x154,0x178};
static const uint16_t label_tile[4] = {0xB0F,0xB18,0xB21,0xB2A};
static const uint16_t portrait_w[12] = {0x21D0,0x31D9,0x41EB,0x51E2,0xD1F4,0x71FD,0x8206,0x920F,0xA218,0xB221,0xC22A,0xD233};

static void gauge_cell(uint32_t a, int slot, int fill){      /* 0x76AA */
    int step=7; uint16_t t=0xB33;
    if(fill){ step=0xB; t=0xAD9; if(slot&2){ step=0xE; t=0xAF2; } }
    t+=fill; fg0_put(a,t,1); fg0_put(a+0x100,t+step,1);
}
static void gauge_draw(obj *o, int slot){                    /* 0x75EE */
    uint32_t a = 0xC0000 + hud_base[slot] + 0x20C;
    for(int i=0;i<6;i++) gauge_cell(a+i*4,slot,0);
    unsigned e = o->hp_disp & 0xFF; if(!e) return;
    unsigned q=e/16, r=e%16; int partial=1;
    if(q>=6){ q=6; partial=0; }
    for(unsigned i=0;i<q;i++) gauge_cell(a+i*4,slot,8);
    if(!partial || !r) return;
    gauge_cell(a+q*4,slot,r/2);                               /* r odd → (r-1)/2 */
}
static void hud_init(void){                                   /* 0x7506 */
    for(int s=0;s<4;s++){ obj*o=&human[s]; if(!o->live||o->cpu) continue;
        o->hp_disp=o->hp; gauge_draw(o,s); portrait(o,s); }
    labels();
}
static void hud_tick_odd(void){                               /* 0x7548 */
    if(g_lock_16c5 || !g_match_live) return;
    labels();
    for(int s=0;s<4;s++){ obj*o=&human[s]; if(!o->live||o->cpu) continue;
        o->hp_acc += o->hp_delta; o->hp_delta=0; if(!o->hp_acc) continue;
        int st = (o->hp_acc==1||o->hp_acc==-1)?1:2;
        if(o->hp_acc<0){ o->hp_disp-=st; o->hp_acc+=st; } else { o->hp_disp+=st; o->hp_acc-=st; }
        if(o->hp_disp<0){ o->hp_disp=0; o->hp_acc=0; } else if(o->hp_disp>=o->hp_max){ o->hp_disp=o->hp_max; o->hp_acc=0; }
        gauge_draw(o,s); }
}
/* clock: even frames */
static void clock_tick_even(void){                            /* 0x262D2 */
    if(!g_match_live || (g_169e&0x10) || !(g_169e&0x80)) return;
    if(!(g_169e&0x40)){ g_169e|=0x40; draw_time_plate(); div=1; sec=0x01; min=0x30; pal[0x1C/2]=0x0FF; pal[0x1E/2]=0x00F; }
    if(--div) return; div=4;                                   /* 8 frames per second-tick */
    if(!bcd_dec(&sec)){ sec=0x59; if(!bcd_dec(&min)){ time_up(); return; } if(min==0x05){ g_169e|=0x20; ym(0x0B);} }
    digit(0,min>>4); digit(1,min&15); digit(3,sec>>4); digit(4,sec&15);   /* cell(29,7+col), tiles 0x4A8+d / 0x4B3+d pal E */
}
static void time_up(void){ g_169e|=0x10; min=sec=0; blit_2503c(0x13); g_lock_16c5=3; wait_vbl(0x40);
    for_each_live(o){ o->a0=0x80; o->result=0x8005; } }
/* KO on finisher: see §3.2; result flags → +0xA0 → 0x11B6 → 0x19BA → stage++ / 0xAC0 / 0x6FC */
```

## 6. Open items (TODO EXACT)
- `$1C169E` bit7 (clock enable) writer PC; `$1C0167` writer; `+0x104` (continue) writer.
- Orientation of 8002/8003 at `0x1FEB2` (which pair is in-ring).
- `0x2503C` blit ids 0x13/0x26/0x50/0x51/0x52/0x8050 → FG0 contents (not transcribed; tilemap-maint/pins-referee style big-blit).
