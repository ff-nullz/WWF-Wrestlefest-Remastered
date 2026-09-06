/* Engine renderer: owns the `wf` state and drives the reused arcade
 * pipeline. No 68k. The `wf` global that video.c / scene_map.c /
 * pal_load.c read is DEFINED HERE — bus.c is not linked.
 *
 * Scene composition: scene_map.c reads the scene word and scroll out of
 * wf.work_ram at the ROM's own offsets, so the engine writes those few
 * cells before composing. That is a rendering-interface contract (the
 * addresses come from the ROM's memory map), not gameplay state — the
 * engine's real state lives in eng_state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include "scene.h"

wf_machine wf;

/* Work-RAM offsets scene_map.c/pal_load.c key on (docs/memory-catalog.csv). */
#define W_SCENE      0x007E
#define W_SCROLL_X   0x1804
#define W_SCROLL_Y   0x1806
#define W_ARENA      0x15F4   /* pal_load: arena palette index */
#define W_TEXTSET    0x15FC   /* pal_load: fg0 text palette set; 1 in a live match */

static int rom_load(const char *rom_dir)
{
    char path[512];
    FILE *fe, *fo;
    static uint8_t even[WF_ROM_SIZE / 2], odd[WF_ROM_SIZE / 2];

    snprintf(path, sizeof path, "%s/31e14-0.ic18", rom_dir);
    fe = fopen(path, "rb");
    snprintf(path, sizeof path, "%s/31e13-0.ic19", rom_dir);
    fo = fopen(path, "rb");
    if (!fe || !fo || fread(even, 1, sizeof even, fe) != sizeof even
                   || fread(odd, 1, sizeof odd, fo) != sizeof odd) {
        fprintf(stderr, "engine: cannot load program ROM from %s\n", rom_dir);
        if (fe) fclose(fe);
        if (fo) fclose(fo);
        return -1;
    }
    fclose(fe);
    fclose(fo);
    for (unsigned i = 0; i < WF_ROM_SIZE / 2; i++) {
        wf.rom[i * 2] = even[i];
        wf.rom[i * 2 + 1] = odd[i];
    }
    return 0;
}

/* need_rom: only the export/verify tools and the WF_DATA=rom reference
 * mode load the program ROM; the game runs from the paks and never opens
 * rom/ (docs/adr-001-data-formats.md rule 10). */
int eng_render_init(const char *rom_dir, int need_rom)
{
    memset(&wf, 0, sizeof wf);
    if (need_rom) {
        if (rom_load(rom_dir) != 0)
            return -1;
        tbl_transition_rom = 1;        /* reference mode: an address no table covers still reads wf.rom (and is reported) */
    }
    if (wf_video_init(rom_dir) != 0) {
        fprintf(stderr, "engine: wf_video_init failed\n");
        return -1;
    }
    return eng_sprite_init(rom_dir);
}

static void publish(const eng_state *st)
{
    wf.work_ram[W_SCENE] = 0;
    wf.work_ram[W_SCENE + 1] = (uint8_t)st->scene;
    wf.work_ram[W_SCROLL_X] = (uint8_t)(st->cam_x >> 8);
    wf.work_ram[W_SCROLL_X + 1] = (uint8_t)st->cam_x;
    wf.work_ram[W_SCROLL_Y] = (uint8_t)(st->cam_y >> 8);
    wf.work_ram[W_SCROLL_Y + 1] = (uint8_t)st->cam_y;
    {
        unsigned arena, textset;       /* match: 0 / 1; scenes publish theirs */
        eng_scene_vals(&arena, &textset);
        if (eng_banner_active()) textset = 0;   /* 0x7BC8 set 0 until 0xC6C after the intro */
        if (!eng_scene_active())
            arena = (unsigned)st->scene;   /* 0xFB6A/0xFC98 (ring-out) and 0xC98's compose:
                                              $1C15F4 = $1C15F8 = $1C007E — the match arena
                                              palette follows the scene word (stages play
                                              scenes 5/0/1 per 0xD12) */
        wf.work_ram[W_ARENA + 1] = (uint8_t)arena;
        wf.work_ram[W_TEXTSET + 1] = (uint8_t)textset;
    }
    wf.frame = (long)st->frame;
}

static int scene_composed = -1;
static int cam_last_x = -1, cam_last_y = -1, bgcam_last = -2, textset_last = -1;
static int blank_last = -1;

/* A scene that scrolls the FG plane alone sets this to the BG plane's own
 * camera x ($1C17EA, 0x26A7A/0x26AB0 keep the planes apart); -1 = both
 * planes follow st->cam_x. The character select's page scroll 0x658C moves
 * $1C17E6 only, so the portraits slide under a fixed SELECT PLAYERS. */
int eng_bg_cam_x = -1;

/* attract.c trademark card (0x790): 0x1F6C clears the tilemaps and the
 * page never composes (no 0x26E66) — black background under the FG0
 * text.  1 = compose() blanks both planes instead of running the scene. */
int eng_scene_blank;

/* attract.c title page (0x81BA): the ROM composes the (0x280,0x400)
 * window ONCE and then writes the two planes' raw X scroll regs each
 * vblank — $100000 = $1C1CD4 (+2/frame), $100004 = $1C1CD6 (+1/frame)
 * (0x8244/0x830C/0x83AE): a horizontal PARALLAX pan wrapping in the
 * 512px ring; the Y regs keep the composed window.  freeze: 1 = compose
 * once then hold, 2 = held.  pan_x0/x1 = the raw plane X values (< 0 =
 * leave the reg alone). */
int eng_compose_freeze;
int eng_pan_x0 = -1, eng_pan_x1 = -1;

/* Compose both planes at the still camera, keep the plane $1C17EA
 * scrolls (reg 0x100004 — the 0x80000 VRAM plane once the scene priority
 * byte 0x26E9E[3] swaps the pairs, video.c), then compose again at
 * st->cam_x and put that plane back through the bus so the tilemap shadow
 * sees it. */
static void compose(const eng_state *st)
{
    if (eng_scene_blank) {                 /* 0x790: cleared tilemaps, no compose */
        for (unsigned i = 0; i < WF_FGRAM_SIZE; i += 2)
            m68k_write_memory_16(WF_FGRAM_BASE + i, 0);
        for (unsigned i = 0; i < WF_BGRAM_SIZE; i += 2)
            m68k_write_memory_16(WF_BGRAM_BASE + i, 0);
        wf_tilemap_shadow_adopt();
        return;
    }
    if (eng_bg_cam_x >= 0 && eng_bg_cam_x != st->cam_x) {
        static uint8_t bgsave[WF_BGRAM_SIZE];
        wf.work_ram[W_SCROLL_X] = (uint8_t)(eng_bg_cam_x >> 8);
        wf.work_ram[W_SCROLL_X + 1] = (uint8_t)eng_bg_cam_x;
        wf_scene_run();
        memcpy(bgsave, wf.bg_videoram, sizeof bgsave);
        wf.work_ram[W_SCROLL_X] = (uint8_t)(st->cam_x >> 8);
        wf.work_ram[W_SCROLL_X + 1] = (uint8_t)st->cam_x;
        wf_scene_run();
        for (unsigned i = 0; i < WF_BGRAM_SIZE; i += 2)
            m68k_write_memory_16(WF_BGRAM_BASE + i, ((unsigned)bgsave[i] << 8) | bgsave[i + 1]);
        wf.work_ram[0x17EA] = (uint8_t)(eng_bg_cam_x >> 8);      /* $1C17EA BG camera x */
        wf.work_ram[0x17EB] = (uint8_t)eng_bg_cam_x;
    } else {
        wf_scene_run();
    }
    wf_tilemap_shadow_adopt();         /* the renderer draws the shadow */
}

void eng_render_frame(const eng_state *st, uint32_t *pixels, int pitch)
{
    static int was_front = -1;
    int front_now = eng_scene_active();
    /* The zoom mod resizes the compose window, so the renderer OWNS the
     * pitch: the caller's buffer must hold 512x384 (the max view) and the
     * pitch argument is ignored. */
    (void)pitch;
    if (was_front > 0 && !front_now)
        eng_sprite_install_body_palettes();   /* 0x2AEA at match start: undo the
                                                 attract fade's rewrites of bank 0x182000 */
    was_front = front_now;
    publish(st);
    if (eng_compose_freeze == 2)
        ;                             /* 0x81BA: hold the composed block */
    else if (scene_composed != st->scene
        || st->cam_x != cam_last_x || st->cam_y != cam_last_y
        || eng_bg_cam_x != bgcam_last || eng_scene_blank != blank_last
        || eng_compose_freeze
        || (int)wf.work_ram[W_TEXTSET + 1] != textset_last) {
        if (eng_compose_freeze == 1) {           /* 0x1F6C first: black ring */
            for (unsigned i = 0; i < WF_FGRAM_SIZE; i += 2)
                m68k_write_memory_16(WF_FGRAM_BASE + i, 0);
            for (unsigned i = 0; i < WF_BGRAM_SIZE; i += 2)
                m68k_write_memory_16(WF_BGRAM_BASE + i, 0);
            eng_compose_freeze = 2;
        }
        compose(st);                  /* recompose follows the scroll window */
        eng_cs_scene_patch();         /* extended select: blank the baked DEMOLITION label (charselect.c) */
        wf_palette_load();            /* scene palettes, C form of 0x2A06 */
        scene_composed = st->scene;
        cam_last_x = st->cam_x;
        cam_last_y = st->cam_y;
        bgcam_last = eng_bg_cam_x;
        blank_last = eng_scene_blank;
        textset_last = (int)wf.work_ram[W_TEXTSET + 1];
    }
    if (wf_tilemap_shadow_active()) {
        /* Prime the renderer's tilemap shadow now, before this frame's
         * scenery stamps: the shadow only mirrors stores made while it is
         * valid, and it is otherwise composed at the video latch — after
         * the stamps — so a one-shot render would lose them. */
        static uint8_t fg_tmp[WF_FGRAM_SIZE], bg_tmp[WF_BGRAM_SIZE];
        wf_tilemap_shadow_latch(fg_tmp, bg_tmp);
    }
    wf_scene_write_scroll_regs();
    if (eng_pan_x0 >= 0)               /* 0x8244: $1C1CD4 -> $100000 raw */
        m68k_write_memory_16(0x100000, (uint16_t)eng_pan_x0);
    if (eng_pan_x1 >= 0)               /* 0x824E: $1C1CD6 -> $100004 raw */
        m68k_write_memory_16(0x100004, (uint16_t)eng_pan_x1);
    {   /* MOD camera zoom: only the live match view zooms — every front-end
         * page (and the attract demo with its overlays) keeps the stock
         * 320x240 layout. */
        int pct = eng_mod_rule(MODR_CAM_ZOOM);
        int in_match = !eng_scene_active() && !eng_demo_active();
        if (in_match && eng_mod_rule(MODR_CAM_FIXED)
            && (st->scene == 0 || st->scene == 1 || st->scene == 5 || st->scene == 7)) {
            /* the lock covers the IN-RING scenes only: the ring-out strips
             * run a separate parallax BG camera and their own pan ("similar
             * offset on the background", playtest) — those episodes keep the
             * stock camera and the view snaps back when the men return. */
            /* fixed view sized PER SCENE: the arena art only exists across
             * the stock pan range + the 320x240 view — a y-LOCKED scene
             * (the ring-out strips) has only 240 rows of art, and showing
             * more painted void ("the arena is corrupted", playtest). */
            int vw = 512, vh = 384;
            uint32_t len = 0;
            if (tbl_bytes(TBL(camera_scene_limits), &len) && len >= 8) {
                unsigned row = (st->scene >= 0 && (uint32_t)st->scene < len / 8u) ? (unsigned)st->scene : 0u;
                int sx2 = (int)tbl16(TBL(camera_scene_limits), row * 8u + 4u)
                        - (int)tbl16(TBL(camera_scene_limits), row * 8u) + 320;
                int sy2 = (int)tbl16(TBL(camera_scene_limits), row * 8u + 6u)
                        - (int)tbl16(TBL(camera_scene_limits), row * 8u + 2u) + 240;
                if (sx2 < vw) vw = sx2;
                if (sy2 < vh) vh = sy2;
            }
            wf_video_set_view(vw, vh);
        }
        else if (in_match && pct >= 63 && pct < 100)
            wf_video_set_view(((320 * 100 / pct) + 3) & ~3, ((240 * 100 / pct) + 3) & ~3);
        else
            wf_video_set_view(320, 240);
    }
    pitch = wf_view_w() * 4;
    wf_palette_latch();                /* before the scene draw: its palette
                                          writes must land after any reload */
    eng_banner_refresh();              /* 0x98BA card: FG0 cells + palette
                                          lines 6/7/E/F, after any reload */
    if (eng_scene_active()) {
        eng_scene_draw(st);            /* front-end: fg0 text, palette cycling */
    } else {
        eng_scenery_tick(st);          /* crowd/ramp animation stamps */
        eng_sprite_emit(st);
        eng_demo_overlay_emit(st);     /* attract demo match card (0x8CF8 slot $1C14CE) */
    }
    wf_video_latch();
    wf_video_draw(pixels, pitch);
}

int eng_render_shot(const eng_state *st, const char *path)
{
    static uint32_t pixels[512 * 384];   /* max view (zoom mod) */
    eng_render_frame(st, pixels, 0);
    if (getenv("WF_VRAMDUMP")) {       /* debug: raw FG/BG tilemaps */
        FILE *f = fopen(getenv("WF_VRAMDUMP"), "wb");
        if (f) { fwrite(wf.fg_videoram, 1, sizeof wf.fg_videoram, f);
                 fwrite(wf.bg_videoram, 1, sizeof wf.bg_videoram, f);
                 fwrite(wf.palette, 1, sizeof wf.palette, f); fclose(f); }
    }
    if (strlen(path) > 4 && !strcmp(path + strlen(path) - 4, ".png")) {   /* the editor's previews */
        int W = wf_view_w(), H = wf_view_h(); uint8_t *rgba = malloc((size_t)W * H * 4); int rc;
        int wf_art_write_rgba_png(const char *, const uint8_t *, int, int);
        for (int i = 0; i < W * H; i++) { uint32_t c = pixels[i]; rgba[i * 4] = (uint8_t)(c >> 16); rgba[i * 4 + 1] = (uint8_t)(c >> 8); rgba[i * 4 + 2] = (uint8_t)c; rgba[i * 4 + 3] = 255; }
        rc = wf_art_write_rgba_png(path, rgba, W, H); free(rgba); return rc;
    }
    return wf_video_write_ppm(path);
}
