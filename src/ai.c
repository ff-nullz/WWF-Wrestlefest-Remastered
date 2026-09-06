/* CPU opponent — v2 "virtual controller" over the ROM's AI (0x1C150–0x1F760,
 * docs/engine-specs/ai-core.md, ai-behaviour.md).
 *
 * The 68k AI writes +0x61 (move id) and jumps into the same move-prep
 * dispatcher 0x1DFE2 the human path uses. This engine keeps the decision
 * logic and replaces the *controller*: every decision is turned into
 * joystick/button bits so the engine's own selector (core.c walk_logic)
 * runs the move with the human rules. Three things are written directly,
 * exactly where the ROM writes them too: the corner claim on the sub-9
 * arrival (0x1D31E → 0xEECE, state 8), the downed opponent's +0x9A bump
 * when the top-rope roll wins (0x1F222), and the run-in break of a
 * STANDING grapple: the rescuer swings and hit.c decides (no record = whiff).
 * PINS no longer use that stand-in: the rescuer stomps the pile and the
 * cover is freed by reaction handler 9 (0x24BC2/0x24C0C) on hit record
 * 0x1D — see docs/engine-specs/pin-partner.md.
 *
 * EXACT: policy records 0x219B4 / 0x2307C (0x1DEA6: row by the OPPONENT's
 * band, forced to 2 under 10 minutes), the d100 walk 0x24CC, the approach
 * thresholds 0x1C93E (near = dx<0x50 && dy<0xC, buckets 0x50/0xB0, policy
 * distance ctx 0x68/0xB0), run-eval 0x1F310 (0x1F3A6/0x1F3BE by id), the
 * run-away roll 0x1D030, run-phase rolls 0x1D8C2 (0x23B26/0x238CE by
 * id×min(stage,5)×band), the running-attack gates 0x1E4DA–0x1E568, the
 * stand-move gates 0x1E6FE–0x1E75C, the walk-to targets 0x1F45A/0x1F4CE/
 * 0x1F51E, the top-rope intent 0x1F1AC (corner pick, 0x1F2E0 roll, sub 9
 * 0x1D31E, state 9 0x1EEA0 with 0x1EF92/0x1EF9E/0x1EF90), the lockup
 * 0x1EFAA (0x1F060[stage]), the hold cascade 0x1F078, pin intent 0x1108C
 * (0x110C8 WORD table), the rescue arm tables 0x2110E/0x2172C + countdown
 * 0x1D526, the run-in roll 0x1D4FC[stage], the RNG 0x21B4 shape (state +
 * frame $1C0080 + known registers, ror by the low 3 bits).
 * TODO EXACT is marked inline: the pseudo-moves 0x3C/0x3E are walked
 * instead of animated (0x18028 record / 0x1DF7E teleport), 0x72/0x75 out
 * of a run map to B2/B1 of the engine's cat 1/2 row, the run-in strike
 * 0x39 / behind grab 0x1C are a B1 press, the rope-line walk sub 5
 * (0x1D078) and the counter table 0x1F5B2 (rescue mode) are not
 * transcribed, the x-zone +0x31 is approximated, state-4 escape rolls
 * (0x1DA7E) are untouched. */
#include <stdlib.h>
#include <stdio.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"

extern int eng_dbgsel;

/* ROM tables this file reads (docs/table-migration-brief.md). Extents
 * from maincpu.asm (each table runs up to the next code or data block);
 * the pointer-chased AI records (policies, run rolls, downed-man rolls)
 * are one U8 blob per region — the pointers inside stay ROM addresses and
 * are followed with tbl_ra8/tbl_ra_ptr, so a blob's bounds are the first
 * pointer table and the last record it reaches (checked end-to-end: every
 * pointer lands inside its own blob). Per-wrestler rows (12 entries by id)
 * are group "wrestler" so the packer can slice them later. */
static const tbl_def ai_tables[] = {
    /* --- move selection policies (0x1DEA0 / 0x1DEA6, 0x1F078 cascade) --- */
    { "move_policy",              "base/ai", 0x219B4, 0x2307C - 0x219B4, TK_U8, 16,
      "0x1DEA0: [wrestler id] long -> 11 context longs -> record {n, id[n], w0[n], w1[n], w2[n]} (band row = opponent's +0x70, forced 2 under 10 min); ctx 1 = cascade [facing][zone 0/4/8] -> record (0x1F078); 0x219B4..0x2307B" },
    { "move_policy_grapple",      "base/ai", 0x2307C, 0x23378 - 0x2307C, TK_U8, 16,
      "0x1DEA6 with A2=0x2307C (opponent mid-grapple-move): [wrestler id] long -> 3 context longs -> record, same layout; 0x2307C..0x23377 (0x23378 = 0x21530 double-team rolls)" },
    /* --- running phase 1 (0x1D8C2): [id] long -> [min(stage,5)] long -> [band] {p,100-p} --- */
    { "run_strike_rolls",         "base/ai", 0x238CE, 0x23B26 - 0x238CE, TK_U8, 16,
      "0x1D8C2 run phase 1 vs a standing man: [wrestler id] long -> [stage 0..5] long -> [band 0..2] pair; 0 = running strike 0x72 (gate 0x48)" },
    { "run_counter_rolls",        "base/ai", 0x23B26, 0x23D7E - 0x23B26, TK_U8, 16,
      "0x1D8C2 run phase 1 vs a running/charging man (+0x33 b4 or move 3): [wrestler id] long -> [stage 0..5] long -> [band 0..2] pair; 0 = counter 0x75 (gate 0x50)" },
    /* --- lying man vs the attacker's move (0x1DB28 row) --- */
    { "downed_escape_rolls",      "base/ai", 0x23D7E, 0x24062 - 0x23D7E, TK_U8, 16,
      "0x1DB28 downed-man rolls, heads: 0x23D7E cover 08 [stage] long -> [band] pair (0x1DB60); 0x23DD0 11/0C/12/0F (0x1DDA2); 0x23E16 [7-move list idx] long -> [ctr +0xD2.. 0..5] pair (0x1DD2A); 0x23E76 move 0A [stage] long -> [ctr +0xE5 0..4] pair (0x1DCD8); 0x23ED0 (0x1C79E); 0x23F78 10/46/0B (0x1DB9A); 0x23FC4 09/22; 0x24010 (0x1C818); code 0x24062" },
    { "rumble_downed_escape_rolls", "base/ai", 0x1DE48, 12, TK_U8, 2,
      "0x1DBE4 / 0x1DB58 rumble (no stage): rows 0..2 = band pairs vs the ground holds 10/46/0B, rows 3..5 = band pairs vs the cover 08 (0x1DE4E); 0x1DE54 = the 7-move list" },
    /* --- small rolls and timers (all {p,100-p} pairs unless noted) --- */
    { "runin_strike_roll",        "base/ai", 0x1D4FC, 10 * 4, TK_U8, 4,
      "0x1D498 AI sub 0x1D398 (the APRON follow: 0x1D3A2 tracks my legal man's y) vs the enemy legal man standing/staggered within dx < 0x40, dy < 0x10: [stage 0..9] {apron punch 0x39, over-the-ropes grab 0x1C, nothing, pad} d100 (0x24CC), once per approach (+0xB7 b1); code 0x1D524" },
    { "run_away_roll",            "base/ai", 0x1D030, 2, TK_U8, 2,
      "0x1CEB4 bucket-2 vs a grapple-ready opponent: 0 = run away (0x1F344); code 0x1D032" },
    { "weapon_throw_roll",        "base/ai", 0x1D066, 2, TK_U8, 2,
      "0x1D046 armed CPU at range 0x68..0x90: 0 = swing/throw the weapon from distance (move 0x1F, 0x1D058)" },
    { "outside_chase_rolls",      "base/ai", 0x1C8F0, 4, TK_U8, 2,
      "row 0 (0x1C9C2) legal man with the opposing legal man outside: 0 = pend a rope move, 1 = chase 0x3D; row 1 (0x1C398); code 0x1C8F4" },
    { "toprope_alt_dive_roll",    "base/ai", 0x1EF90, 2, TK_U8, 2,
      "0x1EF42 state 9 perch (ids 4/7/0xB): 0 = the alternate dive (move 1); followed by the per-id dive tables 0x1EF92/0x1EF9E (not read here), code 0x1EFAA" },
    { "lockup_knee_roll",         "base/ai", 0x1F05E, 2, TK_U8, 2,
      "0x1F01E lockup 0x0B timer expiry: 0 = the tie-up knee 0x29 (cat 8 B1)" },
    { "lockup_timer_by_stage",    "base/ai", 0x1F060, 10, TK_U8, 1,
      "0x1F04A lockup 0x0B: +0xBD reload by stage 0..9 (+ rng&3) when the partner is not human/autopilot; code 0x1F06A" },
    { "tag_wants_dizzy_roll",     "base/ai", 0x1F458, 2, TK_U8, 2,
      "0x1F422 wants-a-tag (+0xB5 b6) for a dizzy man (lying react 1): 0 = wants; code 0x1F45A" },
    { "hold_escape_timer_ptrs",   "base/ai", 0x1F576, 6 * 4, TK_U32, 1,
      "0x1F562 scripted-hold escape (+0xBA): [D0 0..5 hold class] long -> hold_escape_timers row" },
    { "hold_escape_timers",       "base/ai", 0x1F58E, 6 * 3 * 2, TK_U16, 3,
      "0x1F56E: [hold class 0..5][band 0..2] frames before the CPU victim rewrites the move into the escape (0x1E3F2); code 0x1F5B2" },
    /* --- per-wrestler rows --- */
    { "ws_run_in_roll",           "wrestler", 0x1F3A6, 12 * 2, TK_U8, 2,
      "0x1F37A run-eval dx >= 0xC8: [wrestler id] {p,100-p}, 0 = run in at him (once per approach, +0xB6 b1)" },
    { "ws_run_away_roll",         "wrestler", 0x1F3BE, 12 * 2, TK_U8, 2,
      "0x1F330 / 0x1C4A8 run-eval dx >= 0xE0: [wrestler id] {p,100-p}, 0 = run away (once, +0xB6 b2); code 0x1F3D6" },
    { "ws_toprope_roll",          "wrestler", 0x1F2E0, 12 * 4, TK_U8, 4,
      "0x1F202 top-rope intent: [wrestler id] {pair when the opponent's band low byte == 1, pair otherwise}; 0 = go climb; code 0x1F310" },
    { "ws_rescue_cascade_move",   "wrestler", 0x1F150, 12, TK_U8, 1,
      "0x1F096 hold cascade in rescue mode (+0xB5 b7): the move per wrestler id instead of the policy record; code 0x1F15C" },
    { "ws_run_strike_gate",       "wrestler", 0x1E582, 12, TK_U8, 1,
      "0x1E4F6 prep of running strike 0x20: +0xBD distance gate by the TARGET's wrestler id (then -0x22 if he is not running, 0x1E568)" },
    { "ws_run_strike_alt_gate",   "wrestler", 0x1E58E, 12, TK_U8, 1,
      "0x1E4DA prep of running strike 0x2D: +0xBD distance gate by the TARGET's wrestler id; code 0x1E59A" },
    { "ws_pin_intent_chance",     "wrestler", 0x110C8, 12 * 2, TK_U16, 1,
      "0x1108C knockdown pin intent (+0x34 b4): word per wrestler id, rng&0xFF < word -> cover; code 0x110E0" },
};
TBL_REGISTER(ai_tables)

/* ---------------------------------------------------------------- RNG */
/* 0x21B4: D0 = $1C005C + D1..D7 + $1C0080 (frame); ror.l by (D0 & 7);
 * store. $1C005C is never seeded (boot RAM clear → 0). The D1..D7 sum is
 * caller garbage except where noted at the call sites (rng-lockup.md
 * §1b); `known` carries the part we can reproduce, the rest is K=0. */
static uint32_t rng_state = 0;             /* $1C005C */
static uint32_t rng_frame = 0;             /* $1C0080 low long */
uint32_t eng_rng_fold(uint32_t known)
{
    uint32_t s = rng_state + known + rng_frame;
    unsigned n = s & 7u;
    s = n ? ((s >> n) | (s << (32 - n))) : s;
    rng_state = s;
    return s;
}
uint32_t eng_rng(void) { return eng_rng_fold(0); }
void eng_ai_frame(eng_state *st) { rng_frame = (uint32_t)st->frame; }

/* 0x24CC: up to 4 draws of (rng & 0xFF) >> 1 until < 100 (else 0x32), 0 →
 * 1, then walk the weight bytes. D1 (the retry counter 3..0) is the one
 * register the ROM adds that we know. */
static int d100_walk(const uint8_t *w, int n)
{
    int roll = 0x32, acc = 0, k;
    for (k = 3; k >= 0; k--) {
        roll = (int)((eng_rng_fold((uint32_t)k) & 0xFFu) >> 1);
        if (roll < 100) break;
        roll = 0x32;
    }
    if (roll == 0) roll = 1;
    for (k = 0; k < n; k++) { acc += w[k]; if (acc >= roll) return k; }
    return n - 1;
}
/* {p, 100-p} pair at `a`: 0 = first bucket hit */
static int pair_roll(uint32_t a) { return d100_walk(tbl_ra_ptr(a, 2), 2); }

static uint32_t rom32(uint32_t a)
{ return ((uint32_t)tbl_ra8(a) << 24) | ((uint32_t)tbl_ra8(a+1) << 16) | ((uint32_t)tbl_ra8(a+2) << 8) | tbl_ra8(a+3); }

/* --------------------------------------------------------- policies */
/* 0x1DEC0–0x1DEE4 band row: record [n][ids][w0][w1][w2]; row by the
 * opponent's band (+0x70 of `p`), forced to 2 under 10 minutes. */
static unsigned rec_pick(const eng_state *st, uint32_t rec, const eng_obj *p)
{
    unsigned n = tbl_ra8(rec);
    const uint8_t *ids = tbl_ra_ptr(rec + 1, n), *w = tbl_ra_ptr(rec + 1 + n, 3u * n);
    unsigned band = p->band & 0xFFu;
    if (n == 0 || n > 16) return 0xFDu;      /* malformed: do nothing */
    if (st->clk_min < 0x10) band = 2;        /* 0x1DEC8 */
    if (band) w += n << (band >> 1);
    return ids[d100_walk(w, (int)n)];
}
/* ---- WF_AISTATS: what the CPU actually does (per object, dumped at the end
 * of a headless run - tools/ai_bench.sh). Numbers first, then opinions. */
static const eng_state *cur_st;   /* the frame's state (set by ai_inputs_raw; declared here for AST) */
typedef struct { unsigned rolls, memo, flips, fired, withheld, landed, idle, steer_flips, pile_armed, pile_done, frames, walk_starts; uint8_t last_mv, last_s8; uint16_t last_key; } ai_stat_t;
static ai_stat_t ai_stats[ENG_MAX_OBJS];
#define AST(o) (ai_stats[(o) - cur_st->obj])
void eng_ai_stat_landed(const eng_state *st, const eng_obj *a) { int i = (int)(a - st->obj); if (i >= 0 && i < ENG_MAX_OBJS && a->cpu) ai_stats[i].landed++; }
void eng_ai_stat_pile(const eng_state *st, const eng_obj *h, int done) { int i = (int)(h - st->obj); if (i >= 0 && i < ENG_MAX_OBJS) { if (done) ai_stats[i].pile_done++; else ai_stats[i].pile_armed++; } }
void eng_ai_stats_dump(const eng_state *st)
{
    ai_stat_t tot = {0};
    fprintf(stderr, "ai-stats: slot  cpu-frames rolls memo flips fired withheld landed idle steer-flips walk-starts pile(armed/done)\n");
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        const ai_stat_t *a = &ai_stats[i];
        if (!a->frames) continue;
        fprintf(stderr, "ai-stats: o%-2d  %8u %5u %4u %5u %5u %8u %6u %5u %11u %11u %u/%u\n", i, a->frames, a->rolls, a->memo, a->flips, a->fired, a->withheld, a->landed, a->idle, a->steer_flips, a->walk_starts, a->pile_armed, a->pile_done);
        tot.frames += a->frames; tot.rolls += a->rolls; tot.memo += a->memo; tot.flips += a->flips; tot.fired += a->fired; tot.withheld += a->withheld; tot.landed += a->landed; tot.idle += a->idle; tot.steer_flips += a->steer_flips; tot.walk_starts += a->walk_starts; tot.pile_armed += a->pile_armed; tot.pile_done += a->pile_done;
    }
    if (tot.frames)
        fprintf(stderr, "ai-stats: ALL  %8u %5u %4u %5u %5u %8u %6u %5u %11u %u/%u   per 1000 cpu-frames: rolls %.1f flips %.1f fired %.1f landed %.1f idle %.0f steer-flips %.1f walk-starts %.1f\n",
                tot.frames, tot.rolls, tot.memo, tot.flips, tot.fired, tot.withheld, tot.landed, tot.idle, tot.steer_flips, tot.pile_armed, tot.pile_done,
                tot.rolls * 1000.0 / tot.frames, tot.flips * 1000.0 / tot.frames, tot.fired * 1000.0 / tot.frames, tot.landed * 1000.0 / tot.frames, tot.idle * 1000.0 / tot.frames, tot.steer_flips * 1000.0 / tot.frames, tot.walk_starts * 1000.0 / tot.frames);
    (void)st;
}

/* MOD ai_commit: the policy tables are rolled every free frame in stock, so
 * a CPU that has not fired yet re-decides constantly (the flip-flopping). A
 * committed pick is remembered for up to ai_commit frames and re-issued as
 * long as the SITUATION it was taken in holds (same table, ctx, opponent
 * state class, range bucket); a fire, or a change, frees it. The wrestler's
 * own tables still say what he does - only HOW OFTEN he changes his mind. */
static unsigned opp_class(const eng_obj *p)
{
    unsigned ps = p->state & 0xFFu, rc = p->react_id & 0xFFu;
    if (ps == ST_REACT) return (rc == RC_LYING || rc == RC_LYING_B) ? 3u : 2u;
    if (ps == ST_MOVE) return 4u;
    if (ps == ST_HELD || ps == ST_HOLD || ps == ST_LOCKUP) return 5u;
    if (ps == ST_RUN) return 6u;
    return 1u;                                      /* stand / walk / the rest */
}
static unsigned committed_pick(const eng_state *st, eng_obj *o, const eng_obj *p, unsigned table, unsigned ctx, uint32_t rec)
{
    int commit = eng_mod_rule(MODR_AI_COMMIT);
    unsigned mv;
    uint16_t key = (uint16_t)((table << 12) | (ctx << 8) | (opp_class(p) << 4) | (p ? (unsigned)((labs((p->x >> 16) - (o->x >> 16)) < 0x50) ? 0 : (labs((p->x >> 16) - (o->x >> 16)) < 0xB0) ? 1 : 2) : 0));
    if (commit > 0 && o->ai_dec_t && o->ai_dec_key == key) { AST(o).memo++; return o->ai_dec_mv; }
    mv = rec_pick(st, rec, p);
    AST(o).rolls++;
    if (AST(o).last_key == key && AST(o).last_mv != (uint8_t)mv) AST(o).flips++;   /* same situation, different answer */
    AST(o).last_key = key; AST(o).last_mv = (uint8_t)mv;
    /* commit REAL moves only: the idle / wait / reposition answers (0xFD-0xFF,
     * 0x3C-0x3E) carry the ROM's own cooldowns and sub-modes, and holding
     * one of those for 32 frames just parks the man (bench: fired 2.5 -> 1.6) */
    if (commit > 0 && mv < 0xF0u && mv != 0x3Cu && mv != 0x3Du && mv != 0x3Eu) { o->ai_dec_t = (uint8_t)(commit > 255 ? 255 : commit); o->ai_dec_mv = (uint8_t)mv; o->ai_dec_key = key; }
    else o->ai_dec_t = 0;
    return mv;
}
/* 0x1DEA6 with A2 = 0x219B4 (ai_move_policy) */
static unsigned policy_pick(const eng_state *st, eng_obj *o, const eng_obj *p, unsigned ctx)
{
    uint32_t base = tbl32(TBL(move_policy), (uint32_t)eng_ws_base(o->wrestler) * 4u);
    return committed_pick(st, o, p, 1u, ctx, rom32(base + ctx * 4u));
}
/* 0x1DEA6 with A2 = 0x2307C (ai_move_policy2: opponent mid-grapple-move) */
static unsigned policy2_pick(const eng_state *st, eng_obj *o, const eng_obj *p, unsigned ctx)
{
    uint32_t base = tbl32(TBL(move_policy_grapple), (uint32_t)eng_ws_base(o->wrestler) * 4u);
    return committed_pick(st, o, p, 2u, ctx, rom32(base + ctx * 4u));
}
/* x-zone +0x31 (0x1F0D6): <3 left edge, >=9 right edge. TODO EXACT: the
 * writer of +0x31 was not found; 0x20-px columns from x=0x1A0 put the
 * rope lines (0x1C4/0x34C) in columns 1 and 13. */
static unsigned xzone(const eng_obj *o)
{
    int32_t x = (o->x >> 16) - 0x1A0;
    return x < 0 ? 0u : (unsigned)(x >> 5);
}
/* 0x1F078 hold cascade (lockup winner): policy[id] slot 1 → [facing] →
 * [zone 0/4/8] → record, band row from the GRAPPLE PARTNER's +0x70. */
static unsigned cascade_pick(const eng_state *st, const eng_obj *o, const eng_obj *v)
{
    uint32_t a3 = tbl32(TBL(move_policy), (uint32_t)eng_ws_base(o->wrestler) * 4u);
    uint32_t a2 = rom32(a3 + 4u);
    uint32_t rec;
    unsigned z = xzone(o), zoff = z < 3 ? 0u : z >= 9 ? 8u : 4u;
    a3 = rom32(a2 + ((o->facing & 0x8000u) ? 4u : 0u));
    rec = rom32(a3 + zoff);
    if (o->ai_b5 & 0x80u)                    /* rescue mode: 0x1F150[id] */
        return tbl8(TBL(ws_rescue_cascade_move), (unsigned)eng_ws_base(o->wrestler));
    return rec_pick(st, rec, v);
}

/* ------------------------------------------------ controller glue */
/* Which button column yields move id `mv` in the human map row for `cat`
 * (0xE4FE rows: col 0 = B1, 1 = B2, 2 = both). -1 if none. */
static int column_for(const eng_obj *o, unsigned cat, unsigned mv)
{
    for (unsigned c = 0; c < 3; c++)
        if (eng_ws_move8((unsigned)o->wrestler, cat * 3u + c) == mv) return (int)c;
    return -1;
}
static uint32_t col_bits(int c) { return c == 0 ? 0x10u : c == 1 ? 0x20u : c == 2 ? 0x30u : 0u; }
static uint32_t dir_bits(int32_t dx, int32_t dy)    /* 8-way toward (dx,dy) */
{
    uint32_t b = 0;
    if (dx > 0) b |= 1u; else if (dx < 0) b |= 2u;
    if (dy > 0) b |= 4u; else if (dy < 0) b |= 8u;
    return b;
}
/* Hold button bits for `frames` frames: the 0x514E digest only latches a
 * press that lands on an odd frame, and the run steer (0xF34C) reads the
 * stick on the tick after the state latch. */
static uint32_t press(eng_obj *o, uint32_t bits, int frames)
{
    o->ai_press = bits;
    o->ai_press_t = (uint8_t)(frames > 0 ? frames - 1 : 0);
    return bits;
}

/* geometry of one tick (0x1C228 range_linked) */
typedef struct {
    int32_t dx, dy, adx, ady;   /* opp - self */
    unsigned b4;                /* range bucket: 0 <0x50, 1 <0xB0, 2 */
    int opp_right, opp_below;   /* +0xB5 b0/b1 */
    int face_diff;              /* facings differ (facing each other) */
} geo_t;
static void geometry(const eng_obj *o, const eng_obj *p, geo_t *g)
{
    int32_t px = p->x >> 16, oy = o->y >> 16;
    /* 0x1C236-0x1C266: a LYING target is measured at his head point —
     * react 8 biases x by 0x40, react 9 by 0x28, toward his facing. */
    if ((p->state & 0xFFu) == ST_REACT) {
        unsigned rc = p->react_id & 0xFFu;
        int32_t d1 = rc == RC_LYING ? 0x40 : rc == RC_LYING_B ? 0x28 : 0;
        if (d1) px += (p->facing & 0x8000u) ? -d1 : d1;    /* 0x1C25A-0x1C266 */
    }
    /* 0x1C280-0x1C290: a target mounted on (state 5 move 0x22) is
     * measured 8 below own y. */
    if ((p->state & 0xFFu) == ST_MOVE && (p->move_id & 0xFFu) == 0x22)
        oy += 8;
    g->dx = px - (o->x >> 16); g->dy = (p->y >> 16) - oy;
    g->adx = g->dx < 0 ? -g->dx : g->dx; g->ady = g->dy < 0 ? -g->dy : g->dy;
    g->b4 = g->adx < 0x50 ? 0u : g->adx < 0xB0 ? 1u : 2u;
    g->opp_right = g->dx >= 0; g->opp_below = g->dy >= 0;
    g->face_diff = ((o->facing ^ p->facing) & 0x8000u) != 0;
}
static int opp_lying(const eng_obj *p)
{ return (p->state & 0xFFu) == ST_REACT && ((p->react_id & 0xFFu) == RC_LYING || (p->react_id & 0xFFu) == 9); }
static int opp_standing_react(const eng_obj *p)   /* react 0/1/0xA */
{ unsigned r = p->react_id & 0xFFu; return (p->state & 0xFFu) == ST_REACT && (r == 0 || r == 1 || r == 0xA); }
static int opp_move_in(const eng_obj *p, const uint8_t *list, int n)
{
    unsigned m = p->move_id & 0xFFu;
    if ((p->state & 0xFFu) != ST_MOVE) return 0;
    for (int i = 0; i < n; i++) if (list[i] == m) return 1;
    return 0;
}

static void ai_log(const eng_obj *o, const char *what, unsigned v)
{
    if (eng_dbgsel) fprintf(stderr, "ai: o%d %s %02X\n", cur_st ? (int)(o - cur_st->obj) : -1, what, v);
}

/* ------------------------------------------- distance gates (+0xBC) */
/* Per-move prep handlers 0x1E018[mv]: the +0xBC gate a queued move waits
 * for (0x1C5B6 fires when facing each other and dx < gate). */
static int prep_gate(const eng_obj *o, const eng_obj *p, unsigned mv)
{
    int g;
    unsigned oid = (unsigned)eng_ws_base(p->wrestler);   /* victim gates: base id */
    switch (mv) {
    case 0x2D: g = (int)tbl8(TBL(ws_run_strike_alt_gate), oid); break;          /* 0x1E4DA */
    case 0x20: g = (int)tbl8(TBL(ws_run_strike_gate), oid); break;          /* 0x1E4F6 */
    case 0x41: g = 0x5C + (oid == 7 ? 4 : 0); break;       /* 0x1E512 */
    case 0x2A: g = 0x60; break;                            /* 0x1E52A */
    case 0x40: case 0x04: g = 0x70; break;                 /* 0x1E532 */
    case 0x05: g = (oid == 7) ? 0x80 : 0x78;               /* 0x1E53A */
               if (eng_ws_base(o->wrestler) == 8) { g -= 6; }
               break;
    case 0x01: return 0x80;                                /* 0x1E6FE */
    case 0x03: g = 0x70; if (oid != 7) { g -= 8; if (oid == 8) g -= 8; } return g;   /* 0x1E70E */
    case 0x07: case 0x27: return 0x70;                     /* 0x1E73E */
    case 0x2E: case 0x31: case 0x06: return 0x60;          /* 0x1E74E */
    case 0x00: case 0x21: return 0x70;                     /* 0x1E2BE (opp grapple-ready) */
    default: return 0x50;                                  /* TODO EXACT: other rows */
    }
    if (!(p->role & RF_RUNNING)) g -= 0x22;                      /* 0x1E568: opp not running */
    return g;
}

/* ------------------------------------------------ walk-to targets */
static void set_target(eng_obj *o, int32_t x, int32_t y, unsigned mv)
{
    o->ai_tx = (int16_t)x; o->ai_ty = (int16_t)y;
    o->ai_mv = (uint8_t)mv; o->ai_sub = 0xA; o->ai_sub_t = 0;
    o->ai_b5 &= (uint8_t)~0x20u;                           /* 0x1F550 */
}
/* 0x1F45A: beside the downed man for a stomp/cover/hold */
static void target_1F45A(eng_obj *o, const eng_obj *p, unsigned mv)
{
    int32_t d0 = 0x48, d1, d2, px = p->x >> 16, py = p->y >> 16;
    int pinned = (p->state & 0xFFu) == ST_HELD || (p->cue_flags & 1u)
              || ((p->state & 0xFFu) == ST_MOVE && (p->move_id & 0xFFu) == 9);
    if (pinned) {                                          /* 0x1F47E */
        d1 = px;
        if ((p->state & 0xFFu) == ST_MOVE && (p->move_id & 0xFFu) == 0x1A)
            d1 += (p->facing & 0x8000u) ? -0x20 : 0x20;
        else
            d1 += (p->facing & 0x8000u) ? 0x20 : -0x20;
        d2 = py + (py < 0x160 ? 0x10 : -0x10);
        set_target(o, d1, d2, mv);
        return;
    }
    d2 = py;                                               /* 0x1F4D6 */
    if (!((p->react_id & 0xFFu) == RC_LYING || ((p->state & 0xFFu) == ST_MOVE && (p->move_id & 0xFFu) == 0x61))) {
        d2 -= 0x18; d0 = 0x30;
    }
    d1 = px + ((p->facing & 0x8000u) ? -d0 : d0);
    set_target(o, d1, d2, mv);
}
/* 0x1F4CE: 0xA0 beside the lying man (pickup 0x08 / wait 0x3D / drops) */
static void target_1F4CE(eng_obj *o, const eng_obj *p, unsigned mv)
{
    int32_t d0 = 0xA0, d2 = p->y >> 16, d1;
    if (mv == 0x3D) d2 += 0x10;
    else if (!((p->react_id & 0xFFu) == RC_LYING || ((p->state & 0xFFu) == ST_MOVE && (p->move_id & 0xFFu) == 0x61))) {
        d2 -= 0x18; d0 = 0x60;
    }
    d1 = (p->x >> 16) + ((p->facing & 0x8000u) ? -d0 : d0);
    set_target(o, d1, d2, mv);
}
/* 0x1F51E: 8 px on the head side (ground holds 0x09/0x22) */
static void target_1F51E(eng_obj *o, const eng_obj *p, unsigned mv)
{
    int32_t d1 = (p->x >> 16) + ((p->facing & 0x8000u) ? 8 : -8);
    set_target(o, d1, p->y >> 16, mv);
}

/* Queue a real move: run its prep-handler routing (0x1E018 rows that
 * retarget through a walk) and otherwise arm the follow-up gate. */
static void queue_move(eng_state *st, eng_obj *o, const eng_obj *p, unsigned mv)
{
    int32_t px = p->x >> 16;
    o->ai_mv = (uint8_t)mv;
    if (mv != 0x48) o->tag_flags &= (uint16_t)~0x10u;   /* committing to anything but the cover drops the pin
                                                     intent: stock consumes it into move 0x79 at the next
                                                     idle decision (0x1C348) and clears it when 0x79 ends
                                                     (0x1E282) / a walk starts (0x11AD0); the engine has no
                                                     0x79, so a stale bit held the lying man down for
                                                     hundreds of frames (stuck-downed men in rumble) */
    switch (mv) {
    case 0x08:                                             /* 0x1E5F6 pickup */
        if ((p->facing & 0x8000u) ? px >= 0x260 : px < 0x290) { target_1F4CE(o, p, 0x08); return; }
        target_1F45A(o, p, 0x0A); return;                  /* no room: stomp */
    case 0x14: case 0x46: case 0x10: case 0x13:            /* 0x1E62C drops */
        if ((p->facing & 0x8000u) ? px >= 0x250 : px < 0x2A0) { target_1F4CE(o, p, mv); return; }
        target_1F45A(o, p, 0x0A); return;
    case 0x0A: case 0x47: case 0x0B:                       /* 0x1E664 */
        target_1F45A(o, p, mv); return;
    case 0x48:                                             /* 0x1E6A4 cover */
        if (!(o->role & RF_LEGAL) && (p->role & RF_LEGAL)) mv = 0x0A;   /* illegal man: stomp */
        target_1F45A(o, p, mv); return;
    case 0x09: case 0x22:                                  /* 0x1E5AE ground holds */
        if (px >= 0x200 && px < 0x300) { target_1F51E(o, p, mv); return; }
        target_1F45A(o, p, mv); return;
    case 0x3D:                                             /* 0x1EBCE → 0x1F4CE */
        target_1F4CE(o, p, 0x3D); return;
    case 0x00: case 0x21:                                  /* 0x1E2F6: the grab */
        if (!(p->role & RF_RUNNING)) { o->ai_sub = 0; return; } /* runs now */
        /* opp grapple-ready → 0x1E2BE gate 0x70 */
        /* fall through */
    case 0x01: case 0x03: case 0x07: case 0x27: case 0x2E: case 0x31: case 0x06:
        o->ai_bc = (uint16_t)prep_gate(o, p, mv);          /* 0x1E75C: follow-up armed */
        o->ai_b5 |= 0x08u; o->ai_b7 |= 0x0Cu;
        o->ai_sub = 0;
        return;
    default:                                               /* plain strikes: run now */
        o->ai_sub = 0;
        return;
    }
    (void)st;
}

/* Move ids the engine has no handler for yet (docs/gap-map.md §1): a
 * press that would resolve to one of these is withheld — the ROM would
 * run them, the engine would print UNROUTED and bail. */
static int routed(unsigned mv)
{
    /* 0x1C / 0x1D (the behind grabs) were denied here from 2026-08-23, before
     * handler_behind1C/1D existed; with them denied a CPU behind a dizzy man
     * rolled the dead behind row forever (WF_AISTATS 2026-09-06: o7 withheld
     * 116 of 143 decisions in one rumble) - re-allowed */
    static const uint8_t deny[] = { 0x32,0x89,0x3B,0x39,
        0x3F,0x45,0x49,0x54,0x64,0x6C,0x7A,0x8D,0x3C,0x3E,0x50,0x61,0x6B,0x6E,0x6F,0x7B,0x88,0x8E,
        0x7D,0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87 };
    for (unsigned i = 0; i < sizeof deny; i++) if (deny[i] == mv) return 0;
    return mv != 0xFFu;
}
/* The category core.c walk_logic will resolve for a press right now
 * (0xDF3A chain: E27A perch, E02E running, E0B8/E110 downed, E2DC
 * anti-run, E382 behind, E180 tie-up, E4BA fallback). */
static int engine_cat(const eng_obj *o, const eng_obj *p)
{
    unsigned st = o->state & 0xFFu, ps = p->state & 0xFFu, rc = p->react_id & 0xFFu;
    unsigned bank = (((o->alt62 >> 7) & 1u) ^ 1u);         /* bchg happens on the press */
    if (st == 9) {
        if (ps == ST_REACT && (rc == RC_LYING || rc == RC_LYING_B)) return 0x0F;
        if (ps == ST_STAND || ps == ST_WALK || (ps == ST_REACT && rc == RC_DIZZY)) return 0x0E;
        return -1;
    }
    if (st == 2) return (int)tbl8(TBL(run_attack_matrix), bank * 0x24u + (unsigned)p->band * 0xCu + (unsigned)eng_ws_base(o->wrestler));
    if (st != 0 && st != 1) return st == 0x0B ? 8 : -1;
    if (o->weapon_w & WPN_HELD) return 0x11;                     /* 0xE002: holding a weapon
                                                              (the chain's first row) */
    if (ps == ST_HOLD && p->pinning) return 6;                /* pin break, see core.c */
    if (p->partner < 0 && ps == ST_REACT && (rc == RC_LYING || rc == RC_LYING_B)) return rc == RC_LYING ? 5 : (p->band == 0 ? 6 : 7);
    if (ps == ST_RUN || (p->role & RF_RUNNING))                       /* 0xE2EA: the RUNNING FLAG, not state 2 - a running
                                          strike (state 5, 0x05/0x2A/0x2D/0x20/0x41) opens the anti-run row too,
                                          so the policy's duck 0x06 fires instead of degrading to a punch
                                          ("the CPU DUCKED my running clothesline" in MAME, user 2026-08-30) */
        return (int)tbl8(TBL(run_catch_matrix), bank * 0x24u + (unsigned)p->band * 0xCu + (unsigned)eng_ws_base(o->wrestler));
    if (ps == ST_REACT && rc == RC_DIZZY && !((o->facing ^ p->facing) & 0x8000u)
        && abs((int)(p->y >> 16) - (int)(o->y >> 16)) < 0x10
        && abs((int)(p->x >> 16) - (int)(o->x >> 16)) < 0x50) return 0x10;
    return 0;
}
/* Fire the queued move through the selector: the button column whose
 * 0xE4FE entry is `mv` in the category the engine resolves; if the row
 * lacks it, the nearest routed column (B1 first, B2 for 0x48/0x72). */
static uint32_t fire_move(eng_state *st, eng_obj *o, const eng_obj *p, unsigned mv)
{
    int cat = engine_cat(o, p), c = -1;
    o->ai_b5 &= (uint8_t)~0x08u;
    o->ai_sub = 0;
    if (mv != 0x48) o->tag_flags &= (uint16_t)~0x10u;   /* see queue_move */
    (void)st;
    if (cat < 0) return 0;
    {   /* package override, else the ROM row (eng_ws_move8) */
    unsigned mrow = (unsigned)cat * 3u;
    if ((o->state & 0xFFu) == ST_RUN && (mv == 0x72 || mv == 0x75))
        c = (mv == 0x72) ? 1 : 0;                          /* TODO EXACT: B2/B1 stand-ins */
    else
        c = column_for(o, (unsigned)cat, mv);
    if (c >= 0 && !routed(eng_ws_move8((unsigned)o->wrestler, mrow + (unsigned)c))) c = -1;
    if (c < 0) {
        int pref = (mv == 0x48 || mv == 0x72) ? 1 : 0;
        if (routed(eng_ws_move8((unsigned)o->wrestler, mrow + (unsigned)pref))) c = pref;
        else if (routed(eng_ws_move8((unsigned)o->wrestler, mrow + (unsigned)(pref ^ 1)))) c = pref ^ 1;
    }
    }
    if (c < 0) { ai_log(o, "withheld (unrouted cat)", (unsigned)cat); AST(o).withheld++; return 0; }
    ai_log(o, "fire", mv);
    AST(o).fired++; o->ai_dec_t = 0;                       /* the decision is spent */
    return press(o, col_bits(c), 2);
}

/* ------------------------------------------------------- run-eval */
/* 0x1D7A0 + run launch: both buttons = run (cat 0 col 2 = 0xFF), the
 * stick picks the direction on the steer tick (0xF37C/0xF38C). */
static uint32_t run_launch(eng_obj *o, int toward_right)
{
    o->ai_b7 &= (uint8_t)~0x02u;                           /* 0x1D7A0 callers clr b1 */
    o->ai_run_own = 1;                                     /* engine: this run is the AI's (a human-started run is stopped) */
    return press(o, 0x30u | (toward_right ? 1u : 2u), 3);
}
static uint32_t run_away(eng_obj *o, const geo_t *g)       /* 0x1F344 / 0x1CE96 */
{
    o->ai_b6 |= 0x20u;                                     /* engine: +0x32 b2 "running away" */
    ai_log(o, "run-away", 0);
    return run_launch(o, !g->opp_right);
}
/* 0x1F310: returns 1 = keep walking, 0 = a run was launched (bits in *out) */
static int run_eval(eng_obj *o, const geo_t *g, uint32_t *out)
{
    unsigned id = (unsigned)eng_ws_base(o->wrestler);
    if (g->adx >= 0xE0) {
        if (o->role & RF_OUTSIDE) return 1;                      /* outside */
        if (o->ai_b6 & 0x04u) return 1;                    /* once */
        o->ai_b6 |= 0x04u;
        if (pair_roll(0x1F3BEu + id * 2u) != 0) return 1;
        *out = run_away(o, g);
        return 0;
    }
    if (g->adx >= 0xC8) {
        if (o->ai_b6 & 0x02u) return 1;
        o->ai_b6 |= 0x02u;
        if (pair_roll(0x1F3A6u + id * 2u) != 0) return 1;
        ai_log(o, "run-in", 0);
        *out = run_launch(o, g->opp_right);
        return 0;
    }
    return 1;
}

/* -------------------------------------------------- top-rope intent */
/* 0x1F1AC: corner from the opponent's x/y by his facing; 0x1F2E0[id] roll
 * (first pair when his band low byte == 1, else the second). */
static int toprope_eval(eng_state *st, eng_obj *o, eng_obj *p)
{
    int32_t px = p->x >> 16, py = p->y >> 16;
    int c = -1;
    uint32_t a;
    if (st->scene == 1 || (o->ai_b6 & 0x01u) || p->band == 0 || (o->driver & DRV_AUTOPILOT)) return 0;
    if (!(p->facing & 0x8000u)) {                          /* opp faces left */
        if (px < 0x210) { if (py >= 0x170) c = 0; else if (px < 0x200 && py < 0x150) c = 2; }
        else if (px >= 0x260) { if (py >= 0x170) c = 1; else if (px >= 0x268 && py < 0x150) c = 3; }
    } else {                                               /* 0x1F290 faces right */
        if (px < 0x288) { if (py >= 0x170) c = 0; else if (px >= 0x278 && py < 0x150) c = 2; }
        else if (px >= 0x2D8) { if (py >= 0x170) c = 1; else if (px >= 0x2E0 && py < 0x150) c = 3; }
    }
    if (c < 0) return 0;
    a = 0x1F2E0u + (uint32_t)eng_ws_base(o->wrestler) * 4u + (((p->band & 0xFFu) == 1) ? 0u : 2u);
    if (pair_roll(a) != 0) return 0;
    p->down_t = (uint16_t)(p->down_t + 0x100u);           /* 0x1F222: hold him down */
    /* 0x1F228 +0xAB += 0x14 on the mash meter: TODO EXACT (engine meter
     * counts presses, not strength) */
    o->ai_bc = (uint16_t)c;
    o->ai_sub = 9; o->ai_sub_t = 0;
    ai_log(o, "toprope corner", (unsigned)c);
    return 1;
}
/* sub 9 (0x1D31E): walk to 0x1D388[c]; on arrival, corner free and opp
 * still down → claim (0xEECE) and climb (state 8). Direct write, as the
 * ROM does — the human gate 0xEDC0 is bypassed by the AI too. */
static uint32_t corner_walk(eng_state *st, eng_obj *o, const eng_obj *p)
{
    static const int16_t T[4][2] = { {0x1F8,0x1A0},{0x300,0x1A0},{0x1D0,0x120},{0x320,0x120} };
    unsigned c = o->ai_bc & 3u;
    int32_t dx = T[c][0] - (o->x >> 16), dy = T[c][1] - (o->y >> 16);
    int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (!opp_lying(p) || ++o->ai_sub_t > 0x180) { o->ai_sub = 0; return 0; }   /* he got up */
    if (adx < 8 && ady < 8) {
        if (st->corner_bits & (1u << c)) { o->ai_sub = 0; return 0; }
        st->corner_bits |= (uint16_t)(1u << c);
        o->grap44 = (uint16_t)c; o->atk = 0; o->state = ST_CLIMB;
        o->ai_bd = 0;                                      /* 0x1EE98 */
        o->ai_sub = 0;
        ai_log(o, "climb", c);
        return 0;
    }
    return dir_bits(adx >= 8 ? dx : 0, ady >= 8 ? dy : 0);
}
/* state 9 (0x1EEA0): wait 8 ticks, then the dive: 0x1EF9E[id] on a
 * standing man, 0x1EF92[id] on a lying one — these ARE the cat E/F B1
 * entries of 0xE4FE, so B1 reproduces the pick; ids 4/7/0xB roll 0x1EF90
 * {40,60} for move 1 (w4: cat E col 1; w7/wB have no column — TODO EXACT). */
static uint32_t perch(eng_obj *o, const eng_obj *p)
{
    unsigned id = (unsigned)eng_ws_base(o->wrestler);
    int col = 0;
    if (o->ai_bd < 8) { o->ai_bd++; return 0; }
    if (!((p->state & 0xFFu) == ST_MOVE && (p->move_id & 0xFFu) == 0x7B)
        && (id == 4 || id == 7 || id == 0xB) && pair_roll(0x1EF90u) == 0)
        col = (id == 4) ? 1 : 0;
    o->ai_b6 |= 0x01u;                                     /* 0x1EF6A */
    ai_log(o, "dive col", (unsigned)col);
    return press(o, col_bits(col), 2);
}

/* ------------------------------------------------------ lockup 0x0B */
static uint32_t lockup(eng_state *st, eng_obj *o)
{
    eng_obj *v = o->partner >= 0 ? &st->obj[o->partner] : 0;
    if (!(o->state & 0x8000u)) {                           /* 0x1EFB2 entry */
        o->ai_b6 = 0; o->ai_b7 = 0; o->ai_idle = 0; o->ai_bc = 0;
        o->ai_bd = 0x20;
        if (v && !(v->driver & DRV_ANY_CPU))
            o->ai_bd = (uint8_t)tbl8(TBL(lockup_timer_by_stage), st->stage < 10 ? st->stage : 9u);   /* 0x1F04A */
        o->ai_bd = (uint8_t)(o->ai_bd + (eng_rng_fold(0) & 3u));
    }
    if (--o->ai_bd != 0) return 0;                         /* 0x1EFE6 */
    o->ai_bd = 0x20;                                       /* 0x1EFEC reload */
    if (v && !(v->driver & DRV_ANY_CPU))
        o->ai_bd = (uint8_t)tbl8(TBL(lockup_timer_by_stage), st->stage < 10 ? st->stage : 9u);
    o->ai_bd = (uint8_t)(o->ai_bd + (eng_rng_fold(0) & 3u));
    if (o->grap44 & 0x4000u) return 0;                     /* 0x1F016 partner won */
    if (++o->ai_bc < 4 && pair_roll(0x1F05Eu) != 0) return 0;
    /* ^ engine cap (user request, NOT ROM): the 4th timer expiry knees
     * unconditionally — two CPU pairs sat in a rumble lockup for minutes
     * while the 0x1F05E roll kept missing on both sides. */
    o->ai_bc = 0;
    return press(o, 0x10u, 2);                             /* cat 8: the knee */
}

/* ---------------------------------------------------- run-in sub 1 */
/* 0x1D398: follow the legal partner's y along the ring; strike the man
 * holding him (0x39) or, vs a standing man nearby, the 0x1D4FC[stage]
 * 3-way (0 strike / 1 behind grab / 2 nothing). */
static uint32_t runin(eng_state *st, eng_obj *o)
{
    eng_obj *tm = o->teammate >= 0 ? &st->obj[o->teammate] : 0;
    eng_obj *q = 0;
    int32_t dy, ady, dx, adx;
    uint32_t bits = 0;
    if (!tm) { o->ai_sub = 0; return 0; }
    if (o->tag_flags & TF_RECALL) {                                  /* usher recall (tag.c) */
        o->ai_sub = 0; o->ai_runin_t = 0;
        ai_log(o, "run-in recall", 0);
        return 0;
    }
    dy = (tm->y >> 16) - (o->y >> 16); ady = dy < 0 ? -dy : dy;
    if (ady >= 8) bits = dir_bits(0, dy);                  /* 0x1D3A2..0x1D3DE */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {               /* the man holding my partner */
        eng_obj *c = &st->obj[i];
        unsigned tmv = tm->move_id & 0xFFu;
        if (c->active && c != o && c->partner == o->teammate
            && ((c->state & 0xFFu) == ST_HOLD
                || ((c->state & 0xFFu) == ST_MOVE && (c->cue_flags & CF_HOLD_CUES))
                || ((c->state & 0xFFu) == ST_MOVE && (tm->state & 0xFFu) == ST_MOVE   /* my man sits in a VICTIM move:
                                          the hold is live whatever f35 says (tag.c eng_tag_rescue_live) */
                    && (tmv == 0x52 || tmv == 0x53 || (tmv >= 0x5D && tmv <= 0x64))))) q = c;   /* scripted hold/cover too */
    }
    if (!q && (eng_pin_is_pinner(tm) || ((tm->state & 0xFFu) == ST_MOVE && tm->pinning)))
        q = tm;                                            /* my man is the PINNER: I came in
                                          to keep the other rescuer off him (0x21642) */
    if (q && ady < 8) {                                    /* 0x1D3F0: partner held (0x53 analog) */
        int mine = (q == tm);
        if (mine) return 0;                                /* never stomp my own man */
        o->opp = (int)(q - st->obj);
        if (eng_pin_is_pinner(q) || ((q->state & 0xFFu) == ST_MOVE && q->pinning)) {
            /* The REAL break: stomp the pile. Move 0x0A hits the covering
             * man, whose hit record 0x1D (0x15F22) carries reaction
             * handler 9 (0x24BC2) = the pin release. The 0xEF9A remap
             * checks prox box 3 (0xE9BA+0x18: dx 0x08..0x60 mirrored by
             * the target's facing), so 0x1E664/0x1F45A puts the stomper
             * on the target's FRONT side first — from the wrong side the
             * ROM degrades the press to a jab (0xF02E), which breaks the
             * cover through the same reaction handler. */
            int32_t want = (q->x >> 16) + ((q->facing & 0x8000u) ? -0x20 : 0x20);
            int32_t d, qdy = (q->y >> 16) - (o->y >> 16);
            if (want < 0x1E8) want = 0x1E8;    /* a pile at the ROPES put the wanted
                                                  spot outside the ring - the rescuer
                                                  walked into the ropes forever (user
                                                  2026-08-28); clamp, and fire once
                                                  boxed-in near enough */
            if (want > 0x318) want = 0x318;
            d = want - (o->x >> 16);
            if (o->clip && (qdy < 0x10 && qdy > -0x10)
                && labs((long)((q->x >> 16) - (o->x >> 16))) < 0x38) {
                ai_log(o, "run-in stomp (boxed)", (unsigned)(q - st->obj));
                return fire_move(st, o, q, 0x0A);
            }
            if (qdy >= 8 || qdy <= -8) return dir_bits(0, qdy);
            if ((d >= 0xC8 || d <= -0xC8) && (o->state & 0xFFu) <= 1 && !(o->ai_b6 & 0x02u)) {
                o->ai_b6 |= 0x02u; ai_log(o, "run-in dash", 0); return run_launch(o, d > 0);   /* far pile */
            }
            if (d > 6 || d < -6) return dir_bits(d, 0);
            ai_log(o, "run-in stomp", (unsigned)(q - st->obj));
            return fire_move(st, o, q, 0x0A);
        }
        dx = (q->x >> 16) - (o->x >> 16); adx = dx < 0 ? -dx : dx;
        if (adx >= 0xC8 && ady < 8 && (o->state & 0xFFu) <= 1 && !(o->ai_b6 & 0x02u)) {
            /* a FAR pile: run at it. Stock's entered man is on the generic
             * 0x1C93E approach whose 0x1F310 run-in roll fires at dx >= 0xC8
             * ("the tag partner entering RUNS a bit", user 2026-08-30) */
            o->ai_b6 |= 0x02u; ai_log(o, "run-in dash", 0);
            return run_launch(o, dx > 0);
        }
        if (adx < 0x40 && (q->state & 0xFFu) == ST_MOVE) {
            /* Scripted hold (0x09/0x22/0x25/0x36): the holder carries his
             * hittable record (0x14/0x15/0x1E) and a light strike breaks it
             * through reaction handlers 6/7/0x0A (hit.c hold_hit). The
             * run-in strike move 0x39 is untranscribed, so the break is
             * applied directly with a B1 press for the anim. */
            if (q->atk & 0xFFu) eng_hold_break_by(st, o, q);
            else if ((tm->state & 0xFFu) == ST_MOVE && eng_escape_of(tm->move_id & 0xFFu)) {
                /* a holder with NO hittable record (the 0x09-family ground
                 * holds, hold_holder is09): the partner's strike frees my
                 * man through his own escape move (the 0xEBC4 ladder), which
                 * throws the holder off ("tag partner cant stomp the leg
                 * lock", playtest 2026-08-27) */
                tm->mash_aa = 0; tm->state = ST_MOVE; tm->move_id = (uint16_t)eng_escape_of(tm->move_id & 0xFFu);
            }
            ai_log(o, "run-in hold break", (unsigned)(q - st->obj));
            return press(o, 0x10u, 2);
        }
        if (adx < 0x40 && (q->state & 0xFFu) == 0x0Cu
            && labs((long)((q->y >> 16) - (o->y >> 16))) < 0x10) {
            /* STANDING grapple (0x0C, not a pin): the rescuer swings (0x39
             * strike = B1) and hit.c decides - a man in a plain grapple
             * hold carries NO hittable record (atk 0, the ROM's +0x4C
             * prefilter 0x2412A), so the swing whiffs and the grapple
             * stands, as in stock.  The engine used to floor the holder
             * here directly ("knocked down during a grapple, which cannot
             * happen on MAME", user 2026-08-30; earlier "he just touches
             * me and I fall down"). */
            ai_log(o, "run-in strike at grapple", (unsigned)(q - st->obj));
            return press(o, 0x10u, 2);                     /* 0x39 strike: B1 */
        }
        if ((q->state & 0xFFu) != 0x0Cu && (q->state & 0xFFu) != 5u)
            return bits;               /* target no longer holds anybody: stand down
                                          (the brawl/retarget path picks a new goal) */
        return dir_bits(dx, 0);
    }
    if (!(st->sig169e & 0x80u)) return bits;               /* $1C007C not live */
    {
        /* BRAWL (user spec 2026-08-24: after the pile, run-ins "attack one
         * another"). Preferred target = the enemy INTRUDER (the 0x20B44
         * selector already points +0x7A there); a downed/absent pick falls
         * back to the standing legal man. The old code only struck ONCE
         * when the legal man wandered close and never approached anybody —
         * "sometimes they just stand there". */
        eng_obj *a = o->opp >= 0 ? &st->obj[o->opp] : 0;
        unsigned as = a ? a->state & 0xFFu : 0xFFu;
        if (!(a && a->active && !a->apron
              && (as == 0 || as == 1 || as == 2 || opp_standing_react(a))
              && ((a->role ^ o->role) & 0x80u)))
            a = tm->opp >= 0 ? &st->obj[tm->opp] : 0;
        if (!a || !a->active) return bits;
        as = a->state & 0xFFu;
        if (!(as == 0 || as == 1 || as == 2 || opp_standing_react(a))) return bits;
        dy = (a->y >> 16) - (o->y >> 16); ady = dy < 0 ? -dy : dy;
        dx = (a->x >> 16) - (o->x >> 16); adx = dx < 0 ? -dx : dx;
        if (ady >= 0x10 || adx >= 0x40)
            return dir_bits(adx >= 0x30 ? dx : 0, ady >= 8 ? dy : 0);   /* close in */
        if (o->ai_idle) { o->ai_idle--; return bits; }     /* strike cooldown */
        o->ai_idle = 0x30;
        {
            unsigned stg = st->stage < 10 ? st->stage : 9u;
            int r = d100_walk(tbl_ra_ptr(0x1D4FCu + stg * 4u, 3), 3);
            ai_log(o, "run-in roll", (unsigned)r);
            o->opp = (int)(a - st->obj);
            if (r == 0) return press(o, 0x10u, 2);         /* 0x39 strike */
            if (r == 1 && a->partner < 0) return press(o, 0x10u, 2);   /* 0x1C behind grab: TODO EXACT */
        }
    }
    return bits;
}

/* ------------------------------------------- rescue arm + countdown */
/* 0x215B6/0x21040 family (simplified to what the engine can see): the
 * apron partner of a pinned man arms with 0x2110E[band] (pins) or
 * 0x2172C[band] (standing holds); 0x1D526 counts it down and fires the
 * run-in (move 0x4E entry: TODO EXACT — the climb-in is instantaneous,
 * x += ±0x38 as the 0x4E tail does). */
int eng_ai_rescue_tick(eng_state *st, eng_obj *o)
{
    eng_obj *tm = eng_team_legal(st, o);   /* 3-man teams: the circular +0x86
                                              may point at the other apron man */
    cur_st = st;
    if (!tm || !o->apron || !(o->driver & DRV_AUTOPILOT)) return 0;
    if (!(o->tag_flags & TF_USHER_A)) {
        /* Not armed: nothing to count down. Pins arm from the cover catch
         * (0x215B6 + 0x21732), the submission holds from their handlers
         * (rack 0x17F78, bearhug, MDD, mount... eng_tag_arm_hold /
         * eng_tag_arm_holder), the double-team dive from its landing.
         * The engine-only "scan-arm" that used to live here fired on ANY
         * enemy in state 0x0C - the plain grapple stance - so the apron
         * partner ran in on every tie-up win and his rescue strike
         * floored the grappler (user 2026-08-26: "shouldn't happen during
         * a grapple, only a pin or submission"; "he just touches me and I
         * fall"). Stock has no such arm for the stance. */
        return 0;
    } else if (!(o->st_flags & SF_RUNIN_MARK) && !eng_tag_rescue_live(st, o)) {   /* 0x212A0/0x213A6 */
        /* ^ the double-team retaliation (0x21424 arms with bset #2,+0x32,
         * 0x214A4) is a FORCED run-in: its countdown rides to zero even
         * though the emergency (his teammate) is already down — stock
         * only disarms via the hold-ended helper 0x213A6, which never
         * runs for the dive landing. */
        o->tag_flags &= (uint16_t)~0x05u; o->ai_e6 = 0; o->ai_b5 &= (uint8_t)~0x80u;
        return 0;
    }
    if (o->apron && (o->tag_flags & TF_USHER_A) && o->ai_e6 == 0)
        o->ai_e6 = 1;                  /* armed with NO countdown: the arm caught him
                                          mid-climb-out and took the inside branch —
                                          he finished on the apron and stood there
                                          ("stuck outside the ring", user 2026-08-28) */
    if (o->ai_e6 == 0 || --o->ai_e6 != 0) return 0;        /* 0x1D526 */
    /* 0x1D546-0x1D590: autopilot on (unless this man is carrying a human
     * pad handed over by 0x21618 — see pin-partner.md §3), disarm, usher
     * grace $1C1682 = 0x177, then state 5 move 0x4E: he CLIMBS IN over
     * the ropes (32 frames) and cannot act until the anim ends. */
    if (tm && (tm->role & RF_PAD) && tm->input >= 0 && !(o->role & RF_PAD) && !o->cpu) {
        /* ENGINE (user 2026-08-30 "my teammate runs in but the CPU still
         * controls him"): the VICTIM side's pad follows the rescuer too -
         * the pinned / held man goes autopilot (his kick-out is then the
         * CPU roll, ai.c victim_escape) and the pad comes home at the pin's
         * end (0x2130A). Stock never moves the pad on this side. */
        eng_tag_hand_pad(tm, o);
        if (eng_dbgsel) fprintf(stderr, "tag: pad -> rescuer o%d (victim side)\n", (int)(o - st->obj));
    }
    if (!(o->role & RF_PAD) || o->input < 0) o->driver |= DRV_AUTOPILOT;
    else o->driver &= (uint16_t)~0x40u;   /* he holds the pad: 0x21612's
                                          bclr #6,(+0x56) applied to the man
                                          who RUNS in (the ROM only does it
                                          when the partner is already inside
                                          — the deviation that makes the
                                          user-facing feature work) */
    o->tag_flags &= (uint16_t)~0x03u;
    o->st_flags &= (uint16_t)~0x04u;        /* the 0x214A4 forced-run-in mark is spent */
    o->sub = 0;
    o->state = ST_MOVE; o->move_id = 0x4E; o->grap44 = 0; o->count = 0; o->frame = 0;
    o->anim_sel = 0;
    st->usher_t = (uint16_t)eng_tag_rule(TAG_USHER_RUNIN);
    o->ai_runin_t = (uint16_t)eng_tag_rule(TAG_USHER_RUNIN);
    o->ai_b7 = 0;
    o->band = o->hp <= 0x18 ? 2 : o->hp <= (uint16_t)(2 * o->hp_max / 3) ? 1 : 0;   /* 0x24EC2 */
    ai_log(o, "run-in 0x4E", o->ai_runin_t);
    return 1;
}

/* ---- 0x1D428-0x1D524: the APRON man's own attack (autopilot / CPU) ----
 * The enemy LEGAL man (my legal man's +0x7A) standing (state 0/1) or
 * staggered (state 4 react 0/1/0xA) within dx < 0x40, dy < 0x10 of me:
 * once per approach (+0xB7 b1) roll 0x1D4FC[stage] (0x24CC): bucket 0 =
 * the apron PUNCH 0x39 at him, 1 = the over-the-ropes GRAB 0x1C (only if
 * he is unlinked; +0xB5 b4), else nothing. Leaving the box re-arms
 * (0x1D4F4). "the partner on the outside can PUNCH or GRAB the opponent
 * if they come too close" (user 2026-08-30, MAME). */
int eng_ai_apron_attack(eng_state *st, eng_obj *o)
{
    eng_obj *tm = eng_team_legal(st, o), *e;
    int32_t dx, dy; unsigned es, er, stage, d, acc = 0, b;
    if (!tm || tm->opp < 0) return 0;                           /* 0x1D432 */
    e = &st->obj[tm->opp];
    if (!e->active) return 0;
    es = e->state & 0xFFu; er = e->react_id & 0xFFu;
    if (!(es == 0 || es == 1 || (es == 4 && (er == 0 || er == 1 || er == 0x0A)))) return 0;   /* 0x1D43A-0x1D468 */
    dy = (o->y >> 16) - (e->y >> 16); dx = (o->x >> 16) - (e->x >> 16);
    if (labs(dy) >= 0x10 || labs(dx) >= 0x40) { o->ai_b7 &= (uint8_t)~0x02u; return 0; }   /* 0x1D4F4 */
    if (o->ai_b7 & 0x02u) return 0;                            /* 0x1D490 rolled this approach */
    o->ai_b7 |= 0x02u;
    stage = eng_camp_stage() < 10 ? eng_camp_stage() : 9;
    d = 0x32;                                                   /* 0x24CC d100 shape */
    for (int k = 0; k < 4; k++) { d = (eng_rng() & 0xFFu) >> 1; if (d < 0x64) break; d = 0x32; }
    if (d == 0) d = 1;
    for (b = 0; b < 3; b++) { acc += tbl8(TBL(runin_strike_roll), stage * 4u + b); if (acc >= d) break; }
    if (b == 0) {                                               /* 0x1D4BE the apron punch */
        o->opp = tm->opp; o->partner = tm->opp;
        if (e->partner < 0) e->partner = (int)(o - st->obj);
        o->st_flags |= SF_APRON; o->state = ST_MOVE; o->move_id = 0x39; o->grap44 = 0; o->spr_force = 0;
        ai_log(o, "apron punch", (unsigned)tm->opp);
        return 1;
    }
    if (b == 1) {                                               /* 0x1D4D2 the grab, unlinked target only */
        if (e->partner >= 0) return 0;
        o->ai_b5 |= 0x10u; o->ai_b7 &= (uint8_t)~0x02u;
        o->opp = tm->opp; o->partner = tm->opp; e->partner = (int)(o - st->obj);
        o->st_flags |= SF_APRON; o->state = ST_MOVE; o->move_id = 0x1C; o->grap44 = 0; o->spr_force = 0;
        ai_log(o, "apron grab", (unsigned)tm->opp);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------- approach */
static uint32_t approach(eng_state *st, eng_obj *o, eng_obj *p, const geo_t *g);

/* 0x1DE66/0x1DEA6 result handling (0x1DEF0–0x1DFCC) */
static uint32_t policy_result(eng_state *st, eng_obj *o, eng_obj *p, const geo_t *g, unsigned mv)
{
    ai_log(o, "policy", mv);
    if (mv == 0xFF) { o->ai_idle = 1; o->ai_sub = 0; return dir_bits(g->dx, 0); }   /* 0x1DEF0 idle: stays walking */
    if (mv == 0xFE) {                                      /* 0x1DF1E */
        int32_t px = p->x >> 16;
        if ((p->facing & 0x8000u) ? px < 0x270 : px >= 0x2A0) {
            /* walk to the rope line, sub 5 0x1D078: TODO EXACT */
            o->ai_t = 8; return 0;
        }
        mv = 0x3E;
    }
    if (mv == 0xFD) { o->ai_t = 4; return 0; }             /* 0x1DF62 */
    if (mv == 0x3E) {                                      /* 0x1DF7E pursue-reposition */
        o->ai_sub = 0x3E; o->ai_sub_t = 0;
        return dir_bits(-g->dx, 0);
    }
    if (mv == 0x3C) { o->ai_sub = 0x3C; o->ai_sub_t = 0; return dir_bits(g->dx, 0); }
    if (mv == 0x3D) { o->ai_b7 &= (uint8_t)~0x40u; queue_move(st, o, p, 0x3D); return 0; }
    if (mv == 0x75) return fire_move(st, o, p, 0x75);
    queue_move(st, o, p, mv);
    if (o->ai_sub == 0xA) return 0;
    if (!(o->ai_b5 & 0x08u)) return fire_move(st, o, p, mv);                         /* runs now */
    if (g->face_diff && g->adx < (int32_t)o->ai_bc) return fire_move(st, o, p, mv);   /* 0x1C5B6 */
    return dir_bits(g->dx, 0);
}

/* 0x1C93E: state 1 sub 0 */
static uint32_t approach(eng_state *st, eng_obj *o, eng_obj *p, const geo_t *g)
{
    static const uint8_t grapple_moves[] = { 0x05, 0x2A, 0x2D, 0x20, 0x41 };
    unsigned ps = p->state & 0xFFu, rc = p->react_id & 0xFFu;
    uint32_t bits;
    o->ai_b7 &= (uint8_t)~0x80u;
    if (o->weapon_w & WPN_HELD) {                                /* 0x1CABA / 0x1D032: armed with
                                                              a ringside weapon */
        unsigned lying = (ps == ST_REACT && (rc == RC_LYING || rc == RC_LYING_B));
        if (g->adx < 0x68) {                               /* 0x1D032 -> 0x1CD2C */
            if (!lying)                                    /* 0x1CD2C: a lying man is
                                                              approached instead (0x1CCBC) */
                return fire_move(st, o, p, 0x1E);          /* 0x1CD4E swing */
        } else if (g->adx < 0x90                           /* 0x1D03E */
                   && pair_roll(0x1D066u) == 0)            /* 0x1D046 pair {50,50} */
            return fire_move(st, o, p, 0x1F);              /* 0x1D058 */
        /* else: keep closing with the weapon in hand */
    }
    if (eng_mod_rule(MODR_EXT_MOVES) && (ps == ST_CLIMB || ps == ST_PERCH || ps == ST_CLIMBDOWN) && !(o->role & RF_OUTSIDE)) {
        /* MOD ext_moves: a CLIMBING/PERCHED man can be hit off the buckle
         * (hit.c: in front, dx < 0x38, dy < 0x18) - stock has no such move,
         * so the stock AI just waited under him ("the cpu doesn't know it
         * can punch me off the top rope", user 2026-08-30): go under him
         * and swing */
        if (g->adx < 0x30 && g->ady < 0x14) {
            if (!g->face_diff) return dir_bits(g->dx, 0);  /* turn to face him */
            return policy_result(st, o, p, g, policy_pick(st, o, p, 0));
        }
        return dir_bits(g->dx, g->dy);
    }
    if (g->b4 == 0 && g->ady < 0xC) {                      /* ---- NEAR ---- */
        if (o->ai_idle) {                                  /* 0x1CACE */
            if (ps == ST_STAND || (ps == ST_WALK && p->ai_sub == 0) || (ps == ST_REACT && rc == RC_DIZZY)) return g->opp_right ? 1u : 2u;   /* 0x1CFB4 -> 0x1D01A: walk on the +0xB5 b0 side (never stalls at dx == 0) */
            o->ai_idle = 0;
            if (st->g161 & 1u) { o->state = ST_STAND; o->ai_b6 |= 0x80u; }   /* 0x1CB06 rumble: drop back to idle and re-target */
            return 0;
        }
        if (ps == ST_STAND || ps == ST_WALK || ps == ST_GETUP) return policy_result(st, o, p, g, policy_pick(st, o, p, 0));
        if (ps == ST_MOVE) {
            if (p->role & RF_RUNNING) {                          /* opp grapple-ready */
                if (!g->face_diff) return 0;
                if (opp_move_in(p, grapple_moves, 5)) return policy_result(st, o, p, g, policy2_pick(st, o, p, 0));
                return fire_move(st, o, p, 0x75);          /* counter-grab */
            }
            if ((p->move_id & 0xFFu) == 0x4C) { o->ai_b7 |= 0x40u; queue_move(st, o, p, 0x3D); return 0; }
            if (p->role & RF_ENGAGED) return fire_move(st, o, p, 0x00);   /* engaged: grab */
            return 0;
        }
        if (ps == ST_REACT) {
            if (rc == 0 || rc == 0xA) return policy_result(st, o, p, g, policy_pick(st, o, p, 0));
            if (rc == RC_LYING || rc == RC_LYING_B) {                      /* 0x1CC2C lying */
                if (p->hp != 0) {
                    if (toprope_eval(st, o, p)) return 0;
                    return policy_result(st, o, p, g, policy_pick(st, o, p, rc == RC_LYING ? 2u : 3u));
                }
                o->ai_b5 &= (uint8_t)~0x10u;
                queue_move(st, o, p, 0x48); return 0;
            }
            if (rc == RC_DIZZY) return policy_result(st, o, p, g, policy_pick(st, o, p, g->face_diff ? 5u : 7u));
            o->ai_b7 |= 0x40u; queue_move(st, o, p, 0x3D); return 0;   /* still falling: wait */
        }
        if (p->role & RF_RUNNING) {                              /* 0x1CCD0 other state, running */
            if (!g->face_diff) return dir_bits(g->dx, 0);  /* 0x1CCDE same facing: walk */
            return policy_result(st, o, p, g, policy2_pick(st, o, p, 0));   /* 0x1CCE6 */
        }
        /* 0x1CCBC: the target is in none of the states above (held 0xFF,
         * tie-up 0x0B/0x0C, skid...) and not running -> WAIT beside him
         * (+0xB7 b6, move 0x3D) instead of walking into him — walking here
         * made the CPU oscillate across a held man's x every frame (facing
         * flip jitter) */
        o->ai_b7 |= 0x40u; queue_move(st, o, p, 0x3D); return 0;
    }
    if (g->ady >= 0xC) {                                   /* ---- ALIGN (0x1CA9E b4 0 / 0x1CD5C b4 1-2) ---- */
        if (g->b4 <= 1) {                                  /* -> 0x1CDD0 (b4 2 goes straight to 0x1CD6E) */
            if (ps == ST_MOVE && (p->move_id & 0xFFu) == 0x4C) { o->ai_b7 |= 0x40u; queue_move(st, o, p, 0x3D); return 0; }   /* 0x1CDD0 */
            if ((st->g161 & 1u) && !(o->ai_b5 & 0x80u) && (p->role & RF_ENGAGED)) {
                /* 0x1CDF4 RUMBLE: the target is already ENGAGED with
                 * someone — drop him and re-decide (0x1CE0E clr state,
                 * bset #7,+0xB6; the 0x1CE18 +0xB9 b0 "wait for a free
                 * man" is covered by nearest()'s engaged filter in
                 * rumble.c). Keeps the pack from converging vertically
                 * on a man who is already fighting. */
                o->state = ST_STAND; o->ai_b6 |= 0x80u;
                ai_log(o, "align: target engaged, re-target", (unsigned)(p - st->obj));
                return 0;
            }
        }
        if (p->role & RF_RUNNING)                                /* 0x1CD72 opp running -> 0x1D01A */
            return g->opp_right ? 1u : 2u;                 /* horizontal side walk (angle 0x40/0xC0) */
        return dir_bits(g->adx >= 0xC ? g->dx : 0, g->dy);  /* 0x1CD7C-0x1CDCE diagonals */
    }
    /* ---- FAR, aligned (0x1CE38) ---- */
    if (g->b4 == 1) {                                      /* 0x1CEFC */
        if ((p->role & RF_RUNNING) && g->face_diff) {            /* 0x1CF12 */
            if (g->adx >= 0x68) {
                static const uint8_t l3[] = { 0x2D, 0x20, 0x41 };
                if (g->adx < 0xB0 && opp_move_in(p, l3, 3)) return policy_result(st, o, p, g, policy_pick(st, o, p, 0));
                o->ai_sub = 0x3C; o->ai_sub_t = 0; ai_log(o, "dash 3C", 0);
                return dir_bits(g->dx, 0);
            }
            if (opp_move_in(p, grapple_moves, 5)) return policy_result(st, o, p, g, policy_pick(st, o, p, 0));
            return fire_move(st, o, p, 0x75);
        }
        return g->opp_right ? 1u : 2u;                     /* 0x1D01A: angle 0x40/0xC0 by +0xB5 b0 — two men at the
                                                              same x (dx 0, same facing) no longer stand forever */
    }
    if (p->role & RF_RUNNING) {                                  /* 0x1CE96 */
        if (g->face_diff && st->scene != 1 && !(o->role & RF_OUTSIDE) && pair_roll(0x1D030u) != 0)
            return run_away(o, g);
        return dir_bits(g->dx, 0);
    }
    if (ps == ST_REACT && rc == RC_DIZZY) {
        if (g->face_diff) return policy_result(st, o, p, g, policy_pick(st, o, p, 6));
        return dir_bits(g->dx, 0);
    }
    if (!(p->st_flags & SF_APRON) && run_eval(o, g, &bits) == 0) return bits;   /* 0x1F310 */
    return dir_bits(g->dx, 0);
}

/* --------------------------------------------------------- run 0x02 */
static uint32_t run_tick(eng_state *st, eng_obj *o, eng_obj *p, const geo_t *g)
{
    static const uint8_t brk[] = { 0x48, 0x1A, 0x23 };
    unsigned ps = p->state & 0xFFu;
    int brk_off;
    if (o->ai_b6 & 0x20u) return 0;                        /* running away: ride the ropes */
    if (!(o->ai_b7 & 0x02u) && g->ady < 8 && !(o->ai_b5 & 0x08u)) {   /* 0x1D94C phase 0, once */
        unsigned mv = policy_pick(st, o, p, 8);
        o->ai_b7 |= 0x02u;
        ai_log(o, "run policy", mv);
        if (mv < 0xFD && mv != 0x3E && mv != 0x3C && mv != 0x3D) {
            o->ai_mv = (uint8_t)mv; o->ai_bc = (uint16_t)prep_gate(o, p, mv); o->ai_b5 |= 0x08u;
        }
    }
    if (!(o->ai_b5 & 0x08u) && !(o->ai_b7 & 0x01u) && g->face_diff     /* 0x1D814 phase 1 */
        && g->ady < 0xA && g->adx < 0xA0 && !(o->ai_b7 & 0x04u)) {
        unsigned id = (unsigned)eng_ws_base(o->wrestler), stg = st->stage < 5 ? st->stage : 5u;
        unsigned band = o->band & 0xFFu; if (band > 2) band = 2;
        o->ai_b7 |= 0x04u;
        if ((p->role & RF_RUNNING) || (ps == ST_MOVE && (p->move_id & 0xFFu) == 3)) {
            uint32_t row = rom32(tbl32(TBL(run_counter_rolls), id * 4u) + stg * 4u) + band * 2u;   /* 0x1D8C2 */
            if (pair_roll(row) == 0) { o->ai_mv = 0x75; o->ai_bc = 0x50; o->ai_b5 |= 0x08u; ai_log(o, "run 75", 0); }
        } else {
            uint32_t row = rom32(tbl32(TBL(run_strike_rolls), id * 4u) + stg * 4u) + band * 2u;
            if (pair_roll(row) == 0) { o->ai_mv = 0x72; o->ai_bc = 0x48; o->ai_b5 |= 0x08u; o->ai_b7 |= 0x01u; ai_log(o, "run 72", 0); }
        }
    }
    brk_off = (ps == ST_REACT && !opp_standing_react(p)) || (p->role & RF_ENGAGED) || (p->st_flags & SF_TOPROPE)
           || opp_move_in(p, brk, 3) || ps == ST_HOLD || ps == ST_HELD;
    if ((o->ai_b5 & 0x08u) && g->adx < (int32_t)o->ai_bc && !brk_off) {   /* 0x1D99E fire */
        o->ai_b7 &= (uint8_t)~0x01u;
        return fire_move(st, o, p, o->ai_mv);
    }
    if (brk_off && g->adx < 0xA0) {                        /* 0x1D9F8 state 3: the skid */
        o->ai_b5 &= (uint8_t)~0x08u;
        return press(o, (o->angle & 0x80u) ? 1u : 2u, 2);  /* reverse stick (0xF42E) */
    }
    /* ENGINE (user 2026-08-28 "whipped CPU runs forever"): with the
     * opponent dodged away in y the 0x1D94C policy gate (ady < 8) never
     * opens and the 0x1D9F8 skid never fires (brk_off is false while he
     * just walks) — the whipped runner bounced rope-to-rope forever. A
     * whipped man (+0x34 b6, dies at the stand) whose rope-bounce budget
     * (+0x44) is spent skids out on the reverse stick. */
    if (!o->grap44 && !(o->ai_b5 & 0x08u)) {
        /* a run the AI never armed (no 0x1D94C policy, +0x44 budget spent)
           = the tail of a WHIP: skid out on the reverse stick */
        ai_log(o, "whip skid-out", 0);
        return press(o, (o->angle & 0x80u) ? 1u : 2u, 2);
    }
    return 0;
}

/* --------------------------------------------------------- walk-to */
/* sub 0xA (0x1D5A2 + 0x1F15C): steer to (+0xBE,+0xC0), |d| < 8 both axes
 * = arrived → fire (state 5 with +0xB5 b4). Aborts per 0x1D684/0x1D6CA. */
static uint32_t walk_to(eng_state *st, eng_obj *o, eng_obj *p, const geo_t *g)
{
    int32_t dx = o->ai_tx - (o->x >> 16), dy = o->ai_ty - (o->y >> 16);
    int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    unsigned mv = o->ai_mv, ps = p->state & 0xFFu;
    int target_down = (mv == 0x3D || mv == 0x48 || mv == 0x08 || mv == 0x0A || mv == 0x47 || mv == 0x0B
                    || mv == 0x09 || mv == 0x22 || mv == 0x14 || mv == 0x46 || mv == 0x10 || mv == 0x13);
    if (++o->ai_sub_t > 0x100) { o->ai_sub = 0; return 0; }   /* engine guard */
    if (target_down) {                                     /* 0x1D684 */
        int still = opp_lying(p) || (ps == ST_HELD) || (p->cue_flags & 1u)
                 || (ps == ST_MOVE && ((p->move_id & 0xFFu) == 0x61 || (p->move_id & 0xFFu) == 0x4A));
        if (mv == 0x3D && (o->ai_b7 & 0x40u)) still = !(ps == ST_STAND || ps == ST_WALK || ps == ST_RUN);   /* falling man */
        if (!still) { o->ai_b5 &= (uint8_t)~0x20u; o->ai_b7 &= (uint8_t)~0x20u; o->ai_sub = 0; return 0; }   /* 0x1D702 */
        if (o->clip && o->ai_sub_t > 8) {                  /* 0x1D6CA: boxed in */
            o->clip = 0;
            if (mv == 0x3D) { adx = ady = 0; }             /* 0x1D728: counts as arrived */
            else { target_1F45A(o, p, 0x0A); o->ai_sub_t = 9; return 0; }   /* stomp spot instead */
        }
    }
    if (adx < 8 && ady < 8) {                              /* 0x1F15C arrived */
        if (mv == 0x3D) { o->ai_sub = 0x3D; o->ai_b5 |= 0x10u; o->ai_sub_t = 0; o->tag_flags &= (uint16_t)~0x10u; return 0; }
        if (mv == 0x70) {                                  /* 0x1C7C6: the weapon pickup is
                                                              set directly, not selector-routed */
            o->ai_sub = 0; o->state = ST_MOVE; o->move_id = 0x70; o->grap44 = 0;
            return 0;
        }
        return fire_move(st, o, p, mv);
    }
    (void)g;
    return dir_bits(adx >= 8 ? dx : 0, ady >= 8 ? dy : 0);
}

/* ---------------------------------------------------------- stand */
static uint32_t stand(eng_state *st, eng_obj *o, eng_obj *p, const geo_t *g)
{
    if (!(o->state & 0x8000u)) {                           /* 0x1C2CC: wait for entry */
        o->ai_b5 &= 0xCBu; o->ai_b6 &= 0xF1u; o->ai_b7 &= (uint8_t)~0x22u;   /* 0x1C2DC */
        o->ai_b6 &= (uint8_t)~0x20u;                       /* +0x32 b2 clears with the run */
        if (o->ai_sub == 0x3C || o->ai_sub == 0x3E) o->ai_sub = 0;
        return 0;
    }
    if ((o->tag_flags & TF_PIN_INTENT) && !(o->ai_b5 & 0x08u)) {         /* 0x1C348 pin intent → cover */
        o->tag_flags &= (uint16_t)~0x10u;
        if (opp_lying(p)) { queue_move(st, o, p, 0x48); return 0; }
    }
    if (o->ai_b7 & 0x01u) {                                /* 0x1C370 run-attack pending (0x72) */
        o->ai_b7 &= (uint8_t)~0x01u;
        return fire_move(st, o, p, 0x72);
    }
    if (o->ai_b5 & 0x08u) {                                /* 0x1C430 follow-up armed */
        if (!(p->role & RF_RUNNING)) {                           /* 0x1C44C: he is not coming */
            o->ai_b5 &= (uint8_t)~0x08u;
        } else {
            if (!(o->role & RF_OUTSIDE) && !(o->ai_b7 & 0x04u)) {   /* 0x1C462 once: back to the ropes? */
                unsigned z = xzone(o);
                o->ai_b7 |= 0x04u;
                if ((z < 3 && (o->facing & 0x8000u)) || (z >= 9 && !(o->facing & 0x8000u)))
                    if (pair_roll(0x1F3BEu) == 0) { o->ai_b5 &= (uint8_t)~0x08u; return run_away(o, g); }
            }
            if (!(o->ai_b7 & 0x08u)) {                     /* 0x1C4E4 */
                if (!g->face_diff) return 0;
                o->ai_b7 |= 0x08u;
                {
                    unsigned ctx = g->adx < 0x68 ? 0u : g->adx < 0xB0 ? 1u : 2u;
                    unsigned mv = policy2_pick(st, o, p, ctx);
                    ai_log(o, "policy2", mv);
                    if (ctx == 0) { o->ai_b5 &= (uint8_t)~0x08u; return policy_result(st, o, p, g, mv); }
                    o->role &= (uint16_t)~0x10u;
                    if (mv < 0xFD && mv != 0x3E && mv != 0x3C && mv != 0x3D) queue_move(st, o, p, mv);
                    if (p->role & RF_RUNNING) return run_launch(o, g->opp_right);
                }
            }
            if (g->face_diff && g->adx < (int32_t)o->ai_bc) return fire_move(st, o, p, o->ai_mv);   /* 0x1C5B6 */
            return dir_bits(g->dx, 0);
        }
    }
    if (o->ai_sub == 0xA) return walk_to(st, o, p, g);
    if (o->ai_sub == 9) return corner_walk(st, o, p);
    return approach(st, o, p, g);                          /* 0x1C8B2 → state 1 sub 0 */
}

/* ----------------------------------------------------- pseudo-moves */
/* 0x3C (0x1E42A, re-run every frame): keep closing while the opponent is
 * grapple-ready and facing us, else back to stand. The 0x18028 record's
 * own walk is TODO EXACT — this walks with the stick. */
static uint32_t pseudo_3C(eng_obj *o, const eng_obj *p, const geo_t *g)
{
    if (!(p->role & RF_RUNNING) || !g->face_diff || ++o->ai_sub_t > 0x60) { o->ai_sub = 0; return 0; }
    return dir_bits(g->dx, g->ady >= 0xC ? g->dy : 0);
}
/* 0x3D wait (0x1EBCE every frame): b7 b6 = waiting for a falling man to
 * land; b5 b4 = standing beside the lying man until he gets up. */
static uint32_t pseudo_3D(eng_obj *o, const eng_obj *p)
{
    unsigned ps = p->state & 0xFFu, rc = p->react_id & 0xFFu;
    static const uint8_t wait_moves[] = { 0x4C, 0x4D, 0x0D, 0x0C, 0x12, 0x11, 0x0F, 0x1B };
    if (++o->ai_sub_t > 0x200) { o->ai_sub = 0; return 0; }
    if (o->ai_b7 & 0x40u) {                                /* 0x1EC8A */
        if (ps == ST_STAND || ps == ST_WALK) { o->ai_sub = 0; return 0; }
        if (ps == ST_REACT) { if (rc == RC_LYING || rc == RC_LYING_B) o->ai_sub = 0; return 0; }
        if (ps == ST_MOVE) { if (opp_move_in(p, wait_moves, 8) || (p->role & RF_ENGAGED)) return 0; o->ai_sub = 0; return 0; }
        if (p->role & RF_ENGAGED) return 0;
        if (!(p->st_flags & SF_TOPROPE)) o->ai_sub = 0;
        return 0;
    }
    if (o->ai_b5 & 0x10u) {                                /* 0x1ED62 */
        if (ps == ST_REACT && (rc == RC_LYING || rc == RC_LYING_B)) return 0;
        o->ai_sub = 0; o->ai_b5 &= (uint8_t)~0x10u;
        return 0;
    }
    o->ai_sub = 0;
    return 0;
}
/* 0x3E (0x1DF7E): the ROM teleports x by ±(0xB0 - dx + 0x10), i.e. to a
 * 0xC0 gap, then walks (0x1831A). TODO EXACT: we walk the gap instead. */
static uint32_t pseudo_3E(eng_obj *o, const geo_t *g)
{
    if (g->adx >= 0xC0 || o->clip || ++o->ai_sub_t > 0x60) { o->ai_sub = 0; return 0; }
    return dir_bits(-g->dx, 0);
}

/* 0x1E48C — the pinned man's kick-out class by hp: the referee half-count
 * at which he kicks out (8 = never, only hp 0). */
static unsigned pin_kick_class(unsigned hp)
{
    return hp == 0 ? 8 : hp >= 0x80 ? 0 : hp >= 0x65 ? 1
         : hp >= 0x50 ? 2 : hp >= 0x40 ? 3 : hp >= 0x25 ? 4 : 5;
}

/* ------------------------------------------------ victim escape 0x1DA7E */
/* A CPU victim never mashes (0x10D04 skips +0xAA for +0x56 & 0xC0); the
 * ROM rolls ONCE per hold against a stage×band table picked by the
 * attacker's move (0x1DB28/0x1DC48), then counts +0xBC down and enters
 * move 0x38/0x78 (escape). The engine's held/pinned victim sits in a
 * state-5 hold move and escapes on a press once +0xAA == 0x4000 — so the
 * countdown end writes that meter and presses (direct write, as the ROM's
 * state write is). Only the holds the engine routes escape from are
 * covered by the ladder in core.c; the roll itself is exact. */
static uint32_t victim_escape(eng_state *st, eng_obj *o)
{
    eng_obj *a = o->partner >= 0 ? &st->obj[o->partner] : 0;
    unsigned am, stg = st->stage < 10 ? st->stage : 9u, band = o->band & 0xFFu;
    uint32_t row = 0; unsigned noband = 0;
    if (!a || ((a->state & 0xFFu) != ST_MOVE && (a->state & 0xFFu) != ST_HOLD)) return 0;
    if (o->mash_aa == 0x4000u) return press(o, 0x10u, 2);
    if (band > 2) band = 2;
    am = a->move_id & 0xFFu;
    {
        /* State-5 row 0x1E018: a CPU held in a scripted hold escapes on
         * the 0x1F576[D0][band] timer (submissions.md §4) — 0x1E3F2
         * counts +0xBA down and rewrites the move into the escape, which
         * the engine does by mashing out + pressing (the ladder maps the
         * same escape ids). Pins keep the 0x1DB66 roll below. */
        static const struct { uint8_t mv, d0; } tmr[] = {
            { 0x5D, 2 }, { 0x5F, 2 }, { 0x60, 3 }, { 0x61, 5 },
            { 0x62, 4 }, { 0x63, 4 }, { 0x52, 0 }, { 0x53, 1 } };
        unsigned mv = o->move_id & 0xFFu;
        for (unsigned k = 0; k < sizeof tmr / sizeof tmr[0]; k++) {
            if (tmr[k].mv != mv) continue;
            if (!(o->ai_b7 & 0x02u)) {                     /* bset #6,+0x60 once */
                uint32_t row2 = tbl32(TBL(hold_escape_timer_ptrs), tmr[k].d0 * 4u);
                o->ai_b7 |= 0x02u;
                o->ai_ba = (uint16_t)(((unsigned)tbl_ra8(row2 + band * 2u) << 8)
                                      | tbl_ra8(row2 + band * 2u + 1u));
                ai_log(o, "held: escape timer", o->ai_ba);
            }
            if (o->ai_ba && --o->ai_ba == 0) {             /* 0x1E3F2 */
                if ((mv == 0x52 || mv == 0x53 || mv == 0x61) && o->grap44) {
                    o->ai_ba = 1;                          /* 0x1E40A: mid-flinch, wait a tick */
                    return 0;
                }
                o->mash_aa = 0x4000u;
                return press(o, 0x10u, 2);
            }
            return 0;
        }
    }
    {
        /* 0x1E452: PINNED (moves 0x4A / 0x51 / 0x5E / 0x64 — the 0x1E018
         * row of each is 0x8001E452: 0x1E140 / 0x1E15C / 0x1E190 /
         * 0x1E1A8) — a CPU victim kicks out when the referee's
         * half-count (+0x109) reaches his hp class (0x1E48C: hp 0 -> 8 =
         * never; >= 0x80 -> 0; >= 0x65 -> 1; >= 0x50 -> 2; >= 0x40 -> 3;
         * >= 0x25 -> 4; else 5), latched once into +0xBD. The escape is
         * the 0xEBC4 ladder's move (0x1E482 0x4B for 0x4A/0x64, 0x1E47A
         * 0x56 for the rest). */
        unsigned mv = o->move_id & 0xFFu;
        if (mv == 0x4A || mv == 0x51 || mv == 0x5E || mv == 0x64) {
            if (!(o->ai_b7 & 0x02u)) {                     /* bset #6,+0x60 once */
                unsigned cls = pin_kick_class(o->hp);      /* 0x1E48C */
                o->ai_b7 |= 0x02u; o->ai_bd = (uint8_t)cls;
                ai_log(o, "pinned: kick-out at half-count", cls);
            }
            {   /* DOUBLE PIN (user-confirmed stock rumble, 2026-08-24):
                 * every extra man covering the pile pushes the kick-out
                 * one half-count later — being piled on is harder to
                 * escape (TODO EXACT the ROM's pile term). */
                unsigned covers = 0, need;
                for (int ci = 0; ci < ENG_MAX_OBJS; ci++)
                    if (st->obj[ci].active && st->obj[ci].pinning
                        && st->obj[ci].partner == (int)(o - st->obj)) covers++;
                need = o->ai_bd + (covers > 1 ? covers - 1 : 0)
                     + (unsigned)(eng_mod_rule(MODR_KICKOUT_DELAY) - 0x80);   /* mod */
                if ((int)need < 0) need = 0;
                if (need > 7) need = 7;
            if (o->halfct == need) {                       /* 0x1E45C */
                o->mash_aa = 0x4000u;
                return press(o, 0x10u, 2);
            }
            }
            return 0;
        }
    }
    if (!(o->ai_b7 & 0x02u)) {                             /* once per hold */
        unsigned gate = 0x10;
        o->ai_b7 |= 0x02u; o->ai_bc = 0; o->ai_sub_t = 0;
        if (a->pinning || am == 0x08 || am == 0x48 || am == 0x79)
            { row = rom32(0x23D7Eu + stg * 4u); gate = 2; }          /* 0x1DB66 cover */
        else if (am == 0x10 || am == 0x46 || am == 0x0B) row = rom32(0x23F78u + stg * 4u);   /* 0x1DBEC */
        else if (am == 0x09 || am == 0x22) { row = rom32(0x23FC4u + stg * 4u); gate = 2; }
        else if (am == 0x11 || am == 0x0C || am == 0x12 || am == 0x0F) row = rom32(0x23DD0u + stg * 4u);   /* 0x1DDAA */
        else if (am == 0x0A) { row = rom32(0x23E76u + stg * 4u); noband = 1; }   /* 0x1DCE6: pointer [stage]
                                          + ctr(+0xE5)*2 - the stomp counter is TODO EXACT (0) */
        else {
            static const uint8_t l[] = { 0x10, 0x0B, 0x13, 0x14, 0x0E, 0x35, 0x23 };   /* 0x1DE54 */
            for (unsigned k = 0; k < 7; k++) if (l[k] == am) {
                unsigned c = o->hitctr_d2[k] < 5 ? o->hitctr_d2[k] : 5u;
                row = rom32(0x23E16u + k * 4u) + c * 2u; noband = 1;   /* 0x1DD2A-0x1DD4E: pointer [k] +
                                          min(+0xD2[k],5)*2 - the more of that move he has eaten, the
                                          better his roll (the ROM's leg-drop/splash/dive escalation; the
                                          engine used to roll on the pointer bytes = never won: "leg drop
                                          forever" vs a CPU, user 2026-08-30) */
            }
        }
        if (row == 0) return 0;
        if (d100_walk(tbl_ra_ptr(row + (noband ? 0u : band * 2u), 2), 2) == 0) {
            o->ai_bc = (uint16_t)gate; o->ai_b5 |= 0x10u;
            ai_log(o, "escape roll won, gate", gate);
        }
    }
    if ((o->ai_b5 & 0x10u) && o->ai_bc && --o->ai_bc == 0) {   /* 0x1DE08 */
        o->mash_aa = 0x4000u;                              /* 0x1DE2C state 5 (0x38/0x78) */
        o->ai_b5 &= (uint8_t)~0x10u;
        return press(o, 0x10u, 2);
    }
    /* Engine guard (TODO EXACT): a lost roll leaves the ROM victim to the
     * attacker's hold timer 0x1254C, which the engine's hold loops lack —
     * after 0xC0 frames escape anyway instead of sitting in the hold. */
    if (!(o->ai_b5 & 0x10u) && ++o->ai_sub_t > 0xC0) {
        o->ai_sub_t = 0; o->mash_aa = 0x4000u;
        ai_log(o, "hold guard escape", am);
        return press(o, 0x10u, 2);
    }
    return 0;
}
static int held_move(unsigned mv)
{ return mv == 0x5D || mv == 0x5E || mv == 0x5F || mv == 0x60 || mv == 0x62 || mv == 0x63 || mv == 0x4A || mv == 0x51
      || mv == 0x52 || mv == 0x53      /* behind grab victims: 0x1E34E / 0x1E368 timers */
      || mv == 0x64; }                 /* 0x23 splash pin victim: 0x1E018[0x64] (0x1E1A8) -> 0x1E452 */

/* ------------------------------------------------------- the CPU TAG */
/* 0x1F3D6 — "wants a tag" (+0xB5 b6): legal, inside, not on autopilot,
 * match live, not rumble, not in the ring-out scene; once set it sticks;
 * a dizzy man (lying react 1) rolls 0x1F458 {0x1E,0x46}; otherwise set
 * when own hp + 0xC < the partner's hp (tag-mode.md §3a). */
static void wants_tag_eval(eng_state *st, eng_obj *o, const eng_obj *tm)
{
    int want = 0;
    if (!(o->driver & DRV_AUTOPILOT) && (st->sig169e & 0x80u) && !(st->g161 & 3u)
        && !(o->role & RF_OUTSIDE) && (o->role & RF_LEGAL)) {
        if (o->ai_b5 & 0x40u) want = 1;
        else if ((o->state & 0xFFu) == ST_REACT && (o->react_id & 0xFFu) == RC_DIZZY)
            want = d100_walk(tbl_ra_ptr(0x1F458u, 2), 2) == 0;
        else want = ((int)o->hp + 0x0C) < (int)tm->hp;
    }
    if (want) { if (!(o->ai_b5 & 0x40u)) ai_log(o, "wants tag", o->hp); o->ai_b5 |= 0x40u; }
    else o->ai_b5 &= (uint8_t)~0x40u;
}
/* sub 8 (0x1D206): walk to the team's corner spot — side 0 (0x200,0x190)
 * / side 1 (0x308,0x128) — and, with the partner ready at the post, tag
 * (state 5 move 0x4C, 0x1D316). TODO EXACT: the 0x1D244 reactions to a
 * standing opponent on the way; +0x23 = 0x60 idle on arrival. */
static uint32_t tag_walk(eng_state *st, eng_obj *o, const eng_obj *tm)
{
    int side = eng_side(o);
    int32_t tx = side ? 0x308 : 0x200, ty = side ? 0x128 : 0x190;
    int32_t dx = tx - (o->x >> 16), dy = ty - (o->y >> 16);
    int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (!tm->apron || ++o->ai_sub_t > 0x400) { o->ai_sub = 0; return 0; }   /* engine guard */
    if (eng_tag_trigger(st, o)) {                          /* 0x1D316 */
        o->ai_b5 &= (uint8_t)~0x40u; o->ai_sub = 0;
        o->state = ST_MOVE; o->move_id = 0x4C; o->grap44 = 0;
        ai_log(o, "tag 0x4C", (unsigned)side);
        return 0;
    }
    if (adx < 4 && ady < 4) return 0;                      /* at the spot: wait
                                          (the spot sits 6 px inside the 0xDD46
                                          corner zone; 8 px slack left him out) */
    return dir_bits(adx >= 4 ? dx : 0, ady >= 4 ? dy : 0);
}
/* 0x1C640 — state-0/1 row head: with the tag wanted, a live opponent who
 * is not dizzy, and the partner on the apron (sub 1 follow / 2 post) ->
 * send the partner to the post and start sub 8. Without the bit, run the
 * evaluator (0x1C6B2). Returns 1 with the walk bits in *out. */
static int tag_logic(eng_state *st, eng_obj *o, const eng_obj *p, uint32_t *out)
{
    eng_obj *tm = &st->obj[o->teammate];
    if (o->ai_sub == 8) { *out = tag_walk(st, o, tm); return 1; }
    if (o->ai_b5 & 0x40u) {
        if (p->hp == 0) { o->ai_b5 &= (uint8_t)~0x40u; return 0; }   /* 0x1C652 */
        if ((p->state & 0xFFu) == ST_REACT && (p->react_id & 0xFFu) == RC_DIZZY) return 0;
        if (tm->apron && (tm->state & 0xFFu) == ST_WALK
            && ((tm->sub & 0x7Fu) == 1 || (tm->sub & 0x7Fu) == 2)) {   /* 0x1C66E */
            tm->sub = 2;                                   /* 0x1D206 once: to the post */
            o->ai_sub = 8; o->ai_sub_t = 0;
            ai_log(o, "tag walk, side", (unsigned)eng_side(o));
            *out = tag_walk(st, o, tm);
            return 1;
        }
        return 0;                  /* TODO EXACT: partner in move 0x71 (0x1C688) */
    }
    wants_tag_eval(st, o, tm);
    return 0;
}

/* =================================================================== */
/* 0x1C94E-0x1CA82 — the tag arms at the top of approach (sub 0), run
 * for every walker-driven man (CPU or autopilot partner). Returns 1 when
 * it consumed the frame (bits in *out), 0 to fall through to the normal
 * approach against *pp (possibly re-pointed). Rumble arm 0x1C958 TODO.
 *   own OUTSIDE (0x1C99E):
 *     opp inside  -> legal: go home (0x1CA3E: +0xB5 b5, move 0x69, walk
 *                    target (0x310,0x13F), state 1 sub 0xA);
 *                    non-legal: +0x7A := opp's partner (0x1CA64) and idle
 *     opp outside -> approach = the ringside BRAWL
 *   own inside, opp outside (0x1C9A8):
 *     legal vs legal     -> roll 0x1C8F0 {0x1E,0x46}: hit -> move 0x3D
 *                           (+0xB7 b6), else +0xB6 b4 rope-move pending
 *     legal vs non-legal -> +0x7A := opp's partner, range, face, approach
 *     non-legal vs legal -> approach
 *     non-legal vs non-legal -> +0x7A := opp's partner, range, face; new
 *                           opp engaged (f33 b6) -> +0xB6 b5, idle; else approach */
static int tag_outside_arms(eng_state *st, eng_obj *o, eng_obj **pp, geo_t *g, uint32_t *out)
{
    eng_obj *p = *pp;
    *out = 0;
    if (st->g161 & 1u) {                                   /* 0x1C958 RUMBLE arm */
        if (o->ai_b6 & 0x40u) {                            /* +0xB6 b6: no target last time */
            o->ai_b6 &= (uint8_t)~0x40u;
            if (!(o->ai_b5 & 0x80u)) { o->ai_b6 |= 0x80u; o->state = ST_STAND; return 1; }   /* 0x1C96A re-decide */
        }
        if (!p->active || (p->role & RF_OUTSIDE) || (p->st_flags & SF_ELIMINATED)) {   /* 0x1C976-0x1C98C */
            o->ai_b6 |= 0x80u; o->state = ST_STAND; return 1;
        }
        return 0;                                          /* 0x1CA82 approach */
    }
    if (o->teammate < 0) return 0;
    if (o->role & RF_OUTSIDE) {                                  /* 0x1C99E own outside */
        if (p->role & RF_OUTSIDE) return 0;                      /* both outside: brawl */
        if (o->role & RF_LEGAL) {                              /* 0x1CA36 legal: go home */
            o->ai_b5 |= 0x20u; o->move_id = 0x69;
            o->run_tgt = 0x310; o->tgt_y = 0x13F;
            o->state = ST_WALK; o->ai_sub = 0xA;
            ai_log(o, "ringside: legal goes home", 0);
            return 1;
        }
        if (p->teammate >= 0) o->opp = p->teammate;     /* 0x1CA64 +0x7A := opp +0x86 */
        return 1;
    }
    if (!(p->role & RF_OUTSIDE)) return 0;                       /* both inside: plain approach */
    if (o->role & RF_LEGAL) {                                  /* 0x1C9B2 own legal */
        if (p->role & RF_LEGAL) {                              /* 0x1C9C2 opp legal */
            if (pair_roll(0x1C8F0u) != 0) {                /* 0x1CA6E: go after him */
                o->ai_b7 |= 0x40u; o->state = ST_MOVE; o->move_id = 0x3D; o->tag_flags &= (uint16_t)~0x10u;
                ai_log(o, "opp outside: chase 0x3D", 0);
                return 1;
            }
            o->ai_b6 |= 0x10u; o->state = ST_STAND;               /* 0x1C9D6 rope-move pending (0x6C TODO) */
            return 1;
        }
    } else if (p->role & RF_LEGAL) return 0;                   /* 0x1C9E8 */
    if (p->teammate < 0) return 0;
    o->opp = p->teammate;                                  /* 0x1C9EC / 0x1CA14 */
    *pp = &st->obj[o->opp];
    geometry(o, *pp, g);                                   /* 0x1C228 */
    eng_face_opponent(st, o);                              /* 0x10BE8 */
    if (!(o->role & RF_LEGAL) && ((*pp)->role & RF_ENGAGED)) {       /* 0x1CA00 new opp engaged */
        o->ai_b6 |= 0x20u; o->state = ST_STAND;
        return 1;
    }
    return 0;
}

static uint32_t ai_inputs_raw(eng_state *st, eng_obj *o);

/* Engine smoothing (user request, NOT ROM): commit to a horizontal steer
 * for at least 6 frames before allowing the opposite one — the approach
 * chooser flips L/R every frame while the alignment error hovers around
 * zero and the CPU visibly jitters. Buttons and the vertical axis pass
 * through untouched. */
uint32_t eng_ai_inputs(eng_state *st, eng_obj *o)
{
    uint32_t bits = ai_inputs_raw(st, o);
    unsigned h = bits & 3u, v = bits & 0xCu;
    unsigned s8 = o->state & 0xFFu;
    {   /* WF_AISTATS: idle = a free stand/walk frame with no input at all;
         * steer flip = the raw horizontal steer reversing (before smoothing) */
        ai_stat_t *a = &ai_stats[o - st->obj];
        if ((s8 == 0 || s8 == 1) && bits == 0 && !o->ai_t && !o->ai_press_t) a->idle++;
        if (s8 == 1 && a->last_s8 == 0) a->walk_starts++;   /* stand -> walk: the start/stop stutter shows here */
        a->last_s8 = (uint8_t)s8;
        if (h && a->last_key != 0xFFFFu) { static uint8_t prev_h[ENG_MAX_OBJS]; unsigned i = (unsigned)(o - st->obj); if (prev_h[i] && h != prev_h[i]) a->steer_flips++; prev_h[i] = (uint8_t)h; }
    }
    if (getenv("WF_NOSMOOTH") || (s8 != 0 && s8 != 1))
        return bits;                   /* smooth only the stand/walk approach — a
                                          runner's skid press or an in-move steer
                                          must never be eaten by the cooldown */
    if (h && o->ai_hdir && h != o->ai_hdir && o->ai_hage < 6)
        { bits = (bits & ~3u) | o->ai_hdir; h = o->ai_hdir; }
    if (o->ai_hcool) { o->ai_hcool--; if (h) { bits &= ~3u; h = 0; } }   /* 3-frame restart
                                  cooldown after a release: the 1-frame step/stop
                                  stutter read as vibration (user 2026-08-27) */
    else if (!h && o->ai_hdir) o->ai_hcool = 3;
    if (h != o->ai_hdir) { o->ai_hdir = (uint8_t)h; o->ai_hage = 0; }
    else if (o->ai_hage < 250) o->ai_hage++;
    if (v && o->ai_vdir && v != o->ai_vdir && o->ai_vage < 6)   /* the vertical axis gets
                                  the same commit + cooldown as horizontal */
        { bits = (bits & ~0xCu) | o->ai_vdir; v = o->ai_vdir; }
    if (o->ai_vcool) { o->ai_vcool--; if (v) { bits &= ~0xCu; v = 0; } }
    else if (!v && o->ai_vdir) o->ai_vcool = 3;
    if (v != o->ai_vdir) { o->ai_vdir = (uint8_t)v; o->ai_vage = 0; }
    else if (o->ai_vage < 250) o->ai_vage++;
    return bits;
}

static uint32_t ai_inputs_raw(eng_state *st, eng_obj *o)
{
    eng_obj *p;
    unsigned state = o->state & 0xFFu;
    geo_t g;
    cur_st = st;
    if ((st->g161 & 1u) && (st->rumble_phase & 4u))
        return 0;                      /* the rumble is WON (0x20254 latched):
                                          hands off the ceremony. Stock's winner
                                          is always human; this covers the
                                          WF_CPU harness winner (engine-only) */
    AST(o).frames++;
    if (o->ai_dec_t) o->ai_dec_t--;                        /* mod ai_commit */
    if (o->ai_press_t) { o->ai_press_t--; return o->ai_press; }
    if (o->ai_t) o->ai_t--;
    if ((st->g161 & 1u) && (o->ai_b5 & 0x80u))              /* rumble: the double-cover helper */
        return eng_rumble_helper_ai(st, o);
    /* A FREE RUN the AI did not launch (a pad hand-over caught the man running):
     * the ROM stops a run only on the OPPOSITE direction (0xF42E) and the AI never
     * pressed it, so the man ran and rebounded forever ("cpu runs back and forth",
     * user 2026-08-30).  Press it. */
    if (state == ST_RUN && (o->grap44 & 0xFFu) == 0 && !o->ai_run_own) return (o->angle & 0x80u) ? 1u : 2u;
    if (state != ST_RUN) o->ai_run_own = 0;
    if (o->teammate >= 0 && !o->apron && !(o->role & RF_LEGAL)
        && (o->ai_sub == 1 || o->ai_runin_t)) {             /* run-in man: 0x1D398 */
        if (state == ST_STAND || state == ST_WALK) return runin(st, o);
        return 0;
    }
    p = (o->opp >= 0) ? &st->obj[o->opp] : 0;
    if (!p || !p->active || o->result) return 0;
    if (p->apron && o->teammate >= 0) {                    /* 0x1DE6C: opp outside */
        o->ai_sub = 0;
        return 0;
    }
    geometry(o, p, &g);
    /* 0x1108C pin intent (jsr'd by the ROM's knockdown enders, which the
     * engine's handlers don't do — gap-map §8): roll once per knockdown,
     * live and not vs a human-controlled self: rng & 0xFF < 0x110C8[id]
     * (WORD table, 0x110B0 lsl #1). */
    if (opp_lying(p)) {
        if (!o->ai_pinroll) {                              /* once per knockdown (engine latch) */
            o->ai_pinroll = 1;
            if ((st->sig169e & 0x80u) && !(o->role & RF_PAD)) {
                unsigned th = tbl16(TBL(ws_pin_intent_chance), (unsigned)eng_ws_base(o->wrestler) * 2u);
                if ((eng_rng_fold(0) & 0xFFu) < th) { o->tag_flags |= TF_PIN_INTENT; ai_log(o, "pin intent", th); }
            }
        }
    } else o->ai_pinroll = 0;

    if ((state == ST_STAND || state == ST_WALK) && !o->apron && (o->role & RF_LEGAL)
        && o->teammate >= 0 && !(st->g161 & 1u)) {        /* 0x1C640 tag head */
        uint32_t tb = 0;
        if (tag_logic(st, o, p, &tb)) return tb;
    }
    switch (state) {
    case 4: {                                              /* 0x1DA7E lying row */
        unsigned rc = o->react_id & 0xFFu;
        eng_obj *a = o->partner >= 0 ? &st->obj[o->partner] : 0;
        if (!(o->state & 0x8000u)) {                       /* 0x1DA86 fresh entry */
            o->ai_b7 &= (uint8_t)~0x03u; o->ai_bc = 0; o->ai_b5 &= (uint8_t)~0x10u;
        }
        if ((rc != RC_LYING && rc != RC_LYING_B) || !a || (a->state & 0xFFu) != ST_MOVE) return 0;
        if (o->mash_aa == 0x4000u) return press(o, 0x10u, 2);
        {
            /* 0x1DB28: the man who linked to me is mid-move. The pickup
             * 0x08 rolls 0x23D7E[stage][band] ONCE and, won, springs up at
             * once (0x1DB88: state 5 move 0x38 — the 0xEC14 ladder's
             * press). The drops 0x10/0x46/0x0B (0x23F78) and the ground
             * holds 0x09/0x22 (0x23FC4) roll once and, won, count +0xBC
             * (0x10 / 2) down before the spring-up (0x1DE08). The cover
             * 0x48 is NOT rolled here — it is the pinned-move row (0x1E452).
             * TODO EXACT: rumble rows 0x1DE48/0x1DE4E. */
            unsigned am = a->move_id & 0xFFu, gate = 0, band = o->band & 0xFFu;
            uint32_t tab = 0;
            if (band > 2) band = 2;
            if (am == 0x08) { tab = 0x23D7Eu; gate = 0; }
            else if (am == 0x10 || am == 0x46 || am == 0x0B) { tab = 0x23F78u; gate = 0x10; }
            else if (am == 0x09 || am == 0x22) { tab = 0x23FC4u; gate = 2; }
            if (tab && !(o->ai_b7 & 0x02u)) {              /* bset #1,+0xB7 once */
                unsigned stg = st->stage < 10 ? st->stage : 9u;
                uint32_t row = rom32(tab + stg * 4u);
                if (st->g161 & 1u)                          /* 0x1DB4E rumble: flat rows, no stage */
                    row = (am == 0x08) ? 0x1DE4Eu : 0x1DE48u;   /* TODO EXACT 0x1DE48 for the other two */
                o->ai_b7 |= 0x02u;
                if (d100_walk(tbl_ra_ptr(row + band * 2u, 2), 2) == 0) {
                    ai_log(o, "lying: escape roll won vs move", am);
                    if (gate == 0) { o->mash_aa = 0x4000u; return press(o, 0x10u, 2); }
                    o->ai_bc = (uint16_t)gate; o->ai_b5 |= 0x10u;
                } else ai_log(o, "lying: escape roll lost vs move", am);
            }
            if ((o->ai_b5 & 0x10u) && o->ai_bc && --o->ai_bc == 0) {   /* 0x1DE08 */
                o->ai_b5 &= (uint8_t)~0x10u; o->mash_aa = 0x4000u;
                return press(o, 0x10u, 2);
            }
        }
        return 0;
    }
    case 0x0B: return lockup(st, o);
    case 0x0C:                                             /* hold won: 0x1F078 cascade */
        if (o->hold_ph == 1 && !o->pinning && o->partner >= 0) {
            if (!(o->ai_b7 & 0x02u)) {
                unsigned mv = cascade_pick(st, o, &st->obj[o->partner]);
                o->ai_b7 |= 0x02u; o->ai_mv = (uint8_t)mv;
                ai_log(o, "cascade", mv);
                return press(o, mv == 0x17 ? 0x20u : 0x10u, 2);   /* B2 whip / B1 facelock */
            }
        }
        return 0;
    case 5: {
        unsigned mv = o->move_id & 0xFFu;
        if (!(o->state & 0x8000u))                         /* fresh move entry: the ROM's
                                          escape-timer latch is bset #6,(+0x60), and +0x60
                                          is REWRITTEN by every move entry (0x14D36) — so
                                          the latch is per-move. The engine's ai_b7 b1
                                          persisted across moves: a CPU who had armed any
                                          cascade earlier kept the bit, and when he was
                                          later caught in a hold (0x5D/0x5F/0x60/...) the
                                          0x1F576 escape timer never armed — he hung in
                                          the hold until a third man broke it ("perfect
                                          and hogan grappling forever", rumble playtest
                                          2026-08-24: a 1400-frame mount at f569). */
            o->ai_b7 &= (uint8_t)~0x02u;
        if (held_move(mv)) return victim_escape(st, o);    /* 0x1DA7E family */
        if ((mv == 0x15 || mv == 0x16) && o->partner >= 0 && o->ai_t == 0) {   /* stance: the throw */
            /* the stance resolves cat A-D by the 0xE232 matrix (core.c);
             * press the column that holds the cascade's move where the
             * row has it, else B1 — skipping unrouted cells */
            eng_obj *v2 = &st->obj[o->partner];
            unsigned bank = (((o->alt62 >> 7) & 1u) ^ 1u);
            unsigned cat = tbl8(TBL(throw_matrix), (unsigned)v2->band * 0x18u + bank * 0xCu + (unsigned)eng_ws_base(o->wrestler));
            unsigned mrow = cat * 3u;   /* package override, else the ROM row */
            int c = column_for(o, cat, o->ai_mv);
            if (c < 0 || !routed(eng_ws_move8((unsigned)o->wrestler, mrow + (unsigned)c)))
                c = routed(eng_ws_move8((unsigned)o->wrestler, mrow)) ? 0
                  : routed(eng_ws_move8((unsigned)o->wrestler, mrow + 1u)) ? 1 : -1;
            o->ai_t = 0x20;
            if (c < 0) return 0;
            return press(o, col_bits(c), 2);
        }
        return 0; }
    case 0xFF: {
        /* Held victim (+0x21 == 0xFF). The ROM AI loop skips him outright
         * (0x1C186 cmpi.b #$FF,(+0x21)) because ITS plain-cover victim
         * lies in state 5 move 0x4A, whose 0x1E018 row is the pinned-man
         * kick-out 0x1E452. The engine's cover keeps the simplified
         * 0x0C/0xFF pair (handler_cover TODO EXACT), so the 0x1E452 row
         * runs from here for a victim whose partner is the covering man;
         * the kick-out press is consumed by handler_hold's pin branch
         * (the 0x1820C release). Any other 0xFF man stays passive, as the
         * ROM's does. */
        eng_obj *a = o->partner >= 0 ? &st->obj[o->partner] : 0;
        if (!(o->state & 0x8000u))                         /* fresh entry: same per-move
                                          +0x60 latch rule as case 5 (0x14D36) */
            o->ai_b7 &= (uint8_t)~0x02u;
        if (!a || !eng_pin_is_pinner(a)
            || a->partner != (int)(o - st->obj)) { o->ai_sub = 0; return 0; }
        if (o->mash_aa == 0x4000u) return press(o, 0x10u, 2);
        if (!(o->ai_b7 & 0x02u)) {                         /* bset #6,+0x60 once */
            unsigned cls = pin_kick_class(o->hp);          /* 0x1E48C */
            o->ai_b7 |= 0x02u; o->ai_bd = (uint8_t)cls;
            ai_log(o, "pinned (cover): kick-out at half-count", cls);
        }
        if (o->halfct == o->ai_bd) {                       /* 0x1E45C */
            o->mash_aa = 0x4000u;
            return press(o, 0x10u, 2);
        }
        return 0; }
    case 2: return run_tick(st, o, p, &g);
    case 8: o->ai_bd = 0; return 0;                        /* 0x1EE98 */
    case 9: return perch(o, p);
    case 0: case 1: break;
    default: o->ai_sub = 0; return 0;                      /* CPU never mashes (0x10D04) */
    }
    if (o->ai_t) return 0;
    if (o->ai_sub == 0x3C) return pseudo_3C(o, p, &g);
    if (o->ai_sub == 0x3D) return pseudo_3D(o, p);
    if (o->ai_sub == 0x3E) return pseudo_3E(o, &g);
    if (o->ai_sub == 0xA) return walk_to(st, o, p, &g);
    if (o->ai_sub == 9) return corner_walk(st, o, p);
    if ((st->g161 & 2u) && (o->state & 0x8000u)            /* 0x1C6B6 weapon arm: ringside
                                                              scene, state 0/1 */
        && !(o->weapon_w & WPN_HELD)                             /* 0x1C6C2 not holding */
        && (o->role & RF_OUTSIDE)) {                             /* engine: only an OUTSIDE man
                                                              shops (stock filters the inside
                                                              legal man off at 0x1C6D4 via the
                                                              go-home branch — ringout.c) */
        int best = -1; int32_t bd = 0, bx = 0, by = 0;
        for (int k = 0; k < ENG_WEAPONS; k++) {            /* 0x1C70C / 0x1C736 scan */
            const eng_weapon *w = &st->wpn[k];
            int32_t d;
            if (!w->active || (w->state & 0xFFu) != ST_SKID || w->holder) continue;
            d = (w->x >> 16) - (o->x >> 16); if (d < 0) d = -d;   /* 0x1C76A/0x1C772 */
            if (best < 0 || d < bd) {                      /* 0x1C77A nearer wins */
                best = k; bd = d; bx = w->x >> 16; by = w->y >> 16;
            }
        }
        if (best >= 0 && bd < g.adx) {                     /* 0x1C796 cmp (+0xB0): closer
                                                              than the opponent */
            unsigned stg = st->stage < 10 ? st->stage : 9u;
            uint32_t row = rom32(0x23ED0u + (uint32_t)eng_ws_base(o->wrestler) * 4u) + stg * 2u;   /* 0x1C79E-0x1C7B8 */
            if (pair_roll(row) == 0) {                     /* 0x1C7BA jsr 0x24CC */
                o->wobj = 1 + best;                        /* 0x1C784/0x1C78E +0x76 */
                o->ai_mv = 0x70;                           /* 0x1C7C6 (fired on arrival) */
                o->ai_tx = (int16_t)bx; o->ai_ty = (int16_t)by;   /* 0x1C7CC/0x1C7D0 */
                o->ai_sub = 0xA; o->ai_sub_t = 0;          /* 0x1C7D4/0x1C7DA state 1 sub 0xA */
                ai_log(o, "weapon walk", (unsigned)best);
                return walk_to(st, o, p, &g);
            }
        }
    }
    {   uint32_t tb;                                       /* 0x1C94E-0x1CA82 tag arms */
        if (tag_outside_arms(st, o, &p, &g, &tb)) return tb;
    }
    if (state == ST_STAND) return stand(st, o, p, &g);
    /* state 1: the ROM runs sub 0 (approach) while walking; a walking man
     * with an armed follow-up keeps closing until the gate (0x1C5B6). */
    if ((o->ai_b5 & 0x08u) && g.face_diff && g.adx < (int32_t)o->ai_bc && (p->role & RF_RUNNING))
        return fire_move(st, o, p, o->ai_mv);
    return approach(st, o, p, &g);
}
