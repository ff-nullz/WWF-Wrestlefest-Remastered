/* Launch profiles ("switches") — a named, launchable version of the game:
 * an ordered list of mod layers plus its own pak cache. No profile = STOCK:
 * zero mods, the plain build/ paks packed only from data/ — the original
 * game is always launchable and the editor never writes into data/ game
 * data (base is read-only; every save goes to a mod layer).
 *
 *   profiles/<name>.json  = { "name": ..., "description": ..., "mods": [ ... ] }
 *   wfengine --profile X  -> packs (if stale) and loads build/profiles/X/
 *   wfengine --profiles   -> list stock + profiles/
 *   wfengine --pack-profile X -> pack X's paks and exit
 *
 * Mod layers are directories under mods/ holding only the files they
 * change (tables/<group>/<name>.json, wrestlers/NN/<file>, gfx-edit/...,
 * keymap.json). LATER entries in the profile's list win. mods/order.txt
 * is retired: the profile IS the order (a leftover order.txt is ignored
 * with a warning). */
#ifndef WF_PROFILE_H
#define WF_PROFILE_H
#include <stddef.h>

int         wf_profile_set(const char *name);   /* load profiles/<name>.json; 0 = ok */
void        wf_profile_clear(void);             /* back to stock (editor switch) */
const char *wf_profile(void);                   /* active name, "" = stock */
int         wf_profile_mode(void);              /* 0 tag, 1 rumble, -1 none (game select) */
int         wf_profile_hidden(int id);          /* roster slot disabled in this profile */
int         wf_profile_nmods(void);
const char *wf_profile_mod(int i);              /* ordered; later wins */

/* Resolve a data-relative file ("gfx-edit/fg0.png", "wrestlers/03/stats.json",
 * "keymap.json") through the active mod layers, HIGHEST priority first.
 * Returns 1 + fills `out` with an existing override, 0 = use the base file. */
int wf_mod_resolve(const char *rel, char *out, size_t n);

/* Where the active profile's paks live ("" = the stock build/ paths).
 * A profile whose mods touch no wrestlers/gfx reuses the stock pak there. */
void wf_profile_pak_paths(char *base, size_t bn, char *wsdir, size_t wn, char *gfx, size_t gn);
void wf_profile_weapons_pak(char *out, size_t n);   /* build/weapons.pak, or the profile's own when its mods carry weapons/ */
int  wf_profile_arena_assign(int i, int *scene, char *name, size_t n);   /* the profile's "arenas" map, entry i */
int  wf_profile_scene(const char *type, char *name, size_t n);          /* the library scene a type plays; 0 = the ROM's (stock) */
void wf_profile_scene_force(const char *type, const char *name);        /* --play-scene: this run plays NAME for TYPE */

int wf_profile_pack(void);        /* pack the active profile's paks (0 = ok) */
int wf_profile_stale(void);       /* 1 = paks missing/older than sources */
int wf_profile_list(void);        /* print stock + profiles/*.json; 0 */
#endif
