/* Ring hardware — the sprite side ropes. Transcription of ROM 0x1004A
 * (init), 0x10120/0x10172 (per-frame state machine) and the rope-shake
 * tables. Spec: docs/engine-specs/ring-hardware.md (2026-08-22).
 *
 * Four class-14 objects (left/right x front/back rope halves); the
 * "rope-shake byte pairs" of the run probe are these objects' state
 * words. Poses come from stream row 14 through the shared emitter;
 * right side = same frames with bit15 flip. Top/bottom ropes are BG
 * tilemap; only the side ropes are sprites — which is why they were
 * missing.
 */
#include "wf.h"
#include "engine.h"
#include "tbl.h"

typedef struct {
    uint16_t state;        /* +0x1C low byte; nonzero = busy */
    int latched;
    uint8_t frame;         /* +0x05 */
    uint8_t step;          /* +0x24 */
    uint8_t timer;         /* +0x22 low byte */
} rh_obj;

static rh_obj rh[4];       /* LF, LB, RF, RB */

/* Frame families: 7-10 are the NEAR/lower tiers (drawn over, list 1),
 * 2-6 the FAR/upper tiers incl. the red top rope (under, list 3) — the
 * y-offsets in the decoded frames settle it (larger sprite y = higher
 * on screen), and the arcade draws the top rope behind a wrestler. */
static const struct { int16_t wx; uint8_t list, flip, idle; } rh_def[4] = {
    { 0x1C0, 1, 0, 10 },   /* left front  ($1C1134) */
    { 0x1C0, 3, 0, 2 },    /* left back   ($1C1164) */
    { 0x330, 1, 1, 10 },   /* right front ($1C1194) */
    { 0x330, 3, 1, 2 },    /* right back  ($1C11C4) */
};
#define RH_WY 0x178
#define RH_WZ 0x140

/* Shake tables, {frame,ticks} byte pairs (docs/adr-001, group base/scene).
 * Each rope handler steps +0x24 through its table until the cmpi count:
 * 0x101E2 light (back 0x10210 / front 0x1021C, 6 steps), 0x10258 heavy
 * (back 0x10286 / front 0x10292, 6), 0x102CE lean (0x102EE, 3 of 4 pairs),
 * 0x10326 blip (0x10346, 2). Each table ends where the next handler's
 * code begins (0x10228 / 0x1029E / 0x102F6 / 0x1034A). */
static const tbl_def ringhw_tables[] = {
    { "rope_shake_light", "base/scene", 0x10210, 12 * 2, TK_U8, 2,
      "0x101E2 run-bounce shake: rows 0-5 back half (0x10210), 6-11 front half (0x1021C); {frame, ticks} per step, 6 steps (0x101D4 cmpi #6)" },
    { "rope_shake_heavy", "base/scene", 0x10286, 12 * 2, TK_U8, 2,
      "0x10258 Irish-whip shake: rows 0-5 back half (0x10286), 6-11 front half (0x10292); {frame, ticks}, 6 steps" },
    { "rope_shake_lean",  "base/scene", 0x102EE, 4 * 2, TK_U8, 2,
      "0x102CE deep rope-lean shake, front only: {frame, ticks}, 3 steps used (0x102C0 cmpi #3), 4th pair pads to the 0x102F6 handler" },
    { "rope_shake_blip",  "base/scene", 0x10346, 2 * 2, TK_U8, 2,
      "0x10326 through-the-ropes blip, front only: {frame, ticks}, 2 steps (0x10318 cmpi #2)" },
};
TBL_REGISTER(ringhw_tables)

/* state -> {front table, back table, pairs}. State 3/4 are front-only. */
static const struct { uint32_t front, back; uint8_t n; } rh_tab[5] = {
    { 0, 0, 0 },
    { 0x1021Cu, 0x10210u, 6 },         /* light (run bounce)   */
    { 0x10292u, 0x10286u, 6 },         /* heavy (Irish whip)   */
    { 0x102EEu, 0x102EEu, 3 },         /* deep rope-lean       */
    { 0x10346u, 0x10346u, 2 },         /* through-ropes blip   */
};

/* Arm a shake on one side (0 left, 1 right). busy_gated = the run probe's
 * rule: skip when the front half is already mid-shake. Both halves of the
 * side arm for states 1/2; 3/4 are front-only. Sound 0x2C on first tick. */
void eng_ropes_arm(int right, int state, int busy_gated)
{
    int f = right ? 2 : 0;

    if (state < 1 || state > 4)
        return;
    if (busy_gated && (rh[f].state & 0xFFu))
        return;
    rh[f].state = (uint16_t)state;
    rh[f].latched = 0;
    if (state <= 2) {
        rh[f + 1].state = (uint16_t)state;
        rh[f + 1].latched = 0;
    }
}

static void rh_tick(int i)
{
    rh_obj *o = &rh[i];
    unsigned st = o->state & 0xFFu;
    uint32_t tab;

    if (st == 0 || st > 4) {           /* handler 0x10186: idle frame */
        o->frame = rh_def[i].idle;
        return;
    }
    tab = (rh_def[i].list == 1) ? rh_tab[st].front : rh_tab[st].back;
    if (!o->latched) {                 /* first tick: sound + step 0 */
        o->latched = 1;
        o->step = 0;
        o->frame = tbl_ra8(tab);
        o->timer = tbl_ra8(tab + 1);
        eng_sound(0x2C);
        return;
    }
    if (o->timer && --o->timer)
        return;
    if (++o->step >= rh_tab[st].n) {   /* sequence over -> idle */
        o->state = ST_STAND;
        o->frame = rh_def[i].idle;
        return;
    }
    o->frame = tbl_ra8(tab + (uint32_t)o->step * 2u);
    o->timer = tbl_ra8(tab + (uint32_t)o->step * 2u + 1u);
}

/* Tick all four then emit the halves belonging to `list` (1 front /
 * 3 back), through 0x247C's screen math and the shared row-14 emitter. */
void eng_ringhw_emit(const eng_state *st, int list, unsigned *slot)
{
    if (st->scene == 2 || st->scene == 6)
        return;                        /* ring-out scene: the ring is tilemap
                                          art, rope objects come back with
                                          0x1004A on the return (0xFCC0) */
    if (list == 3)                     /* back pass runs first: tick all
                                          four once per frame */
        for (int i = 0; i < 4; i++)
            rh_tick(i);
    for (int i = 0; i < 4; i++) {
        int sx, sy;
        if (rh_def[i].list != list)
            continue;
        sx = rh_def[i].wx - st->cam_x;
        sy = RH_WY + RH_WZ - st->cam_y;
        if (eng_ropeart_has((unsigned)st->scene))          /* the arena's own rope art (ropeart.c) */
            eng_ropeart_emit((unsigned)st->scene, rh[i].frame, rh_def[i].flip, sx, sy, slot);
        else
            eng_sprite_emit_pose(14u,
                             (unsigned)rh[i].frame
                             | (rh_def[i].flip ? 0x8000u : 0u),
                             sx, sy, -1, slot);
    }
}
