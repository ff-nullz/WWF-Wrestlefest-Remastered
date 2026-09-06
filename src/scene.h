/* Front-end scenes — the screens between coin-up and the live match.
 *
 * The ROM runs these as straight-line code off the reset path (0x8F2 jmp
 * 0x52BE: game select -> 0x5978 character select -> match setup). The
 * engine keeps eng_update() as the match step and lets a scene own the
 * frame instead while one is active:
 *
 *   eng_update():        if (eng_scene_update(st)) return;
 *   eng_render_frame():  sprites/scenery stand down while eng_scene_active()
 *
 * A scene is a table of three callbacks. Each scene file registers its own
 * ops (gameselect.c does it from eng_scene_init(); the character select
 * registers ENG_SCENE_CHARSELECT the same way). When a scene finishes it
 * calls eng_scene_set(next); if `next` has no ops registered yet the
 * dispatcher falls through to the match so the build still plays.
 *
 * Scene ids follow the ROM scene word $1C007E where one exists: the game
 * select and the character select both run on scene-word 3 (same tilemap,
 * different scroll), so the ids here are engine-side and the scene word is
 * published per scene through eng_scene_vals().
 */
#ifndef ENG_SCENE_H
#define ENG_SCENE_H

#include <stdint.h>
#include "engine.h"

typedef enum {
    ENG_SCENE_MATCH      = 0,   /* eng_update() owns the frame */
    ENG_SCENE_GAMESELECT = 1,   /* ROM 0x52BE..0x5474 (gameselect.c) */
    ENG_SCENE_CHARSELECT = 2,   /* ROM 0x5978.. (other module) */
    ENG_SCENE_ATTRACT    = 3,   /* ROM 0x6FC.. FBI splash placeholder (attract.c) */
    ENG_SCENE_AISLE      = 4,   /* ROM 0x7B70 entrance walk, scene word 4 (aisle.c) */
    ENG_SCENE_CONTINUE   = 5,   /* ROM 0x1256 + 0x1404 continue screen (campaign.c) */
    ENG_SCENE_CEREMONY   = 6,   /* ROM 0x1B4E championship ending (campaign.c) */
    ENG_SCENE_INTERLUDE  = 7,   /* ROM 0xB608 / 0xBDA6 / 0xAE20: the between-match
                                   title card, belt scene and LOD talk screen
                                   (interlude.c, armed by campaign.c) */
    ENG_SCENE_PLAY       = 8,   /* the engine's own data-driven screens (sceneplay.c):
                                   every non-stock profile's interludes */
    ENG_SCENE_COUNT
} eng_scene_id;

typedef struct {
    /* Called once when the scene becomes current (ROM entry code). */
    void (*begin)(eng_state *st);
    /* One frame of the scene. Return -1 to keep running, or the
     * eng_scene_id to switch to (ENG_SCENE_MATCH to start the match). */
    int  (*update)(eng_state *st);
    /* Optional: extra VRAM authoring after the tilemap compose/palette
     * load for this frame (fg0 text, palette cycling). May be NULL. */
    void (*draw)(const eng_state *st);
} eng_scene_ops;

void eng_scene_register(eng_scene_id id, const eng_scene_ops *ops);
void eng_scene_init(void);                 /* registers the built-in scenes */

void eng_scene_set(eng_scene_id id);       /* switch now; begin() runs on the next update */
eng_scene_id eng_scene_get(void);
int  eng_scene_active(void);               /* 1 while a non-match scene owns the frame */

/* Dispatcher hooks (core.c / render.c). */
int  eng_scene_update(eng_state *st);      /* 1 = frame consumed by the scene */
void eng_scene_draw(const eng_state *st);

/* Values the scene publishes for the reused renderer (scene_map.c /
 * pal_load.c read them out of wf.work_ram at the ROM offsets):
 * $1C15F4 arena palette index and $1C15FC text palette set. */
void eng_scene_vals(unsigned *arena, unsigned *textset);
void eng_scene_publish(unsigned arena, unsigned textset);   /* scenes set theirs */

/* ---- game select results (gameselect.c) ----
 * $1C0161 bit0: 1 = Royal Rumble (LEFT on the select screen), 0 = Tag. */
int  eng_gs_rumble(void);
/* The ROM hands the character select a scene-3 window scrolled to
 * (0x140, 0x300) for tag or (0x140, 0x500) for rumble (0x58EE / 0x5934).
 * The game select leaves st->cam_x/cam_y at that pair on exit. */

/* ---- character select results (charselect.c) ----
 * The roster table $1C0598 after 0x5DAC + the CPU fill 0x10400:
 * {P1, partner, CPU1, CPU2} wrestler ids. NULL until the screen has run. */
const int *eng_cs_picks(void);
const int *eng_cs_ports(void);     /* port per roster slot, -1 = CPU (NULL: select never ran) */
void eng_cs_scene_patch(void);     /* render.c after a recompose: ext-select label blank */

/* ---- shared FG0 text helpers (gameselect.c) ----
 * ROM 0x26122: draw a BCD number by record id (table 0x262A0). `value`
 * replaces the record's RAM pointer read; bit15 of id erases. */
void eng_num_draw(unsigned id, unsigned bcd_value);
/* ROM 0x1E92 body: the CREDIT line. `credits` is the $1C004E word
 * (coins << 8 | credits). The change/force gate lives in credit.c. */
void eng_credit_draw(unsigned credits);

/* attract demo: 0xB1C jsr 0xAE20 D0=1/2 — the LOD taunt screens before
 * the ranking pages; the finished screen returns ENG_SCENE_ATTRACT. */
int  eng_interlude_arm_talk(int mode);
void eng_interlude_register(void);
/* aisle.c: the 0xB29E bottom bar + WWF logo run (shared with 0xAF5E). */
void eng_aisle_bottom_bar(void);

#endif
