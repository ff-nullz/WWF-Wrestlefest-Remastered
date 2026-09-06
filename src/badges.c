/* BADGES — engine-drawn over-head chips with pak art (build/badges.pak,
 * tools/pack_badges.c).  The ROM's row-0x1E chips (1P-4P, CHANGE UP) stay
 * ROM art; a badge is anything the engine adds (the CPU chip of the
 * badge_cpu mod rule).  Tiles live in the BADGE tile arena (ENG_BADGE_TILE0,
 * injected once), the badge's palette borrows a free stock bank the way a
 * weapon palette does (sprite.c eng_sprite_borrow_bank). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "pak.h"

#define MAX_BADGES 8
#define MAX_CELLS  36
typedef struct { char name[16]; uint16_t pens[16]; int w, h, n; struct { int16_t x, y; uint32_t tile; } c[MAX_CELLS]; int bank; } badge_t;
static badge_t badges[MAX_BADGES]; static int nbadges, loaded;
extern int eng_dbgsel;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

static void badges_load(void)
{
    pak *pk; const uint8_t *sec; uint32_t len;
    loaded = 1;
    pk = pak_open("build/badges.pak");
    if (!pk) return;
    if ((sec = pak_section(pk, "btiles", &len)) && len >= 4) {
        uint32_t n = rd32(sec);
        if (4 + n * 256 <= len) wf_video_inject_sprite_tiles(ENG_BADGE_TILE0, n, sec + 4);
    }
    if ((sec = pak_section(pk, "badges", &len)) && len >= 4) {
        const uint8_t *p = sec + 4, *end = sec + len; int n = (int)rd32(sec);
        for (int i = 0; i < n && i < MAX_BADGES && p + 16 + 32 + 6 <= end; i++) {
            badge_t *b = &badges[nbadges];
            memcpy(b->name, p, 16); b->name[15] = 0; p += 16;
            for (int k = 0; k < 16; k++) { b->pens[k] = rd16(p); p += 2; }
            b->w = rd16(p); b->h = rd16(p + 2); b->n = rd16(p + 4); p += 6;
            if (b->n > MAX_CELLS) b->n = MAX_CELLS;
            for (int c = 0; c < b->n && p + 8 <= end; c++) { b->c[c].x = (int16_t)rd16(p); b->c[c].y = (int16_t)rd16(p + 2); b->c[c].tile = rd32(p + 4); p += 8; }
            b->bank = -1; nbadges++;
        }
    }
    if (eng_dbgsel) fprintf(stderr, "badges: %d loaded\n", nbadges);
    /* the pak memory stays mapped: the tiles were copied, the cells too */
}

/* the body-palette install rewrote every bank: forget the borrowed ones */
void eng_badges_reset_banks(void) { for (int i = 0; i < nbadges; i++) badges[i].bank = -1; }

/* draw badge `name` with its origin (horizontal centre, bottom edge) at
 * sprite (sx, sy); 1 = drawn */
int eng_badge_emit(const char *name, int sx, int sy, unsigned *slot)
{
    badge_t *b = NULL;
    if (!loaded) badges_load();
    for (int i = 0; i < nbadges; i++) if (!strcmp(badges[i].name, name)) b = &badges[i];
    if (!b) return 0;
    if (b->bank < 0) b->bank = eng_sprite_borrow_bank(b->pens);
    for (int c = 0; c < b->n && *slot + 16 <= WF_SPRRAM_SIZE; c++) {
        uint8_t *r = wf.spriteram + *slot;
        int x = b->c[c].x + sx, y = b->c[c].y + sy;
        uint16_t w1 = 0x0001u | ((y & 0x100u) >> 7) | ((x & 0x100u) >> 6);
        memset(r, 0, 16);
        r[1] = (uint8_t)y; r[2] = (uint8_t)(w1 >> 8); r[3] = (uint8_t)w1;
        r[5] = (uint8_t)b->c[c].tile; r[7] = (uint8_t)(b->c[c].tile >> 8);
        r[9] = (uint8_t)b->bank; r[11] = (uint8_t)x;
        r[12] = 0xE7; r[13] = (uint8_t)((b->c[c].tile >> 16) & 0x0Fu); r[14] = 0x5C;   /* the clone-art arena marker */
        *slot += 16;
    }
    return 1;
}
