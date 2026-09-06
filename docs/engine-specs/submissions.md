# Submission holds — holder / victim / escape / CPU / referee (read-only transcription)

Source: reference/maincpu.asm. Field names: state +0x20 (+0x21 lo), count +0x22, frame +0x24 (+0x25 lo, 0xFE = finished),
partner +0x26 (bit31 = poison/lazy divorce), facing +0x2E, f32/f33/f35, grap44 +0x44, move_id +0x60 (+0x61 lo; bit15 = once
latch), react_id +0x64, hp +0x66, dmg +0x68, band +0x70 (+0x71 lo), opp +0x7A, helper +0x86, mash_aa +0xAA, spr +0x04,
result +0xFE, anim_sel +0x1C (bit15 = entered). A0 = self, A2 = partner unless stated. `once` = `bset #7,+0x60; bne`.

Helpers: 0x11412 = link test (carry SET if partner->partner != self). 0x10B9A(D0,D1,D2) = place A2 at A0 + facing-mirrored
(dx,dy,dz). 0x10BD0 = same for self. 0x2052(D0) = sound. 0x215B6 / 0x212A0 / 0x21732 / 0x21282 = score/pin bookkeeping
($1C007C-gated). 0x111C8 = KO check (below). $1C15D2/3 = announcer (wrestler id byte +0x03, code).

## 0. The machine in one paragraph

A submission is two scripted state-5 moves. The holder's move hides the victim (`state=0x00FF`) at the end of frame 0,
loops its last two frames forever, and on the FIRST loop writes the victim into a *victim move* 0x5D..0x64 (fixed per
attacker move, not per wrestler). The victim move's record is code-only (0x19530/0x194CA): it draws nothing
(`spr=0xFFFF`), seeds `mash_aa` (0x10C60 with a per-move D0) and calls the 0x10D04 press-decrement each frame. Nothing in
the victim handler ever transitions: when `mash_aa` hits 0x4000 the NEXT B1/B2 press goes through 0xEBC4 (input prefix)
which substitutes the victim move for an *escape move* (0x5D->0x67 etc.). The escape move's init writes the HOLDER to
state 0xFF (he goes hidden, his loop stops running), then at a fixed frame throws him (state 4 + react + dmg), and ends
with the victim in state 7/0 with `partner` poisoned. The holder never "detects" anything — he is overwritten. He only
ends the hold himself on KO (0x111C8 carry) or on `result` bit7 (match event).

## 1. Holder side

### 1a. Move 0x22 mounted punches — 0x15B8C. Rec 0x15B6C {mode 1, n 6, delays 10,10,0A,0A,08,20, spr 18B,CE,CF,D0,D1,D2}

| PC | cond | write |
|---|---|---|
| 0x15B8C | init (+0x1C b7 clear) | `+0x01=0`; `x=v.x; y=v.y` (+0x06/+0x0A); `facing=v.facing` (byte) |
| 0x15BAE-0x15BB8 | +0x25==0 && count==0 (end of frame 0) | |
| 0x15BBA-0x15BD0 | `v.state!=4 \|\| v.react!=8 \|\| link broken (0x11412 carry)` | → 0x15C04 ABORT: `state=0`, `spr=facing`, `partner\|=bit31`, `count=1` |
| 0x15BD2-0x15BE4 | else | `f33\|=0x40` both; **`own f35\|=0x02` (holder role); `v.f35\|=0x04` (held role)** |
| 0x15BEA | | **`v.state=0x00FF`** (hidden) |
| 0x15BF0 | | announcer (own id, 0x0A) |
| 0x15C20-0x15C2C | every committed frame | `+0x4C=0`; if move_id b7 (looping): `+0x4C=0x8014` (hittable, record 0x14) |
| 0x15C32 | +0x25!=0xFE | rts |
| 0x15C3C | `result` b7 set | → 0x15C8E release |
| 0x15C44-0x15C4E | | `frame=3`, `count=0` (LOOP frames 4-5 = 8+0x20 = 40 ticks), **`v.dmg=3`** |
| 0x15C54 | `jsr 0x111C8` carry clear (no KO) | → 0x15CBA |
| 0x15C5C-0x15C88 | KO; if $1C0161 b0 (rumble) | announcer (v id, 0x29); `v.f32\|=0x10` (forced rise); `partner\|=bit31`; `spr=0x68\|facing` |
| 0x15C8E-0x15CB2 | RELEASE | `state=7`; `v.state=4`; `v.react&=0xFF`; `f35&=~0x02`; `v.f35&=~0x04`; `f33&=~0x40` both |
| 0x15CBA | `once` (first loop only) | **`v.state=5; v.move_id=0x5F`**; 0x215B6; snd 0x32 |

Note the victim leaves 0xFF *by this write* on the first loop (32-40 ticks after the hide). Before that he is 0xFF: no
input, no AI, no mash. No per-wrestler branch anywhere: 0x22 → 0x5F always.

### 1b. Move 0x09 ground hold — 0x135EC. Rec 0x135C8 {mode 1, n 7, delays 10,10,0C,16,0A,10,10, spr 18B,D8..DD}

Byte-for-byte the same shape. Differences only:
| PC | write |
|---|---|
| 0x1364A | `v.+0xC7 += 1` (times-held counter) — before the hide |
| 0x13650 | announcer (own id, **0x09**) |
| 0x13660 | `v.state=0x00FF` |
| 0x13698 | loop: `frame=4`, `count=0` (LOOP frames 5-6 = 0x20 ticks); `v.dmg=3` (0x136A2) |
| 0x13716 | `once`: **`v.state=5; v.move_id=0x5D`**; 0x215B6; snd 0x32 |
| 0x136E2 | release identical (state 7, v.state 4, react&=0xFF, f33/f35 clears; order f33 then f35) |
No `+0x4C` hittable-record writes in 0x09 (holder untargetable for the whole hold).

### 1c. 0x111C8 KO check (carry = KO fired)
```
0x111CC  $1C007C==0 → fail          0x111DA own f33 b2 set → fail      0x111E4/0x111EE both f33 b0 (legal) else fail
0x111F8  v.hp != 0 → fail            (exact ==0 test on +0x66 — NOT "<=3"; dmg is applied later by the hit pipeline)
0x11200  bset #3,+0x60 already set → fail (once)
0x1120A  !rumble: $1C1214=$8009; $1C169E=0; snd 0x32 ×2; result=0x8000 (+helper +0x86); v.result=0x8001 (+its helper);
         jsr 0x90D6; $1C169E=0; +0x4A=4; if !(+0x56 b7): $1C15D2=0x0F2A, 0x2503C(0x26)
0x11284  rumble: +0xC4++ ; jsr 0x21358          → carry set (0x1128E)
```

### 1d. Other attacker moves that feed a victim move (writer PC → owning handler)

| attacker move | handler | victim move @PC | per-loop dmg | hold-role bits | release / KO shape |
|---|---|---|---|---|---|
| 0x09 | 0x135EC | 0x5D @0x1371C | 3 | f35 b1/b2 at 0x1363E | §1b |
| 0x22 | 0x15B8C | 0x5F @0x15CC8 | 3 | f35 b1/b2 at 0x15BDE | §1a |
| 0x25 | 0x1612E | 0x60 @0x1620C | 5 (`v.dmg=5` 0x161DE) | f35 b1/b2 set at **0x16212 with the 0x60 write** (not at hide) | hide at init 0x16142 (announcer 0x1F); loop `frame=1,count=0`; KO → own state5 move **0x8A** + +0x18 b7; result b7 → 0x1618E: +0x18 b7, own state 0, spr=facing, v.state 4 react 2, v.facing = own^0x80, 0x10B9A(0x30,-1,0x30), poison. Extra victim writes: `v.react\|=0x20`, `v.f32\|=0x08` (0x1621E/0x16224) |
| 0x36 | 0x1792A | 0x63 @0x17A02 | 5 | f35 b1/b2 at init 0x1796A/0x17970 | init needs link OK and (v.move==0x7B or v.+0x1D==0xFF) else poison; hide 0x17964, announcer 6; loop frame=1; KO → **own 0x6F, victim 0x6E, v.facing flipped**, 0x212A0; result → own 0x6F, v.state 0. `+0x4C=0x8015` while looping |
| 0x37 | 0x17A58 | 0x61 @0x17A6A (AT INIT) | none (hold only) | none (no referee) | standing hold: x/y/z/facing = victim's, f33 b6 both, helper +0x86 → state5 move 0x4E unless its f32 b1; at frame0/count0 `bset #7,v.move_id` (latch → victim hides, countdown starts, §2); handler re-inits itself (0x17ACC) and ends when victim's 0x61 expires |
| 0x3B | 0x17D12 (3 phases by +0x45) | 0x62 @0x17F66 | 5 | f35 b1/b2 at **0x17F54/0x17F5A with the write** | loop frame=2; KO → own 0x6F / v 0x6E (0x17F04), f33/f35 cleared; on latch also 0x10BD0(0x40,0,0), `v.x=own x`, `+0x19=0xD0`; plus `v.react\|=0x20`, `v.f32\|=0x08` |
| 0x1A | 0x15228 | 0x5E @0x152E8 | 0x16 once | **f35 b0 (PIN role)**, `v.react\|=0x20`, `v.f32\|=0x08` | slam-into-cover: hide at init; at 0x25==FE: v.dmg=0x16; if own f33 b2 clear & both legal → 0x5E; else 0x15330: f35 b0 clr, state 7, v.state 4 react 6, own facing flip, 0x10B9A(-0x60,1,0), poison, spr 0x68. TODO EXACT move name |
| 0x23 | 0x15D08 (sub by +0x44&0xF) | 0x64 @0x15F3A | 0x14 | f35 b0 (pin) | cover: `+0x4C=0x801D`, `v.mash_aa=0` 0x15EFE, 0x10B9A on victim (0x30,-1,0). Pin, not submission — listed because 0x64 is in the 0xEBC4 ladder |
| ? → 0x52 / 0x53 | TODO EXACT (no `move.w #$52/#$53,($60,A2)` in ROM — written by another form) | | | | escapes 0x58 / 0x57 below |

## 2. Victim moves

Table 0x12614: 0x5D,0x5E,0x5F,0x60,0x62,0x63 → rec **0x19530** {code 0x19536, mode 0 = code-only}; 0x61 → rec 0x194CA
{code 0x194D0, code-only}; 0x64 → rec 0x19608 {code 0x19618, mode 1, n 2, delays 8, **0xFF00** (hold), spr 0x16, 0x13}.
Code-only records have no frames: +0x25 never reaches 0xFE, the move lasts until overwritten.

### 2a. 0x19536 (victim of 0x09/0x22/0x25/0x36/0x3B/0x1A)
| PC | cond | write |
|---|---|---|
| 0x19540-0x1954E | init | `+0x52=0`; `+0x01=0`; **`spr=0xFFFF`** (composite art is the holder's cell); `count=0xFF00` |
| 0x19554 | init | jsr 0x21732 (victim-side score; for 0x5E sets +0xE6 from table 0x217F8[band], pin links) |
| 0x19562 | move 0x5D | `mash_aa=0`; 0x10C60(**D0=4**) → table row 4 by band: 0x0A/0x0D/0x10, minus 1 |
| 0x1957A | 0x5E | `mash_aa=0`; 0x10C60(**D0=0**) → hp==0 ? 0x100 : max(1, 0x2A-hp) |
| 0x19592 | 0x5F | `mash_aa=0`; 0x10C60(**D0=4**) |
| 0x195AA | 0x60 | `mash_aa=0`; 0x10C60(**D0=6**) → 0x04/0x0C/0x18 −1 |
| 0x195C2 | 0x62 | `mash_aa=0`; 0x10C60(**D0=7**) → 0x04/0x0C/0x18 −1 |
| 0x195DA | 0x63 | `mash_aa=0`; 0x10C60(**D0=7**) |
| (0x61, 0x64 never reach here; any other id: no seed) | | |
| 0x195EC | per frame | jsr 0x11432 (if !rumble && !(+0x56 b7): clear +0x34 b6/b7 on helper +0x86 and on helper->opp — TODO EXACT) |
| 0x195F2-0x195FE | `move==0x5E && hp==0` | skip tick (pinned at zero hp: no mash) |
| 0x19600 | else | **jsr 0x10D04** (the only press decrement; +0xA3 edge; 0x4000 latch) |

escape-machine.md §3a says "0x10C60(0)" for all seven sites — wrong: only 0x5E uses D0=0; 0x5D/0x5F use 4, 0x60 uses 6,
0x62/0x63 use 7 (tables at 0x10CEA). Since `mash_aa` is cleared first, the "already seeded" early-out never applies.

### 2b. 0x194D0 (victim of 0x37, standing) — self-drawn, timed
| PC | cond | write |
|---|---|---|
| 0x194D8-0x194F0 | init, only if move_id b7 clear | 0x10C60(**D0=5**) (NB no clr first; 0x18/0x1C/0x20 −1); 0x211EC; **`grap44=0xAA`** (170-tick timer) |
| 0x194FA | per frame | jsr 0x10D04 |
| 0x19508 | move_id b7 clear | `spr=0x12\|facing` (visible until the holder latches at 0x17AE6) |
| 0x19516-0x19528 | b7 set | `spr=0xFFFF`; `--grap44`; at 0 → **`state=5; move_id=0x59`** (auto-escape = same move the mash gives) |

### 2c. 0x19618 (victim of 0x23 cover) — pinned
init: 0x258E(0x17) (fx); `mash_aa=0`; 0x10C60(**D0=0**). Per frame: if f37 b4 (landed) → `move_id\|=0x8000`, `+0x01=0`,
0x21732; if latched && hp!=0 → 0x10D04. Draws 0x16 for 8 ticks then 0x13 forever.

### 2d. The escape trigger
0x10D04 latches `mash_aa=0x4000`; on the **next** new B1/B2 press 0xDE86→0xEBC4 (escape-machine §2) does
`state=0x0005, move_id=N, mash_aa=0` for the presser only: 0x5D→0x67, 0x5E→0x56, 0x5F→0x5C, 0x60→0x5B, 0x61→0x59,
0x62→0x5A, 0x63→0x55, 0x52→0x58, 0x53→0x57, 0x64→0x4B. The victim handler itself never transitions (except 0x61's timer).

## 3. Escape moves (A0 = escaping victim, A2 = holder). All records mode 1.

| move | rec/code | cells (delay:spr) | init (+0x1C b7 clear) | mid-move | end (+0x25==0xFE) |
|---|---|---|---|---|---|
| **0x67** (ex 0x5D) | 0x19776/0x1978A | 10:1D9 10:1DA 08:1DB | `h.state=0x00FF`; `f35&=~0x04`; `h.f35&=~0x02` | frame2/count0 (0x197B4): **`h.state=4; h.react=6; h.facing^=0x80; h.dmg=2`**; snd 0x32; 0x10B9A(0x38,1,0); 0x212A0; `h.y-=1`; f33 b6 clr both | `state=7`; poison; `spr=0x68\|facing` |
| 0x5C (ex 0x5F) | 0x19436/0x1944A | 10:178 08:179 08:17A | `h.state=0xFF`; f35 clears | frame1/count0 (0x19474): `h.state=4; h.react=0x27; h.dmg=3`; 0x10B9A(0x10,-1,0); f33 clr | `state=7`; 0x212A0; poison; spr 0x68 |
| 0x5B (ex 0x60) | 0x1939A/0x193AA | 10:176 10:177 | `h.state=0xFF`; `+0x1B=0x20`; f35 clears; `facing=h.facing` | frame1: snd 0x29 | `+0x18 b7` both; **`state=0`**; 0x212A0; `h.state=4; h.react=2; h.dmg=3`; poison; `spr=facing`; f33 clr |
| 0x59 (ex 0x61) | 0x19264/0x19274 | 0C:18E 0C:18F | `h.state=0xFF`; f35 clears | frame0/count0: `h.state=4; h.react=2; h.dmg=3; h.facing^=0x80`; poison; f33 clr; 0x213A6; snd 0x29 | `state=7`; spr 0x68 |
| 0x5A (ex 0x62) | 0x192F2/0x19302 | 10:1A8 0C:1A9 | `h.state=0xFF`; f35 clears; `+0x19=0xD0` | frame0: snd 0x2B | `+0x18 b7` both; `state=7`; 0x212A0; `h.state=4; h.react=2; h.dmg=3`; spr 0x68; f33 clr; 0x10B9A(0x20,0,0); poison |
| 0x55 (ex 0x63) | 0x18FEE/0x19002 | 14:17E 0C:17F 0C:180 | `h.state=0xFF`; f35 clears; 0x212A0 | frame1: snd 0x2A | `h.state=4; h.react=2; h.dmg=3`; **`state=0`**; `spr=facing`; f33 clr; poison |
| 0x58 (ex 0x52) | 0x191B4/0x191C8 | 18:181 0C:182 0C:183 | **`h.state=5; h.move_id=0x6D`** (holder scripted); f35 clears; 0x212A0 | frame1: snd 0x2A, `h.dmg=3`; fx 0x10D3A(6)/(7) at frames 0/1 | `state=0`; f33 clr; poison |
| 0x57 (ex 0x53) | 0x1913E/0x1914E | 10:186 10:187 | `y=h.y-1`; **`h.state=5; h.move_id=0x76`**; 0x212A0 | frame1: snd 0x2A, `h.dmg=3` | `state=0`; f33 clr; poison |
| 0x56 (ex 0x5E) | 0x19076/0x19086 | 08:174 08:175 | `h.state=0xFF`; `h.f35&=~0x01` (pin role); `+0x19=0x28; +0x1B=0x60`; `facing=h.facing` | frame0: snd 0x29 | `+0x18 b7` both; `state=7`; `h.state=4; h.react=5; h.dmg=3`; `spr=0x22\|facing`; `h.spr=0x16\|h.facing`; 0x21282; 0x10BD0(0x50,0,0); poison; spr 0x68; f33 clr |
| 0x4B (ex 0x64/0x4A/0x51 kick-out) | 0x181EC/0x1820C | 0C:6E 0C:6F | `h.f35&=~0x01`; `h.state=4; h.react=0x0F`; `h.spr=0xF0\|h.facing`; `h.facing=facing`; f33 clr; `h.+0x9A=0x50`; 0x10B9A(0x50,-1,0x10); 0x21282; second pinner via +0x9C if state5 move 0x49: same + `+0x18 b7`, facing flipped | cell 0x181EC if react==8 else 0x181FC | `state=7; react=0`; poison; `+0x9C=0` |

"f35 clears" = `bclr #2,+0x35` self, `bclr #1,(+0x35,A2)` holder. Holder at 0xFF runs hidden record 0x125CC (stays hidden
while `partner->partner==self`, i.e. for the whole escape anim); the thrown-off writes above replace that state directly.
`h.dmg` is a request consumed by the hit pipeline (hp −= dmg there), same as the holder's `v.dmg=3`.

## 4. CPU victim

**State 4 row 0x1DA7E** (lying; react 8 → 0x1DAEA, react 9 → 0x1DC48). Pre-emptive roll while the holder is still in
frame 0 (victim still state 4): 0x1DB28: partner A1 in state 5; move 0x08 → 0x1DB46 (roll → 0x38); 0x1DB9A: partner move
∈ {0x10,0x46,0x0B} → table 0x23F78, ∈ {0x09,0x22} → table 0x23FC4 (rumble: 0x1DE48), `[difficulty $1C0162][band]`;
once per hold (`bset #1,+0xB7`): `jsr 0x24CC` (RNG) → D0==0 = success: **`+0xBC = 2` for 0x09/0x22 (0x10 otherwise)**;
`move_id = 0x38` (react 8) or `0x78` (react 9); `+0xB5 |= 0x10`. Then 0x1DE08 every frame: `--+0xBC`; at 0 →
`state = 0x0005` (spring-up before the hide lands → the hold's frame-0 check fails → holder aborts to state 0). Covers
(0x23/0x35/0x0E) instead wait for partner `+0x60` b4. TODO EXACT: whether +0xBC=2 beats the hide depends on slot order.

**State 5 row**: table 0x1E018[move_id] (long per id, default 0x1E254 rts). Held victim entries, each `bset #6,+0x60` once:
`+0xBA = 0x1F576[D0][+0x71]` (timer), `+0xBC = escape move`:
0x5D→0x1E39C D0=2 esc 0x67; 0x5F→0x1E334 D0=2 esc 0x5C; 0x60→0x1E3B2 D0=3 esc 0x5B; 0x61→0x1E3C8 D0=5 esc 0x59;
0x62→0x1E3DE D0=4 esc 0x5A; 0x63→0x1E382 D0=4 esc 0x55; 0x52→0x1E34E D0=0 esc 0x58; 0x53→0x1E368 D0=1 esc 0x57.
Timer rows (words, band 0..2): D0=0 {4D,4D,88} 1 {C3,130,1AE} 2 {4D,88,88} 3 {88,88,C3} 4 {88,88,C3} 5 {C3,FE,130}.
0x1E3F2: `--+0xBA`; at 0: for 0x52/0x53/0x61 if `grap44!=0` → `+0xBA=1` (wait); else **`clr.b +0x20` (re-entry),
`move_id=+0xBC`** → CPU escapes by timer, never by mash (0x10D04 returns at once for +0x56 b7/b6; CPU never writes +0xA3).
Pins 0x4A/0x5E/0x64 → 0x1E452: once `+0xBD` = hp class (0x1E48C: hp==0→8; ≥0x80→0; ≥0x65→1; ≥0x50→2; ≥0x40→3; ≥0x25→4;
else 5); each frame if `+0x109 == +0xBD` → re-entry, move 0x4B (0x4A/0x64) or 0x56 (0x5E). +0x109 = TODO EXACT (count).

## 5. Referee during submissions

Referee object $1C11F4; SM = +0x21, table 0x1F952: SM4 and SM8 share **0x1FA40**; visual = +0x1D (copied from +0x20 on
bit7 entry, 0x1FAF0), table 0x1FB1A: visual 4 = 0x1FC22 (approach), visual 8 = **0x20354** (watch), 5 = 0x1FF52 idle.

- **Arm** (hunt 0x20556, from SM5 0x1F9C2 when no target or target result==0, and from SM2 0x1FA02): scan 9 slots
  $1C05B0 stride 0x10C, active (+0x00 b7): `f35 b0` → `+0x20=$8001` (pin); else **`f35 b1 && f33 b0` (holder role +
  legal) → `+0x20=$8004`**, `+0x56=slot`. Setters of f35 b1 (holder) / b2 (victim): 0x1363E/0x13644 (0x09),
  0x15BDE/0x15BE4 (0x22), 0x16212/0x16218 (0x25), 0x1796A/0x17970 (0x36), 0x17F54/0x17F5A (0x3B). 0x37 sets none
  (standing hold, no referee walk). No +0x35 b4 writes exist in these handlers. The referee reads only b1.
- **SM4/SM8 0x1FA40** (every frame): A1=+0x56; `A1.result!=0 || !(A1.f35 b1)` → `$8005` (idle) — i.e. he leaves the
  moment the holder's b1 is cleared, which happens at: holder release 0x15CA0/0x13700/0x17F34 (KO or result), or the escape
  move's init `bclr #1,(+0x35,A2)` (0x1979E, 0x19458/5E, 0x193C4, 0x19288, 0x19316, 0x19016, 0x191E2). Else scan: any slot
  `f35 b0` → `+0x56=it`, `$8001` (a pin elsewhere outranks the watch). Otherwise stay.
- **Visual 4 approach 0x1FC22**: first entry `+0x2E=0, +0x23=1, +0x25=4, +0x05=0, +0x2B=0x1C`; dest `+0x60=A1.x ∓ D1`
  (toward x=0x280), `+0x62=A1.y-0x10`, D1=0x30; holder move 0x22 → `+0x62-=0x10` more; move 0x09 → D1=0x50. Walk
  poses 0..3 @8; on arrive `y=A1.y+1`, **`+0x20=$8008`**.
- **Visual 8 watch 0x20354**: first entry (`bset #7,+0x1C`): `+0x23=0x0A`; pose `+0x05=0x0D` (or **7** if target
  `f32 b0`); facing: `+0x2E=0`, if `A1.x >= x` → `+0x2E=0x80`; `+0x04=+0x2E`. Per frame `--+0x23`; at 0: f32 b0 → toggle
  pose 7 (8 ticks) ↔ 8 (0x0A ticks); else toggle **0xD (0x0A ticks) ↔ 0xE (8 ticks)**. No crouch, no count, no timer, no
  writes to the wrestlers. (Pose ids per referee-walk.md §3: 0xD/0xE "ring-out" pair, 7/8 "usher" pair; f32 b0 = TODO
  EXACT, same bit tested by 0x215D0.)
- After `$8005` the idle visual 0x1FF52/0x1FB46 walks him back to the legal-men midpoint.

## 6. Engine deviations (engine/anim.c handler_mount) — what to change

1. First loop must write `v.state=5, v.move_id=0x5F` (0x09: 0x5D) after the no-KO path, once; today the victim stays 0xFF
   forever → no mash, no escape (the PLAYTEST finding).
2. KO test is `v.hp == 0` at the time of the loop (0x111F8), not `hp <= 3`; `v.dmg=3` is a pipeline request.
3. Release on KO/result only (state 7 / v.state 4 / `react&=0xFF` keeps the low byte, not `react=8`; no `down_t=0x30`,
   no `partner=-1` — the 68k clears f33 b6 + f35 b1/b2 and leaves partner links; poison is set only on the rumble-KO path).
4. Victim handler + 0xEBC4 ladder + escape move 0x5C/0x67 needed (below). Referee SM4/SM8 per §5.

## 7. C sketches

```c
/* 0x15B8C / 0x135EC — holder. VM = 0x5F for 0x22, 0x5D for 0x09; LOOPF = 3 / 4; ANN = 0x0A / 0x09 */
static void hold_holder(eng_obj *o, eng_obj *v, int VM, int LOOPF, int ANN, bool is09) {
    if (!entered(o)) { o->mover=0; o->x=v->x; o->y=v->y; o->facing_lo=v->facing_lo; return; }
    if (o->frame_lo==0 && o->count==0) {
        if ((v->state&0xFF)!=4 || (v->react_id&0xFF)!=8 || !link_ok(o,v)) {
            o->state=0; o->spr=o->facing; o->partner|=BIT31; o->count=1; return; }
        o->f33|=0x40; v->f33|=0x40; o->f35|=0x02; v->f35|=0x04;
        if (is09) v->held_c7++;
        v->state=0x00FF; announcer(o->id, ANN); return;
    }
    if (!is09) o->atk4c = (o->move_id&0x8000) ? 0x8014 : 0;
    if (o->frame_lo!=0xFE) return;
    if (!(o->result&0x8000)) {
        o->frame=LOOPF; o->count=0; v->dmg=3;
        if (!ko_check_111C8(o,v)) {                         /* carry clear */
            if (!(o->move_id&0x8000)) { o->move_id|=0x8000; v->state=5; v->move_id=VM; score_215B6(o); snd(0x32); }
            return;
        }
        if (rumble) { announcer(v->id,0x29); v->f32|=0x10; o->partner|=BIT31; o->spr=0x68|o->facing_hi; }
    }
    o->state=7; v->state=4; v->react_id&=0xFF;               /* release 0x15C8E / 0x136E2 */
    o->f35&=~0x02; v->f35&=~0x04; o->f33&=~0x40; v->f33&=~0x40;
}

/* 0x19536 — victim move 0x5D/0x5E/0x5F/0x60/0x62/0x63 (code-only record, never finishes) */
static void hold_victim(eng_obj *o) {
    static const int8_t seed_mode[] = {[0x5D]=4,[0x5E]=0,[0x5F]=4,[0x60]=6,[0x62]=7,[0x63]=7};
    int m = o->move_id&0xFF;
    if (!entered(o)) { o->f52=0; o->mover=0; o->spr=0xFFFF; o->count=0xFF00; score_21732(o);
        if (m==0x5D||m==0x5E||m==0x5F||m==0x60||m==0x62||m==0x63) { o->mash_aa=0; mash_seed(o, seed_mode[m]); }
        return; }
    helper_11432(o);
    if (m==0x5E && o->hp==0) return;
    down_tick(o);                                            /* 0x10D04 */
}
/* escape: 0xEBC4 on next press → state=5, move=map[m], mash_aa=0 (escape-machine.md §2) */

/* 0x1978A — move 0x67 (escape from 0x5D). h = holder (partner). 0x5C differs only in frame (1), react 0x27, dmg 3,
 * slide (0x10,-1,0), no facing flip, no y-1, and 0x212A0 at the end. */
static void esc_67(eng_obj *o, eng_obj *h) {
    if (!entered(o)) { h->state=0x00FF; o->f35&=~0x04; h->f35&=~0x02; return; }
    if (o->frame_lo==2 && o->count==0) {
        h->state=4; h->react_id=6; h->facing^=0x8000; h->dmg=2; snd(0x32);
        place_10B9A(o,h,0x38,1,0); score_212A0(o); h->y-=1; o->f33&=~0x40; h->f33&=~0x40;
    }
    if (o->frame_lo==0xFE) { o->state=7; o->partner|=BIT31; o->spr=0x68|o->facing_hi; }
}

/* CPU held victim, 0x1E39C family */
static void cpu_held(eng_obj *o, int mode, int esc) {
    if (!(o->move_id&0x4000)) { o->move_id|=0x4000; o->ba = timer_1F576[mode][o->band&0xFF]; o->bc=esc; }
    if (--o->ba) return;
    int m=o->move_id&0xFF;
    if ((m==0x52||m==0x53||m==0x61) && o->grap44) { o->ba=1; return; }
    o->state&=0x00FF; o->move_id=o->bc;                      /* clr.b +0x20 = re-enter state 5 */
}
```

## TODO EXACT
- 0x11432 semantics (+0x34 b6/b7 on helper +0x86 / its opp); f32 b0 (referee pose 7/8 vs 0xD/0xE); +0x109 (pin count?).
- Which handlers write victims into 0x52/0x53 (no immediate-word writer found; probably via register/table).
- 0x10B9A/0x10BD0 exact mirror convention; +0x18 b7, +0x19/+0x1B byte meanings set by 0x56/0x5B/0x5A.
- Move 0x1A identity; 0x37 helper move 0x4E; 0x8A/0x6F/0x6E (KO-by-hold pair) handlers not transcribed.
