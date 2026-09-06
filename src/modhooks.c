/* modhooks.c — where the MOD RULES reach into the transcribed engine.
 *
 * The cell handlers (anim.c), the hit pipeline (hit.c) and the pad gates
 * (core.c) are transcriptions of the ROM; a mod that changes what happens
 * used to live as a block spliced into the middle of one of those, which
 * made ordering against the ROM's own init/land/drain steps invisible (the
 * rope exit's arc was silently re-armed by the fall handler's init for a
 * month before anyone saw it, 2026-09-05). Every such block now lives here
 * behind a named hook; the caller says WHEN, this file says WHAT.
 *
 *   hook                     called from                      rules
 *   mod_fall_launch          handler_fall init (anim.c)       throw_out / toprope_out / exit_ring arcs
 *   mod_fall_landed          handler_fall landing             extended_moves (top-rope bit), the arcs' outside landing
 *   mod_hit_scan_extras      eng_hit_scan per swing (hit.c)   extended_moves, toprope_out, ref_knockdown
 *   mod_weapon_landed        eng_hit_scan, a landed swing     weapon_dq
 *   mod_track_pairs          eng_damage_drain, every frame    parasitic_pct (pairing memory)
 *   mod_damage_scale         eng_damage_drain, before the hit human_hit_mult
 *   mod_damage_taken         eng_damage_drain, hp about to drop  parasitic_pct, ko_dq
 *   mod_exit_ring_gate       eng_pad_drive (core.c)           exit_ring*, cage_escape
 *   eng_pin_allowed          the covers (anim.c, referee.c)   pin_anyone, pin_outside
 *   eng_throw_out/arc        the slam releases / motion.c     throw_out
 *   eng_topple / eng_rope_exit  hit.c, core.c                 extended_moves, toprope_out, exit_ring
 *
 * Still inline where they sit (they ARE the state machine's own branches):
 * the AI's "swing at a perched man" branch (ai.c, extended_moves), the
 * dt_stomp / grapple_pick picker rows (core.c), the throw_pend clause of
 * the ring law (motion.c), the tie-up difficulty offset (tieup.c), the
 * rumble entrant rules (rumble.c), and every HUD / camera / chip rule.
 *
 * Contract: a hook never calls a cell handler and never changes o->state
 * on the ROM's behalf except through the documented launches below; the
 * caller stays a faithful transcription with one line per hook. */
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"
extern int eng_dbgsel;

static int self_idx(const eng_obj *o)
{
    eng_state *st = eng_anim_state();
    return st ? (int)(o - st->obj) : -1;
}
static int32_t ring_xmin_of(const eng_obj *o) { return (((o->y >> 16) << 8) + 0x40000) / 0x2E0; }
static int32_t ring_xmax_of(const eng_obj *o) { return -((((o->y >> 16) << 8) - 0xA3000) / 0x2E0); }

/* ---- pins ------------------------------------------------------------- */

/* the legal-pin gate (the squash's 0x18C68-0x18C86: match live, both men legal,
 * attacker not outside) + the pin mods: pin_anyone lifts the legality test,
 * pin_outside the outside test (rumble: everything pins, as before) */
int eng_pin_allowed(const eng_obj *o, const eng_obj *v)
{
    eng_state *st = eng_anim_state();
    if (st && (st->g161 & 1u)) return 1;
    if ((o->role & RF_OUTSIDE) && !eng_mod_rule(MODR_PIN_OUTSIDE)) return 0;
    if (!((o->role & RF_LEGAL) && (v->role & RF_LEGAL)) && !eng_mod_rule(MODR_PIN_ANYONE)) return 0;
    return 1;
}

/* ---- launches: the mods' own airborne arcs ---------------------------- */

/* mod throw_out: the thrown man rides the over-the-ropes arc and lands at
 * ringside wherever the throw happened (the 0x1B4DC arc, outside law) */
void eng_throw_out(eng_obj *v)
{
    eng_state *st = eng_anim_state();
    if (!eng_mod_rule(MODR_THROW_OUT) || (st && ((st->g161 & 1u) || st->scene == 1)) || (v->role & RF_OUTSIDE)) return;
    /* ARMED here, at the release; the slam's own victim reaction carries
     * him to its landing spot, and the first mat contact there fires the
     * arc (motion.c -> eng_throw_arc).  Was: flagged outside at once with
     * the 0x0E launch, so he sank to the outside floor plane while still
     * over the mat = "falls through the ring" (user 2026-08-30). */
    v->throw_pend = 2;
    if (eng_dbgsel) fprintf(stderr, "mod: throw_out - o%d armed (fires at the impact)\n", self_idx(v));
}

/* the over-the-top-rope arc itself (motion.c calls it at the slam's mat
 * contact): the ROM's 0x1B4DC launch (row 0x0E, the press slam AT the
 * rope) stretched so the landing is 0x28 px past the NEAR rope line, the
 * in-ring law holding him until he actually crosses it (throw_pend 1). */
void eng_throw_arc(eng_obj *v)
{
    int32_t x, dist, T, need, avx; int right;
    x = v->x >> 16; right = x >= 0x280;
    v->state = ST_REACT; v->react_id = RC_FALL;                            /* airborne fall (0x1B134 family) */
    v->partner = -1; v->list = 0; v->grap44 = 0;
    v->floor42 = 0; v->clip_h = 0; v->z = 0x140 << 16; v->mover = 2;      /* off the mat itself: the slam's -0x20 floor bias
                                          had the law snap him straight back down */
    v->facing = (uint16_t)(right ? 0 : 0x8000u);                           /* fly away from the ring centre */
    eng_knockback(v, 0x0E);
    v->vz = (int16_t)((int32_t)v->vz * 5 / 4);                            /* a BIGGER bounce than the press slam's
                                          (apex ~150 px, ~67 frames): mid-ring slams fell short
                                          of the rope and bounced inside (user 2026-08-30) */
    dist = right ? ring_xmax_of(v) - x : x - ring_xmin_of(v);
    if (dist < 0) dist = 0;
    T = v->grav ? (2 * (int32_t)v->vz) / v->grav : 1; if (T < 1) T = 1;   /* flight frames */
    need = ((dist + 0x38) << 8) / T;                                       /* 8.8 px/frame, lands 0x38 past the rope */
    avx = v->vx < 0 ? -v->vx : v->vx;
    if (need > avx) v->vx = (int16_t)(right ? need : -need);
    v->throw_pend = 1;
    eng_sound(0x2B);
    if (eng_dbgsel) fprintf(stderr, "mod: throw_out - o%d over the ropes (dist %ld, vx %d, %ld frames)\n", self_idx(v), (long)dist, v->vx, (long)T);
}

/* MOD extended_moves: struck off the buckle — the climber/percher takes
 * the airborne fall from his elevated z, onto the mat. */
void eng_topple(eng_obj *v, int from_right)
{
    v->state = ST_REACT; v->react_id = RC_FALL;     /* airborne fall (0x1B134 family) */
    v->dmg = 6;
    v->list = 0; v->grap44 = 0; v->partner = -1;
    v->hold_t = 0;                     /* the perch's 0x80 climb-down timer is dead once he is off */
    /* ONTO THE MAT (the rule's wording; toprope_out is the out-of-ring
     * variant): face the ropes so handler_fall's row-1 backward pop carries
     * him INTO the ring. Was: faced the ring and flew outward over the rope
     * line at rope height (zone 2, no clamp) - the first frame back under
     * 0x180 the law yanked him 0x40 inside via the look-ahead ("fell in the
     * middle of the ring", user 2026-09-05). */
    v->facing = (uint16_t)(from_right ? 0x8000u : 0);
    eng_knockback(v, 3);
}

/* MOD exit_ring / toprope_out: the self-initiated (or struck) over-the-rope
 * hop — the same knockback(0x0E) + outside-bit pair the throw exit uses at
 * 0x1B4DC, minus the damage. */
void eng_rope_exit(eng_obj *o)
{
    o->state = ST_REACT; o->react_id = RC_FALL;   /* airborne fall (0x1B134 family) */
    o->dmg = 0; o->partner = -1;
    o->clip_h = 0; o->floor42 = 0; o->hold_t = 0;
    eng_knockback(o, 0x0E);          /* the 0x1B4DC over-the-ropes arc */
    o->role |= RF_OUTSIDE;           /* outside law: lands at ringside */
    /* throw_pend 1 = mod_fall_launch KEEPS this arc (handler_fall's own
     * row-1 backward pop replaced it since the mod was born: the man drifted
     * backward INTO the ring while flagged outside, sank to the ringside
     * plane z 0x100 under the mat and stood there unreachable - "fell in
     * the middle of the ring, can't get up, the AI is stuck", user
     * 2026-09-05, exit_ring hop + toprope_out alike), the ropes never push
     * him back (motion.c) and the landing goes through land_outside (0x68
     * lying outside; a backup leaves). */
    o->throw_pend = 1;
}

/* ---- the airborne fall (handler_fall, 0x1B134) ------------------------ */

/* init: 1 = a mod arc is already flying him (eng_throw_arc / eng_rope_exit),
 * the ROM's launcher + edge arc must not be re-armed over it */
int mod_fall_launch(eng_obj *o)
{
    return o->throw_pend == 1;
}

/* landing frame, before the ROM's thud -> bounce. 1 = the landing was taken
 * over (he lands OUTSIDE via eng_land_outside), 0 = carry on as stock. */
int mod_fall_landed(eng_obj *o)
{
    eng_state *st = eng_anim_state();
    if (o->st_flags & SF_TOPROPE) {  /* MOD extended_moves: knocked OFF the buckle and now on the
                                          ground - drop the top-rope bit and free the corner the way
                                          the climb-down does (0x123AC). Clearing it at the hit
                                          instead left him at rope height for good ("fell into a
                                          void, could walk everywhere, couldn't hit anyone", user
                                          2026-08-30); a man legitimately up there never lands
                                          through this handler (perch = state 9, the dive clears
                                          the bit at launch). */
        o->st_flags &= (uint16_t)~SF_TOPROPE;
        if (st) st->corner_bits = 0;
        if (eng_dbgsel) fprintf(stderr, "mod: o%d down from the buckle - top-rope bit cleared\n", self_idx(o));
    }
    if (o->throw_pend == 1) {        /* throw_out mod: the arc lands - outside as the press
                                          slam's 0x1B5F8 does (-> 0x68), inside = the plain bounce */
        o->throw_pend = 0;
        if ((o->role & RF_OUTSIDE) && st && !(st->g161 & 2u)) { eng_land_outside(o); return 1; }
    }
    return 0;
}

/* ---- the hit scan (eng_hit_scan, 0x24062) ----------------------------- */

/* per swing, after the record's own box is fetched: the mods' extra targets */
void mod_hit_scan_extras(eng_state *st, eng_obj *a)
{
    if (eng_mod_rule(MODR_EXT_MOVES)) {
        /* MOD: the swing knocks a CLIMBING/PERCHED man off the buckle
         * (states 8/9/0xA carry no hit record in stock — new
         * capability, user wish list 2026-08-24). Once per swing via
         * the object's own hit_mask bit. */
        for (int j2 = 0; j2 < ENG_MAX_OBJS; j2++) {
            eng_obj *v2 = &st->obj[j2];
            unsigned vs2 = v2->state & 0xFFu;
            int32_t dx2, dy2;
            if (v2 == a || !v2->active || (v2->st_flags & SF_ELIMINATED)) continue;
            if (vs2 != ST_CLIMB && vs2 != ST_PERCH && vs2 != ST_CLIMBDOWN) continue;
            if (a->hit_mask & (0x8000u >> j2)) continue;
            if (((a->role ^ v2->role) & RF_SIDE) == 0) continue;   /* enemies only */
            dx2 = (v2->x >> 16) - (a->x >> 16);
            dy2 = (v2->y >> 16) - (a->y >> 16);
            if (labs(dx2) >= 0x38 || labs(dy2) >= 0x18) continue;
            if ((a->facing & 0x8000u) ? dx2 < 0 : dx2 > 0) continue;   /* in front */
            a->hit_mask |= (uint16_t)(0x8000u >> j2);
            eng_sound(0x2B);
            if (eng_mod_rule(MODR_TOPROPE_OUT)) {          /* mod: off the buckle and OUT of the ring */
                v2->facing = (uint16_t)(((v2->x >> 16) > 0x280) ? 0x8000u : 0);
                v2->state = ST_REACT; v2->react_id = RC_FALL; v2->dmg = 6; v2->list = 0; v2->grap44 = 0; v2->partner = -1;
                eng_rope_exit(v2);
            } else
            eng_topple(v2, (v2->x >> 16) > 0x280);
            if (eng_dbgsel) fprintf(stderr, "mod: o%d toppled off the buckle\n", j2);
        }
    }
    if (eng_mod_rule(MODR_REF_KO) && st->ref.active
        && !(a->hit_mask & 0x0001u)) {
        /* MOD: the swing can floor the REFEREE (no stock analog).
         * Prefilter shape borrowed from 0x2418A/0x2419E, direction
         * gated by the attacker's facing; once per swing (low
         * hit_mask bit — the object bits use 0x8000 >> j). */
        int32_t rdx = (st->ref.x >> 16) - (a->x >> 16);
        int32_t rdy = (st->ref.y >> 16) - (a->y >> 16);
        if (eng_dbgsel && getenv("WF_REFDBG")) fprintf(stderr, "refko: o%d atk %02X rdx %ld rdy %ld fc %c mask %04X sm %d\n", (int)(a - st->obj), a->atk & 0xFF, (long)rdx, (long)rdy, (a->facing & 0x8000u) ? 'R' : 'L', a->hit_mask, st->ref.sm);
        if (labs(rdy) < 0x0C && labs(rdx) < 0x38
            && ((a->facing & 0x8000u) ? rdx > 0 : rdx < 0)) {
            a->hit_mask |= 0x0001u;
            eng_ref_knockdown(st);
        }
    }
}

/* a LANDED weapon swing (records 0x1E/0x1F) */
void mod_weapon_landed(eng_state *st, eng_obj *a)
{
    if (eng_mod_rule(MODR_WEAPON_DQ))
        eng_match_dq(st, a);   /* mod: a LANDED weapon swing DQs */
}

/* ---- damage (eng_damage_drain, 0x24E58) ------------------------------- */

/* every frame: remember who I was last paired with - the newest change of
 * the +0x92 pairing or the partner wins; a throw's damage drains after both
 * are cleared, so the drain could not name the thrower otherwise */
void mod_track_pairs(eng_state *st)
{
    if (eng_mod_rule(MODR_PARASITIC) <= 0 && eng_mod_rule(MODR_HIT_MULT_HUMAN) <= 1) return;
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        uint8_t lp = (uint8_t)(o->last_pair >= 0 && o->last_pair < ENG_MAX_OBJS ? o->last_pair + 1 : 0);
        uint8_t pn = (uint8_t)(o->partner >= 0 && o->partner < ENG_MAX_OBJS ? o->partner + 1 : 0);
        if (!o->active) { o->foe1 = o->foe_lp1 = o->foe_pn1 = 0; continue; }
        if (lp != o->foe_lp1) { o->foe_lp1 = lp; if (lp && lp != i + 1) o->foe1 = lp; }
        if (pn != o->foe_pn1) { o->foe_pn1 = pn; if (pn && pn != i + 1) o->foe1 = pn; }
    }
}

/* pending damage about to be applied: a HUMAN player's hits do n x damage
 * (human_hit_mult) — the pairing memory names who dealt it (was the +0x92
 * pairing alone, which a throw or a tie-up knee never writes: the rule only
 * scaled strikes, mod gate 2026-09-05) */
void mod_damage_scale(eng_state *st, eng_obj *o)
{
    int mult = eng_mod_rule(MODR_HIT_MULT_HUMAN);
    int ai = o->foe1 ? (int)o->foe1 - 1 : o->last_pair;
    if (mult > 1 && ai >= 0 && ai < ENG_MAX_OBJS && &st->obj[ai] != o) {
        const eng_obj *a = &st->obj[ai];
        if (a->active && a->input >= 0 && !a->cpu && !(a->driver & DRV_AUTOPILOT)) {
            unsigned d = (unsigned)o->dmg * (unsigned)(mult > 9 ? 9 : mult);
            o->dmg = (uint16_t)(d > 0xFFFF ? 0xFFFF : d);
        }
    }
}

/* hp is about to go from o->hp to nhp (the gauge delta is already booked) */
void mod_damage_taken(eng_state *st, eng_obj *o, int i, uint16_t nhp)
{
    {   /* MOD parasitic_pct: the energy a hit takes is GIVEN to the
         * hitter, up to his full gauge; his band follows so the AI / HUD
         * see the refill (user 2026-09-05) */
        int pct = eng_mod_rule(MODR_PARASITIC);
        if (pct > 0) {
            int ai = o->foe1 ? (int)o->foe1 - 1 : -1;   /* mod_track_pairs */
            if (ai >= 0 && ai != i) {
                eng_obj *a = &st->obj[ai];
                unsigned gain = (unsigned)(o->hp - nhp) * (unsigned)(pct > 1000 ? 1000 : pct) / 100u;
                if (a->active && a->hp_max && gain) {
                    if (a->hp + gain > a->hp_max) gain = a->hp_max - a->hp;
                    a->hp = (uint16_t)(a->hp + gain);
                    a->hp_delta = (int16_t)(a->hp_delta + (int16_t)gain);
                    a->band = a->hp <= 0x18 ? 2 : a->hp <= (uint16_t)(2 * a->hp_max / 3) ? 1 : 0;
                    if (eng_dbgsel) fprintf(stderr, "mod: parasitic - o%d takes %u from o%d (hp %u/%u)\n", ai, gain, i, a->hp, a->hp_max);
                }
            } else if (eng_dbgsel) fprintf(stderr, "mod: parasitic - o%d hurt by nobody known (pn %d lp %d)\n", i, o->partner, o->last_pair);
        }
    }
    if (nhp == 0 && o->hp != 0 && eng_mod_rule(MODR_KO_DQ) && !o->result) {
        /* MOD ko_dq: energy 0 = OUT (humans too). Rumble: eliminated
         * (f32 b4 -> the 0x1152A stand row starts his walk-off once he
         * is back on his feet); a match: his team loses on the spot. */
        if (st->g161 & 1u) {
            if (!(o->st_flags & SF_ELIMINATED)) { o->st_flags |= SF_ELIMINATED; o->partner = -1;
                if (eng_dbgsel) fprintf(stderr, "mod: o%d KNOCKED OUT - eliminated\n", (int)(o - st->obj)); }
        } else {
            o->hp = nhp;
            eng_match_dq(st, o);
            if (eng_dbgsel) fprintf(stderr, "mod: o%d KNOCKED OUT - his team loses\n", (int)(o - st->obj));
        }
    }
}

/* ---- the cage escape climb (mode cage_escape_win, move 0x8C) ---------- */

#define CAGE_CLIMB_HOLD 115u    /* ~2 s of pushing DOWN into the bottom wall starts it */
#define CAGE_TOP_Z      0x1C8   /* the top of the fence: 0x88 px above the mat */
#define CAGE_CLIMB_STEP 12u     /* frames per hand-over-hand step (pose 277 mirrored each step) */
#define CAGE_TOP_HOLD   14u     /* frames per pose over the top (37 / 33 / 35) */

/* Code-driven (the stand cell, the pose forced every frame, like the perch):
 *   phase 0  climb the inside of the BOTTOM-of-screen wall: y pinned to the wall line
 *            0x118, z 0x140 -> CAGE_TOP_Z at 1 px/frame, pose 277 flipping
 *            every CAGE_CLIMB_STEP frames (user 2026-09-05: "pose 0277
 *            flipping horizontally until they get to the top")
 *   phase 1  over the top: poses 37 (step up), 33 (crouch), 35 (stand
 *            facing away), CAGE_TOP_HOLD frames each
 *   phase 2  the drop: a plain fall outside (state 4 react 3, the straight
 *            launcher row 3, the mod arc flag so handler_fall keeps it), in
 *            front of the fence (list 0); the WIN is booked at the drop like
 *            the old straddle climb did - the ceremony waits for him to land
 *            and stand (0x1AF4C).
 * The law is off while he is on the wall (apron = 1: the cage law clamps y
 * and z at ANY height, it would snap him back to the mat every frame). */
uint32_t mod_cage_climb(eng_obj *o, uint32_t cell)
{
    eng_state *st = eng_anim_state();
    unsigned t;
    if (!(o->anim_sel & 0x8000u)) {                 /* init */
        o->mover = 0; o->list = 2; o->apron = 1;
        o->partner = -1; o->grap44 = 0; o->hold_t = 0; o->atk = 0;
        o->y = 0x118 << 16; o->z = 0x140 << 16;     /* the wall at the BOTTOM of the screen (world y is inverted on screen) */
        o->vx = o->vy = o->vz = 0;
        /* the climbing drawing is pose 277 (hands up the mesh, knee raised) for
         * EVERY wrestler - a universal frame (user 2026-09-06). The ROM has it
         * for ids 4/9/10 only; the others draw it through the class-generic
         * fallback, and today the class-0 generic 277 is still Hawk's own
         * drawing rather than the neutral generic man (data/generics/0/frames/
         * pose_0277.png), which is why the climber looked like LOD: redraw that
         * generic on the neutral body and everyone's climb follows. */
        o->sub = 1;
        eng_sound(0x32);                            /* the buckle-climb sound (0x120FA) */
        return cell;
    }
    t = ++o->hold_t;
    if (o->grap44 == 0) {                           /* phase 0: up the wall */
        unsigned step = (t / CAGE_CLIMB_STEP) & 1u;
        if ((o->z >> 16) < CAGE_TOP_Z) o->z += 1 << 16;
        o->spr_force = o->sub ? (uint16_t)(277u | (step ? 0x8000u : 0))
                              : (uint16_t)((step ? 369u : 368u) | (o->facing & 0x8000u));
        if ((o->z >> 16) >= CAGE_TOP_Z) { o->grap44 = 1; o->hold_t = 0; }
        return cell;
    }
    if (o->grap44 == 1) {                           /* phase 1: over the top */
        unsigned k = (t - 1) / CAGE_TOP_HOLD;
        static const uint16_t top[3] = { 37, 33, 35 };
        if (k < 3) { o->spr_force = (uint16_t)(top[k] | (o->facing & 0x8000u)); return cell; }
        /* phase 2: the drop, outside, and the win */
        o->grap44 = 2; o->apron = 0; o->sub = 0; o->list = 0;
        o->spr_force = 0;
        /* NOT flagged outside: the outside landing (0x68 -> the ring-out
         * moves 0x6A/0x6B) flips the scene word and restarts the referee
         * into a COUNT-OUT on a decided match - "lands, can't move, wedged,
         * and we lose by countout" (user 2026-09-05). A plain fall lands ->
         * bounce -> lying -> get up -> stand at the wall line, drawn in
         * front of the fence (list 0); the ceremony waits for that stand. */
        o->state = ST_REACT; o->react_id = RC_FALL;
        o->dmg = 0; o->partner = -1;
        o->clip_h = 0; o->floor42 = 0;
        eng_knockback(o, 3);                        /* row 3: vx 0 (mover 2 + gravity) ... */
        o->vz = 0;                                  /* ... minus its pop: a plain drop (user: "the drop can just be a fall") */
        o->throw_pend = 1;                          /* handler_fall keeps this launch (mod_fall_launch) */
        if (st) eng_match_escape_win(st, o);
        if (eng_dbgsel) fprintf(stderr, "mod: o%d ESCAPES over the front of the cage\n", self_idx(o));
        return cell;
    }
    return cell;
}

/* ---- pad gates (eng_pad_drive, core.c) -------------------------------- */

/* MOD exit_ring: keep pushing INTO a rope you are touching for
 * exit_ring_hold frames (0x20 = ~0.5s) and you leave the ring: a hop
 * over it (eng_rope_exit = the 0x1B4DC throw exit minus the damage)
 * or, exit_ring_climb, the rumble elimination climb-out (0x7A phases
 * 1-3, grap44 b3 = "mod exit, not eliminated"); exit_ring_ropes masks
 * which ropes count. Tag only, never the cage, never while already
 * outside. Returns 1 when it took the frame (the caller returns). */
int mod_exit_ring_gate(eng_state *st, eng_obj *o)
{
    static uint8_t exit_hold[ENG_MAX_OBJS];
    static uint8_t esc_pend[ENG_MAX_OBJS];
    int k = (int)(o - st->obj);
    int32_t ex, ey, xmin, xmax;
    unsigned m;
    if (!((eng_mod_rule(MODR_EXIT_RING) || (st->scene == 1 && eng_mode_rule(MODE_CAGE_ESCAPE)))
          && !(st->g161 & 1u)
          && !(o->role & RF_OUTSIDE)
          && (st->scene != 1 || eng_mode_rule(MODE_CAGE_ESCAPE))
          && ((o->state & 0xFFu) == ST_WALK)))
        return 0;
    (void)esc_pend;
    ex = o->x >> 16; ey = o->y >> 16;
    xmin = ((ey << 8) + 0x40000) / 0x2E0;      /* the 0x2818E trapezoid */
    xmax = -(((ey << 8) - 0xA3000) / 0x2E0);
    if (getenv("WF_EXDBG") && k == 0)
        fprintf(stderr, "ex: joy=%X st=%02X x=%d xmin=%d xmax=%d y=%d hold=%u sc=%d\n",
                o->joy, o->state & 0xFF, (int)ex, (int)xmin, (int)xmax, (int)ey, exit_hold[k], st->scene);
    {
        unsigned ropes = (unsigned)eng_mod_rule(MODR_EXIT_ROPES);
        if (!ropes) ropes = 0xFu;
        m = ((o->joy & 1u) && ex >= xmax - 4) ? 1u    /* right rope + R */
          : ((o->joy & 2u) && ex <= xmin + 4) ? 2u    /* left rope + L */
          : ((o->joy & 8u) && ey <= 0x11C) ? 4u       /* top rope + stick -y (joy b3 = walk_angle 0x80) */
          : ((o->joy & 4u) && ey >= 0x194) ? 8u : 0u; /* bottom rope + stick +y (joy b2 = angle 0: the
                                                         old U/D reading was swapped - neither rope
                                                         ever fired) */
        m &= ropes;
        if (st->scene == 1 && m != 4u) m = 0;         /* the CAGE is climbed at the BOTTOM wall of the screen
                                                         only (world y 0x118 - the rasteriser inverts y), pushing
                                                         DOWN into it (user 2026-09-05: "climb the bottom wall by
                                                         pressing down") */
    }
    if (m) {
        unsigned need = (st->scene == 1) ? CAGE_CLIMB_HOLD
                      : (unsigned)eng_mod_rule(MODR_EXIT_HOLD);   /* the cage wall takes longer */
        if (!need) need = 0x20u;
        if (++exit_hold[k] >= need) {
            exit_hold[k] = 0;
            if (o->joy & 1u) o->facing = 0x8000u;      /* toward the rope */
            else if (o->joy & 2u) o->facing = 0;
            if (st->scene == 1 && eng_mode_rule(MODE_CAGE_ESCAPE)) {
                /* the CAGE escape: the front-wall climb (mod_cage_climb -
                 * pose 277 hand over hand, 37/33/35 over the top, a fall
                 * down the outside), the win when he lands */
                o->state = ST_MOVE; o->move_id = MOD_MOVE_CAGE_CLIMB; o->grap44 = 0;
                if (eng_dbgsel) fprintf(stderr, "mod: P%d climbs the cage\n", k + 1);
            } else if (eng_mod_rule(MODR_EXIT_CLIMB)) {
                o->state = ST_MOVE; o->move_id = 0x7A; o->grap44 = 1u | 8u;
                if (eng_dbgsel) fprintf(stderr, "mod: P%d climbs out of the ring\n", k + 1);
            } else {
                eng_rope_exit(o);
                if (eng_dbgsel) fprintf(stderr, "mod: P%d hops out of the ring\n", k + 1);
            }
            return 1;
        }
    } else exit_hold[k] = 0;
    return 0;
}
