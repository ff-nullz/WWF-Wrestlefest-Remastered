/* CLASS TEMPLATE TOOL (user 2026-08-28): the five body classes are
 * one-off TEMPLATES built outside the editor. This file is the standalone
 * driver; wfeditor only READS what it writes.
 *
 *   wfengine --class-verify C|all
 *      Reads data/generics/C/{manifest.json, needs.json, aliases.json,
 *      aliases.override.json?, frames/, gen/} and writes
 *      data/generics/C/status.json: one STATUS per id of the class
 *      universe (552 singles + the class victim ids) -
 *        own        drawn on this body in stock (kind universal/class)
 *        alias      an exact/mirror copy of another id (never drawn)
 *        generated  drawn on this body by the generator (gen/pose_NNNN.png)
 *        stand-in   another body's frame stands in (needs: borrowed) -
 *                   NOT acceptable in a finished template (user: cross-size
 *                   stand-ins give a fatter/thinner frame mid-match)
 *        needs      a mangled/absent frame (self-victim split, mirror hole)
 *        missing    nothing at all
 *      and the VERDICT: complete = no stand-in / needs / missing.
 *      Prints the table. No generation, no spend.
 *
 * Statuses come from the generic builder's own manifest (tools/
 * generic_class.c) - this tool adds no second opinion, it only folds the
 * generated frames in and states the gate. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/json.h"
#include "../src/engine.h"

enum { ST_OWN, ST_ALIAS, ST_GEN, ST_STANDIN, ST_NEEDS, ST_MISSING, ST_N };
static const char *const st_name[ST_N] = { "own", "alias", "generated", "stand-in", "needs", "missing" };

static int have(const char *fmt, int cls, unsigned id)
{
    char p[300]; struct stat st;
    snprintf(p, sizeof p, fmt, cls, id);
    return stat(p, &st) == 0;
}

int tool_class_verify(int cls)
{
    char p[300], err[128]; json_val *man, *needs, *ovr = NULL;
    static unsigned char status[2048]; static unsigned char reason[2048]; static int owner[2048];
    static int aflip[2048], adx[2048], ady[2048];   /* an alias's placement: flip + offset (lost before -> mirrored grapple frames, 2026-08-29) */
    int counts[ST_N] = {0}, universe = 0, complete;
    FILE *f;
    if (cls < 0 || cls >= ENG_BODY_CLASSES) { fprintf(stderr, "class-verify: class 0..%d\n", ENG_BODY_CLASSES - 1); return 2; }
    snprintf(p, sizeof p, "data/generics/%d/manifest.json", cls);
    man = json_parse_file(p, err, sizeof err);
    if (!man) { fprintf(stderr, "class-verify: %s: %s (run --generic-class %d data/generics/%d first)\n", p, err, cls, cls); return 1; }
    snprintf(p, sizeof p, "data/generics/%d/needs.json", cls);
    needs = json_parse_file(p, err, sizeof err);
    snprintf(p, sizeof p, "data/generics/%d/aliases.override.json", cls);
    ovr = json_parse_file(p, err, sizeof err);
    memset(status, ST_MISSING, sizeof status); memset(reason, 0, sizeof reason);
    for (int i = 0; i < 2048; i++) { owner[i] = -1; aflip[i] = 0; adx[i] = 0; ady[i] = 0; }

    /* 1. the universe: singles from "poses", victims from "victims" */
    for (const json_val *e = json_get(man, "poses") ? json_get(man, "poses")->child : NULL; e; e = e->next) {
        unsigned id = (unsigned)atoi(e->key); const char *kind = json_str(json_get(e, "kind"), "");
        if (id >= 2048) continue;
        universe++;
        if (!strcmp(kind, "universal") || !strcmp(kind, "class")) status[id] = ST_OWN;
        else { status[id] = ST_STANDIN; owner[id] = (int)json_int(json_get(e, "ref"), -1); }
    }
    for (const json_val *e = json_get(man, "victims") ? json_get(man, "victims")->child : NULL; e; e = e->next) {
        unsigned id = (unsigned)atoi(e->key);
        if (id >= 2048) continue;
        universe++;
        status[id] = have("data/generics/%d/frames/pose_%04u.png", cls, id) ? ST_OWN : ST_MISSING;
    }
    /* 2. needs override the stock view: self-victim / mirror-hole = needs;
     *    borrowed stays stand-in (the builder copies the owner's frame) */
    if (needs)
        for (const json_val *e = json_get(needs, "entries") ? json_get(needs, "entries")->child : NULL; e; e = e->next) {
            unsigned id = (unsigned)json_int(json_get(e, "id"), 9999); const char *r = json_str(json_get(e, "reason"), "");
            if (id >= 2048) continue;
            if (!strcmp(r, "borrowed") || !strcmp(r, "missing")) { status[id] = ST_STANDIN; owner[id] = (int)json_int(json_get(e, "owner"), owner[id]); }
            else status[id] = ST_NEEDS;
            reason[id] = (unsigned char)(!strcmp(r, "self-victim") ? 1 : !strcmp(r, "mirror-hole") ? 2 : 3);
        }
    /* 3. aliases (builder's map, minus rejected overrides, plus added) */
    for (const json_val *e = json_get(man, "aliases") ? json_get(man, "aliases")->child : NULL; e; e = e->next) {
        unsigned id = (unsigned)atoi(e->key); int rej = 0;
        if (id >= 2048) continue;
        if (ovr) for (const json_val *r = json_get(ovr, "reject") ? json_get(ovr, "reject")->child : NULL; r; r = r->next) if ((unsigned)json_int(r, 9999) == id) rej = 1;
        if (!rej) { status[id] = ST_ALIAS; owner[id] = (int)json_int(json_get(e, "of"), -1); aflip[id] = (int)json_int(json_get(e, "flip"), 0); adx[id] = (int)json_int(json_get(e, "dx"), 0); ady[id] = (int)json_int(json_get(e, "dy"), 0); }
    }
    if (ovr)
        for (const json_val *e = json_get(ovr, "add") ? json_get(ovr, "add")->child : NULL; e; e = e->next) {
            unsigned id = (unsigned)atoi(e->key);
            if (id < 2048) { status[id] = ST_ALIAS; owner[id] = (int)json_int(json_get(e, "of"), -1); aflip[id] = (int)json_int(json_get(e, "flip"), 0); adx[id] = (int)json_int(json_get(e, "dx"), 0); ady[id] = (int)json_int(json_get(e, "dy"), 0); }
        }
    /* 3b. NEAR MERGES (user 2026-08-28: "0171 and 0323 look identical"): the
     *     builder's aliases.json 'suggested' pairs (silhouette IoU >= 0.98) are
     *     ACCEPTED automatically when the two drawings differ in at most 2% of
     *     their bbox pixels (aligned, mirrored when the suggestion is a flip) -
     *     323 -> 171 differs in 12 px. Rejected overrides stay rejected. */
    {
        char ap[300]; json_val *al;
        snprintf(ap, sizeof ap, "data/generics/%d/aliases.json", cls);
        al = json_parse_file(ap, err, sizeof err);
        if (al)
            for (const json_val *e = json_get(al, "suggested") ? json_get(al, "suggested")->child : NULL; e; e = e->next) {
                unsigned id = (unsigned)atoi(e->key); int of = (int)json_int(json_get(e, "of"), -1), flip = (int)json_int(json_get(e, "flip"), 0), rej = 0;
                double iou = json_get(e, "iou") ? (double)json_int(json_get(e, "iou"), 0) : 0;   /* json_int floors: 1 = identical silhouette */
                extern int wf_video_load_rgba_png(const char *path, uint8_t **rgba, int *w, int *h);
                uint8_t *a = NULL, *b = NULL; int aw, ah, bw, bh;
                if (id >= 2048 || of < 0 || of >= 2048 || status[id] == ST_ALIAS || iou < 1) continue;
                if (ovr) for (const json_val *r = json_get(ovr, "reject") ? json_get(ovr, "reject")->child : NULL; r; r = r->next) if ((unsigned)json_int(r, 9999) == id) rej = 1;
                if (rej) continue;
                snprintf(p, sizeof p, "data/generics/%d/frames/pose_%04u.png", cls, id);
                if (wf_video_load_rgba_png(p, &a, &aw, &ah)) continue;
                snprintf(p, sizeof p, "data/generics/%d/frames/pose_%04d.png", cls, of);
                if (wf_video_load_rgba_png(p, &b, &bw, &bh)) { free(a); continue; }
                {
                    int ax0 = aw, ay0 = ah, ax1 = -1, ay1 = -1, bx0 = bw, by0 = bh, bx1 = -1, by1 = -1, diff = 0, area;
                    for (int y = 0; y < ah; y++) for (int x = 0; x < aw; x++) if (a[((size_t)y*aw + x)*4 + 3]) { if (x < ax0) ax0 = x; if (x > ax1) ax1 = x; if (y < ay0) ay0 = y; if (y > ay1) ay1 = y; }
                    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) if (b[((size_t)y*bw + x)*4 + 3]) { if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
                    if (ax1 >= ax0 && bx1 >= bx0 && ax1 - ax0 == bx1 - bx0 && ay1 - ay0 == by1 - by0) {
                        int cw = ax1 - ax0 + 1, ch = ay1 - ay0 + 1;
                        area = cw * ch;
                        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
                            const uint8_t *pa = a + ((size_t)(ay0 + y)*aw + ax0 + x)*4;
                            const uint8_t *pb = b + ((size_t)(by0 + y)*bw + bx0 + (flip ? cw - 1 - x : x))*4;
                            if ((pa[3] != 0) != (pb[3] != 0) || (pa[3] && memcmp(pa, pb, 3))) diff++;
                        }
                        if (diff * 50 <= area) {   /* <= 2% */
                            status[id] = ST_ALIAS; owner[id] = of; reason[id] = 9;   /* 9 = auto near-merge */
                            aflip[id] = flip; adx[id] = (int)json_int(json_get(e, "dx"), 0); ady[id] = (int)json_int(json_get(e, "dy"), 0);   /* the suggestion's placement */
                        }
                    }
                }
                free(a); free(b);
            }
        if (al) json_free(al);
    }
    /* 4. generated frames (drawn from the base ref) win over own / stand-in / missing */
    for (unsigned id = 0; id < 2048; id++)
        if (status[id] == ST_OWN || status[id] == ST_STANDIN || status[id] == ST_NEEDS || status[id] == ST_MISSING)
            if (have("data/generics/%d/gen/pose_%04u.png", cls, id)) status[id] = ST_GEN;
    /* 5. a single id with no frame at all (not even a stand-in) */
    for (unsigned id = 0; id < 1024; id++)
        if (status[id] == ST_OWN && !have("data/generics/%d/frames/pose_%04u.png", cls, id)) status[id] = ST_MISSING;

    for (unsigned id = 0; id < 2048; id++) if (status[id] != ST_MISSING || id < 1024) { }
    for (const json_val *e = json_get(man, "poses") ? json_get(man, "poses")->child : NULL; e; e = e->next) counts[status[atoi(e->key)]]++;
    for (const json_val *e = json_get(man, "victims") ? json_get(man, "victims")->child : NULL; e; e = e->next) counts[status[atoi(e->key)]]++;
    /* COMPLETE = every non-alias id drawn from the class base ref (user
       2026-08-28: the template is the GENERIC man in every frame, a stock
       member's own frame is only the pose reference to convert) */
    complete = counts[ST_OWN] == 0 && counts[ST_STANDIN] == 0 && counts[ST_NEEDS] == 0 && counts[ST_MISSING] == 0;

    snprintf(p, sizeof p, "data/generics/%d/status.json", cls);
    f = fopen(p, "w");
    if (f) {
        int first = 1;
        fprintf(f, "{\n  \"class\": %d, \"name\": \"%s\", \"universe\": %d, \"complete\": %s,\n  \"counts\": {", cls, eng_body_class_name(cls), universe, complete ? "true" : "false");
        for (int s = 0; s < ST_N; s++) fprintf(f, "%s\"%s\": %d", s ? ", " : "", st_name[s], counts[s]);
        fprintf(f, "},\n  \"note\": \"status per id: own = stock frame on this body; alias = copy of 'of'; generated = gen/pose_NNNN.png (drawn on this body); stand-in = another body's frame (owner) - to be generated; needs = mangled/absent (reason); missing = nothing. complete = no stand-in/needs/missing.\",\n  \"ids\": {");
        for (const json_val *e = json_get(man, "poses") ? json_get(man, "poses")->child : NULL; e; e = e->next) {
            unsigned id = (unsigned)atoi(e->key);
            fprintf(f, "%s\n    \"%u\": {\"status\": \"%s\"", first ? "" : ",", id, st_name[status[id]]); first = 0;
            if (status[id] == ST_ALIAS) fprintf(f, ", \"of\": %d, \"flip\": %d, \"dx\": %d, \"dy\": %d%s", owner[id], aflip[id], adx[id], ady[id], reason[id] == 9 ? ", \"auto\": true" : "");
            if (status[id] == ST_STANDIN) fprintf(f, ", \"owner\": %d", owner[id]);
            if (status[id] == ST_NEEDS) fprintf(f, ", \"reason\": \"%s\"", reason[id] == 1 ? "self-victim" : reason[id] == 2 ? "mirror-hole" : "other");
            fprintf(f, "}");
        }
        for (const json_val *e = json_get(man, "victims") ? json_get(man, "victims")->child : NULL; e; e = e->next) {
            unsigned id = (unsigned)atoi(e->key);
            fprintf(f, "%s\n    \"%u\": {\"status\": \"%s\", \"victim\": true, \"holder\": %lld, \"pose\": %lld", first ? "" : ",", id, st_name[status[id]],
                    (long long)json_int(json_get(e, "holder"), -1), (long long)json_int(json_get(e, "pose"), -1)); first = 0;
            if (status[id] == ST_ALIAS) fprintf(f, ", \"of\": %d, \"flip\": %d, \"dx\": %d, \"dy\": %d%s", owner[id], aflip[id], adx[id], ady[id], reason[id] == 9 ? ", \"auto\": true" : "");
            if (status[id] == ST_NEEDS) fprintf(f, ", \"reason\": \"%s\"", reason[id] == 1 ? "self-victim" : reason[id] == 2 ? "mirror-hole" : "other");
            fprintf(f, "}");
        }
        fprintf(f, "\n  }\n}\n");
        fclose(f);
    }
    fprintf(stderr, "class %d %-7s universe %4d | own %3d  alias %3d  generated %3d | stand-in %3d  needs %3d  missing %3d | %s\n",
            cls, eng_body_class_name(cls), universe, counts[ST_OWN], counts[ST_ALIAS], counts[ST_GEN], counts[ST_STANDIN], counts[ST_NEEDS], counts[ST_MISSING],
            complete ? "COMPLETE" : "incomplete");
    json_free(man); if (needs) json_free(needs); if (ovr) json_free(ovr);
    return complete ? 0 : 3;
}

int tool_class_verify_all(void)
{
    int rc = 0;
    for (int c = 0; c < ENG_BODY_CLASSES; c++) if (tool_class_verify(c) == 3) rc = 3;
    return rc;
}

/* ---- MOVE -> POSES (user 2026-08-28, step 1 of the Poses tab):
 *   wfengine --move-poses OUT.json [WRESTLER]
 * joins data/framecoverage.json (every animation's pose ids, universal +
 * per-wrestler rows) with data/classes/C/victmap.json (holder pose ->
 * victim id) and the move catalogue (who routes which move) into ONE table:
 * per animation, per class: the single poses and the victim ids it needs.
 * "generic" = reactions, states and the moves every stock man routes -
 * the set every skin needs; everything else is a MOVE pose, generated
 * as-needed when a move is mapped. WRESTLER (stock id) prints his required
 * set (generic + his move map) as a worked example. */
static int in_list(const int *l, int n, int v) { for (int i = 0; i < n; i++) if (l[i] == v) return 1; return 0; }
int tool_move_poses(const char *out, int example)
{
    char err[128], p[300]; json_val *fc, *cat, *vm[ENG_BODY_CLASSES] = {0};
    FILE *f; int nanim = 0, ngeneric = 0;
    static int gen_pose[ENG_BODY_CLASSES][2048]; static int gen_n[ENG_BODY_CLASSES];
    static int req[2048]; int nreq = 0, nreq_gen = 0;
    fc = json_parse_file("data/framecoverage.json", err, sizeof err);
    if (!fc) { fprintf(stderr, "move-poses: data/framecoverage.json: %s (run --frame-coverage first)\n", err); return 1; }
    cat = json_parse_file("data/movecatalog.json", err, sizeof err);
    for (int c = 0; c < ENG_BODY_CLASSES; c++) { snprintf(p, sizeof p, "data/classes/%d/victmap.json", c); vm[c] = json_parse_file(p, err, sizeof err); }
    f = fopen(out, "w");
    if (!f) { fprintf(stderr, "move-poses: cannot write %s\n", out); return 1; }
    fprintf(f, "{\n \"note\": \"per animation, per body class: the single pose ids it draws (universal + the class members' per-wrestler rows) and the victim ids (1024+V, from data/classes/C/victmap.json: holder pose in the move -> victim). generic = reactions, states and the moves every stock wrestler routes (the set every skin needs); other moves are generated as-needed when mapped. users = stock ids routing the move (data/movecatalog.json).\",\n \"animations\": [");
    for (const json_val *a = json_get(fc, "animations") ? json_get(fc, "animations")->child : NULL; a; a = a->next) {
        const char *kind = json_str(json_get(a, "kind"), ""); int id = (int)json_int(json_get(a, "id"), -1);
        const char *name = json_str(json_get(a, "name"), "");
        int users[12], nusers = 0, generic;
        static int poses[ENG_BODY_CLASSES][600]; static int np[ENG_BODY_CLASSES];
        static int vict[ENG_BODY_CLASSES][600]; static int nv[ENG_BODY_CLASSES];
        if (!strcmp(kind, "move") && cat) {   /* who routes it */
            char key[16]; snprintf(key, sizeof key, "%d", id);
            const json_val *m = json_get(json_get(cat, "moves"), key);
            for (const json_val *u = m && json_get(m, "users") ? json_get(m, "users")->child : NULL; u; u = u->next) {
                int w = (int)json_int(json_get(u, "w"), -1);
                if (w >= 0 && w < 12 && !in_list(users, nusers, w)) users[nusers++] = w;
            }
        }
        generic = strcmp(kind, "move") != 0 || nusers == 12;
        for (int c = 0; c < ENG_BODY_CLASSES; c++) {
            np[c] = nv[c] = 0;
            for (const json_val *q = json_get(a, "poses") ? json_get(a, "poses")->child : NULL; q; q = q->next) {
                int v = (int)json_int(q, -1); if (v >= 0 && np[c] < 600 && !in_list(poses[c], np[c], v)) poses[c][np[c]++] = v;
            }
            for (const json_val *pw = json_get(a, "per_wrestler_poses") ? json_get(a, "per_wrestler_poses")->child : NULL; pw; pw = pw->next) {
                int w = atoi(pw->key);
                if (w < 0 || w >= 12 || eng_ws_body_class(w) != c) continue;
                for (const json_val *q = pw->child; q; q = q->next) {
                    int v = (int)json_int(q, -1); if (v >= 0 && np[c] < 600 && !in_list(poses[c], np[c], v)) poses[c][np[c]++] = v;
                }
            }
            if (vm[c])
                for (const json_val *e = json_get(vm[c], "entries") ? json_get(vm[c], "entries")->child : NULL; e; e = e->next) {
                    int hp = (int)json_int(json_at(e, 1), -1), vid = (int)json_int(json_at(e, 2), -1);
                    if (hp >= 0 && vid >= 0 && in_list(poses[c], np[c], hp) && nv[c] < 600 && !in_list(vict[c], nv[c], 1024 + vid)) vict[c][nv[c]++] = 1024 + vid;
                }
            if (generic)
                for (int k = 0; k < np[c]; k++) if (!in_list(gen_pose[c], gen_n[c], poses[c][k])) gen_pose[c][gen_n[c]++] = poses[c][k];
            /* VICTIM frames are generic whatever the move: any opponent can do
               the move TO him (user 2026-08-28) */
            for (int k = 0; k < nv[c]; k++) if (!in_list(gen_pose[c], gen_n[c], vict[c][k])) gen_pose[c][gen_n[c]++] = vict[c][k];
        }
        if (generic) ngeneric++;
        fprintf(f, "%s\n  {\"kind\": \"%s\", \"id\": %d, \"name\": ", nanim ? "," : "", kind, id); json_write_string(f, name);
        fprintf(f, ", \"generic\": %s, \"users\": [", generic ? "true" : "false");
        for (int k = 0; k < nusers; k++) fprintf(f, "%s%d", k ? ", " : "", users[k]);
        fprintf(f, "],\n   \"poses\": {");
        for (int c = 0; c < ENG_BODY_CLASSES; c++) { fprintf(f, "%s\"%d\": [", c ? ", " : "", c); for (int k = 0; k < np[c]; k++) fprintf(f, "%s%d", k ? "," : "", poses[c][k]); fprintf(f, "]"); }
        fprintf(f, "},\n   \"victims\": {");
        for (int c = 0; c < ENG_BODY_CLASSES; c++) { fprintf(f, "%s\"%d\": [", c ? ", " : "", c); for (int k = 0; k < nv[c]; k++) fprintf(f, "%s%d", k ? "," : "", vict[c][k]); fprintf(f, "]"); }
        fprintf(f, "}}");
        nanim++;
        /* the worked example: this stock man's move map routes the move? */
        if (example >= 0 && example < 12) {
            int mine = generic || in_list(users, nusers, example), c = eng_ws_body_class(example);
            if (mine)
                for (int k = 0; k < np[c]; k++) if (!in_list(req, nreq, poses[c][k])) { req[nreq++] = poses[c][k]; if (generic) nreq_gen++; }
            for (int k = 0; k < nv[c]; k++) if (!in_list(req, nreq, vict[c][k])) req[nreq++] = vict[c][k];   /* victim of anyone's move */
        }
    }
    {   /* FRAGMENTS / no animation (2026-08-29: Hogan's lower leg under Undertaker's leg
           drop): every id of the class universe (status.json) that no animation names is
           still drawn by the engine (handler-set cells, overlay fragments) -> a synthetic
           GENERIC animation per class so a skin generates them too */
        static unsigned char named[ENG_BODY_CLASSES][2048];
        memset(named, 0, sizeof named);
        for (const json_val *a = json_get(fc, "animations") ? json_get(fc, "animations")->child : NULL; a; a = a->next) {
            for (const json_val *q = json_get(a, "poses") ? json_get(a, "poses")->child : NULL; q; q = q->next) { int v = (int)json_int(q, -1); if (v >= 0 && v < 2048) for (int c = 0; c < ENG_BODY_CLASSES; c++) named[c][v] = 1; }
            for (const json_val *pw = json_get(a, "per_wrestler_poses") ? json_get(a, "per_wrestler_poses")->child : NULL; pw; pw = pw->next) {
                int w = atoi(pw->key); if (w < 0 || w >= 12) continue;
                for (const json_val *q = pw->child; q; q = q->next) { int v = (int)json_int(q, -1); if (v >= 0 && v < 2048) named[eng_ws_body_class(w)][v] = 1; }
            }
        }
        for (int c = 0; c < ENG_BODY_CLASSES; c++) for (int k = 0; k < gen_n[c]; k++) if (gen_pose[c][k] >= 0 && gen_pose[c][k] < 2048) named[c][gen_pose[c][k]] = 1;   /* victims already generic */
        fprintf(f, ",\n  {\"kind\": \"other\", \"id\": 0, \"name\": \"fragments (no animation)\", \"generic\": true, \"users\": [],\n   \"poses\": {");
        for (int c = 0; c < ENG_BODY_CLASSES; c++) {
            char sp[300]; json_val *st; int first2 = 1;
            snprintf(sp, sizeof sp, "data/generics/%d/status.json", c);
            st = json_parse_file(sp, err, sizeof err);
            fprintf(f, "%s\"%d\": [", c ? ", " : "", c);
            for (const json_val *e = st && json_get(st, "ids") ? json_get(st, "ids")->child : NULL; e; e = e->next) {
                int id = atoi(e->key);
                if (id < 0 || id >= 1024 || named[c][id]) continue;
                fprintf(f, "%s%d", first2 ? "" : ",", id); first2 = 0;
                if (!in_list(gen_pose[c], gen_n[c], id)) gen_pose[c][gen_n[c]++] = id;
            }
            fprintf(f, "]");
            if (st) json_free(st);
        }
        fprintf(f, "},\n   \"victims\": {");
        for (int c = 0; c < ENG_BODY_CLASSES; c++) fprintf(f, "%s\"%d\": []", c ? ", " : "", c);
        fprintf(f, "}}");
        nanim++;
    }
    fprintf(f, "\n ],\n \"generic\": {");
    for (int c = 0; c < ENG_BODY_CLASSES; c++) { fprintf(f, "%s\n  \"%d\": [", c ? "," : "", c); for (int k = 0; k < gen_n[c]; k++) fprintf(f, "%s%d", k ? "," : "", gen_pose[c][k]); fprintf(f, "]"); }
    fprintf(f, "\n }\n}\n");
    fclose(f);
    fprintf(stderr, "move-poses: %d animations (%d generic) -> %s | generic set per class:", nanim, ngeneric, out);
    for (int c = 0; c < ENG_BODY_CLASSES; c++) fprintf(stderr, " c%d %d", c, gen_n[c]);
    fprintf(stderr, "\n");
    if (example >= 0 && example < 12) {
        int c = eng_ws_body_class(example);
        nreq_gen = 0;                  /* generic = the ids in the class generic set (a signature move may share them) */
        for (int k = 0; k < nreq; k++) if (in_list(gen_pose[c], gen_n[c], req[k])) nreq_gen++;
        fprintf(stderr, "move-poses: wrestler %d (class %d) requires %d ids (%d generic + %d only through his moves) of the class universe\n", example, c, nreq, nreq_gen, nreq - nreq_gen);
    }
    json_free(fc); if (cat) json_free(cat); for (int c = 0; c < ENG_BODY_CLASSES; c++) if (vm[c]) json_free(vm[c]);
    return 0;
}


/* ---- CLASS PACK (user 2026-08-28, step 3): the class GENERIC becomes the
 *   wfengine --class-pack C
 * fallback art: data/generics/C (frames/ + victims + manifest aliases) is
 * ingested as a wrestler package into data/classes/C/generic (clone_of =
 * the class representative, name @classC) - the packer prefers that dir
 * for hidden slot ENG_CLASS_SLOT0+C, and package.c draws a skin's missing
 * pose from it (skin -> generic -> base). Then pack the profile(s). */
int tool_class_pack(int cls)
{
    extern int tool_art_ingest(const char *jobdir, int slot, const char *dstdir);
    char src[128], dst[128], p[300], err[128], cmd[600]; json_val *man; int rep; FILE *f;
    if (cls < 0 || cls >= ENG_BODY_CLASSES) { fprintf(stderr, "class-pack: class 0..%d\n", ENG_BODY_CLASSES - 1); return 2; }
    snprintf(src, sizeof src, "data/generics/%d", cls);
    snprintf(p, sizeof p, "%s/manifest.json", src);
    man = json_parse_file(p, err, sizeof err);
    if (!man) { fprintf(stderr, "class-pack: %s: %s\n", p, err); return 1; }
    rep = (int)json_int(json_get(man, "representative"), -1);
    json_free(man);
    if (rep < 0 || rep >= 12) { fprintf(stderr, "class-pack: class %d has no representative\n", cls); return 1; }
    snprintf(dst, sizeof dst, "data/classes/%d/generic", cls);
    snprintf(cmd, sizeof cmd, "mkdir -p \"%s\"", dst); if (system(cmd) != 0) return 1;
    snprintf(p, sizeof p, "%s/wrestler.json", dst);
    f = fopen(p, "w");
    if (!f) { fprintf(stderr, "class-pack: cannot write %s\n", p); return 1; }
    fprintf(f, "{ \"clone_of\": %d, \"body_class\": %d, \"name\": \"@class%d\", \"note\": \"the class GENERIC packed as the hidden class slot = fallback art for an unfinished skin (--class-pack)\" }\n", rep, cls, cls);
    fclose(f);
    snprintf(cmd, sizeof cmd, "cp \"%s/palette.json\" \"%s/palette.json\" 2>/dev/null; cp \"%s/stats.json\" \"%s/stats.json\" 2>/dev/null; rm -f \"%s/out\"; ln -s gen \"%s/out\"; true", src, dst, src, dst, src, src);
    if (system(cmd) != 0) return 1;   /* out -> gen: --art-ingest takes a generated frame over frames/ */
    {   /* the ingest reads manifest.json "aliases": rewrite it from status.json
           (the reviewed / auto aliases, chains resolved to their root) so a
           reviewed duplicate is packed as a copy, not left unpacked */
        char sp[300], mp[300]; json_val *st, *mn; FILE *mf;
        static int root[2048], flp[2048], ddx[2048], ddy[2048], is_al[2048];
        tool_class_verify(cls);
        snprintf(sp, sizeof sp, "data/generics/%d/status.json", cls);
        st = json_parse_file(sp, err, sizeof err);
        snprintf(mp, sizeof mp, "%s/manifest.json", src);
        mn = json_parse_file(mp, err, sizeof err);
        if (st && mn) {
            memset(is_al, 0, sizeof is_al);
            for (const json_val *e = json_get(st, "ids") ? json_get(st, "ids")->child : NULL; e; e = e->next) {
                int id = atoi(e->key);
                if (id < 0 || id >= 2048 || strcmp(json_str(json_get(e, "status"), ""), "alias")) continue;
                is_al[id] = 1; root[id] = (int)json_int(json_get(e, "of"), -1); flp[id] = 0; ddx[id] = 0; ddy[id] = 0;
            }
            /* flip/dx/dy: the builder's map, then the override adds (both keyed by id) */
            for (int pass = 0; pass < 2; pass++) {
                const json_val *m = pass ? NULL : json_get(mn, "aliases");
                if (pass) { char op[300]; json_val *ov; snprintf(op, sizeof op, "data/generics/%d/aliases.override.json", cls); ov = json_parse_file(op, err, sizeof err); if (!ov) break; m = json_get(ov, "add");
                    for (const json_val *e = m ? m->child : NULL; e; e = e->next) { int id = atoi(e->key); if (id >= 0 && id < 2048 && is_al[id]) { root[id] = (int)json_int(json_get(e, "of"), root[id]); flp[id] = (int)json_int(json_get(e, "flip"), 0); ddx[id] = (int)json_int(json_get(e, "dx"), 0); ddy[id] = (int)json_int(json_get(e, "dy"), 0); } }
                    json_free(ov); break; }
                for (const json_val *e = m ? m->child : NULL; e; e = e->next) { int id = atoi(e->key); if (id >= 0 && id < 2048 && is_al[id]) { root[id] = (int)json_int(json_get(e, "of"), root[id]); flp[id] = (int)json_int(json_get(e, "flip"), 0); ddx[id] = (int)json_int(json_get(e, "dx"), 0); ddy[id] = (int)json_int(json_get(e, "dy"), 0); } }
            }
            for (int id = 0; id < 2048; id++) if (is_al[id])   /* chains: C of B of A -> C of A (flip xor, offsets added; a flipped hop mirrors the x offset) */
                for (int hop = 0; hop < 6 && root[id] >= 0 && root[id] < 2048 && is_al[root[id]]; hop++) {
                    int b = root[id];
                    ddx[id] = flp[b] ? -ddx[id] : ddx[id];
                    ddx[id] += ddx[b]; ddy[id] += ddy[b]; flp[id] ^= flp[b]; root[id] = root[b];
                }
            mf = fopen(mp, "w");
            if (mf) {
                int first = 1;
                fprintf(mf, "{");
                for (const json_val *k = mn->child; k; k = k->next) {
                    if (!k->key || !strcmp(k->key, "aliases")) continue;
                    fprintf(mf, "%s\n \"%s\": ", first ? "" : ",", k->key); json_write(mf, k, 1); first = 0;
                }
                fprintf(mf, "%s\n \"aliases\": {", first ? "" : ","); first = 1;
                for (int id = 0; id < 2048; id++) if (is_al[id] && root[id] >= 0) { fprintf(mf, "%s\n  \"%d\": {\"of\": %d, \"flip\": %d, \"dx\": %d, \"dy\": %d}", first ? "" : ",", id, root[id], flp[id], ddx[id], ddy[id]); first = 0; }
                fprintf(mf, "\n }\n}\n");
                fclose(mf);
                fprintf(stderr, "class-pack: manifest.json aliases <- status.json (reviewed + auto, chains resolved)\n");
            }
        }
        if (st) json_free(st); if (mn) json_free(mn);
    }
    if (tool_art_ingest(src, ENG_CLASS_SLOT0 + cls, dst)) { fprintf(stderr, "class-pack: ingest failed\n"); return 1; }
    fprintf(stderr, "class-pack: class %d generic -> %s (hidden slot %d); now pack: ./build.sh --no-bump or --pack-profile <p>\n", cls, dst, ENG_CLASS_SLOT0 + cls);
    return 0;
}

/* ---- CLASS BUILD (user 2026-08-28): draw the class's stand-in / needs /
 * missing ids ON THIS BODY.
 *   wfengine --class-build C [id,id,...]
 * Per target id the POSE reference is the frame that stands in today
 * (data/generics/C/frames = the owner's body, or needs/ for a mangled
 * one), the IDENTITY anchor is the class generic (base_ref.png), the text
 * is data/generics/C/character.txt | class<C>.txt | generic.txt. The skin
 * pipeline's single-pose path does the work (tools/art_run.c
 * tool_art_pose: placement, IoU gate, hi-res) on a scratch job
 * data/generics/C/_build; an accepted frame is copied to gen/pose_NNNN.png
 * (--class-verify counts it as 'generated'). Resumable: ids already in
 * gen/ are skipped. Provider = WF_ART_PROVIDER (codex by default). */
static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b; char buf[65536]; size_t n;
    if (!a) return -1;
    b = fopen(to, "wb"); if (!b) { fclose(a); return -1; }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a); fclose(b); return 0;
}
int tool_class_build(int cls, const char *only, const char *extra)
{
    extern int tool_art_pose(const char *dir, const char *anchor, const char *descfile, int pose);
    extern const char *wf_art_pose_extra;   /* main.c: the optional per-roll direction */
    if (extra && extra[0]) wf_art_pose_extra = extra;
    /* the pose reference is ANOTHER body (a stand-in) or a stock member: the
       figure must keep the class generic's build from the anchor, not the
       mannequin's (user 2026-08-28: "codex draws them fatter") */
    if (!getenv("WF_ART_BUILD")) setenv("WF_ART_BUILD", "anchor", 1);
    char p[300], bdir[128], anchor[128], desc[128], err[128], cmd[300]; json_val *doc;
    static int targets[2048]; int nt = 0, done = 0, failed = 0, skipped = 0;
    static unsigned char want[2048];
    if (cls < 0 || cls >= ENG_BODY_CLASSES) { fprintf(stderr, "class-build: class 0..%d\n", ENG_BODY_CLASSES - 1); return 2; }
    if (only && !strcmp(only, "all")) only = NULL;
    memset(want, only && only[0] ? 0 : 1, sizeof want);
    if (only && only[0]) { const char *c = only; while (*c) { int id = atoi(c); if (id >= 0 && id < 2048) want[id] = 1; while (*c && *c != ',') c++; if (*c == ',') c++; } }
    if (tool_class_verify(cls) == 1) return 1;     /* fresh status.json */
    snprintf(p, sizeof p, "data/generics/%d/status.json", cls);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) return 1;
    for (const json_val *e = json_get(doc, "ids") ? json_get(doc, "ids")->child : NULL; e; e = e->next) {
        int id = atoi(e->key); const char *st = json_str(json_get(e, "status"), "");
        if (id < 0 || id >= 2048 || !want[id]) continue;
        if (!strcmp(st, "own") || !strcmp(st, "stand-in") || !strcmp(st, "needs") || !strcmp(st, "missing")) targets[nt++] = id;
    }
    json_free(doc);
    snprintf(bdir, sizeof bdir, "data/generics/%d/_build", cls);
    snprintf(cmd, sizeof cmd, "mkdir -p \"%s/ref\" \"%s/out\" \"data/generics/%d/gen\"", bdir, bdir, cls);
    if (system(cmd) != 0) return 1;
    snprintf(anchor, sizeof anchor, "data/generics/%d/base_ref.png", cls);
    snprintf(desc, sizeof desc, "data/generics/%d/character.txt", cls);
    if (access(desc, R_OK) != 0) snprintf(desc, sizeof desc, "data/generics/class%d.txt", cls);
    if (access(desc, R_OK) != 0) snprintf(desc, sizeof desc, "data/generics/generic.txt");
    fprintf(stderr, "class-build: class %d (%s): %d target(s), anchor %s, text %s, provider %s, grid %s\n", cls, eng_body_class_name(cls), nt, anchor, desc, getenv("WF_ART_PROVIDER") ? getenv("WF_ART_PROVIDER") : "codex", getenv("WF_ART_GRID") ? getenv("WF_ART_GRID") : "1 (singles)");
    if (!nt) fprintf(stderr, "class-build: nothing to draw for that selection (every id is generated or an alias)\n");
    if (nt && getenv("WF_ART_GRID") && atoi(getenv("WF_ART_GRID")) >= 2) {
        /* SHEETS (user 2026-08-28: the 4x4/3x3/2x2 dropdown): every target's ref
           into the scratch job, ONE art run (tool_art_run sheets the tall
           figures at WF_ART_GRID, lying/low ones 2x2, and writes the job's
           run_state.json = the editor's progress bar), then harvest out/ */
        extern int tool_art_run(const char *dir, const char *anchor, const char *descfile);
        char cmd2[400]; int nref = 0;
        snprintf(cmd2, sizeof cmd2, "rm -rf \"%s/ref\" \"%s/out\" \"%s/out_hi\"; mkdir -p \"%s/ref\" \"%s/out\"", bdir, bdir, bdir, bdir, bdir);
        if (system(cmd2) != 0) return 1;
        for (int k = 0; k < nt; k++) {
            int id = targets[k]; char src[300], ref[300], gen[300];
            snprintf(gen, sizeof gen, "data/generics/%d/gen/pose_%04d.png", cls, id);
            if (access(gen, R_OK) == 0) { skipped++; continue; }
            snprintf(src, sizeof src, "data/generics/%d/frames/pose_%04d.png", cls, id);
            if (access(src, R_OK) != 0) snprintf(src, sizeof src, "data/generics/%d/needs/pose_%04d.png", cls, id);
            if (access(src, R_OK) != 0) { failed++; continue; }
            snprintf(ref, sizeof ref, "%s/ref/pose_%04d.png", bdir, id);
            if (copy_file(src, ref) == 0) nref++;
        }
        snprintf(cmd2, sizeof cmd2, "cp \"data/generics/%d/victmap.json\" \"%s/\" 2>/dev/null; true", cls, bdir);   /* victims place by holder */
        if (system(cmd2) != 0) return 1;
        fprintf(stderr, "class-build: %d ref(s) staged -> sheet run\n", nref);
        if (nref) tool_art_run(bdir, anchor, desc);
        for (int k = 0; k < nt; k++) {
            int id = targets[k]; char out[300], gen[300];
            snprintf(gen, sizeof gen, "data/generics/%d/gen/pose_%04d.png", cls, id);
            if (access(gen, R_OK) == 0) continue;
            snprintf(out, sizeof out, "%s/out/pose_%04d.png", bdir, id);
            if (access(out, R_OK) == 0 && copy_file(out, gen) == 0) done++; else failed++;
        }
        fprintf(stderr, "class-build: class %d (sheets): %d generated, %d not accepted, %d already in gen/\n", cls, done, failed, skipped);
        return tool_class_verify(cls) == 3 && failed ? 3 : 0;
    }
#define CB_STATE(phase) do { char sp[300]; FILE *sf; snprintf(sp, sizeof sp, "%s/run_state.json", bdir); sf = fopen(sp, "w"); \
        if (sf) { fprintf(sf, "{\"phase\":\"%s\",\"done\":%d,\"todo\":%d,\"ok\":%d,\"miss\":%d}\n", phase, done + failed + skipped, nt, done + skipped, failed); fclose(sf); } } while (0)
    CB_STATE("class-build");           /* the editor's status-bar progress (same shape as an art run's) */
    for (int k = 0; k < nt; k++) {
        int id = targets[k]; char src[300], ref[300], out[300], gen[300];
        snprintf(gen, sizeof gen, "data/generics/%d/gen/pose_%04d.png", cls, id);
        if (access(gen, R_OK) == 0) { skipped++; continue; }
        snprintf(src, sizeof src, "data/generics/%d/frames/pose_%04d.png", cls, id);
        if (access(src, R_OK) != 0) snprintf(src, sizeof src, "data/generics/%d/needs/pose_%04d.png", cls, id);
        if (access(src, R_OK) != 0) { fprintf(stderr, "class-build: pose %04d: no reference frame at all - skipped\n", id); failed++; continue; }
        snprintf(ref, sizeof ref, "%s/ref/pose_%04d.png", bdir, id);
        if (copy_file(src, ref)) { failed++; continue; }
        snprintf(out, sizeof out, "%s/out/pose_%04d.png", bdir, id);
        unlink(out);
        fprintf(stderr, "class-build: [%d/%d] pose %04d ...\n", k + 1, nt, id);
        tool_art_pose(bdir, anchor, desc, id);
        if (access(out, R_OK) == 0 && copy_file(out, gen) == 0) { done++; fprintf(stderr, "class-build: pose %04d -> %s\n", id, gen); }
        else { failed++; fprintf(stderr, "class-build: pose %04d: no accepted frame (see %s/run.log)\n", id, bdir); }
        CB_STATE("class-build");
    }
    CB_STATE("done");
    fprintf(stderr, "class-build: class %d: %d generated, %d failed, %d already in gen/\n", cls, done, failed, skipped);
    return tool_class_verify(cls) == 3 && failed ? 3 : 0;
}

/* ---- CLASS DEDUPE (user 2026-08-28: "how can we make sure we handle
 * duplicates properly?"): every non-alias frame of the class (singles AND
 * victims, data/generics/C/frames + gen) against every other, direct and
 * MIRRORED, bbox-cropped. Identical silhouette (same bbox size, IoU 1) and
 * <= 2% of pixels differing -> written to aliases.override.json "add"
 * (dx/dy = bbox offset, flip) - the certain ones. Near pairs (IoU >= 0.95 or
 * <= 10% differing) -> data/generics/C/dupes.json for the editor's
 * side-by-side review. Frames already aliased (status) are skipped.
 *   wfengine --class-dedupe C */
typedef struct { int id, x0, y0, w, h, area; uint8_t *px; int pw, ph; } dd_frame;
static int dd_load(int cls, int id, dd_frame *f)
{
    extern int wf_video_load_rgba_png(const char *path, uint8_t **rgba, int *w, int *h);
    char p[300]; int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    snprintf(p, sizeof p, "data/generics/%d/gen/pose_%04d.png", cls, id);
    if (access(p, R_OK) != 0) snprintf(p, sizeof p, "data/generics/%d/frames/pose_%04d.png", cls, id);
    if (wf_video_load_rgba_png(p, &f->px, &f->pw, &f->ph)) return -1;
    for (int y = 0; y < f->ph; y++) for (int x = 0; x < f->pw; x++) if (f->px[((size_t)y*f->pw + x)*4 + 3]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x1 < 0) { free(f->px); f->px = NULL; return -1; }
    f->id = id; f->x0 = x0; f->y0 = y0; f->w = x1 - x0 + 1; f->h = y1 - y0 + 1; f->area = f->w * f->h;
    return 0;
}
/* compare a vs b (b mirrored when flip): returns pixels differing, sets *inter/union of the silhouettes */
static int dd_cmp(const dd_frame *a, const dd_frame *b, int flip, int *inter, int *uni)
{
    static const uint8_t clear[4] = { 0, 0, 0, 0 };
    int diff = 0, W = a->w > b->w ? a->w : b->w, H = a->h > b->h ? a->h : b->h;   /* bboxes may differ by a pixel or two: outside = transparent */
    *inter = 0; *uni = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int bx = flip ? b->w - 1 - x : x;
        const uint8_t *pa = x < a->w && y < a->h ? a->px + ((size_t)(a->y0 + y)*a->pw + a->x0 + x)*4 : clear;
        const uint8_t *pb = bx >= 0 && bx < b->w && y < b->h ? b->px + ((size_t)(b->y0 + y)*b->pw + b->x0 + bx)*4 : clear;
        int oa = pa[3] != 0, ob = pb[3] != 0;
        if (oa && ob) (*inter)++;
        if (oa || ob) (*uni)++;
        if (oa != ob || (oa && memcmp(pa, pb, 3))) diff++;
    }
    return diff;
}
int tool_class_dedupe(int cls, double near_iou)
{
    char p[300], err[128]; json_val *st; static dd_frame fr[1400]; int n = 0, nauto = 0, nnear = 0;
    FILE *df, *of; json_val *ovr, *ovr0 = NULL;
    static int auto_id[600], auto_of[600], auto_flip[600], auto_dx[600], auto_dy[600];
    if (cls < 0 || cls >= ENG_BODY_CLASSES) return 2;
    if (tool_class_verify(cls) == 1) return 1;
    snprintf(p, sizeof p, "data/generics/%d/status.json", cls);
    st = json_parse_file(p, err, sizeof err);
    if (!st) return 1;
    for (const json_val *e = json_get(st, "ids") ? json_get(st, "ids")->child : NULL; e && n < 1400; e = e->next) {
        const char *s = json_str(json_get(e, "status"), "");
        if (!strcmp(s, "alias") || !strcmp(s, "missing")) continue;
        if (dd_load(cls, atoi(e->key), &fr[n]) == 0) n++;
    }
    json_free(st);
    snprintf(p, sizeof p, "data/generics/%d/aliases.override.json", cls);
    ovr0 = json_parse_file(p, err, sizeof err);
    snprintf(p, sizeof p, "data/generics/%d/dupes.json", cls);
    df = fopen(p, "w");
    if (!df) return 1;
    fprintf(df, "{\n \"class\": %d, \"near_iou\": %.2f, \"note\": \"near-duplicate pairs (aligned bbox compare, flip = mirrored): review in the editor (Classes > Poses > Duplicates) - alias or keep both. A group of 3 shows as 3 pairs: answering two settles it (aliases are never re-compared).\",\n \"pairs\": [", cls, near_iou);
    for (int i = 0; i < n; i++) for (int j = 0; j < i; j++) {   /* the higher id becomes the alias of the lower */
        dd_frame *a = &fr[i], *b = &fr[j]; int flip, inter, uni, diff; double iou, dfrac; int best_flip = -1, best_diff = 1 << 30; double best_iou = 0;
        if (abs(a->w - b->w) > 2 || abs(a->h - b->h) > 2) continue;   /* +-2 px: dd_cmp pads the smaller */
        for (flip = 0; flip < 2; flip++) {
            diff = dd_cmp(a, b, flip, &inter, &uni);
            iou = uni ? (double)inter / uni : 0;
            if (diff < best_diff) { best_diff = diff; best_flip = flip; best_iou = iou; }
        }
        dfrac = (double)best_diff / (a->area > b->area ? a->area : b->area);
        if (best_iou >= 0.99 && dfrac <= 0.02) {   /* the same pose on the same body, a few pixels apart (user: "0171 and 0323 are the same move") */
            if (nauto < 600) { int hi = a->id > b->id ? i : j, lo = hi == i ? j : i;
                auto_id[nauto] = fr[hi].id; auto_of[nauto] = fr[lo].id; auto_flip[nauto] = best_flip;
                auto_dx[nauto] = fr[hi].x0 - fr[lo].x0; auto_dy[nauto] = fr[hi].y0 - fr[lo].y0; nauto++; }
        } else if (best_iou >= near_iou || dfrac <= 0.20) {   /* default 0.85 (user 2026-08-28: "widen it so i catch as many as possible"); 1370 vs 1037 is 0.904 */
            int kept = 0;   /* pairs the user marked 'keep both' (override "keep": [[a,b],...]) */
            for (const json_val *k = ovr0 && json_get(ovr0, "keep") ? json_get(ovr0, "keep")->child : NULL; k; k = k->next) {
                int ka = (int)json_int(json_at(k, 0), -1), kb = (int)json_int(json_at(k, 1), -1);
                if ((ka == a->id && kb == b->id) || (ka == b->id && kb == a->id)) kept = 1;
            }
            if (kept) continue;
            fprintf(df, "%s\n  {\"a\": %d, \"b\": %d, \"flip\": %d, \"iou\": %.3f, \"diff_pct\": %.1f}", nnear ? "," : "", a->id > b->id ? a->id : b->id, a->id > b->id ? b->id : a->id, best_flip, best_iou, dfrac * 100);
            nnear++;
        }
    }
    fprintf(df, "\n ]\n}\n");
    fclose(df);
    /* the certain ones -> aliases.override.json "add" (kept entries preserved) */
    snprintf(p, sizeof p, "data/generics/%d/aliases.override.json", cls);
    ovr = json_parse_file(p, err, sizeof err);
    of = fopen(p, "w");
    if (of) {
        int first = 1;
        fprintf(of, "{\n \"reject\": [");
        for (const json_val *r = ovr && json_get(ovr, "reject") ? json_get(ovr, "reject")->child : NULL; r; r = r->next) { fprintf(of, "%s%lld", first ? "" : ", ", (long long)json_int(r, -1)); first = 0; }
        fprintf(of, "],\n \"keep\": ["); first = 1;
        for (const json_val *k = ovr && json_get(ovr, "keep") ? json_get(ovr, "keep")->child : NULL; k; k = k->next) { fprintf(of, "%s[%lld, %lld]", first ? "" : ", ", (long long)json_int(json_at(k, 0), -1), (long long)json_int(json_at(k, 1), -1)); first = 0; }
        fprintf(of, "],\n \"add\": {"); first = 1;
        for (const json_val *e = ovr && json_get(ovr, "add") ? json_get(ovr, "add")->child : NULL; e; e = e->next) {
            int dup = 0; for (int k = 0; k < nauto; k++) if (auto_id[k] == atoi(e->key)) dup = 1;
            if (dup) continue;
            fprintf(of, "%s\n  \"%s\": {\"of\": %lld, \"flip\": %lld, \"dx\": %lld, \"dy\": %lld}", first ? "" : ",", e->key, (long long)json_int(json_get(e, "of"), -1), (long long)json_int(json_get(e, "flip"), 0), (long long)json_int(json_get(e, "dx"), 0), (long long)json_int(json_get(e, "dy"), 0)); first = 0;
        }
        for (int k = 0; k < nauto; k++) { fprintf(of, "%s\n  \"%d\": {\"of\": %d, \"flip\": %d, \"dx\": %d, \"dy\": %d, \"auto\": true}", first ? "" : ",", auto_id[k], auto_of[k], auto_flip[k], auto_dx[k], auto_dy[k]); first = 0; }
        fprintf(of, "\n }\n}\n");
        fclose(of);
    }
    if (ovr) json_free(ovr); if (ovr0) json_free(ovr0);
    for (int i = 0; i < n; i++) free(fr[i].px);
    fprintf(stderr, "class-dedupe: class %d: %d frames compared -> %d certain duplicates aliased (override add), %d near pairs for review (dupes.json)\n", cls, n, nauto, nnear);
    return tool_class_verify(cls) == 1 ? 1 : 0;
}
