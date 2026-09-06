/* SELECT PLAYERS screen — transcription of ROM 0x58B2..0x5EA8 and its
 * helpers 0x5EAE (flash), 0x5F28 (cursor spawn), 0x600E (re-seat after a
 * page scroll), 0x60A2 (join), 0x64E0 (input / auto-pilot), 0x658C (page
 * scroll), 0x57E0 (seat), 0x10370..0x104A4 (CPU team fill).
 *
 * Spec: docs/engine-specs/scene-charselect.md.
 *
 * The screen is scene-word 3 composed at (0x140, 0x300) for tag (0x58EE)
 * or (0x140, 0x500) for rumble (0x5934): two rows of five portraits with
 * their names in the tilemap, "SELECT PLAYERS" above, CREDIT on fg0. Only
 * three columns fit the window, so the page scrolls between x 0x140
 * (columns 0-2) and 0x210 (columns 2-4) whenever a cursor lands on the
 * middle column from the side (0x5C16..0x5CC8, 0x658C).
 *
 * Each seated player is an object at $1C05B0 + slot*0x10C carrying sprite
 * row 0xD (the "nP" frame, pose = player index, 0x6484) at the world
 * position of its grid cell (0x64A4 by position). The ROM fields used:
 *   +0x22/+0x23 position (word/byte)   +0x25 previous position
 *   +0x33 b0 picked   +0xA1 b0 seated (human port)   +0x8A port pointer
 *   +0xA8 joystick nibble   +0x91 last nibble   +0xA6/+0xA2/+0xA3 buttons
 *   +0x56 auto-pilot step counter   +0x5E auto-pilot press timer
 *   +0x58 flash frame   +0x5C flash colour index   +0xAE player colour
 * Globals: $1C015C timeout, $1C015E flags (b0 scroll request, b1 right
 * page, b3 scrolling, b4 auto-pilot, b5/b6 2P-vs sub-select), $1C11F4
 * cursor occupancy by position, $1C1214 picked by position, $1C0598 the
 * roster table (10 words, 0xFFFF empty), $1C17E6 camera x.
 *
 * Picks -> match: 0x5DAC writes roster[i] = 0x64CC[pos] for each active
 * slot; 0x10400 fills the first two empty roster words with the CPU team.
 * eng_cs_picks() hands that table to eng_init_picks().
 *
 * Every constant is read out of tbl_ra8() or cites its ROM PC. The
 * 2P-vs sub-select ($1C0066 & 0x800, 0x610A..0x62A2) and the credit test
 * on join (0x55A) are not modelled: TODO EXACT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include "credit.h"
#include "scene.h"
#include "profile.h"

void eng_scene_publish(unsigned arena, unsigned textset);   /* scene.c */

static unsigned r8(uint32_t a)  { return tbl_ra8(a); }
static unsigned r16(uint32_t a) { return ((unsigned)tbl_ra8(a) << 8) | tbl_ra8(a + 1); }
static uint32_t r32(uint32_t a) { return ((uint32_t)r16(a) << 16) | r16(a + 2); }

/* Tables this file owns (docs/adr-001-data-formats.md). Positions are the
 * 10 grid cells 0..9 (two rows of five); pages: left = columns 0-2,
 * right = columns 2-4. The 0x62BC.. move tables are [pos*11 + joystick
 * nibble] -> new pos: the left-page one has 8 rows in the ROM (positions
 * 8/9 are never on the left page), the right-page ones 10. */
static const tbl_def cs_tables[] = {
    { "cs_flash_colours",        "base/front", 0x5F0A, 5 * 2,  TK_U16, 1,
      "0x5ED6 picked-cursor flash: five palette words cycled into the pos's fg bank pen (0x5EF2)" },
    { "cs_pos_palette_bank_tag", "base/front", 0x5F14, 10, TK_U8, 1,
      "0x5EBA tag select: fg palette bank (x0x80) of each grid pos for the flash pen" },
    { "cs_pos_palette_bank_rumble","base/front", 0x5F1E, 10, TK_U8, 1,
      "0x5ECA rumble select: fg palette bank of each grid pos; code 0x5F28 follows" },
    { "cs_slot_index",           "base/front", 0x62A6, 4, TK_U8, 1,
      "0x5AD2 join: dbra counter -> engine slot index (3 2 1 0)" },
    { "cs_free_seats_left",      "base/front", 0x62AA, 6, TK_U8, 1,
      "0x5FD2/0x6018 free-position search order, left page" },
    { "cs_free_seats_right",     "base/front", 0x62B0, 6, TK_U8, 1,
      "0x5FE2/0x602E free-position search order, right page (rumble re-seat, rumble spawn)" },
    { "cs_free_seats_right_tag", "base/front", 0x62B6, 6, TK_U8, 1,
      "0x603E free-position search order, right page tag re-seat" },
    { "cs_move_left",            "base/front", 0x62BC, 8 * 11, TK_U8, 11,
      "0x5B3E cursor move: [pos*11 + nibble] -> pos, left page (8 rows)" },
    { "cs_move_right",           "base/front", 0x6314, 10 * 11, TK_U8, 11,
      "0x5B4E cursor move: [pos*11 + nibble] -> pos, right page" },
    { "cs_move_right_rumble",    "base/front", 0x6382, 10 * 11, TK_U8, 11,
      "0x5B5E cursor move: [pos*11 + nibble] -> pos, right page rumble" },
    { "cs_alt_seat_ptrs",        "base/front", 0x63F0, 10 * 4, TK_U32, 1,
      "0x5B94 occupied target: long -> by previous pos the alternative-pos list indexed by the wanted pos" },
    { "cs_alt_seat_records",     "base/front", 0x6418, 0x646C - 0x6418, TK_U8, 0,
      "0x5BA6 alternative-pos lists (6..10 bytes each, see cs_alt_seat_ptrs)" },
    { "cs_seat_xy",              "base/front", 0x646C, 4 * 4, TK_U16, 2,
      "0x57EC seat: per slot the cursor world (x, y)" },
    { "cs_seat_pos",             "base/front", 0x647C, 4 * 2, TK_U16, 1,
      "0x57F2 seat: per slot the start grid pos" },
    { "cs_cursor_sprite",        "base/front", 0x6484, 4 * 4, TK_U16, 2,
      "0x57F8/0x5F34 seat: per slot {nP pose word, colour word}" },
    { "cs_cursor_sprite_versus", "base/front", 0x6494, 4 * 4, TK_U16, 2,
      "0x584C/0x5F5E seat in the 2P-vs sub-select: per slot {pose word, colour word} (TODO EXACT)" },
    { "cs_pos_xy",               "base/front", 0x64A4, 10 * 4, TK_U16, 2,
      "0x5CD0 grid pos -> cursor world (x, y)" },
    { "cs_pos_wrestler",         "base/front", 0x64CC, 10 * 2, TK_U16, 1,
      "0x5DAC grid pos -> wrestler id (ADR rule 7: each wrestler's grid slot is the inverse of this at slicing time); code 0x64E0 follows" },
    { "cs_auto_dir",             "base/front", 0x6584, 8, TK_U8, 1,
      "0x6566 auto-pilot: rng&7 -> joystick nibble; code 0x658C follows" },
    { "cs_cpu_fill_weight_ptrs", "base/front", 0x106E0, 5 * 4, TK_U32, 1,
      "0x103CA CPU team fill: long -> 0x24CC weight list by stage ($1C0162, -5 when >= 5)" },
    { "cs_cpu_fill_weights",     "base/front", 0x106F4, 0x10708 - 0x106F4, TK_U8, 0,
      "0x24CC weight lists for the CPU team draw (8, 6, 4, 2 bytes); 0x10708 spawn positions follow (core.c)" },
};
TBL_REGISTER(cs_tables)

/* ROM tables (all indexed at runtime; the tables above). */
#define TAB_CURSOR      0x6484u  /* slot -> (pose word, colour word) */
#define TAB_CURSOR_VS   0x6494u  /* same, 2P-vs sub-select (TODO EXACT) */
#define TAB_SEAT_L      0x62AAu  /* 0x5FD2/0x6018: free-position search, left page */
#define TAB_SEAT_R      0x62B0u  /* right page (rumble re-seat, rumble spawn) */
#define TAB_SEAT_RT     0x62B6u  /* right page, tag re-seat */
#define TAB_MOVE_L      0x62BCu  /* 0x5B3E: [pos*11 + nibble] -> pos, left page */
#define TAB_MOVE_R      0x6314u  /* right page */
#define TAB_MOVE_RR     0x6382u  /* right page, rumble */
#define TAB_ALT         0x63F0u  /* 0x5B94: prev -> 10-byte table, occupied -> alt */
#define PAL_FG          0x188000u /* 0x5EAE: fg tile palette bank 0 */
#define CURSOR_ROW      0xDu     /* 0x585A / 0x5F2E: sprite row of the nP frame */
#define CURSOR_Z        0x140u   /* 0x5878 / 0x5F72 */
#define FLASH_LEN       0x4Cu    /* 0x5D6A */
#define TIMEOUT         0x400u   /* 0x59B8 */
#define AUTO_PRESS      0x40u    /* 0x6538 */
#define AUTO_REARM      0x20u    /* 0x6576 */
#define SCROLL_L        0x140u   /* 0x659E */
#define SCROLL_R        0x210u   /* 0x658E */
#define SCROLL_STEP     8        /* 0x658C */
#define ENTRY_WAIT      0x10     /* 0x5978: vblanks before the loop */
#define EXIT_WAIT       0x20     /* 0x5EA0: vblanks after the picks */
#define SCENE_WORD      3        /* 0x58FE */
#define ARENA_PAL       3        /* 0x5912 $1C15F4 */
#define TEXT_PAL_SET    1        /* inherited from 0x52BE */
#define ROSTER_N        10       /* 0x5980 moveq #9 */

/* $1C015E */
#define F_SCROLL_REQ  0x01
#define F_PAGE_R      0x02
#define F_SCROLLING   0x08
#define F_AUTO        0x10
#define F_VS_A        0x20
#define F_VS_B        0x40

typedef struct {
    int active;            /* +0x00 b7 */
    int seated;            /* +0xA1 b0: a human port drives it */
    int picked;            /* +0x33 b0 */
    int port;              /* +0x8A: player index feeding it */
    unsigned pos, prev;    /* +0x22/+0x23, +0x25 */
    int x, y;              /* +0x06, +0x0A */
    int sx, sy;            /* +0x14, +0x16 (0x247C) */
    unsigned pose;         /* +0x04 */
    unsigned colour;       /* +0xAE */
    unsigned flash_t;      /* +0x58 */
    unsigned flash_i;      /* +0x5C */
    unsigned auto_c;       /* +0x56 */
    unsigned auto_t;       /* +0x5E */
    unsigned dir;          /* +0xA8 word (nibble in +0xA9) */
    unsigned last_dir;     /* +0x91 */
    unsigned held;         /* +0xA6 */
    unsigned newp;         /* +0xA2 (low byte +0xA3 = press) */
    unsigned pen_addr, pen_val;   /* last 0x5EAE write, re-applied per frame */
} cs_slot;

static struct {
    cs_slot slot[4];
    int rumble;            /* $1C0161 b0 */
    unsigned timeout;      /* $1C015C */
    unsigned flags;        /* $1C015E */
    unsigned occ[ENG_WS_EXT_MAX];   /* $1C11F4 (12+ = the clone columns) */
    unsigned pick[ENG_WS_EXT_MAX];  /* $1C1214 */
    unsigned roster[ROSTER_N];   /* $1C0598 */
    int cam_x;             /* $1C17E6 */
    int lead;              /* engine-only (extended grid): the cursor that moved last owns the camera */
    int phase;             /* 0 entry wait, 1 loop, 2 exit wait, 3 done */
    int wait;
    long frames;           /* $1C0080 */
    int have_picks;
    int picks[4];
    int ports[4];          /* the PORT that picked each roster slot, -1 = CPU/autopilot
                              (user 2026-08-28 buy-in spec: slot 0/1 = left team,
                              2/3 = right team; a seat is a set of controls) */
} cs;

static int dbg(void) { static int v = -1; if (v < 0) v = getenv("WF_DBGSEL") != 0; return v; }

/* ---- select_extended (mode_rules row `select_extended`, stock 0) ----
 *
 * With the mode on the grid grows to 2x6: a sixth column at the ROM grid's
 * own pitch (0x64A4 cs_pos_xy: x pitch 0x60, portrait cells 0x160 + col*0x60
 * world) holds the LEGION OF DOOM — pos 10 = Hawk (id 4), pos 11 = Animal
 * (id 5). The right page's rest camera moves from 0x210 to 0x260 so it shows
 * columns 3-5 the way the left page shows 0-2; the composer re-runs per
 * scroll window (render.c compose()), and past world 0x350 the scene records
 * emit plain backdrop, so the page is clean (verified by shot). The two new
 * cells are SPRITE overlays: the verified 80x80 select portraits
 * (data/select/04.png / 05.png, pixel-identical provenance — see the commit)
 * are cut into 25 injected sprite tiles each (wf_video_inject_sprite_tiles)
 * and drawn under the cursor records each frame; their 15 colours live in
 * sprite palette banks 14/15, which the 0x2AEA on-demand allocator (deals
 * from bank 0 up) never reaches on this screen. Pen 15 is the panel pen, so
 * the stock 0x5EAE pick-flash works on the new cells by cycling that pen.
 * B2 on any unpicked cell cycles it to a registered clone of its wrestler
 * (package.c eng_ws_clone_base), shown as an FG0 name tag over the cell.
 * Everything in this block is dead when the mode row is 0. */
#define POS_HAWK      10u
#define POS_ANIMAL    11u
/* PEEK stops (user 2026-08-25: stock pagination never left you guessing —
 * reaching the page's last column slides the window so the next column
 * shows). Engine-only rest x's between the ROM stops; applied only while
 * every seated cursor fits the biased window (two cursors can pin the
 * camera to the plain stop). */
#define LOD_TILE0     0xFE00u  /* injected sprite tiles, 25 per cell (5x5 of 16x16) */
#define LOD_BANK0     14u      /* sprite palette banks 14/15 */
#define DEMO_TILE0    0xFE40u  /* Smash/Crush INDIVIDUAL cells (pos 8/9, ext grid) —
                                  after the swatch tiles (0xFE32..0xFE3F) */
#define DEMO_BANK0    7u       /* banks 7/8: the clone rotation deals them to cols 8/9,
                                  which the <=4-column window never shows beside cols 3/4 */
#define PAL_SPR       0x182000u
#define LOD_CELL_X    0x340    /* world x of the col-5 portrait cell (0x160 + 5*0x60) */
static int ext_on(void)
{   /* the extended grid: the mode rule, or simply a registered clone (the
     * shared roster/ makes every non-stock profile carry them, 2026-08-27) */
    if (eng_mode_rule(MODE_SELECT_EXT) != 0) return 1;
    if (wf_profile()[0]) return 1;     /* every non-stock profile (user 2026-08-27) */
    for (int id = 12; id < ENG_WS_EXT_MAX; id++) if (eng_ws_clone_base(id) >= 0 && !eng_ws_hidden(id)) return 1;
    return 0;
}
/* seat priority orders for the extended pages (stock lists cover cols 0-4) */
static struct {
    int loaded;                /* 0 not tried, 1 ok, -1 failed (cells stay bare) */
    uint16_t pal[2][16];       /* xBGR444; pen 15 = panel (black), pen 0 unused */
    unsigned alt_cell[12];     /* B2 toggle: cell resolves to the clone of its base */
} lod;
static struct { int loaded; uint16_t pal[2][16]; } demo;   /* data/select/10/11.png: Smash & Crush
                                  as their OWN cells — the extended grid separates Demolition
                                  the way rumble picks do (user 2026-08-27) */

/* clone select portraits: a mod supplies select/<cloneid>.png (80x80, the
 * LOD cell format); shown over the base cell while its B2 toggle is on.
 * Tiles injected after the LOD pair; palette banks 10..13 installed per
 * frame only while displayed. */
/* a clone slot's select-cell portrait tiles ride the TOP 32 tiles of his
 * own clone-art arena (16-slot rosters no longer fit below the reserved
 * 0xFF00 range); emit_cell carries tile bits 16+ in the record's magic
 * bytes like sprite.c */
#define CLONE_CELL_TILE(sl) (0x10000u + ((unsigned)(sl) + 1u) * 0x6000u - 32u)
#define CLONE_BANK0  10u
/* clone cells are REAL grid positions 12..15 (user 2026-08-24: "add more
 * portraits, not hide"): col 6 = pos 12 (top) / 13 (bottom), col 7 =
 * 14 / 15. Cell world x below; third camera stop shows cols 5..7. */
#define POS_CLONE0    12u
/* clone columns page by COLUMN PAIR: pair k (positions 12+4k..15+4k) rests
 * the camera at 0x320 + k*0x120 (pair 0 shows cols 5(LOD)/6/7; later pairs
 * their own two columns). Sprite palette banks 10..13 are shared across
 * pairs, so cells draw only on their pair's page. */
/* sprite palette banks of the clone cells: up to four columns (8 cells) are
 * on screen mid-scroll, so 8 banks rotate by cell: 5-8 and 10-13 (0-4 hold
 * the cursor/scene palettes the 0x2AEA allocator deals, 9 is the outfit
 * indicator, 14/15 the LOD pair) */
static int cursor_col(unsigned pos); static int ext_cam_target(void); static int ext_cell(int col, int row); static int ext_row(unsigned pos);   /* below */
static unsigned clone_bank_of(unsigned pos)
{
    static const unsigned banks[8] = { 10, 11, 12, 13, 5, 6, 7, 8 };
    return banks[(pos - POS_CLONE0) & 7u];
}
/* DISPLAY COMPACTION (user 2026-08-25): registered clones fill the grid
 * cells consecutively from POS_CLONE0 no matter which SLOT they occupy —
 * superstars keeps only slot 15, which must sit in the FIRST clone cell,
 * not stranded at column 7. clone_disp(pos) = the clone id shown at a
 * grid position (-1 = none). */
static int clone_disp(unsigned pos)
{
    unsigned want, seen = 0;
    if (pos < POS_CLONE0 || pos >= (unsigned)ENG_WS_EXT_MAX) return -1;
    want = pos - POS_CLONE0;
    for (int id = 12; id < ENG_WS_EXT_MAX; id++)
        if (eng_ws_clone_base(id) >= 0 && !eng_ws_hidden(id)) {   /* class templates never show */
            if (seen == want) return id;
            seen++;
        }
    return -1;
}
static int clone_cell_on(unsigned pos)
{
    return clone_disp(pos) >= 0;
}
static int clone_col_x(unsigned pos)   /* world x of the cell */
{
    return 0x3A0 + ((int)(pos - POS_CLONE0) / 2) * 0x60;
}
static struct {
    int state[32];             /* per clone slot 12..43: 0 untried, 1 ok, -1 none */
    uint16_t pal[32][16];
} cpor;

/* grid pos -> wrestler id; the ROM table 0x64CC is 10 entries, the two new
 * cells are the LOD pair (data/wrestlers/04 Hawk, 05 Animal). */
static unsigned pos_wid(unsigned pos)
{
    if (pos >= POS_CLONE0) {             /* clone cells pick the clone DISPLAYED
                                            there (compacted; falls back to the
                                            raw position when unregistered) */
        int id = clone_disp(pos);
        return id >= 0 ? (unsigned)id : pos;
    }
    if (pos == POS_HAWK)   return 4u;
    if (pos == POS_ANIMAL) return 5u;
    return tbl16(TBL(cs_pos_wrestler), pos * 2u);
}

static int clone_of(unsigned base)
{
    for (int k = 12; k < ENG_WS_EXT_MAX; k++)
        if (eng_ws_clone_base(k) == (int)base && !eng_ws_hidden(k)) return k;
    return -1;
}

/* ---- ROM 0x21B4 shape: the engine's shared generator (ai.c) ---- */
static unsigned rng(void) { return eng_rng(); }

/* 0x247C screen position: x - camx, y + z - camy (no sprite offsets here). */
static void screen_pos(cs_slot *s, const eng_state *st)
{
    s->sx = s->x - cs.cam_x;
    s->sy = s->y + (int)CURSOR_Z - st->cam_y;
}

/* 0x5CD0: grid cell of the current position -> object x/y. The extended
 * cells extrapolate the table's own column pitch from the col-4 rows. */
static void pos_xy(cs_slot *s, const eng_state *st)
{
    unsigned p = s->pos, add = 0;
    if (p >= POS_CLONE0) {             /* clone columns 6/7 */
        add = (unsigned)(clone_col_x(s->pos) - 0x2E0);  /* col-4 cell x = 0x2E0 */
        p = (s->pos & 1u) ? 9u : 4u;
    } else if (p >= 10u) {             /* select_extended col 5 */
        p = (s->pos == POS_HAWK) ? 4u : 9u;
        add = 0x60u;                   /* 0x64A4 column pitch */
    }
    s->x = (int)(tbl16(TBL(cs_pos_xy), p * 4u) + add);
    s->y = (int)tbl16(TBL(cs_pos_xy), p * 4u + 2u);
    if (cs.rumble) s->y += 0x200;                      /* 0x5CF4 */
    screen_pos(s, st);
}

/* 0x5F28: spawn a cursor object on slot `s` with the nP identity `idx`. */
static void spawn(int si, unsigned idx)
{
    cs_slot *s = &cs.slot[si];
    unsigned tab = TAB_CURSOR;                         /* 0x6494 path: TODO EXACT */
    s->active = 1;
    s->pose = r16(tab + idx * 4u);
    s->colour = r16(tab + idx * 4u + 2u);
    s->flash_t = s->flash_i = 0;
    /* jsr 0x2AEA: sprite palette bank for row 0xD — sprite.c installs it */
    if (!cs.rumble) {                                  /* 0x5F94 */
        cs_slot *m = &cs.slot[si ^ 1];
        if (m->active && m->picked && m->pos == 8 && !ext_on()) {   /* Demolition pair (stock grid only) */
            s->picked = 1;
            s->pos = (m->pos + 1u) & 0xFFu;
            s->pos = m->prev;                          /* 0x5FC0 as written */
        }
        return;
    }
    {                                                  /* 0x5FC8 rumble */
        unsigned p = 0;
        if (ext_on()) {                /* extended: a free cell inside the shown window */
            int k = (cs.cam_x - (int)SCROLL_L) / 0x60, got = -1;
            for (int c = k; c <= k + 2 && got < 0; c++)
                for (int r = 0; r < 2 && got < 0; r++) { int q = ext_cell(c, r); if (q >= 0 && !cs.occ[q]) got = q; }
            p = got >= 0 ? (unsigned)got : 0u;
        } else {
            unsigned list = (cs.flags & F_PAGE_R) ? TAB_SEAT_R : TAB_SEAT_L;
            for (int i = 5; i >= 0; i--) {
                p = r8(list + (unsigned)i);
                if (!cs.occ[p]) break;
            }
        }
        cs.occ[p] = 1;
        s->pos = s->prev = p;
    }
}

/* 0x5EAE: per-frame highlight of a picked portrait. Pen 15 of the fg
 * palette bank that owns the portrait background cycles through the
 * 0x5F0A colours and settles on the player colour on the last frame. */
static void flash(cs_slot *s)
{
    unsigned val;
    if ((s->flash_t & 0xFFu) == FLASH_LEN - 1u)        /* 0x5EE2 cmpi.b #$4b */
        val = s->colour;
    else
        val = tbl16(TBL(cs_flash_colours), s->flash_i * 2u);
    if (s->pos >= 10u || (ext_on() && s->pos >= 8u && demo.loaded > 0)) {
                                       /* select_extended: the LOD/clone cells
                                          are sprites, their panel pen (15)
                                          lives in the sprite bank — same
                                          flash cycle. Clone cells alias banks
                                          10..13 per column PAIR (draw gate);
                                          pos 8/9 = the Smash/Crush cells. */
        unsigned bank = (s->pos >= POS_CLONE0)
                      ? clone_bank_of(s->pos)            /* the cell's rotating bank (draw) */
                      : s->pos >= 10u ? LOD_BANK0 + (s->pos - POS_HAWK)
                                      : DEMO_BANK0 + (s->pos - 8u);
        s->pen_addr = PAL_SPR + bank * 0x80u + 0x1Eu;
        s->pen_val = val;
        m68k_write_memory_16(s->pen_addr, val);
        s->flash_i++;
        if ((s->flash_i & 0xFFu) == 5u) s->flash_i = 0;
        return;
    }
    {
    unsigned bank = tbl8(cs.rumble ? TBL(cs_pos_palette_bank_rumble) : TBL(cs_pos_palette_bank_tag), s->pos);   /* 0x5F14 / 0x5F1E */
    s->pen_addr = PAL_FG + bank * 0x80u + 0x1Eu;
    s->pen_val = val;
    m68k_write_memory_16(s->pen_addr, val);
    s->flash_i++;
    if ((s->flash_i & 0xFFu) == 5u) s->flash_i = 0;    /* 0x5EFC */
    }
}

/* ROM port byte as 0x64E0 sees it: bits 0-3 R/L/U/D, 4-5 B1/B2, 7 start.
 * The engine keeps start at bit 6. */
static unsigned port_byte(uint32_t bits)
{
    return (bits & 0x3Fu) | ((bits & 0x40u) << 1);
}

/* 0x64E0: joystick edge + button edge digest, or the auto-pilot once the
 * screen has timed out ($1C015E b4). */
static void input_read(cs_slot *s, const eng_state *st)
{
    if (!(cs.flags & F_AUTO)) {
        unsigned raw = port_byte(st->inputs[s->port]) & 0xFFu;
        unsigned nib = raw & 0x0Fu;
        unsigned bt;
        if (s->last_dir == nib) nib = 0;               /* 0x6506: same nibble = no move */
        else s->last_dir = raw & 0x0Fu;
        s->dir = nib;
        bt = (raw >> 4) & 3u;                          /* 0x651A */
        s->newp = bt & ~s->held;                       /* 0x6520 and/eor */
        s->held = bt;
        return;
    }
    /* 0x6534 */
    if (++s->auto_t < AUTO_PRESS) {
        unsigned r;
        s->auto_c++;
        r = (rng() & 0x0Fu) | 4u;
        if (r >= s->auto_c) return;                    /* 0x6552 bcc */
        s->auto_c = 0;
        s->dir = tbl8(TBL(cs_auto_dir), rng() & 7u);    /* 0x6566 */
        return;
    }
    s->auto_t = AUTO_REARM;                            /* 0x6576 */
    s->newp |= 1;                                      /* move.b #1,+0xA3 */
}

/* 0x600E: after the page flips, cursors that are not on the hinge column
 * are moved to the first free position of the new page. */
static void reseat(void)
{
    unsigned list = TAB_SEAT_L;
    if (ext_on()) {                    /* extended: every other cursor must fit the
                                          window the lead cursor asked for */
        int k = (ext_cam_target() - (int)SCROLL_L) / 0x60;
        for (int i = 0; i < 4; i++) {
            cs_slot *s = &cs.slot[i];
            int c, row, want = -1;
            if (!s->active || s->picked || i == cs.lead) continue;
            c = cursor_col(s->pos);
            if (c >= k && c <= k + 2) continue;
            row = ext_row(s->pos);
            c = c < k ? k : k + 2;     /* the nearest window column */
            for (int d = 0; d < 3 && want < 0; d++)                 /* that column, then inward */
                for (int r = 0; r < 2 && want < 0; r++) {
                    int q = ext_cell(c < k + 1 ? c + d : c - d, (row + r) & 1);
                    if (q >= 0 && !cs.occ[q] && !cs.pick[q]) want = q;
                }
            if (want < 0) continue;
            cs.occ[s->pos] = 0; s->pos = s->prev = (unsigned)want; cs.occ[want] = 1;
        }
        return;
    }
    if (cs.flags & F_PAGE_R) list = cs.rumble ? TAB_SEAT_R : TAB_SEAT_RT;
    for (int i = 0; i < 4; i++) {
        cs_slot *s = &cs.slot[i];
        if (!s->active || s->picked || s->pos == 2 || s->pos == 7) continue;
        cs.occ[s->pos] = 0;
        for (int k = 5; k >= 0; k--) {
            unsigned p = r8(list + (unsigned)k);
            if (cs.occ[p]) continue;
            s->pos = s->prev = p;
            cs.occ[p] = 1;
            break;
        }
    }
}

/* 0x658C: slide the window 8 px per frame to the page's rest x. */
static int cursor_col(unsigned pos)    /* grid column of a cursor position */
{
    if (pos >= POS_CLONE0) return 6 + (int)(pos - POS_CLONE0) / 2;
    if (pos >= 10u) return 5;
    return (int)(pos % 5u);
}
/* EXTENDED GRID CAMERA (user 2026-08-27: "the selector should stay in the
 * middle as it shifts the page, until it hits the end - then the selector
 * can shift to the right cell; many more columns once we add a lot of
 * wrestlers"). One column model: col 0-4 stock, 5 LOD, 6+ clones; every
 * cell at world x 0x160 + col*0x60; the window shows THREE columns and
 * rests at 0x140 + k*0x60 (k = first shown column). The LEAD cursor (the
 * one that moved last) sits in the middle column when it can: k = its
 * col - 1, clamped to [0, last col - 2]; the other cursor is re-seated
 * into the window if the shift left it outside (reseat()). */
static int ext_last_col(void)
{
    int last = 5;
    for (unsigned p = POS_CLONE0; p < (unsigned)ENG_WS_EXT_MAX; p++) if (clone_cell_on(p)) last = cursor_col(p);
    return last;
}
static int ext_col_first(int lead_col)
{
    int k = lead_col - 1, kmax = ext_last_col() - 2;
    if (kmax < 0) kmax = 0;
    if (k < 0) k = 0;
    if (k > kmax) k = kmax;
    return k;
}
static int ext_cam_target(void)
{
    int lead = cs.lead >= 0 && cs.lead < 4 && cs.slot[cs.lead].active && !cs.slot[cs.lead].picked ? cs.lead : -1;
    if (lead < 0)                      /* no lead: the first live cursor */
        for (int i = 0; i < 4 && lead < 0; i++) if (cs.slot[i].active && !cs.slot[i].picked) lead = i;
    if (lead < 0) return cs.cam_x;
    return (int)SCROLL_L + ext_col_first(cursor_col(cs.slot[lead].pos)) * 0x60;
}
/* the cell at (col, row): stock 0..9, LOD 10/11, clones 12+; -1 = none */
static int ext_cell(int col, int row)
{
    if (col < 0) return -1;
    if (col < 5) return row * 5 + col;
    if (col == 5) return (int)(row ? POS_ANIMAL : POS_HAWK);
    { unsigned p = POS_CLONE0 + (unsigned)(col - 6) * 2u + (unsigned)row;
      return p < (unsigned)ENG_WS_EXT_MAX && clone_cell_on(p) ? (int)p : -1; }
}
static int ext_row(unsigned pos) { return pos >= POS_CLONE0 ? (int)(pos & 1u) : pos >= 10u ? (int)(pos - 10u) : (int)(pos / 5u); }
static void scroll_step(void)
{
    int target = ext_on() ? ext_cam_target() : (int)SCROLL_R;
    if (!ext_on() && !(cs.flags & F_PAGE_R)) target = (int)SCROLL_L;
    if (cs.cam_x == target) { cs.flags &= ~F_SCROLLING; return; }
    cs.cam_x += (cs.cam_x < target) ? SCROLL_STEP : -SCROLL_STEP;
    if (cs.cam_x == target) cs.flags &= ~F_SCROLLING;
}

/* 0x60A2: a new human pressing START takes HIS OWN slot (port i = cursor
 * slot i: 2P = the left team's partner, 3P/4P = the right team) and PAYS a
 * credit (the 0x55A test). The 2P-vs sub-select (0x610A) is TODO EXACT. */
static int join(const eng_state *st)
{
    for (int i = 0; i < 4; i++) {
        cs_slot *s = &cs.slot[i];
        if (!(st->inputs[i] & 0x40u) || s->seated) continue;
        if (i >= eng_join_maxports()) continue;        /* cabinet SW2:3-4 player cap */
        if (!eng_take_seat()) continue;                /* 0x55A: no credit, no seat */
        eng_seated |= 1 << i;
        s->port = i;
        s->seated = 1;
        cs.timeout = 0;
        if (dbg()) fprintf(stderr, "cs: port %d bought seat/cursor %d (credits %u)\n",
                           i + 1, i, eng_credits());
        return i;
    }
    return -1;
}

/* 0x5A00..0x5B3A: button press on a position. */
static void press(int si, const eng_state *st)
{
    cs_slot *s = &cs.slot[si];
    cs_slot *m = &cs.slot[si ^ 1];                     /* +0x86 teammate (0x58BC) */
    int leader = (si == 0 || si == 2);                 /* 0x5B1A: D6 == 3 or 1 */

    if (cs.pick[s->pos] && !eng_mode_rule(MODE_MIRROR)) {   /* 0x5A0E already taken
                                          (mode mirror_picks: duplicates allowed —
                                          the seating pass gives the second copy
                                          an auto ALT palette) */
        if (cs.flags & F_AUTO) s->newp = 0;            /* 0x5B08 */
        pos_xy(s, st);
        return;
    }
    cs.pick[s->pos] = 1;
    cs.occ[s->pos] = 0;
    if (cs.rumble) { s->picked = 1; return; }          /* 0x5AF2 */
    if (s->pos == 8 && !ext_on()) {                    /* 0x5A2C Demolition: both — the
                                          EXTENDED grid picks Smash and Crush individually
                                          (their own cells, like rumble; user 2026-08-27) */
        if (!m->active) m->port = s->port;             /* 0x5A42 */
        if (m->picked) {                               /* 0x5A48 -> 0x5AFC */
            if (cs.flags & F_AUTO) s->newp = 0;
            pos_xy(s, st);
            return;
        }
        m->picked = 1;
        cs.occ[m->pos] = 0;
        m->active = 1;
        m->pos = s->pos; m->prev = s->prev;
        m->colour = s->colour;
        if (si > (si ^ 1)) s->pos = (s->pos + 1u) & 0xFFu;   /* 0x5A86 cmpa */
        else m->pos = (m->pos + 1u) & 0xFFu;
        {   /* pos+1 off a clone cell can land in a HOLE (an unregistered
             * slot): drop that cursor to the first free stock position */
            cs_slot *w = (si > (si ^ 1)) ? s : m;
            if (w->pos >= 10u && !((w->pos == POS_HAWK || w->pos == POS_ANIMAL) || clone_cell_on(w->pos)))
                for (unsigned k = 0; k < 10u; k++)
                    if (!cs.occ[k] && !cs.pick[k]) { w->pos = w->prev = k; break; }
        }
        s->picked = 1;
        return;
    }
    s->picked = 1;                                     /* 0x5A96 */
    if (m->active) return;                             /* 0x5AA0 */
    if (m->seated) {                                   /* 0x5AAA -> 0x5B10 */
        if (s->seated) return;
        if (leader) { unsigned t = s->pos; s->pos = m->pos; m->pos = t; }   /* 0x5B2A */
        return;
    }
    /* 0x5AB4: hand the port to the partner cursor and spawn it here */
    m->port = s->port;
    m->dir = s->dir; m->last_dir = s->last_dir;
    m->newp = s->newp; m->held = s->held;
    spawn(si ^ 1, tbl8(TBL(cs_slot_index), 3u - (unsigned)si));   /* 0x5AD2 */
    m->pos = s->pos; m->prev = s->prev;                /* 0x5AE2 */
    pos_xy(s, st);
}

/* 0x5B3E..0x5CC8: joystick move with occupancy and the page hinge. */
static void move(int si, const eng_state *st)
{
    cs_slot *s = &cs.slot[si];
    unsigned tab = TAB_MOVE_L;
    unsigned np;
    if (cs.flags & F_PAGE_R) { tab = TAB_MOVE_R; if (cs.rumble) tab = TAB_MOVE_RR; }
    s->prev = s->pos;
    if (ext_on()) {                    /* extended grid: hand movement over the
                                          column model (no ROM page tables) */
        unsigned nib = s->dir & 0xFFu;
        int col = cursor_col(s->pos), row = ext_row(s->pos), last = ext_last_col(), q = -1;
        int dc = (nib & 1u) ? 1 : (nib & 2u) ? -1 : 0, dr = (nib & 0xCu) ? 1 : 0;
        np = s->pos;
        if (dc) {                      /* nearest column that way with a cell in my
                                          row (a clone column may hold one man) */
            for (int c = col + dc; c >= 0 && c <= last && q < 0; c += dc) {
                q = ext_cell(c, row);
                if (q < 0) q = ext_cell(c, row ^ 1);
            }
        } else if (dr) q = ext_cell(col, row ^ 1);
        if (q >= 0) np = (unsigned)q;
        s->pos = np;
        if (cs.occ[np] && np != s->prev) {                  /* occupied: the other row of
                                          that column, else stay (0x5B8C analog) */
            int q2 = ext_cell(cursor_col(np), ext_row(np) ^ 1);
            s->pos = (q2 >= 0 && !cs.occ[q2]) ? (unsigned)q2 : s->prev;
        }
        cs.occ[s->prev] = 0;
        cs.occ[s->pos] = 1;
        cs.lead = si;
        if (!(cs.flags & F_SCROLLING) && s->pos != s->prev && cs.cam_x != ext_cam_target()) goto scroll;
        goto done;
    }
    np = r8(tab + s->pos * 11u + (s->dir & 0xFFu));    /* 0x5B3E: the ROM page table (stock grid) */
    s->pos = np;
    if (cs.occ[np]) {                                  /* 0x5B8C */
        {
        unsigned alt = r8(r32(TAB_ALT + s->prev * 4u) + np);
        s->pos = alt;
        if (!cs.rumble && alt == 9) s->pos = s->prev;  /* 0x5BB6 */
        else if (alt == s->prev) { /* stay */ }        /* 0x5BC0 */
        else if (cs.occ[alt]) s->pos = s->prev;        /* 0x5BD2 */
        }
    }
    cs.occ[s->prev] = 0;                               /* 0x5BF4 */
    cs.occ[s->pos] = 1;
    if (cs.flags & F_SCROLLING) goto done;             /* 0x5BFE */
    if (s->pos == s->prev) goto done;                  /* 0x5C0A */
    if (s->pos == 2) { if (s->prev == 7) goto done; goto scroll; }    /* 0x5C16 */
    if (s->pos == 7) { if (s->prev == 2) goto done; goto scroll; }
    if (cs.flags & F_PAGE_R) {                         /* 0x5C26 */
        if (s->pos == 1) {
            if (s->prev != 2) goto done;
            cs.occ[1] = 0; cs.occ[2] = 1; s->pos = 2; goto scroll;   /* 0x5C6A */
        }
        if (s->pos == 6) {
            if (s->prev != 7) goto done;
            cs.occ[6] = 0; cs.occ[7] = 1; s->pos = 7; goto scroll;   /* 0x5C96 */
        }
        goto done;
    }
    if (s->pos == 3) {
        if (s->prev != 2) goto done;
        cs.occ[3] = 0; cs.occ[2] = 1; s->pos = 2; goto scroll;
    }
    if (s->pos == 8) {
        if (s->prev != 7) goto done;
        cs.occ[8] = 0; cs.occ[7] = 1; s->pos = 7; goto scroll;
    }
    goto done;
scroll:
    cs.flags |= F_SCROLLING | F_SCROLL_REQ;            /* 0x5CC0 */
done:
    pos_xy(s, st);
}

/* B2 on an unpicked cell: cycle the wrestler's OUTFIT when a mod ships
 * alternate palettes for him (palsel.c; pluggable per wrestler). The
 * outfit tag is drawn under the cell by cs_draw. Registered clones are
 * real cells (12..15) — the old clone toggle is retired. */
static void palette_cycle(int si, const eng_state *st)
{
    cs_slot *s = &cs.slot[si];
    int id = (int)pos_wid(s->pos);
    int n = eng_palsel_count(id);
    if (n > 0 && !cs.pick[s->pos]) {
        eng_palsel_set(id, (eng_palsel_get(id) + 1) % (n + 1));
        if (dbg())
            fprintf(stderr, "cs: id %d outfit -> %d (%s)\n", id,
                    eng_palsel_get(id),
                    eng_palsel_get(id) ? eng_palsel_name(id, eng_palsel_get(id)) : "stock");
    }
    pos_xy(s, st);
}

/* the id a position resolves to at the roster write (0x5DAC; clone cells
 * pick their own id via pos_wid) */
static unsigned pos_pick_id(unsigned pos)
{
    return pos_wid(pos);
}

/* 0x5DAC + 0x10370..0x104A4: the roster table and the CPU team.
 * SLOT-MAPPED (user 2026-08-28 buy-in spec): cursor pair (0,1) = the LEFT
 * team, (2,3) = the RIGHT team; every pick stays in its slot and carries
 * the PORT of the seat that made it (cs.ports, -1 = no human). The old
 * sequential compaction put a 3P pick into the LEFT team's partner slot. */
static void finish(void)
{
    for (int i = 0; i < 4; i++) {
        cs_slot *s = &cs.slot[i];
        cs.roster[i] = s->active ? pos_pick_id(s->pos) : 0xFFFFu;   /* 0x64CC via pos_wid */
        cs.ports[i]  = (s->active && s->seated) ? s->port : -1;
    }
    /* 0x5DF0: a leader slot that is active but not human swaps with its
     * seated partner (+0x56/+0x02 and the roster pair) — the human's own
     * pick leads the team. Applied per SIDE (0x5E48 never swaps a CPU pair). */
    if (!cs.rumble)
        for (int side = 0; side < 2; side++) {
            int L = side * 2, P = L + 1;
            if (cs.roster[L] != 0xFFFFu && cs.ports[L] < 0 && cs.ports[P] >= 0) {
                unsigned t = cs.roster[L]; cs.roster[L] = cs.roster[P]; cs.roster[P] = t;
                { int tp = cs.ports[L]; cs.ports[L] = cs.ports[P]; cs.ports[P] = tp; }
            }
        }
    /* 0x10390..0x104A4: the CPU team for stage 0 fills the EMPTY slots —
     * the same routine the ladder reruns every match (campaign.c). Seated
     * 3P/4P picks already sit in [2]/[3] and are kept. */
    {
        unsigned w0 = 0, w1 = 0;
        if (cs.rumble || (cs.roster[2] == 0xFFFFu && cs.roster[3] == 0xFFFFu))
            eng_camp_pick_cpu(0, cs.roster, &w0, &w1);   /* appended at the first empty pair */
        else if (cs.roster[2] == 0xFFFFu || cs.roster[3] == 0xFFFFu) {
            /* one human on the right team: draw ONE CPU partner (a scratch
               roster keeps the append out of the faced list) */
            unsigned tmp[ROSTER_N];
            memcpy(tmp, cs.roster, sizeof tmp);
            eng_camp_pick_cpu(0, tmp, &w0, &w1);
            cs.roster[cs.roster[2] == 0xFFFFu ? 2 : 3] = w0;
        }
    }
    for (int i = 0; i < 4; i++) cs.picks[i] = (int)(cs.roster[i] & 0xFFFFu);
    cs.have_picks = 1;
    /* 0x5EA8 jmp 0xAC0: a new game starts at stage 0 with this roster as
     * the faced list $1C0598 (campaign.c). */
    eng_camp_new_game(cs.picks, cs.roster);
    if (dbg())
        fprintf(stderr, "cs: picks %d %d %d %d\n", cs.picks[0], cs.picks[1], cs.picks[2], cs.picks[3]);
}

const int *eng_cs_picks(void)
{
    return cs.have_picks ? cs.picks : NULL;
}

/* the port that owns each roster slot (-1 = CPU/autopilot); NULL when the
 * select screen never ran (harness matches keep the old side layout) */
const int *eng_cs_ports(void)
{
    return cs.have_picks ? cs.ports : NULL;
}

/* ---- scene ops ---- */

static void cs_begin(eng_state *st)
{
    eng_sprite_scene_pals_begin();     /* 0x58A0/0x5F84 jsr 0x2AEA: the select
                                          screen's sprite ids (cursor box etc.)
                                          install on demand — post-fade the
                                          stale banks drew the box BLACK */
    memset(&cs, 0, sizeof cs);
    cs.rumble = eng_gs_rumble();
    /* 0x58B2..0x5928 (tag) / 0x592A..0x5968 (rumble): window, scene word,
     * compose, fg0 clear (0x1F9E), arena palette 3, 0x2A06. */
    st->scene = SCENE_WORD;
    st->cam_x = cs.cam_x = (int)SCROLL_L; cs.lead = -1;
    st->cam_y = cs.rumble ? 0x500 : 0x300;
    eng_bg_cam_x = (int)SCROLL_L;                      /* $1C17EA stays: only the FG page scrolls */
    eng_scene_publish(ARENA_PAL, TEXT_PAL_SET);
    memset(wf.spriteram, 0, sizeof wf.spriteram);
    memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);
    eng_credit_force(); eng_credit_line();   /* live CREDIT line (0x1E92) */
    /* 0x57E0: every seat that joined BEFORE the select (attract/game
     * select START, credit.c eng_start_scan -> eng_seated) gets its
     * cursor here — port i owns cursor slot i (user 2026-08-28). */
    for (int i = 0; i < 4; i++) {
        cs_slot *s = &cs.slot[i];
        if (!(eng_seated & (1 << i))) continue;
        s->active = 1;
        s->port = i;
        s->pos = s->prev = tbl16(TBL(cs_seat_pos), (unsigned)i * 2u);
        s->x = (int)tbl16(TBL(cs_seat_xy), (unsigned)i * 4u);
        s->y = (int)tbl16(TBL(cs_seat_xy), (unsigned)i * 4u + 2u);
        s->pose = r16(TAB_CURSOR + (unsigned)i * 4u);
        s->colour = r16(TAB_CURSOR + (unsigned)i * 4u + 2u);
        s->seated = 1;
        cs.occ[s->pos] = 1;
        screen_pos(s, st);
    }
    for (unsigned k = 0; k < ROSTER_N; k++) cs.roster[k] = 0xFFFFu;   /* 0x5980 */
    cs.have_picks = 0;
    for (int k = 0; k < 4; k++) cs.ports[k] = -1;
    memset(cpor.state, 0, sizeof cpor.state);          /* re-resolve clone cells per profile */
    eng_palsel_reset();                                /* fresh outfits per game (+ WF_PALSEL) */
    cs.phase = 0;
    cs.wait = ENTRY_WAIT;                              /* 0x5978 */
    if (dbg()) fprintf(stderr, "cs: begin f%lld rumble=%d\n", (long long)st->frame, cs.rumble);
}

static int cs_update(eng_state *st)
{
    int busy = 0;

    if (cs.phase == 0) {
        if (--cs.wait > 0) return -1;
        cs.phase = 1;
        cs.timeout = 0;                                /* 0x5990 */
    }
    if (cs.phase == 2) {
        if (--cs.wait > 0) return -1;
        cs.phase = 3;
        eng_bg_cam_x = -1;
        return ENG_SCENE_MATCH;                        /* 0x5EA8 jmp $AC0 (walk-in) */
    }

    /* 0x5996 loop head */
    cs.frames++;
    if (ext_on() && !(cs.flags & F_SCROLLING)
        && cs.cam_x != ext_cam_target())
        cs.flags |= F_SCROLLING;       /* peek stops: re-aim on any cursor move */
    if (cs.flags & F_SCROLLING) scroll_step();         /* 0x59AC */
    else if (++cs.timeout >= TIMEOUT) cs.flags |= F_AUTO;   /* 0x59B2 */
    st->cam_x = cs.cam_x;                              /* $1C17E6 -> window (0x26A42) */

    for (int i = 0; i < 4; i++) {                      /* 0x59CA */
        cs_slot *s = &cs.slot[i];
        if (!s->active || s->picked) continue;
        if (cs.flags & F_SCROLLING) { pos_xy(s, st); continue; }
        input_read(s, st);
        if (s->newp & 0xFFu) {                         /* 0x59F8 tst.b +0xA3 */
            if ((s->newp & 0xFFu) == 2u
                && eng_palsel_count((int)pos_wid(s->pos)) > 0)
                palette_cycle(i, st);   /* B2 = outfit (only when modded) */
            else press(i, st);
        }
        else move(i, st);
    }
    /* 0x5D10 sprite flush + vblank: the renderer draws what draw() emits */

    {                                                  /* 0x5D28 join */
        int j = join(st);
        if (j >= 0) spawn(j, (unsigned)j);
    }
    if (cs.flags & F_SCROLL_REQ) {                     /* 0x5D32 */
        cs.flags &= ~F_SCROLL_REQ;
        if (!ext_on()) cs.flags ^= F_PAGE_R;           /* (the extended grid has no pages) */
        reseat();
    }
    for (int i = 0; i < 4; i++) {                      /* 0x5D48 */
        cs_slot *s = &cs.slot[i];
        if (!s->active) continue;
        if (!s->picked) { busy = 1; continue; }
        if (++s->flash_t < FLASH_LEN) { flash(s); busy = 1; }
    }
    if (dbg() && ((cs.frames % 30) == 0 || cs.frames < 3))
        fprintf(stderr, "cs: f%lld t=%03X fl=%02X cam=%X s0 pos=%u pk=%d s1 act=%d pos=%u pk=%d\n",
                (long long)st->frame, cs.timeout, cs.flags, cs.cam_x,
                cs.slot[0].pos, cs.slot[0].picked, cs.slot[1].active, cs.slot[1].pos, cs.slot[1].picked);
    if (busy) return -1;
    if ((cs.flags & F_VS_A) && !(cs.flags & F_VS_B)) return -1;   /* 0x5D88 */

    finish();                                          /* 0x5DA0 */
    cs.flags = 0; cs.timeout = 0;                      /* 0x5E82 */
    cs.phase = 2;
    cs.wait = EXIT_WAIT;                               /* 0x5EA0 */
    if (dbg()) fprintf(stderr, "cs: done f%lld\n", (long long)st->frame);
    return -1;
}

/* ---- select_extended cell art ----
 * data/select/04.png / 05.png are the LOD select portraits (80x80 RGB,
 * 15 colours each — the exact xBGR444 palette survives as n*17 channel
 * values). They were verified against this engine's own composed cells:
 * the sibling files for the ten stock wrestlers render pixel-identical to
 * the scene-3 tilemap cells, and the two LOD files carry the same
 * provenance (the attract interview screen's sprite art re-cut to the
 * select cell format). Loaded once, cut into 25 injected sprite tiles per
 * cell, palettes kept for the per-frame bank 14/15 install. */
/* parse one 80x80 <=15-colour select-cell PNG into 25 injected-tile pens
 * (corner colour = pen 15, the 0x5EAE flash target); 0 = ok */
static int cell_png_load(const char *path, uint8_t *pens, uint16_t *pal)
{
    uint8_t *rgba = NULL; int w = 0, h = 0;
    uint32_t cols[16]; int ncol = 0;
    uint32_t panel;
    if (wf_video_load_rgba_png(path, &rgba, &w, &h) != 0) return -1;
    if (w != 80 || h != 80) { free(rgba); return -1; }
    panel = (uint32_t)rgba[0] | ((uint32_t)rgba[1] << 8) | ((uint32_t)rgba[2] << 16);
    memset(pal, 0, 16 * sizeof *pal);
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 80; x++) {
            const uint8_t *px = rgba + ((size_t)y * 80 + x) * 4;
            uint32_t c = (uint32_t)px[0] | ((uint32_t)px[1] << 8)
                       | ((uint32_t)px[2] << 16);
            int pen = -1;
            if (c == panel) pen = 15;
            else {
                for (int i = 0; i < ncol; i++)
                    if (cols[i] == c) { pen = i + 1; break; }
                if (pen < 0) {
                    if (ncol >= 14) { free(rgba); return -1; }   /* not our art */
                    cols[ncol] = c; pen = ++ncol;
                }
            }
            pens[(size_t)((x >> 4) * 5 + (y >> 4)) * 256
                 + (size_t)(y & 15) * 16 + (x & 15)] = (uint8_t)pen;
        }
    for (int i = 0; i < ncol; i++)
        pal[i + 1] = (uint16_t)(((((cols[i] >> 16) & 0xFFu) / 17u) << 8)   /* B */
                              | ((((cols[i] >> 8) & 0xFFu) / 17u) << 4)    /* G */
                              |   ((cols[i] & 0xFFu) / 17u));              /* R */
    pal[15] = (uint16_t)(((((panel >> 16) & 0xFFu) / 17u) << 8)
                       | ((((panel >> 8) & 0xFFu) / 17u) << 4)
                       |   ((panel & 0xFFu) / 17u));
    free(rgba);
    return 0;
}

static void lod_art_load(void)
{
    static uint8_t pens[2][25 * 256];
    if (lod.loaded) return;
    lod.loaded = -1;
    for (int k = 0; k < 2; k++) {
        char path[64];
        snprintf(path, sizeof path, "data/select/%02d.png", 4 + k);
        if (cell_png_load(path, pens[k], lod.pal[k]) != 0) return;
    }
    wf_video_inject_sprite_tiles(LOD_TILE0, 25, pens[0]);
    wf_video_inject_sprite_tiles(LOD_TILE0 + 25u, 25, pens[1]);
    lod.loaded = 1;
    if (dbg()) fprintf(stderr, "cs: LOD select cells loaded (tiles %04X..)\n", LOD_TILE0);
}

/* the Smash / Crush individual cells (data/select/10.png / 11.png): drawn
 * over pos 8 (the shared DEMOLITION tilemap cell) and pos 9 (the empty
 * panel) so the extended grid picks each Demolition man on his own. */
static void demo_art_load(void)
{
    static uint8_t pens[2][25 * 256];
    if (demo.loaded) return;
    demo.loaded = -1;
    for (int k = 0; k < 2; k++) {
        char path[64];
        snprintf(path, sizeof path, "data/select/%02d.png", 10 + k);
        if (cell_png_load(path, pens[k], demo.pal[k]) != 0) return;
    }
    wf_video_inject_sprite_tiles(DEMO_TILE0, 25, pens[0]);
    wf_video_inject_sprite_tiles(DEMO_TILE0 + 25u, 25, pens[1]);
    demo.loaded = 1;
    if (dbg()) fprintf(stderr, "cs: Smash/Crush select cells loaded (tiles %04X..)\n", DEMO_TILE0);
}

/* clone slot 12..15: mod-layer select/<id>.png -> tiles + kept palette */
static int clone_art_ok(int c)
{
    int s = c - 12;
    static uint8_t pens[25 * 256];
    char rel[32], path[512];
    if (s < 0 || s >= ENG_WS_EXT_MAX - 12) return 0;
    if (cpor.state[s]) return cpor.state[s] > 0;
    cpor.state[s] = -1;
    snprintf(rel, sizeof rel, "select/%02d.png", c);
    if (!wf_mod_resolve(rel, path, sizeof path) || cell_png_load(path, pens, cpor.pal[s]) != 0) {
        /* NO PORTRAIT (user 2026-08-27): a placeholder cell - dark panel,
           light frame, a diagonal cross - so the slot is visibly there
           rather than an empty hole. Tile-major pens (25 x 256). */
        for (int y = 0; y < 80; y++) for (int x = 0; x < 80; x++) {
            int edge = x < 2 || y < 2 || x >= 78 || y >= 78;
            int diag = (x - y) * (x - y) <= 2 || (x + y - 79) * (x + y - 79) <= 2;
            pens[(size_t)((x >> 4) * 5 + (y >> 4)) * 256 + (size_t)(y & 15) * 16 + (x & 15)] = (uint8_t)(edge ? 2 : diag ? 3 : 1);
        }
        for (int k = 0; k < 16; k++) cpor.pal[s][k] = 0;
        cpor.pal[s][1] = 0x0222; cpor.pal[s][2] = 0x0999; cpor.pal[s][3] = 0x0555;   /* xBGR: dark grey, light grey, mid grey */
        if (dbg()) fprintf(stderr, "cs: clone %d has no select cell - placeholder\n", c);
    }
    wf_video_inject_sprite_tiles(CLONE_CELL_TILE(s), 25, pens);
    cpor.state[s] = 1;
    if (dbg()) fprintf(stderr, "cs: clone %d select cell loaded (%s)\n", c, path);
    return 1;
}

static int picked_at(unsigned pos)
{
    for (int i = 0; i < 4; i++)
        if (cs.slot[i].active && cs.slot[i].picked && cs.slot[i].pos == pos)
            return 1;
    return 0;
}

/* a clone's select cell (mod select/<id>.png) as 80x80 pens + palette, for
 * the HUD face (hud.c derives the 3x3 FG0 portrait from it) */
int eng_cs_clone_cell(int clone, uint8_t *pens80, uint16_t *pal)
{
    char rel[32], path[512];
    if (clone < 12 || clone >= ENG_WS_EXT_MAX) return 0;
    snprintf(rel, sizeof rel, "select/%02d.png", clone);
    if (!wf_mod_resolve(rel, path, sizeof path)) return 0;
    {   /* cell_png_load fills tile-major pens (25 x 256); re-lay to 80x80 row-major */
        static uint8_t tp[25 * 256];
        if (cell_png_load(path, tp, pal) != 0) return 0;
        for (int y = 0; y < 80; y++) for (int x = 0; x < 80; x++)
            pens80[y * 80 + x] = tp[(size_t)((x >> 4) * 5 + (y >> 4)) * 256 + (size_t)(y & 15) * 16 + (x & 15)];
    }
    return 1;
}
/* one 80x80 cell as 25 single-tile spriteram records (the record shape of
 * eng_sprite_emit_pose; y is the hardware's 256-complement) */
static void emit_cell(unsigned *slot, int scr_x, int scr_y, unsigned tile0,
                      unsigned bank)
{
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 5; r++) {
            int x = scr_x + c * 16, top = scr_y + r * 16;
            if (x <= -16 || x >= 336) continue;
            if (*slot + 16 > WF_SPRRAM_SIZE) return;
            {
                unsigned stored = (unsigned)((232 - top) & 0x1FF);
                unsigned xs = (unsigned)(x & 0x1FF);
                unsigned t = tile0 + (unsigned)(c * 5 + r);
                uint16_t w1 = (uint16_t)(0x0001u | ((stored & 0x100u) >> 7)
                                       | ((xs & 0x100u) >> 6));
                uint8_t *rec = wf.spriteram + *slot;
                rec[1] = (uint8_t)stored; rec[2] = (uint8_t)(w1 >> 8); rec[3] = (uint8_t)w1;
                rec[5] = (uint8_t)t; rec[7] = (uint8_t)(t >> 8);
                rec[9] = (uint8_t)bank; rec[11] = (uint8_t)xs;
                /* clone-art arena tiles: bits 16+ ride the magic bytes
                 * (video.c drain; same scheme as sprite.c emitters) */
                rec[12] = (uint8_t)((t >> 16) ? 0xE7 : 0);
                rec[13] = (uint8_t)((t >> 16) & 0x0Fu);
                rec[14] = (uint8_t)((t >> 16) ? 0x5C : 0);
                *slot += 16;
            }
        }
}

/* 8x8 FG0 text (the 0x2508E mode-0 glyphs: tile = char - 0x10) */
static void fg0_text(unsigned row, int col, const char *s, unsigned pal)
{
    for (; *s; s++, col++) {
        unsigned t = (unsigned)(*s - 0x10);
        unsigned off;
        if (col < 0 || col >= 64 || row >= 32) continue;
        off = (row * 64u + (unsigned)col) * 4u;
        wf.fg0_videoram[off + 1] = (uint8_t)t;
        wf.fg0_videoram[off + 3] = (uint8_t)((pal << 4) | ((t >> 8) & 0x0Fu));
    }
}

static void fg0_clear_row(unsigned row, unsigned c0, unsigned c1)
{
    for (unsigned col = c0; col < c1 && col < 64; col++) {
        unsigned off = (row * 64u + col) * 4u;
        wf.fg0_videoram[off + 1] = 0;
        wf.fg0_videoram[off + 3] = 0;
    }
}

/* render.c calls this right after every scene recompose: on the EXTENDED
 * select the baked DEMOLITION label under pos 8 is blanked from the FG
 * plane — the cell shows Smash alone now and ext_draw writes SMASH /
 * CRUSH in FG0 (user 2026-08-27). Tiles are located exactly the way
 * draw_fg_layer maps them to the screen. */
void eng_cs_scene_patch(void)
{
    extern void m68k_write_memory_16(unsigned int address, unsigned int value);
    int lx0, lx1, scrollx, scrolly, fgi;
    if (!(cs.phase == 1 || cs.phase == 2) || cs.rumble || !ext_on()) return;
    demo_art_load();
    if (demo.loaded <= 0) return;
    lx0 = 0x280 - cs.cam_x; lx1 = lx0 + 80;
    if (lx1 <= 0 || lx0 >= 336) return;
    fgi = (wf.priority == 0x78) ? 0 : 2;
    scrollx = wf.scroll[fgi]; scrolly = wf.scroll[fgi + 1];
    for (int i = 0; i < 32 * 32; i++) {
        int col = i % 32, row = i / 32;
        int bx = col * 16 - (scrollx & 511), by = row * 16 - 8 - (scrolly & 511);
        for (int wx = 0; wx < 2; wx++) for (int wy = 0; wy < 2; wy++) {
            int dx = bx + wx * 512, dy = by + wy * 512;
            if (dx < lx1 && dx + 16 > lx0 && dy < 232 && dy + 16 > 216) {
                unsigned j = (unsigned)(row * 32 + ((col + 12) & 31));   /* 12 tiles right =
                                          the plain backdrop under the LOD column, same
                                          wallpaper phase (192 = 6 pattern repeats) */
                m68k_write_memory_16(WF_FGRAM_BASE + (unsigned)i * 4u,
                                     ((unsigned)wf.fg_videoram[j * 4u] << 8) | wf.fg_videoram[j * 4u + 1u]);
                m68k_write_memory_16(WF_FGRAM_BASE + (unsigned)i * 4u + 2u,
                                     ((unsigned)wf.fg_videoram[j * 4u + 2u] << 8) | wf.fg_videoram[j * 4u + 3u]);
                m68k_write_memory_16(WF_BGRAM_BASE + (unsigned)i * 2u,
                                     ((unsigned)wf.bg_videoram[j * 2u] << 8) | wf.bg_videoram[j * 2u + 1u]);
            }
        }
    }
    wf_tilemap_shadow_adopt();         /* the renderer draws the SHADOW, adopted by
                                          compose() before this patch ran - re-adopt */
}

/* the extended overlay: LOD portrait sprites (under the cursor records),
 * their palettes, the cell names and the clone tags. Screen y of the two
 * panel rows is fixed (40/136 on both the tag and rumble pages — the
 * compose keeps the same window height); x follows the page scroll. */
static void ext_draw(unsigned *slot)
{
    lod_art_load();
    if (lod.loaded > 0) {
        for (int k = 0; k < 2; k++) {
            unsigned pos = k ? POS_ANIMAL : POS_HAWK;
            unsigned bank = LOD_BANK0 + (unsigned)k;
            for (int pen = 1; pen < 16; pen++) {
                if (pen == 15 && picked_at(pos)) continue;   /* the flash owns it */
                m68k_write_memory_16(PAL_SPR + bank * 0x80u + (unsigned)pen * 2u,
                                     lod.pal[k][pen]);
            }
            emit_cell(slot, LOD_CELL_X - cs.cam_x, k ? 136 : 40,
                      LOD_TILE0 + (unsigned)k * 25u, bank);
        }
    }
    /* FG0 does not scroll; the 8px page steps land on cell boundaries, so a
     * per-frame redraw tracks the scroll exactly. Rows 16/28 (under the new
     * cells) and 5/17 (over each cell) are empty on this screen. */
    fg0_clear_row(16, 0, 56); fg0_clear_row(28, 0, 56);
    fg0_clear_row(5, 0, 56);  fg0_clear_row(17, 0, 56);
    {
        int cx = (LOD_CELL_X - cs.cam_x) / 8;
        if (cx < 62) {
            fg0_text(16, cx + 3, "HAWK", 0);
            fg0_text(28, cx + 2, "ANIMAL", 0);
        }
    }
    demo_art_load();
    if (demo.loaded > 0)               /* Smash / Crush as separate cells (pos 8/9) */
        for (int k = 0; k < 2; k++) {
            unsigned pos = 8u + (unsigned)k;
            unsigned bank = DEMO_BANK0 + (unsigned)k;
            int x = 0x160 + (3 + k) * 0x60 - cs.cam_x;
            if (x <= -80 || x >= 336) continue;
            for (int pen = 1; pen < 16; pen++) {
                if (pen == 15 && picked_at(pos)) continue;   /* the flash owns it */
                m68k_write_memory_16(PAL_SPR + bank * 0x80u + (unsigned)pen * 2u,
                                     demo.pal[k][pen]);
            }
            emit_cell(slot, x, 136, DEMO_TILE0 + (unsigned)k * 25u, bank);
            fg0_text(28, x / 8 + (k ? 2 : 2), k ? "CRUSH" : "SMASH", 0);
        }
    /* the clone columns: real cells at positions 12..15 (cols 6/7) with
     * the mod's select/<id>.png portrait and the clone's name beneath */
    for (unsigned p = POS_CLONE0; p < (unsigned)ENG_WS_EXT_MAX; p++) {
        const char *nm;
        unsigned rowi = p & 1u;
        int x = clone_col_x(p) - cs.cam_x;
        if (!clone_cell_on(p)) continue;
        if (x <= -80 || x >= 336) continue;   /* the window (mid-scroll: up to 4 columns) */
        int did = clone_disp(p);           /* the clone shown in this cell */
        if (did >= 0 && clone_art_ok(did)) {
            unsigned bank = clone_bank_of(p);
            for (int pen = 1; pen < 16; pen++) {
                if (pen == 15 && picked_at(p)) continue;   /* the flash owns it */
                m68k_write_memory_16(PAL_SPR + bank * 0x80u + (unsigned)pen * 2u,
                                     cpor.pal[did - 12][pen]);
            }
            emit_cell(slot, x, rowi ? 136 : 40,
                      CLONE_CELL_TILE(did - 12), bank);
        }
        if (did >= 0 && (nm = eng_ws_clone_name(did)) != NULL) {
            char clip[11];                 /* cell width = 10 glyphs */
            snprintf(clip, sizeof clip, "%s", nm);
            fg0_text(rowi ? 28u : 16u, x / 8, clip, 0);
        }
    }
}

/* outfit indicator + tags (palette-select mod): a small solid block of
 * the outfit's swatch colour at the top-left of the cell's portrait
 * (user 2026-08-24: "draw a small block with the color in it in the top
 * left of the portrait" — the earlier panel-pen tint is retired), plus
 * the chosen outfit's name under the LOD/clone cells. The ext pages
 * clear rows 16/28 in ext_draw; the stock grid gets a clear here. */
/* the outfit's most-changed pen vs the stock body palette: the swatch */
static unsigned palsel_swatch(int wid)
{
    const uint16_t *alt = eng_palsel_pens(wid);
    /* the reference is the wrestler's own default body palette —
     * eng_pkg_palette serves clones too (own pens or base delegate) */
    const uint16_t *stock = (wid >= 0 && wid < ENG_WS_EXT_MAX)
                          ? eng_pkg_palette((unsigned)wid) : NULL;
    unsigned best = alt ? alt[7] : 0, bd = 0;
    if (!alt) return 0;
    for (int p = 7; p < 16; p++) {
        unsigned a = alt[p], s = stock ? stock[p] : 0;
        unsigned d = 0;
        int hi = 0, lo = 15;
        for (int c = 0; c < 3; c++) {
            int ac = (int)((a >> (c * 4)) & 15u);
            int da = ac - (int)((s >> (c * 4)) & 15u);
            d += (unsigned)(da * da);
            if (ac > hi) hi = ac;
            if (ac < lo) lo = ac;
        }
        /* weight change by the pen's saturation so the outfit's HUE pen
         * beats its grey/white highlight pens (Andre's ROYAL BLUE would
         * otherwise show a near-white swatch) */
        d *= (unsigned)((1 + hi - lo) * (1 + hi - lo));
        if (d > bd) { bd = d; best = a; }
    }
    return best;
}

/* the indicator block: sprite palette bank 9 with one pen per drawn
 * block. Audit of this screen's sprite banks: the nP cursor stream
 * bakes pal 0x0D (drained spriteram; identity-mapped in the harness
 * drives, dealt bank 0 by the 0x2AEA scene allocator in the front-end
 * flow), clone cells own 10..13, LOD cells 14/15 — banks 1..9 are
 * untouched, so 9 is ours. The 14 solid tiles live in the 0xFE32..
 * 0xFE3F gap after the LOD cell tiles (0xFE00..0xFE31): tile j is pen
 * j+1 in the top-left 8x8 quadrant only (pen 0 = transparent), so a
 * 16x16 hardware tile draws an 8x8 block. A 1px ring of pen 15 (black)
 * frames it — a wardrobe swatch can be exactly the panel colour (id 7
 * outfit C is pen 0x0FF0 = the cyan background) and would vanish. */
#define SWATCH_BANK   9u
#define SWATCH_TILE0  0xFE32u
#define SWATCH_MAX    14
static void swatch_tiles_load(void)
{
    static int done;
    static uint8_t pens[SWATCH_MAX * 256];
    if (done) return;
    for (int j = 0; j < SWATCH_MAX; j++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                pens[(size_t)j * 256 + (size_t)y * 16 + x] =
                    (x == 0 || x == 7 || y == 0 || y == 7) ? 15u
                                                           : (uint8_t)(j + 1);
    wf_video_inject_sprite_tiles(SWATCH_TILE0, SWATCH_MAX, pens);
    done = 1;
}

/* one single-tile sprite record (emit_cell's record shape, chain 0) */
static void emit_block(unsigned *slot, int x, int top, unsigned tile)
{
    if (x <= -16 || x >= 336) return;
    if (*slot + 16 > WF_SPRRAM_SIZE) return;
    {
        unsigned stored = (unsigned)((232 - top) & 0x1FF);
        unsigned xs = (unsigned)(x & 0x1FF);
        uint16_t w1 = (uint16_t)(0x0001u | ((stored & 0x100u) >> 7)
                               | ((xs & 0x100u) >> 6));
        uint8_t *rec = wf.spriteram + *slot;
        rec[1] = (uint8_t)stored; rec[2] = (uint8_t)(w1 >> 8); rec[3] = (uint8_t)w1;
        rec[5] = (uint8_t)tile; rec[7] = (uint8_t)(tile >> 8);
        rec[9] = (uint8_t)SWATCH_BANK; rec[11] = (uint8_t)xs;
        rec[12] = 0; rec[13] = 0; rec[14] = 0;
        *slot += 16;
    }
}

static void palsel_tags(unsigned *slot)
{
    int nblk = 0;
    if (cs.rumble) return;
    if (!ext_on()) { fg0_clear_row(16, 0, 56); fg0_clear_row(28, 0, 56); }
    swatch_tiles_load();
    m68k_write_memory_16(PAL_SPR + SWATCH_BANK * 0x80u + 0x1Eu, 0x0000u);   /* ring pen */
    for (unsigned p = 0; p < (unsigned)ENG_WS_EXT_MAX; p++) {
        int wid, k, x;
        unsigned rowi;
        const char *nm;
        if (p >= POS_CLONE0 && !clone_cell_on(p)) continue;
        if (p >= 10u && !ext_on()) break;
        if (p >= POS_CLONE0 && (clone_col_x(p) - cs.cam_x <= -80 || clone_col_x(p) - cs.cam_x >= 336))
            continue;                  /* off the window (the ext_draw gate) */
        wid = (int)pos_wid(p);
        k = eng_palsel_get(wid);
        if (p >= POS_CLONE0)      { x = clone_col_x(p);              rowi = p & 1u; }
        else if (p >= 10u)        { x = LOD_CELL_X;                  rowi = p - 10u; }
        else                      { x = 0x160 + (int)(p % 5u) * 0x60; rowi = p / 5u; }
        x -= cs.cam_x;
        if (k > 0 && nblk < SWATCH_MAX) {
            /* the INDICATOR: an 8x8 block of the outfit's swatch colour
             * at the top-left of the portrait, over the cell art */
            m68k_write_memory_16(PAL_SPR + SWATCH_BANK * 0x80u
                                 + (unsigned)(nblk + 1) * 2u, palsel_swatch(wid));
            emit_block(slot, x + 4, (rowi ? 136 : 40) + 4,
                       SWATCH_TILE0 + (unsigned)nblk);
            nblk++;
        }
        if (k <= 0 || !(nm = eng_palsel_name(wid, k))) continue;
        if (p < 10u) continue;         /* stock cells: no under-cell tag (it
                                          collided with the tilemap names) */
        if (x <= -80 || x >= 336) continue;
        fg0_text(rowi ? 28u : 16u, x / 8, nm, 1);
    }
}

/* Runs after the compose/palette load each frame: the window follows
 * the page scroll, the nP cursor sprites are emitted (0x27B8 per slot,
 * 0x2836 flush) and the highlight pens are re-asserted. */
static void cs_draw(const eng_state *st)
{
    unsigned slot = 0;
    (void)st;
    memset(wf.spriteram, 0, WF_SPRRAM_SIZE);
    /* Only unpicked slots reach 0x27B8; 0x2836 then clears the enable
     * byte of every record past this frame's count ($1C19B2 vs last
     * frame's $1C19B4, 0x28C8..0x28E6), so a picked cursor's frame
     * disappears on the pick and nothing trails the moving one. The
     * exit wait shows no cursors (oracle f1560). */
    if (ext_on() && (cs.phase == 1 || cs.phase == 2))
        ext_draw(&slot);               /* select_extended: LOD cells first, so
                                          the cursor records draw over them */
    if (cs.phase == 1 || cs.phase == 2)
        palsel_tags(&slot);            /* outfit blocks over the cells +
                                          tags under the LOD/clone cells */
    if (cs.phase == 1) {
        for (int i = 0; i < 4; i++) {
            cs_slot *s = &cs.slot[i];
            if (!s->active || s->picked) continue;
            eng_sprite_emit_pose(CURSOR_ROW, s->pose, s->sx, s->sy, -1, &slot);
        }
    }
    memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);
    for (int i = 0; i < 4; i++)
        if (cs.slot[i].pen_addr)
            m68k_write_memory_16(cs.slot[i].pen_addr, cs.slot[i].pen_val);
}

static const eng_scene_ops cs_ops = { cs_begin, cs_update, cs_draw };

void eng_charselect_register(void)
{
    eng_scene_register(ENG_SCENE_CHARSELECT, &cs_ops);
}
