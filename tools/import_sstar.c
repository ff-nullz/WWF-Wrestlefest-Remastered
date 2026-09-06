/* --sstar: extract wrestler animation frames from WWF Superstars (Technos
 * 1989, the WrestleFest predecessor) for import as clone-art wrestlers.
 *
 *   wfengine --sstar <sstar_rom_dir> <wrestler 0..8> <outdir>
 *
 * Transcribed from the Superstars program ROM's own sprite composer
 * (maincpu.bin, capstone-verified disassembly 2026-08-24):
 *   $4624  frame walker: anim ptr table $6CD8[wid] -> frame = { u16 n,
 *          n x { i8 xoff -> $1C0000, i8 yoff -> $1C0002, u32 block } };
 *          palette: multi-piece frames borrow the OWNING wrestler's bank
 *          ($46A0: whose anim segment contains the block), else
 *          flags & $70.
 *   $46F4  block: u8 flags (bit0 = decoder select, bits 2-3 = flip base
 *          XOR object facing, bits 4-6 = palette), u8 ntiles, u32 geo,
 *          then ntiles x u16 codes ($472C) or ONE u16 base code advancing
 *          +1 per tile / +2 when chained ($47D2).
 *   geo:   u32 chain mask (bit 31 = first tile; set = 2 stacked 16x16,
 *          y -16 instead of -8), then ntiles x { i8 dx, i8 dy }.
 *          X adds xoff+dx (negated when facing left, $4878); Y adds
 *          yoff+dy-8(-8 chained), never negated ($48D4).
 *
 * Sprite tiles: 2MB region (c951.114 + 24j4-0.115 + 24j5-0.116 +
 * c950.117 + 24j2-0.118 + 24j3-0.119), MAME tiles16x16_layout: 4bpp,
 * planes { half+0, half+4, 0, 4 }, x { 3,2,1,0, 16*8+3.. }, 64 bytes
 * per tile — the WrestleFest family format.
 *
 * Output: <outdir>/frames.json (per anim slot: piece list with resolved
 * tiles/positions) + frames.png contact sheet + tiles.json/sheet.png of
 * the unique tiles (import feeds these to --retile-style rebasing). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <png.h>
#include <sys/stat.h>

#define SS_ANIM_TBL 0x6CD8u
#define SS_PAL_TBL  0x3BC8u   /* 8 x u32 -> the per-wrestler MAIN palettes
                                 (15 BE 12-bit xBGR words, pen 0 clear),
                                 wid-indexed — the loader at $3B62 copies
                                 entry [wid] into palette RAM $140100 +
                                 bank*0x20 skipping pen 0. Alt palettes for
                                 duplicate picks: wid list $3BE8 {2,3,5} ->
                                 ptrs $3BEE {2d48,2c94,2c76}. Set 8 is the
                                 PROPS art (tables/boards), not a man. */
#define SS_NWRESTLER 9
/* art sets (user-verified 2026-08-24): 0 Savage, 1 Hogan, 2 Warrior,
 * 3 Duggan, 4 Boss Man, 5 Honky Tonk, 6 DiBiase/MDM, 7 Andre, 8 props */

static uint8_t *mcpu; static size_t mcpu_len;      /* 256K maincpu.bin */
static uint8_t *sgfx; static size_t sgfx_len;      /* 2MB sprite region */

static uint32_t r32(uint32_t a) { return a + 3 < mcpu_len ? ((uint32_t)mcpu[a] << 24) | ((uint32_t)mcpu[a+1] << 16) | ((uint32_t)mcpu[a+2] << 8) | mcpu[a+3] : 0; }
static uint16_t r16(uint32_t a) { return a + 1 < mcpu_len ? (uint16_t)((mcpu[a] << 8) | mcpu[a+1]) : 0; }

static int load_file_at(const char *dir, const char *name, uint8_t *dst, size_t off, size_t want)
{
    char p[512]; FILE *f; size_t n;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "sstar: cannot open %s\n", p); return -1; }
    n = fread(dst + off, 1, want, f);
    fclose(f);
    if (n != want) { fprintf(stderr, "sstar: %s short read\n", p); return -1; }
    return 0;
}

/* MAME tiles16x16_layout decode: pen(t,x,y), plane p bit = MSB-first */
static uint8_t ss_pen(unsigned t, int x, int y)
{
    static const unsigned xoff[16] = { 3,2,1,0, 16*8+3,16*8+2,16*8+1,16*8+0,
                                       32*8+3,32*8+2,32*8+1,32*8+0, 48*8+3,48*8+2,48*8+1,48*8+0 };
    unsigned half = (unsigned)(sgfx_len * 8 / 2);
    unsigned planes[4]; unsigned pen = 0;
    planes[0] = half + 0; planes[1] = half + 4; planes[2] = 0; planes[3] = 4;
    for (int p = 0; p < 4; p++) {
        unsigned bit = planes[p] + t * 512u + (unsigned)y * 8u + xoff[x];
        unsigned byte = bit >> 3;
        if (byte < sgfx_len && (sgfx[byte] & (0x80u >> (bit & 7))))
            pen |= 1u << (3 - p);   /* planeoffset[0] = MSB (verified vs MAME spriteram render) */
    }
    return (uint8_t)pen;
}

/* one composed sprite cell */
typedef struct { int x, y; unsigned code; uint8_t flip, pal, chain, owner; } SsCell;

/* $46A0: a block's OWNER = count of anim-table bases <= its address
 * (the 9 pointers at $6CD8 ascend); >= $3B87E = common art (owner 9) */
static uint8_t block_owner(uint32_t blk)
{
    uint8_t n = 0;
    if (blk >= 0x3B87Eu) return 9;
    for (int i = 0; i < SS_NWRESTLER; i++)
        if (blk >= ((uint32_t)mcpu[SS_ANIM_TBL + i*4] << 24 | (uint32_t)mcpu[SS_ANIM_TBL + i*4 + 1] << 16 | (uint32_t)mcpu[SS_ANIM_TBL + i*4 + 2] << 8 | mcpu[SS_ANIM_TBL + i*4 + 3]))
            n = (uint8_t)(i + 1);
    return n ? (uint8_t)(n - 1) : 9;
}
#define MAX_CELLS 96

/* decode one BLOCK at addr with piece offset (xo,yo) into cells; returns count */
static int block_cells(uint32_t blk, int xo, int yo, SsCell *out, int max)
{
    uint8_t flags = mcpu[blk], ntiles = mcpu[blk + 1];
    uint32_t geo = r32(blk + 2);
    uint32_t mask = r32(geo);
    /* $472C: positions start at geo+2 — the u32 chain mask's LOW word
     * overlaps the first pair (only mask bits 31..16 are meaningful for
     * the <=16-tile blocks; read fresh as a long each tile like the ROM) */
    uint32_t pos = geo + 2, codes = blk + 6;
    unsigned seq = r16(blk + 6);                   /* $47D2 sequential base */
    int n = 0;
    if (geo + 4 >= mcpu_len || ntiles == 0 || ntiles > 32) return 0;
    for (int i = 0; i < ntiles && n < max; i++) {
        int chained = (mask >> (31 - i)) & 1u;
        int dx = (int8_t)mcpu[pos + (uint32_t)i * 2u];
        int dy = (int8_t)mcpu[pos + (uint32_t)i * 2u + 1];
        unsigned code;
        if (flags & 1) {                            /* sequential decoder */
            code = seq & 0x3FFFu;
            seq += chained ? 2u : 1u;
        } else
            code = r16(codes + (uint32_t)i * 2u) & 0x3FFFu;
        out[n].x = xo + dx;
        /* the hardware stores Y INVERTED (MAME: ypos = (256 - stored) - 16)
         * while X's two inversions ($4878 eor 0xFF vs 256-x) cancel: canvas
         * y = -(yo+dy) + 8 + 8*chained  (the MAME ypos, constant dropped) */
        out[n].y = -(yo + dy) + 8 + (chained ? 8 : 0);
        out[n].code = code;
        out[n].chain = (uint8_t)chained;
        out[n].flip = (uint8_t)((flags & 0x0Cu) << 4);   /* $4952 base flip bits */
        out[n].pal = (uint8_t)((flags >> 4) & 7u);
        out[n].owner = block_owner(blk);
        n++;
    }
    return n;
}

/* compose a whole frame into cells */
static int frame_cells(uint32_t fr, SsCell *out, int max)
{
    unsigned n = r16(fr);
    uint32_t p = fr + 2;
    int total = 0;
    if (n == 0 || n > 24) return 0;
    for (unsigned i = 0; i < n && total < max; i++) {
        int xo = (int8_t)mcpu[p], yo = (int8_t)mcpu[p + 1];
        uint32_t blk = r32(p + 2);
        p += 6;
        if (blk + 6 >= mcpu_len) continue;
        total += block_cells(blk, xo, yo, out + total, max - total);
    }
    return total;
}

/* rasterize cells into an 8-bit pen canvas (origin ox,oy) */
static void draw_cells(uint8_t *img, int W, int H, int ox, int oy, const SsCell *c, int n)
{
    for (int i = 0; i < n; i++)
        for (int part = 0; part <= c[i].chain; part++) {
            /* MAME chain: nonflipY tile k at ypos - 16*(chain) + 16*k;
             * flipY at ypos + 16*(chain) - 16*k; code increments */
            unsigned code = (c[i].code + (unsigned)part) & 0x3FFFu;
            int step = (c[i].flip & 0x40) ? (int)c[i].chain - part : part - (int)c[i].chain;
            int bx = ox + c[i].x, by = oy + c[i].y + step * 16;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    int fx = (c[i].flip & 0x80) ? 15 - x : x;
                    int fy = (c[i].flip & 0x40) ? 15 - y : y;
                    uint8_t v = ss_pen(code, fx, fy);
                    int px = bx + x, py = by + y;
                    if (v && px >= 0 && px < W && py >= 0 && py < H)
                        img[(size_t)py * (size_t)W + px] = v;   /* runtime banks are per-SLOT ($46A0): solo = one palette */
                }
        }
}

static int write_png8(const char *path, const uint8_t *img, int W, int H, const png_color *pal, int npal)
{
    FILE *f = fopen(path, "wb");
    png_structp png; png_infop info; png_bytep *rp;
    if (!f) return -1;
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) { fclose(f); return -1; }
    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)W, (png_uint_32)H, 8, PNG_COLOR_TYPE_PALETTE,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_PLTE(png, info, (png_colorp)pal, npal);
    rp = malloc(sizeof(png_bytep) * (size_t)H);
    for (int y = 0; y < H; y++) rp[y] = (png_bytep)(img + (size_t)y * (size_t)W);
    png_set_rows(png, info, rp);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, 0);
    png_destroy_write_struct(&png, &info);
    free(rp); fclose(f);
    return 0;
}

static SsCell cells[256][MAX_CELLS];
static int ncell[256];
static uint32_t fr_addr[256];
static int ss_nanim;

/* load the ROMs (once) and decode wrestler `wid`'s 256 anim slots into
 * the static arrays above; 0 = ok */
static int ss_extract(const char *romdir, int wid)
{
    static const struct { const char *n; size_t sz; } gfx[6] = {
        { "c951.114", 0x80000 }, { "24j4-0.115", 0x40000 }, { "24j5-0.116", 0x40000 },
        { "c950.117", 0x80000 }, { "24j2-0.118", 0x40000 }, { "24j3-0.119", 0x40000 },
    };
    uint32_t base, off = 0;
    if (!mcpu) {
        mcpu_len = 0x40000; mcpu = malloc(mcpu_len);
        sgfx_len = 0x200000; sgfx = malloc(sgfx_len);
        if (!mcpu || !sgfx) return 1;
        if (load_file_at(romdir, "maincpu.bin", mcpu, 0, mcpu_len)) return 1;
        for (int i = 0; i < 6; i++) {
            if (load_file_at(romdir, gfx[i].n, sgfx, off, gfx[i].sz)) return 1;
            off += (uint32_t)gfx[i].sz;
        }
    }
    if (wid < 0 || wid >= SS_NWRESTLER) { fprintf(stderr, "sstar: wrestler 0..8\n"); return 1; }
    base = r32(SS_ANIM_TBL + (uint32_t)wid * 4u);
    fprintf(stderr, "sstar: wrestler %d anim base %06X\n", wid, base);
    if (base < 0x8000 || base >= mcpu_len) { fprintf(stderr, "sstar: bad anim base\n"); return 1; }
    ss_nanim = 0;
    for (int a = 0; a < 256; a++) {
        uint32_t fr = r32(base + (uint32_t)a * 4u);
        if (fr < 0x8000 || fr + 2 >= mcpu_len) { ncell[a] = 0; fr_addr[a] = 0; continue; }
        fr_addr[a] = fr;
        ncell[a] = frame_cells(fr, cells[a], MAX_CELLS);
        if (ncell[a]) ss_nanim = a + 1;
    }
    return 0;
}

int tool_sstar(const char *romdir, int wid, const char *outdir)
{
    char path[512];
    int nanim;
    if (ss_extract(romdir, wid)) return 1;
    nanim = ss_nanim;
    fprintf(stderr, "sstar: %d anim slots populated\n", nanim);

    {   /* contact sheet: 16 cols x ceil(nanim/16) rows of 96x112 cells,
         * origin at (48, 88) inside the cell; grayscale-ish pens */
        int cols = 16, rows = (nanim + cols - 1) / cols;
        int CW = 96, CH = 112, W = cols * CW, H = rows * CH;
        uint8_t *img = calloc((size_t)W * (size_t)H, 1);
        png_color pal[18];
        uint32_t pl = r32(SS_PAL_TBL + (uint32_t)(wid < 8 ? wid : 7) * 4u);
        pal[0].red = 24; pal[0].green = 24; pal[0].blue = 32;
        for (int p = 1; p < 16; p++) {          /* xBGR_444 (verified vs MAME) */
            uint16_t w = r16(pl + (uint32_t)(p - 1) * 2u);
            pal[p].red   = (png_byte)((w & 0x0F) * 17);
            pal[p].green = (png_byte)(((w >> 4) & 0x0F) * 17);
            pal[p].blue  = (png_byte)(((w >> 8) & 0x0F) * 17);
        }
        pal[16].red = 24; pal[16].green = 24; pal[16].blue = 32;
        pal[17].red = 60; pal[17].green = 60; pal[17].blue = 70;
        fprintf(stderr, "sstar: main palette %05X\n", pl);
        uint8_t *cell = malloc((size_t)CW * (size_t)CH);
        for (int a = 0; a < nanim; a++) {
            int ox = (a % cols) * CW, oy = (a / cols) * CH;
            memset(cell, 0, (size_t)CW * (size_t)CH);
            /* private buffer per frame: clipping stays inside the cell
             * (frames used to bleed into their neighbours on the sheet).
             * Draw only the target wrestler's OWN cells (owner == wid):
             * shared/opponent slices of two-man frames stay out, so the
             * sheet doubles as the import preview. */
            {
                SsCell own[MAX_CELLS]; int no = 0;
                for (int i = 0; i < ncell[a]; i++)
                    if (cells[a][i].owner == (uint8_t)wid) own[no++] = cells[a][i];
                draw_cells(cell, CW, CH, 48, 88, own, no);
            }
            for (int y = 0; y < CH; y++)
                for (int x = 0; x < CW; x++)
                    if (cell[(size_t)y * CW + x])
                        img[(size_t)(oy + y) * W + ox + x] = cell[(size_t)y * CW + x];
            for (int x = 0; x < CW; x++)
                if (!img[(size_t)(oy + CH - 1) * W + ox + x])
                    img[(size_t)(oy + CH - 1) * W + ox + x] = 17;  /* baseline */
        }
        free(cell);
        snprintf(path, sizeof path, "%s/frames_%d.png", outdir, wid);
        if (write_png8(path, img, W, H, pal, 18)) { fprintf(stderr, "sstar: cannot write %s\n", path); return 1; }
        fprintf(stderr, "sstar: %s (%dx%d, %d frames)\n", path, W, H, nanim);
        free(img);
    }

    {   /* frames.json: resolved cells per anim slot */
        FILE *f;
        snprintf(path, sizeof path, "%s/frames_%d.json", outdir, wid);
        f = fopen(path, "w");
        if (!f) return 1;
        fprintf(f, "{ \"wrestler\": %d, \"anim_base\": %u, \"frames\": {\n", wid, r32(SS_ANIM_TBL + (uint32_t)wid * 4u));
        for (int a = 0, first = 1; a < nanim; a++) {
            if (!ncell[a]) continue;
            {
                int solo = 1;
                for (int i = 0; i < ncell[a]; i++)
                    if (cells[a][i].owner != (uint8_t)wid) solo = 0;
                fprintf(f, "%s  \"%d\": { \"addr\": %u, \"solo\": %d, \"cells\": [",
                        first ? "" : ",\n", a, fr_addr[a], solo);
            }
            first = 0;
            for (int i = 0; i < ncell[a]; i++)
                fprintf(f, "%s{\"x\":%d,\"y\":%d,\"code\":%u,\"chain\":%d,\"flip\":%d,\"pal\":%d,\"owner\":%d}",
                        i ? "," : "", cells[a][i].x, cells[a][i].y, cells[a][i].code,
                        cells[a][i].chain, cells[a][i].flip, cells[a][i].pal, cells[a][i].owner);
            fprintf(f, "] }");
        }
        fprintf(f, "\n} }\n");
        fclose(f);
    }
    return 0;
}

/* ---- the IMPORTER: Superstars wrestler -> WrestleFest clone package ----
 *
 *   wfengine --import-sstar <romdir> <sswid> <baseid> <arena_hex> <dstdir>
 *
 * Reads the WF-pose -> SS-anim map from data/sstar-posemap.txt
 * ("# comment", "anchor DX DY", "<wfpose> <ssanim>" lines). For every
 * mapped pose it emits the SS frame's OWN cells (owner == sswid) as WF
 * package records; the tiles land in the clone-art arena starting at
 * <arena_hex> (chained pairs even-aligned for the drain's chain mask,
 * BOTTOM tile first: WF chains draw upward, Superstars' downward).
 * Unmapped poses fall back to the base's art at runtime (package.c
 * per-pose clone_delegate). Palette: the wrestler's 15 ROM colours
 * (identical 12-bit xBGR word format in both games). */
typedef struct { unsigned code; int chained; unsigned arena; } TileAlloc;

int tool_import_sstar(const char *romdir, int sswid, int baseid,
                      unsigned arena, const char *dst)
{
    static const char *ss_names[9] = { "RANDY SAVAGE", "HULK HOGAN",
        "ULTIMATE WARRIOR", "JIM DUGGAN", "BIG BOSS MAN",
        "HONKY TONK MAN", "TED DIBIASE", "ANDRE THE GIANT", "PROPS" };
    static int map_wf[512], map_ss[512];
    static TileAlloc ta[4096];
    int nmap = 0, nta = 0;
    unsigned ntile = 0;
    int dx = -8, ay = 16;              /* anchor calibration (posemap file) */
    char path[512], line[128];
    FILE *f;

    if (ss_extract(romdir, sswid)) return 1;
    f = fopen("data/sstar-posemap.txt", "r");
    if (!f) { fprintf(stderr, "import-sstar: missing data/sstar-posemap.txt\n"); return 1; }
    while (fgets(line, sizeof line, f)) {
        int a, b;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "anchor %d %d", &a, &b) == 2) { dx = a; ay = b; continue; }
        if (sscanf(line, "%d %d", &a, &b) == 2 && a >= 0 && a < 0x400 && b >= 0 && b < 256 && nmap < 512) {
            map_wf[nmap] = a; map_ss[nmap] = b; nmap++;
        }
    }
    fclose(f);
    fprintf(stderr, "import-sstar: %d pose mappings, anchor dx=%d ay=%d\n", nmap, dx, ay);

    mkdir(dst, 0775);

    /* pass 1: allocate arena tiles for every referenced (code, chained)
     * unit. Pairs: even-aligned, bottom (code+1) first. */
    for (int m = 0; m < nmap; m++) {
        SsCell *cl = cells[map_ss[m]];
        for (int i = 0; i < ncell[map_ss[m]]; i++) {
            int found = 0;
            if (cl[i].owner != (uint8_t)sswid) continue;
            for (int k = 0; k < nta; k++)
                if (ta[k].code == cl[i].code && ta[k].chained == cl[i].chain) { found = 1; break; }
            if (found || nta >= 4096) continue;
            if (cl[i].chain && (ntile & 1u)) ntile++;      /* even-align pairs */
            ta[nta].code = cl[i].code;
            ta[nta].chained = cl[i].chain;
            ta[nta].arena = arena + ntile;
            ntile += cl[i].chain ? 2u : 1u;
            nta++;
        }
    }
    fprintf(stderr, "import-sstar: %d tile units -> %u arena tiles at 0x%X\n", nta, ntile, arena);
    if (ntile > 0x1800) { fprintf(stderr, "import-sstar: arena overflow (%u > 6144)\n", ntile); return 1; }

    {   /* sheet.png + tiles.json in arena order */
        int cols = 16, rows = ((int)ntile + cols - 1) / cols;
        int W = cols * 16, H = rows * 16;
        uint8_t *img = calloc((size_t)W * (size_t)H, 1);
        png_color pal[16];
        uint32_t pl = r32(0x3BC8u + (uint32_t)(sswid < 8 ? sswid : 7) * 4u);
        FILE *fo;
        pal[0].red = pal[0].green = pal[0].blue = 0;
        for (int p = 1; p < 16; p++) {
            uint16_t w = r16(pl + (uint32_t)(p - 1) * 2u);
            pal[p].red   = (png_byte)((w & 0x0F) * 17);
            pal[p].green = (png_byte)(((w >> 4) & 0x0F) * 17);
            pal[p].blue  = (png_byte)(((w >> 8) & 0x0F) * 17);
        }
        for (int k = 0; k < nta; k++)
            for (int part = 0; part <= ta[k].chained; part++) {
                /* arena slot order: bottom tile (code + chained) first */
                unsigned slot = ta[k].arena - arena + (unsigned)part;
                unsigned code = ta[k].code + (unsigned)(ta[k].chained - part);
                int ox = ((int)slot % cols) * 16, oy = ((int)slot / cols) * 16;
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++)
                        img[(size_t)(oy + y) * W + ox + x] = ss_pen(code, x, y);
            }
        snprintf(path, sizeof path, "%s/sheet.png", dst);
        if (write_png8(path, img, W, H, pal, 16)) { free(img); return 1; }
        free(img);
        snprintf(path, sizeof path, "%s/tiles.json", dst);
        fo = fopen(path, "w"); if (!fo) return 1;
        fprintf(fo, "{\"count\":%u,\"tiles\":[", ntile);
        for (unsigned t = 0; t < ntile; t++) fprintf(fo, "%s%u", t ? "," : "", arena + t);
        fprintf(fo, "]}\n");
        fclose(fo);
        /* palette.json: same 12-bit word layout in both games */
        snprintf(path, sizeof path, "%s/palette.json", dst);
        fo = fopen(path, "w"); if (!fo) return 1;
        fprintf(fo, "{\"pens\":[0");
        for (int p = 1; p < 16; p++) fprintf(fo, ",%u", r16(pl + (uint32_t)(p - 1) * 2u));
        fprintf(fo, "]}\n");
        fclose(fo);
    }

    {   /* poses.json: own + mirrored own_f per mapped pose */
        FILE *fo;
        snprintf(path, sizeof path, "%s/poses.json", dst);
        fo = fopen(path, "w"); if (!fo) return 1;
        fprintf(fo, "{\"poses\":{\n");
        for (int m = 0; m < nmap; m++) {
            SsCell *cl = cells[map_ss[m]];
            int n = ncell[map_ss[m]];
            fprintf(fo, "%s\"%d\":{", m ? ",\n" : "", map_wf[m]);
            for (int fl = 0; fl < 2; fl++) {
                fprintf(fo, "%s\"%s\":[", fl ? "," : "", fl ? "own_f" : "own");
                for (int i = 0, first = 1; i < n; i++) {
                    unsigned tile = 0; int fx;
                    if (cl[i].owner != (uint8_t)sswid) continue;
                    for (int k = 0; k < nta; k++)
                        if (ta[k].code == cl[i].code && ta[k].chained == cl[i].chain) { tile = ta[k].arena; break; }
                    fx = ((cl[i].flip >> 7) & 1) ^ fl;   /* mirror: x -> -x, flipx toggles */
                    fprintf(fo, "%s{\"tile\":%u,\"x\":%d,\"y\":%d,\"flipx\":%d,\"flipy\":%d,\"chain\":%d,\"pal\":%d}",
                            first ? "" : ",", tile,
                            fl ? -(cl[i].x + dx) : (cl[i].x + dx),
                            ay - cl[i].y,
                            fx, (cl[i].flip >> 6) & 1, cl[i].chain, baseid);
                    first = 0;
                }
                fprintf(fo, "]");
            }
            fprintf(fo, "}");
        }
        fprintf(fo, "\n}}\n");
        fclose(fo);
    }

    {   /* wrestler.json */
        FILE *fo;
        snprintf(path, sizeof path, "%s/wrestler.json", dst);
        fo = fopen(path, "w"); if (!fo) return 1;
        fprintf(fo, "{ \"clone_of\": %d, \"name\": \"%s\" }\n", baseid,
                ss_names[sswid < 9 ? sswid : 8]);
        fclose(fo);
    }
    fprintf(stderr, "import-sstar: %s ready (%d poses, %u tiles) - pack a profile using it\n",
            dst, nmap, ntile);
    return 0;
}
