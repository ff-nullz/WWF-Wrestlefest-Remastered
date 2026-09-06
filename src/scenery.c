/* Scene tile-animation overlay — transcription of the ROM 0x285DA cluster
 * (cull 0x28608, slot machine 0x28896/0x28B46, IRQ3 stamper 0x2946A).
 * Spec: docs/engine-specs/tilemap-maint.md (2026-08-22).
 *
 * The raw scene map contains static placeholder art (the "phantom ring"
 * hooks); the stock game stamps prebuilt 8x4-tile animation patches
 * (crowd, ramp) over fixed map-anchored cells every few frames. The
 * engine recomposes the map on camera change, so each frame it re-stamps
 * the current frame of every active slot — byte-identical to stock in
 * the visible window, at trivial cost (<= 0x2F patches of 64 bytes).
 *
 * ROM data (per scene): placement triples 0x2867A[scene] (cellX, cellY,
 * animId; negative terminator), script pointers 0x28C66[scene]
 * (script: [0] last frame, [1] sync group, then (cellByte, duration)
 * pairs), patch pairs 0x295A4[scene] = {offsetTable, patchData} for FG
 * and 0x295DC[scene] for BG. Cell byte bit7 selects the BG tables.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"

#define TAB_PLACE   0x2867Au
#define TAB_SCRIPT  0x28C66u
#define TAB_PAIR_FG 0x295A4u
#define TAB_PAIR_BG 0x295DCu
#define MAX_SLOTS   0x2F

/* Animated-scenery data (docs/adr-001, group base/scene): the 0x285DA
 * placement lists, the 0x28808 script tables, and the 0x2946A IRQ3 stamper's
 * per-scene {cell offset table, patch tile data} pairs. Seven scenes in each
 * pointer table (the next table / routine starts right after entry 6). */
static const tbl_def scenery_tables[] = {
    { "scene_place_ptrs",          "base/scene", 0x2867A, 7 * 4, TK_U32, 1,
      "0x285DA: per scene the ROM address of its placement list" },
    { "scene_place_lists",         "base/scene", 0x28696, 0x28808 - 0x28696, TK_U8, 3,
      "placement triples {camera cell x, cell y, scenery id}, list ends on x bit7 (0xFF); 0x28608 culls by camera units" },
    { "scene_script_ptrs",         "base/scene", 0x28C66, 7 * 4, TK_U32, 1,
      "0x28808: per scene the ROM address of its script table (long per scenery id)" },
    { "scene_scripts",             "base/scene", 0x28C82, 0x2946A - 0x28C82, TK_U8, 16,
      "per-scene script tables (longs, indexed by scenery id) and the scripts {nframes, sync, (cell id, count)*} they point to (0x28B46 sync groups)" },
    { "scene_patch_pairs_fg",      "base/scene", 0x295A4, 7 * 8, TK_U32, 2,
      "0x2946A stamper, FG: per scene {cell offset table, patch data}; cell c -> VRAM offset word, 64-byte patch (8 tiles x 4 rows)" },
    { "scene_patch_pairs_bg",      "base/scene", 0x295DC, 7 * 8, TK_U32, 2,
      "0x2946A stamper, BG: per scene {cell offset table, patch data}; cell c -> VRAM offset word, 32-byte patch (4 tiles x 4 rows)" },
    { "scene_patch_cell_offsets",  "base/scene", 0x29614, 0x2983C - 0x29614, TK_U16, 1,
      "the cell-offset tables the patch pairs point into (torus VRAM addresses of fixed map cells), up to the 0x2983C scroll-limit code" },
    { "scene_patch_tiles",         "base/scene", 0x29A2C, 0x2ED0C - 0x29A2C, TK_U8, 16,
      "the patch tile data the pairs point into (FG 64 bytes/cell, BG 32 bytes/cell), up to the scene tile blocks at 0x2ED0C" },
};
TBL_REGISTER(scenery_tables)

static uint16_t rom16(uint32_t a) { return (uint16_t)((tbl_ra8(a) << 8) | tbl_ra8(a + 1)); }
static uint32_t rom32(uint32_t a) { return ((uint32_t)rom16(a) << 16) | rom16(a + 2); }

typedef struct {
    int active;
    uint8_t id, frame, count, sync;
} slot_t;

static slot_t slots[MAX_SLOTS];
static int cur_scene = -1;

/* IRQ3 stamper 0x2946A: one patch = FG 8 tiles x 4 rows (16 bytes/row,
 * row stride 0x80), BG 4 tiles x 4 rows (8 bytes/row, stride 0x40).
 * Offsets are per-cell constants = torus addresses of fixed map cells. */
static void stamp(int scene, uint8_t cell, int bg)
{
    uint8_t c = cell;
    uint32_t pair = (bg ? TAB_PAIR_BG : TAB_PAIR_FG) + (uint32_t)scene * 8u;
    uint32_t off_tab = rom32(pair);
    uint32_t data = rom32(pair + 4);
    uint16_t dst = rom16(off_tab + (uint32_t)c * 2u);
    uint8_t *vram = bg ? wf.bg_videoram : wf.fg_videoram;
    unsigned vsize = bg ? WF_BGRAM_SIZE : WF_FGRAM_SIZE;
    unsigned rowb = bg ? 8u : 16u;
    unsigned stride = bg ? 0x40u : 0x80u;
    uint32_t src = data + (uint32_t)c * (bg ? 32u : 64u);

    for (unsigned r = 0; r < 4; r++) {
        uint32_t d = dst + r * stride;
        if (d + rowb <= vsize) {
            { const uint8_t *sp = tbl_ra_ptr(src + r * rowb, rowb); if (!sp) continue; memcpy(vram + d, sp, rowb); }
            /* The IRQ3 stamper stores through the bus; the renderer's
             * C-owned tilemap shadow (scene_map.c) only sees bus stores,
             * so mirror each word the way stubs.c would. */
            for (unsigned k = 0; k < rowb; k += 2)
                wf_tilemap_shadow_write((bg ? WF_BGRAM_BASE : WF_FGRAM_BASE) + d + k,
                                        ((unsigned)vram[d + k] << 8) | vram[d + k + 1], 1);
        }
    }
}

static const uint8_t *script(int scene, uint8_t id)
{
    uint32_t tab = rom32(TAB_SCRIPT + (uint32_t)scene * 4u);
    uint32_t a = rom32(tab + (uint32_t)id * 4u);
    /* {last frame, sync, (cell, count) x (last frame + 1)}: 2 + 2*(n+1)
     * bytes — the last script of the block (0x29458, n=7) ends exactly at
     * the 0x2946A routine, so an over-request here reads past the table. */
    return tbl_ra_ptr(a, 2u + 2u * (tbl_ra8(a) + 1u));
}

/* 0x285DA: cull by camera units, maintain slots, tick, stamp. */
void eng_scenery_tick(const eng_state *st)
{
    int scene = st->scene;
    unsigned ux = ((unsigned)st->cam_x >> 5), uy = ((unsigned)st->cam_y >> 5);
    /* 0x28608 cull bounds (unsigned byte compares) */
    uint8_t xlo = (uint8_t)(((ux + 1) >> 1) - 2), xhi = (uint8_t)(((ux - 1) >> 1) + 6);
    uint8_t yhi = (uint8_t)(((uy + 1) >> 1) + 2), ylo = (uint8_t)(yhi - 8);
    uint8_t want[0x30];
    int n_want = 0;
    uint32_t list = rom32(TAB_PLACE + (uint32_t)scene * 4u);

    if (scene != cur_scene) {          /* 0x26E66's $1C185A=FF reset */
        memset(slots, 0, sizeof slots);
        cur_scene = scene;
    }
    if (wf_arena_scenery_tick(st)) return;      /* imported arena: its own patches (or static) */

    for (;;) {                         /* placement triples, 0x28654: x first, bmi ends the list */
        uint8_t cx = tbl_ra8(list), cy, id;
        if (cx & 0x80u)
            break;
        cy = tbl_ra8(list + 1); id = tbl_ra8(list + 2);
        list += 3;
        if (cx >= xlo && cx < xhi && cy >= ylo && cy < yhi
            && n_want < (int)sizeof want)
            want[n_want++] = id;
    }

    /* 0x2884E deactivate */
    for (int s = 0; s < MAX_SLOTS; s++) {
        int keep = 0;
        if (!slots[s].active)
            continue;
        for (int i = 0; i < n_want; i++)
            if (want[i] == slots[s].id)
                keep = 1;
        if (!keep)
            slots[s].active = 0;
    }
    /* 0x28808 activate */
    for (int i = 0; i < n_want; i++) {
        int have = -1, free_s = -1;
        for (int s = 0; s < MAX_SLOTS; s++) {
            if (slots[s].active && slots[s].id == want[i]) have = s;
            else if (!slots[s].active && free_s < 0) free_s = s;
        }
        if (have >= 0 || free_s < 0)
            continue;
        {
            slot_t *sl = &slots[free_s];
            const uint8_t *sc = script(scene, want[i]);
            if (!sc) continue;         /* data-layer miss: leave the slot free */
            sl->active = 1;
            sl->id = want[i];
            sl->sync = sc[1];
            sl->frame = 0;
            sl->count = sc[3];
            if (sl->sync) {            /* sync-group phase copy, 0x28B46 */
                for (int s = 0; s < MAX_SLOTS; s++)
                    if (s != free_s && slots[s].active
                        && slots[s].sync == sl->sync) {
                        sl->frame = slots[s].frame;
                        sl->count = slots[s].count;
                        break;
                    }
            }
        }
    }
    /* tick + stamp current frame of every active slot (the recompose per
     * camera change wipes stamps, so re-stamp each frame — same bytes). */
    for (int s = 0; s < MAX_SLOTS; s++) {
        slot_t *sl = &slots[s];
        const uint8_t *sc;
        if (!sl->active)
            continue;
        sc = script(scene, sl->id);
        if (!sc) { sl->active = 0; continue; }
        if (sl->count-- == 0) {
            if (++sl->frame > sc[0])
                sl->frame = 0;
            sl->count = sc[3 + sl->frame * 2];
        }
        /* 0x28C16: the layer is bit 7 of the slot's +0x03 = the script's
         * sync byte (0x28B74), not of the cell (scene 4: sync 0x81 = BG). */
        stamp(scene, sc[2 + sl->frame * 2], (sc[1] & 0x80u) != 0);
        if (getenv("WF_ANIMDBG")) { uint8_t c = sc[2 + sl->frame * 2]; int bg = (sc[1] & 0x80u) != 0; uint32_t pair = (bg ? TAB_PAIR_BG : TAB_PAIR_FG) + (uint32_t)scene * 8u; uint16_t dst = rom16(rom32(pair) + (uint32_t)c * 2u);
            fprintf(stderr, "rom f%ld: id %d frame %d cell %u vram 0x%X (col %u row %u) layer %d\n", (long)st->frame, sl->id, sl->frame, c, dst, bg ? (dst % 0x40) / 2 : (dst % 0x80) / 4, bg ? dst / 0x40 : dst / 0x80, bg); }
    }
    if (getenv("WF_ANIMDBG")) fprintf(stderr, "rom f%ld: want %d\n", (long)st->frame, n_want);
}
