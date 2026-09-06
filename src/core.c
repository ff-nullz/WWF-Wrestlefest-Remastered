/* Deterministic engine core. No SDL, no wall clock, no floats.
 *
 * Frame order follows the ROM (scratchpad mv/frame-order.md): input ->
 * walk decisions -> camera -> per-object pass (state latch, bounds, anim,
 * screen pos, motion). Sprite compile happens render-side from sx/sy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"

int eng_dbgsel;
int eng_seated = 3;                    /* bit n = player n joined (START). The
                                          harness default seats both humans;
                                          the front end rebuilds it from the
                                          START scan (credit.c) */
int eng_front;                         /* main.c: the front end is live — a
                                          finished match returns to ATTRACT */
#include "engine.h"
#include "tbl.h"
#include "scene.h"
#include "credit.h"

/* ROM tables this file reads (docs/table-migration-brief.md). Extents from
 * maincpu.asm: each runs up to the next code/data block. The selector
 * matrices are [bank][band][id] / [band][bank][id] bytes; the per-wrestler
 * move map rows are reached through move_map_ptrs (values stay ROM
 * addresses, resolved by tbl_ra8). */
/* 0xEBC4 ladder (submissions.md §2d): a mashed-out held man's escape move */
unsigned eng_escape_of(unsigned m)
{
    switch (m) {
    case 0x5D: return 0x67;            /* ground hold */
    case 0x5F: return 0x5C;            /* mounted punches */
    case 0x60: return 0x5B;            /* bearhug */
    case 0x62: return 0x5A;            /* Slaughter's 0x3B */
    case 0x63: return 0x55;            /* Million Dollar Dream */
    case 0x5E: return 0x56;            /* pinned by the Perfect-plex */
    case 0x61: return 0x59;            /* held for the partner (0x37) */
    case 0x64: return 0x4B;            /* pinned by the 0x23 splash */
    default: return 0;
    }
}

static const tbl_def core_tables[] = {
    { "run_attack_matrix",      "base/selector", 0xE070, 0x48, TK_U8, 12,
      "0xE048 cat 1/2 (running attack): [bank 0/1 (+0x62 b7 bchg)][opp band 0..2][attacker wrestler id] -> category; ends 0xE0B7, code 0xE0B8" },
    { "throw_matrix",           "base/selector", 0xE232, 0x48, TK_U8, 12,
      "0xE20E cats A-D (stance 0x15/0x16 throw): [opp band 0..2][bank 0/1][attacker wrestler id] -> category; ends 0xE279, code 0xE27A" },
    { "run_catch_matrix",       "base/selector", 0xE33A, 0x48, TK_U8, 12,
      "0xE30C cat 3/4 (opponent running): [bank 0/1][opp band 0..2][attacker wrestler id] -> category; ends 0xE381, code 0xE382" },
    { "move_map_ptrs",          "base/selector", 0xE4FE, 12 * 4, TK_U32, 1,
      "0xE4FE per-wrestler pointer to his ws_move_map row (0xDF8E/0xF0B0 selector, 0x1E018 AI prep); 12 longs" },
    { "ws_move_map",            "wrestler",      0xE52E, 12 * 0x40, TK_U8, 0x40,
      "0xE52E..0xE82D per wrestler 0x40 bytes: [category 0..0x14][column B1/B2/both] -> move id, 0xFF = unrouted (run); code 0xE82E" },
    { "ws_prox_remap_move",     "wrestler",      0xEFFE, 12, TK_U8, 1,
      "0xEFEA proximity remap: the move a class-0x8x entry degrades to inside box 0xC, per wrestler id; code 0xF00A" },
    { "prox_class_by_move",     "base/selector", 0xF070, 0x4A, TK_U8, 1,
      "0xEFA6 proximity class per selector entry 0..0x49 (0 = no remap, b7 = tiered cover/stomp class); code 0xF0BA" },
    { "hold_escape_roll_cpu",   "base/selector", 0x113AC, 10 * 3, TK_U8, 3,
      "0x112C2 throw-roll entry 0 vs a CPU holder: [stage 0..9][band 0..2] threshold for rng&0xF; next row 0x113CA" },
    { "hold_escape_roll_human", "base/selector", 0x113CA, 3, TK_U8, 3,
      "0x112D4 throw-roll entry 0 vs a human holder: [band 0..2] threshold for rng&0xF; next row 0x113CD (anim.c throw roll)" },
    { "whip_escape_roll",       "base/selector", 0x1140F, 3, TK_U8, 3,
      "0x11368 roll entry 3 (rumble whipped-run escape, 0xF418): [band 0..2] threshold, success when rng&0xF <= byte (0x11396 bls); code 0x11412 follows" },
    { "walk_angle",             "base/walk",     0xF2D6, 16 * 2, TK_U16, 1,
      "0xF2B0/0xF48C walk angle (+0x2C) by held joystick nibble +0xA8 (codes 1..10 live, 16 slots); code 0xF2F6" },
    { "ws_max_energy",          "wrestler",      0x10830, 12 * 2, TK_U16, 1,
      "0x107AA human max/starting energy (+0x66/+0x72) per wrestler id; next 0x10848 = CPU energy by stage" },
    { "whip_ropestop_ptrs",     "base/selector", 0x1EE38, 10 * 4, TK_U32, 1,
      "0x1EDE4 whipped-run rope stop: per stage ($1C0162) the address of its band weight row; code 0x1EE60 is data" },
    { "whip_ropestop_rows",     "base/selector", 0x1EE60, 0x36, TK_U8, 6,
      "the 0x24CC d100 weight rows the ptrs select: [band*2] = {stop%%, rest} — bucket 0 = the rope stop fires (0x1EE04)" },
    { "match_scene_by_stage",   "base/scene",    0x0D12, 10, TK_U8, 1,
      "0xC98: match scene word low byte ($1C007F) per stage ($1C0163): 00 05 01 05 00 05 00 01 05 00 — the ladder's arenas (1 = cage)" },
};
TBL_REGISTER(core_tables)

/* ROM 0xF2D6 walk-angle by held joystick nibble (walk_angle table). */
/* 0xF354-0xF39C: the run's first-tick steer - stick L/R picks the heading and
 * the facing, no stick keeps the facing's way. Marks bit14 so the state
 * handler's own first tick does not steer again. */
static void run_steer_now(eng_obj *o)
{
    switch (o->joy & 3) {
    case 1: o->angle = 0x0040; o->facing = 0x8000; break;         /* 0xF37C */
    case 2: case 3: o->angle = 0x00C0; o->facing = 0x0000; break; /* 0xF38C */
    default: o->angle = (o->facing & 0x8000u) ? 0x0040 : 0x00C0; break;   /* 0xF39C */
    }
    o->state |= 0x4000u;
}

static unsigned walk_angle_at(unsigned joy) { return tbl16(TBL(walk_angle), (joy & 0xFu) * 2u); }

void eng_init(eng_state *st)
{
    eng_palsel_reset();                /* WF_PALSEL applies to front-less runs too */
    eng_init_picks(st, NULL);
}

/* `picks` is the character select's roster table $1C0598 (charselect.c):
 * P1, partner, CPU1, CPU2 — the slot order 0x10504 seats them in. NULL
 * keeps the harness default 0/2 vs 1/3. */
/* 0xC60 match-start tail, shared by tag and rumble: the match scene from
 * 0xD12[stage] (0xC98 writes it to $1C007F — the ladder's arenas, scene 1 =
 * the cage) and the stage's music word from 0xDEE (0xC74 posts it through
 * 0x2052). Stage = $1C0163 (campaign ladder; 0 outside it). */
static void match_stage_scene(eng_state *st)
{
    st->stage = (uint16_t)eng_camp_stage();
    if (getenv("WF_STAGE"))            /* harness poke, same as main.c's per-frame one */
        st->stage = (uint16_t)(atoi(getenv("WF_STAGE")) % 10);
    st->scene = tbl8(TBL(match_scene_by_stage), (st->stage & 0xFFu) % 10u);   /* 0xCA8 */
    /* The stage music word (0xDEE, table stage_bgm) is posted by
     * eng_intro_end (0xC7A), which both init paths reach. */
}

void eng_init_picks(eng_state *st, const int *picks)
{
    memset(st, 0, sizeof *st);
    eng_rank_elims_clear();            /* fresh +0xC4 scores (cleared with the objects) */
    st->scene = 0;
    st->cam_x = 0x01E0;                /* live-match rest, trace vleft f1300 */
    st->cam_y = 0x0230;
    match_stage_scene(st);

    if (eng_rumble_armed()) {          /* 0x10902: the Royal Rumble init (rumble.c) */
        static int hp4[4];
        if (getenv("WF_PICKS") && sscanf(getenv("WF_PICKS"), "%d,%d,%d,%d", &hp4[0], &hp4[1], &hp4[2], &hp4[3]) == 4)
            picks = hp4;               /* harness poke (matches the tag path below) */
        eng_rumble_init(st, picks, eng_seated);
        eng_referee_init(st);
        if (!eng_mode_rule(MODE_REF)) st->ref.active = 0;   /* mode: no referee */
        if (eng_mode_rule(MODE_WEAPONS))
            eng_weapons_spawn(st);     /* 0xCF4 -> 0xFFD2 (mode 0: no weapons) */
        eng_intro_begin(st);
        if (getenv("WF_NOINTRO")) eng_intro_end(st);
        return;
    }
    /* Tag setup 0x10504-0x106CE (tag-mode.md §1): slots 0/1 = P1 team,
     * 2/3 = P2 team; leaders legal + human, partners autopilot + recall.
     * Positions 0x10708. Wrestler ids: 0/2 vs 1/3 (select screen TODO). */
    {
        static const int16_t pos[4][2] = { {0x240,0x150},{0x220,0x160},{0x2C0,0x150},{0x2E0,0x160} };
        static const int def_ids[4] = { 0, 2, 1, 3 };
        int ids[4];
        /* 0x107AA: the ROM's per-id energy word is the fallback when the
         * wrestler package carries no hp stat (the first eng_init of a
         * run precedes the table load — tbl_bytes is NULL then). */
        int en_ok = tbl_bytes(TBL(ws_max_energy), NULL) != NULL;
        for (int i = 0; i < 4; i++)
            ids[i] = (picks && picks[i] >= 0 && picks[i] < ENG_WS_EXT_MAX) ? picks[i] : def_ids[i];
        if (getenv("WF_PICKS")) {                    /* harness: "a,b,c,d" seat ids */
            int a2, b2, c2, d2;
            if (sscanf(getenv("WF_PICKS"), "%d,%d,%d,%d", &a2, &b2, &c2, &d2) == 4)
                { ids[0] = a2; ids[1] = b2; ids[2] = c2; ids[3] = d2; }
        }
        if (getenv("WF_CLONE")) {          /* harness poke (like WF_STAGE): the CPU
                                              leader (slot 2) fights as this id —
                                              the only route to a clone slot 12..15
                                              until the select screen learns them */
            int c = atoi(getenv("WF_CLONE"));
            if (c >= 0 && c < ENG_WS_EXT_MAX) ids[2] = c;
        }
        /* MODE team sizes: 1 = singles, 2 = stock tag, 3 = SURVIVOR-style
         * trios; side B can differ (handicap). Slot map per side:
         * A = 0 (leader), 1, 4; B = 2 (leader), 3, 5. TEAMMATE LINKS ARE
         * CIRCULAR (0->1->4->0), so tags ROTATE through the team with the
         * stock swap untouched. */
        int sa = eng_mode_rule(MODE_TEAM_SIZE), sb = eng_mode_rule(MODE_TEAM_SIZE_B);
        static const int slot_of[2][3] = { { 0, 1, 4 }, { 2, 3, 5 } };
        if (sa < 1) sa = 1; else if (sa > 3) sa = 3;
        if (sb < 1 || sb > 3) sb = sa;
        for (int side = 0; side < 2; side++) {
            int n = side ? sb : sa;
            for (int k = 0; k < n; k++) {
                int i = slot_of[side][k];
                eng_obj *o = &st->obj[i];
                int id;
                if (i < 4) id = ids[i];
                else {                                 /* third man: the first id
                                                          nobody else uses */
                    int used[16] = {0};
                    for (int q = 0; q < ENG_MAX_OBJS; q++)
                        if (st->obj[q].active && st->obj[q].wrestler < 16) used[st->obj[q].wrestler] = 1;
                    for (int q2 = 0; q2 < 4; q2++) if (ids[q2] >= 0 && ids[q2] < 16) used[ids[q2]] = 1;
                    id = (eng_ws_base(ids[side * 2]) + 6) % 12;
                    for (int tries = 0; tries < 12 && used[id]; tries++) id = (id + 1) % 12;
                }
                int px = i < 4 ? pos[i][0] : (side ? 0x2F0 : 0x210);
                int py = i < 4 ? pos[i][1] : 0x170;
                o->active = 1;
                o->wrestler = id;
                o->state = ST_STAND;
                o->x = px << 16; o->y = py << 16; o->z = 0x140 << 16;
                o->facing = (uint16_t)(side == 0 ? 0x8000u : 0);
                o->partner = -1; o->last_pair = -1;
                memset(o->tries, 0, sizeof o->tries);     /* 0x8CBE-0x8CE6 clr.w +0xE8..+0xFC */
                o->hp = o->hp_max = (uint16_t)eng_pkg_stat((unsigned)id, "hp",
                                       en_ok ? (int)tbl16(TBL(ws_max_energy), (uint32_t)eng_ws_base(id) * 2u) : 100);
                o->teammate = n > 1 ? slot_of[side][(k + 1) % n] : -1;   /* circular */
                o->input = -1;
                if (side) o->role |= RF_SIDE;                /* side 1 */
                {   /* SEAT PORTS (user 2026-08-28 buy-in spec): when the select
                     * screen ran, every roster slot carries the port that picked
                     * it (eng_cs_ports: 1P = left leader, 2P = left partner,
                     * 3P/4P = the right team). Without the front end (harness)
                     * the old side layout stands: port 0 = left leader, port 1
                     * = right leader (WF_P2 scripts drive the opposition). */
                    const int *csp = eng_demo_active() ? NULL : eng_cs_ports();   /* the attract
                                          demo must not inherit the last game's seats */
                    int prt = csp ? (i < 4 ? csp[i] : -1)
                                  : (k == 0 && (eng_seated & (1 << side)) ? side : -1);
                    if (k == 0) o->role |= RF_LEGAL;          /* legal man */
                    if (prt >= 0) { o->role |= RF_PAD; o->input = prt; }   /* human pad */
                    else if (k == 0) {
                        /* 0x10504: a leader without a pad — his whole team is
                         * the CPU's when NO seat on the side joined; a human
                         * partner keeps the leader autopilot (he tags out) */
                        int side_human = csp && ((side ? 2 : 0) + 1 < 4)
                                       && (csp[side * 2] >= 0 || csp[side * 2 + 1] >= 0);
                        if (!side_human) o->cpu = 1;
                        else { o->driver |= DRV_AUTOPILOT; o->tag_flags |= TF_RECALL; }   /* autopilot leader */
                    }
                    else { o->driver |= DRV_AUTOPILOT; o->tag_flags |= TF_RECALL; }    /* autopilot partner */
                }
            }
        }
        if (eng_dbgsel)
            for (int i = 0; i < ENG_MAX_OBJS; i++) {
                eng_obj *o = &st->obj[i];
                if (!o->active) continue;
                fprintf(stderr, "init: o%d w%-2d side%d%s input=%d%s%s\n",
                        i, o->wrestler, (o->role & RF_SIDE) ? 1 : 0,
                        (o->role & RF_LEGAL) ? " LEGAL" : "", o->input,
                        o->cpu ? " CPU" : "", (o->driver & DRV_AUTOPILOT) ? " autopilot" : "");
            }
        st->obj[0].opp = 2; st->obj[2].opp = 0;
        if (st->obj[1].active) st->obj[1].opp = 2;
        if (st->obj[3].active) st->obj[3].opp = 0;
        if (st->obj[4].active) st->obj[4].opp = 2;
        if (st->obj[5].active) st->obj[5].opp = 0;
        /* MIRROR PICKS (user 2026-08-24): a later seat with the same id as
         * an earlier one becomes a runtime ALT clone — same wrestler,
         * auto-transformed palette. Stock never seats duplicates. */
        for (int i = 1; i < ENG_MAX_OBJS; i++) {
            eng_obj *o = &st->obj[i];
            if (!o->active || o->wrestler >= 12) continue;
            for (int j = 0; j < i; j++)
                if (st->obj[j].active && st->obj[j].wrestler == o->wrestler) {
                    int alt = eng_pkg_register_alt(o->wrestler);
                    if (alt >= 0) o->wrestler = (int16_t)alt;
                    break;
                }
        }
    }
    /* $1C0163: the campaign stage (match_stage_scene set it above). */
    eng_camp_hp(st);                   /* 0x10782 energy per stage / carry */
    eng_referee_init(st);
    if (!eng_mode_rule(MODE_REF)) st->ref.active = 0;   /* mode: no referee */
    if (eng_mode_rule(MODE_WEAPONS))
        eng_weapons_spawn(st);         /* 0xCF4 -> 0xFFD2 (mode 0: no weapons) */
    /* 0xC66: the ring intro 0xA654 runs before the live loop; its exit
     * (eng_intro_end) posts the clock-on bit 0x8B30 and the opening
     * bell. WF_NOINTRO=1 jumps straight to the live match. */
    eng_intro_begin(st);
    if (getenv("WF_NOINTRO")) eng_intro_end(st);
}

/* ROM 0x514E input digest. Engine input bit layout already matches the
 * ROM nibble: bit0 R, 1 L, 2 U, 3 D; b1/b2 at bits 4/5. Buttons go
 * through the ROM's odd-frame-OR / even-frame-consume accumulator so a
 * 1-frame tap always lands. Direction edges are computed per frame. */
static void input_scan(eng_obj *o, uint32_t bits, int odd_frame)
{
    uint16_t bt = (uint16_t)(((bits >> 4) & 7u)          /* 0x5194 buttons -> b0..b2 */
                           | (((bits >> 6) & 1u) << 8));  /* 0x5196 START -> b8 (0x100) */
    uint16_t lvl;

    o->joy_prev = o->joy;
    o->joy = (uint8_t)(bits & 0x0F);
    o->joy_new = (uint8_t)(o->joy & ~o->joy_prev);

    if (odd_frame) { o->btn_acc |= bt; lvl = o->btn_acc; }   /* 0x51A4 */
    else           { lvl = o->btn_acc; o->btn_acc = 0; }     /* 0x51B4 */
    o->btn_new = (uint16_t)(lvl & ~o->btn_held);             /* +0xA2 */
    o->btn_held = lvl;                                       /* +0xA6 */
}

/* Stand<->walk toggle (0xDCDE prefix: 0xDE34 start / 0xDE44 stop) and the
 * walk decision (0xF292 via state table 0xF1E4[1] sub 0): angle from
 * 0xF2D6 by held nibble; facing from horizontal input (the state-2 rule
 * 0xF37C-0xF398 — face_opponent 0x10BE8 takes over once an opponent
 * object exists). */
static void walk_logic(eng_state *st, eng_obj *o)
{
    unsigned state = o->state & 0xFFu;
    int ringside_gate = 0;

    if (state == ST_WALK && (o->sub & 0x7Fu) == 0x0Cu)
        return;                        /* 0xF244 state-1 input sub table 0xF25A:
                                          entry 0xC -> 0xF33A rts — the winner's
                                          ceremony walk ignores the pad */

    /* Attack selector 0xDE86: context-category chain (0xDF3A ->
     * 0xE002..0xE4BA, docs/engine-specs/strikes.md §1d) then row fetch
     * 0xDF96: entry = 0xE4FE[id][cat*3 + col], col B1/B2/both; 0xFF =
     * run. Implemented categories: 1/2 running (matrix 0xE070), 5/6/7
     * vs downed, 3/4 anti-run (0xE33A), 0x10 behind-standing, 0
     * fallback. Tie-up/stance/weapon/downed-self categories TODO. */
    /* 0xEBC4 pre-consumer: mashed-out pin victim escapes on the next
     * press (state 5 move 0x4A/0x51 -> kick-out 0x4B). */
    if ((o->btn_new & 3u) && (o->state & 0xFFu) == ST_MOVE
        && o->mash_aa == 0x4000u
        && ((o->move_id & 0xFFu) == 0x4A || (o->move_id & 0xFFu) == 0x51)) {
        o->mash_aa = 0;
        o->state = ST_MOVE;
        o->move_id = 0x4B;
        return;
    }
    /* 0xDD02: the TAG pre-consumer — A/B in the corner with the partner
     * waiting at the post -> move 0x4C; while HOLDING the stance 0x15/0x16
     * there -> move 0x66, the buckle TAG (double team, 0xDD9E). Runs BEFORE
     * the stance-throw consumer, as in stock (0xDD02 before 0xDDC8). */
    if (o->btn_new & 3u) {
        int t = eng_tag_trigger(st, o);
        if (t) {
            o->state = ST_MOVE; o->move_id = (uint16_t)(t == 2 ? 0x66 : 0x4C);
            if (t != 2) o->grap44 = 0;   /* 0x66 keeps the hold context */
            return;
        }
    }
    /* 0xEBC4 ladder: a mashed-out held man escapes on the next press
     * (0x5D->0x67, 0x5F->0x5C; the rest TODO as their holds land). */
    if ((o->btn_new & 3u) && (o->state & 0xFFu) == ST_MOVE && o->mash_aa == 0x4000u) {
        unsigned e = eng_escape_of(o->move_id & 0xFFu);
        if (e) { o->mash_aa = 0; o->state = ST_MOVE; o->move_id = (uint16_t)e; return; }
    }
    /* 0xDEBC -> 0xF0BA: the weapon PICKUP press consumer (before the
     * category chain, right after the 0xEBC4 escape ladder): ringside
     * scene showing, not already holding, OUTSIDE, state 0/1 — probe
     * both weapons and start move 0x70 (weapon.c). */
    if ((o->btn_new & 3u)                                       /* 0xF0BA (the scene/side
                                          tests moved into eng_weapon_pickup_try per
                                          weapon: INSIDE slots are picked up in the
                                          ring during normal play, user 2026-08-28) */
        && !(o->weapon_w & WPN_HELD)                                  /* 0xF0C4 */
        && ((o->state & 0xFFu) == ST_STAND || (o->state & 0xFFu) == ST_WALK) /* 0xF0D4/0xF0DC */
        && eng_weapon_pickup_try(st, o))
        return;
    /* 0xEC14/0xEC6A: a mashed-out downed man springs up (react 8 -> 0x38)
     * or rolls away (react 9 -> 0x78) on the next press. */
    if ((o->btn_new & 3u) && (o->state & 0xFFu) == ST_REACT && o->mash_aa == 0x4000u
        && ((o->react_id & 0xFFu) == RC_LYING || (o->react_id & 0xFFu) == 9)
        && !(o->opp >= 0 && (st->obj[o->opp].tag_flags & TF_PIN_INTENT))) {   /* 0xEBFA: the
                                          opponent's pin intent (+0x34 b4) pins
                                          him to the mat; no link test (0xEC0C) */
        o->mash_aa = 0;
        o->state = ST_MOVE;
        o->move_id = ((o->react_id & 0xFFu) == RC_LYING) ? 0x38 : 0x78;
        return;
    }
    /* 0xEA42 -> 0xEB8E: the man held in the facelock stance / drag walk
     * (state 5, move 0x7B) mashes B1/B2 to break out. `bset #7,(+0x60)`
     * at 0xEB9E is a test-and-set: ONE roll per hold — extra mashing is
     * dead until the holder stops walking, because move 0x15's entry
     * rewrites his +0x60 (0x14D36) and clears the latch. The roll is
     * 0x1129E entry 0 (0x112C0): rng(0x21B4) & 0x0F < threshold, the
     * threshold picked by the HOLDER's human/CPU flag (0x112CC).
     * Success substitutes move 0x7C — the reversal into the whip.
     * docs/engine-specs/hold-timeout.md §4. */
    if ((o->btn_new & 3u) && (o->state & 0xFFu) == ST_MOVE
        && !(o->weapon_w & WPN_HELD)         /* 0xEA46: not holding a weapon */
        && (o->move_id & 0xFFu) == 0x7B && !(o->move_id & 0x8000u)) {
        eng_obj *h = o->partner >= 0 ? &st->obj[o->partner] : 0;
        unsigned band = o->band & 0xFFu, th;
        o->move_id |= 0x8000u;                 /* 0xEB9E latch */
        if (band > 2) band = 2;
        if (h && h->cpu) {                     /* 0x112DC: stage*3 + band */
            unsigned stg = st->stage < 10 ? st->stage : 9u;
            th = (unsigned)eng_hold_rule(8 + (int)(stg * 3u + band),
                                         tbl8(TBL(hold_escape_roll_cpu), stg * 3u + band));
        } else {                               /* 0x112D4: band only */
            th = (unsigned)eng_hold_rule(5 + (int)band, tbl8(TBL(hold_escape_roll_human), band));
        }
        if ((eng_rng() & 0x0Fu) < th) {        /* 0x112F8 bcc = fail, bcs = win */
            if (eng_dbgsel)
                fprintf(stderr, "hold: P%d escape roll WON th=%u -> move 7C\n",
                        (int)(o - st->obj) + 1, th);
            o->state = ST_MOVE;
            o->move_id = 0x7C;
            return;
        }
        if (eng_dbgsel)
            fprintf(stderr, "hold: P%d escape roll lost th=%u\n",
                    (int)(o - st->obj) + 1, th);
    }
    /* Cats 0xA-0xD: a new press inside the stance 0x15/0x16 picks a
     * THROW from the 0xE232 matrix (opp band x alternation x wrestler):
     * w0 rows: hip toss / suplex / backdrop / press slam. */
    if ((o->btn_new & 3u) && (o->state & 0xFFu) == ST_MOVE
        && ((o->move_id & 0xFFu) == 0x15 || (o->move_id & 0xFFu) == 0x16)
        && o->partner >= 0) {
        eng_obj *v2 = &st->obj[o->partner];
        unsigned bank;
        int cat;
        o->grap44 = 0;
        o->alt62 ^= 0x80u;
        bank = (o->alt62 >> 7) & 1u;
        cat = (int)tbl8(TBL(throw_matrix), (unsigned)v2->band * 0x18u + bank * 0xCu
                                            + (unsigned)eng_ws_base(o->wrestler));   /* clones: base column */
        {   int oc = eng_ws_tmatrix((unsigned)o->wrestler, (unsigned)(v2->band > 2 ? 2 : v2->band), bank);   /* package / grid override */
            if (oc >= 0) cat = oc; }
        if (eng_mod_rule(MODR_GRAPPLE_PICK)) {
            /* MOD deterministic grapple: the stock 0xDD.. selection feels
             * random because the BANK alternates every press and the CAT
             * follows the victim's energy band. With the mod on, the HELD
             * DIRECTION picks the matrix row instead — neutral/UP/DOWN =
             * the band-0/1/2 throws (bank 0) — so the same input always
             * produces the same, stock-reachable throw. B1/B2/both still
             * pick the column. (user 2026-08-24: "more precision over
             * which move is produced based on direction and button") */
            unsigned dirband = (o->joy & 4u) ? 1u : (o->joy & 8u) ? 2u : 0u;
            cat = (int)tbl8(TBL(throw_matrix), dirband * 0x18u + (unsigned)eng_ws_base(o->wrestler));
        }
        {
            unsigned col = ((o->btn_held & 3u) == 3u) ? 2u
                         : (o->btn_new & 1u) ? 0u : 1u;
            uint8_t entry = (uint8_t)eng_ws_move8((unsigned)o->wrestler, (unsigned)cat * 3u + col);   /* package override, else the ROM row */
            if (getenv("WF_THROW"))    /* harness poke */
                entry = (uint8_t)strtoul(getenv("WF_THROW"), 0, 0);
            if (eng_dbgsel)
                fprintf(stderr, "throw: P%d cat=%X entry=%02X\n",
                        (int)(o - st->obj) + 1, cat, entry);
            if (entry != 0xFFu) {
                o->state = ST_MOVE;
                o->move_id = entry;
                return;
            }
        }
    }
    /* Stance <-> drag-walk toggle (0xDDC8): stick held during 0x15 ->
     * 0x16; released during 0x16 -> back to 0x15 (no-init re-entry). */
    if ((o->state & 0xFFu) == ST_MOVE && (o->state & 0x8000u)) {
        if ((o->move_id & 0xFFu) == 0x15 && o->joy != 0) {
            o->state = ST_MOVE;
            o->move_id = 0x16;
            o->angle = (uint16_t)walk_angle_at(o->joy);
            return;
        }
        if ((o->move_id & 0xFFu) == 0x16) {
            if (o->joy == 0) {
                o->state = ST_MOVE;
                o->move_id = 0x15;
                o->grap44 = 1;         /* seamless re-entry */
                return;
            }
            o->angle = (uint16_t)walk_angle_at(o->joy);
        }
    }
    if (o->result)
        return;                        /* 0xDCE8: match decided — pad dead */
    /* Category 9 — grapple moves from the hold (0xE1B0: state 0x0C in
     * the +0x45==1 window). B2 = the Irish whip 0x17 (real); B1 = a
     * PLACEHOLDER knockdown until the throw ladder (0x15 facelock ->
     * 0x18/0x24/0x30/0x19) is implemented. */
    if ((o->btn_new & 3u) && (o->state & 0xFFu) == ST_HOLD && o->hold_ph == 1
        && !o->pinning && o->partner >= 0) {
        eng_obj *v = &st->obj[o->partner];
        o->grap44 = 0;                 /* 0xE1B0 clr.w +0x44 */
        (void)v;
        if (o->btn_new & 2u) {
            o->state = ST_MOVE;              /* Irish whip */
            o->move_id = 0x17;
        } else {
            o->state = ST_MOVE;              /* front facelock stance (row 9 B1) */
            o->move_id = 0x15;
        }
        return;
    }
    if (state == ST_PERCH && (o->joy & 8u)) { o->state = ST_CLIMBDOWN; return; }   /* 0xF4AA */
    if (o->btn_new & 3u) {
        eng_obj *opp = o->opp >= 0 ? &st->obj[o->opp] : 0;
        int cat = -1, pinbreak = 0;
        unsigned id = (unsigned)eng_ws_base(o->wrestler);   /* matrix column (clones: base) */
        if ((state == ST_STAND || state == ST_WALK) && !(opp && eng_pin_is_pinner(opp))) {
            /* PIN BREAK target (playtest 2026-08-27 "sometimes i cannot stomp
             * off pins when i am over them", rumble + tag): the +0x7A link
             * is whoever the retarget last picked, not necessarily the man
             * covering - any ENEMY pinner whose pile (him or the man under
             * him) is within the break box is the target */
            for (int i = 0; i < ENG_MAX_OBJS; i++) {
                eng_obj *c = &st->obj[i];
                const eng_obj *u;
                if (!c->active || c == o || !eng_pin_is_pinner(c) || c->partner < 0) continue;
                if (!((c->role ^ o->role) & 0x80u) && !(st->g161 & 1u)) continue;   /* tag: enemies only */
                u = &st->obj[c->partner];
                if ((abs((int)(c->y >> 16) - (int)(o->y >> 16)) < 0x18 && abs((int)(c->x >> 16) - (int)(o->x >> 16)) < 0x50)
                    || (abs((int)(u->y >> 16) - (int)(o->y >> 16)) < 0x18 && abs((int)(u->x >> 16) - (int)(o->x >> 16)) < 0x50)) {
                    o->opp = i; opp = c; break;
                }
            }
        }
        if ((state == ST_STAND || state == ST_WALK) && opp
            && !((opp->state & 0xFFu) == ST_REACT && ((opp->react_id & 0xFFu) == RC_LYING || (opp->react_id & 0xFFu) == 9))) {
            /* ENGINE (user 2026-08-30): a DOWNED enemy inside the ground-attack
             * box takes the press even while the +0x7A link sits on the
             * standing legal man - the intruder I just floored gets my leg
             * drop / splash (the splash's pin gate keeps it a non-pin on a
             * non-legal man). TODO EXACT: stock reaches it because the legal
             * man is ENGAGED with my partner (0x20D28 -> the loose partner). */
            for (int i = 0; i < ENG_MAX_OBJS; i++) {
                eng_obj *c = &st->obj[i];
                if (!c->active || c == o || c->partner >= 0 || (c->state & 0xFFu) != ST_REACT) continue;
                if ((c->react_id & 0xFFu) != RC_LYING && (c->react_id & 0xFFu) != 9) continue;
                if (!((c->role ^ o->role) & 0x80u) && !(st->g161 & 1u)) continue;   /* enemies only */
                if (abs((int)(c->y >> 16) - (int)(o->y >> 16)) < 0x18 && abs((int)(c->x >> 16) - (int)(o->x >> 16)) < 0x50) {
                    o->opp = i; opp = c; break;
                }
            }
        }

        if ((o->weapon_w & WPN_HELD) && (state == ST_STAND || state == ST_WALK)) {
            cat = 0x11;                /* 0xE002: holding a weapon, state 0/1
                                          — the FIRST row of the 0xDF3A chain;
                                          cat 0x11 maps to the swing 0x1E */
        } else if (state == ST_PERCH && opp) {       /* 0xE27A: dives from the perch */
            unsigned ps = opp->state & 0xFFu, rc = opp->react_id & 0xFFu;
            if (st->g161 & 2u) o->role |= RF_OUTSIDE;   /* 0xE292: ringside scene — the diver
                                                     becomes an OUTSIDE man (floor z 0x100) */
            if (ps == ST_REACT && (rc == RC_LYING || rc == RC_LYING_B)) cat = 0x0F;
            else if (ps == ST_MOVE && (opp->move_id & 0xFFu) == 0x61) cat = 0x0F;   /* 0xE2B2 held man */
            else cat = 0x0E;           /* 0xE2D0: everything else is the standing dive */
        } else if (state == ST_RUN && !(o->tag_flags & TF_RECALL) && o->grap44 == 0) {
            /* 0xE02E cat 1/2 via 0xE070. 0xE032/0xE042: no running attack
             * with +0x34 b1 set or +0x44 != 0 — a whipped man carries his
             * rope-bounce budget in +0x44 (0x14E9C), so he cannot attack
             * until it is spent (the post-whip lockout). */
            unsigned bank;
            o->alt62 ^= 0x80u;
            bank = (o->alt62 >> 7) & 1u;
            if (opp)
                cat = (int)tbl8(TBL(run_attack_matrix), bank * 0x24u
                                + (unsigned)opp->band * 0xCu + id);
        } else if ((state == ST_STAND || state == ST_WALK) && ({
            /* ANY enemy pinner at the pile qualifies (user 2026-08-28
             * "sometimes neither side can stomp a pinner off"): o->opp
             * often still points at the VICTIM under the pile — retarget
             * to the pinner so the face-down row (stomp) opens. */
            eng_obj *pb = 0;
            for (int q2 = 0; q2 < ENG_MAX_OBJS && !pb; q2++) {
                eng_obj *c2 = &st->obj[q2];
                const eng_obj *pv;
                if (!c2->active || c2 == o || !eng_pin_is_pinner(c2)
                    || !((st->g161 & 1u) || ((c2->role ^ o->role) & 0x80u))) continue;
                /* the PILE is two men: the pinner lies 0x40 across the man
                 * under him, and a player walks up to the pinned man's head
                 * the way he would for a cover - measure against either */
                pv = (c2->partner >= 0 && c2->partner < ENG_MAX_OBJS) ? &st->obj[c2->partner] : c2;
                if ((abs((int)(c2->y >> 16) - (int)(o->y >> 16)) < 0x18
                     && abs((int)(c2->x >> 16) - (int)(o->x >> 16)) < 0x50)
                    || (abs((int)(pv->y >> 16) - (int)(o->y >> 16)) < 0x18
                        && abs((int)(pv->x >> 16) - (int)(o->x >> 16)) < 0x60)) pb = c2;
            }
            if (pb) { o->opp = (int)(pb - st->obj); opp = pb; }
            pb != 0; })) {
            /* PIN BREAK. The ROM's downed rows (0xE0B8/0xE110) need the
             * target in state 4 with no +0x26 link, which a covering man
             * never is — but the ROM's own pin-hold (0x15F22) puts hit
             * record 0x1D on him, i.e. "hittable, and a hit frees the
             * pair" (reaction handler 9, 0x24BC2). Offering the face-down
             * row here is what makes the stomp 0x0A the natural pick for
             * the run-in man; the break itself is the 0x24C0C body in
             * tag.c eng_pin_break(). TODO EXACT: the ROM reaches this
             * through the generic strike categories, not a special row. */
            if ((st->g161 & 1u) && (o->btn_new & 2u) && opp->partner >= 0) {
                /* RUMBLE DOUBLE PIN (user-confirmed stock, 2026-08-24): B2 at
                 * the pile JOINS the cover instead of breaking it — the same
                 * second-cover the CPU helper fires (rumble.c 0x209D6 family:
                 * the target search hands you the PINNER; move 0x48 on the
                 * downed man piles on). TODO EXACT: the ROM reaches this
                 * through the cover row of the generic categories. */
                o->state = ST_MOVE; o->move_id = 0x48;
                o->partner = opp->partner;     /* the downed man under the pile */
                o->facing = (uint16_t)(((o->x >> 16) < (st->obj[opp->partner].x >> 16)) ? 0x8000u : 0);
                if (eng_dbgsel) fprintf(stderr, "rumble: P%d joins the cover (double pin)\n", (int)(o - st->obj) + 1);
                return;
            }
            cat = 6; pinbreak = 1;
            o->partner = o->opp;               /* 0xF178 link (the pile) */
        } else if ((state == ST_STAND || state == ST_WALK) && opp && eng_mod_rule(MODR_DT_STOMP)
                   && (opp->state & 0xFFu) == ST_MOVE && (opp->move_id & 0xFFu) == 0x61
                   && opp->partner >= 0 && opp->partner == o->teammate) {
            /* MOD dt_stomp: the opponent is HELD by my partner (the buckle
             * double team, 0x37/0x61) - offer the face-down row so the stomp
             * lands on him (handler_stomp's 0x1398C case -> his 0x77 flinch).
             * Stock's standing rows (0xE0B8/0xE110) need state 4 with no
             * link; only the perch dive row (0xE2B2) knows a held man, so a
             * stock entering partner kicks the air ("cant stomp", user
             * 2026-08-30). His +0x26 stays on the holder. */
            cat = 6;                   /* the face-down row: B1 = the stomp 0x0A */
            o->partner = o->opp;
        } else if ((state == ST_STAND || state == ST_WALK) && opp
                   && opp->partner < 0 && (opp->state & 0xFFu) == ST_REACT
                   && ((opp->react_id & 0xFFu) == RC_LYING
                       || (opp->react_id & 0xFFu) == 9)) {
            /* 0xE0B8/0xE110 cat 5/6/7: opponent lying */
            if ((opp->react_id & 0xFFu) == RC_LYING)
                cat = 5;
            else
                cat = opp->band == 0 ? 6 : 7;
            o->partner = o->opp;       /* 0xF178 link: +0x26 = A1, and A1's
                                          +0x26 = A0 when he has none — the
                                          lying man's CPU row (0x1DB28) reads
                                          it to roll against the pickup */
            if (opp->partner < 0) opp->partner = (int)(o - st->obj);
        } else if ((state == ST_STAND || state == ST_WALK) && opp
                   && (opp->role & RF_RUNNING)      /* 0xE2EA btst #4,(+0x33,A1): the
                                          RUNNING FLAG, not state 2 — it rides
                                          through the rope TURN (state 6) and
                                          the skid, so a close whip's returning
                                          man still selects the anti-run rows
                                          ("tries to throw just a standard
                                          punch", playtest 2026-08-24) */
                   && !(o->tag_flags & TF_WORRY)) {    /* 0xE2E0 btst #5,(+0x34,A0) */
            unsigned bank;             /* 0xE2DC cat 3/4 via 0xE33A */
            o->alt62 ^= 0x80u;
            bank = (o->alt62 >> 7) & 1u;
            cat = (int)tbl8(TBL(run_catch_matrix), bank * 0x24u
                            + (unsigned)opp->band * 0xCu + id);
            o->partner = o->opp;
        } else if ((state == ST_STAND || state == ST_WALK) && opp
                   && (opp->state & 0xFFu) == ST_REACT
                   && (opp->react_id & 0xFFu) == RC_DIZZY
                   && ((o->facing ^ opp->facing) & 0x8000u) == 0
                   && abs((int)(opp->y >> 16) - (int)(o->y >> 16)) < 0x10
                   && abs((int)(opp->x >> 16) - (int)(o->x >> 16)) < 0x50) {
            cat = 0x10;                /* 0xE382: dizzy victim from behind */
            o->partner = o->opp;
            opp->combo = 0;
        } else if (state == ST_LOCKUP && opp && o->partner == o->opp
                   && !(o->grap44 & 0x2000u)           /* b13 impact pending */
                   && !(o->grap44 & 0x4000u)) {        /* b14 partner won */
            cat = 8;                   /* 0xE180: tie-up exchange press */
            opp->grap44 |= 0x4000u;    /* 0xE1A2: partner locked out */
        } else if (state == ST_STAND || state == ST_WALK) {
            cat = 0;                   /* 0xE4BA fallback */
        }
        if (cat >= 0) {
            unsigned col = ((o->btn_held & 3u) == 3u) ? 2u
                         : (o->btn_new & 1u) ? 0u : 1u;
            uint8_t entry = (uint8_t)eng_ws_move8((unsigned)o->wrestler, (unsigned)cat * 3u + col);   /* package override, else the ROM row */
            if (getenv("WF_ENTRY") && entry != 0xFFu && o == &st->obj[0]) {
                unsigned pc, pe;       /* harness poke "cat:entry" (hex):
                                          P1's selector entry for one category */
                if (sscanf(getenv("WF_ENTRY"), "%x:%x", &pc, &pe) == 2 && pc == (unsigned)cat)
                    entry = (uint8_t)pe;
            }
            if (eng_dbgsel && opp)
                fprintf(stderr, "sel: P%d cat=%X col=%u entry=%02X dx=%d dy=%d "
                        "oppst=%02X opprc=%02X oppfc=%d\n",
                        (int)(o - st->obj) + 1, cat, col, entry,
                        (int)(o->x >> 16) - (int)(opp->x >> 16),
                        (int)(o->y >> 16) - (int)(opp->y >> 16),
                        opp->state & 0xFF, opp->react_id & 0xFF,
                        (opp->facing >> 15) & 1);
            if (pinbreak && entry != 0xFFu) {
                /* the break IS the stomp (0x0A on the covering man's 0x1D
                 * record, reaction handler 9): the 0xEF9A proximity remap
                 * below measures against the PINNER, who lies 0x30 px off
                 * the pile, so from the far side it degraded the press to
                 * a jab thrown over the pair ("they just punch above me",
                 * playtest 2026-08-27) - the pile box above already put us
                 * on the pair */
                o->state = ST_MOVE; o->move_id = 0x0A; o->grap44 = 0;
                return;
            }
            if (cat >= 5 && cat <= 7 && entry != 0xFFu && !(st->g161 & 1u)
                && (!(o->role & RF_LEGAL) || !(opp->role & RF_LEGAL)
                    || (o->role & RF_OUTSIDE) || (opp->role & RF_OUTSIDE))) {
                /* a press over a DOWNED man who cannot be pinned - either
                 * man illegal, or either man outside the ring - is a STOMP
                 * in stock (user, MAME side-by-side 2026-08-29: "it will
                 * only kick, or stomp"); the engine used to start the
                 * cover/pickup/leg drop and refuse the pin at the catch
                 * ("does the pin animation, both spring up"). Tag only:
                 * the rumble pins anyone (0x133A2 cue unconditional). */
                entry = 0x0A;
            }
            if (entry == 0xFFu) {
                o->state = ST_RUN;          /* run, 0xDFC8: also clr.l +0x26 */
                o->partner = -1;
                run_steer_now(o);      /* the ROM's run handler 0xF34C steers on THIS
                                          frame (state dispatch follows the press);
                                          steering at the next input pass drew 2 frames
                                          of the run sprite facing the old way when the
                                          stick pointed the other way (user 2026-08-29) */
                o->grap44 = 0;         /* a SELF-run starts with a clean +0x44:
                                          a stale pin/hold word here disabled the
                                          0xF42E reversal and fed the rope turn's
                                          budget decrement for thousands of
                                          bounces ("warrior is running over and
                                          over", playtest 2026-08-24) */
                if (opp && opp->partner == (int)(o - st->obj))
                    opp->partner = -1; /* drop the back-link the cat branch
                                          just made — a stale one on a LYING
                                          man blocked every later pin (his
                                          partner<0 gate): "sometimes i
                                          cannot pin a downed opponent"
                                          (rumble playtest 2026-08-24) */
                return;
            }
            /* 0xEF9A proximity remap (whiff prevention). Class byte from
             * 0xF070; tiered classes degrade cover->stomp etc.; outside
             * every window the press degrades to jab/kick/run. */
            if (opp && entry < 0x4A) {
                uint8_t cls = (uint8_t)tbl8(TBL(prox_class_by_move), entry);
                if (cls) {
                    int keep = 0;
                    if (!(cls & 0x80u)) {
                        keep = eng_prox_box(o, opp, cls);
                    } else {
                        unsigned b1 = (entry == 8) ? 1u : 0xCu;
                        if (eng_prox_box(o, opp, b1)) {
                            if (entry != 8)
                                entry = (uint8_t)tbl8(TBL(ws_prox_remap_move), (unsigned)eng_ws_base(o->wrestler));
                            keep = 1;
                        } else {
                            unsigned b2 = (entry == 8) ? 3u : 2u;
                            if (eng_prox_box(o, opp, b2)) {
                                if (entry == 8)
                                    entry = 0x0A;
                                keep = 1;
                            }
                        }
                    }
                    if (eng_dbgsel)
                        fprintf(stderr, "sel: remap cls=%02X keep=%d -> %02X\n",
                                cls, keep, entry);
                    if (!keep) {       /* 0xF02E fallback degrade */
                        if (opp && opp->partner == (int)(o - st->obj))
                            opp->partner = -1;   /* same back-link drop */
                        if ((o->btn_held & 3u) == 3u) {
                            o->state = ST_RUN;
                            o->partner = -1;
                            o->grap44 = 0;             /* same clean-run entry */
                        } else {
                            o->state = ST_MOVE;
                            o->move_id = (o->btn_new & 1u) ? 0x00 : 0x72;
                        }
                        return;
                    }
                }
            }
            o->state = ST_MOVE;              /* 0xDFE8 */
            o->move_id = entry;
            o->grap44 = 0;             /* state-5 entry zeroes +0x44 (the
                                          rope-run phase counter etc.) */
            return;
        }
    }
    /* 0xEF0A: ringside scene showing, legal man pushing on the ring-front
     * band (zone 4): outside + stick +y (nibble 4) held 4 frames (0xEF6A)
     * -> climb in 0x69; inside + nibble 8 -> climb OUT 0x6C (TODO EXACT:
     * move 0x6C untranscribed, the press is ignored). */
    if ((st->g161 & 2u) && (o->role & RF_LEGAL) && o->zone == 4 && (state == ST_STAND || state == ST_WALK)) {
        unsigned want = (o->role & RF_OUTSIDE) ? 4u : 8u;
        if (!(o->joy & want)) o->hold4_t = 0;
        else if (++o->hold4_t >= 4) {
            o->hold4_t = 0;
            if (o->role & RF_OUTSIDE) { o->state = ST_MOVE; o->move_id = 0x69; o->grap44 = 0; return; }   /* 0xEF50 */
        }
        ringside_gate = 1;
    }
    /* 0xEDC0: the turnbuckle climb gate (stick held against a corner) */
    if (ringside_gate) { /* 0xEF6A owns +0xAD this frame */ }
    else if (o->joy != 0 && (state == ST_STAND || state == ST_WALK) && !(o->role & RF_OUTSIDE)
        && (o->zone & 3u) && st->corner_bits == 0 && (st->g161 & 2u)) {
        /* 0xEDEA -> 0xEE96: ringside scene showing — two side posts, no
         * direction hold: x >= 0x3CF -> corner 9, x < 0x271 -> corner 8
         * (the 0x28480 walls at 0x3D0/0x270 supply zone 1). Oracle fuzz
         * seed 19 f2572: P1 pushed at 0x26E, stick left -> state 8, g44 8. */
        int32_t x = o->x >> 16;
        int c = x >= 0x3CF ? 9 : (x < 0x271 ? 8 : -1);
        if (c >= 0) {
            st->corner_bits |= (uint16_t)(1u << c);          /* 0xEECE claim */
            o->grap44 = (uint16_t)c; o->atk = 0; o->state = ST_CLIMB;
            return;
        }
    }
    else if (o->joy != 0 && (state == ST_STAND || state == ST_WALK) && !(o->role & RF_OUTSIDE)
        && (o->zone & 3u) && st->corner_bits == 0) {
        static const struct { int c; int xlo; int16_t xlim, ylim; int ylo; uint8_t mask; } W[4] = {
            {2,1,0x1C5,0x11A,1,8},{3,0,0x32B,0x11A,1,8},{0,1,0x1F1,0x197,0,4},{1,0,0x2FC,0x197,0,4} };
        int32_t x = o->x >> 16, y = o->y >> 16;
        int hit = 0;
        for (int i = 0; i < 4 && !hit; i++) {
            int xin = W[i].xlo ? x < W[i].xlim : x >= W[i].xlim;
            int yin = W[i].ylo ? y < W[i].ylim : y >= W[i].ylim;
            if (!(xin && yin)) continue;
            if (!(o->joy & W[i].mask)) { o->hold4_t = 0; break; }
            if (++o->hold4_t < 4) break;                      /* 0xEF6A: 4 frames */
            o->hold4_t = 0;
            st->corner_bits |= (uint16_t)(1u << W[i].c);      /* 0xEECE claim */
            o->grap44 = (uint16_t)W[i].c; o->atk = 0; o->state = ST_CLIMB;
            hit = 1;
        }
        if (hit) return;
    } else o->hold4_t = 0;
    if (o->joy != 0 && state == ST_STAND)
        o->state = ST_WALK;                  /* bit15 clear => new state latch */
    else if (o->joy == 0 && state == ST_WALK)
        o->state = ST_STAND;

    if (mod_exit_ring_gate(st, o)) return;   /* modhooks.c: exit_ring* / cage_escape - hold into a rope to leave */

    /* Facing: face_opponent (0x10BE8) runs from both the stand (0xF236)
     * and walk (0xF2C2) handlers, right after the angle write. */
    if ((o->state & 0xFFu) == ST_WALK)
        o->angle = (uint16_t)walk_angle_at(o->joy);          /* 0xF2BC */

    /* State-2 run handler 0xF34C: the FIRST committed tick steers (bit14
     * latch) and returns; every later tick runs the reversal detector
     * 0xF42E — a run's direction is committed, the only way out is the
     * skid. Arriving from a turn (+0x1D == 6) skips the steer (0xF354). */
    if ((o->state & 0xFFu) == ST_RUN && (o->state & 0x8000u)) {
        unsigned prev = o->prev_sel & 0xFFu;
        if (!(o->state & 0x4000u)) {                           /* 0xF34C bset #6 */
            o->state |= 0x4000u;
            if (prev != 6 && o->grap44 == 0) {                 /* 0xF354: +0x1D = the
                                          PREVIOUS state — by this tick the run is
                                          already latched, so anim_sel is 2 and a
                                          held stick re-steered him back into the
                                          ropes (endless rebound) */
                switch (o->joy & 3) {
                case 1: o->angle = 0x0040; o->facing = 0x8000; break; /* 0xF37C */
                case 2: case 3:
                        o->angle = 0x00C0; o->facing = 0x0000; break; /* 0xF38C */
                default:                                              /* 0xF39C */
                    o->angle = (o->facing & 0x8000u) ? 0x0040 : 0x00C0;
                    break;
                }
            } else if ((o->angle & 0x7Fu) != 0x40u) {      /* not 0x40/0xC0 */
                /* ENGINE GUARD (stock invariant, not a ROM branch): every
                 * ROM path that skips the steer arrives horizontal — the
                 * turn resume flips 0x40<->0xC0 (0x11DEE bchg #7) and the
                 * whip writes 0x40/0xC0 (0x14E5A family) — so a run's
                 * angle is ALWAYS 0x40 or 0xC0 in stock. A stale diagonal
                 * here (transcription gap) never met the 0x11CC4 x-clip
                 * stop and ran pinned on the rope line forever; force the
                 * 0xF39C default instead. */
                if (eng_dbgsel)
                    fprintf(stderr, "run: P%d diagonal angle %02X at run "
                            "entry (g44=%X prev=%02X) forced horizontal\n",
                            (int)(o - st->obj) + 1, o->angle & 0xFF,
                            o->grap44, o->prev_sel & 0xFF);
                o->angle = (o->facing & 0x8000u) ? 0x0040 : 0x00C0;
            }
        } else if ((o->tag_flags & TF_RECALL) && o->grap44 == 0) {       /* 0xF3C6: the usher's recall
                                          (+0x34 b1) skids a free runner at once */
            o->state = ST_SKID;                                      /* 0xF3D4 */
        } else if ((o->joy & 3u) && (st->g161 & 1u)            /* 0xF3DE-0xF42C: RUMBLE, a
                                          whipped runner (+0x45 == 1) holding a
                                          direction rolls ONCE to skid out near
                                          his opponent. TODO EXACT: 0xF3DE reads
                                          the $A3.w global pad byte; the runner's
                                          own held nibble +0xA9 is used here */
                   && (o->grap44 & 0xFFu) == 1
                   && o->opp >= 0
                   && labs(((int32_t)(st->obj[o->opp].x >> 16))
                           - (int32_t)(o->x >> 16)) < 0x60     /* 0xF40A */
                   && !(o->hold_t & 0x100u)                    /* 0xF410 bset #0,+0x46 (byte) */
                   && (o->hold_t |= 0x100u,
                       (eng_rng() & 0x0Fu) <= tbl8(TBL(whip_escape_roll),
                                                   (o->band & 0xFFu) > 2 ? 2u : (o->band & 0xFFu)))) {
            o->state = ST_SKID;                                      /* 0xF426 (roll 0x1129E entry 3,
                                                                  0x11368: table 0x1140F[band]) */
        } else if ((o->grap44 & 0xFFu) == 1                    /* 0x1EDC4: the whipped-run
                                          ROPE STOP (move 0x3F) — a per-whip d100
                                          (0x24CC) under the 0x1EE38[stage][band]
                                          weight (~20/15/10%% by energy band).
                                          Stock gates it to 2P-vs ($1C007C,
                                          0x1EDC4) — the engine rolls for every
                                          whipped man (user request, TODO EXACT) */
                   && !(o->hold_t & 0x200u)
                   && (o->hold_t |= 0x200u, 1)) {
            unsigned d100 = (eng_rng() & 0xFFu) >> 1;          /* 0x24CC (retry quirk
                                                                  folded: >=100 -> 50) */
            uint32_t row = tbl32(TBL(whip_ropestop_ptrs),
                                 (uint32_t)(st->stage < 10 ? st->stage : 9u) * 4u);
            unsigned band = (o->band & 0xFFu) > 2 ? 2u : (o->band & 0xFFu);
            if (d100 >= 100) d100 = 50;
            if (getenv("WF_FORCE_ROPESTOP")) d100 = 0;         /* harness: always the stop */
            if (d100 < tbl_ra8(row + band * 2u)) {             /* bucket 0 = stop */
                o->grap44 = 0;                                 /* 0x1EE26 */
                o->state = ST_MOVE; o->move_id = 0x3F;               /* 0x1EE2A/0x1EE30 */
            }
        } else if (o->grap44 == 0 &&                           /* 0xF42E */
                   ((o->angle & 0x80u) ? (o->joy & 1u)         /* left: R held */
                                       : (o->joy & 2u))) {     /* right: L held */
            o->state = ST_SKID;                                      /* move.w #3,(+0x20) */
        }
    }
}

void eng_update(eng_state *st)
{
    eng_coin_tick(st->inputs);        /* IRQ2 0x434: coins land on every screen */
    if (eng_scene_update(st))          /* front-end scene owns the frame */
        return;
    /* 0x10F8: with $1C007C == 0 the frame loop is the ATTRACT DEMO — the
     * segment timer 0x119A[$1C0166] ends it (0x1102), the 0x8F50 wipe and
     * the START scan run instead of the result machine (attract.c). */
    if (eng_demo_active() && eng_demo_frame(st)) {
        st->frame++;
        return;
    }
    if (eng_intro_tick(st)) {          /* 0xA654 owns the frame until live */
        if (eng_front) { eng_credit_force(); eng_credit_line(); }   /* CREDIT line under the ceremony */
        eng_announce_tick(st);
        st->frame++;
        return;
    }
    eng_ai_frame(st);                  /* $1C0080 feeds the 0x21B4 RNG */
    for (int k = 0; k < ENG_MAX_OBJS; k++) {
        eng_obj *o = &st->obj[k];
        uint32_t bits = 0;
        /* rescue man inside (0x1D398): he stays AI-driven for the whole
         * $1C1682 grace, not just while ai_sub == 1 — firing a move
         * clears ai_sub and must not drop him back onto the apron subs. */
        int runin = (o->driver & DRV_AUTOPILOT) && !o->apron
                    && (o->ai_sub == 1 || o->ai_runin_t
                        || o->partner >= 0);   /* ENGAGED stays AI-driven: when the
                                          grace marker died mid-grapple the man fell
                                          to the apron machine, which rightly waits
                                          out engaged men — leaving a lockup/hold
                                          with nobody at the wheel ("two cpu
                                          wrestlers grappling forever", 2026-08-24) */
        if (!o->active || (o->st_flags & SF_QUEUED)) continue;   /* queued rumble entrant (0x1C17E) */
        if (((o->driver & DRV_AUTOPILOT)
             || (o->apron && (o->role & RF_PAD)))   /* a PAD-owning apron partner (2P buy-in)
                                          parks at the post via the SAME machine —
                                          his sub must reach 0x82 or eng_tag_trigger
                                          never fires ("could not tag my partner
                                          anymore", user 2026-08-28); the pad matters
                                          again once the tag swaps him in */
            && !runin && !(o->role & RF_LEGAL)) {
            /* a LEGAL autopilot man is never the apron machine's (stock
             * 0x1C16E: +0x56 b6 men are walker-driven) — the buckle-tag
             * double team (0x66) leaves the new legal man on b6 until a
             * tie-up releases him (0xF614) */
            if (!(st->g161 & 2u)) { eng_apron_tick(st, o); bits = 0; }   /* autopilot partner on the apron */
            else bits = eng_ai_inputs(st, o);   /* ring-out view: 0x1C16E "+0x56 b6 qualifies" —
                                                   the walker drives the parked 0x6B partners
                                                   (0x1C99E arms: target the other team's
                                                   partner, brawl once both are outside) */
        }
        else if (o->cpu || runin || (o->driver & DRV_AUTOPILOT))
            bits = eng_ai_inputs(st, o);   /* 0x1C15C walker — stock 0x1C16E: +0x56 b6
                                              qualifies, so the buckle-tag's legal
                                              autopilot man is AI-driven until released */
        else if (o->input >= 0) bits = st->inputs[o->input];
        input_scan(o, bits, (int)(st->frame & 1));
        eng_ringside_ai(st, o);        /* 0x1C6DC / 0x1D74A: CPU man sent home from ringside */
        if ((o->driver & DRV_AUTOPILOT) && !runin && !(o->role & RF_LEGAL) && !(st->g161 & 2u))
            continue;                  /* 0xF18A skips autopilot APRON men (the apron
                                          machine owns them); a legal b6 man walks on */
        if (!(o->role & RF_LEGAL) && !o->cpu && !(o->driver & DRV_AUTOPILOT) && (o->role & RF_PAD)
            && o->input >= 0 && !(st->g161 & 3u)
            && (o->apron || !o->ai_runin_t)) {
            /* a NON-LEGAL human in a tag game is the apron machine's man
             * whether he has reached the rail yet or not (the 0x11ABA
             * walk-out runs him off first) — EXCEPT the pin run-in partner
             * carrying the pad handed over at 0x21618: the rescue fire
             * (0x1D526) cleared his b6 so the user steers him (the engine
             * deviation on 0x1D546's unconditional bset), and routing him
             * into the apron machine here made its !apron branch (0x11ABA)
             * walk him straight back out over the ropes ("my teammate walks
             * into the ring, then back out", playtest 2026-08-24). While
             * his $1C1682 run-in grace marker is live he is a pad-driven
             * IN-RING man; the 0x212D4 restore (pin end) puts the pad home
             * and hands him to the change-up autopilot as before. */
            /* HUMAN apron man (mid-game buy-in, 0x6E9A): state-1 sub 1
             * routes his pad through 0xF2F6 (rail walk) and the selector's
             * apron category 0x13 (0xE496/0xE4A4) instead of the in-ring
             * walk logic. */
            eng_apron_press(st, o);    /* 0xE496 cat 0x13: punch 0x39 / grab 0x1C */
            eng_apron_tick(st, o);     /* 0x11796 machine (human steering branch) */
            continue;
        }
        walk_logic(st, o);
        if ((o->state & 0xFFu) <= 1) {
            if ((st->g161 & 2u) && (o->role & RF_OUTSIDE) && !o->cpu && !(o->driver & DRV_AUTOPILOT)) {
                /* Ringside brawl (user 2026-08-23, TODO EXACT): a HUMAN at
                 * ringside with more than one man around him steers his own
                 * facing — horizontal stick turns him, no auto-face. */
                if (o->joy & 1u) o->facing = 0x8000u;
                else if (o->joy & 2u) o->facing = 0;
            } else
                eng_face_opponent(st, o);  /* 0xF236 / 0xF2C2 */
        }
    }

    eng_start_buyin(st);               /* 0x8B3A START buy-in (ROM: right after the input poll) */
    eng_join_tick(st);                 /* 0x18C4 mid-game BUY-IN join (frame list 0x103E) */
    eng_tag_usher_tick(st);            /* $1C1682 grace -> f34 b1 recall (0x202BA) */
    eng_retarget_tick(st);             /* 0x2095A/0x20A3A: +0x7A refresh (fixation lock) */
    eng_tieup_scan(st);                /* pass 2, 0xF560 -> 0xF574 */
    eng_ringout_switch(st);            /* 0xF564 -> 0xF98C ring-out scene switch */
    eng_ringside_retarget(st);         /* outside men face the nearest OUTSIDE enemy */
    eng_floor_scene = st->scene;       /* 0x28124 dispatch key for the object pass */

    eng_camera_update(st);             /* before the object pass (0x26936) */
    eng_referee_frame(st);             /* 0x1F914 wrapper */
    eng_weapons_tick(st);              /* frame list 0xFFA/0x109E -> 0xFDEE */
    eng_rumble_tick(st);               /* 0xBB14 entrants / 0x20AA6 targets / 0x2017A (rumble only) */

    for (int k = 0; k < ENG_FX_SLOTS; k++)
        st->fx[k].active = 0;          /* 0x10E6A frees the companion slots
                                          once per frame (frame list 0x1006 —
                                          after the ROM's pass, but its draw
                                          lists already hold the pointers;
                                          the engine renders from state, so
                                          free BEFORE the pass re-spawns) */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active || (o->st_flags & SF_FROZEN) || (o->st_flags & SF_QUEUED))   /* 0xF4CE: queued entrant */
            continue;
        eng_anim_latch(o);             /* 0xF4E0 */
        o->zone = 0;                   /* 0xF518: cleared even when idle */
        o->clip = 0;
        o->landed = 0;
        if (o->mover)                  /* 0xF51C: probe only while moving */
            eng_ring_bounds(o);        /* 0x280DC/0x2818E (simplified) */
        eng_anim_tick(st, o);          /* 0x1C03E */
        eng_screen_pos(o, st);         /* 0x247C */
        eng_apply_motion(o);           /* 0x2208 — after screen pos, ROM order */
    }
    eng_hit_bookkeep(st);              /* 0x24090 (0xF510 epilogue) */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {   /* 0x2930: the deferred divorce (+0x26 b7) after the draw */
        eng_obj *o = &st->obj[i];
        if (o->divorce) { o->divorce = 0; if (!o->result) o->partner = -1; }
    }
    for (int i = 0; i < ENG_MAX_OBJS; i++) {   /* safety net (engine): a HELD man (0xFF) whose holder is
                                                  gone or no longer links back is released to stand — a
                                                  tag swap while his holder left (tag long run) froze
                                                  P1 in 0xFF for the rest of the match */
        eng_obj *o = &st->obj[i];
        if (!o->active || (o->state & 0xFFu) != 0xFFu) continue;
        if (o->partner < 0 || !st->obj[o->partner].active || st->obj[o->partner].partner != i
            || st->obj[o->partner].apron) {
            if (eng_dbgsel) fprintf(stderr, "core: P%d held by nobody (partner %d) -> released\n", i + 1, o->partner);
            o->state = ST_STAND; o->partner = -1; o->role &= (uint16_t)~0x40u; o->spr_force = 0;
        }
    }
    if ((st->frame & 1) == 0) {        /* hit-detect + drain live on the
                                          EVEN work list (frame-order §1) */
        eng_hit_scan(st);              /* 0x24062 */
        eng_damage_drain(st);          /* 0x24E58 */
    }
    /* 0x263D8 TIME UP: the object loop is done for this match — the ROM
     * spins while the overlay sits on screen and 0x26410 has already
     * stamped every live man 0x8005 / +0xA0 = 0x80, so the 0x1178 test
     * below fires on the next frame. */
    if (st->lock16c5) {
        if (!eng_demo_active() && eng_camp_end_test(st)) { eng_camp_tick(st); st->frame++; return; }   /* 0x10F8: demo skips 0x114C.. */
        st->frame++;
        return;
    }
    eng_hud_tick(st);                  /* 0x7506 / 0x7548 (odd frames) */
    eng_chips_tick(st);
    eng_comeback_tick(st);             /* 0xFD00: on fire / REGAIN POWER (anim.c) */
    eng_mod_energy_tick(st);           /* mod: unlimited energy (neutral in stock) */
    eng_backup_tick(st);               /* mod: random backup run-ins (neutral in stock) */                /* 0x8710 the 1P-4P / CHANGE OVER chips */
    eng_mash_overlay(st);              /* 0x899C get-up overlay (every frame) */
    if (!(st->frame & 1)) eng_powerup_flash(st);   /* 0x88A2 (even frames) */
    if (eng_front) { eng_credit_force(); eng_credit_line(); }   /* CREDIT line stays up in the match too */
    eng_clock_tick(st);                /* 0x262D2 (even frames) */
    eng_rumble_continue_tick(st);      /* 0xBCFE $1C0156 countdown + DQ plate (odd frames) */
    if (!eng_demo_active())
        eng_buyin_prompt(st);          /* 0xC710 coin / buy-in prompt (one slot per
                                          frame) — 0xC718 tst.w $1C007C: rts in the
                                          attract demo (no INSERT COIN row spam) */
    eng_announce_tick(st);             /* 0xA0E8 / 0xA0FE announcer */
    eng_grapple_gauge_tick(st);        /* mod hold-clock bar — LAST: the announce/
                                          overlay erasers must not wipe it */
    /* 0x1178: the two team leaders' +0xA0 bytes decide the match is over.
     * The win/lose poses (anim.c 0x1AD24 / 0x1AD7C) write them, so the
     * ring keeps running until the winner has raised his arms; then the
     * campaign machine (campaign.c) owns the frame: banners 0x11B6, then
     * the continue screen 0x1256 or the next stage 0x19BA. */
    if (!eng_demo_active() && eng_camp_end_test(st)) {   /* 0x10F8: the demo never
                                          reaches the 0x114C result region */
        eng_camp_tick(st);
        st->frame++;
        return;
    }
    st->frame++;
}
