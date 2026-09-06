/* --generic-class C DIR: the GENERIC WRESTLER of a body class (user
 * 2026-08-26: "have all the common frames ... every move ... as few frames
 * as possible; C code is cheap, graphics generation is expensive").
 *
 *   wfengine --generic-class 0 data/generics/0
 *
 * Layer over the class template (tools/export_skin.c tool_class_template_
 * stock): that tool assembles every pose any wrestler draws, from an
 * in-class owner where one exists (kind universal / class) or another
 * class's owner (kind borrowed), the victim bodies through the class victim
 * map, and the alias pass (exact-in-bbox -> mirror -> near silhouette + colour
 * bound, with dx/dy). This tool sorts that into the generic's package:
 *   frames/      assembled STOCK frames (body poses + victims) - no generation
 *   needs/ + needs.json   what the class cannot supply from stock: borrowed
 *                poses (another class's body, kept as the pose reference) and
 *                self-victim MIRROR VARIANTS (the holder-subtraction split of
 *                Hogan-holding-Hogan: a distinct/mangled drawing)
 *   aliases.json the alias groups (never generated; derived at ingest)
 *   review/      one contact sheet per alias group: source first, then the
 *                members as the ROM drew them (for the Classes-tab review)
 *   victmap.json, manifest.json (the template's), summary.json (counts)
 * The template run itself stays in DIR/_template (ref/ = posterised-able
 * pose references for the generator, out/ = the stock copies). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/engine.h"
#include "../src/json.h"
#include "../src/tbl.h"

#define GC_CANVAS   256
#define GC_CANVAS_H 320
#define GC_IDS      0x800

int tool_class_template_stock(int cls, const char *dir);
int class_borrow_source(int cls, unsigned pose);
int art_body_canvas(unsigned base, unsigned pose, uint8_t *cv);
int art_victim_canvas(unsigned row, unsigned pose, unsigned victim, uint8_t *cv);
double art_split_check(unsigned row, unsigned pose, unsigned victim);
int wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H);
int wf_video_load_rgba_png(const char *path, uint8_t **out, int *w, int *h);

static void gc_pens_to_rgba(const uint8_t *cv, const uint16_t *pens, uint8_t *rgba)
{
    for (int i = 0; i < GC_CANVAS * GC_CANVAS_H; i++) {
        uint8_t *d = rgba + (size_t)i * 4; unsigned pen = cv[i];
        if (!pen) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
        d[0] = (uint8_t)((pens[pen] & 0xF) * 17); d[1] = (uint8_t)(((pens[pen] >> 4) & 0xF) * 17);
        d[2] = (uint8_t)(((pens[pen] >> 8) & 0xF) * 17); d[3] = 255;
    }
}

static int gc_copy(const char *a, const char *b)
{
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "cp -f \"%s\" \"%s\"", a, b);
    return system(cmd);
}

/* render id (body pose or 1024+victim) as the ROM drew it, from the manifest's source */
static int gc_render(const json_val *man, unsigned id, uint8_t *cv, uint8_t *rgba)
{
    char key[16]; const json_val *e; int ref;
    snprintf(key, sizeof key, "%u", id);
    if (id < 1024) {
        e = json_get(json_get(man, "poses"), key);
        if (!e) return 0;
        ref = (int)json_int(json_get(e, "ref"), -1);
        if (ref < 0 || !art_body_canvas((unsigned)ref, id, cv)) return 0;
        gc_pens_to_rgba(cv, eng_pkg_palette((unsigned)ref), rgba);
        return 1;
    }
    e = json_get(json_get(man, "victims"), key);
    if (!e) return 0;
    {
        unsigned v = (unsigned)json_int(json_get(e, "ref"), 0), holder = (unsigned)json_int(json_get(e, "holder"), 12), pose = (unsigned)json_int(json_get(e, "pose"), 0);
        unsigned row = holder == 12 ? v : holder;
        if (!art_victim_canvas(row, pose, v, cv)) return 0;
        gc_pens_to_rgba(cv, eng_pkg_palette(v), rgba);
        return 1;
    }
}

/* 3x5 digit font for the sheet labels (digits, '-', 'M' = mirrored, ' ') */
static const uint16_t gc_font[13] = { 0x7B6F, 0x2492, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF, 0x0380, 0x5F6D, 0x0000 };
static void gc_text(uint8_t *img, int iw, int ih, int x, int y, const char *t, int scale)
{
    for (; *t; t++, x += 4 * scale) {
        int g = *t >= '0' && *t <= '9' ? *t - '0' : *t == '-' ? 10 : *t == 'M' ? 11 : 12;
        for (int r = 0; r < 5; r++) for (int c = 0; c < 3; c++)
            if (gc_font[g] & (1u << (14 - (r * 3 + c))))
                for (int sy = 0; sy < scale; sy++) for (int sx = 0; sx < scale; sx++) {
                    int px = x + c * scale + sx, py = y + r * scale + sy;
                    if (px >= 0 && px < iw && py >= 0 && py < ih) { uint8_t *d = img + ((size_t)py * iw + px) * 4; d[0] = 255; d[1] = 255; d[2] = 80; d[3] = 255; }
                }
    }
}
static void gc_bbox(const uint8_t *rgba, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = GC_CANVAS; *x1 = -1; *y0 = GC_CANVAS_H; *y1 = -1;
    for (int y = 0; y < GC_CANVAS_H; y++) for (int x = 0; x < GC_CANVAS; x++)
        if (rgba[((size_t)y * GC_CANVAS + x) * 4 + 3]) { if (x < *x0) *x0 = x; if (x > *x1) *x1 = x; if (y < *y0) *y0 = y; if (y > *y1) *y1 = y; }
}
/* paste a canvas' bbox into a strip cell at 2x, feet on a common line */
static void gc_paste(uint8_t *strip, int sw, int sh, const uint8_t *rgba, int cx, int cell)
{
    int x0, y0, x1, y1; gc_bbox(rgba, &x0, &y0, &x1, &y1);
    if (x1 < 0) return;
    {
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1, ox = cx + (cell - bw * 2) / 2, oy = sh - 24 - bh * 2;
        if (oy < 0) oy = 0;
        for (int y = 0; y < bh * 2 && oy + y < sh; y++) for (int x = 0; x < bw * 2; x++) {
            int dx = ox + x; const uint8_t *s = rgba + ((size_t)(y0 + y / 2) * GC_CANVAS + x0 + x / 2) * 4;
            if (dx < cx || dx >= cx + cell || dx >= sw || !s[3]) continue;
            memcpy(strip + ((size_t)(oy + y) * sw + dx) * 4, s, 4);
        }
    }
}

int tool_generic_class(int cls, const char *dir)
{
    static uint8_t cv[GC_CANVAS * GC_CANVAS_H], rgba[GC_CANVAS * GC_CANVAS_H * 4];
    static int alias_of[GC_IDS]; static uint8_t is_alias[GC_IDS], in_frames[GC_IDS], in_needs[GC_IDS];
    char tdir[512], path[700], src[700], err[128]; json_val *man; const json_val *poses, *victims, *aliases;
    FILE *nf, *sf; int nframes = 0, nvict = 0, nneed_b = 0, nneed_s = 0, nalias = 0, ngroups = 0, universe = 0, first = 1, nconv = 0, nreject = 0, nadd = 0, nstand = 0;
    static const json_val *added[GC_IDS]; static uint8_t rejected[GC_IDS]; static int sug_of[GC_IDS]; int nsug = 0;
    memset(added, 0, sizeof added); memset(rejected, 0, sizeof rejected); memset(sug_of, -1, sizeof sug_of);
    if (cls < 0 || cls >= ENG_BODY_CLASSES) { fprintf(stderr, "generic-class: class 0..4\n"); return 1; }
    mkdir(dir, 0775);
    snprintf(tdir, sizeof tdir, "%s/_template", dir);
    /* ALIASES = EXACT pixels (bbox-relative hash: catches translated copies)
       and MIRRORS only. Near-silhouette merges (0.98-0.999 IoU) still joined
       visibly different drawings (316/317 shading, 59/61 arm) so they are
       SUGGESTIONS for the review, not aliases (user 2026-08-26). */
    setenv("WF_ALIAS_IOU", "0", 1);
    if (tool_class_template_stock(cls, tdir)) return 1;
    {   /* the near pass, harvested into "suggested" (never applied) */
        char sdir[512]; snprintf(sdir, sizeof sdir, "%s/_suggest", dir);
        setenv("WF_ALIAS_IOU", getenv("WF_SUGGEST_IOU") ? getenv("WF_SUGGEST_IOU") : "0.98", 1);
        if (tool_class_template_stock(cls, sdir)) fprintf(stderr, "generic-class: no suggestion pass\n");
        setenv("WF_ALIAS_IOU", "0", 1);
    }
    snprintf(path, sizeof path, "%s/manifest.json", tdir);
    man = json_parse_file(path, err, sizeof err);
    if (!man) { fprintf(stderr, "generic-class: %s: %s\n", path, err); return 1; }
    poses = json_get(man, "poses"); victims = json_get(man, "victims"); aliases = json_get(man, "aliases");
    snprintf(path, sizeof path, "%s/frames", dir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/needs", dir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/review", dir); mkdir(path, 0775);
    memset(alias_of, -1, sizeof alias_of); memset(is_alias, 0, sizeof is_alias); memset(in_frames, 0, sizeof in_frames); memset(in_needs, 0, sizeof in_needs);
    for (const json_val *a = aliases ? aliases->child : NULL; a; a = a->next) {
        unsigned id = (unsigned)atoi(a->key ? a->key : "9999");
        if (id < GC_IDS) { is_alias[id] = 1; alias_of[id] = (int)json_int(json_get(a, "of"), -1); nalias++; }
    }
    {   /* USER REVIEW (aliases.override.json, kept across re-runs):
           { "reject": [id, ...]              -> that id is its own drawing after all
             "add": { "id": { "of": N, "flip": 0, "dx": 0, "dy": 0 } } } -> alias by hand */
        char op[600]; json_val *ov;
        snprintf(op, sizeof op, "%s/aliases.override.json", dir);
        ov = json_parse_file(op, err, sizeof err);
        if (ov) {
            const json_val *rej = json_get(ov, "reject"), *add = json_get(ov, "add");
            for (const json_val *r = rej ? rej->child : NULL; r; r = r->next) {
                unsigned id = (unsigned)json_int(r, 9999);
                if (id < GC_IDS && is_alias[id]) { is_alias[id] = 0; alias_of[id] = -1; nalias--; nreject++; rejected[id] = 1; }
            }
            for (const json_val *a = add ? add->child : NULL; a; a = a->next) {
                unsigned id = (unsigned)atoi(a->key ? a->key : "9999"); int of = (int)json_int(json_get(a, "of"), -1);
                if (id < GC_IDS && of >= 0 && of < GC_IDS && !is_alias[id]) { is_alias[id] = 1; alias_of[id] = of; nalias++; nadd++; added[id] = a; }
            }
            json_free(ov);
        }
    }
    snprintf(path, sizeof path, "%s/needs.json", dir);
    nf = fopen(path, "w");
    if (!nf) { json_free(man); return 1; }
    fprintf(nf, "{ \"class\": %d, \"note\": \"what the class cannot supply from stock art: 'borrowed' = the pose exists only on another body class (owner = that wrestler; needs/pose_NNNN.png is his frame as the POSE reference, to be regenerated on this body); 'self-victim' = the held body of <base holding himself> is a distinct/mangled split (the ROM re-posed him): regenerate from the sibling reference\",\n  \"entries\": [", cls);
    /* body poses */
    for (const json_val *e = poses ? poses->child : NULL; e; e = e->next) {
        unsigned id = (unsigned)atoi(e->key ? e->key : "9999"); const char *kind = json_str(json_get(e, "kind"), "");
        int ref = (int)json_int(json_get(e, "ref"), -1);
        if (id >= 1024) continue;
        universe++;
        if (is_alias[id]) continue;
        if (!strcmp(kind, "borrowed") || !strcmp(kind, "converted")) {
            /* a pose no in-class man draws. The earlier "converted" frames
               (data/classes/C/stock/frames, an old generation run) proved
               unusable (perfect-plex 324: a fragment; 328 missing - user
               2026-08-26), so: the OWNER's stock frame stands in (complete,
               right pose, another body class) and the pose is a NEED with the
               owner recorded as the pose reference. */
            int owner = class_borrow_source(cls, id);
            if (owner < 0) owner = ref;
            snprintf(src, sizeof src, "data/stockskins/%02d/frames/pose_%04u.png", owner, id);
            if (owner < 0 || access(src, R_OK) != 0) snprintf(src, sizeof src, "%s/ref/pose_%04u.png", tdir, id);
            snprintf(path, sizeof path, "%s/frames/pose_%04u.png", dir, id);
            if (gc_copy(src, path) == 0) { in_frames[id] = 1; nstand++; }
            snprintf(path, sizeof path, "%s/needs/pose_%04u.png", dir, id); gc_copy(src, path);
            fprintf(nf, "%s\n    { \"id\": %u, \"reason\": \"borrowed\", \"owner\": %d, \"owner_class\": %d, \"ref\": \"needs/pose_%04u.png\", \"note\": \"the owner's frame stands in until generated on this body\" }", first ? "" : ",", id, owner, owner >= 0 ? eng_ws_body_class(owner) : -1, id);
            first = 0; nneed_b++; in_needs[id] = 1;
            continue;
        }
        if (!strcmp(kind, "aisle") || !strcmp(kind, "portrait")) {   /* surfaces: kept with the frames as-is */
            snprintf(src, sizeof src, "%s/out/pose_%04u.png", tdir, id);
            snprintf(path, sizeof path, "%s/frames/pose_%04u.png", dir, id);
            if (gc_copy(src, path) == 0) in_frames[id] = 1;
            continue;
        }
        /* the CANONICAL stock frame (data/stockskins/NN/frames: what every pak
           is made from) beats a re-render through the compose path (1-px
           draw-order differences at cell overlaps, measured 2026-08-26) */
        snprintf(src, sizeof src, "data/stockskins/%02d/frames/pose_%04u.png", ref, id);
        if (ref < 0 || access(src, R_OK) != 0) snprintf(src, sizeof src, "%s/out/pose_%04u.png", tdir, id);
        snprintf(path, sizeof path, "%s/frames/pose_%04u.png", dir, id);
        if (gc_copy(src, path) == 0) { nframes++; in_frames[id] = 1; }
        else if (rejected[id] && gc_render(man, id, cv, rgba) && !wf_art_write_rgba_png(path, rgba, GC_CANVAS, GC_CANVAS_H)) { nframes++; in_frames[id] = 1; }   /* a REJECTED alias: its own frame again */
        else {   /* no stock frame for it (the template left it to generate) */
            snprintf(src, sizeof src, "%s/ref/pose_%04u.png", tdir, id); snprintf(path, sizeof path, "%s/needs/pose_%04u.png", dir, id); gc_copy(src, path);
            fprintf(nf, "%s\n    { \"id\": %u, \"reason\": \"missing\", \"owner\": %d, \"ref\": \"needs/pose_%04u.png\" }", first ? "" : ",", id, ref, id); first = 0; nneed_b++; in_needs[id] = 1;
        }
    }
    /* victims */
    for (const json_val *e = victims ? victims->child : NULL; e; e = e->next) {
        unsigned id = (unsigned)atoi(e->key ? e->key : "9999");
        unsigned v = (unsigned)json_int(json_get(e, "ref"), 0), holder = (unsigned)json_int(json_get(e, "holder"), 12), pose = (unsigned)json_int(json_get(e, "pose"), 0);
        if (id < 1024 || id >= GC_IDS) continue;
        universe++;
        if (is_alias[id]) continue;
        if (holder == 12) {
            double iou = art_split_check(v, pose, v);
            if (iou >= 0 && iou < 0.7) {
                snprintf(src, sizeof src, "%s/ref/pose_%04u.png", tdir, id);
                snprintf(path, sizeof path, "%s/needs/pose_%04u.png", dir, id);
                gc_copy(src, path);
                fprintf(nf, "%s\n    { \"id\": %u, \"reason\": \"self-victim\", \"owner\": %u, \"holder_pose\": %u, \"split_iou\": %.2f, \"ref\": \"needs/pose_%04u.png\" }", first ? "" : ",", id, v, pose, iou, id);
                first = 0; nneed_s++; in_needs[id] = 1;
                continue;
            }
        }
        snprintf(src, sizeof src, "%s/out/pose_%04u.png", tdir, id);
        snprintf(path, sizeof path, "%s/frames/pose_%04u.png", dir, id);
        if (gc_copy(src, path) == 0) { nvict++; in_frames[id] = 1; }
        else if (rejected[id] && gc_render(man, id, cv, rgba) && !wf_art_write_rgba_png(path, rgba, GC_CANVAS, GC_CANVAS_H)) { nvict++; in_frames[id] = 1; }
        else {   /* MIRROR HOLE: the ROM never drew this man held here; the ref is another body standing in */
            snprintf(src, sizeof src, "%s/ref/pose_%04u.png", tdir, id); snprintf(path, sizeof path, "%s/needs/pose_%04u.png", dir, id); gc_copy(src, path);
            fprintf(nf, "%s\n    { \"id\": %u, \"reason\": \"mirror-hole\", \"holder_pose\": %u, \"ref\": \"needs/pose_%04u.png\" }", first ? "" : ",", id, pose, id); first = 0; nneed_s++; in_needs[id] = 1;
        }
    }
    fprintf(nf, "\n  ] }\n"); fclose(nf);
    /* aliases.json = the template's alias map + what it points at */
    snprintf(path, sizeof path, "%s/aliases.json", dir);
    nf = fopen(path, "w");
    if (nf) {
        int f2 = 1;
        fprintf(nf, "{ \"class\": %d, \"note\": \"id -> {of, flip, dx, dy[, iou]}: never generated; ingest derives the frame from `of` (mirror + shift). review/group_<of>.png shows each group as the ROM drew it: source first. aliases.override.json (reject/add) is applied.\",\n  \"rejected\": %d, \"added\": %d,\n  \"aliases\": {", cls, nreject, nadd);
        for (unsigned id = 0; id < GC_IDS; id++) {
            char key[16]; const json_val *a;
            if (!is_alias[id]) continue;
            snprintf(key, sizeof key, "%u", id);
            a = added[id] ? added[id] : json_get(aliases, key);
            if (!a) continue;
            fprintf(nf, "%s\n    \"%u\": ", f2 ? "" : ",", id); f2 = 0;
            json_write(nf, a, 0);
        }
        {   /* suggested near merges: in the 0.98 run, not exact */
            char sp[600]; json_val *sd; int f3 = 1;
            snprintf(sp, sizeof sp, "%s/_suggest/manifest.json", dir);
            sd = json_parse_file(sp, err, sizeof err);
            fprintf(nf, "\n  },\n  \"suggested\": {");
            if (sd) {
                const json_val *sa = json_get(sd, "aliases");
                for (const json_val *a = sa ? sa->child : NULL; a; a = a->next) {
                    unsigned id = (unsigned)atoi(a->key ? a->key : "9999");
                    if (id >= GC_IDS || is_alias[id] || !json_get(a, "iou")) continue;   /* exact ones are aliases already */
                    fprintf(nf, "%s\n    \"%u\": ", f3 ? "" : ",", id); f3 = 0; json_write(nf, a, 0);
                    if (id < GC_IDS) { sug_of[id] = (int)json_int(json_get(a, "of"), -1); nsug++; }
                }
                json_free(sd);
            }
            fprintf(nf, "\n  }\n}\n"); fclose(nf);
        }
    }
    /* review sheets: one per source that has members - 2x, cells sized to
       the widest member, label = id, then M (mirrored) and the dx dy shift */
    {
        static uint8_t *strip; static size_t strip_n;
        int srcs[GC_IDS], nsrc = 0;
        for (unsigned id = 0; id < GC_IDS; id++) if (is_alias[id] && alias_of[id] >= 0 && alias_of[id] < GC_IDS) {
            int of = alias_of[id], k; for (k = 0; k < nsrc; k++) if (srcs[k] == of) break; if (k == nsrc) srcs[nsrc++] = of; }
        int sugpass = 0, nsuggroups = 0;
    sheets:
        for (int k = 0; k < nsrc; k++) {
            int of = srcs[k], n, cell = 0, sh = 0; unsigned members[64]; int nm = 0;
            static uint8_t *cache[13]; int have[13] = {0};
            for (unsigned id = 0; id < GC_IDS && nm < 64; id++) if ((sugpass ? sug_of[id] == of : (is_alias[id] && alias_of[id] == of))) members[nm++] = id;
            n = 1 + nm; if (n > 12) n = 12;
            for (int i = 0; i < n; i++) {   /* render once, size the cells */
                unsigned id = i == 0 ? (unsigned)of : members[i - 1]; int x0, y0, x1, y1;
                if (!cache[i]) cache[i] = malloc((size_t)GC_CANVAS * GC_CANVAS_H * 4);
                have[i] = cache[i] && gc_render(man, id, cv, cache[i]);
                if (!have[i]) continue;
                gc_bbox(cache[i], &x0, &y0, &x1, &y1);
                if ((x1 - x0 + 1) * 2 + 16 > cell) cell = (x1 - x0 + 1) * 2 + 16;
                if ((y1 - y0 + 1) * 2 + 40 > sh) sh = (y1 - y0 + 1) * 2 + 40;
            }
            if (cell < 96) cell = 96; if (sh < 120) sh = 120;
            if (strip_n < (size_t)n * cell * sh * 4) { free(strip); strip_n = (size_t)n * cell * sh * 4; strip = malloc(strip_n); }
            if (!strip) break;
            for (int i = 0; i < n * cell * sh; i++) { strip[i * 4] = 64; strip[i * 4 + 1] = 64; strip[i * 4 + 2] = 72; strip[i * 4 + 3] = 255; }
            for (int i = 0; i < n; i++) {
                char lab[48]; unsigned id = i == 0 ? (unsigned)of : members[i - 1];
                if (have[i]) gc_paste(strip, n * cell, sh, cache[i], i * cell, cell);
                if (i == 0) snprintf(lab, sizeof lab, "%u", id);
                else if (sugpass) snprintf(lab, sizeof lab, "%u -", id);
                else {
                    char key[16]; const json_val *a; snprintf(key, sizeof key, "%u", id); a = json_get(aliases, key);
                    snprintf(lab, sizeof lab, "%u %s%d %d", id, a && json_int(json_get(a, "flip"), 0) ? "M " : "", a ? (int)json_int(json_get(a, "dx"), 0) : 0, a ? (int)json_int(json_get(a, "dy"), 0) : 0);
                }
                gc_text(strip, n * cell, sh, i * cell + 4, sh - 16, lab, 2);
            }
            snprintf(path, sizeof path, "%s/review/%s_%04d.png", dir, sugpass ? "suggest" : "group", of);
            if (!wf_art_write_rgba_png(path, strip, n * cell, sh)) { if (sugpass) nsuggroups++; else ngroups++; }
        }
        if (!sugpass && nsug) {   /* second pass: the suggested near merges */
            sugpass = 1; nsrc = 0;
            for (unsigned id = 0; id < GC_IDS; id++) if (sug_of[id] >= 0) { int of = sug_of[id], k; for (k = 0; k < nsrc; k++) if (srcs[k] == of) break; if (k == nsrc) srcs[nsrc++] = of; }
            goto sheets;
        }
    }
    {   /* MOVE GRIDS (user 2026-08-26: "every move that exists - in his capability";
           a grid holds 21 situations x 3 buttons, so the union needs several):
           per slot, the distinct move ids any stock wrestler routes there; grid k
           takes the k-th distinct id (the class base's row first), repeating the
           last known id where a slot has fewer. stats.json: move_map = grid A,
           move_grids = the others; eng_ws_grid_set / WF_GRID picks one. */
        static uint8_t ids[63][12]; static int nid[63]; int ngrid = 1, rep = -1;
        for (int w = 0; w < ENG_WS_MAX; w++) if (rep < 0 && eng_ws_body_class(w) == cls) rep = w;
        for (int off = 0; off < 63; off++) {
            nid[off] = 0;
            for (int pass = 0; pass < 2; pass++)
                for (int w = 0; w < ENG_WS_MAX; w++) {
                    unsigned m; int k;
                    if ((pass == 0) != (w == rep)) continue;       /* the base's routing first */
                    m = eng_ws_move8((unsigned)w, (unsigned)off);
                    if (m == 0xFFu) continue;
                    for (k = 0; k < nid[off]; k++) if (ids[off][k] == m) break;
                    if (k == nid[off]) ids[off][nid[off]++] = (uint8_t)m;
                }
            if (nid[off] > ngrid) ngrid = nid[off];
        }
        snprintf(path, sizeof path, "%s/stats.json", dir);
        sf = fopen(path, "w");
        if (sf) {
            fprintf(sf, "{ \"note\": \"the generic's move grids: move_map = grid A (the class base's routing where it has one), move_grids = B, C, ... - together every move any stock wrestler routes; pick with WF_GRID=k or eng_ws_grid_set\",\n  \"grids\": %d,\n  \"move_map\": { \"rows\": [", ngrid);
            for (int cat = 0; cat < 21; cat++) { fprintf(sf, "%s[", cat ? ", " : ""); for (int col = 0; col < 3; col++) { int off = cat * 3 + col; fprintf(sf, "%s%u", col ? "," : "", nid[off] ? ids[off][0] : 255u); } fprintf(sf, "]"); }
            fprintf(sf, "] },\n  \"move_grids\": [");
            for (int g = 1; g < ngrid; g++) {
                fprintf(sf, "%s\n    [", g > 1 ? "," : "");
                for (int cat = 0; cat < 21; cat++) { fprintf(sf, "%s[", cat ? ", " : ""); for (int col = 0; col < 3; col++) { int off = cat * 3 + col, k = g < nid[off] ? g : nid[off] - 1; fprintf(sf, "%s%u", col ? "," : "", k >= 0 ? ids[off][k] : 255u); } fprintf(sf, "]"); }
                fprintf(sf, "]");
            }
            fprintf(sf, "\n  ],\n  \"grid_tmatrix\": [");
            {   /* per grid: the k-th DISTINCT throw-matrix row among the stock men
                   (the base's first; the last repeats) - 0xE232 [band][bank][id] */
                uint8_t rows[12][6]; int nrows = 0;
                for (int pass = 0; pass < 2; pass++)
                    for (int w = 0; w < ENG_WS_MAX; w++) {
                        uint8_t r[6]; int k;
                        if ((pass == 0) != (w == rep)) continue;
                        for (int band = 0; band < 3; band++) for (int bank = 0; bank < 2; bank++)
                            r[band * 2 + bank] = (uint8_t)tbl8(TBL(throw_matrix), (unsigned)band * 0x18u + (unsigned)bank * 0xCu + (unsigned)w);
                        for (k = 0; k < nrows; k++) if (!memcmp(rows[k], r, 6)) break;
                        if (k == nrows) memcpy(rows[nrows++], r, 6);
                    }
                for (int g = 0; g < ngrid; g++) {
                    int k = g < nrows ? g : nrows - 1;
                    fprintf(sf, "%s[%u,%u,%u,%u,%u,%u]", g ? ", " : "", rows[k][0], rows[k][1], rows[k][2], rows[k][3], rows[k][4], rows[k][5]);
                }
                fprintf(stderr, "generic-class: %d distinct throw-matrix rows across the stock men\n", nrows);
            }
            fprintf(sf, "] }\n"); fclose(sf);
            fprintf(stderr, "generic-class: %d move grids (A = the base's routing; every routed move reachable)\n", ngrid);
        }
    }
    snprintf(src, sizeof src, "%s/victmap.json", tdir); snprintf(path, sizeof path, "%s/victmap.json", dir); gc_copy(src, path);
    snprintf(src, sizeof src, "%s/manifest.json", tdir); snprintf(path, sizeof path, "%s/manifest.json", dir); gc_copy(src, path);
    snprintf(path, sizeof path, "%s/summary.json", dir);
    sf = fopen(path, "w");
    if (sf) {
        fprintf(sf, "{ \"class\": %d, \"universe\": %d, \"frames_body\": %d, \"frames_victim\": %d, \"aliases\": %d, \"alias_groups\": %d, \"stand_in_frames\": %d, \"needs_borrowed\": %d, \"needs_self_victim\": %d, \"unique_to_draw\": %d }\n",
                cls, universe, nframes, nvict, nalias, ngroups, nsug, nstand, nneed_b, nneed_s, nframes + nvict + nneed_b + nneed_s);
        fclose(sf);
    }
    fprintf(stderr, "generic-class %d (%s) -> %s: universe %d ids | stock frames %d body + %d victim | %d stand-in frames (the owner's body) | aliases %d in %d groups (review/, + %d suggested near merges to accept via aliases.override.json add) | NEEDS %d borrowed + %d self-victim/mirror-hole | unique drawings %d\n",
            cls, eng_body_class_name(cls), dir, universe, nframes, nvict, nstand, nalias, ngroups, nsug, nneed_b, nneed_s, nframes + nvict + nneed_b + nneed_s);
    json_free(man);
    return 0;
}
