/* Royal Rumble mode — docs/engine-specs/rumble.md (2026-08-23, stage A).
 * Match init 0x10902 (6 men at the bell), entrant spawner 0xBB14 /
 * arrival 0x7430, re-targeting 0x20AA6, controller 0x2017A. The
 * elimination itself is anim.c (0x68 rumble branch 0x1990A -> move 0x7A
 * 0x1A348), which calls eng_rumble_slot_free() when the man has left.
 * Everything not cited is TODO EXACT (mini-select 0x9132, HUD strip,
 * king ceremony 0x1AFC). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include "scene.h"
extern int eng_dbgsel;
extern int eng_front;                /* core.c: the front-end scenes are live */

/* docs/adr-001, group base/rumble: the bell line-up 0x1088A/0x10A42 seats
 * six men from 0x10ACC (x,y word pairs, A1) and 0x10AE4 (facing words,
 * A2); the code resumes at 0x10AF0. Declared and exported here; the read
 * site below still uses the transcribed constants because eng_rumble_init
 * runs inside eng_init (core.c), BEFORE main.c binds the data layer
 * (tbl_load_rom/pak follow eng_render_init) — a tbl16() there reads 0.
 * TODO: switch seat() to tbl16(TBL(rumble_bell_positions/facings), ...)
 * once the data layer is loaded before eng_init. */
static const tbl_def rumble_tables[] = {
    { "rumble_bell_positions", "base/rumble", 0x10ACC, 6 * 4, TK_U16, 2,
      "0x1088A/0x10A42: {x, y} for the six men at the bell (slot order), written to +0x06/+0x0A" },
    { "rumble_bell_facings",   "base/rumble", 0x10AE4, 6 * 2, TK_U16, 1,
      "0x1089E/0x10A5A: facing word per bell slot (0x8000 = faces right), written to +0x2E" },
    { "join_grid_portraits",   "base/rumble", 0x9714, 12 * 2, TK_U16, 1,
      "0x9224/0x92BE: per wrestler id the first FG0 cell word (pal<<12|tile) of the 3x3 mini portrait the BUY-IN pick grid draws at $C1B30 (0x9236: 9 consecutive tiles row-major, stride 0xC per entry)" },
    { "join_cursor_tiles",     "base/rumble", 0x970C, 4 * 2, TK_U16, 1,
      "0x9628: per SEAT the first FG0 cell word of the 3x2 cursor marker the pick grid draws two rows above the hovered portrait ($C1930 + index*0xC, 0x95AE/0x961C)" },
};
TBL_REGISTER(rumble_tables)

int eng_rumble_armed(void)
{
    /* 0xE2C: attract demo segment 1 is a rumble ($1C0160 = 1, 0xECC). */
    return getenv("WF_RUMBLE") != 0 || eng_gs_rumble()
        || eng_demo_rumble_six() != NULL;
}

/* 0x24CC walk over 0x10B5C / 0xBCF8 (4 tries) then 0x10AF0/0x10B26 first
 * free — engine: a uniform 0x21B4 pick among the free ids of the side
 * (TODO EXACT the d100 table). side 0 = ids 0-5, 1 = ids 6-11, -1 any. */
static int pick_unpicked(eng_state *st, int side)
{
    int freeid[12], n = 0;
    for (int id = 0; id < 12; id++) {
        if (side == 0 && id >= 6) continue;
        if (side == 1 && id < 6) continue;
        if (!st->rumble_picked[id]) freeid[n++] = id;
    }
    if (!n) return -1;
    return freeid[eng_rng() % (unsigned)n];
}

/* One man into a slot: the shape of 0x10902 (humans) / 0x10A36 (CPUs). */
static eng_obj *seat(eng_state *st, int slot, int id, int player, int32_t x, int32_t y, uint16_t facing)
{
    eng_obj *o = &st->obj[slot];
    memset(o, 0, sizeof *o);
    o->active = 1;
    o->wrestler = id;
    o->x = x << 16; o->y = y << 16; o->z = 0x140 << 16;
    o->facing = facing;
    o->partner = -1; o->last_pair = -1; o->opp = -1; o->teammate = -1; o->rescue = -1;
    /* +0x33: humans 3 (legal+human, 0x10930 move.w #3,+0x32), CPUs 1
     * (0x10A48), a spawned entrant 5 (0xBCB0 +0x32 = 0x8005: legal + outside)
     * — everyone is "legal" in the rumble, the group camera follows all. */
    o->role = 0x01u;
    if (player >= 0) {                                /* 0x10536 human */
        o->input = player; o->role |= RF_PAD;
        o->hp = o->hp_max = (uint16_t)eng_pkg_stat((unsigned)id, "hp", id < 12 ? (int)tbl16(TBL(ws_max_energy), (uint32_t)id * 2u) : 100);   /* 0x10830 */
    } else {                                          /* 0x10A90 CPU */
        o->input = -1; o->cpu = 1;
        o->hp = o->hp_max = 0x87;                     /* 0x1081A */
        o->ai_b6 |= 0x80u;                            /* 0x10A96 +0xB6 b7: decide */
    }
    st->rumble_picked[eng_ws_base(id)] = 1;           /* $1C16A8/$1C16B4 (a clone marks his base: the
                                                         ladder is stock ids 0..11, and his base would
                                                         otherwise walk in wearing the same body) */
    return o;
}

static struct { int pending, port; } rot;         /* MOD rumble_rotate: a human seat waiting for the next entrant */

void eng_rumble_init(eng_state *st, const int *picks, int seated)
{
    rot.pending = 0;
    /* 0x10ACC positions / 0x10AE4 facings for the six at the bell
     * (= rumble_bell_positions / rumble_bell_facings, see the tbl_def note) */
    static const int16_t pos[6][2] = { {0x240,0x120},{0x2D0,0x120},{0x230,0x150},{0x2C0,0x150},{0x240,0x180},{0x2B0,0x180} };
    static const uint16_t face[6] = { 0x8000, 0, 0x8000, 0, 0x8000, 0 };
    int slot = 0;
    memset(st->rumble_picked, 0, sizeof st->rumble_picked);
    st->rumble_pending = 0; st->rumble_t = 0; st->rumble_phase = 0;
    st->g161 |= 1u;                                   /* $1C0161 b0 */
    {
        /* 0xECC attract demo segment 1: the fixed six of 0xF76 (objects
         * 0..5, all CPU, +0x56 = 0x80), 0x1086C init — no entrant ladder
         * seeding beyond them. */
        const uint8_t *six = eng_demo_rumble_six();
        if (six) {
            for (int i = 0; i < 6; i++) {
                int s = i < 4 ? i : 4;
                while (s < ENG_MAX_OBJS && st->obj[s].active) s++;
                if (s >= ENG_MAX_OBJS) break;
                seat(st, s, six[i], -1, pos[i][0], pos[i][1], face[i]);
                st->rumble_picked[six[i]] = 1;
            }
            if (eng_dbgsel) fprintf(stderr, "rumble: demo six seated\n");
            return;
        }
    }
    if (!(seated & 3)) seated = 1;                    /* harness: P1 always plays */
    for (int p = 0; p < 2; p++) {                     /* humans, slots 0-3 by player */
        int id;
        if (!(seated & (1 << p))) continue;
        id = (picks && picks[p * 2] >= 0 && picks[p * 2] < ENG_WS_EXT_MAX) ? picks[p * 2] : (p == 0 ? 0 : 1);   /* clone slots 12+ too (was < 12: Honky became Hogan, playtest 2026-08-25) */
        if (id < 12 && st->rumble_picked[id]) id = pick_unpicked(st, -1);
        seat(st, slot, id, p, pos[slot][0], pos[slot][1], face[slot]);
        slot++;
    }
    {
        int bell = eng_mode_rule(MODE_R_BELL);        /* mode: men at the bell
                                                         (stock 6; battle royale 8) */
        static const int16_t xpos[6][2] = { {0x250,0x168},{0x2B0,0x138},          /* slots 6/7: the 8-man bell */
                                            {0x288,0x120},{0x278,0x180},{0x218,0x168},{0x2F0,0x168} };   /* slots 8-11: the 12-man
                                          battle royale (every stock wrestler at the bell, nobody left to walk in) */
        if (bell < 2) bell = 2; else if (bell > ENG_MAX_OBJS) bell = ENG_MAX_OBJS;
        while (slot < bell) {                         /* 0x10A36 CPU fill (slots 4..8) */
            int id = pick_unpicked(st, -1);
            if (id < 0) break;
            {
                int s = 4;
                while (s < ENG_MAX_OBJS && st->obj[s].active) s++;
                if (s >= ENG_MAX_OBJS) {              /* bell > 7: the P3/P4 seats
                                                         carry CPUs (a buy-in takes
                                                         one over, 0x9282) */
                    s = 2;
                    while (s < 4 && st->obj[s].active) s++;
                    if (s >= 4) break;
                }
                if (slot < 6) seat(st, s, id, -1, pos[slot][0], pos[slot][1], face[slot]);
                else if (slot - 6 < 6) seat(st, s, id, -1, xpos[slot - 6][0], xpos[slot - 6][1], (uint16_t)((slot & 1) ? 0 : 0x8000u));
                else break;
            }
            slot++;
        }
    }
    if (eng_dbgsel) {
        fprintf(stderr, "rumble: init");
        for (int i = 0; i < ENG_MAX_OBJS; i++)
            if (st->obj[i].active) fprintf(stderr, " [%d]=w%d%s", i, st->obj[i].wrestler, st->obj[i].cpu ? "c" : "h");
        fprintf(stderr, "\n");
    }
}

/* 0x20BDA: the CPU's target search. Candidates (0x20BEA-0x20C2E): live,
 * not self, not held (+0x21 == 0xFF), not engaged (+0x33 b6, +0x26 linked
 * — a man in a tie-up / hold is never picked), not eliminated (+0x32 b4),
 * no pin intent (+0x34 b4), not outside (+0x33 b2), not frozen (+0x32 b7).
 * A HUMAN candidate no other live CPU is targeting (0x20C88) is taken at
 * once (0x20C44); otherwise score = |dx|/2 + |dy| (0x20C4C-0x20C66), best
 * under 0x400 ($1C1692). */
static int nearest(const eng_state *st, int self)
{
    int best = -1; int32_t bd = 0x400;
    const eng_obj *o = &st->obj[self];
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        const eng_obj *q = &st->obj[i];
        int32_t d;
        if (i == self || !q->active || (q->st_flags & SF_QUEUED)) continue;
        if ((q->state & 0xFFu) == 0xFFu) continue;           /* 0x20BFA held */
        if (q->role & RF_ENGAGED) continue;                        /* 0x20C02 engaged */
        if (q->partner >= 0) continue;                       /* 0x20C0A +0x26 linked */
        if (q->st_flags & SF_ELIMINATED) continue;                        /* 0x20C10 eliminated */
        if (q->tag_flags & TF_PIN_INTENT) continue;                        /* 0x20C18 pin intent */
        if (q->role & RF_OUTSIDE) continue;                        /* 0x20C20 outside */
        if (q->st_flags & SF_FROZEN) continue;                        /* 0x20C28 frozen */
        if (!q->cpu) {                                       /* 0x20C30: a human */
            int targeted = 0;
            for (int k = 0; k < ENG_MAX_OBJS; k++) {         /* 0x20C88: by a live CPU? */
                const eng_obj *c = &st->obj[k];
                if (k == self || !c->active || !c->cpu || c->opp != i || (c->st_flags & SF_ELIMINATED)) continue;
                targeted = 1; break;
            }
            if (!targeted) return i;                         /* 0x20C44: free human, take him */
        }
        d = (labs((q->x >> 16) - (o->x >> 16)) >> 1) + labs((q->y >> 16) - (o->y >> 16));
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* 0x216E8 — the rumble variant of 0x215B6 (called from the cover catch):
 * a CPU's cover (move 0x48) picks a second CPU (0x2187E: live, not self,
 * not the victim, no +0xB5 b3, not already in rescue mode b7, not
 * eliminated, in the OTHER roster half — +0x56 b5 = wrestler id >= 6 —
 * nearest by |dx|+|dy| < 0x400) and rolls 0x2172A {0x46, 0x1E}: hit ->
 * he is linked to the victim (+0x7E both ways) and comes to pile on (the
 * stock "double cover"); miss -> rescue mode off, +0xB6 b7 re-target. */
void eng_rumble_arm_helper(eng_state *st, eng_obj *p, eng_obj *v)
{
    int best = -1; int32_t bd = 0x400;
    if (!(st->g161 & 1u) || !p->cpu || (p->move_id & 0xFFu) != 0x48) return;   /* 0x216E8-0x216F6 */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {                                  /* 0x2187E */
        const eng_obj *c = &st->obj[i];
        int32_t d;
        if (c == p || c == v || !c->active || !c->cpu || (c->st_flags & SF_OUT_OF_PLAY)) continue;
        if ((c->ai_b5 & 0x08u) || (c->ai_b5 & 0x80u)) continue;
        if ((c->wrestler >= 6) == (v->wrestler >= 6)) continue;               /* 0x218D2: other half only */
        d = labs((v->x >> 16) - (c->x >> 16)) + labs((v->y >> 16) - (c->y >> 16));
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return;
    {
        eng_obj *h = &st->obj[best];
        unsigned roll = (eng_rng_fold(0) & 0xFFu) >> 1;                       /* 0x24CC d100 */
        h->ai_b5 |= 0x80u;                                                    /* 0x2195C */
        if (roll < 0x46) {                                                    /* 0x2172A {0x46,0x1E} */
            h->rescue = (int)(v - st->obj); p->rescue = best;                 /* 0x2171A/0x21720 */
            { extern void eng_ai_stat_pile(const eng_state *, const eng_obj *, int); eng_ai_stat_pile(st, h, 0); }
            if (eng_dbgsel) fprintf(stderr, "rumble: helper o%d piles onto o%d (cover by o%d)\n", best, (int)(v - st->obj), (int)(p - st->obj));
        } else {
            h->ai_b5 &= (uint8_t)~0x80u; h->ai_b6 |= 0x80u;                   /* 0x2170C/0x21712 */
        }
    }
}

/* The helper's drive while +0xB5 b7 is on (rumble): walk to the pile spot
 * (0x1F47E: beside the pinned man, x -/+0x20 by his facing, y +/-0x10) and
 * throw the cover; the victim is already held, so the 0x48 catch fails the
 * 0x11412 link test and he lands on the pile as the react-5 faller
 * (0x13400) — stock's double cover. Off when the pin is gone. */
uint32_t eng_rumble_helper_ai(eng_state *st, eng_obj *o)
{
    eng_obj *v;
    int32_t tx, ty, dx, dy;
    unsigned state = o->state & 0xFFu;
    if (o->rescue < 0 || o->rescue >= ENG_MAX_OBJS) { o->ai_b5 &= (uint8_t)~0x80u; return 0; }
    v = &st->obj[o->rescue];
    if (!v->active || (v->state & 0xFFu) != 0xFFu || v->partner < 0 || (v->st_flags & SF_ELIMINATED)) {
        o->ai_b5 &= (uint8_t)~0x80u; o->rescue = -1; o->ai_b6 |= 0x80u;       /* pin over: re-target */
        return 0;
    }
    if (state != ST_STAND && state != ST_WALK) return 0;
    if (o->ai_sub) {                     /* drop a walk-to-point / wait sub (0x3D pre-walk) that
                                            would steer him elsewhere: back to idle, re-decide */
        o->ai_sub = 0; o->ai_sub_t = 0; o->grap44 &= (uint16_t)~0x80u; o->state = ST_STAND; return 0;
    }
    tx = (v->x >> 16) + ((v->facing & 0x8000u) ? 0x20 : -0x20);
    ty = (v->y >> 16) + (((v->y >> 16) < 0x160) ? 0x10 : -0x10);
    dx = tx - (o->x >> 16); dy = ty - (o->y >> 16);
    if (dx > 6 || dx < -6 || dy > 4 || dy < -4) {
        uint32_t bits = 0;
        if (dx > 6) bits |= 1u; else if (dx < -6) bits |= 2u;
        if (dy > 4) bits |= 4u; else if (dy < -4) bits |= 8u;   /* dir_bits convention: +dy = bit 2 */
        return bits;
    }
    o->state = ST_MOVE; o->move_id = 0x48; o->partner = o->rescue;                  /* the second cover */
    o->facing = (uint16_t)(((o->x >> 16) < (v->x >> 16)) ? 0x8000u : 0);
    o->ai_b5 &= (uint8_t)~0x80u; o->rescue = -1;
    { extern void eng_ai_stat_pile(const eng_state *, const eng_obj *, int); eng_ai_stat_pile(st, o, 1); }
    if (eng_dbgsel) fprintf(stderr, "rumble: helper o%d covers the pile\n", (int)(o - st->obj));
    return 0;
}

/* 0x20E00: the HUMAN's target search (0x2095C loop, state 0/1/9, no +0x34
 * b6/b7): every other live man not held (0xFF) and not eliminated; a man
 * lying face-up (state 4 react 8) or held in 0x5D is measured at his HEAD
 * point (x -/+ 0x48 by his facing); score = |dx| / 4 + |dy|, best < 0x800.
 * 0x209D6: a found man being covered (0x4A) -> target his pinner (+0x26;
 * TODO EXACT the +0x9C / 0x48 variants). */
static int nearest_human(const eng_state *st, int self)
{
    int best = -1; int32_t bd = 0x800;
    const eng_obj *o = &st->obj[self];
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        const eng_obj *q = &st->obj[i];
        int32_t x, d, dy;
        if (i == self || !q->active || (q->state & 0xFFu) == 0xFFu || (q->st_flags & SF_OUT_OF_PLAY)) continue;
        x = q->x >> 16;
        if (((q->state & 0xFFu) == ST_REACT && (q->react_id & 0xFFu) == RC_LYING)
            || ((q->state & 0xFFu) == ST_MOVE && (q->move_id & 0xFFu) == 0x5D))
            x += (q->facing & 0x8000u) ? -0x48 : 0x48;   /* 0x20E6C head point */
        d = labs(x - (o->x >> 16)) >> 2;
        dy = labs((o->y >> 16) - (q->y >> 16));
        d += dy;
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0 && (st->obj[best].state & 0xFFu) == ST_MOVE && (st->obj[best].move_id & 0xFFu) == 0x4A
        && st->obj[best].partner >= 0)
        best = st->obj[best].partner;                 /* 0x209EE: the pinner */
    return best;
}

/* An eliminated man has walked off (move 0x7A phase 5 end, 0x1A504-0x1A528). */
void eng_rumble_slot_free(eng_state *st, eng_obj *o)
{
    int idx = (int)(o - st->obj);
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *q = &st->obj[i];
        if (q->partner == idx) q->partner = -1;
        if (q->last_pair == idx) q->last_pair = -1;
        if (q->opp == idx) q->opp = -1;
    }
    memset(o, 0, sizeof *o);                          /* 0x1A514 clr.b (A0) + wipe */
    o->partner = o->last_pair = o->opp = o->teammate = -1; o->input = -1; o->rescue = -1;
    st->rumble_pending++;                             /* 0x1A518 $1C16A7++ */
    if (eng_dbgsel) fprintf(stderr, "rumble: slot %d freed, pending %d\n", idx, st->rumble_pending);
}

/* 0xBC6A: a new CPU entrant is QUEUED — the slot goes live with +0x32 b15
 * (skipped by the object pass 0xF4CE, the AI 0x1C17E, the scans), and its
 * select-screen portrait (row id + 0x10) animates at screen (0x100,0x90)
 * through 0x74DE: steps 0-11 poses 0/1 for 4 (+rng&0xF) frames each,
 * step 12 pose 2 for 0x30, step 13 pose 3 for 0x20, then the ARRIVAL
 * 0x7430: row := id, (0x350,0x20,0x100) outside, state-1 sub 0xB walkway
 * walk (0x1D74A: x -> 0x279, y -> 0x100, climb 0x69) on autopilot. */
static const uint8_t q_tab[14][2] = { {0,4},{1,4},{0,4},{1,4},{0,4},{1,4},{0,4},{1,4},{0,4},{1,4},{0,4},{1,4},{2,0x30},{3,0x20} };   /* 0x74DE */

static void spawn_entrant(eng_state *st)
{
    int live_a = 0, live_b = 0, side, id, s = 4;
    for (int i = 0; i < ENG_MAX_OBJS; i++)
        if (st->obj[i].active) { if (st->obj[i].wrestler >= 6) live_b++; else live_a++; }
    side = (live_b < live_a) ? 1 : 0;                 /* 0xBBA6: the side with fewer men */
    id = pick_unpicked(st, side);
    if (id < 0) id = pick_unpicked(st, side ^ 1);
    if (id < 0 && eng_mod_rule(MODR_RUMBLE_ENDLESS)) {   /* MOD endless: the pool RE-OPENS - every id not
                                          in the arena right now may walk in again */
        for (int k = 0; k < 12; k++) {
            int in = 0;
            for (int i = 0; i < ENG_MAX_OBJS; i++)
                if (st->obj[i].active && eng_ws_base(st->obj[i].wrestler) == k) in = 1;
            if (!in) st->rumble_picked[k] = 0;
        }
        id = pick_unpicked(st, side);
        if (id < 0) id = pick_unpicked(st, side ^ 1);
        if (eng_dbgsel) fprintf(stderr, "rumble: endless - the pool re-opens (next w%d)\n", id);
    }
    if (id < 0) return;                               /* pools exhausted */
    if (rot.pending) s = 0;                           /* MOD rotate: the eliminated human's freed seat */
    while (s < ENG_MAX_OBJS && st->obj[s].active) s++;
    if (s >= ENG_MAX_OBJS) return;
    eng_sprite_reclaim_bank((unsigned)eng_ws_base(id));   /* his palette bank may have been lent to a weapon /
                                          badge / clone palette while he waited outside the bell
                                          ("another player walks in with a corrupt palette", user
                                          2026-09-05) - the backup run-in reclaims the same way */
    if (rot.pending) {                                /* MOD rotate: the human takes over this entrant
                                          (the buy-in join shape, 0x10536 human seat) */
        eng_obj *o = seat(st, s, id, rot.port, 0, 0, 0);
        o->st_flags = 0x8000u; o->role |= RF_OUTSIDE;
        o->z = 0x100 << 16;
        o->hold_t = 0; o->ai_bc = 0; o->spr = 0;
        o->cam_mode = 3;
        rot.pending = 0;
        if (eng_dbgsel) fprintf(stderr, "rumble: rotate - port %d takes over entrant w%d in slot %d\n", rot.port + 1, id, s);
        return;
    }
    {
        eng_obj *o = seat(st, s, id, -1, 0, 0, 0);
        o->st_flags = 0x8000u; o->role = 0x05u;             /* 0xBCB0: queued, legal, outside */
        o->z = 0x100 << 16;
        o->hold_t = 0; o->ai_bc = 0; o->spr = 0;      /* +0x22 / +0x24 / +0x05 */
        o->cam_mode = 3;                              /* 0x73CE +0x4A = 3 */
        if (eng_dbgsel) fprintf(stderr, "rumble: entrant w%d queued in slot %d\n", id, s);
    }
}

static void arrive(eng_state *st, eng_obj *o)         /* 0x7430 */
{
    o->st_flags &= (uint16_t)~0x8000u;
    o->x = 0x350 << 16; o->y = 0x20 << 16; o->z = 0x100 << 16;
    o->facing = 0; o->cam_mode = 0;
    o->state = ST_MOVE; o->move_id = 0x69; o->grap44 = 0x80u;   /* sub 0xB walk then climb in */
    o->run_tgt = 0x279; o->tgt_y = 0x100;
    o->spr = 0;
    if (eng_dbgsel) fprintf(stderr, "rumble: entrant w%d arrives (slot %d)\n", o->wrestler, (int)(o - st->obj));
}

/* ---- the mid-rumble BUY-IN entrant (0x18C4 queue -> 0x196A -> 0x9132).
 * Stock stages the joining seat's pick grid: the not-in-play wrestlers
 * (0x91E8, ids 4/5 skipped at 0x926C-0x9272), or — arena full ($1C15CE,
 * 0x9178) — the live $1C09E0 CPU men to TAKE OVER (0x9282). The picked
 * man runs the queued-entrant portrait ($1C1586[slot] 0x2000 -> 0x73AC)
 * and walks in (0x7430). ENGINE: auto-pick (WF_JOINPICK poke overrides);
 * the grid UI is TODO EXACT (0x9132 draws it at FG0 0xC1B30). */
static int join_pick(eng_state *st)
{
    int freeid[12], n = 0;
    if (getenv("WF_JOINPICK")) {
        int id = atoi(getenv("WF_JOINPICK")) % 12;
        if (!st->rumble_picked[id]) return id;
    }
    for (int id = 0; id < 12; id++) {
        if (id == 4 || id == 5) continue;             /* 0x926C-0x9272: ids 4/5 banned */
        if (!st->rumble_picked[id]) freeid[n++] = id;
    }
    if (!n) return -1;
    return freeid[eng_rng() % (unsigned)n];
}

static int try_join_spawn(eng_state *st, int port)
{
    int id = join_pick(st), s = 0;
    if (id < 0) return 0;                             /* pool empty: stock offers a CPU
                                                         takeover (0x9282) — TODO EXACT */
    while (s < ENG_MAX_OBJS && st->obj[s].active) s++;
    if (s >= ENG_MAX_OBJS) return 0;                  /* arena full ($1C15CE) — retry */
    {
        eng_obj *o = seat(st, s, id, port, 0, 0, 0);  /* human seat (0x10536 shape) */
        o->st_flags = 0x8000u; o->role |= RF_OUTSIDE;            /* 0xBCB0: queued, outside */
        o->z = 0x100 << 16;
        o->hold_t = 0; o->ai_bc = 0; o->spr = 0;
        o->cam_mode = 3;                              /* 0x73CE */
        if (eng_dbgsel) fprintf(stderr, "rumble: join port %d -> entrant w%d queued in slot %d\n",
                                port + 1, id, s);
    }
    return 1;
}

/* ---- 0x9132: the BUY-IN PICK GRID ------------------------------------
 * A joining seat picks a wrestler from a portrait strip at FG0 $C1B30:
 * the not-in-play ids (4/5 always banned, 0x926C), or — arena full,
 * 0x9178 ($1C15CE: 6+ live men) — the LIVE CPU men to TAKE OVER
 * (0x9282/0x972C). Stick L/R moves the seat's cursor ($1C1598[n],
 * claim-checked 0x9578), any button commits, and the $1C15D0 timer
 * (0x140 frames, 0x934E) commits for you. The engine draws each port's
 * nP label under its hovered portrait (stock's 0x96C6 cursor art is
 * TODO EXACT). */
#define JG_BASE 0x1B30u                 /* $C1B30 - $C0000 */
static struct {
    int on, size, takeover;
    uint8_t id[10];                     /* grid entries: wrestler ids */
    int8_t  slot[10];                   /* takeover: the CPU's obj slot */
    uint16_t cur[4];                    /* 0x8000|index per port, 0 = idle */
    int16_t timer;
} jg;

static unsigned hud_label_tile2(int port) { return tbl16(TBL(hud_label_tiles), (unsigned)port * 2u); }

static void jg_cell(unsigned off, unsigned w)
{
    if (off + 4 > WF_FG0RAM_SIZE) return;
    wf.fg0_videoram[off + 1] = (uint8_t)w;
    wf.fg0_videoram[off + 3] = (uint8_t)(w >> 8);     /* pal = top nibble (0x9236/0x923E) */
}

static void jg_draw(void)
{
    /* portraits: $C1B30 + e*0xC, 3x3 consecutive tiles (0x9236) */
    for (int e = 0; e < jg.size; e++) {
        unsigned w = tbl16(TBL(join_grid_portraits), (unsigned)jg.id[e] * 2u);
        unsigned base = JG_BASE + (unsigned)e * 0x0Cu;
        for (unsigned r = 0; r < 3; r++)
            for (unsigned c = 0; c < 3; c++)
                jg_cell(base + r * 0x100u + c * 4u, w + r * 3u + c);
    }
    /* cursors: 0x95AE clears the two marker rows at $C1930 (27 cells)
     * then draws each armed seat's 3x2 marker (0x970C tiles) at
     * $C1930 + index*0xC (0x9604 mulu #$c). */
    for (unsigned c = 0; c < 0x1Bu; c++) {
        jg_cell(JG_BASE - 0x200u + c * 4u, 0);
        jg_cell(JG_BASE - 0x100u + c * 4u, 0);
    }
    for (int pt = 0; pt < 4; pt++) {
        unsigned w2, base;
        if (!(jg.cur[pt] & 0x8000u)) continue;
        w2 = tbl16(TBL(join_cursor_tiles), (unsigned)pt * 2u);
        base = JG_BASE - 0x200u + (jg.cur[pt] & 0xFu) * 0x0Cu;
        for (unsigned r = 0; r < 2; r++)
            for (unsigned c = 0; c < 3; c++)
                jg_cell(base + r * 0x100u + c * 4u, w2 + r * 3u + c);
    }
}

static void jg_erase(void)
{
    for (unsigned e = 0; e < 10; e++)
        for (unsigned c = 0; c < 3; c++) {
            for (unsigned r = 0; r < 3; r++)
                jg_cell(JG_BASE + e * 0x0Cu + r * 0x100u + c * 4u, 0);
            jg_cell(JG_BASE - 0x200u + e * 0x0Cu + c * 4u, 0);
            jg_cell(JG_BASE - 0x100u + e * 0x0Cu + c * 4u, 0);
        }
}

static void jg_build(eng_state *st)
{
    int live = 0;
    jg.size = 0; jg.takeover = 0;
    for (int i = 0; i < ENG_MAX_OBJS; i++)
        if (st->obj[i].active && !(st->obj[i].st_flags & SF_ELIMINATED)) live++;
    if (live >= 6) jg.takeover = 1;                   /* 0x9172-0x9178 */
    if (!jg.takeover) {
        for (int id = 0; id < 12 && jg.size < 10; id++) {   /* 0x91F8-0x927A */
            int inplay = 0;
            if (id == 4 || id == 5) continue;         /* 0x926C */
            for (int i = 0; i < ENG_MAX_OBJS; i++)
                if (st->obj[i].active && st->obj[i].wrestler == id) inplay = 1;
            if (inplay) continue;                     /* $1C1576 list */
            jg.slot[jg.size] = -1;
            jg.id[jg.size++] = (uint8_t)id;
        }
    }
    if (jg.takeover || !jg.size) {                    /* 0x9282: live CPU men */
        jg.takeover = 1; jg.size = 0;
        for (int i = 4; i < ENG_MAX_OBJS && jg.size < 10; i++) {
            eng_obj *o = &st->obj[i];
            if (!o->active || !o->cpu || o->wrestler == 4 || o->wrestler == 5) continue;
            jg.slot[jg.size] = (int8_t)i;
            jg.id[jg.size++] = (uint8_t)o->wrestler;
        }
    }
}

static int jg_claimed(int e, int self)                /* 0x9578 */
{
    for (int pt = 0; pt < 4; pt++)
        if (pt != self && (jg.cur[pt] & 0x8000u) && (int)(jg.cur[pt] & 0xFu) == e)
            return 1;
    return 0;
}

static void jg_commit(eng_state *st, int port)
{
    int e = (int)(jg.cur[port] & 0xFu);
    jg.cur[port] = 0;
    st->joinq &= (uint8_t)~(1u << port);              /* queue word consumed */
    if (jg.takeover && jg.slot[e] >= 0) {             /* 0x972C: TAKE OVER the CPU */
        eng_obj *o = &st->obj[jg.slot[e]];
        o->cpu = 0; o->input = port;
        o->role |= RF_PAD; o->driver &= (uint16_t)~0xC0u;
        if (eng_dbgsel) fprintf(stderr, "rumble: port %d TAKES OVER w%d (slot %d)\n",
                                port + 1, o->wrestler, jg.slot[e]);
    } else {
        int id = jg.id[e], sl = 0;
        while (sl < ENG_MAX_OBJS && st->obj[sl].active) sl++;
        if (sl < ENG_MAX_OBJS) {
            eng_obj *o = seat(st, sl, id, port, 0, 0, 0);
            o->st_flags = 0x8000u; o->role |= RF_OUTSIDE;        /* 0xBCB0: queued, outside */
            o->z = 0x100 << 16;
            o->hold_t = 0; o->ai_bc = 0; o->spr = 0;
            o->cam_mode = 3;                          /* 0x73CE */
            if (eng_dbgsel) fprintf(stderr, "rumble: join port %d -> entrant w%d queued in slot %d\n",
                                    port + 1, id, sl);
        }
    }
}

static void jg_tick(eng_state *st)
{
    static uint8_t prev[4], rep[4];
    int any = 0;
    if (!jg.on) return;
    for (int pt = 0; pt < 4; pt++) {
        unsigned in = st->inputs[pt], edge;
        if (!(jg.cur[pt] & 0x8000u)) { prev[pt] = (uint8_t)in; continue; }
        any = 1;
        edge = in & ~prev[pt]; prev[pt] = (uint8_t)in;
        if (!(in & 0x03u)) rep[pt] = 5;               /* 0x9500: neutral reloads
                                                         the 5-frame repeat gate */
        else if (rep[pt] && --rep[pt]) ;              /* 0x9556 countdown */
        else {
            rep[pt] = 5;
            if (in & 0x01u) {                         /* right (0x952C: level) */
                int e = (int)(jg.cur[pt] & 0xFu);
                do e = (e + 1) % jg.size; while (jg_claimed(e, pt) && jg.size > 1);
                jg.cur[pt] = (uint16_t)(0x8000u | e);
            } else if (in & 0x02u) {                  /* left (0x950E) */
                int e = (int)(jg.cur[pt] & 0xFu);
                do e = (e + jg.size - 1) % jg.size; while (jg_claimed(e, pt) && jg.size > 1);
                jg.cur[pt] = (uint16_t)(0x8000u | e);
            }
        }
        if (edge & 0x30u)                             /* any button: commit (0x93BC) */
            jg_commit(st, pt);
    }
    if (jg.timer > 0 && --jg.timer == 0)              /* 0x934E/0x939C: 0x140 frames */
        for (int pt = 0; pt < 4; pt++)
            if (jg.cur[pt] & 0x8000u) jg_commit(st, pt);
    for (int pt = 0; pt < 4; pt++) if (jg.cur[pt] & 0x8000u) any = 1;
    if (!any) { jg.on = 0; jg_erase(); return; }      /* bclr $1C167B b1 */
    jg_draw();
}

void eng_rumble_join(eng_state *st, int port)
{
    if (getenv("WF_JOINPICK")) {                      /* harness: bypass the grid */
        if (try_join_spawn(st, port))
            st->joinq &= (uint8_t)~(1u << port);
        return;
    }
    if (!jg.on) { jg_build(st); if (!jg.size) return; jg.on = 1; jg.timer = 0x140; }
    if (!(jg.cur[port] & 0x8000u)) {                  /* claim the first free cell (0x932E) */
        int e = 0;
        while (e < jg.size && jg_claimed(e, port)) e++;
        if (e >= jg.size) e = 0;
        jg.cur[port] = (uint16_t)(0x8000u | e);
        jg.timer = 0x140;                             /* 0x934E */
        if (eng_dbgsel) fprintf(stderr, "rumble: port %d picks (%s grid, %d entries)\n",
                                port + 1, jg.takeover ? "TAKEOVER" : "join", jg.size);
    }
}

static void joinq_retry(eng_state *st)                /* a full arena at join time: retry */
{
    if (jg.on) return;                                /* the pick grid owns the queue */
    if (!getenv("WF_JOINPICK")) return;               /* only the harness bypass uses
                                                         the instant spawn now */
    for (int p = 0; p < 4; p++)
        if ((st->joinq & (1u << p)) && try_join_spawn(st, p))
            st->joinq &= (uint8_t)~(1u << p);
}

static void queued_tick(eng_state *st)                /* 0x73E4-0x7430 */
{
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active || !(o->st_flags & SF_QUEUED)) continue;
        if (o->hold_t) { o->hold_t--; continue; }
        if (o->ai_bc >= 14) { arrive(st, o); continue; }
        o->spr = q_tab[o->ai_bc][0];                  /* +0x05 portrait pose */
        o->hold_t = q_tab[o->ai_bc][1];               /* +0x22 */
        if (o->ai_bc < 12) o->hold_t = (uint16_t)(o->hold_t + (eng_rng() & 0xFu));   /* 0x7418 */
        o->ai_bc++;                                   /* 0x7424 */
    }
}

/* ================ 0x1AE28: the KING-OF-THE-RUMBLE ceremony ============
 * Stock spawns move 0x8E into the first free slot from $1C09E0
 * (0x20282-0x202AC: active, clr +0x44/+0x32, state 5 move 0x8E); its
 * handler makes itself the RING ANNOUNCER (0x1AE62: +0x02 = id 0x1F,
 * jsr 0x2AEA palette). The engine carries that object on st->ann (the
 * walk-in announcer, sprite row 0x1F in sprite.c) — same cells,
 * positions and clocks; phases follow the +0x44 table 0x1AE40. */
static struct {
    int on, phase, entered, idx;
    int t;                       /* +0x22 pose / countdown clock */
    int winner;                  /* $1C0092 (0x2026E) */
} cer;

static void ceremony_end(eng_state *st)
{
    /* 0x1AFCA $1C16C4 = 1 -> frame loop 0x115C `jmp 0x1B4E`: the
     * championship ceremony scene, then attract — NEVER another match. */
    cer.on = 0;
    st->ann.active = 0;
    for (int i = 0; i < ENG_MAX_OBJS; i++)            /* engine bookkeeping: the
                                                         result words end the match */
        if (st->obj[i].active)
            st->obj[i].result = (uint16_t)(st->obj[i].cpu ? 0x8001 : 0x8000);
    if (eng_dbgsel) fprintf(stderr, "rumble: ceremony done -> 0x1B4E\n");
    if (eng_front)
        eng_scene_set(ENG_SCENE_CEREMONY);            /* 0x1162 jmp 0x1B4E */
}

static void ceremony_tick(eng_state *st)
{
    eng_ann *a = &st->ann;
    eng_obj *w = (cer.winner >= 0 && cer.winner < ENG_MAX_OBJS)
               ? &st->obj[cer.winner] : 0;

    if (!cer.on) return;
    if (!w) { cer.on = 0; return; }
    switch (cer.phase) {
    case 0:                                           /* 0x1AE50 walk-in poses */
    case 1: {                                         /* 0x1AECA talk poses */
        uint32_t script = cer.phase ? 0x1AEB8u : 0x1AEC1u;   /* [n, (pose,dur)...] */
        if (!cer.entered) {
            cer.entered = 1;
            a->active = 1;                            /* 0x1AE70 bset #5/#6,+0x32 */
            a->x = 0x278 << 16;                       /* 0x1AE7C (phase 1: 0x1AED2) */
            a->y = 0x199 << 16;
            a->z = 0x14E << 16;
            cer.idx = 0;                              /* 0x1AF0C clr +0x24 */
            a->spr = tbl_ra8(script + 1u);            /* 0x1AF3E first pose */
            cer.t = (int)tbl_ra8(script + 2u);        /* 0x1AF44 its clock */
            break;
        }
        if (cer.t-- > 0) break;                       /* 0x1AF12 subq +0x22 */
        if (++cer.idx >= (int)tbl_ra8(script)) {      /* 0x1AF1C-0x1AF28: +0x24 = 0xFE */
            cer.phase++;                              /* 0x1AE9A/0x1AEE6 +0x44 + 1 */
            cer.entered = 0;
            break;
        }
        a->spr = tbl_ra8(script + 1u + 2u * (unsigned)cer.idx);   /* 0x1AF3E */
        cer.t = (int)tbl_ra8(script + 2u + 2u * (unsigned)cer.idx);
        break; }
    case 2:                                           /* 0x1AF4C wait for the winner */
        a->spr = 0;                                   /* 0x1AF4C +0x04 = 0 */
        a->z = 0x140 << 16;                           /* 0x1AF56 */
        if ((w->state & 0xFFu) == ST_STAND                   /* 0x1AF62 winner at STAND */
            && !st->ann_active && !st->ann_req_phrase) {   /* 0x1AF6A $1C15D4 idle */
            cer.phase = 3; cer.entered = 0;
        }
        break;
    default:                                          /* 0x1AF7E the announcement */
        if (!cer.entered) {
            cer.entered = 1;
            /* 0x1AF8C-0x1AFB0: $1C15D2 = winner id + 0xC (his name),
             * $1C15D3 = 0x2B — "...the KING of the ROYAL RUMBLE!" */
            eng_announce(st, (unsigned)w->wrestler, 0x2B);
            w->state = ST_MOVE; w->move_id = 0x79;          /* 0x1AFA4 the pose */
            a->spr = 0x800Cu;                         /* 0x1AFB8 cell 0xC, flipped */
            cer.t = 0x200;                            /* 0x1AFBE +0x22 */
            if (eng_dbgsel)
                fprintf(stderr, "rumble: ceremony announce w%d (0x1AF7E)\n", w->wrestler);
        }
        if (--cer.t == 0)                             /* 0x1AFC4 */
            ceremony_end(st);
        break;
    }
    a->sx = (int16_t)((a->x >> 16) - st->cam_x);      /* 0x247C */
    a->sy = (int16_t)((a->y >> 16) + (a->z >> 16) - st->cam_y);
}

/* ---- 0xBCFE (frame list 0x10C4, ODD frames): the continue countdown.
 * Rumble only (0xBD06 $1C0161 b0). The FIRST seat whose $1C0156 word has
 * its low bit set (0xBD16) is serviced: the hi byte counts down by one
 * per pass (0xBD28, 0x30 -> ~96 frames); at zero the word clears, the
 * text palette entry 9 goes home ($180012 := 0x6FF, 0xBD5E) and the
 * plate region is wiped (0xBD6A: 24 cells x 2 rows from FG0 0x520).
 * While counting: the palette flashes 0xFFF/0x00F on frame bit 1
 * (0xBD88), and the plate — blit record 0x4C + seat, "PLAYER-n IS
 * DISQUALIFIED" (0x25204 records, mode 1 off 0x520) — blinks on frame
 * bit 3 (0xBD38: shown, else wiped via 0xBD6A). */
static void cont_wipe(void)                           /* 0xBD6A */
{
    for (unsigned r = 0; r < 2; r++)
        for (unsigned c = 0; c < 24; c++) {
            unsigned a = 0x520u + r * 0x100u + c * 4u;
            if (a + 4 <= WF_FG0RAM_SIZE) memset(&wf.fg0_videoram[a], 0, 4);
        }
}
static void cont_pal(unsigned w)                      /* $180012 word (entry 9, text pal 0) */
{
    wf.palette[0x12] = (uint8_t)(w >> 8);
    wf.palette[0x13] = (uint8_t)w;
}
void eng_rumble_continue_tick(eng_state *st)
{
    int seatn = -1;
    if (!(st->frame & 1)) return;                     /* 0x10C4: odd-frame list */
    if (!(st->g161 & 1u)) return;                     /* 0xBD06 */
    for (int p = 0; p < 4; p++)
        if (st->cont[p] & 0x0001u) { seatn = p; break; }   /* 0xBD16 low-byte b0 */
    if (seatn < 0) return;
    st->cont[seatn] -= 0x100u;                        /* 0xBD28 subi.b #1 (hi byte) */
    if (!(st->cont[seatn] & 0xFF00u)) {               /* 0xBD2E hi byte == 0 */
        st->cont[seatn] = 0;                          /* 0xBD5A clr.w */
        cont_pal(0x6FF);                              /* 0xBD5E */
        cont_wipe();                                  /* 0xBD66 bsr 0xBD6A */
        return;
    }
    cont_pal((st->frame & 2) ? 0xFFF : 0x00F);        /* 0xBD88 ($1C0083 b1) */
    if (!(st->frame & 8)) eng_blit(0x4Cu + (unsigned)seatn);   /* 0xBD38/0xBD46 */
    else cont_wipe();                                 /* 0xBD56 */
}

void eng_rumble_tick(eng_state *st)
{
    int humans = 0, cpus = 0, live = 0;
    if (!(st->g161 & 1u)) return;
    ceremony_tick(st);                                /* move 0x8E (runs under phase 4) */
    jg_tick(st);                                      /* 0x9132 buy-in pick grid */
    queued_tick(st);                                  /* 0x73E4 portrait -> arrival */
    /* ---- 0xBB14 entrant spawner ---- */
    if (st->rumble_pending) {
        if (++st->rumble_t >= eng_mode_rule(MODE_R_SPAWN_FRAMES)) {   /* 0xBB3E (stock 0x126) */
            st->rumble_t = 0;
            for (int i = 0; i < ENG_MAX_OBJS; i++) if (st->obj[i].active) live++;
            if (live < eng_mode_rule(MODE_R_LIVE_CAP))                 /* 0xBB6C (stock 6) */
                { st->rumble_pending--; spawn_entrant(st); }
        }
    }
    /* ---- 0x20AA6 re-target (CPU) + humans (TODO EXACT) ---- */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        unsigned state;
        if (!o->active || (o->st_flags & SF_QUEUED)) continue;
        state = o->state & 0xFFu;
        if (o->cpu) {
            if (!(o->tag_flags & TF_PIN_INTENT) && ((state == ST_STAND && !(o->ai_b5 & 0x08u))
                || (state == ST_WALK && o->ai_sub == 0 && (o->ai_b6 & 0x80u)))) {
                int n;
                o->ai_b6 &= (uint8_t)~0xC0u;
                n = nearest(st, i);
                if (n >= 0) o->opp = n; else o->ai_b6 |= 0x40u;
            }
        } else if ((state <= 1 || state == ST_PERCH) && !(o->tag_flags & TF_WHIP_MARKS)) {   /* 0x2095C human */
            int n = nearest_human(st, i);
            if (n >= 0) o->opp = n;
        }
        if (o->opp >= 0 && (!st->obj[o->opp].active || (st->obj[o->opp].st_flags & SF_ELIMINATED)))
            o->opp = nearest(st, i);
    }
    /* ---- 0x2017A controller ---- */
    if (st->rumble_phase) {                           /* $1C16C5 latched (0x20190/0x2022E) */
        if ((st->rumble_phase & 1u) && !(st->rumble_phase & 2u)) {
            /* 0x20190: b0 (all humans out) — the per-seat continue words
             * $1C0156[0..3] hold the arena live (0x2019A-0x201B0: any
             * nonzero -> rts); armed to 0x3001 at the elimination
             * (0x2021C), decayed by 0xBCFE (eng_rumble_continue_tick).
             * All zero -> bset b1 (0x201B4); $1C16C5 == 3 ends the match
             * (0x1166 -> 0x1AFC game over -> jmp 0x6FC attract). ENGINE
             * GUARD (the V411 walk-out freeze): b1 also waits until no
             * eliminated man is still walking off — stock's 0x1AFC does
             * not freeze the objects, the engine's result stamp does. */
            int hold = 0;
            for (int p = 0; p < 4; p++) if (st->cont[p]) hold = 1;
            for (int i = 0; i < ENG_MAX_OBJS; i++)
                if (st->obj[i].active && (st->obj[i].st_flags & SF_ELIMINATED)) hold = 1;
            if (!hold) {
                st->rumble_phase |= 2u;               /* 0x201B4 bset #1 */
                for (int i = 0; i < ENG_MAX_OBJS; i++)
                    if (st->obj[i].active) st->obj[i].result = 0x8001;
                /* $1C16C5 == 3 ends the match AT ONCE in stock (0x1166 ->
                 * 0x1AFC game over -> attract). The engine's end test
                 * (eng_camp_end_test) keys on the leaders' +0xA0, which
                 * nobody writes on this path — it fell back only after
                 * A0_FALLBACK (0x200 frames), and for that whole window
                 * the result-stamped men stood frozen mid-pose (a pair
                 * caught in a tie-up/hold "grappled forever, nobody won",
                 * rumble playtest 2026-08-24). Stamp +0xA0 now, exactly
                 * as the fallback would. */
                for (int i = 0; i < ENG_MAX_OBJS; i++) {
                    eng_obj *o2 = &st->obj[i];
                    if (!o2->active || !(o2->result & 0x8000u)) continue;
                    o2->a0flags = (uint16_t)((o2->result & 1u) ? 0x80u : 0x40u);
                }
                st->obj[0].a0flags |= 0x80u;          /* the end test reads only the
                                                         leader slots 0/2; a freed seat
                                                         must still trip it */
                if (eng_dbgsel) fprintf(stderr, "rumble: continue words spent -> game over (0x1AFC)\n");
            }
        }
        return;
    }
    joinq_retry(st);                                  /* a queued buy-in waiting for a slot */
    if (st->joinq)                                    /* 0x201BE-0x201D4: any queued join
                                                         ($1C1586 word nonzero) defers the
                                                         census — no b0 while a paid seat
                                                         is still entering */
        return;
    if (getenv("WF_RWIN") && st->frame == atoi(getenv("WF_RWIN"))) {
        /* harness poke: eliminate everyone but P1 now so the 0x20254 win
         * path (ceremony) can be driven headlessly */
        st->rumble_pending = 0;
        for (int i = 1; i < ENG_MAX_OBJS; i++)
            if (st->obj[i].active) { st->obj[i].st_flags |= SF_ELIMINATED; st->obj[i].state = ST_STAND; st->obj[i].partner = -1; }
        st->obj[0].state = ST_STAND; st->obj[0].partner = -1;
    }
    {
        int win_slot = -1;
        for (int i = 0; i < ENG_MAX_OBJS; i++) {      /* 0x201E4-0x2022A the census */
            const eng_obj *o = &st->obj[i];
            if (!o->active) continue;
            if (o->st_flags & SF_ELIMINATED) {                     /* 0x201F6 eliminated */
                /* 0x20210-0x2021C: an eliminated HUMAN's seat word gets
                 * the continue code 0x3001 every pass while his object
                 * lives (stock indexes $1C0156 by the main-array slot;
                 * the engine keys the seat by his port). The word holds
                 * the b1 game-over off (0x2019A) and blinks the
                 * PLAYER-n IS DISQUALIFIED plate (0xBD46). */
                if (!o->cpu && eng_mod_rule(MODR_RUMBLE_ROTATE)) {   /* MOD rotate: no plate, no game
                                          over - his seat waits for the next entrant */
                    if (!rot.pending) { rot.pending = 1; rot.port = o->input >= 0 ? o->input : (i < 4 ? i : 0);
                        if (eng_dbgsel) fprintf(stderr, "rumble: rotate - port %d eliminated, waits for the next entrant\n", rot.port + 1); }
                    humans++;
                } else if (!o->cpu && o->input >= 0 && o->input < 4)
                    st->cont[o->input] = 0x3001u;
                continue;
            }
            if (o->cpu) cpus++;
            else { humans++; win_slot = i; }          /* 0x20206 D5 = the live human */
        }
        if (rot.pending) humans++;                    /* MOD rotate: a seat is coming back */
        if (humans == 0 && !st->rumble_pending
            && !eng_demo_active()) {                  /* 0x2022E/0x20244 b0: no human left.
                                                         NOT in the attract demo: its six men
                                                         are all CPU (0xECC) and the segment
                                                         ends on the 0x119A timer alone
                                                         (0x10F8/0x1102) — the continue-window
                                                         0x8001 stamp here put every demo man
                                                         in the loser pose at the V425 bell
                                                         sweep ("holding their heads") */
            st->rumble_phase |= 1u;                   /* bset #0,$1C16C5 */
            st->rumble_t = 0;                         /* reuse: the continue-window clock */
            st->sig169e = 0;                          /* 0x2024C clr $1C169E — clock off.
                                                         NO result words here (0x20244 sets
                                                         only the latch): the arena keeps
                                                         running so the last man's walk-out
                                                         completes — stamping 0x8001 froze
                                                         him mid-exit ("walk out missing,
                                                         soft lock", playtest 2026-08-23) */
            if (eng_dbgsel) fprintf(stderr, "rumble: all humans out (b0, continue window)\n");
        } else if (humans == 1 && cpus == 0 && !st->rumble_pending) {
            /* ---- 0x20254 b2: THE HUMAN WON — king of the Royal Rumble.
             * No result words yet: they land when the ceremony ends
             * ($1C16C4, ceremony_end). */
            eng_obj *w = &st->obj[win_slot];
            st->rumble_phase |= 4u;                   /* bset #2,$1C16C5 */
            eng_ref_digit_wipe();                     /* 0x2025C jsr 0x206FE */
            st->sig169e = 0;                          /* 0x20260 clr $1C169E (clock off) */
            st->ref.sm = 9; st->ref.win_t = 44;       /* 0x20266 referee $8009 win pose */
            st->ref.target = win_slot;
            w->state = ST_WALK;                             /* 0x20276 */
            w->sub = 0x0C;                            /* 0x2027C +0xAE = 0xC: the ceremony
                                                         walk to (0x2C0,0x160) (0x11B6C) */
            w->ai_sub = 0; w->ai_press = 0; w->ai_press_t = 0;
            memset(&cer, 0, sizeof cer);              /* 0x20282-0x202AC: spawn move 0x8E */
            cer.on = 1; cer.winner = win_slot;
            if (eng_dbgsel) fprintf(stderr, "rumble: human w%d wins -> ceremony (0x20254)\n",
                                    w->wrestler);
        }
    }
}
