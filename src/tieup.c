/* Facing + tie-up entry — transcription of ROM 0x10BE8 (face_opponent)
 * and 0xF574/0xF7E8/0xF5E4 (pass-2 tie-up scan, eligibility, resolution).
 * Spec: docs/engine-specs/face-tieup.md (2026-08-22).
 *
 * There is NO wrestler-vs-wrestler body collision in this game: the tie-up
 * scan is the only interaction; same-facing wrestlers pass through.
 */
#include <stdio.h>
#include <stdlib.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
extern int eng_dbgsel;

/* ROM tables this file reads (docs/table-migration-brief.md). 0xF6D6: the
 * HUMAN-vs-CPU tie-up bias row is 0xF878 + difficulty($1C0162)*8, four
 * words indexed by the human's exchange count (+0x48 & 3); the rumble
 * (0xF6CE) uses the single row 0xF8D0. Eleven stage rows precede the
 * rumble row; code resumes at 0xF8D8. */
static const tbl_def tieup_tables[] = {
    { "tieup_bias_by_stage", "base/tieup", 0xF878, 11 * 8, TK_U16, 4,
      "0xF6D6 human-vs-CPU tie-up bias: [stage/difficulty 0..10][exchange 0..3] word added to rng&0xF (>=0x14 human wins, <0xF CPU wins, else lockup)" },
    { "tieup_bias_rumble",   "base/tieup", 0xF8D0, 8, TK_U16, 4,
      "0xF6CE rumble tie-up bias row: [exchange 0..3] word, same thresholds; code 0xF8D8" },
};
TBL_REGISTER(tieup_tables)

/* 0x10BE8: face the +0x7A opponent, unsigned compare, no hysteresis.
 * Lead applies only when the opponent runs/staggers (state 4 anim 8/9,
 * state 5 anim 0x22/0x52) — those states are not yet transcribed, so the
 * lead branch is a stub returning 0. */
void eng_face_opponent(eng_state *st, eng_obj *o)
{
    const eng_obj *p;
    uint16_t px;

    if (o->opp < 0)
        return;
    p = &st->obj[o->opp];
    px = (uint16_t)(p->x >> 16);        /* + lead (0 until states 4/5 land) */
    {   /* 0x10C46 unsigned compare — plus a 3px dead band the ROM lacks:
         * the rumble retarget flips targets around the own x every frame
         * and the man jittered left-right (user 2026-08-24, "add a bit of
         * debounce"). Inside the band the facing HOLDS. */
        int32_t dx = (int32_t)px - (int32_t)(uint16_t)(o->x >> 16);
        if (dx > 2)        o->facing |= 0x8000u;    /* bset #7 +0x2E */
        else if (dx < -2)  o->facing &= 0x7FFFu;
    }
}

/* 0xF7E8 eligibility — the subset of gates whose fields exist so far.
 * Missing gates (hit latch +0xFE, +0x32 b4/b0, +0xB7 b7,
 * outside-ring +0x33 b2) come with their systems. */
/* Reason codes for the WF_DBGSEL diagnostic (0 = eligible). */
enum { TU_OK = 0, TU_INACTIVE, TU_APRON, TU_RESULT, TU_UNLATCHED,
       TU_STATE5, TU_STATE4, TU_STATE, TU_PARTNER };
static const char *tu_why[] = { "ok", "inactive", "apron", "match-decided",
    "unlatched", "state5-move", "state4-react", "state", "partner-linked" };

static int eligible_why(const eng_obj *o)
{
    unsigned state;

    if (!o->active) return TU_INACTIVE;
    if (o->st_flags & SF_OUT_OF_PLAY) return TU_INACTIVE;  /* +0x32 b4 eliminated / b15 queued (rumble) */
    if (o->weapon_w & WPN_HELD) return TU_STATE;     /* 0xF7E8: holding a weapon — no tie-up */
    if (o->apron) return TU_APRON;      /* 0xF810: +0x32 b0 OUTSIDE */
    /* NOTE: 0xF7E8 does NOT test the autopilot bit (+0x56 b6) — a
     * CPU-driven or puppeteered man engages exactly like a human. That is
     * how the two partners who run in during a pin tie up with each other.
     * (An earlier autopilot gate here made that impossible.) */
    if (o->result) return TU_RESULT;    /* 0xF7F2: the match is decided */
    if (o->tag_flags & TF_RECALL) return TU_STATE;   /* tag recall walk (the engine walks the tagged-out man
                                              out in state 1 where stock keeps him in move 0x4F,
                                              state 5 — not tie-up eligible); a lockup with a man
                                              leaving for the apron froze both (tag long run) */
    if (!(o->state & 0x8000u))          /* 0xF808: latched >= 1 frame */
        return TU_UNLATCHED;
    state = o->state & 0xFFu;
    /* 0xF82A state gate: stand/walk, or state 5 with a strike in progress
     * (jab 0x00 or kick 0x72) — the "engage by punch or kick" rule. */
    if (state == ST_MOVE) {
        unsigned mv = o->move_id & 0xFFu;
        if (mv != 0x00u && mv != 0x72u)
            return TU_STATE5;
    } else if (state == ST_REACT) {
        unsigned rc = o->react_id & 0xFFu;          /* 0xF82A: flinch/dizzy */
        if (rc != 0 && rc != RC_DIZZY)
            return TU_STATE4;
    } else if (state != ST_STAND && state != ST_WALK)
        return TU_STATE;
    if (o->partner >= 0)                /* 0xF866: +0x26 engaged */
        return TU_PARTNER;
    return TU_OK;
}

static int eligible(const eng_obj *o) { return eligible_why(o) == TU_OK; }

/* 0xF5E4 resolution + 0xF726/0xF742/0xF760 writes + 0xF77E common tail.
 * A0 = the lower slot (0x250E iteration order), A1 = his opponent.
 *   0xF600: combos cleared, f34 b2/b6/b7 cleared, humans lose autopilot.
 *   0xF648-0xF68C: a side with f33 b5 wins outright (human or CPU).
 *   0xF690: a dizzy side (state 4 react 1) loses outright.
 *   0xF6B4: HUMAN(A0) vs CPU(A1) -> bias table: 0xF8D0 in the rumble
 *     (+0x48 exchange index), else 0xF878 + difficulty($1C0162)*8;
 *     D2 = table[exch] + rng&0xF; >= 0x14 A0 wins, < 0xF A1 wins, else
 *     lockup. Every other pairing (both human, A0 CPU): the frame-counter
 *     parity ($1C0083>>1)&7: >= 5 A0 wins, < 2 A1 wins, else lockup.
 * TODO EXACT: $1C0162 is fed by st->stage (the engine's match counter);
 * the rumble row 0xF8D0 is selected on $1C0161 b0 (engine g161 b0). */
/* CPU-vs-CPU tie-ups (user 2026-08-27: "two cpus grapple for too long"):
 * the ROM's 0xF70C parity roll locks up 3 times in 8 and every lockup
 * re-rolls after a full hold clock, so two CPUs can stall for many
 * seconds. Engine rule: after cpu_lockup_max consecutive lockups the
 * next roll is forced to a winner (coin flip). Stock value 1. */
static const uint8_t tieup_rules_be[] = { TBL_BE16(1) };
static const char *const tieup_rule_labels[] = { "cpu_lockup_max", NULL };
static const tbl_def tieup_rule_tables[] = {
    { "tieup_rules", "rules", TBL_SYNTH, sizeof tieup_rules_be, TK_U16, 1,
      "engine scalar: consecutive lockups two CPU men may roll before the tie-up is forced to a winner (stock ROM behaviour = unlimited; 1 keeps CPU-vs-CPU exchanges short)", tieup_rules_be, tieup_rule_labels },
};
TBL_REGISTER(tieup_rule_tables)
static int cpu_lockup_max(void)
{
    if (tbl_bytes(TBL(tieup_rules), NULL)) return (int)tbl16(TBL(tieup_rules), 0);
    return 1;
}
static void resolve(eng_state *st, eng_obj *a, eng_obj *b)
{
    enum { A_WINS, B_WINS, LOCKUP } r;
    unsigned d4 = ((unsigned)(st->frame >> 1)) & 7u;   /* $1C0083 parity */

    a->combo = 0; b->combo = 0;                        /* 0xF600 */
    a->tag_flags &= (uint16_t)~0xC4u; b->tag_flags &= (uint16_t)~0xC4u;
    if (a->role & RF_PAD) a->driver &= (uint16_t)~0x40u;   /* 0xF614/0xF622: keyed on the HUMAN bit
                                                         +0x33 b1 — an autopilot partner (not
                                                         human, not CPU) keeps his autopilot */
    if (b->role & RF_PAD) b->driver &= (uint16_t)~0x40u;

    if (a->role & RF_ONFIRE)      r = A_WINS;               /* 0xF648-0xF68C */
    else if (b->role & RF_ONFIRE) r = B_WINS;
    else if ((a->state & 0xFFu) == ST_REACT && (a->react_id & 0xFFu) == RC_DIZZY) r = B_WINS; /* 0xF690 */
    else if ((b->state & 0xFFu) == ST_REACT && (b->react_id & 0xFFu) == RC_DIZZY) r = A_WINS;
    else if (!a->cpu && b->cpu) {                      /* 0xF6B4 human vs CPU */
        unsigned d2;
        if (st->g161 & 1u)                             /* 0xF6C4 btst #0,$1C0161:
                                                          the RUMBLE row 0xF8D0 */
            d2 = tbl16(TBL(tieup_bias_rumble), (a->exch & 3u) * 2u);
        else {                                         /* 0xF6D6 stage row */
            int row = (int)(st->stage < 10 ? st->stage : 9u)
                    + (eng_mod_rule(MODR_DIFFICULTY) - 0x80);   /* mod offset */
            if (row < 0) row = 0; else if (row > 10) row = 10;
            d2 = tbl16(TBL(tieup_bias_by_stage),
                       (uint32_t)row * 8u                       /* $1C0162 */
                       + (a->exch & 3u) * 2u);
        }
        d2 += eng_rng() & 0xFu;                        /* 0x21B4 */
        r = (d2 >= 0x14u) ? A_WINS : (d2 < 0x0Fu) ? B_WINS : LOCKUP;
    } else {                                           /* 0xF70C parity */
        r = (d4 >= 5) ? A_WINS : (d4 < 2) ? B_WINS : LOCKUP;
    }

    if (a->cpu && b->cpu) {                            /* engine: CPU-vs-CPU lockup cap */
        if (r == LOCKUP && ++a->lock_n > cpu_lockup_max()) {
            r = (eng_rng() & 1u) ? A_WINS : B_WINS;
            if (eng_dbgsel) fprintf(stderr, "tieup: CPU-vs-CPU lockup cap -> forced %s\n", r == A_WINS ? "A" : "B");
        }
        if (r != LOCKUP) a->lock_n = 0;
    }
    if (eng_dbgsel)
        fprintf(stderr, "tieup: P%d(exch %u%s) vs P%d(exch %u%s) -> %s\n",
                (int)(a - st->obj) + 1, a->exch & 3u, a->cpu ? " cpu" : "",
                (int)(b - st->obj) + 1, b->exch & 3u, b->cpu ? " cpu" : "",
                r == A_WINS ? "A" : r == B_WINS ? "B" : "lockup");
    if (r == A_WINS) {                  /* 0xF760 */
        a->pinning = 0; a->hold_ph = 0;   /* the 0x0C lean gate re-arms (a stale 1 let the
                                             press fire on the entry frame - "lean skipped") */
        a->state = 0x000C;
        a->ai_bc = 0;                   /* 0xF766 clr.w (+0xBC): the winner's AI
                                           countdown — NOT the +0x48 exchange
                                           counter, which keeps cycling 0..3 so
                                           the 0xF878 bias row reaches the
                                           CPU-favoured exchanges */
        b->state = ST_HELD;              /* held victim */
        a->grap44 = 0x2000;
        b->grap44 = 0;
    } else if (r == B_WINS) {           /* 0xF742 */
        a->state = ST_HELD;
        b->pinning = 0; b->hold_ph = 0;
        b->state = 0x000C;
        b->ai_bc = 0;                   /* 0xF74E clr.w (+0xBC,A1) */
        a->grap44 = 0;
        b->grap44 = 0x2000;
    } else {                            /* lockup: 0xF726 */
        a->state = 0x000B;
        b->state = 0x000B;
        a->grap44 = 0x2000;             /* +0x44 b13: impact cell first */
        b->grap44 = 0x8000 | 0x2000;    /* hidden half */
    }
    {                                   /* 0xF77E tail: midpoint snap */
        int32_t mx = (((a->x >> 16) + (b->x >> 16)) >> 1) << 16;
        int32_t my = (((a->y >> 16) + (b->y >> 16)) >> 1) << 16;
        a->x = b->x = mx;
        a->y = b->y = my;
    }
    a->combo = b->combo = 0;            /* 0xF600 cleanup: +0x52 := 0 both */
    a->exch = (a->exch + 1) & 3;        /* +0x48 mod 4 */
    b->exch = (b->exch + 1) & 3;
    a->partner = (int)(b - st->obj);    /* +0x26 cross-link */
    b->partner = (int)(a - st->obj);
    a->opp = a->partner;                /* +0x7A rewritten */
    b->opp = b->partner;
    a->mover = 0;
    b->mover = 0;
}

/* 0xF574: pair = (object, its +0x7A opponent); first success engages both,
 * which fails the +0x26 gate on the second visit. */
void eng_tieup_scan(eng_state *st)
{
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *a = &st->obj[i];
        eng_obj *b;
        if (!a->active || a->opp < 0)
            continue;
        b = &st->obj[a->opp];
        if (!eligible(a) || !eligible(b)) {
            /* diagnostic: a pair that is close and opposed but cannot engage
             * — the "grapple stopped working" case. WF_DBGSEL=1 only. */
            if (eng_dbgsel
                && ((a->facing ^ b->facing) & 0x8000u)
                && (uint16_t)abs((int)(b->x >> 16) - (int)(a->x >> 16)) < 0x48
                && (uint16_t)abs((int)(b->y >> 16) - (int)(a->y >> 16)) < 0x10) {
                static int last[ENG_MAX_OBJS];
                int wa = eligible_why(a), wb = eligible_why(b), key = wa * 16 + wb;
                if (last[i] != key) {
                    last[i] = key;
                    fprintf(stderr, "tieup: P%d+P%d blocked (%s / %s) st=%04X/%04X mv=%02X/%02X\n",
                            i + 1, a->opp + 1, tu_why[wa], tu_why[wb],
                            a->state, b->state, a->move_id & 0xFF, b->move_id & 0xFF);
                }
            }
            continue;
        }
        if (!((a->facing ^ b->facing) & 0x8000u))      /* 0xF594 opposed */
            continue;
        if (((a->role ^ b->role) & 0x04u))               /* 0xF5A2: same side of the
                                                          ropes (+0x33 b2 equal) */
            continue;
        if ((uint16_t)abs((int)(b->x >> 16) - (int)(a->x >> 16)) >= 0x48)
            continue;                                  /* 0xF5B6 */
        if ((uint16_t)abs((int)(b->y >> 16) - (int)(a->y >> 16)) >= 0x10)
            continue;                                  /* 0xF5CA */
        resolve(st, a, b);                             /* 0xF5E4 */
    }
}

/* 0x2095A / 0x20A3A — the per-frame TARGET SELECTOR (tag + singles; the
 * rumble halves are transcribed in rumble.c: humans 0x20E00, CPU 0x20AA6).
 * This is what re-points +0x7A after a run-in brawl ends: without it a
 * legal man stayed FIXATED on the enemy partner who had returned to the
 * apron, and since the tie-up scan pairs strictly through +0x7A (0xF586)
 * the legal pair could never grapple again ("kind of a soft lock",
 * playtest 2026-08-24). */

/* 0x20CCC (A2 = a HUMAN-driven man; D4 scan). Picks between the enemy
 * LEGAL man (a3) and his teammate (a4); +0x82 also gets the loser of the
 * choice in the ROM (TODO EXACT: the engine has no alt-target field yet
 * and nothing reads +0x82). */
/* a3 = the ENEMY side's legal man, a4 = the most relevant other enemy:
 * an in-ring intruder first, else the (first) apron man. For 2-man teams
 * this is exactly the ROM's teammate-hop normalise (0x20CFE exg); for
 * TRIOS the hop landed on another apron man — and the scan itself never
 * side-checked, so a second own teammate could read as "the enemy"
 * ("the cpu wrestler is just still... i cant grapple him", 3-man
 * playtest 2026-08-24). */
static int enemy_pair(eng_state *st, const eng_obj *o, eng_obj **a3, eng_obj **a4)
{
    eng_obj *legal = 0, *inring = 0, *apron = 0;
    for (int j = 0; j < ENG_MAX_OBJS; j++) {
        eng_obj *p = &st->obj[j];
        if (!p->active || (p->st_flags & SF_ELIMINATED)) continue;
        if (!((p->role ^ o->role) & 0x80u)) continue;     /* enemies only */
        if (p->role & RF_LEGAL) legal = p;
        else if (p->apron) { if (!apron) apron = p; }
        else if (!inring) inring = p;
    }
    if (!legal && !inring && !apron) return 0;
    *a3 = legal ? legal : (inring ? inring : apron);
    *a4 = inring && inring != *a3 ? inring : apron;
    return 1;
}

static void select_human(eng_state *st, eng_obj *o)
{
    {
        eng_obj *a3, *a4;
        int32_t d3, d4;
        if (!enemy_pair(st, o, &a3, &a4))
            return;                                     /* 0x20CD4-0x20CE8 */
        if (o->role & RF_LEGAL) {                           /* 0x20CEC: I am LEGAL */
            if ((o->role & RF_OUTSIDE) && !(a3->role & RF_OUTSIDE) && a4)
                { o->opp = (int)(a4 - st->obj); return; }   /* 0x20D0A: I'm outside,
                                          the legal enemy isn't -> his partner */
            if (!a4 || a4->apron)                       /* 0x20D1E: enemy partner is
                                          back OUT (+0x32 b0) -> the LEGAL man.
                                          The fix case for the fixation lock. */
                { o->opp = (int)(a3 - st->obj); return; }
            if (!(a3->role & RF_ENGAGED))                     /* 0x20D28 not engaged */
                { o->opp = (int)(a3 - st->obj); return; }
            if ((a3->state & 0xFFu) == ST_MOVE && (a3->move_id & 0xFFu) == 0x61u)
                { o->opp = (int)(a3 - st->obj); return; }   /* 0x20D32 */
            if (!(a3->cue_flags & CF_SUB_HOLDER))                     /* 0x20D44 no submission cue:
                                                           the loose partner */
                { o->opp = (int)(a4 - st->obj); return; }
            d3 = abs((int)(a3->x >> 16) - (int)(o->x >> 16))
               + abs((int)(a3->y >> 16) - (int)(o->y >> 16));   /* 0x20D4E */
            d4 = abs((int)(a4->x >> 16) - (int)(o->x >> 16))
               + abs((int)(a4->y >> 16) - (int)(o->y >> 16));
            o->opp = (int)((d4 < d3 ? a4 : a3) - st->obj);      /* 0x20D82 */
            return;
        }
        /* 0x20D88: I am the INTRUDER / apron man */
        if (o->apron)                                   /* 0x20D9C */
            { o->opp = (int)(a3 - st->obj); return; }
        if (!a4 || a4->apron)                           /* 0x20DA4 */
            { o->opp = (int)(a3 - st->obj); return; }
        if (a3->role & RF_ENGAGED) {                          /* 0x20DAC engaged */
            if (a3->cue_flags & CF_PIN_CUE) { o->opp = (int)(a3 - st->obj); return; }   /* 0x20DB4 pin */
            if (a3->cue_flags & CF_SUB_HOLDER) { o->opp = (int)(a3 - st->obj); return; }   /* 0x20DBC sub */
            if ((a3->state & 0xFFu) == ST_MOVE && (a3->move_id & 0xFFu) == 0x1Du)
                { o->opp = (int)(a3 - st->obj); return; }   /* 0x20DCC pin hold */
            o->opp = (int)(a4 - st->obj); return;           /* 0x20DEA */
        }
        o->opp = (int)(((a4->role & RF_ENGAGED) ? a3 : a4) - st->obj);   /* 0x20DD6 */
        return;
    }
}

/* 0x20B44 (A2 = a CPU/autopilot man). Only the LEGAL branch is exact;
 * the ROM's non-legal branch reads a STALE A4 on its first test (0x20B94
 * runs before A4 is loaded) and non-legal CPU men here are governed by
 * the rescue/usher machinery anyway, so they keep their assigned target
 * (TODO EXACT). */
static void select_cpu(eng_state *st, eng_obj *o)
{
    if (!(o->role & RF_LEGAL)) {
        /* ENGINE (user 2026-08-30, "my CPU partner just stares at me and
         * mirrors my movements after a switch-back"): a NON-LEGAL autopilot
         * man inside the ring keeps the ROM's priorities - (1) an armed
         * rescue keeps its target (the 0x1D3F0 think), (2) the enemy's
         * loose partner, (3) the legal enemy - and never his own teammate
         * (the stale +0x7A the pad hand-over left him staring at). TODO
         * EXACT: stock's 0x20B94 branch reads a stale A4. */
        eng_obj *a3, *a4;
        if (o->apron || (o->ai_b5 & 0x80u)) return;
        if (!enemy_pair(st, o, &a3, &a4)) return;
        {   eng_obj *pick = (a4 && !a4->apron && !(a4->st_flags & SF_ELIMINATED) && !a4->result) ? a4 : a3;
            if (pick == a4 && (a4->state & 0xFFu) == ST_REACT && a3 && !a3->apron && (a3->state & 0xFFu) != ST_REACT
                && !(a3->role & RF_ENGAGED)) pick = a3;       /* the loose partner is DOWN and the legal
                                                           man is free: take him meanwhile */
            o->opp = (int)(pick - st->obj); }
        return;
    }
    {
        eng_obj *a3, *a4;
        if (!enemy_pair(st, o, &a3, &a4))
            return;                                     /* 0x20B4C-0x20B5C */
        {
            eng_obj *pick = a3;                         /* 0x20B8E default */
            if ((a3->role & RF_ENGAGED) && a4) {              /* 0x20B7A legal enemy engaged */
                eng_obj *tm = o->teammate >= 0 ? &st->obj[o->teammate] : 0;
                if (tm && !tm->apron)                   /* 0x20B82: my own partner is
                                          inside too (the pin brawl) -> pair off
                                          with the enemy PARTNER */
                    pick = a4;
            }
            /* user spec 2026-08-24 (TODO EXACT vs 0x20B44): a run-in CPU
             * brawls the opposing intruder, but if his pick is DOWNED he
             * takes the STANDING enemy instead. */
            if ((pick->state & 0xFFu) == ST_REACT) {
                eng_obj *other = (pick == a3) ? a4 : a3;
                if (other && !other->apron && (other->state & 0xFFu) != ST_REACT)
                    pick = other;
            }
            o->opp = (int)(pick - st->obj); return;
        }
    }
}

/* 0x2095A loop head (humans) + 0x20A3A loop head (CPU). */
void eng_retarget_tick(eng_state *st)
{
    if (st->g161 & 1u) return;                          /* rumble half in rumble.c */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        unsigned state;
        if (!o->active) continue;
        state = o->state & 0xFFu;
        if (!o->cpu && !(o->driver & DRV_ANY_CPU)) {             /* HUMAN pad man (the engine
                                          keeps the ROM's +0x56 b7 in `cpu`, not in f56:
                                          a CPU legal man went down the pad-man branch,
                                          whose f34 b6/b7 skip parked him on a stale
                                          target — playtest 2026-08-26 "faced away and
                                          didn't engage" after the whip flags stuck) */
            if (o->tag_flags & TF_WHIP_MARKS) continue;               /* 0x20980/0x2098A */
            if (state != ST_STAND && state != ST_WALK && state != ST_PERCH) continue;   /* 0x20994 */
            select_human(st, o);
        } else {                                        /* 0x20A3A CPU/autopilot */
            if (o->ai_b5 & 0x80u) {                     /* 0x20A5A armed rescuer keeps
                                                           his assigned target... */
                eng_obj *t = o->opp >= 0 ? &st->obj[o->opp] : 0;
                if (t && t->active && !t->apron && (t->state & 0xFFu) != ST_REACT
                    && !(t->st_flags & SF_ELIMINATED) && !t->result)
                    continue;   /* hardening (playtest 2026-08-24, "Hawk just
                                   stood there"): an ELIMINATED or result-
                                   stamped body is never a kept target — the
                                   rescuer re-pairs like any inside man */
                /* ...unless it is gone/out/DOWNED: then he re-pairs like
                 * any inside man (user spec 2026-08-24: brawl the standing
                 * opposing man). The stomp-if-pinned goal outranks this in
                 * the rescue think itself (0x1D3F0 family, ai.c). */
            }
            if (o->tag_flags & TF_PIN_INTENT) continue;               /* 0x20A62 pin intent */
            if (!(o->state & 0x8000u)) continue;        /* 0x20A6A latched */
            if (state == ST_STAND) {
                if (o->ai_b5 & 0x08u) continue;         /* 0x20A7A */
            } else if (state == ST_WALK) {
                if (o->sub != 0) continue;              /* 0x20A8E +0xAF scripted walk */
            } else continue;
            select_cpu(st, o);
        }
    }
}
