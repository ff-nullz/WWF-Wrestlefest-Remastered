/* --pose-index: WHICH POSES BELONG TO WHICH ANIMATION (user 2026-08-26:
 * "group up/categorise sprites so I can look at them"). Walks the ROM's
 * cell records through the table layer — the 0x8F moves (move_cell_table
 * 0x12614), the 13 states (state table 0x11478) and the 43 victim
 * reactions (victim_cell_table 0x1AFD4) — and emits, per animation, its
 * frames in order: [pose id, flip, duration]. Pose ids are universal
 * (pose N = the same frame for every wrestler; hitboxes are per move), so
 * ONE index serves every class and skin; the Skins review browser groups
 * its grid by it.
 *
 *   wfengine --pose-index data/poseindex.json
 *
 * Cell record (anim.c): +0 u32 handler PC, +4 u16 mode (0 handler-driven,
 * 2 loop, else hold-last), +6 u16 n, +8 n u16 durations (0xFF00 = hold
 * forever), then n u16 sprite words (pose | 0x8000 flip). Move names from
 * data/move-names.json when present. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/tbl.h"
#include "../src/json.h"

#define IDX_STATE_CELLS  0x11478u
#define IDX_MOVE_CELLS   0x12614u
#define IDX_VICTIM_CELLS 0x1AFD4u

static const char *idx_state_name[13] = {
    "stand", "walk", "run", "skid", "reaction", "move", "turn", "get up",
    "corner", "climb", "perch", "tie-up", "hold" };

static void emit_cells(FILE *f, const char *kind, unsigned id, const char *name, uint32_t c, int *first)
{
    unsigned mode, n;
    if (!c) return;
    mode = tbl_ra16(c + 4); n = tbl_ra16(c + 6);
    if (n == 0 || n > 64) return;
    fprintf(f, "%s\n  { \"kind\": \"%s\", \"id\": %u, \"name\": \"%s\", \"mode\": %u, \"frames\": [", *first ? "" : ",", kind, id, name, mode);
    for (unsigned k = 0; k < n; k++) {
        unsigned dur = tbl_ra16(c + 8 + 2u * k), spr = tbl_ra16(c + 8 + 2u * n + 2u * k);
        fprintf(f, "%s[%u, %u, %u]", k ? ", " : "", spr & 0x7FFFu, spr >> 15, dur);
    }
    fprintf(f, "] }");
    *first = 0;
}

int tool_pose_index(const char *out)
{
    FILE *f = fopen(out, "w"); json_val *names; char err[128]; int first = 1;
    if (!f) { fprintf(stderr, "pose-index: cannot write %s\n", out); return 1; }
    names = json_parse_file("data/move-names.json", err, sizeof err);
    fprintf(f, "{ \"note\": \"every ROM animation and its frames in order: [pose id, flip, duration]; 0xFF00 duration = hold forever; mode 0 = handler-driven, 2 = loop. Pose ids are universal across wrestlers.\",\n  \"animations\": [");
    for (unsigned id = 0; id < 0x8Fu; id++) {
        char key[8], nm[96]; const char *n = NULL;
        snprintf(key, sizeof key, "%u", id);
        if (names) n = json_str(json_get(names, key), NULL);
        if (n) snprintf(nm, sizeof nm, "%s", n); else snprintf(nm, sizeof nm, "move 0x%02X", id);
        for (char *p = nm; *p; p++) if (*p == '"') *p = '\'';
        emit_cells(f, "move", id, nm, tbl_ra32(IDX_MOVE_CELLS + id * 4u), &first);
    }
    for (unsigned s = 0; s < 13; s++)
        emit_cells(f, "state", s, idx_state_name[s], tbl_ra32(IDX_STATE_CELLS + s * 4u), &first);
    for (unsigned r = 0; r < 43; r++) {
        char nm[32]; snprintf(nm, sizeof nm, "reaction 0x%02X", r);
        emit_cells(f, "reaction", r, nm, tbl_ra32(IDX_VICTIM_CELLS + r * 4u), &first);
    }
    fprintf(f, "\n  ] }\n");
    fclose(f);
    if (names) json_free(names);
    fprintf(stderr, "pose-index: -> %s\n", out);
    return 0;
}
