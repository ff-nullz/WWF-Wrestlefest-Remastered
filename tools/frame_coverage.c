/* --frame-coverage: MOVE x WRESTLER x FRAME coverage matrix (user 2026-08-26:
 * the wrestler/skin builder rebuild, step 1 — "do the classes have ALL the
 * moves?" answered from the data instead of assumed).
 *
 *   wfengine --frame-coverage data/framecoverage.json      (loads the ROM: a tool)
 *
 * The pose index only follows the FIRST cell record of each animation (the
 * table entry); handlers switch records at run time (the climb rows, the
 * tag's apron/inside records, phase records, per-wrestler celebration
 * tables). This tool finds every record the ROM holds and attributes it:
 *   1. scan the ROM for valid cell records {u32 handler PC, u16 mode, u16 n,
 *      n u16 durations, n u16 sprite words};
 *   2. seed the three animation tables (moves 0x12614, states 0x11478,
 *      victim reactions 0x1AFD4);
 *   3. every 32-bit immediate in the ROM that names a record (or a pointer
 *      table of records) is a reference; the referencing PC belongs to the
 *      animation whose handler code precedes it (handler PCs come from the
 *      attributed records themselves; ties on one handler go to the
 *      shallower attribution), iterated to closure. A record may belong to
 *      several animations (shared records);
 *   4. a run of exactly 12 consecutive record pointers is a PER-WRESTLER
 *      table: entry k's poses belong to wrestler k only (the seed included).
 * Then per animation the pose set is checked against data/wrestlers/NN/
 * poses.json (the exported stock art — a pose absent there has no frame),
 * and cells whose palette nibble is 15 / 14 mark weapon / rope overlays (they
 * are STILL IN the stock pose lists; the engine tags them at load).
 * Records no reference reaches are listed as orphans (false positives of
 * the scan, or animations nothing routes to). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/wf.h"
#include "../src/json.h"

#define FC_STATE_CELLS  0x11478u
#define FC_MOVE_CELLS   0x12614u
#define FC_VICTIM_CELLS 0x1AFD4u
#define FC_MAX_REC      4096
#define FC_MAX_ANIM     (0x8F + 13 + 43)
#define FC_MAX_POSE     1024
#define FC_NWS          12
#define FC_MAX_OWN      4

typedef struct {
    uint32_t addr, handler;
    unsigned mode, n;
    int nown; int owner[FC_MAX_OWN]; uint16_t ws[FC_MAX_OWN]; uint32_t ref[FC_MAX_OWN];   /* ws = per-wrestler row bitmask, 0 = universal */
    int depth;                                 /* shallowest attribution */
} fc_rec;

static fc_rec  recs[FC_MAX_REC];
static int     nrec;
static int32_t rec_at[WF_ROM_SIZE / 2];       /* rom offset/2 -> record index, -1 */

static uint16_t r16(uint32_t a) { return (uint16_t)((wf.rom[a] << 8) | wf.rom[a + 1]); }
static uint32_t r32(uint32_t a) { return ((uint32_t)r16(a) << 16) | r16(a + 2); }

static int valid_record(uint32_t a)
{
    uint32_t h; unsigned mode, n, k;
    if (a + 8 > WF_ROM_SIZE) return 0;
    h = r32(a); mode = r16(a + 4); n = r16(a + 6);
    if (h < 0x800u || h >= WF_ROM_SIZE || (h & 1u)) return 0;
    if (mode > 8u || n == 0 || n > 64u) return 0;
    if (a + 8 + 4u * n > WF_ROM_SIZE) return 0;
    for (k = 0; k < n; k++) {
        unsigned d = r16(a + 8 + 2u * k), s = r16(a + 8 + 2u * n + 2u * k);
        if (!(d <= 0x200u || d == 0xFF00u)) return 0;
        if ((s & 0x7FFFu) >= FC_MAX_POSE) return 0;
    }
    return 1;
}

static const char *state_name[13] = {
    "stand", "walk", "run", "skid", "reaction", "move", "turn", "get up",
    "corner", "climb", "perch", "tie-up", "hold" };

typedef struct { const char *kind; unsigned id; char name[96]; } fc_anim;
static fc_anim anims[FC_MAX_ANIM];
static int nanim;

static int rec_index(uint32_t a) { return (a & 1u) || a >= WF_ROM_SIZE ? -1 : rec_at[a / 2]; }

static int rec_has_owner(const fc_rec *r, int an)
{
    for (int i = 0; i < r->nown; i++) if (r->owner[i] == an) return 1;
    return 0;
}

/* returns 1 when something changed */
static int attribute(int ri, int an, int ws, uint32_t ref, int depth)
{
    fc_rec *r = &recs[ri];
    uint16_t bit = ws >= 0 ? (uint16_t)(1u << ws) : 0;
    for (int i = 0; i < r->nown; i++)
        if (r->owner[i] == an) {
            if (bit && !(r->ws[i] & bit)) { r->ws[i] |= bit; return 1; }   /* the seed learns its row(s) */
            return 0;
        }
    if (r->nown >= FC_MAX_OWN) return 0;
    r->owner[r->nown] = an; r->ws[r->nown] = bit; r->ref[r->nown] = ref; r->nown++;
    if (r->nown == 1 || depth < r->depth) r->depth = depth;
    return 1;
}

/* the attributed record whose handler PC is the greatest one <= pc within
 * 0x1000 bytes; on the same handler the shallower attribution wins (Hogan's
 * 0x79 celebration seed and a code-referenced sibling share 0x1A28E) */
static int owner_rec_of_pc(uint32_t pc)
{
    int best = -1; uint32_t bh = 0;
    for (int i = 0; i < nrec; i++) {
        if (recs[i].nown == 0) continue;
        if (recs[i].handler > pc || pc - recs[i].handler >= 0x1000u) continue;
        if (best < 0 || recs[i].handler > bh || (recs[i].handler == bh && recs[i].depth < recs[best].depth)) { bh = recs[i].handler; best = i; }
    }
    return best;
}

/* a pointer run starting at `a`: consecutive record addresses */
static int run_len(uint32_t a)
{
    int n = 0;
    if ((a & 1u) || a >= WF_ROM_SIZE - 4u) return 0;
    while (a <= WF_ROM_SIZE - 4u && rec_index(r32(a)) >= 0 && n < 64) { n++; a += 4; }
    return n;
}

int tool_frame_coverage(const char *out)
{
    static uint8_t present[FC_NWS][FC_MAX_POSE], weapon[FC_NWS][FC_MAX_POSE], rope[FC_NWS][FC_MAX_POSE];
    static int cls[FC_NWS];
    static uint8_t anim_pose[FC_MAX_ANIM][FC_MAX_POSE];        /* bit 0 = universal */
    static uint16_t anim_pose_ws[FC_MAX_ANIM][FC_MAX_POSE];    /* bit k = wrestler k's row */
    FILE *f; json_val *names; char err[128]; int changed, rounds = 0, orphans = 0;
    uint32_t a;

    for (a = 0; a < WF_ROM_SIZE / 2; a++) rec_at[a] = -1;
    /* 1. scan */
    for (a = 0x800; a + 8 <= WF_ROM_SIZE && nrec < FC_MAX_REC; a += 2) {
        if (!valid_record(a)) continue;
        recs[nrec].addr = a; recs[nrec].handler = r32(a); recs[nrec].mode = r16(a + 4);
        recs[nrec].n = r16(a + 6); recs[nrec].nown = 0; recs[nrec].depth = 0;
        rec_at[a / 2] = nrec; nrec++;
    }
    /* 2. seeds */
    names = json_parse_file("data/move-names.json", err, sizeof err);
    for (unsigned id = 0; id < 0x8Fu; id++) {
        char key[8]; const char *n = NULL; int r = rec_index(r32(FC_MOVE_CELLS + id * 4u));
        snprintf(key, sizeof key, "%u", id);
        if (names) n = json_str(json_get(names, key), NULL);
        anims[nanim].kind = "move"; anims[nanim].id = id;
        if (n) snprintf(anims[nanim].name, sizeof anims[nanim].name, "%s", n);
        else snprintf(anims[nanim].name, sizeof anims[nanim].name, "move 0x%02X", id);
        for (char *p = anims[nanim].name; *p; p++) if (*p == '"') *p = '\'';
        if (r >= 0) attribute(r, nanim, -1, 0, 0);
        nanim++;
    }
    for (unsigned s = 0; s < 13; s++) {
        int r = rec_index(r32(FC_STATE_CELLS + s * 4u));
        anims[nanim].kind = "state"; anims[nanim].id = s;
        snprintf(anims[nanim].name, sizeof anims[nanim].name, "%s", state_name[s]);
        if (r >= 0) attribute(r, nanim, -1, 0, 0);
        nanim++;
    }
    for (unsigned v = 0; v < 43; v++) {
        int r = rec_index(r32(FC_VICTIM_CELLS + v * 4u));
        anims[nanim].kind = "reaction"; anims[nanim].id = v;
        snprintf(anims[nanim].name, sizeof anims[nanim].name, "reaction 0x%02X", v);
        if (r >= 0) attribute(r, nanim, -1, 0, 0);
        nanim++;
    }
    /* 3. references to closure */
    do {
        changed = 0; rounds++;
        for (a = 0x800; a + 4 <= WF_ROM_SIZE; a += 2) {
            uint32_t v = r32(a); int r = rec_index(v), ob, len;
            if (rec_index(a) >= 0) continue;                       /* a record's own handler field */
            if (r < 0) {
                /* not a record: a POINTER TABLE of records? (climb rows 0x11F86,
                 * the per-wrestler celebration table 0x1A30C...) — code names
                 * the table start, the run belongs to that code */
                len = run_len(v);
                if (len >= 2) {
                    ob = owner_rec_of_pc(a);
                    if (ob < 0) continue;
                    for (int k = 0; k < len; k++)
                        changed |= attribute(rec_index(r32(v + 4u * k)), recs[ob].owner[0],
                                             len == FC_NWS ? k : -1, a, recs[ob].depth + 1);
                }
                continue;
            }
            {   /* a reference from inside a pointer run is handled by the run rule */
                uint32_t p = a; int in_run = 0;
                while (p >= 4 && rec_index(r32(p - 4)) >= 0) { p -= 4; in_run = 1; }
                if (in_run || run_len(a) >= 2) continue;
            }
            ob = owner_rec_of_pc(a);
            if (ob >= 0 && !rec_has_owner(&recs[r], recs[ob].owner[0]))
                changed |= attribute(r, recs[ob].owner[0], -1, a, recs[ob].depth + 1);
        }
    } while (changed && rounds < 12);
    for (int i = 0; i < nrec; i++) if (recs[i].nown == 0) orphans++;

    /* poses per animation */
    for (int i = 0; i < nrec; i++) {
        fc_rec *r = &recs[i];
        for (int o = 0; o < r->nown; o++)
            for (unsigned k = 0; k < r->n; k++) {
                unsigned s = r16(r->addr + 8 + 2u * r->n + 2u * k) & 0x7FFFu;
                {   /* DIRECT OWNER WINS (user 2026-08-28: move 0x70 showed 157
                       frames): an ownership reached through a shared
                       subroutine reference (ref != 0) is dropped when some
                       animation owns the record directly - the weapon pickup
                       0x19EAA and reaction 0x26's 0x1C04C helpers had swept
                       every throw record into themselves. Sole indirect
                       owners keep the record. */
                    int direct = 0;
                    for (int o2 = 0; o2 < r->nown; o2++) if (!r->ref[o2]) direct = 1;
                    if (r->ref[o] && direct) continue;
                }
                if (!r->ws[o]) anim_pose[r->owner[o]][s] |= 1;
                else anim_pose_ws[r->owner[o]][s] |= r->ws[o];
            }
    }

    /* the exported stock art */
    for (int w = 0; w < FC_NWS; w++) {
        char path[128]; json_val *d; const json_val *poses;
        snprintf(path, sizeof path, "data/wrestlers/%02d/poses.json", w);
        d = json_parse_file(path, err, sizeof err);
        if (!d) { fprintf(stderr, "frame-coverage: %s: %s\n", path, err); continue; }
        poses = json_get(d, "poses");
        for (const json_val *p = poses ? poses->child : NULL; p; p = p->next) {
            unsigned id = (unsigned)atoi(p->key ? p->key : "9999");
            const json_val *own = json_get(p, "own");
            if (id >= FC_MAX_POSE || !own || own->n == 0) continue;
            present[w][id] = 1;
            for (const json_val *c = own->child; c; c = c->next) {
                unsigned pal = (unsigned)json_int(json_get(c, "pal"), 0) & 0x0Fu;   /* package.c overlay tags */
                if (pal == 15u) weapon[w][id] = 1;
                if (pal == 14u) rope[w][id] = 1;
            }
        }
        json_free(d);
        { extern int eng_ws_body_class(int); cls[w] = eng_ws_body_class(w); }   /* behind_grab_class 0x18DFA */
    }

#define NEEDS(an, w, s) ((anim_pose[an][s] & 1u) || (anim_pose_ws[an][s] & (1u << (w))))

    /* write */
    f = fopen(out, "w");
    if (!f) { fprintf(stderr, "frame-coverage: cannot write %s\n", out); return 1; }
    fprintf(f, "{ \"note\": \"per animation: every ROM cell record attributed to it, the poses those records draw (universal, or per-wrestler-table rows), and per stock wrestler the poses his exported art LACKS. weapon/rope = poses whose stock cells carry bank 15/14 overlays.\",\n");
    fprintf(f, "  \"records\": %d, \"orphan_records\": %d, \"rounds\": %d,\n  \"wrestlers\": [", nrec, orphans, rounds);
    for (int w = 0; w < FC_NWS; w++) {
        int np = 0; for (unsigned s = 0; s < FC_MAX_POSE; s++) np += present[w][s];
        fprintf(f, "%s{ \"id\": %d, \"class\": %d, \"poses\": %d }", w ? ", " : "", w, cls[w], np);
    }
    fprintf(f, "],\n  \"animations\": [");
    {
        int first = 1, tot_missing_anims = 0, covered = 0;
        for (int an = 0; an < nanim; an++) {
            int nr = 0, any = 0, miss_any = 0; unsigned s;
            for (s = 0; s < FC_MAX_POSE; s++) if (anim_pose[an][s] || anim_pose_ws[an][s]) any = 1;
            fprintf(f, "%s\n  { \"kind\": \"%s\", \"id\": %u, \"name\": \"%s\", \"records\": [", first ? "" : ",", anims[an].kind, anims[an].id, anims[an].name);
            first = 0;
            for (int i = 0, k = 0; i < nrec; i++) for (int o = 0; o < recs[i].nown; o++) {
                if (recs[i].owner[o] != an) continue;
                fprintf(f, "%s{ \"addr\": \"0x%X\", \"handler\": \"0x%X\", \"mode\": %u, \"n\": %u",
                        k++ ? ", " : "", recs[i].addr, recs[i].handler, recs[i].mode, recs[i].n);
                if (recs[i].ws[o]) fprintf(f, ", \"wrestlers\": \"0x%03X\"", recs[i].ws[o]);
                if (recs[i].ref[o]) fprintf(f, ", \"ref\": \"0x%X\"", recs[i].ref[o]);
                if (recs[i].nown > 1) fprintf(f, ", \"shared\": true");
                fprintf(f, " }");
            }
            fprintf(f, "],\n    \"poses\": [");
            for (s = 0, nr = 0; s < FC_MAX_POSE; s++) if (anim_pose[an][s] & 1u) fprintf(f, "%s%u", nr++ ? ", " : "", s);
            fprintf(f, "], \"per_wrestler_poses\": {");
            for (int w = 0, kw = 0; w < FC_NWS; w++) {
                int has = 0; for (s = 0; s < FC_MAX_POSE; s++) if (anim_pose_ws[an][s] & (1u << w)) has = 1;
                if (!has) continue;
                fprintf(f, "%s\"%d\": [", kw++ ? ", " : "", w);
                for (s = 0, nr = 0; s < FC_MAX_POSE; s++) if (anim_pose_ws[an][s] & (1u << w)) fprintf(f, "%s%u", nr++ ? ", " : "", s);
                fprintf(f, "]");
            }
            fprintf(f, "},\n    \"missing\": {");
            for (int w = 0, kw = 0; w < FC_NWS; w++) {
                int m = 0;
                for (s = 0; s < FC_MAX_POSE; s++) if (NEEDS(an, w, s) && !present[w][s]) m++;
                if (!m) continue;
                miss_any = 1;
                fprintf(f, "%s\"%d\": [", kw++ ? ", " : "", w);
                for (s = 0, nr = 0; s < FC_MAX_POSE; s++)
                    if (NEEDS(an, w, s) && !present[w][s]) fprintf(f, "%s%u", nr++ ? ", " : "", s);
                fprintf(f, "]");
            }
            fprintf(f, "}, \"weapon_poses\": [");
            for (s = 0, nr = 0; s < FC_MAX_POSE; s++) {
                int wp = 0; for (int w = 0; w < FC_NWS; w++) if (NEEDS(an, w, s) && weapon[w][s]) wp = 1;
                if (wp) fprintf(f, "%s%u", nr++ ? ", " : "", s);
            }
            fprintf(f, "], \"rope_poses\": [");
            for (s = 0, nr = 0; s < FC_MAX_POSE; s++) {
                int rp = 0; for (int w = 0; w < FC_NWS; w++) if (NEEDS(an, w, s) && rope[w][s]) rp = 1;
                if (rp) fprintf(f, "%s%u", nr++ ? ", " : "", s);
            }
            fprintf(f, "] }");
            if (any && !miss_any) covered++;
            if (miss_any) tot_missing_anims++;
        }
        fprintf(f, "\n  ],\n  \"orphans\": [");
        for (int i = 0, k = 0; i < nrec; i++) {
            if (recs[i].nown) continue;
            fprintf(f, "%s{ \"addr\": \"0x%X\", \"handler\": \"0x%X\", \"mode\": %u, \"n\": %u, \"poses\": [",
                    k++ ? ", " : "", recs[i].addr, recs[i].handler, recs[i].mode, recs[i].n);
            for (unsigned q = 0; q < recs[i].n; q++)
                fprintf(f, "%s%u", q ? ", " : "", r16(recs[i].addr + 8 + 2u * recs[i].n + 2u * q) & 0x7FFFu);
            fprintf(f, "] }");
        }
        fprintf(f, "],\n  \"summary\": { \"animations_fully_covered\": %d, \"animations_with_gaps\": %d }\n}\n", covered, tot_missing_anims);
        fprintf(stderr, "frame-coverage: %d records (%d orphans, %d rounds); %d animations fully covered by all 12, %d with gaps -> %s\n",
                nrec, orphans, rounds, covered, tot_missing_anims, out);
    }
    fclose(f);
    /* stderr: the gap list */
    for (int an = 0; an < nanim; an++) {
        int line = 0;
        for (int w = 0; w < FC_NWS; w++) {
            int m = 0;
            for (unsigned s = 0; s < FC_MAX_POSE; s++) if (NEEDS(an, w, s) && !present[w][s]) m++;
            if (!m) continue;
            if (!line++) fprintf(stderr, "  %s %u %-28s lacks:", anims[an].kind, anims[an].id, anims[an].name);
            fprintf(stderr, " w%d(%d)", w, m);
        }
        if (line) fprintf(stderr, "\n");
    }
    if (names) json_free(names);
    return 0;
}
