/* Palette select — a PLUGGABLE mod feature (user 2026-08-24): a mod ships
 * alternate body palettes per wrestler as palettes/NN.json; on the select
 * screen B2 cycles a cell through them (a small tag under the cell names
 * the outfit), and the choice recolors the wrestler in the match (the
 * body-palette install in sprite.c asks here first).
 *
 *   mods/<m>/palettes/00.json:
 *     { "palettes": [ { "name": "HOLLYWOOD", "pens": [16 words] }, ... ] }
 *
 * Pens are the 12-bit xBGR words of palette.json (pen 0 stays 0).
 * No file for an id = the feature simply doesn't exist for that wrestler;
 * no mod = stock everywhere. Choices reset when the select screen opens
 * (a new game re-picks) and bind to the wrestler ID — a DUPLICATE pick's
 * second copy keeps the automatic ALT recolor (core.c register_alt).
 * WF_PALSEL="id:k,id:k" pre-sets choices (screenshot harness). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "profile.h"
#include "json.h"

#define PS_IDS   44
#define PS_MAX   8

static struct {
    int state;                 /* 0 untried, 1 loaded, -1 none */
    int n;
    char name[PS_MAX][12];
    uint16_t pens[PS_MAX][16];
} ps[PS_IDS];
static int choice[PS_IDS];

static void load_id(int id)
{
    char rel[32], path[512], err[128];
    json_val *doc; const json_val *arr;
    ps[id].state = -1;
    snprintf(rel, sizeof rel, "palettes/%02d.json", id);
    if (!wf_mod_resolve(rel, path, sizeof path)) return;
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "palsel: %s: %s\n", path, err); return; }
    arr = json_get(doc, "palettes");
    ps[id].n = 0;
    if (arr && arr->type == JSON_ARRAY)
        for (const json_val *e = arr->child; e && ps[id].n < PS_MAX; e = e->next) {
            const json_val *pens = json_get(e, "pens");
            int k = ps[id].n, i = 0;
            if (!pens || pens->type != JSON_ARRAY) continue;
            snprintf(ps[id].name[k], sizeof ps[id].name[k], "%s",
                     json_str(json_get(e, "name"), "ALT"));
            for (const json_val *v = pens->child; v && i < 16; v = v->next, i++)
                ps[id].pens[k][i] = (uint16_t)json_int(v, 0);
            if (i == 16) ps[id].n++;
        }
    if (ps[id].n > 0) {
        ps[id].state = 1;
        fprintf(stderr, "palsel: wrestler %02d: %d outfit(s) (%s)\n", id, ps[id].n, path);
    }
    json_free(doc);
}

int eng_palsel_count(int id)
{
    if (id < 0 || id >= PS_IDS) return 0;
    if (!ps[id].state) load_id(id);
    return ps[id].state > 0 ? ps[id].n : 0;
}

const char *eng_palsel_name(int id, int k)
{
    if (k <= 0 || k > eng_palsel_count(id)) return NULL;
    return ps[id].name[k - 1];
}

/* the active alternate pens for id, NULL = stock (choice 0) */
const uint16_t *eng_palsel_pens(int id)
{
    if (id < 0 || id >= PS_IDS || choice[id] <= 0) return NULL;
    if (choice[id] > eng_palsel_count(id)) return NULL;
    return ps[id].pens[choice[id] - 1];
}

int  eng_palsel_get(int id) { return (id >= 0 && id < PS_IDS) ? choice[id] : 0; }
void eng_palsel_set(int id, int k)
{
    if (id < 0 || id >= PS_IDS) return;
    if (k < 0 || k > eng_palsel_count(id)) k = 0;
    choice[id] = k;
}

/* select screen open: fresh picks, fresh outfits (then the harness) */
void eng_palsel_reset(void)
{
    memset(choice, 0, sizeof choice);
    if (getenv("WF_PALSEL")) {
        const char *p = getenv("WF_PALSEL");
        while (*p) {
            int id, k, nc = 0;
            if (sscanf(p, "%d:%d%n", &id, &k, &nc) == 2 && nc)
                { eng_palsel_set(id, k); p += nc; }
            else break;
            if (*p == ',') p++;
        }
    }
}
