/* Weapon pak packer — data/weapons/{weapons.json, palette.json} ->
 * build/weapons.pak (stock) / build/profiles/<p>/weapons.pak (a profile whose
 * mods carry weapons/). "Weapons are built like the wrestlers" (user
 * 2026-08-26): the JSON table is the source, the pak is what the game reads;
 * src/package.c splices the pak's cells into every carry/pickup/swing frame
 * in place of the ROM template's bank-15 cells (eng_compose), so the stock
 * set stays bit-identical (`--weapon-check` proves it cell for cell) and a
 * new weapon type is a table entry.
 *
 * Sections (little-endian, read by src/package.c wpn_load):
 *   "wtypes"  u32 ntypes, per type: char name[16], u8 pal (index into wpal),
 *             u8 pad[3], u32 nposes, per pose: u16 pose, u16 ncells,
 *             ncells x { s16 x, s16 y, u32 tile, u8 flipx, u8 flipy, u8 chain, u8 pad }
 *             (flip-0 cells; the mirror is the ROM own_f rule x -> -x, flipx toggled)
 *   "wattach" u32 n, n x { u8 wrestler, u8 pad, u16 pose, s16 dx, s16 dy }
 *             the whole weapon's shift per (stock wrestler, pose) relative to
 *             the table cells (wrestler 00's hand); a clone takes his base's
 *   "wpal"    u32 npal, npal x 16 u16 pens (index 0 = the ROM weapon bank)
 *   "wrules"  u32 ntypes, per type: u16 carry_dz, u16 carry_dx, u16 tumble_steps, u16 damage
 *             (defaults = the weapon_rules scalars; weapon.c reads them per type)
 *   "wspawn"  u32 n, n x u8 type index — the profile's ringside spawn list
 *             (slot k of ENG_WEAPONS; default 0, 1 = steps, box)
 * Mod layers: mods/<layer>/weapons/weapons.json (whole table) and
 * weapons/palette.json override the base files; a type's "palette" key names
 * a palette file relative to the weapons dir (its own bank). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/json.h"
#include "../src/profile.h"
#include "../src/pak.h"
#include "../src/engine.h"

int wf_video_load_rgba_png(const char *path, uint8_t **rgba, int *w, int *h);

typedef struct { uint8_t *p; size_t n, cap; } wbuf;
static void wput(wbuf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = b->cap ? b->cap * 2 : 4096; while (b->cap < b->n + n) b->cap *= 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void wput8(wbuf *b, unsigned v)  { uint8_t x = (uint8_t)v; wput(b, &x, 1); }
static void wput16(wbuf *b, unsigned v) { wput8(b, v); wput8(b, v >> 8); }
static void wput32(wbuf *b, uint32_t v) { wput16(b, v); wput16(b, v >> 16); }

/* weapons/<file> through the mod layers, else <dir>/<file> */
static void wresolve(const char *dir, const char *file, char *out, size_t n)
{
    char rel[256];
    snprintf(rel, sizeof rel, "weapons/%s", file);
    if (wf_mod_resolve(rel, out, n)) return;
    snprintf(out, n, "%s/%s", dir, file);
}

static int read_pens(const char *path, wbuf *b)
{
    char err[256]; json_val *d = json_parse_file(path, err, sizeof err); const json_val *pens;
    if (!d) { fprintf(stderr, "pack-weapons: %s: %s\n", path, err); return -1; }
    pens = json_get(d, "pens");
    for (int k = 0; k < 16; k++) wput16(b, (unsigned)json_int(json_at(pens, k), 0));
    json_free(d);
    return 0;
}

/* ---- IMAGE weapon types (user 2026-08-27): a type whose entry is
 *   { "image": "chair.png", "image_swing": "...", "nudge": [dx,dy], ... }
 * gets its PNG quantised to an own 15-pen bank, cut into 16x16 tiles in the
 * WEAPON tILE ARENA (ENG_WPN_TILE0, pak section "wtiles"), and cells
 * generated onto the BOX pose set (pickup/swing 120-123, carry 128-131) at
 * the box art's per-pose centre + nudge. "wloose" carries the ringside
 * lying / tumble sprites (the image, then flipped variants). Cell y is
 * bottom-up (larger y = higher on screen). */
typedef struct { uint8_t *rgba; int w, h; uint16_t pens[16]; int npens;
                 uint8_t *idx; } wimg;                     /* idx: per-pixel pen, 0 = transparent */
static int wimg_load(const char *path, wimg *im)
{
    struct { uint16_t v; uint32_t count; } hist[4096]; int nh = 0;
    if (wf_video_load_rgba_png(path, &im->rgba, &im->w, &im->h)) { fprintf(stderr, "pack-weapons: cannot read %s\n", path); return -1; }
    if (im->w > 96 || im->h > 96) { fprintf(stderr, "pack-weapons: %s: %dx%d (max 96x96)\n", path, im->w, im->h); free(im->rgba); return -1; }
    for (int i = 0; i < im->w * im->h; i++) {
        const uint8_t *px = im->rgba + i * 4;
        uint16_t v;
        if (px[3] < 128) continue;
        v = (uint16_t)((px[0] / 17) | ((px[1] / 17) << 4) | ((px[2] / 17) << 8));   /* xBGR444 */
        { int k; for (k = 0; k < nh; k++) if (hist[k].v == v) { hist[k].count++; break; }
          if (k == nh && nh < 4096) { hist[nh].v = v; hist[nh].count = 1; nh++; } }
    }
    for (int i = 1; i < nh; i++) { int j = i - 1; typeof(hist[0]) t = hist[i];   /* desc count */
        while (j >= 0 && hist[j].count < t.count) { hist[j + 1] = hist[j]; j--; } hist[j + 1] = t; }
    im->npens = nh < 15 ? nh : 15;
    im->pens[0] = 0;
    for (int k = 0; k < 15; k++) im->pens[k + 1] = k < im->npens ? hist[k].v : 0;
    im->idx = calloc((size_t)im->w * im->h, 1);
    for (int i = 0; i < im->w * im->h; i++) {
        const uint8_t *px = im->rgba + i * 4; int best = 1, bd = 1 << 30;
        if (px[3] < 128) continue;
        for (int k = 0; k < im->npens; k++) {
            int dr = px[0] / 17 - (im->pens[k + 1] & 0xF), dg = px[1] / 17 - ((im->pens[k + 1] >> 4) & 0xF), db = px[2] / 17 - ((im->pens[k + 1] >> 8) & 0xF);
            int d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = k + 1; }
        }
        im->idx[i] = (uint8_t)best;
    }
    return 0;
}
/* cut the image into arena tiles; returns the count and appends pens to `tiles`;
 * cellbuf gets one cell per non-empty tile at (x0 + 16tx, ytop - 16ty) */
typedef struct { int16_t x, y; uint32_t tile; } wcell;
static int wimg_tiles(const wimg *im, wbuf *tiles, uint32_t tile0, wcell *cells, int maxc)
{
    int tw = (im->w + 15) / 16, th = (im->h + 15) / 16, nc = 0; uint32_t nt = 0;
    for (int ty = 0; ty < th; ty++)
        for (int tx = 0; tx < tw; tx++) {
            uint8_t pens[256]; int used = 0;
            for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
                int ix = tx * 16 + x, iy = ty * 16 + y;
                uint8_t v = (ix < im->w && iy < im->h) ? im->idx[iy * im->w + ix] : 0;
                pens[y * 16 + x] = v; used |= v;
            }
            if (!used) continue;
            wput(tiles, pens, 256);
            if (nc < maxc) { cells[nc].x = (int16_t)(tx * 16); cells[nc].y = (int16_t)(ty * 16); cells[nc].tile = tile0 + nt; nc++; }
            nt++;
        }
    return nc;
}
static void wimg_free(wimg *im) { free(im->rgba); free(im->idx); im->rgba = NULL; im->idx = NULL; }
/* emit the image cells centred at (cx, cy) in cell space (y bottom-up) */
static void wimg_emit_cells(wbuf *b, const wimg *im, const wcell *cells, int nc, int cx, int cy, int *ncells)
{
    int x0 = cx - im->w / 2, ytop = cy + im->h / 2 - 16;
    for (int c = 0; c < nc; c++) {
        wput16(b, (unsigned)(int16_t)(x0 + cells[c].x));
        wput16(b, (unsigned)(int16_t)(ytop - cells[c].y));
        wput32(b, cells[c].tile);
        wput8(b, 0); wput8(b, 0); wput8(b, 0); wput8(b, 0);   /* no flips, chain 0 */
        (*ncells)++;
    }
}

int tool_pack_weapons(const char *dir, const char *out)
{
    char path[1024], err[256]; json_val *doc; const json_val *types, *attach, *spawn;
    pak_writer *w; wbuf b = {0}, pal = {0}, rules = {0}, tiles = {0}, loose = {0}; int ntypes = 0, npal = 0, ncells = 0, natt = 0, rc = 0;
    static char tname[32][16]; int ncached = 0;
    int box_cx[16], box_cy[16], box_pose[16], nbox = 0;   /* the box art's per-pose centre */
    uint32_t next_tile = ENG_WPN_TILE0; int nimg = 0;
    wresolve(dir, "weapons.json", path, sizeof path);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "pack-weapons: %s: %s\n", path, err); return 1; }
    types = json_get(doc, "types");
    for (const json_val *t = types ? types->child : NULL; t; t = t->next) ntypes++;
    if (!ntypes || ntypes > 32) { fprintf(stderr, "pack-weapons: %s: %d weapon types (1..32)\n", path, ntypes); json_free(doc); return 1; }
    /* palette 0 = the shared bank */
    wresolve(dir, "palette.json", path, sizeof path);
    if (read_pens(path, &pal)) { json_free(doc); return 1; }
    npal = 1;
    w = pak_writer_new();
    {   /* the box art's centre per pose: where a generated type's image sits */
        const json_val *box = NULL;
        for (const json_val *t = types->child; t; t = t->next) if (t->key && !strcmp(t->key, "box")) box = t;
        for (const json_val *p = box ? json_get(box, "poses") : NULL, *q = p ? p->child : NULL; q; q = q->next) {
            int x0 = 32767, x1 = -32768, y0 = 32767, y1 = -32768;
            for (const json_val *c = q->child; c; c = c->next) {
                int x = (int)json_int(json_get(c, "x"), 0), y = (int)json_int(json_get(c, "y"), 0);
                int ch = (int)json_int(json_get(c, "chain"), 0);
                if (x < x0) x0 = x; if (x + 16 > x1) x1 = x + 16;
                if (y - 16 * ch < y0) y0 = y - 16 * ch;   /* chain stacks downward = smaller y (bottom-up) */
                if (y + 16 > y1) y1 = y + 16;
            }
            if (x1 > x0 && nbox < 16) { box_pose[nbox] = atoi(q->key ? q->key : "0"); box_cx[nbox] = (x0 + x1) / 2; box_cy[nbox] = (y0 + y1) / 2; nbox++; }
        }
    }
    wput32(&b, (uint32_t)ntypes);
    for (const json_val *t = types->child; t; t = t->next) {
        const json_val *poses = json_get(t, "poses"), *pj = json_get(t, "palette"), *rj = json_get(t, "rules");
        const json_val *ij = json_get(t, "image");
        char name[16] = {0}; int nposes = 0, pidx = 0;
        snprintf(name, sizeof name, "%.15s", t->key ? t->key : "?");
        snprintf(tname[ncached++], 16, "%s", name);
        if (pj && pj->type == JSON_STRING && pj->str[0]) {   /* a type's own palette file = its own bank */
            wresolve(dir, pj->str, path, sizeof path);
            if (read_pens(path, &pal)) rc = 1; else pidx = npal++;
        }
        if (ij && ij->type == JSON_STRING && ij->str[0]) {   /* IMAGE type: generated cells on the box poses */
            wimg im = {0}, sw = {0}; const json_val *sj = json_get(t, "image_swing"), *nj = json_get(t, "nudge");
            wcell cells[64], scells[64]; int nc = 0, nsc = 0;
            int ndx = (int)json_int(json_at(nj, 0), 0), ndy = (int)json_int(json_at(nj, 1), 0);
            static const int carry[6] = { 120, 121, 128, 129, 130, 131 }, swingp[2] = { 122, 123 };
            wresolve(dir, ij->str, path, sizeof path);
            if (wimg_load(path, &im)) { rc = 1; wput(&b, name, 16); wput8(&b, 0); wput8(&b, 1); wput8(&b, 0); wput8(&b, 0); wput32(&b, 0); goto rules_row; }
            if (!(pj && pj->type == JSON_STRING && pj->str[0])) {   /* auto palette from the PNG */
                for (int k = 0; k < 16; k++) wput16(&pal, im.pens[k]);
                pidx = npal++;
            }
            nc = wimg_tiles(&im, &tiles, next_tile, cells, 64); next_tile += 0;   /* tile ids assigned inside */
            {   uint32_t used = 0; for (int c = 0; c < nc; c++) if (cells[c].tile + 1 - ENG_WPN_TILE0 > used) used = cells[c].tile + 1 - ENG_WPN_TILE0;
                next_tile = ENG_WPN_TILE0 + used; }
            if (sj && sj->type == JSON_STRING && sj->str[0]) {
                wresolve(dir, sj->str, path, sizeof path);
                if (!wimg_load(path, &sw)) {
                    for (int c = 0; c < 64; c++) scells[c].tile = 0;
                    nsc = wimg_tiles(&sw, &tiles, next_tile, scells, 64);
                    { uint32_t used = 0; for (int c = 0; c < nsc; c++) if (scells[c].tile + 1 - ENG_WPN_TILE0 > used) used = scells[c].tile + 1 - ENG_WPN_TILE0;
                      next_tile = ENG_WPN_TILE0 + used; }
                }
            }
            nposes = 6 + 2;
            wput(&b, name, 16); wput8(&b, (unsigned)pidx); wput8(&b, 1); wput8(&b, 0); wput8(&b, 0);   /* flags b0 = image type */
            wput32(&b, (uint32_t)nposes);
            for (int k = 0; k < 6; k++) {
                int cx = 0, cy = 0;
                for (int q = 0; q < nbox; q++) if (box_pose[q] == carry[k]) { cx = box_cx[q]; cy = box_cy[q]; }
                wput16(&b, (unsigned)carry[k]); wput16(&b, (unsigned)nc);
                wimg_emit_cells(&b, &im, cells, nc, cx + ndx, cy + ndy, &ncells);
            }
            for (int k = 0; k < 2; k++) {
                const wimg *ui = nsc ? &sw : &im; const wcell *uc = nsc ? scells : cells; int un = nsc ? nsc : nc;
                int cx = 0, cy = 0;
                for (int q = 0; q < nbox; q++) if (box_pose[q] == swingp[k]) { cx = box_cx[q]; cy = box_cy[q]; }
                wput16(&b, (unsigned)swingp[k]); wput16(&b, (unsigned)un);
                wimg_emit_cells(&b, ui, uc, un, cx + ndx, cy + ndy, &ncells);
            }
            {   /* wloose: lying + two tumble variants. All three are the PLAIN
                 * image now — the flipx/flipy tumble variants read as a
                 * different object mid-air ("one frame ... is the box"), and
                 * the bounce "didn't look good at all" (user 2026-08-28). */
                wput8(&loose, 1); wput8(&loose, 0); wput8(&loose, 0); wput8(&loose, 0);
                for (int v = 0; v < 3; v++) {
                    int x0 = -im.w / 2, ytop = im.h - 16;   /* the object STANDS on the origin (the ground line) */
                    wput16(&loose, (unsigned)nc);
                    for (int c = 0; c < nc; c++) {
                        wput16(&loose, (unsigned)(int16_t)(x0 + cells[c].x));
                        wput16(&loose, (unsigned)(int16_t)(ytop - cells[c].y));
                        wput32(&loose, cells[c].tile);
                        wput8(&loose, 0); wput8(&loose, 0); wput8(&loose, 0); wput8(&loose, 0);
                    }
                }
            }
            nimg++;
            wimg_free(&im); if (nsc) wimg_free(&sw);
            goto rules_row;
        }
        wput8(&loose, 0); wput8(&loose, 0); wput8(&loose, 0); wput8(&loose, 0);   /* a cell type: no loose art (ROM) */
        for (const json_val *p = poses ? poses->child : NULL; p; p = p->next) nposes++;
        wput(&b, name, 16); wput8(&b, (unsigned)pidx); wput8(&b, 0); wput8(&b, 0); wput8(&b, 0);
        wput32(&b, (uint32_t)nposes);
        for (const json_val *p = poses ? poses->child : NULL; p; p = p->next) {
            wput16(&b, (unsigned)atoi(p->key ? p->key : "0")); wput16(&b, (unsigned)p->n);
            for (const json_val *c = p->child; c; c = c->next) {
                wput16(&b, (unsigned)(int16_t)json_int(json_get(c, "x"), 0));
                wput16(&b, (unsigned)(int16_t)json_int(json_get(c, "y"), 0));
                wput32(&b, (uint32_t)json_int(json_get(c, "tile"), 0));
                wput8(&b, (unsigned)json_int(json_get(c, "flipx"), 0));
                wput8(&b, (unsigned)json_int(json_get(c, "flipy"), 0));
                wput8(&b, (unsigned)json_int(json_get(c, "chain"), 0));
                wput8(&b, 0);
                ncells++;
            }
        }
rules_row:
        /* per-type rules row: 0 = "use the weapon_rules scalar" */
        wput16(&rules, (unsigned)json_int(json_get(rj, "carry_dz"), 0));
        wput16(&rules, (unsigned)json_int(json_get(rj, "carry_dx"), 0));
        wput16(&rules, (unsigned)json_int(json_get(rj, "tumble_steps"), 0));
        wput16(&rules, (unsigned)json_int(json_get(rj, "damage"), 0));   /* 0 = the record's own */
    }
    pak_writer_add(w, "wtypes", b.p, (uint32_t)b.n); b.n = 0;
    if (next_tile > ENG_WPN_TILE0) {
        if (next_tile - ENG_WPN_TILE0 > 0x2000) { fprintf(stderr, "pack-weapons: weapon tile arena overflow (%u tiles)\n", next_tile - ENG_WPN_TILE0); rc = 1; }
        wput32(&b, next_tile - ENG_WPN_TILE0); wput(&b, tiles.p, tiles.n);
        pak_writer_add(w, "wtiles", b.p, (uint32_t)b.n); b.n = 0;
    }
    if (nimg) {
        wput32(&b, (uint32_t)ntypes); wput(&b, loose.p, loose.n);
        pak_writer_add(w, "wloose", b.p, (uint32_t)b.n); b.n = 0;
    }
    wput32(&b, (uint32_t)npal); wput(&b, pal.p, pal.n);
    pak_writer_add(w, "wpal", b.p, (uint32_t)b.n); b.n = 0;
    wput32(&b, (uint32_t)ntypes); wput(&b, rules.p, rules.n);
    pak_writer_add(w, "wrules", b.p, (uint32_t)b.n); b.n = 0;
    /* attach: {"wrestler": {"pose": [dx, dy]}} */
    attach = json_get(doc, "attach");
    wput32(&b, 0);
    for (const json_val *a = attach ? attach->child : NULL; a; a = a->next) {
        unsigned id = (unsigned)atoi(a->key ? a->key : "99");
        if (id >= ENG_WS_MAX) continue;
        for (const json_val *p = a->child; p; p = p->next) {
            wput8(&b, id); wput8(&b, 0); wput16(&b, (unsigned)atoi(p->key ? p->key : "0"));
            wput16(&b, (unsigned)(int16_t)json_int(json_at(p, 0), 0)); wput16(&b, (unsigned)(int16_t)json_int(json_at(p, 1), 0));
            natt++;
        }
    }
    b.p[0] = (uint8_t)natt; b.p[1] = (uint8_t)(natt >> 8); b.p[2] = (uint8_t)(natt >> 16); b.p[3] = (uint8_t)(natt >> 24);
    pak_writer_add(w, "wattach", b.p, (uint32_t)b.n); b.n = 0;
    /* spawn list: type names per ringside slot (default: the first ENG_WEAPONS types) */
    spawn = json_get(doc, "spawn");
    {
        int n = 0; uint8_t slots[ENG_WEAPONS];
        for (int k = 0; k < ENG_WEAPONS; k++) slots[k] = (uint8_t)(k < 2 && k < ntypes ? k : 0xFF);
        if (spawn && spawn->type == JSON_ARRAY) {
            for (const json_val *s = spawn->child; s && n < ENG_WEAPONS; s = s->next, n++) {
                int found = -1;
                if (s->type == JSON_STRING && !s->str[0]) { slots[n] = 0xFF; continue; }   /* "" = EMPTY slot */
                for (int t = 0; t < ncached; t++) if (s->type == JSON_STRING && !strcmp(s->str, tname[t])) found = t;
                if (found < 0) { fprintf(stderr, "pack-weapons: spawn slot %d: unknown weapon type \"%s\"\n", n, s->type == JSON_STRING ? s->str : "?"); rc = 1; found = 0; }
                slots[n] = (uint8_t)found;
            }
        }
        wput32(&b, ENG_WEAPONS); for (int k = 0; k < ENG_WEAPONS; k++) wput8(&b, slots[k]);
        pak_writer_add(w, "wspawn", b.p, (uint32_t)b.n); b.n = 0;
    }
    {   /* "wslots": slot placements [{ "x":, "y":, "in": }] (user 2026-08-28:
           5 outside + 5 inside; absent = the 2 ROM ringside spots) */
        const json_val *sl = json_get(doc, "slots");
        if (sl && sl->type == JSON_ARRAY) {
            int n = 0;
            for (const json_val *s = sl->child; s && n < ENG_WEAPONS; s = s->next) n++;
            wput32(&b, (uint32_t)n); n = 0;
            for (const json_val *s = sl->child; s && n < ENG_WEAPONS; s = s->next, n++) {
                wput16(&b, (unsigned)(int16_t)json_int(json_get(s, "x"), 0));
                wput16(&b, (unsigned)(int16_t)json_int(json_get(s, "y"), 0));
                wput8(&b, (unsigned)json_int(json_get(s, "in"), 0)); wput8(&b, 0);
            }
            pak_writer_add(w, "wslots", b.p, (uint32_t)b.n); b.n = 0;
        }
    }
    json_free(doc);
    if (pak_writer_save(w, out)) { fprintf(stderr, "pack-weapons: cannot write %s\n", out); rc = 1; }
    pak_writer_free(w); free(b.p); free(pal.p); free(rules.p); free(tiles.p); free(loose.p);
    fprintf(stderr, "pack-weapons: %d types, %d cells, %d attach entries, %d palettes -> %s\n", ntypes, ncells, natt, npal, out);
    return rc;
}

/* --weapon-check: for every stock man x carry pose x facing, the pak's cells
 * (with his attach offset, mirrored by the own_f rule) must equal the ROM
 * template's bank-15 cells one for one — the proof that the splice draws
 * what the ROM drew. */
int tool_weapon_check(void)
{
    int bad = 0, checked = 0;
    for (unsigned id = 0; id < ENG_WS_MAX; id++)
        for (unsigned pose = 0; pose < 0x400; pose++)
            for (int flip = 0; flip < 2; flip++) {
                const eng_pkg_rec *T; const uint8_t *ovl; static eng_pkg_rec W[64]; int tn, wn, nt = 0, miss = 0;
                tn = eng_pkg_template(id, pose, flip, &T);
                ovl = eng_pkg_overlay(id, pose);
                if (tn <= 0 || !ovl) continue;
                for (int t = 0; t < tn; t++) nt += ovl[t] == ENG_OVL_WEAPON;
                if (!nt) continue;
                wn = eng_wpn_cells(pose, (int)id, flip, W, 64);
                checked++;
                if (wn != nt) { fprintf(stderr, "weapon-check: wrestler %02u pose %u flip %d: template %d weapon cells, table %d\n", id, pose, flip, nt, wn); bad++; continue; }
                for (int t = 0; t < tn; t++) {
                    int hit = 0;
                    if (ovl[t] != ENG_OVL_WEAPON) continue;
                    for (int k = 0; k < wn && !hit; k++)
                        hit = W[k].x == T[t].x && W[k].y == T[t].y && W[k].tile == T[t].tile && W[k].flipx == T[t].flipx && W[k].flipy == T[t].flipy && W[k].chain == T[t].chain;
                    if (!hit) { if (!miss) fprintf(stderr, "weapon-check: wrestler %02u pose %u flip %d: template cell tile %u at %d,%d f%d%d c%d has no table cell\n", id, pose, flip, T[t].tile, T[t].x, T[t].y, T[t].flipx, T[t].flipy, T[t].chain); miss++; }
                }
                bad += miss > 0;
            }
    fprintf(stderr, "weapon-check: %d (wrestler, pose, facing) frames checked, %d mismatched\n", checked, bad);
    return bad ? 1 : 0;
}
