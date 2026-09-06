/* --weapon-table DIR: the WEAPON OVERLAY TABLE (user 2026-08-26: weapons
 * are built like the wrestlers — source images -> sheet -> pak; the stock
 * set keeps its ROM cells as the packer's lossless source, the PNGs are
 * the human/editing face).
 *
 *   wfengine --weapon-table data/weapons
 *
 * The ROM draws the ring steps and the box as extra cells INSIDE the 18
 * carry/pickup/swing poses (114-131), marked by palette nibble 15 (the
 * shared weapon bank, palette row 0x0F). Every stock wrestler's pose list
 * carries the same TILES; the whole weapon sits at a per-wrestler attach
 * offset (checked here: uniform over the pose, else reported).
 * Output:
 *   weapons.json   per type: poses -> cells {tile, x, y, flipx, flipy, chain}
 *   palette.json   the weapon bank palette (16 pens, 0x0RGB words)
 *   <type>/overlay_NNNN.png   the cells alone on the 256x320 pose canvas
 *                  (origin 128,176 - the same canvas as the body frames, so
 *                  body + overlay composes by plain layering)
 * Types by the tile range the cells draw: steps 11020-11560, box
 * 23301-23320 (no other weapon exists in the ROM). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "../src/tbl.h"
#include "../src/json.h"

#define WT_CANVAS   256
#define WT_CANVAS_H 320
#define WT_ORG_X    128
#define WT_ORG_Y    176
#define WT_MAXC     64
#define WT_ROM_BODY 0x2F22u

const uint8_t *wf_video_tile_pens(unsigned t);
int wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H);

typedef struct { int tile, x, y, flipx, flipy, chain; } wt_cell;
typedef struct { int n; wt_cell c[WT_MAXC]; } wt_list;

/* the TYPE is decided by the POSE, as the ROM's move cells do (anim.c:
 * type 0 steps = pickup 0x72-0x73, swing 0x74-0x76, walk 0x7C-0x7F, stand
 * 0x7E; type 1 box = pickup 0x77-0x78, swing 0x79-0x7B, walk 0x80-0x83,
 * stand 0x82). A tile-range split was wrong: pose 0x77 draws the box with
 * tiles next to the steps'. */
static const char *wt_type_of_pose(unsigned pose)
{
    if ((pose >= 0x72 && pose <= 0x76) || (pose >= 0x7C && pose <= 0x7F)) return "steps";
    if ((pose >= 0x77 && pose <= 0x7B) || (pose >= 0x80 && pose <= 0x83)) return "box";
    return NULL;
}

static int wt_load(unsigned id, wt_list *L /* [1024] */)
{
    char path[128], err[128]; json_val *d; const json_val *poses;
    snprintf(path, sizeof path, "data/wrestlers/%02u/poses.json", id);
    d = json_parse_file(path, err, sizeof err);
    if (!d) { fprintf(stderr, "weapon-table: %s: %s\n", path, err); return -1; }
    poses = json_get(d, "poses");
    for (const json_val *p = poses ? poses->child : NULL; p; p = p->next) {
        unsigned pid = (unsigned)atoi(p->key ? p->key : "9999");
        const json_val *own = json_get(p, "own");
        if (pid >= 1024 || !own) continue;
        for (const json_val *c = own->child; c; c = c->next) {
            if (((unsigned)json_int(json_get(c, "pal"), 0) & 0x0Fu) != 15u) continue;
            if (L[pid].n >= WT_MAXC) continue;
            wt_cell *w = &L[pid].c[L[pid].n++];
            w->tile = (int)json_int(json_get(c, "tile"), 0); w->x = (int)json_int(json_get(c, "x"), 0);
            w->y = (int)json_int(json_get(c, "y"), 0); w->flipx = (int)json_int(json_get(c, "flipx"), 0);
            w->flipy = (int)json_int(json_get(c, "flipy"), 0); w->chain = (int)json_int(json_get(c, "chain"), 0);
        }
    }
    json_free(d);
    return 0;
}

static void wt_render(const wt_list *L, const uint16_t *pens, uint8_t *rgba)
{
    memset(rgba, 0, (size_t)WT_CANVAS * WT_CANVAS_H * 4);
    for (int i = 0; i < L->n; i++) {
        const wt_cell *w = &L->c[i];
        int x = w->x + WT_ORG_X, y = w->y + 0x80, xpos = x & 0x1FF, ypos;
        if (xpos > 512 - 16) xpos -= 512;
        ypos = ((256 - y) & 0x1FF) - 16;
        for (int c = 0; c <= w->chain; c++) {
            const uint8_t *t = wf_video_tile_pens((unsigned)(w->tile + c));
            int dy = w->flipy ? ypos - 16 * w->chain + 16 * c : ypos - 16 * c;
            if (!t) continue;
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int qx = w->flipx ? 15 - px : px, qy = w->flipy ? 15 - py : py;
                uint8_t pen = t[qy * 16 + qx];
                int ox = xpos + px, oy = dy + py + (WT_ORG_Y - 112);
                if (pen && ox >= 0 && ox < WT_CANVAS && oy >= 0 && oy < WT_CANVAS_H) {
                    uint8_t *d = rgba + ((size_t)oy * WT_CANVAS + ox) * 4; uint16_t v = pens[pen];
                    d[0] = (uint8_t)((v & 0x0F) * 17); d[1] = (uint8_t)(((v >> 4) & 0x0F) * 17);
                    d[2] = (uint8_t)(((v >> 8) & 0x0F) * 17); d[3] = 255;
                }
            }
        }
    }
}

int tool_weapon_table(const char *dir)
{
    static wt_list L[12][1024];
    static uint8_t rgba[WT_CANVAS * WT_CANVAS_H * 4];
    uint16_t pens[16]; char path[512]; FILE *f; int mism = 0, nposes = 0, ncells = 0;
    for (unsigned id = 0; id < 12; id++) if (wt_load(id, L[id])) return 1;
    /* the weapon bank: body-palette row 15 (blues + greys: blue crate, white
     * box) - sprite.c installs it into bank 0xF at match start */
    for (int pen = 0; pen < 16; pen++) pens[pen] = tbl_ra16(WT_ROM_BODY + 15u * 32u + (unsigned)pen * 2u);
    /* agreement: the same TILES for every wrestler; the whole weapon sits at a
     * per-wrestler ATTACH offset (the hand: +-1..2 px between most men, -8 for
     * the small body, +7 for the giant), uniform over a pose's cells */
    static int adx[12][1024], ady[12][1024];
    for (unsigned p = 0; p < 1024; p++)
        for (unsigned id = 1; id < 12; id++) {
            const wt_list *A = &L[0][p], *B = &L[id][p];
            if (A->n != B->n) { fprintf(stderr, "weapon-table: pose %u: wrestler %02u has %d weapon cells, wrestler 00 %d\n", p, id, B->n, A->n); mism++; continue; }
            if (!A->n) continue;
            adx[id][p] = B->c[0].x - A->c[0].x; ady[id][p] = B->c[0].y - A->c[0].y;
            for (int i = 0; i < A->n; i++)
                if (A->c[i].tile != B->c[i].tile || A->c[i].flipx != B->c[i].flipx || A->c[i].flipy != B->c[i].flipy || A->c[i].chain != B->c[i].chain
                    || B->c[i].x - A->c[i].x != adx[id][p] || B->c[i].y - A->c[i].y != ady[id][p]) {
                    fprintf(stderr, "weapon-table: pose %u: wrestler %02u cell %d is not wrestler 00's cell shifted uniformly\n", p, id, i); mism++; break;
                }
        }
    mkdir(dir, 0775);
    snprintf(path, sizeof path, "%s/steps", dir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/box", dir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/palette.json", dir);
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "weapon-table: cannot write %s\n", path); return 1; }
    fprintf(f, "{\"note\":\"the weapon bank: body-palette row 15 (bank 0xF at match start)\",\"rom\":\"0x%X\",\"bank\":15,\"pens\":[", WT_ROM_BODY + 15u * 32u);
    for (int pen = 0; pen < 16; pen++) fprintf(f, "%s%u", pen ? "," : "", pens[pen]);
    fprintf(f, "]}\n"); fclose(f);
    snprintf(path, sizeof path, "%s/weapons.json", dir);
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "weapon-table: cannot write %s\n", path); return 1; }
    fprintf(f, "{ \"note\": \"weapon overlay cells per pose (ROM: palette nibble 15 cells of the carry/pickup/swing poses). Cells are wrestler 00`s; `attach` = each other wrestler`s [dx, dy] shift of the whole weapon per pose (the hand position; a new wrestler takes his class base`s). Cell x/y are relative to the pose origin, the same canvas as the body frames (256x320, origin 128,176). tile = gfx-pak tile id; chain = extra tiles stacked below.\",\n  \"canvas\": [256, 320], \"origin\": [128, 176], \"palette\": \"palette.json\",\n  \"types\": {");
    {
        const char *types[2] = { "steps", "box" }; int first_t = 1;
        for (int t = 0; t < 2; t++) {
            int first_p = 1;
            fprintf(f, "%s\n    \"%s\": { \"poses\": {", first_t ? "" : ",", types[t]); first_t = 0;
            for (unsigned p = 0; p < 1024; p++) {
                wt_list sel; sel.n = 0;
                { const char *ty = wt_type_of_pose(p); if (ty && !strcmp(ty, types[t])) for (int i = 0; i < L[0][p].n; i++) sel.c[sel.n++] = L[0][p].c[i]; }
                if (!sel.n) continue;
                fprintf(f, "%s\n      \"%u\": [", first_p ? "" : ",", p); first_p = 0;
                for (int i = 0; i < sel.n; i++) {
                    const wt_cell *w = &sel.c[i];
                    fprintf(f, "%s{\"tile\":%d,\"x\":%d,\"y\":%d,\"flipx\":%d,\"flipy\":%d,\"chain\":%d}", i ? "," : "", w->tile, w->x, w->y, w->flipx, w->flipy, w->chain);
                    ncells++;
                }
                fprintf(f, "]");
                wt_render(&sel, pens, rgba);
                snprintf(path, sizeof path, "%s/%s/overlay_%04u.png", dir, types[t], p);
                if (wf_art_write_rgba_png(path, rgba, WT_CANVAS, WT_CANVAS_H)) fprintf(stderr, "weapon-table: cannot write %s\n", path);
                nposes++;
            }
            fprintf(f, "\n    } }");
        }
    }
    fprintf(f, "\n  },\n  \"attach\": {");
    {   /* per wrestler: [dx, dy] of the weapon relative to wrestler 00's placement, per pose */
        int first_w = 1;
        for (unsigned id = 1; id < 12; id++) {
            int first_p = 1;
            fprintf(f, "%s\n    \"%u\": {", first_w ? "" : ",", id); first_w = 0;
            for (unsigned p = 0; p < 1024; p++) {
                if (!L[0][p].n || (!adx[id][p] && !ady[id][p])) continue;
                fprintf(f, "%s\"%u\": [%d, %d]", first_p ? "" : ", ", p, adx[id][p], ady[id][p]); first_p = 0;
            }
            fprintf(f, "}");
        }
    }
    fprintf(f, "\n  } }\n"); fclose(f);
    {   /* a weapon pose outside the two move sets would be a third weapon nobody knew about */
        for (unsigned p = 0; p < 1024; p++)
            if (L[0][p].n && !wt_type_of_pose(p)) fprintf(stderr, "weapon-table: pose %u draws nibble-15 cells but belongs to neither weapon's moves\n", p);
    }
    fprintf(stderr, "weapon-table: %d pose overlays, %d cells, %d wrestler mismatches -> %s\n", nposes, ncells, mism, dir);
    return mism ? 1 : 0;
}
