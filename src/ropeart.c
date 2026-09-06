/* SIDE-ROPE ART (2026-08-30): an arena pak's "ropes"/"rtiles" sections
 * (tools/pack_ropes.c) drawn in place of the stream row 14 poses by
 * ringhw.c.  Cells are record-space (x from the origin, sprite y of the top
 * edge); a flipped frame (the right side) mirrors x -> -x - 16 with the
 * tile's flipx bit, exactly what the stream emitter does with bit15.  The
 * palette borrows a free stock bank (sprite.c) like a badge. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "pak.h"

#define RA_SCENES 8
#define RA_FRAMES 16
#define RA_CELLS 64
typedef struct { int n; struct { int16_t x, y; uint32_t tile; } c[RA_CELLS]; } ra_frame;
typedef struct { int nf; uint16_t pens[16]; int bank; uint32_t tile0; ra_frame f[RA_FRAMES]; } ra_set;
static ra_set sets[RA_SCENES];
static uint32_t tiles_used;
extern int eng_dbgsel;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

void eng_ropeart_unbind(unsigned scene) { if (scene < RA_SCENES) sets[scene].nf = 0; }

/* bind an arena pak's rope art to a scene word (arena.c, at arena load) */
void eng_ropeart_bind(unsigned scene, pak *pk)
{
    const uint8_t *sec, *p, *end; uint32_t len; ra_set *s;
    if (scene >= RA_SCENES || !pk) return;
    s = &sets[scene]; s->nf = 0; s->bank = -1;
    if ((sec = pak_section(pk, "rtiles", &len)) && len >= 4) {
        uint32_t n = rd32(sec);
        if (4 + n * 256 <= len && ENG_ROPE_TILE0 + tiles_used + n <= ENG_ROPE_TILE0 + 0x1000u) {
            s->tile0 = ENG_ROPE_TILE0 + tiles_used;
            wf_video_inject_sprite_tiles(s->tile0, n, sec + 4);
            tiles_used += n;
        } else { fprintf(stderr, "ropeart: scene %u: no room for %u tiles\n", scene, n); return; }
    } else return;
    if (!(sec = pak_section(pk, "ropes", &len)) || len < 4 + 32) return;
    p = sec; end = sec + len;
    s->nf = (int)rd32(p); p += 4; if (s->nf > RA_FRAMES) s->nf = RA_FRAMES;
    for (int k = 0; k < 16; k++) { s->pens[k] = rd16(p); p += 2; }
    for (int f = 0; f < s->nf && p + 2 <= end; f++) {
        int nc = rd16(p); p += 2;
        s->f[f].n = 0;
        for (int c = 0; c < nc && p + 8 <= end; c++) {
            if (s->f[f].n < RA_CELLS) { s->f[f].c[s->f[f].n].x = (int16_t)rd16(p); s->f[f].c[s->f[f].n].y = (int16_t)rd16(p + 2); s->f[f].c[s->f[f].n].tile = rd32(p + 4); s->f[f].n++; }
            p += 8;
        }
    }
    if (eng_dbgsel) fprintf(stderr, "ropeart: scene %u: %d frames\n", scene, s->nf);
}

int eng_ropeart_has(unsigned scene) { return scene < RA_SCENES && sets[scene].nf > 0; }
void eng_ropeart_reset_banks(void) { for (int i = 0; i < RA_SCENES; i++) sets[i].bank = -1; }

/* draw frame `frame` of the scene's set at object point (sx, sy); flip = the right side */
int eng_ropeart_emit(unsigned scene, unsigned frame, int flip, int sx, int sy, unsigned *slot)
{
    ra_set *s; const ra_frame *f;
    if (!eng_ropeart_has(scene)) return 0;
    s = &sets[scene];
    if (frame >= (unsigned)s->nf) return 0;
    f = &s->f[frame];
    if (s->bank < 0) s->bank = eng_sprite_borrow_bank(s->pens);
    for (int c = 0; c < f->n && *slot + 16 <= WF_SPRRAM_SIZE; c++) {
        uint8_t *r = wf.spriteram + *slot;
        int x = (flip ? -f->c[c].x : f->c[c].x) + sx, y = f->c[c].y + sy;   /* the stream flip: neg.b of the x byte (0x00D660) - the LEFT edge mirrors, the tile draws flipped from there */
        uint32_t tile = s->tile0 + f->c[c].tile;
        uint16_t w1 = 0x0001u | ((y & 0x100u) >> 7) | ((x & 0x100u) >> 6) | (flip ? 0x0010u : 0);
        memset(r, 0, 16);
        r[1] = (uint8_t)y; r[2] = (uint8_t)(w1 >> 8); r[3] = (uint8_t)w1;
        r[5] = (uint8_t)tile; r[7] = (uint8_t)(tile >> 8);
        r[9] = (uint8_t)s->bank; r[11] = (uint8_t)x;
        r[12] = 0xE7; r[13] = (uint8_t)((tile >> 16) & 0x0Fu); r[14] = 0x5C;      /* the clone-art arena marker */
        *slot += 16;
    }
    return 1;
}
