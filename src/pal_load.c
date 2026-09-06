/* Palette loading — C form of ROM 0x2A06 and its blitter 0x2AD0.
 *
 * This is the last part of the display stack that was still entirely
 * 68k-authored: wf.palette is only ever filled by 68k stores through the bus
 * (src/bus.c), so nothing on the C side could produce a palette from ROM.
 *
 * What the ROM does, and what it does NOT do:
 *
 *   0x2A06  the loader. Waits vblank, then blits three groups from ROM into
 *           palette VRAM, indexed by two plain work-RAM words.
 *   0x2AD0  the blitter. 16 lines of 16 colour words, source contiguous at 32
 *           bytes per line, destination stepping 0x80 per line.
 *   0x26732 NOT a loader. It is `move.l (A0)+,(A1)+` with A0 = 0x180000, i.e.
 *           it copies palette VRAM *out* to work RAM as fade scratch. The
 *           fade-out path at 0x2650E calls it, which is what makes it look
 *           like a loader from that call site.
 *
 * Live hook is the PORTED row for 0x2AD0 (wf_pal_2ad0), not 0x2A06.
 * 0x2A06's two vblank waits stay 68k: match.scn never enters with $140026
 * bit2 already set (start-wait ~3400 iterations/call) and the blit does not
 * run out the rest of vblank either, so a zero-wait-both replacement of
 * 0x2A06 would decline every call. Sprite bank 0x182000 is 0x2AEA, not this
 * loader.
 *
 * The destination stride is 0x80 for a 16-colour line that occupies 0x20 real
 * bytes because palette RAM has A5/A6 unconnected — the hardware folds the
 * word offset and four addresses alias onto the same storage (see
 * wf_palette_write in src/bus.c). Writes therefore go through the bus rather
 * than into wf.palette[] directly: writing the array direct would land a
 * quarter of the expected bytes and fail the oracle, which compares the
 * aliased view MAME presents.
 *
 * There is no compression, no bank switching and no runtime generation. Every
 * palette byte the game shows comes from maincpu ROM.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "wf.h"
#include "tbl.h"
#include "layer.h"
#include "engine.h"

/* Scene/arena selector and text-palette set, both plain work-RAM words. */
#define W_ARENA      0x15F4
#define W_TEXTSET    0x15FC

/* ROM tables. */
#define TAB_FG_TILE  0x2A98      /* 7 longs -> 512-byte blobs, fg tilemap */
#define TAB_BG_TILE  0x2AB4      /* 7 longs -> 512-byte blobs, bg tilemap */
#define TAB_TEXT     0x2B82      /* 4 sets x 16 longs -> 32-byte lines */

/* Palette VRAM banks. 0x184000 is skipped: no GFX decode has colour base
 * 0x800, and the ROM touches it only to clear one word. */
#define PAL_FG0      0x180000    /* text layer   */
#define PAL_SPRITE   0x182000    /* sprites      */
#define PAL_BG_TILE  0x186000    /* bg tilemap   */
#define PAL_FG_TILE  0x188000    /* fg tilemap   */

#define PAL_LINE     0x80u       /* address step per 16-colour line */

/* Palette data (docs/adr-001): the 0x2A06 loader's three pointer tables and
 * the colour lines they land in. The palette block is contiguous ROM from
 * 0x2C82 to 0x5122 — text lines, body palettes (sprite.c), then the 12 tile
 * palette banks the fg/bg pointer tables select (fg 0x3922..0x4722 = arenas
 * 0-6, bg 0x4722..0x5122 = 5 banks shared by the 7 arenas). */
static const tbl_def pal_tables[] = {
    { "fg_tile_palette_ptrs", "base/palette", 0x2A98, 7 * 4, TK_U32, 1,
      "0x2A14 per-arena ($1C15F4, 0..6) long -> 512-byte fg tilemap palette bank (16 lines) blitted to 0x188000 by 0x2AD0" },
    { "bg_tile_palette_ptrs", "base/palette", 0x2AB4, 7 * 4, TK_U32, 1,
      "0x2A30 per-arena long -> 512-byte bg tilemap palette bank blitted to 0x186000 by 0x2AD0" },
    { "text_palette_ptrs",    "base/palette", 0x2B82, 4 * 16 * 4, TK_U32, 16,
      "0x2A4C: 4 text sets ($1C15FC) x 16 lines, long -> 32-byte fg0 palette line copied to 0x180000 + line*0x80 (0x2A6A)" },
    { "text_palette_lines",   "base/palette", 0x2C82, 21 * 32, TK_U16, 16,
      "the 21 distinct 16-colour fg0 text palette lines text_palette_ptrs point into (0x2C82..0x2F22)" },
    { "tile_palette_banks",   "base/palette", 0x3922, 12 * 512, TK_U16, 16,
      "12 tilemap palette banks of 16 lines x 16 colours: fg arenas 0-6 at 0x3922+arena*512, bg banks at 0x4722.. (0x2AD0 blit, 16 lines stepping 0x80)" },
};
TBL_REGISTER(pal_tables)

/* ROM 0x2AD0, opcode-faithful from re_002ad0.c WF_CYC68000:
 *   move.w #imm,Dn 8; move.w (An)+,(An)+ 12; adda.l #imm,An 16; rts 16
 *   dbf base 12, loop -2 = 10, expire +2 = 14
 *   inner: 16*12 + 15*10 + 14 = 356
 *   16 * (8 + 356 + 16) + (15*10 + 14) + 8 + 16 = 6268
 * match.scn --recomp-none: 6268 x7, 6892 x6 (6268+624 IRQ2), 6936 x3. */
#define WF_PAL_2AD0_CYCLES 6268

static uint16_t ram_w(unsigned off)
{
    return (uint16_t)(((unsigned)wf.work_ram[off] << 8) | wf.work_ram[off + 1]);
}

static unsigned rom_long(unsigned addr)
{
    return ((unsigned)tbl_ra8(addr) << 24) | ((unsigned)tbl_ra8(addr + 1) << 16)
         | ((unsigned)tbl_ra8(addr + 2) << 8) | (unsigned)tbl_ra8(addr + 3);
}

static uint16_t rom_word(unsigned addr)
{
    return (uint16_t)(((unsigned)tbl_ra8(addr) << 8) | tbl_ra8(addr + 1));
}

/* ROM 0x2AD0. Source via the bus: the only caller passes a ROM pointer, but
 * the instruction is move.w (A0)+,(A1)+. */
static void pal_blit(unsigned dst, unsigned src)
{
    int line, pen;

    for (line = 0; line < 16; line++) {
        for (pen = 0; pen < 16; pen++)
            m68k_write_memory_16(dst + (unsigned)line * PAL_LINE
                                     + (unsigned)pen * 2u,
                                 m68k_read_memory_16(src + (unsigned)pen * 2u));
        src += 32u;
    }
}

/* pal_blit from a 512-byte memory blob (16 lines x 16 BE words): the arena override */
static void pal_blit_mem(unsigned dst, const uint8_t *src)
{
    for (int line = 0; line < 16; line++)
        for (int pen = 0; pen < 16; pen++) {
            const uint8_t *p = src + line * 32 + pen * 2;
            m68k_write_memory_16(dst + (unsigned)line * PAL_LINE + (unsigned)pen * 2u, (uint16_t)((p[0] << 8) | p[1]));
        }
}

/* One 32-byte line straight into a bank line (the fg0 text path in 0x2A06,
 * which indirects through a per-line pointer rather than a contiguous blob). */
static void pal_line(unsigned dst, unsigned src)
{
    int pen;

    for (pen = 0; pen < 16; pen++)
        m68k_write_memory_16(dst + (unsigned)pen * 2u,
                             rom_word(src + (unsigned)pen * 2u));
}

/* ROM 0x2A06, minus its two vblank waits — the caller owns when this runs.
 * The ROM does it once per scene transition while the screen is blanked, not
 * per frame. */
void wf_palette_load(void)
{
    unsigned arena = ram_w(W_ARENA);
    unsigned set = ram_w(W_TEXTSET);
    unsigned tab;
    int line;

    {   /* imported arena (src/arena.c): its own 16-bank palettes */
        const uint8_t *fp = wf_arena_palette(arena, 0), *bp = wf_arena_palette(arena, 1);
        if (arena >= 7 && !fp) arena = 0;   /* the extra ring (scene 7) without an arena: no ROM
                                                palette row past 6 - draw it as the WWF ring */
        if (fp) pal_blit_mem(PAL_FG_TILE, fp); else pal_blit(PAL_FG_TILE, rom_long(TAB_FG_TILE + arena * 4u));
        if (bp) pal_blit_mem(PAL_BG_TILE, bp); else pal_blit(PAL_BG_TILE, rom_long(TAB_BG_TILE + arena * 4u));
    }

    tab = TAB_TEXT + set * 64u;
    for (line = 0; line < 16; line++)
        pal_line(PAL_FG0 + (unsigned)line * PAL_LINE,
                 rom_long(tab + (unsigned)line * 4u));
}

/* Scene palettes on --port/--mods. --68k never calls this: exact.sh is
 * the arcade layer. Select, menu, and C attract/intro own their own
 * pal_load, so the latch stands down there the same way the tilemap
 * shadow does for select/menu. */
void eng_sprite_palettes_flush(void);   /* sprite.c: deferred boot-time 0x2AEA install */

void wf_palette_latch(void)
{
    static int last_arena = -1, last_set = -1;
    int arena, set;

    eng_sprite_palettes_flush();       /* body palettes before any draw or snapshot of 0x182000 */
    if (wf_layer_arcade() || wf_select_active() || wf_menu_active()
        || wf_attract_active() || wf_intro_active()
        || wf_walkin_active() || wf_native_active()) {
        last_arena = -1;
        last_set = -1;
        return;
    }
    arena = (int)ram_w(W_ARENA);
    set = (int)ram_w(W_TEXTSET);
    if (arena == last_arena && set == last_set)
        return;
    wf_palette_load();
    last_arena = arena;
    last_set = set;
    if (getenv("WF_PAL_CHECK"))
        fprintf(stderr, "PALLOAD f=%ld arena=%u set=%u\n",
                wf.frame, (unsigned)arena, (unsigned)set);
}

/* (port-side register wrapper removed — the engine runs no CPU) */
