# Match end: result -> poses -> banner -> continue/next match (68k transcription)

Source: reference/maincpu.asm. Read-only. Field names: state +0x20, move_id +0x60, react_id +0x64,
a0flags +0xA0 (byte), result +0xFE (+0xFF = low byte), count +0x22, frame +0x24/+0x25, spr +0x04,
facing +0x2E, walk_sub +0xAE, tag_partner +0x86, opponent +0x7A, target_x/y +0xBE/+0xC0, x/y +0x06/+0x0A.

## 0. Key correction: the win/lose poses are MOVES (state 5), not state-4 reactions

`0x12840` and `0x12844` sit inside the MOVE cell table `0x12614` (anim.md §1b: `+0x1D==5` ->
`A1 = *(0x12614 + (+0x61)*4)`). `(0x12840-0x12614)/4 = 0x8B`, `(0x12844-0x12614)/4 = 0x8C`.

| move_id | entry | handler | cell record |
|---|---|---|---|
| 0x8B win pose | 0x1AD10 | 0x1AD24 | `dc.l 1AD24; mode 0001 (hold-last); n 0003; dur 0008 0008 0008; spr 0216 0217 0216` |
| 0x8C lose pose | 0x1AD76 | 0x1AD7C | `dc.l 1AD7C; mode 0000` (handler-driven, no cells) |
| (0x8D) | 0x12848 -> 0x17F9A | | unrelated neighbour |
| (0x8E) | 0x1284C -> 0x1AE28 | | sub-dispatch on +0x44 (special sequence) |

## 1. Who writes the result, who enters the poses, and when

### 1a. Result word writers (+0xFE), PC-ordered
| PC | writer | writes |
|---|---|---|
| 0x11236..0x11250 | KO `0x111C8` (A0 attacker, A2 = +0x26 victim) | self & +0x86 `result=0x8000`; victim & its +0x86 `result=0x8001` (bit7 already set = final) |
| 0x11214 | same | `$1C1214 = 0x8009` (referee state -> SM9 win pose, no count) |
| 0x1121C, 0x1125C | same | `clr $1C169E` (twice) |
| 0x11222, 0x1122C | same | YM cmd 0x32, twice (`0x2052`) |
| 0x11256 | same | `jsr 0x90D6` regain |
| 0x11262 | same | attacker `+0x4A = 4` (NO reader found in maincpu.asm — `grep "4a,A"` shows writers only; treat as dead) |
| 0x11268..0x1127C | same | attacker +0x56 bit7 (human) -> `$1C15D2 = 0x0F2A` (announcer slot 0x0F / phrase 0x2A) and `0x2503C(0x26)` "GIVE UP" banner. CPU winner -> `+0xC4++; jsr 0x21358` |
| 0x200FC..0x2011A | referee SM6 at half-count 6 (A1 = +0x56 pinner) | pinner & +0x86 `0x4000`; pinner->+0x26 & its +0x86 `0x4001` (bit7 CLEAR — not final yet); `clr $1C169E`; `0x90D6` |
| 0x20138..0x20142 | referee SM6 at half-count 7 (tag) | referee `state=0x8009`; digit wipe `0x206FE`; pinner `+0x4A=4`. Rumble: wipe, `0x8005` |
| 0x20472..0x20490 | referee SM9 cell 0x11 | `bset #7` on +0xFE of winner, winner +0x86, winner->+0x26, that one's +0x86 -> `0xC000/0xC001` = FINAL |
| 0x1FE76..0x1FF1C | count-out | `0x8002/0x8003` pairs, `0x8005` both-out (see pins-referee.md, not re-read here) |
| 0x26410 | time-up | all live `0x8005`, a0flags=0x80 |

### 1b. Pose entry — the wrestlers decide on their OWN handlers, keyed on +0xFE bit7
Nothing writes the pose directly at the result stamp. Each wrestler notices `+0xFE bit7` (final) in the
STAND handler (block `0x11598`, inside `0x114B2`'s stand cell; reached every frame while standing):

```
0x11598  btst #7,(+0xFE)      ; result final?  no -> rts
0x115A0  bclr #4,(+0x34)
0x115A6  btst #0,(+0xFF)      ; bit0 = loser (8001/4001->C001/8003)
0x115AC  bne 0x115C4:  state=5, move_id=0x8C            ; LOSER POSE (0x115C4/0x115CA)
0x115AE  btst #1,(+0xFF)      ; bit1 set (8002 = the other count-out side, 8005 draw?) -> nothing, keep standing
0x115B6  else (winner: 8000 / C000): state=1, walk_sub(+0xAE)=0x0C   ; WINNER: walk to ring centre first
```
So timing:
- KO (`0x111C8`): bit7 is set immediately -> next frame a standing loser poses, a standing winner starts
  walking to centre. A downed loser first finishes lying/get-up (lying handler `0x1B39E` skips AI when
  bit7 set; `0x11E7A` variant: `+0xFE != 0` -> get up). TODO EXACT PC of the lying->stand shortcut.
- Pin: `0x4000/0x4001` (bit7 clear) at count "3" (half-count 6) -> nobody poses yet; hits are already
  blocked (`0x24126` tests `+0xFE != 0`). Referee goes SM9 at half-count 7, and only at his cell 0x11
  (8 + 0x12 + 0x12 ticks later, see §3) does `bset #7` make the wrestlers react. So for a pin the poses
  start ~44 frames after the "3" is up, after the referee's arm raise.
- Count-out: `0x8002/0x8003` are written with bit7 -> immediate.

### 1c. Winner walk-to-centre: walk sub-state 0x0C, handler `0x11B6C`
```
first frame (+0x1C bit15 clear):                             ; 0x11B6C-0x11BB4
  +0x34 = 0; bset #2,(+0x34); +0x01 = 1 (polar mover); bsr 0x1174C (walk cell select)
  target_y(+0xC0) = 0x160
  not rumble: +0x33 bit0 (legal man) ? target_x(+0xBE) = 0x254 : 0x29C ; rumble: 0x2C0
every frame while +0xAE bit7 clear:                          ; 0x11BB6-0x11BDC
  facing(+0x2E) = (angle byte +0x2D & 0x80) ^ 0x80 ; bsr 0x11710 (walk anim cell)
  jsr 0x1F15C  -> Z when |x-target_x| < 8 && |y-target_y| < 8 (else it steers via 0x20C8 and returns NZ)
  arrived: bset #7,(+0xAE)  (+0xAE becomes 0x800C)
arrival frame and after (0x11BE2):
  +0x01 = 0; count = 0xFFFF; x = target_x; y = target_y (snap)
  not rumble: facing bit7 = (partner(+0x86).x >= own x) ; spr = facing (stand frame 0)
     if partner +0x21 == 1 && partner +0xAE == 0x800C:     ; both arrived (singles: +0x86 TODO EXACT = self?)
         self  state=5, move_id=0x8B ; partner state=5, move_id=0x8B     ; 0x11C32-0x11C44  WIN POSE (both)
  rumble (0x11C4C): state=0, spr=0, facing=0, bclr #1,(+0x33)
```

## 2. Pose handlers

### 2a. Win pose `0x1AD24` (move 0x8B), cell hold-last n=3, dur 8/8/8, spr 0x216,0x217,0x216 (xor facing)
```
0x1AD24  btst #7,(+0x1C); beq -> clr.b +0x01 (mover off); rts     ; very first frame only (tick inits after)
0x1AD32  if +0x25 == 0 && count == 0: YM 0x30 (0x2052)             ; once, on the 9th tick of frame 0
0x1AD48  if +0x25 == 0xFE (after 3*(8+1)=27 ticks, then every frame):
            self a0=0x40; +0x86->a0=0x40; +0x7A->a0=0x80; +0x7A->+0x86->a0=0x80
```
Sequence: frame0 (0x216) 9 ticks, frame1 (0x217) 9, frame2 (0x216) 9, hold 0x216 forever.

### 2b. Lose pose `0x1AD7C` (move 0x8C), mode-0 cell
```
0x1AD7C  bset #7,(+0x1C); bne skip-init
         init: spr = 0x1D3 | facing ; count = 0x40
0x1AD96  count -= 1; if no borrow rts          ; fires once, 65 ticks after init (0x40 -> -1)
0x1AD9E  +0xFF == 5 (draw/8005): tag: self,+0x86,+0x7A,+0x7A->+0x86 all a0=0x80
                                 rumble (0x1AE04): every live obj (0x250E walk) with +0x56 bit7 clear (CPU) a0=0x80
0x1ADA6  +0xFF == 3 (8003 count-out loser): self & +0x86 a0=0x80; +0x7A & its +0x86 a0=0x40
         +0xFF == 1 (pin/KO loser): nothing — the winner's handler sets both sides.
```

## 3. Referee SM9 `0x203EE` (state 0x8009)
```
first call: bset #7,(+0x1C) was clear -> +0x23 = 8, cell(+0x05) = 0x0F
each call: --+0x23; nonzero -> rts
  cell 0x0F expired: -> cell 0x10, +0x23 = 0x12                         ; 0x204A6
  cell 0x10 expired: (0x2040A) not rumble & +0x56(pinner)->+0x56 bit7 clear?? see note:
       btst #7,(A1=+0x56 pinner, +0x56) clear => HUMAN? (pins-referee: "human winner")
         jsr 0x20156 (YM 0x3109 on stage 4/9 else 0x3105); $1C15D2 = 0x0F2A
       else (CPU winner / rumble): 0x20156; clr $1C15D4; $1C15D2 = 0x0F2C
       then cell 0x11, +0x23 = 0x12
  cell 0x11 expired: not rumble: if winner +0x56 bit7 set: YM 0x3108
       bset #7 +0xFE on winner, winner->+0x86, winner->+0x26, its +0x86 ; state = 0x8005
       rumble: state = 0x800A
```
Cells: 0x0F (8 ticks) -> 0x10 (18) -> 0x11 (18) -> idle. spr = cell | facing (0x2052E path, referee.c:172);
no position write — he raises the arm where he stands (beside the cover). Total 44 ticks, then `0x8005` idle
(idle hunts no target because targets' +0xFE != 0 -> stands). Matches engine/referee.c:224 thresholds 80/40
only if win_t counts 44 -> TODO EXACT: referee.c uses 80/40 splits; ROM is 8/18/18.

## 4. Big-blit `0x2503C` (D0 = id | 0x8000 erase)
```
0x25040 A0 = *(0x25204 + (id&0x7FFF)*4)                 ; record ptr (table 0x25204, 4 bytes/id)
rec+0 mode (0..3) ; rec+1 pal (-> D1 = pal<<12) ; rec+2 word: FG0 byte offset from $C0000 ; rec+4.. script
row stride 0x100 bytes = 64 cells of 4 bytes; cell(col,row) at $C0000 + row*0x100 + col*4
tile word layout per cell: byte+1 = tile bits 0-7 ; byte+3 = (pal<<4) | tile bits 8-11 (bytes 0/2 untouched; erase: clr.l)
script bytes: 00 end ; 01 newline (A1 += 0x100 mode0/3, 0x200 mode1/2) ; 02 xx: D4 = xx<<8 (mode3 attr) ;
              03 hh ll: A1 = $C0000 + hhll ; c >= 8: glyph
mode0 (8x8 font): tile = c-0x10 ; 1 cell, col+1
mode1 (8x16):     t = ((c-0x10)<<1)+0x30 ; (col,row)=t, (col,row+1)=t+1 ; col+1
mode2 (16x16):    c==0x20 -> 4 cells cleared; else t=(c-0x10)*4+0x94: (col,row)=t (col,row+1)=t+1 (col+1,row)=t+2 (col+1,row+1)=t+3 ; col+2
erase (bit15):    same cells, written 0
```
Records used here (hex dumped from ROM):
| id | addr | record | text |
|---|---|---|---|
| 0x13 | 0x25874 | `02 00 0E30` "TIME OVER" 00 | row 14-15, cols 12..29 |
| 0x26 | 0x259A0 | `02 00 0E30` "GIVE UP" 00 | KO banner |
| 0x50 | 0x25E6A | `02 00 0E30` "GAME OVER" 00 | NOT "you lose" |
| 0x51 | 0x25E78 | `00 00 1630` "CONGRATULATIONS!" `03 18 1C` "NOW GO FOR THE NEXT MATCH!" 00 | 8x8 font rows 22/24 |
| 0x52 | 0x25EAA | `02 00 0E30` "RING OUT" 00 FF | double count-out |
| 0x17 | 0x258D2 | `02 00 0234` "1 PLAYER" | continue chrome |
| 0x18 | 0x258EE | `02 00 020C` "1 PLAYER  2 PLAYER" | |
| 0x1D | 0x2593A | `02 00 1628` "INSERT COIN" | |
| 0x1E | 0x2595E | `02 00 1B18` "PUSH 2P BUTTON" | |
| 0x14/0x16 | 0x25882/0x258AA | `00 00 040C` "PUSH SELECT BUTTON TO REGAIN POWER!" / "   INSERT COIN TO REGAIN POWER!    " | |
All pal = 0 (attr high nibble 0). Since TIME OVER / GIVE UP / RING OUT / GAME OVER share 0x0E30, `0x8050`
(erase "GAME OVER", 9 glyphs = cols 12..29) wipes whichever banner is there.

Exact mode-2 tiles (TL/BL|TR/BR per glyph, rows 14/15):
- TIME OVER: c12 1A4/1A5|1A6/1A7 c14 178/179|17A/17B c16 188/189|18A/18B c18 168/169|16A/16B c20 blank c22 190/191|192/193 c24 1AC/1AD|1AE/1AF c26 168/169|16A/16B c28 19C/19D|19E/19F
- GAME OVER: c12 170/171|172/173 c14 158/159|15A/15B c16 188..18B c18 168..16B c20 blank c22 190..193 c24 1AC..1AF c26 168..16B c28 19C..19F
- GIVE UP:   c12 170..173 c14 178..17B c16 1AC..1AF c18 168..16B c20 blank c22 1A8/1A9|1AA/1AB c24 194/195|196/197
- RING OUT:  c12 19C..19F c14 178..17B c16 18C/18D|18E/18F c18 170..173 c20 blank c22 190..193 c24 1A8..1AB c26 1A4..1A7
Mode-0 tiles: CONGRATULATIONS! row 22 col 12: 33 3F 3E 37 42 31 44 45 3C 31 44 39 3F 3E 43 11 ;
NOW GO FOR THE NEXT MATCH! row 24 col 7: 3E 3F 47 10 37 3F 10 36 3F 42 10 44 38 35 10 3E 35 48 44 10 3D 31 44 33 38 11 (0x10 = space tile).

Sounds per step: KO YM 0x32 x2 (0x11222); win pose YM 0x30 (0x1AD3E); referee cell 0x10: YM 0x3109 (stage 4/9)
else 0x3105 (0x20156), announcer `$1C15D2=0x0F2A` human / `0x0F2C` CPU; referee cell 0x11 CPU winner: YM 0x3108;
continue screen YM 0x3107 (0x11264); `0x19BA` -> `0x8EB6` (YM 3100/3120 music stop).

## 5. Frame-loop end detection and `0x11B6` timeline
`0x1178` (tag, each frame after vblank): `($1C0650 | $1C0868) & 0xC000` — slot0 / slot2 a0flags word
(+0xA0 of `$1C05B0` and `$1C07C8`, i.e. 0x40/0x80 in the high byte) -> `0x11B6`. Rumble: `$1C16C5==3`
(time-up) -> `0x1AFC`. Until then the whole object loop (poses, referee) keeps running.
```
0x11B6  clr.l $1C0092,$1C0096,$1C009A,$1C009E     ; live-human ptr list
0x11CE  wait 0x20 (0x21E6 = D0+1 vblank edges of $140026 bit2)
0x11D8  0x2503C(0x8050)                            ; erase the 0x0E30 banner (GIVE UP / TIME OVER / RING OUT)
0x11E2  wait 0x20
0x11EC  D0=0x51; for 4 human slots ($1C05B0 + i*0x10C): a0flags bit6 (won) ->
            $1C0163(stage)==9 ? skip blit : blit 0x51 CONGRATULATIONS ; none won -> blit 0x50 GAME OVER
0x1222  wait 0x40
0x122C  clr $1C016C, $1C008C ; wait 0x40
0x1240  if slot0 or slot2 a0flags bit7 (a human lost) -> 0x1256 continue screen, else -> 0x1396
0x1256  0x206FE digit wipe; 0x1F6C (clear BG0/BG1 VRAM $80000/$82000 + strip state); 0x1FC0 (fill FG0 with
        0xFFC5FF04 = blank tile); YM 0x3107; $1C1804=0x140,$1C1806=0x600 (scroll); $1C007E=3 (scene 3);
        jsr 0x26E66 (scene build); $1C15F4=$1C15F8=3; $1C15FC=2; 0x2A06 (palette load); 0x1FDE; 0x1F9E (clear FG0)
0x12B4  tag-flag rebuild $1C0160 bit6 if both slot0&slot2 a0 bit7
0x12E0  loser code per pair -> $1C0168 (pair A) / $1C016A (pair B): bit0 = slot a0&1 ?? (reads word +0xA0 &1 — low
        byte +0xA1, TODO EXACT meaning), bit1 = other slot
0x1332  pair A lost & $1C0168 != 0: 0x15F4 (clear $1C14CE/$1C14F8 x0x2A), 0x139C(A1=$1C05B0): blits 0x8018 (erase),
        0x801D, 0x801E, then 0x16/0x17(+0x18 if tag) or (D7&0xF)-1+0x16 (+3 for pair B) -> continue countdown
        chrome 0x1404.. ($1C0080 = 9 ticks of 0x40 frames, 0x9ACA digit, 0x2067C)
0x135C  same for pair B ($1C016A, $1C07C8)
0x1382  0x1FC0; $1C15FC=1; 0x2A06
0x1396  jmp 0x19BA
0x19BA  jsr 0x8EB6 (music stop)
0x19C0  any human slot with a0 bit6 or +0x104 != 0 (continued)? none -> jsr 0x65C0; jmp 0x6FC (game over -> attract:
        clr $1C007C/$1C00B2/$1C0076, object wipe 0x1F52)
0x19EE  tag: $1C0160 bit6 / pair words $1C06B4,$1C08CC / a0 bit6 of slot0,slot2 -> 0x1AF0 jmp 0xAC0 (rematch, no
        stage bump; clr $1C008C when both pairs intact)
0x1A5E  $1C0163 == 9 -> 0x1B4E ceremony
0x1A6A  jsr 0xB608, 0xBDA6; $1C0163++, $1C0165++ ; dip $1C0066 bit7: stage 3/8 -> extra ++ ; stage 5: faced-list
        $1C0598[10] = 0xFFFF then {0,+0x57} per live slot ; jmp 0xAC0 (next match setup; objects re-spawned there)
```
Nothing in 0x11B6..0x1396 touches object positions/states; the poses just keep running (frozen screen) under the
banner until `0xAC0` (or `0x6FC` object wipe) rebuilds everything. HUD: `0x7548` updates are gated by `$1C16C5`
(time-up) only; during 0x11B6 the object loop is no longer called (straight-line waits), so clock/HUD simply stop.

## 6. C sketch
```c
/* result stamp (KO 0x111C8 / ref SM6 0x200FC / count-out) — already elsewhere */
/* stand handler tail, 0x11598 */
if (o->result & 0x80) {
    o->f34 &= ~0x10;
    if (o->result & 1)            { o->state = 5; o->move_id = 0x8C; }      /* loser pose */
    else if (!(o->result & 2))    { o->state = 1; o->walk_sub = 0x0C; }     /* winner: walk to centre */
}
/* walk sub 0x0C, 0x11B6C */
if (first) { o->f34 = 4; o->mover = 1; o->ty = 0x160; o->tx = rumble ? 0x2C0 : (o->f33 & 1) ? 0x254 : 0x29C; }
if (!(o->walk_sub & 0x80)) { o->facing = (o->angle & 0x80) ^ 0x80; if (near8(o)) o->walk_sub |= 0x80; else { steer_20C8(o); return; } }
o->mover = 0; o->count = 0xFFFF; o->x = o->tx; o->y = o->ty;
o->facing = (partner->x >= o->x) ? 0x80 : 0; o->spr = o->facing;
if (partner->state_lo == 1 && partner->walk_sub == 0x800C) { o->state = partner->state = 5; o->move_id = partner->move_id = 0x8B; }
/* win pose 0x1AD24: cells {0x216,0x217,0x216} dur 8 each, hold-last */
if (first) o->mover = 0;
if (o->frame == 0 && o->count == 0) ym(0x30);
if (o->frame == 0xFE) { o->a0 = partner->a0 = 0x40; opp->a0 = opp_partner->a0 = 0x80; }
/* lose pose 0x1AD7C */
if (first) { o->spr = 0x1D3 | o->facing; o->count = 0x40; }
if (o->count-- == 0) { if ((o->result & 0xFF) == 5) all_four(0x80) /* rumble: all CPU */; else if ((o->result & 0xFF) == 3) { self_pair(0x80); opp_pair(0x40); } }
/* frame loop: if ((slot0->a0 | slot2->a0) & 0xC0) match_over_11B6(); */
```

## TODO EXACT
- 0x1F15C arrival threshold (<8 on both axes) confirmed; steering body `0x20C8` not read.
- `+0x86` in singles (self vs. NULL) — decides whether the winner poses on arrival alone (0x11C22 test).
- 0x1AD24 first instruction disassembled mid-word: bytes `08 28 00 07 00 1C` = btst (not bset) — verified from hex.
- `+0x4A = 4` reader: none in maincpu.asm (`grep "4a,A"`), safe to omit.
- Mode-3 blit (`0x25196`) writes D1 not D2 (`movep.w D1,(1,A2)`) — looks like a ROM bug; unused by these ids.
- Lying loser's get-up path to reach 0x11598 (0x11E7A / 0x1B39E) not traced to the exact state write.

---

## 7. Engine implementation (`src/campaign.c`, 2026-08-23)

| ROM | engine |
|---|---|
| `+0xA0` byte | `eng_obj.a0flags` (engine.h) |
| win pose `0x1AD48..0x1AD6E` | `anim.c handler_winpose` at `+0x25 == 0xFE`: self/+0x86 `0x40`, +0x7A/its +0x86 `0x80` |
| lose pose `0x1AD96..0x1ADFC` | `anim.c handler_losepose`: `+0x22` 0x40 countdown, then `+0xFF == 5` all four `0x80`, `== 3` own pair `0x80` / other `0x40`, `== 1` nothing |
| KO `0x11236..0x1127C` | `anim.c ko_check`: partners stamped too, human winner blits `0x26` GIVE UP |
| time-up `0x26410` | `hud.c time_up`: `result = 0x8005`, `a0flags = 0x80` |
| frame loop `0x1178` | `core.c` tail -> `eng_camp_end_test()` (slots 0 and 2, `& 0xC0`) |
| `0x11B6..0x1396` | `eng_camp_tick()`: waits `0x21/0x21/0x41/0x41`, blits `0x8050`, `0x51`/`0x50`, branch |
| `0x1256` + `0x1404` | `ENG_SCENE_CONTINUE` (`cont_begin/cont_update/cont_draw`): scene word 3, scroll `(0x140,0x600)`, text set 2, plate `0x139C`, `0x9ACA` run 0x14 at `$C1118`, digit `0x2067C bit15` -> `$C1188`, `0x1C`/`0x1D`/`0x1E` gated on `0xC404`, START + `0x55A` = continue |
| `0x19BA..0x1AF0` | `ladder_step()`: music stop, game over -> attract, continued -> rematch (`$1C008C` keeps the pair), stage 9 -> ceremony, else `$1C0163++`/`$1C0165++`, faced-list reset at 5, opponent pick, aisle |
| `0x1034A/0x10390..0x104A4` | `eng_camp_pick_cpu()` (also called by `charselect.c finish()` for stage 0) |
| `0x10782..0x107F4` | `eng_camp_hp()`: CPU energy `0x10848[stage]` (first CPU slot full, the other −15), human carries his energy unless he continued |
| `0x1B4E..0x1DCA` | `ENG_SCENE_CEREMONY`: 9 name cards (blit `0x58 + i`, portrait row `0x4D` at `0x1DE8[i]`, `0x81` frames each), the 6-man walk-off (row `0x50`, `0x1DD0[i]`, speed 0xC angle 0, `0x501` frames), any button aborts, YM `0x1F` -> attract |

New tables: `cpu_stage_energy` (0x10848), `stage_handicap_dip` (0x1085C),
`continue_port_map` (0x1884), `ending_cards` (0x1DE8), `ending_walkoff_pos` (0x1DD0).

## 8. Between-match interludes (`src/interlude.c`, 2026-08-23)

`ENG_SCENE_INTERLUDE`, armed by `ladder_step()` (queue in ROM order), all on
scene word 4, priority $140010 = 0x7B (0xAE64/0xB654), skipped in the rumble
($1C0161 bit0) and on a continue rematch ($1C008C, 0x1A4A/0xAE26):

| ROM | engine |
|---|---|
| `0xB608` title-win card (only when the stage word == 4, i.e. the first LOD title match was just won; scroll 0x140/0x300) | portraits row `0x4D` cell = wrestler id (0xB -> 0xA), x from `interlude_title_cards` (0xB814 {x0, x1, pal 0x40+id}), y 0x7E, shown at centre step 1 (priority -> 0x78, 0xB8E8); centre object row `0x4E` at (0xF0, 0x6E) pal 0x37, cells `interlude_title_bigcells` (0xB936, 8 ticks, 0xFF wraps to step 3, first step after 0x40); 17-letter marquee row `0x4E` pal 0x3C from x 0x1BF, y 0xC (names 0xF, cells `interlude_name_cells` 0xBA24), -2 px/frame, next released when x + `interlude_letter_widths[cell-0xE]` < 0x140 (0xBA48), dies < 0; ends when letter 0x10 is off (0xB7FA); music 0x3100+0x3107 |
| `0xBDA6` belt scene (stage word 0..2 -> count 3/2/1, 5..7 -> -5, 3/4/>=8 skip; dip bit7 +1 not modelled) | LOD panels row `0x2C` cells 0x16/3 at x 0x60/0xE0 (0xAE20 mode 3, centre hidden); count art row `0x4F` cell `idx*7+4` at (0x60,0x36) pal 0x39, 7 cells x 8 ticks, ends at step 0x10; herald row `0x4F` at (0x49,0x58) pal 0x38 cells `interlude_belt_smallcells` {3,1,2,1,0} x 10 ticks after 0x40, cell 2 -> voice 0x312A + count art starts; text run 0x10 at $C1540 nibble 0x10 ("n MORE VICTORIES TO GET TO TITLE MATCH"), run 0x17 at $C1578 patches the singular when cell base == 0x12; FG0 line 0x188280 ramp k<<8 (0xBE92) |
| `0xAE20` mode 0 LOD talk screen (0xBD2: before the aisle when the NEW stage is 4 or 9; scroll 0x280/0x200) | three row-`0x2C` panels x 0x48/0x98/0xF8 y 0x77 cells 0x16/0x11/3 pals 0x31-0x33 (`interlude_panel_setup` 0xB0C2); bottom bar 0xB29E (aisle.c) + blits 0x27 "WRESTLEFEST TAG TEAM CHAMPION" / 0x28 "THE LEGION OF DOOM"; per-panel {cell, dur} scripts `interlude_talk_scripts` (0xB19C/0xB1A8, cell bit7 = next voice from `interlude_voice_cmds` 0xB14A), centre first, right at centre step 0x1B (+ text wipe 0xB2E4), left at 0x2E, over at left step 0x1C; music 0x3100+0x3110 |

Sprite palettes: the interlude rows bake absolute 0x2AEA ids (0x31-0x33,
0x37-0x39, 0x3C, 0x40+id); `sprite.c eng_sprite_scene_pals_*` deals them
banks on demand (the ROM's $1C1600/$1C1610 allocator), dropped again by the
match body-palette install.

### TODO EXACT (this pass)
- The interludes cut in and out: the palette fades `0x26772`/`0x26844`/
  `0x264E2` around each screen are not run (attract.c owns the fade code).
- `0xB608` runs its portrait loop over all four live slots; only the first
  two ever show (0xB8F0), so the engine builds just those.
- The DIP block does not exist in the engine: the "Championship Game 4th"
  skip (`0x1A86`, stage 3/8 gets an extra bump), the difficulty handicap
  row (`0x10806`, row 0 = +0 is used) and the 2-player-cabinet plate branch
  (`0x13C4`) are all stubbed at the default.
- The ROM picks the opponents in `0xC98` (after the aisle walk); the engine
  picks them at `0x19BA` time.  The aisle only ever walks human teams, so
  nothing on screen differs.
- `0x1BFA` runs `0x2AEA` with `cell + 0x40` for each ending card's palette
  bank; sprite.c colours a row from the stream's own bank byte, so the
  portrait object on the name cards may come out unlit.
- Engine guard, not ROM: `eng_camp_end_test()` synthesises `+0xA0` from
  `+0xFE` after `A0_FALLBACK` (0x200) frames of a standing final result, so
  a winner who cannot reach his pose (unrouted handler, harness poke)
  cannot hang the machine.
- `0x156C` wipes the timed-out pair's object (0x10C bytes, `+0x56` kept);
  the engine just drops it from the continue list.
- With `--no-front` (headless harness) the ladder still advances and picks
  new opponents, but the aisle / continue / ceremony screens are skipped
  and the next match starts immediately — the old `core.c` stand-in
  behaviour, so existing repros keep working.

### Repro
```
# the whole ladder: stage 0 -> 9 -> ceremony -> attract
WF_ROMDIR=... WF_DBGSEL=1 WF_FRONT=1 WF_CREDITS=9 \
  ./wfengine --headless --frames 20000 --drive ladder
# a lost match: continue screen -> START + credit -> same stage
WF_ROMDIR=... WF_DBGSEL=1 WF_FRONT=1 WF_CREDITS=9 WF_LADDER=lose \
  ./wfengine --headless --frames 9000 --drive ladder
# a lost match with no credit: countdown -> GAME OVER -> attract
WF_ROMDIR=... WF_DBGSEL=1 WF_FRONT=1 WF_CREDITS=1 WF_LADDER=lose \
  ./wfengine --headless --frames 4000 --drive ladder
```
`--drive ladder` (main.c) stamps the finishing KO of `0x111C8` by hand
`WF_LADDER_AT` (default 90) frames into every match, so the real poses,
banners and ladder run; `WF_LADDER=lose` stamps the other way round.
