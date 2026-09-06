/* ATTRACT cycle — transcription of the ROM driver at 0x6FC.
 *
 * Reset falls into 0x6F6 (copy the ROM ranking defaults, rank.c) then:
 *
 *   0x6FC  clr $1C007C/$1C00B2/$1C0076, 0x1F52 init, bset 7 $1C0076,
 *          sound 0x3100; btst #7,$1C0067 (FBI dip) -> skip to 0x790.
 *   0x72E  FBI "Winners Don't Use Drugs" card: clears, scene word 3,
 *          arena pal 3 ($1C15F4/F8), 0x2A06, snapshot 0x26772, scroll
 *          (0x280,0x600), compose 0x26E66, fade-in 0x26844, hold 0x1E6C
 *          (0x80 vblanks, IRQ3 live: credit line 0x1E92 + START 0x978 ->
 *          jmp 0x52BE), fade-out 0x264E2.
 *   0x790  trademark card: clears (0x1F6C wipes the tilemaps -> black
 *          background, no compose), 0x2A06, snapshot, BLIT 4 (0x2503C
 *          legal text), fade-in, hold 0x80, fade-out.
 *   0x7C6  Technos card: clears, scene 3, text set 3 ($1C15FC), 0x2A06,
 *          snapshot, 0xA0AE (glyph run 0x27 at $C1044, attr 0x20),
 *          scroll (0x3C0,0x600), compose, fade-in, hold, fade-out.
 *   0x91C  TITLE: sound 0x3102, clr $1C007C, bset 7 $1C0076 (START live
 *          for the whole page), clears, text set 0, 0x2A06, 0x81BA:
 *            scene 3, compose the (0x280,0x400) window ONCE (starfield);
 *            each vblank the raw plane X scroll regs get $1C1CD4 ->
 *            $100000 (+2/frame) and $1C1CD6 -> $100004 (+1/frame): a
 *            horizontal parallax pan wrapping in the 512px ring
 *            (render.c eng_compose_freeze / eng_pan_x0/x1);
 *            0x80 frames pan, then the ROSTER PARADE: 12 sprite slots
 *            from 0x86C8 {row 0x20..0x2B, x, y}, each animating cells
 *            0..4 (dwell 4 frames, 0x1A on the last, 0x8320..0x8352);
 *            then BLIT 1 (logo text) + 0xA44E (runs 0xD/0xE/0xF at
 *            $C0344/$C050C/$C0A10, attrs 0x10/0x60/0x70) and 0x5C8
 *            frames of pan + INSERT COIN / PRESS START blink (bit 6 of
 *            the frame counter; erase blits 0x8056/0x8054, draw id from
 *            the credit state 0x8410..0x847C);
 *          sound 0x3101, bra 0xAC0.
 *   0xAC0  DEMO cycle, $1C0166 = segment 0..0xD ($1C007C == 0 -> 0xB1C):
 *            seg 1: LOD taunt screen 0xAE20 D0=1 (interlude.c talk mode
 *                   1), fade, RANKING page 0x69C8 (tag) 0x100 frames;
 *            seg 2: $1C0161=1, taunt 0xAE20 D0=2 (talk mode 2), RANKING
 *                   page (rumble table), $1C0161=0 after (0xB62);
 *            seg 6/0xA: title card 0x8490 (parade static on cell 4,
 *                   blit 1 + 0xA44E + blink, 0x100 frames);
 *          then the DEMO MATCH: 0xE02 seeds 4 CPU men from 0xF3E[seg]
 *          (seg 1: a 6-man rumble from 0xF76, $1C0160=1), energy from
 *          0xF22[seg] (slot 0 keeps 0x7000), no intro, wipe-in 0x8FD6,
 *          text set 2, jmp 0xF8C: the REAL match loop runs; $1C007C==0
 *          ends it purely on the 0x119A[seg] frame budget (0x1102),
 *          wipe-out 0x8F50, wait 0x20, $1C0166++; >= 0xE -> jmp 0x6FC.
 *   START  at any point (IRQ3 0x978, live on every hold/pan/demo frame):
 *          a credit is taken (0x55A) and jmp 0x52BE = game select.
 *   Game over runs the ranking insert + NAME ENTRY (0x65C0, rank.c)
 *   before jmp 0x6FC; the engine shows it as the first attract page.
 *
 * Oracle for the FBI card fades (boot.scn): black f0-15, fade-in steps
 * at f16+4k, full at f44, CREDIT from f47, hold to f176, fade-out step 6
 * at f181..step 0 at f205, black, trademark fade-in from f217.
 */
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
static const tbl_def attract_tables[] = {
    { "palette_fade_steps", "base/front", 0x266B2, 16 * 8, TK_U8, 8,
      "0x26642 palette fade: per colour nibble (row) the value at fade step 0..7; code 0x26732 follows" },
    { "title_parade",       "base/front", 0x86C8, 12 * 6, TK_U16, 3,
      "0x8284 title-page roster parade: {sprite row 0x20..0x2B, screen x, screen y} per slot 0..11" },
    { "demo_durations",     "base/front", 0x119A, 14 * 2, TK_U16, 1,
      "0x1102 demo-match frame budget per segment $1C0166 0..0xD; then 0x8F50 wipe and the next segment (0x112E)" },
    { "demo_energy",        "base/front", 0x0F22, 14 * 2, TK_U16, 1,
      "0xE7C demo energy (+0x66/+0x72) for slots 1..3 per segment; slot 0 keeps 0x7000 (0xE6A)" },
    { "demo_picks",         "base/front", 0x0F3E, 14 * 4, TK_U8, 4,
      "0xE38 demo wrestler ids, 4 per segment (slot pairs 0-1 / 2-3, 0xF7C); segment 1 is the rumble (0xE2C)" },
    { "demo_rumble_six",    "base/front", 0x0F76, 6, TK_U8, 1,
      "0xEE6 demo segment 1: the six rumble wrestler ids (objects 0..5, all CPU, 0x1086C init)" },
    { "demo_card_rows",     "base/front", 0x8E9A, 14 * 2, TK_U16, 1,
      "0x8DDA demo overlay (0x8CF8): sprite row - 0x30 for the match card per segment (0xFFFF segs 0/1); 2AEA ids row-0x20 and 0x1E" },
};
TBL_REGISTER(attract_tables)

/* ---- shared page constants ---- */
#define SCENE_WORD     3         /* 0x748 / 0x7D6 / 0x81D0 / 0x69E0 */
#define ARENA_PAL      3
#define FBI_CAM_X      0x280     /* 0x76A */
#define FBI_CAM_Y      0x600
#define TECH_CAM_X     0x3C0     /* 0x808 */
#define TECH_CAM_Y     0x600
#define TITLE_CAM_X    0x280     /* 0x81D8 compose window; the raw scroll pan
                                    (0x8244 $100000/$100004) wraps inside its
                                    512px VRAM ring, engine: cam = window +
                                    (pan & 0x1FF) */
#define TITLE_CAM_Y    0x400
#define FADE_STEPS     8         /* 0x26920 / 0x26560 */
#define STEP_FRAMES    4         /* one write + three vblank waits per step */
#define HOLD_FRAMES    0x80      /* 0x1E8A */
#define PRELUDE_FRAMES 16        /* oracle f0-15 */
#define FADEOUT_LEAD   3         /* 0x264E2 head */
#define GAP_FRAMES     5         /* oracle f208-212 */
#define TITLE_PAN_PRE  0x80      /* 0x8260 cmpi #0x80 */
#define PARADE_DWELL   3         /* 0x8326 moveq #3 */
#define PARADE_DWELL4  0x19      /* 0x8332 (cell 4) */
#define PARADE_CELLS   5         /* 0x8348 cmpi #5 */
#define PARADE_N       12        /* 0x8360 cmpi #0xC */
#define TITLE_BLINKF   0x5C8     /* 0x8482 */
#define CARD_BLINKF    0x100     /* 0x86BA (0x8490 variant) */
#define BLIT_LOGO      1u        /* 0x836E */
#define BLIT_TM        4u        /* 0x7AC */
#define RANK_HOLD      0x100     /* 0xB52 */
#define DEMO_SEGS      0xE       /* 0x1136 cmpi #0xE */
#define WIPE_STEPS     8         /* 0x8FB6 / 0x903C */
#define WIPE_FRAMES    3         /* 0x8F9E $1C0082 */
#define WIPE_TAIL      0x20      /* 0x8FC0 -> 0x21E6 */

static const unsigned bank[4] = { 0x180000, 0x182000, 0x186000, 0x188000 };
static int att_cam_x = TITLE_CAM_X, att_cam_y = TITLE_CAM_Y;   /* WF_ATTCAM poke */

enum { PG_FBI, PG_TM, PG_TECH, PG_TITLE, PG_RANK, PG_CARD, PG_NAME };
enum { PH_PRELUDE, PH_FADEIN, PH_HOLD, PH_FADEOUT, PH_GAP };

static struct {
    int      page;
    int      phase;
    unsigned t;              /* frames into the phase */
    int      step;           /* fade step on screen (0 = black, 7 = full) */
    int      irq3;           /* $1C0076 bit 7: START / credit line live */
    int      need_snap;      /* 0x26772 snapshot pending (first draw) */
    uint16_t snap[4][16][16];/* $1C1CD4..: 4 banks x 16 lines x 16 colours */
    long     loops;
    /* title page (0x81BA / 0x8490) */
    unsigned pan_x, pan_y;   /* $1C1CD4/$1C1CD6 raw scroll pan */
    unsigned cd8;            /* $1C1CD8 frame counter */
    int      tphase;         /* 0 pan, 1 parade, 2 blink */
    int      pi;             /* parade slot */
    unsigned pcell, ptick;   /* +0x24 / +0x22 */
    int      pstatic;        /* 0x8490: all slots on cell 4 */
    unsigned blink_total;    /* 0x5C8 or 0x100 */
    /* rank page */
    int      rank_rumble;
    /* demo cycle */
    int      demo_seg;       /* $1C0166; -1 = not cycling */
    int      demo_pending;   /* attract must hand the frame to the match */
    int      arm_talk;       /* 0xB1C jsr 0xAE20: LOD taunt mode 1/2 owed */
    int      talk_done;      /* the taunt ran; next is the ranking page */
} at;

/* demo-match side (runs while ENG_SCENE_MATCH owns the frame) */
static struct {
    int      active;
    unsigned t;              /* $1C0080 live-frame counter */
    int      wipe;           /* 0 none, 1 wipe-in (0x8FD6), 2 wipe-out (0x8F50) */
    unsigned wipe_t;
    int      picks[4];
    /* 0x8CF8 overlay ($1C14CE card slot) */
    int      card;           /* 0 pending, 1 live, 2 done (bit7 / bit6) */
    unsigned card_cell;      /* +0x24 blink 0/1, then 2 */
    unsigned card_dwell;     /* +0x22 */
} demo;

void eng_scene_publish(unsigned arena, unsigned textset);   /* scene.c */
extern int eng_scene_blank;                                 /* render.c: black bg (0x1F6C) */
extern int eng_compose_freeze;                              /* render.c: 0x81BA frozen compose */
extern int eng_pan_x0, eng_pan_x1;                          /* render.c: raw plane X regs (parallax) */

/* 0x26642: one colour word through the fade table at `step`. */
static unsigned fade_word(unsigned w, int step)
{
    unsigned r = 0;
    for (int sh = 8; sh >= 0; sh -= 4) {
        unsigned nib = (w >> sh) & 0xFu;
        r = (r << 4) | tbl8(TBL(palette_fade_steps), nib * 8u + (unsigned)step);
    }
    return r & 0xFFFu;
}

/* 0x26732: 16 lines x 16 words out of each bank (line stride 0x80). */
static void snapshot(void)
{
    for (int b = 0; b < 4; b++)
        for (unsigned l = 0; l < 16; l++)
            for (unsigned k = 0; k < 16; k++)
                at.snap[b][l][k] = (uint16_t)m68k_read_memory_16(bank[b] + l * 0x80u + k * 2u);
}

/* 0x26752 with the 0x26642 map applied: write the snapshot at `step`. */
static void write_step(int step)
{
    for (int b = 0; b < 4; b++)
        for (unsigned l = 0; l < 16; l++)
            for (unsigned k = 0; k < 16; k++)
                m68k_write_memory_16(bank[b] + l * 0x80u + k * 2u,
                                     fade_word(at.snap[b][l][k], step));
}

static int dbg(void) { return getenv("WF_DBGSEL") != NULL; }

/* ---- page setups --------------------------------------------------- */
static void clears(void)
{
    memset(wf.spriteram, 0, sizeof wf.spriteram);             /* 0x1FDE */
    memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);       /* 0x1F9E */
    eng_credit_force();                                       /* bset 7,$1C0072 */
}

static void page_setup(eng_state *st)
{
    at.phase = PH_PRELUDE;
    at.t = 0; at.step = 0; at.irq3 = 0; at.need_snap = 1;
    eng_scene_blank = 0;
    eng_compose_freeze = 0; eng_pan_x0 = eng_pan_x1 = -1;
    clears();
    switch (at.page) {
    case PG_FBI:                                   /* 0x72E */
        st->scene = SCENE_WORD; st->cam_x = FBI_CAM_X; st->cam_y = FBI_CAM_Y;
        eng_scene_publish(ARENA_PAL, 0);           /* 0x750; $1C15FC still 0 */
        break;
    case PG_TM:                                    /* 0x790: cleared tilemaps */
        st->scene = SCENE_WORD;
        eng_scene_blank = 1;                       /* 0x1F6C, no 0x26E66 */
        eng_scene_publish(ARENA_PAL, 0);
        eng_blit(BLIT_TM);                         /* 0x7AC..0x7B0 D0=4 */
        break;
    case PG_TECH:                                  /* 0x7C6 */
        st->scene = SCENE_WORD; st->cam_x = TECH_CAM_X; st->cam_y = TECH_CAM_Y;
        eng_scene_publish(ARENA_PAL, 3);           /* 0x7EE $1C15FC = 3 */
        eng_banner_runs(0xC1044u, 0x27, 0x20);     /* 0x802 jsr 0xA0AE */
        break;
    case PG_TITLE:                                 /* 0x91C + 0x81BA */
    case PG_CARD:                                  /* 0x8490 (0x100-frame variant) */
        if (at.page == PG_TITLE) eng_sound(0x3102);/* 0x91C */
        st->scene = SCENE_WORD;                    /* 0x81D0 */
        if (getenv("WF_ATTCAM")) {                 /* harness poke: probe the map */
            int px = TITLE_CAM_X, py = TITLE_CAM_Y;
            sscanf(getenv("WF_ATTCAM"), "%x,%x", &px, &py);
            att_cam_x = px; att_cam_y = py;
        } else { att_cam_x = TITLE_CAM_X; att_cam_y = TITLE_CAM_Y; }
        /* 0x81D8/0x81E8: compose the window once (over 0x1F6C-cleared
         * planes), then pan the raw scroll regs (0x8244). */
        st->cam_x = att_cam_x; st->cam_y = att_cam_y;
        eng_compose_freeze = 1;
        at.pan_x = 2; at.pan_y = 1;                /* 0x821A first write */
        eng_pan_x0 = (int)at.pan_x; eng_pan_x1 = (int)at.pan_y;
        /* 0x948 title: $1C15FC = 0.  0x8490 (the demo's title card) never
         * writes $1C15FC — it inherits the demo match's set 2 (0xBC2) —
         * but on the engine's set-2 palettes the logo blit went murky
         * (playtest V431 "second appearance palette issue"), so the card
         * takes the title page's set 0.  TODO EXACT: trace the stock
         * $1C15FC at 0x8490 / the set-2 FG0 lines. */
        eng_scene_publish(ARENA_PAL, 0);
        eng_sprite_scene_pals_begin();             /* fresh 0x2AEA rows 0x20..0x2D */
        at.tphase = (at.page == PG_TITLE) ? 0 : 2;
        at.pi = 0; at.pcell = 0; at.ptick = 0; at.cd8 = 0;
        at.pstatic = (at.page == PG_CARD);
        at.blink_total = (at.page == PG_TITLE) ? TITLE_BLINKF : CARD_BLINKF;
        at.irq3 = 1;                               /* 0x92A bset 7,$1C0076 */
        at.phase = PH_HOLD;                        /* no fade: 0x2A06 cuts in */
        if (at.page == PG_CARD) {                  /* 0x85A6: card draws the texts at once */
            eng_blit(BLIT_LOGO);
            eng_banner_runs(0xC0344u, 0xD, 0x10);  /* 0xA44E */
            eng_banner_runs(0xC050Cu, 0xE, 0x60);
            eng_banner_runs(0xC0A10u, 0xF, 0x70);
        }
        break;
    case PG_RANK:                                  /* 0xB4C jsr 0x69C8 */
        eng_rank_page_begin(st, at.rank_rumble);
        at.irq3 = 1;
        break;
    case PG_NAME:                                  /* 0x65C0 body */
        eng_rank_page_begin(st, eng_rank_entry_rumble());
        eng_sound(0x310E);                         /* 0x66B8 */
        at.irq3 = 1;
        break;
    }
    if (dbg())
        fprintf(stderr, "attract: page %d f%lld (seg %d)\n", at.page,
                (long long)0, at.demo_seg);
}

/* ---- the demo cycle ------------------------------------------------- */
static const int *demo_ids(int seg)
{
    static int p[4];
    /* ROM 0xE20 seats slot i with 0xF3E[seg*4+i]; slot pairs are 0-1 and
     * 2-3 (opponent pointers 0xF7C).  The engine's tag layout pairs the
     * leaders in slots 0/2, so the rows map {0,2,1,3} to keep the stock
     * in-ring pairing (row0 vs row1).  TODO EXACT: stock demo objects
     * carry no team/legal bits — four independent men, obj0 vs obj1. */
    p[0] = tbl8(TBL(demo_picks), (unsigned)seg * 4u);
    p[2] = tbl8(TBL(demo_picks), (unsigned)seg * 4u + 1u);
    p[1] = tbl8(TBL(demo_picks), (unsigned)seg * 4u + 2u);
    p[3] = tbl8(TBL(demo_picks), (unsigned)seg * 4u + 3u);
    return p;
}

int eng_demo_active(void) { return demo.active; }

/* interlude.c: a START during the demo taunt screen (IRQ3 0x978, live
 * through 0xAE20) leaves the whole cycle — drop the demo state so a
 * later game-over re-enters the attract at the FBI card (0x6FC). */
void eng_attract_reset_cycle(void)
{
    at.demo_seg = -1;
    at.demo_pending = 0;
    at.arm_talk = 0;
    at.talk_done = 0;
    demo.active = 0;
}
int eng_demo_rumble(void) { return demo.active && at.demo_seg == 1; }   /* 0xE2C */

const uint8_t *eng_demo_rumble_six(void)
{
    static uint8_t six[6];
    if (!eng_demo_rumble()) return NULL;
    for (unsigned i = 0; i < 6; i++)               /* 0xEE6 table 0xF76 */
        six[i] = tbl8(TBL(demo_rumble_six), i);
    return six;
}

const int *eng_demo_picks(void) { return demo.picks; }

/* scene.c calls this right after eng_init_picks on the demo transition
 * (ROM 0xB1C tail: no aisle, no 0xA654 intro, no stage music). */
void eng_demo_seed(eng_state *st)
{
    if (!demo.active) return;
    for (int i = 0; i < ENG_MAX_OBJS; i++) {       /* 0xE44/0xEF2 +0x56 = 0x80 */
        eng_obj *o = &st->obj[i];
        if (!o->active) continue;
        o->cpu = 1;
        o->input = -1;
        o->role &= (uint16_t)~0x02u;                /* nobody is a human */
    }
    if (at.demo_seg != 1) {                        /* 0xE6A/0xE8A energy */
        unsigned e = tbl16(TBL(demo_energy), (unsigned)at.demo_seg * 2u);
        for (int i = 0; i < 4; i++) {
            if (!st->obj[i].active) continue;
            if (st->obj[i].wrestler == demo.picks[0] && i == 0)
                st->obj[i].hp = st->obj[i].hp_max = 0x7000;    /* slot 0, 0xE6A */
            else
                st->obj[i].hp = st->obj[i].hp_max = (uint16_t)e;
        }
    }
    eng_intro_end(st);                             /* demo skips 0xA654 (0xB1C tail) */
    eng_scene_publish(0, 2);                       /* 0xBC2 $1C15FC = 2: the demo's FG0
                                                      text set (render publish keeps the
                                                      arena from st->scene in a match) */
    demo.t = 0;
    demo.wipe = 1; demo.wipe_t = 0;                /* 0xBB0 jsr 0x8FD6 wipe-in */
    if (dbg()) fprintf(stderr, "demo: seg %d match starts\n", at.demo_seg);
}

/* 0x8F50 / 0x8FD6: the FG0 curtain.  Fill every cell with the gradient
 * tile; wipe-out walks 0xBE -> 0xC5 (open -> black), wipe-in 0xC5 -> 0xBE. */
static void wipe_fill(unsigned step, int out)
{
    unsigned tile = out ? 0xBEu + step : 0xC5u - step;
    for (unsigned off = 0; off + 4 <= WF_FG0RAM_SIZE; off += 4) {
        wf.fg0_videoram[off]     = 0xFF;           /* long 0xFFxxFF04 pattern */
        wf.fg0_videoram[off + 1] = (uint8_t)tile;
        wf.fg0_videoram[off + 2] = 0xFF;
        wf.fg0_videoram[off + 3] = 0x04;
    }
}

/* One demo frame, called from eng_update before the match logic.
 * Returns 1 when the frame was consumed (wipe / scene switch). */
int eng_demo_frame(eng_state *st)
{
    if (!demo.active) return 0;
    /* IRQ3 0x978: START with a credit leaves for the game select. */
    if (eng_start_scan(st->inputs)) {
        demo.active = 0; demo.wipe = 0;
        at.demo_seg = -1;
        if (dbg()) fprintf(stderr, "demo: START -> game select\n");
        eng_scene_set(ENG_SCENE_GAMESELECT);
        return 1;
    }
    if (demo.wipe) {
        unsigned step = demo.wipe_t / WIPE_FRAMES;
        int out = (demo.wipe == 2);
        if (step < WIPE_STEPS) {
            wipe_fill(step, out);
            demo.wipe_t++;
            return 1;                              /* both wipes are straight-line vblank
                                                      loops in the ROM (0x8F84/0x900A): the
                                                      match is frozen — the clock's TIME
                                                      plate (0x26300 one-shot) must land
                                                      AFTER the wipe stops painting FG0 */
        }
        if (!out) {
            demo.wipe = 0;
            /* 0xBBC jsr 0x1F9E: the driver clears FG0 right after the
             * wipe-in; the clock's TIME plate (0x26300 one-shot) draws on
             * the first live frame after this, so it survives. */
            memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);
            eng_credit_force();
        }
        else if (demo.wipe_t++ < WIPE_STEPS * WIPE_FRAMES + WIPE_TAIL)
            return 1;                              /* 0x8FC0 wait 0x20 */
        else {
            demo.wipe = 0; demo.active = 0;
            at.demo_seg++;                         /* 0x112E $1C0166++ */
            at.demo_pending = 1;
            if (dbg()) fprintf(stderr, "demo: seg -> %d\n", at.demo_seg);
            eng_scene_set(ENG_SCENE_ATTRACT);      /* 0x1146 jmp 0xAC0 / 0x1140 jmp 0x6FC */
            return 1;
        }
    }
    /* 0x8CF8 (from the frame loop 0x1028): the demo overlay. */
    if (at.demo_seg == 0) {                        /* 0x8D02: how-to text (tag) */
        eng_blit(0x29);
        eng_banner_runs(0xC131Cu, 0x15, 0x70);
    } else if (at.demo_seg == 1) {                 /* 0x8D1E: rumble text */
        eng_blit(0x2A);
        eng_banner_runs(0xC1604u, 0x16, 0x60);
    } else {                                       /* 0x8D4A: the match card */
        unsigned s = (unsigned)at.demo_seg - 2u;
        if (demo.card == 0) {                      /* 0x8D6C first frame */
            eng_blit(6);
            eng_blit(7u + s);                      /* 0x8D84 team names */
            eng_blit(0x40u + s);                   /* 0x8D98 */
            demo.card = 1;
            demo.card_cell = 0;
            demo.card_dwell = 8;                   /* 0x8DBE */
        } else if (demo.card == 1) {
            if (demo.t >= 0x100)                   /* 0x8E02 */
                demo.card_cell = 2;
            else if (demo.card_dwell-- == 0) {     /* 0x8E12 blink */
                demo.card_dwell = (eng_rng() & 0xFu) + 8u;   /* 0x8E20 */
                demo.card_cell ^= 1u;              /* 0x8E32 */
            }
            if (demo.t >= 0x200) {                 /* 0x8E3E remove */
                demo.card = 2;
                eng_blit(0x8006u);                 /* 0x8E66 erases */
                eng_blit(0x8007u + s);
                eng_blit(0x8040u + s);
            }
        }
    }
    demo.t++;                                      /* 0x10F2 $1C0080 */
    if (demo.t >= tbl16(TBL(demo_durations), (unsigned)at.demo_seg * 2u)) {
        demo.wipe = 2; demo.wipe_t = 0;            /* 0x1120 jsr 0x8F50 */
        /* the match clock's FG0 cells (TIME + digits, rows 28-31) must not
         * survive onto the next card page — "time runs over demo"
         * (playtest 2026-08-24; stock's page path re-clears FG0 via 0x1F9E) */
        for (unsigned r = 28; r < 32; r++)
            if ((r + 1) * 0x100u <= WF_FG0RAM_SIZE)
                memset(wf.fg0_videoram + r * 0x100u, 0, 0x100);
        if (dbg()) fprintf(stderr, "demo: seg %d over at f%u\n", at.demo_seg, demo.t);
    }
    return 0;
}

/* render.c (match branch): the demo match-card sprite — $1C14CE slot,
 * row 0x30 + demo_card_rows[seg] at (0x28,0xA8), blink cell (0x8DAC..).
 * TODO EXACT: the 0x2AEA installs (row-0x20 and 0x1E) are not run, so
 * the card takes the stream's own palette bank. */
void eng_demo_overlay_emit(const eng_state *st)
{
    unsigned slot = 0x1E00;            /* engine: park it past the object pass */
    unsigned rowoff;
    (void)st;
    if (!demo.active || at.demo_seg < 2 || demo.card != 1) return;
    rowoff = tbl16(TBL(demo_card_rows), (unsigned)at.demo_seg * 2u);
    if (rowoff == 0xFFFFu) return;
    eng_sprite_emit_pose(0x30u + rowoff, demo.card_cell, 0x28, 0xA8, -1, &slot);
    /* eng_sprite_emit already committed SPRBUF this frame; add the card. */
    memcpy(wf.spriteram_buffered + 0x1E00, wf.spriteram + 0x1E00, WF_SPRRAM_SIZE - 0x1E00);
}

/* Start the demo match for the current segment. */
static int demo_match_go(void)
{
    eng_compose_freeze = 0; eng_pan_x0 = eng_pan_x1 = -1;
    eng_scene_blank = 0;
    demo.active = 1;
    demo.t = 0; demo.wipe = 0;
    demo.card = 0; demo.card_cell = 0; demo.card_dwell = 8;
    memcpy(demo.picks, demo_ids(at.demo_seg), sizeof demo.picks);
    return ENG_SCENE_MATCH;
}

/* Which pre-page (if any) opens segment `seg` (0xB1C). */
static int demo_prepage(int seg)
{
    if (seg == 1) { at.rank_rumble = 0; return PG_RANK; }   /* 0xB20 */
    if (seg == 2) { at.rank_rumble = 1; return PG_RANK; }   /* 0xB2A/0xB38 $1C0161=1 */
    if (seg == 6 || seg == 0xA) return PG_CARD;             /* 0xB6A/0xB74 */
    return -1;
}

/* ---- scene ops ------------------------------------------------------ */
static void at_begin(eng_state *st)
{
    int keep_seg = at.demo_seg, pend = at.demo_pending, tdone = at.talk_done;
    long loops = at.loops;
    memset(&at, 0, sizeof at);
    at.loops = loops;
    at.demo_seg = keep_seg; at.demo_pending = pend; at.talk_done = tdone;
    eng_sound(0x3100);                              /* 0x71C (attract entry silence) */
    if (eng_rank_entry_pending()) {                 /* 0x65C0 before jmp 0x6FC */
        at.page = PG_NAME;
        at.demo_seg = -1; at.demo_pending = 0; at.talk_done = 0;
    } else if (at.demo_pending && at.demo_seg >= 0 && at.demo_seg < DEMO_SEGS) {
        int pre = demo_prepage(at.demo_seg);        /* 0x1146 jmp 0xAC0 */
        /* 0xB1C: segments 1/2 open with the LOD taunt screen (jsr 0xAE20
         * D0=1/2, 0xB40) before the fade + ranking page (0xB46/0xB4C). */
        if ((at.demo_seg == 1 || at.demo_seg == 2) && !at.talk_done) {
            at.arm_talk = at.demo_seg;              /* handled in at_update */
            return;
        }
        at.talk_done = 0;
        at.demo_pending = 0;
        if (pre >= 0) at.page = pre;
        else { at.page = PG_TITLE;                  /* placeholder, replaced below */
               at.demo_pending = 2; }               /* straight to the match */
    } else {                                        /* 0x6FC / 0x1140 jmp 0x6FC */
        at.demo_seg = -1; at.demo_pending = 0;
        at.page = (eng_dip_word() & 0x80u) ? PG_TM : PG_FBI;   /* 0x724 */
        at.loops++;
    }
    if (at.demo_pending != 2)
        page_setup(st);
    if (dbg())
        fprintf(stderr, "attract: begin f%lld page %d seg %d\n",
                (long long)st->frame, at.page, at.demo_seg);
}

/* page finished -> what next (0x6FC straight-line order). */
static int page_advance(eng_state *st)
{
    switch (at.page) {
    case PG_NAME:                                   /* rts -> jmp 0x6FC */
        at.page = (eng_dip_word() & 0x80u) ? PG_TM : PG_FBI;
        break;
    case PG_FBI:  at.page = PG_TM;   break;         /* 0x790 */
    case PG_TM:   at.page = PG_TECH; break;         /* 0x7C6 */
    case PG_TECH: at.page = PG_TITLE; break;        /* 0x830 bra 0x91C */
    case PG_TITLE:                                  /* 0x974 bra 0xAC0, seg 0 */
        eng_sound(0x3101);                          /* 0x96A */
        at.demo_seg = 0;
        return demo_match_go();
    case PG_RANK:                                   /* 0xB5C fade-out then the match */
    case PG_CARD:
        return demo_match_go();
    }
    page_setup(st);
    return -1;
}

/* ---- the title/card page body (0x81BA / 0x8490) -------------------- */
static void title_texts_blink(void)
{
    if (at.cd8 & 0x40u) {                           /* 0x83CA btst #6,$1C1CD9 */
        eng_blit(0x8056u);                          /* 0x83D4 erase PUSH START */
        eng_blit(0x8054u);
    } else {
        unsigned id;
        unsigned credits = eng_credits();
        unsigned coins = (eng_credit_word() >> 8) & 0xFFu;
        if (credits == 0) {                         /* 0x8416 tst.b $1C004F */
            if (coins == 0) id = 2;                 /* 0x841E "INSERT COIN" */
            else if (coins == 2) id = 0x53;         /* 0x842E */
            else if ((eng_dip_word() & 3u) == 3u) id = 0x54;   /* 0x843C coin dip */
            else id = 0x53;                         /* 0x844C */
        } else {
            id = 3;                                 /* 0x8452 */
            if (eng_dip_word() & 8u) id = 0x55;     /* 0x8456 tag dip -> PUSH START
                                                       (0x8460 $1C1278 stays 0 here) */
        }
        eng_blit(id);                               /* 0x847C */
    }
}

static int title_update(eng_state *st)
{
    /* the pan runs on every title frame (0x8244 / 0x830C / 0x83AE): the
     * raw scroll regs slide the frozen compose around the 512px ring */
    at.pan_x += 2; at.pan_y += 1;
    eng_pan_x0 = (int)(at.pan_x & 0xFFFFu);
    eng_pan_x1 = (int)(at.pan_y & 0xFFFFu);
    switch (at.tphase) {
    case 0:                                         /* 0x821A pre-roll */
        if (++at.cd8 >= TITLE_PAN_PRE) { at.tphase = 1; at.cd8 = 0; }
        break;
    case 1:                                         /* 0x8272 parade */
        at.ptick++;                                 /* 0x8320 */
        {
            unsigned dwell = (at.pcell == 4) ? PARADE_DWELL4 : PARADE_DWELL;   /* 0x832A */
            if (at.ptick > dwell) {
                at.ptick = 0;
                if (++at.pcell >= PARADE_CELLS) {   /* 0x8348 */
                    at.pcell = 0;
                    if (++at.pi >= PARADE_N) {      /* 0x8360 */
                        at.tphase = 2; at.cd8 = 0;
                        eng_blit(BLIT_LOGO);        /* 0x836E D0=1 */
                        eng_banner_runs(0xC0344u, 0xD, 0x10);   /* 0x8378 jsr 0xA44E */
                        eng_banner_runs(0xC050Cu, 0xE, 0x60);
                        eng_banner_runs(0xC0A10u, 0xF, 0x70);
                    }
                }
            }
        }
        break;
    case 2:                                         /* 0x837E blink loop */
        title_texts_blink();
        if (++at.cd8 >= at.blink_total)             /* 0x8482 / 0x86BA */
            return 1;
        break;
    }
    return 0;
}

static void title_draw(const eng_state *st)
{
    unsigned slot = 0;
    eng_sprite_scene_pals_rearm();      /* the per-frame pan recompose reloads
                                           the palettes (render.c) and would
                                           flush the 0x2AEA row banks */
    memset(wf.spriteram, 0, WF_SPRRAM_SIZE);
    for (int i = 0; i < PARADE_N; i++) {            /* 0x82A4 compose loop */
        unsigned row  = tbl16(TBL(title_parade), (unsigned)i * 6u);
        int      x    = (int)tbl16(TBL(title_parade), (unsigned)i * 6u + 2u);
        int      y    = (int)tbl16(TBL(title_parade), (unsigned)i * 6u + 4u);
        unsigned cell;
        if (at.pstatic) cell = 4;                   /* 0x8506 card variant */
        else if (at.tphase == 2 || i < at.pi) cell = 4;   /* finished slots hold 4 */
        else if (i == at.pi && at.tphase == 1) cell = at.pcell;   /* 0x8296 */
        else continue;                              /* not activated yet */
        eng_sprite_emit_pose(row, cell, x, y, -1, &slot);
    }
    memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);   /* SPRBUF (interlude.c pattern) */
    (void)st;
}

/* ---- update / draw -------------------------------------------------- */
static int at_update(eng_state *st)
{
    if (at.arm_talk) {                              /* 0xB40 jsr 0xAE20 D0=1/2 */
        int mode = at.arm_talk;
        at.arm_talk = 0;
        at.talk_done = 1;                           /* demo_pending stays: the taunt
                                                       returns here for the rank page */
        eng_interlude_arm_talk(mode);
        return ENG_SCENE_INTERLUDE;
    }
    if (at.demo_pending == 2) {                     /* jmp 0xAC0, no pre-page */
        at.demo_pending = 0;
        return demo_match_go();
    }
    /* IRQ3 (0x834): credit line 0x900 + START 0x978 -> 0x8F2 jmp 0x52BE. */
    if (at.irq3) {
        eng_credit_line();
        if (eng_start_scan(st->inputs)) {
            if (dbg())
                fprintf(stderr, "attract: START f%lld -> game select\n", (long long)st->frame);
            at.demo_seg = -1; demo.active = 0;
            eng_compose_freeze = 0; eng_pan_x0 = eng_pan_x1 = -1;
            eng_scene_blank = 0;
            return ENG_SCENE_GAMESELECT;
        }
    }
    at.t++;
    /* The title/card pages run without the palette fade machine. */
    if (at.page == PG_TITLE || at.page == PG_CARD) {
        at.step = FADE_STEPS - 1;
        if (title_update(st))
            return page_advance(st);
        return -1;
    }
    if (at.page == PG_NAME) {                       /* 0x66FE entry loop */
        at.step = FADE_STEPS - 1;
        if (at.phase == PH_PRELUDE) {               /* fade the page in (0x69C8 tail 0x26844) */
            at.phase = PH_FADEIN; at.t = 0; at.irq3 = 0;
            return -1;
        }
        if (at.phase == PH_FADEIN) {
            at.step = (int)(at.t / STEP_FRAMES);
            if (at.step >= FADE_STEPS - 1) at.step = FADE_STEPS - 1;
            if (at.t >= FADE_STEPS * STEP_FRAMES - 2) { at.phase = PH_HOLD; at.t = 0; at.irq3 = 1; }
            return -1;
        }
        if (at.phase == PH_HOLD) {
            if (!eng_rank_entry_frame(st)) {        /* 0x6794 all done */
                at.phase = PH_FADEOUT; at.t = 0; at.irq3 = 0;
            }
            return -1;
        }
        if (at.phase == PH_FADEOUT) {
            if (at.t >= FADEOUT_LEAD)
                at.step = FADE_STEPS - 1 - (int)((at.t - FADEOUT_LEAD) / STEP_FRAMES);
            if (at.step < 0) at.step = 0;
            if (at.t >= FADEOUT_LEAD + FADE_STEPS * STEP_FRAMES - 1)
                return page_advance(st);            /* 0x678C 0x264E2 done -> 0x6FC */
            return -1;
        }
    }
    /* FBI / TM / TECH / RANK: the fade cycle (0x26844 / 0x1E6C / 0x264E2). */
    switch (at.phase) {
    case PH_PRELUDE:
        at.step = 0;
        if (at.t >= PRELUDE_FRAMES) { at.phase = PH_FADEIN; at.t = 0; at.irq3 = 0; }
        break;
    case PH_FADEIN:                                 /* 0x26844 */
        at.step = (int)(at.t / STEP_FRAMES);
        if (at.step >= FADE_STEPS - 1) at.step = FADE_STEPS - 1;
        if (at.t >= FADE_STEPS * STEP_FRAMES - 2) { /* 0x26928 bset 7 */
            at.phase = PH_HOLD; at.t = 0; at.irq3 = 1;
        }
        break;
    case PH_HOLD:                                   /* 0x1E6C / 0xB52 0x21E6 */
        at.step = FADE_STEPS - 1;
        if (at.t >= (unsigned)(at.page == PG_RANK ? RANK_HOLD : HOLD_FRAMES)) {
            at.phase = PH_FADEOUT; at.t = 0; at.irq3 = 0;
        }
        break;
    case PH_FADEOUT:                                /* 0x264E2 */
        if (at.t >= FADEOUT_LEAD) {
            at.step = FADE_STEPS - 1 - (int)((at.t - FADEOUT_LEAD) / STEP_FRAMES);
            if (at.step < 0) at.step = 0;
        }
        if (at.t >= FADEOUT_LEAD + FADE_STEPS * STEP_FRAMES - 1) {
            at.phase = PH_GAP; at.t = 0; at.irq3 = 1;   /* 0x26636 restores $1C0076 */
        }
        break;
    case PH_GAP:                                    /* the black beat between cards */
        at.step = 0;
        if (at.t >= GAP_FRAMES)
            return page_advance(st);
        break;
    }
    if (dbg() && (st->frame % 64) == 0)
        fprintf(stderr, "attract: f%lld page %d ph=%d t=%u step=%d credits=%u\n",
                (long long)st->frame, at.page, at.phase, at.t, at.step, eng_credits());
    return -1;
}

static void at_draw(const eng_state *st)
{
    if (at.page == PG_TITLE || at.page == PG_CARD)
        title_draw(st);
    else if (at.page == PG_RANK)
        eng_rank_page_frame(at.rank_rumble);        /* static rows (blink lives in PG_NAME) */
    if (at.need_snap) { snapshot(); at.need_snap = 0; }       /* 0x26772 */
    if (at.page != PG_TITLE && at.page != PG_CARD)
        write_step(at.step);
}

static const eng_scene_ops at_ops = { at_begin, at_update, at_draw };

void eng_attract_register(void)
{
    at.demo_seg = -1;
    eng_rank_reset();                               /* 0x6F6 power-on defaults */
    eng_scene_register(ENG_SCENE_ATTRACT, &at_ops);
}
