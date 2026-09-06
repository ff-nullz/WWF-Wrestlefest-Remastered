/* GAME SELECT screen — transcription of ROM 0x52BE..0x54DE.
 *
 * Spec: docs/engine-specs/scene-gameselect.md.
 *
 * Reached from the reset/attract path (0x8EA: 0x1F6C, 0x1FDE, 0x1F9E, then
 * jmp 0x52BE) once a credit and a player start have landed. The screen is
 * scene-word 3 composed at scroll (0x140, 0x200): the GAME SELECT banner,
 * the ROYAL RUMBLE card on the left and the TAG MATCH card on the right,
 * CREDIT text on the fg0 layer. No sprites.
 *
 * Per frame (0x5342 slot loop, one seated slot = one vblank per frame):
 *   $1C015C++ ; >= 0x200 -> leave with the current mode (0x5474)
 *   0x64E0    sample the port: +0xA8 raw byte, +0xA6 held buttons,
 *             +0xA2 new-press word (low byte +0xA3)
 *   +0xA3 != 0 (any new button press) -> leave (0x537C)
 *   $1C0161 bit0 clear (tag):  ROM 0x54EC[+0xA8] -> LEFT arms rumble and
 *             resets the tag card palette (0x54CA); else cycle the tag
 *             card palette (0x5484 on table 0x5518 -> palette 0x188602)
 *   $1C0161 bit0 set (rumble): ROM 0x54E0[+0xA8] -> RIGHT clears it and
 *             resets the rumble palette; else cycle table 0x55D8 ->
 *             palette 0x188482
 *
 * Leaving (0x5474 -> 0x5698): the 2P "vs" sub-select is dip-gated
 * ($1C0066 & 0x800 with P2 seated) and not modelled (TODO EXACT). The
 * character select is then entered at 0x58B2 with scene 3 recomposed at
 * (0x140, 0x300) for tag or (0x140, 0x500) for rumble.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include "scene.h"
#include "credit.h"
#include "profile.h"

/* Tables this file owns (docs/adr-001-data-formats.md). The two direction
 * tables are 12 bytes each in the ROM but indexed by the raw 4-bit
 * joystick nibble (0x5380/0x5400), so nibbles 12..15 read the bytes that
 * follow: the next table, and the 2P-vs sub-select tables 0x54F8/0x5503
 * (0x5710/0x61EA/0x71FE, not modelled — declared so the spill is covered). */
static const tbl_def gs_tables[] = {
    { "gs_dir_rumble_to_tag", "base/front", 0x54E0, 12, TK_U8, 1,
      "0x53FA game select: by joystick nibble, nonzero = RIGHT (rumble -> tag card)" },
    { "gs_dir_tag_to_rumble", "base/front", 0x54EC, 12, TK_U8, 1,
      "0x538E game select: by joystick nibble, nonzero = LEFT (tag -> rumble card)" },
    { "gs_versus_dir_down",   "base/front", 0x54F8, 11, TK_U8, 1,
      "0x5710/0x61EA/0x71FE 2P-vs sub-select: by joystick nibble 0..10, nonzero = down (not modelled, TODO EXACT)" },
    { "gs_versus_dir_up",     "base/front", 0x5503, 11, TK_U8, 1,
      "0x5760 2P-vs sub-select: by joystick nibble 0..10, nonzero = up (not modelled, TODO EXACT)" },
    { "gs_flash_sequence",    "base/front", 0x550E, 10, TK_U8, 1,
      "0x5498 game select card flash: palette-set index per step (1 2 3 4 5 4 3 2 1 0)" },
    { "gs_palette_sets_tag",  "base/front", 0x5518, 6 * 32, TK_U16, 16,
      "0x53A4/0x5498 tag card flash: 6 palette sets x 16 words (15 written to $188602)" },
    { "gs_palette_sets_rumble","base/front", 0x55D8, 6 * 32, TK_U16, 16,
      "0x5410/0x5440 rumble card flash: 6 palette sets x 16 words (15 written to $188482); code 0x5698 follows" },
    { "num_blit_record_ptrs", "base/hud", 0x262A0, 4 * 4, TK_U32, 1,
      "0x26122 two-digit number blit: long -> record per id 0..3 (CREDIT line 0x1E92, match clock 0x262D2 ...)" },
    { "num_blit_records",     "base/hud", 0x262B0, 4 * 8, TK_U8, 8,
      "0x26122 number records {mode, pal, FG0 off hi, lo, long -> BCD value in work RAM}; code 0x262D2 follows" },
};
TBL_REGISTER(gs_tables)

/* ROM addresses still used as bus/ROM addresses below. */
#define ROM_DIR_TAG     0x54EC   /* gs_dir_tag_to_rumble */
#define ROM_DIR_RUMBLE  0x54E0   /* gs_dir_rumble_to_tag */
#define ROM_PAL_TAG     0x5518   /* gs_palette_sets_tag */
#define ROM_PAL_RUMBLE  0x55D8   /* gs_palette_sets_rumble */
#define PAL_TAG_DST     0x188602 /* fg-tile palette bank, line 0xC pen 1 */
#define PAL_RUMBLE_DST  0x188482 /* line 9 pen 1 */
#define FLASH_DIV       5        /* 0x5488 cmpi.w #5 */
#define FLASH_LEN       10       /* 0x54A8 cmpi.w #$a */
#define TIMEOUT         0x200    /* 0x5366 cmpi.w #$200,$1c015c */

#define SCROLL_X        0x140    /* 0x5300 */
#define SCROLL_Y        0x200    /* 0x5308 */
#define SCROLL_Y_TAG    0x300    /* 0x58F6 char-select window, tag */
#define SCROLL_Y_RUMBLE 0x500    /* 0x593C char-select window, rumble */
#define SCENE_WORD      3        /* 0x5310 */
#define ARENA_PAL       3        /* 0x5324 $1C15F4 (and $1C15F8) */
#define TEXT_PAL_SET    1        /* 0x52BE $1C15FC */

void eng_scene_publish(unsigned arena, unsigned textset);   /* scene.c */

/* State the ROM keeps in work RAM / the P1 slot ($1C05B0). */
static struct {
    int      rumble;       /* $1C0161 bit0 */
    unsigned timeout;      /* $1C015C */
    unsigned flash_idx;    /* slot +0x06 */
    unsigned flash_div;    /* slot +0x0A */
    unsigned held;         /* slot +0xA6 */
    unsigned raw;          /* slot +0xA8 */
    int      tag_set, rumble_set;   /* palette set currently shown on each
                                       card (-1 = as loaded by 0x2A06) */
    long     frames;       /* $1C0080 low word, debug */
} gs;

static unsigned rom_w(unsigned a)
{
    return ((unsigned)tbl_ra8(a) << 8) | tbl_ra8(a + 1);
}

int eng_gs_rumble(void) { return gs.rumble; }

/* 0x54CA (set 0) / 0x5484's copy: 15 words from a palette set to VRAM.
 * Writes go through the bus like pal_load.c so the layout matches. */
static void pal_set_write(unsigned table, unsigned dst, int set)
{
    unsigned src = table + (unsigned)set * 32u;   /* asl.w #5 */
    for (unsigned k = 0; k < 15; k++)             /* moveq #$e; dbra */
        m68k_write_memory_16(dst + k * 2u, rom_w(src + k * 2u));
}

/* 0x5484: every FLASH_DIV frames step the 10-entry sequence. */
static void flash_step(int *card_set)
{
    if (++gs.flash_div != FLASH_DIV)
        return;
    gs.flash_div = 0;
    *card_set = tbl8(TBL(gs_flash_sequence), gs.flash_idx);
    if (++gs.flash_idx == FLASH_LEN)
        gs.flash_idx = 0;
}

/* ---- FG0 text: ROM 0x26122 number draw and 0x1E92 CREDIT line ---- */

static void fg0_cell(unsigned off, unsigned word)   /* byte +1 lo, +3 hi */
{
    if (off > WF_FG0RAM_SIZE - 4u) return;   /* wrap-safe */
    wf.fg0_videoram[off + 1] = (uint8_t)word;
    wf.fg0_videoram[off + 3] = (uint8_t)(word >> 8);
}

static void fg0_clear(unsigned off)
{
    if (off > WF_FG0RAM_SIZE - 4u) return;   /* wrap-safe */
    memset(wf.fg0_videoram + off, 0, 4);
}

/* ROM 0x26122. Record 0x262A0[id & 0x7FFF]: byte0 mode, byte1 palette,
 * word2 fg0 byte offset, long4 pointer to the value (two BCD digits in
 * its first byte). The caller passes the value instead of the pointer.
 * Two digits are drawn (moveq #1,D3 / dbra); a leading zero is blanked
 * unless it is the last digit (0x2628E forces D4 after the first). */
void eng_num_draw(unsigned id, unsigned bcd_value)
{
    unsigned rec = tbl32(TBL(num_blit_record_ptrs), (id & 0x7FFFu) * 4u);
    unsigned mode = tbl_ra8(rec);                    /* record: num_blit_records */
    unsigned pal  = ((unsigned)tbl_ra8(rec + 1) << 12) & 0xF000u;
    unsigned a    = rom_w(rec + 2);
    unsigned erase = id & 0x8000u;
    unsigned d2 = bcd_value & 0xFFFFu;
    unsigned d4 = 0;
    int d3;

    for (d3 = 1; d3 >= 0; d3--) {
        unsigned d5;
        d2 <<= 4;                        /* lsl.l #4 */
        d5 = (d2 >> 16) & 0xFu;          /* swap; andi #$f */
        if (!erase && d5 == 0 && d4 == 0) {
            /* blank leading zero */
        } else {
            d4 = 1;
        }
        if (d5 >= 0xA) d5 += 7;          /* 0x2618A hex letters */
        if (mode == 0) {                               /* 0x261A2 */
            if (!erase && (d5 || d4)) fg0_cell(a, pal | (d5 + 0x20u));
            else fg0_clear(a);
        } else if (mode == 1) {                        /* 0x261CE */
            if (!erase && (d5 || d4)) {
                unsigned t = pal | (d5 * 2u + 0x70u);
                fg0_cell(a, t);
                fg0_cell(a + 0x100u, t + 1u);
            } else { fg0_clear(a); fg0_clear(a + 0x100u); }
        } else {                                       /* 0x2620E */
            if (!erase && (d5 || d4)) {
                unsigned t = pal | (d5 * 4u + 0x114u);
                fg0_cell(a, t);
                fg0_cell(a + 0x100u, t + 1u);
                fg0_cell(a + 4u, t + 2u);
                fg0_cell(a + 0x104u, t + 3u);
            } else {
                fg0_clear(a); fg0_clear(a + 4u);
                fg0_clear(a + 0x100u); fg0_clear(a + 0x104u);
            }
        }
        a += (mode == 2) ? 8u : 4u;                     /* 0x26278 */
        if (d3 == 1) d4 = 1;                            /* 0x2628E */
    }
}

/* ROM 0x2790: binary -> BCD (roxl/abcd over 16 bits). Only the low byte
 * is kept by the credit path (move.b D1,$1C0063). */
static unsigned bin_to_bcd(unsigned v)
{
    unsigned r = 0, shift = 0;
    v &= 0xFFFFu;
    while (v && shift < 32) {
        r |= (v % 10u) << shift;
        v /= 10u;
        shift += 4;
    }
    return r;
}

/* ROM 0x1E92 with the redraw forced (bit 7 of $1C0072, set by 0x1F9E):
 * "CREDIT" by the big blit id 0 (0x2503C), the count by number id 0,
 * and the $C1E94 triple cleared while the high byte of $1C004E is 0.
 * The free-play test ($1C004B == 1) is not modelled: TODO EXACT. */
void eng_credit_draw(unsigned credits)
{
    unsigned bcd = bin_to_bcd(credits & 0xFFu) & 0xFFu;   /* $1C0063 */
    eng_blit(0);
    eng_num_draw(0, bcd << 8);          /* pointer reads $1C0063 as byte0 */
    if (((credits >> 8) & 0xFFu) == 0) {
        fg0_clear(0x1E94); fg0_clear(0x1E98); fg0_clear(0x1E9C);   /* 0x1F06 */
    }
    /* else: 0x1EE6 number ids 2/3 + blit 0x25 (TODO EXACT, not reached
     * with a single credit) */
}


/* ---- scene ops ---- */

static void gs_begin(eng_state *st)
{
    memset(&gs, 0, sizeof gs);
    gs.tag_set = gs.rumble_set = -1;

    /* 0x52C6..0x52E4: text palette set 1, palette load, three sound
     * commands through 0x2052 (which ORs 0x3100 while $1C007C != 0). */
    eng_sound(0x3100);
    eng_sound(0x3120);
    eng_sound(0x3103);
    /* 0x5300..0x5318: window and scene word, compose. render.c composes
     * on the scene/cam change it sees from these. */
    st->scene = SCENE_WORD;
    st->cam_x = SCROLL_X;
    st->cam_y = SCROLL_Y;
    eng_scene_publish(ARENA_PAL, TEXT_PAL_SET);
    /* 0x8EA/0x1FDE + 0x531E/0x1F9E: sprite list and text layer cleared;
     * the credit line is redrawn on the next 0x1E92. */
    memset(wf.spriteram, 0, sizeof wf.spriteram);
    memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);
    eng_credit_force();                 /* 0x1F9E: bset 7,$1C0072 */
    eng_credit_line();                  /* first 0x1E92 (0x546A) */

    if (getenv("WF_DBGSEL"))
        fprintf(stderr, "gs: begin f%lld\n", (long long)st->frame);
}

/* ROM port byte as 0x64E0 sees it (not.w of $140026-family word, low 8
 * bits): bits 0-3 joystick R/L/U/D, 4-5 buttons 1/2, 7 start (MAME
 * wwfwfest input map). The engine's start bit is bit 6. */
static unsigned port_byte(uint32_t bits)
{
    return (bits & 0x3Fu) | ((bits & 0x40u) << 1);
}

static int gs_update(eng_state *st)
{
    {   /* PROFILE MODE (user 2026-08-26: a profile targets ONE game mode and
         * boots straight into it): no card - preset and leave for the select */
        extern int wf_profile_mode(void);
        int pm = wf_profile_mode();
        if (pm >= 0) { gs.rumble = pm; if (getenv("WF_DBGSEL")) fprintf(stderr, "gs: profile mode %s - skipping the game select\n", pm ? "rumble" : "tag"); goto leave; }
    }
    unsigned btn, newp;

    gs.frames++;                                   /* $1C0080 */
    if (++gs.timeout >= TIMEOUT)                   /* 0x5360 */
        goto leave;

    /* 0x64E0 (normal path, $1C015E bit4 clear): the raw byte lands in
     * the WORD +0xA8 (high byte 0), then `andi.b #$f,+0xA9` leaves the
     * joystick nibble as the word's value -- so the table index at 0x5380
     * is the direction nibble alone. Buttons come from D1 before the mask. */
    gs.raw = port_byte(st->inputs[0]);
    btn = (gs.raw >> 4) & 3u;                      /* lsr.b #4; andi #3 */
    newp = btn & ~gs.held;                         /* and / eor -> +0xA2 */
    gs.held = btn;
    gs.raw &= 0xFu;                                /* word +0xA8 */
    if (newp)                                      /* tst.b +0xA3 */
        goto leave;

    if (!gs.rumble) {                              /* 0x5384 */
        if (tbl_ra8(ROM_DIR_TAG + gs.raw)) {        /* 0x53C2: LEFT */
            gs.rumble = 1;
            gs.tag_set = 0;                        /* 0x54CA set 0 */
            gs.flash_idx = gs.flash_div = 0;
        } else {
            flash_step(&gs.tag_set);               /* 0x53B0 */
        }
    } else {                                       /* 0x53FA */
        if (tbl_ra8(ROM_DIR_RUMBLE + gs.raw)) {     /* 0x542E: RIGHT */
            gs.rumble = 0;
            gs.rumble_set = 0;
            gs.flash_idx = gs.flash_div = 0;
        } else {
            flash_step(&gs.rumble_set);            /* 0x541C */
        }
    }
    eng_credit_line();                             /* 0x546A jsr 0x1E92 */
    if (getenv("WF_DBGSEL") && (newp || (gs.frames % 60) == 0))
        fprintf(stderr, "gs: f%lld raw=%02X rumble=%d t=%u tagset=%d rumset=%d\n",
                (long long)st->frame, gs.raw, gs.rumble, gs.timeout,
                gs.tag_set, gs.rumble_set);
    return -1;

leave:
    /* 0x5474 -> 0x5698 (2P vs sub-select skipped) -> 0x58B2: the
     * character-select window on the same scene. */
    st->cam_x = SCROLL_X;
    st->cam_y = gs.rumble ? SCROLL_Y_RUMBLE : SCROLL_Y_TAG;
    if (getenv("WF_DBGSEL"))
        fprintf(stderr, "gs: leave f%lld rumble=%d timeout=%u\n",
                (long long)st->frame, gs.rumble, gs.timeout);
    return ENG_SCENE_CHARSELECT;
}

/* Runs after the compose/palette load each frame: re-assert the card
 * palettes the loop has written so far. The ROM writes them in place at
 * the moment of the step; reapplying the current set each frame is the
 * same picture. */
static void gs_draw(const eng_state *st)
{
    (void)st;
    if (gs.tag_set >= 0)
        pal_set_write(ROM_PAL_TAG, PAL_TAG_DST, gs.tag_set);
    if (gs.rumble_set >= 0)
        pal_set_write(ROM_PAL_RUMBLE, PAL_RUMBLE_DST, gs.rumble_set);
}

static const eng_scene_ops gs_ops = { gs_begin, gs_update, gs_draw };

void eng_gameselect_register(void)
{
    eng_scene_register(ENG_SCENE_GAMESELECT, &gs_ops);
}
