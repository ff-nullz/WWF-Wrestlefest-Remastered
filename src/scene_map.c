/* Scene tilemap composer — C form of ROM 0x26E66.
 *
 * This is the routine that authors every scene background: the ring, the
 * walkout, attract, boot, and the interstitials. It is called once per scene
 * change (five times in a 4,000-frame tag match), not per frame; the per-frame
 * motion is the scroll registers, not a recompose.
 *
 * What it actually does, measured from the disassembly:
 *
 *   0x26E66  runs 0x26EA6 twice, once per plane (bit 7 of 0x1C17FE selects),
 *            sets 0x1C185A, and writes a per-scene byte from the table at
 *            0x26E9E to the video control register 0x140010.
 *   0x26EA6  latches the scroll pair, derives a tile-space window origin,
 *            then walks a 16x16 grid of map units building a (src,dst) row
 *            buffer and flushing it a row at a time.
 *   0x26C1C  picks the plane: FG uses the pointer table at 0x2709C and VRAM
 *            0x80000; BG uses 0x270BC and 0x82000. Both are indexed by the
 *            scene word at 0x1C007E.
 *   0x26CA8  source address for one map unit.
 *   0x26D56  destination address for one map unit.
 *   0x26DAC  the copy: four moves per unit, because one map unit is a 2x2
 *            block of tiles. That is why the scroll is shifted by 5 to reach
 *            tile space — a unit is 32 pixels, two 16-pixel tiles.
 *
 * There is no compression and no bytecode here: the scene maps are plain tile
 * data in ROM, addressed through a grid of block pointers. Each block covers
 * 10x8 map units, which is 20x16 cells, and the block grid is 8 wide.
 *
 * The window is 16x16 units = 32x32 tiles, which is the whole tilemap, so a
 * compose writes every cell and the result depends on nothing but the scene
 * and the scroll. That is what makes it checkable from cold: wf_scene_check()
 * recomputes both planes and compares against the VRAM the ROM produced.
 */

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "bus.h"
#include "wf.h"
#include "tbl.h"
#include "layer.h"
#include "menu.h"
#include "engine.h"

/* Work-RAM words, offsets from WF_WORKRAM_BASE. */
#define W_SCENE      0x007E
#define W_SCROLL_X   0x1804
#define W_SCROLL_Y   0x1806
/* Tile-space coordinates derived from the scroll (0x26ADA). */
#define W_TILE_X     0x17D6
#define W_TILE_Y     0x17D8

/* ROM tables. */
#define TAB_FG       0x2709C
#define TAB_BG       0x270BC

/* Per-plane geometry. A map unit is a 2x2 block of tiles; FG cells are four
 * bytes and BG cells two, which is the only difference between the planes. */
#define FG_VRAM_ROW  0x80   /* 32 cells * 4 bytes */
#define BG_VRAM_ROW  0x40   /* 32 cells * 2 bytes */
#define FG_SRC_ROW   0x50   /* 20 cells * 4 bytes, one block row */
#define BG_SRC_ROW   0x28   /* 20 cells * 2 bytes */

/* Scene tilemap sources (docs/adr-001, group base/scene). 0x26C1C picks a
 * per-scene table of 64 block pointers (8x8 blocks of 10x8 map units =
 * 20x16 tiles; FG block 0x500 bytes, BG 0x280) for the scene word at
 * $1C007E; 0x26CA8 indexes it with (x/10)*4 + (y>>3)*0x20. The 16 block
 * tables are interleaved FG/BG per scene from 0x270DC; the blocks they
 * point to fill 0x2ED0C..0x38F14 (one unreferenced 0x208 zero gap at
 * 0x2F20C) up to the sprite meta table. */
static const tbl_def scene_map_tables[] = {
    { "scene_priority_bytes",  "base/scene", 0x26E9E, 8, TK_U8, 1,
      "0x26E86: per scene (0..7) the byte written to the video control register $140010 (layer priority) after a compose" },
    { "scene_fg_map_ptrs",     "base/scene", 0x2709C, 8 * 4, TK_U32, 1,
      "0x26C2A: per scene the ROM address of its FG block-pointer table (64 longs)" },
    { "scene_bg_map_ptrs",     "base/scene", 0x270BC, 8 * 4, TK_U32, 1,
      "0x26C4A: per scene the ROM address of its BG block-pointer table (64 longs)" },
    { "scene_map_block_ptrs",  "base/scene", 0x270DC, 8 * 2 * 0x100, TK_U32, 8,
      "16 tables of 8x8 block pointers (scene n: FG at 0x270DC+n*0x200, BG at +0x100): 0x26CA8 block = (x/10, y>>3), row = 8 blocks" },
    { "scene_tile_blocks",     "base/scene", 0x2ED0C, 0x38F14 - 0x2ED0C, TK_U16, 20,
      "the tile blocks scene_map_block_ptrs point into: FG block = 16 rows x 20 cells x 4 bytes (unit row 0xA0), BG block = 16 x 20 x 2 (unit row 0x50); copied 2x2 units by 0x26DAC" },
};
TBL_REGISTER(scene_map_tables)

static unsigned rom_long(unsigned addr)
{
    return ((unsigned)tbl_ra8(addr) << 24) | ((unsigned)tbl_ra8(addr + 1) << 16)
         | ((unsigned)tbl_ra8(addr + 2) << 8) | (unsigned)tbl_ra8(addr + 3);
}

static uint16_t ram_w(unsigned off)
{
    return (uint16_t)(((unsigned)wf.work_ram[off] << 8) | wf.work_ram[off + 1]);
}

static unsigned ram_l(unsigned off)
{
    return ((unsigned)wf.work_ram[off] << 24)
         | ((unsigned)wf.work_ram[off + 1] << 16)
         | ((unsigned)wf.work_ram[off + 2] << 8)
         | (unsigned)wf.work_ram[off + 3];
}

/* The row flush moves ROM -> videoram through the 68k's own address space.
 * Sources are always ROM and destinations always a tilemap, but going through
 * the bus keeps the mapping in one place. */
static void copy_long(unsigned dst, unsigned src)
{
    m68k_write_memory_32(dst, m68k_read_memory_32(src));
}

static void copy_word(unsigned dst, unsigned src)
{
    m68k_write_memory_16(dst, m68k_read_memory_16(src));
}

/* 0x26CA8 with D1 = 0: the composer's inner loop clears D1 every iteration,
 * so the map unit is always the window origin itself and the whole address
 * comes from 0x1C17E2/0x1C17E4. (The other caller, 0x26C8A, feeds real deltas
 * from a list for incremental scrolls.) */
static unsigned unit_src(unsigned table, uint16_t x, uint16_t y, int plane)
{
    uint16_t d2 = (uint16_t)(-(uint16_t)(y - 0x40));
    uint16_t d1 = x;
    int16_t  d3 = (int16_t)(uint16_t)(((uint16_t)(d1 / 10u) << 2)
                                    + ((uint16_t)(d2 >> 3) << 5));
    unsigned d4 = plane
        ? (unsigned)((uint16_t)((d1 % 10u) << 2)) + (unsigned)(d2 & 7u) * 0x50u
        : (unsigned)((uint16_t)((d1 % 10u) << 3)) + (unsigned)(d2 & 7u) * 0xA0u;

    return rom_long((unsigned)((int)table + d3)) + d4;
}

/* 0x26D56 with D1 = 0. */
static unsigned unit_dst(unsigned base, uint16_t x, uint16_t y, int plane)
{
    uint16_t d2 = (uint16_t)(-(uint16_t)(y - 0x40)) & 0x000Fu;
    uint16_t d1 = (uint16_t)(x + 0x10) & 0x000Fu;

    return plane ? base + ((unsigned)d1 << 2) + ((unsigned)d2 << 7)
                 : base + ((unsigned)d1 << 3) + ((unsigned)d2 << 8);
}

/* 0x26DAC's copy for one unit: the 2x2 block, second row at the source and
 * destination row strides. Reads ROM, writes the caller's tilemap image. */
static void unit_copy(uint8_t *out, unsigned outsize, unsigned dst,
                      unsigned src, int plane)
{
    const unsigned n = plane ? 2u : 4u;
    const unsigned srow = plane ? BG_SRC_ROW : FG_SRC_ROW;
    const unsigned drow = plane ? BG_VRAM_ROW : FG_VRAM_ROW;
    unsigned i;

    for (i = 0; i < 2; i++) {
        unsigned so = src + i * srow, dof = dst + i * drow;
        if (dof + n * 2u > outsize)
            continue;
        { const uint8_t *sp = tbl_ra_ptr(so, n * 2u); if (!sp) continue;
          memcpy(out + dof, sp, n); memcpy(out + dof + n, sp + n, n); }
    }
}

/* One plane of 0x26EA6. Writes the full 32x32 tilemap into `out`. */
static void compose_plane(uint8_t *out, unsigned outsize, int plane)
{
    unsigned scene = ram_w(W_SCENE);
    unsigned table = rom_long((plane ? TAB_BG : TAB_FG) + scene * 4u);
    unsigned base  = 0;                     /* image-relative, not VRAM */
    uint16_t tile_x, tile_y, ox, oy;
    int row, col;

    /* 0x26ADA derives tile space from the latched scroll pair. */
    tile_x = (uint16_t)(ram_w(W_SCROLL_X) >> 5);
    tile_y = (uint16_t)(ram_w(W_SCROLL_Y) >> 5);
    /* 0x26EA6's own bias before the walk: +4 on Y, -3 on X. */
    ox = (uint16_t)(tile_x - 3);
    oy = (uint16_t)(tile_y + 4);

    wf_arena_tiles_for_scene(scene);                  /* imported arena: its tiles + its planes */
    if (wf_arena_compose(out, outsize, plane, scene, ox, oy)) return;

    memset(out, 0, outsize);
    for (row = 0; row < 16; row++) {
        for (col = 0; col < 16; col++) {
            uint16_t x = (uint16_t)(ox + col);
            uint16_t y = (uint16_t)(oy - row);
            unit_copy(out, outsize,
                      unit_dst(base, x, y, plane),
                      unit_src(table, x, y, plane), plane);
        }
    }
}

/* diagnostic for the arena export: the 2x2 cells of one source unit of a
 * scene (ROM bytes, FG 16 / BG 8), 0 = the unit has no source */
int wf_scene_unit_cells(unsigned scene, int plane, uint16_t x, uint16_t y, uint8_t *out16)
{
    unsigned table = rom_long((plane ? TAB_BG : TAB_FG) + scene * 4u);
    unsigned src = unit_src(table, x, y, plane);
    const unsigned n = plane ? 2u : 4u, srow = plane ? BG_SRC_ROW : FG_SRC_ROW;
    for (unsigned i = 0; i < 2; i++) {
        const uint8_t *sp = tbl_ra_ptr(src + i * srow, n * 2u);
        if (!sp) return 0;
        memcpy(out16 + i * n * 2u, sp, n * 2u);
    }
    return 1;
}

void wf_scene_compose_fg(uint8_t *out, unsigned outsize)
{
    compose_plane(out, outsize, 0);
}

void wf_scene_compose_bg(uint8_t *out, unsigned outsize)
{
    compose_plane(out, outsize, 1);
}

/* ---- faithful form, for the seam -------------------------------------
 *
 * The check above only has to get the tilemap right. A replacement has to get
 * everything the routine *leaves behind* right as well, because exact.sh
 * compares work RAM byte for byte. That includes things with no bearing on
 * the picture: the window origin at 0x1C17E2/0x1C17E4 as the loop left it,
 * the direction code 0x1C16D4, the dirty bits 0x1C16D2, and the two 128-byte
 * (src,dst) row buffers at 0x1C16D6 and 0x1C1756, which keep whatever the
 * final row put there.
 *
 * So this is a transliteration rather than a tidy reimplementation. Where the
 * two disagree, the assembly wins. */

#define W_DIRTY      0x16D2
#define W_DIR        0x16D4
#define W_ROWBUF_FG  0x16D6
#define W_ROWBUF_BG  0x1756
#define W_LASTX_FG   0x17DA
#define W_LASTY_FG   0x17DC
#define W_LASTX_BG   0x17DE
#define W_LASTY_BG   0x17E0
#define W_ORIGIN_X   0x17E2
#define W_ORIGIN_Y   0x17E4
#define W_SX0        0x17E6
#define W_SX1        0x17EA
#define W_SY0        0x17EE
#define W_SY1        0x17F2
#define W_CUR_X      0x17F6
#define W_CUR_Y      0x17FA
#define W_PLANE      0x17FE
#define W_FLAGS      0x006F
#define W_MARK       0x185A
#define TAB_PRI      0x26E9E

static void ram_setw(unsigned off, uint16_t v)
{
    wf.work_ram[off]     = (uint8_t)(v >> 8);
    wf.work_ram[off + 1] = (uint8_t)v;
}

static void ram_setl(unsigned off, unsigned v)
{
    wf.work_ram[off]     = (uint8_t)(v >> 24);
    wf.work_ram[off + 1] = (uint8_t)(v >> 16);
    wf.work_ram[off + 2] = (uint8_t)(v >> 8);
    wf.work_ram[off + 3] = (uint8_t)v;
}

/* 0x26ADA. Also sets the direction code the incremental scroll path reads. */
static void tile_coords(void)
{
    uint16_t d1 = (uint16_t)(ram_w(W_CUR_X) >> 5);
    uint16_t d2 = (uint16_t)(ram_w(W_CUR_Y) >> 5);
    uint16_t code;

    ram_setw(W_TILE_X, d1);
    ram_setw(W_TILE_Y, d2);

    if (d1 != ram_w(W_ORIGIN_X))
        code = (d1 > ram_w(W_ORIGIN_X)) ? 1u : 3u;
    else if (d2 != ram_w(W_ORIGIN_Y))
        code = (d2 > ram_w(W_ORIGIN_Y)) ? 0u : 2u;
    else
        code = 0xFFu;
    ram_setw(W_DIR, code);
}

/* 0x26E1A. Bit 4 of 0x1C006F suppresses the register write entirely. */
void wf_scene_write_scroll_regs(void)
{
    if (wf.work_ram[W_FLAGS] & 0x10)
        return;
    m68k_write_memory_16(0x100000, ram_w(W_SX0));
    m68k_write_memory_16(0x100002, (uint16_t)(-(uint16_t)(ram_w(W_SY0) - 0x800)));
    m68k_write_memory_16(0x100004, ram_w(W_SX1));
    m68k_write_memory_16(0x100006, (uint16_t)(-(uint16_t)(ram_w(W_SY1) - 0x800)));
}

/* 0x26DAC, driven by the dirty bits 0x26B4A set. Both planes are tested on
 * every call; only the one just marked actually copies. */
void wf_scene_flush_row(void)
{
    unsigned i;

    if (wf.work_ram[W_DIRTY] & 0x80) {
        wf.work_ram[W_DIRTY] &= (uint8_t)~0x80;
        for (i = 0; i < 16; i++) {
            unsigned src = ram_l(W_ROWBUF_FG + i * 8);
            unsigned dst = ram_l(W_ROWBUF_FG + i * 8 + 4);
            copy_long(dst,        src);
            copy_long(dst + 4,    src + 4);
            copy_long(dst + 0x80, src + 0x50);
            copy_long(dst + 0x84, src + 0x54);
        }
    }
    if (wf.work_ram[W_DIRTY] & 0x40) {
        wf.work_ram[W_DIRTY] &= (uint8_t)~0x40;
        for (i = 0; i < 16; i++) {
            unsigned src = ram_l(W_ROWBUF_BG + i * 8);
            unsigned dst = ram_l(W_ROWBUF_BG + i * 8 + 4);
            copy_word(dst,        src);
            copy_word(dst + 2,    src + 2);
            copy_word(dst + 0x40, src + 0x28);
            copy_word(dst + 0x42, src + 0x2A);
        }
    }
}

/* 0x26EA6 for the plane currently selected by bit 7 of 0x1C17FE. */
static void compose_plane_live(void)
{
    int plane = (wf.work_ram[W_PLANE] & 0x80) ? 1 : 0;
    unsigned scene = ram_w(W_SCENE);
    unsigned table = rom_long((plane ? TAB_BG : TAB_FG) + scene * 4u);
    unsigned vram  = plane ? WF_BGRAM_BASE : WF_FGRAM_BASE;
    unsigned buf   = plane ? W_ROWBUF_BG : W_ROWBUF_FG;
    int row, col;

    ram_setw(W_CUR_X, ram_w(W_SCROLL_X));
    ram_setw(W_CUR_Y, ram_w(W_SCROLL_Y));
    ram_setw(W_SX0, ram_w(W_CUR_X));
    ram_setw(W_SX1, ram_w(W_CUR_X));
    ram_setw(W_SY0, ram_w(W_CUR_Y));
    ram_setw(W_SY1, ram_w(W_CUR_Y));

    tile_coords();
    ram_setw(W_ORIGIN_X, ram_w(W_TILE_X));
    ram_setw(W_ORIGIN_Y, ram_w(W_TILE_Y));
    wf_scene_write_scroll_regs();
    ram_setw(W_ORIGIN_Y, (uint16_t)(ram_w(W_ORIGIN_Y) + 4));
    ram_setw(W_ORIGIN_X, (uint16_t)(ram_w(W_ORIGIN_X) - 3));

    for (row = 0; row < 16; row++) {
        for (col = 0; col < 16; col++) {
            uint16_t x = ram_w(W_ORIGIN_X), y = ram_w(W_ORIGIN_Y);
            ram_setl(buf + (unsigned)col * 8,
                     unit_src(table, x, y, plane));
            ram_setl(buf + (unsigned)col * 8 + 4,
                     unit_dst(vram, x, y, plane));
            ram_setw(W_ORIGIN_X, (uint16_t)(x + 1));
        }
        ram_setw(W_ORIGIN_X, (uint16_t)(ram_w(W_ORIGIN_X) - 0x10));
        /* 0x26B4A */
        wf.work_ram[W_DIRTY] |= (uint8_t)(plane ? 0x40 : 0x80);
        wf_scene_flush_row();
        ram_setw(W_ORIGIN_Y, (uint16_t)(ram_w(W_ORIGIN_Y) - 1));
    }

    /* 0x26F54: all four are written on every pass, not one pair per plane. */
    tile_coords();
    ram_setw(W_LASTX_FG, ram_w(W_TILE_X));
    ram_setw(W_LASTY_FG, ram_w(W_TILE_Y));
    ram_setw(W_LASTX_BG, ram_w(W_TILE_X));
    ram_setw(W_LASTY_BG, ram_w(W_TILE_Y));
}

/* ROM 0x26E66 itself. */
void wf_scene_run(void)
{
    unsigned scene = ram_w(W_SCENE);

    wf.work_ram[W_PLANE] &= (uint8_t)~0x80;
    compose_plane_live();
    wf.work_ram[W_PLANE] |= 0x80;
    compose_plane_live();
    wf_arena_tiles_for_scene(scene);
    if (wf_arena_has(scene)) {         /* imported arena: its planes replace the ROM walk,
                                          written through the bus so the tilemap shadow follows */
        static uint8_t fgb[WF_FGRAM_SIZE], bgb[WF_BGRAM_SIZE];
        uint16_t ox = (uint16_t)((ram_w(W_SCROLL_X) >> 5) - 3), oy = (uint16_t)((ram_w(W_SCROLL_Y) >> 5) + 4);
        wf_arena_compose(fgb, sizeof fgb, 0, scene, ox, oy);
        wf_arena_compose(bgb, sizeof bgb, 1, scene, ox, oy);
        for (unsigned o = 0; o + 1 < WF_FGRAM_SIZE; o += 2) m68k_write_memory_16(WF_FGRAM_BASE + o, (uint16_t)((fgb[o] << 8) | fgb[o + 1]));
        for (unsigned o = 0; o + 1 < WF_BGRAM_SIZE; o += 2) m68k_write_memory_16(WF_BGRAM_BASE + o, (uint16_t)((bgb[o] << 8) | bgb[o + 1]));
    }
    wf.work_ram[W_MARK] = 0xFF;
    m68k_write_memory_16(0x140010,
                         (uint16_t)((scene & 0xFF00u)
                                    | tbl_ra8(TAB_PRI + (unsigned)wf_arena_base_scene(scene & 0xFFFFu))));   /* an override: its base ring's priority */
}



/* ---- C-owned tilemap shadow -------------------------------------------
 *
 * Ownership without touching timing. The 68k composer keeps running — it has
 * to, since it is interruptible and ~66 IRQs land inside one call — but the
 * renderer stops reading its VRAM and reads this buffer instead.
 *
 * Two sources feed the shadow:
 *
 *   the C compose, on a scene change, which fills the whole 32x32 base
 *   immediately instead of four frames later; and
 *   every 68k store into a tilemap, mirrored in, which is what preserves the
 *   edits that land *after* a compose — the HUD text and banners behind the
 *   39/191/223-byte deltas scene_map's check reports.
 *
 * Nothing here writes wf.fg_videoram or wf.bg_videoram. That is deliberate:
 * the live VRAM is what exact.sh, stock-parity and the traces sample, so
 * keeping the shadow renderer-side means this whole change cannot move a
 * gate. What it moves is the picture, and only by making a background appear
 * at the scene change rather than four frames into it.
 */

static uint8_t fg_shadow[WF_FGRAM_SIZE];
static uint8_t bg_shadow[WF_BGRAM_SIZE];
static int shadow_valid;
static int shadow_scene = -1;

int wf_tilemap_shadow_active(void)
{
    /* The C select and the C menu paint wf.fg_videoram / wf.bg_videoram
     * *directly* rather than through the bus, so the mirror below never sees
     * those stores. If the shadow stayed live it would draw the composed
     * scene background straight over their screens. They already own every
     * layer while they run, so the shadow stands down for them and the
     * renderer reads VRAM, which is where their pixels actually are. */
    if (wf_select_active() || wf_menu_active())
        return 0;
    return !wf_layer_arcade();
}

/* Mirror a 68k tilemap store. Called from the bus after the store itself, so
 * the shadow tracks VRAM for everything C did not author. */
void wf_tilemap_shadow_write(unsigned address, unsigned value, int is_word)
{
    uint8_t *dst;
    unsigned off;

    if (!shadow_valid || !wf_tilemap_shadow_active())
        return;
    if (address - WF_FGRAM_BASE < WF_FGRAM_SIZE) {
        dst = fg_shadow;
        off = address - WF_FGRAM_BASE;
    } else if (address - WF_BGRAM_BASE < WF_BGRAM_SIZE) {
        dst = bg_shadow;
        off = address - WF_BGRAM_BASE;
    } else {
        return;
    }
    if (is_word) {
        dst[off & ~1u]       = (uint8_t)(value >> 8);
        dst[(off & ~1u) + 1] = (uint8_t)value;
    } else {
        dst[off] = (uint8_t)value;
    }
}

/* Called once per video latch, before the renderer copies. */
void wf_tilemap_shadow_latch(uint8_t *fg_out, uint8_t *bg_out)
{
    int scene;

    if (!wf_tilemap_shadow_active()) {
        shadow_valid = 0;
        shadow_scene = -1;
        return;
    }
    scene = (int)ram_w(W_SCENE);
    /* Recompose only on a scene change. The 68k does not recompose while
     * scrolling either — it patches the incoming edge through 0x26C8A — so
     * recomposing on a scroll delta would overwrite a correctly scrolled
     * map with a fresh window and fight the hardware every frame. */
    if (!shadow_valid || scene != shadow_scene) {
        wf_scene_compose_fg(fg_shadow, sizeof fg_shadow);
        wf_scene_compose_bg(bg_shadow, sizeof bg_shadow);
        shadow_valid = 1;
        shadow_scene = scene;
    }
    if (getenv("WF_TILEMAP_CHECK")) {
        /* The shadow should equal live VRAM everywhere except the window
         * where C has already composed and the 68k has not caught up. A
         * mismatch that persists means the mirror is missing a write path. */
        static int last = -1;
        unsigned i, d = 0;
        for (i = 0; i < 32u * 32u * 4u; i++)
            if (fg_shadow[i] != wf.fg_videoram[i])
                d++;
        for (i = 0; i < 32u * 32u * 2u; i++)
            if (bg_shadow[i] != wf.bg_videoram[i])
                d++;
        if ((d == 0) != (last == 0)) {
            fprintf(stderr, "TILEMAP f=%ld scene=%d shadow_vs_vram=%u\n",
                    wf.frame, scene, d);
            last = (int)d;
        }
    }
    memcpy(fg_out, fg_shadow, WF_FGRAM_SIZE);
    memcpy(bg_out, bg_shadow, WF_BGRAM_SIZE);
}

/* Engine hook: take the tilemaps the engine just composed into VRAM
 * (render.c compose()) as the shadow, so a recompose on a scroll-window
 * change — the character select's page scroll, a same-scene window move
 * between the game select and the character select — is what gets drawn. */
void wf_tilemap_shadow_adopt(void)
{
    memcpy(fg_shadow, wf.fg_videoram, WF_FGRAM_SIZE);
    memcpy(bg_shadow, wf.bg_videoram, WF_BGRAM_SIZE);
    shadow_valid = 1;
    shadow_scene = (int)ram_w(W_SCENE);
}
