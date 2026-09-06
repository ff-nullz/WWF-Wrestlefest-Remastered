/* SCENE EXPORT (user 2026-08-29: "scenes ... exportable/importable as
 * frames, with addresses"): the non-match screens as files.
 *
 *   wfengine --export-scene NAME DIR      NAME = walkout | talk | title |
 *                                          belt | ending (walk-off + credits)
 *
 * main.c drives the engine headless into the scene (the walk-in pick
 * script, the WF_INTERLUDE / WF_CEREMONY pokes), then this writes:
 *   preview.png        the rendered frame
 *   tilemap/           the scene word's two planes in the ARENA format
 *                      (the layers format: crowd.png, ropes.png, arena.json) - the walkout
 *                      tunnel is scene 4, the front end scene 3
 *   sprites/           every pose of every sprite row the scene draws,
 *                      row_XX_pose_NNN.png (RGBA, the latched palette),
 *                      decoded by the ROM stream decoder (the wrestler
 *                      export's path)
 *   scene.json         rows with their ROM stream/meta addresses, the
 *                      frame list, and the placement tables the scene
 *                      reads (name + ROM address, already JSON under
 *                      data/tables)
 * Import is not part of this step: the tilemap can go back through the
 * arena library, sprite art through the clone-art arena later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include "stream_decode.h"
#include "pak.h"
#include <unistd.h>

int wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H);
const uint8_t *wf_video_tile_pens(unsigned t);
const uint8_t *wf_video_latch_spriteram(void);
uint32_t wf_video_palette_rgb(unsigned index);
void eng_render_frame(const eng_state *st, uint32_t *pixels, int pitch);
int tool_export_arena(const eng_state *st, const char *dir);
void tool_export_arena_crop_screen(int on);
int wf_view_w(void); int wf_view_h(void);

#define TAB_STREAM 0x38FB8u
#define TAB_META   0x38F14u

typedef struct { const char *name; int scene_word; const unsigned char rows[6]; int nrows; const char *tables; const char *note; } scene_spec;
static const scene_spec specs[] = {
    { "walkout",  4, { 0 }, 0,            "aisle_walk_pos, banner_*",                  "the entrance tunnel (scene word 4) + the VS banner; the walkers are the wrestlers' own rows" },
    { "talk",     4, { 0x2C, 0x4E }, 2,   "interlude_talk_panels",                     "the Legion of Doom talk screen: three row-0x2C panels, row-0x4E centre piece" },
    { "title",    4, { 0x4D, 0x4E }, 2,   "interlude_title_cards, interlude_title_cells", "the title-win card: portraits (row 0x4D) + the 17-letter marquee (row 0x4E)" },
    { "belt",     4, { 0x4F }, 1,         "interlude_belt_*",                          "the matches-until-the-title count: the herald + count art (row 0x4F)" },
    { "ending",   4, { 0x4D, 0x50 }, 2,   "ending_cards, ending_walkoff_pos",         "the championship ending: the champion's walk-off poses (row 0x4D) with the credit pages (row 0x50) rolling over the same screen - the pages are kept as they are" },
};

static uint32_t r32(uint32_t a) { return ((uint32_t)tbl_ra8(a) << 24) | ((uint32_t)tbl_ra8(a + 1) << 16) | ((uint32_t)tbl_ra8(a + 2) << 8) | tbl_ra8(a + 3); }
static uint16_t r16(uint32_t a) { return (uint16_t)((tbl_ra8(a) << 8) | tbl_ra8(a + 1)); }

/* the wrestler export's decode_pose, through the DATA LAYER (the game never
 * opens rom/: wf.rom[] is empty in pak mode) */
static int decode_pose(unsigned row, unsigned pose, WfThinkerSpr *spr)
{
    unsigned off_tab = r16(TAB_META + row * 2u), pose_off;
    uint32_t base;
    if (!off_tab) return -1;
    pose_off = r16(TAB_META + off_tab + pose * 2u);
    if (pose_off == 0xFFFEu || pose_off == 0xFFFFu) return -1;
    base = r32(TAB_STREAM + row * 4u);
    if (!base) return -1;
    wf_thinker_set_partner_row(-1);
    return wf_thinker_decode_obj(base, pose_off, 0, 0, 0, (uint16_t)row, (uint16_t)pose, spr, WF_THINKER_MAX_SPR);
}

/* The stream records carry no palette: the scene code hands each object its
 * bank at runtime (0x2AEA: a palette ID -> a sprite bank loaded from the
 * ROM body-palette table 0x2F22 + id*32).  The exported frame is coloured
 * with the LIVE bank when one of its tiles is on screen (the latched sprite
 * list); otherwise straight from the ROM table by the stream record's own
 * palette id - never a guess from another pose. */
static int bank_of_tiles(const WfThinkerSpr *spr, int n, int fallback)
{
    const uint8_t *ram = wf_video_latch_spriteram();
    for (int s = 0; s < WF_SPRRAM_SIZE; s += 16) {
        unsigned w1 = ((unsigned)ram[s + 2] << 8) | ram[s + 3], number, bank;
        if (!(w1 & 1)) continue;
        number = (unsigned)ram[s + 5] | ((unsigned)ram[s + 7] << 8);
        if (ram[s + 12] == 0xE7 && ram[s + 14] == 0x5C) number |= ((unsigned)ram[s + 13] & 0x0Fu) << 16;
        bank = ram[s + 9] & 0x0Fu;
        for (int i = 0; i < n; i++) if (spr[i].tile == number) return (int)bank;
    }
    (void)fallback; return -1;
}
#define ROM_BODY_PAL 0x2F22u
static uint32_t body_rgb(unsigned id, unsigned pen)
{
    unsigned a = ROM_BODY_PAL + id * 32u + pen * 2u, w = ((unsigned)tbl_ra8(a) << 8) | tbl_ra8(a + 1);
    return 0xff000000u | (((w & 0x0f) * 17u) << 16) | ((((w >> 4) & 0x0f) * 17u) << 8) | (((w >> 8) & 0x0f) * 17u);
}

/* one pose -> a tight RGBA PNG; returns 0 + size, -1 = no pose */
static int last_bank = -1;
static int frame_png(unsigned row, unsigned pose, const char *path, int *ow, int *oh, int *obank)
{
    static WfThinkerSpr spr[WF_THINKER_MAX_SPR];
    static uint8_t pens[512 * 512], pal[512 * 512];      /* 512 canvas: the credit pages run past 256 rows */
    int n = decode_pose(row, pose, spr), x0 = 512, x1 = -1, y0 = 512, y1 = -1, bank;
    if (n <= 0) return -1;
    bank = bank_of_tiles(spr, n, 0);
    (void)last_bank; *obank = bank;
    memset(pens, 0, sizeof pens);
    for (int i = 0; i < n; i++) {                 /* the strip's transform (editor render_pose) */
        int x = spr[i].x + 128, y = spr[i].y + 0x80, xpos = x & 0x1FF, ypos;
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
                int cx = xpos + px + 128, cy = dy + py + 192;
                if (pen && cx >= 0 && cx < 512 && cy >= 0 && cy < 512) {
                    pens[cy * 512 + cx] = pen; pal[cy * 512 + cx] = (uint8_t)spr[i].pal;
                    if (cx < x0) x0 = cx; if (cx > x1) x1 = cx; if (cy < y0) y0 = cy; if (cy > y1) y1 = cy;
                }
            }
        }
    }
    if (x1 < 0) return -1;
    {
        int W = x1 - x0 + 1, H = y1 - y0 + 1;
        uint8_t *rgba = calloc((size_t)W * H, 4);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            uint8_t pen = pens[(y0 + y) * 512 + x0 + x];
            if (!pen) continue;
            uint32_t c = bank >= 0 ? wf_video_palette_rgb(0x400u + (unsigned)bank * 16u + pen) : body_rgb(pal[(y0 + y) * 512 + x0 + x], pen);
            uint8_t *p = rgba + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c; p[3] = 255;
        }
        { int rc = wf_art_write_rgba_png(path, rgba, W, H); free(rgba); *ow = W; *oh = H; return rc ? -1 : 0; }
    }
}

/* TRIGGER SETS: which poses a row shows when (the scene code's cell
 * choices, interlude.c / campaign.c) - one labelled set per trigger so
 * the editor lists them as separate rows */
static void set_range(FILE *f, int *n, const char *label, unsigned a, unsigned b)
{
    fprintf(f, "%s\n        { \"label\": \"%s\", \"poses\": [", (*n)++ ? "," : "", label);
    for (unsigned p = a; p <= b; p++) fprintf(f, "%s%u", p > a ? ", " : "", p);
    fprintf(f, "] }");
}
static void write_sets(FILE *f, const char *scene, unsigned row)
{
    int n = 0; char lab[96];
    if (!strcmp(scene, "belt") && row == 0x4F) {                 /* 0xBDA6: herald cells 0-3, count art idx*7+4 */
        set_range(f, &n, "herald (the announcer figure, cells 0-3, script 3,1,2,1,0)", 0, 3);
        for (unsigned i = 0; i < 3; i++) { snprintf(lab, sizeof lab, "count art, defence %u (cells %u-%u%s)", i + 1, i * 7 + 4, i * 7 + 10, i == 2 ? " - the last before the title match" : ""); set_range(f, &n, lab, i * 7 + 4, i * 7 + 10); }
    } else if (!strcmp(scene, "title") && row == 0x4D) {         /* 0xB6B6: cell = wrestler id */
        for (unsigned i = 0; i <= 0xA; i++) { snprintf(lab, sizeof lab, "wrestler %u%s", i, i == 0xA ? " (both Demolition)" : ""); set_range(f, &n, lab, i, i); }
    } else if (!strcmp(scene, "title") && row == 0x4E) {         /* 0xB730: cell 0 centre piece; letters follow */
        set_range(f, &n, "the announcer (cell 0) and the marquee word blocks: fixed words 0xE-0x13, 0x1F-0x23, three name blocks per wrestler (interlude_name_cells)", 0, 0x3FF);
    } else if (!strcmp(scene, "talk") && row == 0x2C) {          /* 0xAEF2: left panel from 0x16, right from 3, centre from panel_setup */
        set_range(f, &n, "right panel (from cell 3)", 3, 0x15);
        set_range(f, &n, "left panel (from cell 0x16)", 0x16, 0x3FF);
        set_range(f, &n, "centre panel / others (cells 0-2)", 0, 2);
    } else if (!strcmp(scene, "ending") && row == 0x4D) {        /* ending_cards 0x1DE8: {x, y, cell} per card i */
        for (unsigned i = 0; i < 9; i++) { unsigned c = r16(0x1DE8u + i * 6u + 4u); snprintf(lab, sizeof lab, "card %u", i); set_range(f, &n, lab, c, c); }
    } else if (!strcmp(scene, "ending") && row == 0x50) {
        set_range(f, &n, "credit pages, in order (kept)", 0, 0x3FF);
    }
}

/* ---- the SCRIPT (wfengine-scene-2): what sceneplay.c runs for a non-stock
 * profile - the ROM screen's objects, cell scripts, triggers, timings, text
 * and sounds, read from the ROM tables (interlude.c documents each) ---- */
#define T_PANEL_SETUP 0xB0C2u
#define T_VOICE_CMDS  0xB14Au
#define T_TALK_PTRS   0xB19Cu
#define T_TITLE_CARDS 0xB814u
#define T_TITLE_BIG   0xB936u
#define T_NAME_CELLS  0xBA24u
#define T_LETTER_W    0xBAACu
#define T_BELT_OBJS   0xBF58u
#define T_BELT_SMALL  0xBFFEu

static void script_steps(FILE *f, uint32_t sa)          /* {cell, dur} pairs to dur 0xFE inclusive */
{
    for (int k = 0; k < 64; k++) {
        unsigned cell = tbl_ra8(sa + (uint32_t)k * 2u), dur = tbl_ra8(sa + (uint32_t)k * 2u + 1u);
        fprintf(f, "%s\n          { \"cell\": %u, \"ticks\": %u%s }", k ? "," : "", cell & 0x7Fu, dur, (cell & 0x80u) ? ", \"voice\": true" : "");
        if (dur == 0xFEu) break;
    }
}
static void write_script(FILE *f, const char *type)
{
    if (!strcmp(type, "talk")) {
        fprintf(f, "  \"camera\": [640, 512], \"priority\": 123, \"arena_pal\": 4, \"text_set\": 0,\n"
                   "  \"text\": [ { \"bottom_bar\": true }, { \"blit\": 39, \"what\": \"WRESTLEFEST TAG TEAM CHAMPION\" }, { \"blit\": 40, \"what\": \"THE LEGION OF DOOM\" } ],\n"
                   "  \"music\": [12544, 12560],\n  \"voices\": [");
        for (unsigned i = 0; i < 4; i++) fprintf(f, "%s%u", i ? ", " : "", r16(T_VOICE_CMDS + i * 2u));
        fprintf(f, "],\n  \"sounds_note\": \"music at begin; a script step with voice:true speaks the next voices[] entry\",\n  \"actors\": [");
        { static const char *nm[3] = { "left", "centre", "right" }; static const char *start[3] = { "{ \"actor\": \"centre\", \"step\": 46 }", "\"begin\"", "{ \"actor\": \"centre\", \"step\": 27 }" };
          unsigned cell[3] = { 0x16u, tbl_ra8(T_PANEL_SETUP + 3u), 3u };
          for (int k = 0; k < 3; k++) {
              fprintf(f, "%s\n    { \"name\": \"%s\", \"kind\": \"script\", \"row\": 44, \"x\": %u, \"y\": 119, \"cell\": %u, \"start\": %s%s,\n      \"script\": [", k ? "," : "", nm[k], tbl_ra8(T_PANEL_SETUP + (unsigned)k), cell[k], start[k], k == 2 ? ", \"wipe_text_on_start\": true" : "");
              script_steps(f, r32(T_TALK_PTRS + (unsigned)k * 4u));
              fprintf(f, "\n      ] }");
          } }
        fprintf(f, "\n  ],\n  \"end\": { \"actor\": \"left\", \"step\": 28 },\n");
    } else if (!strcmp(type, "belt")) {
        unsigned hx = tbl_ra8(T_BELT_OBJS), cx = tbl_ra8(T_BELT_OBJS + 1u), hy = tbl_ra8(T_BELT_OBJS + 2u), cy = tbl_ra8(T_BELT_OBJS + 3u);
        fprintf(f, "  \"camera\": [640, 512], \"priority\": 123, \"arena_pal\": 4, \"text_set\": 1, \"text_ramp\": true,\n"
                   "  \"text\": [ { \"banner_run\": { \"at\": 791872, \"idx\": 16, \"d7\": 16 }, \"what\": \"N more matches to the title match\" }, { \"banner_run\": { \"at\": 791928, \"idx\": 23, \"d7\": 16 }, \"if_belt_idx\": 2, \"what\": \"the last defence line\" } ],\n"
                   "  \"music\": [],\n  \"sounds_note\": \"the count art plays 12586 (voice) when it starts\",\n  \"actors\": [\n"
                   "    { \"name\": \"left\", \"kind\": \"script\", \"row\": 44, \"x\": 96, \"y\": 119, \"cell\": 22, \"start\": \"drawn\" },\n"
                   "    { \"name\": \"right\", \"kind\": \"script\", \"row\": 44, \"x\": 224, \"y\": 119, \"cell\": 3, \"start\": \"drawn\" },\n"
                   "    { \"name\": \"count\", \"kind\": \"script\", \"row\": 79, \"x\": %u, \"y\": %u, \"cell\": 0, \"cell_base\": { \"per_idx\": 7, \"base\": 4 }, \"start\": { \"actor\": \"herald\", \"cell\": 2 }, \"sound_on_start\": 12586, \"tick_on_start\": true,\n      \"script\": [", cx, cy);
        for (int k = 0; k < 16; k++) fprintf(f, "%s\n          { \"cell\": %d, \"ticks\": 7 }", k ? "," : "", k < 6 ? k + 1 : 6);
        fprintf(f, "\n      ] },\n    { \"name\": \"herald\", \"kind\": \"script\", \"row\": 79, \"x\": %u, \"y\": %u, \"cell\": 3, \"wait\": 64, \"start\": \"begin\",\n      \"script\": [", hx, hy);
        for (int k = 0; k < 5; k++) fprintf(f, "%s\n          { \"cell\": %u, \"ticks\": 9 }", k ? "," : "", tbl_ra8(T_BELT_SMALL + (unsigned)k) & 0xFu);
        fprintf(f, "\n      ] }\n  ],\n  \"end\": { \"actor\": \"count\", \"step\": 16 },\n");
    } else if (!strcmp(type, "title")) {
        fprintf(f, "  \"camera\": [320, 768], \"priority\": 123, \"priority_at\": { \"actor\": \"centre\", \"step\": 1, \"value\": 120 }, \"arena_pal\": 4, \"text_set\": 0,\n"
                   "  \"text\": [],\n  \"music\": [12544, 12551],\n  \"sounds_note\": \"music at begin\",\n  \"letter_widths\": [");
        for (unsigned i = 0; i < 23; i++) fprintf(f, "%s%d", i ? ", " : "", (int16_t)r16(T_LETTER_W + i * 2u));
        fprintf(f, "],\n  \"name_cells\": [");
        for (unsigned id = 0; id < 12; id++) fprintf(f, "%s[%u, %u, %u]", id ? ", " : "", tbl_ra8(T_NAME_CELLS + id * 3u), tbl_ra8(T_NAME_CELLS + id * 3u + 1u), tbl_ra8(T_NAME_CELLS + id * 3u + 2u));
        fprintf(f, "],\n  \"actors\": [");
        for (int n = 0; n < 2; n++) {
            fprintf(f, "%s\n    { \"name\": \"portrait%d\", \"kind\": \"portrait\", \"row\": 77, \"y\": 126, \"slot\": %d, \"start\": { \"actor\": \"centre\", \"step\": 1 }, \"xs\": [", n ? "," : "", n + 1, n);
            for (unsigned id = 0; id <= 0xAu; id++) fprintf(f, "%s[%u, %u]", id ? ", " : "", tbl_ra8(T_TITLE_CARDS + id * 3u), tbl_ra8(T_TITLE_CARDS + id * 3u + 1u));
            fprintf(f, "] }");
        }
        fprintf(f, ",\n    { \"name\": \"centre\", \"kind\": \"script\", \"row\": 78, \"x\": 240, \"y\": 110, \"cell\": 0, \"wait\": 64, \"loop_to\": 3, \"start\": \"begin\",\n      \"script\": [");
        for (unsigned k = 0; k < 22; k++) { unsigned c = tbl_ra8(T_TITLE_BIG + k); fprintf(f, "%s\n          { \"cell\": %u, \"ticks\": 8 }", k ? "," : "", c); if (c == 0xFFu) break; }
        fprintf(f, "\n      ] }");
        for (int i = 0; i < 17; i++) {
            fprintf(f, ",\n    { \"name\": \"letter%02d\", \"kind\": \"letter\", \"row\": 78, \"x0\": 447, \"y\": 12, \"speed\": 2, \"gate\": 320", i);
            if (i < 6) fprintf(f, ", \"cell\": %d", 0xE + i);
            else if (i < 9) fprintf(f, ", \"cell\": %d", 0x19 + i);
            else if (i == 0xC) fprintf(f, ", \"cell\": 34");
            else if (i == 0x10) fprintf(f, ", \"cell\": 35");
            else fprintf(f, ", \"slot\": %d, \"name_col\": %d, \"y_name\": 15", i < 0xD ? 0 : 1, i < 0xD ? i - 9 : i - 0xD);
            if (i == 0) fprintf(f, ", \"start\": { \"actor\": \"centre\", \"step\": 5 }");
            fprintf(f, " }");
        }
        fprintf(f, "\n  ],\n  \"end\": { \"actor\": \"letter16\", \"done\": true },\n");
    }
}

/* --pack-scene NAME: scenes/<name>/ -> build/scenes/<name>.pak ("page" = tilemap/ in the arena format, "script" = scene.json) */
int tool_pack_scene(const char *name)
{
    char dir[300], path[400]; pak_writer *w; FILE *f; long len; uint8_t *buf; int rc = 0;
    int tool_pack_view(pak_writer *w, const char *srcdir, const char *secname);
    snprintf(dir, sizeof dir, "scenes/%s", name);
    snprintf(path, sizeof path, "%s/scene.json", dir);
    f = fopen(path, "rb"); if (!f) { fprintf(stderr, "pack-scene: no %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len + 1); if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); return 1; } fclose(f);
    w = pak_writer_new();
    snprintf(path, sizeof path, "%s/tilemap", dir);
    if (tool_pack_view(w, path, "page")) rc = 1;
    if (pak_writer_add(w, "script", buf, (uint32_t)len)) rc = 1;
    mkdir("build/scenes", 0775);
    snprintf(path, sizeof path, "build/scenes/%s.pak", name);
    if (pak_writer_save(w, path)) rc = 1;
    free(buf);
    fprintf(stderr, "pack-scene: %s -> %s%s\n", dir, path, rc ? " (ERRORS)" : "");
    return rc;
}

/* --export-row ROW DIR: every pose of one sprite row as PNGs (art study) */
/* SIDE-ROPE ART (2026-08-30): the four rope objects (ringhw.c) draw row 14
 * poses 0..10 at a fixed world point.  For the arena package every pose is
 * written on ONE canvas (the union of their extents) so a painter edits
 * them in place; ropes.json records the object ORIGIN inside that canvas
 * (canvas px = origin.x + record x, canvas py = origin.y - record y for a
 * cell's top edge) so the packer turns the pixels back into records
 * (tools/pack_ropes.c). frame_png's 512 canvas puts the origin at (256, 304). */
int tool_export_rope_frames(const char *dir)
{
    static WfThinkerSpr spr[WF_THINKER_MAX_SPR];
    static uint8_t pens[11][512 * 512];
    int x0 = 512, x1 = -1, y0 = 512, y1 = -1, n = 0, bank = -1;
    char fp[700]; FILE *f;
    for (unsigned pose = 0; pose < 11; pose++) {
        int cnt = decode_pose(14u, pose, spr);
        memset(pens[pose], 0, sizeof pens[pose]);
        if (cnt <= 0) continue;
        if (bank < 0) bank = bank_of_tiles(spr, cnt, 0);
        for (int i = 0; i < cnt; i++) {
            int x = spr[i].x + 128, y = spr[i].y + 0x80, xpos = x & 0x1FF, ypos;
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
                    int cx = xpos + px + 128, cy = dy + py + 192;
                    if (pen && cx >= 0 && cx < 512 && cy >= 0 && cy < 512) {
                        pens[pose][cy * 512 + cx] = pen;
                        if (cx < x0) x0 = cx; if (cx > x1) x1 = cx; if (cy < y0) y0 = cy; if (cy > y1) y1 = cy;
                    }
                }
            }
        }
        n++;
    }
    if (x1 < 0 || bank < 0) { fprintf(stderr, "export-ropes: row 14 has no poses on screen (the ring scene must be showing)\n"); return 1; }
    x0 &= ~15; y0 &= ~15;                                          /* tile-aligned canvas */
    mkdir(dir, 0775);
    {   int W = x1 - x0 + 1, H = y1 - y0 + 1;
        W = (W + 15) & ~15; H = (H + 15) & ~15;
        for (unsigned pose = 0; pose < 11; pose++) {
            uint8_t *rgba = calloc((size_t)W * H, 4);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                uint8_t pen = (y0 + y < 512 && x0 + x < 512) ? pens[pose][(y0 + y) * 512 + x0 + x] : 0;
                uint32_t c; uint8_t *p = rgba + ((size_t)y * W + x) * 4;
                if (!pen) continue;
                c = wf_video_palette_rgb(0x400u + (unsigned)bank * 16u + pen);
                p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c; p[3] = 255;
            }
            snprintf(fp, sizeof fp, "%s/side_%02u.png", dir, pose);
            wf_art_write_rgba_png(fp, rgba, W, H); free(rgba);
        }
        snprintf(fp, sizeof fp, "%s/ropes.json", dir);
        f = fopen(fp, "w");
        if (f) {
            fprintf(f, "{\n  \"format\": \"wfengine-ropes-1\",\n  \"note\": \"the side ropes (stream row 14): poses 0..10 on one canvas; idle back 2 / front 10, the shake steps come from the ROM tables (ringhw.c). origin = the rope object's point inside the canvas\",\n");
            fprintf(f, "  \"canvas\": [%d, %d],\n  \"origin\": [%d, %d],\n  \"frames\": [", W, H, 256 - x0, 304 - y0);
            for (unsigned pose = 0; pose < 11; pose++) fprintf(f, "%s\"side_%02u.png\"", pose ? ", " : "", pose);
            fprintf(f, "]\n}\n"); fclose(f);
        }
        fprintf(stderr, "export-ropes: %d poses on a %dx%d canvas (origin %d,%d, bank %d) -> %s\n", n, W, H, 256 - x0, 304 - y0, bank, dir);
    }
    return 0;
}

int tool_export_row(unsigned row, const char *dir)
{
    unsigned off_tab = r16(TAB_META + row * 2u), n = 0;
    char fp[700]; int w, h, bank;
    if (!off_tab) { fprintf(stderr, "export-row: row 0x%02X has no poses\n", row); return 1; }
    mkdir(dir, 0775); last_bank = -1;
    for (unsigned pose = 0; pose < 0x400; pose++) {
        if (r16(TAB_META + off_tab + pose * 2u) == 0xFFFFu) break;
        snprintf(fp, sizeof fp, "%s/pose_%03u.png", dir, pose);
        if (frame_png(row, pose, fp, &w, &h, &bank) == 0) n++;
    }
    fprintf(stderr, "export-row: row 0x%02X -> %u poses in %s\n", row, n, dir);
    return 0;
}

int tool_export_scene(const eng_state *st, const char *name, const char *dir)
{
    const scene_spec *sp = NULL;
    static uint32_t pixels[512 * 384];
    char path[600]; FILE *f; int W, H;
    for (unsigned i = 0; i < sizeof specs / sizeof specs[0]; i++) if (!strcmp(specs[i].name, name)) sp = &specs[i];
    if (!sp) { fprintf(stderr, "export-scene: NAME must be walkout | talk | title | belt | ending\n"); return 1; }
    mkdir(dir, 0775);
    /* the frame as the player sees it */
    eng_render_frame(st, pixels, 0);
    W = wf_view_w(); H = wf_view_h();
    {   uint8_t *rgba = malloc((size_t)W * H * 4);
        for (int i = 0; i < W * H; i++) { uint32_t c = pixels[i]; rgba[i * 4] = (uint8_t)(c >> 16); rgba[i * 4 + 1] = (uint8_t)(c >> 8); rgba[i * 4 + 2] = (uint8_t)c; rgba[i * 4 + 3] = 255; }
        snprintf(path, sizeof path, "%s/preview.png", dir); wf_art_write_rgba_png(path, rgba, W, H); free(rgba); }
    /* the tilemap planes of the scene word on screen (the arena format) */
    snprintf(path, sizeof path, "%s/tilemap", dir);
    tool_export_arena_crop_screen(1);                     /* this scene's page only, not the whole scene-word sheet */
    if (tool_export_arena(st, path)) fprintf(stderr, "export-scene: no tilemap export for scene word %d\n", st->scene);
    /* sprite rows: sprites/row_XX/pose_NNN.png, one folder per row */
    snprintf(path, sizeof path, "%s/sprites", dir); mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/scene.json", dir);
    f = fopen(path, "w"); if (!f) return 1;
    fprintf(f, "{\n  \"format\": \"wfengine-scene-2\",\n  \"scene\": \"%s\",\n  \"type\": \"%s\",\n  \"note\": \"%s\",\n  \"scene_word\": %d,\n  \"tilemap\": \"tilemap/\",\n  \"tables\": \"%s (data/tables/base/*.json carry the ROM addresses)\",\n",
            sp->name, sp->name, sp->note, st->scene, sp->tables);
    write_script(f, sp->name);
    fprintf(f, "  \"rows\": [");
    for (int r = 0; r < sp->nrows; r++) {
        unsigned row = sp->rows[r], off_tab = r16(TAB_META + row * 2u), nf = 0;
        fprintf(f, "%s\n    { \"row\": %u, \"stream\": \"0x%X\", \"meta\": \"0x%X\", \"frames\": [", r ? "," : "", row, r32(TAB_STREAM + row * 4u), TAB_META + off_tab);
        last_bank = -1;
        snprintf(path, sizeof path, "%s/sprites/row_%02X", dir, row); mkdir(path, 0775);
        for (unsigned pose = 0; pose < 0x400; pose++) {
            char fp[700]; int w, h, bank;
            if (r16(TAB_META + off_tab + pose * 2u) == 0xFFFFu) break;     /* the table terminator */
            snprintf(fp, sizeof fp, "%s/sprites/row_%02X/pose_%03u.png", dir, row, pose);
            if (frame_png(row, pose, fp, &w, &h, &bank)) continue;
            fprintf(f, "%s\n      { \"pose\": %u, \"png\": \"sprites/row_%02X/pose_%03u.png\", \"w\": %d, \"h\": %d, \"live_bank\": %d }", nf ? "," : "", pose, row, pose, w, h, bank);
            nf++;
        }
        fprintf(f, "\n    ],\n      \"sets\": [");
        write_sets(f, sp->name, row);
        fprintf(f, "\n    ] }");
        fprintf(stderr, "export-scene: row 0x%02X -> %u frames\n", row, nf);
    }
    fprintf(f, "\n  ]\n}\n"); fclose(f);
    fprintf(stderr, "export-scene: %s -> %s (preview.png, tilemap/, sprites/, scene.json)\n", name, dir);
    return 0;
}
