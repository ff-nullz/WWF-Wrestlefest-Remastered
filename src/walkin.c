/* Ring intro / walk-in before the match goes live — transcription of the
 * 0xA654 sequencer (docs/engine-specs/scene-walkin.md).
 *
 * ROM flow (0xBD2 first match, 0xAC0 every match): 0x7B70 aisle scene
 * (tilemap 4, NOT transcribed — TODO EXACT) -> 0xC98 match init (objects
 * at 0x10708, camera 0x1E0/0x230, FG0 clear 0x1FC0) -> 0xA654 ring intro
 * (this file) -> BGM 0xDEE[stage] -> 0x8AE6 (HUD 0x7506 per human slot,
 * $1C169E = 0x8000 clock on) -> 0xF8C live loop.
 *
 * The intro is a 24-step script on the ring-announcer object $1C14CE
 * (step byte +0x1D, "entered" latch +0x1C b7, counters +0x22/+0x24/+0x1E,
 * mouth +0x25). Step handlers come from the pointer table 0xA7B4 and are
 * dispatched here by their ROM PC. Wrestler objects only get the pose
 * driver 0xACB6 (intro pose cycle 0xAD6C/0xAD9C while +0x1D != 0); the
 * referee only gets 0x247C/0x27B8 (screen pos + sprite) — no machine.
 * Every constant below is read from tbl_ra8() or cites its PC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"

static unsigned r16(uint32_t a) { return ((unsigned)tbl_ra8(a) << 8) | tbl_ra8(a + 1); }   /* pointer-chased cells */

/* Tables this file owns (docs/adr-001-data-formats.md). */
static const tbl_def walkin_tables[] = {
    { "stage_bgm",             "base/front", 0xDEE, 10 * 2, TK_U16, 1,
      "0xC7A match BGM sound command per stage ($1C0162 0..9); code 0xE02 follows" },
    { "intro_step_handlers",   "base/front", 0xA7B4, 24 * 4, TK_U32, 1,
      "0xA654 ring intro: handler PC per script step 0..23 (+0x1D); code 0xA814 follows" },
    { "intro_object_pairs",    "base/front", 0xA8CC, 4 * 8, TK_U32, 2,
      "0xA814/0xA8C0 ring intro: {object, fallback object} work-RAM addresses per pair; code 0xA8EC follows" },
    { "intro_step_wait",       "base/front", 0xAA66, 24 * 2, TK_U16, 1,
      "0xA9E2 ring intro: frames to hold per step; code 0xAA96 follows" },
    { "ws_intro_name_phrase",  "wrestler",   0xAC4A, 12, TK_U8, 1,
      "0xAAEA ring intro: announcer name phrase byte per wrestler id" },
    { "intro_step_phrase",     "base/front", 0xAC56, 24, TK_U8, 1,
      "0xAAEA ring intro: phrase byte per step (0/FF = the wrestler's name)" },
    { "intro_step_phrase_frames","base/front", 0xAC6E, 24 * 2, TK_U16, 1,
      "0xAAEA ring intro: speech duration per step (0 = by wrestler id)" },
    { "ws_intro_name_frames",  "wrestler",   0xAC9E, 12 * 2, TK_U16, 1,
      "0xAAEA ring intro: name speech duration per wrestler id; code 0xACB6 follows" },
    { "ws_intro_pose_ptrs",    "wrestler",   0xAD6C, 12 * 4, TK_U32, 1,
      "0xAD0E ring intro pose cycle: long -> cell list per wrestler id" },
    { "ws_intro_pose_count",   "wrestler",   0xAD9C, 12, TK_U8, 1,
      "0xAD26 ring intro pose cycle: cells per wrestler id" },
    { "intro_pose_cells",      "base/front", 0xADA8, 0xAE20 - 0xADA8, TK_U16, 0,
      "0xAD2E ring intro pose lists: n sprite cell words then n duration words per list; code 0xAE20 follows" },
};
TBL_REGISTER(walkin_tables)

#define ANN_ID      0x1Fu       /* 0xA9FA: announcer sprite row */
#define OBJ_BASE    0x1C05B0u   /* slot 0; stride 0x10C */

/* 0x2052: while the match word is non-zero every command is OR'd with
 * 0x3100 before the 16-bit latch write. */
static void snd(unsigned cmd) { eng_sound(cmd | 0x3100u); }

/* 0xA8CC pair -> engine slot index. Slots 4..7 ($1C09E0..) are the 1P
 * CPU team in the ROM; the engine keeps four objects, so a pair's
 * fallback maps onto the same team's slot 2/3 (TODO EXACT 1P layout). */
static int pair_slot(const eng_state *st, unsigned idx)
{
    uint32_t a = tbl32(TBL(intro_object_pairs), idx * 8u);
    int s = (int)((a - OBJ_BASE) / 0x10Cu);
    if (s < ENG_MAX_OBJS && st->obj[s].active) return s;
    a = tbl32(TBL(intro_object_pairs), idx * 8u + 4u);
    s = (int)((a - OBJ_BASE) / 0x10Cu);
    if (s >= 4) s -= 2;                                  /* $1C09E0 -> slot 2, $1C0AEC -> 3 */
    return (s >= 0 && s < ENG_MAX_OBJS && st->obj[s].active) ? s : 0;
}

/* 0xABD8: which wrestler a name step talks about. */
static int name_slot(const eng_state *st)
{
    unsigned s = (unsigned)st->ann.step;
    if (s == 4)   return pair_slot(st, 0);
    if (s == 8)   return pair_slot(st, 1);
    if (s == 0xD) return pair_slot(st, 2);
    return pair_slot(st, 3);
}

static void next_step(eng_ann *a) { a->entered = 0; a->step++; }   /* 0xA8C0 */

/* 0xA9E2: spawn / re-seat the announcer and hold TAB_WAIT[step] frames. */
static void step_wait(eng_state *st)
{
    eng_ann *a = &st->ann;
    if (!a->entered) {
        a->entered = 1;
        /* 0x2AEA #$1F: palette bank for the announcer (sprite.c remaps) */
        a->active = 1;                                   /* +0x00 = 0x8001 */
        a->x = 0x280 << 16; a->y = 0x178 << 16; a->z = 0x140 << 16;   /* 0xAA00 */
        a->t22 = 0;
    }
    a->spr = 2;                                          /* 0xAA16 */
    a->t22++;
    if ((int)tbl16(TBL(intro_step_wait), (unsigned)a->step * 2u) >= a->t22) return;
    next_step(a);
    if (a->step == 1) {
        /* 0xAA4E: 0x9A42 wipes FG0 rows 1..29 — the match card goes; a
         * non-zero stage skips the whole ceremony (0xAA5E: step = 0x18).
         * Stage is 0 here. */
        eng_banner_clear();
    }
}

/* 0xAA96: pose 2 for 0x10 frames, pose 3 for 0x20, then on. */
static void step_bob(eng_state *st)
{
    eng_ann *a = &st->ann;
    int lim;
    if (!a->entered) { a->entered = 1; a->t22 = a->t24 = 0; }
    a->spr = (uint16_t)(2 + a->t24);
    lim = a->t24 ? 0x20 : 0x10;
    a->t22++;
    if (lim >= a->t22) return;
    a->t22 = 0;
    if (++a->t24 < 2) return;
    next_step(a);
}

/* 0xAAEA: a phrase (or a wrestler's name) with mouth flap. */
static unsigned intro_map_frames;   /* frames of a wrestler-sound-map intro (WAV length / a stock phrase's row); 0 = the base's */
static void step_speech(eng_state *st)
{
    eng_ann *a = &st->ann;
    unsigned dur;
    if (!a->entered) {
        unsigned b = tbl8(TBL(intro_step_phrase), (unsigned)a->step);
        a->entered = 1; a->t22 = a->t24 = a->t1E = 0;
        if (b != 0 && b != 0xFF) snd(b);                 /* 0xAB22 */
        else {
            int s = name_slot(st);                       /* 0xABD8 */
            const char *cn = eng_ws_clone_name(st->obj[s].wrestler);
            unsigned mcmd = 0; const char *mwav = NULL;
            int mk = eng_ws_sound(st->obj[s].wrestler, ENG_SND_INTRO, &mcmd, &mwav);   /* wrestler sound map */
            b = tbl8(TBL(ws_intro_name_phrase), (unsigned)eng_ws_base(st->obj[s].wrestler) & 0xFFu);
            intro_map_frames = 0;
            if (getenv("WF_INTROLOG")) fprintf(stderr, "intro speech step %02X: slot %d wrestler %d base %d phrase 0x%02X map %d\n",
                                               a->step, s, st->obj[s].wrestler, eng_ws_base(st->obj[s].wrestler), b, mk);
            if (mk == 2) {
                double secs = eng_sound_wav(mwav);
                intro_map_frames = (unsigned)(secs > 0 ? secs * 57.4448 + 0.5 : 60);
            } else if (mk == 1) {
                b = mcmd & 0xFFu; snd(b);
                for (unsigned r = 0; r < 12; r++)        /* a stock intro phrase: its owner's frames */
                    if (tbl8(TBL(ws_intro_name_phrase), r) == b) { intro_map_frames = tbl16(TBL(ws_intro_name_frames), r * 2u); break; }
            } else
                snd(b);                                  /* 0xAB42 — a clone with no sound
                                          map INHERITS his base's call (user
                                          2026-08-28: "it's not inherited" — the
                                          old named-clone silence is gone; map a
                                          WAV for his own name) */
            (void)cn;
            st->team_name = 0;                           /* $1C0168 */
            if (b == 0x73) st->team_name = 0x8000;       /* shared team name */
        }
    }
    a->spr = (uint16_t)((a->spr & 0xFF00u) | (unsigned)a->mouth);   /* 0xAB64 */
    if (--a->t22 < 0) {                                  /* 0xAB68 */
        a->t22 = 6 + (int)(eng_rng() & 0xFu);
        a->mouth ^= 1;
    }
    a->t1E++;
    dur = tbl16(TBL(intro_step_phrase_frames), (unsigned)a->step * 2u);
    if (dur == 0) {
        int s = name_slot(st);
        dur = intro_map_frames ? intro_map_frames
            : tbl16(TBL(ws_intro_name_frames), ((unsigned)eng_ws_base(st->obj[s].wrestler) & 0xFFu) * 2u);
    }
    if ((int)dur >= a->t1E) return;
    next_step(a);
}

/* 0xA814: point at a wrestler, start his pose cycle, 0x40 frames. */
static void step_present(eng_state *st)
{
    eng_ann *a = &st->ann;
    if (!a->entered) {
        unsigned team = 3, id;
        int s;
        a->entered = 1;
        if (a->step == 5) team = 0; else if (a->step == 9) team = 1; else if (a->step == 0xE) team = 2;
        s = pair_slot(st, team);
        st->obj[s].intro_1d = 1;                         /* 0xA85A */
        id = st->obj[s].wrestler & 0xFFu;
        snd(id < 5 ? 0x32u : 0x31u);                     /* 0xA860/0xA86E sting */
        if (id == 0xA && s + 1 < ENG_MAX_OBJS)           /* 0xA878: twin partner */
            st->obj[s + 1].intro_1d = 1;
        a->t22 = 0;
    }
    a->spr = 0xC;                                        /* 0xA890 */
    if (a->step >= 0xB) a->spr |= 0x8000u;               /* 0xA89E */
    a->t22++;
    if (a->t22 < 0x40) return;
    if (st->team_name) a->step += 4;                     /* 0xA8B2: one name covers both */
    next_step(a);
}

/* 0xA8EC: walk out of the ring (down) with cells 8..B, until y >= 0x1A7. */
static void step_walkout(eng_state *st)
{
    eng_ann *a = &st->ann;
    int32_t dx, dy;
    if (!a->entered) { a->entered = 1; a->t22 = a->t24 = 0; a->speed = 8; a->angle = 0; }   /* 0xA8FC */
    eng_sincos_step(a->angle, a->speed, &dx, &dy);       /* 0x2208 */
    a->x += dx; a->y += dy;
    a->spr = (uint16_t)(a->t24 + 8);
    if (++a->t22 >= 0xC) { a->t22 = 0; if (++a->t24 >= 4) a->t24 = 0; }
    if ((a->y >> 16) < 0x1A7) return;
    next_step(a);
}

/* 0xA952 / 0xA99A: 4 cells from `base`, 0xC frames each, once. */
static void step_cycle(eng_state *st, unsigned base)
{
    eng_ann *a = &st->ann;
    if (!a->entered) { a->entered = 1; a->t22 = a->t24 = 0; }
    a->spr = (uint16_t)(a->t24 + (int)base);
    if (++a->t22 < 0xC) return;
    a->t22 = 0;
    if (++a->t24 < 4) return;
    next_step(a);
}

/* 0xACB6: wrestler intro pose driver. */
static void wrestler_pose(eng_state *st, eng_obj *o)
{
    if (!o->intro_1d) {                                  /* 0xACE8 */
        o->spr = eng_side(o) ? 0 : 0x8000u;
        o->intro_22 = o->intro_24 = 0;
        return;
    }
    {
        unsigned id = (unsigned)eng_ws_base(o->wrestler);   /* clones: the base's intro poses (id 15 read past the 12-row tables) */
        uint32_t cells = tbl32(TBL(ws_intro_pose_ptrs), id * 4u);
        unsigned n = tbl8(TBL(ws_intro_pose_count), id);
        unsigned dur;
        o->spr = (uint16_t)r16(cells + (uint32_t)o->intro_24 * 2u);
        o->intro_22++;
        dur = r16(cells + n * 2u + (uint32_t)o->intro_24 * 2u);
        if ((int)dur >= (int)o->intro_22) return;
        o->intro_22 = 0;
        o->intro_24++;
        if ((int)n - 1 >= (int)o->intro_24) return;
        o->intro_1d = 0;                                 /* 0xAD66 clr.w +0x1C */
    }
}

void eng_intro_begin(eng_state *st)
{
    memset(&st->ann, 0, sizeof st->ann);
    st->intro = 1;
    st->team_name = 0;
    st->hud_inited = 0;
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);   /* 0x1FC0 clear_screen (0xCBA) */
    eng_banner_draw(st);                                 /* 0xA670: 0x98BA match card */
    for (int i = 0; i < ENG_MAX_OBJS; i++) st->obj[i].intro_1d = 0;
    /* 0xA65E: referee +0x06 += 0x80 for the ceremony (trace: (0x300,0x198)) */
    st->ref.x += 0x80 << 16;
    snd(0x32);                                           /* 0xA676: intro BGM 0x3132 */
}

/* 0xA778..0xA7B2 + 0xC7A + 0x8AE6: go live. */
void eng_intro_end(eng_state *st)
{
    st->intro = 0;
    st->ann.active = 0;                                  /* 0xA78E clr $1C14CE */
    eng_banner_clear();                                  /* 0xA782 0x9A42 */
    st->ref.x = 0x280 << 16;                             /* back to the 0x10718 seat */
    for (int i = 0; i < ENG_MAX_OBJS; i++) st->obj[i].intro_1d = 0;
    eng_sound(0x3100u); eng_sound(0x3120u);              /* 0x8EB6 */
    snd(0x2F);                                           /* 0xA7A8 */
    {
        int stage = (st->stage & 0xFF) % 10;             /* $1C0162 low byte */
        eng_sound(tbl16(TBL(stage_bgm), (unsigned)stage * 2u));   /* 0xC7A stage BGM */
    }
    for (int i = 0; i < 4 && i < ENG_MAX_OBJS; i++)      /* 0x9862 (via +0x35 b4):
                                                            the P chip over every
                                                            pad man at the bell —
                                                            TODO EXACT the site */
        if (st->obj[i].active && st->obj[i].input >= 0) st->obj[i].cue_flags |= CF_CHIP_P;
    st->hud_inited = 0;                                  /* 0x7506 from 0x8AE6 */
    st->sig169e |= 0x80;                                 /* 0x8B30 clock runs */
    /* No announce request here. Stock rings the opening bell only through
     * the direct 0x312F posts (0xA7A8 exit + the 0xACDC pose drivers; the
     * Z80 collapses the burst into one bell — MAME tap: 7 CMD 0x312F in
     * frames 906-908, ONE OKI phrase-15 trigger). Announce phrase 0x2A is
     * the match-END sequence (0x2042C / 0x11270 fall path): bell x3 then
     * 0x7A = bank1_036 "Congratulations! Now go for the next match!" —
     * firing it here played the match-end speech at the opening bell. */
}

/* One frame of 0xA680..0xA774. Returns 1 while the intro owns the frame. */
int eng_intro_tick(eng_state *st)
{
    eng_ann *a = &st->ann;
    if (!st->intro) return 0;
    {
        uint32_t pc = tbl32(TBL(intro_step_handlers), (unsigned)(a->step & 0xFF) * 4u);
        switch (pc) {
        case 0xA9E2: step_wait(st); break;
        case 0xAA96: step_bob(st); break;
        case 0xAAEA: step_speech(st); break;
        case 0xA814: step_present(st); break;
        case 0xA8C0: next_step(a); break;
        case 0xA8EC: step_walkout(st); break;
        case 0xA952: step_cycle(st, 4); break;
        case 0xA99A: step_cycle(st, 0xD); break;
        default: next_step(a); break;
        }
        if (a->step >= 0xB) a->spr |= 0x8000u;           /* 0xA6A2 */
        if (getenv("WF_INTROLOG") && a->step != a->log_step) {
            a->log_step = a->step;
            fprintf(stderr, "intro f%lld step %02X pc %05X obj0 spr %04X 1d %d\n",
                    (long long)st->frame, a->step, pc, st->obj[0].spr, st->obj[0].intro_1d);
        }
        a->sx = (int16_t)((a->x >> 16) - st->cam_x);     /* 0x247C */
        a->sy = (int16_t)((a->y >> 16) + (a->z >> 16) - st->cam_y);
    }
    for (int i = 0; i < ENG_MAX_OBJS; i++) {             /* 0xA6CC loop */
        eng_obj *o = &st->obj[i];
        if (!o->active) continue;
        wrestler_pose(st, o);
        eng_screen_pos(o, st);
    }
    st->ref.spr = 0;                                     /* 0xA702: pos + sprite only */
    st->ref.sx = (int16_t)((st->ref.x >> 16) - st->cam_x);
    st->ref.sy = (int16_t)((st->ref.y >> 16) + (st->ref.z >> 16) - st->cam_y);
    /* 0xA732: any player's button 1/2 skips; else run to step 0x18 */
    {
        int held = 0;
        for (int p = 0; p < 4; p++) if (st->inputs[p] & 0x30u) held = 1;
        /* the ROM reads the +0xA2 NEW-press word, so the press that confirmed
         * the previous screen cannot carry over: arm on the first release */
        if (!held) a->skip_arm = 1;
        if ((held && a->skip_arm) || a->step >= 0x18) eng_intro_end(st);
    }
    return 1;
}
