/* Aisle walk-in — the ENG_SCENE_AISLE front-end scene, transcription of
 * ROM 0x7B70 (scene word 4: the entrance tunnel tilemap) and its per-man
 * walker 0x7F0E. Spec: docs/engine-specs/scene-aisle.md.
 *
 * Stock flow (0xBD2 first match): character select -> 0x7B70 (this) ->
 * 0xC98 match init -> 0xA654 ring intro (walkin.c, draws the VS card
 * banner.c) -> live. In tag.scn the scene word is 4 for f1582..1987.
 *
 * 0x7B70: curtain (0x1FC0 fills FG0 with the solid tile), scene 4,
 * window (0x140,0x200), compose, palettes; then one loop per human team
 * (slots 0/1, then 2/3 if slot 2 is active) running 0x7F0E per man,
 * the flash object 0x7E86, the crowd stamps 0x285DA, until $1C0080 goes
 * non-zero; finally 0x1FC0 again.
 *
 * 0x7F0E per man: first entry = FG0 fade-in 0x8FD6 (8 steps x 3
 * frames, leader only), name plates 0x2503C from 0x8186[id], bottom bar
 * 0xB29E, then sprite row 0x40+id, polar walk speed 8 angle 0x80 from
 * (0x1C0|0x1F0, 0x130, z 0x100); every frame 0x2208 motion, pose cycle
 * 0x8136 at 12 frames/cell, 4 cells then optional cells 4..7 for rows
 * 0x40/0x41/0x47/0x49 (rng 0x21B4). The leader reaching y < 0x90 runs
 * 0x8F50 (fade-out 8x3 frames + 0x21E6 wait 0x20) and ends the team.
 *
 * The walkers are private copies: the match objects st->obj[] are only
 * read for the picks (+0x02 ids, side bit), so the MATCH transition's
 * eng_init() sees clean state. Every constant is read from tbl_ra8() or
 * cites its PC; simplifications are marked TODO EXACT. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include "credit.h"
#include "scene.h"


#define SCENE_WORD   4          /* 0x7BD0 */
#define ARENA_PAL    4          /* 0x7BD8 / 0x7BE0 */
#define TEXT_SET     0          /* 0x7BC8 */
#define SCROLL_X     0x140      /* 0x7BE8 */
#define SCROLL_Y     0x200      /* 0x7BF0 */
/* Tables this file owns (docs/adr-001-data-formats.md). */
static const tbl_def aisle_tables[] = {
    { "aisle_pose_cells",     "base/front", 0x8136, 8, TK_U8, 1,
      "0x80AA aisle walk: sprite cell per pose step 0..7" },
    { "ws_aisle_palette_banks","wrestler",  0x813E, 12 * 4, TK_U16, 2,
      "0x7FD0 aisle walk: two 0x2AEA palette bank ids per wrestler id" },
    { "ws_aisle_plate_rumble","wrestler",   0x816E, 12 * 2, TK_U16, 1,
      "0x7FB6 aisle walk, rumble: 0x2503C name plate blit id per wrestler id" },
    { "ws_aisle_plates",      "wrestler",   0x8186, 13 * 4, TK_U16, 2,
      "0x7F58 aisle walk: {leader plate, partner plate} 0x2503C blit ids per wrestler id; the ROM holds a 13th row (= row 0) before code 0x81BA, reader unknown" },
};
TBL_REGISTER(aisle_tables)
#define FLASH_ROW    0x4Cu      /* 0x7EA6 */
#define CURTAIN_LO   0xC5u      /* 0x1FC0 / 0x8FE6: cell bytes FF C5 FF 04 */
#define CURTAIN_HI   0x04u
#define FADEOUT_LO   0xBEu      /* 0x8F60: FF BE FF 04, + step */
#define FADE_STEPS   8          /* 0x8FB6 / 0x903C */
#define FADE_HOLD    3          /* 0x8F9E / 0x9024 frames per step */
#define FADEOUT_WAIT 0x20       /* 0x8FC0: 0x21E6 #$20 */
#define WALK_Y_END   0x90       /* 0x806E */
#define CURTAIN_IN   5          /* TODO EXACT: f1582..1586 — vblank waits in
                                   0x2052/0x26E66/0x2A06 before the loop */
#define CURTAIN_OUT  6          /* TODO EXACT: f1988..1993 — 0xC98 match init
                                   (compose + palette) before the intro draws */

typedef struct {
    int active, entered;
    int slot;               /* engine object index */
    unsigned id;            /* +0x02 low byte before the +0x40 */
    int      raw;           /* the seat's true wrestler id (clone/ALT >= 12) */
    unsigned row;           /* +0x02 after 0x8002: 0x40 + id (0x4B -> 0x4A) */
    eng_obj o;              /* x/y/z/sx/sy/spr/off_y + polar mover */
    unsigned t22, t24;      /* +0x22 frame counter, +0x24 step<<8 | cell */
} walker;

enum { PH_CURTAIN, PH_FADEIN, PH_WALK, PH_FADEOUT, PH_WAIT, PH_END };

static struct {
    int phase, t, step;
    int team;               /* 0: slots 0/1, 1: slots 2/3 */
    walker w[2];
    struct { eng_obj o; unsigned t22, t24; } flash;   /* $1C14CE in this scene */
    int swap01;             /* 0x7C28/0x7C48 display-order swap */
} as;

static void fg0_fill(unsigned lo, unsigned hi)
{
    for (unsigned off = 0; off + 4 <= WF_FG0RAM_SIZE; off += 4) {
        wf.fg0_videoram[off] = 0xFF; wf.fg0_videoram[off + 1] = (uint8_t)lo;
        wf.fg0_videoram[off + 2] = 0xFF; wf.fg0_videoram[off + 3] = (uint8_t)hi;
    }
}

/* 0xB29E: bottom bar (row 0x1D, cols 2..29, tiles 0x4FA/0x4FB pal 1)
 * and the small WWF logo run 0x9B26[0xD] at $C1A7C, palette 1.
 * Shared with the LOD talk screen (0xAF5E, interlude.c). */
void eng_aisle_bottom_bar(void)
{
    unsigned off = 0x1D08;
    for (unsigned i = 0; i < 0x1C; i++, off += 4) {
        if (off + 4 > WF_FG0RAM_SIZE) break;
        wf.fg0_videoram[off] = 0xFF; wf.fg0_videoram[off + 1] = (uint8_t)(i ? 0xFB : 0xFA);
        wf.fg0_videoram[off + 2] = 0xFF; wf.fg0_videoram[off + 3] = 0x14;
    }
    eng_banner_runs(0xC1A7Cu, 0xD, 0x10);                  /* 0x9ACA */
}

/* 0x7C28 / 0x7C48: a Hogan (id 0) leader walks in the partner's spot
 * unless that puts id 9 in the leader's. TODO EXACT: the ROM swaps the
 * object +0x02 words themselves; here only the walk order swaps. */
static void order_swap(const eng_state *st, int base)
{
    unsigned a = (unsigned)eng_ws_base(st->obj[base].wrestler);     /* clones/ALTs:
                                          the aisle streams/plates are BASE art */
    unsigned b = (unsigned)eng_ws_base(st->obj[base + 1].wrestler);
    as.swap01 = 0;
    if (eng_rumble_armed())
        return;    /* 0x7C1C: the rumble branch (bne 0x7E16) skips both
                      swaps — applying them here handed w[0] to slot 1 and
                      DEACTIVATED Hogan's walker (rumble walks k==0 only):
                      "chose Hogan, no walk-in animation" (playtest). */
    if (a == 0) { unsigned t = a; a = b; b = t; as.swap01 ^= 1; }
    if (b == 9) { as.swap01 ^= 1; }
}

static void team_begin(const eng_state *st)
{
    int base = as.team * 2;
    memset(as.w, 0, sizeof as.w);
    order_swap(st, base);
    for (int k = 0; k < 2; k++) {
        int s = base + (k ^ as.swap01);
        as.w[k].slot = s;
        as.w[k].active = st->obj[s].active;
        as.w[k].id = (unsigned)eng_ws_base(st->obj[s].wrestler);   /* base art row */
        as.w[k].raw = st->obj[s].wrestler;
        if (eng_rumble_armed() && k == 1) as.w[k].active = 0;   /* 0x7E16: one man walks at a time in the rumble */
    }
    as.phase = PH_FADEIN; as.t = 0; as.step = 0;
}

/* 0x7FC6..0x8062: seat the walker. */
static void walker_init(walker *w, int leader)
{
    eng_sound(0x3180);                                     /* 0x7FC6 */
    (void)tbl16(TBL(ws_aisle_palette_banks), w->id * 4u);                     /* 0x7FE4/0x7FEE 0x2AEA banks:
                                                              sprite.c maps by the stream's
                                                              own bank byte (TODO EXACT) */
    w->t22 = 0; w->t24 = 0;
    w->row = 0x40u + w->id;                                /* 0x8002 */
    if (w->row == 0x4Bu) w->row--;                         /* 0x8008 */
    if (w->raw >= 12)                  /* clone: virtual aisle row — pak
                                          walkout art if ingested, else the
                                          base stream (sprite.c decode) */
        w->row = eng_sprite_aisle_row(w->raw);
    w->o.active = 1;
    w->o.mover = 1;                                        /* +0x00 = 0x8001 polar */
    w->o.speed = 8;                                        /* 0x801C */
    w->o.angle = 0x80;                                     /* 0x8022 */
    w->o.y = 0x130 << 16;                                  /* 0x8028 */
    w->o.z = 0x100 << 16;                                  /* 0x802E */
    w->o.x = 0x1D8 << 16;                                  /* 0x8034 (rumble) */
    if (!eng_rumble_armed()) {
        w->o.x = 0x1C0 << 16;                              /* 0x8044 */
        if (!leader) w->o.x += 0x30 << 16;                 /* 0x8062 */
    }
    w->entered = 1;
}

/* 0x8068..0x8134: one frame of a seated walker. Returns 1 when the
 * leader has reached the top (0x806E). */
static int walker_tick(walker *w, int leader)
{
    eng_apply_motion(&w->o);                               /* 0x8068 */
    if ((w->o.y >> 16) < WALK_Y_END)
        return 1;                                          /* 0x8074 -> 0x8098/0x809E */
    (void)leader;
    w->o.spr = (uint16_t)tbl8(TBL(aisle_pose_cells), w->t24 & 0xFFu);  /* 0x80AA */
    if (w->row == 0x41u) {                                 /* 0x80BE: Perfect's cells 4..7 */
        w->o.off_y = 0;
        if ((w->o.spr & 0xFFu) >= 4) w->o.off_y = 0x30;
    }
    if (++w->t22 < 0xC) return 0;                          /* 0x80D8 */
    w->t22 = 0;
    w->t24 += 0x101;                                       /* 0x80EA */
    if ((w->t24 & 0xFF00u) < 0x400) return 0;
    w->t24 = 0;                                            /* 0x80FE */
    if (w->row == 0x40u || w->row == 0x41u || w->row == 0x47u || w->row == 0x49u)
        if (!(eng_rng() & 1u)) w->t24 = 4;                 /* 0x8122 */
    return 0;
}

/* 0x7E86: the $1C14CE flash object rides beside the last walker. */
static void flash_tick(void)
{
    eng_obj *f = &as.flash.o;
    const walker *p = &as.w[eng_rumble_armed() ? 1 : 1];      /* A0 - 0x10C = slot 1 */
    unsigned lim;
    f->active = 1;
    f->x = 0x1D8 << 16;                                    /* 0x7EAC */
    f->z = 0x100 << 16;                                    /* 0x7EB2 */
    f->y = p->o.y + (1 << 16);                             /* 0x7EB8/0x7EBE */
    f->spr = (uint16_t)(as.flash.t24 ? 0xFFFFu : 0u);      /* 0x7EC4..0x7ED2 */
    as.flash.t22++;
    lim = as.flash.t24 ? 1u : 2u;                          /* 0x7EDC..0x7EE6 */
    if (lim < as.flash.t22) { as.flash.t22 = 0; as.flash.t24 ^= 1u; }
}

static void aisle_begin(eng_state *st)
{
    eng_sprite_scene_pals_begin();     /* 0x7C16 jsr 0x2AEA #$34 + the per-man
                                          0x7FE4/0x7FEE bank pairs: the walker
                                          rows' baked palette ids install on
                                          demand — after an attract fade the
                                          stale banks drew the walkers BLACK */
    /* The picks live in the match objects: seat them now the way the
     * MATCH transition will (scene.c) so +0x02 ids and the side bit are
     * there to read. eng_init begins the intro, which draws the card;
     * drop it — the real intro redraws it after this scene. */
    {
        int64_t fr = st->frame;
        eng_init_picks(st, eng_camp_picks());
        st->frame = fr;
    }
    eng_banner_clear();
    memset(&as, 0, sizeof as);
    eng_sound(0x310E);                 /* 0x7B9A (stage 0; 0x310D on 4/9 — TODO EXACT stage) */
    fg0_fill(CURTAIN_LO, CURTAIN_HI);  /* 0x7BC2 0x1FC0 */
    st->scene = SCENE_WORD;            /* 0x7BD0 */
    st->cam_x = SCROLL_X;              /* 0x7BE8 */
    st->cam_y = SCROLL_Y;
    eng_scene_publish(ARENA_PAL, TEXT_SET);
    memset(wf.spriteram, 0, sizeof wf.spriteram);
    memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    as.phase = PH_CURTAIN; as.t = 0; as.team = 0;
    if (getenv("WF_DBGSEL"))
        fprintf(stderr, "aisle: begin f%lld ids %d/%d %d/%d\n", (long long)st->frame,
                st->obj[0].wrestler, st->obj[1].wrestler, st->obj[2].wrestler, st->obj[3].wrestler);
}

static int aisle_update(eng_state *st)
{
    switch (as.phase) {
    case PH_CURTAIN:
        if (++as.t >= CURTAIN_IN) team_begin(st);
        return -1;

    case PH_FADEIN:                                        /* 0x8FD6 in the leader's 0x7F0E */
        if (as.t == 0) fg0_fill(CURTAIN_LO - (unsigned)as.step, CURTAIN_HI);
        if (++as.t < FADE_HOLD) return -1;
        as.t = 0;
        if (++as.step < FADE_STEPS) return -1;
        memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);       /* 0x7F52 0x1F9E */
        eng_credit_force(); eng_credit_line();   /* 0x1F9E bset #7,$1C0072 + 0x1E92: live CREDIT line */
        if (eng_rumble_armed()) {
            unsigned rp = tbl16(TBL(ws_aisle_plate_rumble), as.w[0].id * 2u);
            const char *rcn = eng_ws_clone_name(as.w[0].raw);
            if (rcn && rcn[0])
                eng_fg0_bigtext(0x1B, 2, rcn, 0);        /* clone: his own name */
            else if (rp)
                eng_blit(rp);                            /* 0x7FB6 rumble plate 0x816E[id] */
            else {
                /* the ROM has NO rumble plate for this id (LOD rows are 0 —
                 * stock never rumbles them): borrow the TAG plate's text and
                 * set it in the same face ("hawk and animal - their aisle
                 * walkout has no name", playtest 2026-08-24) */
                char nm[40];
                if (eng_blit_text(tbl16(TBL(ws_aisle_plates), as.w[0].id * 4u), nm, sizeof nm))
                    eng_fg0_bigtext(0x1B, 2, nm, 0);
            }
        } else {
            const char *cn0 = eng_ws_clone_name(as.w[0].raw);
            const char *cn1 = as.w[1].active ? eng_ws_clone_name(as.w[1].raw) : NULL;
            if (getenv("WF_DBGSEL"))
                fprintf(stderr, "aisle: plates ids %u/%u raw %d/%d blits %u/%u cn %s/%s\n",
                        as.w[0].id, as.w[1].id, as.w[0].raw, as.w[1].raw,
                        tbl16(TBL(ws_aisle_plates), as.w[0].id * 4u),
                        tbl16(TBL(ws_aisle_plates), as.w[1].id * 4u + 2u),
                        cn0 ? cn0 : "-", cn1 ? cn1 : "-");
            /* LOD (ids 4/5): the ROM's plate rows were authored for the
             * shared tag entrance and both stamp ONE line — a human HAWK +
             * ANIMAL pair overwrote each other (user 2026-08-28). Their
             * names take the big-font path on the proper lines instead. */
            if (!cn0 && (as.w[0].id == 4 || as.w[0].id == 5)) cn0 = as.w[0].id == 4 ? "HAWK" : "ANIMAL";
            if (!cn1 && as.w[1].active && (as.w[1].id == 4 || as.w[1].id == 5)) cn1 = as.w[1].id == 4 ? "HAWK" : "ANIMAL";
            if (cn0 && cn0[0]) eng_fg0_bigtext(0x18, 2, cn0, 0);   /* the CLONE'S OWN
                                          name in the REAL walkout face */
            else eng_blit(tbl16(TBL(ws_aisle_plates), as.w[0].id * 4u));     /* 0x7F6C leader plate */
            if (cn1 && cn1[0]) eng_fg0_bigtext(0x1B, 2, cn1, 0);
            else if (as.w[1].active)
                eng_blit(tbl16(TBL(ws_aisle_plates), as.w[1].id * 4u + 2u));   /* 0x7F8C partner plate */
        }
        eng_aisle_bottom_bar();                                     /* 0x7F92 0xB29E */
        walker_init(&as.w[0], 1);                                   /* 0x7FC6.. */
        if (as.w[1].active) walker_init(&as.w[1], 0);
        as.phase = PH_WALK;
        /* fall through: motion runs on the entry frame (0x8068) */
        /* FALLTHROUGH */
    case PH_WALK: {
        int done = 0;
        for (int k = 0; k < 2; k++) {
            if (!as.w[k].active) continue;
            if (walker_tick(&as.w[k], k == 0) && k == 0) done = 1;   /* 0x8098 leader only */
            eng_screen_pos(&as.w[k].o, st);                          /* 0x7CAA 0x247C */
        }
        flash_tick();                                                /* 0x7CC4 0x7E86 */
        eng_screen_pos(&as.flash.o, st);
        if (getenv("WF_DBGSEL") && (st->frame % 10) == 0)
            fprintf(stderr, "aisle f%lld walk y=%X sy=%d spr=%X | y=%X sy=%d spr=%X | flash sy=%d spr=%X\n",
                    (long long)st->frame, as.w[0].o.y >> 16, as.w[0].o.sy, as.w[0].o.spr,
                    as.w[1].o.y >> 16, as.w[1].o.sy, as.w[1].o.spr, as.flash.o.sy, as.flash.o.spr);
        if (done) { as.phase = PH_FADEOUT; as.t = 0; as.step = 0; }
        return -1; }

    case PH_FADEOUT:                                       /* 0x8F50 */
        if (as.t == 0) fg0_fill(FADEOUT_LO + (unsigned)as.step, CURTAIN_HI);
        if (++as.t < FADE_HOLD) return -1;
        as.t = 0;
        if (++as.step < FADE_STEPS) return -1;
        as.phase = PH_WAIT; as.t = 0;
        return -1;

    case PH_WAIT:                                          /* 0x8FC4 0x21E6 #$20 */
        if (++as.t < FADEOUT_WAIT) return -1;
        /* 0x7D8A: a 2P team seated at $1C07C8 walks next (same machine).
         * The front end here is the 1P flow (charselect picks CPU1/CPU2
         * for slots 2/3, eng_init_picks leaves them input-driven), so the
         * second walk only runs for a team marked neither CPU nor the
         * stock 1P layout — TODO EXACT: a real 2P start (0x7CF6 swap,
         * 0x7D84 loop) once the engine seats P2 before the match. */
        if (as.team == 0 && st->obj[2].active && !st->obj[2].cpu && getenv("WF_AISLE_2P")) {
            as.team = 1;
            team_begin(st);
            return -1;
        }
        fg0_fill(CURTAIN_LO, CURTAIN_HI);                  /* 0x7E0E 0x1FC0 */
        as.phase = PH_END; as.t = 0;
        return -1;

    case PH_END:
    default:
        if (++as.t < CURTAIN_OUT) return -1;
        if (getenv("WF_DBGSEL"))
            fprintf(stderr, "aisle: done f%lld\n", (long long)st->frame);
        return ENG_SCENE_MATCH;
    }
}

/* Runs after the compose/palette load: crowd stamps (0x285DA) and the
 * scene's own sprite list (0x27B8 per walker, then the flash object). */
static void aisle_draw(const eng_state *st)
{
    unsigned slot = 0;
    wf.priority = 0x7B;                /* 0x7BFE move.b #$7b,$140011 after the
                                          compose (0x26E66 wrote its own) */
    eng_scenery_tick(st);
    memset(wf.spriteram, 0, WF_SPRRAM_SIZE);
    if (as.phase == PH_WALK || as.phase == PH_FADEOUT || as.phase == PH_WAIT) {
        for (int k = 0; k < 2; k++) {
            const walker *w = &as.w[k];
            if (!w->active || !w->entered || (w->o.spr & 0x7FFFu) == 0x7FFFu) continue;
            eng_sprite_emit_pose(w->row, w->o.spr, w->o.sx, w->o.sy, -1, &slot);
        }
        /* 0x7E86's row-0x4C object is active with cell 0 three frames in
         * five (trace f1700..1702), yet the oracle sprite RAM never holds a
         * record for it: its pose decodes to tiles 0/2/4.. — blank in the
         * stock sprite ROM, but art in data/gfx-edit. Not emitted (TODO
         * EXACT once the stock blank tiles are honoured by the decoder). */
        if (getenv("WF_AISLE_FLASH") && as.flash.o.active && (as.flash.o.spr & 0x7FFFu) != 0x7FFFu)
            eng_sprite_emit_pose(FLASH_ROW, as.flash.o.spr, as.flash.o.sx, as.flash.o.sy, -1, &slot);
    }
    memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);
}

static const eng_scene_ops aisle_ops = { aisle_begin, aisle_update, aisle_draw };

void eng_aisle_register(void)
{
    eng_scene_register(ENG_SCENE_AISLE, &aisle_ops);
}
