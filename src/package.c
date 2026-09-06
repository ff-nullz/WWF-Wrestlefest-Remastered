/* Wrestler packages — data/wrestlers/NN/{poses,palette,stats}.json as
 * written by tools/export_wrestler.c. When a package exists for a row the
 * renderer takes its pose command lists instead of decoding the ROM
 * bytecode, and the palette installer takes its pens. A minimal parser
 * for exactly this schema (numbers, nested arrays/objects, quoted keys). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "wf.h"
#include "tbl.h"
#include "engine.h"
#include "pak.h"
#include "json.h"
#include "profile.h"
extern int eng_dbgsel;

typedef eng_pkg_rec pkg_rec;
typedef struct { pkg_rec *r; int n; } pkg_list;
typedef struct { pkg_list own, own_f, part[12], part_f[12]; int present; } pkg_pose;
typedef struct { uint16_t pose; uint8_t row; pkg_list own, own_f; } pkg_vict;
typedef struct { int loaded; pkg_pose pose[0x400]; uint16_t pens[16]; int has_pal;
    pkg_vict *vict; int nvict;
    uint8_t *ovl[0x400]; int ovl_loaded;   /* overlay tags per own record: ENG_OVL_* */
    uint16_t (*cellpal)[16]; uint16_t *cellpose; int ncell;   /* portrait surfaces' own palettes */
                 int hp, walk, run; int has_stats;
                 uint8_t movemap[64]; int has_movemap; int skin_layout;
                 uint8_t *grids; int ngrids; int grid_sel;   /* alternate move grids ("mgrids"), 0 = move_map, 1..n = grids */
                 uint8_t *tmat; int ntmat;                   /* per-grid throw matrix rows ("tmatrix", 6 bytes each) */
                 struct { uint8_t ev, kind, cmd; char wav[32]; } snd[8]; int nsnd;   /* "sounds" section (eng_ws_sound) */
                 pkg_list (*skin)[2]; } pkg_wrestler;   /* skin: the SKIN ART lists (own, own_f) of a stock man in the skin layout; pose[] keeps the ROM lists as the TEMPLATE */   /* skin_layout: a stock man packed from data/stockskins - draws as OWN art */
static pkg_wrestler pkg[ENG_WS_EXT_MAX];   /* 0..11 stock, 12..15 clone slots */
static void load_sheet(unsigned id);

/* Clone registry (ids 12..15): filled when NN.pak's "clone" section loads.
 * eng_ws_base() routes every ROM-table index through it. */
typedef struct { int reg; int base; char name[32]; } clone_meta;
static clone_meta clone_reg[ENG_WS_EXT_MAX - ENG_WS_MAX];
static void load_one(unsigned id);
static void calib_load_section(int id, const uint8_t *sec, uint32_t len);
static void voff_load_section(int id, const uint8_t *sec, uint32_t len);
static void depth_load_section(int id, const uint8_t *sec, uint32_t len);

int eng_ws_clone_base(int id)
{
    if (id < ENG_WS_MAX || id >= ENG_WS_EXT_MAX) return -1;
    if (!pkg[id].loaded) load_one((unsigned)id);
    return clone_reg[id - ENG_WS_MAX].reg ? clone_reg[id - ENG_WS_MAX].base : -1;
}
int eng_ws_base(int id)
{
    static int warned;
    int b;
    if ((unsigned)id < (unsigned)ENG_WS_MAX) return id;
    b = eng_ws_clone_base(id);
    if (b >= 0) return b;
    if (!(warned & (1 << (id & 0x1F)))) {
        warned |= 1 << (id & 0x1F);
        fprintf(stderr, "package: wrestler id %d has no registered clone base — using 0\n", id);
    }
    return 0;
}
int eng_ws_sound(int id, int event, unsigned *cmd, const char **wav)
{
    pkg_wrestler *w;
    if (id < 0 || id >= ENG_WS_EXT_MAX) return 0;
    if (!pkg[id].loaded) load_one((unsigned)id);
    w = &pkg[id];
    for (int k = 0; k < w->nsnd; k++) if (w->snd[k].ev == event) {
        if (w->snd[k].kind == 1) { if (wav) *wav = w->snd[k].wav; return 2; }
        if (cmd) *cmd = w->snd[k].cmd; return 1;
    }
    return 0;
}
const char *eng_ws_clone_name(int id)
{
    if (eng_ws_clone_base(id) < 0) return NULL;
    return clone_reg[id - ENG_WS_MAX].name;
}
/* CLASS STOCK TEMPLATES (2026-08-25): data/classes/C/stock packs into
 * hidden slot ENG_CLASS_SLOT0+C — the class's universal skin (the stock
 * member doing every move, every victim frame). Stock wrestlers fall back
 * to their class package for art they lack (moves outside their own set,
 * mirror-hole victim frames). Never selectable: the name starts with '@'. */
int eng_ws_hidden(int id)
{
    const char *n = eng_ws_clone_name(id);
    return n && n[0] == '@';
}
int eng_ws_class_slot(int cls)
{
    int id;
    if (cls < 0 || cls >= ENG_BODY_CLASSES) return -1;
    id = ENG_CLASS_SLOT0 + cls;
    return eng_ws_clone_base(id) >= 0 && eng_ws_hidden(id) ? id : -1;
}

/* Body template = the ROM's own size class (behind_grab_class 0x18DFA,
 * 0..4). It is also the art template: same-class bodies are pixel-near
 * interchangeable (silhouette IoU 0.92-0.98 at the same pose index,
 * measured 2026-08-25), other classes are not (0.63-0.83). */
static const char *const body_class_names[ENG_BODY_CLASSES] = {
    "medium", "lean", "small", "heavy", "giant" };
int eng_ws_body_class(int id)
{
    unsigned c = tbl8(TBL(behind_grab_class), (uint32_t)eng_ws_base(id));
    return c < ENG_BODY_CLASSES ? (int)c : ENG_BODY_CLASSES - 1;
}
const char *eng_body_class_name(int cls)
{
    return (unsigned)cls < ENG_BODY_CLASSES ? body_class_names[cls] : "?";
}

/* Runtime ALT registration (no pak): a DUPLICATE pick becomes a clone of
 * its base with auto-transformed pens, so two of the same wrestler never
 * share colours ("2 warriors... the second one has a different pallet",
 * user 2026-08-24). Returns the clone id, or -1 (slots full). */
static uint16_t alt_pens[ENG_WS_EXT_MAX - ENG_WS_MAX][16];
static uint8_t  alt_on[ENG_WS_EXT_MAX - ENG_WS_MAX];
static void alt_pens_compute(int s, int base)
{
    /* palette-select rule (user 2026-08-24): the first copy wears the
     * CHOSEN outfit (eng_palsel_pens, install_body_palettes), so the
     * duplicate is the auto-ALT of that OUTFIT, not of stock — nibble-
     * swapping stock would ignore the pick entirely. */
    const uint16_t *bp = eng_palsel_pens(base);
    if (!bp) bp = eng_pkg_palette((unsigned)base);
    for (int k = 0; k < 16; k++) {
        unsigned w = bp ? bp[k] : 0;
        /* swap the R and B nibbles (xBGR): a strong, always-visible
         * recolour that keeps luminance shape */
        alt_pens[s][k] = (uint16_t)(((w & 0xFu) << 8) | (w & 0xF0u) | ((w >> 8) & 0xFu));
    }
}
int eng_pkg_register_alt(int base)
{
    if ((unsigned)base >= (unsigned)ENG_WS_MAX) base = eng_ws_base(base);
    for (int id = ENG_WS_MAX; id < ENG_WS_EXT_MAX; id++) {
        int s = id - ENG_WS_MAX;
        /* an ALT slot already holding this base is REUSED (matches are
         * re-inited every campaign stage: without reuse the 4 slots leak
         * away in 4 duplicate-pick matches, and a re-picked outfit kept
         * the first match's pens) */
        if (alt_on[s] && clone_reg[s].base == base) {
            alt_pens_compute(s, base);
            fprintf(stderr, "package: wrestler %d = ALT palette of %02d (reused)\n", id, base);
            return id;
        }
    }
    for (int id = ENG_WS_MAX; id < ENG_WS_EXT_MAX; id++) {
        int s = id - ENG_WS_MAX;
        if (eng_ws_clone_base(id) >= 0 || alt_on[s]) continue;   /* pak clones win */
        alt_pens_compute(s, base);
        clone_reg[s].reg = 1; clone_reg[s].base = base;
        snprintf(clone_reg[s].name, sizeof clone_reg[s].name, "%s", "ALT");
        alt_on[s] = 1;
        pkg[id].loaded = 1;            /* no pak to load */
        fprintf(stderr, "package: wrestler %d = ALT palette of %02d\n", id, base);
        return id;
    }
    return -1;
}

static const char *skip_ws(const char *p) { while (*p && isspace((unsigned char)*p)) p++; return p; }
static const char *expect(const char *p, char c) { p = skip_ws(p); return (*p == c) ? p + 1 : 0; }
static const char *read_int(const char *p, long *v) { char *e; p = skip_ws(p); *v = strtol(p, &e, 10); return e == p ? 0 : e; }
static const char *read_key(const char *p, char *out, size_t n)
{
    size_t i = 0; p = expect(p, '"'); if (!p) return 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0; if (*p != '"') return 0; return expect(p + 1, ':');
}
/* [ {"tile":..,"x":..,"y":..,"flipx":..,"flipy":..,"chain":..,"pal":..}, ... ] */
static const char *read_list(const char *p, pkg_list *L)
{
    pkg_rec tmp[64]; int n = 0; char key[16]; long v;
    p = expect(p, '['); if (!p) return 0;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']') { p++; break; }
        if (*p == ',') { p++; continue; }
        p = expect(p, '{'); if (!p) return 0;
        memset(&tmp[n], 0, sizeof tmp[n]);
        for (;;) {
            p = skip_ws(p);
            if (*p == '}') { p++; break; }
            if (*p == ',') { p++; continue; }
            p = read_key(p, key, sizeof key); if (!p) return 0;
            p = read_int(p, &v); if (!p) return 0;
            if (!strcmp(key, "tile")) tmp[n].tile = (uint32_t)v;
            else if (!strcmp(key, "x")) tmp[n].x = (int16_t)v;
            else if (!strcmp(key, "y")) tmp[n].y = (int16_t)v;
            else if (!strcmp(key, "flipx")) tmp[n].flipx = (uint8_t)v;
            else if (!strcmp(key, "flipy")) tmp[n].flipy = (uint8_t)v;
            else if (!strcmp(key, "chain")) tmp[n].chain = (uint8_t)v;
            else if (!strcmp(key, "pal")) tmp[n].pal = (uint8_t)v;
        }
        if (n < 64) n++;
    }
    L->n = n; L->r = n ? malloc(sizeof(pkg_rec) * (size_t)n) : 0;
    if (L->r) memcpy(L->r, tmp, sizeof(pkg_rec) * (size_t)n);
    return p;
}
static const char *read_partner_map(const char *p, pkg_list *arr)
{
    char key[16]; long prow;
    p = expect(p, '{'); if (!p) return 0;
    for (;;) {
        p = skip_ws(p);
        if (*p == '}') return p + 1;
        if (*p == ',') { p++; continue; }
        p = read_key(p, key, sizeof key); if (!p) return 0;
        prow = strtol(key, 0, 10);
        if (prow >= 0 && prow < 12) p = read_list(p, &arr[prow]);
        else { pkg_list junk; p = read_list(p, &junk); free(junk.r); }
        if (!p) return 0;
    }
}
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb"); long n; char *b;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1); if (!b) { fclose(f); return 0; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    b[n] = 0; fclose(f); return b;
}

/* Pak backend (ADR-001): build/wrestlers/NN.pak written by tools/pack_wrestler.c.
 * Returns 0 when the package came from the pak. */
static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }
static int load_one_pak(unsigned id, const char *path)
{
    pkg_wrestler *w = &pkg[id];
    pak *pk = pak_open(path);
    const uint8_t *sec; uint32_t len;
    extern void wf_video_set_tile_pens(unsigned t, const uint8_t *pens);
    if (!pk) return -1;
    if (id >= ENG_WS_MAX && (sec = pak_section(pk, "clone", &len)) && len >= 3) {
        /* clone meta: u16 base + nul-terminated name (tools/pack_wrestler.c) */
        unsigned base = rd16(sec);
        if (base < ENG_WS_MAX) {
            clone_meta *c = &clone_reg[id - ENG_WS_MAX];
            c->reg = 1; c->base = (int)base;
            snprintf(c->name, sizeof c->name, "%.*s", (int)(len - 2), (const char *)sec + 2);
            fprintf(stderr, "package: wrestler %02u = clone of %02u (\"%s\")\n", id, base, c->name);
        } else
            fprintf(stderr, "package: wrestler %02u clone base %u out of range\n", id, base);
    }
    if ((sec = pak_section(pk, "skinlay", &len)) && len >= 1) w->skin_layout = sec[0] != 0;
    w->nsnd = 0;
    if ((sec = pak_section(pk, "sounds", &len)) && len >= 1) {   /* u8 n; per entry u8 ev, u8 kind, u8 cmd, nul name */
        unsigned n = sec[0], o = 1;
        for (unsigned k = 0; k < n && w->nsnd < 8 && o + 4 <= len; k++) {
            size_t nl = strnlen((const char *)sec + o + 3, len - o - 3);
            w->snd[w->nsnd].ev = sec[o]; w->snd[w->nsnd].kind = sec[o + 1]; w->snd[w->nsnd].cmd = sec[o + 2];
            snprintf(w->snd[w->nsnd].wav, sizeof w->snd[0].wav, "%.*s", (int)nl, (const char *)sec + o + 3);
            w->nsnd++; o += 3 + (unsigned)nl + 1;
        }
    }
    if ((sec = pak_section(pk, "palette", &len)) && len >= 32) {
        for (int k = 0; k < 16; k++) w->pens[k] = (uint16_t)rd16(sec + k * 2);
        w->has_pal = 1;
    }
    if ((sec = pak_section(pk, "stats", &len)) && len >= 6) {
        w->hp = (int)rd16(sec); w->walk = (int)rd16(sec + 2); w->run = (int)rd16(sec + 4);
        w->has_stats = w->hp > 0;
    }
    if ((sec = pak_section(pk, "movemap", &len)) && len >= 63) {
        memcpy(w->movemap, sec, len > 64 ? 64 : len);
        w->has_movemap = 1;
    }
    if ((sec = pak_section(pk, "tmatrix", &len)) && len >= 6) {
        free(w->tmat); w->tmat = malloc(len); w->ntmat = 0;
        if (w->tmat) { memcpy(w->tmat, sec, len); w->ntmat = (int)(len / 6); }
    }
    if ((sec = pak_section(pk, "mgrids", &len)) && len >= 63) {
        free(w->grids); w->grids = malloc(len); w->ngrids = 0;
        if (w->grids) { memcpy(w->grids, sec, len); w->ngrids = (int)(len / 63); }
        if (getenv("WF_GRID")) w->grid_sel = atoi(getenv("WF_GRID"));   /* harness / launch pick */
        if (w->grid_sel < 0 || w->grid_sel > w->ngrids) w->grid_sel = 0;
    }
    if (!getenv("WF_NOPKG") && (sec = pak_section(pk, "tiles2", &len)) && len >= 4) {
        uint32_t n = rd32(sec); uint32_t slen;
        const uint8_t *sheet = pak_section(pk, "sheet", &slen);
        if (sheet && len >= 4 + n * 4 && slen >= n * 256)
            for (uint32_t i = 0; i < n; i++) wf_video_set_tile_pens(rd32(sec + 4 + i * 4), sheet + (size_t)i * 256);
        fprintf(stderr, "package: wrestler %02u sheet loaded from pak (%u tiles, wide ids)\n", id, n);
    } else if (!getenv("WF_NOPKG") && (sec = pak_section(pk, "tiles", &len)) && len >= 4) {
        uint32_t n = rd32(sec), slen;
        const uint8_t *sheet = pak_section(pk, "sheet", &slen);
        if (sheet && len >= 4 + n * 2 && slen >= n * 256)
            for (uint32_t i = 0; i < n; i++) wf_video_set_tile_pens(rd16(sec + 4 + i * 2), sheet + (size_t)i * 256);
        fprintf(stderr, "package: wrestler %02u sheet loaded from pak (%u tiles)\n", id, n);
    }
    if ((sec = pak_section(pk, "poses2", &len)) && len >= 4) {
        uint32_t count = rd32(sec), off = 4;
        for (uint32_t c = 0; c < count && off + 6 <= len; c++) {
            unsigned pose = rd16(sec + off), kind = sec[off + 2], prow = sec[off + 3], n = rd16(sec + off + 4);
            pkg_list *L = NULL;
            off += 6;
            if (off + n * 12 > len) break;
            if (pose < 0x400) {
                if (kind == 0) { L = &w->pose[pose].own; w->pose[pose].present = 1; }
                else if (kind == 1) L = &w->pose[pose].own_f;
                else if (kind == 2 && prow < 12) L = &w->pose[pose].part[prow];
                else if (kind == 3 && prow < 12) L = &w->pose[pose].part_f[prow];
            }
            if (L) {
                L->n = (int)n; L->r = n ? malloc(sizeof(pkg_rec) * n) : NULL;
                for (unsigned i = 0; i < n; i++) {
                    const uint8_t *q = sec + off + i * 12;
                    L->r[i].x = (int16_t)rd16(q); L->r[i].y = (int16_t)rd16(q + 2); L->r[i].tile = rd32(q + 4);
                    L->r[i].flipx = q[8]; L->r[i].flipy = q[9]; L->r[i].chain = q[10]; L->r[i].pal = q[11];
                }
            }
            off += n * 12;
        }
        fprintf(stderr, "package: wrestler %02u poses loaded from pak (wide ids)\n", id);
        if ((sec = pak_section(pk, "skin2", &len)) && len >= 4) {   /* skin art lists (stock man in the skin layout) */
            uint32_t count = rd32(sec), off = 4;
            if (!w->skin) w->skin = calloc(0x400, sizeof *w->skin);
            for (uint32_t c = 0; c < count && w->skin && off + 6 <= len; c++) {
                unsigned pose = rd16(sec + off), kind = sec[off + 2], n = rd16(sec + off + 4);
                pkg_list *L = NULL;
                off += 6;
                if (off + n * 12 > len) break;
                if (pose < 0x400 && kind < 2) L = &w->skin[pose][kind];
                if (L) {
                    L->n = (int)n; L->r = n ? malloc(sizeof(pkg_rec) * n) : NULL;
                    for (unsigned i = 0; i < n; i++) {
                        const uint8_t *q = sec + off + i * 12;
                        L->r[i].x = (int16_t)rd16(q); L->r[i].y = (int16_t)rd16(q + 2); L->r[i].tile = rd32(q + 4);
                        L->r[i].flipx = q[8]; L->r[i].flipy = q[9]; L->r[i].chain = q[10]; L->r[i].pal = q[11];
                    }
                }
                off += n * 12;
            }
            fprintf(stderr, "package: wrestler %02u skin art loaded from pak\n", id);
        }
        if ((sec = pak_section(pk, "calib", &len))) calib_load_section((int)id, sec, len);   /* holder-side offsets of this package */
        if ((sec = pak_section(pk, "voffs", &len))) voff_load_section((int)id, sec, len);
        if ((sec = pak_section(pk, "depth", &len))) depth_load_section((int)id, sec, len);   /* draw-order overrides */
        if ((sec = pak_section(pk, "cellpal", &len)) && len >= 4) {
            uint32_t cnt = rd32(sec), off = 4;
            for (uint32_t c = 0; c < cnt && off + 34 <= len; c++, off += 34) {
                int e = w->ncell++;
                w->cellpal = realloc(w->cellpal, sizeof(uint16_t[16]) * (size_t)w->ncell);
                w->cellpose = realloc(w->cellpose, sizeof(uint16_t) * (size_t)w->ncell);
                w->cellpose[e] = rd16(sec + off);
                for (int k = 0; k < 16; k++) w->cellpal[e][k] = rd16(sec + off + 2 + k * 2);
            }
        }
        if ((sec = pak_section(pk, "vict2", &len)) && len >= 4) {
            /* VICTIM-BODY art: the clone being held inside (row, pose)'s
               composed frame — swapped in by sprite.c for the base's
               victim cells (docs/ai-art-pipeline.md) */
            uint32_t vcount = rd32(sec), voff = 4;
            for (uint32_t c = 0; c < vcount && voff + 6 <= len; c++) {
                unsigned vpose = rd16(sec + voff), kind = sec[voff + 2], vrow = sec[voff + 3], n = rd16(sec + voff + 4);
                pkg_list *L = NULL;
                voff += 6;
                if (voff + n * 12 > len) break;
                if (vpose < 0x400 && vrow <= 12 && kind < 2) {   /* row 12 = SELF (his own base holds him) */
                    int e = -1;
                    for (int k = 0; k < w->nvict; k++)
                        if (w->vict[k].row == vrow && w->vict[k].pose == vpose) { e = k; break; }
                    if (e < 0) {
                        w->vict = realloc(w->vict, sizeof(pkg_vict) * (size_t)(w->nvict + 1));
                        e = w->nvict++;
                        memset(&w->vict[e], 0, sizeof w->vict[e]);
                        w->vict[e].row = (uint8_t)vrow; w->vict[e].pose = (uint16_t)vpose;
                    }
                    L = kind == 0 ? &w->vict[e].own : &w->vict[e].own_f;
                }
                if (L) {
                    L->n = (int)n; L->r = n ? malloc(sizeof(pkg_rec) * n) : NULL;
                    for (unsigned i = 0; i < n; i++) {
                        const uint8_t *q = sec + voff + i * 12;
                        L->r[i].x = (int16_t)rd16(q); L->r[i].y = (int16_t)rd16(q + 2); L->r[i].tile = rd32(q + 4);
                        L->r[i].flipx = q[8]; L->r[i].flipy = q[9]; L->r[i].chain = q[10]; L->r[i].pal = q[11];
                    }
                }
                voff += n * 12;
            }
            if (w->nvict)
                fprintf(stderr, "package: wrestler %02u victim art loaded (%d holds)\n", id, w->nvict);
        }
    } else if ((sec = pak_section(pk, "poses", &len)) && len >= 4) {
        uint32_t count = rd32(sec), off = 4;
        for (uint32_t c = 0; c < count && off + 6 <= len; c++) {
            unsigned pose = rd16(sec + off), kind = sec[off + 2], prow = sec[off + 3], n = rd16(sec + off + 4);
            pkg_list *L = NULL;
            off += 6;
            if (off + n * 10 > len) break;
            if (pose < 0x400) {
                if (kind == 0) { L = &w->pose[pose].own; w->pose[pose].present = 1; }
                else if (kind == 1) L = &w->pose[pose].own_f;
                else if (kind == 2 && prow < 12) L = &w->pose[pose].part[prow];
                else if (kind == 3 && prow < 12) L = &w->pose[pose].part_f[prow];
            }
            if (L) {
                L->n = (int)n; L->r = n ? malloc(sizeof(pkg_rec) * n) : NULL;
                for (unsigned i = 0; i < n; i++) {
                    const uint8_t *q = sec + off + i * 10;
                    L->r[i].x = (int16_t)rd16(q); L->r[i].y = (int16_t)rd16(q + 2); L->r[i].tile = (uint16_t)rd16(q + 4);
                    L->r[i].flipx = q[6]; L->r[i].flipy = q[7]; L->r[i].chain = q[8]; L->r[i].pal = q[9];
                }
            }
            off += n * 10;
        }
        fprintf(stderr, "package: wrestler %02u poses loaded from pak\n", id);
    }
    pak_close(pk);          /* everything was copied out */
    return 0;
}

static void load_one(unsigned id)
{
    char path[256]; char *buf; const char *p; char key[32];
    pkg_wrestler *w = &pkg[id];
    w->loaded = 1;
    {   /* pak first unless WF_DATA=rom (transition: JSON tree stays the fallback) */
        const char *mode = getenv("WF_DATA") ? getenv("WF_DATA") : "auto";
        snprintf(path, sizeof path, "%s/%02u.pak", getenv("WF_PAKDIR") ? getenv("WF_PAKDIR") : "build/wrestlers", id);
        if (strcmp(mode, "rom") && load_one_pak(id, path) == 0) return;
        if (id >= ENG_WS_MAX) return;  /* clone slots live in paks only (no data/ tree) */
        if (!strcmp(mode, "pak")) { fprintf(stderr, "package: %s missing (WF_DATA=pak)\n", path); return; }
    }
    snprintf(path, sizeof path, "data/wrestlers/%02u/poses.json", id);
    buf = slurp(path);
    if (buf) {
        p = strstr(buf, "\"poses\"");
        if (p) {
            p = expect(p + 7, ':'); p = p ? expect(p, '{') : 0;
            while (p) {
                long pose; p = skip_ws(p);
                if (*p == '}') break;
                if (*p == ',') { p++; continue; }
                p = read_key(p, key, sizeof key); if (!p) break;
                pose = strtol(key, 0, 10);
                p = expect(p, '{'); if (!p) break;
                for (;;) {
                    p = skip_ws(p);
                    if (*p == '}') { p++; break; }
                    if (*p == ',') { p++; continue; }
                    p = read_key(p, key, sizeof key); if (!p) break;
                    if (pose < 0 || pose >= 0x400) { pkg_list junk; p = read_list(p, &junk); free(junk.r); continue; }
                    if (!strcmp(key, "own")) { p = read_list(p, &w->pose[pose].own); w->pose[pose].present = 1; }
                    else if (!strcmp(key, "own_f")) p = read_list(p, &w->pose[pose].own_f);
                    else if (!strcmp(key, "with_partner")) p = read_partner_map(p, w->pose[pose].part);
                    else if (!strcmp(key, "with_partner_f")) p = read_partner_map(p, w->pose[pose].part_f);
                    else { pkg_list junk; p = read_list(p, &junk); free(junk.r); }
                    if (!p) break;
                }
            }
            fprintf(stderr, "package: wrestler %02u poses loaded\n", id);
        }
        free(buf);
    }
    snprintf(path, sizeof path, "data/wrestlers/%02u/palette.json", id);
    buf = slurp(path);
    if (buf) {
        p = strstr(buf, "\"pens\"");
        if (p && (p = expect(p + 6, ':')) && (p = expect(p, '['))) {
            for (int k = 0; k < 16; k++) { long v; p = read_int(p, &v); if (!p) break; w->pens[k] = (uint16_t)v; p = skip_ws(p); if (*p == ',') p++; }
            w->has_pal = 1;
        }
        free(buf);
    }
    if (!getenv("WF_NOPKG")) load_sheet(id);
    snprintf(path, sizeof path, "data/wrestlers/%02u/stats.json", id);
    buf = slurp(path);
    if (buf) {
        const char *q; long v;
        if ((q = strstr(buf, "\"hp\"")) && (q = strstr(q, "\"value\"")) && (q = expect(q + 7, ':')) && read_int(q, &v)) w->hp = (int)v;
        if ((q = strstr(buf, "\"walk_speed\"")) && (q = strstr(q, "\"value\"")) && (q = expect(q + 7, ':')) && read_int(q, &v)) w->walk = (int)v;
        if ((q = strstr(buf, "\"run_speed\"")) && (q = strstr(q, "\"value\"")) && (q = expect(q + 7, ':')) && read_int(q, &v)) w->run = (int)v;
        w->has_stats = w->hp > 0;
        free(buf);
    }
}

static void load_sheet(unsigned id)
{
    char path[256]; char *buf; const char *p;
    uint8_t *img; int w, h; long v;
    static uint32_t tiles[8192]; int n = 0;
    extern int wf_video_load_indexed_png(const char *path, uint8_t **out, int *w, int *h);
    extern void wf_video_set_tile_pens(unsigned t, const uint8_t *pens);
    snprintf(path, sizeof path, "data/wrestlers/%02u/tiles.json", id);
    buf = slurp(path); if (!buf) return;
    p = strstr(buf, "\"tiles\"");
    if (p && (p = expect(p + 7, ':')) && (p = expect(p, '['))) {
        while ((p = read_int(p, &v))) {
            if (n < 8192) tiles[n++] = (uint32_t)v;
            p = skip_ws(p); if (*p != ',') break; p++;
        }
    }
    free(buf);
    if (!n) return;
    snprintf(path, sizeof path, "data/wrestlers/%02u/sheet.png", id);
    if (wf_video_load_indexed_png(path, &img, &w, &h) != 0) return;
    for (int i = 0; i < n; i++) {
        int ox = (i % 16) * 16, oy = (i / 16) * 16;
        uint8_t pens[256];
        if (oy + 16 > h || ox + 16 > w) break;
        for (int y = 0; y < 16; y++) memcpy(pens + y * 16, img + (size_t)(oy + y) * w + ox, 16);
        wf_video_set_tile_pens(tiles[i], pens);
    }
    free(img);
    fprintf(stderr, "package: wrestler %02u sheet loaded (%d tiles)\n", id, n);
}

/* Clone delegation: an id 12..15 answers from its OWN pak first, else from
 * its base's package (identity for stock ids; depth-1 recursion — a clone's
 * base is always < 12). */
static int clone_delegate(unsigned id)
{
    int b;
    if (id < ENG_WS_MAX) return -1;
    b = eng_ws_clone_base((int)id);
    return b;                          /* -1 = unregistered */
}
/* ---- overlay tags (weapons / ropes baked into a pose's cells) ----
 * AUTOMATIC (2026-08-25): the ROM marks them by palette bank — the 18
 * carry/swing poses 0x72-0x83 draw the stairs/box with nibble 15 (the
 * shared weapon bank, row 0x0F's palette) and the 10 rope-climb poses
 * 0x20-0x29 draw the rope section with nibble 14; the body always has
 * the wrestler's own nibble. One byte per OWN record (flip-0 order; own_f
 * mirrors record for record): ENG_OVL_WEAPON / ENG_OVL_ROPE. A mod-layer
 * wrestlers/BB/overlay.json {"poses": {"114": [0,0,1,...]}} (editor
 * tagger) OVERRIDES the automatic tags for the poses it lists — normally
 * unnecessary. Stray foreign nibbles (a few cells: Hawk's LOD-duo poses
 * carry Animal's bank, etc.) stay body, as the ROM draws them. */
static void load_overlay(unsigned id)
{
    char rel[64], path[1024], err[256]; json_val *doc; const json_val *poses;
    pkg[id].ovl_loaded = 1;
    for (unsigned p = 0; p < 0x400; p++) {
        const pkg_list *L = &pkg[id].pose[p].own; int any = 0;
        for (int i = 0; i < L->n; i++) any |= (L->r[i].pal & 0x0Fu) == 15u || (L->r[i].pal & 0x0Fu) == 14u;
        if (!any) continue;
        pkg[id].ovl[p] = calloc((size_t)L->n, 1);
        for (int i = 0; i < L->n; i++)
            pkg[id].ovl[p][i] = (L->r[i].pal & 0x0Fu) == 15u ? ENG_OVL_WEAPON : (L->r[i].pal & 0x0Fu) == 14u ? ENG_OVL_ROPE : 0;
    }
    snprintf(rel, sizeof rel, "wrestlers/%02u/overlay.json", id);
    if (!wf_mod_resolve(rel, path, sizeof path)) snprintf(path, sizeof path, "data/wrestlers/%02u/overlay.json", id);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) return;
    poses = json_get(doc, "poses");
    for (unsigned p = 0; p < 0x400 && poses; p++) {
        char key[8]; const json_val *arr; int n;
        snprintf(key, sizeof key, "%u", p);
        arr = json_get(poses, key);
        if (!arr) continue;
        n = pkg[id].pose[p].own.n;
        if (!n) continue;
        if (!pkg[id].ovl[p]) pkg[id].ovl[p] = calloc((size_t)n, 1);
        for (int i = 0; i < n; i++) pkg[id].ovl[p][i] = (uint8_t)json_int(json_at(arr, i), 0);
    }
    json_free(doc);
}
int eng_pkg_own_n(unsigned id, unsigned pose)   /* records in the package's OWN list (what the overlay tags index) */
{
    if (id >= ENG_WS_EXT_MAX || pose >= 0x400) return 0;
    if (!pkg[id].loaded) load_one(id);
    return pkg[id].pose[pose].own.n;
}
const uint8_t *eng_pkg_overlay(unsigned id, unsigned pose)
{
    if (id >= ENG_WS_EXT_MAX || pose >= 0x400) return NULL;
    if (!pkg[id].loaded) load_one(id);
    if (!pkg[id].ovl_loaded) load_overlay(id);
    return pkg[id].ovl[pose];
}
/* ---- WEAPON PAK (2026-08-27): the weapon overlay table as data. The pak
 * (tools/pack_weapons.c, from data/weapons) carries every weapon type's
 * cells per carry/pickup/swing pose, the per-stock-man attach offsets, the
 * palettes, the per-type rules row and the profile's spawn list. eng_compose
 * emits these cells in place of the template's bank-15 cells, so a new
 * weapon is a table entry and the stock set draws exactly as the ROM did
 * (`--weapon-check`). WF_NOWPNPAK=1 = the old verbatim template splice. */
typedef struct { uint16_t pose, n; const uint8_t *cells; } wpn_pose;   /* cells: 12 bytes each */
typedef struct { char name[16]; uint8_t pal, flags; int npose; wpn_pose *poses; uint16_t rules[4];
                 uint16_t ln[3]; const uint8_t *loose[3]; } wpn_type;   /* flags b0 = IMAGE type (rides the box poses); loose = wloose cells */
typedef struct { uint8_t id; uint16_t pose; int16_t dx, dy; } wpn_att;
typedef struct { int16_t x, y; uint8_t inside; } wpn_slot;
static struct { int loaded, ok; pak *pk; int ntypes; wpn_type *types; int natt; wpn_att *att;
                int npal; const uint8_t *pal; uint8_t spawn[ENG_WEAPONS];
                int nslots; wpn_slot slots[ENG_WEAPONS]; } wpn;
static void wpn_load(void)
{
    char path[256]; const uint8_t *sec; uint32_t len;
    wpn.loaded = 1;
    for (int k = 0; k < ENG_WEAPONS; k++) wpn.spawn[k] = (uint8_t)(k < 2 ? k : 0xFF);   /* the ROM's slots: steps, box; the rest EMPTY (before any early return) */
    if (getenv("WF_NOWPNPAK")) return;
    if (getenv("WF_DATA") && !strcmp(getenv("WF_DATA"), "rom")) return;   /* the ROM reference render (verify gates) never reads a pak */
    if (getenv("WF_WPNPAK")) snprintf(path, sizeof path, "%s", getenv("WF_WPNPAK"));
    else wf_profile_weapons_pak(path, sizeof path);
    wpn.pk = pak_open(path);
    if (!wpn.pk) { fprintf(stderr, "package: %s missing - weapons draw from the ROM template cells (run ./build.sh / --pack-profile)\n", path); return; }
    if ((sec = pak_section(wpn.pk, "wtypes", &len)) && len >= 4) {
        const uint8_t *p = sec + 4, *end = sec + len;
        wpn.ntypes = (int)rd32(sec);
        wpn.types = calloc((size_t)wpn.ntypes, sizeof(wpn_type));
        for (int t = 0; t < wpn.ntypes && p + 24 <= end; t++) {
            wpn_type *T = &wpn.types[t];
            memcpy(T->name, p, 16); T->name[15] = 0; T->pal = p[16]; T->flags = p[17]; T->npose = (int)rd32(p + 20); p += 24;
            T->poses = calloc((size_t)T->npose, sizeof(wpn_pose));
            for (int k = 0; k < T->npose && p + 4 <= end; k++) {
                T->poses[k].pose = (uint16_t)rd16(p); T->poses[k].n = (uint16_t)rd16(p + 2); T->poses[k].cells = p + 4;
                p += 4 + 12 * (size_t)T->poses[k].n;
            }
        }
    }
    if ((sec = pak_section(wpn.pk, "wrules", &len)) && len >= 4) {
        uint32_t stride = len >= 4 + 8u * (uint32_t)rd32(sec) ? 8u : 6u;   /* old paks: 3 fields */
        for (int t = 0; t < wpn.ntypes && t < (int)rd32(sec) && 4 + stride * (uint32_t)(t + 1) <= len; t++)
            for (uint32_t f = 0; f < stride / 2; f++) wpn.types[t].rules[f] = (uint16_t)rd16(sec + 4 + stride * (uint32_t)t + 2 * f);
    }
    if ((sec = pak_section(wpn.pk, "wattach", &len)) && len >= 4) {
        wpn.natt = (int)rd32(sec);
        wpn.att = calloc((size_t)wpn.natt, sizeof(wpn_att));
        for (int k = 0; k < wpn.natt && 4 + 8 * (uint32_t)(k + 1) <= len; k++) {
            const uint8_t *p = sec + 4 + 8 * k;
            wpn.att[k].id = p[0]; wpn.att[k].pose = (uint16_t)rd16(p + 2); wpn.att[k].dx = (int16_t)rd16(p + 4); wpn.att[k].dy = (int16_t)rd16(p + 6);
        }
    }
    if ((sec = pak_section(wpn.pk, "wtiles", &len)) && len >= 4) {   /* pak-ingested weapon art -> the weapon tile arena */
        uint32_t count = rd32(sec);
        if (4 + count * 256 <= len) wf_video_inject_sprite_tiles(ENG_WPN_TILE0, count, sec + 4);
    }
    if ((sec = pak_section(wpn.pk, "wloose", &len)) && len >= 4) {   /* ringside lying/tumble cells per type */
        const uint8_t *p = sec + 4, *end = sec + len;
        int nt = (int)rd32(sec);
        for (int t = 0; t < nt && t < wpn.ntypes && p < end; t++) {
            int has = *p; p += 4;
            if (!has) continue;
            for (int v = 0; v < 3 && p + 2 <= end; v++) {
                wpn.types[t].ln[v] = (uint16_t)rd16(p);
                wpn.types[t].loose[v] = p + 2;
                p += 2 + 12 * (size_t)wpn.types[t].ln[v];
            }
        }
    }
    if ((sec = pak_section(wpn.pk, "wpal", &len)) && len >= 4 + 32) { wpn.npal = (int)rd32(sec); wpn.pal = sec + 4; if ((uint32_t)wpn.npal * 32 + 4 > len) wpn.npal = (int)((len - 4) / 32); }
    if ((sec = pak_section(wpn.pk, "wspawn", &len)) && len >= 4)
        for (int k = 0; k < ENG_WEAPONS && k < (int)rd32(sec) && 4 + (uint32_t)k < len; k++) wpn.spawn[k] = sec[4 + k];
    if ((sec = pak_section(wpn.pk, "wslots", &len)) && len >= 4) {   /* slot PLACEMENTS (user 2026-08-28):
                                          per slot {i16 x, i16 y, u8 inside, u8 pad} */
        wpn.nslots = (int)rd32(sec);
        if (wpn.nslots > ENG_WEAPONS) wpn.nslots = ENG_WEAPONS;
        for (int k = 0; k < wpn.nslots && 4 + 6u * (uint32_t)(k + 1) <= len; k++) {
            wpn.slots[k].x = (int16_t)rd16(sec + 4 + 6 * k);
            wpn.slots[k].y = (int16_t)rd16(sec + 4 + 6 * k + 2);
            wpn.slots[k].inside = sec[4 + 6 * k + 4];
        }
    }
    wpn.ok = wpn.ntypes > 0;
    if (eng_dbgsel) fprintf(stderr, "package: %s: %d weapon types, %d attach entries, %d palettes\n", path, wpn.ntypes, wpn.natt, wpn.npal);
}
int eng_wpn_spawn_type(int slot) { if (!wpn.loaded) wpn_load(); return slot >= 0 && slot < ENG_WEAPONS ? wpn.spawn[slot] : 0xFF; }
int eng_wpn_slot(int slot, int *x, int *y, int *inside)
{
    if (!wpn.loaded) wpn_load();
    if (slot < 0 || slot >= wpn.nslots) return 0;
    if (x) *x = wpn.slots[slot].x;
    if (y) *y = wpn.slots[slot].y;
    if (inside) *inside = wpn.slots[slot].inside;
    return 1;
}
int eng_wpn_rule(int type, int field)          /* 0 = no per-type value */
{
    if (!wpn.loaded) wpn_load();
    return type >= 0 && type < wpn.ntypes && field >= 0 && field < 4 ? wpn.types[type].rules[field] : 0;
}
const uint16_t *eng_wpn_palette(int idx)
{
    static uint16_t pens[16];
    if (!wpn.loaded) wpn_load();
    if (!wpn.ok || idx < 0 || idx >= wpn.npal) return NULL;
    for (int k = 0; k < 16; k++) pens[k] = (uint16_t)rd16(wpn.pal + idx * 32 + k * 2);
    return pens;
}
/* the attach shift of `wrestler` for `pose`: a stock man's own entry, a
 * clone / class template / roster man his BASE's (the class base's hand) */
static void wpn_attach(int wrestler, unsigned pose, int *dx, int *dy)
{
    int id = wrestler;
    *dx = *dy = 0;
    if (id >= ENG_WS_MAX) id = eng_ws_base(id);
    if (id < 0 || id >= ENG_WS_MAX) return;
    for (int k = 0; k < wpn.natt; k++) if (wpn.att[k].id == id && wpn.att[k].pose == pose) { *dx = wpn.att[k].dx; *dy = wpn.att[k].dy; return; }
}
/* every weapon type's cells for `pose` (the poses are type-specific: steps
 * 114-119/124-127, box 120-123/128-131) placed for `wrestler`, mirrored by
 * the ROM own_f rule for `flip`. Records carry pal = 15 (the weapon bank).
 * Returns the count, 0 = no weapon in this pose / no pak. */
static int wpn_ctx = -1;               /* the weapon type the object being emitted HOLDS */
void eng_wpn_carry_ctx(int type) { wpn_ctx = type; }
static int wpn_type_has_pose(int t, unsigned pose)
{
    for (int k = 0; k < wpn.types[t].npose; k++) if (wpn.types[t].poses[k].pose == pose) return 1;
    return 0;
}
int eng_wpn_loose(int type, int variant, const eng_pkg_rec **out, int *pal_idx)
{
    static eng_pkg_rec R[32]; int n = 0;
    if (!wpn.loaded) wpn_load();
    if (!wpn.ok || type < 0 || type >= wpn.ntypes || variant < 0 || variant > 2) return 0;
    if (!wpn.types[type].loose[variant]) return 0;
    for (int c = 0; c < wpn.types[type].ln[variant] && n < 32; c++) {
        const uint8_t *q = wpn.types[type].loose[variant] + 12 * c;
        eng_pkg_rec r;
        r.x = (int16_t)rd16(q); r.y = (int16_t)rd16(q + 2);
        r.tile = rd32(q + 4); r.flipx = q[8]; r.flipy = q[9]; r.chain = q[10];
        r.pal = (uint8_t)(15u | ((unsigned)wpn.types[type].pal << 4));
        R[n++] = r;
    }
    if (pal_idx) *pal_idx = wpn.types[type].pal;
    *out = R;
    return n;
}
int eng_wpn_cells(unsigned pose, int wrestler, int flip, eng_pkg_rec *out, int max)
{
    int n = 0, dx, dy, ctx_img;
    if (!wpn.loaded) wpn_load();
    if (!wpn.ok) return 0;
    wpn_attach(wrestler, pose, &dx, &dy);
    ctx_img = wpn_ctx >= 0 && wpn_ctx < wpn.ntypes && (wpn.types[wpn_ctx].flags & 1) && wpn_type_has_pose(wpn_ctx, pose);
    if (eng_dbgsel && wpn_ctx >= 0 && wpn_ctx < wpn.ntypes && (wpn.types[wpn_ctx].flags & 1) && !ctx_img) {
        static int seen[64]; if (pose < 200 && !seen[pose & 63]) { seen[pose & 63] = 1;
            fprintf(stderr, "wpn: img type %d LACKS pose %u - template cells show\n", wpn_ctx, pose); } }
    for (int t = 0; t < wpn.ntypes; t++)
        {
        int img = wpn.types[t].flags & 1;
        if (img) { if (wpn_ctx != t) continue; }            /* an IMAGE type draws only in the hands that hold it */
        else if (ctx_img) continue;                          /* the held image type replaces the box on the shared poses */
        for (int k = 0; k < wpn.types[t].npose; k++) {
            const wpn_pose *P = &wpn.types[t].poses[k];
            if (P->pose != pose) continue;
            for (int c = 0; c < P->n && n < max; c++) {
                const uint8_t *q = P->cells + 12 * c;
                eng_pkg_rec r;
                r.x = (int16_t)((int16_t)rd16(q) + dx); r.y = (int16_t)((int16_t)rd16(q + 2) + dy);
                r.tile = rd32(q + 4); r.flipx = q[8]; r.flipy = q[9]; r.chain = q[10];
                r.pal = (uint8_t)(15u | ((unsigned)wpn.types[t].pal << 4));   /* high nibble = the type's palette index (sprite.c weapon_bank_get) */
                if (flip) {
                    if (img) { r.x = (int16_t)(-r.x - 16); r.flipx = !r.flipx; }   /* TRUE mirror: generated cells are not ROM-compensated */
                    else     { r.x = (int16_t)-r.x; r.flipx = !r.flipx; }          /* ROM own_f rule: x -> -x */
                }
                out[n++] = r;
            }
        }
        }
    return n;
}
int eng_pkg_has_poses(unsigned id)
{
    if (id >= ENG_WS_EXT_MAX) return 0;
    if (!pkg[id].loaded) load_one(id);
    if (pkg[id].pose[0].present || pkg[id].pose[1].present) return 1;
    { int b = clone_delegate(id); return b >= 0 ? eng_pkg_has_poses((unsigned)b) : 0; }
}
const uint16_t *eng_pkg_palette(unsigned id)
{
    if (id >= ENG_WS_EXT_MAX) return 0;
    if (id >= ENG_WS_MAX && alt_on[id - ENG_WS_MAX])
        return alt_pens[id - ENG_WS_MAX];       /* runtime ALT palette */
    if (!pkg[id].loaded) load_one(id);
    if (pkg[id].has_pal) return pkg[id].pens;
    { int b = clone_delegate(id); return b >= 0 ? eng_pkg_palette((unsigned)b) : 0; }
}
uint16_t *eng_pkg_pens_mut(unsigned id)
{
    if (id >= ENG_WS_EXT_MAX) return 0;
    if (!pkg[id].loaded) load_one(id);
    if (!pkg[id].has_pal) return 0;
    return pkg[id].pens;
}
/* The wrestler's OWN move map (build-a-wrestler override): package
 * "movemap" bytes when present; a clone without one uses his base's;
 * everyone else the ROM row (move_map_ptrs 0xE4FE). Always 63 valid
 * bytes ([cat 0..0x14][col B1/B2/both]). */
unsigned eng_ws_move8(unsigned id, unsigned off)
{
    if (id < ENG_WS_EXT_MAX && !getenv("WF_NOPKG")) {
        if (!pkg[id].loaded) load_one(id);
        if (pkg[id].grid_sel > 0 && pkg[id].grid_sel <= pkg[id].ngrids && off < 63)
            return pkg[id].grids[(size_t)(pkg[id].grid_sel - 1) * 63u + off];   /* an ALTERNATE grid */
        if (pkg[id].has_movemap && off < 64)
            return pkg[id].movemap[off];
        if (id >= ENG_WS_MAX) {                       /* clone: the base's map */
            int b = eng_ws_clone_base((int)id);
            if (b >= 0) return eng_ws_move8((unsigned)b, off);
        }
    }
    return tbl_ra8(tbl32(tbl_id("move_map_ptrs"), (uint32_t)eng_ws_base((int)id) * 4u) + off);
}

/* the package's THROW MATRIX row for the active grid: category for (opp band, bank), -1 = none */
int eng_ws_tmatrix(unsigned id, unsigned band, unsigned bank)
{
    int g;
    if (id >= ENG_WS_EXT_MAX || band > 2 || bank > 1) return -1;
    if (!pkg[id].loaded) load_one(id);
    g = pkg[id].grid_sel;
    if (!pkg[id].tmat || g >= pkg[id].ntmat) return -1;
    return pkg[id].tmat[(size_t)g * 6u + band * 2u + bank];
}
/* alternate move grids (the generic's A/B/C, user 2026-08-26): count and pick */
int eng_ws_grid_count(unsigned id) { if (id >= ENG_WS_EXT_MAX) return 0; if (!pkg[id].loaded) load_one(id); return pkg[id].ngrids; }
int eng_ws_grid_get(unsigned id) { return id < ENG_WS_EXT_MAX ? pkg[id].grid_sel : 0; }
void eng_ws_grid_set(unsigned id, int k)
{
    if (id >= ENG_WS_EXT_MAX) return;
    if (!pkg[id].loaded) load_one(id);
    pkg[id].grid_sel = (k < 0 || k > pkg[id].ngrids) ? 0 : k;
}

/* Editor: forget one wrestler's loaded package so the next query re-reads
 * the pak (after the Forge packs a new clone into the slot). The old pose
 * lists leak deliberately — records may still be referenced this frame,
 * and a reload is a rare editor action. */
void eng_pkg_reload(unsigned id)
{
    if (id >= ENG_WS_EXT_MAX) return;
    memset(&pkg[id], 0, sizeof pkg[id]);
    if (id >= ENG_WS_MAX) {
        clone_reg[id - ENG_WS_MAX].reg = 0;
        alt_on[id - ENG_WS_MAX] = 0;
    }
    load_one(id);
}

int eng_pkg_stat(unsigned id, const char *name, int fallback)
{
    if (id >= ENG_WS_EXT_MAX) return fallback;
    if (!pkg[id].loaded) load_one(id);
    if (!pkg[id].has_stats) {
        int b = clone_delegate(id);
        return b >= 0 ? eng_pkg_stat((unsigned)b, name, fallback) : fallback;
    }
    if (!strcmp(name, "hp")) return pkg[id].hp;
    if (!strcmp(name, "walk")) return pkg[id].walk;
    if (!strcmp(name, "run")) return pkg[id].run;
    return fallback;
}
/* Records for (row, pose, flip, partner row); returns the count, 0 if the
 * package has no such pose. Partner variants fall back to the own body. */
static int pkg_pose_own;   /* last eng_pkg_pose: 1 = the id's own pak art,
                              0 = delegated to the base (sprite.c keeps the
                              BASE palette bank for delegated frames) */
int eng_pkg_pose_was_own(void) { return pkg_pose_own; }
/* Which records of holder (row, pose, flip)'s composed frame for victim
 * `victim` are the VICTIM's cells: HOLDER SUBTRACTION — a composed frame's
 * holder cells are exactly the holder's OWN single pose (same records,
 * same order; verified for all 12 by --verify-skin, 2026-08-25), so any
 * record not in the own list is the victim's. This also covers the two
 * cases the palette nibble gets wrong: a holder holding his own base
 * (both bodies one nibble) and the ~20 ROM cells whose nibble is a third
 * wrestler's or, by chance, the holder's (Hawk-holds-Jake pose 265).
 * Returns the victim cell count, 0 if there is no such variant. */
int eng_pkg_victim_mask(unsigned row, unsigned pose, int flip, unsigned victim, uint8_t *mask, int n)
{
    const eng_pkg_rec *pr; const pkg_list *O; int pn, vn = 0;
    if (row >= ENG_WS_MAX || pose >= 0x400 || victim >= ENG_WS_MAX) return 0;
    if (!eng_pkg_has_part(row, pose, (int)victim)) return 0;
    pn = eng_pkg_pose(row, pose, flip, (int)victim, &pr);
    if (pn <= 0 || pn > n) return 0;
    if (!pkg[row].loaded) load_one(row);
    O = flip ? &pkg[row].pose[pose].own_f : &pkg[row].pose[pose].own;
    for (int i = 0; i < pn; i++) {
        int holder = 0;
        for (int k = 0; k < O->n && !holder; k++) {
            const pkg_rec *s = &O->r[k];
            holder = s->x == pr[i].x && s->y == pr[i].y && s->chain == pr[i].chain
                  && s->flipx == pr[i].flipx && s->flipy == pr[i].flipy && s->tile == pr[i].tile;
        }
        mask[i] = !holder; vn += mask[i];
    }
    return vn;
}
/* A hold's variant for `victim` is missing but others exist: the nearest
 * body (class order) that the holder DOES hold in this pose — the
 * template for a MIRROR HOLE (holder holds his own base: the ROM never
 * drew 183 of those, e.g. Earthquake's mounted punches 343/344 on
 * Earthquake; a clone fighting his base hits them constantly and the
 * hidden victim vanished — playtest 2026-08-25). -1 = no variant at all. */
int eng_pkg_alt_victim(unsigned row, unsigned pose, unsigned victim)
{
    static const int order[ENG_BODY_CLASSES][ENG_BODY_CLASSES] = {
        { 0, 3, 1, 4, 2 }, { 1, 0, 2, 3, 4 }, { 2, 1, 0, 3, 4 }, { 3, 0, 4, 1, 2 }, { 4, 3, 0, 1, 2 } };
    int cls;
    if (row >= ENG_WS_MAX || pose >= 0x400 || victim >= ENG_WS_MAX) return -1;
    cls = eng_ws_body_class((int)victim);
    for (int k = 0; k < ENG_BODY_CLASSES; k++)
        for (unsigned w = 0; w < ENG_WS_MAX; w++)
            if (w != victim && eng_ws_body_class((int)w) == order[cls][k] && eng_pkg_has_part(row, pose, (int)w)) return (int)w;
    return -1;
}

/* Is the ROM's SELF variant (row holds his own base) a usable hold? The 68
 * "mirror variants" re-pose the same-colour victim — 265 lifts him off the
 * top of the frame, 263 stands him on the other side — which in a
 * clone-vs-base match reads as a body in the wrong place (playtest: the
 * power slam). A self variant whose victim cell footprint overlaps the
 * cross variants' by less than half is treated as a MIRROR HOLE instead
 * (template from the nearest other held body). Cached per (row, pose). */
int eng_pkg_mirror_ok(unsigned row, unsigned pose)
{
    static int8_t cache[ENG_WS_MAX][0x400];   /* 0 unknown, 1 ok, -1 hole */
    static uint8_t ma[192], mb[192], fa[64 * 64], fb[64 * 64];
    const eng_pkg_rec *pa, *pb; int na, nb, alt; unsigned inter = 0, uni = 0;
    if (row >= ENG_WS_MAX || pose >= 0x400) return 0;
    if (cache[row][pose]) return cache[row][pose] > 0;
    if (!eng_pkg_has_part(row, pose, (int)row)) { cache[row][pose] = -1; return 0; }
    alt = eng_pkg_alt_victim(row, pose, row);
    if (alt < 0) { cache[row][pose] = 1; return 1; }   /* nothing to compare with: keep it */
    na = eng_pkg_pose(row, pose, 0, (int)row, &pa); nb = eng_pkg_pose(row, pose, 0, alt, &pb);
    if (na <= 0 || nb <= 0 || na > 192 || nb > 192
        || eng_pkg_victim_mask(row, pose, 0, row, ma, 192) <= 0 || eng_pkg_victim_mask(row, pose, 0, (unsigned)alt, mb, 192) <= 0) { cache[row][pose] = 1; return 1; }
    memset(fa, 0, sizeof fa); memset(fb, 0, sizeof fb);   /* 4-px footprint grids over -128..127 */
    for (int pass = 0; pass < 2; pass++) {
        const eng_pkg_rec *pr = pass ? pb : pa; const uint8_t *m = pass ? mb : ma; int n = pass ? nb : na; uint8_t *f = pass ? fb : fa;
        for (int i = 0; i < n; i++) {
            if (!m[i]) continue;
            for (int y = pr[i].y; y < pr[i].y + 16 * ((pr[i].chain & 7) + 1); y += 4)
                for (int x = pr[i].x; x < pr[i].x + 16; x += 4) {
                    int gx = (x + 128) / 4, gy = (y + 128) / 4;
                    if (gx >= 0 && gx < 64 && gy >= 0 && gy < 64) f[gy * 64 + gx] = 1;
                }
        }
    }
    for (int i = 0; i < 64 * 64; i++) { inter += fa[i] & fb[i]; uni += fa[i] | fb[i]; }
    cache[row][pose] = (uni && inter * 2 >= uni) ? 1 : -1;
    return cache[row][pose] > 0;
}

/* ---- THE composition path (2026-08-25): one frame for any holder /
 * victim pair, stock or skin, from parts.
 *   TEMPLATE  = the stock composed list of (holder base, pose, victim base)
 *               (or the holder base's own list for a single pose): it is the
 *               DEPTH template — the two bodies interleave (hvh in 2647
 *               frames, vhvh in 959 ...), so each of its cells says which
 *               side draws at that point in the order.
 *   HOLDER    = the holder's body: a clone's own pose when he has it, else
 *               the template's holder cells themselves (stock: identity).
 *   VICTIM    = the victim's body: a clone's vict2 frame for (holder base,
 *               pose) when he has it, else the template's victim cells.
 *   OVERLAY   = the template's weapon cells (bank 15), verbatim. Rope cells
 *               (bank 14) draw only for a stock holder (user: skins are
 *               body-only, the arena paints the ropes).
 * Each skin record is placed at the template cell it matches exactly (a
 * stock cut: 1:1, so stock output is bit-identical) or, for an AI cut, at
 * the first template cell of its side whose footprint holds its centre;
 * unmatched records (skin pixels outside the stock silhouette) go last.
 * Returns the record count; src[k] = ENG_SRC_* for sprite.c's bank rules. */
static int rec_same(const eng_pkg_rec *a, const eng_pkg_rec *b)
{
    return a->x == b->x && a->y == b->y && a->chain == b->chain && a->flipx == b->flipx && a->flipy == b->flipy && a->tile == b->tile;
}
static int rec_overlap(const eng_pkg_rec *cell, const eng_pkg_rec *r)   /* footprint overlap area, px */
{
    int ax0 = cell->x, ax1 = cell->x + 16, ay0 = cell->y, ay1 = cell->y + 16 * ((cell->chain & 7) + 1);
    int bx0 = r->x, bx1 = r->x + 16, by0 = r->y, by1 = r->y + 16 * ((r->chain & 7) + 1);
    int w = (ax1 < bx1 ? ax1 : bx1) - (ax0 > bx0 ? ax0 : bx0), h = (ay1 < by1 ? ay1 : by1) - (ay0 > by0 ? ay0 : by0);
    return w > 0 && h > 0 ? w * h : 0;
}
/* OWN ART: a clone slot, or a stock man packed in the skin layout (non-stock
 * profiles) - eng_compose places his cells on the ROM template like a clone's */
static int pkg_own_art(int id)
{
    if (id < 0 || id >= ENG_WS_EXT_MAX) return 0;
    if (id >= ENG_WS_MAX) return 1;
    if (!pkg[id].loaded) load_one((unsigned)id);
    return pkg[id].skin_layout;
}
static const pkg_list *own_list(unsigned id, unsigned pose, int flip)
{
    if (id >= ENG_WS_EXT_MAX || pose >= 0x400) return NULL;
    if (!pkg[id].loaded) load_one(id);
    if (pkg[id].skin_layout) {         /* a stock man in the skin layout: his ART; NULL = no frame (base art) */
        const pkg_list *L = pkg[id].skin ? &pkg[id].skin[pose][flip ? 1 : 0] : NULL;
        return L && L->n ? L : NULL;
    }
    if (!pkg[id].pose[pose].present) return NULL;
    return flip ? &pkg[id].pose[pose].own_f : &pkg[id].pose[pose].own;
}
int eng_pkg_own_frame(unsigned id, unsigned pose)   /* records of the slot's OWN art for a single pose, 0 = none (base art) */
{
    const pkg_list *L = own_list(id, pose, 0);
    return L ? L->n : 0;
}
static int tmpl_fetch;                 /* eng_compose reading its TEMPLATE list: eng_pkg_pose must not compose */
/* ---- HOLDER-SIDE calibration (user 2026-08-25): per PACKAGE (a skin slot
 * or a class template — whoever's OWN art drew the holder's body), keyed
 * (move id | 0x90+state, frame) -> dx, dy applied to the holder's body
 * cells in eng_compose. Stock ROM art is exact and never shifted. Stored
 * as calib.json beside the package, packed as the "calib" section. */
#define CAL_MOVES 0xA0      /* 0x00-0x8F move ids (state 5); 0x90+state for the other pair-drawing
                              states (0x0B lockup, 0x0C tie-up stance ...) — eng_calib_key() */
#define CAL_FRAMES 16
typedef struct { uint8_t key, frame; int8_t dx, dy; } cal_ent;
static cal_ent *cals[ENG_WS_EXT_MAX]; static int ncals[ENG_WS_EXT_MAX];
static int cal_move = -1, cal_frame = -1;   /* compose context: the attacker's key + frame */
void eng_compose_ctx(int cls, int move, int frame) { (void)cls; cal_move = move; cal_frame = frame; }
int eng_calib_key(const eng_obj *o)
{
    unsigned st5 = o->state & 0xFFu;
    if (st5 == 5u) return (int)(o->move_id & 0xFFu);
    if (o->partner >= 0 && st5 < 0x10u) return 0x90 + (int)st5;
    return -1;
}
static cal_ent *cal_find(int id, int key, int frame, int make)
{
    if (id < 0 || id >= ENG_WS_EXT_MAX || key < 0 || key >= CAL_MOVES || frame < 0 || frame >= CAL_FRAMES) return NULL;
    for (int k = 0; k < ncals[id]; k++) if (cals[id][k].key == key && cals[id][k].frame == frame) return &cals[id][k];
    if (!make) return NULL;
    cals[id] = realloc(cals[id], sizeof(cal_ent) * (size_t)(ncals[id] + 1));
    cals[id][ncals[id]] = (cal_ent){ (uint8_t)key, (uint8_t)frame, 0, 0 };
    return &cals[id][ncals[id]++];
}
int eng_calib_get(int id, int key, int frame, int *dx, int *dy)
{
    const cal_ent *e = cal_find(id, key, frame, 0);
    *dx = e ? e->dx : 0; *dy = e ? e->dy : 0;
    return e && (e->dx || e->dy);
}
void eng_calib_set(int id, int key, int frame, int dx, int dy)
{
    cal_ent *e = cal_find(id, key, frame, 1);
    if (!e) return;
    if (dx < -127) dx = -127; if (dx > 127) dx = 127; if (dy < -127) dy = -127; if (dy > 127) dy = 127;
    e->dx = (int8_t)dx; e->dy = (int8_t)dy;
    if (eng_dbgsel) fprintf(stderr, "calib: package %d key 0x%02X frame %d -> %+d,%+d\n", id, key, frame, dx, dy);
}
int eng_calib_count(int id) { int n = 0; if (id >= 0 && id < ENG_WS_EXT_MAX) for (int k = 0; k < ncals[id]; k++) n += cals[id][k].dx || cals[id][k].dy; return n; }
int eng_calib_save(int id, const char *path)
{
    FILE *f; int first = 1;
    if (id < 0 || id >= ENG_WS_EXT_MAX) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{ \"note\": \"[move id or 0x90+state, frame, dx, dy]: this package's holder body cells shift by (dx, dy) on that frame (Calibrate tab, holder side)\",\n  \"entries\": [");
    for (int k = 0; k < ncals[id]; k++)
        if (cals[id][k].dx || cals[id][k].dy) { fprintf(f, "%s\n    [%d, %d, %d, %d]", first ? "" : ",", cals[id][k].key, cals[id][k].frame, cals[id][k].dx, cals[id][k].dy); first = 0; }
    fprintf(f, "\n  ] }\n");
    fclose(f);
    return 0;
}
static void calib_load_section(int id, const uint8_t *sec, uint32_t len)
{
    for (uint32_t i = 0; i + 4 <= len; i += 4)
        eng_calib_set(id, sec[i], sec[i + 1], (int8_t)sec[i + 2], (int8_t)sec[i + 3]);
}
/* ---- VICTIM-SIDE offsets (user 2026-08-25): per PACKAGE (a skin slot or a
 * class template), keyed (holder row, holder pose) -> dx, dy applied to
 * this man's OWN victim cells when he is held in that pose. Stored as
 * victoffs.json beside the package, packed as the "voffs" section. */
typedef struct { uint8_t row; uint16_t pose; int8_t dx, dy; } voff_ent;
static voff_ent *voffs[ENG_WS_EXT_MAX]; static int nvoffs[ENG_WS_EXT_MAX];
static voff_ent *voff_find(int id, unsigned row, unsigned pose, int make)
{
    if (id < 0 || id >= ENG_WS_EXT_MAX) return NULL;
    for (int k = 0; k < nvoffs[id]; k++) if (voffs[id][k].row == row && voffs[id][k].pose == pose) return &voffs[id][k];
    if (!make) return NULL;
    voffs[id] = realloc(voffs[id], sizeof(voff_ent) * (size_t)(nvoffs[id] + 1));
    voffs[id][nvoffs[id]] = (voff_ent){ (uint8_t)row, (uint16_t)pose, 0, 0 };
    return &voffs[id][nvoffs[id]++];
}
int eng_voff_get(int id, unsigned row, unsigned pose, int *dx, int *dy)
{
    const voff_ent *e = voff_find(id, row, pose, 0);
    *dx = e ? e->dx : 0; *dy = e ? e->dy : 0;
    return e && (e->dx || e->dy);
}
void eng_voff_set(int id, unsigned row, unsigned pose, int dx, int dy)
{
    voff_ent *e = voff_find(id, row, pose, 1);
    if (!e) return;
    if (dx < -127) dx = -127; if (dx > 127) dx = 127; if (dy < -127) dy = -127; if (dy > 127) dy = 127;
    e->dx = (int8_t)dx; e->dy = (int8_t)dy;
    if (eng_dbgsel) fprintf(stderr, "voff: id %d holder row %u pose %u -> %+d,%+d\n", id, row, pose, dx, dy);
}
int eng_voff_count(int id) { int n = 0; if (id >= 0 && id < ENG_WS_EXT_MAX) for (int k = 0; k < nvoffs[id]; k++) n += voffs[id][k].dx || voffs[id][k].dy; return n; }
int eng_voff_save(int id, const char *path)
{
    FILE *f; int first = 1;
    if (id < 0 || id >= ENG_WS_EXT_MAX) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{ \"note\": \"[holder row (12 = own base), holder pose, dx, dy]: this man's own victim cells shift by (dx, dy) when held in that pose (Calibrate tab, held side)\",\n  \"entries\": [");
    for (int k = 0; k < nvoffs[id]; k++)
        if (voffs[id][k].dx || voffs[id][k].dy) { fprintf(f, "%s\n    [%u, %u, %d, %d]", first ? "" : ",", voffs[id][k].row, voffs[id][k].pose, voffs[id][k].dx, voffs[id][k].dy); first = 0; }
    fprintf(f, "\n  ] }\n");
    fclose(f);
    return 0;
}
static void voff_load_section(int id, const uint8_t *sec, uint32_t len)
{
    for (uint32_t i = 0; i + 5 <= len; i += 5)
        eng_voff_set(id, sec[i], (unsigned)(sec[i + 1] | (sec[i + 2] << 8)), (int8_t)sec[i + 3], (int8_t)sec[i + 4]);
}
/* TWO LAYERS (user 2026-08-26): every skin of a class is generated from the
 * same mannequin refs with the same placement, so a grapple that is off is
 * off the same way for all of them. The CLASS template's table is the base
 * and the skin's own table is a delta on top; eng_compose applies the sum.
 * A class slot's own layer IS the class layer (no double count). */
int eng_calib_layer(int id) { int c = id >= 0 && id < ENG_WS_EXT_MAX ? eng_ws_class_slot(eng_ws_body_class(eng_ws_base(id))) : -1; return c == id ? -1 : c; }
int eng_calib_sum(int id, int key, int frame, int *dx, int *dy)
{
    int cx = 0, cy = 0, c = eng_calib_layer(id);
    if (c >= 0) eng_calib_get(c, key, frame, &cx, &cy);
    eng_calib_get(id, key, frame, dx, dy);
    *dx += cx; *dy += cy;
    return *dx || *dy;
}
int eng_voff_sum(int id, unsigned row, unsigned pose, int *dx, int *dy)
{
    int cx = 0, cy = 0, c = eng_calib_layer(id);
    if (c >= 0) eng_voff_get(c, row, pose, &cx, &cy);
    eng_voff_get(id, row, pose, dx, dy);
    *dx += cx; *dy += cy;
    return *dx || *dy;
}
/* ---- DEPTH OVERRIDE (user 2026-08-26): per HOLDER package, keyed (holder
 * pose, victim body class or 0xFF = any) -> 0 template interleave (default),
 * 1 holder over, 2 victim over. depth.json beside the package, pak section
 * "depth" [pose lo, pose hi, vclass, mode]; skin layer wins over class. */
typedef struct { uint16_t pose; uint8_t vcls, mode; } dep_ent;
static dep_ent *deps[ENG_WS_EXT_MAX]; static int ndeps[ENG_WS_EXT_MAX];
static int depth_mode;                /* compose context: the override in force for this frame */
static dep_ent *dep_find(int id, unsigned pose, unsigned vcls, int make)
{
    if (id < 0 || id >= ENG_WS_EXT_MAX || pose >= 0x400) return NULL;
    for (int k = 0; k < ndeps[id]; k++) if (deps[id][k].pose == pose && deps[id][k].vcls == vcls) return &deps[id][k];
    if (!make) return NULL;
    deps[id] = realloc(deps[id], sizeof(dep_ent) * (size_t)(ndeps[id] + 1));
    deps[id][ndeps[id]] = (dep_ent){ (uint16_t)pose, (uint8_t)vcls, 0 };
    return &deps[id][ndeps[id]++];
}
int eng_depth_get(int id, unsigned pose, unsigned vcls)
{
    const dep_ent *e = dep_find(id, pose, vcls, 0);
    if (!e || !e->mode) e = dep_find(id, pose, 0xFFu, 0);   /* the "any class" row */
    return e ? e->mode : 0;
}
void eng_depth_set(int id, unsigned pose, unsigned vcls, int mode)
{
    dep_ent *e = dep_find(id, pose, vcls, 1);
    if (!e) return;
    e->mode = (uint8_t)(mode < 0 ? 0 : mode > 2 ? 2 : mode);
    if (eng_dbgsel) fprintf(stderr, "depth: package %d pose 0x%03X vclass %u -> %d\n", id, pose, vcls, e->mode);
}
int eng_depth_sum(int id, unsigned pose, unsigned vcls)
{
    int m = eng_depth_get(id, pose, vcls), c = eng_calib_layer(id);
    if (!m && c >= 0) m = eng_depth_get(c, pose, vcls);
    return m;
}
int eng_depth_count(int id) { int n = 0; if (id >= 0 && id < ENG_WS_EXT_MAX) for (int k = 0; k < ndeps[id]; k++) n += deps[id][k].mode != 0; return n; }
int eng_depth_save(int id, const char *path)
{
    FILE *f; int first = 1;
    if (id < 0 || id >= ENG_WS_EXT_MAX) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{ \"note\": \"[holder pose, victim body class (255 = any), mode]: 1 = this package's body draws entirely OVER the held man, 2 = the held man draws over it, 0 = the stock frame's interleave (Calibrate tab, depth)\",\n  \"entries\": [");
    for (int k = 0; k < ndeps[id]; k++)
        if (deps[id][k].mode) { fprintf(f, "%s\n    [%u, %u, %u]", first ? "" : ",", deps[id][k].pose, deps[id][k].vcls, deps[id][k].mode); first = 0; }
    fprintf(f, "\n  ] }\n");
    fclose(f);
    return 0;
}
static void depth_load_section(int id, const uint8_t *sec, uint32_t len)
{
    for (uint32_t i = 0; i + 4 <= len; i += 4)
        eng_depth_set(id, (unsigned)(sec[i] | (sec[i + 1] << 8)), sec[i + 2], sec[i + 3]);
}
/* --calib-report: what each loaded package carries, and the MEAN per class —
 * a class whose entries cluster round one (dx, dy) has a placement bug in
 * the art pipeline (art_run place_s), not a calibration problem. */
void eng_calib_report(void)
{
    for (int id = 0; id < ENG_WS_EXT_MAX; id++) {
        long sx = 0, sy = 0; int n = 0, nv = 0;
        if (id >= ENG_WS_MAX && eng_ws_base(id) < 0) continue;   /* unregistered slot */
        eng_pkg_has_poses((unsigned)id);                          /* packages load lazily */
        for (int k = 0; k < ncals[id]; k++) if (cals[id][k].dx || cals[id][k].dy) { sx += cals[id][k].dx; sy += cals[id][k].dy; n++; }
        nv = eng_voff_count(id);
        if (!n && !nv && !eng_depth_count(id)) continue;
        printf("%s %2d %-14s class %d layer %s: %d holder entries (mean %+.1f,%+.1f), %d held entries, %d depth overrides\n",
               eng_calib_layer(id) < 0 ? "CLASS" : "skin ", id, eng_ws_clone_name(id) ? eng_ws_clone_name(id) : "(stock)",
               eng_ws_body_class(eng_ws_base(id)), eng_calib_layer(id) < 0 ? "base" : "delta",
               n, n ? (double)sx / n : 0.0, n ? (double)sy / n : 0.0, nv, eng_depth_count(id));
        for (int k = 0; k < ndeps[id]; k++)
            if (deps[id][k].mode) printf("    depth pose 0x%03X vclass %-3u  %s\n", deps[id][k].pose, deps[id][k].vcls, deps[id][k].mode == 1 ? "holder over" : "victim over");
        for (int k = 0; k < ncals[id]; k++)
            if (cals[id][k].dx || cals[id][k].dy)
                printf("    %s 0x%02X frame %2d  %+4d,%+4d\n", cals[id][k].key < 0x90 ? "move  " : "stance", cals[id][k].key < 0x90 ? cals[id][k].key : cals[id][k].key - 0x90, cals[id][k].frame, cals[id][k].dx, cals[id][k].dy);
    }
}
static uint8_t rsrc_static[256];
static void rsrc_fill(uint8_t code, int n) { for (int i = 0; i < n && i < 256; i++) rsrc_static[i] = code; }
static int compose_slot[2] = { -1, -1 };
int eng_compose_slot(int side) { return compose_slot[side & 1]; }
int eng_compose(int holder, unsigned pose, int flip, int victim, const eng_pkg_rec **out, const uint8_t **src)
{
    static eng_pkg_rec res[256]; static uint8_t rsrc[256];
    static uint8_t side[192], vmask[192];
    static int16_t at[2][192];         /* skin record -> template index, -1 none */
    const eng_pkg_rec *T, *H, *V; int tn, hn = 0, vn = 0, n = 0;
    int bh, bv = -1, tv, two = 0, hown = 0, vown = 0;
    const uint8_t *ovl = NULL;
    if (holder < 0 || holder >= ENG_WS_EXT_MAX || pose >= 0x400) return 0;
    bh = eng_ws_base(holder);
    if (victim >= 0 && victim < ENG_WS_EXT_MAX) bv = eng_ws_base(victim);
    tv = bv;                           /* the template's victim base (an alt for a mirror hole) */
    two = bv >= 0 && eng_pkg_has_part((unsigned)bh, pose, bv);
    {   /* TEMPLATE OWNER (user 2026-08-26: "opponent disappears" on a generic's
         * cross-class move): the base never drew this hold, so the two-man
         * template is borrowed from a stock man who did - same body class first
         * (placement matches), else any. The holder's own art still draws him. */
        int tb = bh;
        if (bv >= 0 && !two && pkg_own_art(holder)) {
            for (int pass = 0; pass < 2 && tb == bh; pass++)
                for (int c = 0; c < ENG_WS_MAX; c++) {
                    if (c == bh) continue;
                    if (pass == 0 && eng_ws_body_class(c) != eng_ws_body_class(bh)) continue;
                    if (c != bv && eng_pkg_has_part((unsigned)c, pose, bv)) { tb = c; break; }   /* (an owner holding HIMSELF is the self split - use his alt) */
                    {   /* the owner never held THIS body (his own class's mirror hole):
                           his nearest held body stands in as the template victim */
                        int alt = eng_pkg_alt_victim((unsigned)c, pose, (unsigned)bv);
                        if (alt >= 0) { tb = c; tv = alt; break; }
                    }
                }
            if (getenv("WF_COMPOSE_DBG")) fprintf(stderr, "compose: holder %d pose %u victim %d: base %d lacks the hold -> template owner %d victim %d\n", holder, pose, victim, bh, tb, tv);
            if (tb != bh) { bh = tb; two = 1; }
        }
    }
    if (two && bv == bh && tv == bv && !eng_pkg_mirror_ok((unsigned)bh, pose)) two = 0;   /* a re-posed self variant: not a hold
                                          (a borrowed template already stands an alt body in: keep it) */
    if (!two && bv >= 0 && bv == bh) {   /* MIRROR HOLE: template from the nearest held body */
        int alt = eng_pkg_alt_victim((unsigned)bh, pose, (unsigned)bv);
        if (alt >= 0) { tv = alt; two = 1; }
    }
    compose_slot[0] = compose_slot[1] = -1;
    tmpl_fetch++;
    tn = eng_pkg_pose((unsigned)bh, pose, flip, two ? tv : -1, &T);   /* the ROM list = the template */
    tmpl_fetch--;
    if (tn <= 0 && !two) {             /* the base never had this pose (aisle cells, a
                                          move outside his set): the holder's OWN frame
                                          when he has one (Honky's walk-out), else the
                                          CLASS template's — each in its package palette */
        const pkg_list *L = pkg_own_art(holder) ? own_list((unsigned)holder, pose, flip) : NULL;
        int cs = holder;
        if (!(L && L->n)) { cs = eng_ws_class_slot(eng_ws_body_class(bh)); L = cs >= 0 ? own_list((unsigned)cs, pose, flip) : NULL; }
        if (L && L->n) {
            static eng_pkg_rec shifted[192]; int dx, dy, n2 = L->n > 192 ? 192 : L->n;
            memcpy(shifted, L->r, sizeof(eng_pkg_rec) * (size_t)n2);
            if (cal_move >= 0 && eng_calib_sum(cs, cal_move, cal_frame, &dx, &dy)) {
                if (flip) dx = -dx;
                for (int k = 0; k < n2; k++) { shifted[k].x = (int16_t)(shifted[k].x + dx); shifted[k].y = (int16_t)(shifted[k].y + dy); }
            }
            *out = shifted; rsrc_fill(ENG_SRC_HOLDER_OWN, n2); *src = rsrc_static; compose_slot[0] = cs; return n2;
        }
    }
    if (tn <= 0 || tn > 192) return 0;
    if (two) {
        if (eng_pkg_victim_mask((unsigned)bh, pose, flip, (unsigned)tv, vmask, 192) <= 0) two = 0;
    }
    if (!two) ovl = eng_pkg_overlay((unsigned)bh, pose);
    for (int t = 0; t < tn; t++)
        side[t] = two ? (vmask[t] ? ENG_SRC_VICTIM : ENG_SRC_HOLDER)
                      : ovl && ovl[t] == ENG_OVL_WEAPON ? ENG_SRC_OVERLAY
                      : ovl && ovl[t] == ENG_OVL_ROPE ? ENG_SRC_ROPE : ENG_SRC_HOLDER;
    /* holder body */
    if (pkg_own_art(holder)) {
        const pkg_list *L = own_list((unsigned)holder, pose, flip);
        if (L && L->n) { H = (const eng_pkg_rec *)L->r; hn = L->n; hown = 1; compose_slot[0] = holder; }
        else if (!two) {               /* a SKIN lacking this pose: the CLASS GENERIC's frame
                                          (neutral body of the right size), not the base's
                                          face and outfit (user 2026-08-28: the generic IS the
                                          fallback for an unfinished skin) */
            int cs = eng_ws_class_slot(eng_ws_body_class(bh));
            const pkg_list *G = cs >= 0 && cs != holder ? own_list((unsigned)cs, pose, flip) : NULL;
            if (getenv("WF_COMPOSE_DBG")) fprintf(stderr, "compose %d pose %d: skin lacks it -> class slot %d %s (%d cells)\n", holder, pose, cs, G && G->n ? "OWN" : "none", G ? G->n : -1);
            if (G && G->n) { H = (const eng_pkg_rec *)G->r; hn = G->n; hown = 1; compose_slot[0] = cs; }
        }
    }
    if (!hown) { H = T; hn = tn; }     /* identity: the template's own holder cells */
    /* victim body: the clone's own vict2 frame, else a STOCK victim's class
       template frame (the ROM's baked victim cells stay the default for
       stock, so stock output is unchanged where they exist) */
    if (two && pkg_own_art(victim)) {
        vn = eng_pkg_vict((unsigned)victim, (unsigned)bh, pose, flip, &V);
        if (vn > 0) { vown = 1; compose_slot[1] = victim; } else vn = 0;
    }
    if (two && !vown && victim >= ENG_WS_MAX && pkg_own_art(victim) && bv >= 0) {   /* a SKIN victim lacking this
                                          held frame: the class generic's victim body (2026-08-28) */
        int cs = eng_ws_class_slot(eng_ws_body_class(bv));
        if (cs >= 0 && cs != victim) {
            vn = eng_pkg_vict((unsigned)cs, (unsigned)bh, pose, flip, &V);
            if (vn > 0) { vown = 1; compose_slot[1] = cs; } else vn = 0;
        }
    }
    if (two && !vown && bv >= 0 && victim < ENG_WS_MAX && tv != bv) {   /* stock victim, mirror hole */
        int cs = eng_ws_class_slot(eng_ws_body_class(bv));
        if (cs >= 0) {
            vn = eng_pkg_vict((unsigned)cs, (unsigned)bh, pose, flip, &V);
            if (vn > 0) { vown = 1; compose_slot[1] = cs; } else vn = 0;
        }
    }
    if (two && !vown) {
        V = T; vn = tn;
        if (tv != bv && eng_ws_body_class(tv) != eng_ws_body_class(bv) && !pkg_own_art(holder)) {
            /* (a GENERIC / skin holder doing a hold nobody in stock could put
               this victim in - "Perfect being plexed" - shows the stand-in
               body rather than nothing; the victim's class generic owes the
               frame: needs, user 2026-08-26) */
            /* mirror hole, STOCK victim, and the only stand-in is another
               body class: draw NO victim cells (his own art for this hold
               does not exist yet — recolouring another class's cells
               scrambles the shading, and showing them as-is puts his
               partner in the hold; playtest 2026-08-25). The class stock
               template will supply these frames. */
            vn = 0;
            for (int t = 0; t < tn; t++) if (side[t] == ENG_SRC_VICTIM) side[t] = 255;   /* no side: never emitted */
        }
    }
    if (hn > 192 || vn > 192) return 0;
    /* place every skin record on a template cell of its side */
    static uint8_t claims[2][192];
    memset(claims, 0, sizeof claims);
    for (int s = 0; s < 2; s++) {
        const eng_pkg_rec *R = s ? V : H; int rn = s ? vn : hn, own = s ? vown : hown;
        uint8_t want = s ? ENG_SRC_VICTIM : ENG_SRC_HOLDER;
        for (int k = 0; k < rn; k++) {
            at[s][k] = -1;
            if (!own) {                /* identity: the record IS template cell k */
                if (side[k] == want || (!s && side[k] == ENG_SRC_ROPE)) at[s][k] = (int16_t)k;
                continue;
            }
            for (int t = 0; t < tn && at[s][k] < 0; t++) if (side[t] == want && rec_same(&T[t], &R[k])) at[s][k] = (int16_t)t;
            {   /* BEST-OVERLAP depth slot (user 2026-08-26: a skin's cells are
                   cut from its own bbox, so "centre inside a stock cell" missed
                   most of them and they floated on top of everything): the
                   same-side template cell this cell overlaps most; a cell that
                   overlaps none rides that side's LAST (topmost) template cell */
                int best = 0, last = -1, bt = -1;
                for (int t = 0; t < tn; t++) {
                    int ov;
                    if (side[t] != want) continue;
                    last = t;
                    ov = rec_overlap(&T[t], &R[k]);
                    /* EXACT: a template-cut record sits on its cell's x and on
                       y + 16c for one of its chain tiles - that beats any mere
                       overlap (the flipped list may order cells differently, so
                       order alone cannot identify the cell) */
                    if (ov > 0 && R[k].x == T[t].x && (R[k].chain & 7) == 0 && R[k].y >= T[t].y && ((R[k].y - T[t].y) & 15) == 0 && (R[k].y - T[t].y) / 16 <= (T[t].chain & 7)) ov += 4096;
                    /* TIES (2026-08-26): ROM cells overlap each other at the same tile
                       position (the punching arm over the body); a skin cut BY THE
                       TEMPLATE emits its records in template order, so a tie goes to
                       the earliest template cell that still has a chain tile
                       unclaimed - the k-th record lands on the k-th template tile and
                       the interleave survives. A cell whose tiles are all claimed
                       yields to the later (upper) cell. */
                    if (ov > best || (ov == best && ov > 0 && bt >= 0 && claims[s][bt] > (T[bt].chain & 7) && claims[s][t] <= (T[t].chain & 7))) { best = ov; bt = t; }
                }
                /* EXTRA ART (2026-08-26, walking facelock): a record that is NOT an
                   exact template tile is art the mannequin never had (an AI body
                   bigger than the ROM's) - it takes the DEEPEST slot it overlaps,
                   so it hides behind the other man rather than covering his arm */
                if (bt >= 0 && best < 4096) {   /* not a template tile: behind everything of this side */
                    int deepest = -1;
                    for (int t = 0; t < tn && deepest < 0; t++) if (side[t] == want) deepest = t;
                    if (deepest >= 0) bt = deepest;
                }
                if (bt >= 0) { at[s][k] = (int16_t)bt; claims[s][bt]++; }
                else if (last >= 0) at[s][k] = (int16_t)last;
            }
        }
    }
    /* DEPTH OVERRIDE (user 2026-08-26): per (holder pose, victim class) in
       the holder's package - class layer under the skin's - when the
       template interleave is wrong for the skin's art: 1 = holder's body
       entirely over the victim, 2 = victim over. Applied after emission as a
       stable partition, so cells keep their in-body order. */
    depth_mode = two ? eng_depth_sum(holder >= ENG_WS_MAX ? holder : eng_ws_class_slot(eng_ws_body_class(bh)), pose,
                                     bv >= 0 ? (unsigned)eng_ws_body_class(bv) : 0xFFu) : 0;
    /* WEAPON cells from the weapon pak (the table), each at the template
       overlay slot it matches; table cells the template never had (a new
       weapon's art) ride the last overlay slot; no pak = template verbatim */
    static eng_pkg_rec W[64]; static uint8_t wused[64]; int wn = 0, wlast = -1;
    for (int t = 0; t < tn; t++) if (side[t] == ENG_SRC_OVERLAY) wlast = t;
    if (wlast >= 0) { wn = eng_wpn_cells(pose, holder, flip, W, 64); memset(wused, 0, sizeof wused); }
    /* emit in template order */
    for (int t = 0; t < tn && n < 256; t++) {
        if (side[t] == ENG_SRC_OVERLAY) {
            if (wn <= 0) { res[n] = T[t]; rsrc[n++] = ENG_SRC_OVERLAY; continue; }
            for (int k = 0; k < wn; k++) if (!wused[k] && rec_same(&W[k], &T[t])) { wused[k] = 1; res[n] = W[k]; rsrc[n++] = ENG_SRC_OVERLAY; break; }
            if (t == wlast) for (int k = 0; k < wn && n < 256; k++) if (!wused[k]) { wused[k] = 1; res[n] = W[k]; rsrc[n++] = ENG_SRC_OVERLAY; }
            continue;
        }
        if (side[t] == ENG_SRC_ROPE) { if (!hown) { res[n] = T[t]; rsrc[n++] = ENG_SRC_HOLDER; } continue; }
        {
            int s = side[t] == ENG_SRC_VICTIM;
            const eng_pkg_rec *R = s ? V : H; int rn = s ? vn : hn, own = s ? vown : hown;
            uint8_t code = s ? (own ? ENG_SRC_VICTIM_OWN : ENG_SRC_VICTIM) : (own ? ENG_SRC_HOLDER_OWN : ENG_SRC_HOLDER);
            for (int k = 0; k < rn && n < 256; k++) if (at[s][k] == t) { res[n] = R[k]; rsrc[n++] = code; }
        }
    }
    for (int s = 0; s < 2; s++) {      /* unmatched skin records (a side with no template cells): on top */
        const eng_pkg_rec *R = s ? V : H; int rn = s ? vn : hn, own = s ? vown : hown;
        uint8_t code = s ? ENG_SRC_VICTIM_OWN : ENG_SRC_HOLDER_OWN;
        const eng_pkg_rec *S = NULL; int sn = 0;
        if (!own) continue;
        if (s == 0 && two) {           /* the holder's SINGLE pose list: a cell the ROM left out of the
                                          composed frame (hidden behind the victim) must not surface */
            tmpl_fetch++; sn = eng_pkg_pose((unsigned)bh, pose, flip, -1, &S); tmpl_fetch--;
            if (sn > 192 || sn < 0) sn = 0;
        }
        for (int k = 0; k < rn && n < 256; k++) {
            int omitted = 0;
            if (at[s][k] >= 0) continue;
            for (int t = 0; t < sn && !omitted; t++)
                if (R[k].x == S[t].x && (R[k].chain & 7) == 0 && R[k].y >= S[t].y && ((R[k].y - S[t].y) & 15) == 0 && (R[k].y - S[t].y) / 16 <= (S[t].chain & 7)) omitted = 1;
            if (omitted) continue;
            res[n] = R[k]; rsrc[n++] = code;
        }
    }
    if (depth_mode) {                  /* stable partition: the side drawn UNDER first */
        static eng_pkg_rec res2[256]; static uint8_t rsrc2[256]; int m = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int k = 0; k < n; k++) {
                int isv = rsrc[k] == ENG_SRC_VICTIM || rsrc[k] == ENG_SRC_VICTIM_OWN;
                int under = depth_mode == 1 ? isv : !isv;   /* holder over: victim under */
                if ((pass == 0) == under) { res2[m] = res[k]; rsrc2[m++] = rsrc[k]; }
            }
        memcpy(res, res2, sizeof(eng_pkg_rec) * (size_t)m); memcpy(rsrc, rsrc2, (size_t)m);
    }
    if (vown && compose_slot[1] >= 0) {   /* VICTIM-SIDE offset: the held man's own cells */
        int dx, dy;
        unsigned krow = (unsigned)bh == (unsigned)eng_ws_base(compose_slot[1]) || eng_ws_hidden(compose_slot[1]) ? 12u : (unsigned)bh;
        if (eng_voff_sum(compose_slot[1], krow, pose, &dx, &dy) || eng_voff_sum(compose_slot[1], (unsigned)bh, pose, &dx, &dy)) {
            if (flip) dx = -dx;
            for (int k = 0; k < n; k++) if (rsrc[k] == ENG_SRC_VICTIM_OWN) { res[k].x = (int16_t)(res[k].x + dx); res[k].y = (int16_t)(res[k].y + dy); }
        }
    }
    if (cal_move >= 0 && compose_slot[0] >= 0) {   /* HOLDER-SIDE calibration: this package's own body cells */
        int dx, dy;
        if (eng_calib_sum(compose_slot[0], cal_move, cal_frame, &dx, &dy)) {
            if (flip) dx = -dx;        /* authored facing left; the mirror flips the residue too */
            for (int k = 0; k < n; k++)
                if (rsrc[k] == ENG_SRC_HOLDER_OWN || rsrc[k] == ENG_SRC_OVERLAY) { res[k].x = (int16_t)(res[k].x + dx); res[k].y = (int16_t)(res[k].y + dy); }
        }
    }
    *out = res; *src = rsrc;
    return n;
}
/* a portrait surface's own palette (continue faces 800+, title card 810), NULL = none */
const uint16_t *eng_pkg_cell_pens(unsigned id, unsigned pose)
{
    if (id >= ENG_WS_EXT_MAX) return NULL;
    if (!pkg[id].loaded) load_one(id);
    for (int k = 0; k < pkg[id].ncell; k++) if (pkg[id].cellpose[k] == pose) return pkg[id].cellpal[k];
    return NULL;
}
/* the clone's VICTIM body inside holder (row, pose)'s composed frame */
int eng_pkg_vict(unsigned id, unsigned row, unsigned pose, int flip, const eng_pkg_rec **out)
{
    unsigned want[2]; int nw = 0;
    if (id < ENG_WS_MAX || id >= ENG_WS_EXT_MAX || row >= 12 || pose >= 0x400) return 0;
    if (!pkg[id].loaded) load_one(id);
    /* held by his own base: the class template keys that as SELF (row 12);
       a per-base victmap may still carry the literal row */
    if ((int)row == eng_ws_base((int)id) || (eng_ws_hidden((int)id) && eng_ws_body_class((int)row) == eng_ws_body_class((int)id))) want[nw++] = 12;   /* own base (a class template: any member of the class) */
    want[nw++] = row;
    for (int w = 0; w < nw; w++)
        for (int k = 0; k < pkg[id].nvict; k++)
            if (pkg[id].vict[k].row == want[w] && pkg[id].vict[k].pose == pose) {
                const pkg_list *L = flip ? &pkg[id].vict[k].own_f : &pkg[id].vict[k].own;
                if (!L->n) return 0;
                *out = (const eng_pkg_rec *)L->r;
                return L->n;
            }
    return 0;
}
/* does (id, pose) carry a REAL two-man variant for partner row prow? */
int eng_pkg_has_part(unsigned id, unsigned pose, int prow)
{
    if (id >= ENG_WS_EXT_MAX || pose >= 0x400 || prow < 0 || prow >= 12) return 0;
    if (!pkg[id].loaded) load_one(id);
    if (!pkg[id].pose[pose].present) return 0;
    return pkg[id].pose[pose].part[prow].n > 0;
}
int eng_pkg_template(unsigned id, unsigned pose, int flip, const eng_pkg_rec **out)   /* the ROM list, never composed (tools) */
{
    int n;
    tmpl_fetch++; n = eng_pkg_pose(id, pose, flip, -1, out); tmpl_fetch--;
    return n;
}
int eng_pkg_pose(unsigned id, unsigned pose, int flip, int prow, const eng_pkg_rec **out)
{
    const pkg_list *L;
    if (id >= ENG_WS_EXT_MAX || pose >= 0x400 || getenv("WF_NOPKG")) return 0;
    if (!pkg[id].loaded) load_one(id);
    if (!pkg[id].pose[pose].present) {
        int b = clone_delegate(id);    /* clone without own poses: the base's */
        if (b >= 0) {
            int r = eng_pkg_pose((unsigned)b, pose, flip, prow, out);
            pkg_pose_own = 0;
            return r;
        }
        return 0;
    }
    L = flip ? &pkg[id].pose[pose].own_f : &pkg[id].pose[pose].own;
    if (prow >= 0 && prow < 12) {
        const pkg_list *P = flip ? &pkg[id].pose[pose].part_f[prow] : &pkg[id].pose[pose].part[prow];
        if (P->n) L = P;
        else if (!tmpl_fetch && pkg_own_art((int)id)) {   /* two-man pose, no partner variant in the
                                          clone pak: THE composition path builds it */
            const uint8_t *src; int n = eng_compose((int)id, pose, flip, prow, out, &src);
            if (n > 0) { pkg_pose_own = 1; return n; }
        }
    }
    if (!tmpl_fetch && pkg_own_art((int)id) && prow < 0) {   /* single pose: weapon overlay splice */
        const uint8_t *src; int n = eng_compose((int)id, pose, flip, -1, out, &src);
        if (n > 0) { pkg_pose_own = 1; return n; }
    }
    pkg_pose_own = 1;
    *out = (const eng_pkg_rec *)L->r;
    return L->n;
}
