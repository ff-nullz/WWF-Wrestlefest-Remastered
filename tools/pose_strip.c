/* --poses: render a wrestler's package poses as a labeled contact sheet,
 * through the ENGINE's own pose lists and tile store (the same data the
 * game draws), for the Superstars pose-mapping work.
 *
 *   wfengine --poses <id> <first> <count> <out.png>
 *
 * 16 poses per row, 96x120 cells, origin at (40, 90) with a baseline. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <png.h>
#include "../src/engine.h"

const uint8_t *wf_video_tile_pens(unsigned t);

/* WF_EMITTEST="row,pose,partner_row": run the LIVE sprite emit for one pose
 * (package path, then the ROM thinker fallback) and report the record count */
int eng_sprite_emit_pose(unsigned row, unsigned pose_word, int sx, int sy, int partner_row, unsigned *slot);
static void emit_test(void)
{
    unsigned row, pose; int prow, wrote; unsigned slot = 0;
    const char *e = getenv("WF_EMITTEST");
    if (!e || sscanf(e, "%u,%u,%d", &row, &pose, &prow) != 3) return;
    wrote = eng_sprite_emit_pose(row, pose, 160, 120, prow, &slot);
    fprintf(stderr, "emit-test: row %u pose %u partner_row %d -> %d records (slot %u)\n", row, pose, prow, wrote, slot);
}
int tool_pose_strip(unsigned id, unsigned first, unsigned count, const char *out)
{
    emit_test();
    enum { CW = 96, CH = 120, COLS = 16 };
    unsigned rows = (count + COLS - 1) / COLS;
    unsigned W = CW * COLS, H = CH * rows;
    uint8_t *img = calloc((size_t)W * (size_t)H, 1);
    png_color pal[18];
    const uint16_t *pens = eng_pkg_palette(id);
    FILE *f; png_structp png; png_infop info; png_bytep *rp;

    if (!img) return 1;
    pal[0].red = 24; pal[0].green = 24; pal[0].blue = 32;
    for (int p = 1; p < 16; p++) {
        uint16_t w = pens ? pens[p] : (uint16_t)(p * 0x111);
        pal[p].red   = (png_byte)((w & 0x0F) * 17);
        pal[p].green = (png_byte)(((w >> 4) & 0x0F) * 17);
        pal[p].blue  = (png_byte)(((w >> 8) & 0x0F) * 17);
    }
    pal[16].red = 60; pal[16].green = 60; pal[16].blue = 70;
    pal[17].red = 90; pal[17].green = 70; pal[17].blue = 40;

    for (unsigned k = 0; k < count; k++) {
        const eng_pkg_rec *pr;
        const uint8_t *src;
        if (getenv("WF_CALIB")) { int c, m, f; if (sscanf(getenv("WF_CALIB"), "%d,%d,%d", &c, &m, &f) == 3) eng_compose_ctx(c, m, f); }   /* calibration context */
        int n = getenv("WF_PARTNER") ? eng_compose((int)id, first + k, getenv("WF_FLIP") ? 1 : 0, atoi(getenv("WF_PARTNER")), &pr, &src)   /* WF_PARTNER=victim id (clones too) */
                                     : eng_pkg_pose(id, first + k, 0, -1, &pr);
        if (getenv("WF_PARTNER") && getenv("WF_COMPOSE_DBG")) {   /* per-pose source census */
            int c[6] = {0};
            for (int i = 0; i < n; i++) if (src[i] < 6) c[src[i]]++;
            {   /* bbox of the victim records vs the holder records (placement check) */
                int hx0 = 9999, hx1 = -9999, hy0 = 9999, hy1 = -9999, vx0 = 9999, vx1 = -9999, vy0 = 9999, vy1 = -9999;
                for (int i = 0; i < n; i++) {
                    int isv = src[i] == ENG_SRC_VICTIM || src[i] == ENG_SRC_VICTIM_OWN;
                    int x0 = pr[i].x, x1 = pr[i].x + 16, y0 = pr[i].y, y1 = pr[i].y + 16 * ((pr[i].chain & 7) + 1);
                    if (isv) { if (x0 < vx0) vx0 = x0; if (x1 > vx1) vx1 = x1; if (y0 < vy0) vy0 = y0; if (y1 > vy1) vy1 = y1; }
                    else if (src[i] != ENG_SRC_OVERLAY) { if (x0 < hx0) hx0 = x0; if (x1 > hx1) hx1 = x1; if (y0 < hy0) hy0 = y0; if (y1 > hy1) hy1 = y1; }
                }
                fprintf(stderr, "compose %u pose %u victim %s: n %d  holder %d/%d own  victim %d/%d own  overlay %d  hbox %d..%d,%d..%d vbox %d..%d,%d..%d\n",
                        id, first + k, getenv("WF_PARTNER"), n, c[0], c[1], c[2], c[3], c[4], hx0, hx1, hy0, hy1, vx0, vx1, vy0, vy1);
                {   /* the per-cell source sequence in draw order: H holder, h holder-own, V victim, v victim-own, W overlay, R rope */
                    static const char code[] = "HhVvWR"; char seq[300]; int m = 0;
                    for (int i = 0; i < n && m < 298; i++) seq[m++] = src[i] < 6 ? code[src[i]] : '?';
                    seq[m] = 0;
                    fprintf(stderr, "compose %u pose %u seq %s\n", id, first + k, seq);
                }
            }
        }
        unsigned ox = (k % COLS) * CW, oy = (k / COLS) * CH;
        static uint8_t cell[256 * 256];
        memset(cell, 0, sizeof cell);
        for (int x = 0; x < CW; x++) img[(size_t)(oy + CH - 2) * W + ox + (unsigned)x] = 16;
        if (n <= 0) continue;
        /* the editor pose browser's PROVEN transform (editor.c render_pose):
         * hardware y inversion + upward chains, on a 256x256 canvas */
        for (int i = 0; i < n; i++) {
            int x = pr[i].x + 128, y = pr[i].y + 0x80;
            int xpos = x & 0x1FF, ypos;
            unsigned chain = pr[i].chain & 7u;
            if (xpos > 512 - 16) xpos -= 512;
            ypos = ((256 - y) & 0x1FF) - 16;
            for (unsigned c = 0; c <= chain; c++) {
                const uint8_t *t = wf_video_tile_pens((unsigned)(pr[i].tile + c));
                int dy = pr[i].flipy ? ypos - (int)(16 * chain) + (int)(16 * c) : ypos - (int)(16 * c);
                if (!t) continue;
                for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                    int qx = pr[i].flipx ? 15 - px : px, qy = pr[i].flipy ? 15 - py : py;
                    uint8_t pen = t[qy * 16 + qx];
                    int cx = xpos + px, cy = dy + py + 64;
                    if (pen && cx >= 0 && cx < 256 && cy >= 0 && cy < 256)
                        cell[cy * 256 + cx] = pen;
                }
            }
        }
        /* blit canvas region around the origin (128, 176-ish feet) into the cell */
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++) {
                uint8_t v = cell[(y + 80) * 256 + (x + 88)];
                if (v) img[(size_t)(oy + (unsigned)y) * W + ox + (unsigned)x] = v;
            }
    }
    f = fopen(out, "wb");
    if (!f) { free(img); return 1; }
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) { fclose(f); return 1; }
    png_init_io(png, f);
    png_set_IHDR(png, info, W, H, 8, PNG_COLOR_TYPE_PALETTE,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_PLTE(png, info, pal, 18);
    rp = malloc(sizeof(png_bytep) * H);
    for (unsigned y = 0; y < H; y++) rp[y] = img + (size_t)y * W;
    png_set_rows(png, info, rp);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, 0);
    png_destroy_write_struct(&png, &info);
    fclose(f); free(rp); free(img);
    fprintf(stderr, "poses: id %u poses %u..%u -> %s\n", id, first, first + count - 1, out);
    return 0;
}
