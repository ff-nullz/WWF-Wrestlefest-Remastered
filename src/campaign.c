/* Campaign — the stock tag-team ladder: match over -> banners -> continue
 * screen or next stage -> ending ceremony.  Transcription of the ROM's
 * straight-line match-over path.
 *
 * Spec: docs/engine-specs/match-end.md (0x11B6..0x1396 timeline, blit ids,
 * pose -> +0xA0 writes) and docs/engine-specs/hud-rules.md §3.6/§4 (the
 * ladder, the opponent pick, the per-stage energy).
 *
 * ROM flow, PC-ordered:
 *
 *   0x1178   frame loop: ($1C0650 | $1C0868) & 0xC000 -> a team leader's
 *            +0xA0 byte carries 0x40 (won) / 0x80 (lost) -> 0x11B6.
 *   0x11B6   clear the live-human ptr list; wait 0x20; erase the result row
 *            (blit 0x8050); wait 0x20; CONGRATULATIONS 0x51 for a human
 *            winner (skipped at stage 9), GAME OVER 0x50 when nobody won;
 *            wait 0x40; clr $1C016C/$1C008C; wait 0x40; a human pair with
 *            +0xA0 bit7 -> 0x1256 continue screen, else 0x1396.
 *   0x1256   continue-screen build (scene 3, scroll 0x140/0x600, YM 0x3107,
 *            text set 2); per losing pair the "n PLAYER" plate 0x139C and
 *            the 0x1404 countdown loop (9 x 0x40 frames, digit 0x2067C into
 *            the $C1188 window, INSERT COIN 0x1C / PUSH nP BUTTON 0x1D/0x1E,
 *            START + a credit = continue -> $1C016C/$1C008C/+0x104).
 *   0x1396   jmp 0x19BA: music stop 0x8EB6; nobody won and nobody continued
 *            -> 0x6FC attract (GAME OVER).  A continued pair -> 0x1AF0
 *            jmp 0xAC0 with NO stage bump (same opponents: 0x10360 sees
 *            $1C008C).  Otherwise stage 9 -> 0x1B4E ceremony, else 0x1A6A
 *            bumps $1C0163/$1C0165 (dip bit7 skips a defence at 3/8),
 *            resets the faced list at stage 5, and jmp 0xAC0.
 *   0xAC0    -> 0xBD2: 0x7B70 aisle walk-in -> 0xC98 match init (which runs
 *            0x1086C -> 0x1034A: the opponent pick + the tag seating) ->
 *            0xA654 ring intro (VS card) -> live.  The engine's scene
 *            machine already routes MATCH through ENG_SCENE_AISLE and the
 *            intro, so "next match" here is eng_scene_set(ENG_SCENE_MATCH).
 *   0x1B4E   ending: scene 4, scroll (0x280,0x300), YM 0x11, nine name
 *            cards (blits 0x58..0x60 with the portrait row 0x4D at
 *            0x1DE8[i]), then the six-man walk-off (row 0x50 from
 *            0x1DD0[i], speed 0xC angle 0) and YM 0x1F -> 0x6FC attract.
 *
 * Everything the engine cannot see (the ROM's 6-object 1P layout, the
 * $1C0092 live-human pointer list, the vblank spins) is called out as
 * TODO EXACT below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "bus.h"
#include "engine.h"
#include "tbl.h"
#include "scene.h"
#include "credit.h"

/* Tables this file owns (docs/adr-001-data-formats.md). */
static const tbl_def camp_tables[] = {
    { "cpu_stage_energy",   "base/front", 0x10848, 10 * 2, TK_U16, 1,
      "0x107D2 CPU start/max energy (+0x66/+0x72) per stage $1C0162; the last CPU slot takes the full value, the other -15 (0x107EC); code 0x1085C follows" },
    { "stage_handicap_dip", "base/front", 0x1085C, 4 * 2, TK_S16, 1,
      "0x10806 difficulty-dip handicap added to the CPU energy, index ($1C0066 & 0x300) >> 8" },
    { "continue_port_map",  "base/front", 0x1884, 16 * 4, TK_U32, 2,
      "0x16BC continue screen: {port A, port B} 0x140020-space input addresses per pair (+0x20 = pair B) and loser code $1C0168 0..3" },
    { "ending_cards",       "base/front", 0x1DE8, 9 * 6, TK_U16, 3,
      "0x1BE8 ending ceremony: {x, y, portrait cell} per name card 0..8 (blit id 0x58 + i, sprite row 0x4D); code 0x1DF6.. is data only up to 0x1E20" },
    { "ending_walkoff_pos", "base/front", 0x1DD0, 6 * 4, TK_U16, 2,
      "0x1CCE ending walk-off: {x, y} per man 0..5 (sprite row 0x50, cell = index, speed 0xC angle 0); 0x1DE8 follows" },
};
TBL_REGISTER(camp_tables)

/* ---- 0x11B6 timeline waits (0x21E6 with D0 = n waits n+1 vblanks) ---- */
#define T_ERASE   0x21u                      /* 0x11CE wait 0x20 -> 0x11D8 */
#define T_RESULT  (T_ERASE + 0x21u)          /* 0x11E2 wait 0x20 -> 0x11EC */
#define T_CLRFLAG (T_RESULT + 0x41u)         /* 0x1222 wait 0x40 -> 0x122C */
#define T_BRANCH  (T_CLRFLAG + 0x41u)        /* 0x1238 wait 0x40 -> 0x1240 */
/* Engine-only guard (TODO EXACT): the ROM leaves 0x11B6 only through the
 * poses' +0xA0 writes.  A result that goes final while its winner cannot
 * reach the pose (an unrouted handler, a harness poke) would hang the
 * machine, so the flags are synthesised from +0xFE after this many frames
 * of a standing final result. */
#define A0_FALLBACK 0x200

#define BLIT_ERASE_ROW  0x8050u   /* 0x11D8: erase the 0x0E30 result row */
#define BLIT_CONGRATS   0x51u     /* 0x11EC "CONGRATULATIONS! / NOW GO FOR THE NEXT MATCH!" */
#define BLIT_GAMEOVER   0x50u     /* 0x1218 "GAME OVER" */
#define BLIT_PLAYERS    0x16u     /* 0x13C4: "1 PLAYER" .. 0x1B (pair B = +3) */
#define BLIT_INSERTCOIN 0x1Cu     /* 0x151C */
#define BLIT_PUSH1P     0x1Du     /* 0x1634: +D5 (pair B = +2) */
#define BLIT_PUSH2P     0x1Eu     /* 0x167E */
#define RUN_CONTINUE    0x14u     /* 0x1416: 0x9ACA glyph run at $C1118, pal nibble 0xF0 */
#define RUN_CONT_DST    0xC1118u
#define CONT_START      9u        /* 0x1404: $1C0080 = 9 */
#define CONT_TICKS      0x40u     /* 0x143A: $1C0082 wraps at 0x40 */
#define CONT_ROW_BASE   0x30u     /* 0x14A0/0x14A6: sprite row = id + 0x30 */
#define CONT_X_A        0x58      /* 0x147C */
#define CONT_X_B        0xE8      /* 0x1490 */
#define CONT_Y          0x88      /* 0x1482 */
#define ENDING_ROW      0x4Du     /* 0x1BDA */
#define ENDING_BLIT0    0x58u     /* 0x1BE4 D3 */
#define ENDING_CARDS    9         /* 0x1BE0 D1 = 8 */
#define ENDING_HOLD     0x81      /* 0x1C30 D0 = 0x80, dbra -> 0x81 frames */
#define ENDING_WALK_ROW 0x50u     /* 0x1CB4 */
#define ENDING_WALK_N   6         /* 0x1CE6 cmpi.b #6 */
#define ENDING_WALK_SPD 0xC       /* 0x1CC2 */
#define ENDING_WALK_MAX 0x501     /* 0x1CEC D1 = 0x500 */
#define ENDING_Y_LO     0x180     /* 0x1CFC */
#define ENDING_Y_HI     0x380     /* 0x1D04 */

/* ---- campaign state (the ROM's $1C0163/$1C0165/$1C0598/$1C008C/...) ---- */
enum { CAMP_IDLE, CAMP_OVER };

static struct {
    int      armed;          /* a game is running (the character select ran) */
    int      phase;          /* CAMP_IDLE / CAMP_OVER */
    unsigned t;              /* frames inside the 0x11B6 timeline */
    unsigned stage;          /* $1C0163 */
    unsigned played;         /* $1C0165 */
    int      picks[4];       /* the roster eng_init_picks seats */
    unsigned roster[10];     /* $1C0598 faced list */
    unsigned keep_cpu;       /* $1C008C: a continue keeps the same opponents */
    unsigned continued;      /* $1C016C */
    int      cont_pair[2];   /* a human pair lost and owes a continue */
    unsigned cont_code[2];   /* $1C0168 / $1C016A: which seats of the pair */
    int      cont_id[2][2];  /* the two wrestler ids per losing pair */
    int      pair_cont[2];   /* +0x104: this pair paid to continue */
    int      won;            /* 0x11EC: a human slot carried +0xA0 bit6 */
    int      hp_carry[4];    /* 0x10782: a human keeps his energy between matches */
    int      hp_valid;
    int      title_card;     /* 0x1A6A: run 0xB608 this transition (stage 4 won) */
    int      belt_idx;       /* 0x1A70: 0xBDA6 count index 0..2, -1 = skip */
} camp;

extern int eng_front;            /* core.c: the front-end scenes are live */

/* interlude.c IL_BRACKET (the 1v1 TOURNAMENT card): the road so far.
 * Fills up to 10 FACED ids (the $1C0598 pair leaders minus the human
 * picks), the NEXT opponent (this stage's CPU pick) and the stage. */
int eng_camp_bracket(int *stage, int faced[10], int *next_id)
{
    int n = 0;
    *stage = (int)camp.stage;
    *next_id = camp.picks[2];
    for (int k = 0; k < 10 && n < 10; k += 2) {
        int id = (int)camp.roster[k];
        if (id == 0xFFFF || id < 0 || id > 15) continue;
        if (id == camp.picks[0] || id == camp.picks[1]) continue;   /* the humans */
        if (id == camp.picks[2] || id == camp.picks[3]) continue;   /* the NEXT pair */
        faced[n++] = id;
    }
    return n;
}

static int dbg(void) { return getenv("WF_DBGSEL") != NULL; }

unsigned eng_camp_stage(void) { return camp.armed ? camp.stage : 0u; }
unsigned eng_camp_played(void) { return camp.played; }   /* $1C0165 (0x1A7E) */
int      eng_camp_armed(void) { return camp.armed; }
const int *eng_camp_picks(void)
{
    return camp.armed ? camp.picks : eng_cs_picks();
}

/* ---- 0x24CC: weighted draw over the byte list at `w` ---------------- */
static unsigned draw_1(uint32_t w)
{
    unsigned d = 0x32, i, acc = 0;
    for (int k = 0; k < 4; k++) {                      /* 0x24CC d100 shape */
        d = (eng_rng() & 0xFFu) >> 1;
        if (d < 0x64) break;
        d = 0x32;
    }
    if (d == 0) d = 1;
    for (i = 0; ; i++) {
        acc += tbl_ra8(w + i);
        if (acc >= d || i >= 11) break;
    }
    return i;
}

/* ---- 0x1034A/0x10390..0x104A4: the CPU team for this stage ----------
 * `roster` is $1C0598 (10 words, 0xFFFF empty): the human picks plus every
 * CPU pair already faced.  The winners go out through *w0/*w1 AND are
 * appended to the first free pair of roster words (0x1049C). */
void eng_camp_pick_cpu(unsigned stage, unsigned *roster, unsigned *w0, unsigned *w1)
{
    unsigned list[12], ln = 0, d0, d1;
    uint32_t wp;

    /* 0x10390: ids not in the faced list and not the LOD pair (4, 5) */
    for (unsigned id = 0; id < 12; id++) {
        int used = (id == 4 || id == 5);                       /* 0x103AE/0x103B4 */
        for (unsigned k = 0; k < 10; k++) if (roster[k] == id) used = 1;
        if (!used) list[ln++] = id;
    }
    /* 0x103CA: the weight list for this stage ($1C0162, -5 when >= 5) */
    wp = tbl32(TBL(cs_cpu_fill_weight_ptrs), (stage >= 5 ? stage - 5u : stage) * 4u);
    d0 = draw_1(wp);                                           /* 0x103E6 */
    d1 = draw_1(wp);                                           /* 0x103EE */
    if (d0 == d1) d1 += (d1 == 0) ? 1u : (unsigned)-1;         /* 0x103F8..0x10404 */
    if (ln == 0) { list[0] = 0; ln = 1; }                      /* engine guard: pool empty */
    *w0 = list[d0 % ln];                                       /* 0x10410 (no ROM bound check) */
    *w1 = list[d1 % ln];
    if (*w0 == 0xA || *w0 == 0xB || *w1 == 0xA || *w1 == 0xB)  /* 0x10418 */
        { *w0 = 0xA; *w1 = 0xB; }                              /* Demolition only as a pair */
    if (stage == 4 || stage == 9)                              /* 0x10438 */
        { *w0 = 4; *w1 = 5; }                                  /* the Legion of Doom */
    for (unsigned k = 0; k + 1 < 10; k++)                      /* 0x1047C..0x104A0 */
        if (roster[k] == 0xFFFFu) { roster[k] = *w0; roster[k + 1] = *w1; break; }
}

/* ---- charselect.c hands over the new game (0x5DAC + the first 0x10400) */
void eng_camp_new_game(const int *picks, const unsigned *roster)
{
    memset(&camp, 0, sizeof camp);
    camp.armed = 1;
    for (int i = 0; i < 4; i++) camp.picks[i] = picks ? picks[i] : -1;
    for (unsigned k = 0; k < 10; k++) camp.roster[k] = roster ? roster[k] : 0xFFFFu;
    if (dbg())
        fprintf(stderr, "camp: new game picks %d %d %d %d stage 0\n",
                camp.picks[0], camp.picks[1], camp.picks[2], camp.picks[3]);
}

/* ---- 0x10782: energy at match init ---------------------------------
 * Called at the end of eng_init_picks once the objects are seated.  A CPU
 * takes 0x10848[stage] (the last CPU slot the full value, the other -15)
 * plus the difficulty-dip handicap; a human keeps what he finished the
 * last match with unless he just paid to continue. */
void eng_camp_hp(eng_state *st)
{
    int first_cpu = -1;
    if (!camp.armed) return;
    for (int i = 3; i >= 0; i--)
        if (st->obj[i].active && st->obj[i & ~1].cpu) first_cpu = i;
    for (int i = 0; i < ENG_MAX_OBJS && i < 4; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active) continue;
        if (st->obj[i & ~1].cpu) {                             /* 0x107C8 */
            int e = (int)tbl16(TBL(cpu_stage_energy), camp.stage * 2u);
            if (i != first_cpu) e -= 0xF;                      /* 0x107E4: only $1C09E0 keeps it */
            /* 0x10806: the difficulty-dip handicap row, index
             * ($1C0066 & 0x300) >> 8 (dips.c; 0=normal 1=easy 2=hard
             * 3=hardest -> +0/-15/+15/+30). */
            e += tbl_s16(TBL(stage_handicap_dip), ((eng_dip_word() & 0x300u) >> 8) * 2u);
            if (e < 1) e = 1;
            o->hp = o->hp_max = (uint16_t)e;
            o->band = 0;                                       /* 0x10826 clr +0x70 */
        } else if (camp.stage != 0 && camp.hp_valid
                   && !camp.pair_cont[i >> 1] && camp.hp_carry[i] > 0) {
            o->hp = (uint16_t)camp.hp_carry[i];                /* 0x10792 -> 0x1082E */
            o->band = 0;
        }
    }
    if (dbg())
        fprintf(stderr, "camp: hp stage %u -> %u %u %u %u\n", camp.stage,
                st->obj[0].hp, st->obj[1].hp, st->obj[2].hp, st->obj[3].hp);
}

/* ================= 0x11B6 the match-over timeline ==================== */

/* A "human pair" is one of the ROM's slots $1C05B0 / $1C07C8 — the two
 * team leaders the banners and the continue screen test. The engine folds
 * the 1P CPU team ($1C09E0/$1C0AEC) onto slots 2/3, and only a LEADER
 * carries +0x56 bit7 (eng_init_picks), so the team is decided by its
 * leader (index & ~1). */
static int human_pair(const eng_state *st, int i)
{
    /* the engine's `cpu` flag TRAVELS with the legal man (eng_tag_swap:
     * in->cpu = out->cpu), so after a CPU tag the CPU LEADER reads cpu=0
     * and his pair passed as human: the CPU's win set camp.won and the
     * continue-screen timeout rolled into the next round instead of GAME
     * OVER (first loss only - the second loss found the leader legal
     * again; user 2026-08-30). A human team never carries the flag on
     * either man, so the pair is CPU when EITHER man is. */
    return st->obj[i].active && !st->obj[i & ~1].cpu && !st->obj[i | 1].cpu;
}
/* +0xA1 bit0 (0x10510/0x12F6): this seat is a live player, not the
 * autopilot partner.  The engine's flag for that is `input >= 0`. */
static int seated(const eng_obj *o) { return o->active && o->input >= 0; }

/* 0x1178: ($1C0650 | $1C0868) & 0xC000 — the two team leaders' +0xA0. */
int eng_camp_end_test(eng_state *st)
{
    if (camp.phase != CAMP_IDLE) return 1;
    if (((st->obj[0].a0flags | st->obj[2].a0flags) & 0xC0u) == 0) {
        /* engine guard, see A0_FALLBACK */
        int final = 0;
        for (int i = 0; i < ENG_MAX_OBJS; i++)
            if (st->obj[i].active && (st->obj[i].result & 0x8000u)) final = 1;
        if (!final) { st->over_t = 0; return 0; }
        st->over_t++;
        if (st->over_t == 30) {
            /* Half a second after the result went FINAL: dissolve the
             * open-ended pairs (holds, lockups, covers never end by
             * themselves, and the pads/AI are dead) so every man reaches
             * the stand and his win/lose pose — a decided match must let
             * the moves play out and the winners walk up for the high
             * five, not freeze until the 0x200-frame fallback (user
             * 2026-08-28). Timed moves (splash, slams...) finish on
             * their own inside the window. */
            for (int i = 0; i < ENG_MAX_OBJS; i++) {
                eng_obj *o = &st->obj[i];
                unsigned s8 = o->state & 0xFFu;
                if (!o->active) continue;
                if (s8 == 0x0B || s8 == 0x0C || s8 == 0xFFu) {
                    o->state = ST_STAND; o->partner = -1; o->grap44 = 0;
                    o->pinning = 0; o->cue_flags &= (uint16_t)~0x03u;
                    o->count = 0; o->spr_force = 0;
                    o->off_x = 0; o->off_y = 0;
                } else if (s8 == 5 && o->pinning) {   /* an undecided cover: clean release */
                    o->state = ST_STAND; o->partner = -1; o->pinning = 0;
                    o->cue_flags &= (uint16_t)~0x03u; o->count = 0;
                    o->off_x = 0; o->off_y = 0;
                }
            }
            if (dbg()) fprintf(stderr, "camp: final result - open pairs dissolved (f%lld)\n", (long long)st->frame);
        }
        if (st->over_t < A0_FALLBACK) return 0;
        for (int i = 0; i < ENG_MAX_OBJS; i++) {              /* TODO EXACT */
            eng_obj *o = &st->obj[i];
            if (!o->active || !(o->result & 0x8000u)) continue;
            o->a0flags = (uint16_t)((o->result & 1u) ? 0x80u : 0x40u);
            if ((o->result & 0xFFu) == 5u) o->a0flags = 0x80u;
        }
        if (dbg()) fprintf(stderr, "camp: +0xA0 fallback at f%lld\n", (long long)st->frame);
    }
    for (int p2 = 0; p2 < 2; p2++) {
        /* a PAIR wins or loses together (0x1AD50-0x1AD6E stamps all four men);
         * a result that stamped only one man left his partner carrying the
         * other verdict - a lost tag match then showed "NOW GO FOR THE NEXT
         * MATCH", took the continue screen and played on for free
         * (playtest 2026-08-27). The leader's flag is the pair's. */
        eng_obj *l = &st->obj[p2 * 2], *m = &st->obj[p2 * 2 + 1];
        unsigned f = (l->a0flags & 0xC0u) ? (l->a0flags & 0xC0u) : (m->a0flags & 0xC0u);
        if (f && l->active) l->a0flags = (uint16_t)f;
        if (f && m->active) m->a0flags = (uint16_t)f;
    }
    camp.phase = CAMP_OVER;
    camp.t = 0;
    if (dbg())
        fprintf(stderr, "camp: match over f%lld a0 %02X/%02X/%02X/%02X stage %u\n",
                (long long)st->frame, st->obj[0].a0flags, st->obj[1].a0flags,
                st->obj[2].a0flags, st->obj[3].a0flags, camp.stage);
    return 1;
}

/* 0x12E0..0x132C: per pair, which seats of it are live players. */
static void loser_codes(const eng_state *st)
{
    for (int p = 0; p < 2; p++) {
        const eng_obj *a = &st->obj[p * 2], *b = &st->obj[p * 2 + 1];
        camp.cont_code[p] = (seated(a) ? 1u : 0u) | (seated(b) ? 2u : 0u);
        camp.cont_id[p][0] = a->wrestler;
        camp.cont_id[p][1] = b->wrestler;
    }
}

/* 0x19BA..0x1AF0 — what happens after the banners (and after a continue
 * screen).  Returns the scene to run next. */
static int ladder_step(eng_state *st)
{
    int any_cont = camp.pair_cont[0] || camp.pair_cont[1];

    eng_sound(0x3100u); eng_sound(0x3120u);            /* 0x19BA jsr 0x8EB6 */
    if (!camp.won && !any_cont) {                      /* 0x19C0..0x19E8 */
        if (dbg()) fprintf(stderr, "camp: GAME OVER -> attract\n");
        /* 0x19E2 jsr 0x65C0: insert the seated players into the BEST
         * table; the name entry runs as the first attract page. */
        eng_rank_gameover_arm(st, eng_gs_rumble());
        camp.armed = 0;
        camp.phase = CAMP_IDLE;
        return ENG_SCENE_ATTRACT;                      /* jsr 0x65C0; jmp 0x6FC */
    }
    if (any_cont) {                                    /* 0x1A4A -> 0x1AF0 */
        camp.keep_cpu = 0x8000u;                       /* 0x15D6: 0x10360 keeps the pair */
        camp.hp_valid = 0;                             /* the continue restores full energy */
        camp.phase = CAMP_IDLE;
        if (dbg()) fprintf(stderr, "camp: continue -> rematch stage %u\n", camp.stage);
        return ENG_SCENE_MATCH;                        /* jmp 0xAC0, no stage bump */
    }
    if (camp.stage == 9) {                             /* 0x1A5E */
        camp.phase = CAMP_IDLE;
        if (dbg()) fprintf(stderr, "camp: stage 9 cleared -> ceremony\n");
        return ENG_SCENE_CEREMONY;                     /* 0x1B4E */
    }
    /* 0x1A6A: the between-match interludes (interlude.c).  0xB608 shows
     * the title-win card only when the stage word is 4 (the first LOD
     * title match was just won); 0xBDA6 shows the matches-until-the-title
     * count for stage 0..2 / 5..7 (0xBDDE..0xBDF2: 3/4 and >= 8 skip).
     * Both skip in the rumble ($1C0161 bit0, 0xB60C/0xBDAA). */
    {
        int title = (camp.stage == 4);                 /* 0xB618 cmpi.w #4,$1C0162 */
        int belt = -1;
        if (camp.stage < 3) belt = (int)camp.stage;    /* 0xBDDE bcs -> idx = stage */
        else if (camp.stage >= 5 && camp.stage < 8)
            belt = (int)camp.stage - 5;                /* 0xBDEE/0xBDF6 subi #5 */
        camp.title_card = title;
        camp.belt_idx = belt;
    }
    camp.stage++;                                      /* 0x1A76 $1C0163 */
    camp.played++;                                     /* 0x1A7E $1C0165 */
    /* 0x1A86: the "Championship Game 4th" dip (btst #7, $1C0066 — set =
     * switch OFF = "4th") skips a defence: stage 3 or 8 gets an extra
     * bump (0x1A90..0x1AA4), so the title matches come one stage sooner. */
    if ((eng_dip_word() & 0x8000u)
        && (camp.stage == 3 || camp.stage == 8))
        camp.stage++;                                  /* 0x1AA4 */
    if (camp.stage == 5) {                             /* 0x1AAC..0x1AEC */
        for (unsigned k = 0; k < 10; k++) camp.roster[k] = 0xFFFFu;
        {
            unsigned k = 0;
            for (int i = 0; i < 4 && k < 10; i++)
                if (human_pair(st, i)) camp.roster[k++] = (unsigned)(st->obj[i].wrestler & 0xFF);
        }
    }
    camp.keep_cpu = 0;
    camp.phase = CAMP_IDLE;
    /* 0xC98 -> 0x1086C -> 0x1034A: the opponents for the new stage. The ROM
     * picks them AFTER the aisle walk (0xBD2 runs 0x7B70 first); the aisle
     * only ever walks the human teams, so picking here is invisible.
     * TODO EXACT: the pick site. */
    {
        unsigned w0 = 0, w1 = 0;
        eng_camp_pick_cpu(camp.stage, camp.roster, &w0, &w1);
        camp.picks[2] = (int)w0;
        camp.picks[3] = (int)w1;
    }
    if (dbg())
        fprintf(stderr, "camp: stage %u opponents %d/%d (faced %X %X %X %X %X %X)\n",
                camp.stage, camp.picks[2], camp.picks[3], camp.roster[0], camp.roster[1],
                camp.roster[2], camp.roster[3], camp.roster[4], camp.roster[5]);
    /* 0xBD2: the new match opens with the LOD talk screen when the stage
     * is 4 or 9 (cmpi.b #4/#9 $1C0163 -> D0 = 0, jsr 0xAE20, jsr 0x264E2)
     * before the 0x7B70 aisle walk.  With the title card / belt scene it
     * makes the interlude queue; a continue rematch never reaches here
     * (0x1A4A -> 0x1AF0, and 0xAE20 exits on $1C008C). */
    if (eng_front && !eng_gs_rumble()) {
        int talk = (camp.stage == 4 || camp.stage == 9);
        int bracket = eng_mode_rule(MODE_TEAM_SIZE) == 1;   /* 1v1 TOURNAMENT card */
        {   /* a non-stock profile: the engine's own data scenes (sceneplay.c) in the ROM's order */
            int wf_sceneplay_arm(const char *const *types, int n, int belt_idx);
            const char *types[3]; int n = 0;
            if (camp.title_card) types[n++] = "title";
            if (camp.belt_idx >= 0) types[n++] = "belt";
            if (talk) types[n++] = "talk";
            if (n && !bracket && wf_sceneplay_arm(types, n, camp.belt_idx) > 0) return ENG_SCENE_PLAY;
        }
        if (eng_interlude_arm2(camp.title_card, camp.belt_idx, talk, bracket) > 0)
            return ENG_SCENE_INTERLUDE;
    }
    return ENG_SCENE_MATCH;
}

/* One frame of 0x11B6..0x1396.  The object pass no longer runs: the ROM
 * falls out of the frame loop into straight-line vblank waits, so the ring
 * is a frozen picture under the banners. */
void eng_camp_tick(eng_state *st)
{
    camp.t++;
    if (camp.t == T_ERASE)                             /* 0x11D8 */
        eng_blit(BLIT_ERASE_ROW);
    else if (camp.t == T_RESULT) {                     /* 0x11EC */
        camp.won = 0;
        for (int i = 0; i < 4; i += 2)                     /* the pair LEADERS (0x11EC reads +0xA0 of the team heads) */
            if (human_pair(st, i) && (st->obj[i].a0flags & 0x40u)) camp.won = 1;
        if (camp.won) {
            if (camp.stage != 9) eng_blit(BLIT_CONGRATS);   /* 0x1202: no card on the last */
        } else
            eng_blit(BLIT_GAMEOVER);                   /* 0x1218 */
    } else if (camp.t == T_CLRFLAG) {                  /* 0x122C */
        camp.continued = 0;
        camp.keep_cpu = 0;
    } else if (camp.t >= T_BRANCH) {                   /* 0x1240 */
        int lose = 0;
        /* the humans' energy carries into the next match (0x10782) */
        camp.hp_valid = 1;
        for (int i = 0; i < 4; i++) camp.hp_carry[i] = st->obj[i].hp;
        loser_codes(st);
        for (int p = 0; p < 2; p++) {
            const eng_obj *l = &st->obj[p * 2];
            camp.cont_pair[p] = human_pair(st, p * 2) && (l->a0flags & 0x80u) && camp.cont_code[p];
            camp.pair_cont[p] = 0;
            if (camp.cont_pair[p]) lose = 1;
        }
        eng_blit(BLIT_ERASE_ROW);                      /* the banners go with the screen */
        eng_blit(0x8000u | BLIT_CONGRATS);
        if (!eng_front) {
            /* Headless harness with no front-end screens: run the ladder
             * (0x19BA) but skip the aisle / continue / ceremony screens —
             * eng_init_picks below starts the next match straight away, as
             * the old core.c stand-in did. */
            if (!lose) ladder_step(st);
            camp.phase = CAMP_IDLE;
        } else if (lose) {                             /* 0x1256 */
            camp.phase = CAMP_IDLE;
            eng_scene_set(ENG_SCENE_CONTINUE);
        } else {                                       /* 0x1252 -> 0x1396 */
            int next = ladder_step(st);
            /* 0xAC0 dispatches on $1C007C (0xAF6/0xB02): the LADDER's next
             * match takes 0xB1C — interludes by $1C0166, match init, ring
             * intro, NO aisle. Only the 0xBD2 entry (the FIRST match, from
             * the character select) walks the 0x7B70 aisle — "we do the
             * aisle walkout for EVERY match; that only occurs on the first"
             * (user, stock-checked 2026-08-23). */
            eng_scene_set((eng_scene_id)next);
        }
        /* Fresh objects for whatever comes next (0xC98 rebuilds them). */
        {
            int64_t fr = st->frame;
            eng_init_picks(st, eng_camp_picks());
            st->frame = fr;
        }
    }
}

/* ================= 0x1256 / 0x1404: the continue screen ============== */

#define SCENE_WORD_CONT   3        /* 0x127E */
#define ARENA_PAL_CONT    3        /* 0x128C/0x1296 */
#define TEXT_SET_CONT     2        /* 0x12A0 */
#define SCROLL_X_CONT     0x140    /* 0x126E */
#define SCROLL_Y_CONT     0x600    /* 0x1276 */

static struct {
    int      pair;             /* which losing pair the loop is serving */
    unsigned n;                /* $1C0080 countdown */
    unsigned tick;             /* $1C0082 */
    int      done;             /* the countdown ran out */
    struct { int t22, t24; } man[2];   /* 0x1496/0x149C per loser sprite */
} cont;

/* 0x139C: erase the previous plate trio, then the "n PLAYER" plate. */
static void cont_plate(int pair)
{
    eng_blit(0x8000u | 0x18u);                         /* 0x139C */
    eng_blit(0x8000u | 0x1Du);                         /* 0x13A6 */
    eng_blit(0x8000u | 0x1Eu);                         /* 0x13B0 */
    /* 0x13E4: (code & 0xF) - 1 + 0x16, +3 for pair B.  (The 0x13C4 branch
     * is the 2-player-cabinet dip, $1C0066 & 0x800 — the engine plays the
     * 4-player plate path; DIP_PLAYERS exists in dips.c but this branch is
     * TODO EXACT, see docs/dip-switches.md.) */
    {
        unsigned code = camp.cont_code[pair] & 0xFu;
        if (code == 0) code = 1;
        eng_blit(BLIT_PLAYERS + code - 1u + (pair ? 3u : 0u));
    }
}

static void cont_begin(eng_state *st)
{
    memset(&cont, 0, sizeof cont);
    eng_sprite_scene_pals_begin();     /* 0x1456: the loop runs 0x2AEA #$1E every
                                          frame — the loser rows' baked palette
                                          ids install on demand (Slaughter's
                                          portrait drew from the body banks) */
    cont.pair = camp.cont_pair[0] ? 0 : 1;
    eng_ref_digit_wipe();                              /* 0x1256 0x206FE */
    eng_sound(0x3107);                                 /* 0x1264 */
    st->scene = SCENE_WORD_CONT;                       /* 0x127E */
    st->cam_x = SCROLL_X_CONT;                         /* 0x126E */
    st->cam_y = SCROLL_Y_CONT;
    eng_scene_publish(ARENA_PAL_CONT, TEXT_SET_CONT);  /* 0x128C..0x12A0 */
    memset(wf.spriteram, 0, sizeof wf.spriteram);      /* 0x12AC 0x1FDE */
    memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);/* 0x12B0 0x1F9E */
    cont_plate(cont.pair);                             /* 0x1332/0x135C -> 0x139C */
    cont.n = CONT_START;                               /* 0x1404 $1C0080 = 9 */
    cont.tick = 0;
    eng_banner_runs(RUN_CONT_DST, RUN_CONTINUE, 0xF0); /* 0x1416 0x9ACA */
    eng_credit_force(); eng_credit_line();
    if (dbg())
        fprintf(stderr, "cont: begin f%lld pair %d code %X ids %d/%d\n",
                (long long)st->frame, cont.pair, camp.cont_code[cont.pair],
                camp.cont_id[cont.pair][0], camp.cont_id[cont.pair][1]);
}

/* 0x160E: the PUSH nP BUTTON prompts and the START+credit test. */
static int cont_input(const eng_state *st)
{
    unsigned code = camp.cont_code[cont.pair] & 0xFu;
    unsigned d5 = cont.pair ? 2u : 0u;                 /* 0x162A */
    int pressed = 0;

    if (code & 1u) {                                   /* 0x1640 */
        unsigned id = BLIT_PUSH1P + d5;
        eng_blit(eng_can_afford() ? id : (0x8000u | id));   /* 0x1654 0xC404 */
    }
    if (code & 2u) {                                   /* 0x167C */
        unsigned id = BLIT_PUSH2P + d5;
        eng_blit(eng_can_afford() ? id : (0x8000u | id));
    }
    /* 0x16B8: the port(s) that answer for this loser code (0x1884). */
    for (int k = 0; k < 2; k++) {
        uint32_t port = tbl32(TBL(continue_port_map),
                              (cont.pair ? 8u : 0u) * 4u + code * 8u + (unsigned)k * 4u);
        int p;
        if (port < 0x140020u || port > 0x140026u) continue;
        p = (int)((port - 0x140020u) >> 1);
        if (p >= 0 && p < 4 && (st->inputs[p] & 0x40u)) pressed = 1;   /* START */
    }
    if (!pressed) return 0;
    /* 0x16F2 D0=1 -> 0x1700 jsr 0x55A: the continue price (SW1:5). The
     * D7==3 pair-continue passes D0=3 (two units, 0x5DE) — TODO EXACT. */
    if (!eng_take_continue()) return 0;
    return 1;                                          /* 0x1756 ori #1,CCR */
}

static int cont_update(eng_state *st)
{
    /* 0x1422: the countdown digit in the $C1188 window. */
    eng_count_digit(0x8000u | cont.n);
    if (++cont.tick >= CONT_TICKS) {                   /* 0x1432..0x144A */
        cont.tick = 0;
        if (cont.n == 0) cont.done = 1; else cont.n--;
    }
    for (int k = 0; k < 2; k++) {                      /* 0x14BA..0x14DE */
        if (--cont.man[k].t22 > 0) continue;
        cont.man[k].t22 = (int)(eng_rng() & 0xFu) + 8;
        cont.man[k].t24 ^= 1;
    }
    eng_blit(eng_can_afford() ? BLIT_INSERTCOIN : (0x8000u | BLIT_INSERTCOIN));  /* 0x151C */
    eng_credit_line();
    if (cont_input(st)) {                              /* 0x1556 -> 0x15CE */
        camp.continued = 0x8000u;                      /* $1C016C */
        camp.keep_cpu = 0x8000u;                       /* $1C008C */
        camp.pair_cont[cont.pair] = 1;                 /* +0x104 */
        camp.cont_pair[cont.pair] = 0;
        if (dbg()) fprintf(stderr, "cont: CONTINUE f%lld pair %d\n", (long long)st->frame, cont.pair);
        return ladder_step(st);
    }
    if (!cont.done) return -1;
    /* 0x156C: the countdown expired — that pair is wiped from the game. */
    camp.cont_pair[cont.pair] = 0;
    if (dbg()) fprintf(stderr, "cont: timeout f%lld pair %d\n", (long long)st->frame, cont.pair);
    if (cont.pair == 0 && camp.cont_pair[1]) {         /* 0x135C: pair B next */
        int64_t fr = st->frame;
        cont_begin(st);
        st->frame = fr;
        return -1;
    }
    return ladder_step(st);
}

/* 0x14E4: the two losers stand beside the counter (row = id + 0x30). */
static void cont_draw(const eng_state *st)
{
    unsigned slot = 0;
    unsigned code = camp.cont_code[cont.pair] & 0xFu;
    (void)st;
    memset(wf.spriteram, 0, WF_SPRRAM_SIZE);
    for (int k = 0; k < 2; k++) {
        int id = camp.cont_id[cont.pair][k];
        if (!(code & (1u << k)) || id < 0) continue;
        eng_sprite_emit_pose(eng_sprite_cont_row(id), (unsigned)cont.man[k].t24,   /* a clone: his own face cards (pak 800+) or the base's */
                             k ? CONT_X_B : CONT_X_A, CONT_Y, -1, &slot);
    }
    memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);
}

static const eng_scene_ops cont_ops = { cont_begin, cont_update, cont_draw };

/* ================= 0x1B4E: the championship ceremony ================= */

#define SCENE_WORD_END  4          /* 0x1B7A */
#define ARENA_PAL_END   4          /* 0x1B82/0x1B8A */
#define TEXT_SET_END    0          /* TODO EXACT: 0x1B4E never writes $1C15FC — the
                                      ending INHERITS the text set the previous
                                      screen left (2 from the match, 0xBC2).  The
                                      engine's publish always names one, so the
                                      ending pins set 0; only the FG0 name plate's
                                      palette line depends on it. */
#define SCROLL_X_END    0x280      /* 0x1B96 */
#define SCROLL_Y_END    0x300      /* 0x1B9E */

enum { END_CARDS, END_WALK, END_DONE };

static struct {
    int phase, card, t;
    int arm;                                   /* button released at least once */
    struct { int32_t x, y; int live; } man[ENDING_WALK_N];   /* 16.16 */
    int32_t px, py; unsigned pcell;            /* the portrait object */
} ceremony;

static int any_button(const eng_state *st)     /* 0x1C34..0x1C6A (b1|b2 of any port) */
{
    for (int p = 0; p < 4; p++) if (st->inputs[p] & 0x30u) return 1;
    return 0;
}

static void end_card(int i)
{
    ceremony.px   = (int32_t)tbl16(TBL(ending_cards), (unsigned)i * 6u);       /* 0x1BE8 +0x14 */
    ceremony.py   = (int32_t)tbl16(TBL(ending_cards), (unsigned)i * 6u + 2u);  /* 0x1BEC +0x16 */
    ceremony.pcell = tbl16(TBL(ending_cards), (unsigned)i * 6u + 4u);          /* 0x1BF0 +0x04 */
    eng_blit(ENDING_BLIT0 + (unsigned)i);                                      /* 0x1C00 */
}

static void end_begin(eng_state *st)
{
    memset(&ceremony, 0, sizeof ceremony);
    eng_sound(0x11);                                   /* 0x1B70 */
    st->scene = SCENE_WORD_END;                        /* 0x1B7A */
    st->cam_x = SCROLL_X_END;                          /* 0x1B96 */
    st->cam_y = SCROLL_Y_END;
    eng_scene_publish(ARENA_PAL_END, TEXT_SET_END);    /* 0x1B82/0x1B8A */
    /* 0x1B64 bsr 0x2988: the wipe clears $1C1600..$1C163F — the 0x2AEA
     * palette allocator starts EMPTY on this screen, so every id the
     * ending loads (0x40 + card portrait cell at 0x1BF6, 0x3B for the
     * credit pages at 0x1C94) gets a fresh sprite bank in load order.
     * Without this the portrait/credit art drew through a wrestler BODY
     * palette (bank = id & 0x0F) — the portraits came out invisible and
     * the credit roll came out as coloured noise. */
    eng_sprite_scene_pals_begin();
    memset(wf.spriteram, 0, sizeof wf.spriteram);
    memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);/* 0x1B60 0x1F9E */
    end_card(0);
    if (dbg()) fprintf(stderr, "ceremony: begin f%lld\n", (long long)st->frame);
}

static int end_update(eng_state *st)
{
    int held = any_button(st);
    /* the press that skipped the last screen must not carry in */
    if (!held) ceremony.arm = 1;
    if (held && ceremony.arm) {                        /* 0x1C6C/0x1D52 -> 0x1D72 */
        if (dbg()) fprintf(stderr, "ceremony: skipped f%lld\n", (long long)st->frame);
        eng_sound(0x1F);                               /* 0x1D72 */
        eng_sprite_scene_pals_end();                   /* 0x1D8E 0x2B58 frees the ending banks */
        eng_rank_gameover_arm(st, eng_gs_rumble());    /* 0x1DC4 jsr 0x65C0 */
        camp.armed = 0;
        return ENG_SCENE_ATTRACT;                      /* 0x1DCA jmp 0x6FC */
    }
    switch (ceremony.phase) {
    case END_CARDS:                                    /* 0x1BE8 loop */
        if (++ceremony.t < ENDING_HOLD) return -1;
        ceremony.t = 0;
        memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);   /* 0x1C88 0x1F9E */
        if (++ceremony.card < ENDING_CARDS) { end_card(ceremony.card); return -1; }
        /* 0x1C94: D0 = 0x3B, jsr 0x2AEA — the credit pages' palette.  The
         * stream's bank byte is 0x3B, so the scene allocator picks it up
         * on the first page drawn.
         * 0x1C9E..0x1CE6: six objects, row 0x50, cell = index, speed 0xC
         * angle 0 (+0x2A/+0x2C) — the ROLLING CREDIT pages. */
        for (int i = 0; i < ENDING_WALK_N; i++) {
            ceremony.man[i].x = (int32_t)tbl16(TBL(ending_walkoff_pos), (unsigned)i * 4u) << 16;
            ceremony.man[i].y = (int32_t)(int16_t)tbl16(TBL(ending_walkoff_pos), (unsigned)i * 4u + 2u) << 16;
            ceremony.man[i].live = 1;
        }
        ceremony.phase = END_WALK;
        return -1;
    case END_WALK:                                     /* 0x1CF0 loop, D1 = 0x500 */
        for (int i = 0; i < ENDING_WALK_N; i++) {
            int32_t dx, dy;
            if (!ceremony.man[i].live) continue;
            eng_sincos_step(0, ENDING_WALK_SPD, &dx, &dy);     /* 0x2208, angle 0 = +y */
            ceremony.man[i].x += dx; ceremony.man[i].y += dy;
        }
        if (++ceremony.t < ENDING_WALK_MAX) return -1;
        /* FALLTHROUGH */
    default:
        eng_sound(0x1F);                               /* 0x1D72 */
        eng_sprite_scene_pals_end();                   /* 0x1D8E 0x2B58 */
        eng_rank_gameover_arm(st, eng_gs_rumble());    /* 0x1DC4 jsr 0x65C0 */
        camp.armed = 0;
        if (dbg()) fprintf(stderr, "ceremony: done f%lld -> attract\n", (long long)st->frame);
        return ENG_SCENE_ATTRACT;
    }
}

static void end_draw(const eng_state *st)
{
    unsigned slot = 0;
    /* 0x1BC0 move.w #$78, $140010 — AFTER 0x26E66 (which stamps the scene's
     * own priority byte 0x26E9E[$1C007E] = 0x7C for scene 4).  0x78 is the
     * BG/FG/SPRITES order: the ending's portrait and credit pages draw ON
     * TOP of the arena.  Under the inherited 0x7C the BG plane painted over
     * them and the whole ending screen looked empty. */
    m68k_write_memory_16(0x140010u, 0x78u);
    eng_sprite_scene_pals_rearm();     /* the boot-time body-palette flush
                                          (pal_load.c) may land after begin()
                                          and drop the 0x2AEA map */
    memset(wf.spriteram, 0, WF_SPRRAM_SIZE);
    if (ceremony.phase == END_CARDS)
        /* 0x1BF4/0x1BF6/0x1BFA: D0 = the card's cell, + 0x40, jsr 0x2AEA.
         * The pose stream's own bank byte IS 0x40 + cell, so the scene
         * allocator armed in end_begin deals the bank as the art loads
         * (sprite.c scene_pal_bank). */
        eng_sprite_emit_pose(ENDING_ROW, ceremony.pcell,
                             (int)ceremony.px, (int)ceremony.py, -1, &slot);
    else
        for (int i = 0; i < ENDING_WALK_N; i++) {
            int sx, sy, wx, wy;
            if (!ceremony.man[i].live) continue;
            wx = (int)(ceremony.man[i].x >> 16);
            wy = (int)(ceremony.man[i].y >> 16);
            if (wy < ENDING_Y_LO || wy >= ENDING_Y_HI)
                continue;                              /* 0x1CFC/0x1D04 */
            sx = wx - st->cam_x;                       /* 0x247C */
            sy = wy + 0x100 - st->cam_y;               /* +0x0E z = 0x100 (0x1CDA) */
            eng_sprite_emit_pose(ENDING_WALK_ROW, (unsigned)i, sx, sy, -1, &slot);
        }
    memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);
}

static const eng_scene_ops end_ops = { end_begin, end_update, end_draw };

void eng_campaign_register(void)
{
    eng_scene_register(ENG_SCENE_CONTINUE, &cont_ops);
    eng_scene_register(ENG_SCENE_CEREMONY, &end_ops);
    eng_interlude_register();          /* ENG_SCENE_INTERLUDE (interlude.c) */
}
