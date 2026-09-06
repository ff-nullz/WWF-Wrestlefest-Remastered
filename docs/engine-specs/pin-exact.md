# Pin machinery, exact — moves 0x48 / 0x49 / 0x4A / 0x4B / 0x51 / 0x08 (read-only, 2026-08-22)

Source: `reference/maincpu.asm`. Field names per `engine/engine.h`. Move cell table `0x12614[move]*4`.
Cell record = `{code.l, mode.w, n.w, delay.w×n, spr.w×n}`. Byte views: `+0x21`=state lo, `+0x25`=frame lo
(0xFE = chain finished), `+0x61`=move lo, `+0x65`=react lo. "`+0x60 bit7`" = **move word bit15** (0x8000).
`+0x18 bit7` = off_x bit15 ("clear both offsets after the anim pass", engine.h). In every handler `A0`=self,
`A2`=`partner` (+0x26). Announcer byte pair `$1C15D2/3`, YM cmd via `0x2052`.

**Headline corrections to the engine's simplification**

1. The pinner of a 0x48 cover does **not** go to state 0x0C and does **not** keep drawing cell 0x1C. After the
   catch he stays **state 5, move `0x8048`**, and his handler immediately swaps to the hidden record `0x125C0`
   (`spr = 0xFFFF`). **The victim draws the composite "pinned" body** (move 0x4A, spr `0x1D` face-up /
   `0x1E` face-down, `FF00` hold). There is no second/ghost sprite; the "ghost" is the pinner being hidden.
2. Referee cue `f35 bit0` is set **once** at the catch (0x133A2) for 0x48 (per-frame only in 0x0E's path),
   and cleared by the kick-out 0x4B (0x18216), by the victim's 0x4A exit paths (0x18166/0x18178), or a hit.
3. Count progress is referee-only (visual 6 `0x20022`); nobody on the wrestler side reads `halfct` except the
   kick-out near-fall announcer `0x21282` and the CPU kick-out scheduler `0x1E452`.
4. `+0xAA = 0x1000` at 0x15D50 / 0x18818 is **not** part of 0x48: 0x15D50 is move 0x23 (cradle) phase 0 and
   0x18818 is move 0x4C (tag-in helper; writes `opp`→`+0x9A/+0xAA`). The 0x48 cover instead lets the
   victim's 0x4A init reseed `+0xAA` via `0x10C60(0)`.
5. Pickup 0x08 freezes the victim to **state 0xFF at cell 0** (hidden) — there is no intermediate victim move;
   ends in holder 0x0C / held 0xFF exactly as the engine does, but with the write list in §5.

---

## 1. Move 0x48 — dive cover. Cell `0x132D8`, handler `0x132F4`

Cell `0x132D8`: `{0x132F4, mode 1, n=5, delays 6,6,6,6,6, spr 0x84,0x85,0x1A,0x1B,0x1C}` (30 frames).

Handler, PC order:

| PC | cond | write / call |
|---|---|---|
| 0x132F4 | `move_id bit15` set (post-catch) | → 0x1342C: **A1 = 0x125C0** (hidden record: `+0x01=0`, `spr=0xFFFF`; releases if `partner->partner != self`) ; rts |
| 0x132FE | `+0x1C bit7` clear (init) | |
| 0x13306 | init | `+0x01 = 0` (mover off) |
| 0x1330A | init | `+0x5C = 0x480` (vz), `+0x5E = 0x48` (grav) |
| 0x13316 | init | `0x26AE(D0=1)` homing: target = partner; tbl `0x275C[1]` = (dx 0x30, dy −0x20), dx negated if partner facing bit7; `T = (0x480/0x48)*2 = 32` frames; `+0x58 vx = ((px+dx−x)<<8)/T` clamp ±0x200; `+0x5A vy = ((py+dy−y)<<8)/T` clamp ±0xC0 |
| 0x13320 | init | facing `+0x2E` = (`+0x58` sign bit) XOR 0x80 (face the travel direction) |
| 0x13330 | init, partner `move_id bit15` clear | partner `state = 0x0005`, partner `move_id = 0x004A` (victim enters pinned-lying now) |
| 0x1334A | per frame, `frame(+0x24)==0 && count(+0x22)==0` | `+0x01 = 2` (ballistic launch, first tick of cell 0) |
| 0x1335C | `frame lo < 4` | rts (still flying) |
| 0x13366 | cell ≥ 4 | `+0x42 = 0xFFE0` (TODO EXACT: hitbox/shadow bias while landing) |
| 0x1336C | `+0x37 bit4` clear (no floor contact yet) | rts |
| 0x13376 | landed | `+0x42 = 0` |
| 0x1337A-0x13388 | partner `state lo != 5 \|\| move lo != 0x4A` | → MISS 0x13400 |
| 0x1338A | `0x11412`: `partner->partner != self` | → MISS |
| **CATCH 0x13392** | | `+0x01 = 0` |
| 0x13396 | | `state = 0x0005` (word write — clears state bit15 latch) |
| 0x1339C | | `move_id \|= 0x8000` (engaged latch → handler takes 0x1342C from now on) |
| 0x133A2 | | **`f35 \|= bit0`** (referee cue, set once) |
| 0x133A8/AE | | `f33 \|= bit6` on self and partner |
| 0x133B4 | | facing `+0x2E` = partner facing |
| 0x133BA-C6 | | `x,y,z = partner x,y,z` |
| 0x133CC-D6 | | `0x10BD0(D0=0x40, D1=−8, D2=0)`: `x += ±0x40` (− if facing bit7), `y −= 8`, z += 0 |
| 0x133DC | | **`spr = 0xFFFF`** |
| 0x133E2 | | `0x215B6` tag maintenance (own partner's `+0x34 bit0`, `$1C1682=0xFA`, legal bits) |
| 0x133EA | on partner | `0x21732` tag-save hook (victim's partner `+0x34 bit0`, CPU partner `+0xB5 bit7`/`+0x7E`, `+0xE6` timer from `0x217F8`) |
| 0x133F2 | | partner `state = 0x0005` (re-written — clears its bit15 latch) |
| 0x133F8 | | partner `move_id \|= 0x8000` (→ `0x804A`: now draws 0x1D/0x1E and mashes) |
| **MISS 0x13400** | | `state = 0x0004`, `react_id = 0x0005` (bounce-off), `spr = 0x001F \| facing<<8`, `+0x68 = 3`, `off_x \|= 0x8000`, partner `+0x9C = 0` (clr.l) |

No YM / no announcer write in 0x48 (0x0E's `$1C15D3=0x20` cover call is not here). No `+0x18/+0x1A` offsets
besides the bit15 clear flag. `+0x34 bit4` is never touched by 0x48 (it is the CPU *intent* bit, cleared
by the walk-to-cover 0x79 ender at 0x1A2F2).

### Double cover 0x49 (cell `0x13434`, handler `0x13444`) — third man piling on
Cell: `{0x13444, mode 1, n=2, delays 0x10, 0xFF00, spr 0x1E9, 0x1EA}`. Init: `+0x01=2`, **`off_y lo (+0x1B) = 0x40`**,
`y = partner.y − 1`, `vz 0x300`, grav 0x48, homing `0x26AE(0)` toward `partner->partner` (the original pinner),
facing from vx. At cell 1 with floor contact: `off_x|=0x8000`, `+0x42=0`; requires `partner->+0x9C == self`,
partner in 5/0x4A with `+0x60 bit5` clear, and partner's partner in 5/0x48 → `+0x01=0`, `move_id|=0x8000`,
**`f35|=bit0`** (second referee cue), partner `+0x60 |= bit5` (0x2000, "double-covered"), `f33|=bit6`,
**victim `mash_aa += 0x0C`**, victim `+0x68 = 0x0A`, pos = pinner pos, then `0x10BD0(dx,−1,dy)` from
`0x135C0` = `{face-up: (0x30,0xF8) same-facing / (0x20,0x18) opposite; face-down: … +4}` (TODO EXACT byte pairs:
`10 30 | 08 30 | F8 20 | 18 20` read as `[up/down][same/opp] = (dx,dy)` bytes at 0x135C0..0x135C7). Failure → state 4 react 5 spr 0x1F (same as 0x48 miss).
Post-engage (`bit15` set): if `partner->partner == 0` → `f35 &= ~bit0` and bounce off.

---

## 2. Pinner side during the count

The pinner has **no per-frame logic**: `0x8048` → hidden record 0x125C0 each frame. Everything is driven by:

- **Referee visual 6 `0x20022`** (pins-referee §2): `+0x23` seed 0x14 then 0x38/0x20 alternating; on each tick
  `+0x25 += 1` and **victim `halfct (+0x109) += 1`**; even `+0x25` → `$1C169A++`, blit `0x2067C(+0x24>>1)`,
  YM `+0x54++` (0x3162 ONE, 0x3163 TWO, 0x3164 THREE). `+0x25 == 6` ("3" up): `+0x23 += 0x10`, tag →
  **pinner pair `result = 0x4000`, victim pair `result = 0x4001`** (via `+0x86`), `clr $1C169E`, `0x90D6`;
  rumble → `0x21358` credit, pinner `+0xC4++`, victim `f32 |= bit4`. `+0x25 == 7` → `$8009` win pose
  (tag) + wipe `0x206FE` + pinner `+0x4A = 4`; rumble → wipe + `$8005`. The writer of `+0xFE` is the
  **referee** (0x200FC-0x2011A), nobody else.
- **End states after the 3**: victim's 0x4A handler sees own `result bit7`… NO — `+0xFE bit7` is only set by
  visual 9 at pose 11 (`$4000→$C000`). Until then both stay in 0x8048/0x804A. When bit7 lands, victim
  0x180FA-0x18184 path: pinner → `state 7`, `f35 &= ~bit0`, `partner |= 0x80000000`; victim → `state 4`,
  `react_id &= 0xFF`, `down_t = 0x20`. (Rumble: `f32 bit4` → same plus `$1C15D2=slot,$1C15D3=0x29`, and a
  0x49 third man released to state 7.)
- **Kick-out detection**: not on the pinner. Victim `mash_aa` hits 0x4000 in `0x10D04`; next press `0xEBC4`
  → victim `state=5, move_id=0x4B, mash_aa=0`. 0x4B's init writes the pinner (§3). CPU victims: `0x1E452`
  writes `move_id = 0x4B` when `halfct == +0xBD`.

---

## 3. Victim side

### Move 0x4A — pinned (cells `0x180AC` / `0x180B8`, handler `0x180C4`)
Cells: `0x180AC {0x180C4, mode 1, n=1, delay 0xFF00, spr 0x1D}` (face-up composite), `0x180B8 {…, spr 0x1E}` (face-down).

| PC | cond | write |
|---|---|---|
| 0x180C4 | init | `+0x01 = 0`; `mash_aa = 0`; `0x10C60(0)` → `mash_aa = hp==0 ? 0x100 : max(1, 0x2A − hp)` |
| 0x180E0 | per frame | `0x11432`: non-CPU, non-rumble: clear `+0x34` bits 6/7 on own `+0x86` partner and on that partner's `opp` |
| 0x180E6-F4 | `hp != 0 && pinner move_id bit15` | `0x10D04` mash tick (`f56 & 0xC0 == 0` gate; `$1C167A bit7`; fresh `+0xA3` → `--mash_aa`, 0 → 0x4000) |
| 0x180FA-1811A | pinner not (state 5 and move ∈ {0x48,0x0E,0x0F}) | → 0x18184 RELEASE (below) |
| 0x1811C | own `f32 bit4` (rumble elim) | `$1C15D2 = slot(+0x03)`, `$1C15D3 = 0x29`; if `+0x9C->partner == self` and `+0x9C` in 5/0x49 → that obj `state=7`, `f35&=~bit0`, its z = own z; then 0x18172 |
| 0x18124 | own `result bit7` (match decided) | 0x18172: pinner `state = 0x0007`, pinner `f35 &= ~bit0`, pinner `partner \|= 0x80000000`; then RELEASE |
| 0x18184 RELEASE | | `down_t = 0x20`, `state = 0x0004`, `react_id &= 0xFF` (8/9 lying) |
| 0x1819A | draw select: `state lo == 0x0E` (TODO EXACT — never true in state 5; dead test) or `move_id bit15` clear | A1 = `0x125C0` hidden rec, then `spr = (facing<<8) \| (react lo==8 ? 0x12 : 0x13)` — plain lying body while the cover is still in the air (TODO EXACT ordering vs. the hidden record's `spr=0xFFFF`; observed game shows the lying body, so the handler's write must land after the cell code) |
| 0x181D4 | `bit15` set | A1 = `react lo==8 ? 0x180AC : 0x180B8` |

### Move 0x4B — kick-out (cells `0x181EC` / `0x181FC`, handler `0x1820C`)
Cells: `0x181EC {0x1820C, mode 1, n=2, delays 0xC,0xC, spr 0x6E,0x6F}` (from face-up), `0x181FC {…, spr 0x70,0x71}` (face-down). Selected at 0x182FC by `react lo == 8`. 24 frames.

| PC | write (init, `+0x1C bit7` clear) |
|---|---|
| 0x18216 | pinner **`f35 &= ~bit0`** (referee SM6 falls to SM1 next frame → wipe + `$8005`, unless `+0x25 >= 6`) |
| 0x1821C | pinner `state = 0x0004`, `react_id = 0x000F` (thrown off) |
| 0x18228 | pinner `spr = 0x00F0 \| facing<<8`; pinner facing = own facing |
| 0x1823A/40 | `f33 &= ~bit6` both |
| 0x18246 | pinner `down_t = 0x50` |
| 0x1824C-58 | `0x10B9A(D0=0x50, D1=−1, D2=0x10)`: pinner pos = own pos, then `x += ±0x50` (− if own facing bit7), `y −= 1`, `z += 0x10` |
| 0x1825E | `0x21282`: `halfct ∈ {4,5}` → **`$1C15D2.w = 0x0F1E`** (near-fall announcer); `+0x108 = 0` (clr.w: resets halfct **and** +0x108); then tag bookkeeping `0x212D4` on self & partner (legal/+0x86 juggling) |
| 0x18264-182DA | if `+0x9C->partner == self` and `+0x9C` in 5/0x49: third man → `state 4`, `react 0x0F`, `off_x\|=0x8000`, facing = own facing flipped, `down_t=0x50`, `f35&=~bit0`, `spr=0xF0\|facing`, `f33&=~bit6`, `0x10B9A(0x50,−1,0x10)` relative to self |
| 0x182DE | per frame: `frame lo == 0xFE` → `state = 0x0007`, `react_id = 0`, `partner \|= 0x80000000`, `+0x9C = 0` |

No YM / no other announcer in 0x4B.

### Move 0x51 — squash (0x0E/0x0F/0x35/0x23 victims; cells `0x18BF4`/`0x18C04`/`0x18C14`, handler `0x18C24`)
Cells: `{0x18C24, mode 1, n=2, delays 0xFF00, 2, spr 0x15,0x1D}` / `{…, 0x17,0x1E}` / `{…, 0x16,0x13}` (pick: pinner in 0x0E → 0x18C14, else react 8 → 0x18BF4, else 0x18C04).
Init: `mash_aa=0; 0x10C60(0); 0x258E(3)` (tbl `0x25CA[3]` = vx 0x200? TODO EXACT row: `0x25CA+18` = `0038 0000 0300` → `+0x58=0x38? ` — row layout is `{vx, vz, grav}`: row3 = (0x38→neg by facing, vz 0x0000?, …)` TODO EXACT; pins-referee says vz=0x300 grav 0x38). Landing (`+0x25==0 && f37 bit4`): `+0x01=0`, `count=0` (breaks FF00). At `0xFE`: live & both legal & not out →
`state=0x8005, +0x1C=0x8005, move_id=0xC04A`, `0x21732`; pinner (unless 0x0E) `state=0x8005, +0x1C=0x8005, move_id=0xC048`, `0x215B6`. Else: `state=4, react&=0xFF, down_t=0`, `0x10F56` (down_t = 0xE0/0x80/0x30 by `+0x71`), pinner `state=7`, `partner|=0x80000000`.
NB `+0x1C = 0x8005` pre-sets the anim-init latch so the 0x4A/0x48 handlers skip init (mash seed from 0x51 survives; pinner skips the leap).

---

## 4. Count digits, colour, sounds

- `0x2067C(D0)`: window `$C0748` (n<10) / `$C0744` (tens at +0, ones at +8) / `$C1188` (D0 bit15, unused in-match).
  Glyph ptr table `0x2073A` (21 longs → `0x2078E + 8n`); 4 rows × 2 tiles, **attr word always `0x00EA`** — same
  palette for 1, 2 and 3 and for the kick-out (the digits are simply wiped by `0x206FE`). Row 3 (digit "3") = tiles
  `6B 6C / 7B 7C / 8B 8C / 9B 9C`; "1" = `67 68 / 77 78 / 87 88 / 97 98`; "2" = `69 6A / 79 7A / 89 8A / 99 9A`.
- **No "2.9"/near-fall visual exists**: half-counts are internal (`+0x25` odd ticks only toggle the referee pose
  5/6 with `+0x23 = 0x38/0x20`). The only near-fall effect is the announcer `0x0F1E` at kick-out when `halfct ∈ {4,5}`.
- Sounds: ONE/TWO/THREE = YM `0x3162/0x3163/0x3164` from `+0x54`; 0x48 catch: none; 0x4B: none; win: visual 9
  `0x20156` (`$3109` on stages 4/9 else `$3105`), `$1C15D2 = $0F2A` human winner / `$0F2C` + `clr $1C15D4` CPU.
- The kick-out aborts only while referee `+0x25 < 6` (SM6 `0x1F9D8` rts at ≥6).

---

## 5. Pickup move 0x08 — cell `0x1316C`, handler `0x13180`

Cell: `{0x13180, mode 1, n=3, delays 6, 0xC, 8, spr 0xCB, 0xCC, 0xCD}` (26 frames; sprites are the two-man composite).

| PC | cond | write |
|---|---|---|
| 0x13188 | init | `+0x01 = 0`; `x,y,z = partner x,y,z`; facing = partner facing |
| 0x131A4 | init | **`off_x = 0x0050`**; `+0x4A = 2` |
| 0x131B4-BE | per frame: `frame lo == 0 && count == 0` (first tick of cell 0) | catch test |
| 0x131C0 | `0x11412` fails, or partner not (`state lo == 4 && react lo == 8`) | MISS 0x131F4: `state = 0`, `off_x \|= 0x8000`, `count = 2`, `+0x4A = 0`, `partner \|= 0x80000000`, `spr = facing word`, `x += ±0x80` (− if facing bit7) |
| 0x131D8 | catch | **partner `state = 0x00FF`** (frozen/hidden via 0x125C0 — no victim move, no 0x4A), `f33 \|= bit6` both, facing = partner facing |
| 0x1322C | `frame lo != 0xFE` | rts |
| 0x13236-54 | end; role pick | A0 human → A0 holds. A0 CPU: if partner CPU **and** partner `f33 bit5` → **swap** (partner holds) |
| 0x13256 (swap) | | partner `state = 0x000C`, partner `grap44 = 0x2000`; self `state = 0x00FF`, `grap44 = 0` |
| 0x1326E (normal) | | `state = 0x000C`, `grap44 = 0x2000`; partner `state = 0x00FF`, partner `grap44 = 0` |
| 0x13284 | both | `off_x \|= 0x8000`; `+0x4A = 0`; partner `down_t(+0x9A)=0`, `+0x54=0`, `+0x52=0`, **`mash_aa=0`**; partner facing = own facing **flipped** (bchg #7); `x += ±0x80`; partner `x,y = own x,y`; `0x10B62(0)` floor-clamp self then partner |

So yes: ends holder `0x0C`/`grap44=0x2000`, held `0xFF`/`grap44=0`, partner pointers untouched (still mutual).
The engine's "snap and hide" matches the victim side; what it must add is `off_x=0x50` during the lift,
`+0x4A=2`, the `+0x80` x-shove at the end, the facing flip on the held man, and the four victim clears.

---

## 6. C sketch (engine field names)

```c
/* move 0x48 — 0x132F4 */
static void mv_cover_48(eng_obj *o) {
    eng_obj *v = OBJ(o->partner);
    if (o->move_id & 0x8000) { use_cell(o, REC_HIDDEN_125C0); return; }   /* 0x1342C: pinner invisible */
    if (!(o->anim_sel & 0x8000)) {                                        /* init */
        o->mover = 0; o->vz = 0x480; o->grav = 0x48;
        homing_26AE(o, v, 0x30, -0x20);            /* T=32 frames, vx±0x200, vy±0xC0 clamps */
        o->facing = (o->vx & 0x8000) ^ 0x80;
        if (!(v->move_id & 0x8000)) { v->state = 5; v->move_id = 0x4A; }
        return;
    }
    if (o->frame == 0 && o->count == 0) o->mover = 2;                     /* 0x1334A launch */
    if ((o->frame & 0xFF) < 4) return;
    o->f42 = 0xFFE0;
    if (!(o->f37 & BIT4)) return;                                         /* not landed */
    o->f42 = 0;
    if ((v->state & 0xFF) == 5 && (v->move_id & 0xFF) == 0x4A && OBJ(v->partner) == o) {
        o->mover = 0; o->state = 5; o->move_id |= 0x8000;
        o->f35 |= BIT0; o->f33 |= BIT6; v->f33 |= BIT6;                   /* referee cue, once */
        o->facing = v->facing; o->x = v->x; o->y = v->y; o->z = v->z;
        o->x += (o->facing & 0x80) ? -0x40 : 0x40; o->y -= 8;
        o->spr = 0xFFFF;
        tag_maint_215B6(o); tag_save_21732(v);
        v->state = 5; v->move_id |= 0x8000;                               /* 0x804A: composite + mash */
    } else {                                                              /* 0x13400 miss */
        o->state = 4; o->react_id = 5; o->spr = 0x1F | o->facing << 8; o->hit68 = 3;
        o->off_x |= 0x8000; v->link9c = 0;
    }
}

/* move 0x4A — 0x180C4, victim */
static void mv_pinned_4A(eng_obj *v) {
    eng_obj *p = OBJ(v->partner);
    if (!(v->anim_sel & 0x8000)) { v->mover = 0; v->mash_aa = 0; mash_seed(v, 0); return; }
    clear_tag_bits_11432(v);
    if (v->hp && (p->move_id & 0x8000)) down_tick_10D04(v);
    int pm = p->move_id & 0xFF;
    if ((p->state & 0xFF) == 5 && (pm == 0x48 || pm == 0x0E || pm == 0x0F)) {
        if (v->f32 & BIT4) { announce(v->slot, 0x29); release_third_man(v); goto throw_off; }
        if (v->result & 0x8000) {
        throw_off: p->state = 7; p->f35 &= ~BIT0; p->partner |= 0x80000000; goto release; }
        /* still pinned: pick composite cell */
        if (!(v->move_id & 0x8000)) { use_cell(v, REC_HIDDEN_125C0);
            v->spr = v->facing << 8 | ((v->react_id & 0xFF) == 8 ? 0x12 : 0x13); }
        else use_cell(v, (v->react_id & 0xFF) == 8 ? CELL_180AC /*1D*/ : CELL_180B8 /*1E*/);
        return;
    }
release:
    v->down_t = 0x20; v->state = 4; v->react_id &= 0xFF;
}

/* move 0x4B — 0x1820C, victim kicks out (entered via 0xEBC4 or AI 0x1E452) */
static void mv_kickout_4B(eng_obj *v) {
    eng_obj *p = OBJ(v->partner);
    if (!(v->anim_sel & 0x8000)) {
        p->f35 &= ~BIT0; p->state = 4; p->react_id = 0x0F;
        p->spr = 0xF0 | v->facing << 8; p->facing = v->facing;
        v->f33 &= ~BIT6; p->f33 &= ~BIT6; p->down_t = 0x50;
        p->x = v->x + ((v->facing & 0x80) ? -0x50 : 0x50); p->y = v->y - 1; p->z = v->z + 0x10;
        if (v->halfct == 4 || v->halfct == 5) announce_w(0x0F1E);       /* 0x21282 */
        v->halfct = 0; v->f108 = 0; tag_fix_212D4(v); tag_fix_212D4(p);
        release_third_man_49(v);                                         /* 0x18264.. */
        return;
    }
    if ((v->frame & 0xFF) == 0xFE) { v->state = 7; v->react_id = 0; v->partner |= 0x80000000; v->link9c = 0; return; }
    use_cell(v, (v->react_id & 0xFF) == 8 ? CELL_181EC /*6E 6F*/ : CELL_181FC /*70 71*/);
}

/* move 0x08 — 0x13180 pickup */
static void mv_pickup_08(eng_obj *o) {
    eng_obj *v = OBJ(o->partner);
    if (!(o->anim_sel & 0x8000)) { o->mover = 0; o->x=v->x; o->y=v->y; o->z=v->z; o->facing=v->facing;
        o->off_x = 0x50; o->f4a = 2; return; }
    if ((o->frame & 0xFF) == 0 && o->count == 0) {
        if (OBJ(v->partner) == o && (v->state & 0xFF) == 4 && (v->react_id & 0xFF) == 8) {
            v->state = 0x00FF; o->f33 |= BIT6; v->f33 |= BIT6; o->facing = v->facing;
        } else { o->state = 0; o->off_x |= 0x8000; o->count = 2; o->f4a = 0;
                 o->partner |= 0x80000000; o->spr = o->facing << 8; o->x += (o->facing & 0x80) ? -0x80 : 0x80; }
        return;
    }
    if ((o->frame & 0xFF) != 0xFE) return;
    eng_obj *h = o, *held = v;
    if ((o->f56 & BIT7) && (v->f56 & BIT7) && (v->f33 & BIT5)) { h = v; held = o; }   /* 0x13236-54 */
    h->state = 0x0C; h->grap44 = 0x2000; held->state = 0xFF; held->grap44 = 0;
    o->off_x |= 0x8000; o->f4a = 0;
    v->down_t = 0; v->f54 = 0; v->f52 = 0; v->mash_aa = 0;
    v->facing = o->facing ^ 0x80;
    o->x += (o->facing & 0x80) ? -0x80 : 0x80; v->x = o->x; v->y = o->y;
    floor_clamp_10B62(o, 0); floor_clamp_10B62(v, 0);
}
```

## TODO EXACT
- `+0x42 = 0xFFE0` during the dive landing (0x13366) — consumer unknown (hit-probe bias?).
- 0x1819A `cmpi.b #$e,(+0x21)` — which state 0x0E is meant; appears unreachable from state 5.
- Ordering of the hidden record's `spr=0xFFFF` vs the handler's `spr=0x12/0x13` write at 0x181B8 (which wins on screen).
- 0x135C0 byte-pair table decode for the 0x49 offset (8 bytes `10 30 08 30 F8 20 18 20`).
- `0x25CA` row 3 for the 0x51 squash launch (`+0x58/+0x5C/+0x5E` exact values).
- `f33 bit5` meaning in the 0x08 role swap (CPU-vs-CPU only).
