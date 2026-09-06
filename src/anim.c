/* Animation core — transcription of ROM 0x1C03E (pick+handler+tick),
 * 0x1C0C8 (tick), 0x1C12C (load), and the state->anim latch 0xF4E0.
 * Spec: scratchpad mv/anim.md (2026-08-22).
 *
 * Cell records are read through the table layer (tbl.h; anim_tables[]
 * below, state table 0x11478):
 *   +0 u32 handler PC   +4 u16 mode (0 handler-driven, 2 loop, else hold)
 *   +6 u16 n            +8 n u16 durations (0xFF00 hold), then n sprites.
 * Handlers embed ROM PCs; the engine maps the PCs it has transcribed to C
 * functions (state 0 stand 0x114B2, state 1 walk 0x11652/0x116C6 so far)
 * and treats any other cell as plain-tick.
 */
#include <stdio.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include <stdlib.h>

extern int eng_dbgsel;

static eng_state *cur_st;   /* frame-scoped, set by eng_anim_tick */

#define TAB_STATE_CELLS 0x11478u
#define TAB_MOVE_CELLS  0x12614u
#define TAB_VICTIM_CELLS 0x1AFD4u

/* Tables this file owns (docs/adr-001-data-formats.md, docs/table-migration-
 * brief.md). Bounds from reference/maincpu.asm. The three cell-record blobs
 * are pointer-chased (the pointer tables and every handler's cell constants
 * are ROM addresses into them) and are read through rom16()/rom32() =
 * tbl_ra*; each blob is the whole span between its pointer table and the
 * next pointer table / routine, records interleaved with the 68k handler
 * code that lives between them. */
static const tbl_def anim_tables[] = {
    { "state_cell_table",   "base/anim", 0x11478u, 13 * 4, TK_U32, 1,
      "0x1C090 pick: state 0..12 -> cell record address (13 longs, 0x11478..0x114AB)" },
    /* the state-cell span 0x114AC..0x12614 holds two per-wrestler byte
     * tables inside the handler code (ws_walk_speed 0x116AE read by 0x11754,
     * ws_run_speed 0x11D4A read by 0x11C96/0x1760A/0x1771A) — carved out as
     * `wrestler` tables, the rest stays three record blobs */
    { "state_cell_records_a", "base/anim", 0x114ACu, 0x116AEu - 0x114ACu, TK_U16, 16,
      "state cell records {handler.l, mode.w, n.w, n durations.w, n sprites.w} + their handler code, 0x114AC up to ws_walk_speed (read by 0x1C0AA/0x1C0C8/0x1C12C)" },
    { "ws_walk_speed",        "wrestler",  0x116AEu, 12, TK_U8, 1,
      "per-wrestler walk speed byte -> +0x2B (0x11754; tired.md: +6 when on fire, -8 with a weapon)" },
    { "state_cell_records_b", "base/anim", 0x116BAu, 0x11D4Au - 0x116BAu, TK_U16, 16,
      "state cell records + handler code between ws_walk_speed and ws_run_speed" },
    { "ws_run_speed",         "wrestler",  0x11D4Au, 12, TK_U8, 1,
      "per-wrestler run speed byte -> +0x2B (0x11C96 state-2 run cell; whip paths 0x1760A/0x1771A)" },
    { "state_cell_records_c", "base/anim", 0x11D56u, 0x12614u - 0x11D56u, TK_U16, 16,
      "state cell records + handler code from ws_run_speed up to the move table 0x12614" },
    { "move_cell_table",    "base/anim", 0x12614u, 0x8F * 4, TK_U32, 1,
      "0x1C042 pick: move id 0x00..0x8E -> cell record address (143 longs, 0x12614..0x1284F)" },
    /* the move-cell span 0x12850..0x1AFD4 holds the behind-grab subsystem's
     * three byte tables inside the 0x52/0x53 victim handlers (behind-grab.md)
     * — carved out like ws_walk_speed, the rest stays three record blobs */
    { "move_cell_records",  "base/anim", 0x12850u, 0x18DFAu - 0x12850u, TK_U16, 16,
      "move cell records + handler code, 0x12850 up to behind_grab_class 0x18DFA; includes the 0x17624 run-out escape-seed words at 0x176BA (5 words, index capped at 5 reads into code — ROM bug)" },
    { "behind_grab_class",  "wrestler",  0x18DFAu, 12, TK_U8, 1,
      "per-wrestler size class 0..4 for the behind-grab x offsets (read by 0x18D7E move 0x52, 0x18EBE move 0x53, 0x1599E move 0x1D)" },
    { "behind_grab_off_hold", "base/anim", 0x18E06u, 6, TK_S8, 1,
      "0x18D88 (move 0x52, held from behind by 0x1C): x offset from the holder by behind_grab_class, negated when facing right; 5 classes + 0xFF pad" },
    { "move_cell_records_b", "base/anim", 0x18E0Cu, 0x18F08u - 0x18E0Cu, TK_U16, 16,
      "move cell records + handler code between behind_grab_off_hold and behind_grab_off_turn" },
    { "behind_grab_off_turn", "base/anim", 0x18F08u, 6, TK_S8, 1,
      "0x18EC8 (move 0x53, held from behind by 0x1D) and 0x159AC (0x1D's turn at FE): x offset from the holder by behind_grab_class, negated when facing right; 5 classes + 0xFF pad" },
    { "move_cell_records_c", "base/anim", 0x18F0Eu, 0x1AFD4u - 0x18F0Eu, TK_U16, 16,
      "move cell records + handler code from behind_grab_off_turn up to the victim table 0x1AFD4" },
    { "victim_cell_table",  "base/anim", 0x1AFD4u, 43 * 4, TK_U32, 1,
      "0x1C056 pick: reaction id 0x00..0x2A -> cell record address (43 longs, 0x1AFD4..0x1B07F)" },
    { "victim_cell_records","base/anim", 0x1B080u, 0x1C03Eu - 0x1B080u, TK_U16, 16,
      "victim (reaction) cell records + handler code, 0x1B080 up to the anim core 0x1C03E" },
    { "throw_roll_rows",    "base/anim", 0x113CDu, 12 * 5, TK_U8, 5,
      "0x1129E entry 1 (0x11302) throw-roll thresholds: rows 0..9 = stage vs a CPU partner (0x113CD + stage*5, 0x11322), row 10 = vs a human (0x113FF), row 11 = rumble (0x11404); column = attempt count capped at 4; fail when rng&0xF > byte (0x11362)" },
    { "mash_seed_rows",     "base/anim", 0x10CEAu, 0x10D00u - 0x10CEAu, TK_U8, 3,
      "0x10C60 mash seed, mode >= 2: byte at (mode-1)*3 + band (0x10C98), stored minus 1; row 0 unused by the mode path; last byte 0xFF pads to 0x10D00" },
    { "mash_seed_quarters", "base/anim", 0x10D00u, 4, TK_U8, 4,
      "0x10C60 mash seed, mode 1: seed by hp quarter band (0x10CD2): 01 03 07 15" },
    { "knockback_launch",   "base/anim", 0x25CAu, 0x26AEu - 0x25CAu, TK_S16, 3,
      "0x258E launcher: class*6 -> {vx, vz, grav} (vx negated when facing left); 38 rows up to the 0x26AE homing routine" },
    { "homing_land_offsets","base/anim", 0x275Cu, 0x2790u - 0x275Cu, TK_S16, 2,
      "0x26AE homing launch: index*4 -> {dx, dy} landing offset from the partner, dx mirrored by his facing; 13 rows up to 0x2790" },
    { "fx_spawn_rows",      "base/anim", 0x10DDAu, 0x10E6Au - 0x10DDAu, TK_S16, 3,
      "0x10D3A effect spawn: index*6 -> {y/z offset, sprite (or'd with facing), list}; 24 rows up to 0x10E6A" },
    { "prox_boxes",         "base/anim", 0xE9BAu, 0xEA42u - 0xE9BAu, TK_S16, 4,
      "0xE958 proximity test: box*8 -> {x0, x1, y0, y1} words, x pair mirrored by the opponent's facing; 17 boxes up to 0xEA42" },
};
TBL_REGISTER(anim_tables)

/* Engine scalars of the standing holds as a synthetic table (group rules):
 * rows 0-4 are ROM immediates (hold-timeout.md); the escape-roll rows that
 * the old hold_rules.json carried after them are the real ROM tables
 * hold_escape_roll_cpu / hold_escape_roll_human (core.c) now. */
static const uint8_t hold_rules_be[] = {
    TBL_BE16(0xE0),     /* 0: 0x12526 tie-up state 0x0C hold clock seed (+0x46) */
    TBL_BE16(0xA0),     /* 1: 0x12550 cat-9 window opens below this */
    TBL_BE16(0x80),     /* 2: 0x1A564 move 0x7B auto-reverse clock, human holder */
    TBL_BE16(0xBB),     /* 3: 0x1A572 same, CPU holder */
    TBL_BE16(0x50),     /* 4: 0x1A60C moves 0x7D/0x7E/0x7F clock */
};
static const char *const hold_rule_labels[] = {
    "tieup_hold_seed", "tieup_cat9_window", "autoreverse_clock_human", "autoreverse_clock_cpu", "hold_7d7e7f_clock", NULL };
static const tbl_def hold_rule_tables[] = {
    { "hold_rules", "rules", TBL_SYNTH, sizeof hold_rules_be, TK_U16, 1,
      "engine scalars (ROM immediates) of the standing holds: 0x12526 seed, 0x12550 window, 0x1A564/0x1A572 auto-reverse clocks, 0x1A60C", hold_rules_be, hold_rule_labels },
};
TBL_REGISTER(hold_rule_tables)

/* ws_walk_speed 0x116AE / ws_run_speed 0x11D4A by wrestler id (12 rows). */
static uint16_t walk_speed_of(const eng_obj *o)
{
    uint16_t v = eng_mod_speed(o, o->wrestler < ENG_WS_EXT_MAX
                            ? tbl8(TBL(ws_walk_speed), (uint32_t)eng_ws_base(o->wrestler)) : 0x1Bu);
    if (o->cpu && (o->role & RF_ONFIRE)) v += 6;               /* 0x11770-0x11780: a CPU ON FIRE walks +6 */
    return v;
}
static uint16_t run_speed_of(const eng_obj *o)
{
    return eng_mod_speed(o, o->wrestler < ENG_WS_EXT_MAX
                            ? tbl8(TBL(ws_run_speed), (uint32_t)eng_ws_base(o->wrestler)) : 0x20u);
}

static uint16_t rom16(uint32_t a) { return (uint16_t)tbl_ra16(a); }   /* data layer (tbl.h), not wf.rom */
static uint32_t rom32(uint32_t a) { return ((uint32_t)rom16(a) << 16) | rom16(a + 2); }

static void add_pos_delta(eng_obj *o, int dx, int dy, int dz);
static void edge_arc(eng_obj *o, int16_t d0);
static void carry_at(eng_obj *o, eng_obj *v, int dx, int dy, int dz);
static void knockback(eng_obj *o, unsigned i);
static int self_idx(const eng_obj *o);
static int throw_roll(eng_obj *o, unsigned ctr_off);
static int walk_to(eng_obj *o, int32_t tx, int32_t ty);
static int ko_check(eng_obj *o, eng_obj *v);
static int32_t ring_xmin_of(const eng_obj *o) { return (((o->y >> 16) << 8) + 0x40000) / 0x2E0; }
static int32_t ring_xmax_of(const eng_obj *o) { return -((((o->y >> 16) << 8) - 0xA3000) / 0x2E0); }
static int exit_test(eng_obj *o, int32_t kr, int32_t kl);
static int land_outside(eng_obj *o);
void eng_throw_arc(eng_obj *v);

#define CELL_HANDLER(c) rom32(c)
#define CELL_MODE(c)    rom16((c) + 4)
#define CELL_N(c)       rom16((c) + 6)
#define CELL_DUR(c, f)  rom16((c) + 8 + 2u * (f))
#define CELL_SPR(c, f)  rom16((c) + 8 + 2u * CELL_N(c) + 2u * (f))

/* 0xF4E0-0xF4F0: any state write with bit15 clear restarts the sequence. */
void eng_anim_latch(eng_obj *o)
{
    if (!(o->state & 0x8000u)) {
        o->prev_sel = o->anim_sel;
        o->anim_sel = o->state;         /* bit15 clear => re-init */
        o->state |= 0x8000u;
    }
}

static void anim_load(eng_obj *o, uint32_t c)          /* 0x1C12C */
{
    o->count = CELL_DUR(c, o->frame);
    if (o->count != 0xFF00u) {         /* MOD turbo: every frame of a wrestler's cells is shorter
                                          (the FF00 hold sentinel excepted); the clocks, counts and
                                          the referee keep real time (user 2026-09-05) */
        unsigned pct = eng_mod_turbo_pct();
        if (pct != 100u) o->count = (uint16_t)((unsigned)o->count * 100u / pct);
    }
    o->spr = (uint16_t)(CELL_SPR(c, o->frame) ^ o->facing);  /* full xor */
}

static void anim_seq_tick(eng_obj *o, uint32_t c)      /* 0x1C0C8 */
{
    if (CELL_MODE(c) == 0)
        return;                                        /* handler-driven */
    if (!(o->anim_sel & 0x8000u)) {
        o->frame = 0;
        anim_load(o, c);
        o->anim_sel |= 0x8000u;
        return;
    }
    if (o->count == 0xFF00u)
        return;                                        /* hold forever */
    if (o->count-- != 0)
        return;                                        /* advance on 0->borrow */
    if (++o->frame < CELL_N(c)) {
        anim_load(o, c);
        return;
    }
    o->frame = 0xFE;                                   /* finished sentinel */
    if (CELL_MODE(c) == 2) {                           /* loop */
        o->frame = 0;
        anim_load(o, c);
    } else {
        o->count = 0;                                  /* hold-last */
    }
}

/* --- transcribed cell handlers, keyed by the ROM PC in the record --- */

/* 0x114B2 — stand (state 0, mode-0 cell 0x114AC). First frame kills the
 * mover; every frame sprite = frame 0 + facing. (Weapon/input branches
 * arrive with those systems.) */
static void hit_count(eng_obj *v, int n);   /* +0xC7 big-hit counter (below) */
static uint32_t handler_stand(eng_obj *o, uint32_t cell)
{
    eng_lazy_divorce(o);               /* 0x11560 */
    o->clip_h = 0;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->count = 0;
        o->frame = 0;
        o->anim_sel |= 0x8000u;
        /* 0x114BC-0x114E0 stand init: +0x01 = 0, +0x33 b3/b4/b6 clear (b6 =
         * the "engaged" flag — a stale one made a lone standing man
         * untargetable in the rumble 0x20C02 filter), +0x32 b2 clear,
         * +0x12/+0xAA/+0x44 cleared */
        o->role &= (uint16_t)~0x58u;
        o->st_flags &= (uint16_t)~0x04u;
        o->mash_aa = 0;
        o->off_x = 0; o->off_y = 0;    /* 0x114F0/0x114F4 clr.w (+0x18),(+0x1A): the sprite
                                          offset a move set (plex pin +0x60, tackle +0x50...)
                                          dies here when the move was cut short (a pin BREAK,
                                          a hit) and never reached its own clear-after-pass -
                                          the man drew 80-96 px above his feet until something
                                          else reset it (user 2026-08-26: "landed at the top of
                                          the ring", "the ring limits moved... then returned") */
    }
    if (o->slowwalk_t == 0) o->slowwalk_t = 0xD0;   /* 0x11524 tired clock */
    if (o->st_flags & SF_ELIMINATED) {               /* 0x1152A: eliminated (rumble) -> 0x7A phase 0:
                                           turn, walk to the ropes, climb out, leave */
        o->state = ST_MOVE; o->move_id = 0x7A; o->grap44 = 0;
        return cell;
    }
    if (o->tag_flags & TF_WHIPPED) {               /* 0x1153E: the Irish-whip "whipped" mark dies
                                           at the stand; the whipper's b7 with it and
                                           $1C1697 = 1 (the ref's "back inside" cue) */
        o->tag_flags &= (uint16_t)~0x40u;
        if (o->opp >= 0 && cur_st) cur_st->obj[o->opp].tag_flags &= (uint16_t)~0x80u;
    }
    o->role &= (uint16_t)~0x10u;         /* not running */
    if (o->result & 0x8000u) {          /* 0x11598: the result is final (+0xFE
                                           high-byte b7 = word b15) */
        o->tag_flags &= (uint16_t)~0x10u;
        if (o->result & 1u) { o->state = ST_MOVE; o->move_id = 0x8C; }        /* loser pose */
        else if (!(o->result & 2u)) { o->state = ST_MOVE; o->move_id = 0x8B; o->grap44 = 0; }
        return cell;                    /* winner: walk to centre, then pose */
    }
    o->spr = o->facing;                 /* move.w (+0x2E),(+0x04) @0x11564 */
    o->atk = 1;                         /* standing stance record, 0x1155A */
    if (o->weapon_w & WPN_HELD) {             /* 0x1156A stand holding a weapon */
        o->atk = 0xE;                   /* 0x11572 weapon stance record */
        o->off_y = (uint16_t)((o->off_y & 0xFF00u) | 0x10u);   /* 0x11578 move.b #$10,(+0x1B): the
                                          carry poses are drawn with a +16 px hotspot - their
                                          art sits 16 px low (feet below the origin) and this
                                          lifts it back; without it the carrier drew 16 px low
                                          for the whole carry ("can't get as close to the ring
                                          with a weapon", user vs MAME 2026-08-29) */
        o->spr = (uint16_t)(((o->weapon_w & 1u) ? 0x82u : 0x7Eu)
                            | (o->facing & 0x8000u));   /* 0x1157E-0x11594 carry pose */
    }
    return cell;
}

/* 0x11B6C (walk sub 0x0C) + 0x1AD24 — the WINNER walks to ring centre
 * (x 0x254 legal / 0x29C partner, y 0x160), faces his partner, then both
 * pose (move 0x8B: cells 0x216/0x217/0x216, 8 ticks each, hold-last; YM
 * 0x30 at the first tick). Phase on grap44: 0 walk, 1 pose. Singles: the
 * lone winner poses on arrival. */
static uint32_t handler_winpose(eng_obj *o, uint32_t cell)
{
    if (o->grap44 == 0) {
        int32_t tx = (o->role & RF_LEGAL) ? 0x254 : 0x29C;
        if (!(o->role & RF_LEGAL) && cur_st && (o - cur_st->obj) >= 4)
            tx = 0x2C8;                /* the THIRD winner (survivor trios,
                                          slots 4/5) takes his own spot —
                                          two men shared 0x29C */
        if (!(o->anim_sel & 0x8000u)) {
            o->mover = 1; o->speed = walk_speed_of(o);
            if (eng_dbgsel && cur_st)
                fprintf(stderr, "win: o%d walks to the high-five spot %03X\n",
                        (int)(o - cur_st->obj), (unsigned)tx);
        }
        if (walk_to(o, tx, 0x160)) {
            o->mover = 0; o->x = tx << 16; o->y = 0x160 << 16;
            /* face the TEAMMATE at the other spot — the pair high-five
             * EACH OTHER (was facing the opponent link: both looked the
             * same way, playtest 2026-08-24). Legal spot 0x254 is left
             * of the partner spot 0x29C. */
            o->facing = (uint16_t)((o->role & RF_LEGAL) ? 0x8000u : 0);
            o->grap44 = 2;                             /* arrived (0x11BDC +0xAE b7) */
        }
        return 0x115F2u;                               /* walk cell */
    }
    if (o->grap44 == 2) {
        /* 0x11C22: the pair pose TOGETHER — the man at his spot stands
         * still until his winning partner has arrived too (playtest
         * 2026-08-26: one posed alone while the other was still walking) */
        eng_obj *p = (cur_st && o->teammate >= 0 && o->teammate < ENG_MAX_OBJS)
                     ? &cur_st->obj[o->teammate] : 0;
        int p_wins = p && p->active && !(p->st_flags & SF_ELIMINATED) && p->result
                     && !(p->result & 1u);
        o->spr_force = ENG_SPR_STANCE;
        if (p_wins && !((p->state & 0xFFu) == ST_MOVE && p->move_id == 0x8B && p->grap44 != 0))
            return 0x115F2u;                           /* partner still walking / climbing in */
        o->grap44 = 1; o->state = ST_MOVE;                   /* re-init onto the pose */
        if (p_wins && p->grap44 == 2) { p->grap44 = 1; p->state = ST_MOVE; }   /* 0x11C3E */
        return 0x115F2u;
    }
    if (!(o->anim_sel & 0x8000u)) o->mover = 0;
    if (o->frame == 0 && o->count == 0) eng_sound(0x30);   /* 0x1AD3E */
    if ((o->frame & 0xFFu) == 0xFEu && cur_st) {           /* 0x1AD48: the arms are up */
        eng_obj *p = (o->teammate >= 0 && o->teammate < ENG_MAX_OBJS)
                     ? &cur_st->obj[o->teammate] : 0;
        eng_obj *q = (o->opp >= 0 && o->opp < ENG_MAX_OBJS) ? &cur_st->obj[o->opp] : 0;
        eng_obj *r = (q && q->teammate >= 0 && q->teammate < ENG_MAX_OBJS)
                     ? &cur_st->obj[q->teammate] : 0;
        o->a0flags = 0x40;                                 /* 0x1AD50 */
        if (p) p->a0flags = 0x40;                          /* 0x1AD5A +0x86 */
        if (q) q->a0flags = 0x80;                          /* 0x1AD64 +0x7A */
        if (r) r->a0flags = 0x80;                          /* 0x1AD6E +0x7A->+0x86 */
    }
    return cell;
}

/* 0x1AD7C — move 0x8C LOSER pose (mode-0 cell): spr 0x1D3|facing, a 0x40
 * countdown, then frozen. */
static uint32_t handler_losepose(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; o->count = 0x40; }
    o->spr_force = (uint16_t)(0x01D3 | (o->facing & 0x8000u));
    /* 0x1AD96: subq.w #1,(+0x22); bcc rts — the 0x40 -> -1 borrow fires once.
     * The cell record is handler-driven (mode 0), so nothing else ticks it. */
    if (o->count-- != 0) return cell;   /* 0x40 -> 0 fires once; 0xFFFF after */
    if (cur_st) {
        eng_obj *p = (o->teammate >= 0 && o->teammate < ENG_MAX_OBJS)
                     ? &cur_st->obj[o->teammate] : 0;
        eng_obj *q = (o->opp >= 0 && o->opp < ENG_MAX_OBJS) ? &cur_st->obj[o->opp] : 0;
        eng_obj *r = (q && q->teammate >= 0 && q->teammate < ENG_MAX_OBJS)
                     ? &cur_st->obj[q->teammate] : 0;
        unsigned lo = o->result & 0xFFu;
        if (lo == 5) {                                 /* 0x1ADD4 draw / time-up: all four out */
            o->a0flags = 0x80;
            if (p) p->a0flags = 0x80;
            if (q) q->a0flags = 0x80;
            if (r) r->a0flags = 0x80;
        } else if (lo == 3) {                          /* 0x1ADAE count-out loser */
            o->a0flags = 0x80;
            if (p) p->a0flags = 0x80;
            if (q) q->a0flags = 0x40;
            if (r) r->a0flags = 0x40;
        }
        /* lo == 1 (pin / KO loser): nothing — the winner's pose sets both
         * sides (0x1ADAC -> rts). */
    }
    return cell;
}


/* 0x12474 — collar-and-elbow lockup (state 0x0B, record 0x12450: loop
 * cells 0x39,3A,39,3B, delay 0x10; the 0x8000-half is hidden — culled at
 * sprite emit). Mash: countdown +0xBD seeded 0x20 (0x1EFAA); on expiry the
 * ROM does a 50/50 rng pick — TODO EXACT (needs the 0x21B4 RNG); the
 * frame-parity rule from 0xF70C stands in, and the winner takes the hold
 * (the 0xF760 write shape) instead of the loser staggering (state 5 anim
 * 0x29, not yet transcribed). */
static uint32_t handler_lockup(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mash = 0x11;                 /* impact record 0x12468 length */
        eng_slide_clip(o, 0x18);        /* 0x12474 entry: X nudge +0x18 via
                                           0x10B62 — off the rope line */
        o->role |= RF_ENGAGED;
    }
    else if (o->grap44 & 0x2000u) {    /* +0x44 b13: the impact cell 0x1D2
                                          shows first (one-shot 0x12468) */
        o->spr_force = (uint16_t)(0x01D2 | (o->facing & 0x8000u));
        if (o->mash && --o->mash == 0)
            o->grap44 &= (uint16_t)~0x2000u;   /* 0x124DA -> cat 8 opens */
    }
    /* 0x1EFAA — the CPU lockup driver (2026-08-27; humans win the exchange
     * by pressing, cat 8 in core.c): a countdown at +0xBD (ai_bc low byte)
     * seeded 0x20 + rng&3 (the 0x1F060 stage row 7/7/6/6/3/5/5/4/4/3 when
     * the partner is neither human nor CPU); on expiry, reload and a d100
     * under the 0x1F05E {50,50} row — bucket 0 fires the KNEE (move 0x29)
     * and marks the partner's +0x44 b6 so he skips his own roll. Without
     * this, a CPU-vs-CPU lockup sat the full 256-frame mash window. */
    if (o->cpu) {
        static const uint8_t stage_bd[10] = { 7, 7, 6, 6, 3, 5, 5, 4, 4, 3 };
        eng_obj *p = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
        int aloof = p && !(p->driver & DRV_AUTOPILOT) && !p->cpu;  /* 0x1EFC6/0x1EFCE partner test */
        unsigned stage = cur_st && cur_st->stage < 10 ? (unsigned)cur_st->stage : 9u;
        if (!(o->anim_sel & 0x8000u)) {                 /* 0x1EFB2 init */
            o->ai_b6 = 0; o->ai_b7 = 0;                 /* clr.w (+0xB6) */
            o->ai_idle = 0;                             /* bclr #1,(+0xB9) */
            o->ai_bd = (uint8_t)((aloof ? stage_bd[stage] : 0x20u) + (eng_rng() & 3u));
        }
        if (o->ai_bd && !--o->ai_bd) {
            o->ai_bd = (uint8_t)((aloof ? stage_bd[stage] : 0x20u) + (eng_rng() & 3u));   /* 0x1EFEC reload */
            if (!(o->grap44 & 0x40u)                    /* 0x1F016: partner already kneeing */
                && ((eng_rng() & 0xFFu) >> 1) < 50u) {  /* 0x24CC under 0x1F05E {50,50} */
                o->state = ST_MOVE; o->move_id = 0x29;        /* 0x1F030 the knee */
                if (p) p->grap44 |= 0x40u;              /* 0x1F040 bset #6,(+0x44,A1) */
            }
        }
    }
    return cell;
}

/* 0x17B12 — move 0x38 spring-up (escape-machine.md §4): mover 1, angle
 * 0x80, speed 0x18 (angle flipped when the scroll x $1C17EE < 0x240 —
 * TODO EXACT); 3 frames x 6 ticks; end -> state 7, react 0. */
static uint32_t handler_springup(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 1; o->angle = 0x80; o->speed = 0x18;
        if (cur_st && cur_st->cam_x < 0x240) o->angle = 0x00;
        o->down_t = 0;
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_GETUP; o->react_id = 0; o->mover = 0; }
    return cell;
}

/* 0x1A192 — move 0x78 roll-away (lying-getup.md §7): single sprite
 * 0x1D8, 17 ticks, no motion; end -> state 7, react 0. */
static uint32_t handler_rollaway(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; o->down_t = 0; return cell; }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_GETUP; o->react_id = 0; o->partner = -1; }
    return cell;
}

/* 0x1878A — move 0x4C TAG: snap to the corner spot (0x218,0x193)/
 * (0x2F6,0x121), partner receives (0x4D); at frame 1 step -0x20 back;
 * at the end SWAP, the outgoing man walks out (recall) from x = partner.x.
 * TODO EXACT: the double-team window 0x1F760/0x214C0. */
static uint32_t handler_tag(eng_obj *o, uint32_t cell)
{
    eng_obj *P = (o->teammate >= 0 && cur_st) ? &cur_st->obj[o->teammate] : 0;
    int side = eng_side(o);
    if (!P) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->x = (side ? 0x2F6 : 0x218) << 16; o->y = (side ? 0x121 : 0x193) << 16; o->z = 0x140 << 16;
        o->facing = (uint16_t)(side ? 0x8000u : 0);        /* face the corner */
        P->state = ST_MOVE; P->move_id = 0x4D; P->apron = 0; P->grap44 = 0;
        eng_announce(cur_st, 0x0F, 0x18);                  /* 0x1885C "tags the corner" */
        return cell;
    }
    if ((o->frame & 0xFFu) == 1 && o->count == 0) {
        if (cur_st && eng_tag_dt_window(cur_st, o)) {      /* 0x187F2 jsr 0x1F760 */
            /* the opponent lies knocked down in this corner: the pair
             * SWAPS now (0x188BC + 0x21978) but the tagger stays INSIDE
             * (0x18808 bclr b0) holding the downed man for his partner —
             * move 0x37 via 0x214C0; the held man's mash counter and down
             * timer are pushed out to 0x1000 (0x18812/0x18818) */
            int vlink = o->opp;
            eng_tag_swap(cur_st, o, P);
            o->tag_flags |= (uint16_t)(P->tag_flags & TF_USHER_BITS); P->tag_flags &= (uint16_t)~TF_USHER_BITS;   /* 0x21978 */
            o->ai_e6 = P->ai_e6; P->ai_e6 = 0;
            o->apron = 0; o->sub = 0; o->st_flags &= (uint16_t)~0x01u;
            o->opp = vlink;
            if (vlink >= 0) {
                cur_st->obj[vlink].down_t = 0x1000;
                cur_st->obj[vlink].mash_aa = 0x1000;
            }
            if (eng_tag_dt_start(cur_st, o)) {             /* 0x1881E: not the dive */
                P->state = ST_WALK; P->sub = 2; P->apron = 1;    /* 0x1882C apron wait: 0x37's
                                                              init calls him in (0x4E) */
                P->st_flags |= SF_APRON;
            }
            if (eng_dbgsel)
                fprintf(stderr, "tag: P%d holds the downed o%d for the partner (0x37)\n",
                        self_idx(o) + 1, vlink);
            return cell;
        } else if (eng_dbgsel && cur_st) {             /* diagnostics: why the 0x1F760 window is shut */
            const eng_obj *v = o->opp >= 0 ? &cur_st->obj[o->opp] : NULL;
            if (!v) fprintf(stderr, "tag: P%d dt window shut: no opp link\n", self_idx(o) + 1);
            else fprintf(stderr, "tag: P%d dt window shut: opp o%d st=%02X react=%02X x=%X y=%X facing=%d side=%d down_t=%d\n",
                         self_idx(o) + 1, o->opp, v->state & 0xFF, v->react_id & 0xFF, (unsigned)(v->x >> 16), (unsigned)(v->y >> 16),
                         (v->facing >> 15) & 1, (o->role & RF_SIDE) ? 1 : 0, (int)v->down_t);
        }
        add_pos_delta(o, -0x20, 0, 0);                     /* 0x18840 */
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        eng_tag_swap(cur_st, o, P);                        /* 0x188BC + 0x18864:
                                          the outgoing man lands on the apron
                                          at the teammate's spot */
        o->state = ST_WALK; o->facing ^= 0x8000u; o->spr = o->facing;
        o->x = P->x;                                       /* x = teammate.x */
        o->partner = -1;
        P->count = 0;                                      /* release 0x4D's FF00 hold */
    }
    return cell;
}

/* 0x18AA0 — move 0x4E ENTER over the ropes (cell 0x18A90: 2 x 0x10 ticks,
 * sprites 0x66/0x67 — the same rope straddle the climb-out plays, run the
 * other way). This is what the rescue run-in fires at 0x1D590, so a man
 * coming off the apron into a pin CLIMBS IN over 32 frames and cannot act
 * until the chain ends; only then does he step inside (x += +/-0x38,
 * 0x18AEA/0x18AF8) and become an in-ring, illegal man. */
static uint32_t handler_enterring(eng_obj *o, uint32_t cell)
{
    int side = eng_side(o);
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;                                      /* 0x18AA8 */
        o->band = o->hp <= 0x18 ? 2                        /* 0x18AAC jsr 0x24EC2 */
                : o->hp <= (uint16_t)(2 * o->hp_max / 3) ? 1 : 0;
        o->list = 2;                                       /* 0x18AB2: +0x12 = 2 */
        o->facing = (uint16_t)(side ? 0 : 0x8000u);        /* face the ring */
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x18ABC */
        o->state = ST_STAND;                                      /* 0x18AC4 */
        o->tag_flags &= (uint16_t)~0x10u;                        /* 0x18AD0 */
        o->sub = 0;                                        /* 0x18AD6 */
        o->spr = o->facing;                                /* 0x18ADA */
        o->list = 0;                                       /* 0x18AE0 */
        o->apron = 0;                                      /* 0x18AE4: f32 b0 clear */
        o->st_flags &= (uint16_t)~0x01u;                        /* (same bit — the human apron
                                                              paths mirror it, 0x1179E) */
        o->role &= (uint16_t)~0x04u;                        /* engine guard: see 0x4D ender */
        o->x += (int32_t)(side ? -eng_tag_rule(TAG_ENTER_STEP_X)
                               :  eng_tag_rule(TAG_ENTER_STEP_X)) << 16;
        o->ai_sub = 1;                                     /* run-in AI 0x1D398 */
        o->ai_b7 = 0;
    }
    return cell;
}

/* 0x18B0E — move 0x4F climb OUT over the ropes (cells 66/67): at the end
 * the man lands on the apron: facing flips, x steps over the rope, the
 * apron sub takes over. */
static uint32_t handler_climbout(eng_obj *o, uint32_t cell)
{
    int side = eng_side(o);
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->list = 2;
        o->facing = (uint16_t)(side ? 0x8000u : 0);        /* 0x18B20: (+0x33 b7) << 8 —
                                          face the rope he climbs */
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->facing ^= 0x8000u;                              /* 0x18B4A */
        o->apron = 1; o->sub = 1; o->regen_t = 0x94;
        o->tag_flags &= (uint16_t)~0x02u;
        o->list = 0;
        o->x = ((side ? ring_xmax_of(o) : ring_xmin_of(o)) + (side ? 0x38 : -0x38)) << 16;
        o->z = 0x140 << 16;
        o->state = ST_WALK;                                      /* apron sub 1 */
    }
    return cell;
}

/* 0x196AE — move 0x66: TAG while holding the stance 0x15/0x16 at the own
 * corner (0xDD8A-0xDD9E): pair teleports to the buckle spot (0x1976E),
 * victim frozen at 0xFF; legality swaps AT INIT (jsr 0x188BC — but the
 * outgoing holder STAYS INSIDE holding him: 0x19728 undoes the outside
 * mark), the usher bookkeeping moves onto the holder (jsr 0x21978:
 * f34&7 + ai_e6 transfer; the $1C1684/$1C1688 slot registration is the
 * engine's usher_t arm, TODO EXACT), and the partner receives with move
 * 0x4D + autopilot b6 (0x19716/0x19722) targeting the HELD man
 * (0x1972E) — his 0x4D branches into the state-8 corner-climb rows 4/7
 * and the perch dive: the double team. Cells 0x1969E: 2 x 0x10, sprs
 * 0x4B/0x4C, mode 1 hold-last. */
static uint32_t handler_holdtag(eng_obj *o, uint32_t cell)
{
    static const int16_t spot[2][2] = { {0x218,0x193}, {0x2F6,0x121} };   /* 0x1976E */
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    eng_obj *P = (o->teammate >= 0 && cur_st) ? &cur_st->obj[o->teammate] : 0;

    if (!(o->anim_sel & 0x8000u)) {                    /* 0x196B8 init */
        int side = eng_side(o);
        int vlink = o->partner;
        o->mover = 0;
        o->x = spot[side][0] << 16; o->y = spot[side][1] << 16;   /* 0x196CC */
        o->facing = (uint16_t)(side ? 0 : 0x8000u);    /* 0x196E4 face the ring */
        if (v) {
            v->x = o->x; v->y = o->y;                  /* 0x196D8 */
            v->facing = o->facing;
            v->state = ST_HELD;                           /* 0x196FA frozen in the hold */
        }
        if (P) {
            P->tag_flags |= TF_BIT3;                           /* 0x19704 double-team receive flag */
            eng_tag_swap(cur_st, o, P);                /* 0x188BC (legal/pad/cpu swap) */
            o->partner = vlink;                        /* 0x188BC never touches +0x26 —
                                                          the holder KEEPS the held man */
            o->apron = 0;                              /* 0x19728 bclr #0,+0x32: stays inside */
            o->st_flags &= (uint16_t)~0x01u;
            o->tag_flags |= (uint16_t)(P->tag_flags & TF_USHER_BITS);      /* 0x21978 usher bits -> holder */
            P->tag_flags &= (uint16_t)~TF_USHER_BITS;
            o->ai_e6 = P->ai_e6; P->ai_e6 = 0;
            cur_st->usher_t = (uint16_t)eng_tag_rule(TAG_USHER_ARM);
            P->state = ST_MOVE; P->move_id = 0x4D;           /* 0x19716 receive */
            P->driver |= DRV_AUTOPILOT;                           /* 0x19722 autopilot: the walker drives
                                                          him (legal b6 men route to the AI,
                                                          core.c) until a tie-up releases him */
            P->opp = vlink;                            /* 0x1972E target the held man */
        }
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu)
        o->move_id |= 0x8000u;                         /* 0x1974A latch, hold the pose */
    if (v && (v->state & 0xFFu) != 0xFFu) {
        /* the double-team landed (or the hold broke): the holder stands
         * back up. Stock leaves him frozen for the usher escort / the
         * retaliation hit to break — engine safety so a missing run-in
         * can never soft-lock him (TODO EXACT). */
        o->state = ST_STAND; o->partner = -1; o->divorce = 1;
    }
    return cell;
}

/* 0x18948 — move 0x4D RECEIVE: apron spot (0x1C8,0x192)/(0x34C,0x120);
 * at the end stands inside at the corner spot, legal. */
static uint32_t handler_tagrecv(eng_obj *o, uint32_t cell)
{
    int side = eng_side(o);
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->x = (side ? 0x34C : 0x1C8) << 16; o->y = (side ? 0x120 : 0x192) << 16; o->z = 0x140 << 16;
        o->facing = (uint16_t)(side ? 0 : 0x8000u);       /* face the ring */
        o->list = 2;                                       /* on the ropes */
        return cell;
    }
    if (o->frame == 0 && o->count == 0) eng_sound(0x30);
    if ((o->tag_flags & TF_BIT3) && (o->frame & 0xFFu) == 1) {
        /* 0x18994-0x189EA double-team receive (f34 b3 from move 0x66):
         * at frame 1 he leaves the anim into the state-8 corner-climb
         * rows 4 (side 0) / 7 (side 1) — list 4, sub 0, facing flipped.
         * b3 STAYS SET (the topdive reads it at 0x153E2 for mode 0, and
         * its landing clears it at 0x154B6); the state change itself is
         * the one-shot — this handler only runs in state 5 (stock's
         * +0x22 = 2 marker, TODO EXACT). */
        o->list = 4; o->sub = 0;
        o->facing ^= 0x8000u;                          /* 0x189C0 */
        o->state = ST_CLIMB; o->grap44 = (uint16_t)(side ? 7 : 4);   /* 0x189CE/0x189DE */
        /* Seed the corner-mount spot NOW: the state-8 anim arms next
         * tick, so without this the transition frame rendered at the
         * apron receive spot — "an out-of-position flash" (playtest
         * 2026-08-24). Values = the 0x12078 init (post table 0x121C8
         * corner 0/3, x -/+ 0x18, z 0x180). */
        if (side) { o->x = (0x338 + 0x18) << 16; o->y = 0x120 << 16; }
        else      { o->x = (0x1DC - 0x18) << 16; o->y = 0x193 << 16; }
        o->z = 0x180 << 16;
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->x = (side ? 0x2F6 : 0x218) << 16; o->y = (side ? 0x121 : 0x193) << 16;
        o->state = ST_STAND; o->spr = o->facing; o->apron = 0; o->list = 0;
        o->st_flags &= (uint16_t)~0x01u;
        o->role &= (uint16_t)~0x04u;    /* engine guard: a stuck OUTSIDE bit on the
                                          entering man flips him onto the ringside
                                          motion law (z floor 0x100 — "fell below
                                          the ring") and blocks the b2-equality
                                          tie-up gate (0xF5A2) — playtest 2026-08-24 */
    }
    return cell;
}

/* 0x16582 — move 0x29, the tie-up KNEE (rng-lockup.md §2c): the winner
 * of an exchange knees the partner (frozen to 0xFF inside the two-man
 * cells F5-F7); at anim end the partner takes 1 and his +0x52 counts the
 * shoves — <2 rematch (both back to 0x0B), >=2 the winner takes the hold
 * (0x0C, +0x44 = 0x2000 impact first; partner stays 0xFF). Never unlinks. */
static uint32_t handler_knee(eng_obj *o, uint32_t cell)
{
    eng_obj *p = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!p) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;                                  /* 0x1658A */
        o->grap44 &= 0x7FFFu;                          /* drawn half */
        p->grap44 |= 0x8000u;                          /* hidden half */
        p->state = ST_HELD;                             /* 0x1659A frozen */
        p->x = o->x; p->y = o->y;
        return cell;
    }
    if ((o->frame & 0xFFu) == 1 && o->count == 0)
        eng_sound(0x2A);                               /* 0x165A4 impact */
    if ((o->frame & 0xFFu) != 0xFEu)
        return cell;
    p->dmg = 1;                                        /* 0x165C4 */
    hit_count(p, 1);                                   /* 0x165CA */
    p->combo_t = 0x100;                                /* 0x165D0 */
    p->combo++;                                        /* 0x165D6 */
    if (eng_dbgsel)
        fprintf(stderr, "knee: P%d shoves=%d -> %s\n", self_idx(o) + 1, p->combo,
                p->combo < 2 ? "rematch" : "HOLD");
    if (p->combo < 2) {                                /* 0x165E4 rematch */
        o->state = 0x000B; p->state = 0x000B;
        o->grap44 = 0; p->grap44 = 0x8000u;
    } else {                                           /* 0x165FC advantage */
        o->state = 0x000C; o->grap44 = 0x2000u; o->pinning = 0; o->hold_ph = 0;
        p->state = ST_HELD; p->grap44 = 0;
    }
    return cell;
}

/* 0x124FA — hold won (state 0x0C, record 0x124E2: loop cells 0x39,3A,3F,3A).
 * Entry: +0x46 := 0xE0; at 0xA0 phase byte +0x45 := 1; at 0 the hold flips
 * (0x1254C): self -> 0xFF, partner -> 0x0C. Victim (0xFF) is carried by the
 * holder (0x10B9A copy pos; facing-signed offset TODO). */
static uint32_t handler_hold(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->hold_t = (uint16_t)eng_hold_rule(0, 0xE0);   /* 0x12526 */
        o->hold_ph = 0;
        if (!o->pinning) {             /* 0x124FA entry: X nudges +0x18 then
                                          -0x18 via 0x10B62 — keeps the pair
                                          0x18 px inside either rope */
            eng_slide_clip(o, 0x18);
            eng_slide_clip(o, -0x18);
        }
    }
    if (o->partner >= 0 && cur_st) {
        eng_obj *p = &cur_st->obj[o->partner];
        if (o->pinning) {
            /* Lying pin (entered from a cover, move 0x08): no flip
             * clock; the referee counts on +0x35 bit0; the hidden victim
             * mashes out. TODO EXACT: the 0x0C-as-pin machinery. */
            if (o->result) {                           /* decided: release */
                o->cue_flags &= (uint16_t)~1u;
                o->pinning = 0;
                o->atk = 0;
                o->state = ST_GETUP;
                p->state = ST_REACT; p->react_id = RC_LYING; p->down_t = 0x20;
                eng_tag_pin_end(cur_st, o, p);         /* 0x212A0: pad home */
                return cell;
            }
            o->cue_flags |= 1u;
            o->atk = 0x801Du;              /* 0x15F22: the covering man carries
                                          hit record 0x1D, whose reaction
                                          handler 9 (0x24BC2) is the pin
                                          break — that is what lets a stomp
                                          land on the pile and free it */
            /* Keep the cover's final interaction art through the pin —
             * park the tick on a hold. TODO EXACT: the ROM's 0x0C-as-pin
             * cell substitution. */
            o->count = 0xFF00;
            o->spr_force = 0xFFFF;     /* 0x1342C: the pinner is HIDDEN (after
                                          the tick, so the hold record's own
                                          init cell never shows); the victim
                                          draws the composite body */

            if (p->hp != 0 && p->mash_aa != 0x4000u && (p->btn_new & 3u)) {
                if (p->mash_aa && --p->mash_aa == 0)
                    p->mash_aa = 0x4000u;
            } else if (p->mash_aa == 0x4000u && (p->btn_new & 3u)) {
                if (eng_dbgsel)
                    fprintf(stderr, "pin: KICK-OUT o%d (halfct %u)\n",
                            (int)(p - cur_st->obj), p->halfct);
                o->cue_flags &= (uint16_t)~1u;               /* KICK-OUT 0x1820C */
                o->pinning = 0;
                o->atk = 0;
                o->role &= (uint16_t)~0x40u;
                p->role &= (uint16_t)~0x40u;
                p->mash_aa = 0;
                o->state = ST_REACT; o->react_id = 0x0F;      /* thrown off */
                o->spr = (uint16_t)(0xF0 | (p->facing & 0x8000u));
                o->facing = p->facing; o->down_t = 0x50;
                o->x = p->x + ((p->facing & 0x8000u) ? -(0x50 << 16) : (0x50 << 16));
                o->y = p->y - (1 << 16); o->z = p->z + (0x10 << 16);
                p->halfct = 0;
                p->state = ST_MOVE; p->move_id = 0x4B;       /* kick-out cells */
                o->partner = -1;
                p->partner = -1;
                eng_tag_pin_end(cur_st, o, p);         /* 0x1825E jsr 0x212D4 on
                                          both men: the pad the pinner handed
                                          to his partner comes home */
                eng_ref_digit_wipe();                  /* 0x206FE via SM6->SM1 */
            }
            return cell;
        }
        p->x = o->x;                    /* 0x10B9A carry */
        p->y = o->y;
        if (o->grap44 & 0x2000u) {      /* impact cell first (0x12468,
                                           0x10 ticks): the pause at the
                                           peak before the hold settles */
            o->spr_force = (uint16_t)(0x01D2 | (o->facing & 0x8000u));
            if (o->hold_t <= eng_hold_rule(0, 0xE0) - 0x10)
                o->grap44 &= (uint16_t)~0x2000u;
        }
        if (o->hold_t && --o->hold_t == 0) {
            o->state = ST_HELD;          /* 0x1254C flip */
            o->grap44 = 0;
            p->state = 0x000C;
            p->grap44 = 0x2000;
            p->pinning = 0; p->hold_ph = 0;
        } else if (o->hold_t == eng_hold_rule(1, 0xA0)) {   /* 0x12550 */
            o->hold_ph = 1;
        }
    }
    return cell;
}

/* 0x11B6C — state-1 sub 0xC: the WINNER'S ceremony walk (anim-side sub
 * table 0x1167A entry 0xC; the input-side entry 0xF25A[0xC] -> 0xF33A is
 * an rts — pad dead, core.c). Rumble (0x11BB0): walk to (0x2C0,0x160)
 * and drop to STAND there (0x11C4C) — the 0x8E ceremony object then puts
 * him in the 0x79 pose (rumble.c). Tag/singles (0x11BA0/0x11BA8) walk to
 * (0x254|0x29C,0x160) and pair into move 0x8B — the engine reaches that
 * through handler_winpose instead (TODO EXACT: the 0x11C22 pairing).
 * TODO EXACT 0x11BBE: facing = ~angle b7 each tick; walk_to()'s dx sign
 * stands in. */
static uint32_t walk_subC(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {                    /* 0x11B72 first entry */
        o->tag_flags = 0x04u;                                /* 0x11B74 clr / 0x11B78 bset #2 */
        o->mover = 1;                                  /* 0x11B7E */
        o->tgt_y = 0x160;                              /* 0x11B88 +0xC0 */
        if (cur_st && (cur_st->g161 & 1u))
            o->run_tgt = 0x2C0;                        /* 0x11BB0 rumble +0xBE */
        else
            o->run_tgt = (o->role & RF_LEGAL) ? 0x254 : 0x29C;   /* 0x11BA0/0x11BA8 */
    }
    if (o->wrestler < ENG_WS_EXT_MAX)
        o->speed = walk_speed_of(o);                   /* 0x11B84 jsr 0x1174C */
    if (!(o->sub & 0x80u)) {                           /* 0x11BB6 +0xAE b7 */
        if (!walk_to(o, o->run_tgt, o->tgt_y))         /* 0x11BD2 jsr 0x1F15C */
            return cell;                               /* 0x11BD8 still walking */
        o->sub |= 0x80u;                               /* 0x11BDC arrived */
        o->mover = 0;                                  /* 0x11BE2 */
        o->count = 0xFFFFu;                            /* 0x11BE6 +0x22 hold */
        o->x = (int32_t)o->run_tgt << 16;              /* 0x11BEC snap */
        o->y = (int32_t)o->tgt_y << 16;
    }
    if (cur_st && (cur_st->g161 & 1u)) {               /* 0x11BF8 -> 0x11C4C rumble */
        o->state = ST_STAND;                                  /* stand where he arrived */
        o->spr = 0; o->facing = 0;                     /* 0x11C52/0x11C56 */
        o->role &= (uint16_t)~0x02u;                    /* 0x11C5A bclr #1 */
    }
    return cell;
}

/* 0x11652 -> 0x116C6 — walk (state 1 sub 0). Arms the polar mover, loads
 * the per-wrestler speed each tick, and returns the cell the tick should
 * animate (run/weapon variants substitute here later — 0x11710). */
static uint32_t handler_walk(eng_obj *o, uint32_t cell)
{
    if ((o->sub & 0x7Fu) == 0x0Cu)
        return walk_subC(o, cell);     /* 0x11668 sub table 0x1167A[0xC] -> 0x11B6C */
    eng_lazy_divorce(o);               /* 0x116DE */
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 1;                   /* 0x116CE, once per sequence */
        if (o->tag_flags & TF_WHIP_MARKS) {           /* 0x11AD6-0x11AF6: the Irish-whip marks (b7
                                           whipper / b6 whipped) die when either man
                                           starts walking — and so do the opponent's */
            o->tag_flags &= (uint16_t)~0xC0u;
            if (o->opp >= 0 && cur_st) cur_st->obj[o->opp].tag_flags &= (uint16_t)~0xC0u;
        }
    }
    if (o->wrestler < ENG_WS_EXT_MAX)
        o->speed = walk_speed_of(o);   /* 0x1174C: ws_walk_speed 0x116AE */
    o->speed = eng_mod_turbo_speed((unsigned)eng_pkg_stat((unsigned)o->wrestler, "walk", o->speed));   /* + mod turbo */
    if (o->weapon_w & WPN_HELD)
        o->speed = (uint16_t)(o->speed - 8);   /* 0x11786: -8 carrying a weapon */
    if (o->st_flags & SF_TIRED)               /* tired (+0x32 b3): stomach-hold
                                           walk, flat speed 0x116BA */
        o->speed = 0x0C;
    if (o->apron) o->speed = 0x10;      /* apron follow 0x11796: speed 0x10 */
    o->atk = 1;                         /* walking stance record, 0x116E2 */
    if ((o->sub & 0x7Fu) < 5 && (o->weapon_w & WPN_HELD))
        o->atk = 0xE;                   /* 0x116E8/0x116F0 weapon stance */
    if (o->result && !o->apron) {       /* 0x11702: tst.w (+0xFE) / move.w #0,(+0x20)
                                           — a decided match drops the walker back to
                                           STAND on the spot. Without it the pad is
                                           already dead (0xDCE8) but the polar mover
                                           keeps its last angle/speed, so a man walking
                                           when the 20-count lands slides on to the
                                           barrier and never reaches the lose pose.
                                           The apron follow 0x11796 is a different
                                           sub with no such test — excluded. */
        o->state = ST_STAND;                   /* the 0x114B2 stand init clears the mover */
    }
    if (o->weapon_w & WPN_HELD) {             /* 0x11710: weapon walk cells first —
                                           0x11622 (sprites 0x7C-0x7F) or, type
                                           odd (+0x75 b0), 0x1163A (0x80-0x83) */
        o->off_y = (uint16_t)((o->off_y & 0xFF00u) | 0x10u);   /* 0x11718: same +16 hotspot */
        return (o->weapon_w & 1u) ? 0x1163Au : 0x11622u;
    }
    return (o->st_flags & SF_TIRED) ? 0x1160Au : cell;   /* 0x11710 cell swap */
}

/* 0x1A28E — move 0x79 KING-OF-THE-RUMBLE pose (the 0x8E ceremony object
 * puts the winner here, 0x1AFA4): per-wrestler celebration cell record
 * from the table 0x1A30C[id] (poses 0x59-0x5D), replayed 0x1A33C[id]
 * times (+0x44 low nibble, 0x1A2C8); entry sound 0x32, ids >= 6 get 0x31
 * (0x1A2A6). The last replay ends in STAND with the cell-0 sprite
 * (0x1A2E6-0x1A2F2). */
static uint32_t handler_kingpose(eng_obj *o, uint32_t cell)
{
    unsigned id = (unsigned)eng_ws_base(o->wrestler);   /* clones celebrate as their base */

    if (!(o->anim_sel & 0x8000u)) {
        if (!(o->move_id & 0x8000u)) {                 /* 0x1A296 bset #7,+0x60: once,
                                                          the replays keep the latch */
            o->move_id |= 0x8000u;
            o->mover = 0;                              /* 0x1A29E */
            eng_sound(id >= 6 ? 0x31 : 0x32);          /* 0x1A2A2-0x1A2B2 jsr 0x2052 */
            o->grap44 = (uint16_t)(tbl_ra8(0x1A33Cu + id) & 0x0Fu);   /* 0x1A2B8-0x1A2C8 */
        }
    } else if ((o->frame & 0xFFu) == 0xFEu) {          /* 0x1A2D0 script done */
        if (o->grap44) o->grap44--;                    /* 0x1A2D8 */
        if (o->grap44) {
            o->state = ST_MOVE;                              /* 0x1A2DE replay */
        } else {
            o->state = ST_STAND;                              /* 0x1A2E6 stand */
            o->spr = (uint16_t)(o->facing & 0x8000u);  /* 0x1A2EC cell 0 + flip */
            o->tag_flags &= (uint16_t)~0x10u;                /* 0x1A2F2 bclr #4 */
            return cell;                               /* 0x1A2F8 -> 0x1A30A */
        }
    }
    return rom32(0x1A30Cu + id * 4u);                  /* 0x1A2FA per-wrestler record */
}

/* 0x11C82 — run (state 2, cell 0x11C62: loop, n 6, dur 6, sprites 9..E).
 * Init arms the mover and loads the per-wrestler run speed (0x11D4A).
 * Footstep sound 0x33 on frames 1/4 arrives with audio. */
static uint32_t handler_run(eng_obj *o, uint32_t cell)
{
    eng_lazy_divorce(o);               /* 0x11CC0 */
    if (o->zone == 3) {                 /* 0x11CE0: ran into the barrier */
        o->dmg = 0x0A; eng_sound(0x28);
        o->state = ST_REACT; o->react_id = 0x17; o->grap44 = 0;
        return cell;
    }
    if (!(o->anim_sel & 0x8000u))
        o->st_flags &= (uint16_t)~0x0800u;   /* 0x117A4: running ends the tired walk */
    o->role |= RF_RUNNING;                /* +0x33 b4: running (strike escalation,
                                       +1 dmg; cleared by the stand) */
    /* TODO EXACT 0x11852-0x1186A: +1 hp once when the run's +0x44
     * countdown expires (not per tick — the per-tick version refilled a
     * whipped man). Needs the run's +0x44 seed; no regen until then. */
    if (!(o->anim_sel & 0x8000u))
        o->mover = 1;
    if (o->wrestler < ENG_WS_EXT_MAX)
        o->speed = eng_mod_turbo_speed((unsigned)eng_pkg_stat((unsigned)o->wrestler, "run", (int)run_speed_of(o)));   /* 0x11C96 */
    /* 0x11CC4 edge test: only the bound you RUN INTO counts; in-ring
     * rope zone 1 -> state 6 turn (turn-then-resume IS the rope
     * rebound), consuming a bounce-budget unit (+0x44). */
    if ((o->facing & 0x8000u) ? (o->clip & 0x01u) : (o->clip & 0x02u)) {
        if (o->zone == 3) {             /* 0x11CE0: the ringside BARRIER —
                                           crash into it, no rebound */
            o->dmg = 0x0A;              /* 0x11CE8 (+0x68) */
            eng_sound(0x28);            /* 0x11CEE */
            o->state = ST_REACT; o->react_id = 0x17; o->grap44 = 0;   /* 0x11D2A-0x11D36 */
            return cell;
        }
        if (o->zone == 5 && o->grap44) {   /* 0x11CFA: the CAGE, whipped
                                           (+0x44 live) — he hits the mesh and
                                           FALLS instead of rebounding */
            o->dmg = 8;                 /* 0x11D08 (+0x68) */
            eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x11D0E -> 0x11D56 */
            o->state = ST_REACT; o->react_id = 0x17; o->grap44 = 0;   /* 0x11D2A-0x11D36 */
            return cell;
        }
        /* 0x11D14 default: the turn (= the rope rebound) — stock takes it
         * for EVERY other facing-side clip (zone 1 ropes, zone 5 cage
         * unwhipped, ...), not just zone 1. */
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x11D1A -> 0x11D56 */
        o->state = ST_TURN;                   /* 0x11D14 */
        if (o->grap44)
            o->grap44--;                /* 0x11D1E */
        return cell;
    }
    if ((o->frame == 1 || o->frame == 4) && o->count == 0)
        eng_sound(0x33);                /* footstep, 0x1112E frames 1/4 */
    o->atk = 3;                         /* running stance record, 0x11D3C */
    return cell;
}

/* 0x12888 — jab (move cell 0x12850: mode 1, n 5, dur A/6/4/A/2, sprites
 * 0,92,93,94,93). Stance record 0x21 (windup); the strike record 7 arms
 * only on frame 3 (0x1289E). Exit at +0x25 == 0xFE -> state 0. */
static uint32_t handler_jab(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u))
        o->mover = 0;
    o->atk = (o->frame == 3) ? 7 : 0x21;
    if ((o->frame & 0xFFu) == 0xFEu)
        o->state = ST_STAND;
    return cell;
}

/* 0x19FDA — kick (move cell 0x19FC6: mode 1, n 3, dur A/A/A, sprites
 * 0,1CD,1CE). Stance 1; strike record 0x11 on frame 2. */
static uint32_t handler_kick(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u))
        o->mover = 0;
    o->atk = (o->frame == 2) ? 0x11 : 1;
    if ((o->frame & 0xFFu) == 0xFEu)
        o->state = ST_STAND;
    return cell;
}

/* 0x11D8E — skid (state 3, cell 0x11D82: hold sprite 0x0A). Decelerate
 * -5 per frame until negative -> stand, mover off. The rope-pressed
 * branch (0x10FC6 carry -> state 6 turn / state 4 bounce) arrives with
 * rope zones. */
static uint32_t handler_skid(eng_obj *o, uint32_t cell)
{
    /* 0x10FC6 rope probe first: pressed into the ropes (zone 1, x-bound
     * clip this frame) -> turn (state 6). Rope-shake objects TODO. */
    if (o->zone == 1 && (o->clip & 0x03u)) {
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x10FC6 */
        o->state = ST_TURN;                                  /* 0x11DA4 carry path */
        return cell;
    }
    {
        int16_t sp = (int16_t)o->speed - 5;            /* 0x11DB2 */
        if (sp < 0) {
            o->state = ST_STAND;                              /* 0x11DC4 */
            o->mover = 0;
            o->speed = 0;
        } else {
            o->speed = (uint16_t)sp;
        }
    }
    return cell;
}

/* 0x11DE0 — turn (state 6, cell 0x11DD0: n 2, dur 8/12, sprite 0x69).
 * Entry flips facing AND angle direction (bchg #7 on +0x2E and +0x2D);
 * completion (+0x25 == 0xFE) resumes the run. */
static uint32_t handler_turn(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->facing ^= 0x8000u;                          /* 0x11DE8 */
        o->angle ^= 0x0080u;                           /* 0x11DEE */
        o->mover = 0;                                  /* 0x11DF4 */
        o->speed = 0x000C;                             /* 0x11DF8 pre-armed drift */
        o->off_x = 0x0020;                             /* 0x11DFE lean into ropes */
    } else if ((o->frame & 0xFFu) == 1) {
        o->mover = 1;                                  /* 0x11E0C drift new way */
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_RUN;                                  /* 0x11E22 resume run */
        o->off_x |= 0x8000u;                           /* 0x11E28 clear after pass */
        if (o->wrestler < ENG_WS_EXT_MAX)              /* the ROM's run init (0x11C96) reloads
                                          the run speed in THIS frame's state dispatch; the
                                          engine's input pass runs first next frame, so a
                                          press there started a running strike (0x05 keeps
                                          the polar speed) at the turn's 0xC drift - a
                                          clothesline that crawled (user 2026-08-29) */
            o->speed = eng_mod_turbo_speed((unsigned)eng_pkg_stat((unsigned)o->wrestler, "run", (int)run_speed_of(o)));
    }
    o->atk = 0x20;                                     /* 0x11E2E */
    return cell;
}

/* 0x258E — knockback launcher: {vx, vz, grav} by class, mover mode 2,
 * vx negated to knock BACKWARD (facing bit7 = facing right). */
static void knockback(eng_obj *o, unsigned i)   /* table 0x25CA, entry*6 */
{
    uint32_t a = i * 6u;                       /* row offset into knockback_launch (0x25CA) */
    o->vx = (int16_t)tbl16(TBL(knockback_launch), a);
    o->vy = 0;
    o->vz = (int16_t)tbl16(TBL(knockback_launch), a + 2);
    o->grav = (uint16_t)tbl16(TBL(knockback_launch), a + 4);
    o->mover = 2;
    if (o->facing & 0x8000u)
        o->vx = (int16_t)-o->vx;
}

/* 0x1B090 — reaction 0 flinch (cell 0x1B080: mode 1, n 2, dur 6/6,
 * spr 0x0F/0x10). No motion; combo bookkeeping; exit to stand. */
static uint32_t handler_flinch(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->combo++;
        o->combo_t = 0x40;
    } else if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND;
    }
    return cell;
}

/* 0x1B3DE — reactions 0x0A / 0x20 (cell 0x1B3D2: mode 1, n 1, dur 0x10).
 * Init: flinch bookkeeping (mover 0, combo++, combo_t 0x40). Per frame:
 * lazy divorce 0x115D2. At 0xFE: react 0x20 (top-rope dive hit) -> state 4
 * react 2 (airborne fall, 0x1B42C); otherwise +0x32 b0 -> state 1 with
 * +0xAE=1 (TODO EXACT: b0/+0xAE untranscribed), else stand (0x1B424). */
static uint32_t handler_divehit(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->combo++;
        o->combo_t = 0x40;
        return cell;
    }
    eng_lazy_divorce(o);                               /* 0x1B3F8 */
    if ((o->frame & 0xFFu) == 0xFEu) {
        if ((o->react_id & 0xFFu) == 0x20u) {
            o->state = ST_REACT; o->react_id = RC_FALL_HIGH;             /* 0x1B42C */
        } else {
            o->state = (o->st_flags & SF_APRON) ? 1 : 0;       /* 0x1B416/0x1B424 */
        }
    }
    return cell;
}

/* 0x1BE7C — reaction 0x23 (cell 0x1BE6C: mode 1, n 2, spr 0xB2/0xB3):
 * thrown by move 0x34. Entry: knockback class 0x1A. Per frame: rope bump
 * 0x10FC6; landed -> thud, lying bounce (react 5), spr 0x1F, step -0x38. */
static uint32_t handler_react23(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 0x1A);
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u))
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x10FC6 */
    if (o->landed) {
        eng_sound(0x29);                                   /* 0x1110E */
        o->state = ST_REACT; o->react_id = RC_BOUNCE;
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        add_pos_delta(o, -0x38, 0, 0);                     /* 0x10BD0 */
    }
    return cell;
}

/* 0x1BCAA — reaction 0x1C (cell 0x1BC9E: mode 1, n 1, dur 0x10, spr
 * 0x1A3 = the composite "dive connects" art): the diver (partner) is
 * hidden while it plays; at 0xFE the victim falls (react 2, facing
 * flipped) and the diver is stood back up (state 0). */
static uint32_t handler_react1C(eng_obj *o, uint32_t cell)
{
    eng_obj *p = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u)) {
        if (p) p->state = ST_HELD;                          /* 0x1BCB6 */
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_REACT; o->react_id = RC_FALL_HIGH;                     /* 0x1BCC6 */
        o->facing ^= 0x8000u;
        if (p) { p->state = ST_STAND; p->role &= (uint16_t)~0x40u; }
        o->role &= (uint16_t)~0x40u;
    }
    return cell;
}

/* 0x1B684 — reaction 0x0F (cell 0x1B66C: mode 1, n 4, dur 8/8/8/FF00,
 * spr 0xB2..): thrown off a pin (kick-out 0x4B, 0x18222/0x18290), stomp
 * 0x0A (0x1381A/0x139D4), dive victims via 0x1E. Entry: knockback class
 * 5, edge arc 0x30; b12 (0x100F from 0x1E) = re-entry without restarting
 * the art (TODO EXACT: modelled as a plain restart). Frame 3 holds with
 * landing bias -0x20; landed -> thud, bounce (react 5), spr 0x1F. The
 * ROM's trailing -0x40 step is dead code (no bsr). */
static uint32_t handler_react0F(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->react_id &= (uint16_t)~0x1000u;
        knockback(o, 5); edge_arc(o, 0x30);
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u))
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x10FC6 */
    if ((o->frame & 0xFFu) == 3) {
        o->floor42 = -0x20;                                /* 0x1B6C6 */
        if (o->landed) {
            eng_sound(0x29); o->floor42 = 0;
            o->state = ST_REACT; o->react_id = RC_BOUNCE;
            o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        }
    }
    return cell;
}

/* 0x1BD0E — reaction 0x1E (cell 0x1BCF6: mode 1, n 1, dur 0x10, spr
 * 0x172): hit by the rope-run dive 0x0E / splash 0x0F while standing.
 * Entry: mover 0, snapped to the diver + (0x28,-1,0x14), thud. At 0xFE
 * -> react 0x0F (b12 re-entry variant). TODO EXACT: the alternate record
 * 0x1BD02 substituted when the victim's own move byte != 0x0F. */
static uint32_t handler_react1E(eng_obj *o, uint32_t cell)
{
    eng_obj *p = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (p) carry_at(p, o, 0x28, -1, 0x14);             /* 0x10B9A (exg) */
        eng_sound(0x29);
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_REACT; o->react_id = 0x100Fu;               /* 0x1BD4A */
    }
    return cell;
}

/* 0x1B85E — reaction 0x15 (cell 0x1B84E: dur 8/FF00, spr 0x11A/0xB3;
 * thrown by moves 0x2C/0x3A): knockback class 8; rope bump; landed ->
 * thud, bounce (react 5), spr 0x1F, step -0x38. */
static uint32_t handler_react15(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { knockback(o, 8); return cell; }
    if (o->zone == 1 && (o->clip & 0x03u))
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x10FC6 */
    if (o->landed) {
        eng_sound(0x29);
        o->state = ST_REACT; o->react_id = RC_BOUNCE;
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        add_pos_delta(o, -0x38, 0, 0);                     /* 0x1B8B2 */
    }
    return cell;
}

/* 0x1BFA6 — reaction 0x26 (cell 0x1BF96: dur 0x20/FF00, spr 0x113/0x1F;
 * thrown by move 0x2F): knockback class 3 (+0x5A = 0x80 TODO EXACT); at
 * the end of cell 0 flip and step (-0x38, 0, -0x10); landed -> thud,
 * bounce (react 5). */
static uint32_t handler_react26(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { knockback(o, 3); return cell; }
    if (o->frame == 0 && o->count == 0) {                  /* 0x1BFC0 */
        o->facing ^= 0x8000u;
        add_pos_delta(o, -0x38, 0, -0x10);
    }
    if (o->landed) { eng_sound(0x29); o->state = ST_REACT; o->react_id = RC_BOUNCE; }
    return cell;
}

/* 0x1BED8 — reaction 0x24 (cell 0x1BECC: mode 1, n 1, dur 0x10, spr
 * 0x122; thrown by move 0x2B): at 0xFE -> lying bounce (react 5), spr 0x1F. */
static uint32_t handler_react24(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; return cell; }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_REACT; o->react_id = RC_BOUNCE;
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
    }
    return cell;
}

/* 0x1BB08 — reactions 0x18 (cell 0x1BAF0, dur 8) and 0x29 (cell 0x1BAFC,
 * dur 4): runner stagger-past / move-0x02 victim. Entry: mover 0, combo
 * timer 0x40. At 0xFE: stand; 0x29 instead falls (react 3). */
static uint32_t handler_react18(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; o->combo_t = 0x40; return cell; }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND;
        if ((o->react_id & 0xFFu) == 0x29u) { o->state = ST_REACT; o->react_id = RC_FALL; }
    }
    return cell;
}

/* 0x1B134 — reactions 2/3/4 airborne fall (cell 0x1B114: spr 0x11 flying,
 * 0x14 flat). Init arms the knockback; landing -> bounce (reaction 5). */
static uint32_t handler_fall(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->clip_h = 0;                 /* +0x40 is 0 through the flight */
        if (!mod_fall_launch(o)) {     /* modhooks.c: a mod arc (throw_out / rope exit) is already flying him */
            knockback(o, (o->react_id & 0xFFu) - 2u);
            edge_arc(o, 0x40);         /* 0x10F9C(0x40) AFTER 0x258E */
        }
    } else {
        if (o->zone == 1 && (o->clip & 0x03u))
            eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);
            /* 0x10FC6 per-frame rope bump during the fall: the shake */
        if (o->frame == 0 && o->count == 0)            /* one-shot, 0x1B172 */
            add_pos_delta(o, -0x10, 0, 0x10);          /* 0x10BD0 hop */
        o->lookahead = (o->st_flags & SF_TOPROPE) ? 0 : 0x40;   /* 0x1B182: re-arm the X look-ahead - not for a man
                                          toppled off the buckle (extended_moves): he starts ON the
                                          post, past the rope line, and the 0x40 probe behind him
                                          teleported him 0x40+ inside the first frame under 0x180 */
        if (o->landed) {
            o->lookahead = 0;          /* 0x1B190 */
            if (mod_fall_landed(o)) return cell;   /* modhooks.c: top-rope bit, the mod arcs' outside landing */
            eng_sound(0x29);           /* thud, 0x1110E */
            o->state = ST_REACT;
            o->react_id = RC_BOUNCE;
        }
    }
    return cell;
}

/* 0x1B1D0 — reaction 5 bounce (cell 0x1B1B8: hold spr 0x1F). Straight-up
 * pop; landing -> lying 8. */
static uint32_t handler_bounce(eng_obj *o, uint32_t cell)
{
    if ((o->anim_sel & 0x8000u) && cur_st && !o->cpu && !(o->driver & DRV_AUTOPILOT))
        cur_st->body_down = 1;         /* 0x1B218 jsr 0x10D04: $1C167A b7 */
    if (!(o->anim_sel & 0x8000u)) {
        o->clip_h = 0;
        eng_slide_clip(o, 0x50);       /* 0x10B62(0x50): X-only edge slide */
        knockback(o, 3);               /* vx=0 vz=0x300 g=0x38 straight pop */
        if (o->mash_aa == 0) {         /* 0x10C60(1): exact quartering */
            unsigned q = o->hp_max >> 2, d1 = q; int d2 = 3;
            while (!(d1 >= o->hp) && --d2) d1 += q;   /* 0x10CC6-0x10CD0 */
            o->mash_aa = (uint16_t)tbl8(TBL(mash_seed_quarters), (unsigned)d2); /* 01 03 07 15 */
        }
        if ((o->react_id & 0xFFu) != 0x2Au && (o->band & 0xFFu) == 2u)
            o->react_id |= 0x8000u;    /* 0x1B210: face-down flag */
    } else if (o->landed) {
        eng_sound(0x29);
        o->state = ST_REACT;
        o->react_id = (uint16_t)((o->react_id & 0xFF00u)   /* move.b +0x65: flags kept */
                    | (((o->react_id & 0xFFu) == 0x2Au) ? 8
                       : (o->react_id & 0x8000u) ? 9 : 8)); /* 0x1B242-0x1B252 */
    }
    return cell;
}

/* 0x1B318 — reactions 8/9 lying (cells 0x1B300/0x1B30C, hold forever).
 * Forced-down time by energy band (0x10F56): band2 0xE0, band1 0x80,
 * else 0x30; expiry -> get-up state 7. Pin/mash arrive later. */
static uint32_t handler_lying(eng_obj *o, uint32_t cell)
{
    eng_lazy_divorce(o);               /* 0x1B398 */
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;                  /* 0x1B320: motion + probe OFF */
        if (o->down_t == 0) {          /* 0x10F56: skip when a move pre-set it */
            if (o->cpu && (o->role & (0x20u | 0x04u)))
                o->down_t = 0x30;      /* 0x10F5C-0x10F94: a CPU (+0x56 b7) ON FIRE or OUTSIDE
                                          the ring (+0x33 b2) lies the short 0x30 whatever his
                                          band - the ringside count-outs stay hard (user
                                          2026-08-30, MAME) */
            else
                o->down_t = ((o->band & 0xFFu) == 2u) ? 0xE0
                          : ((o->band & 0xFFu) == 1u) ? 0x80 : 0x30;
        }
        if (eng_dbgsel)
            fprintf(stderr, "lying: P%d react=%X hp=%d band=%d down_t=%X\n",
                    self_idx(o) + 1, o->react_id & 0xFFu, o->hp, o->band & 0xFFu, o->down_t);
        if ((o->react_id & 0xFFu) == 9) {              /* 0x1B34E face-down */
            /* one manual probe, X AND Y applied: 0x1B362-0x1B388 */
            o->clip_h = -0x18;
            eng_slide_clip(o, 0x30);
            {   int32_t yi = (o->y >> 16) - 0x18;      /* +0x40 bias */
                if (yi < 0x118)      o->y += (0x118 - yi) << 16;
                else if (yi > 0x198) o->y += (0x198 - yi) << 16;
            }                                          /* window [0x130,0x1B0] */
            o->clip_h = 0;                             /* 0x1B388 */
        } else {
            eng_slide_clip(o, 0x50);                   /* id 8: 0x10B62(0x50) */
        }
    } else if (!(o->result & 0x8000u) && o->opp >= 0 && cur_st
               && (cur_st->obj[o->opp].tag_flags & TF_PIN_INTENT)) {
        /* 0x1B3AA: the opponent's pin intent (+0x34 b4) holds him down —
         * no mash, no down-time countdown until it clears */
    } else if ((cur_st && !o->cpu && !(o->driver & DRV_AUTOPILOT) && (cur_st->body_down = 1, 0))
               || (o->hp && o->mash_aa != 0x4000u && (o->btn_new & 3u)
               && o->mash_aa && --o->mash_aa == 0)) {
        o->mash_aa = 0x4000u;          /* 0x10D04: mashed out -> next press
                                          springs up / rolls (0xEBC4) */
    } else if ((o->st_flags & SF_ELIMINATED)        /* 0x1B390: ELIMINATED (rumble, +0x32 b4 —
                                          stock never clears the bit; 0x1152A then
                                          sends the risen man into the 0x7A
                                          shame pose + walk-out) */
               || (o->result & 0x8000u)          /* match over: stand */
               || o->down_t == 0 || --o->down_t == 0) {
        if (eng_dbgsel)
            fprintf(stderr, "lying: P%d RISE (f32=%X result=%X down_t=%X)\n",
                    self_idx(o) + 1, o->st_flags, o->result, o->down_t);
        o->state = ST_GETUP;                  /* RISE, 0x1B3C4 */
    }
    return cell;
}

/* 0x1B440 — reactions 0x0B-0x0D: hit from behind. Flip 180 and convert
 * to the front fall (id - 0x0B + 2); hidden this frame. */
static uint32_t handler_behind(eng_obj *o, uint32_t cell)
{
    o->facing ^= 0x8000u;
    o->state = ST_REACT;
    o->react_id = (uint16_t)((o->react_id & 0xFFu) - 0x0Bu + 2u);
    o->spr = 0xFFFF;
    return cell;
}

/* 0x11E42 — state 7 get-up (cell 0x11E36: 1 frame, 17 ticks, spr 0x68). */
static uint32_t handler_getup(eng_obj *o, uint32_t cell)
{
    o->clip_h = 0;                     /* body margin off once rising */
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->down_t = 0;
        o->mash_aa = 0;                /* 0x11E60 */
    } else if ((o->frame & 0xFFu) == 0xFEu) {
        /* 0x11E7A-0x11E9E: a match-over man (+0xFE) or one whose previous
         * state was a move (+0x1F == 5) just stands; otherwise the
         * rise-dizzy flag +0x64 b5 (engine 0x2000) — set by the big
         * throws (piledriver, slams, suplexes) — is consumed once and
         * sends him up into the stagger, reaction 1. */
        if (!(o->result & 0x8000u) && (o->prev_sel & 0xFFu) != 5u) {
            unsigned dizzy = o->react_id & 0x2000u;
            o->react_id &= (uint16_t)~0x2000u;             /* 0x11E88 bclr */
            if (dizzy) { o->state = ST_REACT; o->react_id = RC_DIZZY; }  /* 0x11E98/0x11E9E */
            else o->state = ST_STAND;
        } else {
            o->state = ST_STAND;                                  /* 0x11E90, flag kept */
        }
    }
    return cell;
}

/* 0x26AE homing launch: land near the partner. Table 0x275C = (dx,dy)
 * signed word pairs, dx mirrored by the PARTNER's facing; flight time
 * T = 2*(vz/grav); vx/vy from the deltas, clamped +/-0x200 / +/-0xC0. */
static void homing_launch(eng_obj *o, const eng_obj *v, unsigned idx)
{
    int16_t dx = (int16_t)tbl16(TBL(homing_land_offsets), idx * 4u);
    int16_t dy = (int16_t)tbl16(TBL(homing_land_offsets), idx * 4u + 2u);
    int T = o->grav ? 2 * (o->vz / o->grav) : 1;
    int32_t ex, ey;

    if (v->facing & 0x8000u)
        dx = (int16_t)-dx;
    if (T <= 0) T = 1;
    ex = ((((v->x >> 16) + dx) - (o->x >> 16)) << 8) / T;
    ey = ((((v->y >> 16) + dy) - (o->y >> 16)) << 8) / T;
    if (ex > 0x200) ex = 0x200; else if (ex < -0x200) ex = -0x200;
    if (ey > 0xC0) ey = 0xC0; else if (ey < -0xC0) ey = -0xC0;
    o->vx = (int16_t)ex;
    o->vy = (int16_t)ey;
    o->mover = 2;
}

/* 0x10B9A: place the partner at own position + facing-mirrored offset. */
static void carry_at(eng_obj *o, eng_obj *v, int dx, int dy, int dz)
{
    if (o->facing & 0x8000u)
        dx = -dx;
    v->x = o->x + (dx << 16);
    v->y = o->y + (dy << 16);
    v->z = o->z + (dz << 16);
}

/* 0x10F9C(D0): ring-band edge arc — inward vy when near a rope line. */
static void edge_arc(eng_obj *o, int16_t d0)
{
    int32_t yi = o->y >> 16;
    if (yi + 0x20 >= 0x198)
        o->vy = (int16_t)-d0;
    else if (yi - 0x20 < 0x118)
        o->vy = d0;
}

/* 0x10BD0: facing-mirrored position delta. */
static void add_pos_delta(eng_obj *o, int dx, int dy, int dz)
{
    if (o->facing & 0x8000u)
        dx = -dx;
    o->x += dx << 16;
    o->y += dy << 16;
    o->z += dz << 16;
}

/* 0x10D3A(D0): spawn a companion sprite for THIS frame — first free
 * $1C1258 slot (0x10D48, none free -> nothing), row := own +0x02, pose :=
 * 0x10DDA[D0].w1 | own facing word, list := .w2, x := own x + hotspot
 * byte +0x19 (facing-mirrored), y := own y + .w0, z := own z - .w0, then
 * 0x247C screen pos and 0x27B8 enqueue on the slot itself. Freed by
 * 0x10E6A after the object pass — callers re-spawn every tick. */
static void fx_spawn(eng_obj *o, unsigned idx)
{
    eng_fx *f = 0;
    uint32_t t = (idx & 0xFFu) * 6u;                       /* row offset into fx_spawn_rows (0x10DDA) */
    int16_t d = (int16_t)tbl16(TBL(fx_spawn_rows), t), hx = (int8_t)(o->off_x & 0xFFu);
    if (!cur_st) return;
    for (int k = 0; k < ENG_FX_SLOTS; k++)
        if (!cur_st->fx[k].active) { f = &cur_st->fx[k]; break; }
    if (!f) return;                                        /* 0x10D5A: all 11 busy */
    if (o->facing & 0x8000u) hx = (int16_t)-hx;            /* 0x10D9E */
    f->active = 1;
    f->row = (uint16_t)eng_sprite_obj_row(o->wrestler);    /* 0x10D7A +0x02; a clone rides his VIRTUAL row so the
                                                              companion (the leg drop's foot 0xE9) draws from HIS
                                                              pak art, not the base's (user 2026-08-29: Hogan's
                                                              lower leg under Undertaker's leg drop) */
    f->spr = (uint16_t)(tbl16(TBL(fx_spawn_rows), t + 2) | (o->facing & 0xFF00u));   /* 0x10D80 or.w +0x2E */
    f->list = (uint16_t)tbl16(TBL(fx_spawn_rows), t + 4);  /* 0x10D8C +0x12 */
    f->x = o->x + ((int32_t)hx << 16);                     /* 0x10D92/0x10DA8 */
    f->y = o->y + ((int32_t)d << 16);                      /* 0x10DBC */
    f->z = o->z - ((int32_t)d << 16);                      /* 0x10DC0 */
    f->sx = (int16_t)((f->x >> 16) - cur_st->cam_x);       /* 0x247C */
    f->sy = (int16_t)((f->y >> 16) + (f->z >> 16) - cur_st->cam_y);
}

/* 0xE958 proximity boxes: 16 x (x0,x1,y0,y1) signed bytes... words —
 * grapple reach boxes, X pair mirrored by the OPPONENT's facing. */
int eng_prox_box(const eng_obj *o, const eng_obj *v, unsigned box)
{
    int16_t x0 = (int16_t)tbl16(TBL(prox_boxes), box * 8u + 0);
    int16_t x1 = (int16_t)tbl16(TBL(prox_boxes), box * 8u + 2);
    int16_t y0 = (int16_t)tbl16(TBL(prox_boxes), box * 8u + 4);
    int16_t y1 = (int16_t)tbl16(TBL(prox_boxes), box * 8u + 6);
    int dx, dy, t;

    if (v->facing & 0x8000u) { x0 = (int16_t)-x0; x1 = (int16_t)-x1;
                               t = x0; x0 = x1; x1 = (int16_t)t; }
    dx = (int)(o->x >> 16) - (int)(v->x >> 16);
    dy = (int)(o->y >> 16) - (int)(v->y >> 16);
    return dx >= x0 && dx <= x1 && dy >= y0 && dy <= y1;
}

/* Pin chain — docs/engine-specs/pins-referee.md §1c. Simplified vs ROM:
 * the walk-up phases collapse (selection happens adjacent), positions
 * snap at connect. */
static uint32_t handler_pickup(eng_obj *o, uint32_t cell)  /* 0x13180, move 0x08 PICKUP */
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        if (!v) { o->state = ST_STAND; return cell; }
        o->mover = 0;
        o->x = v->x; o->y = v->y; o->z = v->z;         /* snap onto the body */
        o->facing = v->facing;
        o->off_x = 0x0050;
        o->cam_mode = 2;                               /* 0x131A4: +0x4A = 2,
                                                          camera frozen */
        v->partner = (int)(o - cur_st->obj);
        return cell;
    }
    if (o->frame == 0 && o->count == 0) {              /* end of frame 0 */
        if (v && (v->state & 0xFFu) == ST_REACT && (v->react_id & 0xFFu) == RC_LYING) {
            v->state = ST_HELD;                         /* HIDE the victim */
            o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;
            o->facing = v->facing;
        } else {                                       /* ABORT 0x131F4 */
            if (eng_dbgsel)
                fprintf(stderr, "cover: ABORT v=%d vst=%02X vrc=%02X\n",
                        o->partner, v ? v->state & 0xFF : -1,
                        v ? v->react_id & 0xFF : -1);
            o->state = ST_STAND;
            o->off_x |= 0x8000u;
            o->cam_mode = 0;                           /* +0x4A = 0 */
            o->partner = -1;
            if (v && v->partner == self_idx(o)) v->partner = -1;   /* unlink him too */
            o->x += (o->facing & 0x8000u) ? -(0x80 << 16) : (0x80 << 16);
        }
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu && v) {            /* finish 0x1322C */
        if (eng_dbgsel)
            fprintf(stderr, "pickup: FINISH -> standing hold\n");
        o->state = 0x000C;                             /* stand up into the hold */
        o->grap44 = 0x2000;
        o->pinning = 0; o->hold_ph = 0;
        v->state = ST_HELD;
        v->grap44 = 0;
        v->down_t = 0; v->combo = 0; v->combo_t = 0;
        v->facing = o->facing ^ 0x8000u;               /* face the holder */
        o->off_x |= 0x8000u;
        o->x += (o->facing & 0x8000u) ? -(0x80 << 16) : (0x80 << 16);
        v->x = o->x; v->y = o->y;
        o->cam_mode = 0;                               /* +0x4A = 0 */
        o->spr_force = (uint16_t)(0x01D2 | (o->facing & 0x8000u)); /* the hold's
                                          impact cell from this frame — no
                                          stale CD cell at the jumped spot */
        eng_slide_clip(o, 0);          /* 0x13180 end: 0x10B62(0) on both —
                                          clamps the pair back into the ring */
        eng_slide_clip(v, 0);
    }
    return cell;
}

static uint32_t handler_squash(eng_obj *o, uint32_t cell)  /* move 0x51 */
{
    eng_obj *p = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    /* 0x18CEA record pick: the squashed man's art depends on WHAT squashed
     * him — a rope-run splash (attacker move 0x0E) gets its own impact +
     * splash pose (0x16/0x13); otherwise face-up (0x15/0x1D) or face-down
     * (0x17/0x1E). Cell 0 holds (FF00) until he lands, cell 1 runs 2 ticks. */
    if (p && (p->move_id & 0xFFu) == 0x0Eu)      cell = 0x18C14u;   /* 0x18D0A */
    else if ((o->react_id & 0xFFu) == RC_LYING)         cell = 0x18BF4u;   /* 0x18CFA */
    else                                          cell = 0x18C04u;  /* 0x18D02 */

    if (!(o->anim_sel & 0x8000u)) {
        o->mash_aa = o->hp == 0 ? 0x100
                   : (o->hp < 0x2A ? (uint16_t)(0x2A - o->hp) : 1);
        knockback(o, 3);                                   /* 0x18C3C */
        return cell;
    }
    if (o->frame == 0 && o->landed) {                      /* 0x18C4C */
        o->mover = 0; o->count = 0;                        /* release the hold */
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x18C5E */
        if (p && eng_pin_allowed(o, p)) {                  /* 0x18C68-0x18C86 (+ the pin mods) */
            o->state = ST_MOVE; o->move_id = 0xC04Au;            /* 0x18C88/0x18C94 struggle, +0x60 = 0xC04A */
            if ((p->move_id & 0xFFu) != 0x0Eu) {           /* 0x18CA0: a rope-run
                                          splash is NOT converted into the cover —
                                          the diver holds his own splash frames */
                p->state = 0x8005; p->anim_sel |= 0x8000u; /* 0x18CA8: state AND the
                                          +0x1C latch pre-set, move 0xC048 — the cover
                                          handler must NOT re-init (no second leap) */
                p->move_id = 0xC048u; p->spr = 0xFFFF; p->mover = 0;
                p->pinning = 1; p->cue_flags |= 1u;
                p->atk = 0x801Du;      /* the covering man's cell frames carry
                                          record 0x1D in stock — a light hit on
                                          him breaks the pin (0x24BC2) */
                eng_tag_arm_pin(cur_st, p, o);             /* 0x215B6: the run-ins arm */
            }
        } else {                                           /* 0x18CC6 not a legal pin */
            o->state = ST_REACT; o->react_id = (uint16_t)(o->react_id & 0xFF00u);
            o->down_t = 0;
            if (p) { p->state = ST_GETUP; p->partner = -1; }
            o->partner = -1;
        }
    }
    return cell;
}

static uint32_t handler_struggle(eng_obj *o, uint32_t cell) /* move 0x4A, 0x180C4 */
{
    eng_obj *p = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    int splash = p && (p->state & 0xFFu) == ST_MOVE && (p->move_id & 0xFFu) == 0x0E;
    if (!(o->anim_sel & 0x8000u))
        o->mover = 0;
    if (cur_st && !o->cpu && !(o->driver & DRV_AUTOPILOT) && o->hp)
        cur_st->body_down = 1;         /* 0x180F4 jsr 0x10D04: $1C167A b7 */
    /* mash: fresh button edges count down +0xAA (0x10D04) */
    if (o->hp != 0 && o->mash_aa != 0x4000u && (o->btn_new & 3u)) {
        if (o->mash_aa && --o->mash_aa == 0)
            o->mash_aa = 0x4000u;
    }
    /* 0x1819A draw select: +0x60 b15 clear (the 0x48 cover still in the
     * air) -> hidden record + the PLAIN lying body 0x12/0x13 written by
     * hand (0x181AA-0x181CC); set -> the cover composite 0x1D/0x1E
     * (0x181D4, the pinner draws hidden as 0x8048). A RUNNING-SPLASH pin
     * (pinner holds his own 0x0E pose, 0x13F0A) must not add the cover-man
     * slice on top — stock shows the splash alone (user, 2026-08-23), so
     * it takes the plain-body branch. TODO EXACT: the ROM gate for it
     * (0x1819A tests +0x21 == 0x0E, dead in state 5 as read). */
    if (!(o->move_id & 0x8000u) || splash) {
        cell = 0x125C0u;
        o->spr_force = (uint16_t)(((o->react_id & 0xFFu) == RC_LYING ? 0x12u : 0x13u) | (o->facing & 0x8000u));
    } else
        cell = ((o->react_id & 0xFFu) == RC_LYING) ? 0x180ACu : 0x180B8u;
    return cell;
}

static uint32_t handler_kickout(eng_obj *o, uint32_t cell) /* move 0x4B */
{
    if (!(o->anim_sel & 0x8000u) && o->partner >= 0 && cur_st) {
        eng_obj *p = &cur_st->obj[o->partner];
        p->cue_flags &= (uint16_t)~1u;       /* 0x1820C: the referee flag dies */
        p->pinning = 0;
        p->role &= (uint16_t)~0x40u;
        o->role &= (uint16_t)~0x40u;
        if ((p->state & 0xFFu) != ST_REACT) p->state = ST_GETUP;   /* thrown off (the 0x0C
                                          pin path already wrote react 0x0F) */
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_GETUP; o->react_id = 0; o->partner = -1;   /* 0x1820C end */
    }
    return cell;
}

/* 0x1B288 — reactions 6/7: hit while lying. Small pop (launcher 0x15),
 * land breaks the frame-0 hold, brief flat frame, back to lying 8/9. */
static uint32_t handler_lyinghit(eng_obj *o, uint32_t cell)
{
    eng_lazy_divorce(o);               /* 0x1B2D4 */
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 0x15);            /* 0x1B290 */
        return cell;
    }
    if (o->frame == 0 && o->landed) {  /* 0x1B2BC */
        eng_sound(0x29);
        o->mover = 0;
        o->count = 0;                  /* break the FF00 hold */
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) { /* 0x1B2E2 */
        o->state = ST_REACT;
        o->react_id = (uint16_t)(((o->react_id & 0xFFu) == 6) ? 8 : 9);
    }
    return cell;
}

/* 0x13034 — move 0x06 anti-run catch: record 4 in the frame-0 window
 * only; whiff exits at end of frame 0. The caught-conversion (pipeline
 * result 3 -> grapple) is TODO EXACT — the hit knocks down for now. */
static uint32_t handler_catch(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (o->move_id & 0x8000u) {    /* caught: re-entry plays 1-2 */
            o->anim_sel |= 0x8000u; o->frame = 0;
            o->spr = (uint16_t)(0x60 | (o->facing & 0x8000u)); o->count = 0x0C;
        } else
            o->last_pair = -1;
        return cell;
    }
    if (!(o->move_id & 0x8000u)) {
        o->atk = 1;
        if ((o->frame & 0xFFu) == 0) {
            o->atk = 4;                /* record 4: box 4, dmg 0x0E */
            if (o->last_pair >= 0) {   /* CAUGHT (pipeline set +0x92) */
                o->atk = 0; o->count = 0x0C; o->move_id |= 0x8000u;
                return cell;
            }
            if ((o->count & 0xFFu) < 2)
                goto done;             /* whiff exit, 0x13088 */
        }
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
done:   o->state = ST_STAND; o->partner = -1; o->atk = 0;   /* no facing change */
    }
    return cell;
}

/* 0x1B478 — reaction 0x0E GRABBED (cell 0x1B460: 5/17/17/hold, spr
 * B1 B1 B2 B3): teleport onto the catcher's head, launcher 4 (forward
 * in the VICTIM's own facing, vz 0x600) held for frame 0, then fly over
 * him; frame 3 landing -> bounce 5 with a 0x38 step. TODO EXACT: the
 * over-the-top-rope variant (launcher 0x0E, rumble elimination). */
/* The mods' own launches (eng_throw_out / eng_throw_arc / eng_topple /
 * eng_rope_exit) and the pin gate live in modhooks.c; these three exports
 * are what they need from the transcription. */
void eng_knockback(eng_obj *o, unsigned i) { knockback(o, i); }
int  eng_land_outside(eng_obj *o) { return land_outside(o); }
eng_state *eng_anim_state(void) { return cur_st; }

/* harness (WF_PINAT, main.c): a cover imposed on a pair right now - the
 * CATCH body of handler_cover without the prox test, so the pin-break and
 * double-pin gates do not depend on the CPU sim happening to pin someone
 * beside P1 (that drive died the first time the AI changed, 2026-09-06). */
void eng_force_cover(eng_state *st, int ai, int vi)
{
    eng_obj *o = &st->obj[ai], *v = &st->obj[vi];
    cur_st = st;
    v->x = o->x; v->y = o->y; v->z = 0x140 << 16;
    v->mover = 0; v->apron = 0;
    add_pos_delta(o, 0x40, -8, 0);
    o->state = ST_HOLD; o->grap44 = 0x2000; o->pinning = 1; o->spr_force = 0xFFFF;
    o->partner = vi; o->mover = 0; o->list = 0; o->atk = 0;
    if (v->hp > 0x18) { v->hp = 0x18; v->band = 2; }   /* nearly done: a full-energy CPU kicks out at half-count 0
                                                          (pin_kick_class) and launches the pinner the next frame */
    v->mash_aa = v->hp == 0 ? 0x100 : (v->hp < 0x2A ? (uint16_t)(0x2A - v->hp) : 1);
    v->down_t = 0; v->combo = 0; v->combo_t = 0;
    v->state = ST_HELD; v->grap44 = 0; v->partner = ai; v->react_id = RC_LYING;
    eng_tag_arm_pin(st, o, v);
    if (eng_dbgsel) fprintf(stderr, "harness: o%d covers o%d (WF_PINAT)\n", ai, vi);
}

static uint32_t handler_grabbed(eng_obj *o, uint32_t cell)
{
    eng_obj *c = (o->last_pair >= 0 && cur_st) ? &cur_st->obj[o->last_pair] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        if (!c) { o->state = ST_REACT; o->react_id = RC_FALL_HIGH; return cell; }
        o->grap44 = 0; o->partner = o->last_pair;
        o->x = c->x + (((c->facing & 0x8000u) ? 0x10 : -0x10) << 16);
        o->y = c->y - (1 << 16);
        o->z = c->z + (0x48 << 16);
        if (cur_st->scene != 1 && !(cur_st->g161 & 2u)      /* 0x1B4BC cage / 0x1B4C8 ringside view showing: no exit */
            && exit_test(o, 0x96000, 0x4B000)) {           /* 0x1B4DC/0x1B4FA */
            knockback(o, 0x0E); o->role |= RF_OUTSIDE;
            if (cur_st->g161 & 1u) {
                o->st_flags |= SF_ELIMINATED;                          /* rumble: eliminated (+0x32 b4) */
                /* 0x11284/0x1B558/0x1B96C/0x200E8: the thrower's +0xC4
                 * elimination score for the BEST ranking (rank.c). */
                eng_rank_rumble_elim(cur_st, (int)(o - cur_st->obj));
            }
        } else {
            knockback(o, 4);                               /* 0x258E(4) */
            edge_arc(o, 0x30);                             /* 0x10F9C(0x30) */
        }
        o->mover = 0;                                      /* held on the head */
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u))                 /* 0x10FC6 */
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);
    if (o->frame == 0 && o->count == 0) { o->mover = 2; return cell; }
    if ((o->frame & 0xFFu) != 3) return cell;
    o->floor42 = -0x20;
    if (!o->landed && o->zone == 3) {                      /* 0x1B5D4 / 0x1B9EA barrier */
        if (cur_st->g161 & 2u) { eng_sound(0x28); o->floor42 = 0; return cell; }   /* 0x1B65C / 0x1BA72: view showing */
        land_outside(o); return cell;                      /* 0x1B632 / 0x1BA48 -> 0x68 */
    }
    if (o->landed && (o->role & RF_OUTSIDE) && !(cur_st->g161 & 2u)) {   /* 0x1B5F8 / 0x1BA0E: outside, view NOT showing */
        land_outside(o); return cell;
    }                                                      /* else 0x1B604 / 0x1BA1A plain landing */
    if (o->landed) {
        o->state = ST_REACT; o->react_id = (uint16_t)((o->react_id & 0xFF00u) | 5);
        add_pos_delta(o, -0x38, 0, 0);
        eng_sound(0x29);
        o->floor42 = 0;
        o->partner = -1;
    }
    return cell;
}


/* ---- running attack family (docs/engine-specs/running-attacks.md) ---- */

static int self_idx(const eng_obj *o) { return cur_st ? (int)(o - cur_st->obj) : -1; }

/* 0x115D2 — the "lazy divorce": a grapple link (+0x26) that is NOT mutual is
 * dropped. The ROM runs this from nine handlers only (stand 0x11560, walk
 * 0x116DE, apron follow 0x11828, walk-out 0x11B38, run 0x11CC0, dizzy
 * 0x1B0F2, lying-hit 0x1B2D4, lying 0x1B398, dive-hit 0x1B3F8) — which is
 * why a one-way link set up mid-move survives, but a man who is walking
 * around with a stale partner (e.g. after his pin was kicked out) is freed
 * and can tie up again. A decided match (+0xFE) skips the check. */
void eng_lazy_divorce(eng_obj *o)
{
    const eng_obj *p;
    if (o->result) return;                             /* 0x115D2 */
    if (o->partner < 0 || !cur_st) return;             /* 0x115D8 */
    p = &cur_st->obj[o->partner];
    if (p->partner == self_idx(o)) return;             /* 0x115E2 mutual */
    o->partner = -1;                                   /* 0x115E8 */
}
static eng_obj *last_hit_obj(const eng_obj *o)
{ return (o->last_pair >= 0 && cur_st) ? &cur_st->obj[o->last_pair] : 0; }
static int rope_contact(eng_obj *o)                    /* 0x10FC6 (C) */
{
    if (o->zone == 1 && (o->clip & 0x03u)) {
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);
        return 1;
    }
    return 0;
}

/* 0x12F40 — move 0x05 running clothesline: record 6 for frames 0-3,
 * run-collision 3 after; keeps the polar run. A caught victim (react
 * 0x10 from result 4) restarts on alt cell 0x12F28, is carried, and is
 * dumped into react 0x19 at frame 2. Ends in the skid (state 3). */
static uint32_t handler_runstrike(eng_obj *o, uint32_t cell)
{
    eng_obj *v = last_hit_obj(o);
    uint32_t alt = 0x12F28u;
    if (!(o->anim_sel & 0x8000u)) { o->mover = 1; o->last_pair = -1; return cell; }
    if (!(o->move_id & 0x8000u)) {
        o->atk = ((o->frame & 0xFFu) < 4) ? 6 : 3;
        if (v && (v->react_id & 0xFFu) == 0x10 && v->last_pair == self_idx(o)) {
            o->partner = o->last_pair; o->anim_sel = 5; o->move_id |= 0x8000u;
            o->atk = 0;
            return alt;
        }
        if (rope_contact(o)) { o->state = ST_TURN; return cell; }
    } else if ((o->frame & 0xFFu) == 2 && o->count == 0 && v) {
        v->state = ST_REACT; v->react_id = (uint16_t)((v->react_id & 0xFF00u) | 0x19);
        carry_at(o, v, 0x10, -1, 0x38);
    }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_SKID; o->partner = -1; }
    return (o->move_id & 0x8000u) ? alt : cell;
}

/* 0x1662C — move 0x2A running shoulder: record 3 on frame 0, 0x13 from
 * frame 1; keeps running, decays -3/frame on the held last frame. */
static uint32_t handler_runshoulder(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) return cell;
    o->atk = ((o->frame & 0xFFu) == 0) ? 3 : 0x13;
    if (rope_contact(o)) { o->state = ST_TURN; return cell; }
    if ((o->frame & 0xFFu) == 3) {
        o->speed -= 3;
        if ((int16_t)o->speed < 0) { o->mover = 0; o->speed = 0; o->state = ST_STAND; o->partner = -1; }
    }
    return cell;
}

/* 0x18404 — move 0x40 running jump strike: launcher 0xB at the end of
 * frame 0, record 0x16 while airborne on frame 2. */
static uint32_t handler_runjump(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 0xB); o->mover = 0; o->role |= RF_BIT3; return cell;
    }
    if (o->frame == 0 && o->count == 0) { o->mover = 2; o->role |= RF_BIT3; return cell; }
    if ((o->frame & 0xFFu) == 2) {
        if (rope_contact(o)) { o->state = ST_REACT; o->react_id = 0x17; return cell; }
        o->atk = 0x16;
        if (o->landed) { eng_sound(0x29); o->role &= (uint16_t)~RF_BIT3; o->mover = 0; o->count = 0; }
    }
    if ((o->frame & 0xFFu) == 0xFEu) o->state = ST_STAND;
    return cell;
}

/* 0x12C22 — move 0x04 running dropkick: launcher 0xA, record 0x17 only
 * while z > 0x160 on frames 0-1; hit (victim react 0x12) -> hang with
 * the victim carried (alt cell 0x12BF2), drop at frame 1; landing at
 * frame 2 flips facing; end -> state 7. TODO EXACT: f32-b1 alt launch,
 * f34-b3 alt cell 0x12C0A. */
static uint32_t handler_dropkick(eng_obj *o, uint32_t cell)
{
    eng_obj *v = last_hit_obj(o);
    uint32_t alt = 0x12BF2u;
    if (!(o->anim_sel & 0x8000u)) {
        o->last_pair = -1; o->z += 0x30 << 16;
        o->role = (uint16_t)((o->role | 0x08u) & ~0x10u);
        knockback(o, 0xA);
        return cell;
    }
    if (!(o->move_id & 0x8000u) && (o->frame & 0xFFu) < 2) {
        uint16_t lim = (o->role & RF_OUTSIDE) ? 0x120 : 0x160;
        o->atk = 0;
        if ((uint16_t)(o->z >> 16) > lim) {
            o->atk = 0x17;
            if (v && (v->react_id & 0xFFu) == 0x12 && v->last_pair == self_idx(o)) {
                o->mover = 0; o->move_id |= 0x8000u; o->z = v->z;
                o->anim_sel = 5; o->partner = o->last_pair;
                return alt;
            }
        }
    } else if ((o->move_id & 0x8000u) && (o->frame & 0xFFu) == 1 && o->count == 0 && v) {
        o->z += 0x40 << 16; o->off_x |= 0x8000u;
        v->state = ST_REACT; v->react_id = 0x19; v->tag_flags |= TF_BIT3;
        v->spr = (uint16_t)(0x00B3 | (v->facing & 0x8000u));
        carry_at(o, v, 0x10, -1, 0x10);
    }
    if ((o->frame & 0xFFu) == 2) {
        o->mover = 2;
        if (o->landed) {
            eng_sound(0x29); o->count = 0; o->mover = 0; o->role &= (uint16_t)~0x18u;
            o->facing ^= 0x8000u; o->off_x = (uint16_t)((o->off_x & 0xFF00u) | 0xD0u);
        }
    }
    if ((o->frame & 0xFFu) < 3) {
        o->lookahead = -0x10;
        if (rope_contact(o)) { o->state = ST_REACT; o->react_id = 0x17; o->dmg = 3; return cell; }
    } else
        o->lookahead = 0;
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_GETUP; o->partner = -1; }
    return (o->move_id & 0x8000u) ? alt : cell;
}

/* 0x18498 — move 0x41 running catch: record 0x10; a victim put in react
 * 0x1D by result 7 is latched and the attacker hides for 12 ticks. */
static uint32_t handler_runcatch(eng_obj *o, uint32_t cell)
{
    eng_obj *v = last_hit_obj(o);
    if (!(o->anim_sel & 0x8000u)) { o->last_pair = -1; return cell; }
    if (rope_contact(o)) { o->state = ST_TURN; return cell; }
    if (!(o->move_id & 0x8000u)) {
        o->atk = 0x10;
        if (v && (v->state & 0xFFu) == ST_REACT && (v->react_id & 0xFFu) == 0x1D) {
            o->move_id |= 0x8000u; o->spr = 0xFFFF; o->count = 0x0C; o->mover = 0;
            v->partner = self_idx(o); o->partner = o->last_pair; return cell;
        }
    }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_STAND; o->partner = -1; }
    return cell;
}

/* 0x1B706 — reactions 0x10/0x12 "caught" freeze: 32-tick hold in the
 * last pose; the attacker's alt cell overwrites it, else stand. */
static uint32_t handler_caught(eng_obj *o, uint32_t cell)
{
    o->spr = 0xFFFF;                   /* 0x1B706: hidden — the attacker's
                                          two-man alt cell draws him */
    o->count = 0xFF00;
    if (!(o->anim_sel & 0x8000u)) { o->grap44 = 0x20; o->mover = 0; return cell; }
    if (o->grap44 && --o->grap44 == 0) o->state = ST_STAND;
    return cell;
}

/* 0x1BB4C — reaction 0x19: dumped off a running strike. Launcher 0x13
 * (0x24 with f34 b3), rope bumps, landing -> bounce 5 with a 0x40 step. */
static uint32_t handler_dumped(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        if (o->tag_flags & TF_BIT3) { o->tag_flags &= (uint16_t)~0x08u; knockback(o, 0x24); }
        else knockback(o, 0x13);
        return cell;
    }
    rope_contact(o);
    if (o->landed) {
        eng_sound(0x29);
        o->state = ST_REACT; o->react_id = RC_BOUNCE;
        o->spr = (uint16_t)(((o->band & 0xFFu) == 2u ? 0x1F : 0x14) | (o->facing & 0x8000u));
        add_pos_delta(o, -0x40, 0, 0);
        o->partner = -1;
    }
    return cell;
}

/* 0x1B7A2 — reaction 0x1D: caught by the running catch 0x41; rides the
 * attacker for 12 ticks then falls (react 2, or 0x1B if he was running). */
static uint32_t handler_caught41(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        eng_obj *c = last_hit_obj(o);
        o->facing ^= 0x8000u; o->partner = o->last_pair; o->mover = 0;
        if (c) { o->x = c->x; o->y = c->y - (1 << 16); o->z = c->z; }
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_REACT; o->facing ^= 0x8000u;
        if ((o->prev_sel & 0xFFu) == 2) { o->react_id = 0x1B; add_pos_delta(o, 8, -1, 0x40); }
        else { o->react_id = RC_FALL_HIGH; add_pos_delta(o, 8, -1, 0x10); }
        o->partner = -1;
    }
    return cell;
}


/* 0x16A7A — move 0x2E FLYING TACKLE (cat 4 vs a hurt runner, w1/w5/w9;
 * record 0x16A56 mode 1 n 7 dur 0x20/2/8/9/6/FF00/0x10 spr 0xB6..0xBC).
 * Entry: mover off, last_pair cleared, sprite offset 0x40/0x50. Frame 0
 * arms record 5 every tick; the pipeline's result 0x0A (0x24784) puts the
 * runner in react 0x1F (placeholder cell 0x1BD60) and pairs +0x92, so the
 * next tick here CONVERTS: victim hidden (state 0xFF), f33 b6 both,
 * move id |= 0x8000, frame 0 cut short. Not converted by the end of frame
 * 0 -> whiff, stand. Frame 2 end -> launcher 9 (0x16B5C), f33 b3,
 * look-ahead 0x50, announcer phrase 0x13; frame 5 (FF00) released by the
 * landing (0x16B86); FE -> victim dropped behind at +0x80,-1 with react 6
 * dmg 0x10 facing flipped, self hops +0x20 into state 7 (0x16BB4).
 * TODO EXACT: 0x1108C pin-chance bit, 0x110E0 rescue arm, victim +0xC7,
 * $1C1800 shake, the one-tick victim spr-byte write at 0x16C1C. */
static uint32_t handler_tackle(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->last_pair = -1;                   /* 0x16A82 */
        o->off_x = 0x40; o->off_y = 0x50;
        return cell;
    }
    if (!(o->move_id & 0x8000u)) {
        if (o->frame == 0) {
            o->atk = 5;                                    /* 0x16AAA */
            if (o->last_pair >= 0 && cur_st) {
                eng_obj *c = &cur_st->obj[o->last_pair];
                if ((c->state & 0xFFu) == ST_REACT && (c->react_id & 0xFFu) == 0x1F) {
                    o->partner = o->last_pair;             /* 0x16ACA */
                    o->role |= RF_ENGAGED; c->role |= RF_ENGAGED;
                    c->state = ST_HELD; c->partner = self_idx(o);
                    o->move_id |= 0x8000u; o->count = 0;
                    o->off_x = 0x30; o->off_y = 0x50;
                    return cell;
                }
            }
        }
        if (o->count == 0) {                               /* 0x16B06 whiff */
            o->state = ST_STAND; o->partner = -1;
            o->off_x |= 0x8000u; o->count = 2;
            o->spr = (uint16_t)(o->facing & 0x8000u);
            return cell;
        }
    }
    if ((o->frame & 0xFFu) == 1 && o->count == 0) { o->off_x = 0x40; o->off_y = 0x50; }
    if (o->frame == 2 && o->count == 0) {                  /* 0x16B5C */
        knockback(o, 9); o->role |= RF_BIT3; o->lookahead = 0x50;
        eng_announce(cur_st, o->wrestler, 0x13);
    }
    if (o->frame == 5 && o->landed) {                      /* 0x16B86 */
        eng_sound(0x29); o->mover = 0; o->count = 0; o->lookahead = 0;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x16BB4 */
        if (v) {
            v->facing = o->facing;
            carry_at(o, v, 0x80, -1, 0);                   /* 0x10B9A */
        }
        add_pos_delta(o, 0x20, 0, 0);                      /* 0x10BD0 */
        o->off_x |= 0x8000u; o->state = ST_GETUP;
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        if (v) {
            v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x2006; v->dmg = 0x10;   /* bset #5,+0x64 */
            v->facing ^= 0x8000u; v->st_flags |= SF_SLAMMED;
            v->partner = -1;
            v->role &= (uint16_t)~0x40u;
        }
        eng_sound(0x32);
        o->partner = -1; o->role &= (uint16_t)~0x40u;
    }
    return cell;
}

/* 0x17000 — move 0x31 CATCH-AND-SLAM / powerslam (cat 4 vs a hurt runner,
 * w3; record 0x16FD8 mode 1 n 8 dur 0x20/4/4/4/4/FF00/8/0x1C spr
 * 0xC2..0xC9). Same catch shape as 0x2E with record 0x1B and victim react
 * 0x28 (0x24784 announces 0x2D for it); conversion bumps the frame to 1.
 * Whiff = end of frame 1 (0x17076). Frame 4 end -> launcher 0x22
 * (0x170B6); landing releases the FF00 hold (0x170C0); frame 6 end drops
 * the victim in front at +0x10,+1 with react 0x2A (bounce) dmg 0x16
 * (0x170EC); FE -> state 7 (0x17144). TODO EXACT as handler_tackle. */
static uint32_t handler_catchslam(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->last_pair = -1; o->off_y = 0x20; /* 0x17008 */
        return cell;
    }
    if (!(o->move_id & 0x8000u) && o->frame == 0) {
        o->atk = 0x1B;                                     /* 0x17028 */
        if (o->last_pair >= 0 && cur_st) {
            eng_obj *c = &cur_st->obj[o->last_pair];
            if ((c->state & 0xFFu) == ST_REACT && (c->react_id & 0xFFu) == 0x28) {
                o->partner = o->last_pair; c->partner = self_idx(o);
                o->role |= RF_ENGAGED; c->role |= RF_ENGAGED;
                c->state = ST_HELD;
                o->move_id |= 0x8000u; o->count = 0;
                o->frame = (uint16_t)((o->frame & 0xFF00u) | ((o->frame + 1) & 0xFFu));
                return cell;
            }
        }
    }
    if ((o->frame & 0xFFu) == 1 && o->count == 0) {        /* 0x17086 whiff */
        o->off_x |= 0x8000u; o->state = ST_STAND; o->partner = -1;
        o->spr = (uint16_t)(o->facing & 0x8000u); o->count = 2;
        return cell;
    }
    if ((o->frame & 0xFFu) == 4 && o->count == 0) knockback(o, 0x22);   /* 0x170B6 */
    if (o->frame == 5 && o->landed) { o->mover = 0; o->count = 0; }    /* 0x170C0 */
    if ((o->frame & 0xFFu) == 6 && o->count == 0) {        /* 0x170E6 */
        eng_sound(0x29);
        if (v) {
            v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x202A; v->dmg = 0x16;
            v->st_flags |= SF_SLAMMED;
            carry_at(o, v, 0x10, 1, 0);                    /* 0x10B9A */
            v->role &= (uint16_t)~0x40u; v->partner = -1;
        }
        o->role &= (uint16_t)~0x40u;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x17144 */
        o->off_x |= 0x8000u; o->state = ST_GETUP;
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        o->partner = -1;
    }
    return cell;
}


/* ---- rope-run finisher family (docs/engine-specs/rope-run-drops.md) ----
 * grap44 is the PHASE counter here (state-5 entry zeroes it): 0 run to
 * the far ropes / 1 rebound turn / 2 run back / 3 the drop. Each phase
 * ends with state=5 (bit15 clear => re-init) and grap44++. */
static uint32_t phase_run_out(eng_obj *o, eng_obj *v, uint32_t cell)   /* 0x175FC */
{
    if (!(o->anim_sel & 0x8000u)) {
        int seed;
        o->mover = 1; o->speed = eng_mod_turbo_speed(run_speed_of(o));
        v->partner = self_idx(o);                          /* mutual link */
        v->down_t = 0x200;                                 /* hold him flat */
        {   unsigned dc = v->hitctr_d2[5] < 5 ? v->hitctr_d2[5] : 5u;   /* 0x17624: word[min(+0xDC,5)] of 0x176BA
                                          (36 1E 16 10 0A, then the 0x176C4 opcode - ROM quirk) minus his hp = the
                                          mash-out need; the drops he has taken make the next one easier to pop
                                          up from mid-run ("leg drop forever", user 2026-08-30) */
            seed = (int)rom16(0x176BAu + dc * 2u) - (int)v->hp; }
        if (seed < 1) seed = 1;                            /* 0x17646 */
        if ((o->move_id & 0xFFu) == 0x23u) seed += 6;      /* 0x1764A the splash */
        v->mash_aa = (uint16_t)seed;
        if ((o->x >> 16) < 0x270) { o->facing = 0x8000; o->angle = 0x40; }
        else                      { o->facing = 0;      o->angle = 0xC0; }
        return 0x11C62u;
    }
    o->atk = 0;
    if ((o->facing & 0x8000u) ? (o->clip & 1u) : (o->clip & 2u)) {
        eng_ropes_arm((o->clip & 1u) ? 1 : 0, 1, 1);       /* 0x10FC6 */
        o->state = ST_MOVE; o->grap44++;
        return cell;
    }
    return 0x11C62u;                                       /* state-2 run cell */
}
static uint32_t phase_rebound(eng_obj *o, uint32_t cell)               /* 0x176C4 */
{
    if (!(o->anim_sel & 0x8000u)) {
        o->facing ^= 0x8000u; o->angle ^= 0x80u; o->mover = 0; o->speed = 0x0C;
    } else if ((o->frame & 0xFFu) == 1) o->mover = 1;
    else if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_MOVE; o->grap44++; return cell; }
    return 0x11DD0u;                                       /* turn cell */
}
static uint32_t phase_run_back(eng_obj *o, eng_obj *v, uint32_t cell)  /* 0x1770C */
{
    int xi;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 1; o->speed = eng_mod_turbo_speed(run_speed_of(o));
        o->run_tgt = (int16_t)((v->x >> 16) + ((v->facing & 0x8000u) ? -0x38 : 0x38)
                                            + ((o->facing & 0x8000u) ? -0x60 : 0x60));
        return 0x11C62u;
    }
    xi = o->x >> 16;
    if ((o->facing & 0x8000u) ? (xi > o->run_tgt) : (xi <= o->run_tgt)) {
        o->state = ST_MOVE; o->grap44++; return cell;
    }
    return 0x11C62u;
}

/* 0x175C4 — move 0x35 LEG DROP (Hogan): phases 0-2 shared, phase 3 the
 * leap (0x1778E): vz 0x600/g 0x48, homing 3/4 by facing parity, land ->
 * 0x11152 prox + mutual -> victim react 0x1A dmg 0x12; miss -> faceplant. */
static uint32_t handler_legdrop(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u) && (o->grap44 & 3u) == 0) {
        eng_announce(cur_st, o->wrestler, 0x12);           /* 0x17890 "Leg drop" */
        if (cur_st->g161 & 2u) o->grap44 = 3;              /* 0x175C4: the ringside view - no run-out /
                                                              rebound / run-back, straight to the drop
                                                              (MAME, user 2026-08-30) */
    }
    switch (o->grap44 & 3u) {
    case 0: return phase_run_out(o, v, cell);
    case 1: return phase_rebound(o, cell);
    case 2: return phase_run_back(o, v, cell);
    default: break;
    }
    if (o->move_id & 0x8000u) goto tail;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->vz = 0x600; o->grav = 0x48;
        homing_launch(o, v, ((o->facing ^ v->facing) & 0x8000u) ? 4 : 3);
        o->mover = 0;
        return cell;
    }
    if (o->frame == 0 && o->count == 0) o->mover = 2;
    if (o->frame == 1 && o->count == 0) o->off_y = 0x28;
    if (o->vz >= 0) { o->move_id |= 0x1000u; o->floor42 = 0x20; goto tail; }   /* 0x177FE bset #4,+0x60 (airborne) */
    o->floor42 = 0x20;
    if (!o->landed) goto tail;
    o->floor42 = 0; o->off_x |= 0x8000u;
    if (eng_prox_box(o, v, 0x0A) && v->partner == self_idx(o)) {   /* HIT */
        o->mover = 0; o->count = 0; o->move_id |= 0x8000u;
        o->y = v->y - (1 << 16);
        v->state = ST_REACT; hit_count(v, 1); v->react_id = 0x1A; v->dmg = 0x12;
        v->hitctr_d2[5]++;                                 /* 0x1786A addq +0xDC */
        o->move_id &= (uint16_t)~0x1000u;                  /* 0x1785E bclr #4 */
        eng_sound(0x2B); eng_sound(0x32);
        v->down_t = 0;
    } else {                                                         /* MISS 0x178CC */
        o->state = ST_REACT; o->react_id = RC_BOUNCE; o->dmg = 3; eng_sound(0x32);
        o->partner = -1; v->partner = -1; v->down_t = 0; v->mash_aa = 0;
        return cell;
    }
tail:
    if ((o->frame & 0xFFu) >= 4) fx_spawn(o, 0x0F);        /* 0x178A4-0x178B0: the leg/foot
                                                              companion 0xE9 beside cell 0xE8 */
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_GETUP; o->partner = -1; v->partner = -1; }
    return cell;
}

/* 0x13D46 — move 0x0E (Warrior/w3 rope-run dive onto a downed man):
 * phases 0-2 shared; phase 3 (0x13D7E) homing 2, lands into a PIN-like
 * hold: victim scripted move 0x51 dmg 0x11, attacker at victim x+/-0x30,
 * y-1. SIMPLIFIED end: the engine's cover-catch pin (state 0x0C pinning)
 * stands in for the 0x215B6 pin start. TODO EXACT. */
static uint32_t handler_dive0E(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u) && (o->grap44 & 3u) == 0) {
        eng_announce(cur_st, o->wrestler, 0x20);           /* 0x13E76 "+Splash!" */
        if (cur_st->g161 & 2u) o->grap44 = 3;              /* 0x13D46: ringside view -> phase 3 only */
    }
    switch (o->grap44 & 3u) {
    case 0: return phase_run_out(o, v, cell);
    case 1: return phase_rebound(o, cell);
    case 2: return phase_run_back(o, v, cell);
    default: break;
    }
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->vz = 0x600; o->grav = 0x48;
        homing_launch(o, v, 2);
        o->mover = 0;
        o->facing = (uint16_t)((o->vx < 0) ? 0 : 0x8000u);
        return cell;
    }
    if (o->frame == 1 && o->count == 0) o->off_y = 0x20;
    if (o->frame == 0 && o->count == 0) o->mover = 2;
    if ((o->frame & 0xFFu) != 3) goto tail;
    if (!o->landed) goto tail;
    o->off_x |= 0x8000u; o->mover = 0;
    if (eng_prox_box(o, v, 0x0A) && v->partner == self_idx(o)) {   /* HIT 0x13E20 */
        o->count = 0;                                      /* 0x13E20 */
        o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;                  /* 0x13E24 engaged */
        v->state = ST_MOVE; v->move_id = 0x51;                   /* 0x13E38: the victim
                                          plays the SQUASH record — for a 0x0E
                                          splash that is 0x18C14 (impact 0x16 ->
                                          splash pose 0x13), not the cover pin */
        v->dmg = 0x11;                                     /* 0x13E44 */
        hit_count(v, 5);                                   /* 0x13E54 */
        v->hitctr_d2[4]++;                                 /* 0x13E50 addq +0xDA */
        eng_sound(0x32); eng_sound(0x2B);
        knockback(o, 3);                                   /* 0x13E7E: the diver's
                                          own little bounce — he KEEPS his splash
                                          record (frames 4/5 hold), the ROM never
                                          converts him into the 0x48 cover */
        o->x = v->x + ((v->facing & 0x8000u) ? -(0x30 << 16) : (0x30 << 16));
        o->y = v->y - (1 << 16);                           /* 0x13E88-0x13EA6 */
        if (!(o->role & RF_OUTSIDE) && (o->role & RF_LEGAL) && (v->role & RF_LEGAL)) {
            o->cue_flags |= 1u;                                  /* 0x13ED2 referee cue */
            o->move_id |= 0x8000u;                         /* 0x13ED8 landed latch */
            o->pinning = 1;
            o->atk = 0x801Du;          /* record 0x1D through the pin frames —
                                          a light hit breaks it (0x24BC2) */
            eng_tag_arm_pin(cur_st, o, v);                 /* 0x13EDE jsr 0x215B6: the
                                          run-ins arm — "splash pin didn't
                                          trigger the team to run in" */
        }
        v->mash_aa = v->hp == 0 ? 0x100 : (v->hp < 0x2A ? (uint16_t)(0x2A - v->hp) : 1);
        v->down_t = 0; v->combo = 0; v->combo_t = 0;
        return cell;
    }
    o->state = ST_REACT; o->react_id = RC_BOUNCE; o->facing ^= 0x8000u; o->dmg = 3;   /* MISS */
    eng_sound(0x32); o->partner = -1; v->partner = -1; v->down_t = 0; v->mash_aa = 0;
    return cell;
tail:                                                  /* 0x13EE8 */
    if ((o->frame & 0xFFu) == 0xFEu) return cell;
    if (!o->landed) return cell;
    o->mover = 0; o->count = 0;                        /* 0x13EFA: stop on the mat */
    if (o->move_id & 0x8000u) return cell;             /* 0x13F0A: pinning — hold
                                          the splash pose while the count runs */
    o->state = ST_GETUP; o->partner = -1; v->partner = -1;    /* 0x13F12 */
    return cell;
}

/* 0x1BBC6 — reaction 0x1A (leg-dropped): launcher 0x17; landing -> thud,
 * down_t 0, lying 9 (low-energy pose). */
static uint32_t handler_react1A(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { knockback(o, 0x17); return cell; }
    if (o->landed) {
        eng_sound(0x29); o->down_t = 0;
        o->state = ST_REACT; o->react_id = (uint16_t)((o->react_id & 0xFF00u) | 9);
        return cell;
    }
    /* 0x1BBFA-0x1BC1A: airborne, one companion per tick — pose 0x1D5
     * (entry 0x11) on the last tick of frame 0, else 0x1D4 (entry 0x10) */
    {
        unsigned d1 = o->frame & 0xFFu;
        if (o->count != 0) d1++;
        fx_spawn(o, d1 == 0 ? 0x11u : 0x10u);
    }
    return cell;
}


/* 0x14B64 — move 0x14 (w4): standing dive onto a downed man, no rope
 * run. Homing 3/4 from victim.x +/-0x38 (+0x48 own step), land at frame 3
 * with floor bias 0x20, 0x11152 prox -> victim react 7 dmg 0x0B. */
static uint32_t handler_dive14(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (o->move_id & 0x8000u) goto tail;
    if (!(o->anim_sel & 0x8000u)) eng_announce(cur_st, 0x0F, 0x1A);   /* 0x14CC2 */
    if (!(o->anim_sel & 0x8000u)) {
        int32_t tx = (v->x >> 16) + ((v->facing & 0x8000u) ? -0x38 : 0x38);
        o->mover = 0;
        o->facing = (uint16_t)(tx >= (o->x >> 16) ? 0x8000u : 0);
        o->vz = 0x500; o->grav = 0x48; o->x = tx << 16;
        add_pos_delta(o, 0x48, -1, 0);
        homing_launch(o, v, ((o->facing ^ v->facing) & 0x8000u) ? 4 : 3);
        o->mover = 0;
        v->partner = self_idx(o);
        return cell;
    }
    if (o->frame == 0 && o->count == 0) { o->mover = 2; o->role |= RF_BIT3; }
    if (o->frame == 1 && o->count == 0) o->off_y = 0x20;
    if ((o->frame & 0xFFu) != 3) goto tail;
    o->floor42 = 0x20;
    if (!o->landed) return cell;
    o->off_x |= 0x8000u; o->floor42 = 0; eng_sound(0x29); o->mover = 0; o->count = 0;
    if (eng_prox_box(o, v, 0x0A)) {
        o->move_id |= 0x8000u;
        v->state = ST_REACT; hit_count(v, 1); v->react_id = 7; v->dmg = 0x0B;
        v->hitctr_d2[3]++;                                 /* 0x14C58 addq +0xD8 */
        eng_sound(0x32); eng_sound(0x2A);
    } else {
        o->state = ST_REACT; o->react_id = RC_BOUNCE; o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        o->partner = -1; v->partner = -1; o->facing ^= 0x8000u; add_pos_delta(o, -0x20, 0, 0);
        o->dmg = 3; eng_sound(0x32);
        return cell;
    }
tail:
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_GETUP; o->partner = -1; v->partner = -1; }
    return cell;
}

/* 0x14458 — moves 0x46/0x47 (w5, w7/w10): leap onto a downed man, dmg
 * 0x0A, victim react = lying id - 2 (8->6, 9->7). handler_leap shape. */
static uint32_t handler_dive46(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned is47 = (o->move_id & 0xFFu) == 0x47u;
    if (!v) { o->state = ST_STAND; return cell; }
    if (o->move_id & 0x8000u) goto tail;
    if (!(o->anim_sel & 0x8000u)) eng_announce(cur_st, 0x0F, 0x1A);   /* 0x14622/0x147D6 */
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->vz = 0x600; o->grav = 0x48;
        homing_launch(o, v, 2);
        o->mover = 0;
        o->facing = (uint16_t)((o->vx < 0) ? 0 : 0x8000u);
        v->partner = self_idx(o);
        return cell;
    }
    if (o->frame == 0 && o->count == 0) o->mover = 2;
    if (o->frame == 1 && o->count == 0) {
        o->off_x = is47 ? 0x00F8u : 0x00F0u; o->off_y = is47 ? 0x00F0u : 0x0028u;
    }
    if (!o->landed) goto tail;
    o->off_x |= 0x8000u; eng_sound(0x29); o->mover = 0;
    if (eng_prox_box(o, v, 0x0A) && v->partner == self_idx(o)
        && (v->state & 0xFFu) == ST_REACT
        && ((v->react_id & 0xFFu) == RC_LYING || (v->react_id & 0xFFu) == 9)) {
        o->count = 0; o->move_id |= 0x8000u; o->y = v->y - (1 << 16);
        v->react_id = (uint16_t)((v->react_id & 0xFFu) - 2u);
        v->state = ST_REACT; hit_count(v, 1); v->dmg = 0x0A;
        eng_sound(0x2B); eng_sound(0x32);
    } else {
        o->state = ST_REACT; o->react_id = RC_BOUNCE; o->dmg = 3; eng_sound(0x32);
        o->partner = -1; v->partner = -1; o->facing ^= 0x8000u;
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        if (!is47) add_pos_delta(o, -0x30, 0, 0);
        return cell;
    }
tail:
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->off_x |= 0x8000u; o->state = ST_GETUP; o->partner = -1; v->partner = -1;
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        add_pos_delta(o, 0x18, -1, 0); v->partner = -1;
    }
    return cell;
}

/* 0x13A2E — move 0x0B (w2): drop with NO prox test — hits if the victim is
 * still lying (react 8/9 -> 6/7, dmg 0x0B). Launcher 0x23. */
static uint32_t handler_drop0B(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) eng_announce(cur_st, 0x0F, 0x1A);   /* 0x13B72 */
    if (!(o->anim_sel & 0x8000u)) {
        if ((v->react_id & 0xFFu) != 9) {
            o->x = v->x; o->y = v->y - (1 << 16);
            add_pos_delta(o, 0x78, -1, 0);
        } else {
            int32_t tx = (v->x >> 16) + ((v->facing & 0x8000u) ? -0x38 : 0x38);
            o->facing = 0;
            if (tx >= (o->x >> 16)) o->facing = 0x8000;
            o->x = tx << 16;
            add_pos_delta(o, 0x38, -8, 0);
        }
        knockback(o, 0x23);
        v->partner = self_idx(o);
        return cell;
    }
    if (!o->landed) {
        if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_GETUP; o->partner = -1; v->partner = -1; }
        return cell;
    }
    o->count = 0; o->mover = 0;
    if ((v->state & 0xFFu) == ST_REACT
        && ((v->react_id & 0xFFu) == RC_LYING || (v->react_id & 0xFFu) == 9)) {
        v->react_id = (uint16_t)((v->react_id & 0xFFu) - 2u);
        v->state = ST_REACT; hit_count(v, 1); v->dmg = 0x0B;
        v->hitctr_d2[1]++;                                 /* 0x13AEA addq +0xD4 */
        eng_sound(0x29); eng_sound(0x32);
    } else {
        o->state = ST_REACT; o->react_id = RC_BOUNCE; o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        o->dmg = 3; eng_sound(0x32); o->partner = -1; v->partner = -1;
    }
    return cell;
}

/* 0x149F2 — move 0x13 (w9): hop from victim.x +/-0x38 (+0x10, z+0x20),
 * vz 0x300 g 0x30 homing 2; landing at frame 1 -> prox -> react 7 dmg 0x0A. */
static uint32_t handler_hop13(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) eng_announce(cur_st, 0x0F, 0x1A);   /* 0x14B38 */
    if (!(o->anim_sel & 0x8000u)) {
        int32_t tx = (v->x >> 16) + ((v->facing & 0x8000u) ? -0x38 : 0x38);
        o->facing = 0;
        if (tx >= (o->x >> 16)) o->facing = 0x8000;
        o->x = tx << 16; o->y = v->y; o->z = v->z;
        add_pos_delta(o, 0x10, 0, 0x20);
        o->vz = 0x300; o->grav = 0x30;
        homing_launch(o, v, 2);
        v->partner = self_idx(o);
        return cell;
    }
    if (!(o->move_id & 0x8000u) && (o->frame & 0xFFu) == 1 && o->landed) {
        eng_sound(0x29);
        if (eng_prox_box(o, v, 0x0A)) {
            v->state = ST_REACT; hit_count(v, 1); v->react_id = 7; v->dmg = 0x0A; o->move_id |= 0x8000u;
            v->hitctr_d2[2]++;                             /* 0x14AAC addq +0xD6 */
            o->mover = 0; o->count = 0x18;
            eng_sound(0x2A); eng_sound(0x32);
        } else {
            o->state = ST_REACT; o->react_id = RC_BOUNCE; o->facing ^= 0x8000u;
            o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
            add_pos_delta(o, -0x40, 0, 0); o->dmg = 3; eng_sound(0x32);
            o->partner = -1;
            return cell;
        }
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_GETUP; o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        add_pos_delta(o, 0x40, 0, 0); o->partner = -1; v->partner = -1;
    }
    return cell;
}


/* ---- out-of-ring (docs/engine-specs/out-of-ring.md) ---- */
/* Corner-diagonal exit test run at launch by reactions 0x16/0x0E: facing
 * right compares x against the right diagonal, left against the left.
 * Sets +0x33 b2 (outside: the probe switches to 0x28288) and launches
 * the over-the-ropes row. */
#define ENG_RINGOUT_ENABLED 1   /* the ring-out camera scene ($1C1678 ->
                                   0xF98C, scene 2/6, 0x28480/0x2851E) and
                                   the 20-count live in ringout.c /
                                   referee.c; WF_NORINGOUT=1 keeps the old
                                   in-ring behaviour for A/B playtests. */
static int exit_test(eng_obj *o, int32_t kr, int32_t kl)
{
    int32_t yi = o->y >> 16, xi = o->x >> 16;
    /* 0x1B4D4 btst #7,(+0x2E,A0): the VICTIM's own facing (bit7 set =
     * faces right, the engine's 0x8000) — the side he flies to after the
     * ender's bchg. Bit7 set -> 0x1B4DC/0x1B8F8 right diagonal (out when
     * x > D3); clear -> 0x1B4FA/0x1B916 left diagonal (out when x <= D3).
     * x is the already re-seated launch spot: oracle fuzz2 f2448, thrower
     * 0x204 (bit7 set, faces right), victim seated at +0x10 = 0x214 facing
     * left, 0x214 <= (0x14D<<8 + 0x4B000)/0x2E0 = 0x215 -> out.
     * TODO EXACT 0x1B8F0 reads (+0x2E,A2) with A2 inherited from the
     * dispatcher; treated as the same object. */
    if (!ENG_RINGOUT_ENABLED) return 0;
    if (cur_st && (cur_st->scene == 1 || (cur_st->g161 & 2u)))
        return 0;                      /* 0x1B4BC/0x1B8D2: cage or ringside view showing */
    if (o->facing & 0x8000u)
        return xi > -(((yi << 8) - kr) / 0x2E0);   /* 0x1B4DC / 0x1B8F8 */
    return xi <= (((yi << 8) + kl) / 0x2E0);       /* 0x1B4FA / 0x1B916 */
}
/* Shared per-frame tail for an outside flight (0x1B976-0x1BA80): on the
 * landing frame, a body flagged outside lands into move 0x68 (lying
 * outside); a barrier hit mid-flight (zone 3) does the same. */
static int land_outside(eng_obj *o)
{
    if (o->backup) {                   /* a mod BACKUP thrown out over the top rope is marked LEAVING
                                          (user 2026-08-30: no override - an AI goal; modrules.c walks
                                          him home once he is free). Here: the plain outside landing,
                                          the bounce -> lying -> get up. */
        o->backup = 2; o->throw_pend = 0;
        o->state = ST_REACT; o->react_id = RC_BOUNCE; o->mover = 0; o->partner = -1;
        eng_sound(0x29);
        if (eng_dbgsel) fprintf(stderr, "mod: backup P%d thrown out over the top - he will leave\n", self_idx(o) + 1);
        return 1;
    }
    o->state = ST_MOVE; o->move_id = 0x68; o->grap44 = 0;
    o->spr = (uint16_t)(0x16 | (o->facing & 0x8000u));
    add_pos_delta(o, 0x10, 0, 0);
    eng_sound(0x28); o->floor42 = 0; o->partner = -1;
    return 1;
}

/* 0x1BA92 — reaction 0x17: barrier / hard-contact fall. Short hop
 * (launcher 0x0D) then the normal bounce chain on whichever floor. */
static uint32_t handler_react17(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->role &= (uint16_t)~0x10u;
        knockback(o, 0x0D);
        return cell;
    }
    if (o->landed) {
        eng_sound((o->role & RF_OUTSIDE) ? 0x2D : 0x29);
        o->state = ST_REACT; o->react_id = (uint16_t)((o->react_id & 0xFF00u) | 5);
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
    }
    return cell;
}

/* 0x1BC36 — reaction 0x1B: flipped up over the anti-run counter (record
 * 0x1BC22 mode 1 n 3 dur 0x10/0x10/FF00 spr 0xB1/0xB2/0xB3). The caught
 * runner reaches it from 0x1B7A2 (react 0x14, the 0x27/0x41 catch): at FE
 * a RUNNING victim (+0x1F == 2) is popped +0x40 in the air (0x1B7E6) and
 * handed here to fly the flip arc and come DOWN. Untranscribed, the
 * generic ran-out fallback put him straight into the lying react with the
 * +0x40 still applied — he lay in MID-AIR over the mat ("whipped someone
 * into the ropes, did the knee in the stomach attack, it glitched out for
 * abit", rumble playtest 2026-08-24). Launcher 0x14 + edge arc 0x30 at
 * entry; per tick rope contact 0x10FC6 and floor42 -0x10; landing: thud
 * 0x1110E, state 4 react 5 (the bounce), spr 0x1F|facing, -0x38 step. */
static uint32_t handler_react1B(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 0x14);                                /* 0x1BC40 0x258E(0x14) */
        edge_arc(o, 0x30);                                 /* 0x1BC4A 0x10F9C(0x30) */
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u))
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x1BC58 0x10FC6 */
    o->floor42 = -0x10;                                    /* 0x1BC5E */
    if (o->landed) {                                       /* 0x1BC64 btst #4,+0x37 */
        eng_sound(0x29);                                   /* 0x1BC6C 0x1110E thud */
        o->floor42 = 0;                                    /* 0x1BC72 */
        o->state = ST_REACT;                                      /* 0x1BC76 */
        o->react_id = RC_BOUNCE;                                   /* 0x1BC7C word write */
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u)); /* 0x1BC82/0x1BC88 */
        add_pos_delta(o, -0x38, 0, 0);                     /* 0x1BC8E-0x1BC96 */
    }
    return cell;
}

/* 0x1BF08 — reaction 0x25: press-slammed over the ropes. Launcher 0x25,
 * facing flip, edge arc 0x30; never flagged outside, so it lands on the
 * mat into the bounce. */
static uint32_t handler_react25(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 0x25);                                /* 0x1BF26 */
        if (o->grap44) o->vx = (int16_t)-o->vx;            /* 0x1BF30 tst +0x44 -> neg +0x58 */
        o->facing ^= 0x8000u;                              /* 0x1BF3A bchg #7,+0x2E */
        edge_arc(o, 0x30);                                 /* 0x1BF40 */
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u))
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x1BF4E 0x10FC6 */
    if ((o->frame & 0xFFu) != 2) return cell;              /* 0x1BF54: only the FF00 hold lands */
    o->floor42 = -0x10;                                    /* 0x1BF5C: falling body meets the mat 0x10 early */
    if (o->landed) {                                       /* 0x1BF62 */
        eng_sound(0x29);                                   /* 0x1110E */
        o->floor42 = 0;                                    /* 0x1BF70 */
        o->state = ST_REACT; o->react_id = RC_BOUNCE;                     /* 0x1BF74/0x1BF7A: word write, flag cleared */
        o->spr = (uint16_t)((o->spr & 0x00FFu) | (o->facing & 0xFF00u));   /* 0x1BF80 move.b +0x2E,+0x04 */
        add_pos_delta(o, -0x28, 0, 0);                     /* 0x1BF86 0x10BD0(-0x28,0,0) */
    }
    return cell;
}

/* 8-way heading toward a target (stand-in for the 0x1F15C AI walk /
 * 0x20C8 angle-to-dest — TODO EXACT). Angle 0 = +y, 0x40 = +x. */
static int walk_to(eng_obj *o, int32_t tx, int32_t ty)
{
    int32_t dx = tx - (o->x >> 16), dy = ty - (o->y >> 16);
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax < 8 && ay < 8) return 1;
    if (ax < 8)       o->angle = dy > 0 ? 0x00 : 0x80;
    else if (ay < 8)  o->angle = dx > 0 ? 0x40 : 0xC0;
    else if (dx > 0)  o->angle = dy > 0 ? 0x20 : 0x60;
    else              o->angle = dy > 0 ? 0xE0 : 0xA0;
    o->facing = (uint16_t)(dx > 0 ? 0x8000u : 0);
    o->mover = 1;
    return 0;
}

/* 0x1A348 — move 0x7A ELIMINATED (rumble), phases on +0x44: phase 4
 * (0x1A464) turns, holds pose 0x1D3 for 0x28 ticks with the camera
 * excluding him (+0x4A = 3), clears the pin-intent links; phase 5
 * (0x1A4CA) walks to (0x140 | 0x3C0 by facing, 0xB8) and leaves: palette
 * slot freed, object cleared, $1C16A7++ (eng_rumble_slot_free). Phases
 * 1-3 (0x1A38E/0x1A3E8/0x1A430) TODO EXACT. */
static uint32_t handler_elim7A(eng_obj *o, uint32_t cell)
{
    (void)cell;
    switch (o->grap44 & 7u) {
    case 1:                                                    /* 0x1A38E: walk to the near rope */
        if (!(o->anim_sel & 0x8000u)) {
            o->mover = 1;                                      /* +0x32 = 0x1000 (b4 kept, engine) */
            o->st_flags = (uint16_t)(0x1000u | ((o->grap44 & 8u) ? 0u : 0x10u));   /* grap44 b3 = the
                                          exit_ring_climb mod: leaving, NOT eliminated */
            o->speed = walk_speed_of(o);
            o->angle = ((o->x >> 16) >= 0x280) ? 0x40 : 0xC0;  /* 0x1A3AE */
            o->facing = (uint16_t)(o->angle == 0x40 ? 0x8000u : 0);   /* 0x1A3BC-0x1A3C8 */
        } else if (o->clip) {                                  /* 0x1A3D0 tst.b +0x37: rope hit */
            o->state = ST_MOVE; o->grap44++;
        }
        return 0x115F2u;
    case 2:                                                    /* 0x1A3E8: climb out */
        if (!(o->anim_sel & 0x8000u)) {
            o->mover = 0; o->role |= RF_OUTSIDE; o->list = 2;        /* outside, between the rope halves */
            return 0x1A354u;
        }
        if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x1A402 */
            add_pos_delta(o, -0x20, 0, 0);
            o->state = ST_MOVE; o->grap44++; o->spr = o->facing;
        }
        return 0x1A354u;
    case 3:                                                    /* 0x1A430: drop to the floor */
        if (!(o->anim_sel & 0x8000u)) { o->list = 4; knockback(o, 0x1F); return 0x1A348u; }
        if (o->landed && (o->grap44 & 8u) && cur_st) {         /* MOD exit_ring_climb: he
                                          chose to leave - a LEGAL man on the floor fires
                                          the ring-out view standing (0x6B, not the 0x6A
                                          faller), anyone else walks back in via 0x68 */
            o->mover = 0; o->list = 0; o->grap44 = 0; o->state = ST_MOVE;
            if (o->backup) { o->state = ST_STAND; o->sub = 0; return 0x1A348u; }   /* a mod backup joins the
                                          ringside brawl on his feet (ringout.c) */
            if ((o->role & RF_LEGAL) && !(cur_st->g161 & 1u)) {
                cur_st->ringout_trig = 0x8000u;
                cur_st->ringout_face = o->facing;
                o->move_id = 0x6B;
            } else { o->move_id = 0x68; o->grap44 = 1; }
            return 0x1A348u;
        }
        if (o->landed) { o->state = ST_MOVE; o->grap44++; }          /* 0x1A44A */
        return 0x1A348u;
    case 0: case 4:
        if (!(o->anim_sel & 0x8000u)) {                        /* 0x1A464 */
            o->mover = 0; o->cam_mode = 3; o->list = 0;
            o->facing ^= 0x8000u;
            o->hold_t = 0x28;                                  /* +0x22 */
            if (o->grap44) {                                   /* 0x1A48E */
                o->tag_flags &= (uint16_t)~0xC0u;                    /* 0x1A494 */
                if (o->opp >= 0 && cur_st) cur_st->obj[o->opp].tag_flags &= (uint16_t)~0x80u;
            }
        }
        o->spr_force = (uint16_t)(0x1D3 | (o->facing & 0x8000u));
        if (o->hold_t && --o->hold_t == 0) {                   /* 0x1A4AC */
            o->state = ST_MOVE; o->grap44++; o->facing ^= 0x8000u;   /* 0 -> 1 (ring exit), 4 -> 5 (walk off) */
        }
        return 0x114ACu;                                       /* stand cell, pose forced */
    case 5:
        if (!(o->anim_sel & 0x8000u)) {                        /* 0x1A4CA */
            o->mover = 1;
            o->speed = walk_speed_of(o);
            o->tgt_y = 0xB8;
            o->run_tgt = (o->facing & 0x8000u) ? 0x3C0 : 0x140;
            if (o->backup) { o->run_tgt = 0x279; o->tgt_y = o->grap44 & 8u ? 0x20 : 0x100; }   /* a leaving BACKUP: the aisle
                                          mouth first (b3 clear), then up the walkway to the spawn point (b3 set) */
        }
        if (o->backup && !(o->grap44 & 8u) && walk_to(o, o->run_tgt, o->tgt_y)) {   /* at the aisle mouth: now up it */
            o->grap44 |= 8u; o->state = ST_MOVE; return 0x115F2u;
        }
        if (walk_to(o, o->run_tgt, o->tgt_y) && cur_st)        /* 0x1F15C arrive */
            eng_rumble_slot_free(cur_st, o);                   /* 0x1A504-0x1A528 */
        return 0x115F2u;
    default:
        o->grap44 = 4;
        return 0x114ACu;
    }
}

/* 0x19832 — move 0x68: landed outside, phases on grap44&3: 0 land
 * (release the FF00 hold), 1 walk to the near corner (0x3B0/0x142,
 * 0xE0), 2 to the centre (0x279, 0xE0), 3 walk up to y 0x100 -> climb
 * in 0x69. TODO EXACT: the ring-out camera scene + count ($1C1678). */
static uint32_t handler_outside(eng_obj *o, uint32_t cell)
{
    switch (o->grap44 & 3u) {
    case 0:
        if (!(o->anim_sel & 0x8000u)) { o->mover = 2; o->vx = o->vy = 0; o->off_x = 0x00D0; return cell; }
        if (o->landed) {
            int illegal_thrower = 0;
            if (cur_st && o->opp >= 0 && o->opp < ENG_MAX_OBJS && !ringout_rule(RO_ILLEGAL_THROW_OUT)) {
                /* engine rule: the man who put him out (+0x7A still points at
                 * the holder) was a run-in partner, not a legal man -> no
                 * ring-out scene; he lands outside and walks back in */
                const eng_obj *t = &cur_st->obj[o->opp];
                illegal_thrower = t->active && ((t->role ^ o->role) & 0x80u) && !(t->role & RF_LEGAL);
            }
            if ((o->role & RF_LEGAL) && cur_st && !(cur_st->g161 & 1u) && !illegal_thrower) {
                /* 0x198A0: a LEGAL man down at ringside (not rumble) fires
                 * the ring-out camera scene: $1C1678 = 0x8000, $1C1679 =
                 * facing, move 0x6A (re-initialised by 0xFAAE once the
                 * scene is composed). */
                cur_st->ringout_trig = 0x8000u;
                cur_st->ringout_face = o->facing;
                o->mover = 0;
                o->state = ST_MOVE; o->move_id = 0x6A;
                return cell;
            }
            o->mover = 0; o->role &= (uint16_t)~RF_BIT3; o->count = 0;
        }
        if ((o->frame & 0xFFu) == 0xFEu) {
            if (cur_st && (cur_st->g161 & 1u)) {               /* 0x1990A rumble: ELIMINATED */
                o->mover = 0; o->state = ST_MOVE; o->move_id = 0x7A; o->grap44 = 4;
                eng_announce(cur_st, o->wrestler, 0x29);        /* 0x1991E "eliminated" */
            } else { o->state = ST_MOVE; o->grap44 = 1; }            /* 0x198FA phase 1 */
        }
        return cell;
    case 1:
        if (!(o->anim_sel & 0x8000u)) {
            o->mover = 1; o->speed = walk_speed_of(o);
            o->run_tgt = (o->x >> 16) >= 0x280 ? 0x3B0 : 0x142;
        }
        if (walk_to(o, o->run_tgt, 0xE0)) { o->state = ST_MOVE; o->grap44 = 2; }
        return 0x115F2u;
    case 2:
        if (!(o->anim_sel & 0x8000u)) o->mover = 1;
        if (walk_to(o, 0x279, 0xE0)) { o->state = ST_MOVE; o->grap44 = 3; }
        return 0x115F2u;
    default:
        if (!(o->anim_sel & 0x8000u)) { o->mover = 1; o->angle = 0; }
        if ((o->y >> 16) >= 0x100) {
            o->mover = 0; o->y = 0x100 << 16;
            o->state = ST_MOVE; o->move_id = 0x69;
        }
        return 0x115F2u;
    }
}

/* 0x19A20 — move 0x69: climb in through the ropes (7 x 0xC). Frame 4
 * clears the outside flag; the end stands on the top rope line — or, in
 * the ringside scene ($1C0161 b1), starts at (y 0x150, z 0xF0) and ends
 * on the mat line (y 0x160, z 0x140) followed by the 0x19B04 scan that
 * fires the return ($1C1678 = 0xC000) once both legal men are in.
 * grap44 b7 (engine-only) = the state-1 sub 0x0A pre-walk to
 * (+0xBE,+0xC0) that 0x1C6DC/0x1EBF4 queue before the climb. */
static uint32_t handler_climbin(eng_obj *o, uint32_t cell)
{
    int ringside = cur_st && (cur_st->g161 & 2u);
    if (o->grap44 & 0x80u) {                                       /* pre-walk (state-1 sub 0x0A) */
        if (!(o->anim_sel & 0x8000u)) {
            o->mover = 1; o->speed = walk_speed_of(o);
        }
        if (walk_to(o, o->run_tgt, o->tgt_y)) {
            /* 0x1D760/0x1D774: the walkway walk CLAMPS to the exact spot
             * (x 0x279, y 0x100) before the climb starts — the climb pose
             * has rope pixels baked in and only lines up there. */
            o->x = (int32_t)o->run_tgt << 16; o->y = (int32_t)o->tgt_y << 16;
            o->grap44 = 0; o->mover = 0; o->state = ST_MOVE;         /* re-init onto the climb */
        }
        return 0x115F2u;
    }
    if (!(o->anim_sel & 0x8000u)) {                                /* 0x19A28 */
        o->mover = 0; o->off_y = 0x0057;
        if (ringside) {
            o->y = 0x150 << 16; o->z = 0xF0 << 16;                 /* 0x19A5A/0x19A60 */
            if (cur_st && eng_mode_rule(MODE_WEAPONS) != 2)
                eng_weapon_drop(cur_st, o);                        /* 0x19A66-0x19A7A: climbing
                                                                      back in DROPS the weapon
                                                                      (+0x1C = 2, clr +0x74) —
                                                                      HARDCORE mode carries it in */
        }
        return cell;
    }
    if ((o->frame & 0xFFu) >= 4) o->role &= (uint16_t)~0x04u;      /* 0x19A82 */
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND; o->off_x |= 0x8000u; o->spr = o->facing; o->sub = 0;
        if (!ringside) {
            o->z = 0x140 << 16; o->y = 0x118 << 16;                /* 0x19A96 */
        } else {
            int in = 0;
            o->z = 0x140 << 16; o->y = 0x160 << 16;                /* 0x19AF0 */
            for (int i = 0; i < ENG_MAX_OBJS; i++) {               /* 0x19B04 scan */
                const eng_obj *q = &cur_st->obj[i];
                if (!q->active || !(q->role & RF_LEGAL)) continue;
                if ((q->st_flags & SF_TOPROPE) || !(q->role & RF_OUTSIDE)) in++;
            }
            if (in == 2) cur_st->ringout_trig = 0xC000u;          /* 0x19B2E */
        }
    }
    return cell;
}

/* 0x18370 — move 0x3F, the ROPE STOP (record 0x1835E: mode 2, FF00 then
 * 0xC ticks, sprs 0x6C/0x6C): a whipped runner arrests himself against
 * the ropes. Queued by the 0x1EDC4 whipped-run row (state entry): a d100
 * (0x24CC) under the 0x1EE38[stage] -> [band*2] weight (20/15/10%%
 * early stages, richer later) — the user's "press both buttons" is
 * folklore; it is a per-whip energy-band roll. Init: run INTO the ropes
 * at speed 0x12 decaying (0x1837C/0x183C0), list 2 (between the rope
 * halves — the lean), off_x 0x18, running bit off, HEAVY rope shake
 * ($1C1150/$1C11B0 = 3 by x < 0x280), fx 0x12 overlay per tick; FE ->
 * stand, list 0. */
static uint32_t handler_ropestop(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 1; o->speed = 0x12;                 /* 0x18376/0x1837C */
        o->list = 2;                                   /* 0x18382 */
        o->off_x = 0x18;                               /* 0x18388 */
        {   /* MAME snap 0017 (user 2026-08-30): at the stop the man has his
             * BACK to the ropes - facing away, so the 0x247C mirror puts the
             * +0x18 lean OUT over the rope (the rope crosses his hip); the
             * engine entered facing the ropes and drew him 24 px inside.
             * The ROM reaches this through the 0x1EDC4 roll on the post-
             * rebound run init (facing already flipped by the turn); the
             * engine's stand-in trigger fires on the whip run, so the init
             * sets it: face away by side (0x18394's x < 0x280 test), the
             * decaying run still pushes INTO the ropes. TODO EXACT. */
            int left = (o->x >> 16) < 0x280;
            o->facing = left ? 0x8000u : 0;
            o->angle = left ? 0x00C0 : 0x0040;
        }
        o->role &= (uint16_t)~0x10u;                    /* 0x1838E bclr #4 running */
        eng_ropes_arm((o->x >> 16) >= 0x280 ? 1 : 0, 3, 0);   /* 0x18394-0x183AE heavy */
        return cell;
    }
    fx_spawn(o, 0x12);                                 /* 0x183B0 0x10D3A */
    if ((o->frame & 0xFFu) == 0) {                     /* 0x183BA */
        if (o->speed) o->speed--;                      /* 0x183C0 subq (+0x2A) */
        else { o->mover = 0; o->count = 0; }           /* 0x183C6 release the FF00 */
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                 /* 0x183D2 */
        o->state = ST_STAND; o->list = 0;                     /* 0x183DA/0x183E0 */
        o->mover = 0;
        o->off_x |= 0x8000u;                           /* 0x183E4 bset #7 (+0x18): the lean's 0x18 x
                                                          offset dies after this pass - it stayed on
                                                          through stand/walk (fuzz probe 2026-08-26) */
    }
    return cell;
}

/* 0x19B58 — move 0x6A: lying at ringside in the ring-out view (cell
 * 0x19B3C: 0x40/0x10/0x08 frames, spr 0x13/0x68/0x00). Entered at the
 * landing (0x198B0) and re-initialised by 0xFAAE after the scene compose,
 * which teleports him to the lying spot of the new view. */
static uint32_t handler_ringside6A(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->off_x = 0x00D0; o->z = 0x100 << 16;
        if ((o->x >> 16) < 0x280) o->x = 0x1F0 << 16; else o->x = 0x438 << 16;
        o->y = 0x168 << 16;
        return cell;
    }
    if (o->frame == 0 && o->count == 0) o->off_x |= 0x8000u;     /* 0x19B8C: +0x18 b7 */
    if ((o->frame & 0xFFu) == 0xFEu) {                           /* 0x19B9E */
        o->state = ST_MOVE; o->move_id = 0x6B; o->facing ^= 0x8000u;
    }
    return cell;
}

/* 0x19BC8 — move 0x6B (mode-0 cell 0x19BBA): walk to the ringside corner
 * spot — legal man by his x (0x42C/0x210, y 0x120), non-legal man by his
 * team side (f33 b7) — then stand (state 0, probe-exempt off). */
static uint32_t handler_ringside6B(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        int right = (o->role & RF_LEGAL) ? ((o->x >> 16) >= 0x280) : ((o->role & RF_SIDE) != 0);
        o->mover = 1; o->speed = walk_speed_of(o);   /* 0x1174C */
        o->run_tgt = right ? 0x42C : 0x210; o->tgt_y = 0x120;    /* 0x19BD4 */
    }
    if (walk_to(o, o->run_tgt, o->tgt_y)) {                      /* 0x11710 + 0x1F15C */
        o->mover = 0; o->state = ST_STAND; o->st_flags &= (uint16_t)~0x20u;  /* 0x19C1A */
    }
    (void)cell;
    return 0x115F2u;                                             /* walk cell */
}

/* 0x19EA0 — move 0x70: pick up a ringside weapon (cells 0x19E80 /
 * 0x19E90: 2 x 8 ticks, sprites 0x72/0x73, or — weapon type 1, +0x75
 * tested at 0x19EFE — 0x77/0x78). Init 0x19EA8: the man snaps onto the
 * weapon's spot and takes its facing, copies the weapon's +0x74 word
 * and sets b7 (holding, 0x19EB0/0x19EB6), weapon +0x1C = 1 (carried,
 * 0x19EBC). At 0xFE (0x19EE2): stand, +0x18 b7, y -= 0x10. */
static uint32_t handler_wpickup(eng_obj *o, uint32_t cell)
{
    eng_weapon *w = (cur_st && o->wobj >= 1 && o->wobj <= ENG_WEAPONS)
                  ? &cur_st->wpn[o->wobj - 1] : 0;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;                                   /* 0x19EA8 clr.b (1,A0) */
        if (!w || (w->state & 0xFFu) != ST_SKID) {            /* engine guard: the
                                          0xF164/0x1C784 reservation must still
                                          point at a floor weapon */
            o->wobj = 0; o->state = ST_STAND;
            return cell;
        }
        o->weapon_w = (uint16_t)(0x8000u | (w->type ? 1u : 0u));   /* 0x19EB0 + bset 0x19EB6; bit0 = the BOX pose set
                                                                 (every non-steps type; the real type stays on the
                                                                 wobj link for the cell splice, 2026-08-27) */
        w->state = ST_WALK;                                   /* 0x19EBC carried */
        w->holder = 1 + self_idx(o);
        o->x = w->x; o->y = w->y; o->z = w->z;          /* 0x19EC2-0x19ED2 */
        o->facing = w->facing;                          /* 0x19ED4 move.b (+0x2E) */
        o->off_y = (uint16_t)((o->off_y & 0xFF00u) | 0x01u);   /* 0x19EDA move.b #1,(+0x1B) */
        return (o->weapon_w & 1u) ? 0x19E90u : cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                  /* 0x19EE2 */
        o->state = ST_STAND;                                   /* 0x19EEA */
        o->off_x |= 0x8000u;                            /* 0x19EF0 bset #7,(+0x18) */
        o->y -= 0x10 << 16;                             /* 0x19EF6 subi.w #$10,(+0x0A) */
    }
    return (o->weapon_w & 1u) ? 0x19E90u : cell;             /* 0x19EFE cell by +0x75 */
}

/* 0x15A72 / 0x15ABE — release the carried weapon in front of the man:
 * weapon +0x1C = 2 (tossed flight), x = own x +/- 0x60 by the facing
 * (0x15A8A-0x15A98), y = own y, z = own z + dz (0x40 on the connect,
 * 0x10 at the whiff end). Clears +0x74 and the +0x76 link. */
static void weapon_toss(eng_obj *o, int dz)
{
    eng_weapon *w = (cur_st && o->wobj >= 1 && o->wobj <= ENG_WEAPONS)
                  ? &cur_st->wpn[o->wobj - 1] : 0;
    o->weapon_w = 0;                                         /* clr.w (+0x74) */
    o->wobj = 0;                                        /* clr.l (+0x76) */
    if (!w) return;
    w->state = ST_RUN;                                       /* move.w #2,(+0x1C,A3) */
    w->x = o->x + ((int32_t)((o->facing & 0x8000u) ? 0x60 : -0x60) << 16);
    w->y = o->y;
    w->z = o->z + ((int32_t)dz << 16);
}

/* 0x15A20 — moves 0x1E/0x1F: swing/throw the carried weapon (cells
 * 0x159F8 / 0x15A0C by +0x75: 3 x 8 ticks, sprites 0x74-0x76 or
 * 0x79-0x7B). Cell 1 is the swing frame: +0x4C = 0xF every tick
 * (hit record 0xF: abox 6, damage 0x14, result handler 6 = the
 * 0x24640 weapon shot). On a CONNECT (+0x92 pairing written by
 * 0x24DEC) the weapon is tossed forward at z + 0x40 and the man
 * stands (0x15A5A-0x15AAE); a WHIFF releases it at 0xFE with
 * z + 0x10 (0x15AB0-0x15B12). One swing per pickup. */
static uint32_t handler_wswing(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;                                   /* 0x15A28 clr.b (1,A0) */
        o->off_y = (uint16_t)((o->off_y & 0xFF00u) | 0x20u);   /* 0x15A2C move.b #$20,(+0x1B):
                                          the swing sprites 0x74-0x76 / 0x79-0x7B sit ~30 px
                                          low in the art; the release's bset #7,(+0x18)
                                          clears it with the carry hotspot (feet level
                                          through swing AND drop - user vs MAME 2026-08-29) */
        o->last_pair = -1;                              /* 0x15A32 clr.l (+0x92) */
        return (o->weapon_w & 1u) ? 0x15A0Cu : cell;
    }
    o->atk = 0;                                         /* 0x15A3A clr.w (+0x4C) */
    if ((o->frame & 0xFFu) == 1) {                      /* 0x15A3E cmpi.b #1,(+0x25) */
        o->atk = 0xF;                                   /* 0x15A46 */
        if (o->count == 0 && o->last_pair >= 0) {       /* 0x15A4C tst.w (+0x22) /
                                                           0x15A52 tst.l (+0x92) */
            weapon_toss(o, 0x40);                       /* 0x15A72-0x15AAE */
            o->state = ST_STAND;                               /* 0x15A5A */
            o->count = 2;                               /* 0x15A60 */
            o->spr = o->facing;                         /* 0x15A66 */
            o->off_x |= 0x8000u;                        /* 0x15A6C bset #7,(+0x18) */
        }
    } else if ((o->frame & 0xFFu) == 0xFEu && (o->weapon_w & WPN_HELD)) {   /* 0x15AB0/0x15AB8 */
        weapon_toss(o, 0x10);                           /* 0x15ABE-0x15AF8 whiff drop */
        o->state = ST_STAND;                                   /* 0x15AFA */
        o->spr = o->facing;                             /* 0x15B00 */
        o->off_x |= 0x8000u;                            /* 0x15B06 */
        o->divorce = 1;                                 /* 0x15B0C bset #7,(+0x26) */
    }
    return (o->weapon_w & 1u) ? 0x15A0Cu : cell;             /* 0x15B14 cell by +0x75 (both
                                                           zero after the release, as in
                                                           stock's cleared +0x75) */
}


/* 0x10C60 — mash seed by mode (escape-machine.md §3b). */
static void mash_seed(eng_obj *o, int mode)
{
    if (o->mash_aa && !(o->mash_aa & 0x8000u)) return;
    if (mode == 0)
        o->mash_aa = o->hp == 0 ? 0x100 : (0x2A > o->hp ? (uint16_t)(0x2A - o->hp) : 1);
    else if (mode == 1) {
        unsigned q = o->hp_max >> 2, d1 = q; int d2 = 3;
        while (!(d1 >= o->hp) && --d2) d1 += q;
        o->mash_aa = (uint16_t)tbl8(TBL(mash_seed_quarters), (unsigned)d2);
    } else {
        unsigned idx = (unsigned)(mode - 1) * 3u + (o->band & 0xFFu);
        o->mash_aa = (uint16_t)(tbl8(TBL(mash_seed_rows), idx) - 1u);
    }
}

/* 0x19536 — victim moves 0x5D/0x5E/0x5F/0x60/0x62/0x63 (code-only
 * record): hidden (the holder's cell is the composite), FF00 hold, mash
 * seed by mode; presses count down (0x10D04); never transitions — the
 * escape is 0xEBC4 on the next press (submissions.md §2). */
static uint32_t handler_held(eng_obj *o, uint32_t cell)
{
    unsigned m = o->move_id & 0xFFu;
    o->spr = 0xFFFF; o->count = 0xFF00;
    if (!(o->anim_sel & 0x8000u)) {
        int mode = (m == 0x5E) ? 0 : (m == 0x60) ? 6 : (m == 0x62 || m == 0x63) ? 7 : 4;
        o->combo = 0; o->mover = 0; o->mash_aa = 0;
        mash_seed(o, mode);
        o->ai_b7 &= (uint8_t)~0x02u;   /* re-arm the CPU victim's escape timer for THIS hold
                                          (ai.c victim_escape sets b1 once and only a run
                                          launch cleared it: a CPU held a second time never
                                          kicked out - "cpu cant kick out of a leg lock",
                                          playtest 2026-08-27) */
        return cell;
    }
    if (m == 0x5E && o->hp == 0) return cell;
    if (o->mash_aa != 0x4000u && (o->btn_new & 3u) && o->mash_aa && --o->mash_aa == 0)
        o->mash_aa = 0x4000u;                          /* 0x10D2E */
    return cell;
}

/* 0x1978A — move 0x67 (escape from 0x5D) / 0x1944A — 0x5C (from 0x5F):
 * the holder is frozen to 0xFF at init, thrown off mid-move (react 6 /
 * 0x27 — 0x27 stands in as 6, TODO EXACT), victim rises (state 7). */
static uint32_t handler_escape(eng_obj *o, uint32_t cell)
{
    eng_obj *h = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned is67 = (o->move_id & 0xFFu) == 0x67u;
    unsigned is59 = (o->move_id & 0xFFu) == 0x59u;     /* 0x19274: out of the 0x37 hold */
    if (!h) { o->state = ST_GETUP; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        h->state = ST_HELD; h->grap44 = 0;
        o->cue_flags &= (uint16_t)~0x04u; h->cue_flags &= (uint16_t)~0x02u;
        o->mover = 0;
        return cell;
    }
    if ((o->frame & 0xFFu) == (is67 ? 2u : is59 ? 0u : 1u) && o->count == 0
        && (h->state & 0xFFu) == 0xFFu) {
        h->state = ST_REACT; h->react_id = (uint16_t)(is59 ? 2 : 6);
        if (is67) { h->facing ^= 0x8000u; h->dmg = 2; carry_at(o, h, 0x38, 1, 0); h->y -= 1 << 16; }
        else if (is59) { h->facing ^= 0x8000u; h->dmg = 3; o->divorce = 1; }   /* 0x192A2-0x192B4 */
        else      { h->dmg = 3; carry_at(o, h, 0x10, -1, 0); }
        eng_sound(is59 ? 0x29 : 0x32);
        o->role &= (uint16_t)~0x40u; h->role &= (uint16_t)~0x40u;
        h->partner = -1;
        if (cur_st) eng_tag_pin_end(cur_st, o, h);   /* 0x192CA jsr 0x213A6 (0x59); the 0x67 / 0x5C escapes
                                          end through 0x212A0 too (0x1916C/0x191E8 family) - only 0x59
                                          restored here, so after a submission hold the pad stayed with
                                          the run-in partner and both men sat on the CPU until the usher
                                          ("switch-back bug", user 2026-08-30) */
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_GETUP; o->partner = -1;
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
    }
    return cell;
}


/* 0x17A58 — move 0x37 HOLD-FOR-PARTNER (entry 0x17A40: 4 x 8 ticks, sprs
 * 0x18B/0x18C/0x18D/0x1CF, mode 4): the man who tagged out with the
 * opponent lying in his corner (0x1F760 window, 0x214C0) pins the downed
 * man's legs while his partner comes in over the ropes (0x4E) and works
 * him freely. Init: victim -> move 0x61, x/y/z/facing = the victim's,
 * f33 b6 both, teammate -> 0x4E unless he is climbing the post (f32 b1:
 * the CPU dive roll). Frame 0 / count 0 latches the victim (b7: he hides,
 * his 0xAA countdown runs); FE loops. It ends when the victim's 0x61
 * expires or is mashed out (0x59: the holder is thrown off) or a hit on
 * the pair breaks it (hit.c). */
static uint32_t handler_hold37(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!v && o->opp >= 0 && cur_st) { o->partner = o->opp; v = &cur_st->obj[o->opp]; }
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        v->state = ST_MOVE; v->move_id = 0x61; v->grap44 = 0;    /* 0x17A64 */
        v->partner = self_idx(o);
        if (!(o->move_id & 0x8000u)) {                     /* 0x17A70 first init */
            eng_obj *P = (o->teammate >= 0 && cur_st) ? &cur_st->obj[o->teammate] : 0;
            o->x = v->x; o->y = v->y; o->z = v->z;         /* 0x17A78 */
            o->facing = v->facing;
            o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;              /* 0x17A90 engaged */
            if (P && P->active && !(P->st_flags & SF_TOPROPE)) {     /* 0x17AA0 not on the post */
                P->state = ST_MOVE; P->move_id = 0x4E; P->grap44 = 0;   /* 0x17AAA enter */
                P->apron = 1;                              /* the 0x4E ender steps him in */
            }
        } else {                                           /* 0x17ACC re-init: keep looping */
            o->frame = 0; o->count = 0;
        }
        return cell;
    }
    if (o->frame == 0 && o->count == 0) v->move_id |= 0x8000u;   /* 0x17AE6 latch */
    if ((o->frame & 0xFFu) == 0xFEu) { o->frame = 0; o->count = 0; }   /* 0x17AF4 loop */
    o->spr_force = 0;
    return cell;
}

/* 0x194D0 — victim move 0x61 (held for the partner, code-only record):
 * init seeds the mash counter (0x10C60 mode 5), arms his own partner to
 * come and break it (0x211EC) and starts the 0xAA-tick countdown; until
 * the holder latches him he shows spr 0x12 (lying), then he is hidden
 * (the holder's cell is the composite) and the countdown runs -> 0x59
 * (escape: the holder is thrown off). The 0xEBC4 ladder mashes him out
 * early (core.c 0x61 -> 0x59). Engine safety: a holder no longer in 0x37
 * (hit, swept) frees him at once. */
static uint32_t handler_held61(eng_obj *o, uint32_t cell)
{
    eng_obj *h = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u)) {
        if (!(o->move_id & 0x8000u)) {                     /* 0x194D8 */
            o->mover = 0; o->combo = 0; o->mash_aa = 0;
            mash_seed(o, 5);                               /* 0x194E0 */
            if (h && cur_st) eng_tag_arm_victim61(cur_st, o, h);   /* 0x194EA */
            o->grap44 = 0xAA;                              /* 0x194F0 */
        }
        return cell;
    }
    if (!h || (h->state & 0xFFu) != ST_MOVE || (h->move_id & 0xFFu) != 0x37) {
        o->state = ST_GETUP; o->partner = -1; o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        o->role &= (uint16_t)~0x40u;
        return cell;
    }
    if (o->mash_aa != 0x4000u && (o->btn_new & 3u) && o->mash_aa && --o->mash_aa == 0)
        o->mash_aa = 0x4000u;                              /* 0x194FA 0x10D04 */
    if (!(o->move_id & 0x8000u)) {                         /* 0x19508 not latched yet */
        o->spr = (uint16_t)(0x12 | (o->facing & 0x8000u));
        return cell;
    }
    o->spr = 0xFFFF; o->count = 0xFF00;                    /* 0x19516 hidden */
    if (o->grap44 && --o->grap44 == 0) {                   /* 0x1951C countdown */
        o->state = ST_MOVE; o->move_id = 0x59;                   /* 0x19522 escape */
    }
    return cell;
}

/* 0x1A154 - move 0x77: the held man's flinch under the partner's stomp
 * (record 0x1A148: 1 cell, 0x18 ticks, spr 0x1DF). Init freezes the holder
 * (+0x26 -> state 0xFF); at 0xFE both snap back: victim 0x8061 (held,
 * latched), holder 0x8037 (holding, latched). */
static uint32_t handler_held77(eng_obj *o, uint32_t cell)
{
    eng_obj *h = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (h) h->state = ST_HELD;                               /* 0x1A15C */
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                          /* 0x1A164 */
        o->state = ST_MOVE; o->move_id = 0x8061u;
        if (h) { h->state = 0x8005u; h->move_id = 0x8037u;
                 h->anim_sel = 0x8005u; h->frame = 0; h->count = 0; }   /* the freeze put his selector on the
                                          HIDDEN cell (0x125C0) - back on the move cells + the 0x17ACC loop
                                          re-init, or the composite never drew again ("sprites disappeared
                                          after the stomp", user 2026-08-30). b15 SET: the ROM's 0x37 handler keys its
                                          init on +0x1C b7, which the freeze never cleared - the engine's
                                          state-b15 init would rerun 0x17A64 (victim +0x60 := 0x61, latch
                                          off) and the 0x61 init reseeded the 0xAA countdown: every stomp
                                          restarted the escape clock (user 2026-08-30) */
    }
    return cell;
}

/* ---- turnbuckle (docs/engine-specs/turnbuckle.md) ---- */
static const int16_t post_xy[4][2]  = { {0x1DC,0x193},{0x314,0x193},{0x1BD,0x120},{0x338,0x120} }; /* 0x121C8 */
static const int16_t perch_xy[4][2] = { {0x1E4,0x190},{0x310,0x190},{0x1D0,0x118},{0x31C,0x118} }; /* 0x12256 */

/* 0x11FAE — state 8 climb (cells 0x11F0A far / 0x11EA6 near, 13 ticks a
 * frame): teleport to the post base at rope height, at frame 1 shift out
 * onto the post and up to the perch height, face the ring; end -> perch. */
/* Corners 8/9 = the ringside-scene side posts (0x121D8 / 0x12276), used
 * while $1C0161 b1 shows the ring-out view: climb handler 0x12122, cell
 * 0x11EE6 (7 x 0xC), climb-down cell 0x122C2. */
static const int16_t post_xy_rs[2][2]  = { {0x278,0x160},{0x3C8,0x160} };  /* 0x121D8 */
static const int16_t perch_xy_rs[2][2] = { {0x268,0x158},{0x3D8,0x158} };  /* 0x12276 */
static uint32_t handler_climb(eng_obj *o, uint32_t cell)
{
    unsigned c = o->grap44 & 0xFu;
    int rs = c >= 8;                   /* 0x12122 branch (corner 8/9) */
    int dt = c >= 4 && c < 8;          /* 0x12078 rows 4-7: the DOUBLE-TEAM corner
                                          mount (move 0x66's partner, corner = c&3) */
    (void)cell;
    /* 0x11F86 cell table by row: 0/1 0x11F0A, 2/3 0x11EA6, 4/5 0x11F26,
     * 6/7 0x11ECA, 8/9 0x11EE6. The double-team rows are the SHORT records
     * (no 0x66/0x67 mount frames — the man starts at rope height), so
     * playing the full ones ran the mount after he was already up
     * (playtest 2026-08-26 "climb animation out of order"). */
    cell = rs ? 0x11EE6u
         : dt ? ((c & 3) < 2 ? 0x11F26u : 0x11ECAu)
         : (c < 2 ? 0x11F0Au : 0x11EA6u);
    if (dt) {
        if (!(o->anim_sel & 0x8000u)) {                /* 0x12082 */
            o->mover = 0; o->sub = 0;
            o->st_flags |= SF_TOPROPE;                           /* 0x1208A bset #1,+0x32 */
            o->x = post_xy[c & 3][0] << 16;            /* 0x121C8 corner post */
            o->y = post_xy[c & 3][1] << 16;
            o->z = 0x180 << 16;                        /* 0x120B2 straight to rope height */
            o->x += ((c & 1) ? 0x18 : -0x18) << 16;    /* 0x120B8 (+0x45 b0) */
            o->facing = (uint16_t)((c & 1) ? 0 : 0x8000u);   /* 0x120CA-0x120DA */
            o->list = (c & 2) ? 4 : 0;                 /* 0x120DE btst #1,+0x45 */
            o->cam_mode = 0;
            eng_sound(0x32);                           /* 0x120FA */
            eng_announce(cur_st, 0x0F, 0x17);          /* 0x120EA "climbs the top rope" */
        } else if ((o->frame & 0xFFu) == 0xFEu) {
            o->state = ST_PERCH; o->list = 0;                 /* 0x12112/0x12118 -> perch */
        }
        return cell;
    }
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->st_flags |= SF_TOPROPE;
        if (rs) { o->x = post_xy_rs[c - 8][0] << 16; o->y = post_xy_rs[c - 8][1] << 16; }
        else    { o->x = post_xy[c & 3][0] << 16;    o->y = post_xy[c & 3][1] << 16; }
        o->z = 0x140 << 16;
        o->facing = (uint16_t)((c & 1) ? 0x8000u : 0);    /* 0x11FEA / 0x12162: (+0x45&1)<<7 —
                                          faces the post (outward), flipped at
                                          frame 1 to face the ring. Oracle seed 19
                                          f2573: corner 8 fc=00, f2599 fc=80. */
        if (!rs) o->list = 2;          /* 0x11FEE; 0x12122 leaves +0x12 alone */
        o->cam_mode = 0;
        eng_sound(0x32);
        eng_announce(cur_st, 0x0F, 0x17);              /* 0x11FFC / 0x12166: "climbs the top rope" */
    } else if ((o->frame & 0xFFu) == 1 && o->count == 0) {
        o->x += ((c & 1) ? 0x18 : -0x18) << 16;       /* 0x12020 / 0x12192 */
        o->z = 0x180 << 16;
        o->facing ^= 0x8000u;
        if (!rs) o->list = (c & 2) ? 0 : 4;
    } else if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_PERCH; if (!rs) o->list = 0;            /* 0x12062 / 0x121BE */
    }
    return cell;
}

/* 0x121E6 — state 9 perch (code-only cell): perch spot, z 0x180, spr 0x30
 * far / 0x36 near, 0x80-tick timer -> climb down; DOWN -> climb down
 * (core.c); buttons -> the cat 0xE/0xF dives (core.c). */
static uint32_t handler_perch(eng_obj *o, uint32_t cell)
{
    unsigned c = o->grap44 & 0xFu;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->list = 0; o->hold_t = 0x80;
        if (c >= 8) { o->x = perch_xy_rs[c - 8][0] << 16; o->y = perch_xy_rs[c - 8][1] << 16; }   /* 0x12256[8/9] */
        else        { o->x = perch_xy[c & 3][0] << 16;    o->y = perch_xy[c & 3][1] << 16; }
        o->z = 0x180 << 16;
    }
    /* 0x12220: spr = 0x1227E[+0x44 & 3] (30 30 36 36) | facing — on the
     * ringside posts he faces INTO the ring (oracle seed 19 f2665: corner
     * 8, fc 0x80); the dive init turns him toward the victim (0x15418). */
    o->spr_force = (uint16_t)(((c & 2) ? 0x36 : 0x30) | (o->facing & 0x8000u));
    if ((o->result & 0x8000u) || (o->hold_t && --o->hold_t == 0)) o->state = ST_CLIMBDOWN;
    o->partner = -1;
    return cell;
}

/* 0x122E6 — state 0xA climb down (cells 0x12282 near / 0x122A6 far, 17
 * ticks a frame): back to the rope step mid-way, finish standing inside
 * (x -/+ 0x28), corner freed. */
static uint32_t handler_climbdown(eng_obj *o, uint32_t cell)
{
    unsigned c = o->grap44 & 0xFu;
    int rs = c >= 8;                   /* 0x122EE / 0x12356 / 0x12420: $1C0161 b1 branches */
    (void)cell;
    cell = rs ? 0x122C2u : (c & 2) ? 0x12282u : 0x122A6u;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (rs) {
            o->role &= (uint16_t)~0x04u;                    /* 0x12304 bclr #2 */
            o->x = (post_xy_rs[c - 8][0] + ((c & 1) ? 0x18 : -0x18)) << 16; o->y = post_xy_rs[c - 8][1] << 16;
        } else {
            o->x = (post_xy[c & 3][0] + ((c & 1) ? 0x18 : -0x18)) << 16; o->y = post_xy[c & 3][1] << 16;
        }
        o->z = 0x180 << 16;
        o->list = (rs || (c & 2)) ? 0 : 4;                 /* 0x1233E/0x1234E, 0x12448 */
    } else if (rs) {
        if ((o->frame & 0xFFu) == 4u && o->count == 0) o->z = 0x140 << 16;   /* 0x123D0 */
        if ((o->frame & 0xFFu) == 0xFEu) {                 /* 0x123E4 */
            o->state = ST_STAND; o->spr = o->facing;
            if (cur_st) cur_st->corner_bits = 0;           /* 0x123F8 clr.l */
            o->role &= (uint16_t)~0x04u; o->st_flags &= (uint16_t)~0x02u;
            o->x += ((c & 1) ? -0x18 : 0x18) << 16;        /* 0x1240E */
        }
    } else if ((o->frame & 0xFFu) == ((c & 2) ? 4u : 2u) && o->count == 0) {
        o->list = 2; o->z = 0x140 << 16;
    } else if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND; o->spr = o->facing; o->list = 0;
        if (cur_st) cur_st->corner_bits = 0;               /* 0x123AC clr.l */
        o->st_flags &= (uint16_t)~0x02u;
        o->x += ((c & 1) ? -0x28 : 0x28) << 16;
    }
    return cell;
}

static void dive_init(eng_obj *o, eng_obj *v, int16_t vz, uint16_t grav, unsigned k)
{
    o->mover = 0;
    if (cur_st) cur_st->corner_bits = 0;                   /* clr.l $1C1670 */
    o->st_flags &= (uint16_t)~0x02u;
    o->vz = vz; o->grav = grav;
    homing_launch(o, v, k);
    o->mover = 0;
    o->list = 0;
}

/* 0x153BC — moves 0x1B / 0x0D (cat 0xE, from the top onto a standing
 * man): vz 0x680 g 0x48, homing 0 from victim.x +/-0x10, flight from
 * frame 0's end, frame 3 lands (floor bias -0x10): victim standing/walking
 * /dizzy within 0x30 -> react 0x20 dmg 0x12 faces the diver. No miss
 * faceplant. TODO EXACT: cases 0/1 (held victim 0x53). */
static uint32_t handler_topdive(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->opp >= 0 && cur_st) ? &cur_st->obj[o->opp] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        int16_t d;
        /* 0x153E2: grap44 = f34 b3 ? 0 : victim in 0x53 ? 1 : 2 (case 2 =
         * the standing victim); 0x15410 case 2 faces the victim by x
         * (bit7 set = right); 0x1542C homes on victim.x +/- 0x10 (ahead of
         * own facing), restored after 0x26AE. Oracle seed 19 f2728: perch
         * (0x268,0x158) fc 0x80, victim 0x26D -> vx -0x3D vy -0xC0. */
        o->grap44 = (o->tag_flags & TF_BIT3) ? 0 : (((v->state & 0xFFu) == ST_MOVE && (v->move_id & 0xFFu) == 0x53) ? 1 : 2);
        if ((o->grap44 & 0xFFu) == 2)
            o->facing = (uint16_t)(((o->x >> 16) < (v->x >> 16)) ? 0x8000u : 0);
        d = (o->grap44 & 0xFFu) ? 0x10 : -0x10;
        if (o->facing & 0x8000u) d = (int16_t)-d;
        v->x += (int32_t)d << 16;
        dive_init(o, v, 0x680, 0x48, 0);
        v->x -= (int32_t)d << 16;
        return cell;
    }
    if (o->frame == 0 && o->count == 0) o->mover = 2;
    if ((o->frame & 0xFFu) == 3) {
        o->floor42 = -0x10;
        if (o->landed) {
            unsigned ps = v->state & 0xFFu, rc = v->react_id & 0xFFu;
            int32_t dx = (v->x >> 16) - (o->x >> 16), dy = (v->y >> 16) - (o->y >> 16);
            int rs_ok = !(cur_st && (cur_st->g161 & 2u)) || (v->role & RF_OUTSIDE);   /* 0x15562: ringside -> victim must be outside */
            eng_sound(0x29); o->count = 0; o->mover = 0; o->floor42 = 0;
            if ((o->grap44 & 0xFFu) == 0) {
                /* 0x154B6 mode 0, the DOUBLE-TEAM landing: the buckle-held
                 * victim crumples (react 0x1C dmg 0x12), crowd $F19, and
                 * 0x21424 arms the aftermath (usher + retaliation run-in;
                 * the diver's pad comes back). No box test — the dive homed
                 * onto the frozen pair. */
                o->tag_flags &= (uint16_t)~0x08u;            /* 0x154B6 bclr #3 */
                eng_sound(0x2B); eng_sound(0x32);
                v->state = ST_REACT; v->react_id = 0x1C;      /* 0x154BC/0x154C2 */
                v->dmg = 0x12;                         /* 0x154C8 */
                eng_dt_land(cur_st, o);                /* 0x21424 */
            } else
            if (!v->apron && rs_ok && !(v->st_flags & SF_APRON) && (ps == ST_STAND || ps == ST_WALK || (ps == ST_REACT && rc == RC_DIZZY))
                && dx > -0x30 && dx < 0x30 && dy > -0x30 && dy < 0x30) {
                eng_sound(0x2B); eng_sound(0x32);
                v->state = ST_REACT; if ((o->move_id & 0xFFu) == 0x1Bu) hit_count(v, 1); v->react_id = 0x20; v->facing = o->facing ^ 0x8000u;
                v->dmg = 0x12;
            }
        }
    }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_STAND; o->spr = o->facing; o->cam_mode = 0; o->partner = -1; }
    return cell;
}

/* 0x13F90 — move 0x0F (cat 0xF splash, ids 0/3/6/8/A): vz 0x700 g 0x48
 * homing 1; landing on a downed man -> he is pinned under the splash
 * (scripted 0x51 -> the pin chain), dmg 0xF; miss -> faceplant. */
static uint32_t handler_splash(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->opp >= 0 && cur_st) ? &cur_st->obj[o->opp] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        dive_init(o, v, 0x700, 0x48, 1); v->partner = self_idx(o);
        eng_announce(cur_st, o->wrestler, 0x15);           /* 0x14074 "Splash!" */
        return cell;
    }
    if (o->frame == 0 && o->count == 0) o->mover = 2;
    if (o->move_id & 0x8000u) {
        o->spr = 0xFFFF; o->mover = 0;                         /* 0x141D0: the latched
                                          splasher draws the hidden record 0x125C0 —
                                          the victim's 0x51/0x4A composite carries him */
        return cell;
    }
    if ((o->frame & 0xFFu) == 3 && o->landed) {
        eng_sound(0x29); o->mover = 0; o->count = 0;
        if ((v->state & 0xFFu) == ST_REACT && ((v->react_id & 0xFFu) == RC_LYING || (v->react_id & 0xFFu) == 9)
            && eng_prox_box(o, v, 0x0A)) {
            o->move_id |= 0x8000u; o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;
            v->dmg = 0x0F; eng_sound(0x2B); eng_sound(0x32);
            o->x = v->x; o->y = v->y - (1 << 16);
            v->state = ST_MOVE; v->move_id = 0x51;                    /* pinned under the splash */
            v->partner = self_idx(o); o->partner = self_idx(v);
        } else {
            o->state = ST_REACT; o->react_id = RC_BOUNCE; o->dmg = 3; eng_sound(0x32);
            o->facing ^= 0x8000u; add_pos_delta(o, -0x30, 0, 0); o->partner = -1;
        }
    }
    return cell;
}

/* 0x13BC8 — move 0x0C (cat 0xF dive, ids 1/2/5/7): vz 0x700 g 0x48
 * homing 2, facing from vx; landing on a downed man -> react 8->6 / 9->7,
 * dmg 0xF, count 0x10; miss -> faceplant; end -> state 7. */
static uint32_t handler_dive0C(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->opp >= 0 && cur_st) ? &cur_st->obj[o->opp] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) eng_announce(cur_st, 0x0F, 0x1A);   /* 0x13D06 */
    if (!(o->anim_sel & 0x8000u)) {
        dive_init(o, v, 0x700, 0x48, 2);
        o->facing = (uint16_t)((o->vx < 0) ? 0 : 0x8000u);
        v->partner = self_idx(o);
        return cell;
    }
    if (o->frame == 0 && o->count == 0) { o->mover = 2; o->role |= RF_BIT3; }
    if (!(o->move_id & 0x8000u) && o->landed && (o->frame & 0xFFu) >= 1) {
        o->move_id |= 0x8000u; o->mover = 0; o->count = 0x10;
        if ((v->state & 0xFFu) == ST_REACT && ((v->react_id & 0xFFu) == RC_LYING || (v->react_id & 0xFFu) == 9)
            && eng_prox_box(o, v, 0x0A)) {
            v->react_id = (uint16_t)((v->react_id & 0xFFu) - 2u); v->state = ST_REACT; hit_count(v, 1);
            v->dmg = 0x0F; o->y = v->y - (1 << 16); eng_sound(0x2B); eng_sound(0x32);
        } else {
            o->state = ST_REACT; o->react_id = RC_BOUNCE; o->dmg = 3; eng_sound(0x32);
            o->facing ^= 0x8000u; o->partner = -1; v->partner = -1;
            return cell;
        }
    }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_GETUP; o->partner = -1; v->partner = -1; }
    return cell;
}

/* 0x17282 — move 0x33 (Jake cat C: lift-and-slam, record 0x17266 mode 1
 * n 5 dur 0x14/0xC/8/FF00/0x18 cells 0x131-0x135). Twin of the press slam
 * 0x19: entry hides the victim (carry +0x100,0,+0x10) and sets the 0x40/0x40
 * hotspot; frame 0 -> hotspot 0x4C; frame 2 -> attacker knockback 0x16;
 * frame 3 probe 0x80 ahead -> over-the-ropes variant (spr 0x135, victim
 * react 0x25 dmg 0x12 at +0x80,1,+0x40); landing releases the FF00 hold
 * and drops the victim (react 0x25 dmg 0x12 at +0xB0,0,+0x10, spr 0x1F);
 * end -> state 7 via the 0x20 hop, spr 0x68. TODO EXACT: the 0x1129E
 * success roll (fail -> move 0x84), announcer $1C15D2, 0x110E0 rescue arm,
 * 0x1108C pin-chance bit, victim +0xC7 += 5. */
static uint32_t handler_jakeslam(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->off_x = 0x40; o->off_y = 0x40;                  /* 0x17294 */
        if (v) { v->state = ST_HELD; carry_at(o, v, 0x100, 0, 0x10); }
        if (!throw_roll(o, 0xF6)) { o->state = ST_MOVE; o->move_id = 0x84; return cell; }   /* 0x172A4/0x172BA */
        eng_announce(cur_st, o->wrestler, 0x07);           /* 0x172DC */
        return cell;
    }
    if (o->frame == 0 && o->count == 0) o->off_x = 0x4C;   /* 0x172F6 */
    if (o->frame == 2 && o->count == 0) knockback(o, 0x16); /* 0x1730A */
    if (o->frame == 3 && o->count == 0 && v && !(o->move_id & 0x8000u)) {
        int32_t px = (o->x >> 16) + ((o->facing & 0x8000u) ? 0x80 : -0x80);
        int32_t py = o->y >> 16;                           /* 0x1732A probe */
        int32_t xmin = ((py << 8) + 0x40000) / 0x2E0, xmax = -(((py << 8) - 0xA3000) / 0x2E0);
        if ((px < 0x220 && px < xmin) || (px > 0x2E0 && px > xmax)) {
            o->move_id |= 0x8000u;                         /* 0x17344 */
            o->spr = (uint16_t)(0x135 | (o->facing & 0x8000u));
            o->frame = 3; o->count = 0xFF00u;              /* 0x17358 */
            v->state = ST_REACT; v->react_id = 0x2025; v->st_flags |= SF_SLAMMED; v->grap44 = 0;   /* 0x1736A + bset #5 */
            v->dmg = 0x12; v->facing ^= 0x8000u;           /* 0x17386 */
            v->partner = -1;
            carry_at(o, v, 0x80, 1, 0x40);                 /* 0x10B9A */
            eng_ropes_arm(px > 0x2E0 ? 1 : 0, 1, 1);       /* 0x11058 */
            o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
        }
    }
    if (o->frame == 3 && o->landed) {                      /* 0x173B8 */
        eng_sound(0x29); o->mover = 0; o->count = 0;
        eng_sound(0x32);                                   /* 0x2052(0x32) */
        if (v && !(o->move_id & 0x8000u)) {
            v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x2005; v->st_flags |= SF_SLAMMED;   /* 0x173F8/0x173FE: record 5 + flag */
            v->dmg = 0x12; v->partner = -1;
            carry_at(o, v, 0xB0, 0, 0x10);
            o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
            o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        }
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x1743E */
        o->off_x |= 0x8000u;
        o->state = ST_GETUP;
        add_pos_delta(o, 0x20, 0, 0);                      /* 0x10BD0 */
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        o->partner = -1;
    }
    return cell;
}

/* 0x16258 — move 0x26 PILEDRIVER (w1/w9 cat C, w5 cat A, w3 cat D;
 * record 0x1623C mode 1 n 5 dur 0x14/8/0x10/FF00/0x20 cells 0x204-0x208).
 * Entry: victim hidden, knockback class 0x12 armed but parked (mover 0),
 * announcer phrase 0x14. Frame 1 -> take off (mover 2, f33 b3). Landing
 * on frame 3 releases the hold; frame 4 drops the victim (react 0x25 dmg
 * 0x16 at +0x70,-1,+0x20, spr 0x1F); end -> state 7, spr 0x68.
 * TODO EXACT: 0x1129E roll (fail -> 0x85), 0x110E0, +0xC7, $1C1800, 0x1108C. */
static uint32_t handler_piledriver(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (v) v->state = ST_HELD;                          /* 0x16264 */
        if (!throw_roll(o, 0xF8)) { o->state = ST_MOVE; o->move_id = 0x85; return cell; }   /* 0x1626E/0x16284 */
        knockback(o, 0x12); o->mover = 0;                  /* 0x1628E/0x16298 */
        eng_announce(cur_st, o->wrestler, 0x14);           /* 0x1629C */
        return cell;
    }
    if (o->frame == 1 && o->count == 0) { o->mover = 2; o->role |= RF_BIT3; } /* 0x162BE */
    if (o->frame == 3 && o->landed) {                      /* 0x162D6 */
        eng_sound(0x29); o->mover = 0; o->count = 0;
    }
    if (o->frame == 4 && o->count == 0 && v) {             /* 0x16306 */
        v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x2005; v->dmg = 0x16; v->st_flags |= SF_SLAMMED;   /* 0x1630C/0x16326: record 5 + rise-dizzy flag */
        eng_sound(0x32); v->partner = -1;
        carry_at(o, v, 0x70, -1, 0x20);                    /* 0x10B9A */
        o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x16360 */
        o->state = ST_GETUP; o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        o->partner = -1;
    }
    return cell;
}

/* 0x16436 — move 0x28 lifting slam (w11 cat C, w4 cat D; record 0x1641E
 * mode 1 n 4 dur 0x10/0x10/FF00/0x20 cells 0x14E-0x151). Entry: X-edge
 * slide -0x30 (0x10B62), victim hidden. Frame 1: announcer phrase 0x11,
 * z += 0x1C, knockback class 0x1D. Landing releases the hold (sounds
 * 0x2B/0x32); frame 3 drops the victim (react 0x25 dmg 0x12, facing
 * flipped, at -0xA0,0,0, spr 0x1F); end -> state 7, spr 0x68.
 * TODO EXACT: 0x1129E roll (fail -> 0x7D), 0x110E0, +0xC7, $1C1800. */
static uint32_t handler_liftslam(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        eng_slide_clip(o, -0x30);                          /* 0x16442 */
        if (v) v->state = ST_HELD;                          /* 0x1644A */
        return cell;
    }
    if (o->frame == 1 && o->count == 0) {                  /* 0x16476 */
        eng_announce(cur_st, o->wrestler, 0x11);
        o->z += 0x1C << 16;                                /* 0x16494 */
        knockback(o, 0x1D);
    }
    if (o->landed && o->mover == 2) {                      /* 0x164A8 */
        o->mover = 0; o->count = 0;
        eng_sound(0x2B); eng_sound(0x32);
    }
    if (o->frame == 3 && o->count == 0 && v) {             /* 0x164D8 */
        v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x2005; v->dmg = 0x12; v->st_flags |= SF_SLAMMED;   /* 0x164EC/0x16502: record 5 + rise-dizzy flag */
        v->facing ^= 0x8000u; v->partner = -1;
        carry_at(o, v, -0xA0, 0, 0);                       /* 0x10B9A */
        o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x1653A */
        o->state = ST_GETUP; o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        o->partner = -1;
    }
    return cell;
}

/* 0x12A08 — move 0x02 stance throw (w4/w7 cat B; record 0x129F0 mode 1
 * n 4 dur 0xC/0xC/FF00/8 cells 0x142,0x143,0xAC,0x17A). Entry: victim
 * hidden, hotspot y 0x10, X-edge slide -0x18. Frame 1: attacker knockback
 * class 7, victim placed -0x20 then hop +0x20,0,+0x50, victim -> react 0x29
 * dmg 0x0F, sounds 0x2B/0x32. Landing: thud, release hold, step -0x28.
 * End -> state 7. TODO EXACT: 0x110E0, 0x1108C (w7). */
static uint32_t handler_throw02(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (v) v->state = ST_HELD;                          /* 0x12A14 */
        o->off_y = 0x10;                                   /* 0x12A1A */
        eng_slide_clip(o, -0x18);                          /* 0x12A24 */
        return cell;
    }
    if (o->frame == 1 && o->count == 0 && v) {             /* 0x12A2C */
        o->off_x |= 0x8000u;
        knockback(o, 7);
        carry_at(o, v, -0x20, 0, 0);                       /* 0x10B9A */
        add_pos_delta(o, 0x20, 0, 0x50);                   /* 0x10BD0 */
        v->state = ST_REACT; v->react_id = 0x29; v->dmg = 0x0F; v->partner = -1;
        o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
        eng_sound(0x2B); eng_sound(0x32);
    } else if (o->landed && o->mover == 2) {               /* 0x12A9E */
        eng_sound(0x29); o->count = 0; o->mover = 0;
        add_pos_delta(o, -0x28, 0, 0);
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x12AC4 */
        o->state = ST_GETUP; o->partner = -1;
    }
    return cell;
}

/* 0x1669A — move 0x2B (w7 cat C, w4 cat D; record 0x1667A mode 1 n 6 dur
 * 0x10/8/A/A/FF00/0x1C cells 0x11B-0x120). Entry: prox 0x11412 (miss ->
 * stand), victim hidden, hotspot (-0x50,0x50); re-entry from the hold
 * (+0x46 set): facing flipped, victim at -0xA0, hotspot -0x10. Announcer
 * phrase 0x08. Frame 3: knockback 0x1E. Frame 4: probe 0xB0 ahead ->
 * over-the-ropes variant (spr 0x120, victim react 0x25 dmg 0x18 at
 * -0x70/-0x40,1,0x40); landing on frame 4 releases the hold and drops the
 * victim (react 0x24 dmg 0x18 at -0x50/-0x80,1,0x20, spr 0x1F). End ->
 * state 7 via step -0x70, spr 0x68. TODO EXACT: 0x1129E roll (fail 0x7F),
 * 0x110E0, 0x1108C, +0xC7, $1C1800. */
static uint32_t handler_throw2B(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (!v || !eng_prox_box(o, v, 0x0A)) { o->state = ST_STAND; return cell; } /* 0x166A8 */
        v->state = ST_HELD;
        o->off_x = 0xB0; o->off_y = 0x50;                  /* 0x166B8 */
        if (o->hold_t) {                                   /* 0x166E8 re-entry */
            o->facing ^= 0x8000u;
            carry_at(o, v, -0xA0, 0, 0);
            o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;
            o->off_x = 0xF0;
        }
        eng_announce(cur_st, o->wrestler, 0x08);           /* 0x16724 */
        return cell;
    }
    if (o->frame == 3 && o->count == 0) knockback(o, 0x1E); /* 0x1675C */
    if (o->frame == 4 && v && !(o->move_id & 0x8000u)) {   /* 0x1677C probe */
        int32_t px = (o->x >> 16) + ((o->facing & 0x8000u) ? 0xB0 : -0xB0);
        int32_t py = o->y >> 16;
        int32_t xmin = ((py << 8) + 0x40000) / 0x2E0, xmax = -(((py << 8) - 0xA3000) / 0x2E0);
        if ((px < 0x220 && px < xmin) || (px > 0x2E0 && px > xmax)) {
            o->move_id |= 0x8000u;
            o->spr = (uint16_t)(0x120 | (o->facing & 0x8000u));
            v->state = ST_REACT; v->react_id = 0x2025; v->st_flags |= SF_SLAMMED; v->grap44 = 1;   /* 0x167B0 + bset #5 */
            v->dmg = 0x18; v->facing ^= 0x8000u; v->partner = -1;
            carry_at(o, v, o->hold_t ? -0x40 : -0x70, 1, 0x40);
            eng_ropes_arm(px > 0x2E0 ? 1 : 0, 1, 1);       /* 0x11058 */
            o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
        }
    }
    if (o->frame == 4 && o->landed) {                      /* 0x1680A */
        eng_sound(0x29); o->mover = 0; o->count = 0;
        if (v && !(o->move_id & 0x8000u)) {
            v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x2024; v->st_flags |= SF_SLAMMED; v->dmg = 0x18;   /* 0x16842 + bset #5 */
            eng_sound(0x32); v->partner = -1;
            carry_at(o, v, o->hold_t ? -0x80 : -0x50, 1, 0x20);
            o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
        }
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x16896 */
        o->off_x |= 0x8000u;
        o->state = ST_GETUP;
        add_pos_delta(o, -0x70, 0, 0);
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        o->partner = -1;
    }
    return cell;
}

/* 0x1853C — move 0x43 (w8/w10 cat C; record 0x18518 mode 1 n 7 dur
 * E/8/10/E/6/FF00/14 cells 0x1B2-0x1B8). Entry: victim hidden, hotspot
 * (0x30,0x50), announcer phrase 0x22, victim carried at +0xC0. Frame 1:
 * hotspot 0x40. Frame 2: knockback 9, lookahead 0x50. Landing on frame 5
 * releases the hold. End (0x16BE2 tail): victim dmg 0x18 at +0x80,-1,0
 * facing flipped -> react 6 (flat), self step +0x60, state 7, spr 0x68.
 * TODO EXACT: 0x1129E roll (fail 0x80), 0x110C/0x110E0, +0xC7, $1C1800. */
static uint32_t handler_throw43(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (v) { v->state = ST_HELD; carry_at(o, v, 0xC0, 0, 0); }
        o->off_x = 0x30; o->off_y = 0x50;                  /* 0x1854E */
        if (!throw_roll(o, 0xEE)) { o->state = ST_MOVE; o->move_id = 0x80; return cell; }   /* 0x1855E/0x18574 */
        eng_announce(cur_st, o->wrestler, 0x22);           /* 0x1857E */
        return cell;
    }
    if (o->frame == 1 && o->count == 0) o->off_x = 0x40;   /* 0x185AE */
    if (o->frame == 2 && o->count == 0) { knockback(o, 9); o->lookahead = 0x50; } /* 0x185C2 */
    if (o->frame == 5 && o->landed) {                      /* 0x185DA */
        o->mover = 0; o->count = 0; o->lookahead = 0;
        eng_sound(0x29);
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x185FC -> 0x16BE2 */
        if (v) {
            v->dmg = 0x18; v->facing ^= 0x8000u;
            carry_at(o, v, 0x80, -1, 0);
            v->state = ST_REACT; v->react_id = 0x2006; v->st_flags |= SF_SLAMMED;   /* 0x16C06 + bset #5 (shared tail) */
            v->spr = (uint16_t)(v->facing);
            v->partner = -1;
            o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
        }
        add_pos_delta(o, 0x60, 0, 0);
        o->off_x |= 0x8000u;
        o->state = ST_GETUP; o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        eng_sound(0x32);
        o->partner = -1;
    }
    return cell;
}

/* 0x128FA — move 0x01 (w4 dropkick; cat 1/2/4 from the ground, cat E from
 * the top rope; alias 0x45; record 0x128DE mode 1 n 5 dur 4/8/FF00/FF00/
 * 0x10 cells 0x85,0xAB,0xAC,0x17A,0x17A). Entry: from the perch (f32 b1)
 * -> vz 0x700 g 0x60 homing class 0, corner freed, flight now; else
 * knockback class 0x1B (0x1C when +0x1F == 2, TODO EXACT), f33 b3. Per
 * frame: rope crash -> react 0x17 dmg 3. Record 0x18 armed on frame 2;
 * frame 2 ends when z < 0x150 (0x110 outside) with a -0x28 step; frame 3
 * ends on landing (thud); end -> state 7. */
static uint32_t handler_move01(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        if (o->st_flags & SF_TOPROPE) {                              /* 0x12902 top rope */
            eng_obj *v = (o->opp >= 0 && cur_st) ? &cur_st->obj[o->opp] : 0;
            o->st_flags &= (uint16_t)~0x02u;
            o->vz = 0x700; o->grav = 0x60;                 /* 0x1292C */
            if (v) homing_launch(o, v, 0);                 /* 0x26AE(0) */
            if (cur_st) cur_st->corner_bits = 0;           /* clr.l $1C1670 */
            o->mover = 2;
        } else {
            /* 0x1290A/0x1290E: class 0x1B (vertical, vz 0x800) from a
             * stand; class 0x1C (vx -0x300 forward) when the previous
             * state (+0x1F) was RUN — the running dropkick travels */
            knockback(o, ((o->prev_sel & 0xFFu) == 2u) ? 0x1C : 0x1B);
            o->role |= RF_BIT3;
        }
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u)) {               /* 0x12952 rope crash */
        o->state = ST_REACT; o->react_id = 0x17; o->dmg = 3;
        eng_sound(0x32);
        return cell;
    }
    o->atk = 0;
    if ((o->frame & 0xFFu) == 2) {
        o->atk = 0x18;                                     /* 0x12986 */
        if ((o->z >> 16) < ((o->role & RF_OUTSIDE) ? 0x110 : 0x150)) {
            o->count = 0;                                  /* 0x129AA */
            add_pos_delta(o, -0x28, 0, 0);
        }
    }
    if ((o->frame & 0xFFu) == 3 && o->landed) {            /* 0x129C4 */
        o->mover = 0; o->count = 0; eng_sound(0x29);
    }
    if ((o->frame & 0xFFu) == 0xFEu) o->state = ST_GETUP;         /* 0x129DA */
    return cell;
}

/* Shared landing for the top-rope drops 0x11/0x12 (0x14718-0x1478C,
 * 0x148DE-0x1496C): prox 0x11152 on a downed man -> react 8->6 / 9->7,
 * hold 0x18, y = victim.y - 1; miss -> faceplant (react 5, dmg 4, spr
 * 0x1F, announcer 0x1A). TODO EXACT: the 0x11192 branch (standing victim
 * caught under the drop -> his move 0x77) is treated as a miss. */
static void topdrop_land(eng_obj *o, eng_obj *v, unsigned dmg, int off_y)
{
    eng_sound(0x29); o->mover = 0;
    if ((v->state & 0xFFu) == ST_REACT && ((v->react_id & 0xFFu) == RC_LYING || (v->react_id & 0xFFu) == 9)
        && eng_prox_box(o, v, 0x0A)) {
        o->count = 0x18;
        o->y = v->y - (1 << 16);
        v->react_id = (uint16_t)((v->react_id & 0xFFu) - 2u); v->state = ST_REACT;
        v->dmg = (uint16_t)dmg; eng_sound(0x32);
        hit_count(v, 1);                                   /* 0x14748 / 0x1490C (TODO EXACT the 0x14966 branch) */
        (void)off_y;
    } else {
        o->state = ST_REACT; o->react_id = RC_BOUNCE; o->dmg = 4; eng_sound(0x32);
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        eng_announce(cur_st, 0x0F, 0x1A);                  /* 0x147D6 */
    }
}

/* 0x14670 — move 0x11 (w11 cat F top-rope drop; record 0x14658 mode 1
 * n 4 dur 8/8/0x28/FF00 cells 0x84,0x85,0xEF,0xF0). Entry: corner freed,
 * vz 0x700 g 0x48, homing class 0xA (5 when the victim is held, move
 * 0x61). Frame 0 end -> flight (f33 b3); frame 1 -> hotspot y 0x20, floor
 * bias +0x20; frame 3 landing -> topdrop_land dmg 0xD; end -> state 7. */
static uint32_t handler_topdrop11(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->opp >= 0 && cur_st) ? &cur_st->obj[o->opp] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        unsigned k = ((v->state & 0xFFu) == ST_MOVE && (v->move_id & 0xFFu) == 0x61) ? 5u : 0xAu;
        dive_init(o, v, 0x700, 0x48, k);                   /* 0x1467C-0x146AC */
        return cell;
    }
    if (o->frame == 0 && o->count == 0) { o->mover = 2; o->role |= RF_BIT3; } /* 0x146C2 */
    if (o->frame == 1 && o->count == 0) { o->off_y = 0x20; o->floor42 = 0x20; } /* 0x146E0 */
    if ((o->frame & 0xFFu) == 3 && o->landed) {            /* 0x146F6 */
        o->off_x |= 0x8000u; o->floor42 = 0;
        topdrop_land(o, v, 0x0D, 0x0C);
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x1478C */
        o->state = ST_GETUP; o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
    }
    return cell;
}

/* 0x14826 — move 0x12 (w9 cat F moonsault; record 0x147E6 mode 1 n 6
 * dur C/C/C/C/0x20/FF00 cells 0x8B,0x8C,0x8E,0x8F,0x15F,0x1B1). Frame 1
 * (human only): big pop 0x31 and skip to frame 3. Frame 3 end: hop +0x20
 * z, vz 0x580 g 0x48, homing class 2 (5 when the victim is held), corner
 * freed, flight. Frame 5 landing -> topdrop_land dmg 0xA; end -> state 7. */
static uint32_t handler_moonsault(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->opp >= 0 && cur_st) ? &cur_st->obj[o->opp] : 0;
    if (!v) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; return cell; }
    if (o->frame == 1 && o->count == 0 && !(o->driver & DRV_CPU)) { /* 0x14836 */
        eng_sound(0x31); o->frame = 3;
    }
    if (o->frame == 3 && o->count == 0) {                  /* 0x1485C */
        unsigned k = ((v->state & 0xFFu) == ST_MOVE && (v->move_id & 0xFFu) == 0x61) ? 5u : 2u;
        add_pos_delta(o, 0, 0, 0x20);
        dive_init(o, v, 0x580, 0x48, k);
        o->mover = 2; o->role |= RF_BIT3;
    }
    if ((o->frame & 0xFFu) == 5 && o->landed)              /* 0x148CA */
        topdrop_land(o, v, 0x0A, 0x0C);
    if ((o->frame & 0xFFu) == 0xFEu) o->state = ST_GETUP;         /* 0x14970 */
    return cell;
}

/* 0x168F8 — move 0x2C backbreaker (w9/w10 cat A-B, w4 cat C; record
 * 0x168D8 mode 1 n 6 dur 8/8/C/14/C/10). Entry: victim hidden, announcer
 * phrase 0x10, X-edge slide 0x30. Frame 1: hotspot x 8. Frame 4: sound
 * 0x2B, victim -> react 0x15 dmg 0x10 facing away, at +0x30,-1,+0x28.
 * End -> stand, step +0x20, spr 0x68. TODO EXACT: 0x1108C/0x110E0 (id != 9). */
static uint32_t handler_backbreaker(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (v) v->state = ST_HELD;
        eng_announce(cur_st, o->wrestler, 0x10);           /* 0x1690A */
        eng_slide_clip(o, 0x30);                           /* 0x1691E */
        return cell;
    }
    if (o->frame == 1 && o->count == 0) o->off_x = 8;      /* 0x1694E */
    if (o->frame == 4 && o->count == 0 && v) {             /* 0x16936/0x16966 */
        eng_sound(0x2B);
        v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x15; v->dmg = 0x10; v->partner = -1;
        eng_sound(0x32);
        v->facing = o->facing ^ 0x8000u;
        carry_at(o, v, 0x30, -1, 0x28);
        o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x169B2 */
        o->off_x |= 0x8000u; o->state = ST_STAND;
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        add_pos_delta(o, 0x20, 0, 0); o->partner = -1;
    }
    return cell;
}

/* 0x16C9E — move 0x2F gorilla press (w1/w5 cat D, w5 cat C; record
 * 0x16C5E for w1, 0x16C7E for everyone else — mode 1 n 6). Entry: victim
 * hidden, slide 0x30 then 0x40, press loop count 2, announcer phrase
 * 0x26. Frame 1: hotspot y 0x30. Frame 3 end: loop cells 1-3 while the
 * count lasts; then drop: w5 (0x16D90) -> victim react 0x16 dmg 0x16 at
 * -0x10,0,+0x40; others -> react 0x26 dmg 0x16 at +0x20,1,+0x50, facing
 * flipped. Frame 4: step +0x20. End -> stand. TODO EXACT: 0x1129E roll
 * (fail 0x81), 0x110E0, 0x1108C, +0xC7. */
static uint32_t handler_gorilla(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (eng_ws_base(o->wrestler) != 1) cell = 0x16C7Eu;    /* 0x16E18 record swap */
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (v) v->state = ST_HELD;
        eng_slide_clip(o, 0x30);                           /* 0x16CB4 */
        if (!throw_roll(o, 0xF0)) { o->state = ST_MOVE; o->move_id = 0x81; return cell; }   /* 0x16CBE/0x16CD4 */
        eng_announce(cur_st, o->wrestler, 0x26);           /* 0x16CDE */
        o->grap44 = 2;                                     /* 0x16CEE press count */
        eng_slide_clip(o, 0x40);
        return cell;
    }
    if (o->frame == 1 && o->count == 0) o->off_y = 0x30;   /* 0x16D10 */
    if (o->frame == 3 && o->count == 0) {                  /* 0x16D28 */
        if (--o->grap44 != 0) { o->frame = 1; o->count = 0; }   /* 0x16D2E/0x16D34:
                                          frame/count only — the sequencer's
                                          borrow advances to cell 2, so the
                                          press loop is 2,3 (NOT a reload of
                                          cell 1, which showed an extra lift) */
        else if (v) {
            if (eng_ws_base(o->wrestler) == 5) {           /* 0x16D90 */
                o->off_x |= 0x8000u;
                v->state = ST_REACT; hit_count(v, 1); v->react_id = 0x2016; v->st_flags |= SF_SLAMMED; v->dmg = 0x16;   /* 0x16D9C + bset #5, 0x16DAE +1 */
                v->y -= 1 << 16; eng_sound(0x32);
                v->facing ^= 0x8000u; v->partner = -1;
                carry_at(o, v, -0x10, 0, 0x40);
            } else {                                       /* 0x16D44 */
                o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
                v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x2026; v->st_flags |= SF_SLAMMED; v->dmg = 0x16;   /* 0x16D5A + bset #5 */
                v->facing ^= 0x8000u; v->partner = -1;
                carry_at(o, v, 0x20, 1, 0x50);
            }
        }
    }
    if (o->frame == 4 && o->count == 0) { o->off_x |= 0x8000u; add_pos_delta(o, 0x20, 0, 0); }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_STAND; o->partner = -1; }  /* 0x16E02 */
    return cell;
}

/* 0x17C18 — move 0x3A gut wrench (w6 cat C; record 0x17BF8 mode 1 n 6
 * dur 8/8/8/10/C/10). Entry: victim hidden, announcer phrase 0x21, slide
 * 0x40. Frame 3: sound 0x2B. Frame 4: victim -> react 0x15 dmg 0x15 facing
 * away at +0x50,-1,+0x28. End -> state 7, spr 0x68, step +0x20.
 * TODO EXACT: 0x1108C, 0x110E0, +0xC7. */
static uint32_t handler_gutwrench(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (v) v->state = ST_HELD;
        eng_announce(cur_st, o->wrestler, 0x21);           /* 0x17C2A */
        eng_slide_clip(o, 0x40);                           /* 0x17C3E */
        return cell;
    }
    if (o->frame == 3 && o->count == 0) eng_sound(0x2B);   /* 0x17C56 */
    if (o->frame == 4 && o->count == 0 && v) {             /* 0x17C6E */
        v->state = ST_REACT; hit_count(v, 1); v->react_id = 0x2015; v->st_flags |= SF_SLAMMED; v->dmg = 0x15;   /* 0x17C74 + bset #5 */
        eng_sound(0x32); v->partner = -1;
        v->facing = o->facing ^ 0x8000u;
        carry_at(o, v, 0x50, -1, 0x28);
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x17CC6 */
        o->state = ST_GETUP; o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        add_pos_delta(o, 0x20, 0, 0); o->partner = -1;
    }
    return cell;
}

/* 0x17D12 — move 0x3B, Sgt Slaughter's TORTURE RACK (cat D; record
 * 0x17CF6 mode 1 n 5 dur 8/C/20/18/18 spr 0x19B-0x19F). Phases (+0x45 =
 * grap44 low byte, table 0x17D32):
 *  0 (0x17D3E) LIFT: victim frozen, step forward 0x20; frame 2 pops him
 *    into move 0x8D (launcher 0xC + two bounces onto the shoulders,
 *    latches his +0x60 b7), y-1, sfx 0x32, facing flip, carried at
 *    (-0x10,0,+0x40), f33 b6 clear both; FE -> phase 1 (+0x20 back).
 *  1 (0x17DDC) CARRY: on the victim's latch, walk (0x1174C speed) to
 *    (victim.x +/- 0x50 by his facing, victim.y) via 0x11710/0x1F15C;
 *    arrive -> phase 2.
 *  2 (0x17E50) THE RACK: snap onto the victim, announce (id, 0x24), sfx
 *    0x31, off_x 0x20, facing := his, engaged both; first latch: sfx
 *    0x32, the SUBMISSION referee cues (+0x35 b1 holder / b2 victim),
 *    victim -> move 0x62 (mash-out ladder -> 0x5A), react |= 0x2000,
 *    +0x32 b3, 0x215B6 run-in arm, step 0x40 back, victim.x := own,
 *    off_x 0xD0; record 0x801F while latched (a light hit breaks it,
 *    hold_hit); every FE loops frame 2 with victim dmg 5 + the 0x111C8
 *    KO test -> holder 0x6F (spr 0x4D) / victim KO drop 0x6E, cues off.
 *    (The 0x17DD4/0x17E48 movea.l-A1 loads are dead code.) */
static uint32_t handler_rack3B(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned ph = o->grap44 & 0xFFu;

    if (!v) { o->state = ST_STAND; return cell; }
    switch (ph) {
    case 0:
        if (!(o->anim_sel & 0x8000u)) {                /* 0x17D46 */
            o->mover = 0;
            v->state = ST_HELD;                         /* 0x17D4A */
            add_pos_delta(o, -0x20, 0, 0);             /* 0x17D50 forward */
            return cell;
        }
        if ((o->frame & 0xFFu) == 2 && o->count == 0) {   /* 0x17D62 */
            v->state = ST_MOVE; v->move_id = 0x8D;           /* 0x17D70 */
            v->y -= 1 << 16;                           /* 0x17D7C */
            eng_sound(0x32);                           /* 0x17D80 */
            v->facing ^= 0x8000u;                      /* 0x17D8A */
            carry_at(o, v, -0x10, 0, 0x40);            /* 0x17D90 0x10B9A */
            o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;   /* 0x17DA0 */
        }
        if ((o->frame & 0xFFu) == 0xFEu) {             /* 0x17DAC */
            o->state = ST_MOVE; o->grap44 = 1;               /* 0x17DBA +0x45++ */
            o->spr = o->facing;                        /* 0x17DC0 */
            add_pos_delta(o, 0x20, 0, 0);              /* 0x17DC6 back */
        }
        return cell;
    case 1:
        if (!(o->anim_sel & 0x8000u)) { o->mover = 0; o->spr = o->facing; }   /* 0x17DE4 */
        if (v->move_id & 0x8000u) {                    /* 0x17DEE his 0x8D landed */
            if (!(o->grap44 & 0x8000u)) {              /* 0x17DF6 one-shot */
                o->grap44 |= 0x8000u;
                o->mover = 1; o->speed = walk_speed_of(o);   /* 0x17DFE/0x1174C */
                o->run_tgt = (v->x >> 16) + ((v->facing & 0x8000u) ? -0x50 : 0x50);   /* 0x17E0A */
                o->tgt_y = v->y >> 16;                 /* 0x17E20 */
            }
            if (walk_to(o, o->run_tgt, o->tgt_y)) {    /* 0x17E26/0x1F15C */
                o->mover = 0;
                o->state = ST_MOVE; o->grap44 = 2;           /* 0x17E3A */
            }
        }
        return cell;
    default:                                           /* phase 2 */
        if (!(o->anim_sel & 0x8000u)) {                /* 0x17E58 */
            o->mover = 0;
            v->state = ST_HELD;                         /* 0x17E5C */
            eng_announce(cur_st, o->wrestler, 0x24);   /* 0x17E62 */
            eng_sound(0x31);                           /* 0x17E72 */
            o->x = v->x; o->y = v->y;                  /* 0x17E7C */
            o->off_x = 0x20;                           /* 0x17E88 */
            o->facing = v->facing;                     /* 0x17E8E */
            o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;          /* 0x17E94 */
            return cell;
        }
        if (!(o->move_id & 0x8000u)) {                 /* 0x17F42 first latch */
            o->move_id |= 0x8000u;
            eng_sound(0x32);                           /* 0x17F4A */
            o->cue_flags |= CF_SUB_HOLDER; v->cue_flags |= CF_SUB_VICTIM;          /* 0x17F54/0x17F5A cues */
            v->state = ST_MOVE; v->move_id = 0x62;           /* 0x17F60 rack'd (0xEBC4
                                                          ladder: mash-out 0x5A) */
            v->react_id |= 0x2000u;                    /* 0x17F6C */
            v->st_flags |= SF_SLAMMED;                           /* 0x17F72 */
            eng_tag_arm_hold(cur_st, o, v);            /* 0x17F78 0x215B6 */
            add_pos_delta(o, 0x40, 0, 0);              /* 0x17F7E back */
            v->x = o->x;                               /* 0x17F8C */
            o->off_x = 0x00D0;                         /* 0x17F92 */
        }
        o->atk = 0x801F;                               /* 0x17EAC record 0x1F */
        if ((o->frame & 0xFFu) == 0xFEu && !o->result) {   /* 0x17EB2/0x17EBC */
            o->frame = 2; o->count = 0;                /* 0x17EC4 loop the strain */
            v->dmg = 5;                                /* 0x17ECE */
            if (ko_check(o, v)) {                      /* 0x17ED4 0x111C8 */
                o->off_x |= 0x8000u;                   /* 0x17EDC */
                o->state = ST_MOVE; o->move_id = 0x6F;       /* 0x17F04 */
                o->spr = (uint16_t)(0x4D | (o->facing & 0x8000u));   /* 0x17F10 */
                v->state = ST_MOVE; v->move_id = 0x6E;       /* 0x17F1C KO drop */
                o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;   /* 0x17F28 */
                o->cue_flags &= (uint16_t)~0x02u; v->cue_flags &= (uint16_t)~0x04u;   /* 0x17F34 */
            }
        }
        return cell;
    }
}

/* 0x17FB2 — move 0x8D: the rack-lift flight (record 0x17F9A mode 1 n 4
 * dur 6/FF00/FF00/FF00 spr 0xB2/0xB3/0x1F/0x12): popped up (launcher
 * 0xC), first landing -> thud + slide 0x50 + relaunch (preset 3), second
 * landing -> thud, mover off, LATCH +0x60 b7 — the rack's phase 1 walks
 * on that signal. The FF00 frames release on landing. */
static uint32_t handler_racklift8D(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { knockback(o, 0x0C); return cell; }   /* 0x17FBA */
    rope_contact(o);                                   /* 0x17FC8 0x10FC6 */
    if ((o->frame & 0xFFu) == 1 && o->landed && !(o->move_id & 0x8000u)) {   /* 0x17FCE */
        eng_sound(0x29);                               /* 0x17FDE 0x1110E */
        eng_slide_clip(o, 0x50);                       /* 0x17FE4 0x10B62 */
        o->count = 0;                                  /* 0x17FEE */
        knockback(o, 3);                               /* 0x17FF2 */
        return cell;
    }
    if ((o->frame & 0xFFu) == 2 && o->landed && !(o->move_id & 0x8000u)) {   /* 0x17FFC */
        eng_sound(0x29);                               /* 0x1800C */
        o->mover = 0; o->count = 0;                    /* 0x18012 */
        o->move_id |= 0x8000u;                         /* 0x1801A latch */
    }
    return cell;
}

/* 0x17194 — moves 0x32 (record 0x1716C dur 0x18/8/0x14, w3/w6 cat A-B)
 * and 0x42 (record 0x17180 dur 8/C/4, w8 cat B): three punches from the
 * hold. Entry (first time only, move b7): loop count 3, victim hidden.
 * Frame 0 end: punch sound 0x2A. Frame 1 end: victim dmg 4 (5 for 0x32);
 * while the count lasts the record restarts (+0x1C = 5); on the last
 * pass 0x32 drops the victim right there (react 2 fall at -0x10,-1,0,
 * hotspot y 0x20) while 0x42 drops him at 0xFE. End -> stand.
 * TODO EXACT: 0x110E0. */
static void punches_drop(eng_obj *o, eng_obj *v)
{
    v->state = ST_REACT; v->react_id = RC_FALL_HIGH; v->partner = -1;
    carry_at(o, v, -0x10, -1, 0);
    o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
}
static uint32_t handler_punches(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    int is32 = (o->move_id & 0xFFu) == 0x32;
    if (!(o->anim_sel & 0x8000u)) {
        if (!(o->move_id & 0x8000u)) {                     /* 0x1719C */
            o->move_id |= 0x8000u; o->mover = 0; o->grap44 = 3;
            if (v) v->state = ST_HELD;
        }
        return cell;
    }
    if (o->frame == 0 && o->count == 0) eng_sound(0x2A);   /* 0x171C6 */
    if (o->frame == 1 && o->count == 0) {                  /* 0x171D0 */
        if (v) v->dmg = (uint16_t)(is32 ? 5 : 4);
        if (--o->grap44 != 0) { o->anim_sel = 5; return cell; } /* 0x171F8 restart */
        if (is32 && v) { o->off_y = 0x20; punches_drop(o, v); }
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x17210 */
        o->off_x |= 0x8000u; o->state = ST_STAND; o->spr = o->facing;
        if (!is32 && v && (v->state & 0xFFu) == 0xFFu) punches_drop(o, v);
        o->partner = -1;
    }
    return cell;
}

/* 0x18668 — move 0x44 (w11 cat D; record 0x1863C mode 1 n 6 dur 10/10/A/
 * A/A/18, alt record 0x1865C when the victim is w8). Entry: hotspot y
 * 0x50, victim hidden, announcer phrase 0x10. Frame 4: sound 0x2B, victim
 * -> react 0x25 dmg 0x18 facing away at 0,-1,+0x20, spr 0x1F. End ->
 * stand. TODO EXACT: 0x1129E roll (fail 0x82), +0xC7, 0x1108C. */
static uint32_t handler_throw44(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (v && eng_ws_base(v->wrestler) == 8) cell = 0x1865Cu;   /* 0x186B4 */
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->off_y = 0x50;
        if (v) v->state = ST_HELD;
        if (!throw_roll(o, 0xF2)) { o->state = ST_MOVE; o->move_id = 0x82; return cell; }   /* 0x18684/0x1869A */
        eng_announce(cur_st, o->wrestler, 0x10);           /* 0x186A4 */
        return cell;
    }
    if (o->frame == 4 && o->count == 0 && v) {             /* 0x186C8 */
        eng_sound(0x2B);
        v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x2005; v->st_flags |= SF_SLAMMED; v->dmg = 0x18;   /* 0x186E6/0x186EC: record 5 + flag */
        v->facing = o->facing ^ 0x8000u; v->partner = -1;
        eng_sound(0x32);
        carry_at(o, v, 0, -1, 0x20);
        o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
        o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x1874A */
        o->off_x |= 0x8000u; o->state = ST_STAND; o->spr = o->facing; o->partner = -1;
    }
    return cell;
}

/* 0x12B16 — move 0x03 flying shoulder / clothesline (w2/w11 cat 3-4;
 * record 0x12AE6, variant record 0x12AFE once it connects). Per frame:
 * record 3 armed (0x12 for frames 0-1); a runner we tagged with react
 * 0x11 who tagged us back -> connect: variant, restart on 0x12AFE, link.
 * Variant frame 1 end: victim -> react 0x19 (spr 0xB3) carried at
 * -0x10,-1,+0x28. End -> stand. */
static uint32_t handler_move03(eng_obj *o, uint32_t cell)
{
    eng_obj *v = last_hit_obj(o);
    if (o->move_id & 0x8000u) cell = 0x12AFEu;             /* 0x12BD2 */
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; o->last_pair = -1; return cell; }
    if (!(o->move_id & 0x8000u)) {
        o->atk = 3;                                        /* 0x12B34 */
        if ((o->frame & 0xFFu) < 2) {
            o->atk = 0x12;
            if (v && (v->react_id & 0xFFu) == 0x11 && v->last_pair == self_idx(o)) {
                o->move_id |= 0x8000u;                     /* 0x12B62 */
                o->partner = o->last_pair;
                o->anim_sel = 5;
                return 0x12AFEu;
            }
        }
    } else if ((o->frame & 0xFFu) == 1 && o->count == 0 && o->partner >= 0 && cur_st) {
        eng_obj *p = &cur_st->obj[o->partner];            /* 0x12B8C */
        p->state = ST_REACT; p->react_id = 0x19;
        p->spr = (uint16_t)(0xB3 | (o->facing & 0x8000u));
        carry_at(o, p, -0x10, -1, 0x28);
    }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_STAND; o->partner = -1; }
    return cell;
}

/* 0x13116 — move 0x07 charging attack (w7; record 0x130E6, 0x130FE for
 * everyone else): record 1 armed, 0x1C on frame 1; end -> stand. */
static uint32_t handler_move07(eng_obj *o, uint32_t cell)
{
    if (eng_ws_base(o->wrestler) != 7) cell = 0x130FEu;    /* 0x13164 */
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; return cell; }
    o->atk = 1;                                            /* 0x13124 */
    if (o->frame == 1) o->atk = 0x1C;                      /* 0x13142 */
    if ((o->frame & 0xFFu) == 0xFEu) o->state = ST_STAND;
    return cell;
}

/* 0x1639C — move 0x27 charging attack (w6/w10 cat 4; record 0x16388 mode
 * 1 n 3 dur C/C/20): record 0x10 armed; a victim we put in react 0x14
 * -> connect: variant, hidden (spr 0xFFFF) on frame 1 for 0xC frames, then
 * stand. End -> stand. */
static uint32_t handler_move27(eng_obj *o, uint32_t cell)
{
    eng_obj *v = last_hit_obj(o);
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; o->last_pair = -1; return cell; }
    if (!(o->move_id & 0x8000u)) {
        o->atk = 0x10;                                     /* 0x163B8 */
        if (v && (v->state & 0xFFu) == ST_REACT && (v->react_id & 0xFFu) == 0x14) {
            o->move_id |= 0x8000u;                         /* 0x163D8 */
            o->frame = 1; o->spr_force = 0xFFFF; o->count = 0x0C;
            return cell;
        }
    } else if (o->frame == 1 && o->count == 0) {
        o->state = ST_STAND;                                      /* 0x16410 */
    }
    if ((o->frame & 0xFFu) == 0xFEu) o->state = ST_STAND;
    return cell;
}

/* 0x174AA — move 0x34 (Jake cat D, record 0x1747E mode 1 n 9 dur
 * 8/9/A/8/8/FF00/10/10/28 cells 0x90,0x91,0x138-0x13D,0x68). Entry hides
 * the victim; frame 3 -> attacker knockback 0x19; landing releases the
 * FF00 hold (frame 5); frame 6 drops the victim (react 0x23 dmg 0x1C at
 * +0x40,-1,+0x40); end -> stand. TODO EXACT: 0x1129E roll (fail -> 0x86),
 * announcer, 0x110E0, victim +0xC7 += 5, $1C1800 = 0xE. */
static uint32_t handler_jakedrop(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (v) v->state = ST_HELD;                          /* 0x174B6 */
        if (!throw_roll(o, 0xFA)) { o->state = ST_MOVE; o->move_id = 0x86; return cell; }   /* 0x174C0/0x174D6 */
        eng_announce(cur_st, o->wrestler, 0x1C);           /* 0x174E8 DDT */
        return cell;
    }
    if (o->frame == 3 && o->count == 0) knockback(o, 0x19); /* 0x17502 */
    if (o->landed) {                                       /* 0x17510 */
        eng_sound(0x29); o->mover = 0; o->count = 0;
    }
    if (o->frame == 6 && o->count == 0 && v) {             /* 0x1752E */
        v->state = ST_REACT; hit_count(v, 5); v->react_id = 0x2023; v->st_flags |= SF_SLAMMED;   /* 0x17542 + bset #5 */
        v->dmg = 0x1C; eng_sound(0x32); v->partner = -1;
        carry_at(o, v, 0x40, -1, 0x40);                    /* 0x10B9A */
        o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x1758C */
        o->state = ST_STAND; o->spr = o->facing; o->partner = -1;
    }
    return cell;
}

/* 0x16A16 — move 0x2D running attack: record 0x0B armed the whole way,
 * speed decays -2/frame until it ends. */
static uint32_t handler_runatk(eng_obj *o, uint32_t cell)
{
    if (o->zone == 3) {                                /* 0x11CE0 barrier */
        o->dmg = 0x0A; eng_sound(0x28);
        o->state = ST_REACT; o->react_id = 0x17; o->grap44 = 0;
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u)) {           /* 0x10FC6 carry */
        o->state = ST_TURN;
        return cell;
    }
    if (!(o->anim_sel & 0x8000u) && (o->move_id & 0xFFu) == 0x2Du)
        eng_announce(cur_st, o->wrestler, 0x13);           /* 0x16B7A "POWERSLAM!" */
    o->atk = 0x0B;
    o->speed -= 2;
    if ((int16_t)o->speed < 0) {
        o->state = ST_STAND;
        add_pos_delta(o, 8, 0, 0);
        o->partner = -1;
    }
    return cell;
}

/* 0x15B34 — move 0x21 anti-run swing: record 8 on frame 1 only. */
static uint32_t handler_swing(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u))
        o->mover = 0;
    o->atk = (o->frame == 1) ? 8 : 1;
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND;
        o->partner = -1;
    }
    return cell;
}

/* 0x13744 — move 0x0A stomp: handler-applied damage at the end of
 * frame 0 on a lying victim (react 8/9 -> lying-hit 6/7). The move-break
 * ladder (0x8048/0x800E/...) arrives with the carries. */
static uint32_t handler_stomp(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        return cell;
    }
    o->atk = 1;
    if (o->frame == 0 && o->count == 0 && v && v->result == 0
        && eng_pin_is_pinner(v)) {
        /* PIN BREAK — the stomp lands on the pile. Handler-applied
         * damage (this move never goes through 0x24062), so run the
         * reaction-9 body (0x24BC2/0x24C0C) directly. */
        eng_obj *pv = &cur_st->obj[v->partner];
        v->dmg = 2;
        eng_sound(0x32); eng_sound(0x2A);
        eng_pin_break(cur_st, v, pv);
        o->partner = -1;
    } else if (o->frame == 0 && o->count == 0 && v && v->result == 0
        && (v->state & 0xFFu) == ST_MOVE && (v->move_id & 0xFFu) == 0x61) {
        /* 0x1398C: the HELD man (0x8061, the buckle double team): 0x24900
         * (the announcer, unless he is my own partner) and his move 0x77 =
         * the 0x18-tick flinch with the holder frozen. ENGINE (dt_stomp
         * mod): 2 damage + the stomp sounds, as on a lying man - the ROM's
         * case applies no damage. */
        if (v->teammate != self_idx(o)) eng_announce(cur_st, 0x0F, 0x19);
        v->dmg = 2;
        eng_sound(0x32); eng_sound(0x2A);
        v->state = ST_MOVE; v->move_id = 0x77; v->frame = 0; v->anim_sel = 0;   /* +0x44 (his 0xAA escape countdown)
                                          is NOT touched (0x1398C): it pauses through the flinch and resumes,
                                          so stomps delay the escape but never reset it - a stomp that zeroed
                                          it made the hold endless (user 2026-08-30) */
    } else if (o->frame == 0 && o->count == 0 && v && v->result == 0
        && (v->state & 0xFFu) == ST_REACT) {
        unsigned rl = v->react_id & 0xFFu;
        if (rl == 8 || rl == 9) {
            v->state = ST_REACT;
            v->react_id = (uint16_t)(rl - 8 + 6);      /* lying-hit band */
            v->dmg = 2;
            eng_sound(0x32); eng_sound(0x2A);
        }
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND;
        o->partner = -1;
    }
    return cell;
}

/* 0x141EC — move 0x10 leap attack onto a downed victim. */
static uint32_t handler_leap(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u) && !(o->move_id & 0x8000u))
        eng_announce(cur_st, 0x0F, 0x1A);                  /* 0x14414 */

    if (o->move_id & 0x8000u) {
        if ((o->frame & 0xFFu) == 0xFEu) {
            o->off_x |= 0x8000u;
            o->state = ST_GETUP;
            add_pos_delta(o, 0x18, -1, 0);
            o->partner = -1;
        }
        return cell;
    }
    if (!(o->anim_sel & 0x8000u)) {
        if (v) {
            o->x = v->x + ((v->facing & 0x8000u) ? -(0x38 << 16) : (0x38 << 16));
            o->facing = (uint16_t)(((o->x < v->x) ? 0 : 0x8000u));
            o->y = v->y; o->z = v->z;
            add_pos_delta(o, 0x10, 0, 0x10);
            o->vz = 0x300; o->grav = 0x48;
            homing_launch(o, v, ((v->react_id & 0xFFu) == RC_LYING) ? 0x0Au : 0x0Bu);
            o->z += 0x40 << 16;
            o->off_y = 0x10;
        }
        return cell;
    }
    if (o->frame == 1) {
        o->floor42 = -0x20;
        if (!o->landed)
            return cell;
        o->floor42 = 0;
        eng_sound(0x29);
        o->mover = 0;
        if (v && (v->state & 0xFFu) == ST_REACT
            && ((v->react_id & 0xFFu) == RC_LYING || (v->react_id & 0xFFu) == 9)
            && eng_prox_box(o, v, 0x0A)) {             /* 0x11152 */
            o->count = 0;
            o->move_id |= 0x8000u;
            o->y = v->y - (1 << 16);
            o->off_y = 0x0C;
            v->state = ST_REACT; hit_count(v, 1);             /* 0x1432C (TODO EXACT: the 0x14394 react-0x77 branch) */
            v->hitctr_d2[0]++;                         /* 0x14338 addq +0xD2 (the leap) */
            v->react_id = (uint16_t)((v->react_id & 0xFFu) - 8 + 6);
            v->dmg = 0x0A;
            eng_sound(0x2B); eng_sound(0x32);
        } else {                                       /* MISS: faceplant */
            o->state = ST_REACT;
            o->react_id = RC_BOUNCE;
            add_pos_delta(o, 0x50, -1, 0);
            o->facing ^= 0x8000u;
            o->dmg = 3;
            eng_sound(0x32);
            o->partner = -1;
        }
    }
    return cell;
}

/* 0x132F4 — move 0x48: the COVER of a downed opponent (dive onto the
 * body, lift). SIMPLIFIED end state: the ROM converts the pair into the
 * scripted carry (victim move 0x4A phases -> follow-up ladder); until
 * that ladder is transcribed the catch goes straight to the hold
 * (0x0C/0xFF pair) — the user-described "stand up into a grapple". */
static uint32_t handler_cover(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (o->move_id & 0x8000u) {        /* 0xC048 from the squash end (0x18CB4):
                                          latched cover under a splash pin — hidden,
                                          the victim's 0x4A composite draws him */
        o->spr = 0xFFFF; o->mover = 0;
        return cell;
    }
    if (!(o->anim_sel & 0x8000u)) {
        if (!v) { o->state = ST_STAND; return cell; }
        o->mover = 0;                                  /* 0x13306 */
        o->vz = 0x480; o->grav = 0x48;                 /* 0x1330A/0x13310 */
        homing_launch(o, v, 1);                        /* dx +0x30 dy -0x20 */
        o->mover = 0;                                  /* flight starts later */
        o->facing = (uint16_t)((o->vx < 0) ? 0 : 0x8000u);
        v->down_t = 0x200;             /* hold the body down (0x175FC family) */
        return cell;
    }
    if (o->frame == 0 && o->count == 0) o->mover = 2;  /* 0x1334A: take off at
                                          the end of cell 0 (the 6-tick crouch) */
    if ((o->frame & 0xFFu) < 4)
        return cell;                                   /* 0x1335C: +0x42 untouched
                                          through the arc */
    o->floor42 = -0x20;                                /* 0x13366 */
    if (!o->landed)
        return cell;
    o->floor42 = 0;
    if (v && (v->state & 0xFFu) == ST_REACT
        && ((v->react_id & 0xFFu) == RC_LYING || (v->react_id & 0xFFu) == 9)) {
        o->mover = 0;                                  /* CATCH 0x13392 */
        o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;
        o->facing = v->facing;
        o->x = v->x; o->y = v->y; o->z = v->z;
        /* Legal-pin gate, modelled on the squash's 0x18C68-0x18C86 (match
         * live, both men legal, attacker not outside): a partner covering
         * at ringside (or anyone outside) gets no pin — he rises and the
         * victim stays down (0x18CC6 shape). TODO EXACT: the 0x48 catch
         * itself sets the referee cue unconditionally (0x133A2) and the
         * hunt 0x20556 takes any cue; stock's refusal point is unconfirmed
         * (user 2026-08-23: CPU partner pinned a partner outside). */
        if (!eng_pin_allowed(o, v)) {
            /* rumble: the plain cover pins anyone (0x133A2 cue unconditional;
             * CPUs are never "legal" there) — the count eliminates (0x200D4) */
            if (eng_dbgsel) fprintf(stderr, "cover: CATCH but not a legal pin (0x18CC6)\n");
            o->state = ST_GETUP; o->partner = -1; o->grap44 = 0;
            v->state = ST_REACT; v->react_id = (uint16_t)(v->react_id & 0xFF00u);
            v->down_t = 0; v->partner = -1;
            return cell;
        }
        add_pos_delta(o, 0x40, -8, 0);
        if (eng_dbgsel)
            fprintf(stderr, "cover: CATCH -> pin\n");
        o->state = 0x000C;                             /* the pin IS state 0x0C;
                                          TODO EXACT 0x0C-as-pin machinery */
        o->grap44 = 0x2000;
        o->pinning = 1;
        o->spr_force = 0xFFFF;         /* hidden from THIS frame — no one-
                                          frame flash of cell 1C at the
                                          snapped position */
        v->mash_aa = v->hp == 0 ? 0x100
                   : (v->hp < 0x2A ? (uint16_t)(0x2A - v->hp) : 1);
        v->down_t = 0; v->combo = 0; v->combo_t = 0;
        v->state = ST_HELD;
        v->grap44 = 0;                 /* the victim stays put (0x133C0:
                                          only the pinner steps +/-0x40,-8) */
        v->partner = (int)(o - cur_st->obj);
        eng_tag_arm_pin(cur_st, o, v); /* 0x133E2 jsr 0x215B6 (my own partner)
                                          + 0x133EA jsr 0x21732 (his partner):
                                          BOTH teams send a man in */
    } else {                                           /* MISS 0x13400 */
        o->state = ST_REACT;
        o->react_id = RC_BOUNCE;
        o->dmg = 3;
        o->partner = -1;
        if (v && v->partner == self_idx(o)) v->partner = -1;
    }
    return cell;
}

/* 0x11412 — link test: carry SET when partner->partner != self. */
static int link_ok(const eng_obj *o, const eng_obj *v)
{ return v && v->partner == self_idx(o); }

/* 0x111C8 — KO check inside a hold (submissions.md §1c): match live,
 * holder in the ring, both legal, victim hp exactly 0, once per move
 * (bset #3 of the move-id high byte). Fires the result words and the
 * referee's win (SIMPLIFIED: $1C1214/$1C169E/0x90D6 bookkeeping is the
 * engine's ref.sm = 9). Returns 1 when the KO fired (carry set). */
static int ko_check(eng_obj *o, eng_obj *v)
{
    if ((o->role & RF_OUTSIDE) || !(o->role & RF_LEGAL) || !(v->role & RF_LEGAL)) return 0;
    if (v->hp != 0) return 0;                          /* 0x111F8 exact == 0 */
    if (o->move_id & 0x0800u) return 0;                /* 0x11200 once */
    o->move_id |= 0x0800u;
    if (cur_st && (cur_st->g161 & 1u)) {               /* 0x1120A RUMBLE: no result — the KO
                                                          victim is eliminated (0x1ACA0-0x1ACB0
                                                          at the KO handler) and walks off */
        v->st_flags |= SF_ELIMINATED;
        eng_announce(cur_st, (unsigned)v->wrestler, 0x29);
        return 1;                                      /* +0xC4++ / 0x21358 TODO */
    }
    o->result = 0x8000; v->result = 0x8001;            /* 0x1120A */
    if (cur_st) {                                      /* 0x11236..0x11250: the partners too */
        if (o->teammate >= 0 && o->teammate < ENG_MAX_OBJS)
            cur_st->obj[o->teammate].result = 0x8000;
        if (v->teammate >= 0 && v->teammate < ENG_MAX_OBJS)
            cur_st->obj[v->teammate].result = 0x8001;
        cur_st->ref.sm = 9, cur_st->ref.win_t = 44;
    }
    eng_sound(0x32); eng_sound(0x32);
    if (!o->cpu) eng_blit(0x26);                       /* 0x1127C: "GIVE UP" (human winner) */
    return 1;
}

/* 0x15B8C (move 0x22 mounted punches, victim move 0x5F, loop frame 3,
 * announcer 0x0A, record 0x14 while looping) and 0x135EC (move 0x09
 * ground hold — Jake/Perfect/DiBiase cat 5 both buttons; victim 0x5D,
 * loop frame 4, announcer 0x09, no hittable record): byte-for-byte the
 * same holder (submissions.md §1a/§1b). Victim frozen at the end of
 * frame 0, the loop deals 3 per pass; the FIRST loop writes the victim
 * into his scripted held move; release only on KO or a match result.
 * TODO EXACT: rumble KO branch, victim +0xC7 in 0x09. */
static uint32_t hold_holder(eng_obj *o, uint32_t cell, unsigned vm,
                            unsigned loopf, unsigned ann, int is09)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        if (!v || (v->state & 0xFFu) != ST_REACT
            || (v->react_id & 0xFFu) != RC_LYING) {
            o->state = ST_STAND;
            return cell;
        }
        o->mover = 0;
        o->x = v->x; o->y = v->y;
        o->facing = v->facing;
        return cell;
    }
    if (o->frame == 0 && o->count == 0) {              /* 0x15BAE: end of frame 0 */
        if (v && (v->state & 0xFFu) == ST_REACT && (v->react_id & 0xFFu) == RC_LYING) {
            o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;          /* hide the victim */
            o->cue_flags |= CF_SUB_HOLDER; v->cue_flags |= CF_SUB_VICTIM;          /* referee: watch */
            eng_announce(cur_st, o->wrestler, ann);    /* 0x15BF0 / 0x13650 */
            if (is09) hit_count(v, 1);                 /* 0x1364A */
            v->state = ST_HELD;
            v->partner = self_idx(o);
        } else {                                       /* ABORT: he got up */
            o->state = ST_STAND; o->spr = o->facing; o->count = 1;
            o->partner = -1;
            return cell;
        }
    }
    if (!is09) o->atk = (o->move_id & 0x8000u) ? 0x8014u : 0;   /* 0x15BF6 */
    if ((o->frame & 0xFFu) != 0xFEu || !v) return cell;
    /* a successful escape overwrote us into 0xFF/4 — never reached here */
    if (!(o->result & 0x8000u)) {
        o->frame = (uint16_t)loopf;                    /* LOOP the last two cells */
        o->count = 0;
        v->dmg = 3;
        if (!ko_check(o, v)) {                         /* 0x111F8 no KO */
            if (!(o->move_id & 0x8000u)) {             /* 0x15CBA / 0x1370E once */
                o->move_id |= 0x8000u;
                v->state = ST_MOVE; v->move_id = (uint16_t)vm;   /* scripted held man */
                eng_tag_arm_pin(cur_st, o, v);         /* 0x215B6 (+ the victim
                                                          move's own 0x21732) */
                eng_sound(0x32);
            }
            return cell;
        }
    }
    o->state = ST_GETUP;                                      /* release 0x15C8E / 0x136E2 */
    v->state = ST_REACT; v->react_id &= 0xFFu;
    o->cue_flags &= (uint16_t)~0x02u; v->cue_flags &= (uint16_t)~0x04u;
    o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
    return cell;
}
static uint32_t handler_mount(eng_obj *o, uint32_t cell)
{ return hold_holder(o, cell, 0x5F, 3, 0x0A, 0); }
static uint32_t handler_groundhold(eng_obj *o, uint32_t cell)
{ return hold_holder(o, cell, 0x5D, 4, 0x09, 1); }

/* 0x1612E — move 0x25 BEARHUG (Earthquake cat D; record 0x16116 mode 1
 * n 4 dur 0xC/0xC/0x10/0x18 spr 0xF8/0x124-0x126). Entry: victim hidden
 * at once, slide -0x18 off the rope line, announcer 0x1F. End of frame 0
 * -> hotspot y 0x20; record 0x1E hittable while looping. FE: a match
 * result releases (0x1618E: stand, victim react 2 facing flipped at
 * +0x30,-1,+0x30); else loop frames 1-3 dealing 5, KO -> own move 0x8A
 * (the drop) — and the first loop (which the KO path also falls into,
 * 0x161FE) writes the victim into held move 0x60 with the holder/held
 * referee bits. TODO EXACT: victim +0xC7. */
static uint32_t handler_bearhug(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        eng_slide_clip(o, -0x18);                          /* 0x1613A */
        if (v) { v->state = ST_HELD; v->partner = self_idx(o); }
        eng_announce(cur_st, o->wrestler, 0x1F);
        return cell;
    }
    if ((o->frame & 0xFFu) == 0 && o->count == 0) o->off_y = 0x20;
    if (o->move_id & 0x8000u) o->atk = 0x801Eu;            /* 0x16176 */
    if ((o->frame & 0xFFu) != 0xFEu || !v) return cell;
    if (o->result & 0x8000u) {                             /* 0x1618E release */
        o->off_x |= 0x8000u; o->state = ST_STAND; o->spr = (uint16_t)(o->facing & 0x8000u);
        v->state = ST_REACT; v->react_id = RC_FALL_HIGH;
        v->facing = o->facing ^ 0x8000u;
        carry_at(o, v, 0x30, -1, 0x30);
        o->partner = -1;
        return cell;
    }
    o->frame = 1; o->count = 0;                            /* 0x161D4 loop */
    v->dmg = 5;
    if (ko_check(o, v)) {                                  /* 0x161EC */
        o->state = ST_MOVE; o->move_id = 0x8A; o->off_x |= 0x8000u;
    }
    if (!(o->move_id & 0x8000u)) {                         /* 0x161FE once */
        o->move_id |= 0x8000u;
        v->state = ST_MOVE; v->move_id = 0x60;
        o->cue_flags |= CF_SUB_HOLDER; v->cue_flags |= CF_SUB_VICTIM;
        v->react_id |= 0x2000u; v->st_flags |= SF_SLAMMED;
        eng_tag_arm_pin(cur_st, o, v);                     /* 0x215B6 */
        eng_sound(0x32);
    }
    return cell;
}

/* 0x1792A — move 0x36 MILLION DOLLAR DREAM (DiBiase cat D from the
 * stance; record 0x17912 mode 1 n 4 dur 8/0x10/0x14/0x14 spr 0xF8/
 * 0x12B-0x12D). Entry needs the link and a victim still in the headlock
 * move 0x7B or already hidden, else it aborts to stand drawing the
 * hidden cell (0x1797A). Victim hidden at once, announcer 6, holder/
 * held bits at entry. Record 0x15 hittable while looping. FE: result ->
 * own move 0x6F, victim stands; else loop frames 1-3 dealing 5, KO ->
 * the 0x6F/0x6E pair with the victim turned round; first loop -> victim
 * move 0x63. */
static uint32_t handler_mdd(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        if (!link_ok(o, v) || !((v->move_id & 0xFFu) == 0x7B || (v->state & 0xFFu) == ST_HELD)) {
            o->partner = -1; o->state = ST_STAND; o->spr = 0xFFFF;  /* 0x1797A: cell 0x125C0 */
            return cell;
        }
        o->mover = 0;
        v->facing = o->facing;                             /* 0x1794E */
        eng_announce(cur_st, o->wrestler, 0x06);
        v->state = ST_HELD;
        o->cue_flags |= CF_SUB_HOLDER; v->cue_flags |= CF_SUB_VICTIM;
        return cell;
    }
    o->atk = (o->move_id & 0x8000u) ? 0x8015u : 0;         /* 0x17990 */
    if ((o->frame & 0xFFu) != 0xFEu || !v) return cell;
    if (o->result & 0x8000u) {                             /* 0x17A26 */
        o->state = ST_MOVE; o->move_id = 0x6F; v->state = ST_STAND;
        eng_tag_pin_end(cur_st, o, v);                     /* 0x212A0 */
        return cell;
    }
    o->frame = 1; o->count = 0;                            /* 0x179B6 loop */
    v->dmg = 5;
    if (ko_check(o, v)) {                                  /* 0x179CE */
        o->state = ST_MOVE; o->move_id = 0x6F;
        v->state = ST_MOVE; v->move_id = 0x6E; v->facing ^= 0x8000u;
        eng_tag_pin_end(cur_st, o, v);
        return cell;
    }
    if (!(o->move_id & 0x8000u)) {                         /* 0x179F4 once */
        o->move_id |= 0x8000u;
        v->state = ST_MOVE; v->move_id = 0x63;
        v->react_id |= 0x2000u; v->st_flags |= SF_SLAMMED;
        eng_tag_arm_pin(cur_st, o, v);
        eng_sound(0x32);
    }
    return cell;
}

/* 0x15228 — move 0x1A PERFECT-PLEX (Perfect cat D; record 0x1520C mode
 * 1 n 5 dur 0xC/8/0xA/0xA/0xC spr 0x144-0x148): the suplex that bridges
 * into a cover. Entry hides the victim, hotspot 0x20/0x60, announcer
 * 0x23. Frame 4 end: thud, hotspot x 0x28. FE: victim dmg 0x16; in the
 * ring with both men legal -> latch: victim into pinned move 0x5E, PIN
 * role bit, 0x215B6; else 0x15336 release: victim react 6 behind at
 * -0xA0,+1, own facing flipped, state 7. Latched: a result or the
 * victim's forced-rise bit (f32 b4) ends the cover the same way after
 * the "KO" phrase 0x29. TODO EXACT: the 0x1129E entry-1 roll at entry
 * (fail -> fumble move 0x87, untranscribed) is skipped — always lands. */
static uint32_t handler_plex(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (o->move_id & 0x8000u) {                            /* 0x15310 pinned */
        if (!v) return cell;
        if (!(o->result & 0x8000u) && !(v->st_flags & SF_ELIMINATED)) return cell;
        eng_announce(cur_st, v->wrestler, 0x29);
        o->cue_flags &= (uint16_t)~0x01u; o->pinning = 0;
        goto release;
    }
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->off_x = 0x20; o->off_y = 0x60;
        if (v) { v->state = ST_HELD; v->partner = self_idx(o); }
        if (!throw_roll(o, 0xFC)) { o->state = ST_MOVE; o->move_id = 0x87; return cell; }   /* 0x15254/0x1526A */
        eng_announce(cur_st, o->wrestler, 0x23);           /* 0x15274 */
        return cell;
    }
    if ((o->frame & 0xFFu) == 4 && o->count == 0) {        /* 0x15296 */
        eng_sound(0x29); o->off_x = 0x28;
    }
    if ((o->frame & 0xFFu) != 0xFEu || !v) return cell;
    v->dmg = 0x16; eng_sound(0x32);                        /* 0x152B4 */
    if (!(o->role & RF_OUTSIDE) && (o->role & RF_LEGAL) && (v->role & RF_LEGAL)) {
        o->move_id |= 0x8000u;                             /* 0x152DC */
        v->state = ST_MOVE; v->move_id = 0x5E; v->dmg = 0x16;
        o->cue_flags |= CF_PIN_CUE; o->pinning = 1;
        o->atk = 0x801Du;              /* record 0x1D through the pin frames (0x24BC2) */
        v->react_id |= 0x2000u; v->st_flags |= SF_SLAMMED;
        eng_tag_arm_pin(cur_st, o, v);                     /* 0x215B6 */
        return cell;
    }
release:                                                   /* 0x15336 */
    o->off_x |= 0x8000u; o->state = ST_GETUP;
    v->state = ST_REACT; v->react_id = 6;
    o->facing ^= 0x8000u;
    carry_at(o, v, -0xA0, 1, 0);
    o->partner = -1;
    o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
    v->react_id |= 0x2000u; v->st_flags |= SF_SLAMMED;
    return cell;
}

/* 0x15D08 — move 0x23 (Earthquake cat 7 B1 on a downed man): the
 * rope-run sit-down splash, phased by grap44 & 0xF (0x15D20 table):
 *   0  0x15D44 face the victim, pin him down (mash 0x1000, down_t 0x1000),
 *      sound 0x31; draws record 0x1A24A; end -> phase 1 (8 in the
 *      ringside scene).
 *  1-4 0x15DB8 hops toward the victim (homing 0x26AE idx 5+phase when
 *      facing him, 10-phase otherwise; vz 0x500 grav 0x48); a rope
 *      contact jumps to phase 5.
 *  5-7 the rope-run family 0x175FC/0x176C4/0x1770C.
 *   8  0x15E42 the splash: homing idx 2 vz 0x600; move-id b4 while
 *      rising; landing -> thud; link + lying-box test (0x11152) -> HIT:
 *      announcer 0x25, and with both men legal in the ring the COVER:
 *      record 0x1D, PIN role, victim into pinned move 0x64 dmg 0x14,
 *      self at victim +0x30,-1 (mirrored by HIS facing), 0x215B6;
 *      otherwise victim react 7 dmg 0x14 (0x10F56 down time) and 0x14
 *      ticks. MISS -> faceplant react 5 facing flipped, victim dmg 3.
 *      Draws record 0x15CF0. Pinned: forced-rise (f32 b4, phrase 0x29)
 *      or a result releases (state 7, victim react 9, step -0x30).
 * TODO EXACT: $1C1800 shake, victim +0xDE. */
static uint32_t handler_quake23(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned ph = o->grap44 & 0x0Fu;

    if (!v) { o->state = ST_STAND; return cell; }
    if (ph == 0) {                                         /* 0x15D44 */
        if (!(o->anim_sel & 0x8000u)) {
            int32_t x0;
            o->mover = 0;
            v->mash_aa = 0x1000; v->down_t = 0x1000;
            eng_sound(0x31);
            o->facing &= (uint16_t)~0x8000u;
            x0 = (v->x >> 16) + ((v->facing & 0x8000u) ? -0x38 : 0x38);
            if (x0 >= (o->x >> 16)) o->facing |= 0x8000u;
            return 0x1A24Au;
        }
        if ((o->frame & 0xFFu) == 0xFEu) {
            o->state = ST_MOVE; o->grap44++;
            if (cur_st && (cur_st->g161 & 2u)) o->grap44 = 8;
        }
        return 0x1A24Au;
    }
    if (ph <= 4) {                                         /* 0x15DB8 */
        if (!(o->anim_sel & 0x8000u)) {
            unsigned idx = ((o->facing ^ v->facing) & 0x8000u) ? 5 + ph : 10 - ph;
            o->vz = 0x500; o->grav = 0x48;
            homing_launch(o, v, idx); o->mover = 2;
            return cell;
        }
        if (o->landed) { eng_sound(0x29); o->mover = 0; o->count = 0; }
        if ((o->frame & 0xFFu) == 0xFEu) {
            o->state = ST_MOVE;
            if (o->zone == 1) o->grap44 = 5; else o->grap44++;
        }
        return cell;
    }
    if (ph == 5) return phase_run_out(o, v, cell);
    if (ph == 6) return phase_rebound(o, cell);
    if (ph == 7) return phase_run_back(o, v, cell);
    /* phase 8 — 0x15E42 */
    if (o->move_id & 0x8000u) {                            /* 0x15FEC pinned */
        if (v->st_flags & SF_ELIMINATED) eng_announce(cur_st, v->wrestler, 0x29);
        else if (!(o->result & 0x8000u)) return 0x15CF0u;
        o->state = ST_GETUP; v->state = ST_REACT; v->react_id = 9;
        o->cue_flags &= (uint16_t)~0x01u; o->pinning = 0;
        add_pos_delta(o, -0x30, 0, 0);
        return 0x15CF0u;
    }
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->vz = 0x600; o->grav = 0x48;
        homing_launch(o, v, 2); o->mover = 0;              /* flight at the end of frame 0 */
        return 0x15CF0u;
    }
    if (o->frame == 0 && o->count == 0) { o->mover = 2; return 0x15CF0u; }
    if (o->vz >= 0) o->move_id |= 0x1000u;                 /* 0x15E8E rising: bset #4
                                                              of the +0x60 HIGH byte */
    if ((o->frame & 0xFFu) == 2 && o->count == 0) { o->off_y = 0x20; o->floor42 = 0x20; }
    if (o->landed) {                                       /* 0x15EB8 */
        o->off_x |= 0x8000u; o->floor42 = 0;
        eng_sound(0x29);
        if (link_ok(o, v) && (v->state & 0xFFu) == ST_REACT
            && ((v->react_id & 0xFFu) == RC_LYING || (v->react_id & 0xFFu) == 9)
            && eng_prox_box(o, v, 0x0A)) {                 /* 0x11152 */
            o->mover = 0;
            eng_announce(cur_st, o->wrestler, 0x25);
            o->move_id &= (uint16_t)~0x1000u;              /* 0x15EF8 bclr #4 */
            v->mash_aa = 0; v->down_t = 0;
            if (eng_pin_allowed(o, v)) {
                o->atk = 0x801Du; o->move_id |= 0x8000u;   /* 0x15F22 cover */
                o->cue_flags |= CF_PIN_CUE; o->pinning = 1;
                v->state = ST_MOVE; v->move_id = 0x64; v->dmg = 0x14;
                o->x = v->x + (((v->facing & 0x8000u) ? -0x30 : 0x30) << 16);
                o->y = v->y - (1 << 16); o->z = v->z;      /* 0x10B9A on the victim */
                eng_tag_arm_pin(cur_st, o, v);
            } else {                                       /* 0x15F64 */
                o->count = 0x14;
                v->state = ST_REACT; v->react_id = 7; v->dmg = 0x14;
                v->hitctr_d2[6]++;                         /* 0x15F06 addq +0xDE (the splash) */
                v->down_t = ((v->band & 0xFFu) == 2u) ? 0xE0
                          : ((v->band & 0xFFu) == 1u) ? 0x80 : 0x30;   /* 0x10F56 */
            }
        } else {                                           /* 0x15F8E miss */
            o->state = ST_REACT; o->react_id = RC_BOUNCE; o->facing ^= 0x8000u;
            o->partner = -1;
            v->dmg = 3; eng_sound(0x32);
            v->down_t = 0; v->mash_aa = 0;
            o->spr = (uint16_t)(0x1F | (o->facing & 0x8000u));
        }
        return 0x15CF0u;
    }
    if ((o->frame & 0xFFu) == 0xFEu) { o->state = ST_GETUP; o->partner = -1; }
    return 0x15CF0u;
}

/* 0x19618 — victim move 0x64: PINNED under the 0x23 splash (record
 * 0x19608 mode 1 n 2 dur 8/FF00 spr 0x16/0x13 — self-drawn). Entry:
 * launcher 0x17 (the little bounce), mash seed mode 0. Landing latches
 * (move-id b7, mover off, 0x21732); latched with hp left -> the press
 * counter (0x10D04). Kick-out is the 0xEBC4 ladder: 0x64 -> 0x4B. */
static uint32_t handler_pinned64(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 0x17); o->mash_aa = 0; mash_seed(o, 0);
        return cell;
    }
    if (o->landed) { o->move_id |= 0x8000u; o->mover = 0; }
    if ((o->move_id & 0x8000u) && o->hp != 0
        && o->mash_aa != 0x4000u && (o->btn_new & 3u) && o->mash_aa && --o->mash_aa == 0)
        o->mash_aa = 0x4000u;                              /* 0x10D2E */
    return cell;
}

/* Escapes from the scripted holds (submissions.md §3), A0 = the escaping
 * victim, h = the holder (partner), all init with the holder hidden and
 * the holder/held referee bits cleared:
 *  0x5B (0x193AA, ex bearhug 0x60): hotspot y 0x20, takes the holder's
 *      facing; frame 1 end thud; FE -> stand, 0x212A0, holder react 2
 *      dmg 3, both +0x18 b7.
 *  0x5A (0x19302, ex 0x62): hotspot x -0x30; frame 0 end sound 0x2B;
 *      FE -> state 7, 0x212A0, holder react 2 dmg 3 placed +0x20.
 *  0x55 (0x19002, ex MDD 0x63): 0x212A0 at init; frame 1 end sound 0x2A;
 *      FE -> stand, holder react 2 dmg 3.
 *  0x56 (0x19086, ex pinned 0x5E): clears the holder's PIN role, hotspot
 *      0x28/0x60, holder's facing; frame 0 end thud; FE -> state 7,
 *      holder react 5 dmg 3 (spr 0x16), self spr 0x22 then 0x68, 0x21282,
 *      step +0x50. */
static uint32_t handler_escape2(eng_obj *o, uint32_t cell)
{
    eng_obj *h = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned m = o->move_id & 0xFFu;
    if (!h) { o->state = ST_GETUP; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        h->state = ST_HELD; h->grap44 = 0;
        if (m == 0x56) { h->cue_flags &= (uint16_t)~0x01u; h->pinning = 0; }
        else { o->cue_flags &= (uint16_t)~0x04u; h->cue_flags &= (uint16_t)~0x02u; }
        switch (m) {
        case 0x5B: o->off_y = 0x20; o->facing = h->facing; break;
        case 0x5A: o->off_x = 0x00D0; break;
        case 0x55: eng_tag_pin_end(cur_st, o, h); break;
        case 0x56: o->off_x = 0x28; o->off_y = 0x60; o->facing = h->facing; break;
        }
        return cell;
    }
    if (o->count == 0) {
        unsigned f = o->frame & 0xFFu;
        if (m == 0x5B && f == 1) eng_sound(0x29);
        if (m == 0x5A && f == 0) eng_sound(0x2B);
        if (m == 0x55 && f == 1) eng_sound(0x2A);
        if (m == 0x56 && f == 0) eng_sound(0x29);
    }
    if ((o->frame & 0xFFu) != 0xFEu) return cell;
    o->off_x |= 0x8000u; h->off_x |= 0x8000u;
    h->state = ST_REACT; h->dmg = 3;
    switch (m) {
    case 0x5B:
        o->state = ST_STAND; eng_tag_pin_end(cur_st, o, h);
        h->react_id = RC_FALL_HIGH; o->spr = (uint16_t)(o->facing & 0x8000u);
        break;
    case 0x5A:
        o->state = ST_GETUP; eng_tag_pin_end(cur_st, o, h);
        h->react_id = RC_FALL_HIGH; o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        carry_at(o, h, 0x20, 0, 0);
        break;
    case 0x55:
        o->state = ST_STAND; h->react_id = RC_FALL_HIGH; o->spr = (uint16_t)(o->facing & 0x8000u);
        break;
    default:                                               /* 0x56 */
        o->state = ST_GETUP; h->react_id = RC_BOUNCE;
        h->spr = (uint16_t)(0x16 | (h->facing & 0x8000u));
        h->halfct = 0;                                     /* 0x21282 */
        add_pos_delta(o, 0x50, 0, 0);
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        break;
    }
    o->role &= (uint16_t)~0x40u; h->role &= (uint16_t)~0x40u;
    o->partner = -1;
    return cell;
}

/* KO by a hold — the pair 0x6E (victim, 0x19D82) / 0x6F (holder,
 * 0x19E14), both mode-0 records: the victim lies in pose 0x4E with the
 * held bit cleared and waits for the result banners (count 0xFFFF
 * outside the rumble); the holder poses 0x4D for 0x40 ticks just below
 * him (y+2) then stands (or walks off, f32 b0 -> state 1 sub 1).
 * 0x8A (0x1AC72, record 0x1AC62 mode 1 n 2 dur 0x20/0x30 spr 0x127/0):
 * the bearhug drop after the KO: end of frame 0 puts the victim down
 * (react 2, facing flipped, +0x30,-1,+0x30); FE -> stand.
 * TODO EXACT: rumble branches (move 0x7A, phrase 0x29). */
static uint32_t handler_ko6E(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->spr = (uint16_t)(0x4E | (o->facing & 0x8000u));
        o->cue_flags &= (uint16_t)~0x04u;
        o->count = (cur_st && (cur_st->g161 & 1u)) ? 0x80 : 0xFFFF;
        return cell;
    }
    o->spr = (uint16_t)(0x4E | (o->facing & 0x8000u));
    if ((o->result & 0x8001u) == 0x8000u) { o->state = ST_STAND; return cell; }   /* 0x19DBC-0x19E06 */
    if (--o->count != 0) return cell;                  /* 0x19DC6 */
    if (cur_st && (cur_st->g161 & 1u)) {
        /* 0x19DCC/0x19DDE RUMBLE: the 0x80 count ran out — the submitted
         * man takes the eliminated walk-off (state 5 move 0x7A, clr +0x44,
         * bset #4,+0x32, announce $1C15D2/$1C15D3 = id/0x29). Without this
         * branch he wrapped to 0xFFFF and lay in the ring forever
         * ("disqualified via submission never leave the ring", playtest
         * 2026-08-24) — and, counted live, he held the 0xBB6C entrant cap
         * down and starved the arena. The announce already ran at the
         * engine's ko_check (0x1120A user elimination rule), so only the
         * bit is (re)set here. */
        o->state = ST_MOVE; o->move_id = 0x7A; o->grap44 = 0;   /* 0x19DDE-0x19DEA */
        o->st_flags |= SF_ELIMINATED;                                  /* 0x19DEE */
        o->partner = -1;                   /* engine: the dead hold link must not
                                              survive into the walk-off */
    } else
        o->count = 0xFFFF;                                /* 0x19DD6 */
    return cell;
}
static uint32_t handler_ko6F(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0; o->spr = (uint16_t)(0x4D | (o->facing & 0x8000u));
        o->count = 0x40;
        if (v) o->y = v->y + (2 << 16);
        o->off_y = 0x20; o->cue_flags &= (uint16_t)~0x02u;
        return cell;
    }
    o->spr = (uint16_t)(0x4D | (o->facing & 0x8000u));
    if (--o->count != 0) return cell;
    o->off_x |= 0x8000u; o->partner = -1;
    if (o->st_flags & SF_APRON) { o->state = ST_WALK; o->sub = 1; } else o->state = ST_STAND;
    return cell;
}
static uint32_t handler_ko8A(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    if (!(o->anim_sel & 0x8000u)) { o->off_y = 0x20; return cell; }
    if (o->frame == 0 && o->count == 0 && v) {             /* 0x1AC86 */
        o->off_x |= 0x8000u;
        v->state = ST_REACT; v->react_id = RC_FALL_HIGH;
        v->facing = o->facing ^ 0x8000u;
        carry_at(o, v, 0x30, -1, 0x30);
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND; o->spr = (uint16_t)(o->facing & 0x8000u); o->partner = -1;
    }
    return cell;
}

/* ---------------- BEHIND GRAB (docs/engine-specs/behind-grab.md) ------
 * Selector category 0x10 (0xE382: a dizzy man, react 1, seen from behind):
 * B1 -> move 0x1C (the standing rear hold — Earthquake's "sleeper"), B2 ->
 * move 0x1D (grab, spin him round to face your corner, hold him up for
 * the partner). Victims 0x52 / 0x53 draw themselves at the holder's (x,
 * y-1) plus a per-class x offset and mash out (0x10C60 seed 3 / 2, 0x10D04
 * per frame) into the escapes 0x58 / 0x57 (0xEBC4 ladder), which throw the
 * holder off (0x6D / 0x76). Holder KO check 0x111C8 -> 0x6F/0x6E. A strike
 * on the pair: hit.c behind_hit() (reactions 1..4), whose double-team
 * punch braces the holder in 0x65 / 0x73 / 0x74 for one cell. */

/* x offset of the held man from the holder: 0x18DFA[id] class -> 0x18E06
 * (hold, 0x18D72) / 0x18F08 (turn, 0x18EB2 / 0x15996), negated when the
 * pair faces right (the table bytes are negative: -0x28 etc. = in front). */
static int behind_off(const eng_obj *v, int turn)
{
    unsigned cls = v->wrestler < ENG_WS_EXT_MAX
                   ? (unsigned)eng_ws_body_class(v->wrestler) : 0;
    int off = tbl_s8(turn ? TBL(behind_grab_off_turn) : TBL(behind_grab_off_hold), cls < 5 ? cls : 4u);
    return (v->facing & 0x8000u) ? -off : off;
}

/* 0x10D04 — the press decrement (escape-machine.md §3): humans only
 * (+0x56 b7/b6 clear), raises the $1C167A b7 mash overlay, a new B1/B2
 * press counts +0xAA down, 0 -> 0x4000 latched (0x10D2E). */
static void mash_tick(eng_obj *o)
{
    if (o->cpu || (o->driver & DRV_AUTOPILOT)) return;
    if (cur_st) cur_st->body_down = 1;
    if (o->mash_aa == 0x4000u || !(o->btn_new & 3u)) return;
    if (o->mash_aa && --o->mash_aa == 0) o->mash_aa = 0x4000u;
}

/* 0x1567C — move 0x1C, record 0x15668 {mode 1, n 3, dur 0xC/0x16/0x16, spr
 * 0x161/0x162/0x163}. Init: the handler does NOTHING until +0x60 b7 is set
 * (0x15688): a fresh selector entry (word 0x001C) just plays frame 0 and
 * catches at its end; a re-entry from the double-team brace 0x65 (0xC01C,
 * 0x19696) skips frame 0 (clr +0x24/+0x22 then the tick advances) and
 * re-seats the victim at once (0x156A0: his +0x60 = 0xC0xx, then 0x15730).
 * Frame 0 end (0x156BE): |dy| < 0x10, |dx| < 0x50, same facing, link
 * 0x11412, victim state 4 react 1 / state 1 / state 0 -> victim state 5
 * move 0x52 (low byte only, 0x15736), +0x44 cleared, his facing = ours;
 * once per entry (+0x60 b7): react &= 0xFF, f33 b6 both, own +0x35 b1
 * (referee: holder), snd 0x32, 0x215B6, announcer (id, 0x1D). Miss
 * (0x1578C): poison, stand (state 1 sub 1 if outside). Every frame after
 * frame 0 (0x156B0): +0x4C = 0x0A unless outside. FE (0x157BA): result b7
 * -> both stand; else loop (frame 0 count 0 -> the tick lands on frame 1),
 * v.dmg = 4, KO 0x111C8 -> own 0x6F / victim 0x6E + 0x212A0. Companion
 * sprites (0x1580E): frame-about-to-show 0 -> fx 0; 1 -> fx 1 + 3; else 4. */
static uint32_t handler_behind1C(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned fr = o->frame & 0xFFu;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;                                      /* 0x15684 */
        if (v && v->partner < 0) v->partner = self_idx(o); /* 0xF178's second half:
                                          the cat-0x10 selector links the pile both
                                          ways (core.c sets only +0x26 of the
                                          presser); the catch's 0x11412 needs it */
        if (!(o->move_id & 0x8000u)) return cell;          /* 0x15688: wait for the tick */
        o->anim_sel |= 0x8000u; o->frame = 0; o->count = 0;   /* 0x15692-0x1569C */
        if (v) { v->move_id = (uint16_t)((v->move_id & 0xFFu) | 0xC000u); goto catch_; }   /* 0x156A0 */
        return cell;
    }
    if (o->frame != 0 && !(o->st_flags & SF_APRON)) o->atk = 0x000A;   /* 0x156AA */
    if (fr == 0 && o->count == 0) {                        /* 0x156BE end of frame 0 */
        int ok = v && abs((int)(o->y >> 16) - (int)(v->y >> 16)) < 0x10
              && abs((int)(o->x >> 16) - (int)(v->x >> 16)) < 0x50
              && ((o->facing ^ v->facing) & 0x8000u) == 0
              && link_ok(o, v)
              && (((v->state & 0xFFu) == ST_REACT && (v->react_id & 0xFFu) == RC_DIZZY)
                  || (v->state & 0xFFu) == ST_WALK || (v->state & 0xFFu) == ST_STAND);
        if (!ok) {                                         /* 0x1578C */
            if (v && v->partner == self_idx(o)) v->partner = -1;   /* engine: drop the
                                          0xF178 back-link too (stock leaves it for the
                                          divorce sweep; the engine's sweep is flag-driven) */
            o->partner = -1;
            if (o->st_flags & SF_APRON) { o->state = ST_WALK; o->sub = 1; o->spr = o->facing; }
            else o->state = ST_STAND;
            if (eng_dbgsel) fprintf(stderr, "behind: P%d 0x1C whiffed%s\n", self_idx(o) + 1,
                                    (o->st_flags & SF_APRON) ? " (apron)" : "");
            return cell;
        }
catch_:
        v->state = ST_MOVE;                                      /* 0x15730 */
        v->move_id = (uint16_t)((v->move_id & 0xFF00u) | 0x52u);
        v->grap44 = 0;
        v->facing = o->facing;
        if (!(o->move_id & 0x8000u)) {                     /* 0x15746 once */
            o->move_id |= 0x8000u;
            v->react_id &= 0xFFu;
            o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;
            o->cue_flags |= CF_SUB_HOLDER;                               /* 0x15762 referee: holder */
            eng_sound(0x32);
            eng_tag_arm_holder(cur_st, o, v);              /* 0x215B6 */
            eng_announce(cur_st, o->wrestler, 0x1D);       /* 0x15778 */
            if (eng_dbgsel) fprintf(stderr, "behind: P%d holds P%d from behind (0x1C -> 0x52)\n",
                                    self_idx(o) + 1, self_idx(v) + 1);
        }
        goto fx;
    }
    if (fr == 0xFEu) {                                     /* 0x157BA */
        if (o->result & 0x8000u) { o->state = ST_STAND; if (v) v->state = ST_STAND; return cell; }   /* 0x15800 */
        o->frame = 0; o->count = 0;                        /* 0x157CA loop */
        if (v) {
            v->dmg = 4;                                    /* 0x157D2 */
            if (ko_check(o, v)) {                          /* 0x157D8 */
                o->state = ST_MOVE; o->move_id = 0x6F;
                v->state = ST_MOVE; v->move_id = 0x6E;
                eng_tag_pin_end(cur_st, o, v);             /* 0x212A0 */
                return cell;
            }
        }
    }
fx:
    {
        unsigned d1 = (o->frame + (o->count == 0 ? 1u : 0u)) & 0xFFu;   /* 0x1580E */
        if (d1 == 0) fx_spawn(o, 0);
        else if (d1 == 1) { fx_spawn(o, 1); fx_spawn(o, 3); }
        else fx_spawn(o, 4);
    }
    return cell;
}

/* 0x17B70 — move 0x39, the APRON PUNCH (selector category 0x13 B1,
 * 0xE496: own state 1 with +0xAF == 1). Record 0x17B5C {mode 1, n 3,
 * dur 6/4/0xA, spr 0x1E2/0x1E3/0x1E4}. Init stops the walk (0x17B78);
 * every tick +0x4C := 0 (0x17B7E) and frame 2 arms hit record 0x19
 * (0x17B82-0x17B8A: flags 0x80, abox 6, damage 1, result 1). The end
 * (0x17B90 +0x25 == 0xFE) goes home to state 1 sub 1 — the apron
 * follow — with the link divorced (0x17BA4 bset #7,+0x26) and the
 * stand pose (0x17BAA +0x04 := +0x2E). Cells cue the swoosh
 * companions 0x15/0x16/0x17 (0x17BB2-0x17BF0; fx rows 0x10DDA:
 * spr 0x1E5-0x1E7 at the fist). */
static uint32_t handler_apron39(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; return cell; }   /* 0x17B78 */
    o->atk = 0;                                            /* 0x17B7E */
    if ((o->frame & 0xFFu) == 2) o->atk = 0x0019;          /* 0x17B8A */
    if ((o->frame & 0xFFu) == 0xFEu) {                     /* 0x17B90 */
        o->atk = 0;
        o->state = ST_WALK; o->sub = 1;                          /* 0x17B98/0x17B9E */
        o->divorce = 1;                                    /* 0x17BA4 */
        o->spr = o->facing;                                /* 0x17BAA */
        return cell;
    }
    {
        unsigned d1 = (o->frame + (o->count == 0 ? 1u : 0u)) & 0xFFu;   /* 0x17BB2 */
        if (d1 == 0 || d1 == 3) fx_spawn(o, 0x15);         /* 0x17BCA */
        else if (d1 == 1) fx_spawn(o, 0x16);               /* 0x17BDE */
        else fx_spawn(o, 0x17);                            /* 0x17BEC */
    }
    return cell;
}

/* 0x15870 — move 0x1D, record 0x1585C {mode 1, n 3, dur 0xC/0x16/0x16, spr
 * 0x161/0x16C/0x16D}. Init (+0x60 b7 clear, a selector entry): victim
 * +0x52 = 0, own +0x44 = 0, nothing else — frame 0 runs; re-entry from the
 * brace 0x73/0x74 (0xC01D, 0x1A03C/0x1A078): skip frame 0, victim +0x60 =
 * 0x80xx and straight into the catch (0x1589E). Frame 0 end (0x158B4):
 * link 0x11412 + victim state 4 react 1 -> +0x60 b7, f33 b6 both, victim
 * state 5 move 0x53 (low byte), +0x44 = 0 (0x158DA); miss -> poison,
 * stand (0x15900). Frames after 0: +0x4C = 0x0D (0x158AE). FE (0x15928):
 * result b7 -> both stand; else loop; THE TURN once (+0x45 b0 = word +0x44
 * bit 0, 0x15946): face the partner (toward his x, 0x15956 — in a match
 * with no partner the ROM reads word $0006 = 0x0654 and faces right), in
 * the rumble face ring centre (x < 0x280 -> right, 0x1596C); the victim
 * takes our facing, x, y-1 plus the turn offset (0x15980-0x159BC); jsr
 * 0x20F04 (arm the partner to come and work him). Companion sprites
 * (0x159C8): frame-about-to-show 1 -> fx 8; other non-zero -> fx 9 (the
 * FE tick reaches 0x159E6 with a stale D1 — fx 9 assumed, TODO EXACT). */
static uint32_t handler_behind1D(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned fr = o->frame & 0xFFu, d1;
    int rumble;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;                                      /* 0x15878 */
        if (v && v->partner < 0) v->partner = self_idx(o); /* 0xF178 second half (see 0x1C) */
        if (!(o->move_id & 0x8000u)) {                     /* 0x1587C */
            if (v) v->combo = 0;                           /* 0x15884 clr +0x52 */
            o->grap44 = 0;                                 /* 0x15888 */
            return cell;
        }
        o->anim_sel |= 0x8000u; o->frame = 0; o->count = 0;   /* 0x15890-0x1589A */
        if (v) { v->move_id = (uint16_t)((v->move_id & 0xFFu) | 0x8000u); goto catch_; }   /* 0x1589E */
        return cell;
    }
    if (o->frame != 0) o->atk = 0x000D;                    /* 0x158A8 */
    if (fr == 0 && o->count == 0) {                        /* 0x158B4 */
        if (!link_ok(o, v) || (v->state & 0xFFu) != ST_REACT || (v->react_id & 0xFFu) != RC_DIZZY) {
            o->partner = -1; o->state = ST_STAND;                 /* 0x15900 */
            if (eng_dbgsel) fprintf(stderr, "behind: P%d 0x1D whiffed\n", self_idx(o) + 1);
            return cell;
        }
catch_:
        o->move_id |= 0x8000u;                             /* 0x158DA */
        o->role |= RF_ENGAGED; v->role |= RF_ENGAGED;
        v->state = ST_MOVE;
        v->move_id = (uint16_t)((v->move_id & 0xFF00u) | 0x53u);   /* 0x158F2 */
        v->grap44 = 0;
        if (eng_dbgsel) fprintf(stderr, "behind: P%d grabs P%d from behind (0x1D -> 0x53)\n",
                                self_idx(o) + 1, self_idx(v) + 1);
        goto fx;
    }
    if (fr == 0xFEu) {                                     /* 0x15910 */
        if (o->result & 0x8000u) { o->state = ST_STAND; if (v) v->state = ST_STAND; return cell; }   /* 0x15918 */
        o->frame = 0; o->count = 0;                        /* 0x15932 loop */
        rumble = cur_st && (cur_st->g161 & 1u);
        if (v && (rumble || !(o->grap44 & 1u))) {          /* 0x15946 the turn: once —
                                          the rumble branch (0x1593A -> 0x1596C)
                                          jumps past the +0x45 b0 latch, so there
                                          it runs on EVERY loop */
            if (rumble) {                                  /* 0x1596C: face ring centre */
                o->facing = (uint16_t)(((o->x >> 16) < 0x280) ? 0x8000u : 0);
            } else {                                       /* 0x15950 toward the partner */
                o->grap44 |= 1u;
                int px = (o->teammate >= 0 && cur_st) ? (int)(cur_st->obj[o->teammate].x >> 16) : 0x654;
                o->facing = (uint16_t)((px >= (int)(o->x >> 16)) ? 0x8000u : 0);
            }
            v->facing = o->facing;                         /* 0x15980 */
            v->x = o->x; v->y = o->y - (1 << 16);
            v->x += behind_off(v, 1) << 16;                /* 0x15996-0x159BC */
            eng_tag_arm_behind_holder(cur_st, o, v);       /* 0x159C0 jsr 0x20F04 */
            if (eng_dbgsel) fprintf(stderr, "behind: P%d turns (face %s), P%d re-seated at x=%d\n",
                                    self_idx(o) + 1, (o->facing & 0x8000u) ? "right" : "left",
                                    self_idx(v) + 1, (int)(v->x >> 16));
            fx_spawn(o, 9);                                /* 0x159C6 -> 0x159E6, stale D1 */
            return cell;
        }
    }
fx:
    d1 = (o->frame + (o->count == 0 ? 1u : 0u)) & 0xFFu;  /* 0x159C8 */
    if (d1 == 1) fx_spawn(o, 8);
    else if (d1 != 0) fx_spawn(o, 9);
    return cell;
}

/* 0x18D2E — move 0x52 (held from behind by 0x1C; record 0x18D12 {mode 1,
 * n 2, dur 0x16/0x16, spr 0x168/0x169}, phase-1 record 0x18D22 {8, spr
 * 0x21B}) and 0x18E64 — move 0x53 (held by 0x1D; record 0x18E3C {mode 1,
 * n 2, 0x16/0x16, spr 0x170/0x171}, phase records 0x18E4C {0xC, 0x4F} /
 * 0x18E58 {0xC, 0x51}). Phase = +0x44 & 1 (0x52) / & 3 (0x53):
 *  phase 0 init (0x18D4C / 0x18E86): mover off, 0x10C60 seed 3 / 2 (no
 *      clear first — a live count carries over), x = holder's, y = his-1,
 *      x += the class offset (0x53 also takes his facing word); 0x52 once
 *      (+0x60 b7): 0x2103E (arm own partner), +0x35 b2 (referee: held),
 *      +0x52 = 0. Per frame: +0x4C = 9 / 0xC, 0x10D04; at FE: +0x1C b7
 *      cleared so the init re-runs and re-seats him (0x18DCA / 0x18EF2),
 *      0x53 once (+0x60 b7): 0x21114. (0x52's fx ids 2/5 are computed and
 *      never spawned — the jsr is a nop at 0x18DF6.)
 *  phase 1/2 (0x18E0C / 0x18F0E / 0x18F38): the double-team flinch —
 *      init: +0x52 += 1, +0x54 = 0x40; FE: 0x52 -> state 5 (re-init) and
 *      +0x44 = 0; 0x53 -> +0x44 = 0 only (the held cell's FE re-inits).
 * Nothing here transitions: the escape is the 0xEBC4 ladder on the next
 * press once +0xAA reads 0x4000 (0x52 -> 0x58, 0x53 -> 0x57). ENGINE: the
 * ladder arm lives here (checked before the 0x10D04 tick, as the ROM's
 * input phase precedes the anim) because core.c is owned elsewhere —
 * TODO: move into walk_logic's 0xEBC4 switch. */
static uint32_t handler_behindvictim(eng_obj *o, uint32_t cell)
{
    eng_obj *h = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned m = o->move_id & 0xFFu, is53 = (m == 0x53u);
    unsigned ph = o->grap44 & (is53 ? 3u : 1u);

    if ((o->anim_sel & 0x8000u) && (o->btn_new & 3u) && o->mash_aa == 0x4000u) {   /* 0xEBC4 */
        o->mash_aa = 0; o->state = ST_MOVE; o->move_id = (uint16_t)(is53 ? 0x57u : 0x58u);
        if (eng_dbgsel) fprintf(stderr, "behind: P%d mashed out -> escape %02X\n",
                                self_idx(o) + 1, o->move_id);
        return cell;
    }
    if (ph == 0) {
        if (!(o->anim_sel & 0x8000u)) {                    /* 0x18D4C / 0x18E86 */
            o->mover = 0;
            mash_seed(o, is53 ? 2 : 3);                    /* 0x10C60 */
            if (h) {
                o->x = h->x; o->y = h->y - (1 << 16);
                if (is53) o->facing = h->facing;           /* 0x18EAC */
                o->x += behind_off(o, is53) << 16;
            }
            if (!is53 && !(o->move_id & 0x8000u)) {       /* 0x18D9C once */
                o->move_id |= 0x8000u;
                if (h) eng_tag_arm_behind_victim(cur_st, o, h, 0x52);   /* 0x2103E */
                o->cue_flags |= CF_SUB_VICTIM;                           /* 0x18DAA referee: held */
                o->combo = 0;
            }
            return cell;
        }
        o->atk = (uint16_t)(is53 ? 0x000Cu : 0x0009u);     /* 0x18DB6 / 0x18EDE */
        mash_tick(o);                                      /* 0x10D04 */
        if ((o->frame & 0xFFu) == 0xFEu) {                 /* 0x18DC2 / 0x18EEA */
            o->anim_sel &= (uint16_t)~0x8000u;             /* re-init: re-seat */
            o->frame = 0; o->count = 1;
            if (is53 && !(o->move_id & 0x8000u)) {        /* 0x18EF8 once */
                o->move_id |= 0x8000u;
                if (h) eng_tag_arm_behind_victim(cur_st, o, h, 0x53);   /* 0x21114 */
            }
        }
        return cell;
    }
    /* phase 1 / 2: the flinch cell */
    cell = is53 ? (ph == 2 ? 0x18E58u : 0x18E4Cu) : 0x18D22u;
    if (!(o->anim_sel & 0x8000u)) {                        /* 0x18E0C / 0x18F0E / 0x18F38 */
        o->combo++; o->combo_t = 0x40;
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->grap44 = 0;                                     /* 0x18E30 / 0x18F2C */
        if (!is53) o->state = ST_MOVE;                           /* 0x18E2A re-init */
    }
    return cell;
}

/* 0x191C8 — move 0x58 (escape from 0x52; record 0x191B4 {mode 1, n 3, dur
 * 0x18/0xC/0xC, spr 0x181/0x182/0x183}) and 0x1914E — move 0x57 (escape
 * from 0x53; record 0x1913E {n 2, 0x10/0x10, spr 0x186/0x187}). Init: the
 * holder is scripted into his throw-off (0x58: h -> move 0x6D, +0x35 b2/b1
 * cleared; 0x57: y = h.y - 1, h -> move 0x76), 0x212A0. Frame 1 start:
 * snd 0x2A, h.dmg = 3. FE: stand (state 0), f33 b6 cleared both, poison.
 * 0x58 companions: frame-about-to-show 0 -> fx 6, 1 -> fx 7. */
static uint32_t handler_behindesc(eng_obj *o, uint32_t cell)
{
    eng_obj *h = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned is58 = (o->move_id & 0xFFu) == 0x58u;
    if (!h) { o->state = ST_STAND; return cell; }
    if (!(o->anim_sel & 0x8000u)) {
        if (is58) {                                        /* 0x191D0 */
            h->state = ST_MOVE; h->move_id = 0x6D;
            o->cue_flags &= (uint16_t)~0x04u; h->cue_flags &= (uint16_t)~0x02u;
        } else {                                           /* 0x19156 */
            o->y = h->y - (1 << 16);
            h->state = ST_MOVE; h->move_id = 0x76;
        }
        eng_tag_pin_end(cur_st, o, h);                     /* 0x212A0 */
        if (eng_dbgsel) fprintf(stderr, "behind: P%d escapes (%02X), holder P%d thrown off (%02X)\n",
                                self_idx(o) + 1, o->move_id & 0xFFu, self_idx(h) + 1, h->move_id);
    } else {
        if ((o->frame & 0xFFu) == 1 && o->count == 0) { eng_sound(0x2A); h->dmg = 3; }   /* 0x191F0 / 0x19174 */
        if ((o->frame & 0xFFu) == 0xFEu) {                 /* 0x1920E / 0x19192 */
            o->state = ST_STAND;
            o->role &= (uint16_t)~0x40u; h->role &= (uint16_t)~0x40u;
            o->partner = -1;
            return cell;
        }
    }
    if (is58) {                                            /* 0x19232 */
        unsigned d1 = (o->frame + (o->count == 0 ? 1u : 0u)) & 0xFFu;
        if (d1 == 0) fx_spawn(o, 6);
        else if (d1 == 1) fx_spawn(o, 7);
    }
    return cell;
}

/* 0x19D06 — move 0x6D (the 0x1C holder thrown off by 0x58; record 0x19CF2
 * {n 3, 0x10 each, spr 0x163/0x163/0x10}): fx 4 while frames 0/1 show;
 * FE -> down, react 2 (0xA when outside, 0x19D1E). 0x1A10E — move 0x76
 * (the 0x1D holder thrown off by 0x57; record 0x1A0FE {n 2, 0x10/0x10,
 * spr 0x16C/0x0F}): fx 8 during frame 0 (count != 0); FE -> react 2. */
static uint32_t handler_behindoff(eng_obj *o, uint32_t cell)
{
    unsigned is6D = (o->move_id & 0xFFu) == 0x6Du;
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; return cell; }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_REACT;
        o->react_id = (uint16_t)((is6D && (o->st_flags & SF_APRON)) ? 0x0A : 2);
        return cell;
    }
    if (is6D) {
        unsigned d1 = (o->frame + (o->count == 0 ? 1u : 0u)) & 0xFFu;   /* 0x19D46 */
        if (d1 == 0 || d1 == 1) fx_spawn(o, 4);
    } else if (o->frame == 0 && o->count != 0)             /* 0x1A130 */
        fx_spawn(o, 8);
    return cell;
}

/* 0x19670 — move 0x65 (record 0x19664 {n 1, 8, spr 0x219}), 0x1A020 —
 * 0x73 ({n 1, 0xC, spr 0x50}), 0x1A05C — 0x74 ({n 1, 0xC, spr 0x52}): the
 * holder braces while his partner's punch lands on the held man (hit.c
 * behind_hit); fx 0xC / 0xA / 0xB every frame; FE -> back into the hold
 * with the once bits kept: state 5 move 0xC01C / 0xC01D (0x19696 /
 * 0x1A03C / 0x1A078). */
static uint32_t handler_dtbrace(eng_obj *o, uint32_t cell)
{
    unsigned m = o->move_id & 0xFFu;
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (m != 0x65u) fx_spawn(o, m == 0x73u ? 0x0Au : 0x0Bu);   /* 0x1A02C: 0x73/0x74
                                          run the fx block on the init tick too */
        return cell;
    }
    if (m == 0x65u) fx_spawn(o, 0x0Cu);                    /* 0x1967E: before the FE test */
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_MOVE; o->move_id = (uint16_t)(m == 0x65u ? 0xC01Cu : 0xC01Du);
        return cell;                                       /* 0x1A042/0x1A07E: no fx on the FE tick */
    }
    if (m != 0x65u) fx_spawn(o, m == 0x73u ? 0x0Au : 0x0Bu);   /* 0x1A044 / 0x1A080 */
    return cell;
}

/* 0x17 — the Irish whip (grapple-moves.md §3): at the end of frame 1
 * the victim is released into a FORCED RUN with rope-bounce budget
 * +0x44=2 (which also suppresses their run steering/reversal), angle
 * away along the whipper's facing; lazy divorce; whipper exits. */
static uint32_t handler_whip(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        return cell;
    }
    if (o->frame == 1 && o->count == 0 && v && (v->state & 0xFFu) == 0xFFu) {
        v->state = 0x0002;             /* forced run, 0x14E5A */
        v->grap44 = 2;                 /* rope-bounce budget */
        v->facing = o->facing ^ 0x8000u;               /* runs AWAY */
        v->angle = (v->facing & 0x8000u) ? 0x0040 : 0x00C0;
        v->x = o->x + ((o->facing & 0x8000u) ? -(0x50 << 16) : (0x50 << 16));
        v->y = o->y - (2 << 16);       /* 0x10B9A(+0x50,-2,0): BEHIND */
        v->z = o->z;
        v->tag_flags |= TF_WHIPPED; o->tag_flags |= TF_WHIPPER;              /* 0x14E84/0x14E8A */
        v->role &= (uint16_t)~0x40u;
        o->role &= (uint16_t)~0x40u;
        v->partner = -1;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND;
        o->facing ^= 0x8000u;          /* 0x14EE2: turn to watch him go */
        o->partner = -1;
    }
    return cell;
}

/* 0x14CF4 — move 0x15 front facelock stance (grapple-moves.md §2b,
 * hold-timeout.md §2): carries the victim at zero offset, faces him,
 * 1 damage per completed wrench cycle, loops. The victim is handed to
 * move 0x7B (0x14D36) — that move runs the auto-reverse clock. */
static uint32_t handler_facelock(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        uint16_t g44 = o->grap44;      /* 0x14D00 and 0x14D2C both read +0x44 */

        o->mover = 0;
        if (g44 == 1) {                /* 0x14D00: re-entry from the drag walk
                                          (0xDE64 wrote +0x44 = 1) — resume the
                                          wrench loop, no nudge */
            o->grap44 = 0;
        } else if ((o->prev_sel & 0xFFu) == 0x0C) {
            /* 0x14D10 `move.w #$20,D0; jsr 0x10BD0` — 0x10BD0 negates D0 when
             * facing right, so a POSITIVE argument steps BACKWARD (away from
             * the hold's midpoint). add_pos_delta has the same sign rule; the
             * old -0x20 here stepped FORWARD and shoved the pair a whole 0x20
             * toward the facing on every 0x0C -> 0x15 transition. */
            add_pos_delta(o, 0x20, 0, 0);
        }
        if (g44 == 3) {                /* 0x14D2C: reversal re-entry (0x7D/0x7E/
                                          0x7F set the new holder's +0x44 = 3) —
                                          the victim is already the 0x7B man */
            o->grap44 = 0;
        } else if (v) {
            v->state = ST_MOVE;              /* 0x14D36: victim -> the held move, whose
                                          word write also clears the +0x60 b15
                                          "escape rolled" latch (0xEB9E) */
            v->move_id = 0x7B;
        }
        return cell;
    }
    if (v) {
        v->x = o->x; v->y = o->y;      /* zero-offset carry, 0x14D44 */
        v->facing = o->facing ^ 0x8000u;
        if (o->result) {               /* match over: split */
            o->state = ST_STAND;
            v->state = ST_REACT; v->react_id = RC_DIZZY;
            o->partner = -1; v->partner = -1;
            return cell;
        }
        if ((o->frame & 0xFFu) == 0xFEu) {
            o->frame = 0;              /* loop the wrench */
            o->count = 0;
            v->dmg = 1;                /* 1 damage per cycle */
        }
    }
    return cell;
}

/* 0x14DA2 — move 0x16 stance drag-walk: victim dragged at the holder's
 * position, half walk speed. */
static uint32_t handler_drag(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u))
        o->mover = 1;
    if (o->wrestler < ENG_WS_EXT_MAX)
        o->speed = (uint16_t)(walk_speed_of(o) >> 1);   /* half */
    if ((o->angle & 0xFFu) != 0 && (o->angle & 0xFFu) != 0x80) {
        /* the holder drags FACING the victim — away from the motion
         * (playtest: motion-facing looked back-to-front vs stock) */
        o->facing = (o->angle & 0x80u) ? 0x8000u : 0;
    }
    if (v) {
        v->x = o->x; v->y = o->y; v->z = o->z;
        v->facing = o->facing ^ 0x8000u;
        if (o->result) {
            o->state = ST_STAND;
            v->state = ST_REACT; v->react_id = RC_DIZZY;
            o->partner = -1; v->partner = -1;
        }
    }
    return cell;
}

/* Standing-hold scalars (docs/engine-specs/hold-timeout.md): rows 0-4 come
 * from the synthetic table "hold_rules" (exported/moddable); higher indices
 * are the ROM escape-roll rows core.c passes in as `def` (its own tables).
 * `def` is also the stock value if the table is not bound. */
int eng_hold_rule(int idx, int def)
{
    if (idx >= 0 && idx < (int)(sizeof hold_rules_be / 2) && tbl_bytes(TBL(hold_rules), NULL))
        return (int)tbl16(TBL(hold_rules), (uint32_t)idx * 2u);
    return def;
}

/* 0x1A538 — move 0x7B, the man held in the facelock stance 0x15 / drag
 * walk 0x16 (record 0x1A532 is mode 0: no frames, the handler is the
 * whole move). His +0x44 is the AUTO-REVERSE clock: 0x80 ticks under a
 * human holder (0x1A564), 0xBB under a CPU one (0x1A572). At 0 he takes
 * move 0x7C and the holder is frozen (0x1A580) — hold-timeout.md §3.
 * A2 in the ROM is +0x26, i.e. the HOLDER. */
static uint32_t handler_heldvictim(eng_obj *o, uint32_t cell)
{
    eng_obj *h = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->spr = 0xFFFF;               /* 0x1A540: the holder draws the pair */
        if (h && (h->role & RF_ONFIRE))     /* 0x1A546: holder ON FIRE -> pre-set the
                                          +0x60 b15 latch, so 0xEA42 gives the
                                          victim no escape roll at all */
            o->move_id |= 0x8000u;
        if (o->grap44 != 1 && (o->prev_sel & 0xFFu) == 5)
            return cell;               /* 0x1A55C: 0x16 -> 0x15 re-entry keeps
                                          the clock running */
        o->grap44 = (uint16_t)eng_hold_rule((h && h->cpu) ? 3 : 2,
                                            (h && h->cpu) ? 0xBB : 0x80);
        return cell;
    }
    if (o->grap44 && --o->grap44 == 0) {               /* 0x1A57A */
        o->state = ST_MOVE;                                  /* 0x1A580 */
        o->move_id = 0x7C;
        if (h) h->state = ST_HELD;                      /* 0x1A58C */
    }
    return cell;
}

/* 0x1A5A0 — move 0x7C, the hold reversal (record 0x1A594: mode 1, one
 * frame, 8 ticks, cell 0x221). The ex-victim turns, the ex-holder is
 * frozen; at the end he runs the Irish whip 0x17 on him (0x1A5CC) — the
 * "usually an Irish whip is the reverse" the feature asked for. */
static uint32_t handler_holdrev(eng_obj *o, uint32_t cell)
{
    eng_obj *h = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;                                  /* 0x1A5A8 clr.b +0x01 */
        o->facing ^= 0x8000u;                          /* 0x1A5B0 bchg */
        if (h) {
            h->mover = 0;                              /* 0x1A5AC */
            h->state = ST_HELD;                         /* 0x1A5B6 */
            h->spr = 0xFFFF;                           /* 0x1A5BC */
        }
        return cell;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {                 /* 0x1A5C4 */
        o->state = ST_MOVE;
        o->move_id = 0x17;                             /* 0x1A5D2: the whip */
        if (h) h->state = ST_HELD;
        o->facing ^= 0x8000u;                          /* 0x1A5DE bchg back */
        add_pos_delta(o, 0x20, 0, 0);                  /* 0x1A5E4 0x10BD0(0x20,0,0):
                                                          0x20 BACKWARD */
    }
    return cell;
}

/* +0xC7: the big hits a man has taken this match - the 28 move handlers
 * bump it (+1, the slams/drops +5: 0x13E54, 0x15186, 0x16332, 0x164FC,
 * 0x16854, 0x16972, 0x16C16, 0x16D78, 0x16F44, 0x17106, 0x1740A, 0x17568,
 * 0x186FE) where the victim is launched; 0xFD72 reads it for the CPU's
 * comeback (below). */
static void hit_count(eng_obj *v, int n) { if (v) v->hits_c7 = (uint8_t)(v->hits_c7 + n); }

/* ---- 0xFD00: the ON-FIRE / REGAIN POWER loop (frame list 0x2672, every
 * match frame, the 6 slots).  +0x33 b5 = on fire (walk +6, wins the
 * tie-up, the HUD flash): counts +0xC8 down, then clears it, +0xC6 and
 * itself (0x8962 is an rts).  Not on fire: a CPU (+0x56 b7) whose big-hit
 * count +0xC7 passed his wrestler's 0xFDCC threshold lights up for
 * 0xFDD8[stage] frames; a human OUTSIDE the ring (+0x32 b0 - the apron
 * machine keeps his +0xC8 climbing, 0x11834/0x1197A) past his 0xFDB4
 * threshold gets +0xC6 b6 = REGAIN POWER armed with a 0x3A9 clock, and
 * the apron machine turns that into the 0x71 flex + fire; inside the ring
 * +0xC8 is cleared. */
static const tbl_def comeback_tables[] = {
    { "comeback_apron_frames", "wrestler", 0xFDB4u, 12 * 2, TK_U16, 1,
      "0xFD54: per wrestler id, apron frames (+0xC8) before REGAIN POWER arms (+0xC6 b6, clock 0x3A9)" },
    { "comeback_cpu_hits",     "wrestler", 0xFDCCu, 12, TK_U8, 1,
      "0xFD78: per wrestler id, big hits taken (+0xC7) before a CPU goes ON FIRE (+0x33 b5)" },
    { "comeback_fire_frames",  "base/anim", 0xFDD8u, 10 * 2, TK_U16, 1,
      "0xFD90: ON-FIRE frames (+0xC8) per stage word $1C0162 for the CPU comeback" },
};
TBL_REGISTER(comeback_tables)
void eng_comeback_tick(eng_state *st)
{
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        unsigned id;
        if (!o->active) continue;
        id = (unsigned)eng_ws_base(o->wrestler) % 12u;
        if (o->role & RF_ONFIRE) {                                   /* 0xFD08 on fire: the clock */
            if (o->cmb_c8 && --o->cmb_c8) continue;
            o->cmb_c8 = 0; o->cmb_c6 = 0; o->hits_c7 = 0;             /* 0xFD20..0xFD2E: clr.w +0xC6 is a WORD
                                                                       clear = +0xC6 AND +0xC7 (the big-hit count;
                                                                       left alone, a CPU re-ignited the very next
                                                                       frame = 'permanent power up', user 2026-08-30) */
            o->role &= (uint16_t)~0x20u;
            continue;
        }
        if (o->cpu) {                                           /* 0xFD72 */
            if (o->hits_c7 <= tbl8(TBL(comeback_cpu_hits), id)) continue;
            o->cmb_c8 = tbl16(TBL(comeback_fire_frames), (eng_camp_stage() < 10 ? eng_camp_stage() : 9) * 2u);
            o->role |= RF_ONFIRE;                                    /* 0xFDA2 */
            if (eng_dbgsel) fprintf(stderr, "comeback: CPU o%d ON FIRE after %d hits (%u frames)\n", i, o->hits_c7, o->cmb_c8);
            continue;
        }
        if (!(o->st_flags & SF_APRON)) { o->cmb_c8 = 0; continue; }     /* 0xFD46 inside the ring */
        {
            static int poke = -1;                               /* harness: WF_CMB_APRON=n lowers the threshold */
            unsigned thr = tbl16(TBL(comeback_apron_frames), id * 2u);
            if (poke < 0) poke = getenv("WF_CMB_APRON") ? atoi(getenv("WF_CMB_APRON")) : 0;
            if (poke > 0) thr = (unsigned)poke;
            if (o->cmb_c8 <= thr) continue;                     /* 0xFD5E */
        }
        o->cmb_c6 |= 0x40u; o->cmb_c8 = 0x3A9;                  /* 0xFD64 REGAIN POWER armed */
        if (eng_dbgsel) fprintf(stderr, "comeback: o%d REGAIN POWER armed\n", i);
    }
}

/* 0x19F1E - move 0x71, the REGAIN POWER flex on the apron (record 0x19F0E:
 * mode 1, 2 x 0xC, spr 0x55/0x56).  Entry: +0x44 = 6 repeats.  Each pass
 * (FE): re-init at count 8, rope shake 4 on his side, sounds 0xD/0xE per
 * frame; when the repeats run out: state 1 sub 1, +0xC6 b6 off, ON FIRE
 * (+0x33 b5). */
static uint32_t handler_regain(eng_obj *o, uint32_t cell)
{
    if (!(o->anim_sel & 0x8000u)) { o->mover = 0; o->grap44 = 6; o->frame = 0; return cell; }   /* 0x19F26 */
    if ((o->frame & 0xFFu) == 0xFEu) {                          /* 0x19F40 */
        o->anim_sel &= (uint16_t)~0x8000u; o->frame = 0; o->count = 8;
        if ((int16_t)--o->grap44 < 0) {                         /* 0x19F58 */
            o->state = ST_WALK; o->sub = 1;
            o->cmb_c6 &= (uint8_t)~0x40u; o->role |= RF_ONFIRE;     /* 0x19F6A / 0x19F70 */
            if (eng_dbgsel) fprintf(stderr, "comeback: o%d flexed -> ON FIRE\n", (int)(o - cur_st->obj));
        }
    }
    if (o->frame == 0 && o->count == 0) eng_ropes_arm(o->role & RF_SIDE ? 1 : 0, 1, 1);   /* 0x19F8A/0x19F94: the rope shakes */
    {   unsigned d1 = o->frame & 0xFFu; if (o->count == 0) d1++;   /* 0x19F9C..0x19FBE: 0x10D3A is the
                                          COMPANION-SPRITE spawn (fx rows 0xD/0xE = the rope-shake
                                          pieces, every tick), not a sound post - the engine used to
                                          latch 0x0D/0x0E = music track commands ("power up plays a
                                          different music track" + the missing chunks, user 2026-08-30) */
        fx_spawn(o, d1 == 0 ? 0x0Du : 0x0Eu); }
    return cell;
}

/* Throw common release: victim -> air reaction with damage. */
/* fmode: 0 = victim facing untouched (backdrop), 1 = "partner +0x2E =
 * own, bchg" = opposite of the thrower (hip toss 0x14F34, press slam
 * 0x15160), 2 = "bchg only" toggle (slam 0x160xx — a held man faced the
 * holder, so he ends up facing the SAME way and flies away; oracle
 * grapple-full f2347). */
static void throw_release(eng_obj *o, eng_obj *v, unsigned react,
                          unsigned dmg, int dx, int dy, int dz, int fmode)
{
    v->state = ST_REACT;
    v->react_id = (uint16_t)react;
    v->dmg = (uint16_t)dmg;
    eng_sound(0x32);
    if (fmode == 1) v->facing = o->facing ^ 0x8000u;
    else if (fmode == 2) v->facing ^= 0x8000u;
    o->role &= (uint16_t)~0x40u;
    v->role &= (uint16_t)~0x40u;
    v->partner = -1;                   /* the ROM's lazy divorce (0x115D2)
                                          modeled as an immediate unlink */
    carry_at(o, v, dx, dy, dz);
}

/* 0x14F08 — move 0x18 hip toss. */
static uint32_t handler_hiptoss(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        if (v) v->state = ST_HELD;
        o->off_x = 0x10;
        eng_announce(cur_st, o->wrestler, 0x02);       /* 0x14F22 */
        return cell;
    }
    if (o->frame == 2 && o->count == 0 && v)
        throw_release(o, v, 0x22, 0x0C, 0x50, 1, 0x50, 1); hit_count(v, 1);
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->off_x |= 0x8000u;
        o->state = ST_STAND;
        o->facing ^= 0x8000u;          /* turn to face the thrown man */
        o->partner = -1;
    }
    return cell;
}

/* 0x16056 — move 0x24 forward suplex/slam. */
static uint32_t handler_suplex(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        eng_slide_clip(o, -0x18);      /* 0x16062 0x10B62(-0x18): rope-CLIPPED slide */
        if (v) v->state = ST_HELD;
        if (eng_ws_base(o->wrestler) == 8) eng_announce(cur_st, o->wrestler, 0x27);   /* 0x16078 Quake */
        else eng_announce(cur_st, o->wrestler, 0x01);  /* 0x16080 */
        return cell;
    }
    if (o->frame == 2 && o->count == 0 && v) {
        throw_release(o, v, 0x16, 0x0E, -0x10, 0, 0x40, 2); hit_count(v, 1);
        if (eng_ws_base(o->wrestler) != 0) eng_throw_out(v);   /* throw_out mod: SLAMS only - Hogan's 0x24 is his
                                          vertical SUPLEX (same id, his art), never a ring-out (user 2026-08-30) */
        v->y -= 1 << 16;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND;
        o->spr = o->facing;            /* 0x160xx exit: +0x04 = +0x2E the
                                          same tick as the 0x18 step back */
        add_pos_delta(o, 0x18, 0, 0);
        o->partner = -1;
    }
    return cell;
}

/* 0x16E48 — move 0x30 backdrop / back suplex. Fumble roll and the
 * behind-take entry TODO (no RNG yet). */
static uint32_t handler_backdrop(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->off_y = 0x20;
        if (v) v->state = ST_HELD;
        eng_announce(cur_st, o->wrestler, 0x04);       /* 0x16ED4 */
        return cell;
    }
    if (o->frame == 3 && o->count == 0)
        eng_sound(0x2B);               /* swing */
    if (o->frame == 4 && o->count == 0 && v) {
        unsigned dmg = 0x11;
        if (eng_ws_base(o->wrestler) == 0 || eng_ws_base(o->wrestler) == 5)
            dmg += 2;                  /* Hogan/Warrior bonus */
        throw_release(o, v, 0x13, dmg, 0x50, 0, 0x10, 0); hit_count(v, 5); eng_throw_out(v);
        v->react_id |= 0x2000u;        /* 0x16F74 bset #5,+0x64: rise dizzy */
        v->st_flags |= SF_TIRED;             /* 0x16F7A bset #3,+0x32: tired walk */
        v->halfct = 0;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_STAND;
        add_pos_delta(o, -0x38, 0, 0);
        o->facing ^= 0x8000u;
        o->off_x |= 0x8000u;
        o->partner = -1;
    }
    return cell;
}

/* 0x1129E entry 1 (0x11302) — the THROW ROLL, run at the entry of the big
 * grapple throws. Carry SET = the throw goes. A CPU thrower never fails;
 * the row is 0x113CD[stage] against a CPU partner (0x11404 in the
 * rumble), 0x113FF against a human, indexed by the thrower's per-match
 * attempt count for THIS throw — each throw keeps its own word, +0xE8..
 * +0xFC (ctr_off; bumped every call, capped at 4 for the lookup): fail
 * when rng & 0xF > the byte — the 5th attempt on fails ~half the time.
 * A failed roll diverts the thrower into a fumble move 0x80-0x87. */
static int throw_roll(eng_obj *o, unsigned ctr_off)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    uint16_t *ctr = &o->tries[(ctr_off - 0xE8u) / 2u];
    unsigned d2 = *ctr, th, row;
    (*ctr)++;                                              /* e.g. 0x15258 addq.w #1,+0xFC */
    if (getenv("WF_FUMBLE")) return 0;                     /* harness poke: always fail */
    if (o->cpu) return 1;                                  /* 0x11302 */
    if (d2 > 4) d2 = 4;
    if (v && v->cpu)                                       /* throw_roll_rows: 0x113CD + stage*5,
                                                              0x113FF human = row 10, 0x11404 rumble = row 11 */
        row = (cur_st->g161 & 1u) ? 11u
            : (cur_st->stage < 10 ? cur_st->stage : 9u);
    else row = 10u;
    th = tbl8(TBL(throw_roll_rows), row * 5u + d2);
    if ((eng_rng() & 0x0Fu) > th) {                        /* 0x11362 bhi = fail */
        if (eng_dbgsel)
            fprintf(stderr, "throw: P%d FUMBLE (attempt %u, threshold %u)\n",
                    self_idx(o) + 1, d2, th);
        return 0;
    }
    return 1;
}

/* The fumble family 0x80-0x87 (mode-2 struggle loops, records 0x1A794..
 * 0x1AA8A): the thrower strains for 0x50 ticks (0x48 for 0x86/0x87; 0x85
 * first steps +0x20, 0x86/0x87 +0x10), then the announcer calls it
 * (0x0F1A) and the man he tried to throw reverses him:
 *  0x80/0x81/0x82 (0x1A7B4/0x1A832): into the victim's FACELOCK — the
 *      thrower becomes the held man (move 0xC07B, +0x44 = 1), the victim
 *      takes stance 0x15 facing him (spr 0x39), thrower hidden.
 *  0x83-0x87 (0x1A8B6/0x1A950/0x1A9E4/0x1AA9A): BACK-DROPPED — thrower
 *      react 0x0E dmg 8 on the victim's head (z +0x48, spr 0x2B), victim
 *      into the latched catch 0xC006 (spr 0x60), +0x92 paired both ways;
 *      0x83 also nudges the victim y+1 and clears f33 b6 (the others set
 *      it; 0x86/0x87 clear). Sprite offsets cleared on both. */
static uint32_t handler_fumble(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;
    unsigned m = o->move_id & 0xFFu;

    if (!(o->anim_sel & 0x8000u)) {
        if (m == 0x85) add_pos_delta(o, 0x20, 0, 0);       /* 0x1A9EC */
        if (m == 0x86 || m == 0x87) add_pos_delta(o, 0x10, 0, 0);   /* 0x1AAA2 */
        o->grap44 = (m == 0x86 || m == 0x87) ? 0x48 : 0x50;
        if (v) { v->x = o->x; v->y = o->y; v->z = o->z; }
        /* ^ engine repair: the diverting throws (0x19/0x33/0x1A) PARK the
         * hidden victim up to 0x100 away (carry_at "clear of play") — the
         * stock carry keeps him at the hotspot. Without the re-seat a
         * fumbled throw resumed the pair at the park spot: "reversal
         * teleported us across the ring" (playtest 2026-08-23). */
        return cell;
    }
    if (--o->grap44 != 0) return cell;
    eng_announce(cur_st, 0x0F, 0x1A);                      /* $1C15D2 = 0x0F1A */
    if (!v) { o->state = ST_STAND; return cell; }
    if (m <= 0x82) {                                       /* 0x1A7D0 */
        o->state = ST_MOVE; o->move_id = 0xC07Bu; o->grap44 = 1;
        v->state = ST_MOVE; v->move_id = 0x15;
        v->facing = o->facing ^ 0x8000u; v->grap44 = 0;
        v->spr = (uint16_t)(0x39 | (v->facing & 0x8000u));
        o->spr = 0xFFFF;
    } else {                                               /* 0x1A8D8 family */
        o->state = ST_REACT; o->react_id = RC_GRABBED; o->dmg = 8;
        v->state = ST_MOVE; v->move_id = 0xC006u;
        o->spr = (uint16_t)(0x2B | (o->facing & 0x8000u));
        o->z += 0x48 << 16;
        if (m == 0x83) v->y += 1 << 16;
        v->spr = (uint16_t)(0x60 | (v->facing & 0x8000u));
        o->last_pair = (int)(v - cur_st->obj); v->last_pair = self_idx(o);
        if (m == 0x84 || m == 0x85) { o->role |= RF_ENGAGED; v->role |= RF_ENGAGED; }
        else { o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u; }
    }
    o->off_x = o->off_y = 0; v->off_x = v->off_y = 0;
    return cell;
}

/* 0x15006 — move 0x19 falling press slam (over-the-ropes variant TODO). */
static uint32_t handler_pressslam(eng_obj *o, uint32_t cell)
{
    eng_obj *v = (o->partner >= 0 && cur_st) ? &cur_st->obj[o->partner] : 0;

    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->off_x = 0x2E; o->off_y = 0x4E;
        if (v) {
            v->state = ST_HELD;
            carry_at(o, v, 0xF0, 0, 0);    /* park clear of play */
        }
        if (!throw_roll(o, 0xF4)) { o->state = ST_MOVE; o->move_id = 0x83; return cell; }   /* 0x15028/0x1503E */
        eng_announce(cur_st, o->wrestler, 0x07);       /* 0x1505C */
        return cell;
    }
    if (o->frame == 2 && o->count == 0)
        knockback(o, 0x16);            /* ATTACKER goes ballistic */
    if (o->frame == 3 && o->count == 0)
        o->off_x = 0x007F;             /* 0x15068: hotspot X = +0x7F from here */
    if (o->frame == 6 && o->count == 0) {                /* 0x151B4 */
        o->off_x |= 0x8000u;           /* +0x18 b7: offsets clear after the pass */
        add_pos_delta(o, 0x30, 0, 0);  /* step 0x30 back (0x10BD0 sign) */
    }
    if (o->frame == 3 && o->count == 0 && v && !(o->move_id & 0x8000u)) {
        /* 0x15094: probe 0x80 ahead; an X-edge crossing -> over the ropes */
        int32_t px = (o->x >> 16) + ((o->facing & 0x8000u) ? 0x80 : -0x80);
        int32_t py = o->y >> 16;
        int32_t xmin = ((py << 8) + 0x40000) / 0x2E0, xmax = -(((py << 8) - 0xA3000) / 0x2E0);
        if ((px < 0x220 && px < xmin) || (px > 0x2E0 && px > xmax)) {
            o->move_id |= 0x8000u;                         /* variant: +0x60
                                          byte b7 = word b15 (0x150CA) */
            o->spr = (uint16_t)(0x1FD | (o->facing & 0x8000u));
            v->state = ST_REACT; v->react_id = 0x25 | 0x2000u; v->grap44 = 0;
            v->facing = o->facing ^ 0x8000u;
            v->partner = -1;
            carry_at(o, v, 0x80, 1, 0x39);
            eng_ropes_arm(px > 0x2E0 ? 1 : 0, 1, 1);
            o->role &= (uint16_t)~0x40u; v->role &= (uint16_t)~0x40u;
            v->dmg = 0x14; v->st_flags |= SF_TIRED;
        }
    }
    if (o->landed && o->mover == 2) {
        eng_sound(0x29);
        o->mover = 0;
        o->count = 0;                  /* release the FF00 hold */
    }
    if (o->frame == 5 && o->count == 0 && v && !(o->move_id & 0x8000u)) {
        throw_release(o, v, 0x21, 0x14, 0xF0, 1, 0, 1); hit_count(v, 5); eng_throw_out(v);
        v->react_id |= 0x2000u;        /* rise dizzy + tired walk */
        v->st_flags |= SF_TIRED;
    }
    if ((o->frame & 0xFFu) == 0xFEu) {
        o->state = ST_GETUP;                  /* attacker rises from the mat */
        o->spr = (uint16_t)(0x68 | (o->facing & 0x8000u));
        o->partner = -1;
    }
    return cell;
}

/* Victim air reactions (grapple-moves.md §4) — each launches and lands
 * into the bounce/lying chain. */
static uint32_t handler_air13(eng_obj *o, uint32_t cell)   /* 0x1B73E backdrop */
{
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 6);
        edge_arc(o, 0x30);
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u))
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x10FC6 */
    if (o->landed) {
        eng_sound(0x29);
        o->state = ST_REACT;
        o->react_id = (uint16_t)((o->react_id & 0xFF00u) | 5);   /* move.b #5,+0x65: flags kept */
        o->spr_force = (uint16_t)(0x1F | (o->facing & 0x8000u));  /* 0x1B772 */
    }
    return cell;
}

static uint32_t handler_air16(eng_obj *o, uint32_t cell)   /* 0x1B8CA slam */
{
    if (!(o->anim_sel & 0x8000u)) {
        o->grap44 = 0;
        if (cur_st->scene != 1 && !(cur_st->g161 & 2u)      /* 0x1B8D2 cage / 0x1B8E0 ringside view: in-ring row */
            && exit_test(o, 0x9C000, 0x48000)) {         /* 0x1B8F8/0x1B916 */
            knockback(o, 0x20); o->role |= RF_OUTSIDE;         /* over the ropes */
            if (cur_st->g161 & 1u) o->st_flags |= SF_ELIMINATED;       /* rumble: eliminated (+0x32 b4) */
        } else {
            knockback(o, 0x0C);                          /* in-ring row */
            edge_arc(o, 0x30);
        }
        return cell;
    }
    if (o->zone == 1 && (o->clip & 0x03u))
        eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);   /* 0x10FC6 */
    if ((o->frame & 0xFFu) != 1) return cell;            /* 0x1B9C2 */
    o->floor42 = -0x20;                                  /* lands 0x20 early */
    if (!o->landed && o->zone == 3) {                      /* 0x1B5D4 / 0x1B9EA barrier */
        if (cur_st->g161 & 2u) { eng_sound(0x28); o->floor42 = 0; return cell; }   /* 0x1B65C / 0x1BA72: view showing */
        land_outside(o); return cell;                      /* 0x1B632 / 0x1BA48 -> 0x68 */
    }
    if (o->landed && (o->role & RF_OUTSIDE) && !(cur_st->g161 & 2u)) {   /* 0x1B5F8 / 0x1BA0E: outside, view NOT showing */
        land_outside(o); return cell;
    }                                                      /* else 0x1B604 / 0x1BA1A plain landing */
    if (o->landed) {
        o->state = ST_REACT;                                    /* 0x1BA1E */
        o->react_id = (uint16_t)((o->react_id & 0xFF00u) | 5);   /* move.b #5,+0x65: flags kept */
        o->spr_force = (uint16_t)(0x1F | (o->facing & 0x8000u));
        add_pos_delta(o, -0x38, 0, 0);
        eng_sound(0x29);
        o->floor42 = 0;
    }
    return cell;
}

static uint32_t handler_air21(eng_obj *o, uint32_t cell)   /* slam drop */
{
    (void)cell;
    cell = 0x1B1B8u;                   /* 0x1BD6C: art from the bounce cell */
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 3);
        o->mash_aa = o->hp == 0 ? 0x100
                   : (o->hp < 0x2A ? (uint16_t)(0x2A - o->hp) : 1);
    } else if (o->landed) {
        eng_sound(0x29);
        o->state = ST_REACT;                  /* straight to lying */
        o->react_id = (uint16_t)((o->react_id & 0xFF00u) | (o->band == 2 ? 9 : 8));   /* 0x1BDC0/0x1BDC8 move.b */
    }
    return cell;
}

static uint32_t handler_air22(eng_obj *o, uint32_t cell)   /* hip toss */
{
    if (!(o->anim_sel & 0x8000u)) {
        knockback(o, 0x18);
        edge_arc(o, 0x30);
    } else {
        if (o->frame == 0 && o->count == 0)
            o->z += 0x10 << 16;        /* 0x1BDE8: addi.w #$10,(+0x0E) */
        if (o->zone == 1 && (o->clip & 0x03u))
            eng_ropes_arm((o->clip & 0x01u) ? 1 : 0, 1, 1);
        if ((o->frame & 0xFFu) == 1) {
            o->floor42 = -0x20;
            if (o->landed) {
                eng_sound(0x29);
                o->floor42 = 0;
                o->state = ST_REACT;
                o->react_id = (uint16_t)((o->react_id & 0xFF00u) | 5);   /* move.b #5,+0x65: flags kept */
                o->spr_force = (uint16_t)(0x1F | (o->facing & 0x8000u));
                add_pos_delta(o, -0x38, 0, 0);
            }
        }
    }
    return cell;
}

/* 0x1B0CC — reaction 1 dizzy stagger: loop, hittable, timed out to stand. */
static uint32_t handler_dizzy(eng_obj *o, uint32_t cell)
{
    eng_lazy_divorce(o);               /* 0x1B0F2 */
    if (!(o->anim_sel & 0x8000u)) {
        o->mover = 0;
        o->hold_t = 0x80;              /* +0x46 duration */
        if (o->combo == 0) { o->combo = 4; o->combo_t = 0x40; }
    }
    o->atk = 1;                        /* hittable while dizzy */
    if (o->hold_t && --o->hold_t == 0)
        o->state = ST_STAND;
    return cell;
}

/* 0x1C03E — per frame, per object: pick cell, run handler, tick. Only the
 * state path exists yet (move/victim tables arrive with the HIT machine). */
void eng_anim_tick(eng_state *st, eng_obj *o)
{
    uint32_t cell;
    unsigned sel = o->anim_sel & 0xFFu;

    cur_st = st;
    if (o->throw_pend && (o->state & 0xFFu) != ST_REACT) o->throw_pend = 0;   /* armed/flying only while he is the throw's
                                          VICTIM (state 4): a stale arm fired on a later move's landing
                                          ("atomic drops cause weird physics", user 2026-08-30) */
    if (o->throw_pend == 2 && o->landed) {   /* throw_out mod: the slam's mat contact fires the
                                          over-the-rope arc - HERE, ahead of the slam reaction's
                                          own landing (the law pass runs before this, its handler
                                          after: a launch from the law was undone the same frame) */
        eng_throw_arc(o);
        eng_anim_latch(o);
        sel = o->anim_sel & 0xFFu;
    }
    if (sel == 0xFFu) {                /* hidden cell 0x125C0: BYPASSES the
                                          +0x21 guard (0x1C074-80) */
        o->spr = 0xFFFF;               /* handler 0x125CC */
        if (o->partner >= 0 && st->obj[o->partner].pinning)
            o->spr = (uint16_t)(((o->react_id & 0xFFu) == 9 ? 0x1E : 0x1D)
                                | (o->facing & 0x8000u)); /* move 0x4A cells
                                          0x180AC/0x180B8: composite pinned
                                          body (pin-exact.md §3) */
        o->mover = 0;
        o->anim_sel |= 0x8000u;
        return;
    }
    if ((o->state & 0xFFu) == 0xFFu)
        return;                                        /* 0x1C082 frozen */
    if (sel == 5 && (o->move_id & 0xFFu) == MOD_MOVE_CAGE_CLIMB)   /* a MOD move past the ROM's table: code-driven,
                                                                      the stand cell + a forced pose (modhooks.c) */
        cell = 0x114ACu;
    else if (sel == 5)                                 /* move cells 0x1C042 */
        cell = rom32(TAB_MOVE_CELLS + (o->move_id & 0xFFu) * 4u);
    else if (sel == 4)                                 /* victim cells 0x1C056 */
        cell = rom32(TAB_VICTIM_CELLS + (o->react_id & 0xFFu) * 4u);
    else {
        if (sel >= 13)
            sel = 0;
        cell = rom32(TAB_STATE_CELLS + sel * 4u);      /* 0x1C090 pick */
    }

    if (sel == 5) {                    /* pin-chain moves, routed by id */
        switch (o->move_id & 0xFFu) {
        /* 0xEF9A settles the id assignment: entry 8 keeps in box 1 (head
         * window) / degrades to stomp in box 3, entry 0x48 keeps in the
         * tight body box 2 / becomes the per-wrestler drop attack
         * (0xEFFE) in box 0xC — so 0x08 is the PICKUP, 0x48 the COVER. */
        case 0x08: cell = handler_pickup(o, cell); goto tick;   /* 0x1316C */
        case 0x48: cell = handler_cover(o, cell); goto tick;    /* 0x132D8 */
        case 0x29: cell = handler_knee(o, cell); goto tick;     /* 0x1656A */
        case 0x38: cell = handler_springup(o, cell); goto tick; /* 0x17AFE */
        case 0x78: cell = handler_rollaway(o, cell); goto tick; /* 0x1A186 */
        case 0x8B: cell = handler_winpose(o, cell); goto tick;  /* 0x1AD10 */
        case 0x1B: case 0x0D: cell = handler_topdive(o, cell); goto tick; /* 0x153BC */
        case 0x0F: cell = handler_splash(o, cell); goto tick;   /* 0x13F90 */
        case 0x0C: cell = handler_dive0C(o, cell); goto tick;   /* 0x13BC8 */
        case 0x4C: cell = handler_tag(o, cell); goto tick;      /* 0x18772 */
        case 0x4D: cell = handler_tagrecv(o, cell); goto tick;  /* 0x1892C */
        case 0x4E: cell = handler_enterring(o, cell); goto tick; /* 0x18A90 */
        case 0x4F: cell = handler_climbout(o, cell); goto tick; /* 0x18AFE */
        case MOD_MOVE_CAGE_CLIMB: cell = mod_cage_climb(o, cell); goto tick;   /* modhooks.c: the cage escape climb (mode cage_escape_win) */
        case 0x8C: cell = handler_losepose(o, cell); goto tick; /* 0x1AD76 */
        case 0x79: cell = handler_kingpose(o, cell); goto tick; /* 0x1A28E rumble winner pose */
        case 0x68: cell = handler_outside(o, cell); goto tick;  /* 0x1981E */
        case 0x7A: cell = handler_elim7A(o, cell); goto tick;   /* 0x1A348 rumble eliminated */
        case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x62: case 0x63:
            cell = handler_held(o, cell); goto tick;            /* 0x19530 */
        case 0x66: cell = handler_holdtag(o, cell); goto tick;  /* 0x1969E buckle tag */
        case 0x67: case 0x5C: case 0x59: cell = handler_escape(o, cell); goto tick;
        case 0x37: cell = handler_hold37(o, cell); goto tick;   /* 0x17A58 hold for the partner */
        case 0x61: cell = handler_held61(o, cell); goto tick;   /* 0x194D0 held for the partner */
        case 0x77: cell = handler_held77(o, cell); goto tick;   /* 0x1A154 stomped while held */
        case 0x69: cell = handler_climbin(o, cell); goto tick;  /* 0x199FC */
        case 0x70: cell = handler_wpickup(o, cell); goto tick;  /* 0x19E80 weapon pickup */
        case 0x1E: case 0x1F: cell = handler_wswing(o, cell); goto tick;   /* 0x159F8 weapon swing */
        case 0x6A: cell = handler_ringside6A(o, cell); goto tick; /* 0x19B3C */
        case 0x6B: cell = handler_ringside6B(o, cell); goto tick; /* 0x19BBA */
        case 0x35: cell = handler_legdrop(o, cell); goto tick;  /* 0x175A8 */
        case 0x0E: cell = handler_dive0E(o, cell); goto tick;   /* 0x13D26 */
        case 0x14: cell = handler_dive14(o, cell); goto tick;   /* 0x14B48 */
        case 0x46: case 0x47: cell = handler_dive46(o, cell); goto tick; /* 0x14424/0x14440 */
        case 0x0B: cell = handler_drop0B(o, cell); goto tick;   /* 0x13A1E */
        case 0x13: cell = handler_hop13(o, cell); goto tick;    /* 0x149E2 */
        case 0x20: cell = handler_runatk(o, cell); goto tick;   /* 0x16A06, same 0x16A16 */
        case 0x05: cell = handler_runstrike(o, cell); goto tick;
        case 0x2A: cell = handler_runshoulder(o, cell); goto tick;
        case 0x40: cell = handler_runjump(o, cell); goto tick;
        case 0x04: cell = handler_dropkick(o, cell); goto tick;
        case 0x41: cell = handler_runcatch(o, cell); goto tick;
        case 0x2E: cell = handler_tackle(o, cell); goto tick;    /* 0x16A7A */
        case 0x31: cell = handler_catchslam(o, cell); goto tick; /* 0x17000 */
        case 0x2D: cell = handler_runatk(o, cell); goto tick;
        case 0x21: cell = handler_swing(o, cell); goto tick;
        case 0x06: cell = handler_catch(o, cell); goto tick;
        case 0x0A: cell = handler_stomp(o, cell); goto tick;
        case 0x10: cell = handler_leap(o, cell); goto tick;
        case 0x22: cell = handler_mount(o, cell); goto tick;
        case 0x09: cell = handler_groundhold(o, cell); goto tick; /* 0x135EC */
        case 0x25: cell = handler_bearhug(o, cell); goto tick;    /* 0x1612E */
        case 0x36: cell = handler_mdd(o, cell); goto tick;        /* 0x1792A */
        case 0x1A: cell = handler_plex(o, cell); goto tick;       /* 0x15228 */
        case 0x23: cell = handler_quake23(o, cell); goto tick;    /* 0x15D08 */
        case 0x64: cell = handler_pinned64(o, cell); goto tick;   /* 0x19618 */
        case 0x5B: case 0x5A: case 0x55: case 0x56:
            cell = handler_escape2(o, cell); goto tick;
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
            cell = handler_fumble(o, cell); goto tick;            /* 0x1A7B4.. */
        case 0x6E: cell = handler_ko6E(o, cell); goto tick;       /* 0x19D82 */
        case 0x6F: cell = handler_ko6F(o, cell); goto tick;       /* 0x19E14 */
        case 0x8A: cell = handler_ko8A(o, cell); goto tick;       /* 0x1AC72 */
        case 0x39: cell = handler_apron39(o, cell); goto tick;    /* 0x17B70 apron punch */
        case 0x1C: cell = handler_behind1C(o, cell); goto tick;   /* 0x1567C behind grab (hold) */
        case 0x1D: cell = handler_behind1D(o, cell); goto tick;   /* 0x15870 behind grab (turn) */
        case 0x52: case 0x53: cell = handler_behindvictim(o, cell); goto tick;   /* 0x18D2E / 0x18E64 */
        case 0x57: case 0x58: cell = handler_behindesc(o, cell); goto tick;      /* 0x1914E / 0x191C8 */
        case 0x6D: case 0x76: cell = handler_behindoff(o, cell); goto tick;      /* 0x19D06 / 0x1A10E */
        case 0x65: case 0x73: case 0x74: cell = handler_dtbrace(o, cell); goto tick;   /* 0x19670 / 0x1A020 / 0x1A05C */
        case 0x17: cell = handler_whip(o, cell); goto tick;
        case 0x15: cell = handler_facelock(o, cell); goto tick;
        case 0x16: cell = handler_drag(o, cell); goto tick;
        case 0x7B: cell = handler_heldvictim(o, cell); goto tick;  /* 0x1A538 */
        case 0x7C: cell = handler_holdrev(o, cell); goto tick;     /* 0x1A5A0 */
        case 0x18: cell = handler_hiptoss(o, cell); goto tick;
        case 0x24: cell = handler_suplex(o, cell); goto tick;
        case 0x71: cell = handler_regain(o, cell); goto tick;   /* 0x19F1E REGAIN POWER flex */
        case 0x30: cell = handler_backdrop(o, cell); goto tick;
        case 0x19: cell = handler_pressslam(o, cell); goto tick;
        case 0x33: cell = handler_jakeslam(o, cell); goto tick;  /* 0x17282 */
        case 0x34: cell = handler_jakedrop(o, cell); goto tick;  /* 0x174AA */
        case 0x26: cell = handler_piledriver(o, cell); goto tick; /* 0x16258 */
        case 0x28: cell = handler_liftslam(o, cell); goto tick;  /* 0x16436 */
        case 0x02: cell = handler_throw02(o, cell); goto tick;   /* 0x12A08 */
        case 0x01: case 0x45: cell = handler_move01(o, cell); goto tick; /* 0x128FA */
        case 0x11: cell = handler_topdrop11(o, cell); goto tick; /* 0x14670 */
        case 0x2C: cell = handler_backbreaker(o, cell); goto tick; /* 0x168F8 */
        case 0x32: case 0x42: cell = handler_punches(o, cell); goto tick; /* 0x17194 */
        case 0x44: cell = handler_throw44(o, cell); goto tick;   /* 0x18668 */
        case 0x03: cell = handler_move03(o, cell); goto tick;    /* 0x12B16 */
        case 0x07: cell = handler_move07(o, cell); goto tick;    /* 0x13116 */
        case 0x27: cell = handler_move27(o, cell); goto tick;    /* 0x1639C */
        case 0x2F: cell = handler_gorilla(o, cell); goto tick;   /* 0x16C9E */
        case 0x3A: cell = handler_gutwrench(o, cell); goto tick; /* 0x17C18 */
        case 0x3F: cell = handler_ropestop(o, cell); goto tick;  /* 0x18370 rope stop */
        case 0x3B: cell = handler_rack3B(o, cell); goto tick;    /* 0x17D12 torture rack */
        case 0x8D: cell = handler_racklift8D(o, cell); goto tick;/* 0x17FB2 */
        case 0x12: cell = handler_moonsault(o, cell); goto tick; /* 0x14826 */
        case 0x2B: cell = handler_throw2B(o, cell); goto tick;   /* 0x1669A */
        case 0x43: cell = handler_throw43(o, cell); goto tick;   /* 0x1853C */
        case 0x51: cell = handler_squash(o, cell); goto tick;
        case 0x4A: cell = handler_struggle(o, cell); goto tick;
        case 0x4B: cell = handler_kickout(o, cell); goto tick;
        default: break;
        }
    }
    switch (CELL_HANDLER(cell)) {                      /* 0x1C0AA */
    case 0x000114B2u: cell = handler_stand(o, cell); break;
    case 0x00011652u: cell = handler_walk(o, cell); break;
    case 0x00012474u: cell = handler_lockup(o, cell); break;
    case 0x000124FAu: cell = handler_hold(o, cell); break;
    case 0x00011C82u: cell = handler_run(o, cell); break;
    case 0x00011D8Eu: cell = handler_skid(o, cell); break;
    case 0x00011DE0u: cell = handler_turn(o, cell); break;
    case 0x00012888u: cell = handler_jab(o, cell); break;
    case 0x00019FDAu: cell = handler_kick(o, cell); break;
    case 0x0001B090u: cell = handler_flinch(o, cell); break;
    case 0x0001B3DEu: cell = handler_divehit(o, cell); break;
    case 0x0001BE7Cu: cell = handler_react23(o, cell); break;
    case 0x0001BCAAu: cell = handler_react1C(o, cell); break;
    case 0x0001B684u: cell = handler_react0F(o, cell); break;
    case 0x0001B85Eu: cell = handler_react15(o, cell); break;
    case 0x0001BFA6u: cell = handler_react26(o, cell); break;
    case 0x0001BD0Eu: cell = handler_react1E(o, cell); break;
    case 0x0001BED8u: cell = handler_react24(o, cell); break;
    case 0x0001BB08u: cell = handler_react18(o, cell); break;
    case 0x0001B134u: cell = handler_fall(o, cell); break;
    case 0x0001B1D0u: cell = handler_bounce(o, cell); break;
    case 0x0001B318u: cell = handler_lying(o, cell); break;
    case 0x0001B440u: cell = handler_behind(o, cell); break;
    case 0x0001B288u: cell = handler_lyinghit(o, cell); break;
    case 0x0001B478u: cell = handler_grabbed(o, cell); break;
    case 0x00011F3Au: cell = handler_climb(o, cell); break;
    case 0x000121E6u: cell = handler_perch(o, cell); break;
    case 0x00012420u: case 0x000122E6u: cell = handler_climbdown(o, cell); break;
    case 0x0001B706u: cell = handler_caught(o, cell); break;
    case 0x0001BB4Cu: cell = handler_dumped(o, cell); break;
    case 0x0001B7A2u: cell = handler_caught41(o, cell); break;
    case 0x0001BBC6u: cell = handler_react1A(o, cell); break;
    case 0x0001BA92u: cell = handler_react17(o, cell); break;
    case 0x0001BC36u: cell = handler_react1B(o, cell); break;
    case 0x0001BF1Cu: cell = handler_react25(o, cell); break;
    case 0x0001B73Eu: cell = handler_air13(o, cell); break;
    case 0x0001B8CAu: cell = handler_air16(o, cell); break;
    case 0x0001BD6Cu: cell = handler_air21(o, cell); break;
    case 0x0001BDE8u: cell = handler_air22(o, cell); break;
    case 0x0001B0CCu: cell = handler_dizzy(o, cell); break;
    case 0x00011E42u: cell = handler_getup(o, cell); break;
    default:
        /* Untranscribed MOVE cells: play the anim, exit at the finished
         * sentinel like the jab/kick pattern so nothing soft-locks. Hit
         * windows land as each handler is transcribed (TODO per move). */
        if (sel == 5 && !(o->anim_sel & 0x8000u)) {
            o->mover = 0;              /* every ROM move handler clears +0x01 on its
                                          init frame (0x15684, 0x15878...): an untranscribed
                                          move used to keep the walk momentum and "slide"
                                          through its animation (user: Earthquake's behind
                                          grab 0x1C vs a dizzy man) */
            if (eng_dbgsel)
                fprintf(stderr, "anim: UNROUTED move %02X (cell handler %05X)\n",
                        o->move_id & 0xFFu, CELL_HANDLER(cell));
        }
        if (sel == 5 && (o->frame & 0xFFu) == 0xFEu) {
            o->state = ST_STAND;
            /* soft-lock guard: an untranscribed move must not leave its
             * partner hidden (state 0xFF) forever — stand him up. */
            if (o->partner >= 0 && cur_st) {
                eng_obj *p = &cur_st->obj[o->partner];
                if ((p->state & 0xFFu) == 0xFFu) {
                    p->state = ST_GETUP; p->grap44 = 0; p->partner = -1;
                    p->x = o->x; p->y = o->y; p->z = 0x140 << 16;
                    o->partner = -1;
                }
            }
        }
        if (sel == 4 && (o->frame & 0xFFu) == 0xFEu) {
            unsigned rl = o->react_id & 0xFFu;
            if (rl == 0x1A) {
                o->state = ST_REACT;          /* squashed-flat -> lying */
                o->react_id = RC_LYING;
            } else if (rl == 0x18) {
                o->state = ST_STAND;          /* runner stagger-past ends standing */
            } else {
                /* TODO EXACT: untranscribed reaction handlers 0x0F/0x15/
                 * 0x1B/0x1C/0x1E/0x24/0x26/0x27/0x29 (gap-map.md) — the
                 * record ran out; go down rather than freeze. */
                if (eng_dbgsel)
                    fprintf(stderr, "anim: UNROUTED react %02X ran out -> lying\n", rl);
                o->state = ST_REACT; o->react_id = RC_LYING; o->partner = -1;
            }
        }
        /* Soft-lock guard for records that only a handler can advance:
         * a 0xFF00 hold, a mode-0 (handler-driven) cell or a mode-2 loop
         * with no transcribed handler would sit forever. After 0x40
         * frames bail out: the move whiffs, the victim is put down.
         * TODO EXACT per id (gap-map.md lists them). */
        if ((sel == 5 || sel == 4) && (o->anim_sel & 0x8000u)
            && (o->count == 0xFF00u || CELL_MODE(cell) != 1)) {
            if (++o->whiff_t >= 0x40) {
                o->whiff_t = 0;
                if (eng_dbgsel)
                    fprintf(stderr, "anim: UNROUTED %s %02X stuck -> bail\n",
                            sel == 5 ? "move" : "react",
                            sel == 5 ? (o->move_id & 0xFFu) : (o->react_id & 0xFFu));
                if (o->partner >= 0 && cur_st) {
                    eng_obj *p = &cur_st->obj[o->partner];
                    if ((p->state & 0xFFu) == 0xFFu) {
                        p->state = ST_GETUP; p->grap44 = 0; p->partner = -1;
                        p->x = o->x; p->y = o->y; p->z = 0x140 << 16;
                    }
                    o->partner = -1;
                }
                if (sel == 5) { o->state = ST_STAND; o->mover = 0; }
                else          { o->state = ST_REACT; o->react_id = RC_LYING; o->z = 0x140 << 16; o->mover = 0; }
            }
        } else if (!(o->anim_sel & 0x8000u)) {
            o->whiff_t = 0;
        }
        break;
    }
tick:
    anim_seq_tick(o, cell);                            /* 0x1C0AC */
    /* A MOVE that has just ended (state 5 -> 0) drops its partner link at
     * the end of the frame: the ROM's move handlers finish with
     * `bset #7,(+0x26)` (0x128C0 jab, 0x129E8, ... ~120 sites) and 0x2930
     * clears the flagged link after the draw. Without it a ground attack
     * degraded to a jab (0xF02E) left a MUTUAL link that the lazy divorce
     * never breaks — the pair could never tie up again ("grappling stops
     * working"). Holds/covers that keep their link change to 0x0C/0xFF,
     * not 0, so they are untouched. */
    if (sel == 5 && (o->state & 0xFFu) == ST_STAND && o->partner >= 0)
        o->divorce = 1;
    if (o->spr_force) {
        o->spr = o->spr_force == ENG_SPR_STANCE ? (uint16_t)(o->facing & 0x8000u) : o->spr_force;
        o->spr_force = 0;
    }
    o->anim_sel |= 0x8000u;            /* mode-0 (handler-driven) cells
                                          latch here too: the handler has
                                          run its entry frame exactly once */
    if (o->off_x & 0x8000u) {                          /* 0x1C0B2 tail */
        o->off_x = 0;
        o->off_y = 0;
    }
}
