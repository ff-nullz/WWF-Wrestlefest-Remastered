/* CROWD ANIMATION — the engine's own, not the ROM's.
 *
 * The ROM animates its crowd by re-stamping 4x4-cell patches into the
 * tilemap (scenery.c is that routine, kept for the stock ROM data).  An
 * imported arena does not use it.  Here a crowd is any number of REGIONS:
 * a rectangle of source-grid cells on one plane, N complete pictures of
 * that rectangle (one per step) and a tick count per step.  Every cell of
 * the rectangle animates - there are no footprints to respect, and the
 * rectangle can be anything the author wants (the crowd tiers, the
 * entrance banner, the whole plane).
 *
 * Two hooks into the arena composer (arena.c):
 *   wf_crowd_cell()  the LIVE cell record for a grid cell - the composer
 *                    reads it whenever it (re)composes the torus, so a
 *                    camera move shows the current step, not the base;
 *   wf_crowd_tick()  once per rendered frame: advance the step timers and
 *                    stamp a region that stepped straight into the torus
 *                    (bus + renderer shadow) for the cells inside the
 *                    composer's window.
 *
 * Pak block (after the tiles of an arena section, big-endian):
 *   u16 nregion; per region: u8 layer (1 = BG), u8 nsteps, u16 x, u16 y,
 *   u16 w, u16 h (source-grid cells), nsteps x { u8 ticks, w*h x
 *   { u16 code, u8 bank, u8 flip } }
 * Timing: a step's count starts at its ticks and the step advances when
 * the count would go below zero (count-- == 0), the ROM's own cadence, so
 * a stock arena's crowd keeps its rhythm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"

#define CROWD_SCENES 8
#define MAX_REGIONS 16

typedef struct { int layer, x, y, w, h, nsteps; const uint8_t *steps; int step, count; } region_t;
typedef struct { int x0, r0, W, H, n; region_t r[MAX_REGIONS]; } crowd_t;
static crowd_t crowds[CROWD_SCENES];
static int cur_scene = -1;
extern int eng_dbgsel;

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

void wf_crowd_unbind(unsigned scene) { if (scene < CROWD_SCENES) crowds[scene].n = 0; }

/* parse a section's crowd block; the grid geometry is the section's */
void wf_crowd_bind(unsigned scene, const uint8_t *blk, uint32_t len, int x0, int r0, int W, int H)
{
    crowd_t *c; const uint8_t *p, *end;
    if (scene >= CROWD_SCENES) return;
    c = &crowds[scene]; c->n = 0; c->x0 = x0; c->r0 = r0; c->W = W; c->H = H;
    if (!blk || len < 2) return;
    p = blk + 2; end = blk + len;
    for (int i = 0, n = be16(blk); i < n && c->n < MAX_REGIONS; i++) {
        region_t *r = &c->r[c->n]; size_t stepbytes;
        if (p + 10 > end) break;
        r->layer = p[0]; r->nsteps = p[1]; r->x = be16(p + 2); r->y = be16(p + 4); r->w = be16(p + 6); r->h = be16(p + 8);
        p += 10; r->steps = p; stepbytes = 1u + (size_t)r->w * r->h * 4u;
        if (!r->nsteps || !r->w || !r->h || p + stepbytes * r->nsteps > end) break;
        p += stepbytes * r->nsteps;
        r->step = 0; r->count = r->steps[0];
        c->n++;
    }
    if (eng_dbgsel) fprintf(stderr, "crowd: scene %u: %d region%s\n", scene, c->n, c->n == 1 ? "" : "s");
}

static const uint8_t *step_cell(const region_t *r, int gcx, int gcy)
{
    return r->steps + (size_t)r->step * (1u + (size_t)r->w * r->h * 4u) + 1u + ((size_t)(gcy - r->y) * r->w + (size_t)(gcx - r->x)) * 4u;
}

/* the live record of a grid cell, 1 = inside a region of this plane (the
 * last region listed wins where two overlap) */
int wf_crowd_cell(unsigned scene, int plane, int gcx, int gcy, uint8_t out[4])
{
    const crowd_t *c; int hit = 0;
    if (scene >= CROWD_SCENES) return 0;
    c = &crowds[scene];
    for (int i = 0; i < c->n; i++) {
        const region_t *r = &c->r[i];
        if (r->layer != plane || gcx < r->x || gcx >= r->x + r->w || gcy < r->y || gcy >= r->y + r->h) continue;
        memcpy(out, step_cell(r, gcx, gcy), 4); hit = 1;
    }
    return hit;
}

/* stamp the region's current step into the torus - the cells whose world
 * unit lies inside the composer's 16x16-unit window (ox, oy) */
static void stamp_region(const crowd_t *c, const region_t *r, uint16_t ox, uint16_t oy)
{
    unsigned vbase = r->layer ? WF_BGRAM_BASE : WF_FGRAM_BASE;
    for (int gcy = r->y; gcy < r->y + r->h; gcy++)
        for (int gcx = r->x; gcx < r->x + r->w; gcx++) {
            uint16_t x = (uint16_t)(c->x0 + gcx / 2), y = (uint16_t)(0x40 - (c->r0 + gcy / 2));
            unsigned d1, d2, cx, cy; const uint8_t *cell; unsigned code, bank, flip;
            if ((uint16_t)(x - ox) >= 16 || (uint16_t)(oy - y) >= 16) continue;      /* outside the window */
            d1 = (unsigned)((uint16_t)(x + 0x10) & 0x0F); d2 = (unsigned)((uint16_t)(-(uint16_t)(y - 0x40)) & 0x0F);
            cx = d1 * 2 + (unsigned)(gcx & 1); cy = d2 * 2 + (unsigned)(gcy & 1);
            cell = step_cell(r, gcx, gcy); code = be16(cell); bank = cell[2]; flip = cell[3];
            if (r->layer) { unsigned ad = vbase + cy * 0x40 + cx * 2; uint16_t w = (uint16_t)((code & 0x0FFFu) | ((bank & 0x0Fu) << 12));
                            m68k_write_memory_16(ad, w); wf_tilemap_shadow_write(ad, w, 1); }
            else { unsigned ad = vbase + cy * 0x80 + cx * 4; uint16_t w0 = (uint16_t)((bank & 0x0Fu) | (flip & 0xC0u)), w1 = (uint16_t)(code & 0x1FFFu);
                   m68k_write_memory_16(ad, w0); wf_tilemap_shadow_write(ad, w0, 1);
                   m68k_write_memory_16(ad + 2, w1); wf_tilemap_shadow_write(ad + 2, w1, 1); }
        }
}

/* once per rendered frame of an imported-arena scene */
void wf_crowd_tick(const eng_state *st, unsigned scene)
{
    crowd_t *c;
    uint16_t ox = (uint16_t)(((unsigned)st->cam_x >> 5) - 3), oy = (uint16_t)(((unsigned)st->cam_y >> 5) + 4);
    if (scene >= CROWD_SCENES) return;
    c = &crowds[scene];
    if (cur_scene != (int)scene) {                    /* a new scene: every region restarts at step 0 */
        cur_scene = (int)scene;
        for (int i = 0; i < c->n; i++) { c->r[i].step = 0; c->r[i].count = c->r[i].steps[0]; stamp_region(c, &c->r[i], ox, oy); }
    }
    for (int i = 0; i < c->n; i++) {
        region_t *r = &c->r[i];
        if (r->count-- == 0) {
            r->step = (r->step + 1) % r->nsteps;
            r->count = r->steps[(size_t)r->step * (1u + (size_t)r->w * r->h * 4u)];
            stamp_region(c, r, ox, oy);
            if (getenv("WF_ANIMDBG")) fprintf(stderr, "crowd f%ld: scene %u region %d -> step %d (%d ticks)\n", (long)st->frame, scene, i, r->step, r->count);
        }
    }
}
