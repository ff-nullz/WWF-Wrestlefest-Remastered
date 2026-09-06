/* The BEST-wrestler ranking table (leaderboard) — transcription of the
 * ROM's ranking subsystem:
 *
 *   0x6F6    jsr 0x6D82  power-on only (reset path falls into 0x6FC):
 *            copy the ROM default tables 0x6DB0 (tag) / 0x6DF2 (rumble)
 *            into work RAM $1C00C2 / $1C0104.  Every later re-entry to
 *            the attract is a jmp 0x6FC, so the table lives across games
 *            within a session (no NVRAM).
 *   entry    11 rows x 6 bytes {score, ini0, ini1, ini2, id, partner-id}.
 *            Tag score = the campaign wins byte $1C0165 (0x1A7E addi.b),
 *            shown +1 (0x6A7E).  Rumble score = the object's +0xC4
 *            elimination count (0x11284/0x1B558/0x1B96C/0x200E8 addq),
 *            zeroed when >= 0x64 (0x67D2), shown as BCD (0x2790).
 *   0x69C8   the ranking PAGE: scene word 3, scroll (0x140, 0x600), text
 *            set 0, rows 0x6A26 + names 0xA4D4 + labels 0x7874, palette
 *            fade in 0x26844.  Shown in the attract demo rotation before
 *            segments 1 (tag table) and 2 (rumble table, $1C0161=1 from
 *            0xB38) for 0x100 frames (0xB4C..0xB5C), and at game over.
 *   0x65C0   game over (0x19E2 / 0x1B42 / 0x1DC4, before jmp 0x6FC):
 *            insert every seated player object (0x7720 test via the
 *            $1C0092 pointers) with 0x67B2, bubble-sort 0x683E, find the
 *            fresh rows 0x68AC ({0x5B,0x5C,seat} marker -> $1C00BA), and
 *            when anyone ranked run the NAME ENTRY on the ranking page:
 *            sound 0x310E, letters $1C00A2 start 0x31, timeout $1C169C =
 *            0x700 frames, per-frame 0x6B6E (letter draw + wheel) and
 *            0x68F6 (row blink), commit on button edge (0x6C80), stored
 *            initial = wheel code + 0x10 (0x6C74: ASCII 'A'..'Z' / 0x5B
 *            blank).  All done / timeout -> final redraw 0x6A26, fade
 *            out 0x264E2, back to the caller (attract 0x6FC).
 *
 * Engine notes (TODO EXACT):
 *  - the blink 0x68F6 saves/restores the row's VRAM longs through a
 *    $1C1CD8 buffer; the engine redraws the table each frame instead and
 *    blanks the row during the dark phase (same 16-frame period, dark on
 *    counter nibble 7..0xE).
 *  - 0x65C0's 0x65C0..0x664E seat-merge for the 2-seat cabinet dip
 *    ($1C0066 bit3) is not modelled; the engine inserts per engine seat.
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
static const tbl_def rank_tables[] = {
    { "rank_defaults_tag",    "base/rank", 0x6DB0, 11 * 6, TK_U8, 6,
      "0x6D82 power-on ranking table (tag): 11 x {wins, 3 ASCII initials, wrestler id, partner id} -> $1C00C2" },
    { "rank_defaults_rumble", "base/rank", 0x6DF2, 11 * 6, TK_U8, 6,
      "0x6D82 power-on ranking table (rumble): 11 x {eliminations, 3 ASCII initials, wrestler id, unused} -> $1C0104" },
    { "rank_blink_rows",      "base/rank", 0x69C2, 3 * 2, TK_U16, 1,
      "0x68F6 name-entry blink: FG0 byte offset of the top line of big row 0..2 (three 0x24-long lines, +0xFC apart)" },
    { "rank_score_offs",      "base/rank", 0x6B58, 11 * 2, TK_U16, 1,
      "0x6A3E score column: FG0 byte offset per rank row 0..10 (+8 in the rumble table, 0x6A64); digits tile 0xF2EB+n (0x6AB6)" },
    { "rank_init_offs",       "base/rank", 0x6B42, 11 * 2, TK_U16, 1,
      "0x6AD0 initials columns: FG0 byte offset per rank row 0..10; cell stride 0xC (rows 0-2, 2x2 font 0x158) or 8 (small font)" },
    { "rank_small_names_tag", "base/rank", 0xA608, 12 * 2, TK_U16, 1,
      "0xA5BE small ranking rows (tag): first FG0 tile word per wrestler id, run of 5 cells (11 when id >= 0xA, 0xA5D0)" },
    { "rank_small_names_rum", "base/rank", 0xA620, 13 * 4, TK_U16, 2,
      "0xA5DE small ranking rows (rumble): {first tile word, count-1} per wrestler id 0..0xB, entry 0xC for id >= 0xA (0xA5A8)" },
    { "rank_label_rows",      "base/rank", 0x7956, 17 * 6, TK_U16, 3,
      "0x78CC ranking page labels: {FG0 byte offset, row stride, row count-1} per label 0..0x10 (tag draws 0..0xB, rumble 0..9 + 0xC..0x10, 0x7884)" },
    { "rank_label_glyphs",    "base/rank", 0x79BC, 17 * 4, TK_U8, 4,
      "0x78F4 ranking page labels: {tile word hi, lo, second-row tile delta, glyph count-1}; tile b15 = draw the +0x100 row too (0x7912); attr |= 0xF0 (0x794C)" },
};
TBL_REGISTER(rank_tables)

#define ROWS            11
#define ROWSZ           6
#define CH_FIRST        0x31u     /* 0x66CA / 0x6C00: wheel start ('A') */
#define CH_BLANK        0x4Bu     /* 0x6C28: wheel blank; 0x6D0C erases */
#define CH_STORE_OFF    0x10u     /* 0x6C74: stored initial = wheel + 0x10 */
#define MK_1            0x5Bu     /* 0x681C fresh-entry marker bytes */
#define MK_2            0x5Cu
#define BIGFONT_BASE    0x158u    /* 0x6D42: tile = (ch - 0x31) * 4 + 0x158 */
#define DIGIT_TILE      0xF2EBu   /* 0x6AB6: score digit tile word 0xF2EB + n */
#define NAME_BIG_A      0x378u    /* 0xA504 big-row name plates (+0xC rumble) */
#define NAME_SMALL_A    0xF64u    /* 0xA54E small-row names (+0xC rumble) */
#define NAME_SMALL_B    0xF7Cu    /* 0xA598 small-row partner name */
#define SEP_LONG        0xFFA2FFFBu /* 0xA590 "&" cell between the pair */
#define RANKNUM_A       0xF34u    /* 0x78A6 small rank numbers 4..11 */
#define RANKNUM_TILE    0x2DCu    /* 0x78AC, 8 rows at +0x200 */
#define PAGE_CAM_X      0x140     /* 0x69D0 */
#define PAGE_CAM_Y      0x600     /* 0x69D8 */
#define PAGE_SCENE      3         /* 0x69E0 */
#define ENTRY_TIMEOUT   0x700u    /* 0x66F0 $1C169C */
#define ENTRY_MUSIC     0x310Eu   /* 0x66B8 */
#define REPEAT_FRAMES   8         /* 0x6BE6 / 0x6C0E $1C00AA */

/* $1C00C2 / $1C0104 work-RAM tables ([0] = tag, [1] = rumble). */
static uint8_t tab[2][ROWS * ROWSZ];
static int have;

/* Rumble elimination scores (+0xC4 per object, 0x11284 etc.). */
static unsigned elims[ENG_MAX_OBJS];

/* Name entry state (0x65C0 locals). */
static struct {
    int      pending;              /* rows inserted, entry screen owed */
    int      rumble;               /* $1C0161 bit0 at game over */
    int      rank[4];              /* $1C00BA: rank per seat, -1 = none/done */
    unsigned letter[4];            /* $1C00A2 */
    unsigned cursor[4];            /* $1C00B2 */
    unsigned rept[4];              /* $1C00AA */
    unsigned prev[4];              /* $1C0146 button edge state */
    unsigned blink[4];             /* $1C1CD4 per-seat counters */
    unsigned timeout;              /* $1C169C */
} ent;

static void ck(void)
{
    if (have) return;
    have = 1;
    for (unsigned i = 0; i < ROWS * ROWSZ; i++) {      /* 0x6D82 word copy */
        tab[0][i] = tbl8(TBL(rank_defaults_tag), i);
        tab[1][i] = tbl8(TBL(rank_defaults_rumble), i);
    }
}

void eng_rank_reset(void) { have = 0; ck(); memset(elims, 0, sizeof elims); }

void eng_rank_rumble_elim(const eng_state *st, int victim)
{
    /* 0x11284 / 0x1B558 / 0x1B96C / 0x200E8: addq.w #1,(+0xC4) on the
     * man the over-the-top throw belongs to (anim.c 0x1539 elimination
     * stamp; the holder is the victim's +0x26 partner link). */
    const eng_obj *o = &st->obj[victim];
    if (o->partner >= 0 && o->partner < ENG_MAX_OBJS)
        elims[o->partner]++;
}
void eng_rank_elims_clear(void) { memset(elims, 0, sizeof elims); }

/* ---- FG0 writers -------------------------------------------------- */
static void put16(unsigned off, unsigned w)            /* 0x6D76 / 0x7940 shape */
{
    if (off + 4 > WF_FG0RAM_SIZE) return;
    wf.fg0_videoram[off + 1] = (uint8_t)(w & 0xFF);
    wf.fg0_videoram[off + 3] = (uint8_t)(w >> 8);
}
static void put_long(unsigned off, uint32_t l)         /* raw long writes (0xA590) */
{
    if (off + 4 > WF_FG0RAM_SIZE) return;
    wf.fg0_videoram[off]     = (uint8_t)(l >> 24);
    wf.fg0_videoram[off + 1] = (uint8_t)(l >> 16);
    wf.fg0_videoram[off + 2] = (uint8_t)(l >> 8);
    wf.fg0_videoram[off + 3] = (uint8_t)l;
}
static void clr_cells(unsigned off, unsigned n)        /* 0x6992 move.l #0 runs */
{
    for (unsigned i = 0; i < n; i++)
        put_long(off + i * 4, 0);
}

/* 0x6CFA: one initial at `a`.  Rows 0-2 use the 2x2 big font (tiles
 * (ch-0x31)*4 + 0x158 in TL/BL/TR/BR order, 0x6D46..0x6D6E); 0x4B is the
 * blank (0x6D0C clears the 2x2).  Rows 3+ write the char code straight
 * into the tile byte (small FG0 font == the code, 0x6D04). */
static void glyph_char(int r, unsigned a, unsigned ch)
{
    if (ch < CH_FIRST || ch > CH_BLANK)
        ch = CH_BLANK;           /* engine guard: a row abandoned mid-entry keeps
                                    its {0x5B,0x5C,seat} marker bytes (0x68AC) —
                                    the ROM would tile-index junk here. TODO EXACT */
    if (r >= 3) {
        if (a + 4 <= WF_FG0RAM_SIZE)
            wf.fg0_videoram[a + 1] = (uint8_t)(ch == CH_BLANK ? 0 : ch);
        return;
    }
    if (ch == CH_BLANK) {
        put_long(a, 0); put_long(a + 4, 0);
        put_long(a + 0x100, 0); put_long(a + 0x104, 0);
        return;
    }
    {
        unsigned t = (ch - CH_FIRST) * 4u + BIGFONT_BASE;
        put16(a, t);            /* TL */
        put16(a + 0x100, t + 1);/* BL (0x6D62 btst #0 -> +0x100) */
        put16(a + 4, t + 2);    /* TR (0x6D5C suba #0xFC) */
        put16(a + 0x104, t + 3);/* BR */
    }
}

/* 0x2790: binary -> BCD (low byte). */
static unsigned bcd(unsigned v) { return ((v / 10u) << 4) | (v % 10u); }

/* 0x77D8: 3x3 name plate for wrestler `id` at FG0 byte offset `a`.
 * The base-tile table 0x7814 is hud.c's ws_portrait_word. */
static void name_plate(unsigned a, unsigned id)
{
    unsigned w = tbl16(TBL(ws_portrait_word), (id % 12u) * 2u);
    for (unsigned row = 0; row < 3; row++)
        for (unsigned col = 0; col < 3; col++)
            put16(a + row * 0x100u + col * 4u, w++);
}

/* 0xA5BE: small-row name run; returns the offset past the run. */
static unsigned name_run(unsigned a, unsigned id, int rumble)
{
    unsigned w, n;
    if (rumble) {
        unsigned idx = (id >= 0xA) ? 0xCu : id;        /* 0xA5A8 */
        w = tbl16(TBL(rank_small_names_rum), idx * 4u);
        n = tbl16(TBL(rank_small_names_rum), idx * 4u + 2u);
    } else {
        w = tbl16(TBL(rank_small_names_tag), (id % 12u) * 2u);
        n = (id >= 0xA) ? 0xAu : 4u;                   /* 0xA5C8/0xA5D0 */
    }
    for (unsigned i = 0; i <= n; i++)
        put16(a + i * 4u, w + i);
    return a + (n + 1u) * 4u;
}

/* ---- 0x7874: the page labels ("THE BEST WRESTLERS", 1ST..3RD, 4..11) */
static void draw_labels(int rumble)
{
    int r = 0;
    for (;;) {
        unsigned off  = tbl16(TBL(rank_label_rows), (unsigned)r * 6u);
        unsigned strd = tbl16(TBL(rank_label_rows), (unsigned)r * 6u + 2u);
        unsigned rows = tbl16(TBL(rank_label_rows), (unsigned)r * 6u + 4u);
        unsigned tile = ((unsigned)tbl8(TBL(rank_label_glyphs), (unsigned)r * 4u) << 8)
                      | tbl8(TBL(rank_label_glyphs), (unsigned)r * 4u + 1u);
        unsigned dlt  = tbl8(TBL(rank_label_glyphs), (unsigned)r * 4u + 2u);
        unsigned n    = tbl8(TBL(rank_label_glyphs), (unsigned)r * 4u + 3u);
        for (unsigned j = 0; j <= rows; j++) {
            unsigned a = off + j * strd, t = tile;
            for (unsigned g = 0; g <= n; g++) {
                unsigned hi = ((t >> 8) & 0xFu) | 0xF0u;       /* 0x794C */
                put16(a + g * 4u, (hi << 8) | (t & 0xFFu));
                if (t & 0x8000u) {                             /* 0x7912 two-row glyph */
                    unsigned t2 = t + dlt;
                    unsigned h2 = ((t2 >> 8) & 0xFu) | 0xF0u;
                    put16(a + g * 4u + 0x100u, (h2 << 8) | (t2 & 0xFFu));
                }
                t++;
            }
        }
        /* 0x787C..0x789E: tag stops after 0xB; rumble runs 0..9 then 0xC..0x10 */
        r++;
        if (rumble) {
            if (r == 0xA) r = 0xC;
            if (r >= 0x11) break;
        } else if (r >= 0xC) break;
    }
    for (unsigned k = 0; k < 8; k++)                   /* 0x78A0 rank numbers 4..11 */
        put16(RANKNUM_A + k * 0x200u,
              ((((RANKNUM_TILE + k) >> 8) | 0xF0u) << 8) | ((RANKNUM_TILE + k) & 0xFFu));
}

/* ---- 0x6A26 + 0xA4D4: the 11 table rows.  `vis` bit r = row visible
 * (name-entry blink); a hidden row's lines are blanked (0x68F6). */
static void draw_rows(int rumble, unsigned vis)
{
    uint8_t *t = tab[rumble ? 1 : 0];
    for (int r = 0; r < ROWS; r++) {
        unsigned so = tbl16(TBL(rank_score_offs), (unsigned)r * 2u) + (rumble ? 8u : 0u);
        unsigned io = tbl16(TBL(rank_init_offs), (unsigned)r * 2u);
        unsigned id = t[r * ROWSZ + 4], pid = t[r * ROWSZ + 5];
        if (!(vis & (1u << r))) {
            /* 0x68F6 dark phase: zero the row's lines. */
            if (r < 3) {
                unsigned base = tbl16(TBL(rank_blink_rows), (unsigned)r * 2u);
                for (unsigned l = 0; l < 3; l++)
                    clr_cells(base + l * 0xFCu, 0x24);         /* 0x695E adda #0x6C tail */
            } else
                clr_cells(0xF0Cu + ((unsigned)r - 3u) * 0x200u, 0x24);  /* 0x6928 */
            continue;
        }
        /* score digits (0x6A6A..0x6ACC) */
        {
            unsigned v = t[r * ROWSZ];
            unsigned d = rumble ? bcd(v) : v + 1u;     /* 0x6A7E addq / 0xA692 bsr 0x2790 */
            unsigned lo = d & 0xFu, hi = (d >> 4) & 0xFu;
            if (hi == 0)
                put16(so + 4u, DIGIT_TILE + lo);       /* 0x6AA8 single digit, right cell */
            else {
                put16(so, DIGIT_TILE + hi);
                put16(so + 4u, DIGIT_TILE + lo);
            }
        }
        /* initials (0x6AEC..0x6B24): stored code - 0x10 (0x6AF6 subi) is
         * the font code — tile 0x31 = 'A' (small), big block 0x158 */
        for (unsigned c = 0; c < 3; c++) {
            unsigned ch = t[r * ROWSZ + 1 + c];
            unsigned cell = c * (r < 3 ? 0xCu : 8u);
            if (ch >= CH_STORE_OFF) ch -= CH_STORE_OFF;        /* 0x6AF6 */
            glyph_char(r, io + cell, ch);                      /* 0x6B14 bsr 0x6CFA */
        }
        /* names (0xA4D4) */
        if (r < 3) {
            unsigned a = NAME_BIG_A + (rumble ? 0xCu : 0u) + (unsigned)r * 0x400u;
            name_plate(a, id);
            if (!rumble) name_plate(a + 0x10u, pid);   /* 0xA53E second plate */
        } else {
            unsigned a = NAME_SMALL_A + (rumble ? 0xCu : 0u) + ((unsigned)r - 3u) * 0x200u;
            if (rumble)
                name_run(a, id, 1);
            else {
                unsigned end = name_run(a, id, 0);
                if (id < 0xA) {                        /* 0xA586: long names take the row */
                    put_long(end, SEP_LONG);           /* 0xA590 "&" */
                    name_run(NAME_SMALL_B + ((unsigned)r - 3u) * 0x200u, pid, 0);
                }
            }
        }
    }
}

/* ---- the ranking page (0x69C8) ------------------------------------ */
void eng_rank_page_begin(eng_state *st, int rumble)
{
    ck();
    st->scene = PAGE_SCENE;                            /* 0x69E0 $1C007E */
    st->cam_x = PAGE_CAM_X;                            /* 0x69D0 */
    st->cam_y = PAGE_CAM_Y;
    eng_scene_publish(PAGE_SCENE, 0);                  /* 0x69E8/0x69FC: pal 3, text set 0 */
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);/* 0x69CC 0x1FC0 */
    memset(wf.spriteram, 0, sizeof wf.spriteram);      /* 0x6A18 0x1FDE */
    memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    draw_rows(rumble, 0x7FFu);                         /* 0x6A1C bsr 0x6A26 */
    draw_labels(rumble);                               /* 0x6B3C bsr 0x7874 */
}

void eng_rank_page_frame(int rumble)
{
    draw_rows(rumble, 0x7FFu);
}

/* ---- 0x67B2 insert + 0x683E sort ---------------------------------- */
static int rank_insert(int rumble, unsigned score, unsigned seat,
                       unsigned id, unsigned partner)
{
    uint8_t *t = tab[rumble ? 1 : 0];
    uint8_t *last = t + (ROWS - 1) * ROWSZ;
    int moved;
    if (rumble && score >= 0x64u) score = 0;           /* 0x67D2 */
    if ((score & 0xFFu) < last[0]) return 0;           /* 0x67DA bcs 0x6838 */
    if (!rumble) last[5] = (uint8_t)partner;           /* 0x67EC..0x6812 */
    last[0] = (uint8_t)score;                          /* 0x6818 */
    last[1] = MK_1; last[2] = MK_2;                    /* 0x681C/0x6822 */
    last[3] = (uint8_t)seat;                           /* 0x6828 */
    last[4] = (uint8_t)id;                             /* 0x682C */
    do {                                               /* 0x683E bubble */
        moved = 0;
        for (int k = ROWS - 2; k >= 0; k--) {
            uint8_t *a = t + k * ROWSZ, *b = a + ROWSZ;
            int swap = 0;
            if (a[0] == b[0])
                swap = (b[2] == MK_2 && a[2] != MK_2); /* 0x686E fresh rises */
            else if (a[0] < b[0]) swap = 1;            /* 0x6880 bcc */
            if (swap) {
                uint8_t tmp[ROWSZ];
                memcpy(tmp, b, ROWSZ); memcpy(b, a, ROWSZ); memcpy(a, tmp, ROWSZ);
                moved = 1;
            }
        }
    } while (moved);
    return 1;
}

/* ---- 0x65C0 head: insert the seated players, find their rows ------ */
int eng_rank_gameover_arm(eng_state *st, int rumble)
{
    int any = 0;
    ck();
    memset(&ent, 0, sizeof ent);
    ent.rumble = rumble ? 1 : 0;
    for (int p = 0; p < 4; p++) ent.rank[p] = -1;      /* 0x664E $1C00BA = -1 */
    for (int i = 0; i < ENG_MAX_OBJS && i < 4; i++) {  /* 0x6662 the $1C0092 seats */
        const eng_obj *o = &st->obj[i];
        unsigned seat, score;
        if (!o->active || o->input < 0) continue;      /* CPU / empty seat */
        if (!(eng_seated & (1 << o->input))) continue;
        seat = (unsigned)o->input;
        score = rumble ? elims[i]                      /* 0x67CE (+0xC4,A0) */
                       : eng_camp_played();            /* 0x67B8 $1C0164 word */
        if (rank_insert(rumble, score, seat, (unsigned)(eng_ws_base(o->wrestler) & 0xFF),
                        (unsigned)(eng_ws_base(st->obj[i ^ 1].wrestler) & 0xFF)))   /* 0x67F2 partner +0x57;
                                                          clones score as their BASE (the table's
                                                          id bytes drive ROM portrait/name rows) */
            ;                                          /* sort ran (0x667E) */
    }
    {                                                  /* 0x68AC find fresh rows */
        uint8_t *t = tab[ent.rumble];
        for (int r = 0; r < ROWS; r++) {
            if (t[r * ROWSZ + 2] != MK_2) continue;
            {
                unsigned seat = t[r * ROWSZ + 3] & 3u;
                ent.rank[seat] = r;                    /* $1C00BA[seat] */
                any = 1;
            }
            t[r * ROWSZ + 1] = MK_1;                   /* 0x68E0 5B 5B */
            t[r * ROWSZ + 2] = MK_1;
        }
    }
    if (!any) return 0;                                /* 0x66B0 beq 0x6792 */
    for (int p = 0; p < 4; p++) {                      /* 0x66C2 init the wheels */
        ent.letter[p] = CH_FIRST;
        ent.cursor[p] = 0;
        ent.rept[p] = 0;
    }
    ent.timeout = ENTRY_TIMEOUT;                       /* 0x66F0 */
    ent.pending = 1;
    if (getenv("WF_DBGSEL"))
        fprintf(stderr, "rank: entry armed (rumble %d) ranks %d/%d/%d/%d\n",
                ent.rumble, ent.rank[0], ent.rank[1], ent.rank[2], ent.rank[3]);
    return 1;
}

int eng_rank_entry_pending(void) { return ent.pending; }
int eng_rank_entry_rumble(void)  { return ent.rumble; }

/* One frame of the 0x66FE loop.  Returns 1 while the entry runs. */
int eng_rank_entry_frame(eng_state *st)
{
    unsigned vis = 0x7FFu;
    int busy = 0;
    if (!ent.pending) return 0;
    for (int p = 3; p >= 0; p--) {                     /* 0x66FE moveq #3 loop */
        int r = ent.rank[p];
        unsigned ph;
        if (r < 0) continue;
        busy = 1;
        ph = ent.blink[p] & 0xFu;                      /* pre-increment value */
        if (ph >= 7 && ph <= 0xE)                      /* 0x6976/0x697E dark phase */
            vis &= ~(1u << r);
        if (!(ent.blink[p] & 8u)) {                    /* 0x671C btst #3 gates 0x6B6E */
            unsigned in = st->inputs[p & 3];
            unsigned nib = in & 0xFu;
            if (nib == 1) {                            /* 0x6BDA RIGHT: next letter */
                if (++ent.rept[p] >= REPEAT_FRAMES) {
                    ent.rept[p] = 0;
                    if (++ent.letter[p] >= CH_BLANK) ent.letter[p] = CH_FIRST;   /* 0x6BF8 */
                }
            } else if (nib == 2) {                     /* 0x6C08 LEFT: previous */
                if (++ent.rept[p] >= REPEAT_FRAMES) {
                    ent.rept[p] = 0;
                    if (--ent.letter[p] == 0x30u) ent.letter[p] = CH_BLANK;      /* 0x6C20 */
                }
            }
            /* 0x6C2E..0x6C78: the live letter lands in the table each frame */
            if (ent.cursor[p] < 3)
                tab[ent.rumble][r * ROWSZ + 1 + ent.cursor[p]] =
                    (uint8_t)(ent.letter[p] + CH_STORE_OFF);
            /* 0x6C80: commit on a button edge (ignored while R/L held) */
            if (!(in & 3u)) {
                unsigned b = (in >> 4) & 3u;           /* 0x6C9C andi #0x30, lsr */
                unsigned edge = b & ~ent.prev[p];
                ent.prev[p] = b;
                if (edge) {
                    ent.letter[p] = CH_FIRST;          /* 0x6CC6 */
                    if (++ent.cursor[p] >= 3)
                        ent.rank[p] = -1;              /* 0x6CDA $1C00BA = -1 */
                }
            } else
                ent.prev[p] = (in >> 4) & 3u;
        }
        ent.blink[p]++;                                /* 0x69BA $1C1CD4 */
    }
    if (ent.timeout && --ent.timeout == 0)             /* 0x6748 */
        for (int p = 0; p < 4; p++) ent.rank[p] = -1;  /* 0x6752 */
    {
        int done = 1;                                  /* 0x6794 all -1 -> done */
        for (int p = 0; p < 4; p++) if (ent.rank[p] >= 0) done = 0;
        if (done || !busy) {
            ent.pending = 0;
            draw_rows(ent.rumble, 0x7FFu);             /* 0x676A final redraw */
            return 0;
        }
    }
    draw_rows(ent.rumble, vis);
    /* the wheel letter on top of the row (0x6B6E, raw wheel code) */
    for (int p = 0; p < 4; p++) {
        int r = ent.rank[p];
        if (r < 0 || !(vis & (1u << r)) || ent.cursor[p] >= 3) continue;
        {
            unsigned io = tbl16(TBL(rank_init_offs), (unsigned)r * 2u);
            unsigned cell = ent.cursor[p] * (r < 3 ? 0xCu : 8u);
            if (r < 3) glyph_char(r, io + cell, ent.letter[p]);
            else if (io + cell + 4 <= WF_FG0RAM_SIZE)
                wf.fg0_videoram[io + cell + 1] = (uint8_t)ent.letter[p];  /* 0x6D04 */
        }
    }
    return 1;
}
