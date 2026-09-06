/* Wrestler package packer — data/wrestlers/NN/{poses,palette,tiles,stats}.json
 * + sheet.png -> build/wrestlers/NN.pak (docs/adr-001-data-formats.md).
 * Run by `wfengine --pack DIR PAK` after the table pak. Sections
 * (little-endian, fixed layouts, read by src/package.c):
 *   "palette"  16 x u16 pens
 *   "stats"    u16 hp, walk, run
 *   "tiles2"   u32 n, n x u32 sprite tile ids (>0xFFFF = clone-art arena)
 *   "sheet"    n x 256 pen bytes (16x16 tiles, row-major, same order as tiles)
 *   "poses2"   u32 count, then count x { u16 pose, u8 kind (0 own, 1 own_f,
 *              2 with_partner, 3 with_partner_f), u8 prow, u16 n,
 *              n x { s16 x, s16 y, u16 tile, u8 flipx, u8 flipy, u8 chain, u8 pal } }
 * Mod layers: mods/<name>/wrestlers/NN/<file> overrides the base file
 * (mods/order.txt, later lines win). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "json.h"
#include "profile.h"
#include "pak.h"
#include "tbl.h"

extern int wf_video_load_indexed_png(const char *path, uint8_t **out, int *w, int *h);

typedef struct { uint8_t *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->cap ? b->cap * 2 : 4096); while (b->cap < b->n + n) b->cap *= 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void put8(buf *b, unsigned v)  { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void put16(buf *b, unsigned v) { put8(b, v); put8(b, v >> 8); }
static void put32(buf *b, uint32_t v) { put16(b, v); put16(b, v >> 16); }

/* Resolve wrestler `id`'s `file` through the active profile's mod layers
 * (later wins, profile.h). For a CLONE slot (base >= 0) a file the clone's
 * own layers do not carry falls back to the BASE id's file — its layers
 * first, then data/wrestlers/BB/. Returns 1 when the CLONE'S OWN layer
 * provided the file (a base fallback returns 0). Stock ids return 1. */
static const char *pack_dir_override;   /* class stock template: every file from ONE dir */
static int pack_stockskins = 1;         /* EVERY profile, stock included: a stock man's ART is the ingested
                                           skin layout data/stockskins/NN (made from frames/ - user 2026-08-26:
                                           "every profile will pull from paks made from frames"); the ROM-layout
                                           export data/wrestlers/NN stays the placement TEMPLATE + the verify
                                           reference. WF_PACK_ROMART=1 packs the ROM-layout art instead (tools). */
static int pack_from_stockskin;         /* set by resolve_c when a stock man's poses came from there */
void wf_pack_set_stockskins(int on) { pack_stockskins = on; }
static int resolve_c(const char *data_dir, unsigned id, int base, const char *file, char *out, size_t n)
{
    char rel[512];
    if (pack_stockskins && !getenv("WF_PACK_ROMART") && id < 12 && base < 0
        && (!strcmp(file, "poses.json") || !strcmp(file, "tiles.json") || !strcmp(file, "sheet.png"))) {
        snprintf(rel, sizeof rel, "wrestlers/%02u/%s", id, file);
        if (wf_mod_resolve(rel, out, n)) return 1;      /* a mod's own art still wins */
        snprintf(out, n, "data/stockskins/%02u/%s", id, file);
        if (access(out, R_OK) == 0) { if (!strcmp(file, "poses.json")) pack_from_stockskin = 1; return 1; }
        fprintf(stderr, "pack: wrestler %02u: %s missing - run `./wfengine --stock-skins`; packing the ROM-layout art instead\n", id, out);
    }
    if (pack_dir_override) {           /* class template dir; stats etc. from the base's package */
        snprintf(out, n, "%s/%s", pack_dir_override, file);
        if (access(out, R_OK) == 0) return 1;
        snprintf(out, n, "%s/%02u/%s", data_dir, (unsigned)(base >= 0 ? base : 0), file);
        return 0;
    }
    snprintf(rel, sizeof rel, "wrestlers/%02u/%s", id, file);
    if (wf_mod_resolve(rel, out, n)) return 1;
    if (base >= 0) {
        snprintf(rel, sizeof rel, "wrestlers/%02u/%s", (unsigned)base, file);
        if (wf_mod_resolve(rel, out, n)) return 0;
        snprintf(out, n, "%s/%02u/%s", data_dir, (unsigned)base, file);
        return 0;
    }
    snprintf(out, n, "%s/%02u/%s", data_dir, id, file);
    return 1;
}

static int put_list(buf *b, const json_val *list, unsigned pose, unsigned kind, unsigned prow, unsigned *count)
{
    if (!list || list->type != JSON_ARRAY) return 0;
    put16(b, pose); put8(b, kind); put8(b, prow); put16(b, (unsigned)list->n);
    for (const json_val *r = list->child; r; r = r->next) {   /* poses2: u32 tile */
        put16(b, (unsigned)(int16_t)json_int(json_get(r, "x"), 0));
        put16(b, (unsigned)(int16_t)json_int(json_get(r, "y"), 0));
        put32(b, (unsigned)json_int(json_get(r, "tile"), 0));
        put8(b, (unsigned)json_int(json_get(r, "flipx"), 0));
        put8(b, (unsigned)json_int(json_get(r, "flipy"), 0));
        put8(b, (unsigned)json_int(json_get(r, "chain"), 0));
        put8(b, (unsigned)json_int(json_get(r, "pal"), 0));
    }
    (*count)++;
    return 0;
}

/* base < 0: stock id (all sections mandatory). base >= 0: CLONE slot —
 * a "clone" meta section (u16 base + nul-terminated name) is embedded;
 * palette/stats fall back to the base's files; tiles/sheet/poses are packed
 * only when the clone's OWN layer provides them (the tiles are a GLOBAL pen
 * store, so re-writing the base's identical sheet would be wasted bytes —
 * src/package.c delegates a missing section to the base's pak at runtime). */
/* WRESTLER SOUND MAP (2026-08-28): skin.json "sounds" then wrestler.json
 * "sounds" (slot wins) -> "sounds" section: u8 n; per entry u8 event,
 * u8 kind (0 cmd / 1 wav), u8 cmd, nul-terminated wav name. */
static const char *const snd_events[] = { "name_call", "intro_phrase" };
static uint8_t snd_sec[8 * 40 + 1]; static unsigned snd_sec_n;
static int snd_ev_kind[8], snd_ev_cmd[8]; static char snd_ev_wav[8][32];
static void snd_map_reset(void) { for (int e = 0; e < 8; e++) snd_ev_kind[e] = -1; }
static void snd_map_take(const json_val *m)
{
    if (!m) return;
    for (int e = 0; e < 2; e++) {
        const char *ref = json_str(json_get(m, snd_events[e]), "");
        unsigned c;
        if (!ref[0]) continue;
        if (!strncmp(ref, "cmd:", 4) && sscanf(ref + 4, "%x", &c) == 1) { snd_ev_kind[e] = 0; snd_ev_cmd[e] = (int)(c & 0xFF); }
        else if (!strncmp(ref, "wav:", 4) && ref[4]) { snd_ev_kind[e] = 1; snprintf(snd_ev_wav[e], 32, "%s", ref + 4); }
        else fprintf(stderr, "pack: sounds.%s: bad ref '%s' (cmd:0xNN or wav:name)\n", snd_events[e], ref);
    }
}
static void snd_map_build(void)
{
    unsigned n = 0, o = 1;
    for (int e = 0; e < 2; e++) if (snd_ev_kind[e] >= 0) {
        size_t nl = snd_ev_kind[e] == 1 ? strlen(snd_ev_wav[e]) : 0;
        snd_sec[o++] = (uint8_t)e; snd_sec[o++] = (uint8_t)snd_ev_kind[e]; snd_sec[o++] = (uint8_t)snd_ev_cmd[e];
        memcpy(snd_sec + o, snd_ev_wav[e], nl); o += (unsigned)nl; snd_sec[o++] = 0; n++;
        if (snd_ev_kind[e] == 1) { char q[300]; snprintf(q, sizeof q, "sounds/%s.wav", snd_ev_wav[e]);
            if (access(q, R_OK) != 0) fprintf(stderr, "pack: sounds.%s -> %s is missing from the sounds/ library\n", snd_events[e], q); }
    }
    snd_sec[0] = (uint8_t)n; snd_sec_n = n ? o : 0;
}

static int pack_one(const char *data_dir, const char *out_dir, unsigned id, int base, const char *name)
{
    char path[1024], err[256];
    json_val *doc;
    pak_writer *w = pak_writer_new();
    buf b = {0};
    int rc = 0;

    if (base >= 0) {                   /* clone meta section (read by src/package.c) */
        put16(&b, (unsigned)base);
        put(&b, name ? name : "", strlen(name ? name : "") + 1);
        pak_writer_add(w, "clone", b.p, (uint32_t)b.n); b.n = 0;
        if (snd_sec_n) { pak_writer_add(w, "sounds", snd_sec, snd_sec_n); snd_sec_n = 0; }
    }

    /* palette */
    resolve_c(data_dir, id, base, "palette.json", path, sizeof path);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "pack: %s: %s\n", path, err); pak_writer_free(w); return 1; }
    { const json_val *pens = json_get(doc, "pens");
      for (int k = 0; k < 16; k++) put16(&b, (unsigned)json_int(json_at(pens, k), 0)); }
    json_free(doc);
    pak_writer_add(w, "palette", b.p, (uint32_t)b.n); b.n = 0;

    /* stats */
    resolve_c(data_dir, id, base, "stats.json", path, sizeof path);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "pack: %s: %s\n", path, err); pak_writer_free(w); return 1; }
    put16(&b, (unsigned)json_int(json_get(json_get(doc, "hp"), "value"), 0));
    put16(&b, (unsigned)json_int(json_get(json_get(doc, "walk_speed"), "value"), 0));
    put16(&b, (unsigned)json_int(json_get(json_get(doc, "run_speed"), "value"), 0));
    pak_writer_add(w, "stats", b.p, (uint32_t)b.n); b.n = 0;
    {   /* "movemap": [cat 0..0x14][col]*3 bytes from stats.json move_map
         * rows (build-a-wrestler override; readers: eng_ws_move8) */
        const json_val *rows = json_get(json_get(doc, "move_map"), "rows");
        if (rows && rows->type == JSON_ARRAY) {
            int n = 0;
            for (const json_val *r = rows->child; r && n < 21; r = r->next, n++)
                for (const json_val *v2 = (r->type == JSON_ARRAY) ? r->child : NULL; v2; v2 = v2->next)
                    put8(&b, (unsigned)json_int(v2, 0xFF));
            if (b.n == 63) pak_writer_add(w, "movemap", b.p, (uint32_t)b.n);
            else if (b.n) fprintf(stderr, "pack: %02u move_map is %d bytes (want 63) - skipped\n", id, (int)b.n);
            b.n = 0;
        }
    }
    {   /* "mgrids": ALTERNATE move grids (stats.json "move_grids": [rows, rows, ...],
         * each 21x3) - the generic's A/B/C grids that together route every
         * move; eng_ws_grid_set picks one at run time (user 2026-08-26) */
        const json_val *grids = json_get(doc, "move_grids"); int ng = 0;
        for (const json_val *g = (grids && grids->type == JSON_ARRAY) ? grids->child : NULL; g; g = g->next) {
            size_t n0 = b.n; int n = 0;
            for (const json_val *r = (g->type == JSON_ARRAY) ? g->child : NULL; r && n < 21; r = r->next, n++)
                for (const json_val *v2 = (r->type == JSON_ARRAY) ? r->child : NULL; v2; v2 = v2->next)
                    put8(&b, (unsigned)json_int(v2, 0xFF));
            if (b.n - n0 != 63) { fprintf(stderr, "pack: %02u move_grids[%d] is %d bytes (want 63) - dropped\n", id, ng, (int)(b.n - n0)); b.n = n0; }
            else ng++;
        }
        if (ng) pak_writer_add(w, "mgrids", b.p, (uint32_t)b.n);
        b.n = 0;
    }
    {   /* "tmatrix": per grid (index 0 = move_map, 1.. = move_grids) the THROW
         * MATRIX row [band 0..2][bank 0/1] -> category (0xE232 is per wrestler:
         * the generic needs every stock man's row reachable) */
        const json_val *tm = json_get(doc, "grid_tmatrix"); int nt = 0;
        for (const json_val *g = (tm && tm->type == JSON_ARRAY) ? tm->child : NULL; g; g = g->next) {
            size_t n0 = b.n;
            for (const json_val *v2 = (g->type == JSON_ARRAY) ? g->child : NULL; v2; v2 = v2->next) put8(&b, (unsigned)json_int(v2, 0xFF));
            if (b.n - n0 != 6) { fprintf(stderr, "pack: %02u grid_tmatrix[%d] is %d bytes (want 6) - dropped\n", id, nt, (int)(b.n - n0)); b.n = n0; } else nt++;
        }
        if (nt) pak_writer_add(w, "tmatrix", b.p, (uint32_t)b.n);
        b.n = 0;
    }
    json_free(doc);

    /* tiles + sheet (clone: own layer only — else the base's pak already
     * holds the identical global pens; runtime delegates) */
    if (base >= 0 && !resolve_c(data_dir, id, base, "tiles.json", path, sizeof path))
        goto poses;
    resolve_c(data_dir, id, base, "tiles.json", path, sizeof path);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "pack: %s: %s\n", path, err); pak_writer_free(w); return 1; }
    {
        const json_val *tiles = json_get(doc, "tiles");
        unsigned n = tiles ? (unsigned)tiles->n : 0;
        uint8_t *img = NULL; int iw = 0, ih = 0;
        buf sh = {0};
        put32(&b, n);
        for (const json_val *t = tiles ? tiles->child : NULL; t; t = t->next) put32(&b, (unsigned)json_int(t, 0));
        pak_writer_add(w, "tiles2", b.p, (uint32_t)b.n); b.n = 0;
        resolve_c(data_dir, id, base, "sheet.png", path, sizeof path);
        if (wf_video_load_indexed_png(path, &img, &iw, &ih) != 0 || iw < 256) {
            fprintf(stderr, "pack: cannot load %s\n", path); json_free(doc); pak_writer_free(w); return 1;
        }
        for (unsigned i = 0; i < n; i++) {
            int ox = (int)(i % 16) * 16, oy = (int)(i / 16) * 16;
            if (oy + 16 > ih || ox + 16 > iw) { fprintf(stderr, "pack: %s too small for %u tiles\n", path, n); rc = 1; break; }
            for (int y = 0; y < 16; y++) put(&sh, img + (size_t)(oy + y) * (size_t)iw + (size_t)ox, 16);
        }
        free(img);
        pak_writer_add(w, "sheet", sh.p, (uint32_t)sh.n); free(sh.p);
    }
    json_free(doc);
    if (rc) { pak_writer_free(w); free(b.p); return rc; }

    /* poses (clone: own layer only, runtime delegates to the base's) */
poses:
    if (base >= 0 && !resolve_c(data_dir, id, base, "poses.json", path, sizeof path))
        goto save;
    pack_from_stockskin = 0;
    resolve_c(data_dir, id, base, "poses.json", path, sizeof path);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "pack: %s: %s\n", path, err); pak_writer_free(w); return 1; }
    {
    json_val *sdoc = doc;              /* the art doc (cellpal, vict2; skin2 for a stock skin) */
    if (pack_from_stockskin) {   /* "skinlay": this stock man draws as OWN art (eng_compose) - the ROM
                                    lists stay in poses2 as the placement TEMPLATE, the skin's own
                                    lists go to "skin2" */
        char tp[1024]; json_val *tdoc;
        put8(&b, 1); pak_writer_add(w, "skinlay", b.p, (uint32_t)b.n); b.n = 0;
        snprintf(tp, sizeof tp, "%s/%02u/poses.json", data_dir, id);
        tdoc = json_parse_file(tp, err, sizeof err);
        if (!tdoc) { fprintf(stderr, "pack: %s: %s\n", tp, err); json_free(sdoc); pak_writer_free(w); return 1; }
        doc = tdoc;
        {
            const json_val *poses = json_get(sdoc, "poses");
            unsigned count = 0; buf pb = {0};
            for (const json_val *pv = poses ? poses->child : NULL; pv; pv = pv->next) {
                unsigned pose = (unsigned)strtoul(pv->key, NULL, 10);
                put_list(&pb, json_get(pv, "own"), pose, 0, 0, &count);
                put_list(&pb, json_get(pv, "own_f"), pose, 1, 0, &count);
            }
            put32(&b, count); put(&b, pb.p, pb.n); free(pb.p);
            pak_writer_add(w, "skin2", b.p, (uint32_t)b.n); b.n = 0;
        }
    }
    {
        const json_val *poses = json_get(doc, "poses");
        unsigned count = 0;
        buf pb = {0};
        for (const json_val *pv = poses ? poses->child : NULL; pv; pv = pv->next) {
            unsigned pose = (unsigned)strtoul(pv->key, NULL, 10);
            put_list(&pb, json_get(pv, "own"), pose, 0, 0, &count);
            put_list(&pb, json_get(pv, "own_f"), pose, 1, 0, &count);
            for (int k = 2; k < 4; k++) {
                const json_val *m = json_get(pv, k == 2 ? "with_partner" : "with_partner_f");
                for (const json_val *e = m ? m->child : NULL; e; e = e->next)
                    put_list(&pb, e, pose, (unsigned)k, (unsigned)strtoul(e->key, NULL, 10), &count);
            }
        }
        put32(&b, count); put(&b, pb.p, pb.n); free(pb.p);
        pak_writer_add(w, "poses2", b.p, (uint32_t)b.n); b.n = 0;
    }
    {   /* "cellpal": u32 count, count x { u16 pose, 16 x u16 pens } — a portrait
           surface's own palette (continue faces 800+, title card 810) */
        const json_val *cp = json_get(sdoc, "cellpal");
        if (cp && cp->child) {
            unsigned count = 0; buf pb = {0};
            for (const json_val *pv = cp->child; pv; pv = pv->next) {
                unsigned pose = (unsigned)atoi(pv->key);
                if (pv->type != JSON_ARRAY || pv->n < 16) continue;
                put16(&pb, pose);
                for (int k = 0; k < 16; k++) put16(&pb, (unsigned)json_int(json_at(pv, k), 0));
                count++;
            }
            if (count) { put32(&b, count); put(&b, pb.p, pb.n); pak_writer_add(w, "cellpal", b.p, (uint32_t)b.n); }
            free(pb.p); b.n = 0;
        }
    }
    {   /* "vict2": VICTIM-BODY art (the clone being held/thrown inside a
           holder's composed frame) keyed "row,pose" — same rec encoding
           as poses2, kind 0 own / 1 own_f, prow byte = the holder ROW */
        const json_val *vict = json_get(sdoc, "vict");
        if (vict && vict->child) {
            unsigned count = 0;
            buf pb = {0};
            for (const json_val *pv = vict->child; pv; pv = pv->next) {
                unsigned vrow = 0, vpose = 0;
                if (sscanf(pv->key, "%u,%u", &vrow, &vpose) != 2 || vrow > 12) continue;   /* 12 = held by his OWN base */
                put_list(&pb, json_get(pv, "own"), vpose, 0, vrow, &count);
                put_list(&pb, json_get(pv, "own_f"), vpose, 1, vrow, &count);
            }
            if (count) {
                put32(&b, count); put(&b, pb.p, pb.n);
                pak_writer_add(w, "vict2", b.p, (uint32_t)b.n);
            }
            free(pb.p); b.n = 0;
        }
    }
    if (sdoc != doc) json_free(sdoc);
    json_free(doc);
    }

save:
    {   /* "voffs": [holder row, pose lo, pose hi, dx, dy] from victoffs.json (Calibrate tab, held side) */
        char vp[1024]; json_val *vd;
        if (resolve_c(data_dir, id, base, "victoffs.json", vp, sizeof vp) || pack_dir_override) {
            vd = json_parse_file(vp, err, sizeof err);
            if (vd) {
                const json_val *ent = json_get(vd, "entries"); unsigned n = 0;
                for (const json_val *e = ent ? ent->child : NULL; e; e = e->next) {
                    unsigned pose;
                    if (e->type != JSON_ARRAY || e->n < 4) continue;
                    pose = (unsigned)json_int(json_at(e, 1), 0);
                    put8(&b, (unsigned)json_int(json_at(e, 0), 0)); put8(&b, pose & 0xFF); put8(&b, pose >> 8);
                    put8(&b, (unsigned)(int8_t)json_int(json_at(e, 2), 0)); put8(&b, (unsigned)(int8_t)json_int(json_at(e, 3), 0));
                    n++;
                }
                if (n) pak_writer_add(w, "voffs", b.p, (uint32_t)b.n);
                b.n = 0; json_free(vd);
            }
        }
    }
    {   /* "calib" = [key, frame, dx, dy] bytes from calib.json (holder-side offsets
           of this package: a clone's mod dir or a class template) */
        char cp[1024]; json_val *cd;
        if (!(resolve_c(data_dir, id, base, "calib.json", cp, sizeof cp) || pack_dir_override)) cp[0] = 0;
        cd = cp[0] ? json_parse_file(cp, err, sizeof err) : NULL;
        if (cd) {
            const json_val *ent = json_get(cd, "entries"); unsigned n = 0;
            for (const json_val *e = ent ? ent->child : NULL; e; e = e->next) {
                if (e->type != JSON_ARRAY || e->n < 4) continue;
                put8(&b, (unsigned)json_int(json_at(e, 0), 0)); put8(&b, (unsigned)json_int(json_at(e, 1), 0));
                put8(&b, (unsigned)(int8_t)json_int(json_at(e, 2), 0)); put8(&b, (unsigned)(int8_t)json_int(json_at(e, 3), 0));
                n++;
            }
            if (n) pak_writer_add(w, "calib", b.p, (uint32_t)b.n);
            b.n = 0; json_free(cd);
        }
    }
    {   /* "depth" = [pose lo, pose hi, victim class, mode] from depth.json (Calibrate tab, draw order) */
        char dp[1024]; json_val *dd;
        if (!(resolve_c(data_dir, id, base, "depth.json", dp, sizeof dp) || pack_dir_override)) dp[0] = 0;
        dd = dp[0] ? json_parse_file(dp, err, sizeof err) : NULL;
        if (dd) {
            const json_val *ent = json_get(dd, "entries"); unsigned n = 0;
            for (const json_val *e = ent ? ent->child : NULL; e; e = e->next) {
                unsigned pose;
                if (e->type != JSON_ARRAY || e->n < 3) continue;
                pose = (unsigned)json_int(json_at(e, 0), 0);
                put8(&b, pose & 0xFF); put8(&b, pose >> 8);
                put8(&b, (unsigned)json_int(json_at(e, 1), 255)); put8(&b, (unsigned)json_int(json_at(e, 2), 0));
                n++;
            }
            if (n) pak_writer_add(w, "depth", b.p, (uint32_t)b.n);
            b.n = 0; json_free(dd);
        }
    }
    snprintf(path, sizeof path, "%s/%02u.pak", out_dir, id);
    rc = pak_writer_save(w, path);
    pak_writer_free(w); free(b.p);
    if (!rc) fprintf(stderr, "pack: wrestler %02u -> %s\n", id, path);
    return rc;
}

int tool_pack_wrestlers(const char *data_dir, const char *out_dir)
{
    int fails = 0;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", out_dir);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    mkdir(tmp, 0775);
    for (unsigned id = 0; id < 12; id++) {            /* TODO: roster manifest (ADR rule 8) */
        char probe[1024];
        snprintf(probe, sizeof probe, "%s/%02u/poses.json", data_dir, id);
        if (access(probe, R_OK) != 0) continue;
        if (pack_one(data_dir, out_dir, id, -1, NULL)) fails++;
    }
    for (unsigned id = 12; id < 44; id++) {           /* CLONE slots (engine.h ENG_WS_EXT_MAX):
                                                         a mod layer registers one with
                                                         wrestlers/NN/wrestler.json */
        char rel[64], probe[1024], err[256], name[64];
        json_val *doc; int base, bc;
        snprintf(rel, sizeof rel, "wrestlers/%02u/wrestler.json", id);
        if (!wf_mod_resolve(rel, probe, sizeof probe)) {   /* (a profile's "disabled" roster slot resolves to nothing too) */
            /* slot no longer registered: DELETE a stale pak from an earlier
             * pack, or the engine keeps loading the removed wrestler (the
             * superstars 12..14 removal left ghosts on the select screen) */
            snprintf(probe, sizeof probe, "%s/%02u.pak", out_dir, id);
            if (access(probe, F_OK) == 0 && unlink(probe) == 0)
                fprintf(stderr, "pack: wrestler %02u unregistered -> removed stale %s\n", id, probe);
            continue;
        }
        doc = json_parse_file(probe, err, sizeof err);
        if (!doc) { fprintf(stderr, "pack: %s: %s\n", probe, err); fails++; continue; }
        base = (int)json_int(json_get(doc, "clone_of"), -1);
        snprintf(name, sizeof name, "%s", json_str(json_get(doc, "name"), ""));
        bc = (int)json_int(json_get(doc, "body_class"), -1);
        snd_map_reset();
        {   /* the skin's sounds first, the slot's over them */
            const char *skin = json_str(json_get(doc, "skin"), "");
            if (skin[0]) {
                char sp[400], serr[128]; json_val *sd;
                snprintf(sp, sizeof sp, "skins/%s/skin.json", skin);
                if (access(sp, R_OK) != 0) { char srel[300]; snprintf(srel, sizeof srel, "skins/%s/skin.json", skin); wf_mod_resolve(srel, sp, sizeof sp); }
                sd = json_parse_file(sp, serr, sizeof serr);
                if (sd) { snd_map_take(json_get(sd, "sounds")); json_free(sd); }
            }
            snd_map_take(json_get(doc, "sounds"));
            snd_map_build();
        }
        json_free(doc);
        if (base < 0 || base >= 12) {
            fprintf(stderr, "pack: %s: clone_of must be a stock id 0..11\n", probe);
            fails++; continue;
        }
        /* body_class is the base's ROM size class (behind_grab_class): the
         * art template his skin was drawn on. A mismatch means the skin
         * was made for another body — refuse rather than ship a wrong fit. */
        {
            int want = tbl_json_byte("data/tables", "behind_grab_class", (uint32_t)base);
            if (bc >= 0 && want >= 0 && bc != want) {
                fprintf(stderr, "pack: %s: body_class %d but clone_of %d is body class %d\n", probe, bc, base, want);
                fails++; continue;
            }
            if (bc < 0 && want >= 0)
                fprintf(stderr, "pack: %s: no body_class (base %d = class %d) — add \"body_class\": %d\n", probe, base, want, want);
        }
        if (pack_one(data_dir, out_dir, id, base, name)) fails++;
    }
    for (int c = 0; c < 5; c++) {                     /* CLASS STOCK TEMPLATES -> hidden slots 39..43 */
        char dir[256], probe[300], err[256], name[64]; json_val *doc; int base;
        unsigned id = 44u - 5u + (unsigned)c;
        snprintf(dir, sizeof dir, "data/classes/%d/generic", c);   /* --class-pack C: the class GENERIC
                                          (neutral body) = the fallback art (user 2026-08-28);
                                          else the older stock members' template */
        snprintf(probe, sizeof probe, "%s/wrestler.json", dir);
        if (access(probe, R_OK) != 0) { snprintf(dir, sizeof dir, "data/classes/%d/stock", c); snprintf(probe, sizeof probe, "%s/wrestler.json", dir); }
        if (access(probe, R_OK) != 0) {
            snprintf(probe, sizeof probe, "%s/%02u.pak", out_dir, id);
            if (access(probe, F_OK) == 0 && unlink(probe) == 0) fprintf(stderr, "pack: class %d template gone -> removed %s\n", c, probe);
            continue;
        }
        doc = json_parse_file(probe, err, sizeof err);
        if (!doc) { fprintf(stderr, "pack: %s: %s\n", probe, err); fails++; continue; }
        base = (int)json_int(json_get(doc, "clone_of"), -1);
        snprintf(name, sizeof name, "@class%d", c);
        json_free(doc);
        if (base < 0 || base >= 12) { fails++; continue; }
        pack_dir_override = dir;
        if (pack_one(data_dir, out_dir, id, base, name)) fails++;
        pack_dir_override = NULL;
    }
    fprintf(stderr, "pack: wrestlers -> %s (%d failed)\n", out_dir, fails);
    return fails ? 1 : 0;
}
