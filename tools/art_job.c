/* The FORGE ART PIPELINE (user 2026-08-24): reskin a wrestler with
 * externally generated frames (an AI image/video model, or an artist).
 * The engine owns the deterministic bookends; whatever fills the job
 * directory is interchangeable.
 *
 *   wfengine --art-job <base> <jobdir>
 *      renders every pose of <base> as ref/pose_NNNN.png (RGBA on
 *      transparency, 256x256 canvas, the figure's ORIGIN fixed at
 *      (128,176) — feet line), writes manifest.json + prompt.txt
 *      template. The generator restyles ref/* into out/* keeping size,
 *      origin and silhouette (img2img, low denoise).
 *
 *   wfengine --art-ingest <jobdir> <slot> <dstdir>
 *      builds a clone-art package from out/*.png: derives a 15-colour
 *      palette across ALL frames, quantizes, cuts 16x16 tiles with
 *      cross-pose dedupe into the slot's arena, writes sheet.png /
 *      tiles.json / poses.json (own + mirrored own_f) / palette.json
 *      into <dstdir> (a mods/<m>/wrestlers/<slot>/ dir). Poses missing
 *      from out/ simply fall back to the base's art at runtime
 *      (package.c per-pose clone delegation).
 *
 * Identity check: copying ref/* to out/* and ingesting must reproduce
 * the base wrestler pixel-exactly (the acceptance test). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <png.h>
#include "../src/engine.h"
#include "../src/tbl.h"
#include "../src/json.h"
#include "stream_decode.h"

const uint8_t *wf_video_tile_pens(unsigned t);
int wf_video_load_rgba_png(const char *path, uint8_t **out, int *w, int *h);

/* the shared 81-row sprite table (sprite.c reads the same addresses
 * through the data layer) — needed for the AISLE walker rows 0x40+id */
#define AJ_TAB_META    0x38F14u
#define AJ_TAB_STREAM  0x38FB8u
#define AISLE_POSE0    768u   /* ref/pose_0768+cell = walkout cells 0..7 */

#define CANVAS 256                 /* width; the row stride everywhere */
#define CANVAS_H 320               /* height (2026-08-26): origin row 176 leaves 144 px below the feet line - 299 ROM poses reach below the old 256 */
#define ORG_X  128
#define ORG_Y  176
#define ART_SPLIT_MIN_IOU 0.7   /* same-holder split vs sibling footprint: below = mirror variant (Hogan: mean 0.89) */

/* ---- shared: render one pose onto a pen canvas (pose_strip math) ----
 * keep[] (optional) selects records — the victim's cells inside a
 * holder's two-man frame; returns 1 if any record was drawn. */
static int render_pens_mask(unsigned id, unsigned pose, int prow, const uint8_t *keep, uint8_t *cv)
{
    const eng_pkg_rec *pr;
    int n = eng_pkg_pose(id, pose, 0, prow, &pr), any = 0;
    if (n <= 0) return 0;
    memset(cv, 0, CANVAS * CANVAS_H);
    for (int i = 0; i < n; i++) {
        int x = pr[i].x + ORG_X, y = pr[i].y + 0x80;
        int xpos = x & 0x1FF, ypos;
        unsigned chain = pr[i].chain & 7u;
        if (keep && !keep[i]) continue;
        any = 1;
        if (xpos > 512 - 16) xpos -= 512;
        ypos = ((256 - y) & 0x1FF) - 16;
        for (unsigned c = 0; c <= chain; c++) {
            const uint8_t *t = wf_video_tile_pens((unsigned)(pr[i].tile + c));
            int dy = pr[i].flipy ? ypos - (int)(16 * chain) + (int)(16 * c) : ypos - (int)(16 * c);
            if (!t) continue;
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int qx = pr[i].flipx ? 15 - px : px, qy = pr[i].flipy ? 15 - py : py;
                uint8_t pen = t[qy * 16 + qx];
                int ox = xpos + px, oy = dy + py + (ORG_Y - 112);
                if (pen && ox >= 0 && ox < CANVAS && oy >= 0 && oy < CANVAS_H)
                    cv[oy * CANVAS + ox] = pen;
            }
        }
    }
    return any;
}
/* single-figure mannequin: BODY ONLY — overlay-tagged cells (weapons,
 * ropes; wrestlers/BB/overlay.json from the editor tagger) are left out
 * so the generated skin never has to draw furniture */
int art_body_canvas(unsigned base, unsigned pose, uint8_t *cv);   /* export_skin.c */
/* the EXACT weapon tile set (data/weapons/weapons.json) - the ROM's tile space
 * interleaves wrestler and weapon tiles, so a range test (2026-08-26, first
 * version) rejected Hogan's own dizzy frames at ids 11384-11416 as
 * "contaminated"; only the listed tiles are weapons */
static int weapon_tile(unsigned t)
{
    static uint8_t *set; static int loaded;
    if (!loaded) {
        char err[128]; json_val *d = json_parse_file("data/weapons/weapons.json", err, sizeof err);
        loaded = 1; set = calloc(0x40000, 1);
        if (d && set) {
            const json_val *types = json_get(d, "types");
            for (const json_val *ty = types ? types->child : NULL; ty; ty = ty->next)
                for (const json_val *po = json_get(ty, "poses") ? json_get(ty, "poses")->child : NULL; po; po = po->next)
                    for (const json_val *c = po->child; c; c = c->next) {
                        unsigned tile = (unsigned)json_int(json_get(c, "tile"), 0), chain = (unsigned)json_int(json_get(c, "chain"), 0);
                        for (unsigned k = 0; k <= chain && tile + k < 0x40000; k++) set[tile + k] = 1;
                    }
        }
        if (d) json_free(d);
    }
    return set && t < 0x40000 && set[t];
}
static int render_pose_pens(unsigned base, unsigned pose, uint8_t *cv)
{
    static uint8_t keep[192];
    const uint8_t *ovl = eng_pkg_overlay(base, pose);
    const eng_pkg_rec *pr; int n, own_n = eng_pkg_own_n(base, pose);
    n = eng_pkg_pose(base, pose, 0, -1, &pr);
    if (n <= 0 || n > 192) return 0;
    /* The list may be the COMPOSED one (a skin-layout package splices the
     * template's weapon cells back in as ENG_SRC_OVERLAY, palette nibble
     * 15 kept) — the overlay tags index the package's OWN list, which for
     * such a package holds no weapon cells at all. So the nibble is the
     * rule (15 weapon / 14 rope, the ROM's own marking); the tags only add
     * an editor override when they line up with the list. 2026-08-26: the
     * in-place --stock-skins re-export baked the box/steps into 18 frames
     * because the tags were NULL and the composed list went through. */
    for (int i = 0; i < n; i++) {
        unsigned nib = pr[i].pal & 0x0Fu, t = pr[i].tile;
        keep[i] = nib != 15u && nib != 14u && !(ovl && own_n == n && ovl[i]);
        /* CONTAMINATION GUARD: a body cell drawing a weapon-bank tile means
         * the source package was ingested from frames that had the steps /
         * box baked in (the 2026-08-26 loop: export from a dirty pak ->
         * ingest -> dirtier pak). Refuse rather than propagate; the clean
         * source is `WF_PACK_ROMART=1 ./build.sh --no-bump` then re-export. */
        if (keep[i] && weapon_tile(t)) {
            fprintf(stderr, "art: wrestler %u pose %u: body cell draws weapon-bank tile %u - the source pak is contaminated (weapon baked into its frames); repack the ROM-layout art first\n", base, pose, t);
            return 0;
        }
    }
    return render_pens_mask(base, pose, -1, keep, cv);
}
int art_body_canvas(unsigned base, unsigned pose, uint8_t *cv) { return render_pose_pens(base, pose, cv); }
/* the VICTIM's cells (pal nibble == victim) inside holder row's two-man
 * frame for `pose`; 0 when the holder has no variant for that victim */
int art_victim_src = -1;           /* whose cells the last art_victim_canvas drew (the ALT body on a mirror hole):
                                      paint them with THAT wrestler's palette, not the victim's (user 2026-08-28:
                                      1072-1076 came out brown/yellow = another man's pens in Hogan's colours) */
int art_victim_canvas(unsigned row, unsigned pose, unsigned victim, uint8_t *cv)
{
    static uint8_t vm[192];
    art_victim_src = (int)victim;
    if (row == victim && (!eng_pkg_has_part(row, pose, (int)victim) || !eng_pkg_mirror_ok(row, pose))) {
        /* MIRROR HOLE (no self variant in the ROM): the nearest held body's
           cells are the mannequin — the generator repaints them anyway */
        int alt = eng_pkg_alt_victim(row, pose, victim);
        if (alt < 0) return 0;
        victim = (unsigned)alt; art_victim_src = alt;
    }
    if (eng_pkg_victim_mask(row, pose, 0, victim, vm, 192) <= 0) return 0;
    return render_pens_mask(row, pose, (int)victim, vm, cv);
}
/* SELF-CHECK for a holder-subtraction split (row == victim's base): the
 * leftover cells' 16x16 footprint must coincide with a sibling victim's
 * footprint (same drawing, other paint). Returns the best IoU over all
 * sibling variants (a plain-costume sibling beats a tasselled one), or
 * -1 when there is nothing to compare. Below ART_SPLIT_MIN_IOU = a MIRROR
 * VARIANT: the ROM drew the self-victim elsewhere (68 across the roster,
 * all inspected 2026-08-25 — legibility for same-colour overlap, not a
 * bad split); kept as distinct template frames. */
static void cell_footprint(const eng_pkg_rec *pr, int n, const uint8_t *keep, uint8_t *fp)
{
    memset(fp, 0, CANVAS * CANVAS_H);
    for (int i = 0; i < n; i++) {
        int x = (pr[i].x + ORG_X) & 0x1FF, y0;
        unsigned chain = pr[i].chain & 7u;
        if (keep && !keep[i]) continue;
        if (x > 512 - 16) x -= 512;
        y0 = (((256 - (pr[i].y + 0x80)) & 0x1FF) - 16) + (ORG_Y - 112);
        for (unsigned c = 0; c <= chain; c++) {
            int y = y0 - (int)(16 * c);
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int ox = x + px, oy = y + py;
                if (ox >= 0 && ox < CANVAS && oy >= 0 && oy < CANVAS_H) fp[oy * CANVAS + ox] = 1;
            }
        }
    }
}
double art_split_check(unsigned row, unsigned pose, unsigned victim)
{
    static uint8_t vm[192], sm[192], fa[CANVAS * CANVAS_H], fb[CANVAS * CANVAS_H];
    const eng_pkg_rec *pr, *sr; int n, sn; double best = -1;
    if (row != victim) return -1;
    if (eng_pkg_victim_mask(row, pose, 0, victim, vm, 192) <= 0) return -1;
    n = eng_pkg_pose(row, pose, 0, (int)victim, &pr);
    if (n <= 0) return -1;
    cell_footprint(pr, n, vm, fa);
    for (unsigned sib = 0; sib < ENG_WS_MAX; sib++) {
        unsigned inter = 0, uni = 0;
        if (sib == row || eng_pkg_victim_mask(row, pose, 0, sib, sm, 192) <= 0) continue;
        sn = eng_pkg_pose(row, pose, 0, (int)sib, &sr);
        if (sn <= 0) continue;
        cell_footprint(sr, sn, sm, fb);
        for (int i = 0; i < CANVAS * CANVAS_H; i++) { inter += fa[i] & fb[i]; uni += fa[i] | fb[i]; }
        if (uni && (double)inter / uni > best) best = (double)inter / uni;
    }
    return best;
}
uint64_t art_canvas_hash(const uint8_t *cv)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < CANVAS * CANVAS_H; i++) { h ^= cv[i]; h *= 1099511628211ull; }
    return h;
}

/* AISLE walkout cell (docs/ai-art-pipeline.md): the shared sprite row
 * 0x40+base, cells 0..7 (back-view walker) -> ref pose 768+cell, decoded
 * with the engine's thinker exactly like sprite.c. 0 = no such cell. */
int art_aisle_canvas(unsigned base, unsigned cell, uint8_t *cv)
{
    unsigned arow = 0x40u + base, off_tab, pose_off;
    uint32_t sbase;
    static WfThinkerSpr spr[WF_THINKER_MAX_SPR];
    int n;
    if (base >= 12 || cell >= 8) return 0;
    if (arow == 0x4Bu) arow = 0x4Au;    /* aisle.c: Crush walks as Smash */
    off_tab = (unsigned)((tbl_ra8(AJ_TAB_META + arow * 2u) << 8) | tbl_ra8(AJ_TAB_META + arow * 2u + 1u));
    sbase = ((uint32_t)((tbl_ra8(AJ_TAB_STREAM + arow * 4u) << 8) | tbl_ra8(AJ_TAB_STREAM + arow * 4u + 1u)) << 16)
          |  (uint32_t)((tbl_ra8(AJ_TAB_STREAM + arow * 4u + 2u) << 8) | tbl_ra8(AJ_TAB_STREAM + arow * 4u + 3u));
    if (!off_tab || !sbase) return 0;
    pose_off = (unsigned)((tbl_ra8(AJ_TAB_META + off_tab + cell * 2u) << 8) | tbl_ra8(AJ_TAB_META + off_tab + cell * 2u + 1u));
    if (pose_off == 0xFFFEu || pose_off == 0xFFFFu) return 0;
    wf_thinker_set_partner_row(-1);
    n = wf_thinker_decode_obj(sbase, pose_off, 0, 0, 0, (uint16_t)arow, (uint16_t)cell, spr, WF_THINKER_MAX_SPR);
    if (n <= 0) return 0;
    memset(cv, 0, CANVAS * CANVAS_H);
    for (int i = 0; i < n; i++) {
        int x = spr[i].x + ORG_X, y = spr[i].y + 0x80;
        int xpos = x & 0x1FF, ypos;
        unsigned chain = spr[i].chain & 7u;
        if (xpos > 512 - 16) xpos -= 512;
        ypos = ((256 - y) & 0x1FF) - 16;
        for (unsigned c = 0; c <= chain; c++) {
            const uint8_t *t = wf_video_tile_pens((unsigned)(spr[i].tile + c));
            int dy = spr[i].flipy ? ypos - (int)(16 * chain) + (int)(16 * c) : ypos - (int)(16 * c);
            if (!t) continue;
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int qx = spr[i].flipx ? 15 - px : px, qy = spr[i].flipy ? 15 - py : py;
                uint8_t pen = t[qy * 16 + qx];
                int ox = xpos + px, oy = dy + py + (ORG_Y - 112);
                if (pen && ox >= 0 && ox < CANVAS && oy >= 0 && oy < CANVAS_H)
                    cv[oy * CANVAS + ox] = pen;
            }
        }
    }
    return 1;
}

int wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H);
static int write_rgba_png(const char *path, const uint8_t *rgba, int W, int H)
{
    return wf_art_write_rgba_png(path, rgba, W, H);
}
int wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H)
{
    /* ATOMIC: write a temp file and rename — the editor polls these PNGs
       ~57x/s and half-written files spammed "libpng error: Read Error" */
    char tmp[1024];
    FILE *f;
    png_structp png; png_infop info; png_bytep *rp;
    snprintf(tmp, sizeof tmp, "%s.tmp~", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) { fclose(f); unlink(tmp); return -1; }
    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)W, (png_uint_32)H, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    rp = malloc(sizeof(png_bytep) * (size_t)H);
    for (int y = 0; y < H; y++) rp[y] = (png_bytep)(rgba + (size_t)y * (size_t)W * 4u);
    png_set_rows(png, info, rp);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, 0);
    png_destroy_write_struct(&png, &info);
    free(rp); fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int tool_art_job(int base, const char *jobdir)
{
    char path[600];
    const uint16_t *pens = eng_pkg_palette((unsigned)base);
    static uint8_t cv[CANVAS * CANVAS_H];
    static uint8_t rgba[CANVAS * CANVAS_H * 4];
    FILE *mf;
    int nposes = 0;

    if (base < 0 || base >= 12 || !pens) { fprintf(stderr, "art-job: base 0..11\n"); return 1; }
    mkdir(jobdir, 0775);
    snprintf(path, sizeof path, "%s/ref", jobdir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/out", jobdir); mkdir(path, 0775);

    snprintf(path, sizeof path, "%s/manifest.json", jobdir);
    mf = fopen(path, "w");
    if (!mf) return 1;
    fprintf(mf, "{ \"base\": %d, \"canvas\": %d, \"origin\": [%d, %d],\n"
                "  \"contract\": \"restyle ref/pose_NNNN.png -> out/pose_NNNN.png; SAME size, SAME origin (the feet line), SAME silhouette; transparent background; missing outputs fall back to the base art\",\n"
                "  \"poses\": [", base, CANVAS, ORG_X, ORG_Y);
    for (unsigned pose = 0; pose < 0x400; pose++) {
        if (!render_pose_pens((unsigned)base, pose, cv)) continue;
        {   /* WF_ARTDBG=csv-of-pose-ids: dump the raw records */
            const char *dbg = getenv("WF_ARTDBG");
            if (dbg) {
                char pat[16]; snprintf(pat, sizeof pat, ",%u,", pose);
                char buf[512]; snprintf(buf, sizeof buf, ",%s,", dbg);
                if (strstr(buf, pat)) {
                    const eng_pkg_rec *pr;
                    int n = eng_pkg_pose((unsigned)base, pose, 0, -1, &pr);
                    fprintf(stderr, "artdbg pose %u: %d recs\n", pose, n);
                    for (int k = 0; k < n; k++)
                        fprintf(stderr, "  [%2d] x=%4d y=%4d tile=0x%05X chain=%u fx=%u fy=%u pal=%u\n",
                                k, pr[k].x, pr[k].y, pr[k].tile, pr[k].chain,
                                pr[k].flipx, pr[k].flipy, pr[k].pal);
                }
            }
        }
        for (int i = 0; i < CANVAS * CANVAS_H; i++) {
            uint8_t p = cv[i];
            uint16_t w = p ? pens[p] : 0;
            rgba[i * 4 + 0] = (uint8_t)((w & 0x0F) * 17);
            rgba[i * 4 + 1] = (uint8_t)(((w >> 4) & 0x0F) * 17);
            rgba[i * 4 + 2] = (uint8_t)(((w >> 8) & 0x0F) * 17);
            rgba[i * 4 + 3] = p ? 255 : 0;
        }
        snprintf(path, sizeof path, "%s/ref/pose_%04u.png", jobdir, pose);
        if (write_rgba_png(path, rgba, CANVAS, CANVAS_H)) { fclose(mf); return 1; }
        fprintf(mf, "%s%u", nposes ? "," : "", pose);
        nposes++;
    }
    /* AISLE walkout cells (docs/ai-art-pipeline.md): the shared sprite
     * row 0x40+base, cells 0..7 (back-view walker) -> ref pose 768+cell,
     * decoded with the engine's thinker exactly like sprite.c. Palette:
     * the body pens (the refs get desaturated for generation anyway). */
    for (unsigned cell = 0; cell < 8; cell++) {
        if (!art_aisle_canvas((unsigned)base, cell, cv)) continue;
        for (int k = 0; k < CANVAS * CANVAS_H; k++) {
            uint8_t p = cv[k];
            uint16_t w = p ? pens[p] : 0;
            rgba[k * 4 + 0] = (uint8_t)((w & 0x0F) * 17);
            rgba[k * 4 + 1] = (uint8_t)(((w >> 4) & 0x0F) * 17);
            rgba[k * 4 + 2] = (uint8_t)(((w >> 8) & 0x0F) * 17);
            rgba[k * 4 + 3] = p ? 255 : 0;
        }
        snprintf(path, sizeof path, "%s/ref/pose_%04u.png", jobdir, AISLE_POSE0 + cell);
        if (write_rgba_png(path, rgba, CANVAS, CANVAS_H)) { fclose(mf); return 1; }
        fprintf(mf, "%s%u", nposes ? "," : "", AISLE_POSE0 + cell);
        nposes++;
    }
    fprintf(mf, "] }\n");
    fclose(mf);
    /* VICTIM-BODY refs (user 2026-08-25: composite artwork dies): for
       every (holder row, pose) whose two-man variant can hold THIS base,
       render ONLY the victim cells (pal == base) — the contorted body the
       AI must reskin so pins/slams ON the clone wear his art. Saved as
       synthetic pose ids ENG_VICT_POSE0+k (the generic pipeline treats
       them like any frame); victmap.json records k -> (holder, pose). */
    {
        FILE *vm; unsigned vidx = 0, nsuspect = 0, suspects[256]; int suspect;
        snprintf(path, sizeof path, "%s/victmap.json", jobdir);
        vm = fopen(path, "w");
        if (vm) fprintf(vm, "{ \"base\": %d, \"first\": %u, \"entries\": [", base, ENG_VICT_POSE0);
        for (unsigned row = 0; row < 12 && vm; row++) {
            /* holder == base (both bodies in one palette) is split by holder
               subtraction inside eng_pkg_victim_mask — no longer skipped */
            for (unsigned pose = 0; pose < 0x400; pose++) {
                if (!eng_pkg_has_part(row, pose, base) && !(row == (unsigned)base && eng_pkg_alt_victim(row, pose, (unsigned)base) >= 0)) continue;
                if (!art_victim_canvas(row, pose, (unsigned)base, cv)) continue;
                suspect = 0;
                if (row == (unsigned)base) {   /* holder-subtraction split: self-check */
                    double iou = art_split_check(row, pose, (unsigned)base);
                    if (iou >= 0 && iou < ART_SPLIT_MIN_IOU) {
                        fprintf(stderr, "art-job: MIRROR variant holder %u pose %u (self-victim footprint IoU %.2f vs best sibling: a distinct drawing)\n", row, pose, iou);
                        if (nsuspect < 256) suspects[nsuspect] = pose;
                        nsuspect++; suspect = 1;
                    }
                }
                {   /* DEDUPE: many holds share a pixel-identical victim
                       body (716 refs = only 284 unique, playtest) —
                       identical canvases share ONE ref/id; the map lists
                       every (holder, pose) that uses it */
                    static uint64_t vhash[1024];
                    uint64_t h = art_canvas_hash(cv);
                    int dup = -1;
                    for (unsigned k = 0; k < vidx && k < 1024; k++)
                        if (vhash[k] == h) { dup = (int)k; break; }
                    if (dup >= 0) {
                        fprintf(vm, ",\n  [%u, %u, %u%s]", ENG_VICT_POSE0 + (unsigned)dup, row, pose, suspect ? ", 1" : "");
                        continue;
                    }
                    if (vidx < 1024) vhash[vidx] = h;
                }
                for (int i = 0; i < CANVAS * CANVAS_H; i++) {
                    uint8_t pn2 = cv[i];
                    uint16_t w = pn2 ? pens[pn2] : 0;
                    rgba[i * 4 + 0] = (uint8_t)((w & 0x0F) * 17);
                    rgba[i * 4 + 1] = (uint8_t)(((w >> 4) & 0x0F) * 17);
                    rgba[i * 4 + 2] = (uint8_t)(((w >> 8) & 0x0F) * 17);
                    rgba[i * 4 + 3] = pn2 ? 255 : 0;
                }
                snprintf(path, sizeof path, "%s/ref/pose_%04u.png", jobdir, ENG_VICT_POSE0 + vidx);
                if (write_rgba_png(path, rgba, CANVAS, CANVAS_H) == 0) {
                    fprintf(vm, "%s\n  [%u, %u, %u%s]", vidx ? "," : "", ENG_VICT_POSE0 + vidx, row, pose, suspect ? ", 1" : "");
                    vidx++; nposes++;
                }
            }
        }
        if (vm) {   /* entries: [ref id, holder row, holder pose(, 1 = mirror variant)] */
            fprintf(vm, "\n],\n  \"mirror_note\": \"4th element 1 = MIRROR VARIANT: the holder==base victim is a distinct drawing from the cross-palette victims (footprint IoU < %.1f) — the ROM re-posed him so two same-colour bodies read apart; kept as its own template frame (user 2026-08-25), shown only when holder and victim share a base\",\n  \"mirror_poses\": [", ART_SPLIT_MIN_IOU);
            for (unsigned k = 0; k < nsuspect && k < 256; k++) fprintf(vm, "%s%u", k ? ", " : "", suspects[k]);
            fprintf(vm, "] }\n"); fclose(vm);
        }
        fprintf(stderr, "art-job: %u victim-body refs (victmap.json), %u mirror variants\n", vidx, nsuspect);
    }
    snprintf(path, sizeof path, "%s/prompt.txt", jobdir);
    if (!(mf = fopen(path, "r"))) {          /* template only if absent */
        mf = fopen(path, "w");
        if (mf) {
            fprintf(mf, "Describe your wrestler here (build, skin tone, hair, outfit, colours).\n"
                        "The generator receives this prompt + your source image + each ref frame.\n");
            fclose(mf);
        }
    } else fclose(mf);
    fprintf(stderr, "art-job: %d ref frames -> %s/ref (origin %d,%d; fill %s/out and run --art-ingest)\n",
            nposes, jobdir, ORG_X, ORG_Y, jobdir);
    return 0;
}

/* ---- generic row/cell render: wfengine --row-cell ROW CELL PALID OUT ----
 * Renders one cell of any of the 81 shared sprite rows (aisle walkers,
 * interlude portraits, heralds...) onto the 256 canvas with wrestler
 * PALID's body pens — a REF/inspection tool (docs/ai-art-pipeline.md). */
/* one cell of a shared sprite row rendered straight to RGBA in its OWN
 * baked palettes — each record carries a bank byte (ROM body-palette entry
 * 0x2F22 + pal*32; a face card mixes the frame's bank and the face's) —
 * the ref for a skin's portrait surfaces (continue faces, title card).
 * 0 = absent. */
#define AJ_ROM_BODY 0x2F22u
int art_row_cell_rgba(unsigned row, unsigned cell, uint8_t *rgba)
{
    static WfThinkerSpr spr[WF_THINKER_MAX_SPR];
    unsigned off_tab, pose_off; uint32_t sbase; int n;
    if (row >= 81) return 0;
    off_tab = (unsigned)((tbl_ra8(AJ_TAB_META + row * 2u) << 8) | tbl_ra8(AJ_TAB_META + row * 2u + 1u));
    sbase = ((uint32_t)((tbl_ra8(AJ_TAB_STREAM + row * 4u) << 8) | tbl_ra8(AJ_TAB_STREAM + row * 4u + 1u)) << 16)
          |  (uint32_t)((tbl_ra8(AJ_TAB_STREAM + row * 4u + 2u) << 8) | tbl_ra8(AJ_TAB_STREAM + row * 4u + 3u));
    if (!off_tab || !sbase) return 0;
    pose_off = (unsigned)((tbl_ra8(AJ_TAB_META + off_tab + cell * 2u) << 8) | tbl_ra8(AJ_TAB_META + off_tab + cell * 2u + 1u));
    if (pose_off == 0xFFFEu || pose_off == 0xFFFFu) return 0;
    wf_thinker_set_partner_row(-1);
    n = wf_thinker_decode_obj(sbase, pose_off, 0, 0, 0, (uint16_t)row, (uint16_t)cell, spr, WF_THINKER_MAX_SPR);
    if (n <= 0) return 0;
    memset(rgba, 0, (size_t)CANVAS * CANVAS_H * 4);
    for (int i = 0; i < n; i++) {
        int x = spr[i].x + ORG_X, y = spr[i].y + 0x80;
        int xpos = x & 0x1FF, ypos;
        unsigned chain = spr[i].chain & 7u, palb = spr[i].pal & 0xFFu;
        uint16_t pens[16];
        for (int k = 0; k < 16; k++) pens[k] = (uint16_t)((tbl_ra8(AJ_ROM_BODY + palb * 32u + (unsigned)k * 2u) << 8) | tbl_ra8(AJ_ROM_BODY + palb * 32u + (unsigned)k * 2u + 1u));
        if (xpos > 512 - 16) xpos -= 512;
        ypos = ((256 - y) & 0x1FF) - 16;
        for (unsigned c = 0; c <= chain; c++) {
            const uint8_t *t = wf_video_tile_pens((unsigned)(spr[i].tile + c));
            int dy = spr[i].flipy ? ypos - (int)(16 * chain) + (int)(16 * c) : ypos - (int)(16 * c);
            if (!t) continue;
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int qx = spr[i].flipx ? 15 - px : px, qy = spr[i].flipy ? 15 - py : py;
                uint8_t pen = t[qy * 16 + qx];
                int ox = xpos + px, oy = dy + py + (ORG_Y - 112);
                uint8_t *d;
                if (!pen || ox < 0 || ox >= CANVAS || oy < 0 || oy >= CANVAS_H) continue;
                d = rgba + ((size_t)oy * CANVAS + ox) * 4;
                d[0] = (uint8_t)((pens[pen] & 0x0F) * 17); d[1] = (uint8_t)(((pens[pen] >> 4) & 0x0F) * 17); d[2] = (uint8_t)(((pens[pen] >> 8) & 0x0F) * 17); d[3] = 255;
            }
        }
    }
    return 1;
}
int tool_row_cell(unsigned row, unsigned cell, unsigned palid, const char *out)
{
    static uint8_t cv[CANVAS * CANVAS_H];
    static uint8_t rgba[CANVAS * CANVAS_H * 4];
    static WfThinkerSpr spr[WF_THINKER_MAX_SPR];
    const uint16_t *pens = eng_pkg_palette(palid);
    unsigned off_tab, pose_off;
    uint32_t sbase;
    int n;
    if (row >= 81 || !pens) { fprintf(stderr, "row-cell: row 0..80, palid 0..11\n"); return 1; }
    off_tab = (unsigned)((tbl_ra8(AJ_TAB_META + row * 2u) << 8) | tbl_ra8(AJ_TAB_META + row * 2u + 1u));
    sbase = ((uint32_t)((tbl_ra8(AJ_TAB_STREAM + row * 4u) << 8) | tbl_ra8(AJ_TAB_STREAM + row * 4u + 1u)) << 16)
          |  (uint32_t)((tbl_ra8(AJ_TAB_STREAM + row * 4u + 2u) << 8) | tbl_ra8(AJ_TAB_STREAM + row * 4u + 3u));
    if (!off_tab || !sbase) { fprintf(stderr, "row-cell: row %u has no table\n", row); return 1; }
    pose_off = (unsigned)((tbl_ra8(AJ_TAB_META + off_tab + cell * 2u) << 8)
                        | tbl_ra8(AJ_TAB_META + off_tab + cell * 2u + 1u));
    if (pose_off == 0xFFFEu || pose_off == 0xFFFFu) { fprintf(stderr, "row-cell: row %u cell %u absent\n", row, cell); return 1; }
    wf_thinker_set_partner_row(-1);
    n = wf_thinker_decode_obj(sbase, pose_off, 0, 0, 0, (uint16_t)row, (uint16_t)cell, spr, WF_THINKER_MAX_SPR);
    if (n <= 0) { fprintf(stderr, "row-cell: decode empty\n"); return 1; }
    memset(cv, 0, sizeof cv);
    for (int i = 0; i < n; i++) {
        int x = spr[i].x + ORG_X, y = spr[i].y + 0x80;
        int xpos = x & 0x1FF, ypos;
        unsigned chain = spr[i].chain & 7u;
        if (xpos > 512 - 16) xpos -= 512;
        ypos = ((256 - y) & 0x1FF) - 16;
        for (unsigned c = 0; c <= chain; c++) {
            const uint8_t *t = wf_video_tile_pens((unsigned)(spr[i].tile + c));
            int dy = spr[i].flipy ? ypos - (int)(16 * chain) + (int)(16 * c) : ypos - (int)(16 * c);
            if (!t) continue;
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int qx = spr[i].flipx ? 15 - px : px, qy = spr[i].flipy ? 15 - py : py;
                uint8_t pen = t[qy * 16 + qx];
                int ox = xpos + px, oy = dy + py + (ORG_Y - 112);
                if (pen && ox >= 0 && ox < CANVAS && oy >= 0 && oy < CANVAS_H)
                    cv[oy * CANVAS + ox] = pen;
            }
        }
    }
    for (int k = 0; k < CANVAS * CANVAS_H; k++) {
        uint8_t p = cv[k];
        uint16_t w = p ? pens[p] : 0;
        rgba[k * 4 + 0] = (uint8_t)((w & 0x0F) * 17);
        rgba[k * 4 + 1] = (uint8_t)(((w >> 4) & 0x0F) * 17);
        rgba[k * 4 + 2] = (uint8_t)(((w >> 8) & 0x0F) * 17);
        rgba[k * 4 + 3] = p ? 255 : 0;
    }
    if (write_rgba_png(out, rgba, CANVAS, CANVAS_H)) return 1;
    fprintf(stderr, "row-cell: row %u cell %u (%d recs) -> %s\n", row, cell, n, out);
    return 0;
}

/* ---- ingest ---- */
typedef struct { uint8_t px[256]; } Tile;

static int tile_eq(const Tile *a, const Tile *b) { return memcmp(a->px, b->px, 256) == 0; }
/* the loaded sprite tile set (gfx pak: the ROM's 0..0xFFFF), hashed once */
#define RT_HASH (1u << 17)
static uint32_t *rt_slot;                 /* tile id + 1, 0 = empty */
static uint32_t tile_hash(const uint8_t *px)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 256; i++) { h ^= px[i]; h *= 16777619u; }
    return h;
}
static int rom_tile_find(const Tile *t)
{
    uint32_t h;
    if (!rt_slot) {
        rt_slot = calloc(RT_HASH, sizeof *rt_slot);
        if (!rt_slot) return -1;
        for (unsigned id = 1; id < 0x10000u; id++) {
            const uint8_t *px = wf_video_tile_pens(id); int blank = 1;
            for (int i = 0; i < 256 && blank; i++) if (px[i]) blank = 0;
            if (blank) continue;
            for (uint32_t k = tile_hash(px) & (RT_HASH - 1); ; k = (k + 1) & (RT_HASH - 1))
                if (!rt_slot[k]) { rt_slot[k] = id + 1; break; }
                else if (memcmp(wf_video_tile_pens(rt_slot[k] - 1), px, 256) == 0) break;   /* duplicate ROM tile: first id wins */
        }
    }
    h = tile_hash(t->px);
    for (uint32_t k = h & (RT_HASH - 1); rt_slot[k]; k = (k + 1) & (RT_HASH - 1))
        if (memcmp(wf_video_tile_pens(rt_slot[k] - 1), t->px, 256) == 0) return (int)(rt_slot[k] - 1);
    return -1;
}

/* palette: most-frequent colours with a DIVERSITY floor — raw frequency
 * spent all 15 pens on near-identical suit shades and left no skin tone
 * (the in-game blue-face bug). Each new pen must sit >= mindist from
 * every picked pen; the floor relaxes if the image genuinely has fewer
 * distinct colours. hist is CONSUMED. Returns the pen count. */
static int build_pal(uint32_t *hist, uint16_t *pal)
{
    int mindist = 8, npal;               /* 4-bit space, sum of squares */
    pal[0] = 0; npal = 1;
    while (npal < 16 && mindist >= 0) {
        uint32_t best = 0; int bi = -1;
        for (int c = 0; c < 4096; c++) {
            int ok = 1;
            if (!hist[c] || hist[c] <= best) continue;
            for (int k = 1; k < npal && ok; k++) {
                int dr = (c & 15) - (pal[k] & 15);
                int dg = ((c >> 4) & 15) - ((pal[k] >> 4) & 15);
                int db = ((c >> 8) & 15) - ((pal[k] >> 8) & 15);
                if (dr * dr + dg * dg + db * db < mindist) ok = 0;
            }
            if (ok) { best = hist[c]; bi = c; }
        }
        if (bi < 0) { mindist -= 4; continue; }   /* relax when exhausted */
        pal[npal++] = (uint16_t)bi;
        hist[bi] = 0;
    }
    return npal;
}
/* PORTRAIT surfaces (ids 800-807 continue faces, 810 title card): full
 * pictures with their own 16-colour palette each — they never share the
 * body's 15 pens (a crowd wall would eat the skin tones) */
static int is_portrait_id(unsigned pose) { return pose >= 800 && pose < 1024; }

static int nearest_pen(const uint8_t *p, const uint16_t *pal, int npal)
{
    int br = p[0] / 17, bg = p[1] / 17, bb = p[2] / 17, bestd = 1 << 30, bp = 1;
    for (int k = 1; k < npal; k++) {
        int r = pal[k] & 15, g = (pal[k] >> 4) & 15, b = (pal[k] >> 8) & 15;
        int d = (r - br) * (r - br) + (g - bg) * (g - bg) + (b - bb) * (b - bb);
        if (d < bestd) { bestd = d; bp = k; }
    }
    return bp;
}
/* HI-RES quantise (user 2026-08-26, bridging the gap to stock): when the run
 * archived the placed figure at 4x (out_hi/), quantise THAT to the skin's
 * pens and shrink by MAJORITY VOTE per 1x pixel - flat regions stay flat,
 * outlines stay continuous, no muddy in-between colours from averaging
 * first. Returns 1 and fills q (CANVAS^2 pens) when the archive exists. */
#define HI 4
static int quant_hi(const char *jobdir, unsigned pose, const uint16_t *pal, int npal, uint8_t *q, int win_y)
{
    char path[700]; uint8_t *hi; int w, h;
    snprintf(path, sizeof path, "%s/out_hi/pose_%04u.png", jobdir, pose);
    if (wf_video_load_rgba_png(path, &hi, &w, &h) || w != CANVAS * HI || h != CANVAS * HI) { if (w == CANVAS * HI) free(hi); return 0; }
    memset(q, 0, (size_t)CANVAS * CANVAS_H);
    for (int yw = 0; yw < CANVAS; yw++)      /* the archive covers the 256-row WINDOW at win_y */
        for (int x = 0; x < CANVAS; x++) {
            int y = yw + win_y;
            if (y >= CANVAS_H) break;
            int cnt[16] = {0}, cover = 0, best = 0, bestc = 0;
            for (int yy = 0; yy < HI; yy++)
                for (int xx = 0; xx < HI; xx++) {
                    const uint8_t *p = hi + ((size_t)(yw * HI + yy) * w + x * HI + xx) * 4;
                    if (p[3] < 128) continue;
                    cover++; cnt[nearest_pen(p, pal, npal)]++;
                }
            q[y * CANVAS + x] = 0;
            if (cover * 2 < HI * HI) continue;
            for (int k = 1; k < 16; k++) if (cnt[k] > bestc) { bestc = cnt[k]; best = k; }
            q[y * CANVAS + x] = (uint8_t)best;
        }
    free(hi);
    return 1;
}
/* SILHOUETTE OUTLINE (stock look): every opaque pixel on the silhouette
 * edge takes the darkest pen of ITS OWN hue (a lit material's shadow pen),
 * unless it is already dark. Materials with no darker pen (a white boot in
 * a palette without grey) are left alone. */
static void outline_pass(uint8_t *q, const uint16_t *pal)
{
    static uint8_t q2[CANVAS * CANVAS_H];
    memcpy(q2, q, sizeof q2);
    for (int y = 1; y < CANVAS_H - 1; y++)
        for (int x = 1; x < CANVAS - 1; x++) {
            uint8_t me = q2[y * CANVAS + x]; unsigned mw; int mr, mg, mb, mbright, best = -1, bestscore = 1 << 30;
            if (!me) continue;
            if (q2[y * CANVAS + x + 1] && q2[y * CANVAS + x - 1] && q2[(y + 1) * CANVAS + x] && q2[(y - 1) * CANVAS + x]) continue;
            mw = pal[me]; mr = mw & 15; mg = (mw >> 4) & 15; mb = (mw >> 8) & 15; mbright = mr + mg + mb;
            if (mbright <= 12) continue;                  /* already dark */
            for (int k = 1; k < 16; k++) {
                unsigned w2 = pal[k]; int r = w2 & 15, g = (w2 >> 4) & 15, b = (w2 >> 8) & 15, bright = r + g + b;
                int hue = (r * mg - g * mr) * (r * mg - g * mr) + (g * mb - b * mg) * (g * mb - b * mg);
                if (bright <= 3 || bright * 10 > mbright * 7) continue;   /* >= 30% darker, not black */
                if (hue + bright * 4 < bestscore) { bestscore = hue + bright * 4; best = k; }
            }
            if (best > 0) q[y * CANVAS + x] = (uint8_t)best;
        }
}
int tool_art_ingest(const char *jobdir, int slot, const char *dstdir)
{
    char path[700];
    static uint32_t hist_c[4096]; /* 12-bit colour histogram over all frames */
    static uint8_t *frames[2048]; static int have[2048]; static short winy[2048];
    int nframes = 0;
    uint16_t pal[16]; int npal = 0;
    static Tile tiles[0x6000]; static int ntile;
    /* STOCK MEN IN THE SKIN LAYOUT (user 2026-08-26): ids 0..11 ingest too -
       their arena sits above the clone arenas (0xD0000 + id*0x2000; tile
       reuse keeps it nearly empty for un-repainted art), records carry their
       own id as pal nibble; the pack keeps the ROM lists as the placement
       TEMPLATE (poses2) and ships these as "skin2" */
    unsigned arena = slot < 12 ? 0xD0000u + (unsigned)slot * 0x2000u : 0x10000u + ((unsigned)slot - 12u) * 0x6000u;
    unsigned arena_cap = slot < 12 ? 0x2000u - 0x20u : 0x5FE0u;
    json_val *tdoc = NULL;               /* the base's ROM poses.json: cell geometry template */
    FILE *pf;

    int palid = slot < 12 ? slot : 0;
    if (slot < 0 || slot >= ENG_WS_EXT_MAX) { fprintf(stderr, "art-ingest: slot 0..%d\n", ENG_WS_EXT_MAX - 1); return 1; }
    mkdir(dstdir, 0775);
    memset(hist_c, 0, sizeof hist_c); memset(have, 0, sizeof have); ntile = 0;   /* statics: fresh per call */
    for (unsigned pz = 0; pz < 2048; pz++) { free(frames[pz]); frames[pz] = NULL; }

    if (slot >= 12) {   /* records carry the BASE id in their pal nibble (sprite.c remaps
         * bank==row to the clone's borrowed bank) — read clone_of from an
         * existing wrestler.json; write it BEFORE ingesting */
        char wj[760]; char err[128];
        snprintf(wj, sizeof wj, "%s/wrestler.json", dstdir);
        {
            FILE *f = fopen(wj, "r");
            if (f) {
                char buf[256]; size_t n = fread(buf, 1, sizeof buf - 1, f);
                buf[n] = 0; fclose(f);
                const char *p = strstr(buf, "clone_of");
                if (p && (p = strchr(p, ':'))) palid = atoi(p + 1);
            } else
                fprintf(stderr, "art-ingest: NOTE no %s yet - records use pal 0 (write wrestler.json first for the right base)\n", wj);
        }
        (void)err;
    }
    {   /* CUT BY TEMPLATE (user 2026-08-26): the ROM list of the base's pose is the
           cell geometry - one 16x16 record per template tile at the template's
           position (chain tile c sits at y + 16c), so eng_compose maps each record
           onto its template cell exactly and the ROM's holder/victim interleave
           survives (a grid cut straddling body and arm sank the arm under the
           victim); pixels no template cell covers are cut on the grid after */
        char tp[256], err[128]; int base_id = slot < 12 ? slot : palid;
        snprintf(tp, sizeof tp, "data/wrestlers/%02d/poses.json", base_id);
        tdoc = json_parse_file(tp, err, sizeof err);
        if (!tdoc) fprintf(stderr, "art-ingest: no %s - cutting on the grid only\n", tp);
    }

    {   /* WINDOW offsets (2026-08-26): the generator works on a 256-row window of the
           256x320 canvas; manifest.json records win_y per pose (poses[].win_y,
           victims[].win_y) - the hi-res archive is window space */
        char mp[760], err[128]; json_val *doc;
        memset(winy, 0, sizeof winy);
        snprintf(mp, sizeof mp, "%s/manifest.json", jobdir);
        doc = json_parse_file(mp, err, sizeof err);
        if (doc) {
            for (int sec = 0; sec < 2; sec++) {
                const json_val *m = json_get(doc, sec ? "victims" : "poses");
                for (const json_val *e = m ? m->child : NULL; e; e = e->next) {
                    unsigned pz = (unsigned)atoi(e->key);
                    if (pz < 2048) winy[pz] = (short)json_int(json_get(e, "win_y"), 0);
                }
            }
            json_free(doc);
        }
    }

    /* pass 1: load every out/ frame (body poses AND the synthetic
       victim-body ids >= ENG_VICT_POSE0), build the colour histogram */
    for (unsigned pose = 0; pose < 2048; pose++) {
        uint8_t *rgba = NULL; int w = 0, h = 0;
        snprintf(path, sizeof path, "%s/out/pose_%04u.png", jobdir, pose);
        if (wf_video_load_rgba_png(path, &rgba, &w, &h) != 0) {
            /* no generated frame: the package's own frames/ set (the canonical
             * art of a stock man / a generic / a finished skin — user
             * 2026-08-26: every pak is made from frames) */
            snprintf(path, sizeof path, "%s/frames/pose_%04u.png", jobdir, pose);
            if (wf_video_load_rgba_png(path, &rgba, &w, &h) != 0) continue;
        }
        if (w == CANVAS && h < CANVAS_H) {   /* an older 256-tall frame: same origin, pad at the bottom */
            uint8_t *pad = calloc((size_t)CANVAS * CANVAS_H, 4);
            if (pad) { memcpy(pad, rgba, (size_t)CANVAS * (size_t)h * 4); free(rgba); rgba = pad; h = CANVAS_H; }
        }
        if (w != CANVAS || h != CANVAS_H) {
            fprintf(stderr, "art-ingest: %s is %dx%d (want %dx%d) - skipped\n", path, w, h, CANVAS, CANVAS_H);
            free(rgba); continue;
        }
        frames[pose] = rgba; have[pose] = 1; nframes++;
        if (is_portrait_id(pose)) continue;          /* own palette, below */
        for (int i = 0; i < CANVAS * CANVAS_H; i++) {
            const uint8_t *p = rgba + (size_t)i * 4u;
            if (p[3] < 128) continue;
            hist_c[((p[2] / 17) << 8) | ((p[1] / 17) << 4) | (p[0] / 17)]++;
        }
    }
    {   /* ALIASES (class template manifest): derive never-generated poses
           from their source frame — copy, or mirror about the origin x */
        char mp[760], err[128]; json_val *doc;
        snprintf(mp, sizeof mp, "%s/manifest.json", jobdir);
        doc = json_parse_file(mp, err, sizeof err);
        if (doc) {
            const json_val *al = json_get(doc, "aliases"); int na = 0;
            for (const json_val *e = al ? al->child : NULL; e; e = e->next) {
                unsigned p = (unsigned)atoi(e->key), of = (unsigned)json_int(json_get(e, "of"), 9999);
                int flip = (int)json_int(json_get(e, "flip"), 0);
                int dx = (int)json_int(json_get(e, "dx"), 0), dy = (int)json_int(json_get(e, "dy"), 0);
                int bx0 = CANVAS, bx1 = -1;
                if (p >= 2048 || of >= 2048 || have[p] || !have[of]) continue;
                for (int i = 0; i < CANVAS * CANVAS_H; i++)          /* source bbox x range (mirror pivot) */
                    if (frames[of][(size_t)i * 4 + 3]) { int x = i % CANVAS; if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; }
                frames[p] = malloc((size_t)CANVAS * CANVAS_H * 4);
                if (!frames[p]) continue;
                for (int y = 0; y < CANVAS_H; y++) for (int x = 0; x < CANVAS; x++) {
                    int sx = x - dx, sy = y - dy;                    /* undo the placement shift */
                    if (flip) sx = bx0 + bx1 - sx;                   /* mirror within the source bbox */
                    const uint8_t *q = (sx >= 0 && sx < CANVAS && sy >= 0 && sy < CANVAS_H) ? frames[of] + ((size_t)sy * CANVAS + sx) * 4 : NULL;
                    uint8_t *d = frames[p] + ((size_t)y * CANVAS + x) * 4;
                    if (q) memcpy(d, q, 4); else memset(d, 0, 4);
                }
                have[p] = 1; nframes++; na++;
            }
            if (na) fprintf(stderr, "art-ingest: %d aliased poses derived from their source frames (manifest)\n", na);
            json_free(doc);
        }
    }
    if (!nframes) { fprintf(stderr, "art-ingest: no out/pose_NNNN.png frames in %s\n", jobdir); return 1; }

    npal = build_pal(hist_c, pal);
    int exact_pal = 0;                  /* palette.json pens in force: no outline/retint passes */
    {   /* EXACT palette (user 2026-08-26): a skin that ships palette.json (a
           stock man exported as a skin, or a saved skin) ingests with THOSE
           pens in THAT order - no re-quantisation, so its tiles match the ROM's
           byte for byte and reuse them */
        char pp[760]; FILE *pf2;
        snprintf(pp, sizeof pp, "%s/palette.json", jobdir);
        pf2 = fopen(pp, "r");
        if (pf2) {
            char buf[512]; size_t n = fread(buf, 1, sizeof buf - 1, pf2); const char *q;
            buf[n] = 0; fclose(pf2);
            q = strstr(buf, "\"pens\"");
            if (q && (q = strchr(q, '['))) {
                int k = 0; q++;
                while (k < 16) { char *e; long v = strtol(q, &e, 10); if (e == q) break; pal[k++] = (uint16_t)v; q = e; while (*q == ',' || *q == ' ') q++; }
                if (k == 16) { npal = 16; exact_pal = 1; fprintf(stderr, "art-ingest: exact palette from %s (outline passes off: the art is already in its final pens)\n", pp); }
            }
        }
    }
    int nreuse = 0;                     /* records pointing at tiles the gfx pak already holds */
    int nhi = 0;                        /* frames quantised from the hi-res archive */

    /* victim-body map: jobs victmap.json routes synthetic ids >= 1024 to
       (holder row, holder pose) — they become the "vict" object below */
    static short vmvid[1024], vmrow[1024], vmpose[1024]; int vmn = 0;
    {
        FILE *vf;
        snprintf(path, sizeof path, "%s/victmap.json", jobdir);
        vf = fopen(path, "r");
        if (vf) {
            char line[128];
            while (fgets(line, sizeof line, vf)) {
                int vid, r2, p2;
                if (sscanf(line, " [%d, %d, %d", &vid, &r2, &p2) == 3) {   /* optional 4th = suspect flag */
                    if (vid >= 1024 && vid < 2048 && vmn < 1024) {
                        vmvid[vmn] = (short)vid;   /* ENTRY list: vids repeat
                                                      (deduped shared frames) */
                        vmrow[vmn] = (short)r2;
                        vmpose[vmn] = (short)p2;
                        vmn++;
                    }
                }
            }
            fclose(vf);
        }
    }
    {   /* pass 2: quantize + cut tiles (16-aligned grid, cross-pose dedupe) */
        FILE *pj;
        snprintf(path, sizeof path, "%s/poses.json", dstdir);
        pj = fopen(path, "w"); if (!pj) return 1;
        fprintf(pj, "{\"poses\":{\n");
        int firstpose = 1, missing = 0;
        static uint16_t cellpal[1024][16]; static uint8_t hascell[1024];
        for (unsigned pose = 0; pose < 0x400; pose++) {
            static uint8_t q[CANVAS * CANVAS_H];
            int x0 = CANVAS, x1 = -1, y0 = CANVAS_H, y1 = -1;
            const uint16_t *upal = pal; int unpal = npal;
            if (!have[pose]) { missing++; continue; }
            if (is_portrait_id(pose)) {                  /* own palette from this frame alone */
                static uint32_t h1[4096];
                memset(h1, 0, sizeof h1);
                for (int i = 0; i < CANVAS * CANVAS_H; i++) {
                    const uint8_t *p2 = frames[pose] + (size_t)i * 4u;
                    if (p2[3] >= 128) h1[((p2[2] / 17) << 8) | ((p2[1] / 17) << 4) | (p2[0] / 17)]++;
                }
                unpal = build_pal(h1, cellpal[pose]); upal = cellpal[pose]; hascell[pose] = 1;
            }
            {
                int fromhi = !is_portrait_id(pose) && quant_hi(jobdir, pose, upal, unpal, q, pose < 2048 ? winy[pose] : 0);
                if (fromhi) nhi++;
                for (int i = 0; i < CANVAS * CANVAS_H; i++) {
                    const uint8_t *p = frames[pose] + (size_t)i * 4u;
                    if (!fromhi) {
                        q[i] = 0;
                        if (p[3] < 128) continue;
                        q[i] = (uint8_t)nearest_pen(p, upal, unpal);   /* nearest pen (RGB distance in 4-bit space) */
                    } else if (!q[i]) continue;
                    { int x = i % CANVAS, y = i / CANVAS;
                      if (x < x0) x0 = x; if (x > x1) x1 = x;
                      if (y < y0) y0 = y; if (y > y1) y1 = y; }
                }
            }
            if (x1 < 0) { missing++; continue; }       /* empty frame */
            if (!is_portrait_id(pose) && !exact_pal) outline_pass(q, pal);
            if (!is_portrait_id(pose) && !exact_pal)
            {   /* OUTLINE RETINT: stock art tints its outlines (dark
                 * brown vs skin, dark navy vs suit) — the generator
                 * draws pure black. An edge pixel (borders transparency)
                 * holding a near-black pen takes the darkest palette pen
                 * that shares its inward neighbour's hue; black hair is
                 * untouched (its neighbour is black too). */
                for (int y = 1; y < CANVAS_H - 1; y++)
                    for (int xx = 1; xx < CANVAS - 1; xx++) {
                        uint8_t me = q[y * CANVAS + xx];
                        unsigned mw;
                        int edge = 0, nb = -1;
                        if (!me) continue;
                        mw = pal[me];
                        if (((mw & 15) + ((mw >> 4) & 15) + ((mw >> 8) & 15)) > 6)
                            continue;                     /* not near-black */
                        for (int d = 0; d < 4; d++) {
                            static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
                            uint8_t n = q[(y + dy4[d]) * CANVAS + xx + dx4[d]];
                            if (!n) edge = 1;
                            else if (n != me) {
                                unsigned nwd = pal[n];
                                if (((nwd & 15) + ((nwd >> 4) & 15) + ((nwd >> 8) & 15)) > 8)
                                    nb = n;               /* a lit material neighbour */
                            }
                        }
                        if (!edge || nb < 0) continue;
                        {   /* darkest pen sharing nb's hue (not near-black) */
                            unsigned nwd = pal[nb];
                            int nr = nwd & 15, ng = (nwd >> 4) & 15, nbl = (nwd >> 8) & 15;
                            int best = -1, bestscore = 1 << 30;
                            for (int k = 1; k < 16; k++) {
                                unsigned w2 = pal[k];
                                int r = w2 & 15, g = (w2 >> 4) & 15, b = (w2 >> 8) & 15;
                                int bright = r + g + b;
                                int hue = (r * ng - g * nr) * (r * ng - g * nr)
                                        + (g * nbl - b * ng) * (g * nbl - b * ng);
                                if (bright <= 6 || bright >= nr + ng + nbl) continue;
                                if (hue + bright * 8 < bestscore) { bestscore = hue + bright * 8; best = k; }
                            }
                            if (best > 0) q[y * CANVAS + xx] = (uint8_t)best;
                        }
                    }
            }
            {   /* despeckle: an isolated pen pixel surrounded by >=6
                 * neighbours of ONE other pen joins them (the generator's
                 * soft edges quantize into salt-and-pepper otherwise) */
                static uint8_t q2[CANVAS * CANVAS_H];
                memcpy(q2, q, sizeof q2);
                for (int y = 1; y < CANVAS_H - 1; y++)
                    for (int xx = 1; xx < CANVAS - 1; xx++) {
                        int cnt[16] = {0}, bestp = -1, bestc = 0;
                        uint8_t me = q2[y * CANVAS + xx];
                        for (int dy2 = -1; dy2 <= 1; dy2++)
                            for (int dx2 = -1; dx2 <= 1; dx2++) {
                                if (!dy2 && !dx2) continue;
                                cnt[q2[(y + dy2) * CANVAS + xx + dx2]]++;
                            }
                        for (int k = 0; k < 16; k++)
                            if (cnt[k] > bestc) { bestc = cnt[k]; bestp = k; }
                        if (bestp >= 0 && bestp != me && bestc >= 6 && cnt[me] == 0)
                            q[y * CANVAS + xx] = (uint8_t)bestp;
                    }
            }
            /* the tile grid anchors to the BBOX (not the canvas): a pose
             * that is a shifted copy of another yields IDENTICAL tiles,
             * which is where the dedupe budget comes from */
            {   /* GRID PHASE (user 2026-08-26, tile reuse): slide the 16x16 cut grid
                   within one tile to the phase where the most cells are tiles the
                   gfx pak already holds - a stock man's cells then land on their
                   own ROM tiles exactly */
                int bestn = -1, bpx = 0, bpy = 0;
                for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                    int n = 0;
                    for (int ty = y0 - py; ty <= y1; ty += 16)
                        for (int tx = x0 - px; tx <= x1; tx += 16) {
                            Tile t; int blank = 1;
                            for (int yy = 0; yy < 16; yy++)
                                for (int xx = 0; xx < 16; xx++) {
                                    int cx = tx + xx, cy = ty + yy;
                                    uint8_t v = (cx >= 0 && cy >= 0 && cx < CANVAS && cy < CANVAS_H) ? q[cy * CANVAS + cx] : 0;
                                    t.px[yy * 16 + xx] = v;
                                    if (v) blank = 0;
                                }
                            if (!blank && rom_tile_find(&t) >= 0) n++;
                        }
                    if (n > bestn) { bestn = n; bpx = px; bpy = py; }
                }
                x0 -= bpx; y0 -= bpy;
            }
            fprintf(pj, "%s\"%u\":{", firstpose ? "" : ",\n", pose);
            firstpose = 0;
            {   /* cell list: template tiles first (in template order), then grid cells
                   over whatever the template leaves uncovered */
                static int cellx[1024], celly[1024]; static const uint8_t *celltp[1024]; static uint8_t cellfx[1024], cellfy[1024]; int ncell = 0;
                static uint8_t covered[CANVAS * CANVAS_H]; static short owner[CANVAS * CANVAS_H], leftover[CANVAS * CANVAS_H];
                memset(covered, 0, sizeof covered); memset(owner, 0, sizeof owner); memset(leftover, 0, sizeof leftover);
                int ntmpl = 0;   /* template cells: [0, ntmpl) own their pixels; grid cells after take everything */
                if (tdoc) {
                    char key[8]; const json_val *tl; snprintf(key, sizeof key, "%u", pose);
                    tl = json_get(json_get(json_get(tdoc, "poses"), key), "own");
                    for (const json_val *r = tl ? tl->child : NULL; r && ncell < 1024; r = r->next) {
                        int rx = (int)json_int(json_get(r, "x"), 0), ry = (int)json_int(json_get(r, "y"), 0), ch = (int)json_int(json_get(r, "chain"), 0) & 7;
                        int rfx = (int)json_int(json_get(r, "flipx"), 0), rfy = (int)json_int(json_get(r, "flipy"), 0);
                        unsigned tile0 = (unsigned)json_int(json_get(r, "tile"), 0);
                        for (int c = 0; c <= ch && ncell < 1024; c++) {
                            /* a flipy chain stacks the other way (pose_render: dy = ypos - 16*chain + 16*c) */
                            int tx = rx + ORG_X, ty = ORG_Y - (ry + 16 * (rfy ? ch - c : c));
                            const uint8_t *tp = wf_video_tile_pens(tile0 + (unsigned)c);
                            cellx[ncell] = tx; celly[ncell] = ty; cellfx[ncell] = (uint8_t)rfx; cellfy[ncell] = (uint8_t)rfy;
                            for (int yy = 0; yy < 16; yy++) for (int xx = 0; xx < 16; xx++) {
                                int cx = tx + xx, cy = ty + yy, qx = rfx ? 15 - xx : xx, qy = rfy ? 15 - yy : yy;
                                if (cx < 0 || cy < 0 || cx >= CANVAS || cy >= CANVAS_H) continue;
                                covered[cy * CANVAS + cx] = 1;
                                if (tp && tp[qy * 16 + qx]) owner[cy * CANVAS + cx] = 1;   /* some template tile is opaque here (the ROM tile as DRAWN: flips applied) */
                                leftover[cy * CANVAS + cx] = (short)(ncell + 1);          /* the topmost covering cell */
                            }
                            celltp[ncell] = tp; ncell++;
                        }
                    }
                }
                ntmpl = ncell;
                for (int ty = y0; ty <= y1; ty += 16)
                    for (int tx = x0; tx <= x1 && ncell < 1024; tx += 16) {
                        int need = 0;
                        for (int yy = 0; yy < 16 && !need; yy++) for (int xx = 0; xx < 16 && !need; xx++) {
                            int cx = tx + xx, cy = ty + yy;
                            if (cx >= 0 && cy >= 0 && cx < CANVAS && cy < CANVAS_H && q[cy * CANVAS + cx] && !covered[cy * CANVAS + cx]) need = 1;
                        }
                        if (need) { cellx[ncell] = tx; celly[ncell] = ty; cellfx[ncell] = cellfy[ncell] = 0; celltp[ncell] = NULL; ncell++; }
                    }
            for (int fl = 0; fl < 2; fl++) {
                fprintf(pj, "%s\"%s\":[", fl ? "," : "", fl ? "own_f" : "own");
                int firstrec = 1;
                for (int ci = 0; ci < ncell; ci++) {
                    {   int tx = cellx[ci], ty = celly[ci];
                        Tile t; int blank = 1, ti = -1; unsigned tid = 0;
                        for (int yy = 0; yy < 16; yy++)
                            for (int xx = 0; xx < 16; xx++) {
                                int cx = tx + xx, cy = ty + yy, in = cx >= 0 && cy >= 0 && cy < CANVAS_H && cx < CANVAS;
                                uint8_t v = in ? q[cy * CANVAS + cx] : 0;
                                if (v && ci < ntmpl) {   /* template tile: the pixels its OWN ROM tile is
                                                            opaque at (non-exclusive: an overlapping cell
                                                            keeps its pixels too, draw order sorts them),
                                                            plus leftovers no ROM tile owns, to the topmost */
                                    const uint8_t *tp = celltp[ci];
                                    int mine = tp && tp[(cellfy[ci] ? 15 - yy : yy) * 16 + (cellfx[ci] ? 15 - xx : xx)];
                                    if (!mine && !(!owner[cy * CANVAS + cx] && leftover[cy * CANVAS + cx] == ci + 1)) v = 0;
                                }
                                t.px[yy * 16 + xx] = v;
                                if (v) blank = 0;
                            }
                        if (blank) continue;
                        {   /* TILE REUSE (user 2026-08-26): a tile pixel-identical to one
                               the gfx pak already holds (a stock man's ROM cells, or an
                               earlier skin's) is referenced, not copied - an un-repainted
                               stock man in the skin layout costs no arena tiles */
                            int ext = rom_tile_find(&t);
                            if (ext >= 0) { tid = (unsigned)ext; nreuse++; }
                        }
                        if (tid == 0) {
                        for (int k = 0; k < ntile; k++)
                            if (tile_eq(&t, &tiles[k])) { ti = k; break; }
                        if (ti < 0) {
                            if ((unsigned)ntile >= arena_cap) { fprintf(stderr, "art-ingest: arena overflow at pose %u\n", pose); fclose(pj); return 1; }
                            tiles[ntile] = t; ti = ntile++;
                        }
                        tid = arena + (unsigned)ti;
                        }
                        {   /* record: package x/y with the HARDWARE y sense
                             * (render_pose_pens inverts on draw; mirror math
                             * matches the exporter's own_f: x -> -x) */
                            int rx = tx - ORG_X;
                            int ry = ORG_Y - ty;   /* inverse of render: canvasY = ORG_Y - y_r */
                            fprintf(pj, "%s{\"tile\":%u,\"x\":%d,\"y\":%d,\"flipx\":%d,\"flipy\":0,\"chain\":0,\"pal\":%d}",
                                    firstrec ? "" : ",",
                                    tid,
                                    fl ? -rx : rx,   /* ROM own_f rule: x -> -x (the -16 "true mirror" put every right-facing skin frame one tile left, 2026-08-26) */
                                    ry, fl, palid);
                            firstrec = 0;
                        }
                    }
                }
                fprintf(pj, "]");
            }
            }
            fprintf(pj, "}");
        }
        fprintf(pj, "\n},\"vict\":{\n");
        {   /* victim bodies: same cutter, keyed \"row,pose\" */
            int firstv = 1;
        for (int vk = 0; vk < vmn; vk++) {
            unsigned pose = (unsigned)vmvid[vk];   /* shared (deduped) frame id */
            static uint8_t q[CANVAS * CANVAS_H];
            int x0 = CANVAS, x1 = -1, y0 = CANVAS_H, y1 = -1;
            if (!have[pose]) { missing++; continue; }
            {
                int fromhi = quant_hi(jobdir, pose, pal, npal, q, pose < 2048 ? winy[pose] : 0);
                if (fromhi) nhi++;
                for (int i = 0; i < CANVAS * CANVAS_H; i++) {
                    const uint8_t *p = frames[pose] + (size_t)i * 4u;
                    if (!fromhi) {
                        q[i] = 0;
                        if (p[3] < 128) continue;
                        q[i] = (uint8_t)nearest_pen(p, pal, npal);
                    } else if (!q[i]) continue;
                    { int x = i % CANVAS, y = i / CANVAS;
                      if (x < x0) x0 = x; if (x > x1) x1 = x;
                      if (y < y0) y0 = y; if (y > y1) y1 = y; }
                }
            }
            if (!exact_pal) outline_pass(q, pal);
            if (x1 < 0) { missing++; continue; }       /* empty frame */
            if (!is_portrait_id(pose) && !exact_pal)
            {   /* OUTLINE RETINT: stock art tints its outlines (dark
                 * brown vs skin, dark navy vs suit) — the generator
                 * draws pure black. An edge pixel (borders transparency)
                 * holding a near-black pen takes the darkest palette pen
                 * that shares its inward neighbour's hue; black hair is
                 * untouched (its neighbour is black too). */
                for (int y = 1; y < CANVAS_H - 1; y++)
                    for (int xx = 1; xx < CANVAS - 1; xx++) {
                        uint8_t me = q[y * CANVAS + xx];
                        unsigned mw;
                        int edge = 0, nb = -1;
                        if (!me) continue;
                        mw = pal[me];
                        if (((mw & 15) + ((mw >> 4) & 15) + ((mw >> 8) & 15)) > 6)
                            continue;                     /* not near-black */
                        for (int d = 0; d < 4; d++) {
                            static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
                            uint8_t n = q[(y + dy4[d]) * CANVAS + xx + dx4[d]];
                            if (!n) edge = 1;
                            else if (n != me) {
                                unsigned nwd = pal[n];
                                if (((nwd & 15) + ((nwd >> 4) & 15) + ((nwd >> 8) & 15)) > 8)
                                    nb = n;               /* a lit material neighbour */
                            }
                        }
                        if (!edge || nb < 0) continue;
                        {   /* darkest pen sharing nb's hue (not near-black) */
                            unsigned nwd = pal[nb];
                            int nr = nwd & 15, ng = (nwd >> 4) & 15, nbl = (nwd >> 8) & 15;
                            int best = -1, bestscore = 1 << 30;
                            for (int k = 1; k < 16; k++) {
                                unsigned w2 = pal[k];
                                int r = w2 & 15, g = (w2 >> 4) & 15, b = (w2 >> 8) & 15;
                                int bright = r + g + b;
                                int hue = (r * ng - g * nr) * (r * ng - g * nr)
                                        + (g * nbl - b * ng) * (g * nbl - b * ng);
                                if (bright <= 6 || bright >= nr + ng + nbl) continue;
                                if (hue + bright * 8 < bestscore) { bestscore = hue + bright * 8; best = k; }
                            }
                            if (best > 0) q[y * CANVAS + xx] = (uint8_t)best;
                        }
                    }
            }
            {   /* despeckle: an isolated pen pixel surrounded by >=6
                 * neighbours of ONE other pen joins them (the generator's
                 * soft edges quantize into salt-and-pepper otherwise) */
                static uint8_t q2[CANVAS * CANVAS_H];
                memcpy(q2, q, sizeof q2);
                for (int y = 1; y < CANVAS_H - 1; y++)
                    for (int xx = 1; xx < CANVAS - 1; xx++) {
                        int cnt[16] = {0}, bestp = -1, bestc = 0;
                        uint8_t me = q2[y * CANVAS + xx];
                        for (int dy2 = -1; dy2 <= 1; dy2++)
                            for (int dx2 = -1; dx2 <= 1; dx2++) {
                                if (!dy2 && !dx2) continue;
                                cnt[q2[(y + dy2) * CANVAS + xx + dx2]]++;
                            }
                        for (int k = 0; k < 16; k++)
                            if (cnt[k] > bestc) { bestc = cnt[k]; bestp = k; }
                        if (bestp >= 0 && bestp != me && bestc >= 6 && cnt[me] == 0)
                            q[y * CANVAS + xx] = (uint8_t)bestp;
                    }
            }
            /* the tile grid anchors to the BBOX (not the canvas): a pose
             * that is a shifted copy of another yields IDENTICAL tiles,
             * which is where the dedupe budget comes from */
            {   /* GRID PHASE (user 2026-08-26, tile reuse): slide the 16x16 cut grid
                   within one tile to the phase where the most cells are tiles the
                   gfx pak already holds - a stock man's cells then land on their
                   own ROM tiles exactly */
                int bestn = -1, bpx = 0, bpy = 0;
                for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                    int n = 0;
                    for (int ty = y0 - py; ty <= y1; ty += 16)
                        for (int tx = x0 - px; tx <= x1; tx += 16) {
                            Tile t; int blank = 1;
                            for (int yy = 0; yy < 16; yy++)
                                for (int xx = 0; xx < 16; xx++) {
                                    int cx = tx + xx, cy = ty + yy;
                                    uint8_t v = (cx >= 0 && cy >= 0 && cx < CANVAS && cy < CANVAS_H) ? q[cy * CANVAS + cx] : 0;
                                    t.px[yy * 16 + xx] = v;
                                    if (v) blank = 0;
                                }
                            if (!blank && rom_tile_find(&t) >= 0) n++;
                        }
                    if (n > bestn) { bestn = n; bpx = px; bpy = py; }
                }
                x0 -= bpx; y0 -= bpy;
            }
            fprintf(pj, "%s\"%d,%d\":{", firstv ? "" : ",\n",
                    (int)vmrow[vk], (int)vmpose[vk]);
            firstv = 0;
            {   /* VICTIM cells by template: the victim cells of the holder's composed
                   list for this frame's first (row, pose) - same interleave logic as
                   the body cut */
                static int cellx[1024], celly[1024]; static const uint8_t *celltp[1024]; static uint8_t cellfx[1024], cellfy[1024]; int ncell = 0;
                static uint8_t covered[CANVAS * CANVAS_H]; static short owner[CANVAS * CANVAS_H], leftover[CANVAS * CANVAS_H];
                memset(covered, 0, sizeof covered); memset(owner, 0, sizeof owner); memset(leftover, 0, sizeof leftover);
                int ntmpl = 0;   /* template cells: [0, ntmpl) own their pixels; grid cells after take everything */
                {
                    int vk0 = -1;
                    for (int k2 = 0; k2 < vmn; k2++) if (vmvid[k2] == (short)pose) { vk0 = k2; break; }
                    if (vk0 >= 0) {
                        unsigned hrow = vmrow[vk0] == 12 ? (unsigned)(slot < 12 ? slot : palid) : (unsigned)vmrow[vk0];
                        unsigned hpose = (unsigned)vmpose[vk0]; int vbase = slot < 12 ? slot : palid;
                        const eng_pkg_rec *T; uint8_t vmask[192]; int tn;
                        tn = eng_pkg_pose(hrow, hpose, 0, vbase, &T);
                        if (tn > 0 && tn <= 192 && eng_pkg_victim_mask(hrow, hpose, 0, (unsigned)vbase, vmask, 192) > 0)
                            for (int t2 = 0; t2 < tn; t2++) {
                                int ch = T[t2].chain & 7;
                                if (!vmask[t2]) continue;
                                for (int c = 0; c <= ch && ncell < 1024; c++) {
                                    int rfx = T[t2].flipx, rfy = T[t2].flipy;
                                    int tx = T[t2].x + ORG_X, ty = ORG_Y - (T[t2].y + 16 * (rfy ? ch - c : c));
                                    const uint8_t *tp = wf_video_tile_pens(T[t2].tile + (unsigned)c);
                                    cellx[ncell] = tx; celly[ncell] = ty; cellfx[ncell] = (uint8_t)rfx; cellfy[ncell] = (uint8_t)rfy;
                                    for (int yy = 0; yy < 16; yy++) for (int xx = 0; xx < 16; xx++) {
                                        int cx = tx + xx, cy = ty + yy, qx = rfx ? 15 - xx : xx, qy = rfy ? 15 - yy : yy;
                                        if (cx < 0 || cy < 0 || cx >= CANVAS || cy >= CANVAS_H) continue;
                                        covered[cy * CANVAS + cx] = 1;
                                        if (tp && tp[qy * 16 + qx]) owner[cy * CANVAS + cx] = 1;
                                        leftover[cy * CANVAS + cx] = (short)(ncell + 1);
                                    }
                                    celltp[ncell] = tp; ncell++;
                                }
                            }
                    }
                }
                ntmpl = ncell;
                for (int ty = y0; ty <= y1; ty += 16)
                    for (int tx = x0; tx <= x1 && ncell < 1024; tx += 16) {
                        int need = 0;
                        for (int yy = 0; yy < 16 && !need; yy++) for (int xx = 0; xx < 16 && !need; xx++) {
                            int cx = tx + xx, cy = ty + yy;
                            if (cx >= 0 && cy >= 0 && cx < CANVAS && cy < CANVAS_H && q[cy * CANVAS + cx] && !covered[cy * CANVAS + cx]) need = 1;
                        }
                        if (need) { cellx[ncell] = tx; celly[ncell] = ty; cellfx[ncell] = cellfy[ncell] = 0; celltp[ncell] = NULL; ncell++; }
                    }
            for (int fl = 0; fl < 2; fl++) {
                fprintf(pj, "%s\"%s\":[", fl ? "," : "", fl ? "own_f" : "own");
                int firstrec = 1;
                for (int ci = 0; ci < ncell; ci++) {
                    {   int tx = cellx[ci], ty = celly[ci];
                        Tile t; int blank = 1, ti = -1; unsigned tid = 0;
                        for (int yy = 0; yy < 16; yy++)
                            for (int xx = 0; xx < 16; xx++) {
                                int cx = tx + xx, cy = ty + yy, in = cx >= 0 && cy >= 0 && cy < CANVAS_H && cx < CANVAS;
                                uint8_t v = in ? q[cy * CANVAS + cx] : 0;
                                if (v && ci < ntmpl) {   /* template tile: the pixels its OWN ROM tile is
                                                            opaque at (non-exclusive: an overlapping cell
                                                            keeps its pixels too, draw order sorts them),
                                                            plus leftovers no ROM tile owns, to the topmost */
                                    const uint8_t *tp = celltp[ci];
                                    int mine = tp && tp[(cellfy[ci] ? 15 - yy : yy) * 16 + (cellfx[ci] ? 15 - xx : xx)];
                                    if (!mine && !(!owner[cy * CANVAS + cx] && leftover[cy * CANVAS + cx] == ci + 1)) v = 0;
                                }
                                t.px[yy * 16 + xx] = v;
                                if (v) blank = 0;
                            }
                        if (blank) continue;
                        {   int ext = rom_tile_find(&t);
                            if (ext >= 0) { tid = (unsigned)ext; nreuse++; }
                        }
                        if (tid == 0) {
                        for (int k = 0; k < ntile; k++)
                            if (tile_eq(&t, &tiles[k])) { ti = k; break; }
                        if (ti < 0) {
                            if ((unsigned)ntile >= arena_cap) { fprintf(stderr, "art-ingest: arena overflow at pose %u\n", pose); fclose(pj); return 1; }
                            tiles[ntile] = t; ti = ntile++;
                        }
                        tid = arena + (unsigned)ti;
                        }
                        {
                            int rx = tx - ORG_X;
                            int ry = ORG_Y - ty;
                            fprintf(pj, "%s{\"tile\":%u,\"x\":%d,\"y\":%d,\"flipx\":%d,\"flipy\":0,\"chain\":0,\"pal\":%d}",
                                    firstrec ? "" : ",", tid, fl ? -rx : rx, ry, fl, palid);
                            firstrec = 0;
                        }
                    }
                }
                fprintf(pj, "]");
            }
            }
            fprintf(pj, "}");
        }
        }
        fprintf(pj, "\n},\"cellpal\":{");
        {   int fc = 1;
            for (unsigned pose = 800; pose < 1024; pose++) {
                if (!hascell[pose]) continue;
                fprintf(pj, "%s\n\"%u\":[", fc ? "" : ",", pose); fc = 0;
                for (int k = 0; k < 16; k++) fprintf(pj, "%s%u", k ? "," : "", cellpal[pose][k]);
                fprintf(pj, "]");
            }
        }
        fprintf(pj, "\n}}\n");
        fclose(pj);
        fprintf(stderr, "art-ingest: %d frames (%d poses fall back to the base, %d quantised from the hi-res archive), %d unique tiles -> arena 0x%X, %d records reuse gfx-pak tiles\n",
                nframes, missing, nhi, ntile, arena, nreuse);
    }

    {   /* tiles.json + sheet.png + palette.json */
        FILE *f;
        int cols = 16, rows = (ntile + cols - 1) / cols;
        int W = cols * 16, H = rows * 16;
        uint8_t *img = calloc((size_t)W * (size_t)H, 1);
        png_color pc[16];
        snprintf(path, sizeof path, "%s/tiles.json", dstdir);
        f = fopen(path, "w"); if (!f) return 1;
        fprintf(f, "{\"count\":%d,\"tiles\":[", ntile);
        for (int t = 0; t < ntile; t++) fprintf(f, "%s%u", t ? "," : "", arena + (unsigned)t);
        fprintf(f, "]}\n"); fclose(f);
        for (int t = 0; t < ntile; t++) {
            int ox = (t % cols) * 16, oy = (t / cols) * 16;
            for (int y = 0; y < 16; y++)
                memcpy(img + (size_t)(oy + y) * W + ox, tiles[t].px + y * 16, 16);
        }
        for (int k = 0; k < 16; k++) {
            uint16_t w = k < npal ? pal[k] : 0;
            pc[k].red = (png_byte)((w & 15) * 17);
            pc[k].green = (png_byte)(((w >> 4) & 15) * 17);
            pc[k].blue = (png_byte)(((w >> 8) & 15) * 17);
        }
        snprintf(path, sizeof path, "%s/sheet.png", dstdir);
        {
            FILE *sf = fopen(path, "wb");
            png_structp png; png_infop info; png_bytep *rp;
            if (!sf) { free(img); return 1; }
            png = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
            info = png_create_info_struct(png);
            if (setjmp(png_jmpbuf(png))) { fclose(sf); free(img); return 1; }
            png_init_io(png, sf);
            png_set_IHDR(png, info, (png_uint_32)W, (png_uint_32)H, 8, PNG_COLOR_TYPE_PALETTE,
                         PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            png_set_PLTE(png, info, pc, 16);
            rp = malloc(sizeof(png_bytep) * (size_t)H);
            for (int y = 0; y < H; y++) rp[y] = img + (size_t)y * W;
            png_set_rows(png, info, rp);
            png_write_png(png, info, PNG_TRANSFORM_IDENTITY, 0);
            png_destroy_write_struct(&png, &info);
            free(rp); fclose(sf);
        }
        free(img);
        snprintf(path, sizeof path, "%s/palette.json", dstdir);
        pf = fopen(path, "w"); if (!pf) return 1;
        fprintf(pf, "{\"pens\":[");
        for (int k = 0; k < 16; k++) fprintf(pf, "%s%u", k ? "," : "", k < npal ? pal[k] : 0);
        fprintf(pf, "],\"note\":\"derived by --art-ingest from the generated frames\"}\n");
        fclose(pf);
    }
    for (unsigned pose = 0; pose < 2048; pose++) { free(frames[pose]); frames[pose] = NULL; have[pose] = 0; }
    if (tdoc) json_free(tdoc);   /* re-entrant: --stock-skins runs 12 ingests */
    fprintf(stderr, "art-ingest: %s ready (add/keep wrestler.json, then pack the profile)\n", dstdir);
    return 0;
}
