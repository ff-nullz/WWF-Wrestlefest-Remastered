/* Tag-team mode — transcription of the apron partner (state-1 subs
 * 0x11796 follow / 0x1192C post / 0x11ABA walk-out, move 0x4F climb-out)
 * and the tag exchange (0xDD02 trigger, moves 0x4C/0x4D, SWAP 0x188BC),
 * plus the PIN RUN-IN: 0x215B6 / 0x21732 arming, the 0x21618 pad hand-over
 * and its 0x212D4 restore, the 0x24C0C pin break and the $1C1682 usher
 * grace (docs/engine-specs/pin-partner.md).
 * Spec: docs/engine-specs/tag-mode.md. SIMPLIFIED: the apron men move on
 * the slanted apron line directly (no ring probe), the walk-out climbs at
 * the rope line, the tag's double-team window (0x1F760/0x214C0) and the
 * referee's usher escort walk are TODO EXACT. */
#include <stdlib.h>
#include <stdio.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"

extern int eng_dbgsel;

/* ROM tables this file reads (docs/table-migration-brief.md): the rescue
 * arm countdowns (+0xE6) by the apron partner's band. 0x2172C is read at
 * 0x21674 (own partner, non-pin move, 0x215B6) and by ai.c's standing-
 * hold arm; 0x217F8 at 0x217BA (the victim's partner, 0x21732). Both are
 * three words and run straight into code (0x21732 / 0x217FE). */
static const tbl_def tag_tables[] = {
    { "rescue_arm_own_partner",    "base/tag", 0x2172C, 3 * 2, TK_U16, 3,
      "0x21674 run-in countdown (+0xE6) for the pinner's/holder's own apron partner by his band 0..2; preceded by the 0x2172A pair, code 0x21732" },
    { "rescue_arm_victim_partner", "base/tag", 0x217F8, 3 * 2, TK_U16, 3,
      "0x217BA run-in countdown (+0xE6) for the victim's apron partner by his band 0..2; code 0x217FE" },
    /* the behind-grab family's own arm routines (docs/engine-specs/behind-grab.md) */
    { "behind_arm_holder_partner", "base/tag", 0x21038, 3 * 2, TK_U16, 3,
      "0x20FAC (0x20F04, A0 = the 0x1D holder, every loop): +0xE6 countdown for his apron partner by band {1,1,1}; code 0x2103E follows" },
    { "behind_arm_victim52_partner", "base/tag", 0x2110E, 3 * 2, TK_U16, 3,
      "0x210B4 (0x2103E, A0 = the 0x52 victim, once): +0xE6 countdown for his apron partner by band {3E,5D,7D}; code 0x21114 follows" },
    { "behind_arm_victim53_partner", "base/tag", 0x2127C, 3 * 2, TK_U16, 3,
      "0x2119E (0x21114, A0 = the 0x53 victim, once): +0xE6 countdown for his apron partner by band {7D,BB,BB}; code 0x21282 follows" },
    { "dt_retaliate_arm",            "base/tag", 0x214BA, 3 * 2, TK_U16, 3,
      "0x21480 (0x21424, the double-team dive landing): +0xE6 run-in countdown for the VICTIM's apron partner by band {1F,1F,1F}; code 0x214C0 follows" },
    { "double_team_rolls",           "base/tag", 0x23378, 12 * 4 + 12 * 6, TK_U8, 16,
      "0x21530 (0x214C0 hold-for-partner tag): [partner id] long -> [held man's band 0..2] {p,100-p} d100 pair (0x24CC); bucket 0 = the corner dive (state 8), else run in; 0x23378..0x233EF" },
};
TBL_REGISTER(tag_tables)

int eng_side(const eng_obj *o) { return (o->role & RF_SIDE) ? 1 : 0; }

/* The LEGAL man of o's own team. With 3-man teams the teammate links are
 * CIRCULAR (core.c seating), so an apron man's +0x86 may point at the
 * OTHER apron man — the machinery that means "my in-ring man" must use
 * this instead ("i couldnt tag out my partner", survivor playtest
 * 2026-08-24). Falls back to the linked teammate. */
eng_obj *eng_team_legal(eng_state *st, const eng_obj *o)
{
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *q = &st->obj[i];
        if (q->active && (q->role & RF_LEGAL) && !(q->st_flags & SF_ELIMINATED)
            && !((q->role ^ o->role) & 0x80u) && q != o)
            return q;
    }
    return o->teammate >= 0 ? &st->obj[o->teammate] : NULL;
}

/* ------------------------------------------------------------------ *
 * data/romdata/tag_rules.json — the run-in / pin-rescue scalars.      *
 * Row order IS the slot index; the ROM immediates below are the       *
 * compiled-in defaults so a missing table changes nothing.            *
 * ------------------------------------------------------------------ */
static const int tag_rule_def[TAG_N_RULES] = {
    0x00FA,   /* TAG_USHER_ARM            0x215FA/0x2176E/0x18B40/...   */
    0x0177,   /* TAG_USHER_RUNIN          0x1D582 — in-ring grace       */
    0x0001,   /* TAG_PIN_ARM_DELAY        0x2164E / 0x2179C             */
    0x0010,   /* TAG_ENTER_TICKS          0x18A90 cell delay            */
    0x0002,   /* TAG_ENTER_CELLS          0x18A90 cell count            */
    0x0038,   /* TAG_ENTER_STEP_X         0x18AEA/0x18AF8               */
    0x000F,   /* TAG_BREAK_RC_PINNER      0x1821C (ROM 0x24BD8 = 2)     */
    0x0009,   /* TAG_BREAK_RC_VICTIM      0x24BF4                       */
    0x0008,   /* TAG_BREAK_DOWN_T         0x24BFA                       */
    0x0050    /* TAG_BREAK_PINNER_DOWN_T  0x18246                       */
};
/* The same scalars as a synthetic table ("rules" group): exported to
 * data/tables/rules/tag_rules.json, packed, moddable (ADR-001). */
static const uint8_t tag_rules_be[] = {
    TBL_BE16(0x00FA), TBL_BE16(0x0177), TBL_BE16(0x0001), TBL_BE16(0x0010), TBL_BE16(0x0002),
    TBL_BE16(0x0038), TBL_BE16(0x000F), TBL_BE16(0x0009), TBL_BE16(0x0008), TBL_BE16(0x0050),
};
static const char *const tag_rule_labels[] = {
    "usher_arm_frames", "runin_grace_frames", "pin_arm_delay", "enter_ticks", "enter_cells",
    "enter_step_x", "break_react_pinner", "break_react_victim", "break_down_t", "break_pinner_down_t", NULL };
static const tbl_def tag_rule_tables[] = {
    { "tag_rules", "rules", TBL_SYNTH, sizeof tag_rules_be, TK_U16, 1,
      "engine scalars (ROM immediates): usher arm 0x215FA, run-in grace 0x1D582, pin arm delay 0x2164E, enter ticks/cells/step 0x18A90/0x18AEA, pin-break reacts 0x1821C/0x24BF4, down times 0x24BFA/0x18246 — order = TAG_* enum", tag_rules_be, tag_rule_labels },
};
TBL_REGISTER(tag_rule_tables)

int eng_tag_rule(int slot)
{
    if (slot < 0 || slot >= TAG_N_RULES)
        return 0;
    if (tbl_bytes(TBL(tag_rules), NULL))
        return (int)tbl16(TBL(tag_rules), (uint32_t)slot * 2u);
    return tag_rule_def[slot];
}

/* The covering man of a live pin: engine state 0x0C with `pinning`
 * (the ROM keeps him in state 5 move 0x8048, pin-exact.md §1). */
int eng_pin_is_pinner(const eng_obj *o)
{
    if (!o->active || !o->pinning || o->partner < 0) return 0;
    if ((o->state & 0xFFu) == ST_HOLD) return 1;              /* the 0x48 cover */
    /* a SCRIPTED cover: the diver holds his own splash / leg-drop record
     * (state 5, 0x13E20 / 0x18CA8) with the referee cue armed - the same
     * live pin as far as a rescuer is concerned (user 2026-08-28: the
     * partner "just punched in the air" over a corner-splash pin because
     * the break retarget only knew the 0x0C form; hit.c's reaction-9 test
     * already accepted this shape) */
    return (o->state & 0xFFu) == ST_MOVE && (o->cue_flags & 1u);
}

/* 0x24BC2/0x24C0C — reaction handler 9, the REAL pin break: a hit that
 * lands on the covering man frees the pair. `p` = pinner, `v` = the man
 * he is covering. The referee's cue +0x35 b0 dies here, so SM6 falls to
 * SM1 and wipes the digits exactly as on a kick-out (0x1F9D8/0x1F9E0),
 * unless the count already reached "3" (+0x25 >= 6). */
/* 0x21282 (A0 = the pinned man): the announcer's "It's a two-count!"
 * ($0F1E) when the pin ENDED with the half-count at 4 or 5 - i.e. after a
 * two - then the half-count dies (clr.w +0x108). The engine used to say
 * it inside the count whenever cell 4 was reached, so every pin that got
 * to two announced it, kick-out or not (user 2026-08-26). */
void eng_tag_pin_end_count(eng_state *st, eng_obj *v)
{
    if (!v) return;
    if (v->halfct == 4 || v->halfct == 5) eng_announce(st, 0x0F, 0x1E);
    v->halfct = 0;
}
void eng_pin_break(eng_state *st, eng_obj *p, eng_obj *v)
{
    if (!p || !v)
        return;
    p->role &= (uint16_t)~0x40u;                 /* 0x24C00 */
    v->role &= (uint16_t)~0x40u;                 /* 0x24C06 */
    p->cue_flags &= (uint16_t)~0x01u;                 /* 0x24C0C: the count dies */
    p->pinning = 0;
    p->grap44 = 0;
    p->state = ST_REACT;
    p->react_id = (uint16_t)eng_tag_rule(TAG_BREAK_RC_PINNER);   /* thrown off */
    p->down_t = (uint16_t)eng_tag_rule(TAG_BREAK_PINNER_DOWN_T);
    p->count = 0;                               /* break the FF00 pin hold */
    p->atk = 0;
    p->off_x = 0; p->off_y = 0;                 /* the pin frames' sprite offset (plex +0x28/+0x60,
                                                   cover +0x28...) dies with the pin: the move's own
                                                   release (0x15336 bset #7) would have cleared it, and
                                                   the break skips that - both men drew 60-96 px above
                                                   their feet through the fall and the lying frames
                                                   (user log 2026-08-26, frames 6968-7141). TODO EXACT:
                                                   the ROM's 0x24BC2 path has no such write; the stock
                                                   clear sites are 0x114F0 (stand), 0x11AC2, 0x1A662,
                                                   0xFA44/0xFBEA (scene init) only. */
    v->off_x = 0; v->off_y = 0;
    v->state = ST_REACT;
    v->react_id = (uint16_t)eng_tag_rule(TAG_BREAK_RC_VICTIM);   /* 0x24BF4 */
    v->down_t = (uint16_t)eng_tag_rule(TAG_BREAK_DOWN_T);        /* 0x24BFA */
    v->count = 0;
    v->mash_aa = 0;
    eng_tag_pin_end_count(st, v);               /* 0x21282: 'two-count!' when it ended at 4-5, then clr.w +0x108 */
    p->partner = -1;                            /* 0x24C1C bset #7,(+0x26) */
    v->partner = -1;
    eng_tag_pin_end(st, p, v);                  /* 0x212A0: pads come home */
    eng_ref_digit_wipe();                       /* 0x206FE via SM6 -> SM1 */
    if (eng_dbgsel)
        fprintf(stderr, "pin: BREAK (0x24C0C) pinner o%d react %02X, freed o%d\n",
                (int)(p - st->obj), p->react_id, (int)(v - st->obj));
}

static int32_t apron_x(int side, int32_t y)          /* 0x11796 entry */
{ return side ? ((0xE48 - y) >> 2) : ((y + 0x580) >> 2); }

static int32_t ring_xmin(int32_t y) { return ((y << 8) + 0x40000) / 0x2E0; }
static int32_t ring_xmax(int32_t y) { return -(((y << 8) - 0xA3000) / 0x2E0); }

/* Drive through the normal walk/stand states: state 1 with an angle and
 * the apron speed 0x10 (handler_walk honours it for apron men), state 0
 * when parked. Only rewrite the state when it changes. */
static void apron_walk(eng_obj *o, int moving, unsigned angle, int face_right)
{
    o->facing = (uint16_t)(face_right ? 0x8000u : 0);
    if (moving) {
        o->angle = (uint16_t)angle;
        if ((o->state & 0xFFu) != ST_WALK) o->state = ST_WALK;
    } else if ((o->state & 0xFFu) != ST_STAND) o->state = ST_STAND;
}

void eng_apron_tick(eng_state *st, eng_obj *o)
{
    if (eng_dbgsel && (o->role & RF_PAD)) { static int once;
        if (!once++) fprintf(stderr, "apron: PAD man o%d in the apron machine (sub %02X)\n", (int)(o - st->obj), o->sub); }
    int side = eng_side(o);
    eng_obj *L = eng_team_legal(st, o);        /* the side's IN-RING man (3-man
                                                  teams: the circular +0x86 link
                                                  may be the other apron man) */
    int32_t x = o->x >> 16, y = o->y >> 16;

    if (o->result) return;                               /* match decided: the
                                          0x11598 pose row owns everyone — the
                                          walk-out must not march the entered
                                          winner back to the ropes */
    eng_lazy_divorce(o);                                 /* 0x11828 / 0x11B38 —
                                          first: it drops STALE one-sided links,
                                          which must not freeze the guard below */
    {   /* The machine acts only on a FREE-STANDING man (stock 0x202BA
         * points out state 0/1 men only). Blocking just 5/0x0C/0xFF let a
         * just-THROWN man (state 4, or 2/3 whip-run) fall through into the
         * walk-out from a lying/flying pose — "sprite placement was all
         * wrong, walked off screen above the ropes" (playtest 2026-08-24,
         * timeout during a grapple on the recalled man). He waits until he
         * is back on his feet, then walks. */
        unsigned s = o->state & 0xFFu;
        if ((s != 0 && s != 1) || o->partner >= 0) return;
    }
    /* 0x110E0 (bsr'd from the slam impacts — 0x12A96/0x12CFA/0x12FF2/
     * 0x15192/0x16312/0x164F8/0x16828/0x169D4/0x16C12/0x16D50/0x16F4A/
     * 0x17124/0x17244/0x173CE/0x17564/0x17C8C): the slammed man's partner
     * in state 1 sub 1 gets +0x34 b5; the apron machine (0x11870) then
     * holds the "grabs his head" pose 0x1D3 for +0x46 = 0x20 frames
     * (0x1187E/0x1189A) and resumes (0x11890). Not in the ring-out scene
     * (0x110E4 btst #1,$1C0161). ENGINE: an edge detector on the
     * teammate's slam react stands in for the 16 call sites (TODO EXACT
     * the exact impact frames); the pose fires for human and autopilot
     * apron men alike. */
    if (!(st->g161 & 1u)) {
        uint8_t tok = (uint8_t)((L && (L->state & 0xFFu) == ST_REACT
                                 && (L->react_id & 0xFFu) == RC_BOUNCE) ? 1 : 0);
        if (tok && !o->worry_seen && o->apron && (o->sub & 0x7Fu) == 1
            && !(st->g161 & 2u))
            o->worry_t = 0x20;                            /* 0x1187E +0x46 := 0x20 */
        o->worry_seen = tok;
    }
    if (o->worry_t) {                                     /* 0x11884-0x118B0 */
        if (--o->worry_t) {
            o->spr_force = (uint16_t)(0x1D3u | (o->facing & 0x8000u));   /* 0x1189A */
            if ((o->state & 0xFFu) != ST_STAND) o->state = ST_STAND;    /* clr.b (+0x01): stands */
            o->x = apron_x(side, y) << 16; o->z = 0x140 << 16;
            return;
        }
        o->spr_force = 0;                                 /* 0x11890 +0x04 := +0x2E */
    }
    if (eng_ai_rescue_tick(st, o)) return;                /* 0x1D526 run-in fired (ai.c) */
    if (o->apron && ((o->driver & DRV_AUTOPILOT) || o->input < 0 || !(o->role & RF_PAD))
        && eng_ai_apron_attack(st, o)) return;              /* 0x1D428: the apron man's own attack (AI men) */
    if (!o->apron) {                                      /* sub 4: walk out 0x11ABA */
        int at_rope = side ? (x >= ring_xmax(y) - 2) : (x <= ring_xmin(y) + 2);
        apron_walk(o, !at_rope, side ? 0x40u : 0xC0u, side == 1);   /* 0x11B00:
                                          facing = side (walks toward own rope) */
        if (at_rope) {                                    /* 0x4F climb out */
            o->state = ST_MOVE; o->move_id = 0x4F; o->grap44 = 0;
        }
        return;
    }
    o->st_flags |= SF_APRON;                                      /* 0x1179E bset #0,(+0x32): EVERY apron
                                          man is "outside" (was the pad branch only —
                                          0xFD3E cleared the autopilot partner's +0xC8
                                          every frame, so no team ever earned REGAIN
                                          POWER; user 2026-08-30) */
    if (--o->regen_t == 0) {                              /* 0x94-frame regen */
        o->regen_t = 0x94;
        if (o->hp < o->hp_max) { o->hp++; o->hp_delta++; }
    }
    /* 0x11834 (sub 1) / 0x1197A (sub 2): the apron frames count +0xC8 while
     * not on fire; once 0xFD64 armed REGAIN POWER (+0xC6 b6): the post man
     * (sub 2, 0x11988) lights up on the spot, the follow man (sub 1,
     * 0x11840) switches to sub 3 = walk to y 0x158 and flex (move 0x71). */
    if (!(o->role & RF_ONFIRE)) o->cmb_c8++;
    if ((o->cmb_c6 & 0x40u) && !(st->g161 & 1u) && (o->sub & 0x7Fu) == 2) {   /* 0x11988 post man lights up */
        o->cmb_c6 &= (uint8_t)~0x40u; o->role |= RF_ONFIRE; o->sub = 1;
    } else if ((o->sub & 0x7Fu) == 3 || ((o->cmb_c6 & 0x40u) && !(st->g161 & 1u))) {
        /* 0x11A20 sub 3: walk to y 0x158 (mid-apron); there, REGAIN POWER armed
         * -> the flex 0x71 (0x11A9E), else back to the follow (0x11AAC). Entered
         * by the tag's climb-out (0x18882) and by the follow's arm (0x11848). */
        int at = y >= 0x158 - 1 && y <= 0x158 + 1;
        o->sub = (uint8_t)((o->sub & 0x80u) | 3u);
        o->x = apron_x(side, y) << 16; o->z = 0x140 << 16;
        if (!at) { o->speed = 0x10; apron_walk(o, 1, y < 0x158 ? 0x00u : 0x80u, side == 0); return; }
        if ((o->cmb_c6 & 0x40u) && !(st->g161 & 1u)) {
            o->state = ST_MOVE; o->move_id = 0x71; o->grap44 = 0;                /* 0x11A9E: the flex */
            if (eng_dbgsel) fprintf(stderr, "comeback: o%d REGAIN POWER flex (0x71)\n", (int)(o - st->obj));
            return;
        }
        o->sub = 1;                                                        /* 0x11AAC/0x11AB2 */
    }
    if ((o->role & RF_PAD) && o->input >= 0 && !(o->driver & DRV_AUTOPILOT)) {
        /* HUMAN apron man (a bought-in partner, 0x6E9A): the stick walks
         * him along the rail — 0xF2F6 (state-1 input sub 1): U held ->
         * +0x2C := 0x0008, D -> 0x0088, mirrored by the side (not.b
         * +0x2D); the machine clamps y to [0x120, 0x192] (0x118CE /
         * 0x118E0) and stands him otherwise (0x118EE). ENGINE: vertical
         * angles 0x00/0x80 — the rail x is re-pinned from y every tick,
         * so the 8-step slant of the stock bytes is already in apron_x
         * (TODO EXACT 0x08/0x88). His attacks are eng_apron_press(). */
        int32_t ty = y;
        o->st_flags |= SF_APRON;                                  /* 0x1179E bset #0,(+0x32) */
        if ((o->joy & 4u) && !(o->joy & 8u)) ty = 0x192;  /* 0xF2F6 btst #2,(+0xA9) */
        else if ((o->joy & 8u) && !(o->joy & 4u)) ty = 0x120;   /* 0xF2FE btst #3 */
        o->sub = 1;                                       /* stays the follow sub */
        o->x = apron_x(side, y) << 16; o->z = 0x140 << 16;
        apron_walk(o, ty != y, ty < y ? 0x80u : 0x00u, side == 0);
        return;
    }
    {
        int in_zone = L && ((side == 0) ? ((L->x >> 16) < 0x220 && (L->y >> 16) >= 0x186)
                                        : ((L->x >> 16) >= 0x2F0 && (L->y >> 16) < 0x12E));
        int32_t ty;
        if (in_zone) o->sub = (uint8_t)((o->sub & 0x80u) | 2u);
        else o->sub = 1;
        if ((o->sub & 0x7Fu) == 2) ty = side ? 0x130 : 0x182;            /* post */
        else {                                                            /* follow */
            ty = L ? (L->y >> 16) : y;
            if (ty < 0x120) ty = 0x120; else if (ty > 0x192) ty = 0x192;
        }
        {
            /* hysteresis: start walking at >= 4 px off, keep walking until
             * on the line — no walk/stand flicker on 1-px targets */
            int was = (o->state & 0xFFu) == ST_WALK;
            int moving = was ? (y != ty) : ((y < ty - 3) || (y > ty + 3));
            o->x = apron_x(side, y) << 16; o->z = 0x140 << 16;     /* re-pin */
            if (!moving && (o->sub & 0x7Fu) == 2) o->sub |= 0x80u;  /* tag-ready */
            else if (moving) o->sub &= 0x7Fu;
            apron_walk(o, moving, y < ty ? 0x00u : 0x80u, side == 0);
        }
    }
}

/* 0xE496 category 0x13 — the APRON attacks. The selector's cat-0x13 row
 * (0xE49A own state 1, 0xE4A4 +0xAF == 1 apron follow) links the opp
 * (0xE4B2 bsr 0xF178) and reads ws_move_map[id][0x13*3 + col]: every
 * wrestler's row is {B1 0x39, B2 0x1C, both 0x1C} — B1 the apron PUNCH
 * (move 0x39: record 0x17B5C mode 1, spr 0x1E2-0x1E4, hit record 0x19
 * on cell 2), B2 the over-the-ropes GRAB (move 0x1C with +0x32 b0: no
 * 0x0A hit record, whiff back to the apron — anim.c handler_behind1C),
 * which HOLDS the victim in 0x52 for the legal partner to work. */
void eng_apron_press(eng_state *st, eng_obj *o)
{
    unsigned s = o->state & 0xFFu;
    unsigned col;
    uint8_t entry;
    if (s != 0 && s != 1) return;                         /* 0xE49A (+0x21 == 1; the
                                                             engine parks at state 0) */
    if (!(o->btn_new & 3u)) return;
    if (o->opp < 0 || !st->obj[o->opp].active) return;    /* 0xE496 A1 = (+0x7A) */
    col = ((o->btn_held & 3u) == 3u) ? 2u : (o->btn_new & 1u) ? 0u : 1u;
    {
        entry = (uint8_t)eng_ws_move8((unsigned)o->wrestler, 0x13u * 3u + col);   /* 0xDF96 row fetch (package override aware) */
    }
    if (entry == 0xFFu) return;
    o->partner = o->opp;                                  /* 0xE4B2 bsr 0xF178 */
    if (st->obj[o->opp].partner < 0)                      /* 0xF178 links BOTH ways when
                                                             the victim is free — without
                                                             it eng_lazy_divorce drops the
                                                             one-sided link next tick */
        st->obj[o->opp].partner = (int)(o - st->obj);
    o->st_flags |= SF_APRON;                                      /* 0x1179E: the handlers'
                                                             apron branches key on it */
    o->state = ST_MOVE; o->move_id = entry; o->grap44 = 0;
    o->spr_force = 0;
    if (eng_dbgsel)
        fprintf(stderr, "apron: P%d cat 13 col %u -> move %02X on o%d (vp=%d dx=%d dy=%d)\n",
                (int)(o - st->obj) + 1, col, entry, o->opp, st->obj[o->opp].partner,
                (int)((st->obj[o->opp].x - o->x) >> 16), (int)((st->obj[o->opp].y - o->y) >> 16));
}

/* SWAP 0x188BC + 0x21978: legal flag, inside/outside, input routing,
 * autopilot bits; opp links re-pointed at the new legal men. */
void eng_tag_swap(eng_state *st, eng_obj *out, eng_obj *in)
{
    out->role &= (uint16_t)~0x01u; in->role |= RF_LEGAL;
    if (!out->cpu) {                                      /* 0x188DE: pad/port move is
                                          HUMAN-team only — a CPU swap (b7) keeps both
                                          men padless (was unconditional: a CPU tag
                                          minted a pad-reader, and the next cover
                                          "handed the pad" with no restore ever) */
        if (!(in->role & RF_PAD)) {                         /* 0x188E6 */
            in->input = out->input; out->input = -1;      /* 0x188EE +0x8A swap */
            in->role |= RF_PAD; out->role &= (uint16_t)~0x02u;   /* 0x188FE */
        }
    }
    in->driver &= (uint16_t)~0x40u; out->driver |= DRV_AUTOPILOT;       /* 0x1890A / 0x18918 */
    if (!(in->role & RF_PAD)) in->cpu = out->cpu;           /* a pad man never becomes CPU:
                                          stock's +0x56 b7 stays where it was (0x188BC
                                          moves only b6) — a bought-in human tagging in
                                          from a CPU legal man keeps his pad (0x6E9A) */
    out->cpu = 0;
    out->apron = 1;                                       /* SWAP 0x188D2: the
                                          outgoing man is OUTSIDE from here —
                                          his 0x4C tail cells (66/67) are him
                                          climbing out at the corner */
    if (eng_dbgsel) fprintf(stderr, "tag: swap out o%d in o%d\n", (int)(out - st->obj), (int)(in - st->obj));
    out->sub = 3; out->regen_t = 0x94;                    /* 0x18882: the tag's climb-out ender sends him
                                          to sub 3 = walk to y 0x158 (mid-apron) and only then back to the
                                          follow (0x11AAC) - stock's tag debounce (user 2026-08-30) */
    in->apron = 0; in->sub = 0;
    in->st_flags &= (uint16_t)~0x01u; in->spr_force = 0; in->worry_t = 0;
    in->role &= (uint16_t)~0x04u;    /* engine guard: no stuck OUTSIDE bit on
                                       the man stepping in (see 0x4D ender) */
    in->band = in->hp <= 0x18 ? 2 : in->hp <= (uint16_t)(2 * in->hp_max / 3) ? 1 : 0;
    for (int i = 0; i < ENG_MAX_OBJS; i++) {              /* re-point +0x7A */
        eng_obj *q = &st->obj[i];
        if (!q->active) continue;
        if (q->opp == (int)(out - st->obj)) q->opp = (int)(in - st->obj);
    }
    in->opp = out->opp; in->partner = -1; out->partner = -1;
}

/* 0x21424 — the double-team dive LANDS (topdive mode 0). A0 = the diver:
 * his teammate (the buckle HOLDER) goes to the usher slot ($1C1684/8,
 * engine: f34 b0 + the usher grace) — and if he is not a pad man he gets
 * autopilot + targets the enemy partner; the VICTIM's apron partner is
 * armed to run in (+0xE6 = 0x214BA[band], autopilot, +0x7A = the holder:
 * the retaliation that breaks the pair up); the diver's own autopilot
 * CLEARS — the pad comes back at the landing (0x2149E). */
void eng_dt_land(eng_state *st, eng_obj *diver)
{
    eng_obj *holder = diver->teammate >= 0 ? &st->obj[diver->teammate] : 0;
    diver->driver &= (uint16_t)~0x40u;                    /* 0x2149E */
    if (holder) {
        holder->tag_flags |= TF_USHER_A;                          /* 0x21442 + $1C1684/8 slot */
        st->usher_t = (uint16_t)eng_tag_rule(TAG_USHER_ARM);
        holder->ai_runin_t = 1;                        /* engine: the usher's intruder marker */
        if (holder->partner >= 0) {
            eng_obj *v = &st->obj[holder->partner];
            eng_obj *ep = v->teammate >= 0 ? &st->obj[v->teammate] : 0;
            if (ep) {                                  /* 0x21448-0x21486 enemy partner */
                ep->tag_flags |= TF_USHER_A;
                ep->ai_e6 = (uint16_t)tbl16(TBL(dt_retaliate_arm),
                                            (ep->band < 2 ? ep->band : 2u) * 2u);
                ep->st_flags |= SF_RUNIN_MARK;                      /* 0x214A4 bset #2,+0x32 */
                ep->driver |= DRV_AUTOPILOT;                      /* 0x214AA autopilot */
                ep->opp = (int)(holder - st->obj);     /* 0x214B0 runs in at the holder */
            }
            if (!(holder->role & RF_PAD)) {              /* 0x2148C: CPU holder fights back */
                holder->driver |= DRV_AUTOPILOT;
                if (ep) holder->opp = (int)(ep - st->obj);   /* 0x2149A */
            }
        }
    }
}

/* 0xDD02: A/B, inside, partner waiting at the post, in the corner zone.
 * Returns 1 = plain tag (state 0/1 -> move 0x4C, 0xDDAC); 2 = TAG WHILE
 * HOLDING (state 5 stance 0x15/0x16 -> move 0x66, 0xDD8A..0xDD9E — the
 * double team). */
int eng_tag_trigger(eng_state *st, eng_obj *o)
{
    eng_obj *P = (o->teammate >= 0) ? &st->obj[o->teammate] : 0;
    int side = eng_side(o);
    unsigned state = o->state & 0xFFu, mv = o->move_id & 0xFFu;
    if (!P || !(o->role & RF_LEGAL) || o->apron) return 0;
    if (!(P->apron && (P->sub & 0x80u) && (P->sub & 0x7Fu) == 2)) {
        if (eng_dbgsel && (side == 0 ? ((o->x >> 16) < 0x220 && (o->y >> 16) >= 0x186)
                                     : ((o->x >> 16) >= 0x2F0 && (o->y >> 16) < 0x12E))) {
            static int last = -1; int k = (P->apron << 8) | P->sub;
            if (k != last) { last = k; fprintf(stderr, "tag: trigger blocked - partner apron %d sub %02X\n", P->apron, P->sub); }
        }
        return 0;
    }
    if (side == 0 ? !((o->x >> 16) < 0x220 && (o->y >> 16) >= 0x186)
                  : !((o->x >> 16) >= 0x2F0 && (o->y >> 16) < 0x12E)) return 0;
    if (state == ST_MOVE && (mv == 0x15 || mv == 0x16)) return 2;   /* 0xDD80-0xDD9A */
    if (state != ST_STAND && state != ST_WALK) return 0;
    return 1;
}

/* ================================================================== *
 * PIN RUN-IN — both partners are armed by a cover (pin-partner.md).  *
 *   0x133E2  jsr 0x215B6  (A0 = the PINNER)  -> arms HIS OWN partner *
 *   0x133EA  exg A0,A2 ; jsr 0x21732 (A0 = the VICTIM) -> arms the   *
 *            other team's partner.                                   *
 * Both write +0x34 b0 (armed) / clear b1 (recall) and seed +0xE6; a  *
 * cover puts 1 in +0xE6 on BOTH sides (0x2164E / 0x2179C), so both   *
 * men come in on the next frame.                                     *
 * ================================================================== */

/* 0x21612-0x21624: the pad moves from the legal man to his partner.
 * The ROM leaves the +0x8A port pointer alone and lets f33 b1 (+ f56 b6)
 * decide who the input pass feeds; the engine's `input` index is that
 * pointer, so it travels with the flag. */
static void hand_pad(eng_obj *out, eng_obj *in)
{
    out->role &= (uint16_t)~0x02u;               /* 0x21618 */
    in->role |= RF_PAD;                           /* 0x2161E */
    in->cue_flags |= CF_CHIP_CHANGE;                           /* 0x21624 */
    if (out->input >= 0) { in->input = out->input; out->input = -1; }
    if (!out->cpu) out->driver |= DRV_AUTOPILOT;            /* engine invariant: a human-team man
                                                    with no pad drives himself (0x1890A shape) -
                                                    the pinned man's pad went to his partner and
                                                    the padless legal man then stood at ringside
                                                    until the count-out (user 2026-08-29) */
    if ((out->state & 0xFFu) == ST_RUN) out->state = ST_SKID;   /* the man LOSING the pad mid-run skids out
                                                        (the usher-recall shape, 0xF3C6/0xF3D4): a
                                                        human run only ends on the stick, and the CPU
                                                        never releases one - he ran forever (user
                                                        2026-08-26). TODO EXACT: stock never hands the
                                                        pad to a running man */
    if (eng_dbgsel)
        fprintf(stderr, "tag: pad -> partner (0x215B6)\n");
}

/* ENGINE: a partner in move 0x4F is CLIMBING OUT - his +0x32 b0 is still
 * clear (0x1179E sets it once he stands on the apron), so the ROM's inside
 * test would hand him the pad on his way out: he finished the climb
 * stranded on the apron holding the player's pad while the pinner went
 * autopilot ("my tag partner stayed outside", user 2026-08-30).  He takes
 * the apron branch instead: armed with the +0xE6 countdown, he runs back
 * in (with the pad) once the climb lands.  TODO EXACT: stock takes the
 * inside branch here. */
static int partner_inside(const eng_obj *p)
{
    return !p->apron && !((p->state & 0xFFu) == ST_MOVE && p->move_id == 0x4F);
}

void eng_tag_hand_pad(eng_obj *out, eng_obj *in) { hand_pad(out, in); }

/* 0x215B6 (A0 = A, his own partner) — `link` is the +0x7A/+0x7E target
 * the ROM hands the rescuer (0x21642/0x216A8: the other pair's man). */
static void arm_own_partner(eng_state *st, eng_obj *a, int link, int fast)
{
    eng_obj *p;
    if (!(a->role & RF_LEGAL) || a->apron || a->teammate < 0)
        return;                                 /* 0x215D0/0x215D8 */
    p = &st->obj[a->teammate];
    if (!p->active)
        return;
    if (partner_inside(p)) {                    /* 0x215EE: already inside */
        p->tag_flags |= TF_USHER_A; p->tag_flags &= (uint16_t)~0x02u;
        st->usher_t = (uint16_t)eng_tag_rule(TAG_USHER_ARM);      /* 0x215FA */
        if (!(p->role & RF_LEGAL) && !p->ai_runin_t)
            p->ai_runin_t = 1;                  /* engine intruder marker (stock keeps
                                          him in the $1C1684/8 usher slots and the
                                          0x215F4 bclr cancels his recall): without
                                          it a re-armed inside/mid-recall partner
                                          fell out of the run-in gate and was walked
                                          out with the fresh grace still running */
        if (p->role & RF_PAD) return;             /* 0x21602 */
        if (a->role & RF_PAD) { p->driver &= (uint16_t)~0x40u; hand_pad(a, p); }  /* 0x21612 */
        else if (!(p->weapon_w & WPN_HELD)) {          /* 0x21630 / 0x20F92: a partner
                                                    carrying a weapon is not armed */
            p->ai_b5 |= 0x80u; if (link >= 0) p->opp = link;                 /* 0x21638 */
        }
        return;
    }
    /* 0x2164E: partner is on the apron — +0xE6 = 1 for the pin moves
     * {0x48,0x23,0x1A,0x0E}, else the band table 0x2172C. */
    p->ai_e6 = (uint16_t)(fast ? eng_tag_rule(TAG_PIN_ARM_DELAY)
                               : (int)tbl16(TBL(rescue_arm_own_partner), (p->band < 2 ? p->band : 2u) * 2u));
    p->tag_flags |= TF_USHER_A; p->tag_flags &= (uint16_t)~0x02u;                  /* 0x21688 */
    if (p->role & RF_PAD) {                                         /* 0x216B8 */
        if (link >= 0) p->opp = link;
        p->tag_flags |= TF_USHER_C;
        return;
    }
    if (a->role & RF_PAD) {                                         /* 0x216A4 */
        if (link >= 0) p->opp = link;
        p->tag_flags |= TF_USHER_C;
        hand_pad(a, p);
        return;
    }
    p->tag_flags |= TF_USHER_C; p->ai_b5 |= 0x80u;                           /* 0x216CC */
    if (link >= 0) p->opp = link;
}

/* 0x21732 (A0 = the pin VICTIM): arm his partner. No pad juggling on
 * this side — the ROM never moves f33 b1 here. */
static void arm_victim_partner(eng_state *st, eng_obj *v, int link, int fast)
{
    eng_obj *p;
    if (!(v->role & RF_LEGAL) || v->teammate < 0)
        return;                                 /* 0x2174C */
    p = &st->obj[v->teammate];
    if (!p->active)
        return;
    p->tag_flags |= TF_USHER_A; p->tag_flags &= (uint16_t)~0x02u;                  /* 0x2175A */
    if (partner_inside(p)) {                                      /* 0x21766 */
        st->usher_t = (uint16_t)eng_tag_rule(TAG_USHER_ARM);      /* 0x2176E */
        if (!(p->role & RF_LEGAL) && !p->ai_runin_t)
            p->ai_runin_t = 1;                                    /* see arm_own_partner */
        if (p->role & RF_PAD) return;
        if (v->role & RF_PAD) {          /* ENGINE (user 2026-08-28): the PINNED man's pad
                                          follows to his IN-RING partner — same 0x21612
                                          shape as the pinner side, so the human drives
                                          the rescuer instead of watching the CPU brawl.
                                          (Stock never moves the pad on the victim side;
                                          delete this block to revert. The apron case
                                          keeps the pad for the kick-out mash.) */
            p->driver &= (uint16_t)~0x40u;
            if (link >= 0) p->opp = link;
            hand_pad(v, p);
            return;
        }
        if (p->weapon_w & WPN_HELD) return;                             /* 0x2177E weapon gate */
        p->ai_b5 |= 0x80u;                                        /* 0x21786 */
        if (link >= 0) p->opp = link;
        return;
    }
    p->ai_e6 = (uint16_t)(fast ? eng_tag_rule(TAG_PIN_ARM_DELAY)
                               : (int)tbl16(TBL(rescue_arm_victim_partner), (p->band < 2 ? p->band : 2u) * 2u));
    if (p->role & RF_PAD) { if (link >= 0) p->opp = link; p->tag_flags |= TF_USHER_C; return; }  /* 0x217CE */
    p->tag_flags |= TF_USHER_C; p->ai_b5 |= 0x80u;                           /* 0x217E4 */
    if (link >= 0) p->opp = link;
}

/* Called from the cover catch (0x133E2 + 0x133EA). */
/* 0x215B6 alone — the holds (Slaughter's rack 0x17F78 etc.) arm only the
 * HOLDER's own partner; fast=0 takes the 0x2172C band table. The link the
 * partner targets is the victim (TODO EXACT: the ROM's internal link). */
void eng_tag_arm_hold(eng_state *st, eng_obj *p, eng_obj *v)
{
    if (!p || !v) return;
    if (!eng_mode_rule(MODE_RUNIN)) return;     /* mode: partners never run in */
    if (st->g161 & 1u) { eng_rumble_arm_helper(st, p, v); return; }
    arm_own_partner(st, p, (int)(v - st->obj), 0);
}

void eng_tag_arm_pin(eng_state *st, eng_obj *p, eng_obj *v)
{
    if (!p || !v) return;
    if (!eng_mode_rule(MODE_RUNIN)) return;     /* mode: partners never run in */
    if (st->g161 & 1u) { eng_rumble_arm_helper(st, p, v); return; }   /* 0x215C4 -> 0x216E8 (rumble) */
    arm_own_partner(st, p, v->teammate, 1);     /* 0x215B6: move 0x48 -> +0xE6 = 1 */
    arm_victim_partner(st, v, (int)(p - st->obj), 1);  /* 0x21732: move 0x4A -> 1 */
    if (eng_dbgsel)
        fprintf(stderr, "tag: pin armed both sides (o%d e6=%u / o%d e6=%u)\n",
                p->teammate, p->teammate >= 0 ? st->obj[p->teammate].ai_e6 : 0,
                v->teammate, v->teammate >= 0 ? st->obj[v->teammate].ai_e6 : 0);
}

/* 0x215B6 from a NON-pin hold (the behind grab 0x1C at 0x15772): the
 * holder's own apron partner is armed with the band countdown 0x2172C
 * (0x21674), not the pin's +0xE6 = 1. Rumble: 0x216E8 only arms for the
 * 0x48 cover, so nothing here. */
void eng_tag_arm_holder(eng_state *st, eng_obj *h, eng_obj *v)
{
    if (!h || !v || (st->g161 & 1u)) return;
    arm_own_partner(st, h, v->teammate, 0);
}

/* 0x20F04 (A0 = the 0x1D holder, called on every FE loop): the holder's
 * own partner is sent in to work the held man (+0x7E = the victim).
 * Inside (0x20F50): usher grace, then +0xB5 b7 unless he holds the pad
 * (0x20F6C: the pad hand-over 0x20F7A instead) or a weapon (0x20F92).
 * On the apron (0x20F3C): nothing while he is within 0x50 px of the
 * victim, else the 0x21038 {1,1,1} countdown (0x20FAC) with the same
 * pad / rescue split (0x20FCC..0x21004). Same shape as 0x215B6's pin
 * arm with +0xE6 = 1 (arm_own_partner fast), link = the victim.
 * TODO EXACT: rumble branch 0x21010 (0x2187E helper pick, +0x7E pair). */
void eng_tag_arm_behind_holder(eng_state *st, eng_obj *h, eng_obj *v)
{
    eng_obj *p;
    if (!h || !v || h->teammate < 0 || (st->g161 & 1u)) return;
    if (!(h->role & RF_LEGAL)) return;                          /* 0x20F1E */
    p = &st->obj[h->teammate];
    if (!p->active) return;
    if (p->apron && abs((int)(v->x >> 16) - (int)(p->x >> 16)) < 0x50)
        return;                                             /* 0x20F3C-0x20F4E */
    arm_own_partner(st, h, (int)(v - st->obj), 1);
}

/* 0x2103E (mv 0x52, A0 = the man grabbed from behind by 0x1C, once at his
 * init) / 0x21114 (mv 0x53, the 0x1D victim, once at his first loop): the
 * victim's own partner is armed to come and break it (+0x7E = the holder).
 * 0x2103E: nothing while the HOLDER is outside (0x2105C) or self is not
 * legal; partner inside -> usher grace + rescue mode unless pad/weapon
 * (0x21086..0x210AA); on the apron -> 0x2110E countdown (0x210B4).
 * 0x21114: keyed on the HOLDER's partner instead (0x21148): outside and
 * within 0x50 px of self -> nothing; outside and far -> the 0x2127C
 * countdown (0x2119E); inside -> usher grace + rescue mode unless pad /
 * weapon (0x21168..0x21198). The apron branches share arm_victim_partner's
 * shape (f34 b0/~b1/b2, +0xB5 b7 unless f33 b1, link). TODO EXACT: rumble
 * branches 0x210EE / 0x2125C. */
void eng_tag_arm_behind_victim(eng_state *st, eng_obj *v, eng_obj *h, unsigned mv)
{
    eng_obj *p, *hp;
    int link, countdown;
    if (!v || !h || v->teammate < 0 || (st->g161 & 1u)) return;
    if (!(v->role & RF_LEGAL)) return;                          /* 0x21064 / 0x2112E */
    p = &st->obj[v->teammate];
    if (!p->active) return;
    link = (int)(h - st->obj);
    if (mv == 0x52) {
        if (h->st_flags & SF_APRON) return;                         /* 0x2105C: holder outside */
        countdown = p->apron;                               /* 0x2107E */
    } else {
        hp = h->teammate >= 0 ? &st->obj[h->teammate] : 0;  /* 0x21144 */
        if (hp && hp->active && (hp->apron || (hp->st_flags & SF_APRON))) {
            if (abs((int)(v->x >> 16) - (int)(hp->x >> 16)) < 0x50)
                return;                                     /* 0x21150-0x21166 */
            countdown = 1;                                  /* 0x2119E */
        } else
            countdown = 0;                                  /* 0x21168 */
    }
    p->tag_flags |= TF_USHER_A; p->tag_flags &= (uint16_t)~0x02u;            /* 0x21072 / 0x21168 / 0x211B2 */
    if (!countdown) {
        st->usher_t = (uint16_t)eng_tag_rule(TAG_USHER_ARM);  /* 0x21086 / 0x21174 */
        if (p->role & RF_PAD) return;                         /* holds the pad */
        if (p->weapon_w & WPN_HELD) return;                       /* 0x21096 / 0x21184 weapon gate */
        p->ai_b5 |= 0x80u; p->opp = link;                   /* 0x2109E / 0x2118C (+0x7E) */
        return;
    }
    p->ai_e6 = (uint16_t)tbl16(mv == 0x52 ? TBL(behind_arm_victim52_partner)
                                          : TBL(behind_arm_victim53_partner),
                               (p->band < 2 ? p->band : 2u) * 2u);
    p->tag_flags |= TF_USHER_C;
    if (!(p->role & RF_PAD)) p->ai_b5 |= 0x80u;               /* 0x210D0 / 0x211CC */
    p->opp = link;                                          /* +0x7E / +0x7A */
}

/* 0x1F760 — the post-tag DOUBLE-TEAM window (A0 = the man tagging out):
 * tag mode, the opponent (+0x7A) lying knocked down (state 4 react 8)
 * inside the tagger's own corner zone:
 *   side 1: y < 0x160, x >= 0x298 (facing left) / 0x2D0 <= x < 0x300
 *   side 0: y >= 0x170, 0x1F8 <= x < 0x248 (facing left) / x < 0x26A
 * 1 = the window is open (the ROM returns 0 for "open"). */
int eng_tag_dt_window(const eng_state *st, const eng_obj *o)
{
    const eng_obj *v;
    int vx, vy;
    if (o->opp < 0 || (st->g161 & 1u)) return 0;
    v = &st->obj[o->opp];
    if (!v->active || (v->state & 0xFFu) != ST_REACT || (v->react_id & 0xFFu) != RC_LYING) return 0;
    vx = (int)(v->x >> 16); vy = (int)(v->y >> 16);
    if (o->role & RF_SIDE) {                                   /* 0x1F78A side 1 */
        if (vy >= 0x160) return 0;
        return (v->facing & 0x8000u) ? (vx >= 0x2D0 && vx < 0x300) : vx >= 0x298;
    }
    if (vy < 0x170) return 0;                               /* 0x1F7B6 side 0 */
    return (v->facing & 0x8000u) ? vx < 0x26A : (vx >= 0x1F8 && vx < 0x248);
}

/* 0x214C0 — start the hold-for-partner double team (A0 = the tagger,
 * already swapped out by 0x188BC but staying INSIDE): he goes on
 * autopilot into move 0x37 (holding the downed man for his partner),
 * takes the usher slot; a HUMAN partner returns 1 (he comes in with the
 * pad via 0x37's helper 0x4E), a CPU partner rolls 0x23378[id][band]:
 * 0 -> the corner dive (state 8 rows 4/7, returns 0), else rescue mode
 * (+0xB5 b7, returns 1). */
int eng_tag_dt_start(eng_state *st, eng_obj *o)
{
    eng_obj *p;
    if (o->teammate < 0 || (st->g161 & 1u)) return 1;
    p = &st->obj[o->teammate];
    o->driver |= DRV_AUTOPILOT; p->driver &= (uint16_t)~0x40u;            /* 0x214EA */
    o->tag_flags |= TF_USHER_A; o->tag_flags &= (uint16_t)~0x02u;            /* 0x2150C usher slot */
    st->usher_t = (uint16_t)eng_tag_rule(TAG_USHER_ARM);
    o->ai_b5 &= (uint8_t)~0x10u;                            /* 0x2151C */
    o->state = ST_MOVE; o->move_id = 0x37; o->grap44 = 0;         /* 0x21518/0x21522 */
    if (p->role & RF_PAD) return 1;                           /* 0x21528 human partner */
    if (o->opp >= 0) {
        const eng_obj *v = &st->obj[o->opp];
        unsigned band = v->band < 2 ? v->band : 2u;
        uint32_t row = tbl32(TBL(double_team_rolls), (uint32_t)eng_ws_base(p->wrestler) * 4u);
        unsigned d100 = (eng_rng() & 0xFFu) >> 1;           /* 0x24CC (one draw) */
        if (d100 < tbl_ra8(row + band * 2u)) {              /* bucket 0: the dive */
            p->grap44 = (uint16_t)((p->role & RF_SIDE) ? 7 : 4);   /* 0x2155A/0x21568 */
            p->opp = o->opp; p->state = ST_CLIMB;                  /* 0x2156E/0x21574 */
            return 0;
        }
    }
    p->ai_b5 |= 0x80u; if (o->opp >= 0) p->opp = o->opp;    /* 0x21582 rescue mode */
    return 1;
}

/* 0x211EC (A0 = the 0x61 held man, at his init): his own partner is armed
 * to come and break it — +0xE6 = 0x2127C[band], f34 b0 (the rest of the
 * routine mirrors 0x210B4's apron branch). */
void eng_tag_arm_victim61(eng_state *st, eng_obj *v, eng_obj *h)
{
    eng_obj *p;
    if (!v || !h || v->teammate < 0 || (st->g161 & 1u)) return;
    p = &st->obj[v->teammate];
    if (!p->active) return;
    p->ai_e6 = (uint16_t)tbl16(TBL(behind_arm_victim53_partner), (p->band < 2 ? p->band : 2u) * 2u);
    p->tag_flags |= TF_USHER_A; p->tag_flags &= (uint16_t)~0x02u; p->tag_flags |= TF_USHER_C;
    if (!(p->role & RF_PAD)) p->ai_b5 |= 0x80u;
    p->opp = (int)(h - st->obj);
}

/* 0x212D4 — the disarm / control restore. A0 must be a LEGAL man. The
 * "neither human-flagged nor CPU" case is exactly the man who handed his
 * pad to his partner at 0x21618: he takes it back and the partner goes
 * on autopilot (f56 b6) for the rest of his time in the ring. */
void eng_tag_restore_control(eng_state *st, eng_obj *o)
{
    eng_obj *p;
    if (!o || !(o->role & RF_LEGAL) || o->teammate < 0)
        return;                                 /* 0x212D4 */
    p = &st->obj[o->teammate];
    if (!p->active)
        return;
    if (o->role & RF_PAD) {                       /* 0x212E2: still holds a pad */
        if (p->role & RF_PAD) p->driver &= (uint16_t)~0x40u;   /* 0x212F2 2P team */
        else p->ai_b5 &= (uint8_t)~0x80u;                 /* 0x212FA */
    } else if (o->cpu) {                        /* 0x21302 */
        p->ai_b5 &= (uint8_t)~0x80u;
    } else {                                    /* 0x2130A: pad comes home */
        o->role |= RF_PAD;
        p->role &= (uint16_t)~0x02u;
        p->cue_flags |= CF_CHIP_HIDE;                        /* 0x21316 */
        p->driver |= DRV_AUTOPILOT;                        /* 0x2131C: CPU takes over */
        if (!o->cpu) o->driver &= (uint16_t)~0x40u;   /* the engine's hand_pad put the giver on autopilot (the ROM
                                                   never sets b6 there); the pad's return must take it off, or
                                                   the AI keeps driving the man the player now holds ("CPU
                                                   controlling both / the wrong character", user 2026-08-29) */
        if (p->input >= 0) { o->input = p->input; p->input = -1; }
        if ((p->state & 0xFFu) == ST_RUN) p->state = ST_SKID;   /* change-back caught him RUNNING:
                                                   skid out so the CPU is not stuck in
                                                   the run (user 2026-08-28) */
        if (!p->apron) {                        /* the CHANGE-UP man is INSIDE:
                                          he keeps the in-ring GRACE and brawls
                                          instead of the apron machine walking
                                          him straight back out ("he climbs
                                          back out immediately", playtest
                                          2026-08-24; user spec V420: the
                                          run-ins brawl until the recall) */
            p->ai_sub = 1;
            if (!p->ai_runin_t) p->ai_runin_t = 1;
            p->tag_flags &= (uint16_t)~0x02u;
        }
        if (eng_dbgsel)
            fprintf(stderr, "tag: pad <- partner (0x2130A), partner on autopilot\n");
    }
    if (p->apron) {                             /* 0x2132A */
        p->tag_flags &= (uint16_t)~0x05u;
        p->ai_e6 = 0;
    }
}

/* 0x212A0: run the restore over both men of the pair, then 0x213A6's
 * "victim freed" clears the rescue arming that is no longer relevant. */
void eng_tag_pin_end(eng_state *st, eng_obj *p, eng_obj *v)
{
    /* 0x21282 precedes 0x212A0 on every pin end: whichever man carried the
       half-count (the pinned one) triggers "It's a two-count!" at 4-5 */
    if (p && (p->halfct == 4 || p->halfct == 5)) eng_tag_pin_end_count(st, p); else if (p) p->halfct = 0;
    if (v && (v->halfct == 4 || v->halfct == 5)) eng_tag_pin_end_count(st, v); else if (v) v->halfct = 0;
    if (p) eng_tag_restore_control(st, p);
    if (v) eng_tag_restore_control(st, v);
    for (int i = 0; i < ENG_MAX_OBJS; i++) {    /* 0x213A6 */
        eng_obj *q = &st->obj[i];
        if (!q->active || !q->apron) continue;
        if (q->tag_flags & TF_USHER_A) { q->tag_flags &= (uint16_t)~0x05u; q->ai_e6 = 0; }
        q->ai_b5 &= (uint8_t)~0x80u;
    }
}

/* Is the pin/hold that armed `o` still running? (the engine's stand-in
 * for the explicit 0x212A0/0x213A6 disarm call sites the moves make) */
int eng_tag_rescue_live(eng_state *st, const eng_obj *o)
{
    const eng_obj *tm;
    if (o->teammate < 0) return 0;
    tm = &st->obj[o->teammate];
    if ((tm->state & 0xFFu) == ST_HOLD && tm->partner >= 0) return 1;   /* my man is pinning/holding */
    if ((tm->state & 0xFFu) == ST_MOVE && tm->partner >= 0) {
        /* my man holds someone in a VICTIM move (behind holds 0x52/0x53,
         * the hold-for-partner 0x61, the submission victims 0x5D-0x64):
         * the hold is live whatever f35 says — the behind holds never set
         * b1, so the 0x20F04 arm of the holder's partner was cancelled one
         * tick after it fired (playtest 2026-08-26: "the tagged-out player
         * is meant to climb in - he doesn't") */
        const eng_obj *v = &st->obj[tm->partner];
        unsigned vm = v->move_id & 0xFFu;
        if (v->active && (v->state & 0xFFu) == ST_MOVE
            && (vm == 0x52 || vm == 0x53 || (vm >= 0x5D && vm <= 0x64))) return 1;
    }
    if ((tm->state & 0xFFu) == ST_MOVE && tm->partner >= 0
        && (tm->cue_flags & CF_HOLD_CUES)) return 1;         /* scripted hold / cover (f35 b0 pin, b1 holder) */
    if ((tm->state & 0xFFu) == ST_MOVE && tm->partner >= 0) {
        unsigned tmv = tm->move_id & 0xFFu;
        if (tmv == 0x61 || tmv == 0x77) return 1;   /* my man is HELD for the buckle double team
                                          (0x37 holder, no f35 bit): the 0x211EC arm must
                                          survive - his partner runs in and breaks it
                                          (user 2026-08-30) */
    }
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        const eng_obj *c = &st->obj[i];
        if (!c->active || c == tm) continue;
        if (c->partner == o->teammate && (c->state & 0xFFu) == ST_HOLD) return 1;
        if (c->partner == o->teammate && (c->state & 0xFFu) == ST_MOVE && (c->cue_flags & CF_HOLD_CUES)) return 1;
    }
    return 0;
}

/* $1C1682 usher grace + the recall it produces. The ROM walks the
 * referee over (SM0 0x1F97E -> SM2 0x1F9F8 -> usher visual 7 0x202BA,
 * which sets f34 b1 on $1C1684/$1C1688); the engine keeps the per-man
 * clock and sets the same bit when it expires. TODO EXACT: the referee
 * never actually walks the escort. */
void eng_tag_usher_tick(eng_state *st)
{
    int intruder = -1;
    /* $1C1682 ticks ONLY at the head of the referee's idle SM5 (0x1F97E):
     * while he counts a pin / watches a hold the grace is frozen — which is
     * exactly why run-in men linger while the action lasts and are pointed
     * out soon after it ends (SM2 escort, referee.c). */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active || !o->ai_runin_t) continue;
        if ((o->role & RF_LEGAL)
            || (o->apron && ((o->tag_flags & TF_RECALL) || (o->state & 0xFFu) != ST_MOVE))) {   /* legal again /
                                          back OUT (0x202F4's f32 b0 test; the state-5 guard
                                          keeps the marker alive through the 0x4E climb-in) */
            o->ai_runin_t = 0; continue;
        }
        if (o->ai_runin_t > 1) o->ai_runin_t--;   /* 1 = "inside, awaiting the usher" marker */
        if (!o->apron && !(o->tag_flags & TF_RECALL) && intruder < 0) intruder = i;
    }
    if (intruder < 0) return;
    if (st->ref.sm != 5) return;                /* 0x1F97E: SM5 only */
    if (st->usher_t) { st->usher_t--; return; }
    st->ref.sm = 2;                             /* 0x1F98E -> $8002, target $1C1684 */
    st->ref.target = intruder;
    st->ref.p23 = 0; st->ref.pose = 0;
    st->usher_t = (uint16_t)eng_tag_rule(TAG_USHER_ARM);   /* re-seed for the next one */
    if (eng_dbgsel) fprintf(stderr, "tag: usher grace expired -> referee escorts o%d\n", intruder);
}
