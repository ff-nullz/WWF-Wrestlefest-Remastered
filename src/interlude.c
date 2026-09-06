/* Between-match interludes — the three screens the campaign ladder runs
 * between matches, transcribed from the ROM:
 *
 *   0x1A6A  (after a win, before the stage bump)
 *       jsr 0xB608   title-win card: only when the word $1C0162 == 4 (the
 *                    first LOD title match was just won) and not rumble
 *                    ($1C0161 bit0).  Human-pair portraits (row 0x4D) +
 *                    the animated centre piece (row 0x4E, palette 0x37) +
 *                    a 17-object lettering marquee (row 0x4E, palette
 *                    0x3C) sliding right-to-left.
 *       jsr 0xBDA6   belt scene: D0 = stage word (+1 with dip $1C0066
 *                    bit7); D0 < 3 or 5..7 shows the "matches until the
 *                    title match" count art (row 0x4F cells idx*7+4), the
 *                    other stages skip (0xBDDE..0xBDF2).  The LOD panels
 *                    (row 0x2C, from 0xAE20 mode 3) sit left/right.
 *   0xBD2   (match start, after the stage bump, before the 0x7B70 aisle)
 *       stage 4 or 9 (0xBDE/0xBE8):
 *       jsr 0xAE20 (D0=0)  the LOD talk screen: three row-0x2C panels
 *                    (0xB0C2), per-panel mouth-cell scripts (0xB19C) with
 *                    speech samples (0xB14A), text blits 0x27/0x28 and
 *                    the aisle bottom bar (0xB29E); jsr 0x264E2 fade-out.
 *
 * The engine runs them as one scene (ENG_SCENE_INTERLUDE) with a queue
 * armed by campaign.c ladder_step(): TITLE -> BELT -> TALK in ROM order,
 * then the aisle.  A continue rematch arms nothing (ROM: 0x1A4A jumps
 * past 0x1A6A, and 0xAE20 exits on $1C008C).
 *
 * TODO EXACT: the palette fades 0x26772 / 0x26844 (in) and 0x264E2 (out)
 * around each screen are not run (the engine's fade lives in attract.c);
 * the screens cut in and out. */
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
static const tbl_def interlude_tables[] = {
    { "interlude_panel_setup", "base/front", 0xB0C2, 6, TK_U8, 1,
      "0xAEDA/0xAEA8: LOD talk screen, three row-0x2C panel x's (0x48/0x98/0xF8) then the centre-panel start cell per 0xAE20 mode 0..2 (0x11/0x0A/0x15)" },
    { "interlude_voice_cmds", "base/front", 0xB14A, 4 * 2, TK_U16, 1,
      "0xB102: YM speech commands fired in order by script cells with bit7 ($1C1276 index)" },
    { "interlude_talk_script_ptrs", "base/front", 0xB19C, 3 * 4, TK_U32, 1,
      "0xB0DC: long -> {cell, duration} byte-pair script per talk-screen panel 0..2; cell bit7 = speech, duration 0xFE = end" },
    { "interlude_talk_scripts", "base/front", 0xB1A8, 0xB29E - 0xB1A8, TK_U8, 2,
      "0xB0F0/0xB12A: the three panel scripts interlude_talk_script_ptrs points into" },
    { "interlude_title_cards", "base/front", 0xB814, 11 * 3, TK_U8, 3,
      "0xB6C2/0xB6E4: title-win card, {x slot 0, x slot 1, palette} per wrestler id 0..0xA (id 0xB uses row 0xA, 0xB6A0)" },
    { "interlude_title_bigcells", "base/front", 0xB936, 22, TK_U8, 1,
      "0xB8BE: title-win card centre object (row 0x4E) cell script, 8 ticks each; 0xFF wraps to step 3 (0xB920)" },
    { "interlude_name_cells", "base/front", 0xBA24, 12 * 3, TK_U8, 3,
      "0xB9EC: title-win card lettering, three row-0x4E name cells per wrestler id (0 = none)" },
    { "interlude_letter_widths", "base/front", 0xBAAC, 23 * 2, TK_S16, 1,
      "0xBA8E/0xBB02: per lettering cell 0xE..0x24 the x offset that releases the next letter (< 0x140) and kills this one (< 0)" },
    { "interlude_belt_objs", "base/front", 0xBF58, 6, TK_U8, 3,
      "0xBE32..0xBE46: belt scene {x, y, palette} for the herald (row 0), then the count art (row 1); read column-wise by object index" },
    { "interlude_belt_smallcells", "base/front", 0xBFFE, 6, TK_U8, 1,
      "0xBF86: belt-scene herald (row 0x4F) cell script {3,1,2,1,0,FF}, 10 ticks each; cell 2 starts the count art + voice 0x312A" },
};
TBL_REGISTER(interlude_tables)

/* ---- shared scene constants ---- */
#define SCENE_WORD_IL   4          /* 0xAE56 / 0xB646 $1C007E */
#define ARENA_PAL_IL    4          /* $1C15F4/$1C15F8 = scene word */
#define PRIORITY_IL     0x7B      /* 0xAE64 / 0xB654 $140010 */
#define PANEL_ROW       0x2Cu     /* row of the big LOD art (0xAEEC: +0x03 = 0x2C) */
#define PANEL_Y         0x77      /* 0xAEE6 */
#define PANEL_PAL0      0x31u     /* 0xAF0A: 0x2AEA ids 0x31 + panel index */
#define BLIT_CHAMP      0x27u     /* 0xAF62 "WRESTLEFEST TAG TEAM CHAMPION" at $C1908 */
#define BLIT_LOD        0x28u     /* 0xAF6C "THE LEGION OF DOOM" at $C1B08 */

/* ---- talk screen (0xAE20 mode 0) ---- */
#define TALK_SCROLL_X   0x280     /* 0xAE46 */
#define TALK_SCROLL_Y   0x200     /* 0xAE4E */
#define TALK_B2_AT      0x1B      /* 0xB00A: centre step that starts the right panel */
#define TALK_B0_AT      0x2E      /* 0xB020: ... the left panel */
#define TALK_END_AT     0x1C      /* 0xB086: left-panel step count that ends the scene */

/* ---- belt scene (0xBDA6) ---- */
#define BELT_PANEL0_X   0x60      /* 0xBDBC $1C126C */
#define BELT_PANEL2_X   0xE0      /* 0xBDC4 $1C12C0 */
#define BELT_ROW        0x4Fu     /* 0xBE24 */
#define BELT_CELL_BASE  4u        /* 0xBDFE: cell = idx*7 + 4 */
#define BELT_CELL_STEP  7u        /* 0xBDFA mulu #7 */
#define BELT_HERALD_CELL 3u       /* 0xBE08 $1C1304 */
#define BELT_HERALD_WAIT 0x40     /* 0xBEEC $1C1322 */
#define BELT_HERALD_TICK 0xA      /* 0xBF76 */
#define BELT_ART_TICK   8         /* 0xBFDC */
#define BELT_ART_STEPS  7         /* 0xBFE8: cell advances while step < 7 */
#define BELT_END_AT     0x10      /* 0xBF3E: count-art step count that ends the scene */
#define BELT_VOICE      0x312Au   /* 0xBFB8 */
#define BELT_RUN_MAIN   0x10u     /* 0xBEBE D2, at $C1540, nibble 0x10 */
#define BELT_RUN_LAST   0x17u     /* 0xBEDA D2, at $C1578, when cell base == 0x12 */

/* ---- title-win card (0xB608) ---- */
#define TITLE_SCROLL_X  0x140     /* 0xB636 */
#define TITLE_SCROLL_Y  0x300     /* 0xB63E */
#define PORTRAIT_ROW    0x4Du     /* 0xB6D4 */
#define PORTRAIT_Y      0x7E      /* 0xB6CE */
#define TITLE_BIG_ROW   0x4Eu     /* 0xB714 */
#define TITLE_BIG_X     0xF0      /* 0xB71A */
#define TITLE_BIG_Y     0x6E      /* 0xB720 */
#define TITLE_BIG_WAIT  0x40      /* 0xB726 */
#define TITLE_BIG_PAL   0x37u     /* 0xB738 */
#define TITLE_BIG_TICK  8         /* 0xB8CA */
#define TITLE_BIG_WRAP  3         /* 0xB924: 0xFF -> step 3 */
#define TITLE_PRIO_ON   0x78u     /* 0xB8E8 $140010 at centre step 1 */
#define LETTER_N        17        /* 0xB7D6 cmpi #0x11 */
#define LETTER_ROW      0x4Eu     /* 0xB95E */
#define LETTER_X0       0x1BF     /* 0xB96A / 0xBAA0 */
#define LETTER_Y        0xC       /* 0xB964 */
#define LETTER_NAME_Y   0xF       /* 0xB9C0: the name cells sit lower */
#define LETTER_PAL      0x3Cu     /* 0xB94C / 0xB87A 0x2AEA id */
#define LETTER_SPEED    2         /* 0xBA52 subi #2 */
#define LETTER_GATE_X   0x140     /* 0xBA94 */
#define TITLE_MUSIC0    0x3100u   /* 0xB778 */
#define TITLE_MUSIC1    0x3107u   /* 0xB782 */
#define TALK_MUSIC0     0x3100u   /* 0xAF90 */
#define TALK_MUSIC1     0x3110u   /* 0xAF9A */

enum { IL_TITLE, IL_BELT, IL_TALK, IL_BRACKET };

typedef struct {
    int      on;                 /* +0x00 bit7 */
    int      done;               /* +0x00 = 0x4000 (letters, 0xBB0C) */
    int      x, y;               /* +0x14 / +0x16 */
    unsigned row;                /* +0x02 */
    unsigned cell;               /* +0x04 (0xFFFF = hidden) */
    int      cnt;                /* +0x22 */
    unsigned step;               /* +0x24 */
} il_obj;

static struct {
    int    queue[3], qn, qi;
    int    belt_idx;             /* 0..2 */
    int    cur;                  /* current sub-screen (queue[qi]) */
    il_obj o[3 + LETTER_N];      /* talk/belt: 0..4; title: 0,1 portraits, 2 big, 3.. letters */
    unsigned portrait_id[2];     /* title card +0x04 of $1C1134/$1C1164; 0xFF = clone slot */
    const char *cname[2];        /* clone: name drawn as the standard big font (user
                                    2026-08-24: no bespoke lettering for new wrestlers) */
    int    voice;                /* $1C1276 / 2 */
    int    fired_b2, fired_b0;   /* talk one-shots */
    int    talk_mode;            /* $1C1278: 0xAE20 D0 — 0 campaign, 1/2 attract demo taunts */
    int    demo_return;          /* armed by the attract: finish -> ENG_SCENE_ATTRACT */
    int    fired_show;           /* title: $1C127A gate (0xB8D6) */
    unsigned prio;               /* $140010: 0x7B at setup, 0x78 from 0xB8E8 */
} il;

static int dbg(void) { return getenv("WF_DBGSEL") != NULL; }

int eng_interlude_arm2(int title, int belt_idx, int talk, int bracket)
{
    il.qn = 0;
    if (bracket)       il.queue[il.qn++] = IL_BRACKET; /* engine: the 1v1 TOURNAMENT card */
    if (title)         il.queue[il.qn++] = IL_TITLE;   /* 0x1A6A jsr 0xB608 */
    if (belt_idx >= 0) il.queue[il.qn++] = IL_BELT;    /* 0x1A70 jsr 0xBDA6 */
    if (talk)          il.queue[il.qn++] = IL_TALK;    /* 0xBD2 jsr 0xAE20 */
    il.qi = 0;
    il.belt_idx = belt_idx;
    il.talk_mode = 0;
    il.demo_return = 0;
    return il.qn;
}

/* attract.c: the demo cycle's LOD taunt screens — 0xB1C runs jsr 0xAE20
 * with D0=1 (segment 1) / D0=2 (segment 2) right before the fade-out and
 * the ranking page (0xB40..0xB4C).  Mode picks the centre panel's start
 * cell (0xAEA8), which panel speaks and the end gate (0xAFA4..0xB0A8). */
int eng_interlude_arm_talk(int mode)
{
    il.qn = 1;
    il.queue[0] = IL_TALK;
    il.qi = 0;
    il.talk_mode = mode;
    il.demo_return = 1;
    return 1;
}

/* 0xB2E4: wipe the 5 FG0 rows under the talk-screen text ($C1908..). */
static void talk_text_wipe(void)
{
    for (unsigned row = 0; row < 5; row++)
        memset(wf.fg0_videoram + 0x1908 + row * 0x100u, 0, 0x24u * 4u);
}

static void sub_clear(eng_state *st)
{
    memset(&il.o, 0, sizeof il.o);
    memset(wf.spriteram, 0, sizeof wf.spriteram);              /* 0x1FDE */
    memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);        /* 0x1F9E */
    eng_credit_force();
    il.voice = 0;
    il.fired_b2 = il.fired_b0 = il.fired_show = 0;
    il.prio = PRIORITY_IL;                                     /* 0xAE64 / 0xB654 */
    st->scene = SCENE_WORD_IL;                                 /* $1C007E = 4 */
    eng_sprite_scene_pals_begin();                             /* fresh 0x2AEA slots */
}

/* ---- 0xAE20 (modes 0/3): the three row-0x2C LOD panels ---- */
static void panels_init(int mode3)
{
    for (int k = 0; k < 3; k++) {                              /* 0xAEC2 loop */
        il.o[k].on   = 1;
        il.o[k].row  = PANEL_ROW;
        il.o[k].x    = (int)tbl8(TBL(interlude_panel_setup), (unsigned)k);
        il.o[k].y    = PANEL_Y;
        il.o[k].cell = 0;
    }
    il.o[0].cell = 0x16;                                       /* 0xAEF2 $1C125C */
    /* 0xAEA8: centre start cell by mode (0x11 / 0x0A / 0x15) */
    il.o[1].cell = tbl8(TBL(interlude_panel_setup),
                        3u + (unsigned)(il.talk_mode < 3 ? il.talk_mode : 0));
    il.o[2].cell = 3;                                          /* 0xAEFA $1C12B0 */
    if (mode3)
        il.o[1].cell = 0xFFFFu;                                /* 0xAEBA $1C1286 hidden */
}

/* ================= 0xAE20 mode 0: the LOD talk screen ================ */

static void talk_begin(eng_state *st)
{
    sub_clear(st);
    st->cam_x = TALK_SCROLL_X;                                 /* 0xAE46 */
    st->cam_y = TALK_SCROLL_Y;
    eng_scene_publish(ARENA_PAL_IL, 0);                        /* 0xAE6C/0xAE80 */
    panels_init(0);
    if (il.talk_mode == 0) {                                   /* 0xAF56/0xAF88: mode 0 only */
        eng_aisle_bottom_bar();                                /* 0xAF5E bsr 0xB29E */
        eng_blit(BLIT_CHAMP);                                  /* 0xAF62 */
        eng_blit(BLIT_LOD);                                    /* 0xAF6C */
        eng_sound(TALK_MUSIC0);                                /* 0xAF90 */
        eng_sound(TALK_MUSIC1);                                /* 0xAF9A */
        il.o[1].on = 2;                                        /* 0xAFB8 $1C129F bit0: centre talks first
                                                                  (on == 2: script armed; panels stay drawn) */
    } else if (il.talk_mode == 1) {                            /* 0xAFC2 */
        il.voice = 1;                                          /* $1C1276 = 2 (word index 1) */
        il.o[2].on = 2;                                        /* bset #0,$1C12C9: right panel speaks */
    } else {                                                   /* 0xAFD4 (mode 2) */
        il.voice = 2;                                          /* $1C1276 = 4 (word index 2) */
        il.o[0].on = 2;                                        /* bset #0,$1C1275: left panel speaks */
    }
    if (dbg()) fprintf(stderr, "interlude: talk begin f%lld mode %d\n",
                       (long long)st->frame, il.talk_mode);
}

/* 0xB0C8: one panel through its {cell, duration} script. */
static void talk_anim(int k)
{
    il_obj *o = &il.o[k];
    unsigned cell, dur;
    uint32_t script;
    if (o->on != 2) return;                                    /* btst #0,(0x1D,A0) */
    if (--o->cnt >= 0) return;                                 /* subi #1; bpl */
    script = tbl32(TBL(interlude_talk_script_ptrs), (unsigned)k * 4u);
    cell = tbl_ra8(script + o->step * 2u);                     /* 0xB0F0 */
    if (cell & 0x80u) {                                        /* 0xB0F6: speech */
        if (il.voice < 4)
            eng_sound(tbl16(TBL(interlude_voice_cmds), (unsigned)il.voice * 2u));  /* 0xB102..0xB11A */
        il.voice++;
    }
    o->cell = cell & 0x7Fu;                                    /* 0xB124 */
    dur = tbl_ra8(script + o->step * 2u + 1u);                 /* 0xB12A */
    o->cnt = (int)dur;
    o->step++;                                                 /* 0xB136 */
    if (dur == 0xFEu) o->on = 1;                               /* 0xB13C: script over, stay drawn */
}

static int talk_update(eng_state *st)
{
    (void)st;
    for (int k = 0; k < 3; k++) talk_anim(k);
    if (il.talk_mode == 1) {                                   /* 0xAFF6 -> 0xB034 */
        if (il.o[2].step == 0x1C) il.o[2].step = 0x1E;         /* 0xB03E skip two entries */
        return il.o[2].step >= 0x1F;                           /* 0xB0A0 end gate */
    }
    if (il.talk_mode == 2)                                     /* 0xB000 -> 0xB094 */
        return il.o[0].on == 1;                                /* tst.w $1C1274: left script done
                                                                  (0xB144 clr on dur 0xFE) */
    if (!il.fired_b2 && il.o[1].step == TALK_B2_AT) {          /* 0xB00A */
        il.fired_b2 = 1;
        il.o[2].on = 2;                                        /* $1C12C9 */
        talk_text_wipe();                                      /* bsr 0xB2E4 */
    }
    if (!il.fired_b0 && il.o[1].step >= TALK_B0_AT) {          /* 0xB020 */
        il.fired_b0 = 1;
        il.o[0].on = 2;                                        /* $1C1275 */
    }
    if (il.o[0].step >= TALK_END_AT)                           /* 0xB086 */
        return 1;                                              /* 0xB0AC jsr 0x264E2 (fade TODO EXACT) */
    return 0;
}

/* ================= 0xBDA6: the belt / matches-left scene ============= */

static void belt_begin(eng_state *st)
{
    unsigned base = (unsigned)il.belt_idx * BELT_CELL_STEP + BELT_CELL_BASE;  /* 0xBDFA */
    sub_clear(st);
    st->cam_x = TALK_SCROLL_X;                                 /* via 0xAE20 (D0=3) */
    st->cam_y = TALK_SCROLL_Y;
    eng_scene_publish(ARENA_PAL_IL, 1);                        /* 0xBE84 $1C15FC = 1 */
    panels_init(1);
    il.o[0].x = BELT_PANEL0_X;                                 /* 0xBDBC */
    il.o[2].x = BELT_PANEL2_X;                                 /* 0xBDC4 */
    /* 0xBE10 loop, table 0xBF58 read column-wise: obj4 herald column 0,
     * obj3 count art column 1. Engine slots: o[3] = count art, o[4] = herald. */
    il.o[3].on   = 1;                                          /* 0xBE1E bset #7 */
    il.o[3].row  = BELT_ROW;
    il.o[3].x    = (int)tbl8(TBL(interlude_belt_objs), 1u);    /* (A1,D0) D0=1: 0x60 */
    il.o[3].y    = (int)tbl8(TBL(interlude_belt_objs), 3u);    /* (2,A1,D0): 0x36 */
    il.o[3].cell = base;                                       /* 0xBE02 $1C12DA */
    il.o[4].on   = 1;
    il.o[4].row  = BELT_ROW;
    il.o[4].x    = (int)tbl8(TBL(interlude_belt_objs), 0u);    /* (A1,D0) D0=0: 0x49 */
    il.o[4].y    = (int)tbl8(TBL(interlude_belt_objs), 2u);    /* (2,A1,D0): 0x58 */
    il.o[4].cell = BELT_HERALD_CELL;                           /* 0xBE08 */
    il.o[4].cnt  = BELT_HERALD_WAIT;                           /* 0xBEEC $1C1322 */
    il.o[4].step = 0;
    il.o[4].done = 0;
    il.o[4].on   = 2;                                          /* 0xBEF4 bset #7 $1C131C: animating */
    /* 0xBEB8: the message text at $C1540; 0xBECA: the extra line at $C1578
     * only on the last defence before the title match (cell base 0x12). */
    eng_banner_runs(0xC1540u, BELT_RUN_MAIN, 0x10);
    if (base == 0x12u)
        eng_banner_runs(0xC1578u, BELT_RUN_LAST, 0x10);
    if (dbg()) fprintf(stderr, "interlude: belt begin f%lld idx %d cell base %02X\n",
                       (long long)st->frame, il.belt_idx, base);
}

static int belt_update(eng_state *st)
{
    (void)st;
    /* 0xBF64: the herald steps its {3,1,2,1,0} cells every 10 ticks. */
    if (il.o[4].on == 2) {
        if (--il.o[4].cnt < 0) {                               /* 0xBF6E tst / 0xBFF6 subi */
            il.o[4].cnt  = BELT_HERALD_TICK - 1;               /* 0xBF76 (the shared subi eats one) */
            il.o[4].cell = tbl8(TBL(interlude_belt_smallcells), il.o[4].step) & 0xFu;  /* 0xBF86 */
            il.o[4].step++;                                    /* 0xBF92 */
            if (il.o[4].step >= 5) il.o[4].on = 1;             /* 0xBF98 clr +0x1C */
            if (il.o[4].cell == 2) {                           /* 0xBFA4 */
                il.o[3].on = 2;                                /* bset #7 $1C12F2 */
                eng_sound(BELT_VOICE);                         /* 0xBFB8 */
            }
        }
    }
    /* 0xBFCE: the count art runs its 7 cells every 8 ticks, then holds
     * while the step counter runs on to 0x10. */
    if (il.o[3].on == 2) {
        if (--il.o[3].cnt < 0) {
            il.o[3].cnt = BELT_ART_TICK - 1;                   /* 0xBFDC */
            il.o[3].step++;                                    /* 0xBFE2 */
            if (il.o[3].step < BELT_ART_STEPS) il.o[3].cell++; /* 0xBFE8/0xBFF0 */
        }
        if (il.o[3].step >= BELT_END_AT)                       /* 0xBF3E $1C12FA */
            return 1;                                          /* 0xBF48 jsr 0x264E2 (fade TODO EXACT) */
    }
    return 0;
}

/* 0xBE92: the message text palette — FG0 line at 0x188280 becomes a ramp
 * (word k = k << 8).  Redone every frame: the engine reloads the text set
 * on a publish. */
static void belt_text_ramp(void)
{
    for (unsigned k = 0; k < 16; k++)
        m68k_write_memory_16(0x188280u + k * 2u, k << 8);
}

/* ================= 0xB608: the title-win card ======================== */

static void title_begin(eng_state *st)
{
    unsigned n = 0;
    sub_clear(st);
    st->cam_x = TITLE_SCROLL_X;                                /* 0xB636 */
    st->cam_y = TITLE_SCROLL_Y;
    eng_scene_publish(ARENA_PAL_IL, 0);                        /* 0xB65C/0xB670 */
    /* 0xB68A loop: a portrait per live slot; only the first two are ever
     * shown (0xB8F0 sets $1C1134/$1C1164 alone), i.e. the human pair. */
    il.cname[0] = il.cname[1] = NULL;
    for (int i = 0; i < 2 && i < ENG_MAX_OBJS; i++) {
        unsigned id;
        if (!st->obj[i].active) continue;
        id = (unsigned)st->obj[i].wrestler & 0xFFu;
        if (id >= 12u) {                                       /* CLONE: no bespoke card art —
                                                                  portrait absent (pak hook later),
                                                                  name = standard big font at the
                                                                  letters' step (title_update) */
            const char *cn = eng_ws_clone_name((int)id);
            int trow = eng_sprite_title_row((int)id);
            if (!cn || !cn[0]) continue;
            il.cname[n]       = cn;
            il.portrait_id[n] = 0xFFu;                         /* name cells: blank */
            if (trow >= 0) {                                   /* his own card art (pak 810) */
                il.o[n].on   = 0;
                il.o[n].row  = (unsigned)trow;
                il.o[n].cell = 0;
                il.o[n].x    = (int)tbl8(TBL(interlude_title_cards), (unsigned)eng_ws_base((int)id) * 3u + n);
                il.o[n].y    = PORTRAIT_Y;
            } else
                il.o[n].cell = 0xFFFFu;                        /* no portrait sprite */
            n++;
            continue;
        }
        if (id == 0xBu) id = 0xAu;                             /* 0xB6A0: Smash's card for both Demolition */
        if (id > 0xAu) continue;
        il.portrait_id[n] = id;
        il.o[n].on   = 0;                                      /* 0xB69C clr: shown at centre step 1 */
        il.o[n].row  = PORTRAIT_ROW;                           /* 0xB6D4 */
        il.o[n].cell = id;                                     /* 0xB6B6 +0x04 */
        il.o[n].x    = (int)tbl8(TBL(interlude_title_cards), id * 3u + n);   /* 0xB6BE..0xB6CA */
        il.o[n].y    = PORTRAIT_Y;                             /* 0xB6CE */
        n++;
    }
    if (n == 1) il.portrait_id[1] = il.portrait_id[0];
    if (n == 0) { il.portrait_id[0] = il.portrait_id[1] = 0; }
    /* 0xB708: the centre object. */
    il.o[2].on   = 2;                                          /* bset #7 + $1C131C-style anim */
    il.o[2].row  = TITLE_BIG_ROW;
    il.o[2].x    = TITLE_BIG_X;
    il.o[2].y    = TITLE_BIG_Y;
    il.o[2].cnt  = TITLE_BIG_WAIT;                             /* 0xB726 +0x22 */
    il.o[2].cell = 0;                                          /* 0xB730 clr +0x04 (palette 0x37 via stream) */
    il.o[2].step = 0;
    /* 0xB94C: the 17-letter marquee, all parked at x 0x1BF, hidden. */
    for (int i = 0; i < LETTER_N; i++) {
        il_obj *o = &il.o[3 + i];
        o->on  = 0;
        o->row = LETTER_ROW;                                   /* 0xB95E */
        o->x   = LETTER_X0;                                    /* 0xB96A (0x1BF via 0xBAA0 re-park) */
        o->y   = LETTER_Y;                                     /* 0xB964 */
        if (i < 6)            o->cell = 0xEu + (unsigned)i;    /* 0xB97E */
        else if (i < 9)       o->cell = 0x19u + (unsigned)i;   /* 0xB988: 0x1F..0x21 */
        else if (i == 0xC)    o->cell = 0x22u;                 /* 0xB9A8 */
        else if (i == 0x10)   o->cell = 0x23u;                 /* 0xB9B4 */
        else {                                                 /* 0xB9C0: the two names */
            unsigned pid = il.portrait_id[i < 0xD ? 0 : 1];
            unsigned col = (unsigned)(i < 0xD ? i - 9 : i - 0xD);
            unsigned c   = pid == 0xFFu ? 0u
                         : tbl8(TBL(interlude_name_cells), pid * 3u + col);  /* 0xB9E2..0xB9F2 */
            o->y    = LETTER_NAME_Y;                           /* 0xB9C0 +0x16 = 0xF */
            o->cell = c ? c : 0xFFFFu;                         /* 0xB9FA/0xB9FE */
        }
    }
    eng_sound(TITLE_MUSIC0);                                   /* 0xB778 */
    eng_sound(TITLE_MUSIC1);                                   /* 0xB782 */
    if (dbg()) fprintf(stderr, "interlude: title card begin f%lld ids %u/%u\n",
                       (long long)st->frame, il.portrait_id[0], il.portrait_id[1]);
}

/* 0xBA8E / 0xBB02: the per-cell x offset (cell 0xFFFF -> -0x80). */
static int letter_off(unsigned cell)
{
    if (cell == 0xFFFFu) return -0x80;                         /* 0xBA82/0xBAF6 */
    if (cell < 0xEu || cell > 0x24u) return 0;
    return tbl_s16(TBL(interlude_letter_widths), (cell - 0xEu) * 2u);
}

static int title_update(eng_state *st)
{
    (void)st;
    /* 0xB8A4 (run for the centre object only): cell script, 8 ticks. */
    {
        il_obj *o = &il.o[2];
        if (o->cnt > 0) o->cnt--;                              /* 0xB92E subi */
        else {
            unsigned c = tbl8(TBL(interlude_title_bigcells), o->step);   /* 0xB8BE */
            if (c == 0xFFu) {                                  /* 0xB918..0xB92A */
                o->step = TITLE_BIG_WRAP;
                c = tbl8(TBL(interlude_title_bigcells), o->step);
            }
            o->cell = c;
            o->cnt  = TITLE_BIG_TICK;                          /* 0xB8CA */
            o->step++;                                         /* 0xB8D0 */
            if (!il.fired_show) {                              /* 0xB8D6 btst $1C127A gate */
                if (o->step == 1) {                            /* 0xB8E0 */
                    il.prio = TITLE_PRIO_ON;                   /* 0xB8E8 $140010 = 0x78 */
                    il.o[0].on = il.o[0].cell != 0xFFFFu;      /* 0xB8F0/0xB8F8 portraits in
                                                                  (clone slots have none) */
                    il.o[1].on = il.o[1].cell != 0xFFFFu;
                }
                if (o->step == 5) {                            /* 0xB900 */
                    il.o[3].on = 1;                            /* 0xB908 bset $1C1258: first letter */
                    il.fired_show = 1;                         /* 0xB910 bset $1C127A */
                    for (int k = 0; k < 2; k++)                /* clone names: the standard
                                                                  big font, aisle-style rows */
                        if (il.cname[k] && il.cname[k][0])
                            eng_fg0_bigtext(k ? 0x1Bu : 0x18u, 2, il.cname[k], 0);
                }
            }
        }
    }
    /* 0xBA48 per live letter: slide left, release the next, die off-screen. */
    for (int i = 0; i < LETTER_N; i++) {
        il_obj *o = &il.o[3 + i];
        if (!o->on || o->done) continue;                       /* btst #6/#7 (+0x00) */
        o->x -= LETTER_SPEED;                                  /* 0xBA52 */
        if (i != LETTER_N - 1) {                               /* 0xBA58 cmpi #0x10 */
            il_obj *nx = &il.o[3 + i + 1];
            if (!nx->on && !nx->done
                && o->x + letter_off(o->cell) < LETTER_GATE_X) {   /* 0xBA74..0xBA98 */
                nx->on = 1;                                    /* 0xBA9A */
                nx->x  = LETTER_X0;                            /* 0xBAA0 */
            }
        }
        if (o->x + letter_off(o->cell) < 0) {                  /* 0xBADA..0xBB0A */
            o->done = 1;                                       /* 0xBB0C +0x00 = 0x4000 */
            o->on   = 0;
        }
    }
    if (il.o[3 + LETTER_N - 1].done)                           /* 0xB7FA btst #6 $1C14F8 */
        return 1;                                              /* 0xB804 jsr 0x264E2 (fade TODO EXACT) */
    return 0;
}

/* ---- IL_BRACKET: the 1v1 TOURNAMENT card (engine page, no ROM analog;
 * shown when mode team_size == 1). Black page, the ASCII FG0 font
 * (code = char - 0x10, the CREDIT-stamp face), the small ranking name
 * runs (0xA5BE tables) for the road so far. START or ~5s moves on. */
static unsigned br_t;

static void btext(unsigned row, unsigned col, const char *txt, unsigned pal)
{
    unsigned a = row * 0x100u + col * 4u;
    for (; *txt && a + 4 <= WF_FG0RAM_SIZE; txt++, a += 4) {
        unsigned ch = (unsigned char)*txt;
        if (ch == ' ') { wf.fg0_videoram[a + 1] = 0; wf.fg0_videoram[a + 3] = 0; continue; }
        wf.fg0_videoram[a + 1] = (uint8_t)(ch - 0x10u);
        wf.fg0_videoram[a + 3] = (uint8_t)((pal << 4) | (((ch - 0x10u) >> 8) & 0xFu));
    }
}
static void br_name(unsigned row, unsigned col, int id)
{
    unsigned w = tbl16(TBL(rank_small_names_tag), ((unsigned)id % 12u) * 2u);
    unsigned n = ((unsigned)id >= 0xAu) ? 0xAu : 4u;   /* 0xA5C8/0xA5D0 */
    unsigned a = row * 0x100u + col * 4u;
    for (unsigned i = 0; i <= n && a + 4 <= WF_FG0RAM_SIZE; i++, a += 4) {
        wf.fg0_videoram[a + 1] = (uint8_t)(w + i);
        wf.fg0_videoram[a + 3] = (uint8_t)(((w + i) >> 8) & 0x0Fu);
    }
}
static void bracket_begin(eng_state *st)
{
    int stage = 0, next = -1, faced[10], n;
    (void)st;
    { extern int eng_scene_blank; eng_scene_blank = 1; }   /* black planes */
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);
    for (int k = 0; k < 3 + LETTER_N && k < (int)(sizeof il.o / sizeof il.o[0]); k++)
        il.o[k].on = 0;                    /* no sprites on this page */
    n = eng_camp_bracket(&stage, faced, &next);
    btext(4, 10, "TOURNAMENT ROAD", 0xE);
    {
        char line[32];
        snprintf(line, sizeof line, "ROUND %d", stage + 1);
        btext(6, 13, line, 0xE);
    }
    if (n > 0) {
        btext(9, 4, "DEFEATED", 0xE);
        for (int i = 0; i < n && i < 8; i++)
            br_name((unsigned)(11 + i * 2), 6, faced[i]);
    }
    if (next >= 0) {
        btext(9, 22, "NEXT", 0xE);
        br_name(11, 24, next);
    }
    {
        char line[32];
        int left = 10 - stage;
        snprintf(line, sizeof line, "%d WIN%s TO THE TITLE", left, left == 1 ? "" : "S");
        btext(27, 10, line, 0xE);
    }
    br_t = 0x140;
    il.prio = 0x7C;
}
static int bracket_update(eng_state *st)
{
    if (eng_start_scan(st->inputs)) return 1;
    for (int p = 0; p < 4; p++) if (st->inputs[p] & 0x30u) return 1;
    return br_t && --br_t == 0;
}

/* ================= the scene ========================================= */

static void il_begin_sub(eng_state *st)
{
    il.cur = il.queue[il.qi];
    switch (il.cur) {
    case IL_TITLE: title_begin(st); break;
    case IL_BELT:  belt_begin(st);  break;
    case IL_BRACKET: bracket_begin(st); break;
    default:       talk_begin(st);  break;
    }
}

static void il_begin(eng_state *st)
{
    il.qi = 0;
    if (il.qn == 0) il.queue[il.qn++] = IL_TALK;   /* engine guard: never armed empty */
    il_begin_sub(st);
}

static int il_update(eng_state *st)
{
    int over;
    /* Attract taunt (0xB1C): IRQ3 stays live ($1C0076 bit7 from 0x92A),
     * so START + credit leaves for the game select (0x978 -> 0x52BE). */
    if (il.demo_return) {
        eng_credit_line();
        if (eng_start_scan(st->inputs)) {
            void eng_attract_reset_cycle(void);
            il.qn = 0; il.demo_return = 0; il.talk_mode = 0;
            eng_sprite_scene_pals_end();
            eng_attract_reset_cycle();
            if (dbg()) fprintf(stderr, "interlude: START f%lld -> game select\n",
                               (long long)st->frame);
            return ENG_SCENE_GAMESELECT;
        }
    }
    switch (il.cur) {
    case IL_TITLE: over = title_update(st); break;
    case IL_BELT:  over = belt_update(st);  break;
    case IL_BRACKET: over = bracket_update(st); break;
    default:       over = talk_update(st);  break;
    }
    if (!over) return -1;
    if (dbg()) fprintf(stderr, "interlude: screen %d over f%lld\n", il.cur, (long long)st->frame);
    if (++il.qi < il.qn) { il_begin_sub(st); return -1; }
    il.qn = 0;
    eng_sprite_scene_pals_end();
    if (il.demo_return) {          /* attract taunt (0xB1C): back to the driver
                                      for the 0xB46 fade + 0x69C8 ranking page */
        il.demo_return = 0;
        il.talk_mode = 0;
        return ENG_SCENE_ATTRACT;
    }
    return ENG_SCENE_MATCH;        /* scene.c routes any front scene -> 0x7B70 aisle */
}

static void il_draw(const eng_state *st)
{
    unsigned slot = 0;
    int n = (il.cur == IL_TITLE) ? 3 + LETTER_N : (il.cur == IL_BELT) ? 5
          : (il.cur == IL_BRACKET) ? 0 : 3;
    (void)st;
    m68k_write_memory_16(0x140010u, il.prio);                  /* 0xAE64 / 0xB654 / 0xB8E8 (after the compose) */
    eng_sprite_scene_pals_rearm();     /* the boot-time body-palette flush
                                          (pal_load.c) may land after begin()
                                          and drop the 0x2AEA map */
    if (il.cur == IL_BELT) belt_text_ramp();                   /* 0xBE92 */
    memset(wf.spriteram, 0, WF_SPRRAM_SIZE);
    for (int k = 0; k < n; k++) {                              /* the 0x27B8 compose loops */
        const il_obj *o = &il.o[k];
        if (!o->on || o->done || (o->cell & 0x7FFFu) == 0x7FFFu || o->cell == 0xFFFFu)
            continue;
        eng_sprite_emit_pose(o->row, o->cell, o->x, o->y, -1, &slot);
    }
    memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);
}

static const eng_scene_ops il_ops = { il_begin, il_update, il_draw };

void eng_interlude_register(void)
{
    eng_scene_register(ENG_SCENE_INTERLUDE, &il_ops);
}
