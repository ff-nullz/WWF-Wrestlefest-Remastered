/* Match-card "VS" banner — transcription of ROM 0x98BA (draw), 0x9A42
 * (erase) and their helpers 0x993A / 0x99D0 / 0x9A78 / 0x9ACA.
 * Spec: docs/engine-specs/scene-banner.md.
 *
 * The ring intro 0xA654 draws the card at 0xA670 (before its first step)
 * straight onto the FG0 text layer over the ring: both team-1 names on
 * the top rows, "VS" in the middle, both team-2 names on the bottom rows.
 * Step 0 of the intro holds TAB_WAIT[0] = 0x80 frames (0xAA66), then the
 * 0 -> 1 step change calls 0x9A42 (0xAA4E) which wipes FG0 rows 1..29.
 * The card also goes when the intro is skipped (0xA782).
 *
 * Glyph runs: 0x9ACA walks the record 0x9B26[id] = {first tile, count-1,
 * then (byte delta, count-1) pairs, 0 ends}; every cell gets the next
 * consecutive tile, byte +3 = pal<<4 | tile hi. Per-id placement comes
 * from 0x9980[id*8 + pos*2] (pos = slot parity | team<<1); the glyph
 * palette line from 0x9E66[id] into FG0 palette line 6/7 (team 1) or
 * 0xE/0xF (team 2) through the bus (aliased palette RAM, pal_load.c).
 * Ids >= 0xA use the fixed 8x38-cell bitmap at 0x9D36 instead (0x99D0).
 * Every number below is read from tbl_ra8() or cites its PC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bus.h"
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include "scene.h"            /* eng_gs_rumble */

static unsigned r8(uint32_t a)  { return tbl_ra8(a); }
static unsigned r16(uint32_t a) { return ((unsigned)tbl_ra8(a) << 8) | tbl_ra8(a + 1); }

/* Tables this file owns (docs/adr-001-data-formats.md). The glyph-run
 * pointer table 0x9B26 serves every 0x9ACA caller (hud.c prompts,
 * aisle.c bar, the continue/result screens); its records sit in five
 * separate ROM regions, each bounded by code on both sides:
 * {tile u16, count-1 u16, (FG0 delta u16, count-1 u16)*, 0 u16}. */
static const tbl_def banner_tables[] = {
    { "banner_placement",     "base/hud", 0x9980, 10 * 8,  TK_U16, 4,
      "0x993A VS card: per wrestler id 0..9 the FG0 byte offset of the name by pos (slot parity | team<<1)" },
    { "banner_run_ptrs",      "base/hud", 0x9B26, 40 * 4,  TK_U32, 1,
      "0x9ACA glyph runs: long -> run record per id 0..0x27 (0..0xC VS card names/VS, 0xD..0xF bars, 0x18..0x26 HUD prompts)" },
    { "banner_name_runs",     "base/hud", 0x9BC6, 0x9D36 - 0x9BC6, TK_U16, 0,
      "0x9ACA run records for ids 0..0xC (wrestler names, VS); 0xA/0xB share the VS record but draw the bitmap instead" },
    { "banner_bitmap",        "base/hud", 0x9D36, 8 * 0x26, TK_U8, 0x26,
      "0x99D0 fixed 8-row x 0x26-byte tile bitmap for ids >= 0xA (LOD team name)" },
    { "banner_palette_ptrs",  "base/hud", 0x9E66, 12 * 4,  TK_U32, 1,
      "0x9A78 VS card: long -> 16-word FG0 palette line per wrestler id 0..11" },
    { "banner_palettes",      "base/hud", 0x9E96, 9 * 32,  TK_U16, 16,
      "0x9A78 VS card palette lines (9 distinct, 16 words each) pointed to by banner_palette_ptrs" },
    { "banner_continue_runs", "base/hud", 0x9FB6, 0xA034 - 0x9FB6, TK_U16, 0,
      "0x9ACA run records for ids 0x10, 0x14..0x17 (full-screen messages: 0xBEBE/0xBEDA continue screen, 0x140E) — exact wording unconfirmed" },
    { "banner_prompt_runs",   "base/hud", 0xA034, 0xA0AE - 0xA034, TK_U16, 0,
      "0x9ACA run records for ids 0x18..0x26: INSERT COIN / BUY-IN / PUSH n BUTTON / REGAIN POWER HUD prompts (0xC710, hud.c)" },
    { "banner_startup_run",   "base/hud", 0xA0CA, 0xA0E0 - 0xA0CA, TK_U16, 0,
      "0x9ACA run record id 0x27, drawn at $C1044 by 0xA0AE (from 0x802) — meaning unconfirmed" },
    { "banner_bar_runs",      "base/hud", 0xA496, 0xA4D4 - 0xA496, TK_U16, 0,
      "0x9ACA run records for ids 0xD..0xF: 0xD = aisle/intro bottom bar (0xB29E, aisle.c), 0xE/0xF from 0xA44E" },
    { "banner_countdown_runs","base/hud", 0xC1A0, 0xC1EA - 0xC1A0, TK_U16, 0,
      "0x9ACA run records for ids 0x11..0x13 drawn by 0xC12A..0xC158 ($1C16CC/$1C16CD countdown) — meaning unconfirmed" },
};
TBL_REGISTER(banner_tables)

#define FG0_BASE    0xC0000u
/* banner_placement 0x9980 (0x993A), banner_run_ptrs 0x9B26 (0x9ACA),
 * banner_palette_ptrs 0x9E66 (0x9A78): named tables above. */
#define TAB_BITMAP  0x9D36u     /* 0x99D0: 8 rows x 0x26 bytes, ids >= 0xA */
#define PAL_TEAM1   0x180300u   /* 0x9A94: FG0 palette line 6 (+0x80 -> 7) */
#define PAL_TEAM2   0x180700u   /* 0x9A88: line 0xE (+0x80 -> 0xF) */
#define VS_DEFAULT  0xC0E40u    /* 0x98E2 */
#define VS_TALL     0xC1040u    /* 0x98FC: ids 4/5 on team 1 */

static int active;
/* What 0x98BA decided per active slot, replayed by eng_banner_refresh():
 * the engine's eng_render_init() / scene palette reload run after the
 * intro began (eng_init) and would wipe the one-shot ROM writes. */
static struct { unsigned id, pos, d7, pal_dst; uint32_t pal_src; const char *cname; } card[ENG_MAX_OBJS];   /* cname: a clone's own name (big font) */
static int n_card;
static uint32_t vs_addr;

static void cell(uint32_t addr, unsigned lo, unsigned hi)
{
    unsigned off = addr - FG0_BASE;
    if (off + 4 > WF_FG0RAM_SIZE) return;
    wf.fg0_videoram[off + 1] = (uint8_t)lo;
    wf.fg0_videoram[off + 3] = (uint8_t)hi;
}

/* 0x9ACA: glyph run record `idx` at FG0 address a2, palette nibble d7. */
void eng_banner_runs(uint32_t a2, unsigned idx, unsigned d7)
{
    uint32_t rec = tbl32(TBL(banner_run_ptrs), idx * 4u);
    unsigned tile = r16(rec), cnt = r16(rec + 2);
    uint32_t p = rec + 4, a3 = a2;
    for (;;) {
        for (unsigned i = 0; i <= cnt; i++) {               /* dbra D3 */
            cell(a3, tile & 0xFFu, ((tile >> 8) & 0xFFu) | d7);
            a3 += 4; tile++;
        }
        {
            unsigned delta = r16(p);                          /* 0x9B0A */
            if (!delta) break;
            cnt = r16(p + 2);
            a2 += delta;                                      /* andi.l #$ffff: unsigned */
            a3 = a2;
            p += 4;
        }
    }
}

/* 0x99D0: fixed 8-row bitmap for the two ids without a glyph run. */
static void bitmap(int team2, unsigned d7)
{
    uint32_t a2 = FG0_BASE + (team2 ? 0x1504u : 0x504u);
    uint32_t a1 = TAB_BITMAP;
    d7 += 9;                                                  /* 0x99EC (lands in tile hi) */
    for (int row = 0; row < 8; row++) {
        uint32_t a3 = a2;
        for (unsigned i = 0; i < 0x13; i++) {
            cell(a3, r8(a1 + i * 2u), d7);
            cell(a3 + 4, r8(a1 + i * 2u + 1u), d7);
            a3 += 8;
        }
        a1 += 0x26; a2 += 0x100;
    }
}

static void pal_write(unsigned dst, uint32_t src)
{
    for (unsigned i = 0; i < 16; i++)                         /* 0x9AAA loop */
        m68k_write_memory_16(dst + i * 2u, r16(src + i * 2u));
}

/* 0x98BA. Slots are the engine's 0..3 (team 2 at 2/3 plays the ROM's
 * $1C07C8/$1C09E0 pairs; only the slot parity and the side bit matter). */
void eng_banner_draw(const eng_state *st)
{
    uint32_t a3 = VS_DEFAULT;
    n_card = 0;
    if (eng_gs_rumble()) return;                              /* 0x98BA btst #0,$1C0161 */
    for (int s = 0; s < ENG_MAX_OBJS && n_card < ENG_MAX_OBJS; s++) {
        const eng_obj *o = &st->obj[s];
        unsigned id, pos;
        if (!o->active) continue;
        id = (unsigned)eng_ws_base(o->wrestler) & 0xFu;       /* +0x56 & 0xF; clones:
                                                                 the BASE id's plates
                                                                 (TODO EXACT: custom name
                                                                 FG0 glyph runs) */
        /* 0x993A */
        pos = (unsigned)(s & 1) | ((o->role & RF_SIDE) ? 2u : 0u);
        a3 = VS_DEFAULT;                                      /* 0x98E2 */
        if (id == 5 || id == 4) {
            if (!(o->role & RF_SIDE)) a3 = VS_TALL;
            a3 -= 0x100;                                      /* 0x9902 */
        }
        card[n_card].id = id;
        card[n_card].cname = o->wrestler >= 12 ? eng_ws_clone_name(o->wrestler) : NULL;   /* user 2026-08-25: font, not the base's plate */
        card[n_card].pos = pos;
        card[n_card].d7 = ((pos & 2u) ? pos + 0xCu : pos + 6u) << 4;
        /* 0x9A78 (the ROM read itself waits for refresh: eng_init runs
         * before the ROM is loaded) */
        card[n_card].pal_dst = ((pos & 2u) ? PAL_TEAM2 : PAL_TEAM1) + ((pos & 1u) ? 0x80u : 0u);
        n_card++;
    }
    vs_addr = a3;
    active = 1;
    /* eng_init() runs before the data layer is loaded (main.c: eng_init,
     * eng_render_init, then tbl_load_*); a refresh there reads zeros —
     * every pointer resolves to ROM address 0 — and everything it writes
     * (FG0 cells, palette lines) is wiped by eng_render_init's memset
     * anyway. render.c replays eng_banner_refresh() every frame the card
     * is up, so skipping the unloaded refresh changes nothing. */
    {
        uint32_t n;
        if (tbl_bytes(TBL(banner_palette_ptrs), &n)) eng_banner_refresh();
    }
}

/* The 0x98BA writes themselves (FG0 cells + palette lines), in ROM
 * order: per slot name glyphs then palette, finally "VS" in palette 0.
 * Idempotent; render.c replays it every frame the card is up. */
void eng_banner_refresh(void)
{
    if (!active) return;
    for (int i = 0; i < n_card; i++) {
        if (card[i].cname && card[i].cname[0]) {   /* CLONE: his name in the standard big font at the plate's place */
            unsigned off = tbl16(TBL(banner_placement), card[i].id * 8u + card[i].pos * 2u);
            eng_fg0_bigtext(off / 0x100u, (off % 0x100u) / 4u, card[i].cname, 0);
        } else if (card[i].id >= 0xA) bitmap((card[i].pos & 2u) != 0, card[i].d7);
        else eng_banner_runs(FG0_BASE + tbl16(TBL(banner_placement), card[i].id * 8u + card[i].pos * 2u),
                             card[i].id, card[i].d7);
        card[i].pal_src = tbl32(TBL(banner_palette_ptrs), card[i].id * 4u);
        pal_write(card[i].pal_dst, card[i].pal_src);
        if (getenv("WF_DBGSEL") && wf.frame < 3)
            fprintf(stderr, "banner: id %u pos %u d7 %02X pal %05X <- %05X [%04X %04X %04X] now %02X%02X\n",
                    card[i].id, card[i].pos, card[i].d7, card[i].pal_dst, card[i].pal_src,
                    r16(card[i].pal_src), r16(card[i].pal_src + 2), r16(card[i].pal_src + 4),
                    wf.palette[card[i].pal_dst - 0x180000u + 2], wf.palette[card[i].pal_dst - 0x180000u + 3]);
    }
    eng_banner_runs(vs_addr, 0xC, 0);                        /* 0x992A: "VS", palette 0 */
}

/* 0x9A42: clear FG0 rows 1..29 ($C0100..$C1DFF). */
void eng_banner_clear(void)
{
    if (0x1E00u <= WF_FG0RAM_SIZE)
        memset(wf.fg0_videoram + 0x100, 0, 0x1E00 - 0x100);
    active = 0;
    n_card = 0;
}

int eng_banner_active(void) { return active; }

