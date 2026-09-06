/* SCENE PLAY — the engine's own between-match screens, run from DATA.
 *
 * The stock profile keeps the ROM's screens (interlude.c: 0xAE20 talk,
 * 0xBDA6 belt count, 0xB608 title card; campaign.c: the ending).  Every
 * other profile runs THIS: a scene is a library folder scenes/<name>/
 * packed into build/scenes/<name>.pak =
 *   "page"    the tilemap page in the arena format (bg/fg planes, palettes,
 *             tiles, crowd regions) - bound as the scene-word-4 arena for
 *             the scene's duration;
 *   "script"  scene.json (wfengine-scene-2): camera, priority, text items,
 *             music, the ACTORS and the end gate.
 * An actor is a sprite object of one row with a cell SCRIPT ({cell, ticks}
 * steps, the ROM cadence: wait `ticks` frames after a load), a START
 * trigger ("begin" / "drawn" / another actor's step or cell), and a kind:
 *   script    a plain cell script (talk panels, the herald, the count art,
 *             the title card's centre piece; "loop_to" for the wrap)
 *   portrait  the title card's portrait: cell = the wrestler in a slot,
 *             x from the per-id table
 *   letter    one object of the title marquee: slides left at "speed",
 *             releases the next letter at the gate, dies off-screen; a
 *             name letter takes its cell from the slot's wrestler
 * Wrestler SLOTS ("p1", "p2") are filled from the seated objects, so the
 * same scene plays with anyone (skins included).
 * The types queue like the ROM's (title, belt, talk); the stock scenes
 * were exported from the ROM tables with the ROM's own timings, so a
 * non-stock profile plays frame-for-frame what the ROM plays until the
 * author changes the files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wf.h"
#include "engine.h"
#include "scene.h"
#include "json.h"
#include "pak.h"
#include "profile.h"

#define MAX_ACTORS 32
#define MAX_STEPS  64
#define MAX_QUEUE  4
#define SCENE_WORD 4

enum { K_SCRIPT, K_PORTRAIT, K_LETTER };
enum { ST_BEGIN, ST_DRAWN, ST_OFF, ST_ACTOR_STEP, ST_ACTOR_CELL };

typedef struct {
    char name[24]; int kind, row, x, y; unsigned cell; int on, done, cnt, step, started;
    int start_kind; char start_actor[24]; int start_val; int wipe_on_start, voice_on_start, tick_on_start;
    int nsteps; struct { unsigned cell; int ticks; int voice; } steps[MAX_STEPS]; int loop_to; int cell_rel_base;
    int slot;                                  /* portrait / name letter: wrestler slot index */
    int letter_i, name_col, y_name, x0, speed, gate;   /* letters */
} actor_t;

static struct {
    char queue[MAX_QUEUE][32]; int qn, qi, belt_idx;
    pak *pk; json_val *doc;
    actor_t a[MAX_ACTORS]; int na;
    unsigned prio; int voice, ramp, textset;
    int end_kind; char end_actor[24]; int end_val;   /* 0 step >=, 1 done */
    int prio_actor_fired; unsigned prio_at_value; int prio_at_step; char prio_actor[24];
    int fired_text_names;
    int ws[2];                                 /* the wrestler slots */
    unsigned name_cells[16][3]; int letter_w[64]; int nletter_w;
    int64_t t0;                     /* st->frame at the sub-scene's begin: "sounds" frames count from it */
    unsigned snd_fired;             /* bit per "sounds" entry (max 32) */
} sp;
double audio_play_wav(const char *path);

static int dbg(void) { return getenv("WF_DBGSEL") != NULL; }
void wf_arena_bind(unsigned scene, const uint8_t *sec, uint32_t len);   /* arena.c */
void wf_arena_release(unsigned scene);
void eng_aisle_bottom_bar(void);
void eng_blit(unsigned id);
void eng_banner_runs(uint32_t a2, unsigned idx, unsigned d7);
void eng_credit_force(void);

static actor_t *actor_by_name(const char *n) { for (int i = 0; i < sp.na; i++) if (!strcmp(sp.a[i].name, n)) return &sp.a[i]; return NULL; }

/* ---- the scene pak of a type, through the profile's "scenes" map ---- */
int wf_sceneplay_available(const char *type)
{
    char name[64], path[300];
    if (!wf_profile_scene(type, name, sizeof name)) return 0;
    snprintf(path, sizeof path, "build/scenes/%s.pak", name);
    return access(path, R_OK) == 0;
}

int wf_sceneplay_arm(const char *const *types, int n, int belt_idx)
{
    sp.qn = 0; sp.qi = 0; sp.belt_idx = belt_idx;
    for (int i = 0; i < n && sp.qn < MAX_QUEUE; i++) { if (!wf_sceneplay_available(types[i])) return 0; snprintf(sp.queue[sp.qn++], 32, "%s", types[i]); }
    return sp.qn;
}

/* ---- load one scene ---- */
static int load_actor(actor_t *A, const json_val *j)
{
    const json_val *st = json_get(j, "start"), *steps = json_get(j, "script");
    const char *kind = json_str(json_get(j, "kind"), "script");
    memset(A, 0, sizeof *A);
    snprintf(A->name, sizeof A->name, "%s", json_str(json_get(j, "name"), "?"));
    A->kind = !strcmp(kind, "portrait") ? K_PORTRAIT : !strcmp(kind, "letter") ? K_LETTER : K_SCRIPT;
    A->row = (int)json_int(json_get(j, "row"), 0); A->x = (int)json_int(json_get(j, "x"), 0); A->y = (int)json_int(json_get(j, "y"), 0);
    A->cell = (unsigned)json_int(json_get(j, "cell"), 0);
    A->cnt = (int)json_int(json_get(j, "wait"), 0);
    A->loop_to = (int)json_int(json_get(j, "loop_to"), -1);
    A->slot = (int)json_int(json_get(j, "slot"), 0);
    A->letter_i = (int)json_int(json_get(j, "letter"), -1); A->name_col = (int)json_int(json_get(j, "name_col"), -1);
    A->y_name = (int)json_int(json_get(j, "y_name"), A->y); A->x0 = (int)json_int(json_get(j, "x0"), A->x);
    A->speed = (int)json_int(json_get(j, "speed"), 2); A->gate = (int)json_int(json_get(j, "gate"), 0x140);
    A->wipe_on_start = json_int(json_get(j, "wipe_text_on_start"), 0) != 0;
    A->voice_on_start = (int)json_int(json_get(j, "sound_on_start"), 0);
    A->tick_on_start = json_int(json_get(j, "tick_on_start"), 0) != 0;
    A->cell_rel_base = json_get(j, "cell_base") != NULL;
    if (st && st->type == JSON_STRING) A->start_kind = !strcmp(json_str(st, ""), "drawn") ? ST_DRAWN : !strcmp(json_str(st, ""), "off") ? ST_OFF : ST_BEGIN;
    else if (st) { snprintf(A->start_actor, sizeof A->start_actor, "%s", json_str(json_get(st, "actor"), ""));
                   if (json_get(st, "cell")) { A->start_kind = ST_ACTOR_CELL; A->start_val = (int)json_int(json_get(st, "cell"), 0); }
                   else { A->start_kind = ST_ACTOR_STEP; A->start_val = (int)json_int(json_get(st, "step"), 0); } }
    else A->start_kind = ST_BEGIN;
    for (const json_val *s = steps ? steps->child : NULL; s && A->nsteps < MAX_STEPS; s = s->next) {
        A->steps[A->nsteps].cell = (unsigned)json_int(json_get(s, "cell"), 0);
        A->steps[A->nsteps].ticks = (int)json_int(json_get(s, "ticks"), 0);
        A->steps[A->nsteps].voice = json_int(json_get(s, "voice"), 0) != 0;
        A->nsteps++;
    }
    return 1;
}

static void start_actor(actor_t *A)
{
    A->on = 2; A->started = 1;
    if (A->wipe_on_start)                                       /* 0xB2E4: the 5 FG0 rows under the talk text */
        for (unsigned row = 0; row < 5; row++) memset(wf.fg0_videoram + 0x1908 + row * 0x100u, 0, 0x24u * 4u);
    if (A->voice_on_start) eng_sound((unsigned)A->voice_on_start);
}

static void unload(void)
{
    if (sp.doc) { json_free(sp.doc); sp.doc = NULL; }
    if (sp.pk) { pak_close(sp.pk); sp.pk = NULL; }
    wf_arena_release(SCENE_WORD);
}

static int begin_sub(eng_state *st)
{
    char name[64], path[300], err[128]; const uint8_t *sec; uint32_t len; const json_val *acts, *e;
    const char *type = sp.queue[sp.qi];
    unload();
    if (!wf_profile_scene(type, name, sizeof name)) return 0;
    snprintf(path, sizeof path, "build/scenes/%s.pak", name);
    sp.pk = pak_open(path); if (!sp.pk) return 0;
    sec = pak_section(sp.pk, "script", &len); if (!sec) { unload(); return 0; }
    sp.doc = json_parse((const char *)sec, len, err, sizeof err); if (!sp.doc) { fprintf(stderr, "sceneplay: %s: %s\n", path, err); unload(); return 0; }
    sec = pak_section(sp.pk, "page", &len); if (sec) wf_arena_bind(SCENE_WORD, sec, len);
    /* the wrestler slots: the seated pair */
    for (int i = 0; i < 2; i++) sp.ws[i] = (i < ENG_MAX_OBJS && st->obj[i].active) ? (int)(st->obj[i].wrestler & 0xFFu) : -1;
    if (getenv("WF_PICKS")) { int a, b, c, d; if (sscanf(getenv("WF_PICKS"), "%d,%d,%d,%d", &a, &b, &c, &d) == 4) { sp.ws[0] = a; sp.ws[1] = b; } }
    /* the ROM's common setup (sub_clear + the per-screen begin) */
    memset(wf.spriteram, 0, sizeof wf.spriteram); memset(wf.spriteram_buffered, 0, sizeof wf.spriteram_buffered);
    memset(wf.fg0_videoram, 0, sizeof wf.fg0_videoram);
    eng_credit_force();
    sp.voice = 0; sp.prio_actor_fired = 0; sp.fired_text_names = 0;
    sp.prio = (unsigned)json_int(json_get(sp.doc, "priority"), 0x7B);
    st->scene = SCENE_WORD;
    eng_sprite_scene_pals_begin();
    st->cam_x = (int)json_int(json_at(json_get(sp.doc, "camera"), 0), 0x280);
    st->cam_y = (int)json_int(json_at(json_get(sp.doc, "camera"), 1), 0x200);
    sp.textset = (int)json_int(json_get(sp.doc, "text_set"), 0);
    eng_scene_publish((unsigned)json_int(json_get(sp.doc, "arena_pal"), 4), (unsigned)sp.textset);
    sp.ramp = json_int(json_get(sp.doc, "text_ramp"), 0) != 0;
    /* text items */
    for (e = json_get(sp.doc, "text") ? json_get(sp.doc, "text")->child : NULL; e; e = e->next) {
        const json_val *cond = json_get(e, "if_belt_idx");
        if (cond && (int)json_int(cond, -1) != sp.belt_idx) continue;
        if (json_get(e, "blit")) eng_blit((unsigned)json_int(json_get(e, "blit"), 0));
        else if (json_get(e, "bottom_bar")) eng_aisle_bottom_bar();
        else if (json_get(e, "banner_run")) { const json_val *b = json_get(e, "banner_run"); eng_banner_runs((uint32_t)json_int(json_get(b, "at"), 0), (unsigned)json_int(json_get(b, "idx"), 0), (unsigned)json_int(json_get(b, "d7"), 0x10)); }
    }
    for (e = json_get(sp.doc, "music") ? json_get(sp.doc, "music")->child : NULL; e; e = e->next) eng_sound((unsigned)json_int(e, 0));
    sp.t0 = st->frame; sp.snd_fired = 0;
    /* tables the letters need */
    sp.nletter_w = 0;
    for (e = json_get(sp.doc, "letter_widths") ? json_get(sp.doc, "letter_widths")->child : NULL; e && sp.nletter_w < 64; e = e->next) sp.letter_w[sp.nletter_w++] = (int)json_int(e, 0);
    memset(sp.name_cells, 0, sizeof sp.name_cells);
    { int id = 0; for (e = json_get(sp.doc, "name_cells") ? json_get(sp.doc, "name_cells")->child : NULL; e && id < 16; e = e->next, id++) for (int c = 0; c < 3; c++) sp.name_cells[id][c] = (unsigned)json_int(json_at(e, c), 0); }
    /* actors */
    sp.na = 0; acts = json_get(sp.doc, "actors");
    for (e = acts ? acts->child : NULL; e && sp.na < MAX_ACTORS; e = e->next) {
        actor_t *A = &sp.a[sp.na];
        load_actor(A, e);
        if (A->cell_rel_base) {                                 /* the belt count art: cells relative to idx*step + base */
            const json_val *cb = json_get(e, "cell_base"); unsigned base = (unsigned)(sp.belt_idx * (int)json_int(json_get(cb, "per_idx"), 7) + (int)json_int(json_get(cb, "base"), 4));
            A->cell += base; for (int k = 0; k < A->nsteps; k++) A->steps[k].cell += base;
        }
        if (A->kind == K_PORTRAIT) {                            /* 0xB68A: cell = the slot's wrestler, x per id */
            int id = A->slot < 2 ? sp.ws[A->slot] : -1; const json_val *xs = json_get(e, "xs");
            if (id == 0xB) id = 0xA;                            /* 0xB6A0: Smash's card for both Demolition */
            if (id < 0 || id > 0xA) { A->cell = 0xFFFFu; A->on = 0; }
            else { A->cell = (unsigned)id; A->x = (int)json_int(json_at(json_at(xs, id), A->slot), A->x); A->on = 0; }
            sp.na++; continue;
        }
        if (A->kind == K_LETTER) {                              /* 0xB94C: parked hidden at x0 */
            A->on = 0; A->x = A->x0;
            if (A->name_col >= 0) { int id = A->slot < 2 ? sp.ws[A->slot] : -1; unsigned c = (id >= 0 && id < 16) ? sp.name_cells[id][A->name_col] : 0; A->y = A->y_name; A->cell = c ? c : 0xFFFFu; }
            sp.na++; continue;
        }
        if (A->start_kind == ST_BEGIN) start_actor(A); else if (A->start_kind == ST_DRAWN) A->on = 1; else A->on = (A->start_kind == ST_OFF) ? 0 : 1;
        sp.na++;
    }
    /* end gate, priority trigger */
    { const json_val *en = json_get(sp.doc, "end"); sp.end_kind = json_get(en, "done") ? 1 : 0; snprintf(sp.end_actor, sizeof sp.end_actor, "%s", json_str(json_get(en, "actor"), "")); sp.end_val = (int)json_int(json_get(en, "step"), 0); }
    { const json_val *pa = json_get(sp.doc, "priority_at"); sp.prio_actor[0] = 0; if (pa) { snprintf(sp.prio_actor, sizeof sp.prio_actor, "%s", json_str(json_get(pa, "actor"), "")); sp.prio_at_step = (int)json_int(json_get(pa, "step"), 1); sp.prio_at_value = (unsigned)json_int(json_get(pa, "value"), 0x78); } }
    if (dbg()) fprintf(stderr, "sceneplay: %s (%s) begin f%lld, %d actors, slots %d/%d\n", type, name, (long long)st->frame, sp.na, sp.ws[0], sp.ws[1]);
    return 1;
}

/* ---- one frame ---- */
static void tick_script(actor_t *A)
{
    if (A->on != 2 || A->nsteps == 0) return;
    if (--A->cnt >= 0) return;
    if (A->step >= A->nsteps) { A->on = 1; return; }
    if (A->steps[A->step].cell == 0xFFu && A->loop_to >= 0) A->step = A->loop_to;   /* 0xB920 wrap */
    if (A->steps[A->step].voice) {                              /* 0xB0F6: speech in order */
        const json_val *v = json_get(sp.doc, "voices");
        if (sp.voice < 4 && v) eng_sound((unsigned)json_int(json_at(v, sp.voice), 0));
        sp.voice++;
    }
    A->cell = A->steps[A->step].cell & 0x7Fu;
    A->cnt = A->steps[A->step].ticks;
    A->step++;
    if (A->steps[A->step - 1].ticks == 0xFE) A->on = 1;         /* 0xB13C: script over, stay drawn */
    if (A->step >= A->nsteps && A->loop_to < 0 && A->kind == K_SCRIPT && json_int(json_get(sp.doc, "hold_scripts"), 1)) { /* stays drawn on its last cell */ }
}

static int letter_off(unsigned cell)
{
    if (cell == 0xFFFFu) return -0x80;                          /* 0xBA82 */
    if (cell < 0xEu || (int)(cell - 0xEu) >= sp.nletter_w) return 0;
    return sp.letter_w[cell - 0xEu];
}

/* scene.json "sounds": [{"frame": N, "ref": "cmd:0x20" | "wav:name"}] - the
 * editor's Scenes > Sounds tab (user 2026-08-30): fired once when the
 * sub-scene reaches that frame; wav = sounds/<name>.wav from the library */
static void frame_sounds(eng_state *st)
{
    const json_val *L = json_get(sp.doc, "sounds"); int k = 0;
    for (const json_val *e = L ? L->child : NULL; e && k < 32; e = e->next, k++) {
        const char *ref = json_str(json_get(e, "ref"), "");
        if ((sp.snd_fired >> k) & 1u || st->frame - sp.t0 < json_int(json_get(e, "frame"), 0)) continue;
        sp.snd_fired |= 1u << k;
        if (!strncmp(ref, "cmd:", 4)) eng_sound((unsigned)strtoul(ref + 4, NULL, 0));
        else if (!strncmp(ref, "wav:", 4)) { char p[300]; snprintf(p, sizeof p, "sounds/%s.wav", ref + 4); audio_play_wav(p); }
        if (dbg()) fprintf(stderr, "sceneplay: f%lld sound %s\n", (long long)(st->frame - sp.t0), ref);
    }
}
static int update_sub(eng_state *st)
{
    frame_sounds(st);
    (void)st;
    for (int i = 0; i < sp.na; i++) if (sp.a[i].kind == K_SCRIPT) tick_script(&sp.a[i]);
    /* triggers: another actor's step / cell (one-shot) */
    for (int i = 0; i < sp.na; i++) {
        actor_t *A = &sp.a[i], *T;
        if (A->started || A->kind == K_LETTER) continue;
        if (A->start_kind != ST_ACTOR_STEP && A->start_kind != ST_ACTOR_CELL) continue;
        T = actor_by_name(A->start_actor); if (!T) continue;
        if ((A->start_kind == ST_ACTOR_STEP && T->step >= A->start_val) || (A->start_kind == ST_ACTOR_CELL && T->on == 2 && (int)T->cell == A->start_val && T->step > 0)) {
            if (A->kind == K_PORTRAIT) { A->started = 1; A->on = A->cell != 0xFFFFu; }
            else { start_actor(A); if (A->tick_on_start) tick_script(A); }   /* 0xBFCE: the count art steps in the frame it starts */
        }
    }
    if (sp.prio_actor[0] && !sp.prio_actor_fired) { actor_t *T = actor_by_name(sp.prio_actor); if (T && T->step >= sp.prio_at_step) { sp.prio = sp.prio_at_value; sp.prio_actor_fired = 1; } }
    /* the marquee: letter 0 starts on its trigger; each slides, releases the next, dies off-screen (0xBA48) */
    { actor_t *first = NULL, *last = NULL; int nl = 0;
      for (int i = 0; i < sp.na; i++) if (sp.a[i].kind == K_LETTER) { if (!first) first = &sp.a[i]; last = &sp.a[i]; nl++; }
      if (first && !first->started && first->start_kind == ST_ACTOR_STEP) { actor_t *T = actor_by_name(first->start_actor);
          if (T && T->step >= first->start_val) { first->started = 1; first->on = 1; first->x = first->x0;
              if (!sp.fired_text_names) { sp.fired_text_names = 1;     /* clone names in the standard big font */
                  for (int k = 0; k < 2; k++) if (sp.ws[k] >= 12) { const char *cn = eng_ws_clone_name(sp.ws[k]); if (cn && cn[0]) eng_fg0_bigtext(k ? 0x1Bu : 0x18u, 2, cn, 0); } } } }
      for (int i = 0; i < sp.na; i++) {
          actor_t *o = &sp.a[i], *nx = NULL;
          if (o->kind != K_LETTER || !o->on || o->done) continue;
          o->x -= o->speed;
          for (int j = i + 1; j < sp.na; j++) if (sp.a[j].kind == K_LETTER) { nx = &sp.a[j]; break; }
          if (nx && !nx->on && !nx->done && o->x + letter_off(o->cell) < o->gate) { nx->on = 1; nx->started = 1; nx->x = nx->x0; }
          if (o->x + letter_off(o->cell) < 0) { o->done = 1; o->on = 0; }
      }
      (void)last; (void)nl; }
    /* the end gate */
    { actor_t *E = actor_by_name(sp.end_actor);
      if (E && ((sp.end_kind == 1 && E->done) || (sp.end_kind == 0 && E->step >= sp.end_val))) return 1; }
    return 0;
}

static void pl_begin(eng_state *st)
{
    sp.qi = 0;
    if (!begin_sub(st)) { fprintf(stderr, "sceneplay: cannot start %s\n", sp.queue[0]); sp.qn = 0; }
}

static int pl_update(eng_state *st)
{
    if (!sp.qn || !sp.doc) { unload(); eng_sprite_scene_pals_end(); return ENG_SCENE_MATCH; }
    if (!update_sub(st)) return -1;
    if (dbg()) fprintf(stderr, "sceneplay: %s over f%lld\n", sp.queue[sp.qi], (long long)st->frame);
    if (++sp.qi < sp.qn && begin_sub(st)) return -1;
    sp.qn = 0; unload();
    eng_sprite_scene_pals_end();
    if (getenv("WF_SCENE_EXIT")) { fprintf(stderr, "sceneplay: scene over, exiting (WF_SCENE_EXIT)\n"); exit(0); }   /* --play-scene preview */
    return ENG_SCENE_MATCH;
}

static void pl_draw(const eng_state *st)
{
    unsigned slot = 0;
    (void)st;
    if (!sp.doc) return;
    m68k_write_memory_16(0x140010u, sp.prio);
    eng_sprite_scene_pals_rearm();
    if (sp.ramp) for (unsigned k = 0; k < 16; k++) m68k_write_memory_16(0x188280u + k * 2u, k << 8);   /* 0xBE92 */
    memset(wf.spriteram, 0, WF_SPRRAM_SIZE);
    for (int i = 0; i < sp.na; i++) {
        const actor_t *o = &sp.a[i];
        if (!o->on || o->done || (o->cell & 0x7FFFu) == 0x7FFFu || o->cell == 0xFFFFu) continue;
        eng_sprite_emit_pose((unsigned)o->row, o->cell, o->x, o->y, -1, &slot);
    }
    memcpy(wf.spriteram_buffered, wf.spriteram, WF_SPRRAM_SIZE);
}

static const eng_scene_ops pl_ops = { pl_begin, pl_update, pl_draw };
void wf_sceneplay_register(void) { eng_scene_register(ENG_SCENE_PLAY, &pl_ops); }
