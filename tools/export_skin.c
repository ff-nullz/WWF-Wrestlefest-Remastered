/* STOCK AS SKIN 0 — export a stock wrestler in the skin-package layout
 * and prove the decomposition is lossless.
 *
 *   wfengine --export-skin BASE DIR
 *      DIR/skin.json                 {class, base, name}
 *      DIR/palette.json              the 16 pens
 *      DIR/frames/pose_NNNN.png      BODY-ONLY single frames (no baked
 *                                    weapon/rope cells), 256x256 RGBA,
 *                                    origin (128,176)
 *      DIR/victims/vict_NNNN.png     this wrestler as VICTIM, one frame per
 *                                    class victim id, drawn in HOLDER
 *                                    coordinates (same origin)
 *      data/classes/C/victmap.json   (holder row, holder pose) -> victim id
 *                                    for the whole class (rewritten, ids
 *                                    deterministic: representative first)
 *
 *   wfengine --class-template C DIR
 *      the GENERATOR's job directory for body class C — what a new skin
 *      of that class must fill:
 *      DIR/manifest.json     class, members, representative, every pose id
 *                            with its ref source (universal / class /
 *                            borrowed:<wrestler> = needs a one-off conversion
 *                            onto this body), victim ids with their holds
 *      DIR/ref/pose_NNNN.png body-only mannequins for ALL 552 single ids
 *                            (borrowed ones from the nearest class's member)
 *      DIR/ref/pose_1024+V.png  victim mannequins, one per class victim id
 *      DIR/victmap.json      art-ingest format: [1024+V, holder row, pose]
 *                            (row 12 = the skin's own base holds him)
 *      DIR/out/              the skin's frames go here (same names), then
 *                            --art-ingest DIR SLOT DST as for any job
 *      DIR/prompt.txt        template (kept if present)
 *
 *   wfengine --verify-skin BASE DIR
 *      recomposes every two-man frame (holder body frame + victim frame,
 *      cells ordered by the stock composed list — the ROM interleaves the
 *      two bodies: hvh in 2647 frames, vhvh in 959...) and compares it
 *      pixel for pixel with the baked ROM frame. 0 mismatches = the
 *      stock art round-trips through the skin layout.
 *
 * Decomposition rules (all verified 2026-08-25, docs/ai-art-pipeline.md):
 *  - a two-man frame's holder cells == the holder's own single pose;
 *  - victim cells: palette nibble, or holder subtraction for same-base;
 *  - weapon cells = bank 15, rope cells = bank 14 (dropped from skins);
 *  - victim drawings dedupe by canvas hash; mirror variants kept. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
int tool_class_template(int cls, const char *dir);
#include "../src/engine.h"
#include "../src/json.h"

#define CANVAS 256                 /* width / row stride */
#define CANVAS_H 320               /* height: 144 px below the feet line (2026-08-26) */
#define ORG_X  128
#define ORG_Y  176
#define MAXV   4096

int      art_body_canvas(unsigned base, unsigned pose, uint8_t *cv);
int      art_aisle_canvas(unsigned base, unsigned cell, uint8_t *cv);
int      art_row_cell_rgba(unsigned row, unsigned cell, uint8_t *rgba);
int      art_victim_canvas(unsigned row, unsigned pose, unsigned victim, uint8_t *cv);
uint64_t art_canvas_hash(const uint8_t *cv);
int      wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H);
int      wf_video_load_rgba_png(const char *path, uint8_t **out, int *w, int *h);
const uint8_t *wf_video_tile_pens(unsigned t);

/* class victim map: key (holder row or SELF, holder pose) -> victim id.
 * Built over every member (representative first) so ids are stable. */
#define SELF 12u
typedef struct { uint8_t row; uint16_t pose; uint16_t vid; } vm_ent;
static vm_ent  vm[MAXV * 3]; static int vmn;
static uint64_t vh[MAXV]; static int nvid;
static uint8_t  vcv[CANVAS * CANVAS_H];
/* WINDOW: the square generator sees rows [win_y, win_y+256) of the 256x320
 * canvas; 0 unless the figure reaches below row 256, then just enough */
static int ct_win_y(const uint8_t *rgba)
{
    int y1 = -1;
    for (int y = 0; y < CANVAS_H; y++) for (int x = 0; x < CANVAS; x++) if (rgba[((size_t)y * CANVAS + x) * 4 + 3]) y1 = y;
    if (y1 < 256) return 0;
    return y1 - 255 > CANVAS_H - 256 ? CANVAS_H - 256 : y1 - 255;
}


static int vm_find(unsigned row, unsigned pose)
{
    for (int i = 0; i < vmn; i++) if (vm[i].row == row && vm[i].pose == pose) return vm[i].vid;
    return -1;
}
static int build_victmap(int cls)
{
    vmn = 0; nvid = 0;
    for (unsigned v = 0; v < ENG_WS_MAX; v++) {
        if (eng_ws_body_class((int)v) != cls) continue;
        for (unsigned row = 0; row < ENG_WS_MAX; row++)
            for (unsigned p = 0; p < 0x400; p++) {
                unsigned key = row == v ? SELF : row; uint64_t h; int k;
                if (!eng_pkg_has_part(row, p, (int)v) && !(row == v && eng_pkg_alt_victim(row, p, v) >= 0)) continue;   /* mirror holes included */
                if (vm_find(key, p) >= 0) continue;          /* an earlier member named it */
                if (!art_victim_canvas(row, p, v, vcv)) continue;
                h = art_canvas_hash(vcv);
                for (k = 0; k < nvid; k++) if (vh[k] == h) break;
                if (k == nvid) { if (nvid >= MAXV) continue; vh[nvid++] = h; }
                if (vmn < MAXV * 3) { vm[vmn].row = (uint8_t)key; vm[vmn].pose = (uint16_t)p; vm[vmn].vid = (uint16_t)k; vmn++; }
            }
    }
    return nvid;
}
static void write_victmap(int cls)
{
    char path[256]; FILE *f;
    snprintf(path, sizeof path, "data/classes"); mkdir(path, 0775);
    snprintf(path, sizeof path, "data/classes/%d", cls); mkdir(path, 0775);
    snprintf(path, sizeof path, "data/classes/%d/victmap.json", cls);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{ \"class\": %d, \"note\": \"[holder row (12 = the victim's own base), holder pose, victim id]; victim frames live in a skin's victims/vict_NNNN.png in holder coordinates\",\n  \"count\": %d,\n  \"entries\": [", cls, nvid);
    for (int i = 0; i < vmn; i++) fprintf(f, "%s\n  [%u, %u, %u]", i ? "," : "", vm[i].row, vm[i].pose, vm[i].vid);
    fprintf(f, "\n] }\n");
    fclose(f);
}

static void pens_to_rgba(const uint8_t *cv, const uint16_t *pens, uint8_t *rgba)
{
    for (int i = 0; i < CANVAS * CANVAS_H; i++) {
        uint16_t w = cv[i] ? pens[cv[i]] : 0;
        rgba[i * 4 + 0] = (uint8_t)((w & 0x0F) * 17);
        rgba[i * 4 + 1] = (uint8_t)(((w >> 4) & 0x0F) * 17);
        rgba[i * 4 + 2] = (uint8_t)(((w >> 8) & 0x0F) * 17);
        rgba[i * 4 + 3] = cv[i] ? 255 : 0;
    }
}

int tool_export_skin(int base, const char *dir)
{
    static uint8_t cv[CANVAS * CANVAS_H], rgba[CANVAS * CANVAS_H * 4];
    const uint16_t *pens = eng_pkg_palette((unsigned)base);
    char path[600]; FILE *f; int cls, nf = 0, nv = 0;
    if (base < 0 || base >= ENG_WS_MAX || !pens) { fprintf(stderr, "export-skin: base 0..11\n"); return 1; }
    cls = eng_ws_body_class(base);
    mkdir(dir, 0775);
    snprintf(path, sizeof path, "%s/frames", dir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/victims", dir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/skin.json", dir);
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f, "{ \"class\": %d, \"base\": %d, \"stock\": true, \"name\": \"stock %02d\", \"canvas\": %d, \"origin\": [%d, %d] }\n", cls, base, base, CANVAS, ORG_X, ORG_Y);
    fclose(f);
    snprintf(path, sizeof path, "%s/palette.json", dir);
    f = fopen(path, "w");
    if (f) { fprintf(f, "{\"pens\":["); for (int k = 0; k < 16; k++) fprintf(f, "%s%u", k ? "," : "", pens[k]); fprintf(f, "]}\n"); fclose(f); }
    /* body-only single frames */
    for (unsigned p = 0; p < 0x400; p++) {
        if (!art_body_canvas((unsigned)base, p, cv)) continue;
        pens_to_rgba(cv, pens, rgba);
        snprintf(path, sizeof path, "%s/frames/pose_%04u.png", dir, p);
        if (wf_art_write_rgba_png(path, rgba, CANVAS, CANVAS_H) == 0) nf++;
    }
    /* victim frames by class victim id */
    build_victmap(cls);
    write_victmap(cls);
    {
        static uint8_t done[MAXV];
        memset(done, 0, sizeof done);
        for (int i = 0; i < vmn; i++) {
            unsigned row = vm[i].row == SELF ? (unsigned)base : vm[i].row;
            if (done[vm[i].vid]) continue;
            if (!art_victim_canvas(row, vm[i].pose, (unsigned)base, cv)) continue;
            done[vm[i].vid] = 1;
            pens_to_rgba(cv, pens, rgba);
            snprintf(path, sizeof path, "%s/victims/vict_%04u.png", dir, vm[i].vid);
            if (wf_art_write_rgba_png(path, rgba, CANVAS, CANVAS_H) == 0) nv++;
            snprintf(path, sizeof path, "%s/frames/pose_%04u.png", dir, 1024u + vm[i].vid);   /* ingest layout: victim ids 1024+ */
            wf_art_write_rgba_png(path, rgba, CANVAS, CANVAS_H);
        }
    }
    {   /* victmap.json in the INGEST format: [1024+vid, holder row (12 = own base), holder pose] */
        snprintf(path, sizeof path, "%s/victmap.json", dir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "{ \"class\": %d, \"base\": %d, \"first\": 1024, \"note\": \"[victim id, holder row (12 = own base), holder pose]\",\n  \"entries\": [", cls, base);
            for (int i = 0; i < vmn; i++) fprintf(f, "%s\n  [%u, %u, %u]", i ? "," : "", 1024u + vm[i].vid, vm[i].row, vm[i].pose);
            fprintf(f, "\n] }\n");
            fclose(f);
        }
    }
    fprintf(stderr, "export-skin: base %d (class %d) -> %s: %d body frames, %d victim frames of %d class victim ids (victmap %d entries)\n",
            base, cls, dir, nf, nv, nvid, vmn);
    return 0;
}

/* ---- the class template job dir ---- */
int class_borrow_source(int cls, unsigned pose);   /* class_inventory.c: nearest-class member with the pose, -1 */
/* alias detection: the drawing INSIDE the bbox (position-independent) and
 * its horizontal mirror; the alias records the shift (dx, dy) between the
 * two bboxes so ingest can place the derived frame exactly — a shifted
 * copy or an offset mirror is still the same drawing */
#define CT_IDS 0x800               /* body poses 0..0x3FF and victim bodies 1024+ */
#define CT_W   5                   /* packed mask words per row: 256 px + shift slack */
static uint64_t ct_hash[CT_IDS], ct_mhash[CT_IDS]; static uint8_t ct_alias[CT_IDS];
static int ct_card_alias[8];       /* continue-face cell k (4..6) is a copy of k-4 */
/* pieces + bbox of an RGBA canvas (8-connected opaque components) */
static int ct_pieces(const uint8_t *rgba, int W, int H, int *bw, int *bh)   /* W x H <= CANVAS x CANVAS_H (the old 256-row canvas too) */
{
    static int lab[CANVAS * CANVAS_H]; static int stack[CANVAS * CANVAS_H];
    int n = 0, x0 = W, y0 = H, x1 = -1, y1 = -1;
    memset(lab, 0, sizeof(int) * (size_t)(W * H));
    for (int i = 0; i < W * H; i++) {
        int sp = 0;
        if (!rgba[(size_t)i * 4 + 3]) continue;
        { int x = i % W, y = i / W; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        if (lab[i]) continue;
        n++; lab[i] = n; stack[sp++] = i;
        while (sp) {
            int q = stack[--sp], x = q % W, y = q / W;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy, r;
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                r = ny * W + nx;
                if (!rgba[(size_t)r * 4 + 3] || lab[r]) continue;
                lab[r] = n; stack[sp++] = r;
            }
        }
    }
    *bw = x1 >= x0 ? x1 - x0 + 1 : 0; *bh = y1 >= y0 ? y1 - y0 + 1 : 0;
    return n;
}
/* a CONVERTED class-template frame is accepted as a reference only when it
 * agrees with the OWNER's real frame: no more pieces than the owner has, and
 * a bounding box within 0.7x-1.4x of the owner's on both axes */
static int ct_converted_ok(const char *conv_png, unsigned owner, unsigned pose)
{
    static uint8_t ocv[CANVAS * CANVAS_H], orgba[CANVAS * CANVAS_H * 4];
    uint8_t *c = NULL; int cw, ch, cpieces, cbw, cbh, opieces, obw, obh, ok;
    if (!art_body_canvas(owner, pose, ocv)) return 1;         /* nothing to compare against */
    pens_to_rgba(ocv, eng_pkg_palette(owner), orgba);
    opieces = ct_pieces(orgba, CANVAS, CANVAS_H, &obw, &obh);
    if (wf_video_load_rgba_png(conv_png, &c, &cw, &ch) || cw != CANVAS || ch > CANVAS_H) { if (c) free(c); fprintf(stderr, "class-template: converted ref %04u REJECTED (unreadable / wrong canvas)\n", pose); return 0; }
    cpieces = ct_pieces(c, cw, ch, &cbw, &cbh);   /* the converted set predates the 320-row canvas: 256x256 is fine... */
    {   int clipped = 0;                       /* ...unless the figure ran off its bottom row (434/435: the crouch's boots gone) */
        for (int x = 0; x < cw; x++) if (c[((size_t)(ch - 1) * cw + x) * 4 + 3]) clipped = 1;
        free(c);
        ok = !clipped && cpieces <= opieces && cbw * 10 >= obw * 7 && cbw * 10 <= obw * 14 && cbh * 20 >= obh * 17 && cbh * 10 <= obh * 14;
        if (!ok) fprintf(stderr, "class-template: converted ref %04u REJECTED (%spieces %d vs owner %d, box %dx%d vs %dx%d) - the owner's frame is the ref\n", pose, clipped ? "CLIPPED at the canvas bottom; " : "", cpieces, opieces, cbw, cbh, obw, obh);
    }
    return ok;
}
/* drop DUST: opaque components smaller than `minpx` pixels (a stray ROM dot
 * far from the body - 373 has one at the canvas edge - widens a ref's bbox
 * and skews placement; real detached pieces like a hand are far bigger) */
static int ct_drop_dust(uint8_t *rgba, int minpx)
{
    static int lab[CANVAS * CANVAS_H]; static int stack[CANVAS * CANVAS_H]; static int cnt[4096];
    int n = 0, dropped = 0;
    memset(lab, 0, sizeof lab);
    for (int i = 0; i < CANVAS * CANVAS_H; i++) {
        int sp = 0;
        if (!rgba[(size_t)i * 4 + 3] || lab[i]) continue;
        if (n >= 4095) break;
        n++; lab[i] = n; cnt[n] = 0; stack[sp++] = i;
        while (sp) {
            int q = stack[--sp], x = q % CANVAS, y = q / CANVAS;
            cnt[n]++;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy, r;
                if (nx < 0 || ny < 0 || nx >= CANVAS || ny >= CANVAS_H) continue;
                r = ny * CANVAS + nx;
                if (!rgba[(size_t)r * 4 + 3] || lab[r]) continue;
                lab[r] = n; stack[sp++] = r;
            }
        }
    }
    if (n <= 1) return 0;
    for (int i = 0; i < CANVAS * CANVAS_H; i++) if (lab[i] && cnt[lab[i]] < minpx) { memset(rgba + (size_t)i * 4, 0, 4); dropped++; }
    return dropped;
}
/* keep the largest 8-connected opaque component of a CANVAS x CANVAS_H RGBA
 * canvas; returns the number of pixels cleared (0 = one body already) */
static int ct_keep_main_body(uint8_t *rgba)
{
    static int lab[CANVAS * CANVAS_H]; static int stack[CANVAS * CANVAS_H]; static int cnt[4096];
    int n = 0, best = 0, bestn = 0, dropped = 0;
    memset(lab, 0, sizeof lab);
    for (int i = 0; i < CANVAS * CANVAS_H; i++) {
        int sp = 0;
        if (!rgba[(size_t)i * 4 + 3] || lab[i]) continue;
        if (n >= 4095) break;
        n++; lab[i] = n; cnt[n] = 0; stack[sp++] = i;
        while (sp) {
            int q = stack[--sp], x = q % CANVAS, y = q / CANVAS;
            cnt[n]++;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy, r;
                if (nx < 0 || ny < 0 || nx >= CANVAS || ny >= CANVAS_H) continue;
                r = ny * CANVAS + nx;
                if (!rgba[(size_t)r * 4 + 3] || lab[r]) continue;
                lab[r] = n; stack[sp++] = r;
            }
        }
        if (cnt[n] > bestn) { bestn = cnt[n]; best = n; }
    }
    if (n <= 1) return 0;
    for (int i = 0; i < CANVAS * CANVAS_H; i++) if (lab[i] && lab[i] != best) { memset(rgba + (size_t)i * 4, 0, 4); dropped++; }
    return dropped;
}
static int16_t ct_bx[CT_IDS], ct_by[CT_IDS], ct_bw[CT_IDS], ct_bh[CT_IDS];   /* bbox x0, y0, width, height */
static uint64_t *ct_mask[CT_IDS], *ct_mmask[CT_IDS];   /* silhouette inside the bbox, packed rows; and its mirror */
static int ct_area[CT_IDS];
/* COLOUR fingerprint (user 2026-08-26: a near-alias must not merge a same-
 * silhouette different-drawing pair - a turned face): mean RGB over a 4x4
 * grid of the bbox; a near alias needs the covered cells to agree */
static uint8_t ct_col[CT_IDS][16][3], ct_coln[CT_IDS][16];
static int ct_col_ok(unsigned p, unsigned q, int mirror)
{
    long d = 0, n = 0;
    for (int cy = 0; cy < 4; cy++) for (int cx = 0; cx < 4; cx++) {
        int a = cy * 4 + cx, b = cy * 4 + (mirror ? 3 - cx : cx);
        if (!ct_coln[p][a] || !ct_coln[q][b]) continue;
        d += abs(ct_col[p][a][0] - ct_col[q][b][0]) + abs(ct_col[p][a][1] - ct_col[q][b][1]) + abs(ct_col[p][a][2] - ct_col[q][b][2]);
        n++;
    }
    return n == 0 || d / n <= 60;              /* sum of channel diffs, mean per cell */
}
/* NEAR-DUPLICATE aliases (user 2026-08-26: "the point is to minimise the
 * generated images"): two ids whose silhouettes match to IoU >= WF_ALIAS_IOU
 * (default 0.95, 0 = exact only) after the best shift within +-3 px, plain or
 * mirrored, are ONE drawing - the later id is derived from the earlier at
 * ingest. Honky's job measured: 702 refs -> 351 distinct drawings at 0.97. */
static double ct_near_thr(void) { const char *e = getenv("WF_ALIAS_IOU"); return e ? atof(e) : 0.95; }
static void ct_pack(const uint8_t *rgba, int x0, int y0, int w, int h, uint64_t **out, uint64_t **mout)
{
    uint64_t *m = calloc((size_t)h * CT_W, sizeof *m), *mm = calloc((size_t)h * CT_W, sizeof *mm);
    if (!m || !mm) { free(m); free(mm); *out = *mout = NULL; return; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
        if (rgba[((size_t)(y0 + y) * CANVAS + x0 + x) * 4 + 3]) {
            m[y * CT_W + x / 64] |= 1ull << (x % 64);
            mm[y * CT_W + (w - 1 - x) / 64] |= 1ull << ((w - 1 - x) % 64);
        }
    *out = m; *mout = mm;
}
/* IoU of mask A against mask B placed at (dx, dy) in A's bbox space */
static double ct_iou_at(const uint64_t *A, int ah, const uint64_t *B, int bh, int dx, int dy)
{
    long inter = 0, uni = 0;
    int y0 = dy < 0 ? dy : 0, y1 = (bh + dy > ah ? bh + dy : ah);
    for (int y = y0; y < y1; y++) {
        const uint64_t *a = (y >= 0 && y < ah) ? A + (size_t)y * CT_W : NULL;
        const uint64_t *b = (y - dy >= 0 && y - dy < bh) ? B + (size_t)(y - dy) * CT_W : NULL;
        for (int i = 0; i < CT_W; i++) {
            uint64_t av = a ? a[i] : 0, bv = 0;
            if (b) {                   /* b shifted right by dx (left when negative) */
                if (dx >= 0) { bv = b[i] << dx; if (i > 0 && dx) bv |= b[i - 1] >> (64 - dx); }
                else { int s = -dx; bv = b[i] >> s; if (i + 1 < CT_W) bv |= b[i + 1] << (64 - s); }
            }
            inter += __builtin_popcountll(av & bv); uni += __builtin_popcountll(av | bv);
        }
    }
    return uni ? (double)inter / (double)uni : 0.0;
}
/* best IoU of p vs q (q's mask, plain or mirrored, shifted) -> shift out */
static double ct_near_r(unsigned p, unsigned q, int mirror, int range, int slack, int *sx, int *sy)
{
    const uint64_t *B = mirror ? ct_mmask[q] : ct_mask[q]; double best = 0;
    if (!ct_mask[p] || !B) return 0;
    if (abs(ct_bw[p] - ct_bw[q]) > slack || abs(ct_bh[p] - ct_bh[q]) > slack) return 0;
    if (ct_area[p] * 10 < ct_area[q] * 7 || ct_area[q] * 10 < ct_area[p] * 7) return 0;
    for (int dy = -range; dy <= range; dy++) for (int dx = -range; dx <= range; dx++) {
        double v = ct_iou_at(ct_mask[p], ct_bh[p], B, ct_bh[q], dx, dy);
        if (v > best) { best = v; *sx = dx; *sy = dy; }
    }
    return best;
}
static double ct_near(unsigned p, unsigned q, int mirror, int *sx, int *sy) { return ct_near_r(p, q, mirror, 3, 6, sx, sy); }
/* FAMILY rule (user 2026-08-26): the held man in a hold is ONE drawing per
 * holder pose, whoever holds him - the ROM re-drew him per holder body
 * (arm drape, a few px of height) but a skin needs one; the held-side
 * calibration is the fine-tune. WF_ALIAS_FAMILY_IOU (default 0.6, 0 = off)
 * is the sanity floor that keeps genuinely different drawings (mirror
 * variants, re-posed victims) apart. */
static double ct_family_thr(void) { const char *e = getenv("WF_ALIAS_FAMILY_IOU"); return e ? atof(e) : 0.6; }
static void ct_hash_rgba(const uint8_t *rgba, unsigned p)
{
    int x0 = CANVAS, x1 = -1, y0 = CANVAS_H, y1 = -1;
    uint64_t h = 1469598103934665603ull, m = 1469598103934665603ull;
    if (p >= CT_IDS) return;
    for (int y = 0; y < CANVAS_H; y++) for (int x = 0; x < CANVAS; x++)
        if (rgba[((size_t)y * CANVAS + x) * 4 + 3]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x1 < 0) { ct_hash[p] = 0; return; }
    ct_bh[p] = (int16_t)(y1 - y0 + 1);
    free(ct_mask[p]); free(ct_mmask[p]);
    ct_pack(rgba, x0, y0, x1 - x0 + 1, y1 - y0 + 1, &ct_mask[p], &ct_mmask[p]);
    ct_area[p] = 0;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) ct_area[p] += rgba[((size_t)y * CANVAS + x) * 4 + 3] != 0;
    {   /* colour fingerprint */
        long acc[16][3] = {{0}}; int cnt[16] = {0}, bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
            const uint8_t *a = rgba + ((size_t)y * CANVAS + x) * 4;
            int c = ((y - y0) * 4 / bh) * 4 + (x - x0) * 4 / bw;
            if (!a[3]) continue;
            acc[c][0] += a[0]; acc[c][1] += a[1]; acc[c][2] += a[2]; cnt[c]++;
        }
        for (int c = 0; c < 16; c++) { ct_coln[p][c] = cnt[c] > 8; for (int k = 0; k < 3; k++) ct_col[p][c][k] = cnt[c] ? (uint8_t)(acc[c][k] / cnt[c]) : 0; }
    }
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const uint8_t *a = rgba + ((size_t)y * CANVAS + x) * 4;
        const uint8_t *b = rgba + ((size_t)y * CANVAS + (x1 - (x - x0))) * 4;   /* mirrored within the bbox */
        uint32_t va = a[3] ? ((uint32_t)a[0] << 16 | (uint32_t)a[1] << 8 | a[2] | 0x01000000u) : 0;
        uint32_t vb = b[3] ? ((uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2] | 0x01000000u) : 0;
        h ^= va; h *= 1099511628211ull; m ^= vb; m *= 1099511628211ull;
    }
    h ^= (uint64_t)(x1 - x0 + 1) << 40 ^ (uint64_t)(y1 - y0 + 1) << 52;   /* shape */
    m ^= (uint64_t)(x1 - x0 + 1) << 40 ^ (uint64_t)(y1 - y0 + 1) << 52;
    if (!h) h = 1; if (!m) m = 1;
    ct_hash[p] = h; ct_mhash[p] = m; ct_bx[p] = (int16_t)x0; ct_by[p] = (int16_t)y0; ct_bw[p] = (int16_t)(x1 - x0 + 1);
}
static void ct_hash_png(const char *path, unsigned p)
{
    uint8_t *rgba; int w, h;
    if (wf_video_load_rgba_png(path, &rgba, &w, &h) != 0) return;
    if (w == CANVAS && h == CANVAS_H) ct_hash_rgba(rgba, p);
    free(rgba);
}

/* stock mode: pre-fill out/ with the class's own frames (universal/class
 * singles, victims, aisle) so a generation run touches ONLY the borrowed
 * poses, and write anchor.png = the representative's standing pose at 4x
 * (nearest) as the identity anchor — the result is the class's complete
 * STOCK template (the stock member doing every move in the game). */
static int ct_stock;
static void ct_copy_out(const char *dir, unsigned id)
{
    char a[600], b[600], cmd[1400];
    if (!ct_stock) return;
    snprintf(a, sizeof a, "%s/ref/pose_%04u.png", dir, id);
    snprintf(b, sizeof b, "%s/out/pose_%04u.png", dir, id);
    snprintf(cmd, sizeof cmd, "cp -f \"%s\" \"%s\"", a, b);
    if (system(cmd) != 0) fprintf(stderr, "class-template: cannot copy %s\n", a);
}
int tool_class_template_stock(int cls, const char *dir)
{
    int rc;
    ct_stock = 1;
    rc = tool_class_template(cls, dir);
    ct_stock = 0;
    return rc;
}
int tool_class_template(int cls, const char *dir)
{
    static uint8_t cv[CANVAS * CANVAS_H], rgba[CANVAS * CANVAS_H * 4], done[MAXV];
    static uint8_t has[ENG_WS_MAX][0x400];
    char path[600]; FILE *mf, *vf; int rep = -1, nref = 0, nvict = 0, nbor = 0, nconv = 0, nalias = 0, first = 1;
    if (cls < 0 || cls >= ENG_BODY_CLASSES) { fprintf(stderr, "class-template: class 0..4\n"); return 1; }
    memset(ct_alias, 0, sizeof ct_alias); memset(ct_area, 0, sizeof ct_area); memset(ct_card_alias, 0, sizeof ct_card_alias);
    mkdir(dir, 0775);
    snprintf(path, sizeof path, "%s/ref", dir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/out", dir); mkdir(path, 0775);
    for (unsigned w = 0; w < ENG_WS_MAX; w++) {
        const eng_pkg_rec *pr;
        if (rep < 0 && eng_ws_body_class((int)w) == cls) rep = (int)w;
        for (unsigned p = 0; p < 0x400; p++) has[w][p] = eng_pkg_pose(w, p, 0, -1, &pr) > 0;
    }
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    mf = fopen(path, "w");
    if (!mf) return 1;
    fprintf(mf, "{ \"class\": %d, \"name\": \"%s\", \"representative\": %d, \"members\": [", cls, eng_body_class_name(cls), rep);
    for (unsigned w = 0; w < ENG_WS_MAX; w++) if (eng_ws_body_class((int)w) == cls) { fprintf(mf, "%s%u", first ? "" : ", ", w); first = 0; }
    fprintf(mf, "],\n  \"canvas\": [%d, %d], \"origin\": [%d, %d], \"window\": 256,\n"
                "  \"contract\": \"restyle ref/pose_NNNN.png -> out/pose_NNNN.png; SAME size, origin (feet line) and silhouette; transparent background; body only (weapons/ropes are composited by the engine); ids >= 1024 are VICTIM bodies in holder coordinates; missing outputs fall back to the class's stock art\",\n"
                "  \"poses\": {", CANVAS, CANVAS_H, ORG_X, ORG_Y);
    first = 1;
    memset(ct_hash, 0, sizeof ct_hash); memset(ct_mhash, 0, sizeof ct_mhash);
    for (unsigned p = 0; p < 0x400; p++) {
        int src = -1, all = 1, any = 0; const char *kind;
        for (unsigned w = 0; w < ENG_WS_MAX; w++) {
            if (!has[w][p]) { all = 0; continue; }
            any = 1;
            if (src < 0 && eng_ws_body_class((int)w) == cls) src = (int)w;
        }
        if (!any) continue;
        if (src >= 0) kind = all ? "universal" : "class";
        else { src = class_borrow_source(cls, p); kind = "borrowed"; nbor++; }
        if (src < 0) continue;
        {   /* the class GENERIC (data/generics/C/gen, drawn from the class base ref by
               --class-build) is THE reference whenever it exists (user 2026-08-28:
               "will Refs pull in this new class set?") - the neutral body every
               skin of the class converts from */
            char gp[600], cmd[1400]; struct stat st;
            snprintf(gp, sizeof gp, "data/generics/%d/gen/pose_%04u.png", cls, p);
            if (stat(gp, &st) == 0) {
                snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, p);
                snprintf(cmd, sizeof cmd, "cp -f \"%s\" \"%s\"", gp, path);
                if (system(cmd) == 0) {
                    int wy = 0; uint8_t *cr; int cw, chh;
                    if (wf_video_load_rgba_png(path, &cr, &cw, &chh) == 0) { if (cw == CANVAS && chh == CANVAS_H) { wy = ct_win_y(cr); ct_hash_rgba(cr, p); } free(cr); }   /* aliases: the GENERIC drawing */
                    fprintf(mf, "%s\n    \"%u\": { \"kind\": \"generic\", \"ref\": %d, \"win_y\": %d }", first ? "" : ",", p, rep, wy); first = 0;
                    nref++;
                    if (ct_stock) ct_copy_out(dir, p);
                    continue;
                }
            }
        }
        {   /* the class's CONVERTED stock template (data/classes/C/stock/frames,
               the stock member doing every move) is the ref for a borrowed
               pose whenever it exists — a same-body mannequin instead of
               another class's */
            char cs[600], cmd[1400]; struct stat st;
            snprintf(cs, sizeof cs, "data/classes/%d/stock/frames/pose_%04u.png", cls, p);
            if (!strcmp(kind, "borrowed") && stat(cs, &st) == 0 && ct_converted_ok(cs, (unsigned)src, p)) {   /* VALIDATED against the owner's real
                                          frame (user 2026-08-29: "make sure frames aren't corrupt before i generate against them"):
                                          a smear (202) or a floating piece (327) is rejected -> the owner's frame is the ref */
                snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, p);
                snprintf(cmd, sizeof cmd, "cp -f \"%s\" \"%s\"", cs, path);
                if (system(cmd) == 0) {
                    int wy = 0; { uint8_t *cr; int cw, chh; if (wf_video_load_rgba_png(path, &cr, &cw, &chh) == 0) { if (cw == CANVAS && chh == CANVAS_H) wy = ct_win_y(cr); free(cr); } }
                    fprintf(mf, "%s\n    \"%u\": { \"kind\": \"converted\", \"ref\": %d, \"win_y\": %d }", first ? "" : ",", p, rep, wy); first = 0;
                    nref++; nconv++;
                    if (ct_stock) ct_copy_out(dir, p);
                    if (art_body_canvas((unsigned)src, p, cv)) { pens_to_rgba(cv, eng_pkg_palette((unsigned)src), rgba); ct_hash_rgba(rgba, p); }   /* aliases: the STOCK drawing */
                    continue;
                }
            }
        }
        if (!art_body_canvas((unsigned)src, p, cv)) continue;
        pens_to_rgba(cv, eng_pkg_palette((unsigned)src), rgba);
        { int d = ct_drop_dust(rgba, 16); if (d) fprintf(stderr, "class-template: ref %04u: %d dust pixel(s) dropped\n", p, d); }
        snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, p);
        if (wf_art_write_rgba_png(path, rgba, CANVAS, CANVAS_H)) continue;
        fprintf(mf, "%s\n    \"%u\": { \"kind\": \"%s\", \"ref\": %d, \"win_y\": %d }", first ? "" : ",", p, kind, src, ct_win_y(rgba)); first = 0;
        nref++;
        if (strcmp(kind, "borrowed")) ct_copy_out(dir, p);
        ct_hash_rgba(rgba, p);
    }
    fprintf(mf, "\n  },\n  \"poses_note\": \"(single ids above; aliases are derived at ingest - see 'aliases' below)\"");
    for (unsigned cell = 0; cell < 8; cell++) {   /* aisle walk-in cells (768+): the representative's */
        if (!art_aisle_canvas((unsigned)rep, cell, cv)) continue;
        pens_to_rgba(cv, eng_pkg_palette((unsigned)rep), rgba);
        snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, 768u + cell);
        if (wf_art_write_rgba_png(path, rgba, CANVAS, CANVAS_H)) continue;
        fprintf(mf, ",\n    \"%u\": { \"kind\": \"aisle\", \"ref\": %d }", 768u + cell, rep);
        ct_copy_out(dir, 768u + cell);
    }
    {   /* PORTRAIT surfaces (user 2026-08-25: part of a skin's generation list):
           800+cell = the continue-screen faces (row 0x30+rep, six 80x80 cards with
           the crowd behind, each in its own palette), 810 = the title-win card
           (row 0x4D, 124x144). Generated with the portrait recipe, not the mannequin one. */
        {   /* the ROM's continue row: cells 4-6 are BYTE-IDENTICAL to 0-2 (checked on
               every stock man; user 2026-08-28: "isn't 0800-0802 the same sequence
               as 0804-0806?") - three drawings, not six: the duplicates become
               aliases (the ingest derives them), so a skin generates three faces */
            static uint8_t card[8][CANVAS * CANVAS_H * 4]; static int card_ok[8];
            memset(card_ok, 0, sizeof card_ok);
            for (unsigned cell = 0; cell < 8; cell++) card_ok[cell] = art_row_cell_rgba(0x30u + (unsigned)rep, cell, card[cell]) != 0;
            for (unsigned cell = 0; cell < 8; cell++) {
                if (!card_ok[cell]) continue;
                if (cell >= 4 && card_ok[cell - 4] && !memcmp(card[cell], card[cell - 4], sizeof card[0])) { ct_card_alias[cell] = 1; continue; }
                snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, 800u + cell);
                if (wf_art_write_rgba_png(path, card[cell], CANVAS, CANVAS_H)) continue;
                fprintf(mf, ",\n    \"%u\": { \"kind\": \"portrait\", \"ref\": %d, \"surface\": \"continue\", \"cell\": %u }", 800u + cell, rep, cell);
                if (ct_stock) ct_copy_out(dir, 800u + cell);
            }
            for (unsigned cell = 4; cell < 8; cell++) if (ct_card_alias[cell]) {   /* stale refs/outs of an earlier template run */
                snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, 800u + cell); unlink(path);
                snprintf(path, sizeof path, "%s/out/pose_%04u.png", dir, 800u + cell); unlink(path);
            }
        }
        if (art_row_cell_rgba(0x4Du, (unsigned)(rep == 11 ? 10 : rep), rgba)) {
            snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, 810u);
            if (!wf_art_write_rgba_png(path, rgba, CANVAS, CANVAS_H)) {
                fprintf(mf, ",\n    \"810\": { \"kind\": \"portrait\", \"ref\": %d, \"surface\": \"title\" }", rep);
                if (ct_stock) ct_copy_out(dir, 810u);
            }
        }
    }
    if (ct_stock && art_body_canvas((unsigned)rep, 0, cv)) {   /* anchor: the representative, standing, 4x */
        static uint8_t big[CANVAS * 4 * CANVAS * 4 * 4];
        pens_to_rgba(cv, eng_pkg_palette((unsigned)rep), rgba);
        for (int y = 0; y < CANVAS * 4; y++) for (int x = 0; x < CANVAS * 4; x++)
            memcpy(big + ((size_t)y * CANVAS * 4 + x) * 4, rgba + ((size_t)(y / 4) * CANVAS + x / 4) * 4, 4);
        snprintf(path, sizeof path, "%s/anchor.png", dir);
        wf_art_write_rgba_png(path, big, CANVAS * 4, CANVAS * 4);
    }
    /* victim ids: every (holder, pose) that holds a member, ref from the
       first member owning the entry (the representative for most) */
    build_victmap(cls);
    snprintf(path, sizeof path, "%s/victmap.json", dir);
    vf = fopen(path, "w");
    if (vf) fprintf(vf, "{ \"class\": %d, \"base\": %d, \"first\": %u, \"note\": \"[ref id, holder row (12 = the skin's own base), holder pose]; ref ids >= 1024 are shared victim frames\",\n  \"entries\": [", cls, rep, 1024u);
    memset(done, 0, sizeof done);
    fprintf(mf, ",\n  \"victims\": {");
    first = 1;
    for (int i = 0; i < vmn; i++) {
        if (vf) fprintf(vf, "%s\n  [%u, %u, %u]", i ? "," : "", 1024u + vm[i].vid, vm[i].row, vm[i].pose);
        if (done[vm[i].vid]) continue;
        for (unsigned v = 0; v < ENG_WS_MAX && !done[vm[i].vid]; v++) {
            unsigned row = vm[i].row == SELF ? v : vm[i].row;
            if (eng_ws_body_class((int)v) != cls) continue;
            {   /* the class GENERIC's victim frame first (see the singles) */
                char gp[600], cmd[1400]; struct stat st;
                snprintf(gp, sizeof gp, "data/generics/%d/gen/pose_%04u.png", cls, 1024u + vm[i].vid);
                if (stat(gp, &st) == 0) {
                    snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, 1024u + vm[i].vid);
                    snprintf(cmd, sizeof cmd, "cp -f \"%s\" \"%s\"", gp, path);
                    if (system(cmd) == 0) {
                        int wy = 0; uint8_t *cr; int cw, chh;
                        if (wf_video_load_rgba_png(path, &cr, &cw, &chh) == 0) { if (cw == CANVAS && chh == CANVAS_H) { wy = ct_win_y(cr); ct_hash_rgba(cr, 1024u + vm[i].vid); } free(cr); }
                        done[vm[i].vid] = 1; nvict++;
                        fprintf(mf, "%s\n    \"%u\": { \"ref\": %u, \"holder\": %u, \"pose\": %u, \"win_y\": %d, \"generic\": true }", first ? "" : ",", 1024u + vm[i].vid, v, vm[i].row, vm[i].pose, wy); first = 0;
                        continue;
                    }
                }
            }
            if (!art_victim_canvas(row, vm[i].pose, v, cv)) continue;
            { extern int art_victim_src; pens_to_rgba(cv, eng_pkg_palette(art_victim_src >= 0 ? (unsigned)art_victim_src : v), rgba); }   /* the drawn body's own palette */
            {   /* a holder-subtraction split can leave a SLIVER of the holder far from the
                   body (user 2026-08-29: 1085 had one at the canvas edge) - it wrecks the
                   ref's bbox and the placement gate: keep the largest connected body only */
                int dropped = ct_keep_main_body(rgba);
                if (dropped) fprintf(stderr, "class-template: victim %u: %d stray pixel(s) dropped (holder leftover)\n", 1024u + vm[i].vid, dropped);
            }
            snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, 1024u + vm[i].vid);
            if (wf_art_write_rgba_png(path, rgba, CANVAS, CANVAS_H)) continue;
            done[vm[i].vid] = 1; nvict++;
            if (eng_pkg_has_part(row, vm[i].pose, (int)v) && (row != v || eng_pkg_mirror_ok(row, vm[i].pose)))
                ct_copy_out(dir, 1024u + vm[i].vid);   /* a MIRROR HOLE ref is another body: generate it */
            fprintf(mf, "%s\n    \"%u\": { \"ref\": %u, \"holder\": %u, \"pose\": %u, \"win_y\": %d }", first ? "" : ",", 1024u + vm[i].vid, v, vm[i].row, vm[i].pose, ct_win_y(rgba)); first = 0;
            ct_hash_rgba(rgba, 1024u + vm[i].vid);      /* victims join the alias pass */
        }
    }
    if (vf) { fprintf(vf, "\n] }\n"); fclose(vf); }
    /* ALIASES (user 2026-08-25: minimise generation): an id whose canvas is
       pixel-identical to an earlier id, or the exact mirror of one, or (2026-
       08-26) a NEAR-duplicate silhouette (IoU >= WF_ALIAS_IOU after the best
       shift, plain or mirrored) is never generated — ingest materialises it
       from the source frame with the recorded flip + (dx, dy). Bodies and
       victim bodies alike; exact matches are preferred, then the best IoU. */
    {
        double thr = ct_near_thr(); int nnear = 0, nrev = 0;
        fprintf(mf, "\n  },\n  \"aliases\": {");   /* closes "victims" */
        first = 1;
        for (unsigned cell = 4; cell < 8; cell++) if (ct_card_alias[cell]) {   /* the duplicate continue faces */
            fprintf(mf, "%s\n    \"%u\": { \"of\": %u, \"flip\": 0, \"dx\": 0, \"dy\": 0, \"card\": true }", first ? "" : ",", 800u + cell, 800u + cell - 4); first = 0; nalias++;
        }
        {   /* REVIEWED aliases first (user 2026-08-28: the skin must not re-draw a
               pair merged in the class review): data/generics/C/status.json
               'alias' ids (auto near-merges + the user's overrides), chains
               resolved to the root; flip/dx/dy from the override or the bbox */
            char sp[300], err[128]; json_val *st, *ov = NULL;
            static int rroot[CT_IDS], rflip[CT_IDS], rdx[CT_IDS], rdy[CT_IDS], rhas[CT_IDS]; static uint8_t ral[CT_IDS];
            memset(ral, 0, sizeof ral); memset(rhas, 0, sizeof rhas);
            snprintf(sp, sizeof sp, "data/generics/%d/status.json", cls);
            st = json_parse_file(sp, err, sizeof err);
            if (st) {
                for (const json_val *e = json_get(st, "ids") ? json_get(st, "ids")->child : NULL; e; e = e->next) {
                    int id = atoi(e->key);
                    if (id < 0 || id >= (int)CT_IDS || strcmp(json_str(json_get(e, "status"), ""), "alias")) continue;
                    ral[id] = 1; rroot[id] = (int)json_int(json_get(e, "of"), -1); rflip[id] = (int)json_int(json_get(e, "flip"), 0); rdx[id] = 0; rdy[id] = 0;   /* status.json carries the flip (2026-08-29) */
                }
                snprintf(sp, sizeof sp, "data/generics/%d/aliases.override.json", cls);
                ov = json_parse_file(sp, err, sizeof err);
                for (const json_val *e = ov && json_get(ov, "add") ? json_get(ov, "add")->child : NULL; e; e = e->next) {
                    int id = atoi(e->key);
                    if (id >= 0 && id < (int)CT_IDS && ral[id]) { rroot[id] = (int)json_int(json_get(e, "of"), rroot[id]); rflip[id] = (int)json_int(json_get(e, "flip"), 0); rhas[id] = 1; }
                }
                if (ov) json_free(ov);
                json_free(st);
                for (unsigned p = 0; p < CT_IDS; p++) if (ral[p])
                    for (int hop = 0; hop < 6 && rroot[p] >= 0 && rroot[p] < (int)CT_IDS && ral[rroot[p]]; hop++) { rflip[p] ^= rflip[rroot[p]]; rroot[p] = rroot[rroot[p]]; }
                for (unsigned p = 0; p < CT_IDS; p++) {
                    int of = ral[p] ? rroot[p] : -1;
                    if (of < 0 || of >= (int)CT_IDS || !ct_hash[p] || !ct_hash[of] || ct_alias[of]) continue;   /* both refs exist in this job, the root is real */
                    ct_alias[p] = 1; nalias++; nrev++;
                    fprintf(mf, "%s\n    \"%u\": { \"of\": %d, \"flip\": %d, \"dx\": %d, \"dy\": %d, \"reviewed\": true }", first ? "" : ",", p, of, rflip[p], ct_bx[p] - ct_bx[of], ct_by[p] - ct_by[of]); first = 0;
                    snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, p); unlink(path);
                    snprintf(path, sizeof path, "%s/out/pose_%04u.png", dir, p); unlink(path);
                }
                (void)rdx; (void)rdy; (void)rhas;
            }
            if (nrev) fprintf(stderr, "class-template: %d reviewed class aliases applied (data/generics/%d/status.json)\n", nrev, cls);
        }
        for (unsigned p = 0; p < CT_IDS; p++) {
            int of = -1, flip = 0, sx = 0, sy = 0; double best = 0;
            if (!ct_hash[p] || ct_alias[p]) continue;
            for (unsigned q = 0; q < p && of < 0; q++) {
                if (!ct_hash[q] || ct_alias[q]) continue;          /* alias to a real source only */
                if (ct_hash[q] == ct_hash[p]) of = (int)q;
                else if (ct_mhash[q] == ct_hash[p]) { of = (int)q; flip = 1; }
            }
            if (of < 0 && thr > 0) {
                for (unsigned q = 0; q < p; q++) {
                    int tx, ty; double v;
                    if (!ct_hash[q] || ct_alias[q]) continue;
                    /* a victim body may alias a BODY pose (a pinned man = his lying frame) */
                    v = ct_near(p, q, 0, &tx, &ty);
                    if (v > best && v >= thr && ct_col_ok(p, q, 0)) { best = v; of = (int)q; flip = 0; sx = tx; sy = ty; }
                    v = ct_near(p, q, 1, &tx, &ty);
                    if (v > best && v >= thr && ct_col_ok(p, q, 1)) { best = v; of = (int)q; flip = 1; sx = tx; sy = ty; }
                }
                if (of >= 0) nnear++;
            }
            if (of < 0) continue;
            ct_alias[p] = 1; nalias++;
            {   /* placement: p's bbox origin relative to the source's (+ the residual shift that matched) */
                int dx = ct_bx[p] - ct_bx[of] + sx, dy = ct_by[p] - ct_by[of] + sy;
                if (best > 0) fprintf(mf, "%s\n    \"%u\": { \"of\": %d, \"flip\": %d, \"dx\": %d, \"dy\": %d, \"iou\": %.3f }", first ? "" : ",", p, of, flip, dx, dy, best);
                else fprintf(mf, "%s\n    \"%u\": { \"of\": %d, \"flip\": %d, \"dx\": %d, \"dy\": %d }", first ? "" : ",", p, of, flip, dx, dy);
                first = 0;
            }
            snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, p); unlink(path);   /* not a generation target */
            snprintf(path, sizeof path, "%s/out/pose_%04u.png", dir, p); unlink(path);   /* ingest derives it */
        }
        {   /* FAMILY pass: per holder pose, every victim id serving it -> the first */
            double fthr = ct_family_thr(); int nfam = 0;
            if (fthr > 0)
                for (unsigned P = 0; P < 0x400; P++) {
                    int rep = -1;
                    for (int i = 0; i < vmn; i++) {
                        unsigned v = 1024u + vm[i].vid;
                        if (vm[i].pose != P || v >= CT_IDS || !ct_hash[v] || ct_alias[v]) continue;
                        if (rep < 0) { rep = (int)v; continue; }
                        if ((int)v == rep) continue;
                        {   int tx = 0, ty = 0; double b = ct_near_r(v, (unsigned)rep, 0, 8, 24, &tx, &ty);
                            if (b < fthr) continue;
                            ct_alias[v] = 1; nalias++; nfam++;
                            fprintf(mf, "%s\n    \"%u\": { \"of\": %d, \"flip\": 0, \"dx\": %d, \"dy\": %d, \"iou\": %.3f, \"family\": %u }", first ? "" : ",", v, rep, ct_bx[v] - ct_bx[rep] + tx, ct_by[v] - ct_by[rep] + ty, b, P); first = 0;
                            snprintf(path, sizeof path, "%s/ref/pose_%04u.png", dir, v); unlink(path);
                            snprintf(path, sizeof path, "%s/out/pose_%04u.png", dir, v); unlink(path);
                        }
                    }
                }
            fprintf(mf, "\n  },\n  \"alias_iou\": %.3f, \"alias_family_iou\": %.3f", thr, fthr);
            fprintf(stderr, "class-template: %d aliases (%d near-duplicates at IoU >= %.2f, %d family at >= %.2f)\n", nalias, nnear, thr, nfam, fthr);
        }
    }
    fprintf(mf, ",\n  \"counts\": { \"single\": %d, \"borrowed\": %d, \"converted\": %d, \"aliases\": %d, \"victim\": %d }\n}\n", nref, nbor, nconv, nalias, nvict);
    fclose(mf);
    snprintf(path, sizeof path, "%s/prompt.txt", dir);
    if (!(mf = fopen(path, "r"))) {
        mf = fopen(path, "w");
        if (mf) { fprintf(mf, "Describe your wrestler here (build, skin tone, hair, outfit, colours).\nBody class %d (%s): keep the mannequin's proportions.\n", cls, eng_body_class_name(cls)); fclose(mf); }
    } else fclose(mf);
    fprintf(stderr, "class-template: class %d (%s) -> %s: %d single refs (%d borrowed, %d of them from the converted stock template, %d ALIASED = never generated), %d victim refs (%d victmap entries)%s\n",
            cls, eng_body_class_name(cls), dir, nref, nbor, nconv, nalias, nvict, vmn,
            ct_stock ? " — STOCK mode: out/ pre-filled, only the borrowed poses are pending; anchor.png = the representative" : "");
    return 0;
}

/* ---- verification: recompose from the skin parts and compare ---- */
static int load_pens(const char *path, const uint16_t *pens, uint8_t *cv)
{
    uint8_t *rgba; int w, h;
    if (wf_video_load_rgba_png(path, &rgba, &w, &h) != 0 || w != CANVAS || h != CANVAS_H) return 0;
    for (int i = 0; i < CANVAS * CANVAS_H; i++) {
        uint8_t *p = &rgba[i * 4]; cv[i] = 0;
        if (!p[3]) continue;
        for (int k = 1; k < 16; k++)       /* exact pen match (own palette) */
            if ((pens[k] & 0xF) * 17 == p[0] && ((pens[k] >> 4) & 0xF) * 17 == p[1] && ((pens[k] >> 8) & 0xF) * 17 == p[2]) { cv[i] = (uint8_t)k; break; }
    }
    free(rgba);
    return 1;
}
/* draw the stock composed list, taking each cell's pixels from the flat
 * frame of its side — holder cells from H, victim cells from V — in the
 * stock order (a cell = its 16x16 footprint per chain link) */
static void compose(unsigned row, unsigned pose, unsigned victim, const uint8_t *H, const uint8_t *V, uint8_t *out)
{
    static uint8_t vm2[192];
    const eng_pkg_rec *pr; int n = eng_pkg_pose(row, pose, 0, (int)victim, &pr);
    memset(out, 0, CANVAS * CANVAS_H);
    if (n <= 0 || eng_pkg_victim_mask(row, pose, 0, victim, vm2, 192) <= 0) return;
    for (int i = 0; i < n; i++) {
        const uint8_t *src = vm2[i] ? V : H;
        int x = (pr[i].x + ORG_X) & 0x1FF, y0;
        unsigned chain = pr[i].chain & 7u;
        if (x > 512 - 16) x -= 512;
        y0 = (((256 - (pr[i].y + 0x80)) & 0x1FF) - 16) + (ORG_Y - 112);
        for (unsigned c = 0; c <= chain; c++) {
            const uint8_t *t = wf_video_tile_pens((unsigned)(pr[i].tile + c));
            int y = pr[i].flipy ? y0 - (int)(16 * chain) + (int)(16 * c) : y0 - (int)(16 * c);
            if (!t) continue;
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int qx = pr[i].flipx ? 15 - px : px, qy = pr[i].flipy ? 15 - py : py;
                int ox = x + px, oy = y + py;
                if (!t[qy * 16 + qx] || ox < 0 || ox >= CANVAS || oy < 0 || oy >= CANVAS_H) continue;
                /* the cell's own pen decides WHERE it paints; the flat frame
                   supplies WHAT (strict: an empty flat pixel paints nothing) */
                out[oy * CANVAS + ox] = src[oy * CANVAS + ox];
            }
        }
    }
}
int tool_verify_skin(int base, const char *dir)
{
    static uint8_t H[CANVAS * CANVAS_H], V[CANVAS * CANVAS_H], want[CANVAS * CANVAS_H], got[CANVAS * CANVAS_H];
    const uint16_t *pens = eng_pkg_palette((unsigned)base);
    char path[600]; int cls, nfr = 0, bad = 0, missing = 0, single_bad = 0;
    if (base < 0 || base >= ENG_WS_MAX || !pens) return 1;
    cls = eng_ws_body_class(base);
    build_victmap(cls);
    /* single frames: body-only PNG must equal the body render */
    for (unsigned p = 0; p < 0x400; p++) {
        if (!art_body_canvas((unsigned)base, p, want)) continue;
        snprintf(path, sizeof path, "%s/frames/pose_%04u.png", dir, p);
        if (!load_pens(path, pens, H)) { missing++; continue; }
        for (int i = 0; i < CANVAS * CANVAS_H; i++)
            if ((H[i] ? pens[H[i]] : 0xFFFF) != (want[i] ? pens[want[i]] : 0xFFFF)) { single_bad++; break; }
    }
    /* two-man frames: base as HOLDER of every victim member v (holder body
       = frames/pose_P, victim body = v's skin victims/ — here v's stock
       render, the same thing the export writes) */
    for (unsigned v = 0; v < ENG_WS_MAX; v++) {
        for (unsigned p = 0; p < 0x400; p++) {
            const eng_pkg_rec *pr; int n;
            if (!eng_pkg_has_part((unsigned)base, p, (int)v)) continue;
            n = eng_pkg_pose((unsigned)base, p, 0, (int)v, &pr);
            if (n <= 0) continue;
            /* the baked frame */
            memset(want, 0, sizeof want);
            for (int i = 0; i < n; i++) {
                int x = (pr[i].x + ORG_X) & 0x1FF, y0; unsigned chain = pr[i].chain & 7u;
                if (x > 512 - 16) x -= 512;
                y0 = (((256 - (pr[i].y + 0x80)) & 0x1FF) - 16) + (ORG_Y - 112);
                for (unsigned c = 0; c <= chain; c++) {
                    const uint8_t *t = wf_video_tile_pens((unsigned)(pr[i].tile + c));
                    int y = pr[i].flipy ? y0 - (int)(16 * chain) + (int)(16 * c) : y0 - (int)(16 * c);
                    if (!t) continue;
                    for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                        int qx = pr[i].flipx ? 15 - px : px, qy = pr[i].flipy ? 15 - py : py;
                        int ox = x + px, oy = y + py;
                        if (t[qy * 16 + qx] && ox >= 0 && ox < CANVAS && oy >= 0 && oy < CANVAS_H) want[oy * CANVAS + ox] = t[qy * 16 + qx];
                    }
                }
            }
            snprintf(path, sizeof path, "%s/frames/pose_%04u.png", dir, p);
            if (!load_pens(path, pens, H)) { missing++; continue; }
            if (!art_victim_canvas((unsigned)base, p, v, V)) continue;   /* = v's victims/vict_N.png */
            compose((unsigned)base, p, v, H, V, got);
            nfr++;
            {   /* compare in COLOUR: a skin is paint, and some stock palettes
                   carry two pens of one colour (Hawk/Smash/Crush), which a
                   PNG round trip cannot tell apart */
                const uint16_t *vp = eng_pkg_palette(v);
                int diff = 0;
                for (int i = 0; i < CANVAS * CANVAS_H; i++) {
                    unsigned a = got[i] ? pens[got[i]] : 0xFFFF, b = want[i] ? pens[want[i]] : 0xFFFF;
                    (void)vp;
                    if (a != b) { diff++; if (diff <= 3 && bad < 4) fprintf(stderr, "   px (%d,%d) got pen %u want pen %u\n", i % CANVAS, i / CANVAS, got[i], want[i]); }
                }
                if (diff) {
                    if (bad < 12) fprintf(stderr, "verify-skin: MISMATCH holder %d pose %u victim %u: %d px\n", base, p, v, diff);
                    bad++;
                }
            }
        }
    }
    fprintf(stderr, "verify-skin: base %d: %d two-man frames recomposed, %d mismatched; single frames %d bad, %d missing\n", base, nfr, bad, single_bad, missing);
    return bad || single_bad || missing ? 1 : 0;
}
