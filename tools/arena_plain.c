/* arena_plain.c — the PLAIN arena template + the per-cell ROLE MAP (user
 * 2026-08-30: "a very very plain arena that can be themed later"; the first
 * cut hatched the rails away - they live inside the crowd frames, which are
 * complete pictures of the whole band, so a role map is needed).
 *
 *   wfengine --arena-plain SRC DST      e.g. --arena-plain wwf generic-ring
 *
 * Copies arenas/SRC to arenas/DST (or refreshes DST from SRC when it already
 * exists) and writes, per view, roles.png - one flat colour per pixel:
 *   grey    (128,128,128)  GEOMETRY  rails, posts, apron edge, floor lines: never repainted
 *   red     (255,0,0)      CROWD     the cells the ROM animates (pixels differ between steps)
 *   green   (0,255,0)      STANDS    static audience art: upper tiers, flags, banners, signs
 *   blue    (0,0,255)      MAT       the mat (logo included)
 *   magenta (255,0,255)    SKIRT     the apron curtain / sponsor band
 *   cyan    (0,255,255)    FLOOR     the floor surface (sparkle highlights included)
 *   dark cyan (0,128,128)  FLINE     the floor's perspective lines (the second most frequent blue)
 *   orange  (255,128,0)    POST      the far ring posts in the crowd band (pads + logo flattened)
 * An existing DST/<view>/roles.png is REUSED (hand-fix a cell in a pixel
 * editor, run again) - the classification below only runs when there is none.
 * Then every picture is repainted from SRC by role: MAT / SKIRT / FLOOR flat
 * greys, CROWD + STANDS flat dark with faint head-row hatching (all steps
 * identical - the theme paints the animation), GEOMETRY greyscale + lifted.
 * Rope pictures and the side-rope sprites are copied untouched (stock ropes
 * are geometry and look right).
 * Classification is by the WWF export's exact colours (histogram
 * 2026-08-30); another source arena needs its own rows in the tables. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/json.h"

int wf_video_load_rgba_png(const char *path, uint8_t **out, int *w, int *h);
int wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H);

enum { R_NONE, R_GEOM, R_CROWD, R_STANDS, R_MAT, R_SKIRT, R_FLOOR, R_FLINE, R_POST, R_N };
static const uint8_t role_rgb[R_N][3] = { { 0, 0, 0 }, { 128, 128, 128 }, { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 0, 255 }, { 0, 255, 255 }, { 0, 128, 128 }, { 255, 128, 0 } };
static const struct { uint8_t r, g, b; int role; } colours[] = {
    { 136, 170, 238, R_MAT }, { 153, 187, 255, R_MAT }, { 119, 153, 221, R_MAT }, { 85, 119, 187, R_MAT },
    { 102, 17, 85, R_SKIRT }, { 136, 34, 119, R_SKIRT },
};
/* the floor: every blue-dominant overlay pixel; the most frequent blue is the
   surface, the second the perspective lines (dark cyan FLINE), any other blue
   (sparkle highlights) is surface too - flattened */
static int is_blue(const uint8_t *p) { return p[2] >= 120 && p[2] > p[0] + 60 && p[2] > p[1] + 60; }
static const uint8_t MAT_GREY[3] = { 178, 178, 186 }, SKIRT_GREY[3] = { 104, 104, 112 }, FLOOR_GREY[3] = { 70, 70, 78 }, FLINE_GREY[3] = { 52, 52, 60 };
static const uint8_t CROWD_DARK[3] = { 44, 44, 52 }, CROWD_HEAD[3] = { 72, 72, 82 }, POST_GREY[3] = { 150, 150, 158 };
/* a far ring post in the crowd band: blue pillar, dark-blue turnbuckle pads, yellow logo */
static int is_post_colour(const uint8_t *p) { return is_blue(p) || (p[2] >= 90 && p[0] < 50 && p[1] < 50) || (p[0] >= 200 && p[1] >= 200 && p[2] < 60); }

typedef struct { uint8_t *px; int w, h; } img;
static int iload(const char *p, img *m) { return wf_video_load_rgba_png(p, &m->px, &m->w, &m->h) ? -1 : 0; }
static int colour_role(const uint8_t *p)
{
    for (unsigned i = 0; i < sizeof colours / sizeof colours[0]; i++) if (p[0] == colours[i].r && p[1] == colours[i].g && p[2] == colours[i].b) return colours[i].role;
    return R_NONE;
}
static int is_rail_colour(const uint8_t *p)          /* the white / grey / black families: rails, posts, outlines */
{
    int mx = p[0] > p[1] ? p[0] : p[1]; if (p[2] > mx) mx = p[2];
    int mn = p[0] < p[1] ? p[0] : p[1]; if (p[2] < mn) mn = p[2];
    return mx - mn <= 34 && mx >= 120;                /* near-neutral AND light (238,255,255 passes: the rail
                                                         highlight; the dark stadium backdrop does not) */
}
/* per-pixel roles of one view: rl[w*h] (R_*) from the crowd frames (under +
 * over) and the overlay */
static void classify(const char *vdir, const json_val *layers, uint8_t *rl, int w, int h)
{
    char p[600]; img ov = { 0 }, fr[8] = { { 0 } }; int nf = 0, pf[8];
    const json_val *L = json_get(layers, "under"), *O = json_get(layers, "over");
    /* the crowd frames of both planes: a pixel that differs between two steps
       OF THE SAME PLANE is CROWD (comparing across planes marked nearly every
       ringside pixel animated - the rail tubes went with it); an opaque static
       one is STANDS unless it is rail-coloured */
    for (int pl = 0; pl < 2; pl++) {
        const json_val *fs = json_get(pl ? O : L, "frames");
        for (const json_val *q = fs ? fs->child : NULL; q && nf < 8; q = q->next) {
            if (pl && strncmp(json_str(q, ""), "backcrowd", 9)) continue;   /* the ring's over plane = rope lines, not crowd */
            snprintf(p, sizeof p, "%s/%s", vdir, json_str(q, ""));
            if (iload(p, &fr[nf]) == 0 && fr[nf].w == w && fr[nf].h == h) pf[nf++] = pl;
        }
    }
    uint8_t *first_px = calloc((size_t)w * h, 4);
    for (int i = 0; i < w * h; i++) {
        int anim = 0, opaque = 0; const uint8_t *first = NULL; const uint8_t *base[2] = { NULL, NULL };
        for (int k = 0; k < nf; k++) {
            const uint8_t *q = fr[k].px + (size_t)i * 4;
            if (q[3] >= 128) { opaque = 1; if (!first) first = q; }
            if (!base[pf[k]]) base[pf[k]] = q; else if (memcmp(q, base[pf[k]], 4)) anim = 1;
        }
        if (!opaque) continue;
        memcpy(first_px + (size_t)i * 4, first, 4);
        if (anim) rl[i] = R_CROWD;
        else { int cr0 = colour_role(first);           /* the far ring's mat / skirt edge inside the band */
               rl[i] = (cr0 == R_MAT || cr0 == R_SKIRT) ? cr0 : is_rail_colour(first) ? R_GEOM : R_STANDS; }
    }
    /* the far POSTS (ring view: the two posts behind the mat stand in the
       crowd band as static blue pillars with pads + logos): a static
       post-coloured 8-connected component that is tall (>= 40 px, height >=
       3 x width, >= 120 px) is a post; painted flat, logos gone */
    { int *lab = calloc((size_t)w * h, sizeof *lab), *stack = malloc((size_t)w * h * sizeof *stack);
      uint8_t *cand = calloc((size_t)w * h, 1);
      for (int i = 0; i < w * h; i++) if (rl[i] == R_STANDS && is_post_colour(first_px + (size_t)i * 4)) cand[i] = 1;
      /* the post's black outline splits its blue into slivers: a dark static
         pixel touching a blue candidate joins it (one step only - crowd
         outlines must not sprawl in) */
      for (int i = 0; i < w * h; i++) {
          const uint8_t *q = first_px + (size_t)i * 4; int x = i % w, y = i / w, near = 0;
          if (rl[i] != R_STANDS || cand[i] || q[3] < 128 || (q[0] | q[1] | q[2]) > 70) continue;
          for (int dy = -1; dy <= 1 && !near; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy; if (xx >= 0 && yy >= 0 && xx < w && yy < h && cand[yy * w + xx] == 1) { near = 1; break; } }
          if (near) cand[i] = 2;
      }
      /* the shape test runs on the BLUE pillar alone (the pads and rope ends
         would widen it - 8 x 22 px shows above the ringside overlay); a tall
         pillar then floods POST out through its dilated outline so the pads
         go with it */
      for (int i = 0; i < w * h; i++) if (cand[i] == 1 && !lab[i]) {
          int n = 0, sp = 0, x0 = w, x1 = 0, y0 = h, y1 = 0; stack[sp++] = i; lab[i] = i + 1;
          while (sp) { int j = stack[--sp], x = j % w, y = j / w; n++;
              if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
              for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy, k;
                  if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue; k = yy * w + xx;
                  if (cand[k] == 1 && !lab[k]) { lab[k] = i + 1; stack[sp++] = k; } } }
          if (getenv("WF_PLAINDBG") && n >= 50) fprintf(stderr, "post cand: n %d bbox x %d..%d y %d..%d\n", n, x0, x1, y0, y1);
          if (n >= 60 && y1 - y0 >= 16 && y1 - y0 >= 2 * (x1 - x0)) {
              sp = 0;
              for (int k = 0; k < w * h; k++) if (lab[k] == i + 1) { rl[k] = R_POST; stack[sp++] = k; }
              while (sp) { int j = stack[--sp], x = j % w, y = j / w;
                  for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy, k;
                      if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue; k = yy * w + xx;
                      if (cand[k] && rl[k] != R_POST) { rl[k] = R_POST; lab[k] = i + 1; stack[sp++] = k; } } }
          }
      }
      free(lab); free(stack); free(cand); }
    /* CROWD is a per-CELL role (the packer and the ROM animate 16x16 tiles):
       any animated pixel makes the whole cell crowd */
    for (int cy = 0; cy < h / 16; cy++) for (int cx = 0; cx < w / 16; cx++) {
        int any = 0;
        for (int y = cy * 16; y < cy * 16 + 16 && !any; y++) for (int x = cx * 16; x < cx * 16 + 16; x++) if (rl[(size_t)y * w + x] == R_CROWD) { any = 1; break; }
        if (any) for (int y = cy * 16; y < cy * 16 + 16; y++) for (int x = cx * 16; x < cx * 16 + 16; x++) if (rl[(size_t)y * w + x] == R_STANDS) rl[(size_t)y * w + x] = R_CROWD;
        /* static rail pixels INSIDE an animated cell stay geometry: the rail's
           lower tube runs in front of a moving crowd (it was hatched away) */
    }
    /* rails and posts stand at the animated tiers' level and below; light
       pixels ABOVE the first animated row (flag stripes, lights, the truss)
       are stands art */
    { int top = h;
      for (int i = 0; i < w * h; i++) if (rl[i] == R_CROWD) { top = i / w; break; }
      for (int i = 0; i < top * w; i++) if (rl[i] == R_GEOM) rl[i] = R_STANDS; }
    for (int k = 0; k < nf; k++) free(fr[k].px);
    free(first_px);
    /* the overlay: mat / skirt / floor by colour, spans filled so the logo and
       the banner lettering take the surface's role; the rest is geometry */
    { const char *ovn = json_str(json_get(L, "overlay"), NULL);
      if (ovn) { snprintf(p, sizeof p, "%s/%s", vdir, ovn); if (iload(p, &ov) || ov.w != w || ov.h != h) { free(ov.px); ov.px = NULL; } } }
    if (ov.px) {
        uint8_t *cr = calloc((size_t)w * h, 1);
        uint32_t bc[64] = { 0 }; int bn = 0; uint32_t base = 0, line = 0; int c1 = 0, c2 = 0;
        for (int i = 0; i < w * h; i++) {
            const uint8_t *q = ov.px + (size_t)i * 4; int r;
            if (q[3] < 128) continue;
            r = colour_role(q);
            if (r == R_NONE && is_blue(q)) {              /* tally the blues for the surface / line split */
                uint32_t key = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2]; int k;
                for (k = 0; k < bn; k += 2) if (bc[k] == key) { bc[k + 1]++; break; }
                if (k == bn && bn < 62) { bc[bn] = key; bc[bn + 1] = 1; bn += 2; }
                r = R_FLOOR;
            }
            cr[i] = (uint8_t)r;
        }
        for (int k = 0; k < bn; k += 2) { if ((int)bc[k + 1] > c1) { c2 = c1; line = base; c1 = (int)bc[k + 1]; base = bc[k]; } else if ((int)bc[k + 1] > c2) { c2 = (int)bc[k + 1]; line = bc[k]; } }
        for (int i = 0; i < w * h; i++) if (cr[i] == R_FLOOR) {
            const uint8_t *q = ov.px + (size_t)i * 4; uint32_t key = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
            if (key == line && line != base) cr[i] = R_FLINE;
        }
        for (int y = 0; y < h; y++) for (int role = R_MAT; role <= R_SKIRT; role++) {
            int x0 = -1, x1 = -1;
            for (int x = 0; x < w; x++) if (cr[(size_t)y * w + x] == role) { if (x0 < 0) x0 = x; x1 = x; }
            for (int x = x0; x0 >= 0 && x <= x1; x++) if (ov.px[((size_t)y * w + x) * 4 + 3] >= 128) cr[(size_t)y * w + x] = (uint8_t)role;   /* the mat and the skirt are convex per row: everything inside the span is theirs (the logo's blue outlines too) */
        }
        /* the far posts stand ABOVE the mat's top edge (the overlay's rows up
           there hold nothing else but the corner rail posts, which are rail-
           coloured): blue pillar, dark pads, yellow logo -> POST */
        { int mat_top = h;
          for (int i = 0; i < w * h; i++) if (cr[i] == R_MAT) { mat_top = i / w; break; }
          for (int i = 0; i < mat_top * w; i++) if (ov.px[(size_t)i * 4 + 3] >= 128 && cr[i] != R_MAT && cr[i] != R_SKIRT && !is_rail_colour(ov.px + (size_t)i * 4)) cr[i] = R_POST; }
        for (int i = 0; i < w * h; i++) {
            if (ov.px[(size_t)i * 4 + 3] < 128) continue;
            if (cr[i] == R_MAT || cr[i] == R_SKIRT || cr[i] == R_FLOOR || cr[i] == R_FLINE || cr[i] == R_POST) rl[i] = cr[i];
            else rl[i] = R_GEOM;      /* the overlay is composited OVER the crowd: its rails, posts and lines own the pixel whatever the band says */
        }
        free(cr); free(ov.px);
    }
    /* light static specks in the audience (shirts, signs, hands) pass the
       rail-colour test: rails are big connected structures, so a geometry
       component (8-connected) under 48 px is stands - counted AFTER the overlay
       stage so the crowd frame's rail bars connect through the overlay's tube */
    { int *lab = calloc((size_t)w * h, sizeof *lab), *stack = malloc((size_t)w * h * sizeof *stack);
      for (int i = 0; i < w * h; i++) if (rl[i] == R_GEOM && !lab[i]) {
          int n = 0, sp = 0; stack[sp++] = i; lab[i] = 1;
          while (sp) { int j = stack[--sp], x = j % w, y = j / w; n++;
              for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy, k;
                  if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue; k = yy * w + xx;
                  if (rl[k] == R_GEOM && !lab[k]) { lab[k] = 1; stack[sp++] = k; } } }
          if (n < 48) { sp = 0; stack[sp++] = i; lab[i] = 2;
              while (sp) { int j = stack[--sp], x = j % w, y = j / w; rl[j] = R_STANDS;
                  for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy, k;
                      if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue; k = yy * w + xx;
                      if (rl[k] == R_GEOM && lab[k] == 1) { lab[k] = 2; stack[sp++] = k; } } } }
      }
      free(lab); free(stack); }
}
static void roles_write(const char *path, const uint8_t *rl, int w, int h)
{
    uint8_t *px = calloc((size_t)w * h, 4);
    for (int i = 0; i < w * h; i++) if (rl[i]) { memcpy(px + (size_t)i * 4, role_rgb[rl[i]], 3); px[(size_t)i * 4 + 3] = 255; }
    wf_art_write_rgba_png(path, px, w, h); free(px);
}
static int roles_read(const char *path, uint8_t *rl, int w, int h)
{
    img m; if (iload(path, &m)) return -1;
    if (m.w != w || m.h != h) { free(m.px); return -1; }
    for (int i = 0; i < w * h; i++) {
        const uint8_t *p = m.px + (size_t)i * 4; int best = R_NONE, bd = 1 << 30;
        if (p[3] < 128) continue;
        for (int r = R_GEOM; r < R_N; r++) { int d = abs(p[0] - role_rgb[r][0]) + abs(p[1] - role_rgb[r][1]) + abs(p[2] - role_rgb[r][2]); if (d < bd) { bd = d; best = r; } }
        rl[i] = (uint8_t)best;
    }
    free(m.px); return 0;
}
/* repaint one picture by role */
static int repaint(const char *path, const uint8_t *rl, int w, int h, int force_crowd)
{
    img m; int rc;
    if (iload(path, &m)) { fprintf(stderr, "arena-plain: cannot read %s\n", path); return 1; }
    if (m.w != w || m.h != h) { free(m.px); fprintf(stderr, "arena-plain: %s is not %dx%d\n", path, w, h); return 1; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        size_t i = (size_t)y * w + x; uint8_t *p = m.px + i * 4; int l, head;
        if (p[3] < 128) continue;
        switch (force_crowd ? R_CROWD : rl[i]) {       /* the over plane's far crowd is crowd whatever lies under it */
        case R_MAT:   memcpy(p, MAT_GREY, 3); break;
        case R_SKIRT: memcpy(p, SKIRT_GREY, 3); break;
        case R_FLOOR: memcpy(p, FLOOR_GREY, 3); break;
        case R_FLINE: memcpy(p, FLINE_GREY, 3); break;
        case R_POST:  memcpy(p, POST_GREY, 3); break;
        case R_CROWD: case R_STANDS:
            head = (y % 8) >= 2 && (y % 8) <= 3 && (((x + ((y / 8) & 1) * 3) % 6) < 2);
            memcpy(p, head ? CROWD_HEAD : CROWD_DARK, 3); break;
        default:                                       /* geometry: greyscale, lifted */
            l = (299 * p[0] + 587 * p[1] + 114 * p[2]) / 1000; l = 36 + l * 200 / 255;
            p[0] = p[1] = p[2] = (uint8_t)l; break;
        }
    }
    rc = wf_art_write_rgba_png(path, m.px, w, h); free(m.px);
    return rc != 0;
}
static int plain_view(const char *src, const char *dst, const char *view)
{
    char sd[600], dd[600], p[700], err[128]; json_val *doc; const json_val *layers, *L; img probe; uint8_t *rl; int w, h, bad = 0;
    snprintf(sd, sizeof sd, "arenas/%s/%s", src, view); snprintf(dd, sizeof dd, "arenas/%s/%s", dst, view);
    snprintf(p, sizeof p, "%s/arena.json", sd);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) return 0;                                       /* the cage has no ringside view */
    layers = json_get(doc, "layers"); L = json_get(layers, "under");
    snprintf(p, sizeof p, "%s/%s", sd, json_str(json_get(L, "overlay"), "ring.png"));
    if (iload(p, &probe)) { json_free(doc); fprintf(stderr, "arena-plain: cannot read %s\n", p); return 1; }
    w = probe.w; h = probe.h; free(probe.px);
    rl = calloc((size_t)w * h, 1);
    snprintf(p, sizeof p, "%s/roles.png", dd);
    if (roles_read(p, rl, w, h) == 0) fprintf(stderr, "arena-plain: %s: roles.png reused\n", dd);
    else { classify(sd, layers, rl, w, h); roles_write(p, rl, w, h); fprintf(stderr, "arena-plain: %s: roles.png written\n", dd); }
    { int cnt[R_N] = { 0 }; for (int i = 0; i < w * h; i++) cnt[rl[i]]++;
      fprintf(stderr, "arena-plain: %s roles: geometry %d crowd %d stands %d mat %d skirt %d floor %d lines %d posts %d px\n", view, cnt[R_GEOM], cnt[R_CROWD], cnt[R_STANDS], cnt[R_MAT], cnt[R_SKIRT], cnt[R_FLOOR], cnt[R_FLINE], cnt[R_POST]); }
    /* the pictures: fresh copies from SRC, repainted; the under-plane crowd
       frames take the repainted OVERLAY's pixel wherever it is opaque - the
       ROM's crowd steps re-stamp the rail cells without the rail (the other
       plane draws it), so a crowd frame on its own showed chopped rails */
    { const char *ov = json_str(json_get(L, "overlay"), NULL); img ovp = { 0 };
      if (ov) { snprintf(p, sizeof p, "cp \"%s/%s\" \"%s/%s\"", sd, ov, dd, ov); if (system(p)) bad = 1; snprintf(p, sizeof p, "%s/%s", dd, ov); bad |= repaint(p, rl, w, h, 0); iload(p, &ovp); }
      for (int pl = 0; pl < 2; pl++) {
          const json_val *fs = json_get(pl ? json_get(layers, "over") : L, "frames");
          for (const json_val *q = fs ? fs->child : NULL; q; q = q->next) {
              if (pl && strncmp(json_str(q, ""), "backcrowd", 9)) continue;
              snprintf(p, sizeof p, "cp \"%s/%s\" \"%s/%s\"", sd, json_str(q, ""), dd, json_str(q, "")); if (system(p)) bad = 1;
              snprintf(p, sizeof p, "%s/%s", dd, json_str(q, "")); bad |= repaint(p, rl, w, h, pl);
              if (!pl && ovp.px && ovp.w == w && ovp.h == h) {
                  img m;
                  if (iload(p, &m) == 0) {
                      for (int i = 0; i < w * h; i++) if (ovp.px[(size_t)i * 4 + 3] >= 128 && m.px[(size_t)i * 4 + 3] >= 128) memcpy(m.px + (size_t)i * 4, ovp.px + (size_t)i * 4, 4);
                      wf_art_write_rgba_png(p, m.px, w, h); free(m.px);
                  }
              }
          }
      }
      free(ovp.px); }
    /* palettes: derived from the new art at pack time (the ROM banks would quantise the greys away) */
    json_remove(doc, "fg_palette"); json_remove(doc, "bg_palette");
    json_set_string(doc, "palettes", "derived from the art at pack time (16 banks x 15 colours per plane, greedy per cell)");
    json_set_string(doc, "roles", "roles.png");
    snprintf(p, sizeof p, "%s/arena.json", dd); json_write_file(p, doc); json_free(doc);
    free(rl);
    return bad;
}
int tool_arena_plain(const char *src, const char *dst)
{
    char cmd[900], p[600], err[128]; json_val *doc; struct stat st; int bad = 0;
    if (!src[0] || !dst[0] || strpbrk(dst, "/\\ .") || strpbrk(src, "/\\ .")) { fprintf(stderr, "arena-plain: plain names only\n"); return 1; }
    snprintf(p, sizeof p, "arenas/%s/ring/arena.json", src);
    if (stat(p, &st)) { fprintf(stderr, "arena-plain: no arena %s\n", src); return 1; }
    snprintf(p, sizeof p, "arenas/%s", dst);
    if (stat(p, &st)) {                                       /* first run: the copy (a later run refreshes the pictures and keeps roles.png) */
        snprintf(cmd, sizeof cmd, "cp -r \"arenas/%s\" \"arenas/%s\" && rm -rf \"arenas/%s/_gen\" \"arenas/%s/_orig\" \"arenas/%s/recipe.json\"", src, dst, dst, dst, dst);
        if (system(cmd)) { fprintf(stderr, "arena-plain: copy failed\n"); return 1; }
    }
    bad |= plain_view(src, dst, "ring");
    bad |= plain_view(src, dst, "ringside");
    snprintf(p, sizeof p, "arenas/%s/arena.json", dst);
    doc = json_parse_file(p, err, sizeof err);
    if (doc) {
        char d[200]; snprintf(d, sizeof d, "PLAIN TEMPLATE from %s: flat surfaces + geometry, ready for a theme (Arenas > AI recipe)", src);
        json_set_string(doc, "name", dst); json_set_string(doc, "description", d); json_set_number(doc, "template", 1);
        json_write_file(p, doc); json_free(doc);
    }
    fprintf(stderr, "arena-plain: arenas/%s written%s\n", dst, bad ? " (with errors)" : "");
    return bad;
}

/* --arena-slice NAME VIEW EMPTY.png [crowd=F1,F2..] [front=G1,G2..]
 * (user 2026-08-30: the template comes back from codex as WHOLE SCENE
 * pictures - the empty arena, the crowd cut out per animation frame, the
 * front crowd per frame).  Every picture is fitted to the view (padded with
 * opaque black at the bottom when only the height is short - the 960x240
 * ringside returns - box-scaled otherwise).  Under-plane crowd frame k =
 * EMPTY with crowd frame (k mod n) composited, cut by the layer file's
 * CURRENT alpha (the band's rails and posts stay in it); the overlay(s) are
 * cut from EMPTY alone; the over-plane front crowd frame k = front frame
 * (k mod n) with ITS OWN alpha (codex redrew the figures - the stock
 * silhouettes would clip them), or empty when none is given.  Rope pictures
 * are never written (stock ropes).  With one crowd frame nothing animates. */
static int fit_img(const char *path, int w, int h, uint8_t **out)
{
    img s; uint8_t *fit;
    if (iload(path, &s)) { fprintf(stderr, "arena-slice: cannot read %s\n", path); return -1; }
    fit = calloc((size_t)w * h, 4);
    if (s.w == w && s.h == h) memcpy(fit, s.px, (size_t)w * h * 4);
    else if (s.w == w && s.h < h) {                           /* short: the bottom rows are the black strip outside the world */
        memcpy(fit, s.px, (size_t)w * s.h * 4);
        for (int i = w * s.h; i < w * h; i++) fit[(size_t)i * 4 + 3] = 255;
        fprintf(stderr, "arena-slice: %s %dx%d padded to %dx%d\n", path, s.w, s.h, w, h);
    } else {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int sy0 = y * s.h / h, sy1 = (y + 1) * s.h / h, sx0 = x * s.w / w, sx1 = (x + 1) * s.w / w, acc[4] = { 0 }, cnt = 0;
            if (sy1 <= sy0) sy1 = sy0 + 1; if (sx1 <= sx0) sx1 = sx0 + 1;
            for (int yy = sy0; yy < sy1 && yy < s.h; yy++) for (int xx = sx0; xx < sx1 && xx < s.w; xx++) { const uint8_t *q = s.px + ((size_t)yy * s.w + xx) * 4; for (int c = 0; c < 4; c++) acc[c] += q[c]; cnt++; }
            if (!cnt) cnt = 1;
            for (int c = 0; c < 4; c++) fit[((size_t)y * w + x) * 4 + c] = (uint8_t)(acc[c] / cnt);
        }
        fprintf(stderr, "arena-slice: %s %dx%d scaled to %dx%d\n", path, s.w, s.h, w, h);
    }
    free(s.px); *out = fit;
    return 0;
}
static int split_list(const char *spec, char out[8][300])     /* "a,b,c" -> 3 */
{
    int n = 0; const char *p = spec;
    while (*p && n < 8) { const char *c = strchr(p, ','); size_t l = c ? (size_t)(c - p) : strlen(p); if (l >= 300) l = 299; memcpy(out[n], p, l); out[n][l] = 0; n++; if (!c) break; p = c + 1; }
    return n;
}
/* the stock rope pictures overlay the scene and their positions are gameplay
 * (rope lean / whip geometry): codex's own rope lines, a pixel or two off,
 * showed beside them (user 2026-08-30).  Erase them: every column of a band
 * 3 px around a stock rope pixel is interpolated from the pixel above the
 * band to the pixel below (ropes are thin horizontals). */
static void erase_under_ropes(uint8_t *px, int w, int h, const char *vd, const char *rope)
{
    char p[700]; img r; uint8_t *band;
    snprintf(p, sizeof p, "%s/%s", vd, rope);
    if (iload(p, &r)) return;
    if (r.w != w || r.h != h) { free(r.px); return; }
    band = calloc((size_t)w * h, 1);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (r.px[((size_t)y * w + x) * 4 + 3] >= 128)
        for (int dy = -3; dy <= 3; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy; if (xx >= 0 && yy >= 0 && xx < w && yy < h) band[yy * w + xx] = 1; }
    for (int x = 0; x < w; x++) for (int y = 0; y < h; y++) if (band[y * w + x]) {
        int y0 = y, y1 = y; const uint8_t *a, *b;
        while (y1 + 1 < h && band[(y1 + 1) * w + x]) y1++;
        a = px + ((size_t)(y0 > 0 ? y0 - 1 : y0) * w + x) * 4; b = px + ((size_t)(y1 + 1 < h ? y1 + 1 : y1) * w + x) * 4;
        for (int yy = y0; yy <= y1; yy++) {
            uint8_t *q = px + ((size_t)yy * w + x) * 4; float t = (float)(yy - y0 + 1) / (float)(y1 - y0 + 2);
            if (q[3] < 128) continue;
            for (int c = 0; c < 3; c++) q[c] = (uint8_t)((1 - t) * a[c] + t * b[c] + 0.5f);
        }
        y = y1;
    }
    free(band); free(r.px);
}
int tool_arena_slice(const char *name, const char *view, const char *scene, const char *opt1, const char *opt2, const char *opt3)
{
    char vd[400], p[700], err[128], cl[8][300], fl[8][300]; json_val *doc; const json_val *layers, *L, *O; img probe;
    uint8_t *base = NULL, *cf[8] = { 0 }, *ff[8] = { 0 }; int w, h, n = 0, nc = 0, nfr = 0;
    const char *opts[3] = { opt1, opt2, opt3 }; int sdx = 0, sdy = 0;
    for (int k = 0; k < 3; k++) {
        if (!opts[k] || !opts[k][0]) continue;
        if (!strncmp(opts[k], "crowd=", 6)) nc = split_list(opts[k] + 6, cl);
        else if (!strncmp(opts[k], "front=", 6)) nfr = split_list(opts[k] + 6, fl);
        else if (!strncmp(opts[k], "shift=", 6)) { if (!strcmp(opts[k] + 6, "auto")) sdy = 1 << 20; else sscanf(opts[k] + 6, "%d,%d", &sdx, &sdy); }   /* nudge the codex pictures into register; auto = measure (below) */
        else { snprintf(cl[0], 300, "%s", opts[k]); nc = 1; }              /* bare = one crowd picture */
    }
    snprintf(vd, sizeof vd, "arenas/%s/%s", name, view);
    snprintf(p, sizeof p, "%s/arena.json", vd);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) { fprintf(stderr, "arena-slice: no %s\n", p); return 1; }
    layers = json_get(doc, "layers"); L = json_get(layers, "under"); O = json_get(layers, "over");
    snprintf(p, sizeof p, "%s/%s", vd, json_str(json_get(L, "overlay"), "ring.png"));
    if (iload(p, &probe)) { fprintf(stderr, "arena-slice: cannot read %s\n", p); json_free(doc); return 1; }
    w = probe.w; h = probe.h; free(probe.px);
    if (fit_img(scene, w, h, &base)) { json_free(doc); return 1; }
    for (int k = 0; k < nc; k++) if (fit_img(cl[k], w, h, &cf[k])) { json_free(doc); return 1; }
    for (int k = 0; k < nfr; k++) if (fit_img(fl[k], w, h, &ff[k])) { json_free(doc); return 1; }
    if (sdy == (1 << 20)) {                                    /* shift=auto: register the fitted EMPTY picture against the STOCK
                                                                  composite (build/arena-preview/wwf/<view>_0.png) by cross-
                                                                  correlating the per-row / per-column mean-luma edge profiles
                                                                  over +-16 px (the hand measurement of 2026-08-30 in C) */
        char rp[400]; img r; sdx = sdy = 0;
        snprintf(rp, sizeof rp, "build/arena-preview/wwf/%s_0.png", view);
        if (iload(rp, &r) == 0 && r.w == w && r.h == h) {
            float *ea = calloc((size_t)h, sizeof *ea), *eb = calloc((size_t)h, sizeof *eb), *ca = calloc((size_t)w, sizeof *ca), *cb = calloc((size_t)w, sizeof *cb);
            for (int y = 0; y < h; y++) { double sa = 0, sb = 0; for (int x = w / 6; x < w - w / 6; x++) { const uint8_t *pa = r.px + ((size_t)y * w + x) * 4, *pb = base + ((size_t)y * w + x) * 4; sa += (299 * pa[0] + 587 * pa[1] + 114 * pa[2]) / 1000.0; sb += (299 * pb[0] + 587 * pb[1] + 114 * pb[2]) / 1000.0; } ea[y] = (float)(sa / w); eb[y] = (float)(sb / w); }
            for (int y = h - 1; y > 0; y--) { ea[y] = ea[y] - ea[y - 1]; if (ea[y] < 0) ea[y] = -ea[y]; eb[y] = eb[y] - eb[y - 1]; if (eb[y] < 0) eb[y] = -eb[y]; } ea[0] = eb[0] = 0;
            /* the ring BODY must register (mat edge, apron, posts, rails) - not the
               crowd: rows 40%..95% of the picture (ring view: mat + apron + floor;
               ringside: the far ring, rails, floor) */
            { double best = -1; int bd = 0, y0 = h * 2 / 5, y1 = h * 19 / 20; for (int d = -16; d <= 16; d++) { double sc = 0; for (int y = y0; y < y1; y++) if (y - d >= 0 && y - d < h) sc += ea[y] * eb[y - d]; if (sc > best) { best = sc; bd = d; } } sdy = bd; }
            for (int x = 0; x < w; x++) { double sa = 0, sb = 0; for (int y = 0; y < h; y++) { const uint8_t *pa = r.px + ((size_t)y * w + x) * 4, *pb = base + ((size_t)(y - sdy >= 0 && y - sdy < h ? y - sdy : y) * w + x) * 4; sa += (299 * pa[0] + 587 * pa[1] + 114 * pa[2]) / 1000.0; sb += (299 * pb[0] + 587 * pb[1] + 114 * pb[2]) / 1000.0; } ca[x] = (float)(sa / h); cb[x] = (float)(sb / h); }
            for (int x = w - 1; x > 0; x--) { ca[x] = ca[x] - ca[x - 1]; if (ca[x] < 0) ca[x] = -ca[x]; cb[x] = cb[x] - cb[x - 1]; if (cb[x] < 0) cb[x] = -cb[x]; } ca[0] = cb[0] = 0;
            { double best = -1; int bd = 0; for (int d = -8; d <= 8; d++) { double sc = 0; for (int x = w / 6; x < w - w / 6; x++) if (x - d >= 0 && x - d < w) sc += ca[x] * cb[x - d]; if (sc > best) { best = sc; bd = d; } } sdx = bd; }
            free(ea); free(eb); free(ca); free(cb); free(r.px);
            fprintf(stderr, "arena-slice: shift=auto -> %d,%d (vs %s)\n", sdx, sdy, rp);
        } else fprintf(stderr, "arena-slice: shift=auto: no stock composite %s (pack wwf first) - no shift\n", rp);
    }
    if (sdx || sdy) {                                          /* shift every fitted picture; uncovered rows/cols repeat the edge */
        uint8_t **all[17]; int na = 0; all[na++] = &base; for (int k = 0; k < nc; k++) all[na++] = &cf[k]; for (int k = 0; k < nfr; k++) all[na++] = &ff[k];
        for (int a = 0; a < na; a++) {
            uint8_t *src = *all[a], *dst = calloc((size_t)w * h, 4);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                int sx = x - sdx, sy = y - sdy;
                if (sx < 0) sx = 0; if (sx >= w) sx = w - 1; if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
                memcpy(dst + ((size_t)y * w + x) * 4, src + ((size_t)sy * w + sx) * 4, 4);
            }
            free(src); *all[a] = dst;
        }
        fprintf(stderr, "arena-slice: pictures shifted by %d,%d\n", sdx, sdy);
    }
    { static const char *ropes[2] = { "ropes.png", "ropes_back.png" };  /* codex's ropes out from under the stock ones */
      for (int r = 0; r < 2; r++) { erase_under_ropes(base, w, h, vd, ropes[r]); for (int k = 0; k < nc; k++) erase_under_ropes(cf[k], w, h, vd, ropes[r]); } }
    {   /* the under plane: crowd frames (EMPTY + crowd frame k mod n), then the overlay(s) from EMPTY */
        const json_val *fs = json_get(L, "frames"), *ovs = json_get(L, "overlays"); const char *ov1 = json_str(json_get(L, "overlay"), NULL);
        const char *files[32]; int nf = 0, ncrowd = 0;
        for (const json_val *q = fs ? fs->child : NULL; q && nf < 32; q = q->next) files[nf++] = json_str(q, "");
        ncrowd = nf;
        if (ov1 && nf < 32) files[nf++] = ov1;
        for (const json_val *z = ovs ? ovs->child : NULL; z && nf < 32; z = z->next) { int dup = 0; for (int j = 0; j < nf; j++) if (!strcmp(files[j], json_str(z, ""))) dup = 1; if (!dup) files[nf++] = json_str(z, ""); }
        for (int k = 0; k < nf; k++) {
            const char *f = files[k]; img m; const uint8_t *cfr = k < ncrowd && nc ? cf[k % nc] : NULL;
            if (!strncmp(f, "ropes", 5)) { fprintf(stderr, "arena-slice: %s kept (stock ropes)\n", f); continue; }
            snprintf(p, sizeof p, "%s/%s", vd, f);
            if (iload(p, &m) || m.w != w || m.h != h) { fprintf(stderr, "arena-slice: %s skipped\n", f); continue; }
            for (int i = 0; i < w * h; i++) if (m.px[(size_t)i * 4 + 3] >= 128) {
                const uint8_t *src = cfr && cfr[(size_t)i * 4 + 3] >= 128 ? cfr + (size_t)i * 4 : base + (size_t)i * 4;
                memcpy(m.px + (size_t)i * 4, src, 3); m.px[(size_t)i * 4 + 3] = 255;
            }
            /* WF_SLICE_ROPES=<arena>: the STOCK rope pixels of the same layer file
               (red / white / blue in the far ring's rope rows, y < 60) pasted back -
               the ring-out scene draws rope SPRITES at the ROM rows and codex's
               strands never land there (user 2026-08-30: regenerate without ropes) */
            if (getenv("WF_SLICE_ROPES") && getenv("WF_SLICE_ROPES")[0]) {
                char rp[700]; img r;
                snprintf(rp, sizeof rp, "arenas/%s/%s/%s", getenv("WF_SLICE_ROPES"), view, f);
                if (iload(rp, &r) == 0) {
                    int np = 0;
                    if (r.w == w && r.h == h) for (int y = 0; y < 60 && y < h; y++) for (int x = 0; x < w; x++) {
                        const uint8_t *q = r.px + ((size_t)y * w + x) * 4;
                        int red = q[0] > 200 && q[1] < 80 && q[2] < 80, white = q[0] > 220 && q[1] > 220 && q[2] > 220, blue = q[2] > 200 && q[0] < 80 && q[1] < 80;
                        if (q[3] >= 128 && (red || white || blue) && m.px[((size_t)y * w + x) * 4 + 3] >= 128) { memcpy(m.px + ((size_t)y * w + x) * 4, q, 3); np++; }
                    }
                    free(r.px);
                    if (np) fprintf(stderr, "arena-slice: %s: %d stock rope pixels pasted\n", f, np);
                }
            }
            wf_art_write_rgba_png(p, m.px, w, h); free(m.px); n++;
            fprintf(stderr, "arena-slice: %s <- %s\n", f, cfr ? (k % nc ? "empty + crowd frame 2" : "empty + crowd frame 1") : "empty scene");
        }
    }
    {   /* the over plane: the front crowd frames with their own alpha (ropes kept) */
        const json_val *fs = O ? json_get(O, "frames") : NULL; int k = 0;
        for (const json_val *q = fs ? fs->child : NULL; q; q = q->next, k++) {
            const char *f = json_str(q, ""); uint8_t *px;
            if (strncmp(f, "backcrowd", 9)) { fprintf(stderr, "arena-slice: %s kept\n", f); continue; }
            px = calloc((size_t)w * h, 4);
            if (nfr) for (int i = 0; i < w * h; i++) if (ff[k % nfr][(size_t)i * 4 + 3] >= 128) { memcpy(px + (size_t)i * 4, ff[k % nfr] + (size_t)i * 4, 3); px[(size_t)i * 4 + 3] = 255; }
            snprintf(p, sizeof p, "%s/%s", vd, f); wf_art_write_rgba_png(p, px, w, h); free(px); n++;
            fprintf(stderr, "arena-slice: %s <- %s\n", f, nfr ? "front crowd frame" : "empty (no front crowd)");
        }
    }
    free(base); for (int k = 0; k < nc; k++) free(cf[k]); for (int k = 0; k < nfr; k++) free(ff[k]); json_free(doc);
    fprintf(stderr, "arena-slice: %s/%s: %d pictures written\n", name, view, n);
    return n ? 0 : 1;
}
