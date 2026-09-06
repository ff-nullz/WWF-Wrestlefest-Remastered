/* SIDE-ROPE ART pak section (2026-08-30): arenas/<name>/ring/ropes/
 * {ropes.json, side_00..10.png} -> sections "ropes" + "rtiles" of the arena
 * pak (src/ropeart.c draws them through ringhw.c).
 *   "rtiles"  u32 n, n x 256 pens
 *   "ropes"   u32 nframes, u16 pens[16] (xBGR444, one palette for all
 *             frames, <= 15 colours), then per frame u16 ncells,
 *             ncells x { i16 x, i16 y, u32 tile } in RECORD space (x = the
 *             cell's left edge from the object origin, y = the sprite y of
 *             its top edge: hardware-inverted, larger = higher on screen)
 * Tiles are numbered from 0 here; the loader adds its arena base. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/json.h"
#include "../src/pak.h"

int wf_video_load_rgba_png(const char *path, uint8_t **rgba, int *w, int *h);

typedef struct { uint8_t *p; size_t n, cap; } rbuf;
static void rput(rbuf *b, const void *d, size_t n) { if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 256; b->p = realloc(b->p, b->cap); } memcpy(b->p + b->n, d, n); b->n += n; }
static void rput8(rbuf *b, unsigned v) { uint8_t x = (uint8_t)v; rput(b, &x, 1); }
static void rput16(rbuf *b, unsigned v) { rput8(b, v); rput8(b, v >> 8); }
static void rput32(rbuf *b, uint32_t v) { rput16(b, v); rput16(b, v >> 16); }

int tool_pack_ropes(pak_writer *w, const char *ringdir)
{
    char path[700], err[256]; json_val *doc; const json_val *fr, *org;
    uint8_t *img[16]; int iw[16], ih[16], nf = 0, ox, oy, rc = 0;
    uint16_t pens[16] = {0}; int npens = 0;
    struct { uint16_t v; uint32_t count; } hist[4096]; int nh = 0;
    rbuf tiles = {0}, sec = {0}; uint32_t ntiles = 0;
    snprintf(path, sizeof path, "%s/ropes/ropes.json", ringdir);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) return 0;                                            /* no rope art: the stock sprites draw */
    fr = json_get(doc, "frames"); org = json_get(doc, "origin");
    ox = (int)json_int(json_at(org, 0), 0); oy = (int)json_int(json_at(org, 1), 0);
    for (const json_val *q = fr ? fr->child : NULL; q && nf < 16; q = q->next) {
        snprintf(path, sizeof path, "%s/ropes/%s", ringdir, json_str(q, ""));
        if (wf_video_load_rgba_png(path, &img[nf], &iw[nf], &ih[nf])) { fprintf(stderr, "pack-ropes: cannot read %s\n", path); rc = 1; continue; }
        if (nf && (iw[nf] != iw[0] || ih[nf] != ih[0])) { fprintf(stderr, "pack-ropes: %s: %dx%d, the canvas is %dx%d\n", path, iw[nf], ih[nf], iw[0], ih[0]); rc = 1; free(img[nf]); continue; }
        nf++;
    }
    if (!nf) { json_free(doc); return rc; }
    for (int f = 0; f < nf; f++) for (int i = 0; i < iw[f] * ih[f]; i++) {   /* one palette for every frame */
        const uint8_t *px = img[f] + i * 4; uint16_t v; int k;
        if (px[3] < 128) continue;
        v = (uint16_t)((px[0] / 17) | ((px[1] / 17) << 4) | ((px[2] / 17) << 8));
        for (k = 0; k < nh; k++) if (hist[k].v == v) { hist[k].count++; break; }
        if (k == nh && nh < 4096) { hist[nh].v = v; hist[nh].count = 1; nh++; }
    }
    for (int i = 1; i < nh; i++) { int j = i - 1; typeof(hist[0]) t = hist[i]; while (j >= 0 && hist[j].count < t.count) { hist[j + 1] = hist[j]; j--; } hist[j + 1] = t; }
    if (nh > 15) fprintf(stderr, "pack-ropes: %s: %d colours across the frames - 15 kept, the rest snap\n", ringdir, nh);
    npens = nh < 15 ? nh : 15;
    for (int k = 0; k < npens; k++) pens[k + 1] = hist[k].v;
    rput32(&sec, (uint32_t)nf);
    for (int k = 0; k < 16; k++) rput16(&sec, pens[k]);
    for (int f = 0; f < nf; f++) {
        int tw = iw[f] / 16, th = ih[f] / 16; rbuf cells = {0}; unsigned nc = 0;
        for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) {
            uint8_t t[256]; int used = 0;
            for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
                const uint8_t *px = img[f] + (((size_t)(ty * 16 + y) * iw[f]) + (size_t)(tx * 16 + x)) * 4; int best = 0, bd = 1 << 30;
                if (px[3] >= 128) for (int k = 0; k < npens; k++) {
                    int dr = px[0] / 17 - (pens[k + 1] & 0xF), dg = px[1] / 17 - ((pens[k + 1] >> 4) & 0xF), db = px[2] / 17 - ((pens[k + 1] >> 8) & 0xF);
                    int d = dr * dr + dg * dg + db * db; if (d < bd) { bd = d; best = k + 1; } }
                t[y * 16 + x] = (uint8_t)best; used |= best;
            }
            if (!used) continue;
            rput(&tiles, t, 256);
            rput16(&cells, (unsigned)(int16_t)(tx * 16 - ox));                 /* record x: left edge from the origin */
            rput16(&cells, (unsigned)(int16_t)(oy - ty * 16));                 /* record y: top edge, hardware-inverted */
            rput32(&cells, ntiles++); nc++;
        }
        rput16(&sec, nc); rput(&sec, cells.p, cells.n); free(cells.p);
    }
    { rbuf t2 = {0}; rput32(&t2, ntiles); rput(&t2, tiles.p, tiles.n); if (pak_writer_add(w, "rtiles", t2.p, (uint32_t)t2.n)) rc = 1; free(t2.p); }
    if (pak_writer_add(w, "ropes", sec.p, (uint32_t)sec.n)) rc = 1;
    fprintf(stderr, "pack-ropes: %d frames, %d colours, %u tiles (origin %d,%d)\n", nf, npens, ntiles, ox, oy);
    for (int f = 0; f < nf; f++) free(img[f]);
    free(tiles.p); free(sec.p); json_free(doc);
    return rc;
}
