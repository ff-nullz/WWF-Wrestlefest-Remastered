/* Stock in-match HUD + clock — transcription of 0x7506/0x7548/0x75EE/
 * 0x76AA/0x776C/0x77D8 (energy gauges, labels, portraits) and 0x262D2
 * (match clock). Spec: docs/engine-specs/hud-rules.md. FG0 cell (r,c) =
 * 0xC0000 + r*0x100 + c*4; byte +1 = tile lo, +3 = pal<<4 | tile hi. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wf.h"
#include "engine.h"
#include "scene.h"
#include "tbl.h"
#include "credit.h"
extern int eng_dbgsel;

/* Tables this file owns (docs/adr-001-data-formats.md; exported by
 * `wfengine --export-all`, read back from the pak). Names per ADR rule 4. */
static const tbl_def hud_tables[] = {
    { "mash_overlay_rows",     "base/hud", 0x8A7E, 13 * 4, TK_U16, 2,
      "0x899C get-up overlay: per row {count<<8|first tile} for frame 0 and frame 1 (0x8A0C)" },
    { "mash_overlay_row_step", "base/hud", 0x8AB2, 13 * 4, TK_S32, 1,
      "0x899C get-up overlay: signed FG0 byte offset added after each row (+0x100 implicit, 0x8A58)" },
    { "powerup_flash_frames",  "base/hud", 0x8974, 4 * 4,  TK_U32, 1,
      "0x88A2 POWER-UP flash: per frame the FG0 cell long {FF,tile,00,pal|hi} (0x8906); row 2 = tile+6" },
    { "hud_slot_base",         "base/hud", 0x76A2, 4 * 2,  TK_U16, 1,
      "0x76A2 per slot the FG0 byte offset of its 9x4 HUD block (gauge 0x75EE, labels 0x776C, portrait 0x77D8, flash 0x8964, prompts 0xC710)" },
    { "hud_label_tiles",       "base/hud", 0x77D0, 4 * 2,  TK_U16, 1,
      "0x77D0 first tile of the 3-cell nP label per port (0x776C)" },
    { "ws_portrait_word",      "wrestler", 0x7814, 12 * 2, TK_U16, 1,
      "0x7814 per wrestler id the portrait cell word pal<<12|first tile of the 3x3 HUD portrait (0x77D8)" },
    { "blit_record_ptrs",      "base/hud", 0x25204, 103 * 4, TK_U32, 1,
      "0x2503C big FG0 blit: long -> record per id 0..0x66 (text plates, CREDIT, names, prompts; 0x25040)" },
    { "blit_records",          "base/hud", 0x253A0, 0x26122 - 0x253A0, TK_U8, 0,
      "0x2503C blit records {mode, pal, FG0 off hi, lo, script: 00 end 01 newline 02 xx skip 03 hh ll seek, >=8 glyph} (0x2508E); ends at code 0x26122" },
};
TBL_REGISTER(hud_tables)

static void fg0_put(unsigned off, unsigned tile, unsigned pal)  /* 0x76E8 */
{
    if (off + 4 > WF_FG0RAM_SIZE) return;
    wf.fg0_videoram[off + 1] = (uint8_t)tile;
    wf.fg0_videoram[off + 3] = (uint8_t)((pal << 4) | ((tile >> 8) & 0x0Fu));
}
#define CELL(r, c) ((unsigned)((r) * 0x100 + (c) * 4))

/* The BIG AISLE NAME FACE (blit mode 1, 0x250DC): 8x16 glyphs, tile
 * (c-0x10)*2+0x30 top / +1 bottom — the records feed it ASCII, so ANY
 * string renders in the genuine walkout font (user 2026-08-24:
 * "reproduce that font ... for new wrestlers names"). */
void eng_fg0_bigtext(unsigned row, unsigned col, const char *txt, unsigned pal)
{
    unsigned a = row * 0x100u + col * 4u;
    for (; *txt && a + 0x104u <= WF_FG0RAM_SIZE; txt++, a += 4) {
        unsigned c = (unsigned char)*txt;
        unsigned t = (c - 0x10u) * 2u + 0x30u;
        if (c == ' ') {
            memset(wf.fg0_videoram + a, 0, 4);
            memset(wf.fg0_videoram + a + 0x100u, 0, 4);
            continue;
        }
        fg0_put(a, t, pal);
        fg0_put(a + 0x100u, t + 1u, pal);
    }
}

/* Decode a mode-1 blit record's glyph stream back to ASCII (the records
 * store the characters verbatim) — the wrestler's plate IS his name. */
int eng_blit_text(unsigned id, char *out, int cap)
{
    uint32_t rec = tbl32(TBL(blit_record_ptrs), (id & 0x7FFFu) * 4u);
    uint32_t p = rec + 4;
    int n = 0;
    if (tbl_ra8(rec) != 1) return 0;              /* mode-1 plates only */
    for (;;) {
        unsigned c = tbl_ra8(p++);
        if (c == 0 || n + 1 >= cap) break;
        if (c == 1) break;                        /* one line */
        if (c == 2) { p++; continue; }
        if (c == 3) { p += 2; continue; }
        if (c < 8) continue;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

/* 0x76A2 slot block base, 0x77D0 label tile, 0x7814 portrait word: the
 * ROM tables above, read through the data layer. */
static unsigned hud_base(int s)   { return tbl16(TBL(hud_slot_base), (unsigned)s * 2u); }
static unsigned label_tile(int p) { return tbl16(TBL(hud_label_tiles), (unsigned)p * 2u); }
static unsigned portrait_w(int w) { return tbl16(TBL(ws_portrait_word), (unsigned)w * 2u); }

static void gauge_cell(unsigned a, int slot, unsigned fill)        /* 0x76AA */
{
    unsigned step = 7, t = 0xB33;
    if (fill) { step = 0xB; t = 0xAD9; if (slot & 2) { step = 0xE; t = 0xAF2; } }
    t += fill;
    fg0_put(a, t, 1);
    fg0_put(a + 0x100, t + step, 1);
}

static void gauge_draw(const eng_obj *o, int slot)                 /* 0x75EE */
{
    unsigned a = hud_base(slot) + 0x20C;
    unsigned e, q, r;
    int partial = 1;
    for (unsigned i = 0; i < 6; i++) gauge_cell(a + i * 4, slot, 0);
    e = (unsigned)o->hp_disp & 0xFFu;
    if (!e) return;
    q = e / 16; r = e % 16;
    if (q >= 6) { q = 6; partial = 0; }
    for (unsigned i = 0; i < q; i++) gauge_cell(a + i * 4, slot, 8);
    if (!partial || !r) return;
    gauge_cell(a + q * 4, slot, r / 2);
}

/* A HUD row is a "CPU row" (empty seat in stock, where CPU men live in the
 * CPU rows) when its man is CPU-flagged or is the autopilot partner of a
 * CPU man; the human team's autopilot partner is a live non-CPU row. */
static int row_is_cpu(const eng_state *st, int s)
{
    const eng_obj *o = &st->obj[s];
    if (o->cpu) return 1;
    if (!(o->role & RF_PAD) && o->teammate >= 0 && st->obj[o->teammate].cpu) return 1;
    return 0;
}
/* 0x77D0 index = the row's PORT (+0x8A): a seated player's own port; the
 * autopilot partner carries his team's port (MAME: "1P" over the partner). */
static int label_port(const eng_state *st, int s)
{
    const eng_obj *o = &st->obj[s];
    if (o->input >= 0) return o->input & 3;
    if (o->teammate >= 0 && st->obj[o->teammate].input >= 0) return st->obj[o->teammate].input & 3;
    return s & 3;
}
static void labels(const eng_state *st)                            /* 0x776C */
{
    for (int s = 0; s < 4; s++) {
        if (!st->obj[s].active) continue;
        if (!st->obj[s].active || (row_is_cpu(st, s) && !eng_mod_rule(MODR_CPU_METERS))) continue;   /* 0x776C: live non-CPU rows (the autopilot partner too)
                                                  (CPU men get no nP tag) */
        for (unsigned i = 0; i < 3; i++)       /* 0x77D0 indexed by the PORT (+0x8A & 0xF):
                                                  $140020 = 1P, $140022 = 2P, whichever slot */
            fg0_put(hud_base(s) + i * 4, label_tile(label_port(st, s)) + i, 1);
    }
}

/* CLONE HUD FACE (user 2026-08-25): the 3x3 FG0 portrait derived from the
 * clone's 80x80 select cell — box-downscaled to 24x24, quantized to the
 * BASE's FG0 portrait palette (its bank nibble), written into a run of
 * FREE FG0 tiles (the fg0 sheet has four blank runs >= 9 tiles: 2759+,
 * 448+, 198+, 219+ — up to four clones on the HUD at once). Returns the
 * portrait word (pal<<12 | tile0) or 0 = use the base's. */
int eng_cs_clone_cell(int clone, uint8_t *pens80, uint16_t *pal);
uint32_t wf_video_palette_rgb(unsigned index);
void wf_video_fg0_write(unsigned code, const uint8_t *in);
static int face_pending[4];
static unsigned clone_face_word(int clone, unsigned base_word)
{
    static const unsigned runs[4] = { 2759u, 448u, 198u, 219u };
    static int run_of[32]; static unsigned word_of[32];
    static uint8_t used[4];
    int s = clone - 12;
    if (s < 0 || s >= 32) return 0;
    if (word_of[s]) return word_of[s];
    if (run_of[s] == 0) {              /* allocate a run once per clone */
        int r;
        for (r = 0; r < 4 && used[r]; r++) ;
        if (r >= 4) { run_of[s] = -1; return 0; }
        used[r] = 1; run_of[s] = r + 1;
    }
    if (run_of[s] < 0) return 0;
    {
        static uint8_t pens80[80 * 80]; uint16_t cpal[16];
        unsigned bank = base_word >> 12, tile0 = runs[run_of[s] - 1];
        int lit = 0;
        for (int pen = 1; pen < 16; pen++) if (wf_video_palette_rgb(bank * 16u + (unsigned)pen) & 0x00FFFFFFu) lit++;
        if (!lit) {                    /* the FG0 bank is installed in the render path: not yet on
                                          the HUD's first tick — retry per frame (face_pending) */
            return 0;
        }
        if (!eng_cs_clone_cell(clone, pens80, cpal)) { if (eng_dbgsel) fprintf(stderr, "hud: clone %d has no select cell - base face\n", clone); run_of[s] = -1; return 0; }
        if (eng_dbgsel) fprintf(stderr, "hud: clone %d face -> fg0 tiles %u.. bank %u\n", clone, tile0, bank);
        for (int ty = 0; ty < 3; ty++) for (int tx = 0; tx < 3; tx++) {
            uint8_t tile[64];
            for (int py = 0; py < 8; py++) for (int px = 0; px < 8; px++) {
                /* box 80 -> 24: each HUD pixel averages a 10/3-px cell of the select portrait */
                int X0 = (tx * 8 + px) * 80 / 24, X1 = (tx * 8 + px + 1) * 80 / 24;
                int Y0 = (ty * 8 + py) * 80 / 24, Y1 = (ty * 8 + py + 1) * 80 / 24;
                long r = 0, g = 0, b = 0, n = 0; int best = 1, bestd = 1 << 30;
                for (int y = Y0; y < Y1; y++) for (int x = X0; x < X1; x++) {
                    unsigned w2 = cpal[pens80[y * 80 + x]];
                    r += (w2 & 0xF) * 17; g += ((w2 >> 4) & 0xF) * 17; b += ((w2 >> 8) & 0xF) * 17; n++;
                }
                if (n) { r /= n; g /= n; b /= n; }
                for (int pen = 1; pen < 16; pen++) {   /* nearest pen of the base's FG0 bank */
                    uint32_t c = wf_video_palette_rgb(bank * 16u + (unsigned)pen);
                    int dr = (int)((c >> 16) & 0xFF) - (int)r, dg = (int)((c >> 8) & 0xFF) - (int)g, db = (int)(c & 0xFF) - (int)b;
                    int d = dr * dr + dg * dg + db * db;
                    if (d < bestd) { bestd = d; best = pen; }
                }
                tile[py * 8 + px] = (uint8_t)best;
            }
            wf_video_fg0_write(tile0 + (unsigned)(ty * 3 + tx), tile);
        }
        word_of[s] = (bank << 12) | tile0;
        return word_of[s];
    }
}
static void portrait(const eng_obj *o, int slot)                   /* 0x77D8 */
{
    unsigned pid = (unsigned)eng_ws_base(o->wrestler);   /* clones: the base's portrait */
    unsigned w = portrait_w(pid == 5 ? 4u : pid % 12);
    face_pending[slot & 3] = 0;
    if (o->wrestler >= 12 && o->wrestler < ENG_WS_EXT_MAX) {   /* a clone with a select cell: his own face */
        unsigned cw = clone_face_word(o->wrestler, w);
        if (cw) w = cw;
        else face_pending[slot & 3] = 1;   /* the FG0 bank was not lit yet at HUD init: retry per frame */
    }
    /* ^ LOD share ONE HUD portrait (user 2026-08-24): Animal draws Hawk's.
     *   Stock never draws CPU-row portraits — visible only under the
     *   cpu_energy_meters mod. */
    unsigned a = hud_base(slot) + 0x100;
    for (unsigned r = 0; r < 3; r++)
        for (unsigned c = 0; c < 3; c++) {
            unsigned off = a + r * 0x100 + c * 4, t = w + r * 3 + c;
            if (off + 4 > WF_FG0RAM_SIZE) continue;
            wf.fg0_videoram[off + 1] = (uint8_t)t;            /* raw +1/+3 */
            wf.fg0_videoram[off + 3] = (uint8_t)(t >> 8);     /* pal = top nibble */
        }
}

void eng_hud_tick(eng_state *st)
{
    if (!st->hud_inited) {                                         /* 0x7506 */
        st->hud_inited = 1;
        if (getenv("WF_BLIT"))                     /* harness: draw one blit record */
            eng_blit((unsigned)strtoul(getenv("WF_BLIT"), 0, 0));
        for (int s = 0; s < 4; s++) {
            eng_obj *o = &st->obj[s];
            if (!o->active || (row_is_cpu(st, s) && !eng_mod_rule(MODR_CPU_METERS))) continue;   /* 0x7506/0x782C: live rows not CPU-flagged
                                                     (+0x56 b7) — the autopilot partner gets his
                                                     gauge + portrait (MAME 0021.png); CPU men
                                                     live in the CPU rows in stock = empty seats */
            o->hp_disp = (int16_t)o->hp;
            gauge_draw(o, s);
            portrait(o, s);
        }
        labels(st);
        return;
    }
    if (!(st->frame & 1)) return;                                  /* odd frames */
    if (st->lock16c5) return;                                      /* 0x7548 */
    labels(st);
    for (int s = 0; s < 4; s++)        /* clone faces that could not install at init (bank unlit) */
        if (face_pending[s] && st->obj[s].active) portrait(&st->obj[s], s);
    for (int s = 0; s < 4; s++) {
        eng_obj *o = &st->obj[s];
        int step;
        if (!o->active || (row_is_cpu(st, s) && !eng_mod_rule(MODR_CPU_METERS))) continue;   /* 0x7548: live non-CPU rows */
        o->hp_acc += o->hp_delta; o->hp_delta = 0;
        if (!o->hp_acc) continue;
        step = (o->hp_acc == 1 || o->hp_acc == -1) ? 1 : 2;
        if (o->hp_acc < 0) { o->hp_disp -= step; o->hp_acc += step; }
        else               { o->hp_disp += step; o->hp_acc -= step; }
        if (o->hp_disp < 0) { o->hp_disp = 0; o->hp_acc = 0; }          /* 0x76F8 */
        else if (o->hp_disp >= (int16_t)o->hp_max) { o->hp_disp = (int16_t)o->hp_max; o->hp_acc = 0; }
        gauge_draw(o, s);
    }
}

/* ---- match clock 0x262D2 (even frames) ---- */
static int bcd_dec(uint8_t *v)                 /* sbcd by 1; 0 = borrow */
{
    if (*v == 0) return 0;
    if ((*v & 0x0F) == 0) *v = (uint8_t)((*v - 0x10) | 0x09);
    else (*v)--;
    return 1;
}
static void digit(unsigned col, unsigned d)                        /* 0x2647C */
{
    fg0_put(CELL(29, 7 + col), 0x4A8 + d, 0xE);
    fg0_put(CELL(30, 7 + col), 0x4B3 + d, 0xE);
}
static void clock_draw(const eng_state *st)                        /* 0x2641E */
{
    digit(0, st->clk_min >> 4); digit(1, st->clk_min & 15);
    digit(3, st->clk_sec >> 4); digit(4, st->clk_sec & 15);
}
/* 0x2503C — big FG0 blit by id (bit15 = erase). Record at 0x25204[id]:
 * mode, pal, FG0 byte offset, then the glyph script (00 end, 01 newline,
 * 03 hh ll = new offset, c >= 8 = glyph). Mode 0 = 8x8 tile c-0x10;
 * mode 2 = 16x16 glyph (c-0x10)*4+0x94 in TL/BL/TR/BR order, 0x20 =
 * 4 cleared cells. (match-end.md §4) */
void eng_blit(unsigned id)
{
    unsigned erase = id & 0x8000u;
    uint32_t rec = tbl32(TBL(blit_record_ptrs), (id & 0x7FFFu) * 4u);   /* 0x25040 */
    unsigned mode = tbl_ra8(rec), pal = tbl_ra8(rec + 1);             /* record: blit_records */
    unsigned a = ((unsigned)tbl_ra8(rec + 2) << 8) | tbl_ra8(rec + 3);
    uint32_t p = rec + 4;
    /* 0x250AC per-mode line stride; 0x251DE newline: A1 += stride, the
     * cursor returns to the RECORD's column (A2 = A1), not column 0. */
    static const unsigned stride[4] = { 0x100, 0x200, 0x200, 0x100 };
    unsigned line = stride[mode & 3], col = a, d4 = 0;
    for (;;) {
        unsigned c = tbl_ra8(p++);
        if (c == 0) break;
        if (c == 1) { a += line; col = a; continue; }             /* 0x251DE */
        if (c == 2) { d4 = (unsigned)tbl_ra8(p++) << 8; continue; }   /* 0x251E4 mode-3 attr */
        if (c == 3) { a = ((unsigned)tbl_ra8(p) << 8) | tbl_ra8(p + 1); p += 2; col = a; continue; }
        if (c < 8) continue;
        (void)d4;                                                 /* 0x2519E: mode 3 ORs D4
                                                                     (stock movep quirk, see below) */
        if (mode == 2) {
            unsigned t = (c - 0x10u) * 4u + 0x94u;
            unsigned cells[4] = { col, col + 0x100u, col + 4u, col + 0x104u };
            for (unsigned k = 0; k < 4; k++) {
                if (cells[k] + 4 > WF_FG0RAM_SIZE) continue;
                if (erase || c == 0x20) { wf.fg0_videoram[cells[k] + 1] = 0; wf.fg0_videoram[cells[k] + 3] = 0; }
                else fg0_put(cells[k], t + k, pal);
            }
            col += 8;
        } else if (mode == 1) {                   /* 0x250DC: 8x16 glyph (c-0x10)*2+0x30, TL then BL */
            unsigned t = (c - 0x10u) * 2u + 0x30u;
            if (col + 0x104 <= WF_FG0RAM_SIZE) {
                if (erase) { memset(wf.fg0_videoram + col, 0, 4); memset(wf.fg0_videoram + col + 0x100, 0, 4); }
                else { fg0_put(col, t, pal); fg0_put(col + 0x100, t + 1, pal); }
            }
            col += 4;
        } else {                                  /* mode 0 (3 approximated) */
            if (col + 4 <= WF_FG0RAM_SIZE) {
                if (erase) { wf.fg0_videoram[col + 1] = 0; wf.fg0_videoram[col + 3] = 0; }
                else fg0_put(col, c - 0x10u, pal);
            }
            col += 4;
        }
    }
}

static void time_up(eng_state *st)                                 /* 0x263D8 */
{
    st->sig169e |= 0x10;
    st->clk_min = st->clk_sec = 0;
    st->lock16c5 = 3;
    eng_blit(0x13);                    /* "TIME OVER" */
    for (int i = 0; i < ENG_MAX_OBJS; i++)
        if (st->obj[i].active) {
            st->obj[i].result = 0x8005;                      /* draw */
            st->obj[i].a0flags = 0x80;                       /* 0x26410 +0xA0 */
        }
}

void eng_clock_tick(eng_state *st)
{
    if (st->frame & 1) return;
    if (eng_demo_active()) return;     /* the ATTRACT DEMO shows no clock (MAME
                                          checked, user 2026-08-28) - the TIME
                                          plate collided with the SATURDAY
                                          NIGHT banner */
    if ((st->sig169e & 0x10) || !(st->sig169e & 0x80)) return;
    if (!(st->sig169e & 0x40)) {                                   /* 0x26300 init */
        st->sig169e |= 0x40;
        for (unsigned i = 0; i < 6; i++) {                         /* "TIME" plate */
            fg0_put(CELL(29, 1 + i), 0x49C + i, 0xE);
            fg0_put(CELL(30, 1 + i), 0x4A2 + i, 0xE);
        }
        fg0_put(CELL(29, 9), 0x4B2, 0xE);
        fg0_put(CELL(30, 9), 0x4B2, 0xE);
        st->clk_div = 1; st->clk_sec = 0x01;
        st->clk_min = (uint8_t)eng_mode_rule(MODE_TIME_MIN);       /* 30:01 stock */
        if (0x720 <= WF_PALETTE_SIZE) {        /* $18071C = 0x0FF, $18071E = 0x00F */
            wf.palette[0x71C] = 0x00; wf.palette[0x71D] = 0xFF;
            wf.palette[0x71E] = 0x00; wf.palette[0x71F] = 0x0F;
        }
        /* TODO EXACT: the <=5:00 flash on the divider parity (0x264AA) */
    }
    if (eng_mod_rule(MODR_UNL_TIME) || !eng_mode_rule(MODE_TIME_MIN))
        return;                                    /* mod/mode: the clock never ticks */
    if (--st->clk_div) return;                                     /* 0x26370 */
    st->clk_div = 4;                   /* one tick per 8 frames */
    if (!bcd_dec(&st->clk_sec)) {
        st->clk_sec = 0x59;
        if (!bcd_dec(&st->clk_min)) { time_up(st); return; }
        if (st->clk_min == 0x05) { st->sig169e |= 0x20; eng_sound(0x0B); }
    }
    clock_draw(st);
}

/* 0x899C — the "mash to get up" FG0 overlay. $1C167A b7 is set by 0x10D04
 * every frame a HUMAN body is down/pinned; while it (or last frame's copy
 * $1C167B) is set, 13 rows of tiles from 0x8A7E[row][frame] ({count, first
 * tile}, tiles 0x400+, attr 0xE4 = pal E) are written from $C1008, each
 * row offset by 0x100 + 0x8AB2[row]; the frame toggles every 2 frames
 * ($1C00AA/$1C00B2). The frame after the body rises (b6) writes blanks. */
void eng_mash_overlay(eng_state *st)
{
    int now = st->body_down, prev = st->body_down_prev, clearing;
    unsigned off = 0x1008u;
    st->body_down_prev = (uint8_t)now;                     /* 0x8A68 copy, 0x8A72 clear */
    st->body_down = 0;
    if (!now && !prev) return;
    clearing = !now;                                       /* 0x89C2 bset #6 */
    if (++st->mash_t >= 3) { st->mash_t = 0; st->mash_f ^= 1; }   /* 0x89CA: stock
                                          alternates every 2 frames; slowed ~30%%
                                          (user 2026-08-24: "the get-up button
                                          press animation is too fast") */
    for (unsigned r = 0; r < 13; r++) {                    /* 0x89F4 */
        unsigned w = tbl16(TBL(mash_overlay_rows), r * 4u + (unsigned)st->mash_f * 2u);
        unsigned cnt = w >> 8, tile = w & 0xFFu, a = off;
        for (unsigned k = 0; k < cnt; k++, a += 4u, tile++) {
            if (a + 4 > WF_FG0RAM_SIZE) break;
            if (clearing) { wf.fg0_videoram[a + 1] = 0; wf.fg0_videoram[a + 3] = 0; }   /* 0x8A28 */
            else { wf.fg0_videoram[a + 1] = (uint8_t)tile; wf.fg0_videoram[a + 3] = 0xE4u; }   /* 0x8A30 */
        }
        {
            uint32_t d = tbl32(TBL(mash_overlay_row_step), r * 4u);
            off = (unsigned)((int32_t)off + 0x100 + (int32_t)d);   /* 0x8A48/0x8A5C */
        }
    }
}

/* ---- HUD block prompts, buy-in, POWER-UP flash --------------------------
 * The 9x4-cell HUD block of each slot (0x76A2 base) also carries, right of
 * the "nP" label / above the gauge (cols c0+3.., rows 1-2):
 *   0xC710  per frame, ONE slot ($1C16CE cycles 0..3): the coin/buy-in
 *           prompt by slot state (0x9ACA glyph runs 0x18..0x26), cached
 *           in +0xC2/+0xC3 so a cell is rewritten only when the id changes;
 *   0x88A2  even frames: the POWER-UP flash (+0x33 b5 "on fire"), 4 frames
 *           of a 6x2 tile strip (0x8974) over the same cells, +0xCE b7 owns
 *           the block while it runs;
 *   0x8B3A/0x8BA2 per frame: START buy-in — a seated human's START edge
 *           (+0xA2 b8) takes one coin/credit (0x55A D0=2) and refills him
 *           (and his non-human partner, tag).
 * Cabinet dip $1C0066 & 0xC00: the engine is a TWO-player board (ports
 * $140020/$140022, seats in slots 0/2 tag or 0/1 rumble), so the 0x800
 * branches are taken where the three paths differ (0xC8A2/0xC92A, 0xC9F8,
 * 0xC81E/0xC75E). 0x400 and the 4-player default are TODO EXACT, as is
 * the mid-match JOIN of an empty slot (IRQ3 0x978 -> 0x1F38 select). */

static void banner_at(unsigned off, unsigned id)                   /* 0x9ACA, D7 = 0x10 */
{
    extern void eng_banner_runs(uint32_t a2, unsigned idx, unsigned d7);
    eng_banner_runs(0xC0000u + off, id, 0x10);
}

/* 0x88A2 (even frames): slots 0..3, not CPU. */
void eng_powerup_flash(eng_state *st)
{
    for (int s = 0; s < 4; s++) {
        eng_obj *o = &st->obj[s];
        if (!o->active) continue;              /* (row read regardless in ROM) */
        /* 0x88AA: +0x56 b7 skips.  ENGINE: the CPU side's rows are skipped
         * too (their HUD is INSERT COIN / BUY-IN, stock's b7 covers that
         * team; the engine keeps the CPU's apron man autopilot cpu=0) —
         * unless the cpu_energy_meters mod draws a CPU HUD */
        if (o->cpu || (row_is_cpu(st, s) && !eng_mod_rule(MODR_CPU_METERS))) { o->flash_cf = 0; continue; }
        if (!(o->role & RF_ONFIRE) && !(o->cmb_c6 & 0x40u)) {   /* 0x88B2 ON FIRE / 0x88BA REGAIN POWER armed */
            if (o->flash_ce & 0x80u) {         /* 0x88C2 bclr: hand the block back */
                o->flash_ce &= (uint8_t)~0x80u;
                o->hud_c2 = o->hud_c3 = 0;     /* clr.w +0xC2 -> the prompt redraws */
                if (row_is_cpu(st, s) && eng_mod_rule(MODR_CPU_METERS))
                    o->hud_c2 = 1;             /* mod cpu_energy_meters: no prompt machine paints over
                                          the flash tiles on a CPU row - mark it so the CPU branch
                                          (0xC7FE mod path) wipes the block and redraws the meter
                                          ("POWER UP stays after the flash", user 2026-08-30) */
            }
            o->flash_cf = 0;
            continue;
        }
        {
            unsigned a = hud_base(s) + 0xC;    /* 0x8964: base + 0xC, rows 1 and 2 */
            unsigned f = o->flash_cf & 0x7Fu;
            uint32_t w = tbl32(TBL(powerup_flash_frames), f * 4u);
            unsigned tile = ((w >> 16) & 0xFFu) | ((w & 0xFu) << 8), pal = (w >> 4) & 0xFu;
            o->flash_ce |= 0x80u;
            for (unsigned i = 0; i < 6; i++) { /* 0x8916: row 1 tiles t.., row 2 t+6.. */
                fg0_put(a + i * 4, tile + i, pal);
                fg0_put(a + 0x100 + i * 4, tile + 6 + i, pal);
            }
            if (++o->flash_cc >= 2) {          /* 0x892A: every 2 even frames */
                o->flash_cc = 0;
                if (++o->flash_cf >= 4) o->flash_cf = 0;
            }
        }
    }
}

/* 0xC9BE/0xCA10 (empty slot, credits in), 0xCA88/0xCA58 (empty, none),
 * 0xC892 (+0x22 small PUSH n BUTTON), 0xCAB2 REGAIN POWER, 0xCAE2 INSERT
 * COIN, 0xCB12 BUY-IN, 0xCB42 clear. */
static void prompt_c2(eng_obj *o, int slot, unsigned add, unsigned id)
{
    if (o->hud_c2 == id) return;
    o->hud_c2 = (uint8_t)id;
    banner_at(hud_base(slot) + add, id);
}
static void prompt_c3(eng_obj *o, int slot, unsigned add, unsigned id)
{
    if (o->hud_c3 == id) return;
    o->hud_c3 = (uint8_t)id;
    banner_at(hud_base(slot) + add, id);
}

static int port_bound(const eng_state *st, int p);   /* below, with eng_join_tick */

void eng_buyin_prompt(eng_state *st)                               /* 0xC710 */
{
    int slot, rumble = (st->g161 & 1u) != 0;
    eng_obj *o;
    unsigned d3;

    if (st->lock16c5 || st->rumble_phase) return;                  /* $1C16C4 */
    if (st->hud_slot >= 4) st->hud_slot = 0;                       /* 0xC722 */
    slot = st->hud_slot;
    o = &st->obj[slot];
    if (o->flash_ce & 0x80u) { st->hud_slot++; return; }           /* 0xC744: the flash owns it */
    /* Stock keeps only SEATED players in HUD rows 0-3 (0xCB78); CPU men and
     * the autopilot partner live in the CPU rows, so their HUD block is an
     * EMPTY SEAT there — the big white plates (MAME default dips: 0xC7D0).
     * The engine seats the CPU team / partner in rows 1-3, so a non-human
     * row is treated as that empty seat. Default (4-player) dip path as in
     * the MAME reference; dips 0x400/0x800 TODO EXACT. */
    if (!o->active || (row_is_cpu(st, slot)
                       && !(eng_mod_rule(MODR_CPU_METERS) && o->active))) {   /* 0xC75E empty seat
                                          (mod cpu_energy_meters: a LIVE CPU row keeps its
                                          gauge — no BUY-IN plate over it; the join press
                                          itself still works) */
        int clear = (!rumble && (st->stage == 4 || st->stage == 9))  /* 0xC784/0xC78E */
                 || slot >= eng_join_maxports()      /* Players dip: seat can't join */
                 || port_bound(st, slot);            /* engine: the seat's port is in
                                                        use (stock marks the SEAT via
                                                        +0xA1 b0; the engine's port =
                                                        seat rule makes this the same
                                                        test) — no dead BUY-IN plate */
        if (clear) {                                               /* 0xCB42 */
            unsigned b = hud_base(slot);
            if (b + 0xC + 4 <= WF_FG0RAM_SIZE
                && (wf.fg0_videoram[b + 0xD] || wf.fg0_videoram[b + 0xF]))
                for (unsigned r = 0; r < 4; r++)
                    for (unsigned c = 0; c < 9; c++) {
                        unsigned a = b + r * 0x100 + c * 4;
                        if (a + 4 <= WF_FG0RAM_SIZE) memset(&wf.fg0_videoram[a], 0, 4);
                    }
        } else if (eng_can_afford()) {                             /* 0xC7D0 D0=5 */
            d3 = (unsigned)slot;                                   /* 0xC9CE: +0x1C by HUD slot */
            prompt_c2(o, slot, 0, 0x1C + d3);                      /* big PUSH n BUTTON */
            prompt_c3(o, slot, 0x204, (o->hud_c2 & 1u) ? 0x1A : 0x1B);   /* 0xCA10 BUY-IN */
        } else {
            prompt_c2(o, slot, 0, 0x18);                           /* 0xCA88 INSERT COIN (big) */
            prompt_c3(o, slot, 0x204, 0x19);                       /* 0xCA58 BUY-IN (big) */
        }
        st->hud_slot++;
        return;
    }
    if (row_is_cpu(st, slot) && eng_mod_rule(MODR_CPU_METERS)) {
        /* MOD cpu_energy_meters (user 2026-08-24): a live CPU row shows
         * its ENERGY METER, and the stock pad-less-row plates (0xC7FE
         * small PUSH n BUTTON / INSERT COIN + 0xCB12 BUY-IN) clash with
         * it — the prompt machine stands down for CPU seats. A plate
         * drawn earlier (while the seat was empty) is wiped once, the
         * meter block redrawn under it. */
        if (o->hud_c2 || o->hud_c3) {
            unsigned b = hud_base(slot);
            for (unsigned rr = 0; rr < 4; rr++)
                for (unsigned cc = 0; cc < 9; cc++) {
                    unsigned a = b + rr * 0x100 + cc * 4;
                    if (a + 4 <= WF_FG0RAM_SIZE) memset(&wf.fg0_videoram[a], 0, 4);
                }
            o->hud_c2 = o->hud_c3 = 0;
            gauge_draw(o, slot);
            portrait(o, slot);
        }
        st->hud_slot++;
        return;
    }
    if (!(o->role & RF_PAD)) {                                       /* 0xC7FE live, no pad: the autopilot partner */
        d3 = (unsigned)slot;                                       /* 0xC892 default: port != 0xCB88[slot] -> slot */
        if (eng_can_afford()) prompt_c2(o, slot, 0xC, 0x22 + d3);  /* 0xC834 D0=5 -> small PUSH n BUTTON */
        else                  prompt_c2(o, slot, 0xC, 0x20);       /* 0xCAE2 INSERT COIN */
        prompt_c3(o, slot, 0x10C, 0x21);                           /* 0xCB12 BUY-IN */
        st->hud_slot++;
        return;
    }
    /* 0xC862 a seated human: 0xC892 picks the button number from the port
     * (0xCB88[slot] == +0x8A -> the slot, else its pair 0xCB98). The engine's
     * P2 sits in row 2 on port 2, which stock's 4P table would call "4" —
     * the player's own port number is used instead (deviation, noted). */
    d3 = o->input >= 0 ? (unsigned)(o->input & 3) : (unsigned)slot;
    if (eng_can_afford()) prompt_c2(o, slot, 0xC, 0x22 + d3);      /* 0xC862 D0=2 -> PUSH n BUTTON */
    else                  prompt_c2(o, slot, 0xC, 0x20);           /* 0xCAE2 INSERT COIN */
    prompt_c3(o, slot, 0x10C, 0x26);                               /* 0xCAB2 REGAIN POWER */
    st->hud_slot++;
}

/* ---- 0x18C4 (frame list 0x103E, EVEN frames): the mid-game BUY-IN join.
 * Gate: a live match ($1C007C != 0 at 0x18C4). Non-rumble ($1C0161 b0
 * clear, 0x18CE beq 0x6E34):
 *   0x6E3E: the cabinet dip $1C0066 & 0x800 picks the 2-player path
 *   0x70CC (the join-side mini-menu $1C1690/0x54F8, the $1C09E0/$1C0AEC
 *   shadow-pair promotion 0x6F32); the engine is seated 4-wide, so it
 *   takes the 4-SEAT loop 0x6E5E: per seat (A0 = slot, A1 = port), a
 *   man without the live-player bit (+0xA1 b0, 0x6E82) whose port holds
 *   START (0x6E5E btst #7,(1,A1), active-low) and has no result
 *   (+0xFE, 0x6E72) or verdict (+0xA0, 0x6E7A) pays the BUY-IN price
 *   (0x6E8C jsr 0x55A D0=5 -> SW1:3) and joins:
 *   - 0x6E9A the LEGAL-man handling: if the scanned man is his team's
 *     legal man (+0x33 b0), b1 is set on BOTH (0x6EA2/0x6EB0) and the
 *     pad (+0x8A := the port, 0x6EAC), the autopilot clear (+0x56 b6,
 *     0x6EB6) and the rest of the join land on his APRON PARTNER
 *     (0x6EBC movea A2,A0) — you enter on the apron, the in-ring man
 *     is not yanked mid-action; otherwise the scanned man himself
 *     takes the pad (0x6ED2-0x6EDC).
 *   - 0x6EE2 +0xA6 = 1 (input latch reset), 0x6EE8 hp init 0x1079E,
 *     0x6EEE +0x6E := +0x66, 0x6EF4 the P chip (+0x35 b4, chips.c).
 *   An INACTIVE slot in stock is the 1P layout's empty seat and joins
 *   the CPU team by promoting the $1C09E0/$1C0AEC shadow pair into the
 *   main slots (0x6F0E/0x6F32, the joiner takes the +0x33 b0 leader) —
 *   the engine seats the CPU team in the live rows already, so that
 *   path never arises (TODO EXACT if the shadow layout ever lands).
 * Rumble ($1C0161 b0): 0x18DA no joins once $1C16C5 is latched; a seat
 * whose main slot is INACTIVE (0x1914) with a clear queue word
 * ($1C1586[seat], 0x1926) and START held (0x192E) pays 0x55A D0=5
 * (0x1936) and queues $1C1586[seat] := 0x8000 (0x194A); the staging
 * 0x196A -> 0x9132 opens the pick grid of the not-in-play wrestlers
 * (ids 4/5 excluded, 0x926C-0x9272; arena full -> pick a $1C09E0 man
 * to take over, $1C15CE, 0x9282). ENGINE: eng_rumble_join() queues the
 * entrant with an auto-pick — the grid UI is TODO EXACT (0x9132). */
int eng_join_maxports(void)
{
    /* SW2:3-4 "Players" (docs/dip-switches.md): raw 3 = 4 players,
     * 2 = 3, 1 = 2. Stock consumes the inverted word ($1C0066 & 0xC00)
     * as cabinet-path picks (0x6E44/0x8BA8/0x97E); the engine keeps the
     * 4-seat loop and clamps how many ports may join (TODO EXACT).
     * Read through eng_dip_word() so the WF_DIPS poke works. */
    int v = (int)((~eng_dip_word() >> 10) & 3u);   /* $1C0066 bits 10-11 = SW2:3-4 */
    return v == 3 ? 4 : v == 2 ? 3 : 2;
}

static int port_bound(const eng_state *st, int p)
{
    for (int i = 0; i < ENG_MAX_OBJS; i++)
        if (st->obj[i].active && st->obj[i].input == p) return 1;
    return 0;
}

void eng_join_tick(eng_state *st)
{
    int maxp = eng_join_maxports();
    if (st->frame & 1) return;                    /* 0x103E: even-frame list only */
    if (st->lock16c5) return;                     /* 0x18C4 tst.w $1C007C analog */
    if (st->g161 & 1u) {                          /* 0x18CE: RUMBLE */
        if (st->rumble_phase) return;             /* 0x18DA tst.b $1C16C5 */
        for (int p = 0; p < 4; p++) {             /* 0x1908 the 4 ports */
            if (p >= maxp) continue;
            if (st->joinq & (1u << p)) continue;  /* 0x1926 queue word != 0 */
            if (port_bound(st, p)) continue;      /* 0x1914 seat slot active */
            if (!(st->inputs[p] & 0x40u)) continue;   /* 0x192E START (active-low in stock) */
            if (!eng_take_join()) continue;       /* 0x1936 jsr 0x55A D0=5 */
            st->joinq |= (uint8_t)(1u << p);      /* 0x194A $1C1586[seat] := 0x8000 */
            eng_rumble_join(st, p);               /* 0x196A staging -> 0x9132 */
            if (eng_dbgsel) fprintf(stderr, "join: rumble port %d queued (credits %u)\n",
                                    p + 1, eng_credits());
            break;                                /* 0x1950: one join per pass */
        }
        return;
    }
    for (int s = 0; s < 4; s++) {                 /* 0x6E5E: seat s, port s */
        eng_obj *o = &st->obj[s], *j;
        int p = s;
        if (p >= maxp) continue;
        if (!o->active) continue;                 /* stock: -> 0x6F0E shadow path (see above) */
        if (o->result) continue;                  /* 0x6E72 */
        if (o->a0flags) continue;                 /* 0x6E7A */
        /* 0x6E82 +0xA1 b0 "seat taken": the ROM marks the SCANNED man; the
         * engine's pad bit lands on the REDIRECTED man instead, so the seat
         * test here would wrongly lock seat 3 after a 3P apron redirect —
         * port_bound + the post-redirect pad test below cover it. */
        if (port_bound(st, p)) continue;          /* engine: P2 sits in row 2 on port 1
                                                     (charselect deviation) — a bound port
                                                     cannot buy a second seat */
        if (!(st->inputs[p] & 0x40u)) continue;   /* 0x6E5E START held */
        j = o;
        if ((o->role & RF_LEGAL) && o->teammate >= 0 && st->obj[o->teammate].active)
            j = &st->obj[o->teammate];            /* 0x6E9A/0x6EBC: the apron partner joins
                                                     (stock also bsets +0x33 b1 on the legal
                                                     man, 0x6EA2 — the engine's f33 b1 doubles
                                                     as "has the pad", so only j takes it) */
        if ((j->role & RF_PAD) && j->teammate >= 0 && st->obj[j->teammate].active
            && !(st->obj[j->teammate].role & RF_PAD))
            j = &st->obj[j->teammate];            /* an earlier join's redirect already pads
                                                     this man (3P on the apron partner): the
                                                     seat falls to the REMAINING teammate so
                                                     4P still gets the opposing team's other
                                                     man (user 2026-08-28 spec) */
        if (j->role & RF_PAD) continue;             /* engine guard: never steal a pad man
                                                     (stock's 2-seat layouts cannot reach
                                                     this; resolved before paying) */
        if (!eng_take_join()) continue;           /* 0x6E8C 0x55A D0=5 */
        j->role |= RF_PAD;                          /* 0x6EB0/0x6ED6 */
        j->input = p;                             /* 0x6EAC/0x6ED2 +0x8A := the port */
        j->driver &= (uint16_t)~0x40u;               /* 0x6EB6/0x6EDC autopilot off */
        j->cpu = 0;                               /* +0x56 b7: the pad man drives himself */
        j->btn_new = 0;                           /* 0x6EE2 move.b #1,(+0xA6): START is
                                                     marked already-HELD (bit8 of the
                                                     held word) so the same press does
                                                     not edge into the REGAIN-POWER
                                                     buy-in (0x8B3A) next frame */
        j->btn_held = 0x100u; j->btn_acc = 0x100u;
        j->hp = j->hp_max = (uint16_t)eng_pkg_stat((unsigned)j->wrestler, "hp",
                    j->wrestler < ENG_WS_EXT_MAX ? (int)tbl16(TBL(ws_max_energy), (uint32_t)eng_ws_base(j->wrestler) * 2u) : 100);
        j->hp_disp = (int16_t)j->hp;              /* 0x6EEE +0x6E := +0x66 */
        j->hp_delta = 0; j->hp_acc = 0; j->band = 0;   /* 0x6EE8 hp init 0x1079E */
        j->cue_flags |= CF_CHIP_P;                          /* 0x6EF4 the P chip (+0x35 b4) */
        {   /* 0x7008/0x70AC: jsr 0x782C (HUD re-init) + 0xC1EA + the
             * 0x7720/0x75EE portrait/gauge redraw — engine: wipe the
             * joined row's HUD block (its BUY-IN plates) and re-init. */
            int row = (int)(j - st->obj);
            if (row >= 0 && row <= 3) {
                unsigned b = hud_base(row);
                for (unsigned r = 0; r < 4; r++)
                    for (unsigned c = 0; c < 9; c++) {
                        unsigned a2 = b + r * 0x100 + c * 4;
                        if (a2 + 4 <= WF_FG0RAM_SIZE) memset(&wf.fg0_videoram[a2], 0, 4);
                    }
                j->hud_c2 = j->hud_c3 = 0;
            }
            st->hud_inited = 0;                   /* 0x782C full redraw */
        }
        if (eng_dbgsel)
            fprintf(stderr, "join: port %d bought seat %d -> controls o%d (w%d)%s, credits %u\n",
                    p + 1, s, (int)(j - st->obj), j->wrestler,
                    (j != o) ? " (apron partner, 0x6E9A)" : "", eng_credits());
        return;                                   /* 0x6EFA rts: one join per pass */
    }
}

/* 0x8B3A -> 0x8BA2: START buy-in, slots 0..3 in order (general path:
 * the object's own START edge +0xA2 b8; the 0x800 co-op port path is
 * the same test for a seated player). */
void eng_start_buyin(eng_state *st)
{
    if (st->lock16c5 || st->rumble_phase) return;                  /* $1C16C4 */
    for (int s = 0; s < 4; s++) {
        eng_obj *o = &st->obj[s];
        if (!o->active || !(o->btn_new & 0x100u)) continue;        /* 0x8C04 tst.b +0xA2 */
        if (o->st_flags & SF_FROZEN) continue;                              /* 0x8C0C held */
        if (o->st_flags & SF_ELIMINATED) continue;                              /* 0x8C16 eliminated */
        if (!(o->role & RF_PAD)) continue;                           /* 0x8C20 +0xA1 b0 seated human */
        if (o->result) continue;                                   /* 0x8C2A decided */
        if (!eng_take_buyin()) continue;                           /* 0x8C32 0x55A D0=2 */
        if (eng_dbgsel) fprintf(stderr, "buy-in: P%d hp %u -> ", s + 1, o->hp);
        o->hp = o->hp_max; o->hp_delta = (int16_t)o->hp_max;      /* 0x8C3E/0x8C44 */
        o->band = 0;                                               /* 0x8C4A */
        if (!(st->g161 & 1u) && o->teammate >= 0) {                /* 0x8C4E-0x8C62 tag partner */
            eng_obj *p = &st->obj[o->teammate];
            if (p->active && !(p->role & RF_PAD)) {                  /* not himself a human */
                p->hp = p->hp_max; p->hp_delta = (int16_t)p->hp_max; p->band = 0;
                p->mash_aa = 1;                                    /* 0x8C74 */
                memset(p->tries, 0, sizeof p->tries);              /* 0x8C7A.. +0xE8..+0xFC */
            }
        }
        o->mash_aa = 1;                                            /* 0x8CA6 */
        o->alt62 = 0;                                              /* 0x8CAC */
        st->sig169e = 0x80;                                        /* 0x8CB0: clock re-inits (30:00) */
        memset(o->tries, 0, sizeof o->tries);                      /* 0x8CBE.. */
        if (eng_dbgsel) fprintf(stderr, "%u (credits left %u)\n", o->hp, eng_credits());
    }
}

/* ---- MOD grapple gauge (modrules grapple_gauge; no stock analog) ----
 * A DISCREET 3-cell bar floating over a grappling pair: the holder's
 * +0x46 hold clock draining against the 0x12526 seed. Green while the
 * hold is safe, the RED tile set once the clock falls under the cat-9
 * throw window (0x12550) — "your throw is ready". Cells are wiped and
 * redrawn as the pair moves; FG0 is the fixed text layer, so this rides
 * over the action without touching sprites. */
static unsigned gg_prev[8]; static int gg_prev_n;
static void gg_wipe(void)
{
    for (int i = 0; i < gg_prev_n; i++) {
        unsigned off = gg_prev[i];
        if (off + 4 <= WF_FG0RAM_SIZE) {
            wf.fg0_videoram[off + 1] = 0; wf.fg0_videoram[off + 3] = 0;
            if (off + 0x104 <= WF_FG0RAM_SIZE) {
                wf.fg0_videoram[off + 0x101] = 0; wf.fg0_videoram[off + 0x103] = 0;
            }
        }
    }
    gg_prev_n = 0;
}
void eng_grapple_gauge_tick(eng_state *st)
{
    eng_obj *h = 0;
    if (!eng_mod_rule(MODR_GRAPPLE_GAUGE)) { if (gg_prev_n) gg_wipe(); return; }
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (o->active && (o->state & 0xFFu) == ST_HOLD && o->partner >= 0 && o->hold_t
            && (o->input >= 0 || st->obj[o->partner].input >= 0))
            { h = o; break; }          /* HUMAN grapples only (user 2026-08-24:
                                          no gauge on CPU-vs-CPU holds) */
    }
    gg_wipe();
    if (!h) return;
    {
        /* TUG-OF-WAR (user 2026-08-24: "the point of it is to show WHO IS
         * WINNING"): six cells at bottom centre, split at the middle. The
         * HOLDER's screen side fills with his remaining hold clock — a full
         * side = he is in charge; as the clock drains toward the auto-
         * reverse the bar crosses to the VICTIM's side. Blinks when the
         * throw window opens (act now). */
        unsigned seed = tbl16(TBL(hold_rules), 0), win = tbl16(TBL(hold_rules), 2);
        unsigned t = h->hold_t > seed ? seed : (unsigned)h->hold_t;
        int red = t < (win ? win : 0xA0u);
        int col = 17, row = 26;                              /* cells 17..22 */
        eng_obj *v = h->partner >= 0 ? &st->obj[h->partner] : 0;
        int holder_right = v ? ((h->x >> 16) > (v->x >> 16)) : 0;
        unsigned hshare = seed ? (t * 6u + seed / 2u) / seed : 3u;   /* 0..6 cells */
        if (eng_dbgsel)
            fprintf(stderr, "gg: t=%u/%u holder %s share %u%s\n", t, seed,
                    holder_right ? "right" : "left", hshare, red ? " RED" : "");
        if (red && (st->frame & 8u)) return;                 /* throw window: BLINK */
        for (int c = 0; c < 6; c++) {
            /* cell c filled when it belongs to the CURRENT leader's side:
             * the holder's share grows from HIS side of the centre line */
            int filled = holder_right ? (c >= 6 - (int)hshare) : (c < (int)hshare);
            unsigned off = (unsigned)(row * 0x100 + (col + c) * 4);
            unsigned step = 7, tl = 0xB33;                   /* empty tiles */
            if (filled) { step = 0xB; tl = 0xAD9 + 8; }      /* full HUD bar cell */
            else tl += 0;
            fg0_put(off, tl, 1);
            fg0_put(off + 0x100, tl + step, 1);
            gg_prev[gg_prev_n++] = off;
        }
    }
}
