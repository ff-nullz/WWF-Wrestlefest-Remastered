/* Front-end scene dispatcher. See scene.h. */
#include <stdio.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "scene.h"
#include "credit.h"

static const eng_scene_ops *ops[ENG_SCENE_COUNT];
static eng_scene_id cur = ENG_SCENE_MATCH;
static int need_begin;
static unsigned pub_arena, pub_textset = 1;   /* match defaults (render.c) */

void eng_gameselect_register(void);          /* gameselect.c */
void eng_charselect_register(void);          /* charselect.c */
void eng_attract_register(void);             /* attract.c */
void eng_aisle_register(void);               /* aisle.c */

void eng_scene_register(eng_scene_id id, const eng_scene_ops *o)
{
    if ((unsigned)id < ENG_SCENE_COUNT)
        ops[id] = o;
}

void eng_scene_init(void)
{
    eng_gameselect_register();
    eng_charselect_register();
    eng_attract_register();
    eng_aisle_register();
    eng_campaign_register();                 /* continue screen + ending */
    { void wf_sceneplay_register(void); wf_sceneplay_register(); }   /* ENG_SCENE_PLAY (sceneplay.c) */
}

void eng_scene_set(eng_scene_id id)
{
    if ((unsigned)id >= ENG_SCENE_COUNT)
        id = ENG_SCENE_MATCH;
    /* A scene nobody registered (e.g. the character select while it is
     * still being built) falls through to the match. */
    if (id != ENG_SCENE_MATCH && !ops[id]) {
        fprintf(stderr, "scene: %d not registered, starting the match\n", (int)id);
        id = ENG_SCENE_MATCH;
    }
    /* 0xBD2: the character select hands the match to 0x7B70 (the aisle
     * walk) before 0xC98/0xA654. Any front-end scene leaving for the
     * match goes through it; the aisle itself leaves for the match. */
    if (id == ENG_SCENE_MATCH && cur == ENG_SCENE_CHARSELECT
        && ops[ENG_SCENE_AISLE])
        id = ENG_SCENE_AISLE;          /* 0xBD2 (the FIRST match, from the
                                          character select) walks the 0x7B70
                                          aisle; every other entry is 0xAC0 ->
                                          0xB1C — interludes + init, NO aisle
                                          (user stock-checked 2026-08-23) */
    cur = id;
    need_begin = 1;
    if (id == ENG_SCENE_MATCH) {
        pub_arena = 0;
        pub_textset = 1;
    }
}

eng_scene_id eng_scene_get(void) { return cur; }
int eng_scene_active(void) { return cur != ENG_SCENE_MATCH; }

void eng_scene_vals(unsigned *arena, unsigned *textset)
{
    *arena = pub_arena;
    *textset = pub_textset;
}

/* Scenes set their own palette selectors through this (gameselect.c). */
void eng_scene_publish(unsigned arena, unsigned textset)
{
    pub_arena = arena;
    pub_textset = textset;
}

int eng_scene_update(eng_state *st)
{
    int next;

    if (cur == ENG_SCENE_MATCH)
        return 0;
    if (need_begin) {
        need_begin = 0;
        /* 0x1FC0 clear_screen: every ROM scene setup wipes the FG0 text
         * layer, so result plates / the match card never survive into the
         * next screen. */
        memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);
        eng_banner_clear();
        if (cur == ENG_SCENE_ATTRACT) { extern int eng_seated; eng_seated = 0; }   /* seats drop at attract */
        if (ops[cur]->begin)
            ops[cur]->begin(st);
    }
    next = ops[cur]->update(st);
    st->frame++;
    /* The CREDIT line is kept on every screen (0x1E92 is called from every
     * ROM loop that owns the bottom rows); scenes that wipe FG0 get it
     * back the same frame. */
    eng_credit_force(); eng_credit_line();
    if (next >= 0) {
        eng_scene_set((eng_scene_id)next);
        if (eng_scene_get() == ENG_SCENE_MATCH) {
            /* Fresh match state (core.c does the same between matches).
             * TODO EXACT: the rumble mode from the game select is not a
             * match type the engine plays yet; tag is always set up. */
            int64_t fr = st->frame;
            /* 0xAC0 dispatch on $1C007C: the attract demo (0xB1C) seeds
             * its own men (0xE02) and skips the intro; a real game takes
             * the campaign / character-select roster (0xBD2). */
            eng_init_picks(st, eng_demo_active() ? eng_demo_picks() : eng_camp_picks());
            if (eng_demo_active())
                eng_demo_seed(st);
            st->frame = fr;
        }
    }
    return 1;
}

void eng_scene_draw(const eng_state *st)
{
    if (cur != ENG_SCENE_MATCH && ops[cur] && ops[cur]->draw)
        ops[cur]->draw(st);
}
