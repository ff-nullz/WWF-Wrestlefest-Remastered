# Lying / get-up timing — exact transcription (read-only)

Source: `reference/maincpu.asm`. Field names: state +0x20, react_id +0x64 (lo +0x65), down_t +0x9A,
mash_aa +0xAA, f32/f33/f34/f35 = +0x32..+0x35, partner +0x26 (long), opp +0x7A (long), result +0xFE,
count +0x22, frame +0x24 (lo +0x25), band +0x70 (word; 0x10F56 reads the lo byte +0x71), mover +0x01,
btn_new +0xA2 (lo +0xA3), f56 +0x56 (b7 CPU, b6 apron/eliminated), anim_sel +0x1C.

Cadence: the cell handler is called from the anim script `0x1C03E` inside the pose pass `0xF4C2`, which is
in BOTH the even and odd work lists (frame-order.md §1b) → **one handler call per frame per object**.
`+0x9A` is therefore in frames (57.44 Hz). A cell duration `d` is shown for `d+1` tick calls (anim.md §1c).

## 1. 0x10F56 — seed forced-down time

| PC | test / write | meaning |
|---|---|---|
| 0x10F56 | `tst.w +0x9A; bne 0x10F9A` | **pre-set by a move → leave it** (no reseed) |
| 0x10F5C | `btst #7,+0x56; beq 0x10F74` | human → straight to the band seed |
| 0x10F64 | `btst #5,+0x33; bne 0x10F94` | CPU with "AI hustle" bit → 0x30 |
| 0x10F6C | `btst #2,+0x33; bne 0x10F94` | CPU that is the tag corner-man / illegal man → 0x30 |
| 0x10F74 | `cmpi.b #2,+0x71; bne` | band 2 (hp ≤ 0x18) → `+0x9A = 0xE0` (0x10F7C) |
| 0x10F84 | `cmpi.b #1,+0x71; bne` | band 1 (hp ≤ 2·hp_max/3) → `+0x9A = 0x80` (0x10F8C) |
| 0x10F94 | fallthrough / gate target | `+0x9A = 0x30` |

Seeds are exactly 0x30 / 0x80 / 0xE0 frames. The gates only ever force the SHORT path (0x30) for a CPU
that is hustling or is the non-legal tag man; a plain CPU and a human use the band seed. Band is computed by
`0x24EC2` (called at 0x188BE / 0x18AAC on hit apply): `hp ≤ 0x18 → 2`, `hp ≤ 2*hp_max/3 → 1`, else 0.
Callers of 0x10F56: lying init 0x1B348; and three move handlers that `clr.w +0x9A` first and land the
victim directly lying: 0x15F78/0x15F82 (victim → state 4 react 7, `+0x68 = 0x14`), 0x17898/0x1789C
(victim react 0x1A, `+0x68 = 0x12`), 0x18CD2/0x18CD6 (attacker self → state 4, `react &= 0xFF`; partner → 7).

## 2. Lying handler 0x1B318 (cells 0x1B300 id 8 / 0x1B30C id 9: mode 1, n=1, dur 0xFF00, spr 0x12/0x13)

Init (anim_sel b15 clear), 0x1B320-0x1B38C:

| PC | write |
|---|---|
| 0x1B320 | `mover = 0` |
| 0x1B324 / 0x1B32A | `bclr #4,f33`; `bclr #7,+0x60` |
| 0x1B330-0x1B338 | `+0x52 = +0x54 = +0xD0 = 0` |
| 0x1B33C / 0x1B342 | `bclr #6,f33` (engaged); `bclr #3,f33` |
| 0x1B348 | `jsr 0x10F56` (§1) |
| 0x1B34E | id 9: `+0x3E=0x30, +0x40=-0x18, jsr 0x280DC, X+=+0x38, Y+=+0x3A, +0x3E=+0x40=0`; id 8: `jsr 0x10B62(0x50)` |

No write to +0xAA at init. Per frame (anim_sel b15 set), 0x1B390-0x1B3D0:

| PC | test | effect |
|---|---|---|
| 0x1B390 | `btst #4,f32` set | → RISE (forced: opponent picked you up, reactions.md §2d) |
| 0x1B398 | `jsr 0x115D2` | if result==0 and partner's +0x26 != self → `partner = 0, +0x9C = 0` |
| 0x1B39E | `btst #7,(+0xFE)` (= result bit15) set | → RISE (match final) |
| 0x1B3A6-0x1B3B0 | `A3 = opp(+0x7A); btst #4,(A3->+0x34)` set | **rts — opponent is pinning: no tick, no mash, stay down** |
| 0x1B3B2 | `jsr 0x10D04` | `bset #7,$1C167A` ("body down" for referee). If `(+0x56 & 0xC0)==0` (human, legal) and `+0xAA != 0x4000` and `+0xA3 != 0` (NEW button press this frame): `+0xAA -= 1`; at 0 → `+0xAA = 0x4000` ("mashed out") |
| 0x1B3B8 | `tst.w +0x9A` == 0 | → RISE |
| 0x1B3BE | `subq.w #1,+0x9A` → 0 | → RISE, else rts |
| 0x1B3C4 RISE | `state = 0x0007` (0x1B3C4); `bclr #7,(+0x64)` (0x1B3CA = react_id bit7 "face-down" flag) | +0x9A / +0xAA NOT touched here |

Findings the engine should match:
- **Countdown is every frame, ungated except by "opponent pinning".** No minimum-down hold exists.
- **Buttons never change +0x9A.** Mashing only drives +0xAA. A human rises EARLY through the action
  prefix `0xEBC4` (only caller: `bsr 0xEBC4` at 0xDEB4 inside input walk 0xDE86, which runs from the
  human SM 0xF18A): `mash_aa == 0x4000` and opp not pinning (0xEBFA) → id 8: state 5 move **0x38**
  spring-up (0xEC14); id 9: move **0x78** roll-away (0xEC6A) unless partner is in state 5 move
  0x35/0x0E/0x0F/0x23 with +0x60 bit4 clear (0xEC34-0xEC66 → fail). Success clears +0xAA (0xEDB6).
- **The CPU never mashes.** +0xA2 is written only by the input poll (0x51C4/0x6528, which skips +0x56 b7)
  and cleared at 0x5B08; no AI code writes it, so 0x10D04 never decrements a CPU's +0xAA. A CPU rises on
  +0x9A expiry, or via its AI 0x1DAD6 (state-4 CPU think): id 8 + partner in state 5 move 0x08 (cover
  setup) → d100 `0x24CC` against table `0x23D7E[$1C0162]`[band] (rumble: `0x1DE4E`) → `move = 0x38,
  state = 5` (0x1DB88). Attract mode (`$1C007C == 0`, not rumble): `+0x9A = 0x1000` (0x1DB20) — stays
  down. Id 9 (0x1DC48): partner-move table dispatch, no +0x9A/+0xAA writes (TODO EXACT 0x1DD20 table).
- Pressing buttons cannot LENGTHEN the down time anywhere.

## 3. Every other writer of +0x9A

| PC | target | value | context |
|---|---|---|---|
| 0x11E64 | self | 0 | get-up init (§4) |
| 0x1328E | A2 victim | 0 | victim setup in a grapple move (clr, with +0xAA=0 at 0x1329A) |
| 0x13F6E | A2 | 0 | same pattern (+0xAA=0 at 0x13F72) |
| 0x15D56 | A2 victim | **0x1000** | pin cell 0x15D44 init (with +0xAA=0x1000): victim frozen, un-mashable |
| 0x15F02 / 0x15F78 / 0x15FBE | A2 / self | 0 | slam variants; 0x15F78 then `jsr 0x10F56` → band reseed |
| 0x1761E | A2 victim | **0x200** | pickup/cover approach phase 0 (moves 0x0E/0x35, 0x175FC): hold body down while walking to it |
| 0x17898 / 0x178F6 | A2 | 0 | 0x17898 then `jsr 0x10F56` (react 0x1A) |
| 0x18184 | self (pinner) | **0x20** | pin RELEASE 0x180FA-0x18184: pinner → state 4, `react &= 0xFF` (lying), partner A2 → state 7, `bset #7,A2->+0x26` |
| 0x18246 | A2 pinner | **0x50** | kick-out 0x1820C: pinner → state 4 react 0x0F, down 0x50 |
| 0x182A8 | A3 (+0x9C helper) | **0x50** | kick-out second body (tag cover) |
| 0x18812 | opp | **0x1000** | pin cell: `opp->+0x9A = opp->+0xAA = 0x1000` |
| 0x18CD2 | self | 0 | then `jsr 0x10F56` (reseed) |
| 0x1BBE8 | self | 0 | clr in a state-4 reaction init TODO EXACT which |
| 0x1DB20 | self (CPU) | **0x1000** | attract-mode CPU lying (AI) |
| 0x1E29C | self | 0 | CPU AI reset (with +0xAA=0 at 0x1E292) |
| 0x1F222 | opp (A1) | **+= 0x100** | CPU AI 0x1F1AC: opp lying in-ring, self legal, `0x24CC` roll == 0 → opp down +0x100 frames, `opp->+0xAB += 0x14`, self → state 1 walk-sub 9 (taunt/stomp setup TODO EXACT) |
| 0x2448E / 0x244A0 | A0 and A1 | **0x50** | hit apply 0x2447A: both → state 4 react 2 (mutual clothesline/collision trade) |
| 0x244C4 | A1 | 0x50 | same block, single-side variant |
| 0x24BFA | A2 (victim's partner) | **8** | hit apply 0x24BC2: A1 (react 0/0xA) → state 4 react 2; A1's partner A2 → state 4 react **9**, down 8 — hitting a holder drops the held man briefly |
| 0x24C80 | A2 | 8 | same, partner react 0x0B |
| 0x3E19E | A1 | byte `$1C5C` | non-match (select/attract object) — ignore |

Hit-while-lying reactions 6/7 do NOT write +0x9A (see §6).

## 4. Get-up state 7 — handler 0x11E42, cell 0x11E36 {mode 1, n=1, dur 0x10, spr 0x68}

| PC | write |
|---|---|
| 0x11E4A | `mover = 0` |
| 0x11E4E-0x11E5A | `bclr #3,#4,#6,f33` |
| 0x11E60 | **`mash_aa = 0`** |
| 0x11E64 | **`down_t = 0`** |
| on `+0x25 == 0xFE` (0x11E6A): 0x11E72 | `$1C1697 = 1` (referee "he's up" cue) |
| 0x11E7A | `result != 0` → `state = 0` |
| 0x11E80 | `+0x1F == 5` (previous anim selector was a move) → `state = 0` |
| 0x11E88 | `bclr #5,react_id`: was set → `state = 4, react_id = 1` (dizzy); else `state = 0` |

Single sprite 0x68 for 0x10+1 = **17 tick calls**; the handler sees 0xFE on the tick after the 17th
decrement (TODO EXACT ±1: handler-vs-tick order inside 0x1C03E). No delay other than the anim itself;
state 0 stand follows immediately.

## 5. Bounce 0x1B1D0 and id 8 vs id 9

Bounce init (0x1B1D8-0x1B216): `bclr #4,f33`; `0x10B62(0x50)`; `0x258E(3)` pop vx=0 vz=0x300 g=0x38
(flight ≈ 2·0x300/0x38 ≈ 27 frames); **`+0xAA = 0` (0x1B1F2) then `0x10C60(1)`** = mash seed by hp
quarter of hp_max: ≤¼ → 0x15 presses, ≤½ → 7, ≤¾ → 3, else 1 (`0x10D00` = 01 03 07 15);
`react != 0x2A && band == 2` → `bset #7,react_id` (face-down). Per frame: `0x10D04` tick (mash counts
during the bounce too), landed (+0x37 b4) → thud, `state = 4`, `react = 0x2A ? 8 : bit7 ? 9 : 8`.
Id 9 vs id 8 differ ONLY in the init probe (§2) and sprite; **both use the same +0x9A countdown, id 9 does
not stay down longer or wait for a button.** Id 9 gets band 2's 0xE0 seed simply because band 2 is what
selects id 9. Id 9's mash exit is move 0x78 instead of 0x38. The "pin release" at 0x18184 and the other
pre-setters do not distinguish 8/9 either.

## 6. Hit while lying — reactions 6/7, handler 0x1B288 (cells 0x1B268/0x1B278: dur 0x14/0x16)

Init: `0x258E(0x15)` knockback, `0x10B62(0x50)`, `0x10C60(1)` (**re-seed mash only if +0xAA == 0**
— 0x10C6A returns when non-zero, so a partly-mashed count survives). Per frame: `0x10D04` tick; at
`+0x24 == 0` && landed → thud, `mover = 0`, `count = 0`; `0x115D2`; on 0xFE → `state 4`, `react = (was 6) ? 8 : 9`.
**No +0x9A write.** The stomp does not reset the down timer: +0x9A pauses while 6/7 runs (lying handler
not executing) and resumes from its residual at lying re-init (0x10F56 sees non-zero → no reseed).
Net effect: a stomp ADDS its own ~21-23 frame animation on top of the remaining down time. Whatever
damage the stomp did may move the band, but the band is only consulted when +0x9A is 0.

## 7. Move 0x78 roll-away (table 0x12614[0x78] = 0x127F4 → cell 0x1A186)

Cell 0x1A186: `{handler 0x1A192, mode 1, n=1, dur 0x10, spr 0x1D8}` — one sprite, 17 ticks.

| PC | write |
|---|---|
| 0x1A19A (init) | `mover = 0` — **no motion at all: no angle/speed/direction writes** |
| 0x1A1A0 on 0xFE | `state = 7` (0x1A1A8); `react_id = 0` (0x1A1AE); `bset #7,(+0x26)` (0x1A1B2, detach partner pointer); `clr.l +0x9C` (0x1A1B8) |

So 0x78 is a stationary "roll to sitting" frame, not a roll-away; it doesn't touch +0x9A/+0xAA (0xEBC4
already cleared +0xAA at 0xEDB6; get-up init clears both). Total human id-9 mash exit = 17 + 17 frames.

Cross-check move 0x38 (cell 0x17AFE `{0x17B12, mode 1, n=3, dur 6,6,6, spr 0x6A,0x6B,0x6A}` = 21 ticks):
init `mover = 1` (polar), `+0x2C = 0x0080` (angle 0x80), `+0x2A = 0x18` (speed); `cmpi.w #0x240,$1C17EE;
bcc` → if **`$1C17EE < 0x240` then `bchg #7,(+0x2D)`** → angle 0x00. On 0xFE: `state 7, react_id 0,
bset #7,+0x26, clr.l +0x9C`. `$1C17EE` is the camera **scroll Y**, not X: the camera follow 0x26936
writes `$1C17E6/EA/EE/F2` and the screen-position pass uses `Y = +0x0A + +0x0E − $1C17EE` (anim.md §1,
apply-motion.md 0x24C2). So the spring-up hops along the 256-step angle 0x80 (down-screen) when the
camera is scrolled to ≥ 0x240, else angle 0 (up-screen) — a keep-on-screen rule, not a facing rule.
TODO EXACT: which way angle 0/0x80 maps in 0x2208 polar mode (0xF2D6 angle map).

## 8. Resulting stock down durations (frames @ 57.44 Hz), from lying init to standing

lying = seed frames (decrement starts the frame after init, rise on the frame it hits 0) + 1 frame state
change + get-up 17 ticks (+1). Add the preceding fall+bounce (~27 frames pop + landing) for "from hit".

| band | seed | CPU plain | CPU hustle/illegal | human not mashing | human mashing |
|---|---|---|---|---|---|
| 0 (hp > 2/3 max) | 0x30 = 48 | 48 + ~18 = **~66** (1.15 s) | 48 + 18 = 66 | 66 | 1 press (seeded 1) → up as soon as pressed: ~1 + 21 (0x38) + 18 ≈ **40** |
| 1 (hp ≤ 2/3 max) | 0x80 = 128 | **~146** (2.5 s) | 66 | 146 | 3 or 7 presses → early |
| 2 (hp ≤ 0x18) | 0xE0 = 224 | **~242** (4.2 s) | 66 | 242 | 0x15 = 21 presses → then 0x78 17 + 18 |

Pre-set overrides: pin release 0x20 (+18 → ~50), kick-out pinner 0x50 (~98), clothesline trade 0x50,
pickup hold 0x200 (stays until the attacker's move changes state), pins/attract 0x1000 (effectively forever).
Mash seed by hp quarter is separate from the band: a full-health human needs ONE new press after the bounce
(press during the bounce counts — 0x10D04 runs there too), then 0xEBC4 fires on the next input-walk frame.

## 9. C sketch

```c
static void seed_down(eng_obj *o)                            /* 0x10F56 */
{
    if (o->down_t) return;                                   /* pre-set by a move */
    if ((o->f56 & 0x80) && (o->f33 & 0x24)) { o->down_t = 0x30; return; } /* CPU hustle/illegal */
    o->down_t = (o->band & 0xFF) == 2 ? 0xE0 : (o->band & 0xFF) == 1 ? 0x80 : 0x30;
}
static void down_tick(eng_obj *o)                            /* 0x10D04 */
{
    ref_body_down = 1;                                       /* bset #7,$1C167A */
    if (o->f56 & 0xC0) return;                               /* CPU or apron: no mash */
    if (o->mash_aa == 0x4000 || !(o->btn_new & 0xFF)) return;
    if (--o->mash_aa == 0) o->mash_aa = 0x4000;             /* NOTE: decrements even if it was 0 -> 0xFFFF; 0x10C60 seeded it */
}
static uint32_t handler_lying(eng_obj *o, uint32_t cell)     /* 0x1B318 */
{
    if (!(o->anim_sel & 0x8000)) {
        o->mover = 0; o->f33 &= ~0x58; o->atk60 &= ~0x80; o->t52 = o->t54 = o->d0 = 0;
        seed_down(o);
        /* id 9 one-shot probe / id 8 slide: unchanged from today */
        return cell;
    }
    if (!(o->f32 & 0x10)) {
        partner_sanity(o);                                   /* 0x115D2 */
        if (!(o->result & 0x8000)) {
            if (o->opp >= 0 && (OBJ(o->opp)->f34 & 0x10)) return cell;   /* pinned: freeze */
            down_tick(o);
            if (o->down_t != 0 && --o->down_t != 0) return cell;
        }
    }
    o->state = 7; o->react_id &= ~0x80;                      /* RISE 0x1B3C4; f32 bit4 NOT cleared here */
    return cell;
}
static uint32_t handler_getup(eng_obj *o, uint32_t cell)     /* 0x11E42, 17 ticks spr 0x68 */
{
    if (!(o->anim_sel & 0x8000)) { o->mover = 0; o->f33 &= ~0x58; o->mash_aa = 0; o->down_t = 0; }
    else if ((o->frame & 0xFF) == 0xFE) {
        ref_up_cue = 1;                                      /* $1C1697 */
        if (o->result || (o->prev_sel & 0xFF) == 5) o->state = 0;
        else if (o->react_id & 0x20) { o->react_id &= ~0x20; o->state = 4; o->react_id = 1; }
        else o->state = 0;
    }
    return cell;
}
/* input walk (humans only), before move selection: */
if (o->mash_aa == 0x4000 && state == 4 && !(OBJ(o->opp)->f34 & 0x10)) {
    if (react == 8) { o->mash_aa = 0; o->state = 5; o->move_id = 0x38; }          /* 0xEC14 */
    else if (react == 9 && !partner_covering(o)) { o->mash_aa = 0; o->state = 5; o->move_id = 0x78; }
}
```

## 10. Where the engine diverges (engine/anim.c handler_lying / handler_getup, core.c)

1. `handler_lying` clears `f32 bit4` on rise; stock does not (0x1B3C4 only writes state and react bit7).
   Harmless unless something reads it later. TODO EXACT who clears +0x32 b4.
2. Missing the **opponent-pinning freeze** (0x1B3A6): while `opp->f34 & 0x10` the countdown must not run.
   (Pins also pre-set 0x1000, so this mostly matters for the cover-approach window.)
3. Missing the CPU gate (`f56 b7 && f33 & 0x24 → 0x30`). This only ever SHORTENS stock, so it is not the
   "gets up too fast" cause.
4. `handler_getup` does not clear `mash_aa`, set `$1C1697`, or do the react bit5 → dizzy branch.
5. Seeds, band formula (hit.c 0x24EC2), once-per-frame dispatch (core.c:352) all match stock. So if a
   downed CPU rises faster than stock, check (a) whether the engine's CPU mashes (stock: never — +0xA2 is
   only written by the human input poll; if the engine's AI feeds `btn_new` into 0x10D04 or into the
   0xEBC4 pre-consumer, the CPU will spring up after 1-3 presses), (b) whether the band actually reaches
   1/2 — placeholder hp/hp_max (`wrestler.json`) keeps hp > 2/3 max, so every knockdown is the 48-frame
   band-0 case; stock rumble CPUs start at 0x87 and drop to band 1 after ~3 hits, (c) whether reactions
   6/7 re-entry preserves the residual +0x9A rather than reseeding (stock keeps it).

TODO EXACT: +0x1F==5 semantics (0x11E80); 0x1DD20 CPU id-9 table; 0x1F222 trigger meaning;
angle 0/0x80 screen direction for move 0x38; 0x1BBE8 owner.
