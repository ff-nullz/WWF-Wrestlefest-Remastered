/* BADGES pak (2026-08-29): the over-head chips the engine draws that the ROM
 * has no art for (the CPU badge of the badge_cpu mod rule).
 *
 *   wfengine --pack-badges DIR OUT.pak    (build.sh / make pack: data/badges
 *                                          -> build/badges.pak)
 *
 * data/badges/badges.json = { "badges": { "<name>": "<png>" } }.  Each PNG
 * (<= 96x96, <= 15 colours) is quantised to its own 16-pen palette, cut into
 * 16x16 tiles in the BADGE TILE ARENA (ENG_BADGE_TILE0, section "btiles") and
 * written as a cell list centred on the badge's origin (cell y bottom-up,
 * the sprite convention).  Sections:
 *   "btiles"  u32 n, n x 256 pens
 *   "badges"  u32 n; per badge: char name[16], u16 pens[16] (xBGR444),
 *             u16 w, u16 h, u16 ncells, ncells x { i16 x, i16 y, u32 tile }
 * src/badges.c reads it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/json.h"
#include "../src/pak.h"
#include "../src/engine.h"

int wf_video_load_rgba_png(const char *path, uint8_t **rgba, int *w, int *h);

typedef struct { uint8_t *p; size_t n, cap; } bbuf;
static void bput(bbuf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 256; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void bput8(bbuf *b, unsigned v)  { uint8_t x = (uint8_t)v; bput(b, &x, 1); }
static void bput16(bbuf *b, unsigned v) { bput8(b, v); bput8(b, v >> 8); }
static void bput32(bbuf *b, uint32_t v) { bput16(b, v); bput16(b, v >> 16); }

int tool_pack_badges(const char *dir, const char *out)
{
    char path[1024], err[256]; json_val *doc; const json_val *list;
    bbuf tiles = {0}, badges = {0}; uint32_t ntiles = 0; int nb = 0, rc = 0;
    pak_writer *w;
    snprintf(path, sizeof path, "%s/badges.json", dir);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "pack-badges: %s: %s\n", path, err); return 1; }
    list = json_get(doc, "badges");
    bput32(&badges, 0);                                       /* count, patched below */
    for (const json_val *e = list ? list->child : NULL; e; e = e->next) {
        uint8_t *rgba; int iw, ih; uint16_t pens[16] = {0}; int npens = 0; uint8_t *idx;
        struct { uint16_t v; uint32_t count; } hist[4096]; int nh = 0;
        char name[16] = {0};
        if (!e->key) continue;
        snprintf(name, sizeof name, "%s", e->key);
        snprintf(path, sizeof path, "%s/%s", dir, json_str(e, ""));
        if (wf_video_load_rgba_png(path, &rgba, &iw, &ih)) { fprintf(stderr, "pack-badges: cannot read %s\n", path); rc = 1; continue; }
        if (iw > 96 || ih > 96) { fprintf(stderr, "pack-badges: %s: %dx%d (max 96x96)\n", path, iw, ih); rc = 1; free(rgba); continue; }
        for (int i = 0; i < iw * ih; i++) {                      /* the colour histogram -> 15 pens */
            const uint8_t *px = rgba + i * 4; uint16_t v; int k;
            if (px[3] < 128) continue;
            v = (uint16_t)((px[0] / 17) | ((px[1] / 17) << 4) | ((px[2] / 17) << 8));
            for (k = 0; k < nh; k++) if (hist[k].v == v) { hist[k].count++; break; }
            if (k == nh && nh < 4096) { hist[nh].v = v; hist[nh].count = 1; nh++; }
        }
        for (int i = 1; i < nh; i++) { int j = i - 1; typeof(hist[0]) t = hist[i]; while (j >= 0 && hist[j].count < t.count) { hist[j + 1] = hist[j]; j--; } hist[j + 1] = t; }
        if (nh > 15) fprintf(stderr, "pack-badges: %s has %d colours - the 15 most used are kept, the rest snap to the nearest\n", path, nh);
        npens = nh < 15 ? nh : 15;
        for (int k = 0; k < npens; k++) pens[k + 1] = hist[k].v;
        idx = calloc((size_t)iw * ih, 1);
        for (int i = 0; i < iw * ih; i++) {
            const uint8_t *px = rgba + i * 4; int best = 1, bd = 1 << 30;
            if (px[3] < 128) continue;
            for (int k = 0; k < npens; k++) {
                int dr = px[0] / 17 - (pens[k + 1] & 0xF), dg = px[1] / 17 - ((pens[k + 1] >> 4) & 0xF), db = px[2] / 17 - ((pens[k + 1] >> 8) & 0xF);
                int d = dr * dr + dg * dg + db * db; if (d < bd) { bd = d; best = k + 1; }
            }
            idx[i] = (uint8_t)best;
        }
        {   /* tiles + cells: the badge's origin is its horizontal centre, bottom edge (y up) */
            int tw = (iw + 15) / 16, th = (ih + 15) / 16, nc = 0; bbuf cells = {0};
            for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) {
                uint8_t t[256]; int used = 0;
                for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int ix = tx * 16 + x, iy = ty * 16 + y; uint8_t v = (ix < iw && iy < ih) ? idx[iy * iw + ix] : 0; t[y * 16 + x] = v; used |= v; }
                if (!used) continue;
                bput(&tiles, t, 256);
                bput16(&cells, (unsigned)(int16_t)(tx * 16 - iw / 2));                    /* x from the centre */
                bput16(&cells, (unsigned)(int16_t)(ih - 16 - ty * 16));                   /* y bottom-up: the top row is highest */
                bput32(&cells, ENG_BADGE_TILE0 + ntiles); ntiles++; nc++;
            }
            bput(&badges, name, 16);
            for (int k = 0; k < 16; k++) bput16(&badges, pens[k]);
            bput16(&badges, (unsigned)iw); bput16(&badges, (unsigned)ih); bput16(&badges, (unsigned)nc);
            bput(&badges, cells.p, cells.n); free(cells.p);
            fprintf(stderr, "pack-badges: %s %dx%d, %d colours, %d cells\n", name, iw, ih, npens, nc);
            nb++;
        }
        free(idx); free(rgba);
    }
    badges.p[0] = (uint8_t)nb; badges.p[1] = (uint8_t)(nb >> 8); badges.p[2] = 0; badges.p[3] = 0;
    w = pak_writer_new();
    { bbuf t2 = {0}; bput32(&t2, ntiles); bput(&t2, tiles.p, tiles.n); if (pak_writer_add(w, "btiles", t2.p, (uint32_t)t2.n)) rc = 1; free(t2.p); }
    if (pak_writer_add(w, "badges", badges.p, (uint32_t)badges.n)) rc = 1;
    if (pak_writer_save(w, out)) rc = 1;
    fprintf(stderr, "pack-badges: %d badge%s, %u tiles -> %s%s\n", nb, nb == 1 ? "" : "s", ntiles, out, rc ? " (ERRORS)" : "");
    free(tiles.p); free(badges.p); json_free(doc);
    return rc;
}
