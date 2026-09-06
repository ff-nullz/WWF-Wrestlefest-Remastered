/* --skin-fidelity SKIN|SLOT (user 2026-08-27: "we should have a fidelity
 * test"): for every registered slot wearing the skin (wrestler.json "skin"
 * through the profile's layers), which frames the GAME still draws from the
 * BASE wrestler's art:
 *   body   - single poses the base's ROM package has that the slot's own
 *            art lacks (aliases resolved at ingest count as own);
 *   held   - victim frames: every (stock holder, hold pose) the base is a
 *            victim of, where the slot has no vict2 frame -> the template's
 *            victim cells = the base's body;
 *   cards  - continue faces 800-805 and the title card 810.
 * Counts + id lists on stderr (the editor log) and jobs/<skin>/fidelity.json. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/engine.h"
#include "../src/profile.h"
#include "../src/json.h"

static int slot_skin(int slot, char *out, size_t n)
{
    char rel[64], path[600], err[128]; json_val *d; const char *s;
    snprintf(rel, sizeof rel, "wrestlers/%02d/wrestler.json", slot);
    if (!wf_mod_resolve(rel, path, sizeof path)) { snprintf(path, sizeof path, "roster/wrestlers/%02d/wrestler.json", slot); }
    d = json_parse_file(path, err, sizeof err);
    if (!d) return 0;
    s = json_str(json_get(d, "skin"), "");
    snprintf(out, n, "%s", s);
    json_free(d);
    return out[0] != 0;
}
static void put_list(FILE *f, FILE *j, const int *ids, int n)
{
    int first = 1;
    for (int i = 0; i < n; i++) {
        int k = i;                                  /* compact runs a-b */
        while (k + 1 < n && ids[k + 1] == ids[k] + 1) k++;
        if (k > i + 1) fprintf(f, "%s%d-%d", first ? "" : " ", ids[i], ids[k]); else for (int q = i; q <= k; q++) fprintf(f, "%s%d", first && q == i ? "" : " ", ids[q]);
        first = 0; i = k;
    }
    if (j) { for (int i = 0; i < n; i++) fprintf(j, "%s%d", i ? ", " : "", ids[i]); }
}
static int fidelity_slot(int slot, const char *skin, FILE *j)
{
    static int body_miss[1024], card_miss[16], held_miss[4096];
    int base = eng_ws_base(slot), nb = 0, nbm = 0, nc = 0, ncm = 0, nh = 0, nhm = 0;
    const eng_pkg_rec *r;
    if (base < 0 || base >= ENG_WS_MAX) return 1;
    for (unsigned p = 0; p < 768; p++) {
        if (eng_pkg_template((unsigned)base, p, 0, &r) <= 0) continue;   /* the base never draws it */
        nb++;
        if (!eng_pkg_own_frame((unsigned)slot, p)) body_miss[nbm++] = (int)p;
    }
    for (unsigned p = 800; p <= 810; p++) {
        if (p > 805 && p < 810) continue;
        nc++;
        if (!eng_pkg_own_frame((unsigned)slot, p)) card_miss[ncm++] = (int)p;
    }
    for (unsigned h = 0; h < ENG_WS_MAX; h++)
        for (unsigned p = 0; p < 768; p++) {
            if (!eng_pkg_has_part(h, p, base)) continue;                  /* the base is held by h in pose p */
            nh++;
            if (eng_pkg_vict((unsigned)slot, h, p, 0, &r) <= 0 && nhm < 4096) held_miss[nhm++] = (int)(h * 1000 + p);
        }
    fprintf(stderr, "fidelity: slot %d '%s' (skin '%s', base %d): body %d/%d own, held %d/%d own, cards %d/%d own -> %d frame(s) still draw the base\n",
            slot, eng_ws_clone_name(slot) ? eng_ws_clone_name(slot) : "?", skin, base, nb - nbm, nb, nh - nhm, nh, nc - ncm, nc, nbm + nhm + ncm);
    if (j) fprintf(j, "  { \"slot\": %d, \"base\": %d, \"body_needed\": %d, \"held_needed\": %d, \"cards_needed\": %d,\n    \"body_base\": [", slot, base, nb, nh, nc);
    if (nbm) { fprintf(stderr, "fidelity:   body poses from the base (%d): ", nbm); put_list(stderr, j, body_miss, nbm); fprintf(stderr, "\n"); }
    if (j) fprintf(j, "],\n    \"cards_base\": [");
    if (ncm) { fprintf(stderr, "fidelity:   cards from the base (%d): ", ncm); put_list(stderr, j, card_miss, ncm); fprintf(stderr, "\n"); }
    if (j) fprintf(j, "],\n    \"held_base\": [");
    if (nhm) {
        int last_h = -1;
        fprintf(stderr, "fidelity:   held frames from the base (%d) as holder:pose:", nhm);
        for (int i = 0; i < nhm; i++) {
            int h = held_miss[i] / 1000, p = held_miss[i] % 1000;
            if (h != last_h) { fprintf(stderr, "\nfidelity:     holder %02d:", h); last_h = h; }
            fprintf(stderr, " %d", p);
            if (j) fprintf(j, "%s[%d, %d]", i ? ", " : "", h, p);
        }
        fprintf(stderr, "\n");
    }
    if (j) fprintf(j, "] }");
    return 0;
}
int tool_skin_fidelity(const char *arg)
{
    char skin[64], jp[300]; FILE *j = NULL; int n = 0, first = 1;
    int only = (arg[0] >= '0' && arg[0] <= '9') ? atoi(arg) : -1;
    if (only < 0) { snprintf(jp, sizeof jp, "jobs/%s/fidelity.json", arg); j = fopen(jp, "w"); if (j) fprintf(j, "{ \"skin\": \"%s\", \"slots\": [\n", arg); }
    for (int slot = ENG_WS_MAX; slot < ENG_WS_EXT_MAX; slot++) {
        if (eng_ws_clone_base(slot) < 0) continue;
        if (only >= 0) { if (slot != only) continue; skin[0] = 0; slot_skin(slot, skin, sizeof skin); }
        else if (!slot_skin(slot, skin, sizeof skin) || strcmp(skin, arg)) continue;
        if (j && !first) fprintf(j, ",\n");
        fidelity_slot(slot, skin, j); n++; first = 0;
    }
    if (j) { fprintf(j, "\n] }\n"); fclose(j); }
    if (!n) fprintf(stderr, "fidelity: no registered slot wears '%s' (wrestler.json \"skin\") in this profile\n", arg);
    else if (j) fprintf(stderr, "fidelity: -> %s\n", jp);
    return n ? 0 : 1;
}
