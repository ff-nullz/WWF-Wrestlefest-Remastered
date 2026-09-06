/* SDL2 / software renderer. Layouts and compose order are from
 * mame/technos/ddragon3_v.cpp (screen_update_wwfwfest, draw_sprites).
 * Palette is xBGR_444. fg0 is drawn 8px into the visarea because the
 * screen clips lines 8..248. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <png.h>

#include "wf.h"
#include "pak.h"
#include "assetlog.h"
#include "ext_wrestler.h"
#include "menu.h"
#include "layer.h"
#include "version.h"
#include "profile.h"

#define FG0_TILES  4096
#define BG_TILES   4096
/* 0x0000-0xFFFF = the ROM sprite space (8MB, fully packed - only 112
 * blank tiles); 0x10000+ = the CLONE-ART ARENA: 32 slots x 0x6000 tiles
 * for new-art clone wrestlers (tools --retile / --art-ingest; slot N
 * base = 0x10000 + (N-12)*0x6000; the slot's select-cell portrait rides
 * the arena's TOP 32 tiles). Tile bits 16-18 travel in the sprite
 * record's unused word 6 behind a 2-byte magic (a full re-cut wrestler
 * runs ~6.2-7k tiles, so an arena is 8192). */
#define SPR_TILES  0xEC000          /* + the BADGE tile arena 0xEA000-0xEB000 (build/badges.pak, 2026-08-29) */ /* + 12 stock-skin arenas of 0x2000 above the clone arenas (2026-08-26), + the WEAPON tile arena 0xE8000-0xEA000 (pak-ingested weapon art, 2026-08-27) */
#define SPR_ROM_TILES 0x10000      /* the gfx pak carries only the ROM tile space; arenas come from wrestler paks */

static uint8_t fg0_pen[FG0_TILES][8 * 8];
static uint8_t tile_pen[BG_TILES][16 * 16];
static uint8_t spr_pen[SPR_TILES][16 * 16];
static uint8_t spr_have[(SPR_TILES + 7) / 8];

/* Offline stills for ./wfgame. Default off — exact.sh must not see this.
 *   WF_BAKE_ARENA  tiles + bank-14 ropes; no wrestlers, no FG0
 *   WF_BAKE_TILES  FG/BG only (mat, crowd, apron)
 *   WF_BAKE_ROPES  bank-14 sprites only (posts, ropes) on black */
static int bake_env(const char *name)
{
    const char *e = getenv(name);

    return e && *e && strcmp(e, "0") != 0;
}

static int bake_arena(void)
{
    static int on = -1;

    if (on < 0)
        on = bake_env("WF_BAKE_ARENA");
    return on;
}

static int bake_tiles(void)
{
    static int on = -1;

    if (on < 0)
        on = bake_env("WF_BAKE_TILES");
    return on;
}

static int bake_ropes(void)
{
    static int on = -1;

    if (on < 0)
        on = bake_env("WF_BAKE_ROPES");
    return on;
}
static uint8_t *spr_rom;
static size_t spr_rom_len;
static int video_ready;

static const char *gfx1_chip[] = { "31e12-0.ic33" };
static const char *gfx3_chips[] = { "31j1.ic2", "31j0.ic1" };
static const char *gfx2_chips[] = {
    "31j3.ic9", "31j2.ic8", "31j5.ic11", "31j4.ic10",
    "31j6.ic12", "31j7.ic13", "31j9.ic15", "31j8.ic14"
};

static FILE *open_gfx_chip(const char *dir, const char *name, char *path, size_t path_sz)
{
    snprintf(path, path_sz, "data/gfx/%s", name);
    FILE *f = fopen(path, "rb");
    if (f)
        return f;
    if (dir && dir[0]) {
        snprintf(path, path_sz, "%s/%s", dir, name);
        f = fopen(path, "rb");
    }
    return f;
}

static uint8_t *load_concat(const char *dir, const char **names, int n, size_t *out_len)
{
    size_t total = 0;
    uint8_t *blob = NULL;
    for (int i = 0; i < n; i++) {
        char path[512];
        FILE *f = open_gfx_chip(dir, names[i], path, sizeof path);
        if (!f) {
            fprintf(stderr, "video: cannot open data/gfx/%s or %s/%s\n",
                    names[i], dir ? dir : "", names[i]);
            free(blob);
            return NULL;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        blob = realloc(blob, total + (size_t)sz);
        if (!blob || fread(blob + total, 1, (size_t)sz, f) != (size_t)sz) {
            fprintf(stderr, "video: short read %s\n", path);
            fclose(f);
            free(blob);
            return NULL;
        }
        total += (size_t)sz;
        fclose(f);
    }
    *out_len = total;
    return blob;
}

static int bit_at(const uint8_t *data, size_t len, unsigned bit)
{
    unsigned byte = bit >> 3;
    if (byte >= len)
        return 0;
    return (data[byte] >> (7 - (bit & 7))) & 1;
}

static void decode_fg0(const uint8_t *data, size_t len)
{
    static const int planes[] = { 0, 2, 4, 6 };
    static const int xs[] = { 1, 0, 65, 64, 129, 128, 193, 192 };
    for (int t = 0; t < FG0_TILES; t++) {
        unsigned base = (unsigned)t * 256;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int v = 0;
                for (int p = 0; p < 4; p++) {
                    unsigned bit = base + (unsigned)planes[p] + (unsigned)(y * 8) + (unsigned)xs[x];
                    v |= bit_at(data, len, bit) << (3 - p);
                }
                fg0_pen[t][y * 8 + x] = (uint8_t)v;
            }
    }
}

static void decode_tiles(const uint8_t *data, size_t len)
{
    unsigned half = (unsigned)(len * 8 / 2);
    static const int xs[] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        256, 257, 258, 259, 260, 261, 262, 263
    };
    static const int ys[] = {
        0, 16, 32, 48, 64, 80, 96, 112,
        128, 144, 160, 176, 192, 208, 224, 240
    };
    int planes[4] = { 8, 0, (int)half + 8, (int)half };
    for (int t = 0; t < BG_TILES; t++) {
        unsigned base = (unsigned)t * 512;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int v = 0;
                for (int p = 0; p < 4; p++) {
                    unsigned bit = base + (unsigned)planes[p] + (unsigned)ys[y] + (unsigned)xs[x];
                    v |= bit_at(data, len, bit) << (3 - p);
                }
                tile_pen[t][y * 16 + x] = (uint8_t)v;
            }
    }
}

void wf_video_fg0_write(unsigned code, const uint8_t *in)
{
    if (code < FG0_TILES)
        memcpy(fg0_pen[code], in, 8 * 8);
}

static void decode_one_sprite(unsigned t)
{
    unsigned quarter;
    if (!spr_rom || !spr_rom_len)
        return;
    quarter = (unsigned)(spr_rom_len * 8 / 4);
    unsigned planes[4] = { 0, quarter, 2 * quarter, 3 * quarter };
    static const int xs[] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        128, 129, 130, 131, 132, 133, 134, 135
    };
    unsigned base = t * 256;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int v = 0;
            for (int p = 0; p < 4; p++) {
                unsigned bit = base + planes[p] + (unsigned)(y * 8) + (unsigned)xs[x];
                v |= bit_at(spr_rom, spr_rom_len, bit) << (3 - p);
            }
            spr_pen[t][y * 16 + x] = (uint8_t)v;
        }
    spr_have[t >> 3] |= (uint8_t)(1u << (t & 7));
}

/* WF_SPRITE_LOG: record which sprite tiles actually reach the screen, so a
 * differential run (same scenario, different WF_SELECT_ROSTER) can attribute
 * tile ranges to a wrestler. sprite_tile() is the only path by which a sprite
 * pixel is read, so this sees everything and nothing else. Off unless the env
 * var is set; the bitmap costs 8KB. */
static uint8_t spr_used[(SPR_TILES + 7) / 8];
static int spr_log_on = -1;
static const uint8_t *sprite_tile(unsigned t);
const uint8_t *wf_video_tile_pens(unsigned t) { return sprite_tile(t); }
void wf_video_set_tile_pens(unsigned t, const uint8_t *p)
{
    if (t >= SPR_TILES) return;
    memcpy(spr_pen[t], p, 16 * 16);
    spr_have[t >> 3] |= (uint8_t)(1u << (t & 7));
}

const uint8_t *wf_video_tile_pens(unsigned t);
void wf_video_set_tile_pens(unsigned t, const uint8_t *p);

static const uint8_t *sprite_tile(unsigned t)
{
    if (t >= SPR_TILES)
        return spr_pen[0];
    if (spr_log_on < 0)
        spr_log_on = getenv("WF_SPRITE_LOG") ? 1 : 0;
    if (spr_log_on)
        spr_used[t >> 3] |= (uint8_t)(1u << (t & 7));
    if (!(spr_have[t >> 3] & (1u << (t & 7))))
        decode_one_sprite(t);
    return spr_pen[t];
}

/* Dump the used-tile bitmap. Raw bits, tile t at byte t/8 bit t%8. */
void wf_video_sprite_log_dump(void)
{
    const char *path = getenv("WF_SPRITE_LOG");
    FILE *f;
    long n = 0;

    if (!path || !*path)
        return;
    if (!(f = fopen(path, "wb"))) {
        fprintf(stderr, "WF_SPRITE_LOG: cannot write %s\n", path);
        return;
    }
    fwrite(spr_used, 1, sizeof spr_used, f);
    fclose(f);
    for (unsigned t = 0; t < SPR_TILES; t++)
        if (spr_used[t >> 3] & (1u << (t & 7)))
            n++;
    fprintf(stderr, "WF_SPRITE_LOG: %ld distinct sprite tiles drawn -> %s\n",
            n, path);
}

int wf_video_load_indexed_png(const char *path, uint8_t **out, int *w, int *h)
{
    FILE *f;
    png_structp png;
    png_infop info;
    png_uint_32 width, height;
    int bit, color, interlace, i;
    png_bytep *rows;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "video: missing %s\n", path);
        return -1;
    }
    wf_asset_log("gfx", path);
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        fprintf(stderr, "video: bad PNG %s\n", path);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
    }
    png_init_io(png, f);
    png_read_info(png, info);
    png_get_IHDR(png, info, &width, &height, &bit, &color, &interlace,
                 NULL, NULL);
    if (color == PNG_COLOR_TYPE_PALETTE && bit < 8)
        png_set_packing(png);
    else if (color == PNG_COLOR_TYPE_GRAY && bit < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    else if (color != PNG_COLOR_TYPE_PALETTE && color != PNG_COLOR_TYPE_GRAY) {
        fprintf(stderr, "video: %s is not indexed/grey (edit as 16-pen PNG)\n",
                path);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
    }
    if (bit == 16)
        png_set_strip_16(png);
    png_read_update_info(png, info);
    *w = (int)width;
    *h = (int)height;
    *out = malloc((size_t)width * (size_t)height);
    rows = malloc(sizeof *rows * height);
    if (!*out || !rows) {
        free(*out);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
    }
    {
        size_t rb = png_get_rowbytes(png, info);
        png_bytep raw = malloc(rb * height);
        if (!raw) {
            free(*out);
            free(rows);
            png_destroy_read_struct(&png, &info, NULL);
            fclose(f);
            return -1;
        }
        for (i = 0; i < (int)height; i++)
            rows[i] = raw + (size_t)i * rb;
        png_read_image(png, rows);
        for (i = 0; i < (int)height; i++)
            memcpy(*out + (size_t)i * width, rows[i], (size_t)width);
        free(raw);
    }
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return 0;
}

/* QUIET libpng: the editor polls art PNGs while tools (and plain `cp`) are
 * still writing them; a short read is an expected miss retried next tick,
 * not "libpng error: Read Error" on the terminal (user 2026-08-26). */
static void png_quiet_err(png_structp png, png_const_charp msg) { (void)msg; png_longjmp(png, 1); }
static void png_quiet_warn(png_structp png, png_const_charp msg) { (void)png; (void)msg; }

int wf_video_load_rgba_png(const char *path, uint8_t **rgba, int *w, int *h)
{
    FILE *f;
    png_structp png;
    png_infop info;
    png_uint_32 width, height;
    int bit, color, interlace, i;
    png_bytep *rows;

    *rgba = NULL;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    wf_asset_log("gfx", path);
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_quiet_err, png_quiet_warn);
    info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
    }
    png_init_io(png, f);
    png_read_info(png, info);
    png_get_IHDR(png, info, &width, &height, &bit, &color, &interlace,
                 NULL, NULL);
    if (bit == 16)
        png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && bit < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_RGB || color == PNG_COLOR_TYPE_GRAY ||
        color == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);
    *w = (int)width;
    *h = (int)height;
    *rgba = malloc((size_t)width * (size_t)height * 4u);
    rows = malloc(sizeof *rows * height);
    if (!*rgba || !rows) {
        free(*rgba);
        *rgba = NULL;
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
    }
    for (i = 0; i < (int)height; i++)
        rows[i] = *rgba + (size_t)i * (size_t)width * 4u;
    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return 0;
}

static int unsheet(const uint8_t *img, int iw, int ih, int tw, int th,
                   int columns, uint8_t *dst, int dst_stride, int count)
{
    int t;
    if (iw != columns * tw)
        return -1;
    for (t = 0; t < count; t++) {
        int col = t % columns;
        int row = t / columns;
        int y;
        if ((row + 1) * th > ih)
            return -1;
        for (y = 0; y < th; y++)
            memcpy(dst + t * dst_stride + y * tw,
                   img + (row * th + y) * iw + col * tw, (size_t)tw);
    }
    return 0;
}

/* gfx source file: the active profile's mod layers first (profile.h) */
static const char *gfx_src(const char *rel, char *buf, size_t n)
{
    char r2[128];
    snprintf(r2, sizeof r2, "gfx-edit/%s", rel);
    if (wf_mod_resolve(r2, buf, n)) return buf;
    snprintf(buf, n, "data/gfx-edit/%s", rel);
    return buf;
}

static int load_gfx_edit(void)
{
    char srcbuf[512];
    uint8_t *img = NULL;
    int w = 0, h = 0, i;

    if (wf_video_load_indexed_png(gfx_src("fg0.png", srcbuf, sizeof srcbuf), &img, &w, &h) != 0)
        return -1;
    if (unsheet(img, w, h, 8, 8, 64, fg0_pen[0], 8 * 8, FG0_TILES) != 0) {
        fprintf(stderr, "video: fg0.png is not a 64x64 sheet of 8x8 tiles\n");
        free(img);
        return -1;
    }
    free(img);

    if (wf_video_load_indexed_png(gfx_src("tiles.png", srcbuf, sizeof srcbuf), &img, &w, &h) != 0)
        return -1;
    if (unsheet(img, w, h, 16, 16, 64, tile_pen[0], 16 * 16, BG_TILES) != 0) {
        fprintf(stderr, "video: tiles.png is not a 64x64 sheet of 16x16 tiles\n");
        free(img);
        return -1;
    }
    free(img);

    memset(spr_have, 0xff, sizeof spr_have);
    for (i = 0; i < 256; i++) {
        char path[64];
        {   char rel[64]; snprintf(rel, sizeof rel, "sprites/sheet_%04d.png", i);
            snprintf(path, sizeof path, "%s", gfx_src(rel, srcbuf, sizeof srcbuf)); }
        if (wf_video_load_indexed_png(path, &img, &w, &h) != 0)
            return -1;
        if (unsheet(img, w, h, 16, 16, 16, spr_pen[i * 256], 16 * 16, 256) != 0) {
            fprintf(stderr, "video: %s is not a 16x16 sheet of 16x16 tiles\n",
                    path);
            free(img);
            return -1;
        }
        free(img);
    }
    free(spr_rom);
    spr_rom = NULL;
    spr_rom_len = 0;
    fprintf(stderr, "video: loaded fg0/tiles/sprites from data/gfx-edit\n");
    return 0;
}

/* `wfengine --verify-gfx [rom_dir]` (ADR-001 rule 2 for graphics): decode
 * the three gfx chip sets with the decoders above and compare pen-for-pen
 * with what data/gfx-edit/*.png loaded. 0 = identical. */
int wf_video_verify_gfx(const char *rom_dir)
{
    uint8_t *snap_fg0 = malloc(sizeof fg0_pen), *snap_tile = malloc(sizeof tile_pen), *snap_spr = malloc(sizeof spr_pen);
    uint8_t *blob; size_t len; int bad = 0;
    if (!snap_fg0 || !snap_tile || !snap_spr) { fprintf(stderr, "verify-gfx: oom\n"); return 1; }
    if (load_gfx_edit() != 0) { fprintf(stderr, "verify-gfx: data/gfx-edit did not load\n"); return 1; }
    memcpy(snap_fg0, fg0_pen, sizeof fg0_pen);
    memcpy(snap_tile, tile_pen, sizeof tile_pen);
    memcpy(snap_spr, spr_pen, sizeof spr_pen);

    blob = load_concat(rom_dir, gfx1_chip, 1, &len);
    if (!blob) return 1;
    decode_fg0(blob, len); free(blob);
    for (int t = 0; t < FG0_TILES; t++)
        if (memcmp(fg0_pen[t], snap_fg0 + t * 64, 64)) { if (bad < 10) fprintf(stderr, "verify-gfx: fg0 tile %d differs\n", t); bad++; }
    fprintf(stderr, "verify-gfx: fg0 %d tiles, %d differ\n", FG0_TILES, bad);

    blob = load_concat(rom_dir, gfx3_chips, 2, &len);
    if (!blob) return 1;
    decode_tiles(blob, len); free(blob);
    { int b2 = 0;
      for (int t = 0; t < BG_TILES; t++)
          if (memcmp(tile_pen[t], snap_tile + t * 256, 256)) { if (b2 < 10) fprintf(stderr, "verify-gfx: bg/fg tile %d differs\n", t); b2++; }
      fprintf(stderr, "verify-gfx: tiles %d, %d differ\n", BG_TILES, b2); bad += b2; }

    spr_rom = load_concat(rom_dir, gfx2_chips, 8, &spr_rom_len);
    if (!spr_rom) return 1;
    { int b3 = 0;
      for (unsigned t = 0; t < SPR_TILES; t++) {
          decode_one_sprite(t);
          if (memcmp(spr_pen[t], snap_spr + (size_t)t * 256, 256)) { if (b3 < 10) fprintf(stderr, "verify-gfx: sprite tile %u differs\n", t); b3++; }
      }
      fprintf(stderr, "verify-gfx: sprites %d, %d differ\n", SPR_TILES, b3); bad += b3; }
    free(spr_rom); spr_rom = NULL; spr_rom_len = 0;
    memcpy(fg0_pen, snap_fg0, sizeof fg0_pen); memcpy(tile_pen, snap_tile, sizeof tile_pen); memcpy(spr_pen, snap_spr, sizeof spr_pen);
    free(snap_fg0); free(snap_tile); free(snap_spr);
    fprintf(stderr, "verify-gfx: %s\n", bad ? "MISMATCH" : "data/gfx-edit is byte-identical to the ROM chips");
    return bad ? 1 : 0;
}

/* build/gfx.pak: sections fg0 (4096 x 64 pens), tiles (4096 x 256), sprites
 * (65536 x 256) — the decoded tile sets, written by `--pack` from the
 * data/gfx-edit PNGs, read at start-up instead of decoding 258 PNGs. */
/* --export-gfx PAK (tools/bootstrap.sh): build/gfx.pak straight from the
 * ROM chips - a fresh clone has no data/gfx-edit PNGs (they are the ROM's
 * art and are not distributed); the same three decoders --verify-gfx
 * checks the PNGs against. The PNG tree stays the modder's editable
 * source: --pack rebuilds the pak from it whenever it exists. */
int wf_video_export_gfx_pak(const char *rom_dir, const char *pak_path)
{
    uint8_t *blob; size_t len; pak_writer *w; int rc;
    blob = load_concat(rom_dir, gfx1_chip, 1, &len);
    if (!blob) return 1;
    decode_fg0(blob, len); free(blob);
    blob = load_concat(rom_dir, gfx3_chips, 2, &len);
    if (!blob) return 1;
    decode_tiles(blob, len); free(blob);
    spr_rom = load_concat(rom_dir, gfx2_chips, 8, &spr_rom_len);
    if (!spr_rom) return 1;
    for (unsigned t = 0; t < SPR_ROM_TILES; t++) decode_one_sprite(t);
    free(spr_rom); spr_rom = NULL; spr_rom_len = 0;
    mkdir("build", 0775);
    w = pak_writer_new();
    pak_writer_add(w, "fg0", fg0_pen[0], sizeof fg0_pen);
    pak_writer_add(w, "tiles", tile_pen[0], sizeof tile_pen);
    pak_writer_add(w, "sprites", spr_pen[0], (uint32_t)SPR_ROM_TILES * 256u);
    rc = pak_writer_save(w, pak_path);
    pak_writer_free(w);
    fprintf(stderr, "export-gfx: %s -> %s%s\n", rom_dir, pak_path, rc ? " FAILED" : "");
    return rc;
}

int wf_video_pack_gfx(const char *pak_path)
{
    pak_writer *w;
    int rc;
    {   /* no PNG source tree (a bootstrapped clone): the pak --export-gfx wrote stands */
        char srcbuf[512]; FILE *f = fopen(gfx_src("fg0.png", srcbuf, sizeof srcbuf), "rb");
        if (!f) { FILE *pk = fopen(pak_path, "rb"); if (pk) { fclose(pk); fprintf(stderr, "pack: gfx - no data/gfx-edit, keeping %s (from --export-gfx)\n", pak_path); return 0; } }
        else fclose(f);
    }
    if (load_gfx_edit() != 0) return 1;
    w = pak_writer_new();
    pak_writer_add(w, "fg0", fg0_pen[0], sizeof fg0_pen);
    pak_writer_add(w, "tiles", tile_pen[0], sizeof tile_pen);
    pak_writer_add(w, "sprites", spr_pen[0], (uint32_t)SPR_ROM_TILES * 256u);   /* ROM tiles only: 16 MB, not 219 */
    rc = pak_writer_save(w, pak_path);
    pak_writer_free(w);
    fprintf(stderr, "pack: gfx -> %s%s\n", pak_path, rc ? " FAILED" : "");
    return rc;
}

static int load_gfx_pak(const char *path)
{
    pak *p = pak_open(path);
    const uint8_t *sec; uint32_t len;
    if (!p) return -1;
    if (!(sec = pak_section(p, "fg0", &len)) || len != sizeof fg0_pen) { pak_close(p); return -1; }
    memcpy(fg0_pen, sec, len);
    if (!(sec = pak_section(p, "tiles", &len)) || len != sizeof tile_pen) { pak_close(p); return -1; }
    memcpy(tile_pen, sec, len);
    if (!(sec = pak_section(p, "sprites", &len)) || len > sizeof spr_pen || len % 256) { pak_close(p); return -1; }
    memcpy(spr_pen, sec, len);              /* an older full-size pak still loads */
    memset(spr_have, 0xff, sizeof spr_have);
    pak_close(p);
    free(spr_rom); spr_rom = NULL; spr_rom_len = 0;
    fprintf(stderr, "video: loaded fg0/tiles/sprites from %s\n", path);
    return 0;
}

int wf_video_init(const char *rom_dir)
{
    const char *mode = getenv("WF_DATA") ? getenv("WF_DATA") : "auto";
    const char *gfx_pak = getenv("WF_GFXPAK") ? getenv("WF_GFXPAK") : "build/gfx.pak";
    (void)rom_dir;
    if (strcmp(mode, "rom") == 0 || load_gfx_pak(gfx_pak) != 0) {
        if (!strcmp(mode, "pak")) { fprintf(stderr, "video: cannot load %s (WF_DATA=pak)\n", gfx_pak); return -1; }
        if (load_gfx_edit() != 0) {  /* transition: the PNG source tree */
            /* WF_DATA=rom on a bootstrapped clone (no PNGs): the pak that
             * --export-gfx decoded from the ROM chips is the ROM's art too */
            if (strcmp(mode, "rom") == 0 && load_gfx_pak(gfx_pak) == 0) { video_ready = 1; wf_extended_flush_sprites(); return 0; }
            return -1;
        }
    }
    video_ready = 1;
    wf_extended_flush_sprites();
    return 0;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* MAME runs screen_update_wwfwfest at the start of vblank, before the CPU's
 * vblank IRQ handler executes. That handler is where the game writes
 * WF_SPRBUF_ADDR — 384 of 386 writes land on line 248 — so drawing after the
 * whole frame has run shows state one frame fresher than MAME's. Latch the
 * render inputs at vblank start and draw from the latch. */
static struct {
    uint8_t fg_videoram[WF_FGRAM_SIZE];
    uint8_t bg_videoram[WF_BGRAM_SIZE];
    uint8_t fg0_videoram[WF_FG0RAM_SIZE];
    uint8_t spriteram[WF_SPRRAM_SIZE];
    uint8_t palette[WF_PALETTE_SIZE];
    uint16_t scroll[4];
    uint8_t priority;
} latch;

/* CREDIT font: code = ASCII - 0x10 (select.c). Lowercase v (0x66)
 * is a blank tile — stamp is "V10" with the uppercase V at 0x46. */
#define FG0_COLS 64
static const unsigned credit_codes[6] = { 0x33, 0x42, 0x35, 0x34, 0x39, 0x44 };

static unsigned latch_fg0_code(int tc, int tr)
{
    unsigned off = (unsigned)((((tr & 31) * FG0_COLS) + (tc & 63)) * 4);

    if (off + 3 >= sizeof latch.fg0_videoram)
        return 0;
    return latch.fg0_videoram[off + 1];
}

static void latch_fg0_put(int tc, int tr, unsigned bank, unsigned code)
{
    unsigned off = (unsigned)((((tr & 31) * FG0_COLS) + (tc & 63)) * 4);

    if (off + 3 >= sizeof latch.fg0_videoram)
        return;
    latch.fg0_videoram[off]     = 0;
    latch.fg0_videoram[off + 1] = (uint8_t)code;
    latch.fg0_videoram[off + 2] = 0;
    latch.fg0_videoram[off + 3] =
        (uint8_t)(((bank & 0x0fu) << 4) | ((code >> 8) & 0x0fu));
}

static void paint_version_before_credit(void)
{
    char text[12];
    int tr, tc, i, n, start;
    unsigned bank = 0;

    /* Display only. Exact / scenarios leave fg0 alone. */
    if (!wf_menu_enabled || (!wf_render_c_enabled && !wf_render_68k_enabled))
        return;
    snprintf(text, sizeof text, "V%s", WF_VERSION_STRING);
    n = (int)strlen(text);
    for (tr = 0; tr < 32; tr++) {
        for (tc = 0; tc <= FG0_COLS - 6; tc++) {
            int hit = 1;
            unsigned off;

            for (i = 0; i < 6; i++) {
                if (latch_fg0_code(tc + i, tr) != credit_codes[i]) {
                    hit = 0;
                    break;
                }
            }
            if (!hit)
                continue;
            off = (unsigned)((((tr & 31) * FG0_COLS) + (tc & 63)) * 4);
            bank = (latch.fg0_videoram[off + 3] >> 4) & 0x0fu;
            start = tc - n - 1;
            if (start < 0)
                start = 0;
            for (i = 0; text[i]; i++) {
                unsigned ch = (unsigned char)text[i];
                unsigned code;

                if (ch == ' ')
                    code = 0;
                else
                    code = ch - 0x10u;
                latch_fg0_put(start + i, tr, bank, code);
            }
            return;
        }
    }
}

static uint8_t spr_skip_live[WF_SPRRAM_SIZE / 16];
static uint8_t spr_skip_buf[WF_SPRRAM_SIZE / 16];
static uint8_t spr_skip_latch[WF_SPRRAM_SIZE / 16];

static int c_owns_tilemaps(void);
static int c_owns_fg0(void);

void wf_video_latch(void)
{
    if (!video_ready)
        return;
    wf_hud_shadow_latch();
    wf_ext_fix_banners();
    wf_hud_redraw_native();
    wf_ext_fix_hud();
    wf_ext_apply_palettes();
    /* Scene palettes on --port/--mods. --68k never enters this. */
    wf_palette_latch();
    /* BG/FG come from the C-owned shadow on the non-arcade layers. The
     * shadow is never written back into wf.*_videoram, so the live VRAM the
     * traces sample is untouched and no gate moves. */
    if (wf_tilemap_shadow_active()) {
        wf_tilemap_shadow_latch(latch.fg_videoram, latch.bg_videoram);
    } else {
        memcpy(latch.fg_videoram, wf.fg_videoram, sizeof latch.fg_videoram);
        memcpy(latch.bg_videoram, wf.bg_videoram, sizeof latch.bg_videoram);
    }
    memcpy(latch.fg0_videoram, wf.fg0_videoram, sizeof latch.fg0_videoram);
    paint_version_before_credit();
    memcpy(latch.spriteram, wf.spriteram_buffered, sizeof latch.spriteram);
    memcpy(spr_skip_latch, spr_skip_buf, sizeof spr_skip_latch);
    /* Do not clear a gameplay skip mask merely because the match flag fell:
     * the latched sprite buffer may still be the old wrestler list. Keep mask
     * and buffer paired until the next inactive-scene SPRBUF copy commits a
     * fresh clear mask, otherwise stale 68k bodies flash over high scores. */
    memcpy(latch.palette, wf.palette, sizeof latch.palette);
    memcpy(latch.scroll, wf.scroll, sizeof latch.scroll);
    latch.priority = wf.priority;
}

/* Palette index -> RGB888. xBGR_444.
 * MAME folds CPU word offsets on write: fold(W)=(W&0xf)|((W&0x7fc0)>>2).
 * We mirror to the four A5/A6 aliases of W, so the colour lives at the
 * inverse: W = (C&0xf)|((C&0x1ff0)<<2). */
static uint32_t pal_rgb(unsigned index)
{
    unsigned woff = (index & 0x000fu) | ((index & 0x1ff0u) << 2);
    unsigned byte = woff << 1;
    if (byte + 1 >= WF_PALETTE_SIZE)
        return 0;
    uint16_t w = (uint16_t)((latch.palette[byte] << 8) | latch.palette[byte + 1]);
    unsigned r = (w & 0x0f) * 17u;
    unsigned g = ((w >> 4) & 0x0f) * 17u;
    unsigned b = ((w >> 8) & 0x0f) * 17u;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/* the FG plane's latched scroll (the priority byte swaps the pairs) */
void wf_video_latch_scroll_fg(int *sx, int *sy)
{
    if (latch.priority == 0x78) { *sx = latch.scroll[0]; *sy = latch.scroll[1]; }
    else                        { *sx = latch.scroll[2]; *sy = latch.scroll[3]; }
}

uint32_t wf_video_palette_rgb(unsigned index)
{
    return pal_rgb(index);
}
/* arena export (tools/export_arena.c): the latched planes, a tile's pens,
 * a latched palette word (xBGR444, folded like pal_rgb) */
const uint8_t *wf_video_latch_fg(void) { return latch.fg_videoram; }
const uint8_t *wf_video_latch_bg(void) { return latch.bg_videoram; }
const uint8_t *wf_video_latch_spriteram(void) { return latch.spriteram; }
const uint8_t *wf_video_bgtile_pens(unsigned code) { return code < BG_TILES ? tile_pen[code] : NULL; }
/* arena overrides (src/arena.c): an imported arena's tiles sit over the
 * shared sheet while its scene is up; the stock pens come back for any
 * other scene. */
static uint8_t *tile_stock;
void wf_video_tiles_override(const uint8_t *pens, unsigned n)
{
    if (!tile_stock) { tile_stock = malloc(sizeof tile_pen); if (!tile_stock) return; memcpy(tile_stock, tile_pen, sizeof tile_pen); }
    else memcpy(tile_pen, tile_stock, sizeof tile_pen);
    if (n > BG_TILES) n = BG_TILES;
    memcpy(tile_pen, pens, (size_t)n * 256u);
}
void wf_video_tiles_restore(void)
{
    if (tile_stock) memcpy(tile_pen, tile_stock, sizeof tile_pen);
}
uint16_t wf_video_palette_word(unsigned index)
{
    unsigned woff = (index & 0x000fu) | ((index & 0x1ff0u) << 2);
    unsigned byte = woff << 1;
    if (byte + 1 >= WF_PALETTE_SIZE) return 0;
    return (uint16_t)((latch.palette[byte] << 8) | latch.palette[byte + 1]);
}

/* Per-layer census, for renderer parity work. put_px is the only writer,
 * so px[] is post-clip: what actually landed on screen. lit[] is the
 * subset that is not black, which separates "layer drew nothing" from
 * "layer drew, but every pen resolved to black". */
enum { LAYER_FG, LAYER_BG, LAYER_SPR, LAYER_FG0, LAYER_N };
static const char *layer_name[LAYER_N] = { "fg", "bg", "spr", "fg0" };
static long layer_px[LAYER_N], layer_lit[LAYER_N], layer_tiles[LAYER_N];
static int cur_layer = -1;
/* Last layer to write each pixel; dumped beside the PPM so a pixel diff
 * can be attributed to a layer instead of guessed at. */
/* MOD camera zoom (modrules camera_zoom_pct): the compose window can grow
 * beyond the stock 320x240 up to 512x384 — the FG/BG planes are 512x512
 * and the sprite coordinate space is 9-bit, so a wider window simply
 * shows more of the SAME arena. Stock = 320x240 always. */
#define WF_VIEW_MAX_W 512
#define WF_VIEW_MAX_H 384
static int view_w = WF_SCREEN_WIDTH, view_h = WF_SCREEN_HEIGHT;
int  wf_view_w(void) { return view_w; }
int  wf_view_h(void) { return view_h; }
void wf_video_set_view(int w, int h)
{
    view_w = w < WF_SCREEN_WIDTH ? WF_SCREEN_WIDTH : w > WF_VIEW_MAX_W ? WF_VIEW_MAX_W : w;
    view_h = h < WF_SCREEN_HEIGHT ? WF_SCREEN_HEIGHT : h > WF_VIEW_MAX_H ? WF_VIEW_MAX_H : h;
}

static uint8_t layer_owner[WF_VIEW_MAX_W * WF_VIEW_MAX_H];
static uint16_t layer_index[WF_VIEW_MAX_W * WF_VIEW_MAX_H];
static unsigned cur_index;

static void put_px(uint32_t *pix, int pitch, int x, int y, uint32_t rgb)
{
    if ((unsigned)x >= (unsigned)view_w || (unsigned)y >= (unsigned)view_h)
        return;
    pix[y * (pitch / 4) + x] = rgb;
    if (cur_layer >= 0) {
        layer_px[cur_layer]++;
        if (rgb & 0x00ffffffu)
            layer_lit[cur_layer]++;
        layer_owner[y * view_w + x] = (uint8_t)cur_layer;
        layer_index[y * view_w + x] = (uint16_t)cur_index;
    }
}

static void draw_pen_tile(uint32_t *pix, int pitch,
                         const uint8_t *pens, int tw, int th,
                         int dx, int dy, unsigned color_base,
                         int flipx, int flipy, int opaque)
{
    if (cur_layer >= 0)
        layer_tiles[cur_layer]++;
    for (int ty = 0; ty < th; ty++) {
        int sy = flipy ? (th - 1 - ty) : ty;
        for (int tx = 0; tx < tw; tx++) {
            int sx = flipx ? (tw - 1 - tx) : tx;
            uint8_t pen = pens[sy * tw + sx];
            if (!opaque && pen == 0)
                continue;
            cur_index = color_base + pen;
            put_px(pix, pitch, dx + tx, dy + ty, pal_rgb(cur_index));
        }
    }
}

/* Display y = visarea y - 8. Tilemap world y uses visarea coordinates. */
static void draw_fg0(uint32_t *pix, int pitch)
{
    int shx = view_w - WF_SCREEN_WIDTH, shy = view_h - WF_SCREEN_HEIGHT;
    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 64; col++) {
            const uint8_t *p = &latch.fg0_videoram[(row * 64 + col) * 4];
            unsigned code = p[1] | ((unsigned)(p[3] & 0x0f) << 8);
            unsigned bank = (p[3] >> 4) & 0x0f;
            if (code >= FG0_TILES)
                continue;
            int dx = col * 8;
            int dy = row * 8 - 8;          /* visarea starts at line 8 */
            if (shx | shy) {               /* zoomed: the HUD text ANCHORS to the
                                              view edges — bottom rows ride down,
                                              the right HUD blocks ride right
                                              (stock layout assumes 320x240) */
                if (row >= 16) dy += shy;
                if (col >= 20) dx += shx;
            }
            draw_pen_tile(pix, pitch, fg0_pen[code], 8, 8,
                          dx, dy, bank * 16u, 0, 0, 0);
        }
    }
}

static void draw_fg_layer(uint32_t *pix, int pitch, int opaque,
                          int scrollx, int scrolly)
{
    for (int i = 0; i < 32 * 32; i++) {
        const uint8_t *p = &latch.fg_videoram[i * 4];
        uint16_t w0 = be16(p);
        uint16_t w1 = be16(p + 2);
        unsigned code = w1 & 0x1fff;
        unsigned bank = w0 & 0x000f;
        int flipx = (w0 & 0x0040) != 0;
        int flipy = (w0 & 0x0080) != 0;
        if (code >= BG_TILES)
            code &= BG_TILES - 1;
        int col = i % 32;
        int row = i / 32;
        int base_x = col * 16 - (scrollx & 511);
        int base_y = row * 16 - 8 - (scrolly & 511);
        for (int wx = 0; wx < 2; wx++) {
            int dx = base_x + wx * 512;
            if (dx <= -16 || dx >= view_w)
                continue;
            for (int wy = 0; wy < 2; wy++) {
                int dy = base_y + wy * 512;
                if (dy <= -16 || dy >= view_h)
                    continue;
                draw_pen_tile(pix, pitch, tile_pen[code], 16, 16,
                              dx, dy, 0x1000u + bank * 16u,
                              flipx, flipy, opaque);
            }
        }
    }
}

static void draw_bg_layer(uint32_t *pix, int pitch, int opaque,
                          int scrollx, int scrolly)
{
    for (int i = 0; i < 32 * 32; i++) {
        uint16_t attr = be16(&latch.bg_videoram[i * 2]);
        unsigned code = attr & 0x0fff;
        unsigned bank = attr >> 12;
        if (code >= BG_TILES)
            code &= BG_TILES - 1;
        int col = i % 32;
        int row = i / 32;
        int base_x = col * 16 - (scrollx & 511);
        int base_y = row * 16 - 8 - (scrolly & 511);
        for (int wx = 0; wx < 2; wx++) {
            int dx = base_x + wx * 512;
            if (dx <= -16 || dx >= view_w)
                continue;
            for (int wy = 0; wy < 2; wy++) {
                int dy = base_y + wy * 512;
                if (dy <= -16 || dy >= view_h)
                    continue;
                draw_pen_tile(pix, pitch, tile_pen[code], 16, 16,
                              dx, dy, 0x0c00u + bank * 16u, 0, 0, opaque);
            }
        }
    }
}

/* Inject pre-decoded sprite tiles directly into spr_pen[], bypassing ROM decode.
 * Used for extended wrestlers whose sprite data comes from external files.
 * Marks tiles as "have" so decode_one_sprite() is skipped.
 * base: first tile number; count: number of tiles; pixels: count*256 bytes
 * of pen indices (0-15), row-major 16x16 per tile. */
void wf_video_inject_sprite_tiles(unsigned base, unsigned count, const uint8_t *pixels)
{
    for (unsigned i = 0; i < count; i++) {
        unsigned t = base + i;
        if (t >= SPR_TILES)
            break;
        memcpy(spr_pen[t], pixels + i * 256, 256);
        spr_have[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
}

/* Fixed tile range for extended wrestler sprites.
 * Tiles 0xFF00-0xFF7F are reserved for extended wrestler sprite injection.
 * draw_sprites() remaps any tile in this range to the registered base. */
#define EXT_SPRITE_BASE  0xFF00u
#define EXT_SPRITE_COUNT 128   /* max injected sprite tiles */
#define EXT_SPRITE_MAX   16    /* max registered wrestlers with sprites */

static struct {
    unsigned tile_base;   /* base in spr_pen[] */
    unsigned count;      /* number of tiles */
    unsigned redirect;   /* where to inject into spr_pen[] */
} ext_sprite[EXT_SPRITE_MAX];
static int ext_sprite_count = 0;

static unsigned remap_ext_sprite(unsigned tile)
{
    for (int i = 0; i < ext_sprite_count; i++) {
        if (tile >= ext_sprite[i].tile_base &&
            tile < ext_sprite[i].tile_base + ext_sprite[i].count) {
            return ext_sprite[i].redirect + (tile - ext_sprite[i].tile_base);
        }
    }
    return tile;
}

static int bitwidth(unsigned v)
{
    int n = 0;
    while (v) {
        v >>= 1;
        n++;
    }
    return n;
}

static void draw_native_sprite_command(uint32_t *pix, int pitch, int xpos,
                                       int compiled_y, unsigned number,
                                       uint16_t w1, unsigned bank)
{
    if (xpos > 512 - 16)
        xpos -= 512;
    int ypos = (256 - compiled_y) & 0x1ff;
    ypos -= 16;
    int flipx = (w1 >> 4) & 1;
    int flipy = (w1 >> 3) & 1;
    unsigned chain = (w1 & 0x00e0) >> 5;
    unsigned cmask = ~((1u << bitwidth(chain)) - 1u);

    number &= cmask;
    bank &= 0x0f;
    for (unsigned count = 0; count <= chain; count++) {
        unsigned tile = number + count;
        tile = remap_ext_sprite(tile);
        if (tile >= SPR_TILES)
            continue;
        int dy;
        if (flipy)
            dy = ypos - (int)(16 * chain) + (int)(16 * count);
        else
            dy = ypos - (int)(16 * count);
        if (tile >= 0xFF00u && tile < 0xFF80u) {
            /* Filesystem stickman tiles: ink/transparent, ignore bank. */
            const uint8_t *pens = sprite_tile(tile);
            if (cur_layer >= 0)
                layer_tiles[cur_layer]++;
            for (int py = 0; py < 16; py++)
                for (int px = 0; px < 16; px++)
                    if (pens[py * 16 + px])
                        put_px(pix, pitch, xpos + px, dy - 8 + py,
                               0xff101010u);
        } else {
            draw_pen_tile(pix, pitch, sprite_tile(tile), 16, 16,
                          xpos, dy - 8, 0x0400u + bank * 16u,
                          flipx, flipy, 0);
        }
    }
}

static void draw_sprites(uint32_t *pix, int pitch)
{
    const uint8_t *ram = latch.spriteram;
    for (int s = 0; s < WF_SPRRAM_SIZE; s += 16) {
        uint16_t w1 = be16(ram + s + 2);
        if (spr_skip_latch[s / 16])
            continue;
        if (!(w1 & 0x0001))
            continue;
        int xpos = (ram[s + 11]) | ((w1 & 0x0004) << 6);
        int ypos = ram[s + 1] | ((w1 & 0x0002) << 7);
        unsigned number = (unsigned)ram[s + 5] | ((unsigned)ram[s + 7] << 8);
        unsigned bank = ram[s + 9] & 0x0f;
        if (ram[s + 12] == 0xE7 && ram[s + 14] == 0x5C)   /* clone-art arena */
            number |= ((unsigned)ram[s + 13] & 0x0Fu) << 16;
        static int sprdump = -1;
        if (sprdump < 0) sprdump = getenv("WF_SPRDUMP") ? 1 : 0;
        if (sprdump)                   /* harness: every record reaching the rasteriser */
            fprintf(stderr, "sprdump: slot %3d x %3d y %3d tile 0x%05X flipx %d flipy %d chain %u bank %u\n",
                    s / 16, xpos > 512 - 16 ? xpos - 512 : xpos, ypos, number, (w1 >> 4) & 1, (w1 >> 3) & 1, (w1 & 0xE0u) >> 5, bank);
        /* Render-owner diagnostic. Every sprite reaching this rasteriser is
         * drawn by C whoever compiled the record, so F1 hides all of them.
         *
         * Bank 14 used to be exempt, to keep the ring hardware on screen when
         * both toggles were off. That made the instrument lie once C owned
         * the tilemaps: the attract roster page draws a portrait from six
         * bank-14 records, and it survived F1 looking exactly like a surface
         * nobody had converted. It was C drawing it the whole time. */
        if (!wf_render_c_enabled && !wf_render_68k_enabled)
            continue;
        if (bake_tiles())
            continue;
        if ((bake_arena() || bake_ropes()) && bank != 14u)
            continue;
        draw_native_sprite_command(pix, pitch, xpos, ypos, number, w1, bank);
    }
}

/* --- render owner (F1 / F2) ---------------------------------------------
 *
 * F1 toggles what C draws; F2 toggles what the 68k draws. The 68k's output is
 * *suppressed* wherever C has replaced it, which is why 68k starts off: F2 is
 * "also show me the 68k's version of what C took over", not "hide the 68k".
 *
 * Surfaces the 68k still solely authors are not part of that A/B and stay
 * visible on their own — that is deliberate, and it is the instrument for
 * finding them. Press F1 in a match and whatever remains (the ring, the
 * backgrounds, the one attract portrait that is still a tilemap) is exactly
 * the list of things not yet converted.
 *
 * C owns a tilemap only where it paints the whole screen: the C select and
 * the C menu. The gameplay FG0 HUD shadow does *not* count — it is a
 * transport that republishes the 68k's own FG0 stores, not an author. */
static int c_owns_tilemaps(void)
{
    return wf_select_active() || wf_menu_active() || wf_tilemap_shadow_active();
}

static int c_owns_fg0(void)
{
    return c_owns_tilemaps();
}

void wf_video_draw(uint32_t *pixels, int pitch)
{
    if (!video_ready)
        return;
    memset(layer_px, 0, sizeof layer_px);
    memset(layer_lit, 0, sizeof layer_lit);
    memset(layer_tiles, 0, sizeof layer_tiles);
    memset(layer_owner, 0xff, sizeof layer_owner);
    int n = (pitch / 4) * view_h;
    for (int i = 0; i < n; i++)
        pixels[i] = 0xff000000u;

    int fgx = latch.scroll[0];
    int fgy = latch.scroll[1];
    int bgx = latch.scroll[2];
    int bgy = latch.scroll[3];
    uint8_t pri = latch.priority;

    /* pri==0x78 uses named scrolls; any other value swaps the pairs. */
    int fg_sx, fg_sy, bg_sx, bg_sy;
    if (pri == 0x78) {
        fg_sx = fgx; fg_sy = fgy;
        bg_sx = bgx; bg_sy = bgy;
    } else {
        bg_sx = fgx; bg_sy = fgy;
        fg_sx = bgx; fg_sy = bgy;
    }

    /* The frame is already cleared to opaque black above, so suppressing the
     * layer that would have painted opaque leaves black rather than stale
     * pixels.
     *
     * A C-owned tilemap follows F1 alone: while C paints the whole screen the
     * 68k is frozen and has no version to fall back to, so F2 has nothing to
     * reveal. A 68k-owned tilemap is not part of the A/B and stays up unless
     * both toggles are off, which is the "show me nothing" case. */
    int both_off = !wf_render_c_enabled && !wf_render_68k_enabled;
    /* BG/FG the 68k still authors stay up in every state: they are the ring
     * and the scenery, and blanking them would hide the very thing the
     * diagnostic is meant to leave behind. */
    int show_tiles = c_owns_tilemaps() ? wf_render_c_enabled : 1;
    /* FG0 does go dark with both off. That reproduces the old diagnostic
     * blank, which used to memset fg0_videoram and keep a copy to restore;
     * gating at draw time gets the same picture without a display toggle
     * writing to VRAM the 68k is still reading. */
    int show_fg0 = c_owns_fg0() ? wf_render_c_enabled : !both_off;
    if (bake_arena() || bake_tiles() || bake_ropes())
        show_fg0 = 0;
    if (bake_ropes())
        show_tiles = 0;

    if (pri == 0x7b) {
        if (show_tiles) {
            cur_layer = LAYER_FG;  draw_fg_layer(pixels, pitch, 1, fg_sx, fg_sy);
            cur_layer = LAYER_BG;  draw_bg_layer(pixels, pitch, 0, bg_sx, bg_sy);
        }
        cur_layer = LAYER_SPR; draw_sprites(pixels, pitch);
    } else if (pri == 0x7c) {
        if (show_tiles) {
            cur_layer = LAYER_FG;  draw_fg_layer(pixels, pitch, 1, fg_sx, fg_sy);
        }
        cur_layer = LAYER_SPR; draw_sprites(pixels, pitch);
        if (show_tiles) {
            cur_layer = LAYER_BG;  draw_bg_layer(pixels, pitch, 0, bg_sx, bg_sy);
        }
    } else if (pri == 0x78) {
        if (show_tiles) {
            cur_layer = LAYER_BG;  draw_bg_layer(pixels, pitch, 1, bg_sx, bg_sy);
            cur_layer = LAYER_FG;  draw_fg_layer(pixels, pitch, 0, fg_sx, fg_sy);
        }
        cur_layer = LAYER_SPR; draw_sprites(pixels, pitch);
    }
    cur_layer = LAYER_SPR;
    if (show_fg0) {
        cur_layer = LAYER_FG0; draw_fg0(pixels, pitch);
    }
    cur_layer = -1;
}

int wf_video_write_ppm(const char *path)
{
    int fg0_n = 0, fg_n = 0, bg_n = 0, spr_n = 0, pal_n = 0;
    for (int i = 0; i < WF_FG0RAM_SIZE; i++)
        if (wf.fg0_videoram[i]) fg0_n++;
    for (int i = 0; i < WF_FGRAM_SIZE; i++)
        if (wf.fg_videoram[i]) fg_n++;
    for (int i = 0; i < WF_BGRAM_SIZE; i++)
        if (wf.bg_videoram[i]) bg_n++;
    for (int s = 0; s < WF_SPRRAM_SIZE; s += 16)
        if (wf.spriteram_buffered[s + 3] & 1) spr_n++;
    for (int i = 0; i < 4096; i++)
        if (wf.palette[i]) pal_n++;
    int live_spr = 0;
    for (int s = 0; s < WF_SPRRAM_SIZE; s += 16)
        if (wf.spriteram[s + 3] & 1) live_spr++;
    int uniq = 0;
    uint8_t seen[512] = {0};
    for (int i = 0; i < 32 * 32; i++) {
        unsigned code = be16(&wf.bg_videoram[i * 2]) & 0x0fff;
        if (code && code < sizeof seen && !seen[code]) {
            seen[code] = 1;
            uniq++;
        }
    }
    fprintf(stderr,
            "video: pri=%02X scroll=%04X %04X %04X %04X fg0=%d fg=%d bg=%d "
            "spr_buf=%d spr_live=%d pal=%d bg_uniq=%d spr0=%02X%02X%02X%02X\n",
            wf.priority, wf.scroll[0], wf.scroll[1], wf.scroll[2], wf.scroll[3],
            fg0_n, fg_n, bg_n, spr_n, live_spr, pal_n, uniq,
            wf.spriteram[0], wf.spriteram[1], wf.spriteram[2], wf.spriteram[3]);

    uint32_t *buf = calloc((size_t)view_w * view_h, 4);   /* the ZOOMED view */
    if (!buf)
        return -1;
    wf_video_draw(buf, view_w * 4);

    int black = 0;
    for (int i = 0; i < view_w * view_h; i++)
        if (!(buf[i] & 0x00ffffffu)) black++;
    fprintf(stderr, "video: frame black=%d/%d", black, view_w * view_h);
    for (int l = 0; l < LAYER_N; l++)
        fprintf(stderr, " %s[tiles=%ld px=%ld lit=%ld]", layer_name[l],
                layer_tiles[l], layer_px[l], layer_lit[l]);
    fputc('\n', stderr);

    /* WF_VRAM_DUMP writes the raw layer memories next to the ppm. Reading a
     * tilemap back is how the select screen's cell layout was measured
     * (docs/select-screen.md) — inferring it from the drawn pixels cannot
     * separate "which tile code" from "which palette bank". */
    if (getenv("WF_VRAM_DUMP")) {
        char vp[512];
        snprintf(vp, sizeof vp, "%s.vram", path);
        FILE *vf = fopen(vp, "wb");
        if (vf) {
            fwrite(wf.fg_videoram, 1, sizeof wf.fg_videoram, vf);
            fwrite(wf.bg_videoram, 1, sizeof wf.bg_videoram, vf);
            fwrite(wf.fg0_videoram, 1, sizeof wf.fg0_videoram, vf);
            fwrite(wf.spriteram, 1, sizeof wf.spriteram, vf);
            fwrite(wf.scroll, 1, sizeof wf.scroll, vf);
            fwrite(wf.palette, 1, sizeof wf.palette, vf);
            fclose(vf);
        }
    }

    if (getenv("WF_OWNER_MAP")) {
        char op[512];
        snprintf(op, sizeof op, "%s.owner", path);
        FILE *of = fopen(op, "wb");
        if (of) {
            fwrite(layer_owner, 1, sizeof layer_owner, of);
            fwrite(layer_index, sizeof layer_index[0],
                   sizeof layer_index / sizeof layer_index[0], of);
            fclose(of);
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", view_w, view_h);
    for (int i = 0; i < view_w * view_h; i++) {
        uint32_t p = buf[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    free(buf);
    return 0;
}
