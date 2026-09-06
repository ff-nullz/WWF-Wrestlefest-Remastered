/* ARENA OVERRIDES — library arena art (tools/export_arena.c packs
 * arenas/<name>/{ring,ringside}/ into build/arenas/<name>.pak, sections
 * "ring" / "ringside"; the profile names its arenas per in-ring scene).
 *
 * An override replaces, for one scene word, everything the ROM composer
 * would have produced: the 32x32-cell FG and BG tilemaps (stored as the
 * torus the export captured, anchored at the world unit window it was
 * composed at), both 16-bank tile palettes, and the 16x16 tiles the cells
 * reference — installed over the shared tile sheet while the scene is up
 * and put back when a stock scene is composed (the FG/BG planes carry
 * nothing but the scene picture, so the swap is invisible elsewhere).
 * The ROM's crowd patch stamper (scenery.c) stands down for an override
 * scene: the arena's crowd is crowd.c's (regions, bound from the section's
 * crowd block).
 *
 * Section layout (all big-endian):
 *   u8  scene, u8 pad, u16 ntiles, u16 x0 (first unit), u16 r0 (top row =
 *   0x40 - y), u16 W, u16 H (units)
 *   fg cells (W*2 x H*2) x { u16 code, u8 bank, u8 flip(b6 x, b7 y) }
 *   bg cells same count x { u16 code, u8 bank, u8 0 }
 *   fg palette 16 x 16 x u16 xBGR444, bg palette same
 *   tiles ntiles x 256 pens
 *   [crowd block] see crowd.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "pak.h"
#include "profile.h"

#define ARENA_SCENES 8
#define CELLS (32 * 32)
#define HDR 12
#define PAL_BYTES 512

typedef struct {
    const uint8_t *sec; uint32_t len;
    unsigned ntiles; uint16_t x0, r0, W, H;
    uint32_t cellbytes;                /* one plane's cells */
    int base;                          /* the stock scene the view derives from (priority byte) */
} arena_t;

static int loaded;
static arena_t arenas[ARENA_SCENES];
static int tiles_scene = -1;           /* scene whose tiles are installed, -1 = stock */

extern int eng_dbgsel;
void wf_video_tiles_override(const uint8_t *pens, unsigned n);   /* video.c */
void wf_video_tiles_restore(void);

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

int wf_arena_out_scene(int in_scene)   /* the ring-out view paired with an in-ring scene, -1 = none */
{
    return in_scene == 0 ? 2 : in_scene == 5 ? 6 : -1;
}

static int arena_take(int scene, const uint8_t *b, uint32_t len, const char *from)
{
    arena_t *a;
    if (scene < 0 || scene >= ARENA_SCENES || !b || len < HDR) return 0;
    a = &arenas[scene];
    a->sec = b; a->len = len;
    a->ntiles = be16(b + 2); a->x0 = be16(b + 4); a->r0 = be16(b + 6); a->W = be16(b + 8); a->H = be16(b + 10);
    a->base = b[1] < 8 ? b[1] : 0;
    a->cellbytes = (uint32_t)a->W * 2u * a->H * 2u * 4u;
    if (!a->W || !a->H || len < HDR + 2 * a->cellbytes + 2 * PAL_BYTES + (uint32_t)a->ntiles * 256u) { a->sec = NULL; return 0; }
    {   uint32_t ao = HDR + 2 * a->cellbytes + 2 * PAL_BYTES + (uint32_t)a->ntiles * 256u;
        if (len >= ao + 2) wf_crowd_bind((unsigned)scene, b + ao, len - ao, a->x0, a->r0, a->W, a->H); else wf_crowd_unbind((unsigned)scene); }
    if (eng_dbgsel) fprintf(stderr, "arena: scene %d <- %s (%ux%u units at %u,%u; %u tiles)\n", scene, from, a->W, a->H, a->x0, a->r0, a->ntiles);
    return 1;
}

/* a scene page (sceneplay.c): bind a section as the arena of a scene word
 * for the scene's duration, release = back to whatever the profile had */
static arena_t saved[ARENA_SCENES]; static int saved_on[ARENA_SCENES];
static void arena_load(void);
void wf_arena_bind(unsigned scene, const uint8_t *sec, uint32_t len)
{
    if (scene >= ARENA_SCENES) return;
    arena_load();
    if (!saved_on[scene]) { saved[scene] = arenas[scene]; saved_on[scene] = 1; }
    if (!arena_take((int)scene, sec, len, "scene page")) arenas[scene].sec = NULL;
    if (tiles_scene == (int)scene) tiles_scene = -1;      /* the sheet must be re-installed */
}
void wf_arena_release(unsigned scene)
{
    if (scene >= ARENA_SCENES || !saved_on[scene]) return;
    arenas[scene] = saved[scene]; saved_on[scene] = 0;
    if (arenas[scene].sec) { uint32_t ao = HDR + 2 * arenas[scene].cellbytes + 2 * PAL_BYTES + (uint32_t)arenas[scene].ntiles * 256u;
        if (arenas[scene].len >= ao + 2) wf_crowd_bind(scene, arenas[scene].sec + ao, arenas[scene].len - ao, arenas[scene].x0, arenas[scene].r0, arenas[scene].W, arenas[scene].H); else wf_crowd_unbind(scene); }
    else wf_crowd_unbind(scene);
    if (tiles_scene == (int)scene) {                      /* the page's sheet is still installed: put the
                                                             stock sheet back NOW (was: forget it, and the
                                                             next stock scene - no override, tiles_scene < 0 -
                                                             never restored it: round 2 after the talk screen
                                                             drew the Challenge arena with the page's tiles =
                                                             black/white stripes, user 2026-08-30) */
        wf_video_tiles_restore();
        tiles_scene = -1;
    }
}

/* the profile's "arenas": {"<in-ring scene>": "<name>"} -> build/arenas/<name>.pak
 * (sections "ring" and "ringside"); the stock profile names none and keeps the ROM */
static pak *apaks[8]; static int napaks;
static void arena_load(void)
{
    int scene; char name[64], path[300];
    if (loaded) return;
    loaded = 1;
    if (getenv("WF_NOARENAPAK")) return;
    for (int i = 0; wf_profile_arena_assign(i, &scene, name, sizeof name); i++) {
        pak *pk; const uint8_t *b; uint32_t len;
        snprintf(path, sizeof path, "build/arenas/%s.pak", name);
        pk = pak_open(path);
        if (!pk) { fprintf(stderr, "arena: profile names arena '%s' but %s is missing (pack the profile)\n", name, path); continue; }
        if (napaks < 8) apaks[napaks++] = pk;
        b = pak_section(pk, "ring", &len);     arena_take(scene, b, len, path);
        eng_ropeart_bind((unsigned)scene, pk);                           /* the side-rope art, if the pak has it */
        b = pak_section(pk, "ringside", &len); if (b && wf_arena_out_scene(scene) >= 0) arena_take(wf_arena_out_scene(scene), b, len, path);
    }
}

/* the stock scene an override derives from (its layer-priority byte lives
 * there; the extra ring 7 has 0xFF in the ROM table) */
int wf_arena_base_scene(unsigned scene)
{
    arena_load();
    return (scene < ARENA_SCENES && arenas[scene].sec) ? arenas[scene].base : (int)scene;
}

int wf_arena_has(unsigned scene)
{
    arena_load();
    return scene < ARENA_SCENES && arenas[scene].sec != NULL;
}

/* the tiles of `scene` over the shared sheet (or the stock sheet back) */
void wf_arena_tiles_for_scene(unsigned scene)
{
    arena_load();
    if (scene < ARENA_SCENES && arenas[scene].sec) {
        if (tiles_scene == (int)scene) return;
        const arena_t *a = &arenas[scene];
        wf_video_tiles_override(a->sec + HDR + 2 * a->cellbytes + 2 * PAL_BYTES, a->ntiles);
        tiles_scene = (int)scene;
    } else if (tiles_scene >= 0) {
        wf_video_tiles_restore();
        tiles_scene = -1;
    }
}

/* Fill one plane of the 32x32 torus for the composer's unit window (ox, oy =
 * the top-left unit, 16x16 units, y descending per row - the 0x26EA6 walk):
 * every unit inside the arena's source grid lands in its torus cell exactly
 * as the ROM walk would place it; units outside the grid are blank. */
int wf_arena_compose(uint8_t *out, unsigned outsize, int plane, unsigned scene, uint16_t ox, uint16_t oy)
{
    const arena_t *a;
    arena_load();
    if (scene >= ARENA_SCENES || !arenas[scene].sec) return 0;
    a = &arenas[scene];
    memset(out, 0, outsize);
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            uint16_t x = (uint16_t)(ox + col), y = (uint16_t)(oy - row);
            unsigned gx = (uint16_t)(x - a->x0), gr = (uint16_t)((0x40 - y) - a->r0);   /* grid unit */
            unsigned d1 = (unsigned)((uint16_t)(x + 0x10) & 0x0F), d2 = (unsigned)((uint16_t)(-(uint16_t)(y - 0x40)) & 0x0F);
            if (gx >= a->W || gr >= a->H) continue;
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++) {
                    unsigned gcx = gx * 2 + (unsigned)sx, gcy = gr * 2 + (unsigned)sy, gi = gcy * (a->W * 2u) + gcx;
                    unsigned cx = d1 * 2 + (unsigned)sx, cy = d2 * 2 + (unsigned)sy, ci = cy * 32 + cx;
                    const uint8_t *c = a->sec + HDR + (plane ? a->cellbytes : 0) + gi * 4;
                    uint8_t live[4]; unsigned code, bank, flip;
                    if (wf_crowd_cell(scene, plane, (int)gcx, (int)gcy, live)) c = live;   /* the crowd's current step */
                    code = be16(c); bank = c[2]; flip = c[3];
                    if (plane) {                       /* BG: 2-byte cell, 12-bit code, bank in the top nibble */
                        unsigned o = ci * 2; if (o + 2 > outsize) continue;
                        uint16_t w = (uint16_t)((code & 0x0FFFu) | ((bank & 0x0Fu) << 12));
                        out[o] = (uint8_t)(w >> 8); out[o + 1] = (uint8_t)w;
                    } else {                           /* FG: 4-byte cell, w0 = bank | flips, w1 = 13-bit code */
                        unsigned o = ci * 4; if (o + 4 > outsize) continue;
                        uint16_t w0 = (uint16_t)((bank & 0x0Fu) | (flip & 0xC0u)), w1 = (uint16_t)(code & 0x1FFFu);
                        out[o] = (uint8_t)(w0 >> 8); out[o + 1] = (uint8_t)w0; out[o + 2] = (uint8_t)(w1 >> 8); out[o + 3] = (uint8_t)w1;
                    }
                }
        }
    }
    return 1;
}

/* the 2 x 512-byte palette blobs (16 lines x 16 pens x BE word), the
 * shape pal_load.c's pal_blit reads from ROM */
const uint8_t *wf_arena_palette(unsigned scene, int plane)
{
    arena_load();
    if (scene >= ARENA_SCENES || !arenas[scene].sec) return NULL;
    return arenas[scene].sec + HDR + 2 * arenas[scene].cellbytes + (plane ? PAL_BYTES : 0);
}

/* ---- animation: the ROM's 0x285DA slot machine (scenery.c) driven by the
 * arena's own patches.  Placements cull by camera 64-px units exactly as
 * 0x28608; a patch is stamped at the torus cell its grid cell maps to for
 * the composed window (the same unit -> torus mapping the composer uses). */
/* 1 = handled (an override scene: crowd.c runs its regions), 0 = let the ROM stamper run */
int wf_arena_scenery_tick(const eng_state *st)
{
    unsigned scene = (unsigned)st->scene;
    if (!wf_arena_has(scene)) return 0;
    wf_crowd_tick(st, scene);
    return 1;
}
