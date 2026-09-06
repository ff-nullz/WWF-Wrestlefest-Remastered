/* Body-class template inventory (docs/ai-art-pipeline.md "Body templates").
 *
 *   wfengine --class-inventory OUT.json
 *
 * The 12 stock bodies are 5 templates (ROM behind_grab_class). A class's
 * skin template must carry EVERY pose id in the game so any move works on
 * any body; this tool says, per class, where each id's reference frame
 * comes from:
 *   universal - every stock wrestler has it (any member is a ref)
 *   class     - some member of the class has it (that member is the ref)
 *   borrowed  - no member has it: the ref is another class's frame and
 *               needs a one-off AI conversion onto this body (source =
 *               a wrestler of the nearest class that has it)
 * plus the class's VICTIM body set: every (holder row, pose) whose two-man
 * frame holds the class's REPRESENTATIVE member (its first id), deduped
 * by canvas hash (identical contortions share one template id). The ROM
 * drew each member's victim body separately — same-class silhouettes are
 * near-identical (IoU 0.96) but not pixel-equal, so a class-wide dedupe
 * gains nothing and one member's set IS the template; the other members'
 * per-member counts are reported. Same-class holder == victim pairs cannot
 * be split by palette nibble; the engine splits them by HOLDER
 * SUBTRACTION (eng_pkg_victim_mask) — the few with no sibling variant to
 * subtract against are counted as unsplittable.
 *
 * Reads the packages through the engine's own pose/tile decoders only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/engine.h"

#define NPOSE   0x400u
#define NCLS    ENG_BODY_CLASSES
#define CANVAS  256

int      art_victim_canvas(unsigned row, unsigned pose, unsigned victim, uint8_t *cv);   /* art_job.c */
uint64_t art_canvas_hash(const uint8_t *cv);
double   art_split_check(unsigned row, unsigned pose, unsigned victim);   /* same-holder split vs siblings: IoU, or -1; < 0.7 = mirror variant */

/* silhouette-nearest classes to borrow from, per class (IoU table,
 * docs/ai-art-pipeline.md): closer bodies convert with fewer re-rolls */
static const int borrow_order[NCLS][NCLS - 1] = {
    { 3, 1, 4, 2 },   /* 0 medium */
    { 0, 2, 3, 4 },   /* 1 lean   */
    { 1, 0, 3, 4 },   /* 2 small  */
    { 0, 4, 1, 2 },   /* 3 heavy  */
    { 3, 0, 1, 2 },   /* 4 giant  */
};

/* nearest-class member that has `pose` (the borrow source), -1 if none */
int class_borrow_source(int cls, unsigned pose)
{
    const eng_pkg_rec *pr;
    if (cls < 0 || cls >= NCLS || pose >= NPOSE) return -1;
    for (int k = 0; k < NCLS - 1; k++)
        for (unsigned w = 0; w < ENG_WS_MAX; w++)
            if (eng_ws_body_class((int)w) == borrow_order[cls][k] && eng_pkg_pose(w, pose, 0, -1, &pr) > 0) return (int)w;
    return -1;
}
int tool_class_inventory(const char *out)
{
    static uint8_t has[ENG_WS_MAX][NPOSE];        /* own single-frame art */
    static uint8_t cv[CANVAS * CANVAS];
    static unsigned vrow[4096], vpose[4096];
    int cls_of[ENG_WS_MAX];
    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 1; }
    for (unsigned w = 0; w < ENG_WS_MAX; w++) {
        const eng_pkg_rec *pr;
        cls_of[w] = eng_ws_body_class((int)w);
        for (unsigned p = 0; p < NPOSE; p++)
            has[w][p] = eng_pkg_pose(w, p, 0, -1, &pr) > 0;
    }
    fprintf(f, "{ \"desc\": \"per body class: reference source of every pose id (universal/class/borrowed) + deduped victim-body set\",\n"
               "  \"classes\": [\n");
    for (int c = 0; c < NCLS; c++) {
        unsigned nu = 0, nc = 0, nb = 0, nv = 0, nsame = 0, nvent = 0, nsus = 0, nvm[ENG_WS_MAX] = {0};
        int first, rep;
        fprintf(f, "%s  { \"class\": %d, \"name\": \"%s\", \"members\": [", c ? ",\n" : "", c, eng_body_class_name(c));
        first = 1;
        for (unsigned w = 0; w < ENG_WS_MAX; w++)
            if (cls_of[w] == c) { fprintf(f, "%s%u", first ? "" : ", ", w); first = 0; }
        fprintf(f, "],\n    \"universal\": [");
        first = 1;
        for (unsigned p = 0; p < NPOSE; p++) {
            int all = 1;
            for (unsigned w = 0; w < ENG_WS_MAX; w++) if (!has[w][p]) { all = 0; break; }
            if (!all) continue;
            fprintf(f, "%s%u", first ? "" : ",", p); first = 0; nu++;
        }
        fprintf(f, "],\n    \"class\": {");           /* pose -> member ref */
        first = 1;
        for (unsigned p = 0; p < NPOSE; p++) {
            int all = 1, src = -1;
            for (unsigned w = 0; w < ENG_WS_MAX; w++) {
                if (!has[w][p]) all = 0;
                else if (cls_of[w] == c && src < 0) src = (int)w;
            }
            if (all || src < 0) continue;
            fprintf(f, "%s\"%u\": %d", first ? "" : ", ", p, src); first = 0; nc++;
        }
        fprintf(f, "},\n    \"borrowed\": {");        /* pose -> source wrestler of the nearest class */
        first = 1;
        for (unsigned p = 0; p < NPOSE; p++) {
            int any = 0, mine = 0, src = -1;
            for (unsigned w = 0; w < ENG_WS_MAX; w++) {
                if (!has[w][p]) continue;
                any = 1;
                if (cls_of[w] == c) mine = 1;
            }
            if (!any || mine) continue;
            for (int k = 0; k < NCLS - 1 && src < 0; k++)
                for (unsigned w = 0; w < ENG_WS_MAX; w++)
                    if (has[w][p] && cls_of[w] == borrow_order[c][k]) { src = (int)w; break; }
            fprintf(f, "%s\"%u\": %d", first ? "" : ", ", p, src); first = 0; nb++;
        }
        /* victim bodies: every (holder, pose) holding a member, deduped */
        fprintf(f, "},\n    \"victim\": [");
        nv = 0; rep = -1;
        for (unsigned v = 0; v < ENG_WS_MAX; v++) {
            static uint64_t ph[4096]; unsigned np = 0;   /* per-member pixel dedupe (the art-job count) */
            if (cls_of[v] != c) continue;
            if (rep < 0) rep = (int)v;
            for (unsigned row = 0; row < ENG_WS_MAX; row++) {
                for (unsigned p = 0; p < NPOSE; p++) {
                    uint64_t hp; unsigned k; int fresh;
                    if (!eng_pkg_has_part(row, p, (int)v) && !(row == v && eng_pkg_alt_victim(row, p, v) >= 0)) continue;   /* mirror holes count */
                    if (!art_victim_canvas(row, p, v, cv)) { nsame++; continue; }   /* unsplittable (no sibling variant) */
                    if (row == v) {                       /* holder-subtraction self-check */
                        double iou = art_split_check(row, p, v);
                        if (iou >= 0 && iou < 0.7) {
                            fprintf(stderr, "inventory: mirror variant holder %u pose %u IoU %.2f\n", row, p, iou);
                            nsus++;
                        }
                    }
                    nvent++;
                    hp = art_canvas_hash(cv);
                    for (k = 0; k < np; k++) if (ph[k] == hp) break;
                    fresh = k == np;
                    if (fresh && np < 4096) ph[np++] = hp;
                    if ((int)v != rep || !fresh) continue;      /* template list: the representative's uniques */
                    if (nv < 4096) { vrow[nv] = row; vpose[nv] = p; nv++; }
                }
            }
            nvm[v] = np;
        }
        for (unsigned k = 0; k < nv; k++)             /* [holder row, holder pose] of the representative */
            fprintf(f, "%s[%u,%u]", k ? "," : "", vrow[k], vpose[k]);
        fprintf(f, "],\n    \"victim_representative\": %d,\n    \"victim_unique_by_member\": {", rep);
        first = 1;
        for (unsigned v = 0; v < ENG_WS_MAX; v++)
            if (cls_of[v] == c) { fprintf(f, "%s\"%u\": %u", first ? "" : ", ", v, nvm[v]); first = 0; }
        fprintf(f, "},\n    \"counts\": { \"universal\": %u, \"class\": %u, \"borrowed\": %u, \"template\": %u,"
                   " \"victim_template\": %u, \"victim_entries\": %u, \"victim_unsplittable\": %u, \"victim_mirror_variants\": %u } }",
                nu, nc, nb, nu + nc + nb, nv, nvent, nsame, nsus);
        fprintf(stderr, "class %d %-6s: universal %u  class %u  borrowed %u  = %u single ids;  victim template %u (member %d) of %u entries (%u unsplittable, %u mirror variants)\n",
                c, eng_body_class_name(c), nu, nc, nb, nu + nc + nb, nv, rep, nvent, nsame, nsus);
    }
    fprintf(f, "\n] }\n");
    fclose(f);
    return 0;
}
