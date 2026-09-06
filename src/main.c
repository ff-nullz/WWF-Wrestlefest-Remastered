/* Native engine SDL shell. Window + fixed 57.44 Hz pacing here;
 * everything deterministic lives in core.c. */
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>
#include <time.h>
#include "engine.h"
#include "tbl.h"
#include "profile.h"
#include "json.h"
extern int eng_dbgsel;
#include "version.h"
#include "audio.h"
#include "scene.h"
#include "credit.h"
#include "editor.h"
#include "keymap.h"

static int sound_on;
double eng_sound_wav(const char *name)
{
    char path[300];
    if (getenv("WF_SNDLOG")) fprintf(stderr, "snd: wav %s\n", name);
    if (!sound_on) return 0;
    snprintf(path, sizeof path, "sounds/%s.wav", name);
    return audio_play_wav(path);
}

void eng_sound(unsigned cmd)
{
    if (getenv("WF_SNDLOG")) fprintf(stderr, "snd: %02X\n", cmd);
    if (sound_on)
        audio_on_sound_latch((uint16_t)cmd);
}

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* WF_TRACE=<objmask>: per-frame state trace (bit 0x10 = referee), shared by
 * the headless loop and the window so a live capture carries positions. */

/* Taskbar icon + foreground raise for our SDL windows (user 2026-08-28:
 * "wfeditor spawns ... always in the background" + "give it a proper
 * taskbar icon"). The icon is a chunky yellow W on ring blue, built in
 * code so there is no asset to ship. */
void wf_window_dress(SDL_Window *w2)
{
    static uint32_t px[32 * 32];
    SDL_Surface *s;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int border = x < 2 || y < 2 || x >= 30 || y >= 30;
            px[y * 32 + x] = border ? 0xFF14204Cu : 0xFF2540A8u;
        }
    for (int y = 7; y <= 25; y++) {                    /* four strokes = W */
        int t = ((y - 7) * 256) / 18;
        int xs[4] = { 4 + (6 * t >> 8), 16 - (6 * t >> 8), 16 + (6 * t >> 8), 28 - (6 * t >> 8) };
        for (int k = 0; k < 4; k++)
            for (int dx2 = 0; dx2 < 3; dx2++) {
                int x = xs[k] + dx2;
                if (x >= 2 && x < 30) px[y * 32 + x] = 0xFFF7CE18u;
            }
    }
    s = SDL_CreateRGBSurfaceWithFormatFrom(px, 32, 32, 32, 32 * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) { SDL_SetWindowIcon(w2, s); SDL_FreeSurface(s); }
    SDL_RaiseWindow(w2);
    SDL_SetWindowInputFocus(w2);
}

static void trace_frame(const eng_state *st, long i)
{
    int mask = getenv("WF_TRACE") ? (int)strtol(getenv("WF_TRACE"), 0, 0) : 0;
    if (!mask) return;
    {
        for (int k = 0; k < ENG_MAX_OBJS; k++) {   /* bits 0-3 = slots 0-3, 0x10 = referee, 0x20.. = slots 4-8 */
            int bit = k < 4 ? (1 << k) : (1 << (k + 1));
            if (!(bit & mask)) continue;
            fprintf(stderr, "tr f%04ld o%d (%X,%X,%X) st=%04X mv=%02X rc=%02X frm=%02X cnt=%X g44=%X ap=%d pn=%d hp=%d pin=%d fc=%c f33=%02X zn=%d ai=%X/%02X op=%d f56=%02X in=%d f34=%02X e6=%u rt=%u atk=%04X f35=%02X f32=%04X hp2=%u sp=%04X off=%04X/%04X c6=%02X c7=%u c8=%u\n",
                    i, k, st->obj[k].x >> 16, st->obj[k].y >> 16, st->obj[k].z >> 16,
                    st->obj[k].state, st->obj[k].move_id & 0xFF, st->obj[k].react_id & 0xFF,
                    st->obj[k].frame & 0xFF, st->obj[k].count, st->obj[k].grap44, st->obj[k].apron,
                    st->obj[k].partner, st->obj[k].hold_ph, st->obj[k].pinning,
                    (st->obj[k].facing & 0x8000u) ? 'R' : 'L', st->obj[k].role & 0xFF, st->obj[k].zone,
                    st->obj[k].ai_sub, st->obj[k].ai_mv,
                    st->obj[k].opp, st->obj[k].driver & 0xFF,
                    st->obj[k].input, st->obj[k].tag_flags & 0xFF, st->obj[k].ai_e6,
                    st->obj[k].ai_runin_t, st->obj[k].atk, st->obj[k].cue_flags & 0xFF, st->obj[k].st_flags, st->obj[k].hp, st->obj[k].spr, st->obj[k].off_x, st->obj[k].off_y, st->obj[k].cmb_c6, st->obj[k].hits_c7, st->obj[k].cmb_c8);
        }
        if (0x10 & mask)
            fprintf(stderr, "tr f%04ld ref (%X,%X) sm=%d tgt=%d cell=%d spr=%02X ush=%u | scene=%d g161=%02X trig=%04X count=%d cam=(%X,%X)\n",
                    i, st->ref.x >> 16, st->ref.y >> 16, st->ref.sm, st->ref.target, st->ref.cell, st->ref.spr & 0xFF,
                    st->usher_t,
                    st->scene, st->g161, st->ringout_trig, st->count_out & 0xFF, st->cam_x, st->cam_y);
    }
}

const char *wf_art_pose_extra;   /* --art-pose optional per-roll direction */
int main(int argc, char **argv)
{
    /* The engine's own ROM set lives in ./rom (see HANDOFF.md); the old
     * shared path is kept as a fallback so a run from another cwd still
     * works. --roms DIR overrides both. */
    const char *rom_dir = "rom";
    const char *shot = NULL;
    const char *drive = "demo";
    int export_id = -1; const char *export_dir = "data/wrestlers";
    const char *export_arena_dir = NULL; int export_arena_scene = -1, arena_extent = -1, verify_arenas = 0;
    const char *pack_arena = NULL, *new_arena = NULL; int new_arena_scene = 0;
    const char *export_scene = NULL, *export_scene_dir = NULL; int export_scene_wait = 0; const char *pack_scene = NULL, *play_scene = NULL; int export_row = -1; const char *export_row_dir = NULL;
    const char *export_all = NULL, *export_stock = NULL, *pack_dir = NULL, *pack_out = NULL; int list_tables = 0, verify_gfx = 0, editor = 0;
    const char *profile = NULL; int pack_profile = 0, editor_play = 0; const char *sparsify_mod = NULL; const char *export_gfx = NULL; int rules_doc = 0;
    int export_sound = 0; const char *rm_cmd = NULL, *rm_out = NULL; double rm_secs = 0;
    const char *rt_src = NULL, *rt_dst = NULL; unsigned rt_base = 0;
    const char *ss_dir = NULL, *ss_out = NULL; int ss_wid = -1;
    int ps_id = -1, ps_first = 0, ps_count = 0; const char *ps_out = NULL;
    int is_base = -1; unsigned is_arena = 0; const char *is_dst = NULL;
    const char *mc_out = NULL, *pi_out = NULL, *vp_name = NULL, *fc_out = NULL, *wt_dir = NULL, *br_in = NULL, *br_out = NULL, *gc_dir = NULL; int br_cls = -1, gc_cls = -1; const char *cv_arg = NULL, *mp_out = NULL; int mp_ex = -1, cp_cls = -1, cb_cls = -1, cd_cls = -1; double cd_iou = 0.85; const char *cb_only = NULL, *cb_extra = NULL; int stock_skins = 0, weapon_check = 0; const char *sf_skin = NULL; const char *pw_dir = NULL, *pw_out = NULL;
    int aj_base = -1; const char *aj_dir = NULL, *ci_out = NULL;
    int es_base = -1, es_verify = 0; const char *es_dir = NULL;
    int ct_cls = -1, ct_stock = 0; const char *ct_dir = NULL;
    int rc_row = -1, rc_cell = 0, rc_pal = 0; const char *rc_out = NULL;
    const char *ar_dir = NULL, *ar_anchor = NULL, *ar_desc = NULL; const char *aar_name = NULL, *atp_style = NULL, *atp_out = NULL; const char *apl_src = NULL, *apl_dst = NULL; const char *asl_name = NULL, *asl_view = NULL, *asl_scene = NULL, *asl_over = NULL, *asl_front = NULL, *asl_shift = NULL;
    const char *apr_d=NULL,*apr_o=NULL, *aa_dir=NULL,*aa_desc=NULL,*aa_out=NULL, *apo_a=NULL,*apo_b=NULL,*apo_d=NULL,*apo_o=NULL, *api_raw=NULL, *ak_in=NULL,*ak_out=NULL; int ak_w=0,ak_h=0; const char *api_dst=NULL;
    const char *ap_dir=NULL,*ap_anchor=NULL,*ap_desc=NULL; int ap_pose=-1; int calib_report = 0;
    const char *al_dir=NULL; int al_iou = 0; const char *afh_dir=NULL, *ark_dir=NULL;
    const char *ai_dir = NULL; int ai_slot = -1; const char *ai_dst = NULL;
    long frames = -1;
    int scale = 3, headless = 0, selftest = 0, fullscreen = 0, front = -1;

    fprintf(stderr, "wfengine V%s\n", WF_VERSION_STRING);   /* major.minor (build.sh bumps the minor) */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--roms") && i + 1 < argc) rom_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--front")) front = 1;      /* force the front end */
        else if (!strcmp(argv[i], "--no-front")) front = 0;   /* straight into the match */
        else if (!strcmp(argv[i], "--selftest")) { selftest = 1; headless = 1; }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--drive") && i + 1 < argc) drive = argv[++i];
        else if (!strcmp(argv[i], "--export") && i + 2 < argc) { export_id = atoi(argv[++i]); export_dir = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--arena-extent") && i + 1 < argc) { arena_extent = atoi(argv[++i]); headless = 1; }
        else if (!strcmp(argv[i], "--verify-arenas")) { verify_arenas = 1; headless = 1; }
        else if (!strcmp(argv[i], "--pack-arena") && i + 1 < argc) { pack_arena = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--pack-scene") && i + 1 < argc) { pack_scene = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--export-row") && i + 2 < argc) { export_row = (int)strtol(argv[++i], 0, 0); export_row_dir = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--play-scene") && i + 1 < argc) {   /* NAME: play scenes/<name> (sceneplay.c) - windowed, or headless with --frames/--shot */
            char pth[300], err[128]; json_val *doc; const char *type;
            play_scene = argv[++i];
            snprintf(pth, sizeof pth, "scenes/%s/scene.json", play_scene);
            doc = json_parse_file(pth, err, sizeof err);
            if (!doc) { fprintf(stderr, "play-scene: %s: %s\n", pth, err); return 2; }
            type = json_str(json_get(doc, "type"), "talk");
            wf_profile_scene_force(type, play_scene);
            setenv("WF_INTERLUDE", type, 1); setenv("WF_SCENE_EXIT", "1", 0);
            if (frames < 0) frames = !strcmp(type, "talk") ? 220 : 160;
            json_free(doc);
        }
        else if (!strcmp(argv[i], "--export-scene") && i + 2 < argc) {   /* NAME DIR: tools/export_scene.c */
            export_scene = argv[++i]; export_scene_dir = argv[++i]; headless = 1;
            if (!strcmp(export_scene, "walkout")) {           /* the select's pick script -> the aisle */
                drive = "charselect"; frames = 3000; export_scene_wait = 4;
                setenv("WF_INTRO", "1", 0); setenv("WF_SEATED", "1", 0); setenv("WF_P1", "130-131:10,160-161:8,190-191:10", 0);
            } else if (!strcmp(export_scene, "talk"))   { setenv("WF_INTERLUDE", "talk", 1); frames = 220; }
            else if (!strcmp(export_scene, "title"))    { setenv("WF_INTERLUDE", "title", 1); frames = 160; }
            else if (!strcmp(export_scene, "belt"))     { setenv("WF_INTERLUDE", "belt", 1); frames = 160; }
            else if (!strcmp(export_scene, "ending"))   { setenv("WF_CEREMONY", "1", 1); frames = 500; }
        }
        else if (!strcmp(argv[i], "--new-arena") && i + 2 < argc) { new_arena = argv[++i]; new_arena_scene = atoi(argv[++i]); headless = 1; }
        else if (!strcmp(argv[i], "--export-arena") && i + 2 < argc) {   /* SCENE DIR: tools/export_arena.c */
            static const char *stage_of[8] = { "0", "2", "0", NULL, NULL, "1", "1", NULL };   /* match_scene_by_stage 00 05 01 05 .. */
            int sc = atoi(argv[++i]); export_arena_dir = argv[++i]; export_arena_scene = sc;
            headless = 1; drive = "script"; frames = 4;
            if (sc >= 0 && sc < 8 && stage_of[sc]) setenv("WF_STAGE", stage_of[sc], 1);
            if (sc == 2 || sc == 6) { setenv("WF_OUT2", "1", 1); frames = 120; }   /* ringside view: the throw-out poke */
        }
        else if (!strcmp(argv[i], "--export-all") && i + 1 < argc) { export_all = argv[++i]; headless = 1; }   /* tables -> JSON tree */
        else if (!strcmp(argv[i], "--export-stock") && i + 1 < argc) { export_stock = argv[++i]; headless = 1; }   /* pristine values -> one JSON */
        else if (!strcmp(argv[i], "--pack") && i + 2 < argc) { pack_dir = argv[++i]; pack_out = argv[++i]; headless = 1; }   /* JSON tree -> pak */
        else if (!strcmp(argv[i], "--export-gfx") && i + 1 < argc) { export_gfx = argv[++i]; headless = 1; }   /* build/gfx.pak straight from the ROM chips (bootstrap) */
        else if (!strcmp(argv[i], "--sparsify") && i + 1 < argc) { sparsify_mod = argv[++i]; headless = 1; }   /* a mod's full rules layers -> sparse overrides (stackable) */
        else if (!strcmp(argv[i], "--rules-doc")) { rules_doc = 1; headless = 1; }   /* the rule tables as markdown, from src/rules.def */
        else if (!strcmp(argv[i], "--tables")) { list_tables = 1; headless = 1; }
        else if (!strcmp(argv[i], "--verify-gfx")) { verify_gfx = 1; headless = 1; }
        else if (!strcmp(argv[i], "--editor")) editor = 1;   /* wfeditor (editor-only; + --play = beside the game) */
        else if (!strcmp(argv[i], "--play")) editor_play = 1;
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];   /* named mod profile (profile.h) */
        else if (!strcmp(argv[i], "--profiles")) { extern int wf_profile_list(void); return wf_profile_list(); }
        else if (!strcmp(argv[i], "--pack-profile") && i + 1 < argc) { profile = argv[++i]; pack_profile = 1; headless = 1; }
        else if (!strcmp(argv[i], "--export-sound")) { export_sound = 1; headless = 1; }   /* rom/ sound roms -> data/sound/ */
        else if (!strcmp(argv[i], "--retile") && i + 3 < argc) { rt_src = argv[++i]; rt_dst = argv[++i]; rt_base = (unsigned)strtoul(argv[++i], NULL, 0); headless = 1; }
        else if (!strcmp(argv[i], "--sstar") && i + 3 < argc) { ss_dir = argv[++i]; ss_wid = atoi(argv[++i]); ss_out = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--poses") && i + 4 < argc) { ps_id = atoi(argv[++i]); ps_first = atoi(argv[++i]); ps_count = atoi(argv[++i]); ps_out = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--move-catalog") && i + 1 < argc) { mc_out = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--pose-index") && i + 1 < argc) { pi_out = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--frame-coverage") && i + 1 < argc) { fc_out = argv[++i]; headless = 1; }   /* ROM scan: needs the ROM */
        else if (!strcmp(argv[i], "--weapon-table") && i + 1 < argc) { wt_dir = argv[++i]; headless = 1; }   /* data/wrestlers -> data/weapons */
        else if (!strcmp(argv[i], "--weapon-check")) { weapon_check = 1; headless = 1; }
        else if (!strcmp(argv[i], "--skin-fidelity") && i + 1 < argc) { sf_skin = argv[++i]; headless = 1; }   /* SKIN name or slot: what still draws the base */   /* weapon pak cells == ROM template cells, every man x pose x facing */
        else if (!strcmp(argv[i], "--pack-weapons") && i + 2 < argc) { pw_dir = argv[++i]; pw_out = argv[++i]; headless = 1; }   /* weapons dir -> weapons pak */
        else if (!strcmp(argv[i], "--generic-class") && i + 2 < argc) { gc_cls = atoi(argv[++i]); gc_dir = argv[++i]; headless = 1; }   /* the class's generic wrestler */
        else if (!strcmp(argv[i], "--class-verify") && i + 1 < argc) { cv_arg = argv[++i]; headless = 1; }   /* tools/classgen.c: the class template gate */
        else if (!strcmp(argv[i], "--class-build") && i + 1 < argc) { cb_cls = atoi(argv[++i]); if (i + 1 < argc && argv[i + 1][0] != '-') cb_only = argv[++i]; if (i + 1 < argc && argv[i + 1][0] != '-') cb_extra = argv[++i]; headless = 1; }   /* [ids] [direction] */   /* tools/classgen.c: draw the class's stand-ins on its body */
        else if (!strcmp(argv[i], "--class-dedupe") && i + 1 < argc) { cd_cls = atoi(argv[++i]); if (i + 1 < argc && argv[i + 1][0] != '-') cd_iou = atof(argv[++i]); headless = 1; }   /* [near IoU, default 0.85] */   /* tools/classgen.c: alias every certain duplicate, list the near ones */
        else if (!strcmp(argv[i], "--class-pack") && i + 1 < argc) { cp_cls = atoi(argv[++i]); headless = 1; }   /* tools/classgen.c: the class generic as the fallback package */
        else if (!strcmp(argv[i], "--move-poses") && i + 1 < argc) { mp_out = argv[++i]; if (i + 1 < argc && argv[i + 1][0] != '-') mp_ex = atoi(argv[++i]); headless = 1; }   /* tools/classgen.c: move -> pose ids per class */
        else if (!strcmp(argv[i], "--art-base-ref") && i + 3 < argc) { br_cls = atoi(argv[++i]); br_in = argv[++i]; br_out = argv[++i]; headless = 1; }   /* generic base ref for a class */
        else if (!strcmp(argv[i], "--stock-skins")) { stock_skins = 1; headless = 1; }
        else if (!strcmp(argv[i], "--verify-profile") && i + 1 < argc) { vp_name = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--class-inventory") && i + 1 < argc) { ci_out = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--export-skin") && i + 2 < argc) { es_base = atoi(argv[++i]); es_dir = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--class-template") && i + 2 < argc) { ct_cls = atoi(argv[++i]); ct_dir = argv[++i]; headless = 1;
                                                                            if (i + 1 < argc && !strcmp(argv[i + 1], "stock")) { ct_stock = 1; i++; } }
        else if (!strcmp(argv[i], "--verify-skin") && i + 2 < argc) { es_base = atoi(argv[++i]); es_dir = argv[++i]; es_verify = 1; headless = 1; }
        else if (!strcmp(argv[i], "--art-job") && i + 2 < argc) { aj_base = atoi(argv[++i]); aj_dir = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--row-cell") && i + 4 < argc) { rc_row = atoi(argv[++i]); rc_cell = atoi(argv[++i]); rc_pal = atoi(argv[++i]); rc_out = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--art-run") && i + 3 < argc) { ar_dir = argv[++i]; ar_anchor = argv[++i]; ar_desc = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--art-template") && i + 2 < argc) { atp_style = argv[++i]; atp_out = argv[++i]; headless = 1; }   /* the 8 template pictures of a style through codex */
        else if (!strcmp(argv[i], "--art-arena") && i + 1 < argc) { extern const char *wf_art_arena_only; aar_name = argv[++i]; headless = 1;
                                                                       if (i + 1 < argc && argv[i + 1][0] != '-') wf_art_arena_only = argv[++i]; }   /* [view/file.png] = that one picture only */   /* arena recipe: every picture of both views repainted in the theme (Arenas > AI recipe) */
        else if (!strcmp(argv[i], "--arena-plain") && i + 2 < argc) { apl_src = argv[++i]; apl_dst = argv[++i]; headless = 1; }   /* the plain arena template from a library arena */
        else if (!strcmp(argv[i], "--arena-slice") && i + 3 < argc) { asl_name = argv[++i]; asl_view = argv[++i]; asl_scene = argv[++i]; if (i + 1 < argc && argv[i + 1][0] != '-') asl_over = argv[++i]; if (i + 1 < argc && argv[i + 1][0] != '-') asl_front = argv[++i]; if (i + 1 < argc && argv[i + 1][0] != '-') asl_shift = argv[++i]; headless = 1; }   /* whole-scene picture -> the view's layer files */
        else if (!strcmp(argv[i], "--art-prompt-defaults") && i + 1 < argc) { apr_o = argv[++i]; apr_d = ""; headless = 1; }
        else if (!strcmp(argv[i], "--calib-report")) { calib_report = 1; headless = 1; }
        else if (!strcmp(argv[i], "--art-prompts") && i + 2 < argc) { apr_d = argv[++i]; apr_o = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--art-anchor") && i + 3 < argc) { aa_dir = argv[++i]; aa_desc = argv[++i]; aa_out = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--art-portrait") && i + 4 < argc) { apo_a = argv[++i]; apo_b = argv[++i]; apo_d = argv[++i]; apo_o = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--art-portrait-install") && i + 2 < argc) { api_raw = argv[++i]; api_dst = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--art-key") && i + 4 < argc) { ak_in = argv[++i]; ak_out = argv[++i]; ak_w = atoi(argv[++i]); ak_h = atoi(argv[++i]); headless = 1; }   /* magenta-key + crop + box-downscale one PNG to fit W x H (badges) */
        else if (!strcmp(argv[i], "--art-fill-holes") && i + 1 < argc) { afh_dir = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--art-rekey") && i + 1 < argc) { ark_dir = argv[++i]; headless = 1; }   /* key the enclosed magenta pockets of finished frames */
        else if (!strcmp(argv[i], "--art-align") && i + 1 < argc) { al_dir = argv[++i]; headless = 1;
                                                                      if (i + 1 < argc && !strcmp(argv[i + 1], "iou")) { al_iou = 1; i++; } }
        else if (!strcmp(argv[i], "--art-pose") && i + 4 < argc) { ap_dir = argv[++i]; ap_anchor = argv[++i]; ap_desc = argv[++i]; ap_pose = atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') { extern const char *wf_art_pose_extra; wf_art_pose_extra = argv[++i]; }
            headless = 1; }
        else if (!strcmp(argv[i], "--art-ingest") && i + 3 < argc) { ai_dir = argv[++i]; ai_slot = atoi(argv[++i]); ai_dst = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--import-sstar") && i + 5 < argc) { ss_dir = argv[++i]; ss_wid = atoi(argv[++i]); is_base = atoi(argv[++i]); is_arena = (unsigned)strtoul(argv[++i], NULL, 0); is_dst = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--render-music") && i + 3 < argc) { rm_cmd = argv[++i]; rm_secs = atof(argv[++i]); rm_out = argv[++i]; headless = 1; }
        else {
            fprintf(stderr, "wfengine: unknown or incomplete argument '%s' (a flag with a missing value?)\n", argv[i]);
            fprintf(stderr, "usage: wfengine [--roms DIR] [--frames N] "
                            "[--scale N] [--fullscreen] [--front|--no-front] [--headless] [--shot out.ppm]\n");
            return 2;
        }
    }

    eng_state st;
    eng_dbgsel = getenv("WF_DBGSEL") != 0;
    /* Harness drives and the selftest digest assume a live match at
     * frame 0: skip the 0xA654 intro there unless WF_INTRO=1 asks for it. */
    if ((selftest || (headless && strcmp(drive, "demo"))) && !getenv("WF_INTRO"))
        setenv("WF_NOINTRO", "1", 0);
    if (getenv("WF_ROMDIR")) rom_dir = getenv("WF_ROMDIR");
    /* Data layer (docs/adr-001-data-formats.md): the game runs from the
     * paks (build/base.pak, build/wrestlers/NN.pak, build/gfx.pak) and
     * never opens rom/. The ROM is loaded only for the tools
     * (--export-all / --export / --verify-gfx) and for WF_DATA=rom, the
     * reference mode the regress gate compares the paks against. */
    if (profile && wf_profile_set(profile) != 0)
        return 1;
    if (export_sound) { extern int tool_export_sound(const char *); return tool_export_sound(rom_dir); }
    if (rt_src) { extern int tool_retile(const char *, const char *, unsigned); return tool_retile(rt_src, rt_dst, rt_base); }
    if (is_dst) { extern int tool_import_sstar(const char *, int, int, unsigned, const char *); return tool_import_sstar(ss_dir, ss_wid, is_base, is_arena, is_dst); }
    if (ss_dir) { extern int tool_sstar(const char *, int, const char *); return tool_sstar(ss_dir, ss_wid, ss_out); }
    if (rm_cmd) { extern int tool_render_music(const char *, double, const char *); return tool_render_music(rm_cmd, rm_secs, rm_out); }
    {
        char prof_base[256], prof_ws[256], prof_gfx[256];
        const char *mode = getenv("WF_DATA") ? getenv("WF_DATA") : "pak";
        const char *pak_path;
        int tool = export_all || export_stock || list_tables || export_id >= 0 || verify_gfx || fc_out;
        wf_profile_pak_paths(prof_base, sizeof prof_base, prof_ws, sizeof prof_ws, prof_gfx, sizeof prof_gfx);
        if (!getenv("WF_PAKDIR")) setenv("WF_PAKDIR", prof_ws, 0);   /* wrestler paks (package.c) */
        if (!getenv("WF_GFXPAK")) setenv("WF_GFXPAK", prof_gfx, 0);  /* gfx pak (video.c) */
        pak_path = getenv("WF_PAK") ? getenv("WF_PAK") : prof_base;
        if (profile && (pack_profile || wf_profile_stale())) {
            fprintf(stderr, "profile: packing %s...\n", profile);
            if (wf_profile_pack() != 0) return 1;
            if (pack_profile) return 0;
        }
        int rom_mode = tool || !strcmp(mode, "rom");
        if (export_gfx) { extern int wf_video_export_gfx_pak(const char *, const char *); return wf_video_export_gfx_pak(rom_dir, export_gfx); }
        if (pack_dir) {                /* the packer reads the JSON/PNG trees only */
            extern int tool_pack_wrestlers(const char *data_dir, const char *out_dir);
            int rc = tbl_pack(pack_dir, pack_out);
            rc = tool_pack_wrestlers("data/wrestlers", "build/wrestlers") || rc;   /* TODO: data root arg */
            { extern int tool_pack_weapons(const char *, const char *); rc = tool_pack_weapons("data/weapons", "build/weapons.pak") || rc; }
            { extern int tool_pack_badges(const char *, const char *); rc = tool_pack_badges("data/badges", "build/badges.pak") || rc; }
            { extern int tool_pack_all_arenas(void); rc = tool_pack_all_arenas() || rc; }   /* every arenas/<name>/ whose pak is missing or stale */
            return wf_video_pack_gfx("build/gfx.pak") || rc;
        }
        if (eng_render_init(rom_dir, rom_mode) != 0)
            return 1;
        if (rom_mode) {
            if (tbl_load_rom() != 0) return 1;
        } else if (tbl_load_pak(pak_path) != 0) {
            fprintf(stderr, "engine: cannot load %s — run ./build.sh (or ./wfengine --pack data/tables build/base.pak)\n", pak_path);
            return 1;
        }
        if (eng_dbgsel || tool) fprintf(stderr, "data: %d tables, backend=%s\n", tbl_count(), tbl_backend());
    }
    if (list_tables) return tbl_list();
    if (mc_out) { extern int tool_move_catalog(const char *); return tool_move_catalog(mc_out); }
    if (pi_out) { extern int tool_pose_index(const char *); return tool_pose_index(pi_out); }   /* --pose-index OUT.json */
    if (fc_out) { extern int tool_frame_coverage(const char *); return tool_frame_coverage(fc_out); }   /* --frame-coverage OUT.json */
    if (wt_dir) { extern int tool_weapon_table(const char *); return tool_weapon_table(wt_dir); }   /* --weapon-table DIR */
    if (pw_dir) { extern int tool_pack_weapons(const char *, const char *); return tool_pack_weapons(pw_dir, pw_out); }   /* --pack-weapons DIR OUT.pak */
    if (weapon_check) { extern int tool_weapon_check(void); return tool_weapon_check(); }
    if (sf_skin) { extern int tool_skin_fidelity(const char *); return tool_skin_fidelity(sf_skin); }   /* --weapon-check: pak cells == ROM template cells */
    if (gc_dir) { extern int tool_generic_class(int, const char *); return tool_generic_class(gc_cls, gc_dir); }
    if (cd_cls >= 0) { extern int tool_class_dedupe(int, double); return tool_class_dedupe(cd_cls, cd_iou); }
    if (cb_cls >= 0) { extern int tool_class_build(int, const char *, const char *); return tool_class_build(cb_cls, cb_only, cb_extra); }
    if (cp_cls >= 0) { extern int tool_class_pack(int); return tool_class_pack(cp_cls); }
    if (mp_out) { extern int tool_move_poses(const char *, int); return tool_move_poses(mp_out, mp_ex); }
    if (cv_arg) { extern int tool_class_verify(int), tool_class_verify_all(void); return strcmp(cv_arg, "all") ? tool_class_verify(atoi(cv_arg)) : tool_class_verify_all(); }
    if (br_in) { extern int tool_art_base_ref(int, const char *, const char *); return tool_art_base_ref(br_cls, br_in, br_out); }
    if (vp_name) { extern int tool_verify_profile(const char *); return tool_verify_profile(vp_name); }   /* --verify-profile P: the fidelity gate */
    if (stock_skins) {                 /* --stock-skins: the 12 stock men exported + ingested into data/stockskins/NN
                                          (the skin layout every non-stock profile packs them from; user 2026-08-26) */
        extern int tool_export_skin(int, const char *), tool_art_ingest(const char *, int, const char *);
        int fails = 0;
        eng_init(&st);
        for (int id = 0; id < 12; id++) {
            char dst[128], outd[160];
            /* the package IS the job dir: frames/ (canonical art, kept),
             * victmap/manifest/palette beside it, sheet/poses/tiles derived
             * by the ingest (frames/ is read when out/ has no frame) */
            snprintf(dst, sizeof dst, "data/stockskins/%02d", id);
            mkdir("data/stockskins", 0775);
            if (tool_export_skin(id, dst)) { fails++; continue; }
            if (tool_art_ingest(dst, id, dst)) fails++;
            snprintf(outd, sizeof outd, "%s/out", dst); rmdir(outd);   /* empty: nothing generated */
        }
        fprintf(stderr, "stock-skins: %d of 12 done%s\n", 12 - fails, fails ? " (FAILURES)" : "");
        return fails ? 1 : 0;
    }
    if (rules_doc) { extern int eng_rules_doc(void); return eng_rules_doc(); }
    if (sparsify_mod) return tbl_sparsify_rules(sparsify_mod);
    if (export_all) return tbl_export_json(export_all);
    if (export_stock) return tbl_export_stock(export_stock);
    if (verify_gfx) return wf_video_verify_gfx(rom_dir);
    if (ps_out) {                      /* --poses: after the data layer, before the loop */
        extern int tool_pose_strip(unsigned, unsigned, unsigned, const char *);
        eng_init(&st);
        return tool_pose_strip((unsigned)ps_id, (unsigned)ps_first, (unsigned)ps_count, ps_out);
    }
    if (ct_dir) {                      /* --class-template C DIR (the generator's job dir) */
        extern int tool_class_template(int, const char *), tool_class_template_stock(int, const char *);
        eng_init(&st);
        return ct_stock ? tool_class_template_stock(ct_cls, ct_dir) : tool_class_template(ct_cls, ct_dir);
    }
    if (es_dir) {                      /* --export-skin / --verify-skin BASE DIR (stock as skin 0) */
        extern int tool_export_skin(int, const char *), tool_verify_skin(int, const char *);
        eng_init(&st);
        return es_verify ? tool_verify_skin(es_base, es_dir) : tool_export_skin(es_base, es_dir);
    }
    if (ci_out) {                      /* --class-inventory OUT.json (body templates) */
        extern int tool_class_inventory(const char *);
        eng_init(&st);
        return tool_class_inventory(ci_out);
    }
    if (aj_dir) {                      /* --art-job: needs the package/tile layer */
        extern int tool_art_job(int, const char *);
        eng_init(&st);
        return tool_art_job(aj_base, aj_dir);
    }
    if (rc_out) {                      /* --row-cell ROW CELL PALID OUT (art ref/inspection) */
        extern int tool_row_cell(unsigned, unsigned, unsigned, const char *);
        eng_init(&st);
        return tool_row_cell((unsigned)rc_row, (unsigned)rc_cell, (unsigned)rc_pal, rc_out);
    }
    if (calib_report) { eng_init(&st); eng_calib_report(); return 0; }   /* --calib-report: class vs skin calibration layers */
    if (asl_scene) { extern int tool_arena_slice(const char *, const char *, const char *, const char *, const char *, const char *); return tool_arena_slice(asl_name, asl_view, asl_scene, asl_over, asl_front, asl_shift); }
    if (apl_dst) { extern int tool_arena_plain(const char *, const char *); return tool_arena_plain(apl_src, apl_dst); }
    if (atp_out) { extern int tool_art_template(const char *, const char *); return tool_art_template(atp_style, atp_out); }
    if (aar_name) {                    /* --art-arena NAME (arenas/NAME/recipe.json) */
        extern int tool_art_arena(const char *);
        return tool_art_arena(aar_name);
    }
    if (ar_dir) {                      /* --art-run JOBDIR ANCHOR CHARDESC.TXT (codex batch) */
        extern int tool_art_run(const char *, const char *, const char *);
        return tool_art_run(ar_dir, ar_anchor, ar_desc);
    }
    if (apr_o) {                       /* --art-prompts CHARDESC OUT.txt (skins tab, prompt page) */
        extern int tool_art_prompts(const char *, const char *), tool_art_prompt_defaults(const char *);
        return apr_d[0] ? tool_art_prompts(apr_d, apr_o) : tool_art_prompt_defaults(apr_o);
    }
    if (aa_out) {                      /* --art-anchor JOBDIR CHARDESC OUT (skins tab) */
        extern int tool_art_anchor(const char *, const char *, const char *);
        return tool_art_anchor(aa_dir, aa_desc, aa_out);
    }
    if (apo_o) {                       /* --art-portrait STYLE ANCHOR CHARDESC OUT */
        extern int tool_art_portrait(const char *, const char *, const char *, const char *);
        return tool_art_portrait(apo_a, apo_b, apo_d, apo_o);
    }
    if (api_dst) {                     /* --art-portrait-install RAW DST (80x80 cell) */
        extern int tool_art_portrait_install(const char *, const char *);
        return tool_art_portrait_install(api_raw, api_dst);
    }
    if (ak_in) { extern int tool_art_key(const char *, const char *, int, int); return tool_art_key(ak_in, ak_out, ak_w, ak_h); }
    if (afh_dir) { extern int tool_art_fill_holes(const char *); return tool_art_fill_holes(afh_dir); }
    if (ark_dir) { extern int tool_art_rekey(const char *); return tool_art_rekey(ark_dir); }
    if (al_dir) {                      /* --art-align JOBDIR [iou] (re-alignment, no codex) */
        extern int tool_art_align_iou(const char *);
        if (al_iou) return tool_art_align_iou(al_dir);
        extern int tool_art_align(const char *);
        return tool_art_align(al_dir);
    }
    if (ap_pose >= 0) {                /* --art-pose JOBDIR ANCHOR CHARDESC POSE (manual re-roll) */
        extern int tool_art_pose(const char *, const char *, const char *, int);
        return tool_art_pose(ap_dir, ap_anchor, ap_desc, ap_pose);
    }
    if (ai_dir) {
        extern int tool_art_ingest(const char *, int, const char *);
        eng_init(&st);
        return tool_art_ingest(ai_dir, ai_slot, ai_dst);
    }
    eng_init(&st);                     /* AFTER the data layer is bound: init-time table reads
                                          (rumble seats 0x10ACC, banner card, body palettes, hp
                                          fallback) see real bytes, and eng_init's FG0/palette
                                          writes are no longer wiped by eng_render_init's memset */
    if (list_tables) return tbl_list();
    if (verify_gfx) return wf_video_verify_gfx(rom_dir);
    if (export_all) return tbl_export_json(export_all);
    if (pack_dir) {
        extern int tool_pack_wrestlers(const char *data_dir, const char *out_dir);
        int rc = tbl_pack(pack_dir, pack_out);
        rc = tool_pack_wrestlers("data/wrestlers", "build/wrestlers") || rc;   /* TODO: data root arg */
        return wf_video_pack_gfx("build/gfx.pak") || rc;
    }
    eng_scene_init();
    /* The front end (ATTRACT 0x6FC -> coin + START -> GAME SELECT 0x52BE
     * -> character select -> intro) is the default for the window, as in
     * stock. --no-front / WF_NOFRONT=1 drop straight into the match;
     * headless harness runs do that unless --front / WF_FRONT /
     * --drive attract|gameselect|charselect ask for the screens. --drive gameselect
     * (or WF_FRONT=gameselect) starts at the game select as if a credit
     * had just been taken. */
    if (front < 0) front = headless ? 0 : 1;
    if (getenv("WF_FRONT") || !strcmp(drive, "gameselect") || !strcmp(drive, "attract") || !strcmp(drive, "charselect")) front = 1;
    if (getenv("WF_NOFRONT")) front = 0;
    { extern int eng_front; eng_front = front; }
    if (front && !getenv("WF_SEATED")) {
        /* FRONT-END runs collect seats from real START presses only: the
         * harness default (eng_seated = 3, both sides human for WF_P2
         * scripts) leaked a phantom 2P cursor into the select on paths
         * that skip the attract's clear — the editor's Launch, WF_FRONT=2
         * (user 2026-08-28: "only pressing start PER slot should induce a
         * slot", on the select screen AND mid-match). */
        extern int eng_seated;
        eng_seated = 1;
    }
    if (front) {
        const char *wf_front = getenv("WF_FRONT");
        if (!strcmp(drive, "gameselect") || (wf_front && !strcmp(wf_front, "gameselect")))
            eng_scene_set(ENG_SCENE_GAMESELECT);
        else
            eng_scene_set(ENG_SCENE_ATTRACT);
    }
    /* --drive charselect / WF_FRONT=2: straight to SELECT PLAYERS (tag),
     * the frame after the game select confirmed (0x58B2). */
    if (!strcmp(drive, "charselect") || (getenv("WF_FRONT") && atoi(getenv("WF_FRONT")) == 2))
        eng_scene_set(ENG_SCENE_CHARSELECT);
    if (getenv("WF_CEREMONY")) {       /* harness poke: straight into the 0x1B4E
                                          championship ending (campaign.c) */
        if (!eng_camp_armed()) eng_camp_new_game(NULL, NULL);
        eng_scene_set(ENG_SCENE_CEREMONY);
    }
    if (getenv("WF_INTERLUDE")) {      /* harness poke: one between-match screen (interlude.c):
                                          talk = the LOD talk screen, title = the title-win card,
                                          belt = the matches-until-the-title count */
        const char *k = getenv("WF_INTERLUDE");
        int wf_sceneplay_arm(const char *const *types, int n, int belt_idx);
        const char *types[1] = { k }; int belt_idx = !strcmp(k, "belt") ? (getenv("WF_BELT_IDX") ? atoi(getenv("WF_BELT_IDX")) : 0) : -1;
        if (!eng_camp_armed()) eng_camp_new_game(NULL, NULL);
        if (wf_sceneplay_arm(types, 1, belt_idx) > 0) eng_scene_set(ENG_SCENE_PLAY);   /* a non-stock profile: the data scene */
        else {
            if (!strcmp(k, "talk")) eng_interlude_arm_talk(0);
            else eng_interlude_arm2(!strcmp(k, "title"), belt_idx, 0, 0);
            eng_scene_set(ENG_SCENE_INTERLUDE);
        }
    }
    if (arena_extent >= 0) { extern int tool_arena_extent(int); return tool_arena_extent(arena_extent); }
    if (verify_arenas) { extern int tool_verify_arenas(const char *); return tool_verify_arenas(profile ? profile : ""); }
    if (pack_arena) { extern int tool_pack_arena(const char *); return tool_pack_arena(pack_arena); }
    if (pack_scene) { extern int tool_pack_scene(const char *); return tool_pack_scene(pack_scene); }
    if (export_row >= 0) { extern int tool_export_row(unsigned, const char *); return tool_export_row((unsigned)export_row, export_row_dir); }
    if (new_arena) { extern int tool_new_arena(const char *, int); return tool_new_arena(new_arena, new_arena_scene); }
    if (export_id >= 0) {
        extern int tool_export_wrestler(unsigned id, const char *dir);
        return tool_export_wrestler((unsigned)export_id, export_dir);
    }

    if (headless) {
        int render_all = getenv("WF_RENDER_ALL") != NULL;
        long n = frames < 0 ? (selftest ? 600 : 1) : frames;
        if (getenv("WF_SEATED")) {               /* harness poke: which players joined
                                                    (default 3 = both; 1 makes the P2
                                                    pair a stock CPU team at init) */
            extern int eng_seated;
            eng_seated = atoi(getenv("WF_SEATED")) & 0xF;
            eng_init_picks(&st, NULL);           /* the first init ran with the default */
        }
        uint64_t h = 1469598103934665603ull;              /* FNV-1a */
        for (long i = 0; i < n; i++) {
            if (export_scene_wait && st.scene == export_scene_wait) { n = i + 100; export_scene_wait = 0; }   /* --export-scene walkout: the aisle showed, 100 more frames */
            if (i == 30 && getenv("WF_ANNOUNCE")) {  /* harness poke (frame 30, after the match init): "id,phrase" -> the ring
                                                        announcer says the name then the phrase
                                                        (wrestler sound map proof, WF_SNDLOG=1) */
                int aid = 0x0F, aph = 0;
                if (sscanf(getenv("WF_ANNOUNCE"), "%i,%i", &aid, &aph) >= 1) eng_announce(&st, (unsigned)aid, (unsigned)aph);
            }
            if (getenv("WF_CPU1")) {                 /* P1 = CPU too (CPU vs CPU runs) */
                st.obj[0].cpu = 1;
                st.obj[0].role &= (uint16_t)~0x02u;
            }
            if (getenv("WF_PINAT") && i == atol(getenv("WF_PINAT"))) {   /* harness: a cover imposed beside P1 this frame
                                                                              (double-pin / pin-break gates): WF_PINPAIR="a,b"
                                                                              = pinner,victim slots (default 4,5 = seated rumble
                                                                              CPUs), placed 0x30 px toward the ring centre from P1 */
                int pa = 4, pv = 5; const char *pp = getenv("WF_PINPAIR");
                if (pp) sscanf(pp, "%d,%d", &pa, &pv);
                if (pa >= 0 && pa < ENG_MAX_OBJS && pv >= 0 && pv < ENG_MAX_OBJS && st.obj[pa].active && st.obj[pv].active) {
                    int32_t dxp = ((st.obj[0].x >> 16) > 0x280) ? -0x30 : 0x30;
                    st.obj[pa].x = st.obj[0].x + (dxp << 16); st.obj[pa].y = st.obj[0].y; st.obj[pa].z = 0x140 << 16; st.obj[pa].mover = 0;
                    eng_force_cover(&st, pa, pv);
                } else fprintf(stderr, "harness: WF_PINPAIR %d,%d not both active\n", pa, pv);
            }
            if (getenv("WF_CPU2")) {                 /* P2 = CPU opponent (re-applied
                                                        every frame: a decided match
                                                        re-inits the state) */
                st.obj[2].cpu = 1;
                st.obj[2].role &= (uint16_t)~0x02u;   /* not human (+0x33 b1) */
            }
            if (getenv("WF_STAGE"))                  /* $1C0162 stage/difficulty poke */
                st.stage = (uint16_t)(atoi(getenv("WF_STAGE")) % 10);
            if (i == 0 && getenv("WF_X1"))           /* harness poke: P1 start x/y */
                st.obj[0].x = (int32_t)strtol(getenv("WF_X1"), 0, 0) << 16;
            if (i == 0 && getenv("WF_Y1"))
                st.obj[0].y = (int32_t)strtol(getenv("WF_Y1"), 0, 0) << 16;
            if (i == 0 && getenv("WF_HP1")) {         /* harness poke: P1 hp (+band 0x24EC2) */
                eng_obj *o = &st.obj[0];
                o->hp = (uint16_t)strtoul(getenv("WF_HP1"), 0, 0);
                o->band = o->hp <= 0x18 ? 2 : o->hp <= (uint16_t)(2 * o->hp_max / 3) ? 1 : 0;
            }
            if (getenv("WF_CPU2AT") && i == atol(getenv("WF_CPU2AT")))   /* hand P2 to the CPU mid-run */
                st.obj[2].cpu = 1;
            if (getenv("WF_ELIM") && i == atol(getenv("WF_ELIM"))) {   /* harness poke (rumble): slot
                                                        WF_OUTSLOT (default 4) has just been
                                                        thrown out — lands at ringside and
                                                        runs the 0x7A walk-off */
                int s = getenv("WF_OUTSLOT") ? atoi(getenv("WF_OUTSLOT")) : 4;
                eng_obj *o = &st.obj[s];
                for (int q = 0; q < ENG_MAX_OBJS; q++) {   /* drop every link to him */
                    if (st.obj[q].partner == s) st.obj[q].partner = -1;
                    if (st.obj[q].last_pair == s) st.obj[q].last_pair = -1;
                }
                o->role |= RF_OUTSIDE; o->st_flags |= SF_ELIMINATED; o->state = ST_MOVE; o->move_id = 0x7A; o->grap44 = 4;
                o->partner = -1; o->x = 0x1B0 << 16; o->y = 0x120 << 16; o->z = 0x100 << 16;
            }
            if (getenv("WF_LIE") && i == atol(getenv("WF_LIE"))) {   /* harness poke: slot WF_OUTSLOT
                                                        lies face-up 0x60 px on P1's head side */
                int s = getenv("WF_OUTSLOT") ? atoi(getenv("WF_OUTSLOT")) : 4;
                eng_obj *o = &st.obj[s], *p1 = &st.obj[0];
                o->x = p1->x + (((p1->facing & 0x8000u) ? 0x60 : -0x60) << 16); o->y = p1->y; o->z = 0x140 << 16;
                o->facing = p1->facing; o->partner = -1;           /* head toward P1 */
                o->state = ST_REACT; o->react_id = RC_LYING; o->down_t = 0x300; o->hp = 20;
            }
            if (i == 1 && getenv("WF_DEACT")) {      /* harness poke: comma list of slots
                                                        to deactivate at f1 (choreography
                                                        runs without the other CPUs) */
                const char *q = getenv("WF_DEACT");
                while (*q) {
                    int s = (int)strtol(q, (char **)&q, 0);
                    if (s >= 0 && s < ENG_MAX_OBJS) {
                        st.obj[s].active = 0;
                        for (int k = 0; k < ENG_MAX_OBJS; k++) {
                            if (st.obj[k].partner == s) st.obj[k].partner = -1;
                            if (st.obj[k].opp == s) st.obj[k].opp = -1;
                        }
                    }
                    while (*q == ',' || *q == ' ') q++;
                }
            }
            if (getenv("WF_KO6E") && i == atol(getenv("WF_KO6E"))) {   /* harness poke (rumble):
                                                        slot WF_OUTSLOT was just SUBMITTED —
                                                        the post-ko_check 0x6E lie (0x17F1C/
                                                        0x179E2 family). The 0x19D82 handler
                                                        must walk him off after the 0x80
                                                        count (0x19DDE). */
                int s = getenv("WF_OUTSLOT") ? atoi(getenv("WF_OUTSLOT")) : 4;
                eng_obj *o = &st.obj[s];
                for (int q = 0; q < ENG_MAX_OBJS; q++)
                    if (st.obj[q].partner == s) st.obj[q].partner = -1;
                o->hp = 0; o->st_flags |= SF_ELIMINATED;            /* engine ko_check rumble branch */
                o->state = ST_MOVE; o->move_id = 0x6E; o->partner = -1;
            }
            if (getenv("WF_ELIMIN") && i == atol(getenv("WF_ELIMIN"))) {   /* harness poke (rumble):
                                                        slot WF_OUTSLOT was pinned/submitted
                                                        inside: eliminated bit, the stand
                                                        handler sends him off (0x1152A) */
                int s = getenv("WF_OUTSLOT") ? atoi(getenv("WF_OUTSLOT")) : 4;
                eng_obj *o = &st.obj[s];
                for (int q = 0; q < ENG_MAX_OBJS; q++) {
                    if (st.obj[q].partner == s) st.obj[q].partner = -1;
                    if (st.obj[q].last_pair == s) st.obj[q].last_pair = -1;
                }
                o->st_flags |= SF_ELIMINATED; o->state = ST_STAND; o->partner = -1;
            }
            if (getenv("WF_OUT2") && i == atol(getenv("WF_OUT2"))) {   /* harness poke: P2 is a
                                                        faller down at ringside and the
                                                        ring-out scene fires (0xF98C) */
                eng_obj *o = &st.obj[2];
                o->role |= RF_OUTSIDE; o->state = ST_MOVE; o->move_id = 0x6A; o->partner = -1;
                o->x = 0x1B0 << 16; o->y = 0x120 << 16; o->z = 0x100 << 16;
                st.ringout_trig = 0x8000u; st.ringout_face = (uint16_t)(getenv("WF_OUTFACE") ? 0x8000u : 0);
                if (getenv("WF_OUT2_NOPAD")) { o->input = -1; o->role &= (uint16_t)~0x02u; o->driver &= (uint16_t)~0x40u; }   /* the
                                                        stranded shape: legal, outside, nobody's pad, no autopilot */
            }
            if (getenv("WF_WOUT1") && i == atol(getenv("WF_WOUT1"))) {   /* harness poke: P1
                                                        stands OUTSIDE at ringside beside
                                                        the box weapon (weapon probe) */
                eng_obj *o = &st.obj[0];
                o->role |= RF_OUTSIDE; o->state = ST_STAND; o->partner = -1; o->mover = 0;
                o->x = 0x320 << 16; o->y = 0x120 << 16; o->z = 0x100 << 16;
            }
            if (getenv("WF_HOLD1") && i == atol(getenv("WF_HOLD1"))) {   /* harness poke: P1 holds
                                                        P2 in the facelock stance 0x15 (the
                                                        buckle-tag / double-team probe) */
                eng_obj *a = &st.obj[0], *v = &st.obj[2];
                v->x = a->x; v->y = a->y; v->z = a->z;
                a->partner = 2; v->partner = 0; a->hold_ph = 1;
                a->state = ST_MOVE; a->move_id = 0x15; a->grap44 = 0;   /* handler init hands
                                                        the victim to move 0x7B */
            }
            if (getenv("WF_DT1") && i == atol(getenv("WF_DT1"))) {   /* harness poke: the
                                                        hold-for-partner tag (0x1F760 window):
                                                        P1 at his corner, his partner waiting
                                                        at the post, P2 lying knocked down in
                                                        the corner zone; a P1 press then tags */
                eng_obj *a = &st.obj[0], *p = &st.obj[1], *v = &st.obj[2];
                a->x = 0x218 << 16; a->y = 0x193 << 16; a->z = 0x140 << 16;
                a->state = ST_STAND; a->partner = -1; a->mover = 0; if (!getenv("WF_DT1_NOOPP")) a->opp = 2;   /* NOOPP: let the retarget pick (real play) */
                p->apron = 1; p->sub = 0x82; p->state = ST_WALK;
                p->x = 0x1C8 << 16; p->y = 0x192 << 16; p->z = 0x140 << 16;
                v->x = 0x210 << 16; v->y = 0x190 << 16; v->z = 0x140 << 16;
                v->facing = 0; v->partner = -1; v->mover = 0;
                v->state = ST_REACT; v->react_id = RC_LYING; v->down_t = 0x200;
            }
            if (getenv("WF_HOLD2") && i == atol(getenv("WF_HOLD2"))) {   /* harness poke: P2 (CPU)
                                                        holds P1 in the facelock 0x15 */
                eng_obj *a2 = &st.obj[2], *v2 = &st.obj[0];
                v2->x = a2->x; v2->y = a2->y; v2->z = a2->z;
                a2->partner = 0; v2->partner = 2; a2->hold_ph = 1;
                a2->state = ST_MOVE; a2->move_id = 0x15; a2->grap44 = 0;
            }
            if (getenv("WF_DIZZY2") && i == atol(getenv("WF_DIZZY2"))) {   /* harness poke: P2 is
                                                        DIZZY (state 4 react 1) 0x30 px in
                                                        front of P1, facing P1's way — the
                                                        behind-grab cat 0x10 geometry */
                eng_obj *o = &st.obj[2], *p1 = &st.obj[0];
                o->x = p1->x + (((p1->facing & 0x8000u) ? 0x30 : -0x30) << 16); o->y = p1->y; o->z = 0x140 << 16;
                o->facing = p1->facing; o->partner = -1; o->mover = 0;
                o->state = ST_REACT; o->react_id = RC_DIZZY;
            }
            if (i == 0 && getenv("WF_W1"))            /* harness poke: P1 wrestler */
                {   /* a registered clone slot (12+) is taken as is - skins in a scripted match */
                    int w1 = atoi(getenv("WF_W1"));
                    st.obj[0].wrestler = w1 >= ENG_WS_MAX && w1 < ENG_WS_EXT_MAX && eng_ws_base(w1) >= 0 ? w1 : w1 % 12;
                }
            if (i == 0 && getenv("WF_W2")) {          /* harness poke: P2 wrestler (clone ids too) */
                int w2 = atoi(getenv("WF_W2"));
                st.obj[2].wrestler = w2 >= ENG_WS_MAX && w2 < ENG_WS_EXT_MAX && eng_ws_base(w2) >= 0 ? w2 : w2 % 12;
            }
            if (i == 0 && getenv("WF_DMG2"))         /* harness poke: pending
                                                        damage -> band via the drain */
                st.obj[2].dmg = (uint16_t)strtoul(getenv("WF_DMG2"), 0, 0);
            if (selftest) {
                /* Deterministic drive covering all directions, taps,
                 * reversals and idle for both players. */
                /* Directions, strikes, chords and mixed holds so the
                 * digest covers walk/run/skid/tie-up/hit/knockdown. */
                static const uint8_t script[16] = {
                    0x1, 0x1, 0x10, 0x2, 0x9, 0x30, 0x4, 0x0,
                    0x20, 0x6, 0xA, 0x10, 0x5, 0x30, 0x0, 0x10 };
                st.inputs[0] = script[(i / 60) % 16];
                st.inputs[1] = script[(i / 45 + 5) % 16];
            } else if (!strcmp(drive, "script") || !strcmp(drive, "gameselect") || !strcmp(drive, "charselect") || !strcmp(drive, "attract")) {
                if (i == 0 && getenv("WF_ALT1"))     /* harness poke: P1 throw bank */
                    st.obj[0].alt62 = 0x80;

                /* WF_P1..WF_P4 = "start-end:bits,start-end:bits,..."
                 * (frame ranges inclusive, bits in hex: 1 R 2 L 4 U 8 D
                 * 10 B1 20 B2 40 START 80 COIN). Unlisted frames are idle. */
                for (int p = 0; p < 4; p++) {
                    static const char *const pe[4] = { "WF_P1", "WF_P2", "WF_P3", "WF_P4" };
                    const char *sp = getenv(pe[p]);
                    unsigned bits = 0;
                    while (sp && *sp) {
                        int a, b; unsigned v; int nc = 0;
                        if (sscanf(sp, "%d-%d:%x%n", &a, &b, &v, &nc) == 3 && nc) {
                            if (i >= a && i <= b) bits |= v;
                            sp += nc;
                        } else break;
                        if (*sp == ',') sp++;
                    }
                    st.inputs[p] = bits;
                }
            } else if (!strcmp(drive, "ladder")) {
                /* Campaign E2E harness (docs/engine-specs/match-end.md).
                 * WF_LADDER=win (default) / lose picks who is stamped;
                 * WF_LADDER_AT = frames after a match goes live before the
                 * finishing KO of 0x111C8 is written by hand (result words
                 * + referee SM9), so the real win/lose poses, the 0x11B6
                 * banners and the 0x19BA ladder all run. P1 taps button 1
                 * on every front-end screen (char select pick, intro skip)
                 * and START on the continue screen. */
                static int64_t live_at; static int stamped; static int was_scene;
                int lose = getenv("WF_LADDER") && getenv("WF_LADDER")[0] == 'l';
                if (i == 0 && !eng_camp_armed()) {
                    /* bare headless run: arm the campaign the way the
                     * character select would (0x5DAC), default picks —
                     * without it eng_camp_stage() pins the ladder at 0 */
                    eng_camp_new_game(NULL, NULL);
                    eng_init_picks(&st, eng_camp_picks());
                }
                int hold = getenv("WF_LADDER_AT") ? atoi(getenv("WF_LADDER_AT")) : 90;
                int scene = eng_scene_active();
                if (scene || st.intro) { live_at = -1; stamped = 0; }
                else if (live_at < 0 || was_scene) { live_at = i; stamped = 0; }
                else if (stamped && st.obj[0].active && !st.obj[0].result && !st.obj[2].result) {
                    /* no-front re-init (campaign skipped the aisle/continue
                     * screens): the result words were wiped by the next
                     * eng_init_picks — re-arm the stamp */
                    live_at = i; stamped = 0;
                }
                was_scene = scene || st.intro;
                st.inputs[0] = st.inputs[1] = 0;
                if (eng_scene_get() == ENG_SCENE_CONTINUE
                    || eng_scene_get() == ENG_SCENE_ATTRACT)
                    st.inputs[0] = (i % 8) < 2 ? 0x40u : 0u;                      /* START */
                else if ((scene || st.intro) && eng_scene_get() != ENG_SCENE_CEREMONY)
                    st.inputs[0] = (i % 8) < 2 ? 0x10u : 0u;                      /* button 1 */
                if (!scene && !st.intro && !stamped && live_at >= 0 && i - live_at >= hold
                    && st.obj[0].active && st.obj[2].active && !st.obj[0].result) {
                    int w = lose ? 2 : 0;                       /* the winning pair's base */
                    for (int k = 0; k < 4; k++)
                        st.obj[k].result = (uint16_t)(((k >> 1) == (w >> 1)) ? 0x8000 : 0x8001);
                    st.ref.sm = 9; st.ref.win_t = 44;
                    stamped = 1;
                    fprintf(stderr, "ladder: stamp f%ld %s stage %u ids %d/%d vs %d/%d\n",
                            i, lose ? "LOSE" : "WIN", st.stage,
                            st.obj[0].wrestler, st.obj[1].wrestler,
                            st.obj[2].wrestler, st.obj[3].wrestler);
                }
            } else if (!strcmp(drive, "fuzz")) {
                /* seeded random play for both players (WF_SEED); holds a
                 * random input for 4..36 frames at a time */
                static uint32_t lcg; static int hold[2]; static uint32_t cur[2];
                if (i == 0) lcg = getenv("WF_SEED") ? (uint32_t)strtoul(getenv("WF_SEED"), 0, 0) : 12345u;
                for (int p = 0; p < 2; p++) {
                    if (hold[p] == 0) {
                        lcg = lcg * 1664525u + 1013904223u;
                        cur[p] = (lcg >> 24) & 0x3Fu;
                        if (((lcg >> 16) & 3u) == 0) cur[p] &= 0x0Fu;   /* some plain walking */
                        hold[p] = 4 + (int)((lcg >> 8) & 31u);
                    }
                    hold[p]--;
                    st.inputs[p] = cur[p];
                }
            } else if (!strcmp(drive, "rpin")) {
                /* rumble pin-break probe: at f200 slot 5 lies beside P1 and
                 * slot 4 covers him; from f330 P1 taps B1 (the stomp) to
                 * break the cover. Run with WF_RUMBLE=1 WF_CPU2=1. */
                st.inputs[0] = 0; st.inputs[1] = 0;
                if (i == 5 && st.obj[4].active && st.obj[5].active) {
                    int32_t px = st.obj[0].x >> 16, py = st.obj[0].y >> 16;
                    st.obj[5].state = ST_REACT; st.obj[5].react_id = RC_LYING; st.obj[5].down_t = 0x200;
                    st.obj[5].x = (px + 0x38) << 16; st.obj[5].y = py << 16; st.obj[5].facing = 0;
                    st.obj[5].partner = -1; st.obj[5].hp = 1;
                    st.obj[5].cpu = 0;     /* passive victim: no 0x1E452 auto kick-out */
                    st.obj[4].state = ST_MOVE; st.obj[4].move_id = 0x48; st.obj[4].partner = 5;
                    st.obj[4].x = st.obj[5].x; st.obj[4].y = st.obj[5].y; st.obj[4].opp = 5;
                    st.obj[4].tag_flags |= TF_PIN_INTENT;   /* pin intent: the retarget skips him, P1's B1 = the JAB */
                    if (getenv("WF_RPIN") && *getenv("WF_RPIN") == 's') {
                        /* SPLASH-PIN form (0x13E20 landing): the diver keeps his
                           own record in state 5, pinning with the referee cue -
                           the shape the corner splash leaves (user 2026-08-28) */
                        st.obj[4].move_id = 0x800Eu; st.obj[4].anim_sel |= 0x8000u; st.obj[4].grap44 = 3;
                        st.obj[4].frame = 4; st.obj[4].landed = 1; st.obj[4].mover = 0;
                        st.obj[4].pinning = 1; st.obj[4].cue_flags |= 1u; st.obj[4].atk = 0x801Du;
                        st.obj[4].role |= RF_ENGAGED; st.obj[5].role |= RF_ENGAGED; st.obj[5].partner = 4;
                        st.obj[5].state = ST_MOVE; st.obj[5].move_id = 0x51; st.obj[5].mash_aa = 0x100;
                    }
                    for (int k = 1; k < ENG_MAX_OBJS; k++) if (k != 4 && k != 5 && st.obj[k].active) {   /* park the rest */
                        st.obj[k].state = ST_REACT; st.obj[k].react_id = RC_LYING; st.obj[k].down_t = 0x7FFF;
                        st.obj[k].x = 0x1C0 << 16; st.obj[k].y = 0x1A0 << 16; st.obj[k].partner = -1;
                    }
                }
                if (i >= 100 && i < 170) st.inputs[0] = 0x1;   /* walk beside the pair */
                if (getenv("WF_JAB")) {                /* raw jab instead of the selector */
                    if (i == 220) { st.obj[0].state = ST_MOVE; st.obj[0].move_id = 0; }
                } else if (getenv("WF_RPIN") && *getenv("WF_RPIN") == 'j') {
                    if (i > 180 && (i % 20) <= 1) st.inputs[0] = 0x20;   /* B2: JOIN the cover */
                } else if (i > 180 && (i % 20) <= 1) st.inputs[0] = 0x10;
            } else if (!strcmp(drive, "right")) {
                st.inputs[0] = 0x1;
                st.inputs[1] = 0x1;
            } else if (!strcmp(drive, "pin")) {
                /* demo knockdown, then inject the cover directly — a
                 * harness poke to E2E the 0x48 cover -> run-in -> count
                 * path (docs/engine-specs/pin-partner.md).
                 * WF_PIN=three (default) the victim has no energy left and
                 *   eats the 3; =kick he is topped up and mashes out;
                 *   =stomp nobody mashes, the run-in rescuer breaks it. */
                const char *pinmode = getenv("WF_PIN");
                char pm = pinmode ? pinmode[0] : 't';
                if (i < 32) st.inputs[0] = 0x1;
                else if (i < 240 && (i % 45) < 2 && i >= 40) st.inputs[0] = 0x10;
                else st.inputs[0] = 0;
                st.inputs[1] = 0;
                if (i == 299 && pm == 'k') st.obj[2].hp = st.obj[2].hp_max;
                if (getenv("WF_PIN_MOVE") && i >= 300 && (i - 300) % 160 == 0 && i < 300 + 160 * 8
                    && (st.obj[2].state & 0xFF) == 4) {      /* harness: repeat a downed-man move (e.g. 0x35) */
                    st.obj[0].state = ST_MOVE; st.obj[0].move_id = (uint16_t)strtoul(getenv("WF_PIN_MOVE"), 0, 0);
                    st.obj[0].partner = 2; st.obj[0].grap44 = 0; st.obj[0].frame = 0; st.obj[0].anim_sel = 0;
                    st.obj[0].x = st.obj[2].x - (0x30 << 16); st.obj[0].y = st.obj[2].y; st.obj[0].opp = 2;
                } else
                if (i == 300 && (st.obj[2].state & 0xFF) == 4
                    && (st.obj[2].react_id & 0xFF) == 8) {
                    st.obj[0].state = ST_MOVE;
                    st.obj[0].move_id = 0x48;
                    st.obj[0].partner = 2;
                }
                if (pm == 'k' && i > 340) st.inputs[1] = ((i >> 2) & 1) ? 0x10 : 0;
                if (getenv("WF_ROPESTOP")) {     /* harness poke: WF_ROPESTOP=F - at frame F o2 runs INTO the
                                          left ropes and arrests himself (move 0x3F, the rope stop) */
                    long F = atol(getenv("WF_ROPESTOP"));
                    if (i == F) {
                        eng_obj *r = &st.obj[2];
                        r->state = ST_MOVE; r->move_id = 0x3F; r->grap44 = 0; r->frame = 0; r->anim_sel = 0; r->partner = -1;
                        r->x = 0x1E0 << 16; r->y = 0x150 << 16; r->z = 0x140 << 16;
                        r->facing = getenv("WF_ROPESTOP_R") ? 0x8000u : 0; r->angle = 0xC0; r->role |= RF_RUNNING; r->x = 0x1D9 << 16;
                    }
                }
                if (getenv("WF_TAGTEST")) {      /* harness poke: WF_TAGTEST=F - at frame F o0 (pad) stands in
                                          his corner zone with o1 latched at the post; B1 at F+2 = the tag */
                    long F = atol(getenv("WF_TAGTEST"));
                    if (i == F) {
                        eng_obj *a = &st.obj[0], *b = &st.obj[1];
                        a->state = ST_STAND; a->move_id = 0; a->partner = -1; a->grap44 = 0; a->x = 0x200 << 16; a->y = 0x190 << 16; a->z = 0x140 << 16;
                        b->apron = 1; b->sub = 0x82; b->state = ST_WALK; b->move_id = 0; b->partner = -1; b->driver |= DRV_AUTOPILOT; b->st_flags |= SF_APRON;
                        b->x = 0x1C0 << 16; b->y = 0x182 << 16; b->z = 0x140 << 16;
                    }
                    if (i == F + 2) st.inputs[0] = 0x10;
                }
                if (getenv("WF_DT")) {           /* harness poke: WF_DT=F - at frame F the partner
                                          (o1) holds the downed o2 for the double team (0x37),
                                          o0 (pad) stands beside; B1 at F+30 = the stomp */
                    long F = atol(getenv("WF_DT"));
                    if (i == F) {
                        eng_obj *h = &st.obj[1], *v = &st.obj[2];
                        h->apron = 0; h->sub = 0; h->st_flags &= (uint16_t)~0x01u; h->driver |= DRV_AUTOPILOT;
                        v->state = ST_REACT; v->react_id = RC_LYING; v->down_t = 0x200; v->move_id = 0; v->partner = -1; v->grap44 = 0;
                        h->opp = 2; h->partner = 2; h->state = ST_MOVE; h->move_id = 0x37; h->grap44 = 0; h->frame = 0; h->anim_sel = 0;
                        {   /* o3 (the held man's partner) waits on his apron */
                            eng_obj *q = &st.obj[3];
                            q->apron = 1; q->sub = 1; q->state = ST_WALK; q->move_id = 0; q->partner = -1; q->grap44 = 0;
                            q->driver |= DRV_AUTOPILOT; q->tag_flags = 0; q->ai_e6 = 0; q->st_flags |= SF_APRON;
                            q->x = 0x40E << 16; q->y = 0x117 << 16; q->z = 0x140 << 16;
                        }
                        st.obj[0].state = ST_STAND; st.obj[0].move_id = 0; st.obj[0].partner = -1; st.obj[0].opp = 2;
                        st.obj[0].x = v->x - (0x18 << 16); st.obj[0].y = v->y; st.obj[0].facing = 0x8000u;
                    }
                    if (i == F + 44) {               /* stand on his FRONT side (prox box 3 mirrors by his facing) */
                        st.obj[0].x = st.obj[2].x + (((st.obj[2].facing & 0x8000u) ? -0x20 : 0x20) << 16); st.obj[0].y = st.obj[2].y;
                        st.obj[0].facing = (st.obj[0].x < st.obj[2].x) ? 0x8000u : 0;
                    }
                    if (i == F + 45 || i == F + 46) st.inputs[0] = 0x10;   /* after his 0x4E entry */
                }
                if (getenv("WF_PIN_RUN")) {      /* harness poke: WF_PIN_RUN=F - at frame F the
                                          pinner's PARTNER (o1, holding the pad after the
                                          0x215B6 hand-over) is dropped into the ring
                                          running at the pile from the left, and presses
                                          B1 six frames later = a running strike INTO his
                                          own pinner (user 2026-08-30 friendly-fire bug) */
                    long F = atol(getenv("WF_PIN_RUN"));
                    if (i == F) {
                        eng_obj *r = &st.obj[1];
                        r->apron = 0; r->sub = 0; r->st_flags &= (uint16_t)~0x01u;
                        r->x = st.obj[0].x - (0x90 << 16); r->y = st.obj[0].y; r->z = 0;
                        r->facing = 0x8000u; r->angle = 0; r->state = ST_RUN; r->anim_sel = 0; r->frame = 0;
                        r->move_id = 0; r->grap44 = 0; r->partner = -1; r->mover = 1; r->speed = 0x20;
                    }
                    if (getenv("WF_PIN_RUNMV")) {      /* ...or fire a given running move outright */
                        if (i == F + 6) { st.obj[1].state = ST_MOVE; st.obj[1].move_id = (uint16_t)strtoul(getenv("WF_PIN_RUNMV"), 0, 0); st.obj[1].grap44 = 0; st.obj[1].frame = 0; st.obj[1].anim_sel = 2; }
                    } else if (i == F + 6 || i == F + 7) st.inputs[0] = 0x10;
                }
                if (pm == 'p' && i > 380) st.inputs[0] = 0x1;   /* P1 walks RIGHT:
                                          it must be the PARTNER who moves */
                if (pm == 'a') {
                    /* WF_PIN=again[:frame2] — the USER-SPEC two-pin cycle:
                     * cover f300 (victim topped up, mashes out), both
                     * partners run in, brawl, then a SECOND cover at
                     * frame2 (default 600; put it after the usher escort
                     * to test the re-arm of pointed-out / walked-out
                     * partners). The second victim also kicks out so the
                     * grace expiry + escort can be watched to the end. */
                    long f2 = 600;
                    const char *c2 = strchr(pinmode, ':');
                    if (c2) f2 = atol(c2 + 1);
                    if (i == 299) st.obj[2].hp = st.obj[2].hp_max;
                    if (i > 340 && i < 430) st.inputs[1] = ((i >> 2) & 1) ? 0x10 : 0;
                    if (i == f2 - 2) {         /* park the victim down, pinner beside him */
                        st.obj[2].state = ST_REACT; st.obj[2].react_id = RC_LYING; st.obj[2].down_t = 0x60;
                        st.obj[2].move_id = 0; st.obj[2].partner = -1; st.obj[2].grap44 = 0;
                        st.obj[2].count = 0; st.obj[2].hp = st.obj[2].hp_max;
                        st.obj[0].state = ST_STAND; st.obj[0].move_id = 0; st.obj[0].partner = -1;
                        st.obj[0].grap44 = 0; st.obj[0].count = 0; st.obj[0].pinning = 0;
                        st.obj[0].x = st.obj[2].x - (0x20 << 16); st.obj[0].y = st.obj[2].y;
                        for (int k = 0; k < ENG_MAX_OBJS; k++)   /* nobody keeps a stale link */
                            if (k != 0 && st.obj[k].active && st.obj[k].partner == 2)
                                st.obj[k].partner = -1;
                    }
                    if (i == f2) {
                        st.obj[0].state = ST_MOVE; st.obj[0].move_id = 0x48;
                        st.obj[0].partner = 2; st.obj[0].frame = 0; st.obj[0].anim_sel = 0;
                    }
                    if (i > f2 + 40 && i < f2 + 130) st.inputs[1] = ((i >> 2) & 1) ? 0x10 : 0;
                }
                if (pm != 's' && pm != 'a' && !getenv("WF_DT") && i > 301) {   /* harness poke: hold the OTHER
                                          team's rescuer on the apron so the
                                          3-count / kick-out is the ender.
                                          WF_PIN=stomp lets him come in. */
                    st.obj[3].tag_flags &= (uint16_t)~0x01u;
                    st.obj[3].ai_e6 = 0;
                }
            } else if (!strcmp(drive, "throw")) {
                if (i < 110) { st.inputs[0] = 0x1; st.inputs[1] = 0x1; }
                else if (i == 180 || i == 181 || i == 230 || i == 231) {
                    st.inputs[0] = 0x10; st.inputs[1] = 0x10;
                } else { st.inputs[0] = 0; st.inputs[1] = 0; }
            } else if (!strcmp(drive, "solorun")) {
                st.inputs[0] = (i < 4) ? 0x30 : 0x1;   /* run right into ropes */
                st.inputs[1] = (i < 40) ? 0x8 : 0;     /* P2 steps out of band */
            } else {
                /* demo: jab P2 down, walk to the body, cover, count. */
                if (i < 32) st.inputs[0] = 0x1;
                else if (i < 240 && (i % 45) < 2 && i >= 40) st.inputs[0] = 0x10;
                else if (i >= 260 && i < 296) st.inputs[0] = getenv("WF_WALK") ? 0x1 : 0;
                else if (i == 300 || i == 301) st.inputs[0] = (getenv("WF_PICKUP") ? 0x10 : 0x20);   /* B2 cover / B1 pickup */
                else st.inputs[0] = 0;
                st.inputs[1] = 0;
            }
            if (st.intro && !getenv("WF_INTRO"))
                st.inputs[0] = st.inputs[1] = st.inputs[2] = st.inputs[3] = 0;   /* drives target the live match */
            eng_update(&st);
            if (render_all) { static uint32_t rbuf[512 * 384]; eng_render_frame(&st, rbuf, 0); }   /* WF_RENDER_ALL: exercise the renderer headless (coverage) */
            if (!strcmp(drive, "fuzz")) {
                static uint32_t sig_last; static int same;
                uint32_t sig = 0;
                for (int k = 0; k < 4; k++)
                    sig = sig * 31u + (uint32_t)(st.obj[k].state & 0xFFu) * 7u + (uint32_t)(st.obj[k].move_id & 0xFFu) * 131u
                        + (uint32_t)(st.obj[k].x >> 16) * 17u + (uint32_t)(st.obj[k].react_id & 0xFFu) * 5u;
                if (sig == sig_last) { if (++same == 900) fprintf(stderr, "STUCK? f%ld P1 st=%04X mv=%02X rc=%02X  P2 st=%04X mv=%02X rc=%02X\n", i,
                        st.obj[0].state, st.obj[0].move_id & 0xFF, st.obj[0].react_id & 0xFF, st.obj[2].state, st.obj[2].move_id & 0xFF, st.obj[2].react_id & 0xFF); }
                else same = 0;
                sig_last = sig;
            }
            trace_frame(&st, i);
            if (getenv("WF_SHOT_EVERY") && i % atol(getenv("WF_SHOT_EVERY")) == 0) {   /* docs media: a PPM every N frames into WF_SHOT_DIR */
                char sp[512]; snprintf(sp, sizeof sp, "%s/f%06ld.ppm", getenv("WF_SHOT_DIR") ? getenv("WF_SHOT_DIR") : ".", i);
                eng_render_shot(&st, sp);
            }
            if (getenv("WF_CAMX")) st.cam_x = (int32_t)strtol(getenv("WF_CAMX"), 0, 0);  /* debug freeze */
            if (getenv("WF_CAMY")) st.cam_y = (int32_t)strtol(getenv("WF_CAMY"), 0, 0);
            if (selftest) {
                const uint8_t *b = (const uint8_t *)&st;
                for (size_t k = 0; k < sizeof st; k++) {
                    h ^= b[k];
                    h *= 1099511628211ull;
                }
            }
        }
        if (selftest) {
            printf("selftest: %ld frames digest %016llx\n",
                   n, (unsigned long long)h);
            { static uint32_t rbuf2[512 * 384]; eng_render_frame(&st, rbuf2, 0); }
            return 0;
        }
        fprintf(stderr, "dbg: clock %02X:%02X div=%d sig=%02X\n", st.clk_min, st.clk_sec, st.clk_div, st.sig169e);
        fprintf(stderr, "dbg: cam %X,%X\n", st.cam_x, st.cam_y);
        fprintf(stderr, "dbg: ref sm=%d cell=%d spr=%02X xy=(%X,%X) tgt=%d\n",
                st.ref.sm, st.ref.cell, st.ref.spr,
                st.ref.x >> 16, st.ref.y >> 16, st.ref.target);
        for (int k = 0; k < 4; k++)
            fprintf(stderr, "dbg: obj%d ap=%d list=%d ", k, st.obj[k].apron, st.obj[k].list), fprintf(stderr, "dbg: obj%d xyz=(%X,%X,%X) vz=%d Ld=%d spr=%04X state=%04X rc=%02X atk=%02X hp=%d/%d cnt=%04X frm=%02X sel=%04X zone=%d clip=%02X mv=%d f42=%d f33=%04X f32=%04X vz2=%d\n",
                    k, st.obj[k].x >> 16, st.obj[k].y >> 16, st.obj[k].z >> 16,
                    st.obj[k].vz, st.obj[k].landed, st.obj[k].spr, st.obj[k].state, st.obj[k].react_id & 0xFF, st.obj[k].atk,
                    st.obj[k].hp, st.obj[k].hp_max, st.obj[k].count, st.obj[k].frame, st.obj[k].anim_sel, st.obj[k].zone, st.obj[k].clip, st.obj[k].mover, st.obj[k].floor42, st.obj[k].role, st.obj[k].st_flags, st.obj[k].vz), fprintf(stderr, "  obj%d ap=%d list=%d\n", k, st.obj[k].apron, st.obj[k].list);
        if (shot && eng_render_shot(&st, shot) != 0)
            return 1;
        if (export_scene) {
            extern int tool_export_scene(const eng_state *, const char *, const char *);
            return tool_export_scene(&st, export_scene, export_scene_dir);
        }
        if (export_arena_dir) {
            extern int tool_export_arena(const eng_state *, const char *);
            if (st.scene != export_arena_scene)
                fprintf(stderr, "export-arena: the engine is in scene %d, not %d (no stage maps there yet)\n", st.scene, export_arena_scene);
            return tool_export_arena(&st, export_arena_dir);
        }
        wf_video_sprite_log_dump();       /* WF_SPRITE_LOG, no-op if unset */
        if (getenv("WF_DUMPPAL")) {       /* debug: sprite palette bank N */
            unsigned b = (unsigned)atoi(getenv("WF_DUMPPAL"));
            extern unsigned int m68k_read_memory_16(unsigned int);
            fprintf(stderr, "palbank %u:", b);
            for (int p = 0; p < 16; p++)
                fprintf(stderr, " %04X", m68k_read_memory_16(0x182000u + b * 0x80u + (unsigned)p * 2u));
            fprintf(stderr, "\n");
        }
        if (getenv("WF_AISTATS")) { extern void eng_ai_stats_dump(const eng_state *); eng_ai_stats_dump(&st); }
        printf("engine: %ld frames, frame counter %lld\n", n, (long long)st.frame);
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    /* --editor is EDITOR-ONLY: no game window, no game audio — the
     * Sounds panel lazily opens the device for auditions (user 2026-08-24:
     * "background music plays when we open editor without the game"). */
    int editor_only = editor && !getenv("WF_EDITOR_PLAY") && !editor_play;
    sound_on = !editor_only && audio_init("data/sounds", 0) == 0;   /* bank%d_%03d.wav live here */
    keymap_load();                        /* data/keymap.json + mods over defaults */
    if (!editor_only) {
        /* Match music: stage_sound (ROM 0xDEE) command for stage 0,
         * posted the way 0xC7A does at scene start. Stock posts through
         * 0x2052, which drops the command while $1C007C == 0 (no game
         * yet) and the demo-sounds dip is off (0x205E) — i.e. only the
         * attract front end is muted, a --no-front match is not. */
        if (tbl_bytes(TBL(stage_bgm), NULL) && !(front && (eng_dip_word() & 0x20u)))
            eng_sound(tbl16(TBL(stage_bgm), 0) & 0xFFu);
    }
    char title[64];
    snprintf(title, sizeof title, "WrestleFest native V%s%s%s", WF_VERSION_STRING,
             wf_profile()[0] ? " - " : "", wf_profile()[0] ? wf_profile() : "");
    /* --editor is EDITOR-ONLY (user 2026-08-24): games are launched from
     * the Profiles panel's Launch buttons as separate processes. --editor
     * --play keeps the old side-by-side game window. */
    SDL_Window *win = editor_only ? NULL : SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        320 * scale, 240 * scale, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, 0);   /* software (Xvfb / VMs) */
    if (ren) SDL_RenderSetLogicalSize(ren, 320, 240);   /* 4:3 letterbox in any window */
    {   /* MAME's default -filter smooths the scaled frame (user 2026-08-29: "stock
         * has some smoothing ours doesn't"): game_rules video_smooth (1 = bilinear,
         * default) / WF_SMOOTH=0|1 override. Must be set before the texture exists. */
        const char *e = getenv("WF_SMOOTH");
        int smooth = e ? atoi(e) != 0 : eng_mod_rule(MODR_VIDEO_SMOOTH) != 0;
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, smooth ? "linear" : "nearest");
    }
    if (win) wf_window_dress(win);     /* icon + foreground */
    if (win && (editor_play || play_scene)) SDL_RaiseWindow(win);   /* Launch / Play scene from the editor: the game window on top */
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, 512, 384) : NULL;   /* max zoomed view */
    static uint32_t pixels[512 * 384];
    if (editor || getenv("WF_EDITOR")) {
        void ed_set_game_live(int on);
        ed_open();
        ed_set_game_live(win != NULL);
    }

    int64_t next = now_ns();
    int running = 1;
    while (running && (frames < 0 || st.frame < frames)) {
        SDL_Event ev;
        static int esc_hold;
        if (win && editor_play && st.frame == 30) {   /* Launch from the editor: the editor's
                                          own frame-22 raise (ed_frame) put ITS window on
                                          top of the game's - raise the game after it
                                          (user 2026-08-29: "the game sits BEHIND wfeditor") */
            SDL_RaiseWindow(win);
            SDL_SetWindowInputFocus(win);
        }
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = 0;
            if (win && ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE
                && ev.window.windowID == SDL_GetWindowID(win)) {
                if (editor && editor_play) {   /* closing the game window from a Launch: back to the editor */
                    unsetenv("WF_PAKDIR"); unsetenv("WF_GFXPAK"); unsetenv("WF_PAK");
                    if (profile) execl("./wfengine", "wfengine", "--editor", "--profile", profile, (char *)NULL);
                    else execl("./wfengine", "wfengine", "--editor", (char *)NULL);
                }
                running = 0;
            }
            ed_handle_event(&ev);
        }
        if (editor_only && !ed_is_open()) running = 0;   /* editor closed = quit */
        if (win && st.frame == 10) { SDL_RaiseWindow(win); SDL_SetWindowInputFocus(win); }   /* the WM often opens the
                                          game window BEHIND (user: "click it on the taskbar") — raise once the
                                          window exists for real */
        {
            const Uint8 *k = SDL_GetKeyboardState(NULL);
            static int f11_was;
            if (win && k[SDL_SCANCODE_ESCAPE] && !ed_wants_keyboard()) {
                if (editor && editor_play) {   /* launched from the editor: Esc RETURNS to it
                                                  (user 2026-08-27) - same exec dance as Launch */
                    unsetenv("WF_PAKDIR"); unsetenv("WF_GFXPAK"); unsetenv("WF_PAK");
                    if (profile) execl("./wfengine", "wfengine", "--editor", "--profile", profile, (char *)NULL);
                    else execl("./wfengine", "wfengine", "--editor", (char *)NULL);
                }
                running = 0;                   /* Esc quits (game window) */
            }
            if (win && k[SDL_SCANCODE_F11] && !f11_was) {       /* F11: fullscreen toggle */
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            f11_was = k[SDL_SCANCODE_F11];
            {   /* F9: cycle P1's wrestler through his ALTERNATE MOVE GRIDS (the
                 * generic's A/B/C... - user 2026-08-26: "for testing, just a grid
                 * swap"); no-op for a package without "mgrids" */
                static int f9_was;
                if (win && k[SDL_SCANCODE_F9] && !f9_was && st.obj[0].active) {
                    unsigned w1 = (unsigned)st.obj[0].wrestler; int n = eng_ws_grid_count(w1);
                    if (n > 0) {
                        int g = (eng_ws_grid_get(w1) + 1) % (n + 1);
                        eng_ws_grid_set(w1, g);
                        fprintf(stderr, "grid: wrestler %u -> %s (%d of %d alternates)\n", w1, g ? "grid" : "move_map", g, n);
                    }
                }
                f9_was = k[SDL_SCANCODE_F9];
            }
        }
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        /* keymap.c (data/keymap.json): bit 0 R, 1 L, 2 U, 3 D, 4 B1,
         * 5 B2, 6 START, 7 COIN. Defaults = the old hardcoded MAME-style
         * keys (P1 arrows/A/Z/1/5, P2 G-D-R-F/Q/W/2/6). */
        for (int p = 0; p < KM_PLAYERS; p++) {         /* 4 ports (mid-game buy-in seats) */
            uint32_t bits = 0;
            for (int b = 0; b < 8; b++)                /* the 8 cabinet lines */
                if (keys[keymap_sc(p, b)]) bits |= 1u << b;
            if (keymap_sc(p, KM_RUN) != SDL_SCANCODE_UNKNOWN
                && keys[keymap_sc(p, KM_RUN)])
                bits |= 0x30u;                         /* dedicated RUN = B1+B2 held */
            st.inputs[p] = bits;
        }

        if (ed_wants_keyboard())                       /* typing in the editor */
            for (int p = 0; p < KM_PLAYERS; p++) st.inputs[p] = 0;
        if (win) {
            /* frame history for the editor's < step: a ring of engine states
               (the engine is deterministic and self-contained in `st`) */
            static eng_state *hist; static int hist_n, hist_head;
            enum { HIST = 120 };
            if (!hist) hist = calloc(HIST, sizeof *hist);
            if (ed_consume_back()) {
                if (hist && hist_n > 0) {
                    hist_head = (hist_head + HIST - 1) % HIST; hist_n--;
                    st = hist[hist_head];
                }
            } else if (!ed_game_paused() || ed_consume_step()) {
                if (hist) { hist[hist_head] = st; hist_head = (hist_head + 1) % HIST; if (hist_n < HIST) hist_n++; }
                eng_update(&st);
                trace_frame(&st, (long)st.frame);
            }
            eng_render_frame(&st, pixels, 0);   /* renderer owns the pitch (zoom) */
            if (eng_mod_rule(MODR_CAM_FIXED)) {   /* diagnostic: the state rides
                                          the title so a screenshot documents it */
                static int lt = -1;
                int sig = (st.scene << 20) ^ (st.cam_x << 10) ^ st.cam_y ^ (wf_view_w() << 1);
                if (sig != lt) {
                    char t2[96];
                    lt = sig;
                    snprintf(t2, sizeof t2, "%s  [sc%d cam %d,%d view %dx%d]",
                             title, st.scene, st.cam_x, st.cam_y, wf_view_w(), wf_view_h());
                    SDL_SetWindowTitle(win, t2);
                }
            }
            {
                SDL_Rect src = { 0, 0, wf_view_w(), wf_view_h() };
                SDL_UpdateTexture(tex, &src, pixels, wf_view_w() * 4);
                {   /* bilinear scaling samples half a texel PAST the source rect: the
                     * texture is 512x384 for the zoomed view, so a 320x240 frame bled
                     * stale zoomed pixels along its right/bottom edges (user 2026-08-29
                     * "strip of sprites"). Pad the rect with a copy of its last
                     * column and row so the filter sees the edge colour. */
                    static uint32_t edge[512 + 1];
                    int w = wf_view_w(), h = wf_view_h();
                    if (w < 512) {
                        SDL_Rect col = { w, 0, 1, h };
                        for (int y = 0; y < h; y++) edge[y] = pixels[(size_t)y * w + w - 1];
                        SDL_UpdateTexture(tex, &col, edge, 4);
                    }
                    if (h < 384) {
                        SDL_Rect row = { 0, h, w < 512 ? w + 1 : w, 1 };
                        memcpy(edge, pixels + (size_t)(h - 1) * w, (size_t)w * 4);
                        edge[w] = pixels[(size_t)h * w - 1];
                        SDL_UpdateTexture(tex, &row, edge, row.w * 4);
                    }
                }
                SDL_RenderSetLogicalSize(ren, wf_view_w(), wf_view_h());
                SDL_RenderClear(ren);
                /* Flip-screen dip (0x2006: btst #6, $1C0067 -> $10000B bit 0):
                 * the cocktail 180-degree rotation, done at the blit here. */
                if (eng_dip_word() & 0x40u)
                    SDL_RenderCopyEx(ren, tex, &src, NULL, 180.0, NULL, SDL_FLIP_NONE);
                else
                    SDL_RenderCopy(ren, tex, &src, NULL);
                SDL_RenderPresent(ren);
            }
        }
        ed_frame(&st);

        next += ENG_FRAME_NS;
        int64_t wait = next - now_ns();
        if (wait > 0) {
            struct timespec ts = { wait / 1000000000, wait % 1000000000 };
            nanosleep(&ts, NULL);
        } else if (wait < -ENG_FRAME_NS * 4) {
            next = now_ns();           /* fell behind; resync, don't spiral */
        }
    }

    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    {   /* the editor's tool children (codex) die with it — no invisible
           request-burning zombies (user 2026-08-25) */
        extern void wf_editor_kill_tools(void);
        wf_editor_kill_tools();
    }
    SDL_Quit();
    return 0;
}
