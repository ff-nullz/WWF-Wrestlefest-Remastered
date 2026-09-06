/* ARENA EXPORT (user 2026-08-29: "arenas are numbered, with a PNG
 * import/export function ... first step: work with the stock arenas").
 *
 *   wfengine --export-arena SCENE DIR
 *
 * An arena is what the composer (scene_map.c, ROM 0x26EA6) leaves in the
 * two 32x32-cell tilemaps for a scene word: the BG plane (mat, apron,
 * top/bottom ropes, crowd) and the FG plane (the crowd band above the
 * sprites, arena-specific dressing), drawn from the shared 16x16 4bpp tile
 * sheet with a 16-bank palette per plane.  The side ropes are sprites
 * (ringhw.c, bank 14) and NOT part of the arena.  The crowd animation is
 * per-scene patch stamping (scenery.c) - the export takes the composed base
 * frame; patches are a later addition.
 *
 * The verb runs the engine headless for a few frames on a stage that maps
 * to SCENE (or the ring-out poke for the ringside views), renders one
 * frame so video.c's latch holds the planes, palette and tiles the game
 * really drew, then writes into DIR:
 *   bg.png / fg.png   512x512 RGBA, pen 0 = transparent on both planes
 *   arena.json        scene, scroll, both palettes (16 banks x 16 pens,
 *                     xBGR444 words) and every cell {code, bank, flipx,
 *                     flipy} - the lossless round trip for --import-arena
 * The PNG pixels are the palette colours exactly (4 bits per channel x17),
 * so an unedited PNG re-imports pixel-exact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"

int wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H);
const uint8_t *wf_video_latch_fg(void);          /* video.c: the latched tilemaps */
const uint8_t *wf_video_latch_bg(void);
const uint8_t *wf_video_bgtile_pens(unsigned code);   /* the 16x16 tile pens */
uint16_t wf_video_palette_word(unsigned index);  /* latched xBGR444 word */
uint32_t wf_video_palette_rgb(unsigned index);
void eng_render_frame(const eng_state *st, uint32_t *pixels, int pitch);

#define CELLS 32
#define TILE 16
#define PLANE_PX (CELLS * TILE)

static void put_tile(uint8_t *rgba, int pw, unsigned code, int cx, int cy, unsigned base, int fx, int fy)
{
    const uint8_t *pens = wf_video_bgtile_pens(code);
    for (int ty = 0; ty < TILE; ty++) {
        int sy = fy ? TILE - 1 - ty : ty;
        for (int tx = 0; tx < TILE; tx++) {
            int sx = fx ? TILE - 1 - tx : tx;
            uint8_t pen = pens ? pens[sy * TILE + sx] : 0;
            uint8_t *px = rgba + (((size_t)(cy * TILE + ty) * (size_t)pw) + (size_t)(cx * TILE + tx)) * 4;
            if (pen == 0) { px[0] = px[1] = px[2] = px[3] = 0; continue; }
            uint32_t c = wf_video_palette_rgb(base + pen);
            px[0] = (uint8_t)(c >> 16); px[1] = (uint8_t)(c >> 8); px[2] = (uint8_t)c; px[3] = 255;
        }
    }
}

int wf_scene_unit_cells(unsigned scene, int plane, uint16_t x, uint16_t y, uint8_t *out16);

/* the scene's source extent in units (x first..last, top row..bottom row in
 * the 0x40 - y row convention), union of both planes; 0 = nothing */
static int scene_extent(unsigned scene, int *x0, int *x1, int *r0, int *r1)
{
    int n = 0;
    *x0 = 999; *x1 = -1; *r0 = 999; *r1 = -1;
    for (int plane = 0; plane < 2; plane++)
        for (int r = 0; r < 64; r++) for (int x = 0; x < 80; x++) {
            uint8_t c[16] = {0}; int nz = 0;
            if (wf_scene_unit_cells(scene, plane, (uint16_t)x, (uint16_t)(0x40 - r), c))
                for (int i = 0; i < (plane ? 8 : 16); i++) if (c[i]) nz = 1;
            if (nz) { n++; if (x < *x0) *x0 = x; if (x > *x1) *x1 = x; if (r < *r0) *r0 = r; if (r > *r1) *r1 = r; }
        }
    return n;
}


/* ---- crowd / scenery animation (scenery.c, ROM 0x285DA cluster) ----
 * Per scene: placement triples (cx, cy in 64-px camera units, anim id) that
 * ACTIVATE an animation while the camera is near, scripts per id {last
 * frame, sync (bit7 = BG layer), (patch cell, ticks)...} and patches = 4x4
 * cells stamped at a fixed torus offset.  Re-expressed as crowd.json + one
 * 64x64 PNG per (id, frame); the patch position is given in SOURCE GRID
 * CELLS (gx, gy) so an author can move it and the engine derives the torus
 * offset the same way the composer does. */
#define AR_TAB_PLACE   0x2867Au
#define AR_TAB_SCRIPT  0x28C66u
#define AR_TAB_PAIR_FG 0x295A4u
#define AR_TAB_PAIR_BG 0x295DCu
static uint16_t ar_rom16(uint32_t a) { return (uint16_t)((tbl_ra8(a) << 8) | tbl_ra8(a + 1)); }
static uint32_t ar_rom32(uint32_t a) { return ((uint32_t)ar_rom16(a) << 16) | ar_rom16(a + 2); }

/* torus cell (cx, cy) -> the source grid cell inside the export window */
static void torus_to_grid(int cx, int cy, uint16_t ox, uint16_t oy, int x0, int r0, int *gx, int *gy)
{
    unsigned d1 = (unsigned)cx / 2, d2 = (unsigned)cy / 2;
    unsigned ux = (unsigned)(uint16_t)(ox + ((d1 - (unsigned)((uint16_t)(ox + 0x10) & 15)) & 15));
    unsigned row = (unsigned)(uint16_t)((0x40 - oy) + ((d2 - (unsigned)((uint16_t)(0x40 - oy) & 15)) & 15));
    *gx = ((int)ux - x0) * 2 + (cx & 1);
    *gy = ((int)row - r0) * 2 + (cy & 1);
}

static int crop_screen, cropped_page;   /* cropped_page: the export in progress is a fixed-camera page */
typedef struct { int id, bg, sync, nf, ingroup; int gx[16], gy[16], ticks[16]; uint8_t *img[16]; } ex_anim;

/* a torus cell unwrapped onto the source grid: the torus is 32 cells, the
 * view may be wider (the ringside strip: 60) - among the in-range copies
 * take the one nearest the animation's placement (its camera position) */
static int unwrap(int g, int ncell, int target)
{
    int best = g, bd = 1 << 30;
    for (int n = -2; n <= 2; n++) { int c = g + 32 * n, d; if (c < 0 || c + 4 > ncell) continue; d = abs(c - target); if (d < bd) { bd = d; best = c; } }
    return best;
}

/* ---- CROWD REGIONS (crowd.c): the ROM's patch animation re-expressed as
 * our own: every lockstep group (patches sharing a sync byte, one timer)
 * becomes ONE region = the rectangle its patches cover, with a complete
 * picture per step (the base art with the step's patches stamped in);
 * an independent (sync 0) patch becomes a 4x4-cell region of its own.
 * Written as crowd.json + crowd/<layer>_<n>_<step>.png.  Repeated steps
 * are aliases ("same_as").  The author may then change any rectangle,
 * step count, timing or picture: the whole rectangle animates. */
static char layers_json[4096];          /* the "layers" object export_layers builds for arena.json */
static int export_layers(const eng_state *st, const char *dir, int x0, int r0, int W, int H, uint8_t *planes[2], int is_out)
{
    unsigned scene = (unsigned)st->scene;
    uint32_t list = ar_rom32(AR_TAB_PLACE + scene * 4u);
    uint16_t ox = (uint16_t)((st->cam_x >> 5) - 3), oy = (uint16_t)((st->cam_y >> 5) + 4);
    uint8_t seen[256] = {0}; int ids[256], nid = 0;
    int pl_cx[512], pl_cy[512], pl_id[512], npl = 0;
    char path[560]; FILE *f; int rc = 0, nreg = 0;
    static ex_anim an[256]; uint8_t grouped[256] = {0};
    uint8_t *plane[2] = { planes[0], planes[1] }; int pw[2] = { W * 32, W * 32 }, ph[2] = { H * 32, H * 32 };

    for (;;) {                                        /* placement triples, bit7 ends */
        uint8_t cx = tbl_ra8(list);
        if (cx & 0x80u) break;
        if (npl < 512) { pl_cx[npl] = cx; pl_cy[npl] = tbl_ra8(list + 1); pl_id[npl] = tbl_ra8(list + 2); if (!seen[pl_id[npl]]) { seen[pl_id[npl]] = 1; ids[nid++] = pl_id[npl]; } npl++; }
        list += 3;
    }
    if (nid) for (int k = 0; k < nid; k++) {          /* decode: script, cells, patch pictures */
        ex_anim *A = &an[k]; int id = ids[k];
        uint32_t tab = ar_rom32(AR_TAB_SCRIPT + scene * 4u), sa = ar_rom32(tab + (uint32_t)id * 4u);
        int last = tbl_ra8(sa), sync = tbl_ra8(sa + 1), bg = (sync & 0x80) != 0;
        uint32_t pair = (bg ? AR_TAB_PAIR_BG : AR_TAB_PAIR_FG) + scene * 8u, off_tab = ar_rom32(pair), data = ar_rom32(pair + 4);
        A->id = id; A->bg = bg; A->sync = sync & 0x7F; A->nf = last + 1 > 16 ? 16 : last + 1; A->ingroup = 0;
        for (int fr = 0; fr < A->nf; fr++) {
            int cell = tbl_ra8(sa + 2u + (uint32_t)fr * 2u), pcx = 0, pcy = 0;
            uint16_t dst = ar_rom16(off_tab + (uint32_t)cell * 2u);
            uint32_t src = data + (uint32_t)cell * (bg ? 32u : 64u);
            int cx = bg ? (dst % 0x40) / 2 : (dst % 0x80) / 4, cy = bg ? dst / 0x40 : dst / 0x80;
            A->ticks[fr] = tbl_ra8(sa + 3u + (uint32_t)fr * 2u);
            torus_to_grid(cx, cy, ox, oy, x0, r0, &A->gx[fr], &A->gy[fr]);
            for (int i = 0; i < npl; i++) if (pl_id[i] == id) { pcx = pl_cx[i]; pcy = pl_cy[i]; break; }
            /* the placement's own grid cell (measured on the ring views: gx = 4*cx - 2*x0 + 4, gy = 124 - 4*cy - 2*r0) */
            A->gx[fr] = unwrap(A->gx[fr], W * 2, pcx * 4 - x0 * 2 + 4);
            A->gy[fr] = unwrap(A->gy[fr], H * 2, 124 - pcy * 4 - r0 * 2);
            A->img[fr] = calloc(64 * 64, 4);
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
                if (bg) { unsigned attr = ar_rom16(src + (uint32_t)r * 8u + (uint32_t)c * 2u);
                          put_tile(A->img[fr], 64, attr & 0x0FFFu, c, r, 0x0C00u + (attr >> 12) * 16u, 0, 0); }
                else    { unsigned w0 = ar_rom16(src + (uint32_t)r * 16u + (uint32_t)c * 4u), w1 = ar_rom16(src + (uint32_t)r * 16u + (uint32_t)c * 4u + 2u);
                          put_tile(A->img[fr], 64, w1 & 0x1FFFu, c, r, 0x1000u + (w0 & 0x0Fu) * 16u, (w0 & 0x40) != 0, (w0 & 0x80) != 0); }
            }
        }
    }
    {   /* patches that never touch this plane (another page of the same scene word) are
         * dropped; for a SCREEN-CROPPED page (a fixed camera) so are patches the ROM's
         * placement cull (0x28608) would never activate at this camera */
        int keep = 0;
        unsigned ux = ((unsigned)st->cam_x >> 5), uy = ((unsigned)st->cam_y >> 5);
        uint8_t xlo = (uint8_t)(((ux + 1) >> 1) - 2), xhi = (uint8_t)(((ux - 1) >> 1) + 6), yhi = (uint8_t)(((uy + 1) >> 1) + 2), ylo = (uint8_t)(yhi - 8);
        for (int k = 0; k < nid; k++) { int in = 0;
            for (int fr = 0; fr < an[k].nf; fr++) if (an[k].gx[fr] + 4 > 0 && an[k].gx[fr] < W * 2 && an[k].gy[fr] + 4 > 0 && an[k].gy[fr] < H * 2) in = 1;
            if (in && cropped_page) { int act = 0;
                for (int i = 0; i < npl; i++) if (pl_id[i] == an[k].id && (uint8_t)pl_cx[i] >= xlo && (uint8_t)pl_cx[i] < xhi && (uint8_t)pl_cy[i] >= ylo && (uint8_t)pl_cy[i] < yhi) act = 1;
                if (!act) in = 0; }
            if (in) an[keep++] = an[k]; else { for (int fr = 0; fr < an[k].nf; fr++) free(an[k].img[fr]); } }
        if (keep < nid) fprintf(stderr, "export-arena: %d patch%s outside this page dropped\n", nid - keep, nid - keep == 1 ? "" : "es");
        nid = keep;
    }
    /* ---- LAYERS (2026-08-30, one format): per hardware plane, N complete
     * pictures (the base plane with the lockstep step k stamped in) plus,
     * on the under plane, a static overlay = the ring (the cells no patch
     * ever touches below the crowd's full-width band) cut out of the
     * pictures.  under = the ROM's FG plane (drawn under the sprites in
     * the ring scenes' 0x7C order), over = the ROM's BG plane (drawn OVER
     * the sprites: the rope lines, the ringside far crowd). */
    {
        char lj[4096]; int ln = 0; int ticks = 4;
        static uint8_t chg[2][128][128];
        memset(chg, 0, sizeof chg);
        ln += snprintf(lj + ln, sizeof lj - ln, "\"layers\": {");
        for (int pl = 0; pl < 2; pl++) {
            int nf = 1, CWc = pw[pl] / 16, CHc = ph[pl] / 16, first = 1;
            const char *lname = pl ? "over" : "under";
            const char *fname = pl ? (is_out ? "backcrowd" : "ropes") : "crowd";
            uint8_t *out = malloc((size_t)pw[pl] * ph[pl] * 4);
            /* the plane's step count = the longest lockstep group on it; every patch
             * footprint = a changing cell */
            for (int k = 0; k < nid; k++) if (an[k].bg == pl) { if (an[k].nf > nf) nf = an[k].nf; ticks = an[k].ticks[0] ? an[k].ticks[0] : ticks;
                for (int fr = 0; fr < an[k].nf; fr++) for (int cy = 0; cy < 4; cy++) for (int cx = 0; cx < 4; cx++) { int ax = an[k].gx[fr] + cx, ay = an[k].gy[fr] + cy; if (ax >= 0 && ax < 128 && ay >= 0 && ay < 128) chg[pl][ay][ax] = 1; } }
            /* the overlay MASK (under plane only, 2026-08-30 "crowd only / ring
             * only"): every static opaque cell below the crowd's full-width band,
             * plus the static cells 4-connected to those up through the band -
             * the mat top, the logo, the posts (the ring rises into the crowd
             * rows).  overlay_rect = its bounding box (the editor's label). */
            static uint8_t rmask[128][128];
            int rx0 = 1 << 30, ry0 = 1 << 30, rx1 = -1, ry1 = -1, nmask = 0;
            uint8_t *pxchg = NULL, *pxmask = NULL, *rbmask = NULL; int nrb = 0;   /* per-pixel: changed in some step / the ring / the rear ropes */
            memset(rmask, 0, sizeof rmask);
            if (pl == 0 && nf > 1) {
                int band_bottom = -1, grew = 1;
                static uint8_t sopq[128][128], pchg[128][128];
                /* "changes" = pixel-different in SOME step (the patch footprints
                 * are 4x4-cell stamps that also cover ring cells whose pixels
                 * never change) */
                memset(pchg, 0, sizeof pchg);
                pxchg = calloc((size_t)pw[pl] * ph[pl], 1);
                {   uint8_t *base = malloc((size_t)pw[pl] * ph[pl] * 4), *stp = malloc((size_t)pw[pl] * ph[pl] * 4);
                    for (int fr = 0; fr < nf; fr++) {
                        uint8_t *dst = fr ? stp : base;
                        memcpy(dst, plane[pl], (size_t)pw[pl] * ph[pl] * 4);
                        for (int k = 0; k < nid; k++) if (an[k].bg == pl) {
                            int f2 = fr % an[k].nf, px = an[k].gx[f2] * 16, py = an[k].gy[f2] * 16;
                            for (int y = 0; y < 64; y++) { if (py + y < 0 || py + y >= ph[pl]) continue;
                                for (int x = 0; x < 64; x++) if (px + x >= 0 && px + x < pw[pl]) memcpy(dst + ((size_t)(py + y) * pw[pl] + (size_t)(px + x)) * 4, an[k].img[f2] + ((size_t)y * 64 + (size_t)x) * 4, 4); }
                        }
                        if (fr) for (int cy = 0; cy < CHc; cy++) for (int cx = 0; cx < CWc; cx++) {
                            if (pchg[cy][cx]) continue;
                            for (int y = 0; y < 16 && !pchg[cy][cx]; y++)
                                if (memcmp(base + ((size_t)(cy * 16 + y) * pw[pl] + (size_t)cx * 16) * 4, stp + ((size_t)(cy * 16 + y) * pw[pl] + (size_t)cx * 16) * 4, 16 * 4)) pchg[cy][cx] = 1;
                        }
                        if (fr) for (size_t i = 0; i < (size_t)pw[pl] * ph[pl]; i++)      /* per-pixel: changed in some step */
                            if (memcmp(base + i * 4, stp + i * 4, 4)) pxchg[i] = 1;
                    }
                    free(base); free(stp);
                }
                for (int y = 0; y < CHc; y++) { int n = 0; for (int x = 0; x < CWc; x++) n += chg[pl][y][x]; if (n * 10 >= CWc * 8) band_bottom = y; }
                for (int y = 0; y < CHc; y++) for (int x = 0; x < CWc; x++) {
                    int opaque = 0;
                    sopq[y][x] = 0;
                    if (pchg[y][x]) continue;
                    for (int yy = 0; yy < 16 && !opaque; yy++) for (int xx = 0; xx < 16; xx++) if (plane[pl][((size_t)(y * 16 + yy) * pw[pl] + (size_t)(x * 16 + xx)) * 4 + 3] >= 128) { opaque = 1; break; }
                    sopq[y][x] = (uint8_t)opaque;
                    if (opaque && y > band_bottom) rmask[y][x] = 1;                /* the seed: below the band */
                }
                while (grew) {                                                     /* grow up through the band */
                    grew = 0;
                    for (int y = CHc - 1; y >= 0; y--) for (int x = 0; x < CWc; x++) {
                        if (rmask[y][x] || !sopq[y][x]) continue;
                        if ((y + 1 < CHc && rmask[y + 1][x]) || (y > 0 && rmask[y - 1][x]) || (x > 0 && rmask[y][x - 1]) || (x + 1 < CWc && rmask[y][x + 1])) { rmask[y][x] = 1; grew = 1; }
                    }
                }
                /* PIXEL refinement (2026-08-30 "the ring is cut flat at the crowd
                 * band"): the posts and the far corner pads share tiles with the
                 * animated crowd, so the cell mask stops under them.  Grow the ring
                 * per PIXEL into the unmasked cells: a pixel joins when it never
                 * changes across the steps, is 4-connected to ring pixels and has
                 * one of the ring's own colours (the colours of the masked cells);
                 * the composite stays exact - every pixel keeps its colour, it only
                 * moves to the overlay. */
                pxmask = calloc((size_t)pw[pl] * ph[pl], 1);
                {   static uint8_t ringcol[4096]; int grew2 = 1, npx = 0;
                    memset(ringcol, 0, sizeof ringcol);
                    for (int cy = 0; cy < CHc; cy++) for (int cx = 0; cx < CWc; cx++) if (rmask[cy][cx])
                        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
                            const uint8_t *px = plane[pl] + ((size_t)(cy * 16 + y) * pw[pl] + (size_t)(cx * 16 + x)) * 4;
                            pxmask[(size_t)(cy * 16 + y) * pw[pl] + (size_t)(cx * 16 + x)] = 1;
                            if (px[3] >= 128) ringcol[((px[0] + 8) / 17) | (((px[1] + 8) / 17) << 4) | (((px[2] + 8) / 17) << 8)] = 1;
                        }
                    while (grew2) {
                        grew2 = 0;
                        for (int y = 1; y < ph[pl] - 1; y++) for (int x = 1; x < pw[pl] - 1; x++) {
                            size_t i = (size_t)y * pw[pl] + (size_t)x; const uint8_t *px = plane[pl] + i * 4;
                            if (pxmask[i] || pxchg[i] || px[3] < 128) continue;
                            if (rmask[y / 16][x / 16]) continue;
                            if (!ringcol[((px[0] + 8) / 17) | (((px[1] + 8) / 17) << 4) | (((px[2] + 8) / 17) << 8)]) continue;
                            if (pxmask[i - 1] || pxmask[i + 1] || pxmask[i - pw[pl]] || pxmask[i + pw[pl]]) { pxmask[i] = 1; grew2 = 1; npx++; }
                        }
                    }
                    fprintf(stderr, "export-arena: ring mask + %d pixels grown into the crowd band\n", npx);
                    /* the REAR ROPES (under the wrestlers, between the posts): static
                     * pixels in long horizontal runs (>= 32 px) above the ring mask -
                     * their own layer ropes_back.png (a crowd speck is never that long) */
                    if (!is_out) {
                        int px0 = 1 << 30, px1 = -1, py0 = 1 << 30;
                        for (size_t i = 0; i < (size_t)pw[pl] * ph[pl]; i++) if (pxmask[i]) { int x = (int)(i % pw[pl]), y = (int)(i / pw[pl]); if (x < px0) px0 = x; if (x > px1) px1 = x; if (y < py0) py0 = y; }
                        rbmask = calloc((size_t)pw[pl] * ph[pl], 1);
                        for (int y = py0; y < ph[pl]; y++) {
                            int run = 0;
                            for (int x = px0; x <= px1 + 1; x++) {
                                size_t i = (size_t)y * pw[pl] + (size_t)x;
                                int ok = x <= px1 && !pxmask[i] && !pxchg[i] && plane[pl][i * 4 + 3] >= 128;
                                if (ok) run++;
                                else { if (run >= 32) for (int k = x - run; k < x; k++) { rbmask[(size_t)y * pw[pl] + (size_t)k] = 1; nrb++; } run = 0; }
                            }
                        }
                        fprintf(stderr, "export-arena: ropes_back.png = %d pixels (static runs above the ring)\n", nrb);
                    }
                    for (int cy = 0; cy < CHc; cy++) for (int cx = 0; cx < CWc; cx++) if (!rmask[cy][cx]) {   /* the bbox covers them */
                        int any = 0;
                        for (int y = 0; y < 16 && !any; y++) for (int x = 0; x < 16; x++) if (pxmask[(size_t)(cy * 16 + y) * pw[pl] + (size_t)(cx * 16 + x)]) { any = 1; break; }
                        if (any) { if (cx < rx0) rx0 = cx; if (cy < ry0) ry0 = cy; if (cx > rx1) rx1 = cx; if (cy > ry1) ry1 = cy; }
                    }
                }
                if (getenv("WF_ARENA_DBG"))                                        /* the cell map: # overlay, s static, . changing/blank */
                    for (int y = 0; y < CHc; y++) { char row[130]; for (int x = 0; x < CWc; x++) row[x] = rmask[y][x] ? '#' : sopq[y][x] ? 's' : pchg[y][x] ? '.' : ' '; row[CWc] = 0; fprintf(stderr, "%3d %s\n", y, row); }
                for (int y = 0; y < CHc; y++) for (int x = 0; x < CWc; x++) if (rmask[y][x]) {
                    nmask++;
                    if (x < rx0) rx0 = x; if (y < ry0) ry0 = y; if (x > rx1) rx1 = x; if (y > ry1) ry1 = y;
                }
            }
            ln += snprintf(lj + ln, sizeof lj - ln, "%s\n    \"%s\": { \"frames\": [", pl ? "," : "", lname);
            { uint8_t *distinct[16]; int ndist = 0, stepf[16], stept[16];
            for (int fr = 0; fr < nf; fr++) {
                int same = -1, tk = 4;
                memcpy(out, plane[pl], (size_t)pw[pl] * ph[pl] * 4);
                for (int k = 0; k < nid; k++) if (an[k].bg == pl) {
                    int f2 = fr % an[k].nf, px = an[k].gx[f2] * 16, py = an[k].gy[f2] * 16;
                    for (int y = 0; y < 64; y++) { if (py + y < 0 || py + y >= ph[pl]) continue;
                        for (int x = 0; x < 64; x++) if (px + x >= 0 && px + x < pw[pl]) memcpy(out + ((size_t)(py + y) * pw[pl] + (size_t)(px + x)) * 4, an[k].img[f2] + ((size_t)y * 64 + (size_t)x) * 4, 4); }
                }
                if (rx1 >= 0 && pxmask)                        /* the ring mask is the overlay's: clear it from the crowd pictures */
                    for (size_t i = 0; i < (size_t)pw[pl] * ph[pl]; i++) if (pxmask[i] || (rbmask && rbmask[i])) memset(out + i * 4, 0, 4);
                for (int k = 0; k < nid; k++) if (an[k].bg == pl) { tk = an[k].ticks[fr % an[k].nf]; break; }
                for (int d = 0; d < ndist && same < 0; d++) if (!memcmp(distinct[d], out, (size_t)pw[pl] * ph[pl] * 4)) same = d;
                if (same < 0) {                                /* a new picture */
                    distinct[ndist] = malloc((size_t)pw[pl] * ph[pl] * 4); memcpy(distinct[ndist], out, (size_t)pw[pl] * ph[pl] * 4);
                    if (nf > 1) snprintf(path, sizeof path, "%s/%s_%d.png", dir, fname, ndist); else snprintf(path, sizeof path, "%s/%s.png", dir, fname);
                    if (wf_art_write_rgba_png(path, out, pw[pl], ph[pl])) rc = 1;
                    ln += snprintf(lj + ln, sizeof lj - ln, "%s\"%s\"", first ? "" : ", ", path + strlen(dir) + 1); first = 0;
                    same = ndist++;
                }
                stepf[fr] = same; stept[fr] = tk;
            }
            ln += snprintf(lj + ln, sizeof lj - ln, "]");
            if (nf > 1) {                                      /* the step sequence: which picture, how long (ROM ticks) */
                ln += snprintf(lj + ln, sizeof lj - ln, ", \"steps\": [");
                for (int fr = 0; fr < nf; fr++) ln += snprintf(lj + ln, sizeof lj - ln, "%s{ \"frame\": %d, \"ticks\": %d }", fr ? ", " : "", stepf[fr], stept[fr]);
                ln += snprintf(lj + ln, sizeof lj - ln, "]");
            }
            for (int d = 0; d < ndist; d++) free(distinct[d]); }
            if (rx1 >= 0) {                                    /* the overlay: the ring mask */
                const char *oname = is_out ? "floor" : "ring";
                memset(out, 0, (size_t)pw[pl] * ph[pl] * 4);
                if (pxmask) for (size_t i = 0; i < (size_t)pw[pl] * ph[pl]; i++) if (pxmask[i]) memcpy(out + i * 4, plane[pl] + i * 4, 4);
                snprintf(path, sizeof path, "%s/%s.png", dir, oname);
                if (wf_art_write_rgba_png(path, out, pw[pl], ph[pl])) rc = 1;
                fprintf(stderr, "export-arena: %s.png = %d static cells (bbox %d,%d %dx%d)\n", oname, nmask, rx0, ry0, rx1 - rx0 + 1, ry1 - ry0 + 1);
                ln += snprintf(lj + ln, sizeof lj - ln, ", \"overlay\": \"%s.png\", \"overlay_rect\": [%d, %d, %d, %d]", oname, rx0, ry0, rx1 - rx0 + 1, ry1 - ry0 + 1);
                if (rbmask && nrb) {                            /* the rear ropes: a second overlay, composited after the ring */
                    memset(out, 0, (size_t)pw[pl] * ph[pl] * 4);
                    for (size_t i = 0; i < (size_t)pw[pl] * ph[pl]; i++) if (rbmask[i]) memcpy(out + i * 4, plane[pl] + i * 4, 4);
                    snprintf(path, sizeof path, "%s/ropes_back.png", dir);
                    if (wf_art_write_rgba_png(path, out, pw[pl], ph[pl])) rc = 1;
                    ln += snprintf(lj + ln, sizeof lj - ln, ", \"overlays\": [\"%s.png\", \"ropes_back.png\"]", oname);
                }
            }
            ln += snprintf(lj + ln, sizeof lj - ln, " }");
            free(out); free(pxchg); free(pxmask); free(rbmask);
            fprintf(stderr, "export-arena: %s plane: %d frame%s%s\n", lname, nf, nf == 1 ? "" : "s", rx1 >= 0 ? " + overlay" : "");
        }
        ln += snprintf(lj + ln, sizeof lj - ln, "\n  }"); (void)ticks;
        snprintf(layers_json, sizeof layers_json, "%s", lj);
    }
    for (int k = 0; k < nid; k++) for (int fr = 0; fr < an[k].nf; fr++) { free(an[k].img[fr]); an[k].img[fr] = NULL; }
    return rc;
}

/* SCREEN CROP (the scenes): export only the part of the source grid the
 * player sees, so each scene gets its own page instead of scene word 4's
 * whole front-end sheet.  Set for the next export, cleared after it. */
void tool_export_arena_crop_screen(int on) { crop_screen = on; }
void wf_video_latch_scroll_fg(int *sx, int *sy);

int tool_export_arena(const eng_state *st, const char *dir)
{
    static uint32_t pixels[512 * 384];
    char path[512];
    FILE *f; uint8_t *planes[2] = { NULL, NULL };
    int x0, x1, r0, r1, W, H, PW, PH;
    uint8_t *rgba;
    unsigned scene = (unsigned)st->scene;

    eng_render_frame(st, pixels, 0);          /* fresh latch: palette + tiles as drawn */
    if (!scene_extent(scene, &x0, &x1, &r0, &r1)) { fprintf(stderr, "export-arena: scene %u has no source cells\n", scene); return 1; }
    if (crop_screen) {                        /* the visible window in grid cells -> whole units */
        int sx, sy, gx, gy, cw = (wf_view_w() + 15) / 16, chh = (wf_view_h() + 8 + 15) / 16, ux0, ux1, ur0, ur1;   /* +8: the row offset (base_y - 8) */
        uint16_t ox = (uint16_t)((st->cam_x >> 5) - 3), oy = (uint16_t)((st->cam_y >> 5) + 4);
        wf_video_latch_scroll_fg(&sx, &sy);
        torus_to_grid(((sx & 511) / 16) & 31, (((sy & 511) + 8) / 16) & 31, ox, oy, x0, r0, &gx, &gy);
        ux0 = x0 + (gx < 0 ? (gx - 1) / 2 : gx / 2); ux1 = x0 + (gx + cw - 1) / 2;
        ur0 = r0 + (gy < 0 ? (gy - 1) / 2 : gy / 2); ur1 = r0 + (gy + chh - 1) / 2;
        if (ux0 < x0) ux0 = x0; if (ux1 > x1) ux1 = x1; if (ur0 < r0) ur0 = r0; if (ur1 > r1) ur1 = r1;
        fprintf(stderr, "export-arena: screen crop: scroll %d,%d cam %d,%d ox %u oy %u -> grid cell %d,%d -> units %d..%d rows %d..%d (grid x0 %d r0 %d)\n", sx, sy, (int)st->cam_x, (int)st->cam_y, ox, oy, gx, gy, ux0, ux1, ur0, ur1, x0, r0);
        x0 = ux0; x1 = ux1; r0 = ur0; r1 = ur1; crop_screen = 0; cropped_page = 1;
    }
    W = x1 - x0 + 1; H = r1 - r0 + 1; PW = W * 2 * TILE; PH = H * 2 * TILE;
    rgba = calloc((size_t)PW * PH, 4);
    if (!rgba) return 1;
    mkdir(dir, 0775);

    for (int plane = 0; plane < 2; plane++) {   /* 0 = FG, 1 = BG; pen 0 transparent on both */
        memset(rgba, 0, (size_t)PW * PH * 4);
        for (int r = 0; r < H; r++) for (int x = 0; x < W; x++) {
            uint8_t c[16] = {0};
            uint16_t ux = (uint16_t)(x0 + x), uy = (uint16_t)(0x40 - (r0 + r));
            wf_scene_unit_cells(scene, plane, ux, uy, c);
            /* the base grid is the pure ROM source: the crowd patches are exported as
             * animations (anim.json) and stamped by the engine exactly when the ROM
             * would - baking the composed frame here left stamped art where stock
             * still shows the placeholder cells (revealed by a camera pan) */
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) {
                int cx = x * 2 + sx, cy = r * 2 + sy;
                if (plane) {
                    unsigned attr = ((unsigned)c[sy * 4 + sx * 2] << 8) | c[sy * 4 + sx * 2 + 1];
                    put_tile(rgba, PW, attr & 0x0FFFu, cx, cy, 0x0C00u + (attr >> 12) * 16u, 0, 0);
                } else {
                    const uint8_t *q = c + sy * 8 + sx * 4;
                    unsigned w0 = ((unsigned)q[0] << 8) | q[1], w1 = ((unsigned)q[2] << 8) | q[3];
                    put_tile(rgba, PW, w1 & 0x1FFFu, cx, cy, 0x1000u + (w0 & 0x0Fu) * 16u, (w0 & 0x40) != 0, (w0 & 0x80) != 0);
                }
            }
        }
        planes[plane] = rgba; rgba = calloc((size_t)PW * PH, 4);
    }
    free(rgba);
    { int rc = export_layers(st, dir, x0, r0, W, H, planes, scene == 2 || scene == 6); if (rc) { free(planes[0]); free(planes[1]); return rc; } }
    if (!(scene == 2 || scene == 6) && !cropped_page) {           /* the ring view: the side-rope art too */
        extern int tool_export_rope_frames(const char *dir); char rd[600];
        snprintf(rd, sizeof rd, "%s/ropes", dir); tool_export_rope_frames(rd);
    }
    free(planes[0]); free(planes[1]);

    snprintf(path, sizeof path, "%s/arena.json", dir);
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "export-arena: cannot write %s\n", path); return 1; }
    fprintf(f, "{\n  \"format\": \"wfengine-arena-3\",\n  \"scene\": %u,\n  \"stage\": %u,\n  \"cam\": [%d, %d],\n",
            scene, (unsigned)(st->stage & 0xFF), st->cam_x, st->cam_y);
    fprintf(f, "  \"origin\": [%d, %d],\n  \"size_units\": [%d, %d],\n  \"unit_px\": 32,\n  %s,\n", x0, r0, W, H, layers_json);
    for (int pl = 0; pl < 2; pl++) {
        fprintf(f, "  \"%s_palette\": [", pl ? "fg" : "bg");
        for (int b = 0; b < 16; b++) {
            fprintf(f, "%s\n    [", b ? "," : "");
            for (int k = 0; k < 16; k++)
                fprintf(f, "%s\"%03X\"", k ? ", " : "", wf_video_palette_word((pl ? 0x1000u : 0x0C00u) + (unsigned)b * 16u + (unsigned)k) & 0xFFFu);
            fprintf(f, "]");
        }
        fprintf(f, "\n  ],\n");
    }
    fprintf(f, "  \"note\": \"layers.under = the plane drawn UNDER the sprites: crowd frames (whole plane, one per step) with the overlay (ring/floor) composited on top at pack time; layers.over = the plane drawn OVER the sprites (rope lines / the ringside far crowd); ticks per step; palettes 16 banks x 16 pens\"\n}\n");
    fclose(f);
    fprintf(stderr, "export-arena: scene %u -> %s (%dx%d units = %dx%d px, origin unit %d row %d; layers + arena.json)\n", scene, dir, W, H, PW, PH, x0, r0);
    cropped_page = 0;
    return 0;
}

/* ---- PACK: mods/<layer>/arenas/<scene>/{bg.png,fg.png,arena.json} ->
 * the arena pak sections "ring" / "ringside" (layout: src/arena.c header).
 * Cells are quantised against 16 banks of 16 pens per plane: the banks come
 * from arena.json when present (an export, or a hand-written palette), else
 * they are built greedily from the cells; every cell then takes the bank
 * that covers its colours best and each pixel its nearest pen.  Tiles are
 * deduplicated by content; flipped copies become their own tile. */
#include "pak.h"
#include "json.h"
#include "profile.h"
int wf_video_load_rgba_png(const char *path, uint8_t **rgba, int *w, int *h);

#define MAXT 4096
typedef struct { uint16_t pal[16][16]; int npen[16]; int nbank; } palset;

static unsigned rgb444(const uint8_t *px) { return ((px[0] + 8) / 17u) | (((px[1] + 8) / 17u) << 4) | (((px[2] + 8) / 17u) << 8); }
static int cdist(unsigned a, unsigned b)
{
    int dr = (int)(a & 15) - (int)(b & 15), dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15), db = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
    return dr * dr + dg * dg + db * db;
}
/* nearest pen of bank b for colour c; FG keeps pen 0 for transparency */
static int nearest_pen(const palset *ps, int b, unsigned c, int first)
{
    int best = first, bd = 1 << 30;
    for (int k = first; k < 16; k++) { int d = cdist(c, ps->pal[b][k]); if (d < bd) { bd = d; best = k; } }
    return best;
}
static int bank_cost(const palset *ps, int b, const unsigned *cols, const int *cnt, int n, int first)
{
    long cost = 0;
    for (int i = 0; i < n; i++) cost += (long)cdist(cols[i], ps->pal[b][nearest_pen(ps, b, cols[i], first)]) * cnt[i];
    return cost > (1 << 30) ? (1 << 30) : (int)cost;
}
/* greedy bank builder (superseded by cluster_banks, kept for reference) */
static void __attribute__((unused)) greedy_bank(palset *ps, const unsigned *cols, int n, int first)
{
    for (int b = 0; b < ps->nbank; b++) {
        int add = 0;
        for (int i = 0; i < n; i++) { int k; for (k = first; k < ps->npen[b]; k++) if (ps->pal[b][k] == cols[i]) break; if (k == ps->npen[b]) add++; }
        if (ps->npen[b] + add <= 16) {
            for (int i = 0; i < n; i++) { int k; for (k = first; k < ps->npen[b]; k++) if (ps->pal[b][k] == cols[i]) break; if (k == ps->npen[b]) ps->pal[b][ps->npen[b]++] = (uint16_t)cols[i]; }
            return;
        }
    }
    if (ps->nbank < 16) { int b = ps->nbank++; ps->npen[b] = first; for (int i = 0; i < n && ps->npen[b] < 16; i++) ps->pal[b][ps->npen[b]++] = (uint16_t)cols[i]; }
}
static void cell_colours(const uint8_t *rgba, int pw, int cx, int cy, unsigned *cols, int *cnt, int *n)
{
    *n = 0;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        const uint8_t *px = rgba + (((size_t)(cy * 16 + y) * (size_t)pw) + (size_t)(cx * 16 + x)) * 4;
        unsigned c; int i;
        if (px[3] < 128) continue;              /* transparent = pen 0, both planes */
        c = rgb444(px);
        for (i = 0; i < *n; i++) if (cols[i] == c) { cnt[i]++; break; }
        if (i == *n && *n < 256) { cols[*n] = c; cnt[*n] = 1; (*n)++; }
    }
}

/* GLOBAL bank builder (2026-08-30: the greedy one filled the banks with the
 * first cells' greys and desaturated the colour template): every cell of
 * every frame is a colour histogram; k-means (16) on the cells' mean colour
 * seeds the banks, each bank's pens are a weighted median cut of its member
 * cells' colours, then cells move to the bank that quantises them best and
 * the banks are rebuilt - three rounds. */
typedef struct { unsigned cols[256]; int cnt[256]; int n; float mean[3]; int bank; } cellrec;
typedef struct { unsigned c; long w; } wcol;
static void wc_mean(const wcol *v, int n, float m[3]) { double a[3] = { 0, 0, 0 }, W = 0; for (int i = 0; i < n; i++) { a[0] += (double)(v[i].c & 15) * v[i].w; a[1] += (double)((v[i].c >> 4) & 15) * v[i].w; a[2] += (double)((v[i].c >> 8) & 15) * v[i].w; W += v[i].w; } if (W <= 0) W = 1; m[0] = (float)(a[0] / W); m[1] = (float)(a[1] / W); m[2] = (float)(a[2] / W); }
static int wc_axis;
static int wc_cmp(const void *a, const void *b) { unsigned ca = ((const wcol *)a)->c >> (wc_axis * 4) & 15, cb = ((const wcol *)b)->c >> (wc_axis * 4) & 15; return (int)ca - (int)cb; }
/* median cut of v[0..n) into up to k boxes; writes the boxes' weighted means */
static int median_cut(wcol *v, int n, int k, uint16_t *out)
{
    int lo[64], hi[64], nb = 1; lo[0] = 0; hi[0] = n;
    if (n <= 0) return 0;
    while (nb < k) {
        int bi = -1; long bw = -1;
        for (int b = 0; b < nb; b++) {           /* the box with the largest weighted range */
            int mn[3] = { 16, 16, 16 }, mx[3] = { -1, -1, -1 }; long W = 0;
            for (int i = lo[b]; i < hi[b]; i++) for (int a = 0; a < 3; a++) { int c = (int)(v[i].c >> (a * 4)) & 15; if (c < mn[a]) mn[a] = c; if (c > mx[a]) mx[a] = c; W += v[i].w; }
            { int r = 0; for (int a = 0; a < 3; a++) if (mx[a] - mn[a] > r) r = mx[a] - mn[a]; if (hi[b] - lo[b] >= 2 && r > 0 && (long)r * W > bw) { bw = (long)r * W; bi = b; } }
        }
        if (bi < 0) break;
        {   int mn[3] = { 16, 16, 16 }, mx[3] = { -1, -1, -1 }, ax = 0, r = -1; long W = 0, acc = 0; int split;
            for (int i = lo[bi]; i < hi[bi]; i++) for (int a = 0; a < 3; a++) { int c = (int)(v[i].c >> (a * 4)) & 15; if (c < mn[a]) mn[a] = c; if (c > mx[a]) mx[a] = c; }
            for (int a = 0; a < 3; a++) if (mx[a] - mn[a] > r) { r = mx[a] - mn[a]; ax = a; }
            wc_axis = ax; qsort(v + lo[bi], (size_t)(hi[bi] - lo[bi]), sizeof *v, wc_cmp);
            for (int i = lo[bi]; i < hi[bi]; i++) W += v[i].w;
            split = lo[bi] + 1;
            for (int i = lo[bi]; i < hi[bi] - 1; i++) { acc += v[i].w; if (acc * 2 >= W) { split = i + 1; break; } }
            lo[nb] = split; hi[nb] = hi[bi]; hi[bi] = split; nb++;
        }
    }
    for (int b = 0; b < nb; b++) { float m[3]; wc_mean(v + lo[b], hi[b] - lo[b], m); out[b] = (uint16_t)(((unsigned)(m[0] + 0.5f) & 15) | (((unsigned)(m[1] + 0.5f) & 15) << 4) | (((unsigned)(m[2] + 0.5f) & 15) << 8)); }
    return nb;
}
static void rebuild_bank(palset *ps, int b, cellrec *cells, int nc, int first)
{
    static wcol *v; static int vcap; int n = 0;
    for (int i = 0; i < nc; i++) if (cells[i].bank == b) n += cells[i].n;
    if (n > vcap) { vcap = n + 1024; v = realloc(v, (size_t)vcap * sizeof *v); }
    n = 0;
    for (int i = 0; i < nc; i++) if (cells[i].bank == b)
        for (int k = 0; k < cells[i].n; k++) {          /* merge equal colours */
            int j; for (j = 0; j < n && j < 4096; j++) if (v[j].c == cells[i].cols[k]) { v[j].w += cells[i].cnt[k]; break; }
            if (j == n || j == 4096) { if (j == 4096) continue; v[n].c = cells[i].cols[k]; v[n].w = cells[i].cnt[k]; n++; }
        }
    ps->npen[b] = first;
    if (n) ps->npen[b] = first + median_cut(v, n, 16 - first, ps->pal[b] + first);
    for (int k = ps->npen[b]; k < 16; k++) ps->pal[b][k] = ps->pal[b][first];   /* pad: a bank always has 16 pens */
    ps->npen[b] = 16;
}
static void cluster_banks(palset *ps, cellrec *cells, int nc, int first)
{
    float cen[16][3]; int nb = nc < 16 ? (nc ? nc : 1) : 16;
    /* seed: cells sorted by luma of their mean, quantile centres */
    { int *idx = malloc((size_t)nc * sizeof *idx); for (int i = 0; i < nc; i++) idx[i] = i;
      for (int i = 1; i < nc; i++) for (int j = i; j > 0; j--) { float la = cells[idx[j - 1]].mean[0] + cells[idx[j - 1]].mean[1] * 2 + cells[idx[j - 1]].mean[2], lb = cells[idx[j]].mean[0] + cells[idx[j]].mean[1] * 2 + cells[idx[j]].mean[2]; if (la > lb) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; } else break; }
      for (int b = 0; b < nb; b++) { int i = idx[(int)(((long)b * 2 + 1) * nc / (2 * nb))]; memcpy(cen[b], cells[i].mean, sizeof cen[b]); }
      free(idx); }
    for (int it = 0; it < 6; it++) {                  /* k-means on the mean colour */
        double acc[16][3] = { { 0 } }; int num[16] = { 0 };
        for (int i = 0; i < nc; i++) { int best = 0; float bd = 1e30f; for (int b = 0; b < nb; b++) { float d = 0; for (int a = 0; a < 3; a++) d += (cells[i].mean[a] - cen[b][a]) * (cells[i].mean[a] - cen[b][a]); if (d < bd) { bd = d; best = b; } } cells[i].bank = best; for (int a = 0; a < 3; a++) acc[best][a] += cells[i].mean[a]; num[best]++; }
        for (int b = 0; b < nb; b++) if (num[b]) for (int a = 0; a < 3; a++) cen[b][a] = (float)(acc[b][a] / num[b]);
    }
    ps->nbank = nb;
    for (int round = 0; round < 3; round++) {
        for (int b = 0; b < nb; b++) rebuild_bank(ps, b, cells, nc, first);
        for (int i = 0; i < nc; i++) { int best = cells[i].bank, bc = 1 << 30; for (int b = 0; b < nb; b++) { int c = bank_cost(ps, b, cells[i].cols, cells[i].cnt, cells[i].n, first); if (c < bc) { bc = c; best = b; } } cells[i].bank = best; }
    }
    for (int b = 0; b < nb; b++) rebuild_bank(ps, b, cells, nc, first);
}
typedef struct { uint8_t (*pens)[256]; uint32_t *hash; int n; } tileset;
static uint32_t thash(const uint8_t *t) { uint32_t h = 2166136261u; for (int i = 0; i < 256; i++) h = (h ^ t[i]) * 16777619u; return h; }
static int tile_add(tileset *ts, const uint8_t *t)
{
    uint32_t h = thash(t);
    for (int i = 0; i < ts->n; i++) if (ts->hash[i] == h && !memcmp(ts->pens[i], t, 256)) return i;
    if (ts->n >= MAXT) return -1;
    memcpy(ts->pens[ts->n], t, 256); ts->hash[ts->n] = h;
    return ts->n++;
}
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static const json_val *alias_of(const json_val *frames, const json_val *fr)   /* "same_as" -> the source step */
{
    for (int guard = 0; guard < 16 && fr && json_get(fr, "same_as"); guard++) fr = json_at(frames, (int)json_int(json_get(fr, "same_as"), 0));
    return fr;
}
int tool_pack_view(pak_writer *w, const char *srcdir, const char *secname);
static int pack_dir(pak_writer *w, const char *srcdir, const char *secname, int scene);
int tool_pack_view(pak_writer *w, const char *srcdir, const char *secname) { return pack_dir(w, srcdir, secname, 0); }
static int pack_dir(pak_writer *w, const char *srcdir, const char *secname, int scene)
{
    char rel[64], path[400], err[128];
    uint8_t *bg = NULL, *fg = NULL; int bw = 0, bh = 0, fw = 0, fh = 0;
    json_val *doc = NULL;
    palset ps[2]; int have_pal[2] = { 0, 0 };
    int x0 = 10, r0 = 40, W, H, CW, CH;     /* default origin = the ring scenes' source (unit 10, row 40) */
    uint8_t *cells[2];
    static uint8_t tpens[MAXT][256]; static uint32_t thsh[MAXT];
    tileset ts = { tpens, thsh, 0 };
    uint8_t *sec; uint32_t seclen, cellbytes; int rc = 0;

    (void)rel;
    /* ---- the ONE arena format (2026-08-30): arena.json "layers" -------
     *   under: frames[] (whole-plane pictures, one per crowd step) + overlay
     *          (ring / floor, composited on top of every frame at pack time)
     *   over:  frames[] (the plane drawn over the sprites: rope lines, the
     *          ringside far crowd)
     * plane 0 (the ROM's FG) = under, plane 1 (BG) = over.  A plane with
     * more than one frame becomes ONE crowd region covering the whole plane,
     * "ticks" per step. */
    uint8_t *frames[2][16]; int nframes[2] = { 0, 0 }; uint8_t *overlay = NULL, *overlays[4] = { 0, 0, 0, 0 }; int novl = 0; int nsteps[2] = { 0, 0 }, stepf[2][32], stept[2][32];
    snprintf(path, sizeof path, "%s/arena.json", srcdir);
    if (access(path, R_OK) != 0) return 0;                    /* no such view */
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "pack-arenas: %s: %s\n", path, err); return 1; }
    memset(frames, 0, sizeof frames);
    {   const json_val *layers = json_get(doc, "layers");
        if (!layers) { fprintf(stderr, "pack-arenas: %s has no \"layers\" - the old bg/fg format is gone: re-export the view (--export-arena SCENE DIR)\n", path); json_free(doc); return 1; }
        for (int pl = 0; pl < 2; pl++) {
            const json_val *L = json_get(layers, pl ? "over" : "under"), *fr = L ? json_get(L, "frames") : NULL, *stp = L ? json_get(L, "steps") : NULL;
            for (const json_val *q = stp ? stp->child : NULL; q && nsteps[pl] < 32; q = q->next) { stepf[pl][nsteps[pl]] = (int)json_int(json_get(q, "frame"), 0); stept[pl][nsteps[pl]] = (int)json_int(json_get(q, "ticks"), 4); nsteps[pl]++; }
            for (const json_val *q = fr ? fr->child : NULL; q && nframes[pl] < 16; q = q->next) {
                int w2, h2; char fp[600];
                snprintf(fp, sizeof fp, "%s/%s", srcdir, json_str(q, ""));
                if (wf_video_load_rgba_png(fp, &frames[pl][nframes[pl]], &w2, &h2) || w2 % 32 || h2 % 32 || w2 < 32 || h2 < 32 || w2 > 2560 || h2 > 2048) { fprintf(stderr, "pack-arenas: %s: not a PNG of 32-px unit multiples (max 2560x2048)\n", fp); rc = 1; continue; }
                if (!nframes[0] && !nframes[1]) { bw = w2; bh = h2; }
                else if (w2 != bw || h2 != bh) { fprintf(stderr, "pack-arenas: %s: %dx%d, the view is %dx%d\n", fp, w2, h2, bw, bh); rc = 1; free(frames[pl][nframes[pl]]); continue; }
                nframes[pl]++;
            }
            if (pl == 0 && L && (json_get(L, "overlays") || json_get(L, "overlay"))) {
                const json_val *ovs = json_get(L, "overlays");
                for (const json_val *q = ovs ? ovs->child : json_get(L, "overlay"); q && novl < 4; q = ovs ? q->next : NULL) {
                    int w2, h2; char fp[600];
                    snprintf(fp, sizeof fp, "%s/%s", srcdir, json_str(q, ""));
                    if (wf_video_load_rgba_png(fp, &overlays[novl], &w2, &h2) || w2 != bw || h2 != bh) { fprintf(stderr, "pack-arenas: %s: the overlay must match the frames (%dx%d)\n", fp, bw, bh); rc = 1; continue; }
                    novl++;
                }
                overlay = novl ? overlays[0] : NULL;
            }
        }
    }
    if (!nframes[0]) { fprintf(stderr, "pack-arenas: %s: the under plane needs at least one frame\n", srcdir); json_free(doc); return 1; }
    W = bw / 32; H = bh / 32; CW = W * 2; CH = H * 2; cellbytes = (uint32_t)CW * CH * 4u;
    /* the composited plane pictures: under = frame + overlay (alpha over), over = frame */
    uint8_t *comp[2][16]; memset(comp, 0, sizeof comp);
    for (int pl = 0; pl < 2; pl++) for (int k = 0; k < nframes[pl]; k++) {
        uint8_t *c = malloc((size_t)bw * bh * 4); memcpy(c, frames[pl][k], (size_t)bw * bh * 4);
        if (pl == 0) for (int ov = 0; ov < novl; ov++) for (size_t i = 0; i < (size_t)bw * bh; i++) if (overlays[ov][i * 4 + 3] >= 128) memcpy(c + i * 4, overlays[ov] + i * 4, 4);   /* in order: ring, then the rear ropes */
        comp[pl][k] = c;
    }
    bg = nframes[1] ? comp[1][0] : NULL; fg = comp[0][0];
    memset(ps, 0, sizeof ps);
    {   const json_val *org = json_get(doc, "origin");
        if (org) { x0 = (int)json_int(json_at(org, 0), x0); r0 = (int)json_int(json_at(org, 1), r0); }
        for (int pl = 0; pl < 2; pl++) {
            const json_val *pa = json_get(doc, pl ? "bg_palette" : "fg_palette");
            if (!pa) continue;
            ps[pl].nbank = 16;
            for (int b2 = 0; b2 < 16; b2++) { const json_val *bk = json_at(pa, b2); ps[pl].npen[b2] = 16;
                for (int k = 0; k < 16; k++) ps[pl].pal[b2][k] = (uint16_t)strtoul(json_str(json_at(bk, k), "0"), NULL, 16); }
            have_pal[pl] = 1;
        }
    }
    cells[0] = calloc(cellbytes, 1); cells[1] = calloc(cellbytes, 1);
    for (int pl = 0; pl < 2; pl++) {                 /* pl 0 = under (FG), 1 = over (BG); pen 0 = transparent on both */
        const uint8_t *img = pl ? bg : fg;
        int first = 1;
        if (!img) continue;
        if (!have_pal[pl]) {                          /* GLOBAL banks from the cells of EVERY frame (cluster_banks) */
            cellrec *cells = calloc((size_t)nframes[pl] * CW * CH, sizeof *cells); int nc = 0;
            for (int k = 0; k < nframes[pl]; k++)
                for (int ci = 0; ci < CW * CH; ci++) {
                    cellrec *c = &cells[nc]; long W = 0; double a[3] = { 0, 0, 0 };
                    cell_colours(comp[pl][k], bw, ci % CW, ci / CW, c->cols, c->cnt, &c->n);
                    if (!c->n) continue;
                    for (int i = 0; i < c->n; i++) { a[0] += (double)(c->cols[i] & 15) * c->cnt[i]; a[1] += (double)((c->cols[i] >> 4) & 15) * c->cnt[i]; a[2] += (double)((c->cols[i] >> 8) & 15) * c->cnt[i]; W += c->cnt[i]; }
                    for (int i = 0; i < 3; i++) c->mean[i] = (float)(a[i] / W);
                    nc++;
                }
            if (nc) cluster_banks(&ps[pl], cells, nc, first);
            if (ps[pl].nbank == 0) ps[pl].nbank = 1;
            for (int b2 = 0; b2 < 16; b2++) ps[pl].npen[b2] = 16;
            free(cells);
        }
        for (int ci = 0; ci < CW * CH; ci++) {
            unsigned cols[256]; int cnt[256], n, best = 0, bc = 1 << 30;
            uint8_t t[256];
            cell_colours(img, bw, ci % CW, ci / CW, cols, cnt, &n);
            for (int b2 = 0; b2 < (ps[pl].nbank ? ps[pl].nbank : 1); b2++) { int c = bank_cost(&ps[pl], b2, cols, cnt, n, first); if (c < bc) { bc = c; best = b2; if (!c) break; } }
            for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
                const uint8_t *px = img + (((size_t)((ci / CW) * 16 + y) * (size_t)bw) + (size_t)((ci % CW) * 16 + x)) * 4;
                t[y * 16 + x] = (px[3] < 128) ? 0 : (uint8_t)nearest_pen(&ps[pl], best, rgb444(px), first);
            }
            { int id = tile_add(&ts, t);
              if (id < 0) { fprintf(stderr, "pack-arenas: scene %d needs more than %d tiles\n", scene, MAXT); rc = 1; id = 0; }
              put16(cells[pl] + ci * 4, (unsigned)id); cells[pl][ci * 4 + 2] = (uint8_t)best; cells[pl][ci * 4 + 3] = 0; }
        }
    }
    /* the crowd block: one whole-plane region per plane with more than one frame */
    uint8_t *ablk = NULL; uint32_t alen = 0;
    {
        int nr = 0; size_t cap = 2;
        for (int pl = 0; pl < 2; pl++) {                  /* no step list: the frames in order, 4 ticks each */
            if (nframes[pl] > 1 && !nsteps[pl]) for (int k = 0; k < nframes[pl]; k++) { stepf[pl][k] = k; stept[pl][k] = 4; nsteps[pl] = nframes[pl]; }
            if (nframes[pl] > 1) { nr++; cap += 10u + (size_t)nsteps[pl] * (1u + (size_t)CW * CH * 4u); } }
        if (nr) {
            ablk = calloc(1, cap); put16(ablk, (unsigned)nr); alen = 2;
            for (int pl = 0; pl < 2; pl++) {
                if (nframes[pl] <= 1) continue;
                ablk[alen++] = (uint8_t)pl; ablk[alen++] = (uint8_t)nsteps[pl];
                put16(ablk + alen, 0); put16(ablk + alen + 2, 0); put16(ablk + alen + 4, (unsigned)CW); put16(ablk + alen + 6, (unsigned)CH); alen += 8;
                for (int sidx = 0; sidx < nsteps[pl]; sidx++) {
                    int k = stepf[pl][sidx] >= 0 && stepf[pl][sidx] < nframes[pl] ? stepf[pl][sidx] : 0;
                    const uint8_t *img = comp[pl][k];
                    ablk[alen++] = (uint8_t)stept[pl][sidx];                   /* the ROM's own tick values (its cadence: count-- == 0 advances) */
                    for (int ci = 0; ci < CW * CH; ci++) {
                        unsigned cols[256]; int cnt[256], n = 0, best = 0, bc = 1 << 30; uint8_t t[256] = {0}; int id = 0;
                        cell_colours(img, bw, ci % CW, ci / CW, cols, cnt, &n);
                        for (int b2 = 0; b2 < (ps[pl].nbank ? ps[pl].nbank : 1); b2++) { int c = bank_cost(&ps[pl], b2, cols, cnt, n, 1); if (c < bc) { bc = c; best = b2; if (!c) break; } }
                        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
                            const uint8_t *px = img + (((size_t)((ci / CW) * 16 + y) * (size_t)bw) + (size_t)((ci % CW) * 16 + x)) * 4;
                            t[y * 16 + x] = (px[3] < 128) ? 0 : (uint8_t)nearest_pen(&ps[pl], best, rgb444(px), 1);
                        }
                        id = tile_add(&ts, t); if (id < 0) { fprintf(stderr, "pack-arenas: %s: the crowd needs more than %d tiles\n", srcdir, MAXT); rc = 1; id = 0; }
                        put16(ablk + alen, (unsigned)id); ablk[alen + 2] = (uint8_t)best; ablk[alen + 3] = 0; alen += 4;
                    }
                }
            }
        }
    }
    {   /* the editor's composite previews: build/arena-preview/<arena>/<view>_<step>.png =
         * under frame (+ overlay) with the over plane on top, one per crowd step */
        const char *an = strrchr(srcdir, '/'); char base[300], pp[400]; int nsteps0 = nsteps[0] ? nsteps[0] : 1;
        (void)an;
        { const char *a1 = strstr(srcdir, "arenas/"); const char *slash = a1 ? strchr(a1 + 7, '/') : NULL;
          if (a1 && slash) snprintf(base, sizeof base, "build/arena-preview/%.*s", (int)(slash - (a1 + 7)), a1 + 7); else snprintf(base, sizeof base, "build/arena-preview/%s", secname); }
        mkdir("build/arena-preview", 0775); mkdir(base, 0775);
        for (int sidx = 0; sidx < nsteps0 && sidx < 16; sidx++) {
            int k = nsteps[0] ? (stepf[0][sidx] >= 0 && stepf[0][sidx] < nframes[0] ? stepf[0][sidx] : 0) : 0;
            int ko = nframes[1] ? (nsteps[1] ? (stepf[1][sidx % nsteps[1]] < nframes[1] ? stepf[1][sidx % nsteps[1]] : 0) : 0) : -1;
            uint8_t *img = malloc((size_t)bw * bh * 4); memcpy(img, comp[0][k], (size_t)bw * bh * 4);
            if (ko >= 0) for (size_t i = 0; i < (size_t)bw * bh; i++) if (comp[1][ko][i * 4 + 3] >= 128) memcpy(img + i * 4, comp[1][ko] + i * 4, 4);
            snprintf(pp, sizeof pp, "%s/%s_%d.png", base, secname, sidx);
            wf_art_write_rgba_png(pp, img, bw, bh); free(img);
        }
    }
    seclen = 12 + 2 * cellbytes + 2 * 512 + (uint32_t)ts.n * 256u + alen;
    sec = calloc(1, seclen);
    sec[0] = (uint8_t)scene; put16(sec + 2, (unsigned)ts.n); put16(sec + 4, (unsigned)x0); put16(sec + 6, (unsigned)r0); put16(sec + 8, (unsigned)W); put16(sec + 10, (unsigned)H);
    {   /* header byte 1 = the STOCK scene this view derives from (its layer priority,
         * ring law...): the view's arena.json "scene" (an export) else 0 */
        int base = doc ? (int)json_int(json_get(doc, "scene"), 0) : 0;
        sec[1] = (uint8_t)(base >= 0 && base < 8 ? base : 0); }
    memcpy(sec + 12, cells[0], cellbytes); memcpy(sec + 12 + cellbytes, cells[1], cellbytes);
    for (int pl = 0; pl < 2; pl++) for (int b = 0; b < 16; b++) for (int k = 0; k < 16; k++) put16(sec + 12 + 2 * cellbytes + pl * 512 + b * 32 + k * 2, ps[pl].pal[b][k]);
    memcpy(sec + 12 + 2 * cellbytes + 1024, tpens, (size_t)ts.n * 256u);
    if (ablk) { memcpy(sec + 12 + 2 * cellbytes + 1024 + (size_t)ts.n * 256u, ablk, alen); free(ablk); }
    if (pak_writer_add(w, secname, sec, seclen)) rc = 1;
    fprintf(stderr, "pack-arena: %s <- %s (%dx%d units, %d tiles, %s palettes, origin %d,%d%s)\n", secname, srcdir, W, H, ts.n, have_pal[0] || have_pal[1] ? "json" : "derived", x0, r0, alen > 2 ? ", crowd" : "");
    (void)scene;
    free(sec); free(cells[0]); free(cells[1]); if (doc) json_free(doc); for (int ov = 0; ov < novl; ov++) free(overlays[ov]); (void)overlay;
    for (int pl = 0; pl < 2; pl++) for (int k = 0; k < 16; k++) { free(frames[pl][k]); free(comp[pl][k]); }
    return rc;
}

/* ---- the ARENA LIBRARY: arenas/<name>/{ring,ringside}/ (each view = the export
 * format) + arena.json {name, stock_scene, description} -> build/arenas/
 * <name>.pak with sections "ring" and "ringside".  A profile names its arenas in
 * profiles/<p>.json "arenas": {"<in-ring scene>": "<name>"}; the stock
 * profile names none.  (user 2026-08-29: arenas load from their own paks) */
static time_t newest_mtime(const char *dir)
{
    char p[500]; struct stat st; time_t t = 0;
    static const char *files[] = { "ring/arena.json", "ring/ring.png", "ring/ropes.png", "ring/crowd_0.png", "ringside/arena.json", "ringside/floor.png", "ringside/crowd_0.png", "ringside/backcrowd_0.png", "arena.json" };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) { snprintf(p, sizeof p, "%s/%s", dir, files[i]); if (stat(p, &st) == 0 && st.st_mtime > t) t = st.st_mtime; }
    /* anim frames: the newest PNG under in/anim and out/anim */
    for (int v = 0; v < 2; v++) { DIR *d; struct dirent *e; snprintf(p, sizeof p, "%s/%s", dir, v ? "ringside" : "ring"); d = opendir(p);
        if (d) { while ((e = readdir(d))) { char q[700]; snprintf(q, sizeof q, "%s/%s", p, e->d_name); if (stat(q, &st) == 0 && st.st_mtime > t) t = st.st_mtime; } closedir(d); } }
    return t;
}

int tool_pack_arena(const char *name)
{
    char dir[300], in[400], out[400], pakp[300];
    pak_writer *w; int rc = 0, n = 0;
    snprintf(dir, sizeof dir, "arenas/%s", name);
    snprintf(in, sizeof in, "%s/ring", dir); snprintf(out, sizeof out, "%s/ringside", dir);
    snprintf(pakp, sizeof pakp, "build/arenas/%s.pak", name);
    mkdir("build", 0775); mkdir("build/arenas", 0775);
    w = pak_writer_new();
    { char p[500]; snprintf(p, sizeof p, "%s/arena.json", in); if (access(p, R_OK) == 0) { rc = pack_dir(w, in, "ring", 0) || rc; n++; } }
    { char p[500]; snprintf(p, sizeof p, "%s/arena.json", out); if (access(p, R_OK) == 0) { rc = pack_dir(w, out, "ringside", 0) || rc; n++; } }
    if (!n) { fprintf(stderr, "pack-arena: %s has no ring/arena.json or ringside/arena.json\n", dir); pak_writer_free(w); return 1; }
    { extern int tool_pack_ropes(pak_writer *, const char *); rc = tool_pack_ropes(w, in) || rc; }   /* the side-rope art, if the ring view has it */
    if (pak_writer_save(w, pakp)) rc = 1;
    pak_writer_free(w);
    fprintf(stderr, "pack: arena %s -> %s (%d view%s)\n", name, pakp, n, n == 1 ? "" : "s");
    return rc;
}

/* `make pack`: every library arena (arenas/<name>/) whose pak is missing or
 * older than its sources - the stock three included, since every non-stock
 * profile plays the packed stock arenas by default (profile.c) */
int tool_pack_all_arenas(void)
{
    DIR *d = opendir("arenas"); struct dirent *e; int rc = 0, n = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char dir[300], pakp[300], ring[400]; struct stat st; time_t src;
        if (e->d_name[0] == '.') continue;
        snprintf(dir, sizeof dir, "arenas/%s", e->d_name); snprintf(ring, sizeof ring, "%s/ring/arena.json", dir);
        if (access(ring, R_OK) != 0) continue;
        snprintf(pakp, sizeof pakp, "build/arenas/%s.pak", e->d_name);
        src = newest_mtime(dir);
        if (stat(pakp, &st) == 0 && st.st_mtime >= src) continue;
        rc = tool_pack_arena(e->d_name) || rc; n++;
    }
    closedir(d);
    if (n) fprintf(stderr, "pack: %d arena%s packed\n", n, n == 1 ? "" : "s");
    return rc;
}

/* every arena the active profile names, packed when its pak is missing or
 * older than its sources */
int tool_pack_profile_arenas(void)
{
    int scene, rc = 0; char name[64];
    for (int i = 0; wf_profile_arena_assign(i, &scene, name, sizeof name); i++) {
        char dir[300], pakp[300]; struct stat st; time_t src;
        snprintf(dir, sizeof dir, "arenas/%s", name); snprintf(pakp, sizeof pakp, "build/arenas/%s.pak", name);
        src = newest_mtime(dir);
        if (!src) { fprintf(stderr, "pack: profile names arena '%s' but arenas/%s/ has no sources\n", name, name); rc = 1; continue; }
        if (stat(pakp, &st) == 0 && st.st_mtime >= src) continue;
        rc = tool_pack_arena(name) || rc;
    }
    return rc;
}

/* --new-arena NAME SCENE: a library arena from a stock in-ring scene (0, 5
 * or 1) and its ring-out view - two headless exports into arenas/NAME/ */
int tool_new_arena(const char *name, int scene)
{
    char dir[300], cmd[700]; FILE *f; int out = wf_arena_out_scene(scene);
    if (scene != 0 && scene != 5 && scene != 1) { fprintf(stderr, "new-arena: SCENE must be 0 (WWF ring), 5 (Wrestling Challenge) or 1 (the cage)\n"); return 1; }
    snprintf(dir, sizeof dir, "arenas/%s", name); mkdir("arenas", 0775); mkdir(dir, 0775);
    snprintf(cmd, sizeof cmd, "./wfengine --export-arena %d \"%s/ring\" >/dev/null 2>&1", scene, dir);
    if (system(cmd)) { fprintf(stderr, "new-arena: the in-ring export failed\n"); return 1; }
    if (out >= 0) { snprintf(cmd, sizeof cmd, "./wfengine --export-arena %d \"%s/ringside\" >/dev/null 2>&1", out, dir); if (system(cmd)) { fprintf(stderr, "new-arena: the ring-out export failed\n"); return 1; } }
    for (int v = 0; v < 2; v++) {                   /* a NEW arena's palettes are derived from its art at pack time
                                                       (the export carries the ROM's banks, which would quantise any
                                                       new colour away): drop them from the copies */
        char pth[400], err[128]; json_val *d;
        snprintf(pth, sizeof pth, "%s/%s/arena.json", dir, v ? "ringside" : "ring");
        d = json_parse_file(pth, err, sizeof err);
        if (d) { json_remove(d, "fg_palette"); json_remove(d, "bg_palette"); json_set_string(d, "palettes", "derived from the art at pack time (16 banks x 15 colours per plane, greedy per cell)"); json_write_file(pth, d); json_free(d); }
    }
    snprintf(cmd, sizeof cmd, "%s/arena.json", dir);
    f = fopen(cmd, "w");
    if (f) { fprintf(f, "{\n  \"name\": \"%s\",\n  \"stock_scene\": %d,\n  \"description\": \"from the stock %s\"\n}\n", name, scene, scene == 0 ? "WWF ring" : scene == 5 ? "Wrestling Challenge ring" : "cage"); fclose(f); }
    fprintf(stderr, "new-arena: %s/ (ring%s) from scene %d - edit the PNGs, then --pack-arena %s\n", dir, out >= 0 ? " + ringside" : "", scene, name);
    return 0;
}

/* --arena-extent SCENE: which source units of a scene carry non-blank cells
 * (the ROM scene image is a torus of 8x8 blocks of 10x8 units) */
int tool_arena_extent(int scene)
{
    for (int plane = 0; plane < 2; plane++) {
        int x0 = 999, x1 = -1, y0 = 999, y1 = -1, n = 0;
        static char grid[64][81];
        for (int y = 0; y < 64; y++) { for (int x = 0; x < 80; x++) {
            uint8_t c[16] = {0}; int nz = 0;
            if (wf_scene_unit_cells((unsigned)scene, plane, (uint16_t)x, (uint16_t)(0x40 - y), c))
                for (int i = 0; i < (plane ? 8 : 16); i++) if (c[i]) nz = 1;
            grid[y][x] = nz ? '#' : '.';
            if (nz) { n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        } grid[y][80] = 0; }
        fprintf(stderr, "scene %d %s: %d non-blank units, x %d..%d, y(row) %d..%d\n", scene, plane ? "BG" : "FG", n, x0, x1, y0, y1);
        for (int y = y0; y <= y1 && y1 >= 0; y++) fprintf(stderr, "  %2d %s\n", y, grid[y]);
    }
    return 0;
}

/* --verify-arenas (with --profile P): every scene the profile overrides is
 * rendered headless twice at frame 4 - with the arenas.pak and with
 * WF_NOARENAPAK=1 (the ROM composer) - and the two frames are compared
 * byte for byte.  A re-imported stock export must be IDENTICAL; edited art
 * is expected to differ, so the report lists the pixel count per scene. */
int tool_verify_arenas(const char *profile)
{
    static const int stage_of[8] = { 0, 2, 0, -1, -1, 1, 1, -1 };   /* 2/6 = the ringside views of stages 0/1 */
    int fails = 0, n = 0;
    for (int sc = 0; sc < 8; sc++) {
        char cmd[700], a[200], b[200];
        FILE *fa, *fb; long la, lb; int same = 1, diffpx = 0;
        if (!wf_arena_has((unsigned)sc)) continue;
        if (stage_of[sc] < 0) { fprintf(stderr, "verify-arenas: scene %d imported but no headless stage reaches it - skipped\n", sc); continue; }
        n++;
        snprintf(a, sizeof a, "/tmp/wfe-arena-%d-a.ppm", sc); snprintf(b, sizeof b, "/tmp/wfe-arena-%d-b.ppm", sc);
        {   const char *poke = (sc == 2 || sc == 6) ? "WF_OUT2=1 " : "";
            int fr = (sc == 2 || sc == 6) ? 120 : 4;
            snprintf(cmd, sizeof cmd, "%sWF_STAGE=%d ./wfengine --headless --profile \"%s\" --drive script --frames %d --shot %s >/dev/null 2>&1", poke, stage_of[sc], profile, fr, a);
            if (system(cmd)) { fprintf(stderr, "verify-arenas: scene %d: render failed\n", sc); fails++; continue; }
            snprintf(cmd, sizeof cmd, "%sWF_NOARENAPAK=1 WF_STAGE=%d ./wfengine --headless --profile \"%s\" --drive script --frames %d --shot %s >/dev/null 2>&1", poke, stage_of[sc], profile, fr, b); }
        if (system(cmd)) { fprintf(stderr, "verify-arenas: scene %d: stock render failed\n", sc); fails++; continue; }
        fa = fopen(a, "rb"); fb = fopen(b, "rb");
        if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); fails++; continue; }
        fseek(fa, 0, SEEK_END); la = ftell(fa); fseek(fb, 0, SEEK_END); lb = ftell(fb);
        if (la != lb) same = 0;
        else {
            uint8_t *ba = malloc((size_t)la), *bb = malloc((size_t)lb);
            fseek(fa, 0, SEEK_SET); fseek(fb, 0, SEEK_SET);
            if (fread(ba, 1, (size_t)la, fa) == (size_t)la && fread(bb, 1, (size_t)lb, fb) == (size_t)lb) {
                long hdr = 0; int nl = 0;                       /* P6 header = 3 newlines */
                while (hdr < la && nl < 3) if (ba[hdr++] == '\n') nl++;
                for (long i = hdr; i + 2 < la; i += 3) if (memcmp(ba + i, bb + i, 3)) diffpx++;
                same = diffpx == 0;
            }
            free(ba); free(bb);
        }
        fclose(fa); fclose(fb); remove(a); remove(b);
        {   /* WF_VERIFY_TOL=n: pixels a scene may differ by and still pass (the cage's
             * 258: a crowd patch at the plane's left edge that the ROM's 0x28608
             * placement cull never activates at the verify camera although a
             * sliver of it is on screen - the whole-plane region animates it) */
            int tol = getenv("WF_VERIFY_TOL") ? atoi(getenv("WF_VERIFY_TOL")) : 0;
            if (!same && diffpx > tol) fails++;
            fprintf(stderr, "verify-arenas: scene %d  %s  (%d pixel%s differ from the ROM composer%s)\n", sc, same ? "IDENTICAL" : diffpx <= tol ? "within tolerance" : "DIFFERS", diffpx, diffpx == 1 ? "" : "s", !same && diffpx <= tol ? ", tolerated" : "");
        }
    }
    fprintf(stderr, "verify-arenas: %d scene%s checked, %d render failure%s\n", n, n == 1 ? "" : "s", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
