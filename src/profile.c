/* Launch profiles — see profile.h. The active profile is process-global:
 * every mod-layer lookup (tables packer, wrestler packer, gfx loader,
 * keymap) goes through wf_mod_resolve()/wf_profile_mod(), so STOCK (no
 * profile) is guaranteed mod-free. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "profile.h"
#include "json.h"

#define P_MAX_MODS 16

static char p_name[64];                    /* "" = stock */
static char p_mods[P_MAX_MODS][64];
static int  p_nmods;
static int  p_mode = -1;                   /* "mode": 0 tag, 1 rumble, -1 none (stock flow: game select) */
static struct { int scene; char name[64]; } p_arenas[8];   /* "arenas": {"<in-ring scene>": "<library arena>"} */
static int  p_narenas;
static struct { char type[24]; char name[64]; } p_scenes[8];  /* "scenes": {"<type>": "<library scene>"} */
static int  p_nscenes;
static unsigned char p_hidden[64];         /* "disabled": [slot ids] hidden from this profile's roster */
int wf_profile_mode(void)   { return p_name[0] ? p_mode : -1; }
int wf_profile_hidden(int id) { return id >= 0 && id < 64 && p_hidden[id]; }
/* ROSTER (user 2026-08-26): roster/ is a shared library of added wrestlers
 * (wrestlers/NN/*, select/NN.png) every non-stock profile includes as its
 * LOWEST layer; a profile hides entries with "disabled". Stock never sees it. */
static int roster_rel(const char *rel)
{
    return !strncmp(rel, "wrestlers/", 10) || !strncmp(rel, "select/", 7);
}
static int roster_hidden_rel(const char *rel)
{
    const char *d = strchr(rel, '/');
    return d && wf_profile_hidden(atoi(d + 1));
}

const char *wf_profile(void)      { return p_name; }
int         wf_profile_nmods(void){ return p_nmods; }
const char *wf_profile_mod(int i) { return (i >= 0 && i < p_nmods) ? p_mods[i] : NULL; }

int wf_profile_set(const char *name)
{
    char path[256], err[256];
    json_val *doc; const json_val *mods;
    if (!name || !name[0] || strchr(name, '/') || strchr(name, '.')) {
        fprintf(stderr, "profile: bad name\n");
        return -1;
    }
    snprintf(path, sizeof path, "profiles/%s.json", name);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "profile: %s: %s\n", path, err); return -1; }
    snprintf(p_name, sizeof p_name, "%s", name);
    p_nmods = 0; p_mode = -1; memset(p_hidden, 0, sizeof p_hidden);
    p_nscenes = 0;
    {   const json_val *sc = json_get(doc, "scenes");
        for (const json_val *e = sc ? sc->child : NULL; e && p_nscenes < 8; e = e->next)
            if (e->key && json_str(e, NULL) && json_str(e, "")[0]) { snprintf(p_scenes[p_nscenes].type, 24, "%s", e->key); snprintf(p_scenes[p_nscenes].name, 64, "%s", json_str(e, "")); p_nscenes++; } }
    p_narenas = 0;
    {   const json_val *ar = json_get(doc, "arenas");
        for (const json_val *e = ar ? ar->child : NULL; e && p_narenas < 8; e = e->next)
            if (e->key && json_str(e, NULL) && json_str(e, "")[0]) { p_arenas[p_narenas].scene = atoi(e->key); snprintf(p_arenas[p_narenas].name, 64, "%s", json_str(e, "")); p_narenas++; }
    }
    if (!p_narenas && p_name[0] && !getenv("WF_ROMARENAS")) {
        /* a non-stock profile with no "arenas" plays the PACKED stock arenas
         * (arenas/wwf, challenge, cage -> build/arenas/*.pak): one pipeline for
         * a clone and its original; the stock profile keeps the ROM scenery
         * (user 2026-08-30). WF_ROMARENAS=1 = the ROM composer for a check. */
        static const struct { int scene; const char *name; } def[3] = { { 0, "wwf" }, { 5, "challenge" }, { 1, "cage" } };
        for (int k = 0; k < 3; k++) { p_arenas[p_narenas].scene = def[k].scene; snprintf(p_arenas[p_narenas].name, 64, "%s", def[k].name); p_narenas++; }
    }
    {   const char *m = json_str(json_get(doc, "mode"), NULL);
        if (m && !strcmp(m, "tag")) p_mode = 0; else if (m && !strcmp(m, "rumble")) p_mode = 1;
        else if (m) fprintf(stderr, "profile: mode '%s' is not tag|rumble - ignored\n", m);
        for (const json_val *e = json_get(doc, "disabled") ? json_get(doc, "disabled")->child : NULL; e; e = e->next) {
            int id = (int)json_int(e, -1); if (id >= 0 && id < 64) p_hidden[id] = 1; }
    }
    mods = json_get(doc, "mods");
    if (mods && mods->type == JSON_ARRAY)
        for (const json_val *e = mods->child; e && p_nmods < P_MAX_MODS; e = e->next) {
            const char *m = json_str(e, NULL);
            if (m && m[0] && !strchr(m, '/') && !strchr(m, '.'))
                snprintf(p_mods[p_nmods++], 64, "%s", m);
        }
    json_free(doc);
    fprintf(stderr, "profile: %s (%d mod%s)\n", p_name, p_nmods, p_nmods == 1 ? "" : "s");
    if (access("mods/order.txt", R_OK) == 0)
        fprintf(stderr, "profile: NOTE mods/order.txt is retired and ignored — the profile lists its mods\n");
    return 0;
}

int wf_mod_resolve(const char *rel, char *out, size_t n)
{
    if (p_name[0] && roster_rel(rel) && roster_hidden_rel(rel)) return 0;   /* disabled in this profile */
    for (int i = p_nmods - 1; i >= 0; i--) {           /* later wins */
        snprintf(out, n, "mods/%s/%s", p_mods[i], rel);
        if (access(out, R_OK) == 0) return 1;
    }
    if (p_name[0] && roster_rel(rel)) {                /* the shared roster: lowest layer */
        snprintf(out, n, "roster/%s", rel);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

/* does any active mod carry files under <sub>/ ? */
static int mods_touch(const char *sub)
{
    char d[256]; struct stat st;
    for (int i = 0; i < p_nmods; i++) {
        snprintf(d, sizeof d, "mods/%s/%s", p_mods[i], sub);
        if (stat(d, &st) == 0) return 1;
    }
    return 0;
}

void wf_profile_pak_paths(char *base, size_t bn, char *wsdir, size_t wn, char *gfx, size_t gn)
{
    if (!p_name[0]) {                                   /* stock */
        snprintf(base, bn, "build/base.pak");
        snprintf(wsdir, wn, "build/wrestlers");
        snprintf(gfx, gn, "build/gfx.pak");
        return;
    }
    snprintf(base, bn, "build/profiles/%s/base.pak", p_name);
    /* a profile that mods no wrestler/gfx files reuses the stock paks */
    { struct stat rs; if (mods_touch("wrestlers") || stat("roster/wrestlers", &rs) == 0) snprintf(wsdir, wn, "build/profiles/%s/wrestlers", p_name);
    else snprintf(wsdir, wn, "build/wrestlers"); }
    if (mods_touch("gfx-edit")) snprintf(gfx, gn, "build/profiles/%s/gfx.pak", p_name);
    else snprintf(gfx, gn, "build/gfx.pak");
}

/* the library scene a TYPE plays in this profile: the "scenes" map, else
 * the type's own name (the converted stock scene); the stock profile plays
 * the ROM's screens and gets 0 */
static char force_type[24], force_name[64];
void wf_profile_scene_force(const char *type, const char *name) { snprintf(force_type, sizeof force_type, "%s", type); snprintf(force_name, sizeof force_name, "%s", name); }
int wf_profile_scene(const char *type, char *name, size_t n)
{
    if (force_type[0] && !strcmp(force_type, type)) { snprintf(name, n, "%s", force_name); return 1; }
    if (!p_name[0] || getenv("WF_ROMSCENES")) return 0;
    for (int i = 0; i < p_nscenes; i++) if (!strcmp(p_scenes[i].type, type)) { snprintf(name, n, "%s", p_scenes[i].name); return 1; }
    snprintf(name, n, "%s", type);
    return 1;
}

int wf_profile_arena_assign(int i, int *scene, char *name, size_t n)   /* i-th "arenas" entry, 0 = none */
{
    if (i < 0 || i >= p_narenas) return 0;
    *scene = p_arenas[i].scene; snprintf(name, n, "%s", p_arenas[i].name);
    return 1;
}

void wf_profile_weapons_pak(char *out, size_t n)
{
    if (p_name[0] && mods_touch("weapons")) snprintf(out, n, "build/profiles/%s/weapons.pak", p_name);
    else snprintf(out, n, "build/weapons.pak");
}

/* newest mtime under a directory tree (small trees: mod layers) */
static time_t tree_mtime(const char *dir)
{
    DIR *d = opendir(dir); struct dirent *e; struct stat st;
    time_t newest = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char p[512];
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { time_t t = tree_mtime(p); if (t > newest) newest = t; }
        else if (st.st_mtime > newest) newest = st.st_mtime;
    }
    closedir(d);
    return newest;
}

int wf_profile_stale(void)
{
    char base[256], wsdir[256], gfx[256], p[256];
    struct stat st, ps;
    time_t src = 0;
    if (!p_name[0]) return 0;                            /* stock: build.sh owns it */
    wf_profile_pak_paths(base, sizeof base, wsdir, sizeof wsdir, gfx, sizeof gfx);
    if (stat(base, &st)) return 1;                       /* never packed */
    { int tbl_pak_matches(const char *); if (!tbl_pak_matches(base)) return 1; }   /* the ENGINE's table set changed */
    snprintf(p, sizeof p, "profiles/%s.json", p_name);
    if (stat(p, &ps) == 0 && ps.st_mtime > src) src = ps.st_mtime;
    for (int i = 0; i < p_nmods; i++) {
        time_t t;
        snprintf(p, sizeof p, "mods/%s", p_mods[i]);
        t = tree_mtime(p);
        if (t > src) src = t;
    }
    { time_t t = tree_mtime("roster"); if (t > src) src = t; }   /* the shared roster */
    {   /* base data changes (exports) also invalidate: cheap proxy */
        struct stat ms;
        if (stat("data/tables/manifest.json", &ms) == 0 && ms.st_mtime > src) src = ms.st_mtime;
    }
    return src > st.st_mtime;
}

static void mkdir_p2(const char *path)
{
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(tmp, 0775); *q = '/'; }
    mkdir(tmp, 0775);
}

int wf_profile_pack(void)
{
    extern int tbl_pack(const char *data_dir, const char *out_path);
    extern int tool_pack_wrestlers(const char *data_dir, const char *out_dir);
    extern int wf_video_pack_gfx(const char *pak_path);
    char base[256], wsdir[256], gfx[256], dir[256];
    int rc;
    wf_profile_pak_paths(base, sizeof base, wsdir, sizeof wsdir, gfx, sizeof gfx);
    if (p_name[0]) { snprintf(dir, sizeof dir, "build/profiles/%s", p_name); mkdir_p2(dir); }
    rc = tbl_pack("data/tables", base);
    if (strncmp(wsdir, "build/wrestlers", 16) != 0) {    /* profile-own wrestler paks */
        extern void wf_pack_set_stockskins(int on);
        mkdir_p2(wsdir);
        wf_pack_set_stockskins(1);       /* non-stock profile: stock men in the SKIN layout (data/stockskins) */
        rc = tool_pack_wrestlers("data/wrestlers", wsdir) || rc;
        wf_pack_set_stockskins(0);
    }
    if (strcmp(gfx, "build/gfx.pak") != 0)
        rc = wf_video_pack_gfx(gfx) || rc;
    {   /* the weapon table (profile-own only when its mods carry weapons/) */
        extern int tool_pack_weapons(const char *dir, const char *out);
        char wp[256];
        wf_profile_weapons_pak(wp, sizeof wp);
        if (strcmp(wp, "build/weapons.pak") != 0) rc = tool_pack_weapons("data/weapons", wp) || rc;
    }
    {   /* the library arenas this profile names ("arenas": {scene: name}) -> build/arenas/<name>.pak */
        extern int tool_pack_profile_arenas(void);
        rc = tool_pack_profile_arenas() || rc;
    }
    return rc;
}

int wf_profile_list(void)
{
    DIR *d = opendir("profiles"); struct dirent *e;
    printf("stock            the original game (no mods) — plain `wfengine`\n");
    if (d) {
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            char path[300], err[128];
            json_val *doc;
            if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
            snprintf(path, sizeof path, "profiles/%s", e->d_name);
            doc = json_parse_file(path, err, sizeof err);
            if (doc) {
                char name[64]; snprintf(name, sizeof name, "%.*s", (int)(l - 5), e->d_name);
                printf("%-16s %s\n", name, json_str(json_get(doc, "description"), ""));
                json_free(doc);
            }
        }
        closedir(d);
    }
    return 0;
}

void wf_profile_clear(void)          /* back to STOCK (editor profile switch) */
{
    p_name[0] = 0; p_nmods = 0;
}
