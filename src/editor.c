/* wfeditor — see editor.h. Nuklear UI over an SDL2 renderer in a second
 * window. Everything here works on the same in-process data the game
 * uses: the table registry (tbl.h), the wrestler packages (package.c), the
 * live eng_state. Files are read/written with json.c; the C tools run as
 * subprocesses of this binary. */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#include "editor_nk.h"
#include "editor.h"
#include "engine.h"
#include "tbl.h"
#include "json.h"
#include "wf.h"
#include "credit.h"
#include "keymap.h"
#include "profile.h"
#include "version.h"

extern int eng_dbgsel;
const uint8_t *wf_video_tile_pens(unsigned t);

/* ---------------------------------------------------------------- state */
static SDL_Window *win;
static SDL_Renderer *ren;
static struct nk_context *ctx;
static int opened, focused, paused, step_once, step_back;

enum { NAV_MODS, NAV_TABLES, NAV_RULES, NAV_INPUT, NAV_WRESTLERS, NAV_FORGE, NAV_SKINS, NAV_CLASSES, NAV_WEAPONS, NAV_CALIB, NAV_SOUNDS, NAV_ENGINE, NAV_TOOLS, NAV_ARENAS, NAV_SCENES, NAV_N };
static const char *nav_names[NAV_N] = { "Profiles", "Tables", "Rules", "Input", "Wrestlers", "Forge", "Skins", "Classes", "Weapons", "Calibrate", "Sounds", "Match", "Tools", "Arenas", "Scenes" };
static const char *nav_help[NAV_N] = {
    "Launch profiles (named versions of the game): create, set rules, launch. Stock is locked.",
    "Every ROM data table the engine reads (moves, AI, damage, positions...): edit cells, save JSON.",
    "Engine rule scalars (hold clocks, ring-out counts, tag timers) with labels and explanations.",
    "Keyboard bindings for both players: click a key button, press the new key (Esc cancels); Save writes data/keymap.json or the active mod layer.",
    "Per-wrestler package: stats, body palette, pose browser.",
    "BUILD A WRESTLER: base + name + stats + move map + palette -> a clone (slot 12-43) written into a mod and packed into the profile.",
    "AI ART SKINS: describe a character, pick a base, approve an anchor, run the codex batch - a reusable art set the Forge can dress a wrestler in.",
    "BODY CLASSES: the five generic wrestlers (every pose from the in-class stock owners), their needs lists and the alias-group review that decides what a new skin must draw.",
    "WEAPONS: the ring steps / box table the game draws - spawn slots, per-type palette and rules; a new weapon type is a table entry.",
    "CALIBRATE contact: pause the live match on a hold or strike, drag the holder's body or the held man's cells until they meet, save into the skin's package. Mirrors with facing.",
    "Every sound command on the emulated board: play tunes/SFX, render a tune to a WAV in the profile's mod to edit it.",
    "The LIVE match: pause/step, and every object's decoded state with editable hp/position.",
    "Run the data pipeline tools (pack, export, verify, regress) and read their output.",
    "ARENAS: the ring as layered PNG art - crowd frames, the ring, the rope lines - per view; new arenas are copies of a stock one, packed, assigned under Rules > Arenas.",
    "SCENES: the non-match screens (walkout, LOD talk, title card, belt count, ending, credits) rendered headless and exported as frames: the tilemap in the arena format + every sprite pose as PNG + scene.json with the ROM addresses.",
};
static int nav = NAV_MODS;
static char save_msg[128]; static Uint32 save_msg_t;
static int nav_env_applied;

/* table editor */
static int cur_tbl = -1;
static uint8_t *edit_buf; static uint32_t edit_len; static int edit_dirty;
static int page, hex_view;
static char filter[64];
static int group_open[64];      /* per group collapsed state (by index of first appearance) */

/* wrestler panel */
static int cur_ws, cur_pose, pose_flip, pose_partner = -1, cur_pen;
static int fg_reload_slot = -1;   /* slot awaiting a package re-read after the async pack */
/* DELETE CONFIRM dialog (user 2026-08-27: a popup, not a double right-click):
   the context menus set what to delete, the sheet after the main window asks */
static int cf_kind;                 /* 0 none, 1 wrestler slot, 2 skin, 3 profile, 4 arena, 5 scene */
static int cf_id; static char cf_name[80];
static void skin_delete(const char *nm);
static void profile_delete(int i);
/* PROFILE SWITCH is synchronous (paks reload): request it, draw the locking
   sheet for one frame, then do it (user 2026-08-27: "looks like it froze") */
void wf_window_dress(void *win);   /* main.c: taskbar icon + raise (SDL_Window*) */
static char pend_prof[64]; static int pend_prof_on, pend_prof_frames;
static int pend_fg_on, pend_fg_frames;   /* deferred Save wrestler: draw the sheet FIRST, work next frame
                                            (the skin staging + ingest are synchronous and slow, 2026-08-27) */
static void request_edit_profile(const char *name) { snprintf(pend_prof, sizeof pend_prof, "%s", name ? name : ""); pend_prof_on = 1; pend_prof_frames = 0; }
static SDL_Texture *pose_tex; static int pose_tex_valid;
static int ws_hp, ws_walk, ws_run, ws_loaded = -1;

/* input panel (keymap.c): the armed row waits for the next key press */
static int rebind_p = -1, rebind_b = -1;
static int keymap_dirty;

/* mods */
static char mod_layer[64] = "";       /* "" = base tree */
static char new_mod[64];
static char mod_list[16][64]; static char mod_desc[16][96]; static int n_mods;

/* tools log */
static char logbuf[64][160]; static int log_n, log_head;
static SDL_mutex *log_mx;
static SDL_Thread *tool_thread; static volatile int tool_busy;
static char tool_cmd[4608];           /* >= the 4096-byte re-dress chain + ' 2>&1' (a 512 cap
                                          truncated it mid-quote: sh 'Unterminated quoted string',
                                          exit 2 on Save + pack - user 2026-08-28) */

static void logline(const char *s)
{
    SDL_LockMutex(log_mx);
    snprintf(logbuf[log_head], sizeof logbuf[0], "%s", s);
    log_head = (log_head + 1) % 64; if (log_n < 64) log_n++;
    SDL_UnlockMutex(log_mx);
}
static void logf_(const char *fmt, ...)
{
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); logline(b);
}

/* ------------------------------------------------- naming / help text */
/* Roster (docs/engine-specs/scene-charselect.md: grid ids 0 7 2 8 6 /
 * 9 1 3 A B = Hogan, Perfect, Jake, Earthquake, Slaughter / DiBiase,
 * Warrior, Boss Man, Smash, Crush; 4/5 = the Legion of Doom, CPU-only). */
static const char *ws_names[12] = {
    "Hulk Hogan", "Ultimate Warrior", "Jake Roberts", "Big Boss Man",
    "L.O.D. Hawk", "L.O.D. Animal", "Sgt. Slaughter", "Mr. Perfect",
    "Earthquake", "Ted DiBiase", "Demolition Smash", "Demolition Crush" };
static const char *ws_name(int id)
{
    if (id >= 0 && id < 12) return ws_names[id];
    if (id >= 12 && id < ENG_WS_EXT_MAX) {          /* registered clone slots */
        const char *n = eng_ws_clone_name(id);
        if (n && n[0]) return n;
    }
    return "?";
}

static const char *state_name(unsigned s)
{
    switch (s & 0xFFu) {
    case 0x00: return "STAND";     case 0x01: return "WALK";
    case 0x02: return "RUN";       case 0x03: return "SKID";
    case 0x04: return "REACTION";  case 0x05: return "MOVE";
    case 0x06: return "TURN";      case 0x07: return "GET UP";
    case 0x08: return "CORNER";    case 0x09: return "CLIMB";
    case 0x0A: return "PERCH";     case 0x0B: return "TIE-UP";
    case 0x0C: return "HOLD";      case 0xFF: return "HELD/frozen";
    default:   return "?";
    }
}

/* tooltip on the LAST widget: capture nk_widget_bounds() BEFORE it */
static int prof_popup_open(void);   /* below: any modal popup up? */
static void hint_at(struct nk_rect b, const char *txt)
{
    /* a tooltip IS a popup, and Nuklear asserts on a popup inside a popup
       (mod edit + a hovered row hint crashed the editor, 2026-08-25) */
    if (txt && !prof_popup_open() && nk_input_is_mouse_hovering_rect(&ctx->input, b))
        nk_tooltip(ctx, txt);
}
#define WB() nk_widget_bounds(ctx)

/* tab widget (nuklear issue #828 style, user 2026-08-28: tabs must not look
   like action buttons).  Flat, square, flush; the ACTIVE tab is lit with the
   amber underline, inactive tabs sit dark and dimmed.  Returns 1 when an
   INACTIVE tab is clicked (i.e. the selection should change). */
/* a tab's width = its text + a small margin (user 2026-08-28: "tabs could
 * be a bit tighter around the text"); callers push this before ed_tab */
static float ed_tab_w(const char *label)
{
    const struct nk_user_font *f = ctx->style.font;
    return f->width(f->userdata, f->height, label, nk_strlen(label)) + 28;
}

static int ed_tab(const char *label, int active)
{
    struct nk_style_button t = ctx->style.button;
    struct nk_rect b = nk_widget_bounds(ctx);
    int hit;
    t.rounding = 0; t.border = 0;
    t.padding = nk_vec2(4, 2);
    if (active) {
        t.normal = t.hover = t.active = nk_style_item_color(nk_rgb(58, 62, 76));
        t.text_normal = t.text_hover = t.text_active = nk_rgb(240, 180, 60);
    } else {
        t.normal = nk_style_item_color(nk_rgb(33, 35, 42));
        t.hover  = nk_style_item_color(nk_rgb(46, 50, 60));
        t.active = nk_style_item_color(nk_rgb(46, 50, 60));
        t.text_normal = nk_rgb(222, 224, 232);      /* clearly brighter than a disabled tab (user 2026-08-30) */
        t.text_hover = t.text_active = nk_rgb(255, 255, 255);
    }
    hit = nk_button_label_styled(ctx, &t, label);
    if (active)
        nk_fill_rect(nk_window_get_canvas(ctx),
                     nk_rect(b.x, b.y + b.h - 3, b.w, 3), 0, nk_rgb(240, 180, 60));
    return hit && !active;
}

/* a greyed, unclickable tab: STOCK context (user 2026-08-28: every tab but
 * Profiles disabled until a profile is picked) */
static void ed_tab_disabled(const char *label)
{
    struct nk_style_button t = ctx->style.button;
    t.rounding = 0; t.border = 0; t.padding = nk_vec2(4, 2);
    t.normal = t.hover = t.active = nk_style_item_color(nk_rgb(30, 32, 38));
    t.text_normal = t.text_hover = t.text_active = nk_rgb(66, 68, 76);
    nk_button_label_styled(ctx, &t, label);
}

/* section heading with an accent colour + optional wrapped explainer */
static void heading(const char *title, const char *sub)
{
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label_colored(ctx, title, NK_TEXT_LEFT, nk_rgb(240, 180, 60));
    if (sub && sub[0]) {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label_colored_wrap(ctx, sub, nk_rgb(165, 165, 175));
    }
}
static void note(const char *txt)      /* dimmed one-liner */
{
    nk_layout_row_dynamic(ctx, 16, 1);
    nk_label_colored(ctx, txt, NK_TEXT_LEFT, nk_rgb(150, 150, 160));
}

/* per-row explanations for the `rules` tables (editor documentation of
 * the engine constants; the authoritative source stays the tbl desc /
 * the cited ROM PCs in src). Keyed "table/rowlabel". */
static const struct { const char *key, *help; } rule_help[] = {
    /* hold_rules (src/anim.c) */
    { "hold_rules/tieup_hold_seed",        "Tie-up clock seed (0x12526): how long a standing hold lasts before it auto-resolves. Bigger = longer holds." },
    { "hold_rules/tieup_cat9_window",      "The throw window opens once the hold clock falls below this (0x12550). Bigger = throws come sooner." },
    { "hold_rules/autoreverse_clock_human","Frames until a human's front facelock is auto-reversed by the victim (0x1A564)." },
    { "hold_rules/autoreverse_clock_cpu",  "Frames until a CPU's front facelock is auto-reversed (0x1A572)." },
    { "hold_rules/hold_7d7e7f_clock",      "Clock of the reversal holds 0x7D/0x7E/0x7F (0x1A60C)." },
    /* ringout_rules (src/ringout.c, RO_* enum) */
    { "ringout_rules/frames_per_count",    "Frames per ring-out count (0x1FDF4 tick, stock 0x50 = 80). Smaller = the 20-count runs faster." },
    { "ringout_rules/warn_count",          "Count at which the referee's warning voice clips start (0x1FE36, stock 17)." },
    { "ringout_rules/resolve_count",       "The count that decides a count-out (stock 20, 0x1FDE6)." },
    { "ringout_rules/ref_x_min",           "Left clamp of the referee's walk during the outside count (0x1FD92, world x)." },
    { "ringout_rules/ref_x_max",           "Right clamp of the same walk (0x1FD9E)." },
    { "ringout_rules/ref_entry_x",         "Where the referee is teleported when the ring-out scene opens (0xFB94, world x)." },
    { "ringout_rules/ref_entry_y",         "...and its y (0xFB9C)." },
    { "ringout_rules/pose_first",          "First sprite cell of the counting referee's pose cycle (0x1FD80)." },
    { "ringout_rules/pose_last",           "Last cell of that cycle (0x1FDB6)." },
    { "ringout_rules/pose_frames",         "Frames each pose is held (0x1FD7A)." },
    { "ringout_rules/pose_last_extra",     "Extra frames on the last pose (0x1FDD8)." },
    { "ringout_rules/warn_ym",             "First YM sound id of the count warnings ($31xx low byte, 0x1FD84)." },
    { "ringout_rules/freeze_t22",          "Pose-timer freeze value once the count resolves (0x1FE5A)." },
    { "ringout_rules/faller_damage",       "Damage taken from falling out of the ring (0xFAC0)." },
    { "ringout_rules/digit_win_tens",      "FG0 VRAM window of the count's tens digit ($C0000-relative, 0x2069E)." },
    { "ringout_rules/digit_win_ones",      "...and the ones digit (0x20692)." },
    { "ringout_rules/result_out",          "Referee state word given to the OUTSIDE pair at resolve ($8003, 0x1FEC2)." },
    { "ringout_rules/result_in",           "...the inside pair ($8002, 0x1FEB2)." },
    { "ringout_rules/result_double",       "...both out: double count-out ($8005, 0x1FE76)." },
    /* tag_rules (src/tag.c, TAG_* enum) */
    { "tag_rules/usher_arm_frames",        "Frames after a run-in until the referee walks over to escort him out (0x215FA)." },
    { "tag_rules/runin_grace_frames",      "How long an illegal man may brawl before the recall counts against him (0x1D582)." },
    { "tag_rules/pin_arm_delay",           "Delay before a fresh pin arms the partner's rescue run-in (0x2164E)." },
    { "tag_rules/enter_ticks",             "Anim ticks per cell of the tag partner's over-the-ropes entry (0x18A90)." },
    { "tag_rules/enter_cells",             "Cells in that entry animation (0x18AEA)." },
    { "tag_rules/enter_step_x",            "X pixels per entry step (0x18AEA)." },
    { "tag_rules/break_react_pinner",      "Reaction id the pinner takes when his cover is broken by a strike (0x1821C)." },
    { "tag_rules/break_react_victim",      "Reaction id the pinned man takes at the break (0x24BF4)." },
    { "tag_rules/break_down_t",            "Down-timer of the broken pile (0x24BFA)." },
    { "tag_rules/break_pinner_down_t",     "Down-timer of the struck pinner (0x18246)." },
    /* game_rules, the 2026-09-05 rows */
    /* game_rules, the 2026-08-29 rows */
    /* mode_rules (src/modrules.c, MODE_* enum) */
    /* tieup_rules (src/tieup.c) */
    { "tieup_rules/cpu_lockup_max",        "Consecutive tie-up lockups two CPU men may roll before one is forced to win (0 = unlimited like the ROM; 1 keeps CPU-vs-CPU short)." },
    /* weapon_rules (src/weapon.c, ROM immediates) */
    { "weapon_rules/w0_x",                 "Ringside spawn x of weapon slot 0, the steps (0xFFEE, stock 0x40D = 1037)." },
    { "weapon_rules/w0_y",                 "...and its y (0xFFF4, stock 0x140 = 320, the ring-skirt line)." },
    { "weapon_rules/w1_x",                 "Ringside spawn x of weapon slot 1, the box (0x10022, stock 0x330)." },
    { "weapon_rules/w1_y",                 "...and its y (0x10028, stock 0x119)." },
    { "weapon_rules/floor_z",              "Floor height of the ringside walkway a weapon rests on (0xFFFA, stock 0x100)." },
    { "weapon_rules/pickup_range",         "Pickup window: |dx| and |dy| to the weapon must both be under this (0xF14A/0xF15E, stock 32 px)." },
    { "weapon_rules/carry_dz",             "Height of a carried weapon above its holder (0xFE68, stock 0x50 = held overhead)." },
    { "weapon_rules/carry_dx",             "Forward offset of a carried weapon toward the holder's facing (0xFE6E, stock 32 px)." },
    { "weapon_rules/tumble_tick",          "Frames between a tossed weapon's tumble poses (0xFF54)." },
    { "weapon_rules/tumble_steps",         "Tumble poses a tossed weapon cycles through before it lies still (0xFF54)." },
    /* dips (docs/dip-switches.md, raw MAME values) */
    { "dips/coin_a",                       "Coin A rate: 3 = 1 coin/1 credit, 2 = 1 coin/2 credits, 1 = 2 coins/1 credit, 0 = 3 coins/1 credit." },
    { "dips/buyin_price",                  "BUY-IN price: 1 = one coin, 0 = the same as starting a game." },
    { "dips/regain_power_price",           "REGAIN POWER price: 1 = one coin, 0 = the same as starting a game." },
    { "dips/continue_price",               "CONTINUE price: 1 = one coin, 0 = the same as starting a game." },
    { "dips/demo_sounds",                  "1 = sound during the attract mode; 0 = silent attract." },
    { "dips/flip_screen",                  "1 = normal; 0 = the screen is rotated 180 degrees (cocktail cabinet)." },
    { "dips/fbi_logo",                     "1 = the FBI anti-piracy screen shows at boot; 0 = skipped." },
    { "dips/difficulty",                   "3 = normal, 2 = easy, 1 = hard, 0 = hardest (CPU tie-up bias and aggression)." },
    { "dips/players",                      "Cabinet player count: 3 = 4 players, 2 = 3 players, 1 = 2 players." },
    { "dips/sw2_5_unused",                 "Unused switch (SW2-5); the game never reads it." },
    { "dips/clear_stage_powerup",          "Energy restored after a cleared stage: 3 = 24, 2 = 32, 1 = 12, 0 = none." },
    { "dips/championship_game",            "Which match is the title match: 1 = the 5th, 0 = the 4th." },
    /* game_rules (src/modrules.c) — all NEUTRAL in stock */
    /* mode_rules (src/modrules.c) */
};
static const char *rule_help_for(const char *tbl, const char *row)
{
    char key[96];
    snprintf(key, sizeof key, "%s/%s", tbl, row);
    for (unsigned i = 0; i < sizeof rule_help / sizeof rule_help[0]; i++)
        if (!strcmp(rule_help[i].key, key)) return rule_help[i].help;
    {   /* game_rules / mode_rules: the one list in src/rules.def */
        extern const char *eng_rule_help(const char *tbl, const char *label);
        const char *h = eng_rule_help(tbl, row);
        if (h && *h) return h;
    }
    return NULL;
}

static volatile pid_t tool_pgid;   /* the tool's PROCESS GROUP: Cancel kills
                                      it directly (no pkill quoting), and the
                                      editor's exit kills it too — no zombie
                                      codex grinding invisibly (user 2026-08-25) */
static int tool_runner(void *arg)
{
    int fds[2];
    pid_t pid;
    char line[256];
    FILE *p;
    (void)arg;
    if (pipe(fds) != 0) { logline("cannot run tool"); tool_busy = 0; return 1; }
    pid = fork();
    if (pid == 0) {
        setpgid(0, 0);                 /* own group: children (codex) ride along */
        dup2(fds[1], 1); dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        execl("/bin/sh", "sh", "-c", tool_cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); logline("cannot run tool"); tool_busy = 0; return 1; }
    setpgid(pid, pid);                 /* both sides set it: no race */
    tool_pgid = pid;
    p = fdopen(fds[0], "r");
    while (p && fgets(line, sizeof line, p)) {
        size_t l = strlen(line); while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (l) logline(line);
    }
    if (p) fclose(p); else close(fds[0]);
    {
        int st = 0;
        waitpid(pid, &st, 0);
        logf_("[exit %d]", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
    tool_pgid = 0;
    tool_busy = 0;
    return 0;
}
void wf_editor_kill_tools(void)        /* editor exit + Cancel: kill the group */
{
    pid_t g = tool_pgid;
    if (g > 0) { kill(-g, SIGTERM); SDL_Delay(80); kill(-g, SIGKILL); }
}
/* the pack command of the editing context: a profile's paks, or STOCK's
 * (build/base.pak + wrestlers + gfx via --pack). An empty profile name used
 * to yield "./wfengine --pack-profile " = usage error, exit 2 (2026-08-27). */
static const char *pack_cmd(void)
{
    static char buf[200];
    if (wf_profile()[0]) snprintf(buf, sizeof buf, "./wfengine --pack-profile \"%s\"", wf_profile());
    else snprintf(buf, sizeof buf, "./wfengine --pack data/tables build/base.pak");
    return buf;
}
static int tool_modal;             /* a LOCKING spinner overlays the editor while this tool runs
                                      (packs, deletes, re-dress, saves - user 2026-08-27); the long
                                      codex runs stay non-modal so the editor keeps working */
static void run_tool(const char *cmd)
{
    char local[sizeof tool_cmd];
    if (tool_busy) { logline("a tool is still running"); return; }
    snprintf(local, sizeof local, "%s", cmd);   /* cmd may BE tool_cmd (aliasing broke a caller once) */
    snprintf(tool_cmd, sizeof tool_cmd, "%s 2>&1", local);
    logf_("$ %s", cmd);
    tool_busy = 1; tool_modal = 0;
    tool_thread = SDL_CreateThread(tool_runner, "wf-tool", NULL);
    SDL_DetachThread(tool_thread);
}
static void run_tool_modal(const char *cmd) { int was = tool_busy; run_tool(cmd); if (!was && tool_busy) tool_modal = 1; }

/* ------------------------------------------------------------ helpers */
static void mkdir_p(const char *path)
{
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    mkdir(tmp, 0775);
}
static void read_mods(void)   /* scan mods/ subdirectories (order.txt is retired) */
{
    DIR *d = opendir("mods"); struct dirent *e;
    n_mods = 0;
    if (!d) return;
    while ((e = readdir(d)) && n_mods < 16) {
        char pth[300]; struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(pth, sizeof pth, "mods/%s", e->d_name);
        if (stat(pth, &st) == 0 && S_ISDIR(st.st_mode)) {
            char mj[320], err[96]; json_val *doc;
            snprintf(mod_list[n_mods], 64, "%s", e->d_name);
            mod_desc[n_mods][0] = 0;
            snprintf(mj, sizeof mj, "mods/%s/mod.json", e->d_name);
            doc = json_parse_file(mj, err, sizeof err);
            if (doc) { snprintf(mod_desc[n_mods], 96, "%s", json_str(json_get(doc, "description"), "")); json_free(doc); }
            n_mods++;
        }
    }
    closedir(d);
}
/* where a table file is written: the active mod layer or the base tree */
static void table_path(const tbl_def *d, char *out, size_t n)
{
    if (mod_layer[0]) snprintf(out, n, "mods/%s/tables/%s/%s.json", mod_layer, d->group, d->name);
    else snprintf(out, n, "data/tables/%s/%s.json", d->group, d->name);
}
static void ws_path(unsigned id, const char *file, char *out, size_t n)
{
    if (mod_layer[0]) snprintf(out, n, "mods/%s/wrestlers/%02u/%s", mod_layer, id, file);
    else snprintf(out, n, "data/wrestlers/%02u/%s", id, file);
}
static void dirname_of(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", path); char *s = strrchr(out, '/'); if (s) *s = 0;
}

/* element access on the edit buffer (big-endian, by kind) */
static int64_t el_get(const uint8_t *b, int kind, uint32_t i)
{
    switch (kind) {
    case TK_U8: return b[i]; case TK_S8: return (int8_t)b[i];
    case TK_U16: return (b[i*2] << 8) | b[i*2+1]; case TK_S16: return (int16_t)((b[i*2] << 8) | b[i*2+1]);
    case TK_U32: return ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) | (b[i*4+2] << 8) | b[i*4+3];
    default: return (int32_t)(((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) | (b[i*4+2] << 8) | b[i*4+3]);
    }
}
static void el_set(uint8_t *b, int kind, uint32_t i, int64_t v)
{
    switch (tbl_kind_size(kind)) {
    case 1: b[i] = (uint8_t)v; break;
    case 2: b[i*2] = (uint8_t)(v >> 8); b[i*2+1] = (uint8_t)v; break;
    default: b[i*4] = (uint8_t)(v >> 24); b[i*4+1] = (uint8_t)(v >> 16); b[i*4+2] = (uint8_t)(v >> 8); b[i*4+3] = (uint8_t)v;
    }
}
static void el_range(int kind, int *lo, int *hi)
{
    switch (kind) {
    case TK_U8: *lo = 0; *hi = 255; break; case TK_S8: *lo = -128; *hi = 127; break;
    case TK_U16: *lo = 0; *hi = 65535; break; case TK_S16: *lo = -32768; *hi = 32767; break;
    default: *lo = -2147483647; *hi = 2147483647; break;   /* u32 edited as int (hex column shows the real word) */
    }
}

/* height left in the current panel (for the scrolling groups) */
static float avail_h(void)
{
    struct nk_rect r = nk_window_get_content_region(ctx);
    struct nk_vec2 p = nk_widget_position(ctx);
    float h = r.y + r.h - p.y - 6;
    return h < 60 ? 60 : h;
}

/* ------------------------------------------------- stock reference */
/* data/stock.json (wfengine --export-stock): every table's pristine
 * ROM/engine-default values in one file. The editor compares against it
 * to highlight what a mod changed, and reverts to it on demand. */
static json_val *stock_doc; static int stock_tried;
static uint8_t *stock_buf; static uint32_t stock_len;   /* cur_tbl's stock bytes */
static uint8_t *tbl_changed;   /* per-table flag: engine bytes differ from stock */
static int tbl_changed_n = -1, tbl_changed_total;

static void stock_load(void)
{
    char err[256];
    if (stock_tried) return;
    stock_tried = 1;
    stock_doc = json_parse_file("data/stock.json", err, sizeof err);
    if (!stock_doc) logf_("no stock reference: %s (Tools > Export stock)", err);
}
/* decode one table's stock values into malloc'd BE bytes (NULL if absent) */
static uint8_t *stock_bytes_for(const tbl_def *d, uint32_t *out_len)
{
    char key[128]; const json_val *t, *vals, *e; uint32_t j = 0;
    uint8_t *buf; int ksz = tbl_kind_size(d->kind);
    stock_load();
    if (d->rom_addr == TBL_SYNTH && d->defaults && d->len) {   /* engine scalars: the COMPILED defaults are stock - data/stock.json
                                                                  lags behind new rows (user 2026-08-30: every diverging rule amber) */
        buf = malloc(d->len); memcpy(buf, d->defaults, d->len); *out_len = d->len;
        return buf;
    }
    if (!stock_doc) return NULL;
    snprintf(key, sizeof key, "%s/%s", d->group, d->name);
    t = json_get(json_get(stock_doc, "tables"), key);
    vals = t ? json_get(t, "values") : NULL;
    if (!vals || vals->type != JSON_ARRAY || !vals->n) return NULL;
    buf = malloc((size_t)vals->n * (size_t)ksz);
    for (e = vals->child; e; e = e->next, j++) el_set(buf, d->kind, j, json_int(e, 0));
    *out_len = j * (uint32_t)ksz;
    return buf;
}
/* rescan which tables differ from stock (cheap: a few hundred KB memcmp) */
static void stock_rescan(void)
{
    int n = tbl_count();
    if (tbl_changed_n != n) { free(tbl_changed); tbl_changed = calloc((size_t)(n ? n : 1), 1); tbl_changed_n = n; }
    tbl_changed_total = 0;
    stock_load();
    for (int i = 0; i < n; i++) {
        const tbl_def *d = tbl_def_at(i);
        uint32_t len, sl; const uint8_t *b = tbl_bytes(i, &len); uint8_t *sb;
        tbl_changed[i] = 0;
        if (!b || !stock_doc) continue;
        sb = stock_bytes_for(d, &sl);
        if (sb) { tbl_changed[i] = (sl != len) || memcmp(sb, b, len) != 0; free(sb); }
        tbl_changed_total += tbl_changed[i];
    }
}
static int64_t stock_el(uint32_t e, int kind)   /* cur_tbl's stock element (INT64_MIN = none) */
{
    if (!stock_buf || (e + 1) * (uint32_t)tbl_kind_size(kind) > stock_len) return INT64_MIN;
    return el_get(stock_buf, kind, e);
}
/* amber styling for a cell whose value differs from stock */
static void changed_style_push(void)
{
    nk_style_push_style_item(ctx, &ctx->style.property.normal, nk_style_item_color(nk_rgb(96, 66, 18)));
    nk_style_push_style_item(ctx, &ctx->style.property.hover,  nk_style_item_color(nk_rgb(120, 84, 26)));
    nk_style_push_style_item(ctx, &ctx->style.property.active, nk_style_item_color(nk_rgb(140, 98, 30)));
    nk_style_push_color(ctx, &ctx->style.property.label_normal, nk_rgb(255, 210, 120));
    nk_style_push_color(ctx, &ctx->style.property.label_hover,  nk_rgb(255, 220, 140));
    nk_style_push_color(ctx, &ctx->style.property.label_active, nk_rgb(255, 225, 150));
}
static void changed_style_pop(void)
{
    nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_color(ctx);
    nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx);
}

static void select_table(int id)
{
    uint32_t len; const uint8_t *b;
    free(edit_buf); edit_buf = NULL; edit_len = 0; edit_dirty = 0; page = 0;
    free(stock_buf); stock_buf = NULL; stock_len = 0;
    cur_tbl = id;
    if (id < 0) return;
    b = tbl_bytes(id, &len);
    if (!b) return;
    edit_buf = malloc(len ? len : 1); memcpy(edit_buf, b, len); edit_len = len;
    stock_buf = stock_bytes_for(tbl_def_at(id), &stock_len);
}


/* a WRITE button: dims in the STOCK context (no mod layer) and explains
 * itself when pressed (user 2026-08-25: stock should be VISIBLY
 * read-only, not scold-after-click) */
/* the SAVE icon (user 2026-08-27: every save button carries it): a small
   green disk-ish square at the button's left edge, painted over the button */
static void save_icon(struct nk_rect b, int dim)
{
    struct nk_command_buffer *c = nk_window_get_canvas(ctx);
    struct nk_color g = dim ? nk_rgb(70, 100, 70) : nk_rgb(90, 200, 110);
    nk_fill_rect(c, nk_rect(b.x + 7, b.y + b.h / 2 - 6, 12, 12), 2, g);
    nk_fill_rect(c, nk_rect(b.x + 10, b.y + b.h / 2 - 6, 6, 4), 0, dim ? nk_rgb(38, 38, 44) : nk_rgb(30, 60, 36));
}
/* a green TICK at the right edge of a step button: this step has run once
 * (user 2026-08-28) - the button stays live for a re-run */
static void tick_icon(struct nk_rect b)
{
    struct nk_command_buffer *c = nk_window_get_canvas(ctx);
    struct nk_color g = nk_rgb(90, 220, 110);
    float x = b.x + b.w - 16, y = b.y + b.h / 2;
    nk_stroke_line(c, x, y, x + 4, y + 4, 2.5f, g);
    nk_stroke_line(c, x + 4, y + 4, x + 11, y - 5, 2.5f, g);
}
static int save_button(const char *label)
{
    struct nk_rect b = WB();
    if (!mod_layer[0]) {
        struct nk_style_button dim = ctx->style.button;
        dim.text_normal = dim.text_hover = dim.text_active = nk_rgb(110, 110, 120);
        dim.normal = dim.hover = dim.active = nk_style_item_color(nk_rgb(38, 38, 44));
        if (nk_button_label_styled(ctx, &dim, label))
            logline("STOCK is read-only - pick or create a profile in the header dropdown to save");
        save_icon(b, 1);
        return 0;
    }
    {   int r = nk_button_label(ctx, label); save_icon(b, 0); return r; }
}

/* ------------------------------------------------------- table editor */
/* the Apply/Revert/Save/Load row shared by Tables and Rules */
static void table_actions(const tbl_def *d)
{
    struct nk_rect b;
    nk_layout_row_static(ctx, 24, 130, 3);   /* Save | Revert | Stock (user 2026-08-27: no Load/Save JSON) */
    b = WB();
    if (save_button(edit_dirty ? "Save *" : "Save")) {
        char path[1024], dir[1024]; FILE *f;
        tbl_set_bytes(cur_tbl, edit_buf, edit_len); edit_dirty = 0;   /* apply too */
        table_path(d, path, sizeof path); dirname_of(path, dir, sizeof dir); mkdir_p(dir);
        f = fopen(path, "w");
        if (f) {
            if (mod_layer[0] && d->rom_addr == TBL_SYNTH && d->defaults && d->labels) {
                /* a rules layer: only the rows that differ from the table the
                 * mods BENEATH this one produce (stock + the profile's earlier
                 * layers), so the layer stays minimal and STACKS - diffing
                 * against stock would bake the lower mods' rows into it */
                extern int wf_profile_nmods(void); extern const char *wf_profile_mod(int);
                int below = 0; uint32_t blen = 0; int nm, sm; char why[256];
                for (int i = 0; i < wf_profile_nmods(); i++) if (!strcmp(wf_profile_mod(i), mod_layer)) { below = i; break; }
                uint8_t *base = tbl_merge_layers("data/tables", d, below, &blen, &nm, &sm, why, sizeof why);
                if (base) { tbl_write_json_sparse(f, d, edit_buf, edit_len, base, blen); free(base); }
                else tbl_write_json_sparse(f, d, edit_buf, edit_len, (const uint8_t *)d->defaults, d->len);
            } else
                tbl_write_json(f, d, edit_buf, edit_len);
            fclose(f); stock_rescan();
            logf_("saved %s (packing...)", path);
            snprintf(save_msg, sizeof save_msg, "saved %s", d->name);
            save_msg_t = SDL_GetTicks();
        } else logf_("cannot write %s", path);
        { char cmd[256]; snprintf(cmd, sizeof cmd, "%s", pack_cmd()); run_tool_modal(cmd); }
    }
    hint_at(b, "Write this table's JSON into the profile's save layer and repack its paks. '*' = unsaved edits.");
    b = WB();
    if (nk_button_label(ctx, "Revert")) select_table(cur_tbl);
    hint_at(b, "Throw the edits away and re-read the engine's current values.");
    b = WB();
    if (nk_button_label(ctx, "Stock")) {
        if (stock_buf) {
            free(edit_buf); edit_buf = malloc(stock_len ? stock_len : 1);
            memcpy(edit_buf, stock_buf, stock_len); edit_len = stock_len; edit_dirty = 1;
            logf_("%s reset to stock (Save to make it stick)", d->name);
        } else logf_("no stock values for %s (Tools > Export stock writes data/stock.json)", d->name);
    }
    hint_at(b, "Load the PRISTINE ROM/default values (data/stock.json) into the editor. Amber cells mark values that differ from stock. Save afterwards.");
}

static void draw_table_editor(void)
{
    const tbl_def *d = tbl_def_at(cur_tbl);
    int ksz, stride; uint32_t ne, nrows, rows_per_page = 48, r0;
    struct nk_rect b;
    if (!d || !edit_buf) {
        heading("Tables", "Every data table the engine reads, straight from the running process.");
        nk_layout_row_dynamic(ctx, 60, 1);
        nk_label_wrap(ctx, "Pick a table in the list on the left (type in the search box to filter). Each table shows its ROM address, element type and a description of what the values mean.");
        return;
    }
    ksz = tbl_kind_size(d->kind); ne = edit_len / (uint32_t)ksz;
    stride = d->stride ? d->stride : (int)ne; if (stride > 64) stride = 64;
    nrows = stride ? (ne + (uint32_t)stride - 1) / (uint32_t)stride : 0;

    nk_layout_row_dynamic(ctx, 22, 1);
    nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "%s   [%s]", d->name, d->group);
    nk_layout_row_dynamic(ctx, 16, 1);
    if (d->rom_addr == TBL_SYNTH)
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "engine scalars (no ROM home)   %u bytes   %s x %d per row   %u rows", edit_len, tbl_kind_name(d->kind), stride, nrows);
    else
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "ROM 0x%05X..0x%05X   %u bytes   %s x %d per row   %u rows", d->rom_addr, d->rom_addr + d->len, edit_len, tbl_kind_name(d->kind), stride, nrows);
    /* about box: what this table means (from the registration) */
    nk_layout_row_dynamic(ctx, 46, 1);
    if (nk_group_begin(ctx, "about", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 34, 1);
        nk_label_wrap(ctx, d->desc ? d->desc : "(no description registered)");
        nk_group_end(ctx);
    }

    table_actions(d);
    if (stock_buf) {
        uint32_t diff = 0, se = stock_len / (uint32_t)ksz;
        for (uint32_t e2 = 0; e2 < ne && e2 < se; e2++)
            diff += el_get(edit_buf, d->kind, e2) != el_get(stock_buf, d->kind, e2);
        if (diff || ne != se) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(255, 210, 120),
                              "%u value%s differ%s from stock (amber cells; hover one for the stock value, 'Stock' reverts the whole table)",
                              diff, diff == 1 ? "" : "s", diff == 1 ? "s" : "");
        }
    }
    nk_layout_row_dynamic(ctx, 24, 3);
    b = WB();
    nk_checkbox_label(ctx, "hex view", &hex_view);
    hint_at(b, "Show the raw values in hexadecimal (read-only) instead of editable decimal cells.");
    nk_labelf(ctx, NK_TEXT_LEFT, "page %u / %u", page + 1, nrows ? (nrows + rows_per_page - 1) / rows_per_page : 1);
    nk_label_colored(ctx, "hover a cell for its offset/ROM address", NK_TEXT_RIGHT, nk_rgb(150, 150, 160));
    nk_layout_row_static(ctx, 22, 90, 4);   /* text-sized pager (a full-width row of four
                                               was the "wrongly sized" complaint, 2026-08-29) */
    if (nk_button_label(ctx, "<< first")) page = 0;
    if (nk_button_label(ctx, "< prev") && page > 0) page--;
    if (nk_button_label(ctx, "next >") && (page + 1) * rows_per_page < nrows) page++;
    if (nk_button_label(ctx, "last >>")) page = nrows ? (nrows - 1) / rows_per_page : 0;

    r0 = (uint32_t)page * rows_per_page;
    nk_layout_row_dynamic(ctx, avail_h(), 1);
    if (nk_group_begin(ctx, "grid", NK_WINDOW_BORDER)) {
        /* fixed-width cells: 160 px label + 96 px per value (a one-column table
         * used to stretch its editor across the whole pane); rules tables get
         * the per-row explanation in a trailing column */
        int is_rules = !strcmp(d->group, "rules");
        int cols = stride + 1 + (is_rules ? 1 : 0);
        int i;
        nk_layout_row_begin(ctx, NK_STATIC, 18, cols);
        nk_layout_row_push(ctx, 160); nk_label(ctx, "row", NK_TEXT_LEFT);
        for (i = 0; i < stride; i++) { nk_layout_row_push(ctx, 96); nk_labelf(ctx, NK_TEXT_CENTERED, "%d", i); }
        if (is_rules) { nk_layout_row_push(ctx, 900); nk_label(ctx, "meaning", NK_TEXT_LEFT); }
        nk_layout_row_end(ctx);
        for (uint32_t r = r0; r < r0 + rows_per_page && r < nrows; r++) {
            char rl[48];
            if (d->labels) { int k = 0; while (k < (int)r && d->labels[k]) k++; snprintf(rl, sizeof rl, "%s", d->labels[k] ? d->labels[k] : ""); }
            else snprintf(rl, sizeof rl, "%u", r);
            nk_layout_row_begin(ctx, NK_STATIC, 22, cols);
            nk_layout_row_push(ctx, 160);
            nk_label(ctx, rl, NK_TEXT_LEFT);
            for (i = 0; i < stride; i++) {
                uint32_t e = r * (uint32_t)stride + (uint32_t)i;
                nk_layout_row_push(ctx, 96);
                if (e >= ne) { nk_label(ctx, "", NK_TEXT_LEFT); continue; }
                if (hex_view) {
                    char hx[16]; int64_t v = el_get(edit_buf, d->kind, e);
                    snprintf(hx, sizeof hx, ksz == 1 ? "%02llX" : ksz == 2 ? "%04llX" : "%08llX", (unsigned long long)(v & (ksz == 1 ? 0xFF : ksz == 2 ? 0xFFFF : 0xFFFFFFFFu)));
                    nk_label(ctx, hx, NK_TEXT_CENTERED);
                } else {
                    int v = (int)el_get(edit_buf, d->kind, e), old = v, lo, hi;
                    int64_t sv = stock_el(e, d->kind);
                    int changed = (sv != INT64_MIN && sv != v);
                    char name[24]; snprintf(name, sizeof name, "##%u", e);   /* "##": unique id, no label */
                    struct nk_rect cb = WB();
                    el_range(d->kind, &lo, &hi);
                    if (changed) changed_style_push();
                    nk_property_int(ctx, name, lo, &v, hi, 1, 1);
                    if (changed) changed_style_pop();
                    if (v != old) { el_set(edit_buf, d->kind, e, v); edit_dirty = 1; }
                    if (nk_input_is_mouse_hovering_rect(&ctx->input, cb)) {
                        char sline[48] = "";
                        if (changed) snprintf(sline, sizeof sline, "\nSTOCK: %lld (0x%llX)", (long long)sv, (unsigned long long)sv & (ksz == 1 ? 0xFFu : ksz == 2 ? 0xFFFFu : 0xFFFFFFFFu));
                        if (d->rom_addr == TBL_SYNTH)
                            nk_tooltipf(ctx, "row %u col %d  =  %d (0x%X)\nsynthetic offset 0x%X%s", r, i, v, (unsigned)v & (ksz == 1 ? 0xFFu : ksz == 2 ? 0xFFFFu : 0xFFFFFFFFu), e * (uint32_t)ksz, sline);
                        else
                            nk_tooltipf(ctx, "row %u col %d  =  %d (0x%X)\nROM 0x%05X (offset 0x%X)%s", r, i, v, (unsigned)v & (ksz == 1 ? 0xFFu : ksz == 2 ? 0xFFFFu : 0xFFFFFFFFu), d->rom_addr + e * (uint32_t)ksz, e * (uint32_t)ksz, sline);
                    }
                }
            }
            if (is_rules) {
                const char *help = d->labels ? rule_help_for(d->name, rl) : NULL;
                nk_layout_row_push(ctx, 900);
                nk_label_colored(ctx, help ? help : "", NK_TEXT_LEFT, nk_rgb(165, 165, 175));
            }
            nk_layout_row_end(ctx);
        }
        nk_group_end(ctx);
    }
}

/* ---------------------------------------------------------- rules */
/* The `rules` group are engine scalars with one LABELLED value per row —
 * show them as a friendly form: name, value widget, explanation. */
/* case-insensitive substring (the search boxes) */
static int ci_strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) { size_t k = 0; while (k < n && hay[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++; if (k == n) return 1; }
    return 0;
}
/* a rule row matches the search when its NAME or its DESCRIPTION does */
static int rule_row_matches(const tbl_def *d, const char *lab, const char *q)
{
    const char *help = rule_help_for(d->name, lab);
    return ci_strstr(lab, q) || (help && ci_strstr(help, q));
}
/* a rule SET matches when its name, its description or any of its rows do */
static int rule_tbl_matches(const tbl_def *d, const char *q)
{
    if (!q[0] || ci_strstr(d->name, q) || (d->desc && ci_strstr(d->desc, q))) return 1;
    for (int k = 0; d->labels && d->labels[k]; k++) if (rule_row_matches(d, d->labels[k], q)) return 1;
    return 0;
}

static void draw_rules(void)
{
    const tbl_def *d = tbl_def_at(cur_tbl);
    int ksz; uint32_t ne;
    if (!d || !edit_buf || strcmp(d->group, "rules")) {
        heading("Ruleset", "Engine rule scalars: the hold clocks, ring-out counts and tag timers that used to be hard-coded.");
        nk_layout_row_dynamic(ctx, 70, 1);
        nk_label_wrap(ctx, "Pick a rule set on the left. Every value has a name and an explanation; edit it, then Save (header) writes it into the profile's save layer and repacks.");
        return;
    }
    ksz = tbl_kind_size(d->kind); ne = edit_len / (uint32_t)ksz;

    nk_layout_row_dynamic(ctx, 22, 1);
    nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "%s", d->name);
    nk_layout_row_dynamic(ctx, 34, 1);
    nk_label_wrap(ctx, d->desc ? d->desc : "");
    table_actions(d);

    if (filter[0]) {                                   /* the search box (left) filters the ROWS too: name or description */
        int nm = 0;
        for (uint32_t e = 0; e < ne; e++) { const char *lab = NULL; if (d->labels) { uint32_t k = 0; while (k < e && d->labels[k]) k++; lab = d->labels[k]; } if (lab && rule_row_matches(d, lab, filter)) nm++; }
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "search \"%s\": %d row%s in this set (names + descriptions; the list on the left shows every set with a match)", filter, nm, nm == 1 ? "" : "s");
    }
    nk_layout_row_dynamic(ctx, avail_h(), 1);
    if (nk_group_begin(ctx, "rules", NK_WINDOW_BORDER)) {
        for (uint32_t e = 0; e < ne; e++) {
            const char *lab = NULL, *help;
            int v = (int)el_get(edit_buf, d->kind, e), old = v, lo, hi;
            int64_t sv = stock_el(e, d->kind);
            int changed = (sv != INT64_MIN && sv != v);
            char pname[48], fallback[16];
            float rr[5] = { 0.26f, 0.15f, 0.10f, 0.05f, 0.44f };
            if (d->labels) { uint32_t k = 0; while (k < e && d->labels[k]) k++; lab = d->labels[k]; }
            if (!lab) { snprintf(fallback, sizeof fallback, "row %u", e); lab = fallback; }
            if (filter[0] && !rule_row_matches(d, lab, filter)) continue;
            help = rule_help_for(d->name, lab);
            el_range(d->kind, &lo, &hi);
            nk_layout_row(ctx, NK_DYNAMIC, 26, 5, rr);
            {
                struct nk_rect lb = WB();
                if (changed) nk_label_colored(ctx, lab, NK_TEXT_LEFT, nk_rgb(255, 210, 120));
                else nk_label(ctx, lab, NK_TEXT_LEFT);
                if (help && nk_input_is_mouse_hovering_rect(&ctx->input, lb))
                    nk_tooltip(ctx, help);        /* full text (the column clips) */
            }
            snprintf(pname, sizeof pname, "##rv%u", e);
            {
                struct nk_rect vb = WB();
                if (changed) changed_style_push();
                nk_property_int(ctx, pname, lo, &v, hi, 1, 1);
                if (changed) changed_style_pop();
                if (nk_input_is_mouse_hovering_rect(&ctx->input, vb))
                    nk_tooltipf(ctx, "%s = %d (0x%X)%s", lab, v, (unsigned)v & 0xFFFFu,
                                changed ? "  [differs from stock]" : "");
            }
            /* stock column + one-click revert */
            if (sv == INT64_MIN) { nk_label(ctx, "", NK_TEXT_LEFT); nk_label(ctx, "", NK_TEXT_LEFT); }
            else if (changed) {
                struct nk_rect sb2 = WB();
                nk_labelf_colored(ctx, NK_TEXT_RIGHT, nk_rgb(255, 210, 120), "stock %lld", (long long)sv);
                hint_at(sb2, "The pristine ROM/default value of this row.");
                sb2 = WB();
                if (nk_button_label(ctx, "<")) { v = (int)sv; }
                hint_at(sb2, "Put the stock value back into this row.");
            } else {
                nk_labelf_colored(ctx, NK_TEXT_RIGHT, nk_rgb(110, 115, 125), "= stock");
                nk_label(ctx, "", NK_TEXT_LEFT);
            }
            if (help) nk_label_colored(ctx, help, NK_TEXT_LEFT, nk_rgb(165, 165, 175));
            else nk_label_colored(ctx, "(see the table description above)", NK_TEXT_LEFT, nk_rgb(120, 120, 130));
            if (v != old) { el_set(edit_buf, d->kind, e, v); edit_dirty = 1; }
        }
        nk_group_end(ctx);
    }
}

static int *sorted_ids; static int sorted_n;
static int cmp_tbl(const void *a, const void *b)
{
    const tbl_def *x = tbl_def_at(*(const int *)a), *y = tbl_def_at(*(const int *)b);
    int c = strcmp(x->group, y->group);
    return c ? c : strcmp(x->name, y->name);
}
static void draw_table_list(int rules_only)
{
    int n = tbl_count(), gi = 0;
    const char *last_group = NULL;
    if (tbl_changed_n != n) stock_rescan();
    if (sorted_n != n) {
        free(sorted_ids); sorted_ids = malloc(sizeof(int) * (size_t)(n ? n : 1));
        for (int i = 0; i < n; i++) sorted_ids[i] = i;
        qsort(sorted_ids, (size_t)n, sizeof(int), cmp_tbl);
        sorted_n = n;
    }
    {
        float fr[2] = { 0.25f, 0.75f };
        nk_layout_row(ctx, NK_DYNAMIC, 26, 2, fr);
        nk_label(ctx, "search", NK_TEXT_LEFT);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, filter, sizeof filter, nk_filter_default);
    }
    note(rules_only ? "search matches set names, rule names AND their descriptions" : "groups: click to fold/unfold");
    nk_layout_row_dynamic(ctx, avail_h(), 1);
    if (nk_group_begin(ctx, "tables", NK_WINDOW_BORDER)) {
        for (int si = 0; si < n; si++) {
            int i = sorted_ids[si];
            const tbl_def *d = tbl_def_at(i);
            int is_rule = !strcmp(d->group, "rules");
            if (rules_only != is_rule) continue;
            if (filter[0] && !(rules_only ? rule_tbl_matches(d, filter) : (ci_strstr(d->name, filter) || ci_strstr(d->group, filter)))) continue;
            if (!rules_only && (!last_group || strcmp(last_group, d->group))) {
                last_group = d->group; gi++;
                nk_layout_row_dynamic(ctx, 20, 1);
                { char gl[80]; snprintf(gl, sizeof gl, "%s %s", group_open[gi & 63] || filter[0] ? "-" : "+", d->group);
                  if (nk_button_label(ctx, gl)) group_open[gi & 63] = !group_open[gi & 63]; }
            }
            if (!rules_only && !group_open[gi & 63] && !filter[0]) continue;
            {
                int sel = (i == cur_tbl);
                int chg = (tbl_changed_n == n && tbl_changed[i]);
                char nm[80];
                snprintf(nm, sizeof nm, "%s%s", chg ? "* " : "", d->name);
                nk_layout_row_dynamic(ctx, 18, 1);
                if (chg) nk_style_push_color(ctx, &ctx->style.selectable.text_normal, nk_rgb(255, 210, 120));
                if (nk_selectable_label(ctx, nm, NK_TEXT_LEFT, &sel) && sel) select_table(i);
                if (chg) nk_style_pop_color(ctx);
            }
        }
        nk_group_end(ctx);
    }
}

/* ---------------------------------------------------- wrestler panel */
static uint16_t pose_pens_last[16]; static int pose_ws_last = -1;
static void pose_render(const uint16_t *pens)
{
    static uint32_t pix[256 * 256];
    const eng_pkg_rec *pr; int n;
    uint32_t rgb[16];
    if (pens) memcpy(pose_pens_last, pens, sizeof pose_pens_last);
    pose_ws_last = cur_ws;
    if (!pose_tex) pose_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 256);
    for (int k = 0; k < 16; k++) {
        unsigned w = pens ? pens[k] : (unsigned)(k * 0x111);
        rgb[k] = 0xFF000000u | ((w & 0xF) * 17u) << 16 | (((w >> 4) & 0xF) * 17u) << 8 | (((w >> 8) & 0xF) * 17u);
    }
    for (int i = 0; i < 256 * 256; i++) pix[i] = 0xFF303040u;
    n = eng_pkg_pose((unsigned)cur_ws, (unsigned)cur_pose, pose_flip, pose_partner, &pr);
    for (int i = 0; i < n; i++) {
        int sx = 128, sy = 0x80;                 /* object origin at the canvas centre */
        int x = pr[i].x + sx, y = pr[i].y + sy;
        int xpos = x & 0x1FF, ypos;
        unsigned chain = pr[i].chain & 7u;
        if (xpos > 512 - 16) xpos -= 512;
        ypos = ((256 - y) & 0x1FF) - 16;
        for (unsigned c = 0; c <= chain; c++) {
            const uint8_t *t = wf_video_tile_pens(pr[i].tile + c);   /* arena ids > 0xFFFF: never mask (the mask drew garbage for skin-layout men, 2026-08-27) */
            int dy = pr[i].flipy ? ypos - (int)(16 * chain) + (int)(16 * c) : ypos - (int)(16 * c);
            if (!t) continue;
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int qx = pr[i].flipx ? 15 - px : px, qy = pr[i].flipy ? 15 - py : py;
                uint8_t pen = t[qy * 16 + qx];
                int ox = xpos + px, oy = dy + py + 64;   /* +64: room above the origin */
                if (!pen || ox < 0 || ox >= 256 || oy < 0 || oy >= 256) continue;
                pix[oy * 256 + ox] = rgb[pen];
            }
        }
    }
    SDL_UpdateTexture(pose_tex, NULL, pix, 256 * 4);
    pose_tex_valid = 1;
}

/* Remove a clone slot from its OWNING layer (mods/<m>/ or the shared
 * roster/): package + select portrait + outfits, then repack so the stale
 * pak drops and he leaves the select grid (V508). Returns 1 if a delete ran. */
static int slot_delete(int slot)
{
    char rel[64], rpath[512], own[80] = "", cmd2[900];
    snprintf(rel, sizeof rel, "wrestlers/%02d/wrestler.json", slot);
    if (wf_mod_resolve(rel, rpath, sizeof rpath)) {
        const char *m = strstr(rpath, "mods/");
        if (m) {
            const char *e2 = strchr(m + 5, '/');
            if (e2 && (size_t)(e2 - m) < sizeof own) { memcpy(own, m, (size_t)(e2 - m)); own[e2 - m] = 0; }
        } else if (!strncmp(rpath, "roster/", 7)) snprintf(own, sizeof own, "roster");
    }
    if (!own[0]) { logline("cannot find which layer owns this slot"); return 0; }
    snprintf(cmd2, sizeof cmd2,
             "rm -rf \"%s/wrestlers/%02d\" \"%s/select/%02d.png\" \"%s/palettes/%02d.json\"",
             own, slot, own, slot, own, slot);
    if (system(cmd2) != 0) { logf_("delete failed (%s)", own); return 0; }
    logf_("deleted slot %d from %s - repacking", slot, own);
    snprintf(cmd2, sizeof cmd2, "%s", pack_cmd());
    run_tool_modal(cmd2);
    return 1;
}
static void fg_create(int sync);
static void fg_load_base(void);
static void fg_load_slot(void);
static void draw_forge(void);
static int fg_slot, fg_base, fg_base_loaded, fg_sub;   /* tentative: defined with the Forge below */
static int fg_seen = -1;                /* the slot the Wrestlers page last loaded into the form */
static int fg_pen_sel;                  /* tentative: the Forge palette swatch selection */
static int pose_zoom_on;                /* the standing-sprite zoom modal */
static char fg_name[32], fg_skin[40];
static uint16_t fg_pens[16];
static void draw_wrestlers(void)
{
    uint16_t *pens = eng_pkg_pens_mut((unsigned)cur_ws);
    struct nk_rect hb;
    if (cur_ws < ENG_WS_MAX) {
        nk_layout_row_dynamic(ctx, 24, 2);
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "%s  (id %02d)", ws_name(cur_ws), cur_ws);
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "package data/wrestlers/%02d   save layer: %s", cur_ws, mod_layer[0] ? mod_layer : "base");
    }
    if (cur_ws >= ENG_WS_MAX) {   /* sub-tabs above everything (user 2026-08-27) */
        nk_layout_row_begin(ctx, NK_STATIC, 24, 3);
        nk_layout_row_push(ctx, ed_tab_w("Identity")); if (ed_tab("Identity", fg_sub == 0)) fg_sub = 0;
        nk_layout_row_push(ctx, ed_tab_w("Move map")); if (ed_tab("Move map", fg_sub == 1)) fg_sub = 1;
        nk_layout_row_push(ctx, ed_tab_w("Sound map")); if (ed_tab("Sound map", fg_sub == 2)) fg_sub = 2;   /* user 2026-08-28 */
        nk_layout_row_end(ctx);
    }
    if (cur_ws >= ENG_WS_MAX && fg_sub == 0) heading(eng_ws_clone_base(cur_ws) >= 0 ? "Wrestler" : "New wrestler", NULL);
    if (cur_ws < ENG_WS_MAX || (fg_sub == 0 && eng_ws_clone_base(cur_ws) >= 0))   /* no sprite for a FREE slot (user 2026-08-27) */
    /* the standing sprite (pose 0) at the TOP of the page, drawn with the
       pens being edited - stock: the live package table, clone: the form's
       fg_pens - so a palette tweak shows at once and is only written by Save
       (user 2026-08-27) */
    {
        const uint16_t *show = cur_ws >= ENG_WS_MAX ? fg_pens : (pens ? pens : eng_pkg_palette((unsigned)cur_ws));
        if (cur_pose || pose_flip || pose_partner != -1) { cur_pose = 0; pose_flip = 0; pose_partner = -1; pose_tex_valid = 0; }
        if (pose_ws_last != cur_ws || (show && memcmp(pose_pens_last, show, sizeof pose_pens_last))) pose_tex_valid = 0;
        if (!pose_tex_valid) pose_render(show);
        nk_layout_row_begin(ctx, NK_STATIC, 200, 2);
        nk_layout_row_push(ctx, 200);
        {   struct nk_rect ib = WB();
            if (pose_tex) nk_image(ctx, nk_image_ptr(pose_tex));
            if (nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, ib)) pose_zoom_on = 2;   /* zoom modal (user 2026-08-27); 2 = the opening click's frame */
            hint_at(ib, "Click to zoom."); }
        nk_layout_row_push(ctx, 700);
        if (cur_ws >= ENG_WS_MAX && eng_ws_clone_base(cur_ws) >= 0 && nk_group_begin(ctx, "fg_pal", NK_WINDOW_NO_SCROLLBAR)) {
            /* the clone's palette beside the sprite, like the stock form (user 2026-08-27) */
                    nk_layout_row_begin(ctx, NK_STATIC, 20, 2);
                    nk_layout_row_push(ctx, 80); nk_label_colored(ctx, "Palette", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
                    nk_layout_row_push(ctx, 130);
                    {   struct nk_rect rb = WB();
                        if (nk_button_label(ctx, "Reset to base")) {   /* the base's stock pens (user 2026-08-27) */
                            const uint16_t *bp = eng_pkg_palette((unsigned)fg_base);
                            for (int k = 0; k < 16; k++) fg_pens[k] = bp ? bp[k] : (uint16_t)(k * 0x111);
                        }
                        hint_at(rb, "Put the base wrestler's stock palette back (unsaved until Save wrestler)."); }
                    nk_layout_row_end(ctx);
            nk_layout_row_static(ctx, 24, 36, 16);   /* compact swatches (user 2026-08-27) */
            for (int k = 0; k < 16; k++) {
                unsigned w = fg_pens[k];
                struct nk_color c = nk_rgb((w & 0xF) * 17, ((w >> 4) & 0xF) * 17, ((w >> 8) & 0xF) * 17);
                struct nk_rect sb = WB();
                if (nk_button_color(ctx, c)) fg_pen_sel = k;
                if (nk_input_is_mouse_hovering_rect(&ctx->input, sb))
                    nk_tooltipf(ctx, "pen %d = 0x%03X%s%s", k, w, k == fg_pen_sel ? " (selected)" : "", k == 0 ? " (0 = transparent)" : "");
            }
            {
                int r = fg_pens[fg_pen_sel] & 0xF, g = (fg_pens[fg_pen_sel] >> 4) & 0xF, bl = (fg_pens[fg_pen_sel] >> 8) & 0xF;
                struct nk_colorf cf = { (float)r / 15.f, (float)g / 15.f, (float)bl / 15.f, 1.f };
                nk_layout_row_begin(ctx, NK_STATIC, 104, 2);   /* picker | stacked R/G/B (user 2026-08-27) */
                nk_layout_row_push(ctx, 220);
                cf = nk_color_picker(ctx, cf, NK_RGB);
                nk_layout_row_push(ctx, 150);
                if (nk_group_begin(ctx, "fgpen", NK_WINDOW_NO_SCROLLBAR)) {
                    int nr = (int)(cf.r * 15.f + 0.5f), ng = (int)(cf.g * 15.f + 0.5f), nb = (int)(cf.b * 15.f + 0.5f);
                    if (nr != r || ng != g || nb != bl) { r = nr; g = ng; bl = nb; }
                    nk_layout_row_dynamic(ctx, 18, 1);
                    nk_labelf(ctx, NK_TEXT_LEFT, "pen %d = 0x%03X", fg_pen_sel, fg_pens[fg_pen_sel]);
                    nk_layout_row_dynamic(ctx, 22, 1);
                    nk_property_int(ctx, "#R", 0, &r, 15, 1, 1);
                    nk_property_int(ctx, "#G", 0, &g, 15, 1, 1);
                    nk_property_int(ctx, "#B", 0, &bl, 15, 1, 1);
                    nk_group_end(ctx);
                }
                nk_layout_row_end(ctx);
                fg_pens[fg_pen_sel] = (uint16_t)(r | (g << 4) | (bl << 8));
            }
            nk_group_end(ctx);
        }
        if (cur_ws < ENG_WS_MAX && nk_group_begin(ctx, "ws_pal", NK_WINDOW_NO_SCROLLBAR)) {
            /* the stock man's palette beside the sprite; Save writes the
               profile's save layer, never data/ (user 2026-08-27) */
            nk_layout_row_begin(ctx, NK_STATIC, 20, 2);
            nk_layout_row_push(ctx, 100); nk_label_colored(ctx, "Body palette", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            nk_layout_row_push(ctx, 130);
            {   struct nk_rect rb = WB();
                if (nk_button_label(ctx, "Reset to stock") && pens) {   /* the ROM pens from data/wrestlers/NN/palette.json */
                    char pp[128], err[96]; json_val *doc;
                    snprintf(pp, sizeof pp, "data/wrestlers/%02d/palette.json", cur_ws);
                    doc = json_parse_file(pp, err, sizeof err);
                    if (doc) {
                        const json_val *arr = json_get(doc, "pens"); int k = 0;
                        for (const json_val *e = arr ? arr->child : NULL; e && k < 16; e = e->next) pens[k++] = (uint16_t)json_int(e, 0);
                        json_free(doc); pose_tex_valid = 0; logf_("wrestler %02d palette reset to stock (unsaved)", cur_ws);
                    } else logf_("cannot read %s", pp);
                }
                hint_at(rb, "Put the ROM palette back (data/wrestlers/NN/palette.json); unsaved until Save palette.json."); }
            nk_layout_row_end(ctx);
    nk_layout_row_static(ctx, 24, 36, 16);   /* compact swatches (user 2026-08-27) */
    for (int k = 0; k < 16; k++) {
        unsigned w = pens ? pens[k] : 0;
        struct nk_color c = nk_rgb((w & 0xF) * 17, ((w >> 4) & 0xF) * 17, ((w >> 8) & 0xF) * 17);
        struct nk_rect sb = WB();
        if (nk_button_color(ctx, c)) cur_pen = k;
        if (nk_input_is_mouse_hovering_rect(&ctx->input, sb))
            nk_tooltipf(ctx, "pen %d = 0x%03X%s", k, w, k == 0 ? " (0 = transparent in sprites)" : "");
    }
    if (pens) {
        int r = pens[cur_pen] & 0xF, g = (pens[cur_pen] >> 4) & 0xF, b = (pens[cur_pen] >> 8) & 0xF, o = pens[cur_pen];
        struct nk_colorf cf = { (float)r / 15.f, (float)g / 15.f, (float)b / 15.f, 1.f };
        nk_layout_row_begin(ctx, NK_STATIC, 104, 2);   /* picker | stacked R/G/B (user 2026-08-27) */
        nk_layout_row_push(ctx, 220);
        cf = nk_color_picker(ctx, cf, NK_RGB);
        nk_layout_row_push(ctx, 150);
        if (nk_group_begin(ctx, "pen", NK_WINDOW_NO_SCROLLBAR)) {
            int nr = (int)(cf.r * 15.f + 0.5f), ng = (int)(cf.g * 15.f + 0.5f), nb = (int)(cf.b * 15.f + 0.5f);
            if (nr != r || ng != g || nb != b) { r = nr; g = ng; b = nb; }
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_labelf(ctx, NK_TEXT_LEFT, "pen %d = 0x%03X", cur_pen, pens[cur_pen]);
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_property_int(ctx, "R", 0, &r, 15, 1, 1); nk_property_int(ctx, "G", 0, &g, 15, 1, 1); nk_property_int(ctx, "B", 0, &b, 15, 1, 1);
            nk_group_end(ctx);
        }
        nk_layout_row_end(ctx);
        pens[cur_pen] = (uint16_t)(r | (g << 4) | (b << 8));
        if (pens[cur_pen] != o) pose_tex_valid = 0;
    }
            nk_group_end(ctx);
        }
        nk_layout_row_end(ctx);
    }
    if (cur_ws < ENG_WS_MAX) {
        /* stats below (user 2026-08-27) */
        heading("Stats", NULL);
    nk_layout_row_static(ctx, 24, 220, 1);
    hb = WB(); nk_property_int(ctx, "energy (hp)", 1, &ws_hp, 255, 1, 1);
    hint_at(hb, "Starting/maximum energy (ROM 0x10830). Stock humans: 100-135 by wrestler.");
    hb = WB(); nk_property_int(ctx, "walk speed", 1, &ws_walk, 255, 1, 1);
    hint_at(hb, "Polar walk speed, 4.4 fixed - 16 = one pixel per frame (ROM 0x116AE).");
    hb = WB(); nk_property_int(ctx, "run speed", 1, &ws_run, 255, 1, 1);
    hint_at(hb, "Run/whip speed, same 4.4 scale (ROM 0x11D4A).");
    nk_layout_row_static(ctx, 24, 150, 2);   /* Save stats | Save palette */
    if (pens && save_button("Save palette.json")) {
        if (!mod_layer[0]) logline("base is READ-ONLY - pick or create a mod layer in the Profiles panel first");
        else {
            char dst[256], dir[256]; FILE *f;
            ws_path((unsigned)cur_ws, "palette.json", dst, sizeof dst); dirname_of(dst, dir, sizeof dir); mkdir_p(dir);
            f = fopen(dst, "w");
            if (f) { fprintf(f, "{\"rom\":\"0x%X\",\"pens\":[", 0x2F22 + cur_ws * 32); for (int k = 0; k < 16; k++) fprintf(f, "%s%u", k ? "," : "", pens[k]); fprintf(f, "]}\n"); fclose(f); logf_("saved %s", dst); }
            else logf_("cannot write %s", dst);
        }
    }
    if (save_button("Save stats.json")) {
        if (!mod_layer[0]) { logline("base is READ-ONLY - pick or create a mod layer in the Profiles panel first"); goto stats_done; }
        {
        char src[256], dst[256], dir[256], err[256]; json_val *doc;
        snprintf(src, sizeof src, "data/wrestlers/%02d/stats.json", cur_ws);
        ws_path((unsigned)cur_ws, "stats.json", dst, sizeof dst);
        if (access(dst, R_OK) == 0) snprintf(src, sizeof src, "%s", dst);
        doc = json_parse_file(src, err, sizeof err);
        if (doc) {
            json_val *m;
            if ((m = (json_val *)json_get(doc, "hp"))) json_set_number(m, "value", ws_hp);
            if ((m = (json_val *)json_get(doc, "walk_speed"))) json_set_number(m, "value", ws_walk);
            if ((m = (json_val *)json_get(doc, "run_speed"))) json_set_number(m, "value", ws_run);
            dirname_of(dst, dir, sizeof dir); mkdir_p(dir);
            if (json_write_file(dst, doc) == 0) logf_("saved %s", dst); else logf_("cannot write %s", dst);
            json_free(doc);
        } else logf_("stats.json: %s", err);
        }
    }
    }
stats_done:
    if (cur_ws >= ENG_WS_MAX) {
        /* clone slot, free or used: the FORGE form (merged into this page,
           user 2026-08-27) - name / base / skin / stats / moves / palette,
           'Add' on a free slot creates the record and packs, 'Save
           wrestler' on a used one rewrites it. The roster column drives
           the slot. */
        if (fg_seen != cur_ws) {
            fg_seen = cur_ws; fg_slot = cur_ws;
            if (eng_ws_clone_base(cur_ws) >= 0) fg_load_slot();
            else { fg_name[0] = 0; fg_skin[0] = 0; fg_base_loaded = -1; }
        }
        draw_forge();
        if (fg_slot != cur_ws) { cur_ws = fg_slot; fg_seen = fg_slot; }   /* the form's slot combo moved */
        return;
    }
    if (ws_loaded != cur_ws) {
        ws_hp = eng_pkg_stat((unsigned)cur_ws, "hp", 0); ws_walk = eng_pkg_stat((unsigned)cur_ws, "walk", 0); ws_run = eng_pkg_stat((unsigned)cur_ws, "run", 0);
        ws_loaded = cur_ws; pose_tex_valid = 0;
    }


    /* palette */

}

/* ------------------------------------------------------- match panel */
static void obj_flags_text(const eng_obj *o, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s%s%s%s%s%s",
             (o->role & RF_LEGAL) ? "legal " : "",
             (o->role & RF_OUTSIDE) ? "OUTSIDE " : "",
             (o->role & RF_RUNNING) ? "running " : "",
             (o->role & RF_ENGAGED) ? "engaged " : "",
             (o->tag_flags & TF_WHIPPED) ? "whipped " : "",
             (o->st_flags & SF_ELIMINATED) ? "ELIMINATED " : "",
             o->apron ? "apron " : "",
             o->pinning ? "pinning " : "");
}

static void draw_engine(eng_state *st)
{
    struct nk_rect b;
    heading("Live match", "Every value edits the running game immediately. Raw hex stays on the dimmed line of each card; hover it for the field names.");
    nk_layout_row_dynamic(ctx, 24, 4);
    nk_labelf(ctx, NK_TEXT_LEFT, "camera x %d  y %d", st->cam_x, st->cam_y);
    nk_labelf(ctx, NK_TEXT_LEFT, "match clock %02X:%02X (BCD)", st->clk_min, st->clk_sec);
    b = WB();
    { int stg = (int)st->stage; nk_property_int(ctx, "stage", 0, &stg, 9, 1, 1); st->stage = (uint16_t)stg; }
    hint_at(b, "Campaign stage 0-9: picks the CPU energy table row and the opponents of the NEXT match.");
    b = WB();
    { int cm = (int)st->clk_min; nk_property_int(ctx, "clock min", 0, &cm, 0x99, 1, 1); st->clk_min = (uint8_t)cm; }
    hint_at(b, "Minutes of the on-screen match clock, in BCD (0x05 = 5:xx).");

    nk_layout_row_dynamic(ctx, avail_h(), 1);
    if (nk_group_begin(ctx, "objs", 0)) {
        for (int i = 0; i < ENG_MAX_OBJS; i++) {
            eng_obj *o = &st->obj[i];
            int v;
            char card[32], flags[96];
            if (!o->active) continue;
            snprintf(card, sizeof card, "obj%d", i);
            nk_layout_row_dynamic(ctx, 132, 1);
            if (!nk_group_begin(ctx, card, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) continue;
            /* line 1: who + decoded state */
            obj_flags_text(o, flags, sizeof flags);
            nk_layout_row_dynamic(ctx, 20, 2);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "slot %d   %s   %s   faces %s",
                              i, ws_name(o->wrestler),
                              o->cpu ? "CPU" : (o->input >= 0 ? "PLAYER" : ((o->driver & DRV_AUTOPILOT) ? "autopilot" : "-")),
                              (o->facing & 0x8000u) ? "right" : "left");
            nk_labelf(ctx, NK_TEXT_LEFT, "state %s (%X)%s   move 0x%02X   reaction 0x%02X   %s",
                      state_name(o->state), o->state & 0xFFu,
                      (o->state & 0x8000u) ? "" : " (new)",
                      o->move_id & 0xFFu, o->react_id & 0xFFu, flags);
            /* line 2: energy bar + links */
            {
                nk_size hp = o->hp, hmax = o->hp_max ? o->hp_max : 1;
                float er[4] = { 0.10f, 0.30f, 0.30f, 0.30f };
                nk_layout_row(ctx, NK_DYNAMIC, 20, 4, er);
                nk_label(ctx, "energy", NK_TEXT_LEFT);
                b = WB();
                nk_progress(ctx, &hp, hmax, NK_FIXED);
                hint_at(b, "Energy vs maximum. Edit the number to the right; the HUD gauge follows.");
                v = o->hp; nk_property_int(ctx, "#hp", 0, &v, 999, 1, 1);
                if (v != o->hp) { o->hp_delta = (int16_t)(o->hp_delta + (v - (int)o->hp)); o->hp = (uint16_t)v; }
                nk_labelf(ctx, NK_TEXT_LEFT, "opponent %s   partner %s",
                          o->opp >= 0 ? ws_name(st->obj[o->opp].wrestler) : "-",
                          o->partner >= 0 ? ws_name(st->obj[o->partner].wrestler) : "-");
            }
            /* line 3: position (world px; x right, y toward the back line, z height) */
            nk_layout_row_dynamic(ctx, 20, 4);
            v = (int)(o->x >> 16); { int old = v; b = WB(); nk_property_int(ctx, "#x", -512, &v, 2048, 1, 1); hint_at(b, "World x (ring 0x270..0x3D0-ish, centre 0x280 = 640). Bigger = right."); if (v != old) o->x = (int32_t)v << 16; }
            v = (int)(o->y >> 16); { int old = v; b = WB(); nk_property_int(ctx, "#y", -512, &v, 2048, 1, 1); hint_at(b, "World y (ring 0x118..0x198 = 280..408). Bigger = the BACK line, up-screen."); if (v != old) o->y = (int32_t)v << 16; }
            v = (int)(o->z >> 16); { int old = v; b = WB(); nk_property_int(ctx, "#z", -512, &v, 2048, 1, 1); hint_at(b, "Height. Mat = 0x140 (320); bigger = airborne."); if (v != old) o->z = (int32_t)v << 16; }
            nk_labelf(ctx, NK_TEXT_LEFT, "speed %u  max hp %u", o->speed, o->hp_max);
            /* line 4: the raw words, dimmed (hover for names) */
            nk_layout_row_dynamic(ctx, 16, 1);
            b = WB();
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(130, 130, 140),
                              "raw: state %04X sel %04X f32 %04X f33 %04X f34 %04X f56 %04X g44 %04X mash %04X down %u",
                              o->state, o->anim_sel, o->st_flags, o->role, o->tag_flags, o->driver,
                              o->grap44, o->mash_aa, o->down_t);
            hint_at(b, "ROM object fields: +0x20 state (b15 anim armed), +0x1C anim sel, +0x32/+0x33/+0x34/+0x56 flag words, +0x44 grapple/phase word, +0xAA mash counter, +0x9A down timer.");
            nk_group_end(ctx);
        }
        nk_group_end(ctx);
    }
}

/* ------------------------------------------------------- input panel */
static void keymap_path(char *out, size_t n)
{
    if (mod_layer[0]) snprintf(out, n, "mods/%s/keymap.json", mod_layer);
    else snprintf(out, n, "data/keymap.json");
}
static void draw_input(void)
{
    static const char *const row_names[KM_N] =
        { "RIGHT", "LEFT", "UP", "DOWN", "BUTTON 1", "BUTTON 2", "START", "COIN",
          "RUN (= B1+B2)" };
    char path[192];
    keymap_path(path, sizeof path);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_labelf(ctx, NK_TEXT_LEFT, "Keyboard map (%s)%s - click a key, then press the new one (Esc cancels)",
              path, keymap_dirty ? "  [unsaved]" : "");
    /* all four players on one page (user 2026-08-27): P3/P4 are the buy-in seats */
    nk_layout_row_dynamic(ctx, 24, 4);
    for (int p = 0; p < 4; p++)
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "PLAYER %d%s", p + 1, p >= 2 ? "  (buy-in seat)" : "");
    for (int b = 0; b < KM_N; b++) {
        nk_layout_row_dynamic(ctx, 24, 8);
        for (int p = 0; p < 4; p++) {
            int armed = (rebind_p == p && rebind_b == b);
            nk_labelf(ctx, NK_TEXT_LEFT, "%s  %s", keymap_entry_name(p, b), row_names[b]);
            if (nk_button_label(ctx, armed ? "<press a key>" : keymap_keyname(p, b))) {
                rebind_p = armed ? -1 : p;
                rebind_b = armed ? -1 : b;
            }
        }
    }
    nk_layout_row_dynamic(ctx, 12, 1); nk_label(ctx, "", NK_TEXT_LEFT);
    nk_layout_row_static(ctx, 24, 120, 2);
    { struct nk_rect sb = WB(); int hit = nk_button_label(ctx, "Save"); save_icon(sb, 0);
    if (hit) {
        if (keymap_save(mod_layer[0] ? path : NULL) == 0) {
            keymap_dirty = 0;
            logf_("keymap saved -> %s (loaded at startup: data/keymap.json, then mods in order)", path);
        } else logf_("keymap: save to %s FAILED", path);
    } }
    if (nk_button_label(ctx, "Defaults")) {
        keymap_reset_defaults(); keymap_dirty = 1; rebind_p = rebind_b = -1;
    }
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Changes apply to the game window at once; Save makes them stick across restarts.", NK_TEXT_LEFT);
}

/* Editing-context switch (header dropdown): which profile's TABLES the
 * editor shows/edits. Stock is selectable but read-only — flip back and
 * forth to compare values. Table/rule data reloads from the profile's
 * pak; wrestler art/gfx stay from launch (relaunch for those). */
static void switch_edit_profile(const char *name);   /* below */

/* -------------------------------------------- profiles & mods panel */
/* A PROFILE is a named launchable version of the game: an ordered list of
 * mod layers with its own pak cache (profile.h). STOCK is locked: no mods,
 * never editable — the original game is always one click away. */
static char prof_list[16][64]; static char prof_desc[16][160]; static char prof_cat[16][24]; static int prof_mode[16]; static int n_profs;
static int cur_prof = -1;              /* -1 = stock */
static char pf_mods[16][64]; static int pf_nmods, pf_loaded = -2;
static char new_prof[64], pf_descbuf[160];
static int pf_mode;                    /* "mode": 0 tag, 1 rumble (a profile boots into ONE mode) */
static unsigned char pf_hidden[64];    /* "disabled": roster slots hidden from this profile */
static char ro_name[64][48]; static unsigned char ro_have[64]; static int ro_scanned;   /* roster/wrestlers/NN */
static void read_roster(void)
{
    char pth[300], err[128];
    ro_scanned = 1; memset(ro_have, 0, sizeof ro_have);
    for (int id = 12; id < 64; id++) {
        json_val *doc;
        snprintf(pth, sizeof pth, "roster/wrestlers/%02d/wrestler.json", id);
        doc = json_parse_file(pth, err, sizeof err);
        if (!doc) continue;
        ro_have[id] = 1; snprintf(ro_name[id], 48, "%s", json_str(json_get(doc, "name"), "?"));
        json_free(doc);
    }
}
static int prof_popup; static int del_arm; static int layer_popup;
static int sk_zoom_on_flag(void);
static int fr_on_flag(void);
static int prof_popup_open(void) { return prof_popup || layer_popup || sk_zoom_on_flag() || fr_on_flag() || (tool_busy && tool_modal) || cf_kind || pend_prof_on || pend_fg_on || pose_zoom_on; }

static void read_profiles(void)
{
    DIR *d = opendir("profiles"); struct dirent *e;
    n_profs = 0;
    if (!d) return;
    while ((e = readdir(d)) && n_profs < 16) {
        size_t l = strlen(e->d_name);
        char pth[300], err[128]; json_val *doc;
        if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
        snprintf(pth, sizeof pth, "profiles/%s", e->d_name);
        doc = json_parse_file(pth, err, sizeof err);
        if (!doc) continue;
        snprintf(prof_list[n_profs], 64, "%.*s", (int)(l - 5), e->d_name);
        snprintf(prof_desc[n_profs], 160, "%s", json_str(json_get(doc, "description"), ""));
        snprintf(prof_cat[n_profs], 24, "%s", json_str(json_get(doc, "category"), "misc"));
        prof_mode[n_profs] = !strcmp(json_str(json_get(doc, "mode"), "tag"), "rumble");
        json_free(doc); n_profs++;
    }
    closedir(d);
}
static void load_profile_edit(int idx)
{
    char pth[300], err[128]; json_val *doc; const json_val *mods;
    pf_nmods = 0; pf_descbuf[0] = 0; pf_loaded = idx; pf_mode = 0; memset(pf_hidden, 0, sizeof pf_hidden);
    if (!ro_scanned) read_roster();
    if (idx < 0 || idx >= n_profs) return;
    snprintf(pth, sizeof pth, "profiles/%s.json", prof_list[idx]);
    doc = json_parse_file(pth, err, sizeof err);
    if (!doc) return;
    snprintf(pf_descbuf, sizeof pf_descbuf, "%s", json_str(json_get(doc, "description"), ""));
    pf_mode = !strcmp(json_str(json_get(doc, "mode"), "tag"), "rumble");
    { const json_val *h = json_get(doc, "disabled");
      for (const json_val *e = h ? h->child : NULL; e; e = e->next) { int id = (int)json_int(e, -1); if (id >= 0 && id < 64) pf_hidden[id] = 1; } }
    mods = json_get(doc, "mods");
    if (mods && mods->type == JSON_ARRAY)
        for (const json_val *e = mods->child; e && pf_nmods < 16; e = e->next)
            if (json_str(e, NULL)) snprintf(pf_mods[pf_nmods++], 64, "%s", json_str(e, NULL));
}
static void save_profile_edit(void)
{
    char pth[300]; FILE *f;
    if (cur_prof < 0) return;
    mkdir_p("profiles");
    snprintf(pth, sizeof pth, "profiles/%s.json", prof_list[cur_prof]);
    f = fopen(pth, "w");
    if (!f) { logf_("cannot write %s", pth); return; }
    fprintf(f, "{\n  \"name\": "); json_write_string(f, prof_list[cur_prof]);
    fprintf(f, ",\n  \"description\": "); json_write_string(f, pf_descbuf);
    fprintf(f, ",\n  \"category\": "); json_write_string(f, prof_cat[cur_prof][0] ? prof_cat[cur_prof] : "misc");
    fprintf(f, ",\n  \"mode\": \"%s\"", pf_mode ? "rumble" : "tag");
    fprintf(f, ",\n  \"mods\": [");
    for (int i = 0; i < pf_nmods; i++) { fprintf(f, "%s", i ? ", " : ""); json_write_string(f, pf_mods[i]); }
    fprintf(f, "]");
    { int first = 1; fprintf(f, ",\n  \"disabled\": [");
      for (int id = 0; id < 64; id++) if (pf_hidden[id]) { fprintf(f, "%s%d", first ? "" : ", ", id); first = 0; }
      fprintf(f, "]"); }
    fprintf(f, "\n}\n");
    fclose(f);
    snprintf(prof_desc[cur_prof], 160, "%s", pf_descbuf);
    logf_("saved %s (pack + launch to play it)", pth);
}

static int wp_gen;                     /* weapons tab: reload on a profile switch (defined with the tab) */
/* PROGRESS frame (user 2026-08-27: the spinner cannot spin while the UI
   thread loads paks) - draw one frame right now with a rolling 4-line log,
   then hand the input state back as if the frame had ended normally. Only
   valid BEFORE the main window is begun (the profile switch runs at the top
   of ed_frame). */
/* the opaque panel behind a sheet's text (user 2026-08-27: readable over the page) */
static void sheet_box(float cx, float cy, float bw, float bh)
{
    struct nk_command_buffer *c = nk_window_get_canvas(ctx);
    struct nk_rect r = nk_rect(cx - bw / 2, cy - bh / 2, bw, bh);
    nk_fill_rect(c, r, 6, nk_rgb(40, 40, 46));
    nk_stroke_rect(c, r, 6, 1.5f, nk_rgb(120, 120, 135));
}
static char prog_ln[4][100]; static int prog_n;
static char prog_title[48] = "loading profile...";
static SDL_Texture *prog_bg; static int prog_bg_ok;   /* the page as it looked when the switch was requested */
static void ed_progress(const char *fmt, ...)
{
    int w, h; va_list ap;
    va_start(ap, fmt); vsnprintf(prog_ln[prog_n % 4], sizeof prog_ln[0], fmt, ap); va_end(ap); prog_n++;
    SDL_GetWindowSize(win, &w, &h);
    nk_input_end(ctx);
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 100)));
    if (nk_begin(ctx, "progress", nk_rect(0, 0, (float)w, (float)h), NK_WINDOW_NO_SCROLLBAR)) {
        /* ONE status line that gets replaced (user 2026-08-27), under the title */
        sheet_box((float)w / 2, (float)h / 2 - 14, 640, 100);
        nk_layout_row_dynamic(ctx, (float)h / 2 - 40, 1); nk_label(ctx, "", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label_colored(ctx, prog_title, NK_TEXT_CENTERED, nk_rgb(240, 180, 60));
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, prog_ln[(prog_n - 1) % 4], NK_TEXT_CENTERED, nk_rgb(220, 220, 230));
    }
    nk_end(ctx);
    nk_style_pop_style_item(ctx);
    SDL_SetRenderDrawColor(ren, 30, 30, 34, 255);
    SDL_RenderClear(ren);
    if (prog_bg && prog_bg_ok) SDL_RenderCopy(ren, prog_bg, NULL, NULL);   /* the page, greyed by the sheet */
    nk_sdl_render(NK_ANTI_ALIASING_ON);
    SDL_RenderPresent(ren);
    nk_input_begin(ctx);
}
static void switch_edit_profile(const char *name)
{
    char base[256], wsdir[256], gfx[256];
    wp_gen++; prog_n = 0; snprintf(prog_title, sizeof prog_title, "loading profile...");
    if (!name || !name[0]) {                          /* STOCK, read-only */
        wf_profile_clear();
        ed_progress("tables: build/base.pak");
        if (tbl_load_pak("build/base.pak") != 0) { logline("cannot load build/base.pak"); return; }
        setenv("WF_PAKDIR", "build/wrestlers", 1);
        for (int c = ENG_WS_MAX; c < ENG_WS_EXT_MAX; c++) {
            ed_progress("wrestler slot %02d", c);
            eng_pkg_reload((unsigned)c);   /* clone registry follows the pakdir */
        }
        mod_layer[0] = 0;
    } else {
        if (wf_profile_set(name) != 0) { logf_("profile %s: load failed", name); return; }
        if (wf_profile_stale()) {
            ed_progress("mods changed: repacking %s (this is the slow part)", name);
            logf_("profile %s: mods changed - repacking (the pause you feel is this)...", name);
            if (wf_profile_pack() != 0) { logf_("profile %s: pack failed", name); return; }
            logf_("profile %s: repacked", name);
        }
        wf_profile_pak_paths(base, sizeof base, wsdir, sizeof wsdir, gfx, sizeof gfx);
        ed_progress("tables: %s", base);
        if (tbl_load_pak(base) != 0) {                    /* a pak from an older engine: repack once and retry */
            logf_("%s does not match this engine - repacking %s", base, name);
            if (wf_profile_pack() != 0 || tbl_load_pak(base) != 0) { logf_("cannot load %s - the profile is READ-ONLY until it packs", base); return; }
        }
        setenv("WF_PAKDIR", wsdir, 1);   /* wrestler packages follow the context
                                            (the Forge slot registry reads them) */
        for (int c = ENG_WS_MAX; c < ENG_WS_EXT_MAX; c++) {
            ed_progress("wrestler slot %02d", c);
            eng_pkg_reload((unsigned)c);   /* flush stale clone slots cached under
                                              the previous pakdir (slot showed
                                              "(free)" for a registered clone) */
        }
        /* the save target follows the profile: its LAST mod wins; a
         * profile with no mods gets one named after itself. */
        if (wf_profile_nmods() > 0)
            snprintf(mod_layer, sizeof mod_layer, "%s", wf_profile_mod(wf_profile_nmods() - 1));
        else {
            char d[256], pth[300]; FILE *f;
            snprintf(d, sizeof d, "mods/%s", name); mkdir_p(d);
            snprintf(pth, sizeof pth, "profiles/%s.json", name);
            f = fopen(pth, "w");
            if (f) {
                fprintf(f, "{\n  \"name\": "); json_write_string(f, name);
                fprintf(f, ",\n  \"description\": "); json_write_string(f, "");
                fprintf(f, ",\n  \"mods\": ["); json_write_string(f, name); fprintf(f, "]\n}\n");
                fclose(f);
            }
            wf_profile_set(name); read_mods();
            snprintf(mod_layer, sizeof mod_layer, "%s", name);
            logf_("profile %s had no mods - created mod '%s' as its save layer", name, name);
        }
    }
    select_table(cur_tbl);                            /* re-read from the new pak */
    stock_rescan();
    { extern void audio_reset_music_overrides(void); audio_reset_music_overrides(); }
}

/* ---- the PROFILE SETTINGS model (user 2026-08-24, option 2): the popup
 * shows the two game-rules tables (mode_rules + game_rules) MERGED for the
 * profile; edits write the full tables into the profile's OWN mod (named
 * after it, created + appended on first save). Layers stay underneath as
 * the advanced view. */
static uint8_t ps_buf[2][64]; static uint32_t ps_len[2];
static int ps_tid[2] = { -1, -1 };
static int ps_adv;

static void ps_overlay(const tbl_def *d, uint8_t *buf, uint32_t len, const char *path)
{
    char why[128]; uint32_t l2;
    uint8_t *b = tbl_json_to_bytes_file(path, d, &l2, why, sizeof why);
    if (!b) return;
    memcpy(buf, b, l2 < len ? l2 : len);
    free(b);
}
static void ps_load(void)
{
    static const char *names[2] = { "mode_rules", "game_rules" };
    for (int t = 0; t < 2; t++) {
        const tbl_def *d;
        uint32_t len;
        const uint8_t *cur;
        char path[512];
        ps_tid[t] = tbl_id(names[t]);
        if (ps_tid[t] < 0) continue;
        d = tbl_def_at(ps_tid[t]);
        cur = tbl_bytes(ps_tid[t], &len);
        /* size from the ENGINE's declaration, not the loaded pak: a profile
           packed before a rule was added carries a short table (19 rows
           when human_hit_mult made it 20) and the new row must still show */
        {
            uint32_t dlen = d->len > sizeof ps_buf[0] ? (uint32_t)sizeof ps_buf[0] : d->len;
            if (d->defaults) memcpy(ps_buf[t], d->defaults, dlen);   /* compiled stock */
            else memset(ps_buf[t], 0, dlen);
            if (cur) memcpy(ps_buf[t], cur, len < dlen ? len : dlen);
            len = dlen;
        }
        ps_len[t] = len;
        {   /* ...actually start from the DATA tree (pure stock), then the
             * profile's layers in order */
            snprintf(path, sizeof path, "data/tables/%s/%s.json", d->group, d->name);
            ps_overlay(d, ps_buf[t], len, path);
        }
        if (cur_prof >= 0)
            for (int m = 0; m < pf_nmods; m++) {
                snprintf(path, sizeof path, "mods/%s/tables/%s/%s.json", pf_mods[m], d->group, d->name);
                ps_overlay(d, ps_buf[t], len, path);
            }
    }
}
static void ps_save(void)
{
    static const char *names[2] = { "mode_rules", "game_rules" };
    char own[64], path[512], dir[512];
    int have = 0;
    if (cur_prof < 0) return;
    snprintf(own, sizeof own, "%s", prof_list[cur_prof]);
    for (int m = 0; m < pf_nmods; m++) if (!strcmp(pf_mods[m], own)) have = 1;
    if (!have && pf_nmods < 16) {                 /* the profile's own top layer */
        char d2[256];
        snprintf(d2, sizeof d2, "mods/%s", own); mkdir_p(d2);
        snprintf(pf_mods[pf_nmods++], 64, "%s", own);
        read_mods();
    }
    for (int t = 0; t < 2; t++) {
        const tbl_def *d = ps_tid[t] >= 0 ? tbl_def_at(ps_tid[t]) : NULL;
        FILE *f;
        if (!d) continue;
        snprintf(path, sizeof path, "mods/%s/tables/%s/%s.json", own, d->group, d->name);
        dirname_of(path, dir, sizeof dir); mkdir_p(dir);
        f = fopen(path, "w");
        if (f) { tbl_write_json(f, d, ps_buf[t], ps_len[t]); fclose(f); }
        (void)names;
    }
    save_profile_edit();                          /* keeps the mods list + desc */
    {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "./wfengine --pack-profile %s", prof_list[cur_prof]);
        run_tool(cmd);
    }
    logf_("profile %s settings saved into mods/%s (packing...)", own, own);
}
static int ps_stockval(int t, uint32_t e)
{
    const tbl_def *d = ps_tid[t] >= 0 ? tbl_def_at(ps_tid[t]) : NULL;
    const uint8_t *df;
    if (!d || !d->defaults) return 0;
    df = (const uint8_t *)d->defaults;
    return (int)(((unsigned)df[e * 2] << 8) | df[e * 2 + 1]);
}
static void ps_rows(int t, const char *title)
{
    const tbl_def *d = ps_tid[t] >= 0 ? tbl_def_at(ps_tid[t]) : NULL;
    uint32_t ne;
    if (!d) return;
    ne = ps_len[t] / 2u;
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, title, NK_TEXT_LEFT, nk_rgb(240, 180, 60));
    for (uint32_t e = 0; e < ne; e++) {
        const char *lab = NULL, *help;
        int v = (int)(((unsigned)ps_buf[t][e * 2] << 8) | ps_buf[t][e * 2 + 1]);
        int old = v, sv = ps_stockval(t, e), changed;
        char pname[48], fallback[16];
        /* visible description column (user 2026-08-24: "mod edit is
         * missing instructions/descriptions") */
        float rr[4] = { 0.20f, 0.10f, 0.10f, 0.60f };
        if (d->labels) { uint32_t k = 0; while (k < e && d->labels[k]) k++; lab = d->labels[k]; }
        if (!lab) { snprintf(fallback, sizeof fallback, "row %u", e); lab = fallback; }
        changed = (v != sv);
        help = rule_help_for(d->name, lab);
        nk_layout_row(ctx, NK_DYNAMIC, 24, 4, rr);
        {
            struct nk_rect lb = WB();
            if (changed) nk_label_colored(ctx, lab, NK_TEXT_LEFT, nk_rgb(255, 210, 120));
            else nk_label(ctx, lab, NK_TEXT_LEFT);
            if (help && nk_input_is_mouse_hovering_rect(&ctx->input, lb)) nk_tooltip(ctx, help);
        }
        snprintf(pname, sizeof pname, "##ps%d_%u", t, e);
        if (changed) changed_style_push();
        nk_property_int(ctx, pname, 0, &v, 65535, 1, 1);
        if (changed) changed_style_pop();
        if (changed) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(255, 210, 120), "stock %d", sv);
        else nk_label_colored(ctx, "= stock", NK_TEXT_LEFT, nk_rgb(110, 115, 125));
        {
            struct nk_rect hb = WB();
            nk_label_colored(ctx, help ? help : "", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            if (help && nk_input_is_mouse_hovering_rect(&ctx->input, hb)) nk_tooltip(ctx, help);
        }
        if (v != old) { ps_buf[t][e * 2] = (uint8_t)(v >> 8); ps_buf[t][e * 2 + 1] = (uint8_t)v; }
    }
}


/* ------------------------------------------------------------ Sounds
 * (user 2026-08-24: "sounds will need to also be in wfeditor")
 * Every latch command on the emulated board (sndboard.c). Play auditions
 * in-process; "Render to mod" dumps the stock tune as a WAV into the
 * editing profile's save layer so the user can edit it in place — that
 * WAV then OVERRIDES the tune for the profile (audio.c music_override). */
#include "audio.h"
typedef struct { int cmd; char kind; int tab, utab; char label[64]; char user[64]; } SndRow;   /* utab = user's tab override (-1 none) */   /* kind m/s/x; tab 0 music / 1 announcer / 2 effects (from the GENERATED label); user = data/sound-names.json override */
static SndRow snd_rows[160];
static int snd_nrows = -1;

/* user names for sound commands (user 2026-08-28: right-click rename):
 * data/sound-names.json {"0x2A": "punch", ...} - an overlay, because
 * data/sound-commands.txt is generated and says not to hand-edit it */
static void snd_names_save(void)
{
    FILE *f = fopen("data/sound-names.json", "w");
    int first = 1;
    if (!f) { logline("cannot write data/sound-names.json"); return; }
    fprintf(f, "{\n \"note\": \"user names for sound commands (wfeditor Sounds tab, right-click rename); overlays data/sound-commands.txt\"");
    for (int i = 0; i < snd_nrows; i++) if (snd_rows[i].user[0] || snd_rows[i].utab >= 0) {
        static const char *tabs[3] = { "music", "announcer", "effects" };
        fprintf(f, ",\n \"0x%02X\": {", snd_rows[i].cmd);
        if (snd_rows[i].user[0]) { fprintf(f, "\"name\": "); json_write_string(f, snd_rows[i].user); }
        if (snd_rows[i].utab >= 0) fprintf(f, "%s\"tab\": \"%s\"", snd_rows[i].user[0] ? ", " : "", tabs[snd_rows[i].utab]);
        fprintf(f, "}"); first = 0;
    }
    (void)first;
    fprintf(f, "\n}\n");
    fclose(f);
}

static void snd_read_rows(void)
{
    FILE *f = fopen("data/sound-commands.txt", "r");
    char ln[256];
    snd_nrows = 0;
    if (!f) return;
    while (fgets(ln, sizeof ln, f) && snd_nrows < 160) {
        SndRow *r = &snd_rows[snd_nrows];
        char wav[64]; int bankdummy; char banks[8];
        if (ln[0] == '#' || ln[0] == '\n') continue;
        if (sscanf(ln, "music 0x%x %63[^\n]", &r->cmd, r->label) == 2) { r->kind = 'm'; snd_nrows++; }
        else if (sscanf(ln, "stop 0x%x %63[^\n]", &r->cmd, r->label) == 2) { r->kind = 'x'; snd_nrows++; }
        else if (sscanf(ln, "0x%x %7s %d %63s %63[^\n]", &r->cmd, banks, &bankdummy, wav, r->label) >= 4) {
            if (r->label[0] == 0) snprintf(r->label, sizeof r->label, "%s", wav);
            r->kind = 's'; snd_nrows++;
        }
    }
    fclose(f);
    for (int i = 0; i < snd_nrows; i++) {   /* the sub-tab from the generated label: only a
                                                 QUOTED phrase is announcer speech (user 2026-08-28:
                                                 the 'speech, unintelligible' rows are effects) */
        SndRow *r = &snd_rows[i];
        r->tab = (r->kind == 'm' || r->kind == 'x') ? 0 : r->label[0] == '"' ? 1 : 2;
        r->user[0] = 0; r->utab = -1;
    }
    {   char err[96]; json_val *doc = json_parse_file("data/sound-names.json", err, sizeof err);
        if (doc) {
            for (int i = 0; i < snd_nrows; i++) {
                char key[8]; const json_val *e; const char *t;
                snprintf(key, sizeof key, "0x%02X", snd_rows[i].cmd);
                e = json_get(doc, key);
                if (!e) continue;
                snprintf(snd_rows[i].user, 64, "%s", json_str(json_get(e, "name"), ""));
                t = json_str(json_get(e, "tab"), "");
                snd_rows[i].utab = !strcmp(t, "music") ? 0 : !strcmp(t, "announcer") ? 1 : !strcmp(t, "effects") ? 2 : -1;
            }
            json_free(doc);
        }
    }
}

static void snd_play(int cmd)
{
    if (!audio_ready()) {                 /* lazy: editor-only runs have no
                                             game audio until first Play */
        if (audio_init(NULL, 0) != 0) { logline("audio init failed"); return; }
        logline("audio device opened (emulated board)");
    }
    audio_on_sound_latch((uint16_t)cmd);
}

/* Sounds sub-tabs (user 2026-08-28: "at the very least separating music
 * from sounds"): 0 Music (tunes + the stop-all commands), 1 Announcer (the
 * OKI samples whose label is a quoted phrase / speech), 2 Effects (every
 * other sample: bell, crowd, impacts, unlabelled). */
static int snd_sub;
static int snd_row_tab(const SndRow *r) { return r->utab >= 0 ? r->utab : r->tab; }

/* the row's name label + a right-click RENAME popup (user 2026-08-28) */
static void snd_name_label(SndRow *r)
{
    struct nk_rect nb = WB();
    nk_label(ctx, r->user[0] ? r->user : r->label, NK_TEXT_LEFT);   /* plain: names/categories are global
                                          cosmetics, not a change off stock (user 2026-08-28) */
    if (nk_contextual_begin(ctx, 0, nk_vec2(340, 90), nb)) {
        static int rn_for = -1; static char rn_buf[64];
        if (rn_for != r->cmd) { rn_for = r->cmd; snprintf(rn_buf, sizeof rn_buf, "%s", r->user[0] ? r->user : r->label); }
        nk_layout_row_begin(ctx, NK_STATIC, 24, 3);
        nk_layout_row_push(ctx, 200); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, rn_buf, sizeof rn_buf, nk_filter_default);
        nk_layout_row_push(ctx, 70);
        if (nk_button_label(ctx, "Rename")) {
            snprintf(r->user, sizeof r->user, "%s", rn_buf);
            snd_names_save(); logf_("sound 0x%02X named '%s'", r->cmd, r->user);
            nk_contextual_close(ctx);
        }
        nk_layout_row_push(ctx, 50);
        if (nk_button_label(ctx, "Clear")) {
            r->user[0] = 0; snd_names_save(); rn_for = -1;
            nk_contextual_close(ctx);
        }
        nk_layout_row_end(ctx);
        {   /* move the row to another sub-tab (user 2026-08-28: some are
               filed wrong) */
            static const char *tabs[3] = { "Music", "Announcer", "Effects" };
            int cur = snd_row_tab(r);
            nk_layout_row_begin(ctx, NK_STATIC, 24, 4);
            nk_layout_row_push(ctx, 60); nk_label(ctx, "move to", NK_TEXT_LEFT);
            for (int t = 0; t < 3; t++) {
                nk_layout_row_push(ctx, 85);
                if (t == cur) nk_label_colored(ctx, tabs[t], NK_TEXT_CENTERED, nk_rgb(110, 115, 125));
                else if (nk_button_label(ctx, tabs[t])) {
                    r->utab = t == r->tab ? -1 : t;
                    snd_names_save(); logf_("sound 0x%02X moved to %s", r->cmd, tabs[t]);
                    nk_contextual_close(ctx);
                }
            }
            nk_layout_row_end(ctx);
        }
        nk_contextual_end(ctx);
    }
}

/* ---- sounds/ LIBRARY + wrestler sound-map references (user 2026-08-28):
 * WAVs live in the top-level sounds/ dir, shared by every profile like
 * skins/; a skin (skin.json "sounds") or a slot (wrestler.json "sounds")
 * POINTS at one ("wav:name") or at a stock clip ("cmd:0xNN"). Deleting a
 * referenced WAV is refused; renaming one re-points every reference. */
static char snd_lib[64][40]; static int snd_lib_n = -1;
static void snd_lib_scan(void)
{
    DIR *d = opendir("sounds"); struct dirent *e;
    snd_lib_n = 0;
    if (!d) return;
    while ((e = readdir(d)) && snd_lib_n < 64) {
        size_t l = strlen(e->d_name);
        if (l > 4 && l < 40 && !strcasecmp(e->d_name + l - 4, ".wav")) {
            snprintf(snd_lib[snd_lib_n], 40, "%.*s", (int)(l - 4), e->d_name); snd_lib_n++;
        }
    }
    closedir(d);
    for (int i = 1; i < snd_lib_n; i++) for (int j = i; j > 0 && strcmp(snd_lib[j - 1], snd_lib[j]) > 0; j--) { char t[40]; memcpy(t, snd_lib[j], 40); memcpy(snd_lib[j], snd_lib[j - 1], 40); memcpy(snd_lib[j - 1], t, 40); }
}
/* who references wav:<name>? count + the first few file names */
static int snd_wav_refs(const char *name, char *who, size_t n)
{
    char cmd[400], ln[300]; FILE *pf; int c = 0;
    if (who) who[0] = 0;
    snprintf(cmd, sizeof cmd, "grep -ls 'wav:%s\"' skins/*/skin.json mods/*/skins/*/skin.json mods/*/wrestlers/*/wrestler.json roster/wrestlers/*/wrestler.json 2>/dev/null", name);
    pf = popen(cmd, "r");
    if (!pf) return 0;
    while (fgets(ln, sizeof ln, pf)) {
        ln[strcspn(ln, "\n")] = 0; c++;
        if (who && strlen(who) + strlen(ln) + 2 < n) { if (who[0]) strcat(who, ", "); strcat(who, ln); }
    }
    pclose(pf);
    return c;
}
static int snd_audio_up(void)
{
    if (audio_ready()) return 1;
    if (audio_init(NULL, 0) != 0) { logline("audio init failed"); return 0; }
    logline("audio device opened (emulated board)");
    return 1;
}
static void snd_ref_play(const char *ref)
{
    unsigned c;
    if (!ref || !ref[0] || !snd_audio_up()) return;
    if (!strncmp(ref, "cmd:", 4) && sscanf(ref + 4, "%x", &c) == 1) audio_on_sound_latch((uint16_t)c);
    else if (!strncmp(ref, "wav:", 4)) { char pth[300]; snprintf(pth, sizeof pth, "sounds/%s.wav", ref + 4); if (audio_play_wav(pth) <= 0) logf_("cannot play %s", pth); }
}
static const char *const snd_ev_key[ENG_SND_N] = { "name_call", "intro_phrase" };
static const char *const snd_ev_lbl[ENG_SND_N] = { "name call", "ring intro" };
/* the reference picker: "(inherit)" | the Announcer clips | the library WAVs.
 * `ref` (cmd:0xNN / wav:name / "") is updated in place; returns 1 on change */
static int snd_ref_combo(char *ref, size_t n, const char *inherit_lbl, float width)
{
    static char lab[1 + 160 + 64][64]; static const char *items[1 + 160 + 64]; static char refs[1 + 160 + 64][48];
    int cnt = 0, cur = 0, sel;
    if (snd_nrows < 0) snd_read_rows();
    if (snd_lib_n < 0) snd_lib_scan();
    snprintf(lab[cnt], 64, "%s", inherit_lbl); refs[cnt][0] = 0; items[cnt] = lab[cnt]; cnt++;
    for (int i = 0; i < snd_nrows && cnt < 1 + 160; i++) {
        SndRow *r = &snd_rows[i];
        if (snd_row_tab(r) != 1) continue;
        snprintf(lab[cnt], 64, "0x%02X %s", r->cmd, r->user[0] ? r->user : r->label);
        snprintf(refs[cnt], 48, "cmd:0x%02X", r->cmd); items[cnt] = lab[cnt];
        if (!strcasecmp(refs[cnt], ref)) cur = cnt;
        cnt++;
    }
    for (int i = 0; i < snd_lib_n && cnt < 1 + 160 + 64; i++) {
        snprintf(lab[cnt], 64, "wav  %s", snd_lib[i]);
        snprintf(refs[cnt], 48, "wav:%s", snd_lib[i]); items[cnt] = lab[cnt];
        if (!strcmp(refs[cnt], ref)) cur = cnt;
        cnt++;
    }
    sel = nk_combo(ctx, items, cnt, cur, 22, nk_vec2(width, 320));
    if (sel != cur) { snprintf(ref, n, "%s", refs[sel]); return 1; }
    return 0;
}

static void draw_sounds(void)
{
    char path[512];
    static const char *subs[4] = { "Music", "Announcer", "Effects", "Library" };
    if (snd_nrows < 0) snd_read_rows();
    nk_layout_row_begin(ctx, NK_STATIC, 26, 4);
    for (int t = 0; t < 4; t++) {
        nk_layout_row_push(ctx, ed_tab_w(subs[t]));
        if (ed_tab(subs[t], snd_sub == t)) { snd_sub = t; if (t == 3) snd_lib_n = -1; }
    }
    nk_layout_row_end(ctx);
    if (snd_sub == 3) {                /* ---- LIBRARY: sounds/*.wav, shared by every profile ---- */
        heading("Library", "WAV files in the top-level sounds/ folder - shared by every profile, like skins. A skin or a wrestler slot POINTS at one (Skins > Prompt > Sounds, Wrestlers > Sounds). Drop a .wav in the folder; right-click a name to rename (references follow) or delete (refused while referenced).");
        if (snd_lib_n < 0) snd_lib_scan();
        nk_layout_row_static(ctx, 26, 150, 1);
        if (nk_button_label(ctx, "Rescan sounds/")) snd_lib_n = -1;
        nk_layout_row_dynamic(ctx, avail_h() - 60, 1);
        if (nk_group_begin(ctx, "snd_lib", NK_WINDOW_BORDER)) {
            if (snd_lib_n <= 0) { nk_layout_row_dynamic(ctx, 24, 1); nk_label_colored(ctx, "no WAV files in sounds/ yet", NK_TEXT_LEFT, nk_rgb(150, 150, 160)); }
            for (int i = 0; i < (snd_lib_n > 0 ? snd_lib_n : 0); i++) {
                static char who[64][200]; static int whon[64]; static Uint32 who_t;
                float lr[3] = { 0.30f, 0.08f, 0.62f };
                struct nk_rect nb;
                if (SDL_GetTicks() - who_t > 3000 || who_t == 0) { for (int k = 0; k < snd_lib_n; k++) whon[k] = snd_wav_refs(snd_lib[k], who[k], sizeof who[k]); who_t = SDL_GetTicks(); }
                nk_layout_row(ctx, NK_DYNAMIC, 24, 3, lr);
                nb = WB();
                nk_label(ctx, snd_lib[i], NK_TEXT_LEFT);
                if (nk_contextual_begin(ctx, 0, nk_vec2(360, 64), nb)) {
                    static int rn_for = -1; static char rn_buf[40];
                    if (rn_for != i) { rn_for = i; snprintf(rn_buf, sizeof rn_buf, "%s", snd_lib[i]); }
                    nk_layout_row_begin(ctx, NK_STATIC, 24, 3);
                    nk_layout_row_push(ctx, 180); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, rn_buf, sizeof rn_buf, nk_filter_default);
                    nk_layout_row_push(ctx, 75);
                    if (nk_button_label(ctx, "Rename") && rn_buf[0] && strcmp(rn_buf, snd_lib[i]) && !strpbrk(rn_buf, "/\\ .\"'")) {
                        char cmd[900];
                        snprintf(cmd, sizeof cmd, "mv \"sounds/%s.wav\" \"sounds/%s.wav\" && { grep -ls 'wav:%s\"' skins/*/skin.json mods/*/skins/*/skin.json mods/*/wrestlers/*/wrestler.json roster/wrestlers/*/wrestler.json 2>/dev/null | xargs -r sed -i 's/wav:%s\"/wav:%s\"/g'; true; }",
                                 snd_lib[i], rn_buf, snd_lib[i], snd_lib[i], rn_buf);
                        if (system(cmd) == 0) logf_("sound '%s' renamed to '%s' (references re-pointed; repack the profiles that use it)", snd_lib[i], rn_buf);
                        else logf_("rename failed (%s)", snd_lib[i]);
                        snd_lib_n = -1; who_t = 0; rn_for = -1;
                        nk_contextual_close(ctx);
                    }
                    nk_layout_row_push(ctx, 75);
                    if (whon[i] > 0) nk_label_colored(ctx, "in use", NK_TEXT_CENTERED, nk_rgb(110, 115, 125));
                    else if (nk_button_label(ctx, "Delete")) {
                        char pth[300]; snprintf(pth, sizeof pth, "sounds/%s.wav", snd_lib[i]);
                        if (unlink(pth) == 0) logf_("deleted %s", pth); else logf_("cannot delete %s", pth);
                        snd_lib_n = -1; who_t = 0; rn_for = -1;
                        nk_contextual_close(ctx);
                    }
                    nk_layout_row_end(ctx);
                    nk_contextual_end(ctx);
                }
                if (snd_lib_n < 0) break;
                if (nk_button_label(ctx, "Play")) { char ref[48]; snprintf(ref, sizeof ref, "wav:%s", snd_lib[i]); snd_ref_play(ref); }
                if (whon[i] > 0) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 210, 140), "used by %s", who[i]);
                else nk_label_colored(ctx, "unused", NK_TEXT_LEFT, nk_rgb(110, 115, 125));
            }
            nk_group_end(ctx);
        }
        return;
    }
    if (snd_sub == 0)
        heading("Music", "The emulated sound board (Z80+YM2151+OKI) plays the game's own driver. Play auditions the REAL tune; Render to mod writes it as music/cmd_XX.wav in the editing profile's save layer - edit that file and it overrides the tune for the profile. Right-click a name to rename it.");
    else if (snd_sub == 1)
        heading("Announcer", "OKI speech samples: name calls and move calls. Right-click a name to rename it (data/sound-names.json).");
    else
        heading("Effects", "OKI effect samples: the bell, crowd, impacts, grunts and the unlabelled ones. Right-click a name to rename it (data/sound-names.json).");
    {
        float r0[3] = { 0.12f, 0.12f, 0.76f };
        nk_layout_row(ctx, NK_DYNAMIC, 26, 3, r0);
        if (nk_button_label(ctx, "Stop music")) snd_play(0x00);
        if (nk_button_label(ctx, "Stop voices")) snd_play(0x20);
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160),
                          "save layer: %s%s", mod_layer[0] ? "mods/" : "", mod_layer[0] ? mod_layer : "(stock: read-only, no renders)");
    }
    nk_layout_row_dynamic(ctx, avail_h() - 90, 1);
    if (nk_group_begin(ctx, "snd_rows", NK_WINDOW_BORDER)) {
        for (int i = 0; i < snd_nrows; i++) {
            SndRow *r = &snd_rows[i];
            if (snd_row_tab(r) != snd_sub || r->kind == 'x') continue;   /* stop-all = the button above (user 2026-08-28) */
            if (r->kind == 'm') {
                float mr[6] = { 0.07f, 0.33f, 0.08f, 0.08f, 0.14f, 0.30f };
                int ovr;
                snprintf(path, sizeof path, "music/cmd_%02X.wav", r->cmd);
                { char res[512]; ovr = wf_mod_resolve(path, res, sizeof res); }
                nk_layout_row(ctx, NK_DYNAMIC, 24, 6, mr);
                nk_labelf(ctx, NK_TEXT_LEFT, "0x%02X", r->cmd);
                snd_name_label(r);
                if (nk_button_label(ctx, "Play")) snd_play(r->cmd);
                if (nk_button_label(ctx, "Stop")) snd_play(0x00);
                if (mod_layer[0]) {
                    if (nk_button_label(ctx, "Render to mod")) {
                        char cmd2[512];
                        snprintf(cmd2, sizeof cmd2, "mkdir -p mods/%s/music && ./wfengine --render-music 0x%02X 90 mods/%s/music/cmd_%02X.wav",
                                 mod_layer, r->cmd, mod_layer, r->cmd);
                        run_tool(cmd2);
                    }
                } else nk_label(ctx, "", NK_TEXT_LEFT);
                if (ovr) nk_label_colored(ctx, "WAV override in a mod layer", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
                else nk_label_colored(ctx, "emulated (stock tune)", NK_TEXT_LEFT, nk_rgb(120, 190, 120));
            } else {
                float sr[4] = { 0.07f, 0.33f, 0.08f, 0.52f };
                nk_layout_row(ctx, NK_DYNAMIC, 24, 4, sr);
                nk_labelf(ctx, NK_TEXT_LEFT, "0x%02X", r->cmd);
                snd_name_label(r);
                if (nk_button_label(ctx, "Play")) snd_play(r->cmd);
                nk_label_colored(ctx, r->kind == 'x' ? "stop-all command" : "OKI sample", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            }
        }
        nk_group_end(ctx);
    }
}

/* ------------------------------------------------------------- Forge
 * BUILD A WRESTLER (user task 2026-08-24): compose a clone slot (12..15)
 * from a stock base + name + stats + per-cell move map + palette, write
 * it as a mod (mods/<m>/wrestlers/NN/{wrestler,stats,palette}.json) and
 * pack the ACTIVE profile. The runtime plumbing already exists
 * (package.c "clone"/"movemap" pak sections, tools/pack_wrestler.c);
 * this panel only authors the JSON the packer understands. data/ and the
 * stock profile are never written.
 * Headless harness: WF_FORGE="slot,base,NAME[,cat.col=move]..." on the
 * first editor frame drives fg_create() — the same code as the Create
 * button — with the pack run synchronously so `--frames N` exits clean.
 * Run it with --editor --profile <p> so a save layer exists. */
#define FG_NCAT 21
static int fg_slot = 12, fg_base = 0;
/* the profile's wrestler-pak dir can CHANGE after the first forge (a mod
 * starts touching wrestlers/ -> the profile grows its own pak dir):
 * re-resolve before any package reload */
static void fg_repoint_pakdir(void)
{
    char base[256], wsdir[256], gfx[256];
    wf_profile_pak_paths(base, sizeof base, wsdir, sizeof wsdir, gfx, sizeof gfx);
    setenv("WF_PAKDIR", wsdir, 1);
}
static char fg_name[32], fg_mod[64];
static char fg_snd[ENG_SND_N][48];     /* wrestler.json "sounds": slot-level overrides ("" = inherit skin / base) */
static int fg_hp = 100, fg_walk = 20, fg_run = 40;
static int fg_map[FG_NCAT][3];
static uint16_t fg_pens[16];
static int fg_pen_sel;
static int fg_base_loaded = -1;        /* base whose defaults fill the form */
static int fg_sub;                     /* Forge form sub-tab: 0 wrestler, 1 move map */
static int fg_inited;

/* move catalog (data/movecatalog.json, built by --move-catalog):
 * category labels, per-cell candidate lists (every move some stock
 * wrestler routes at that cat+column = the SAFE pick list) and the
 * sparse name overlay (data/move-names.json is merged in by the tool). */
static char fg_cat_lab[FG_NCAT][40];
static uint8_t fg_cand[FG_NCAT][3][16];
static int fg_ncand[FG_NCAT][3];
static char fg_mvname[256][40];
static int fg_cat_state = -1;          /* -1 untried, 0 failed, 1 loaded */

static int fg_col_idx(const char *c)
{
    if (!c) return -1;
    if (!strcmp(c, "B1")) return 0;
    if (!strcmp(c, "B2")) return 1;
    if (!strcmp(c, "B1+B2")) return 2;
    return -1;
}
static void fg_catalog_load(void)
{
    char err[128]; json_val *doc;
    fg_cat_state = 0;
    doc = json_parse_file("data/movecatalog.json", err, sizeof err);
    if (!doc) { logf_("movecatalog: %s", err); return; }
    {
        const json_val *cats = json_get(doc, "categories");
        if (cats) for (const json_val *c = cats->child; c; c = c->next) {
            int i = c->key ? atoi(c->key) : -1;
            if (i >= 0 && i < FG_NCAT) snprintf(fg_cat_lab[i], sizeof fg_cat_lab[0], "%s", json_str(c, "?"));
        }
    }
    {
        const json_val *mvs = json_get(doc, "moves");
        if (mvs) for (const json_val *m = mvs->child; m; m = m->next) {
            int id = m->key ? atoi(m->key) : -1;
            const char *nm = json_str(json_get(m, "name"), NULL);
            const json_val *us = json_get(m, "users");
            if (id < 0 || id > 255) continue;
            if (nm) snprintf(fg_mvname[id], sizeof fg_mvname[0], "%s", nm);
            if (us) for (const json_val *u = us->child; u; u = u->next) {
                int cat = (int)json_int(json_get(u, "cat"), -1);
                int col = fg_col_idx(json_str(json_get(u, "col"), NULL));
                if (cat < 0 || cat >= FG_NCAT || col < 0) continue;
                {
                    int *n = &fg_ncand[cat][col], k;
                    for (k = 0; k < *n; k++) if (fg_cand[cat][col][k] == (uint8_t)id) break;
                    if (k == *n && *n < 16) fg_cand[cat][col][(*n)++] = (uint8_t)id;
                }
            }
        }
    }
    json_free(doc);
    for (int c = 0; c < FG_NCAT; c++) for (int col = 0; col < 3; col++)   /* ascending ids */
        for (int a = 0; a < fg_ncand[c][col]; a++) for (int b = a + 1; b < fg_ncand[c][col]; b++)
            if (fg_cand[c][col][b] < fg_cand[c][col][a])
            { uint8_t t = fg_cand[c][col][a]; fg_cand[c][col][a] = fg_cand[c][col][b]; fg_cand[c][col][b] = t; }
    fg_cat_state = 1;
}
static void fg_move_label(int id, char *out, size_t n)
{
    if (id == 0xFF) snprintf(out, n, "-- none (FF)");
    else if (id >= 0 && id < 256 && fg_mvname[id][0]) snprintf(out, n, "0x%02X %s", id, fg_mvname[id]);
    else snprintf(out, n, "0x%02X", id);
}
/* fill the form with the chosen base's package values */
static void fg_load_base(void)
{
    const uint16_t *bp = eng_pkg_palette((unsigned)fg_base);
    fg_hp = eng_pkg_stat((unsigned)fg_base, "hp", 100);
    fg_walk = eng_pkg_stat((unsigned)fg_base, "walk", 20);
    fg_run = eng_pkg_stat((unsigned)fg_base, "run", 40);
    for (int c = 0; c < FG_NCAT; c++) for (int col = 0; col < 3; col++)
        fg_map[c][col] = (int)eng_ws_move8((unsigned)fg_base, (unsigned)(c * 3 + col));
    if (!fg_skin[0])                   /* a DRESSED slot keeps its pens: the palette belongs to the skin's
                                          ingest, not the base (user 2026-08-29: changing 'plays like'
                                          painted the preview in the new base's palette) */
        for (int k = 0; k < 16; k++) fg_pens[k] = bp ? bp[k] : (uint16_t)(k * 0x111);
    fg_base_loaded = fg_base;
}
/* default target mod = the profile's save layer (how every other save
 * targets mods/); --editor --profile N never called switch_edit_profile,
 * so fall back to the active profile's last mod (or its own name). */
static void fg_default_mod(void)
{
    if (fg_mod[0]) return;
    if (mod_layer[0]) snprintf(fg_mod, sizeof fg_mod, "%s", mod_layer);
    else if (wf_profile()[0]) {
        if (wf_profile_nmods() > 0) snprintf(fg_mod, sizeof fg_mod, "%s", wf_profile_mod(wf_profile_nmods() - 1));
        else snprintf(fg_mod, sizeof fg_mod, "%s", wf_profile());
    }
}
static int fg_mod_ok(void)      /* plain dir name only: never data/, never a path */
{
    if (!fg_mod[0] || fg_mod[0] == '.') return 0;
    for (const char *p = fg_mod; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ' ') return 0;
    return 1;
}
/* append <mod> to the ACTIVE profile's mods list if missing (same file
 * format as save_profile_edit); reload the live profile afterwards. */
static int fg_profile_ensure_mod(const char *mod)
{
    char pn[64];   /* COPY: wf_profile() aliases the profile module's own
                      buffer, which wf_profile_set() rewrites */
    char pth[300], err[128], desc[160], catg[24];
    char fmods[16][64]; int nm = 0, have = 0;
    json_val *doc;
    snprintf(pn, sizeof pn, "%s", wf_profile());
    if (!pn[0]) return -1;
    snprintf(pth, sizeof pth, "profiles/%s.json", pn);
    doc = json_parse_file(pth, err, sizeof err);
    if (!doc) { logf_("%s: %s", pth, err); return -1; }
    snprintf(desc, sizeof desc, "%s", json_str(json_get(doc, "description"), ""));
    snprintf(catg, sizeof catg, "%s", json_str(json_get(doc, "category"), "misc"));
    {
        const json_val *ms = json_get(doc, "mods");
        if (ms && ms->type == JSON_ARRAY)
            for (const json_val *e = ms->child; e && nm < 16; e = e->next)
                if (json_str(e, NULL)) {
                    snprintf(fmods[nm], 64, "%s", json_str(e, NULL));
                    if (!strcmp(fmods[nm], mod)) have = 1;
                    nm++;
                }
    }
    json_free(doc);
    if (!have) {
        FILE *f;
        if (nm >= 16) { logline("profile mods list is full"); return -1; }
        snprintf(fmods[nm++], 64, "%s", mod);
        f = fopen(pth, "w");
        if (!f) { logf_("cannot write %s", pth); return -1; }
        fprintf(f, "{\n  \"name\": "); json_write_string(f, pn);
        fprintf(f, ",\n  \"description\": "); json_write_string(f, desc);
        fprintf(f, ",\n  \"category\": "); json_write_string(f, catg);
        fprintf(f, ",\n  \"mods\": [");
        for (int i = 0; i < nm; i++) { fprintf(f, "%s", i ? ", " : ""); json_write_string(f, fmods[i]); }
        fprintf(f, "]\n}\n");
        fclose(f);
        logf_("profile %s: mod '%s' appended", pn, mod);
    }
    wf_profile_set(pn);                              /* reload the live mods list */
    read_mods();
    if (cur_prof >= 0 && cur_prof < n_profs && !strcmp(prof_list[cur_prof], pn))
        load_profile_edit(cur_prof);                 /* keep the Profiles panel honest */
    return 0;
}
/* the Create button (sync=0) and the WF_FORGE harness (sync=1: pack
 * synchronously so a --frames run can exit) share this writer. */
/* Forge skin dressing: skins of the active layer built on fg_base */
static char fg_skin[40] = "";
static int fg_skin_cls[16], fg_skin_base[16];
static int fg_skins_for_base(char names[16][40])   /* ALL finished skins (class/base in fg_skin_cls/base) */
{
    int n = 0;
    for (int m = -1; m < wf_profile_nmods() && n < 16; m++) {   /* the LIBRARY
                                       (skins/), then ALL layers, like the
                                       Skins tab */
        char p[300]; DIR *d; struct dirent *e;
        if (m < 0) snprintf(p, sizeof p, "skins");
        else snprintf(p, sizeof p, "mods/%s/skins", wf_profile_mod(m));
        d = opendir(p);
        if (!d) continue;
        while ((e = readdir(d)) && n < 16) {
            char q[560], err[96]; json_val *doc; struct stat st; int dup = 0;
            if (e->d_name[0] == '.') continue;
            snprintf(q, sizeof q, "%s/%s/frames", p, e->d_name);
            if (stat(q, &st)) continue;             /* finished skins only */
            snprintf(q, sizeof q, "%s/%s/skin.json", p, e->d_name);
            doc = json_parse_file(q, err, sizeof err);
            if (!doc) continue;
            for (int k = 0; k < n; k++) if (!strcmp(names[k], e->d_name)) dup = 1;
            {   /* a skin fits every base of its CLASS (pre-V597 skins: the base's class) */
                int sb = (int)json_int(json_get(doc, "base"), -1);
                int sc = (int)json_int(json_get(doc, "class"), sb >= 0 && sb < ENG_WS_MAX ? eng_ws_body_class(sb) : -1);
                if (!dup) { fg_skin_cls[n] = sc; fg_skin_base[n] = sb; snprintf(names[n++], 40, "%s", e->d_name); }
            }
            json_free(doc);
        }
        closedir(d);
    }
    return n;
}

static int sk_resolve(const char *name, char *buf, size_t n);
static void fg_create(int sync)
{
    char dir[400], pth[480];
    FILE *f;
    if (!wf_profile()[0]) { logline("Forge: STOCK is read-only - pick or create a profile in the header dropdown first"); return; }
    fg_default_mod();
    if (!fg_mod_ok()) { logline("Forge: bad target mod name (plain directory name, no slashes/spaces)"); return; }
    if (!fg_name[0]) { logline("Forge: give him a name first"); return; }
    if (fg_slot < ENG_WS_MAX || fg_slot >= ENG_WS_EXT_MAX || (unsigned)fg_base >= ENG_WS_MAX)
        { logline("Forge: bad slot/base"); return; }
    if (eng_ws_clone_base(fg_slot) >= 0)             /* collision: warn, later layer wins */
        logf_("Forge: slot %d already carries '%s' in the loaded paks - the later mod layer wins at pack",
              fg_slot, eng_ws_clone_name(fg_slot) ? eng_ws_clone_name(fg_slot) : "?");
    snprintf(dir, sizeof dir, "mods/%s/wrestlers/%02d", fg_mod, fg_slot);
    mkdir_p(dir);
    snprintf(pth, sizeof pth, "%s/wrestler.json", dir);
    f = fopen(pth, "w");
    if (!f) { logf_("cannot write %s", pth); return; }
    fprintf(f, "{ \"clone_of\": %d, \"body_class\": %d, \"name\": ", fg_base, eng_ws_body_class(fg_base)); json_write_string(f, fg_name);
    if (fg_skin[0]) { fprintf(f, ", \"skin\": "); json_write_string(f, fg_skin); }
    {   int any = 0, first = 1;
        for (int e = 0; e < ENG_SND_N; e++) if (fg_snd[e][0]) any = 1;
        if (any) {
            fprintf(f, ", \"sounds\": {");
            for (int e = 0; e < ENG_SND_N; e++) if (fg_snd[e][0]) { fprintf(f, "%s\"%s\": ", first ? "" : ", ", snd_ev_key[e]); json_write_string(f, fg_snd[e]); first = 0; }
            fprintf(f, "}");
        }
    }
    fprintf(f, " }\n");
    fclose(f);
    snprintf(pth, sizeof pth, "%s/stats.json", dir);
    f = fopen(pth, "w");
    if (!f) { logf_("cannot write %s", pth); return; }
    fprintf(f, "{\"id\": %d, \"hp\": {\"value\": %d}, \"walk_speed\": {\"value\": %d}, \"run_speed\": {\"value\": %d}, \"move_map\": {\"rows\": [",
            fg_slot, fg_hp, fg_walk, fg_run);
    for (int c = 0; c < FG_NCAT; c++)
        fprintf(f, "%s[%d, %d, %d]", c ? ", " : "", fg_map[c][0] & 0xFF, fg_map[c][1] & 0xFF, fg_map[c][2] & 0xFF);
    fprintf(f, "]}}\n");
    fclose(f);
    snprintf(pth, sizeof pth, "%s/palette.json", dir);
    f = fopen(pth, "w");
    if (!f) { logf_("cannot write %s", pth); return; }
    fprintf(f, "{\"pens\": [");
    for (int k = 0; k < 16; k++) fprintf(f, "%s%u", k ? "," : "", fg_pens[k]);
    fprintf(f, "]}\n");
    fclose(f);
    logf_("forged %s -> mods/%s/wrestlers/%02d (clone of %02d %s)", fg_name, fg_mod, fg_slot, fg_base, ws_name(fg_base));
    if (fg_profile_ensure_mod(fg_mod) != 0) return;
    if (fg_skin[0]) {   /* dress the slot in the chosen skin: ingest its
                           frames into THIS slot (arena tiles are per-slot)
                           and take its select portrait */
        char sd[300], cmd2[700];
        if (!sync) { prog_n = 0; snprintf(prog_title, sizeof prog_title, "saving wrestler...");
                     ed_progress("dressing slot %d in skin '%s' (ingest is the slow part)", fg_slot, fg_skin); }
        sk_resolve(fg_skin, sd, sizeof sd);   /* the library, else a mod layer */
        snprintf(cmd2, sizeof cmd2, "rm -rf jobs/_dress && mkdir -p jobs/_dress && ln -s \"$(pwd)/%s/frames\" jobs/_dress/out && { [ -d \"%s/frames_hi\" ] && ln -s \"$(pwd)/%s/frames_hi\" jobs/_dress/out_hi; cp \"$(pwd)/%s/victmap.json\" jobs/_dress/ 2>/dev/null; cp \"$(pwd)/%s/palette.json\" jobs/_dress/ 2>/dev/null; cp \"$(pwd)/%s/manifest.json\" jobs/_dress/ 2>/dev/null; true; }", sd, sd, sd, sd, sd, sd);
        if (system(cmd2) != 0) logline("Forge: skin staging failed");
        else {
            snprintf(cmd2, sizeof cmd2, "./wfengine --art-ingest jobs/_dress %d %s", fg_slot, dir);
            if (system(cmd2) != 0) logf_("Forge: skin ingest failed (%s)", fg_skin);
            snprintf(cmd2, sizeof cmd2, "mkdir -p mods/%s/select && cp %s/select.png mods/%s/select/%02d.png 2>/dev/null",
                     fg_mod, sd, fg_mod, fg_slot);
            if (system(cmd2) != 0) logline("Forge: skin has no select portrait (skipped)");
            logf_("Forge: dressed slot %d in skin '%s'", fg_slot, fg_skin);
        }
    }
    {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "%s", pack_cmd());
        if (sync) { int rc = system(cmd); logf_("$ %s [exit %d]", cmd, rc); fg_repoint_pakdir(); eng_pkg_reload((unsigned)fg_slot); }
        else { run_tool_modal(cmd); fg_reload_slot = fg_slot; }   /* reload once the pack lands */
    }
    logf_("Forge: %s is in profile %s - Launch to pick him (select cell %d)", fg_name, wf_profile(), fg_slot);
}
/* read a forged wrestler (target mod + slot) back into the form */
static void fg_load_slot(void)
{
    char dir[400], pth[480], err[128];
    json_val *doc;
    {   /* the OWNING mod layer wins over the typed target (auto-load on
           selecting a registered slot needs no fields filled in) */
        char rel[64], rpath[512];
        snprintf(rel, sizeof rel, "wrestlers/%02d/wrestler.json", fg_slot);
        if (wf_mod_resolve(rel, rpath, sizeof rpath)) {
            const char *m = strstr(rpath, "mods/");
            if (m) {
                const char *e = strchr(m + 5, '/');
                if (e && (size_t)(e - (m + 5)) < sizeof fg_mod) {
                    memcpy(fg_mod, m + 5, (size_t)(e - (m + 5)));
                    fg_mod[e - (m + 5)] = 0;
                }
            }
        }
    }
    fg_default_mod();
    if (!fg_mod_ok()) { logline("Forge: set the target mod first"); return; }
    snprintf(dir, sizeof dir, "mods/%s/wrestlers/%02d", fg_mod, fg_slot);
    snprintf(pth, sizeof pth, "%s/wrestler.json", dir);
    doc = json_parse_file(pth, err, sizeof err);
    if (!doc) { logf_("no forged wrestler at %s (%s)", pth, err); return; }
    fg_base = (int)json_int(json_get(doc, "clone_of"), fg_base);
    if ((unsigned)fg_base >= (unsigned)ENG_WS_MAX) fg_base = 0;
    snprintf(fg_name, sizeof fg_name, "%s", json_str(json_get(doc, "name"), ""));
    for (int e = 0; e < ENG_SND_N; e++) snprintf(fg_snd[e], sizeof fg_snd[e], "%s", json_str(json_get(json_get(doc, "sounds"), snd_ev_key[e]), ""));
    snprintf(fg_skin, sizeof fg_skin, "%s", json_str(json_get(doc, "skin"), ""));   /* KEEP the skin link: a Forge re-save
                                          rewrote wrestler.json without it, so 'Save skin + pack' skipped the slot and it
                                          kept a stale ingest ("some frames appear as Earthquake", 2026-08-27) */
    json_free(doc);
    fg_load_base();                                   /* base defaults under the overrides */
    snprintf(pth, sizeof pth, "%s/stats.json", dir);
    doc = json_parse_file(pth, err, sizeof err);
    if (doc) {
        const json_val *rows = json_get(json_get(doc, "move_map"), "rows");
        fg_hp = (int)json_int(json_get(json_get(doc, "hp"), "value"), fg_hp);
        fg_walk = (int)json_int(json_get(json_get(doc, "walk_speed"), "value"), fg_walk);
        fg_run = (int)json_int(json_get(json_get(doc, "run_speed"), "value"), fg_run);
        if (rows && rows->type == JSON_ARRAY) {
            int c = 0;
            for (const json_val *r = rows->child; r && c < FG_NCAT; r = r->next, c++)
                for (int col = 0; col < 3; col++)
                    fg_map[c][col] = (int)json_int(json_at(r, col), fg_map[c][col]);
        }
        json_free(doc);
    }
    snprintf(pth, sizeof pth, "%s/palette.json", dir);
    doc = json_parse_file(pth, err, sizeof err);
    if (doc) {
        const json_val *pens = json_get(doc, "pens");
        if (pens && pens->type == JSON_ARRAY)
            for (int k = 0; k < 16; k++) fg_pens[k] = (uint16_t)json_int(json_at(pens, k), fg_pens[k]);
        json_free(doc);
    }
    logf_("loaded mods/%s/wrestlers/%02d: %s (clone of %02d %s)", fg_mod, fg_slot, fg_name, fg_base, ws_name(fg_base));
}
/* amber combo styling for a cell that differs from the base (the
 * changed_style_push pattern, applied to nk_combo's style slots) */
static void fg_combo_changed_push(void)
{
    nk_style_push_style_item(ctx, &ctx->style.combo.normal, nk_style_item_color(nk_rgb(96, 66, 18)));
    nk_style_push_style_item(ctx, &ctx->style.combo.hover,  nk_style_item_color(nk_rgb(120, 84, 26)));
    nk_style_push_style_item(ctx, &ctx->style.combo.active, nk_style_item_color(nk_rgb(140, 98, 30)));
    nk_style_push_color(ctx, &ctx->style.combo.label_normal, nk_rgb(255, 210, 120));
    nk_style_push_color(ctx, &ctx->style.combo.label_hover,  nk_rgb(255, 220, 140));
    nk_style_push_color(ctx, &ctx->style.combo.label_active, nk_rgb(255, 225, 150));
}
static void fg_combo_changed_pop(void)
{
    nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_color(ctx);
    nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx);
}
/* one move-map cell: a dropdown over the catalog's candidates for this
 * category+column, plus "none" (0xFF = unrouted; unrouted moves freeze
 * on FF00-hold records, so only the catalog's SAFE list is offered). */
static void fg_cell(int cat, int col)
{
    char buf[20][44]; const char *items[20];
    int n = 0, sel = -1, cur = fg_map[cat][col];
    int basev = (int)eng_ws_move8((unsigned)fg_base, (unsigned)(cat * 3 + col));
    int chg = (cur != basev);
    struct nk_rect b = WB();
    for (int i = 0; i < fg_ncand[cat][col] && n < 18; i++) {
        int id = fg_cand[cat][col][i];
        fg_move_label(id, buf[n], sizeof buf[0]); items[n] = buf[n];
        if (id == cur) sel = n;
        n++;
    }
    fg_move_label(0xFF, buf[n], sizeof buf[0]); items[n] = buf[n];
    if (cur == 0xFF) sel = n;
    n++;
    if (sel < 0) {   /* off-catalog value (hand-edited JSON): show, keep */
        snprintf(buf[n], sizeof buf[0], "0x%02X (custom)", cur); items[n] = buf[n]; sel = n; n++;
    }
    if (chg) fg_combo_changed_push();
    {
        int r = nk_combo(ctx, items, n, sel, 22, nk_vec2(250, 262));
        if (r != sel) {
            if (r < fg_ncand[cat][col]) fg_map[cat][col] = fg_cand[cat][col][r];
            else if (r == fg_ncand[cat][col]) fg_map[cat][col] = 0xFF;
        }
    }
    if (chg) fg_combo_changed_pop();
    if (chg && nk_input_is_mouse_hovering_rect(&ctx->input, b)) {
        char bl[48]; fg_move_label(basev, bl, sizeof bl);
        nk_tooltipf(ctx, "base: %s", bl);
    }
}


/* ------------------------------------------------------------- Skins
 * AI ART SKINS (user 2026-08-25): a reusable art set = base + prompt
 * variables + approved anchor + generated frames, stored under
 * mods/<layer>/skins/<name>/ (skin.json, character.txt, anchor.png,
 * select.png, frames/). Skins are per BODY CLASS (V597): refs come from
 * --class-template. The tab drives the C verbs (--class-template,
 * --art-anchor, --art-portrait[-install], --art-run) via run_tool and
 * watches jobs/<name>/{run_state.json,out}. Forge dresses a slot in a
 * finished skin. */
static char sk_name[40] = "", sk_char[600] = "", sk_outl[600] = "";
static void sk_pt_load(const char *dir); static void sk_pt_save(const char *dir);
extern const char *wf_art_provider_names[];   /* tools/art_run.c */
static int sk_provider;
static int sk_grid = 3;               /* GO sheet grid for standing figures: 2/3/4 (WF_ART_GRID; 3 = 341 px per figure, user 2026-08-27) */
static int sk_height = 100, sk_width = 100, sk_build_anchor;   /* body: skin.json height_pct/width_pct/build */               /* index into wf_art_provider_names (skin.json "provider") */
static int sk_base = 8, sk_class = 4, sk_sel = -1, sk_nlist, sk_stock;   /* sk_stock: class STOCK template (borrowed poses only) */   /* skins are per BODY CLASS (V597); base = the class representative */
static const char *const sk_class_names[ENG_BODY_CLASSES] = {
    "0 medium (Hogan/Warrior/Hawk/Animal)", "1 lean (Jake/DiBiase/Smash/Crush)", "2 small (Perfect)",
    "3 heavy (Boss Man/Slaughter)", "4 giant (Earthquake)" };
static int sk_class_rep(int cls)
{
    for (int w = 0; w < ENG_WS_MAX; w++) if (eng_ws_body_class(w) == cls) return w;
    return 0;
}
static char sk_list[32][40];
static SDL_Texture *sk_tex_ref, *sk_tex_anchor, *sk_tex_portrait, *sk_tex_frame;
/* FRAME modal: a list of PNGs (an animation, a sprite row) with prev/next -
 * the arena crowd animations and the scene sprite rows open it from their
 * tiles (user 2026-08-29: "tiles, with the modal, next through the frames") */
#define FR_MAX 128
static int fr_on; static char fr_title[96]; static char fr_paths[FR_MAX][300]; static int fr_n, fr_idx;
static SDL_Texture *fr_tex; static time_t fr_mt; static char fr_cur[300];
static int fr_alt;                 /* the modal shows the COUNTERPART (original <-> generated) */
static void fr_open(const char *title, int n, int idx) { snprintf(fr_title, sizeof fr_title, "%s", title); fr_n = n; fr_idx = idx < n ? idx : 0; fr_on = 2; fr_cur[0] = 0; fr_alt = 0; }
/* an arena picture's counterpart for the compare toggle (user 2026-08-30,
 * the skins modal's ref toggle): arenas/N/_gen/<view>/<f> <-> arenas/N/
 * <view>/<f>, arenas/N/_orig/... <-> the live file; a live file pairs with
 * _orig/ (it was applied: the live one IS the generated) else with _gen/.
 * Returns 0 none, 1 = path is the original, 2 = path is the generated. */
static int fr_counterpart(const char *path, char *out, size_t n)
{
    const char *g = strstr(path, "/_gen/"), *o = strstr(path, "/_orig/");
    if (g) { snprintf(out, n, "%.*s/%s", (int)(g - path), path, g + 6); return access(out, R_OK) == 0 ? 2 : 0; }
    if (o) { snprintf(out, n, "%.*s/%s", (int)(o - path), path, o + 7); return access(out, R_OK) == 0 ? 1 : 0; }
    if (!strncmp(path, "arenas/", 7)) {
        const char *sl = strchr(path + 7, '/');
        if (sl) {
            snprintf(out, n, "%.*s/_orig/%s", (int)(sl - path), path, sl + 1);
            if (access(out, R_OK) == 0) return 2;
            snprintf(out, n, "%.*s/_gen/%s", (int)(sl - path), path, sl + 1);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    out[0] = 0;
    return 0;
}
static int sk_zoom_on;             /* click a preview -> modal blow-up */
static int sk_zoom_on_flag(void) { return sk_zoom_on; }
static int fr_on_flag(void) { return fr_on; }
static char sk_zoom_title[80];
static char sk_zoom_path[560];     /* file the modal shows (re-read on mtime change,
                                      so a re-rolled image lands LIVE in the popup) */
static SDL_Texture *sk_zoom_tex;   /* the modal's OWN texture (slot textures get
                                      destroyed on reload — never point at them) */
static time_t sk_zoom_mt;
static int sk_zoom_pose = -1;      /* >= 0: the modal tracks this pose's out/ref */
static int sk_zoom_ghost;          /* modal: overlay the pose's grey ref under the result */
static const int cls_rep[ENG_BODY_CLASSES] = { 0, 2, 7, 3, 8 };   /* class representative (data/generics/C/manifest.json) */
static int sk_zoom_showref;        /* toggle: show the ORIGINAL ref instead */
static char sk_zoom_out_fmt[200], sk_zoom_ref_fmt[200]; static int sk_zoom_cls = -1;   /* class viewer: the modal's result/original path templates (%04d) + class for the underlay; empty = the skin job */
/* the four preview tiles (paths + names) - the zoom modal's prev/next walk
 * them when it was opened from a tile (user 2026-08-26: the arrows belong
 * in the modal, ref <-> anchor comparison) */
static char sk_zsrc[4][560];
static const char *sk_zsrc_name[4] = { "base ref", "anchor", "portrait", "newest frame" };
static int sk_zoom_tile = -1;      /* >= 0: the modal shows sk_zsrc[tile] */
static int sk_an_n, sk_an_cur = -1;   /* anchor candidate history (<skin>/anchors/NN.png) */
/* "<png>.meta" sidecar from art_run (provider=, when=) -> "2026-08-26 12:11 · codex";
 * older files: the mtime and "?" */
static void sk_meta_line(const char *png, char *out, size_t n)
{
    char p[620], line[200], prov[40] = "?", when[40] = ""; FILE *f; struct stat st;
    snprintf(p, sizeof p, "%s.meta", png);
    f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            if (!strncmp(line, "provider=", 9)) snprintf(prov, sizeof prov, "%s", line + 9);
            else if (!strncmp(line, "when=", 5)) snprintf(when, sizeof when, "%s", line + 5);
        }
        fclose(f);
    }
    if (!when[0] && stat(png, &st) == 0) {
        struct tm tm; localtime_r(&st.st_mtime, &tm);
        snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }
    if (!when[0]) { out[0] = 0; return; }
    snprintf(out, n, "%s  %s", when, prov);
}
static char sk_zoom_extra[220];    /* per-re-roll prompt tweak (this pose only) */
/* RE-ROLL QUEUE (user 2026-08-27: "instead of opening the modal, clicking
 * the button and waiting, can I just queue it?"): poses marked for a
 * re-roll - right-click a review tile, or "Queue re-roll" in the zoom modal
 * (carries the tweak text) - collect in jobs/<skin>/reroll_queue.txt
 * ("pose|extra" lines) and run as ONE tool (a shell script of --art-pose
 * calls) from the "Re-roll queued (N)" button, so codex works through them
 * while you keep reviewing. */
static void sk_dir(char *buf, size_t n);   /* below */
#define SK_RQ_MAX 128
static int sk_rq_pose[SK_RQ_MAX]; static char sk_rq_extra[SK_RQ_MAX][222]; static int sk_rq_n;
static char sk_rq_skin[40];
static void sk_rq_path(char *buf, size_t n) { snprintf(buf, n, "jobs/%s/reroll_queue.txt", sk_name); }
static void sk_rq_save(void)
{
    char p[300]; FILE *f;
    if (!sk_name[0]) return;
    sk_rq_path(p, sizeof p);
    if (!sk_rq_n) { remove(p); return; }
    f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < sk_rq_n; i++) fprintf(f, "%d|%s\n", sk_rq_pose[i], sk_rq_extra[i]);
    fclose(f);
}
static void sk_rq_load(void)
{
    char p[300], line[300]; FILE *f;
    sk_rq_n = 0; snprintf(sk_rq_skin, sizeof sk_rq_skin, "%s", sk_name);
    if (!sk_name[0]) return;
    sk_rq_path(p, sizeof p);
    f = fopen(p, "r"); if (!f) return;
    while (sk_rq_n < SK_RQ_MAX && fgets(line, sizeof line, f)) {
        char *bar = strchr(line, '|'); size_t l;
        if (!bar) continue;
        *bar++ = 0; l = strlen(bar); while (l && (bar[l-1] == '\n' || bar[l-1] == '\r')) bar[--l] = 0;
        sk_rq_pose[sk_rq_n] = atoi(line); snprintf(sk_rq_extra[sk_rq_n], sizeof sk_rq_extra[0], "%s", bar); sk_rq_n++;
    }
    fclose(f);
}
static int sk_rq_find(int pose) { for (int i = 0; i < sk_rq_n; i++) if (sk_rq_pose[i] == pose) return i; return -1; }
static void sk_rq_toggle(int pose, const char *extra)
{
    int i = sk_rq_find(pose);
    if (i >= 0) { for (; i < sk_rq_n - 1; i++) { sk_rq_pose[i] = sk_rq_pose[i+1]; snprintf(sk_rq_extra[i], sizeof sk_rq_extra[0], "%s", sk_rq_extra[i+1]); } sk_rq_n--; logf_("re-roll queue: pose %04d removed (%d queued)", pose, sk_rq_n); }
    else if (sk_rq_n < SK_RQ_MAX) { sk_rq_pose[sk_rq_n] = pose; snprintf(sk_rq_extra[sk_rq_n], sizeof sk_rq_extra[0], "%s", extra ? extra : ""); sk_rq_n++; logf_("re-roll queue: pose %04d added (%d queued)", pose, sk_rq_n); }
    sk_rq_save();
}
static void sk_rq_run(void)                /* one shell script = one tool run */
{
    char d[300], p[300], cmd[400]; FILE *f;
    if (!sk_rq_n || !sk_name[0]) return;
    sk_dir(d, sizeof d);
    snprintf(p, sizeof p, "jobs/%s/reroll_queue.sh", sk_name);
    f = fopen(p, "w"); if (!f) { logf_("cannot write %s", p); return; }
    fprintf(f, "#!/bin/sh\n# re-roll queue, %d pose(s)\n", sk_rq_n);
    for (int i = 0; i < sk_rq_n; i++) {
        fprintf(f, "echo '%d %d' > \"jobs/%s/reroll_queue.progress\"\necho 'queue: pose %04d (%d/%d)'\n./wfengine --art-pose \"jobs/%s\" \"%s/anchor.png\" \"%s/character.txt\" %d",
                i + 1, sk_rq_n, sk_name, sk_rq_pose[i], i + 1, sk_rq_n, sk_name, d, d, sk_rq_pose[i]);
        if (sk_rq_extra[i][0]) { fputs(" '", f); for (const char *c = sk_rq_extra[i]; *c; c++) if (*c != '\'' && *c != '"') fputc(*c, f); fputs("'", f); }
        fputs("\n", f);
    }
    fprintf(f, "rm -f \"jobs/%s/reroll_queue.progress\"\n", sk_name);
    fclose(f);
    snprintf(cmd, sizeof cmd, "sh \"%s\"", p);
    logf_("re-rolling %d queued pose(s) in one run", sk_rq_n);
    sk_rq_n = 0; sk_rq_save();
    run_tool(cmd);
}
static int sk_zoom_append = 1;     /* checked: tweak APPENDS to the C scaffold;
                                      unchecked: tweak REPLACES it entirely */
static int sk_all_poses[2048], sk_all_n;   /* the review list (modal prev/next) */
#define SK_REV_N 16                /* review browser: 2 rows x 8 per page */
static SDL_Texture *sk_rev_tex[SK_REV_N];
static time_t sk_rev_mt[SK_REV_N];
static int sk_rev_pose[SK_REV_N], sk_rev_page;
static char sk_rev_find[6];        /* review browser pose-id filter (prefix of the 4-digit id) */
/* GROUP BY MOVE (user 2026-08-26): data/poseindex.json (--pose-index) lists
 * every ROM animation's frames in order; the grid becomes sections - a header
 * cell per animation, then its frames as the flipbook plays. Poses no
 * animation names (handler-set, two-man variants) close the list as "other";
 * victim bodies (1024+) as their own section. */
static int sk_rev_group;           /* 0 = by id, 1 = by move */
static int sk_rev_move;            /* by move: WHICH animation (index into pi_anim; pi_n = "other", pi_n+1 = "victim bodies") */
typedef struct { char kind[10]; int id; char name[96]; int n; uint16_t pose[64]; } pi_anim_t;
static pi_anim_t *pi_anim; static int pi_n = -1;
static void pi_load(void)
{
    char err[128]; json_val *doc; const json_val *a;
    pi_n = 0;
    doc = json_parse_file("data/poseindex.json", err, sizeof err);
    if (!doc) { logline("data/poseindex.json missing - run ./wfengine --pose-index data/poseindex.json"); return; }
    a = json_get(doc, "animations");
    for (const json_val *e = a ? a->child : NULL; e; e = e->next) {
        const json_val *fr = json_get(e, "frames"); pi_anim_t *p;
        pi_anim = realloc(pi_anim, sizeof(pi_anim_t) * (size_t)(pi_n + 1));
        p = &pi_anim[pi_n++]; memset(p, 0, sizeof *p);
        snprintf(p->kind, sizeof p->kind, "%s", json_str(json_get(e, "kind"), "?"));
        p->id = (int)json_int(json_get(e, "id"), 0);
        snprintf(p->name, sizeof p->name, "%s", json_str(json_get(e, "name"), "?"));
        for (const json_val *f = fr ? fr->child : NULL; f && p->n < 64; f = f->next)
            p->pose[p->n++] = (uint16_t)json_int(json_at(f, 0), 0);
    }
    json_free(doc);
}
/* the PAIR preview: a two-man frame drawn over the class-0 template's victim
 * body for (this base, pose) - the same "you can only judge contact with both
 * bodies" lesson as the Calibrate tab. jobs/stock-class0/victmap.json maps
 * [victim id, holder row, holder pose]. */
static int pi_pair_victim(int base, int pose)
{
    static int loaded; static int vn; static uint16_t (*ve)[3];
    if (!loaded) {
        char err[128]; json_val *doc; const json_val *a;
        loaded = 1;
        doc = json_parse_file("jobs/stock-class0/victmap.json", err, sizeof err);
        if (!doc) return -1;
        a = json_get(doc, "entries");
        for (const json_val *e = a ? a->child : NULL; e; e = e->next) {
            if (e->type != JSON_ARRAY || e->n < 3) continue;
            ve = realloc(ve, sizeof *ve * (size_t)(vn + 1));
            ve[vn][0] = (uint16_t)json_int(json_at(e, 0), 0); ve[vn][1] = (uint16_t)json_int(json_at(e, 1), 0); ve[vn][2] = (uint16_t)json_int(json_at(e, 2), 0);
            vn++;
        }
        json_free(doc);
    }
    for (int k = 0; k < vn; k++) if (ve[k][1] == base && ve[k][2] == pose) return ve[k][0];
    return -1;
}
static const char *sk_png_under;   /* sk_png_tex: composite this canvas UNDER the frame (pair preview) */
static const char *sk_png_ghost;   /* sk_png_tex: blend this canvas OVER the frame at ~35%
                                      (the under-composite hid a SMALLER ref completely
                                      behind a 110% result - "ghost ref doesn't do anything") */
static int sk_png_canvas;          /* sk_png_tex: keep the WHOLE canvas - no alpha-bbox crop, no
                                      square letterbox. The crop normalised every figure to the
                                      same apparent size, hiding a skin's height_pct/width_pct
                                      (user 2026-08-28: "the 110%% difference must be visible") */
static time_t sk_mt_ref, sk_mt_anchor, sk_mt_portrait, sk_mt_frame;
static char sk_frame_name[64];

/* default prompt variables for a NEW skin: skins/defaults.json
 * overrides the compiled seed (Honky's proven text, user 2026-08-25) */
static const char *SK_DEF_CHAR =
    "an Elvis-impersonator wrestler (jet-black pompadour quiff, sideburns, "
    "red jumpsuit with white flared collar and gold trim, white boots), "
    "16-bit arcade pixel-art style";
static const char *SK_DEF_OUTL =
    "Outlines: black ONLY on the black hair; DARK RED on the red jumpsuit; "
    "exposed skin gets LOW CONTRAST medium warm orange outlines, never black.";
static void sk_defaults_load(void)
{
    char p[300], err[128]; json_val *doc;
    snprintf(sk_char, sizeof sk_char, "%s", SK_DEF_CHAR);
    snprintf(sk_outl, sizeof sk_outl, "%s", SK_DEF_OUTL);
    snprintf(p, sizeof p, "skins/defaults.json");
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) return;
    snprintf(sk_char, sizeof sk_char, "%s", json_str(json_get(doc, "character"), sk_char));
    snprintf(sk_outl, sizeof sk_outl, "%s", json_str(json_get(doc, "outlines"), sk_outl));
    json_free(doc);
}
static void sk_defaults_save(void)
{
    char p[300]; FILE *f;
    snprintf(p, sizeof p, "mods/%s/skins", mod_layer); mkdir_p(p);
    snprintf(p, sizeof p, "skins/defaults.json");
    f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "{\n  \"character\": "); json_write_string(f, sk_char);
    fprintf(f, ",\n  \"outlines\": "); json_write_string(f, sk_outl);
    fprintf(f, "\n}\n");
    fclose(f);
}
static char sk_mod[64];               /* the mod layer OWNING the loaded skin */
static char sk_snd[ENG_SND_N][48];    /* skin.json "sounds": per event cmd:0xNN / wav:name / "" = the base's */
static void sk_slug(void)             /* names are dir names AND shell args:
                                         lowercase, spaces -> '-', plain chars */
{
    int j = 0;
    for (int i = 0; sk_name[i] && j < (int)sizeof sk_name - 1; i++) {
        char c = sk_name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == ' ') c = '-';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            sk_name[j++] = c;
    }
    sk_name[j] = 0;
}
/* SKIN LIBRARY (user 2026-08-25: "how can we use skins across profiles?"):
 * skins live in the top-level `skins/<name>/`, independent of any profile
 * or mod; a slot in any profile dresses in one by name. Skins still found
 * under a mod layer (mods/<m>/skins/<name>, the old home) keep working. */
static int sk_resolve(const char *name, char *buf, size_t n)   /* 1 = found */
{
    struct stat st; char q[600];
    snprintf(buf, n, "skins/%s", name);
    snprintf(q, sizeof q, "%s/skin.json", buf);
    if (stat(q, &st) == 0) return 1;
    for (int m = wf_profile_nmods() - 1; m >= 0; m--) {
        snprintf(buf, n, "mods/%s/skins/%s", wf_profile_mod(m), name);
        snprintf(q, sizeof q, "%s/skin.json", buf);
        if (stat(q, &st) == 0) return 1;
    }
    snprintf(buf, n, "skins/%s", name);   /* a NEW skin: the library */
    return 0;
}
static void sk_dir(char *buf, size_t n)
{
    if (sk_mod[0]) snprintf(buf, n, "mods/%s/skins/%s", sk_mod, sk_name);
    else snprintf(buf, n, "skins/%s", sk_name);
}
static void sk_scan(void)             /* the library, then every mod layer of the profile */
{
    sk_nlist = 0;
    for (int m = -1; m < wf_profile_nmods(); m++) {
        char p[300]; DIR *d; struct dirent *e;
        if (m < 0) snprintf(p, sizeof p, "skins");
        else snprintf(p, sizeof p, "mods/%s/skins", wf_profile_mod(m));
        d = opendir(p);
        if (!d) continue;
        while ((e = readdir(d)) && sk_nlist < 32) {
            struct stat st; char q[560]; int dup = 0;
            if (e->d_name[0] == '.') continue;
            snprintf(q, sizeof q, "%s/%s/skin.json", p, e->d_name);
            if (stat(q, &st)) continue;
            for (int k = 0; k < sk_nlist; k++)
                if (!strcmp(sk_list[k], e->d_name)) dup = 1;
            if (!dup) snprintf(sk_list[sk_nlist++], 40, "%s", e->d_name);
        }
        closedir(d);
    }
}
static void sk_find_mod(const char *name)   /* which layer owns this skin ("" = the library) */
{
    char b[600];
    sk_mod[0] = 0;
    if (sk_resolve(name, b, sizeof b) && !strncmp(b, "mods/", 5)) {
        const char *e = strchr(b + 5, '/');
        snprintf(sk_mod, sizeof sk_mod, "%.*s", (int)(e - (b + 5)), b + 5);
    }
}
static void sk_save(void)
{
    char d[300], p[420]; FILE *f;
    sk_slug();
    if (!sk_name[0]) return;
    sk_dir(d, sizeof d); mkdir_p("skins"); mkdir_p(d);
    snprintf(p, sizeof p, "%s/skin.json", d);
    f = fopen(p, "w");
    if (f) {
        sk_base = sk_class_rep(sk_class);
        fprintf(f, "{\n  \"class\": %d,\n  \"base\": %d,\n  \"provider\": \"%s\",\n  \"height_pct\": %d,\n  \"width_pct\": %d,\n  \"build\": \"%s\",\n  \"character\": ", sk_class, sk_base, wf_art_provider_names[sk_provider], sk_height, sk_width, sk_build_anchor ? "anchor" : "mannequin");
        json_write_string(f, sk_char);
        fprintf(f, ",\n  \"outlines\": ");
        json_write_string(f, sk_outl);
        {   int any = 0;
            for (int e = 0; e < ENG_SND_N; e++) if (sk_snd[e][0]) any = 1;
            if (any) {
                int first = 1;
                fprintf(f, ",\n  \"sounds\": {");
                for (int e = 0; e < ENG_SND_N; e++) if (sk_snd[e][0]) { fprintf(f, "%s\"%s\": ", first ? "" : ", ", snd_ev_key[e]); json_write_string(f, sk_snd[e]); first = 0; }
                fprintf(f, "}");
            }
        }
        fprintf(f, "\n}\n");
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/character.txt", d);
    f = fopen(p, "w");
    if (f) { fprintf(f, "%s\nOUTLINES: %s\n", sk_char, sk_outl); fclose(f); }
    sk_pt_save(d);
    sk_scan();                         /* a NEW skin joins the list and is selected */
    for (int k = 0; k < sk_nlist; k++) if (!strcmp(sk_list[k], sk_name)) sk_sel = k;
}
static void sk_load(const char *name)
{
    char d[300], p[420], err[128]; json_val *doc;
    snprintf(sk_name, sizeof sk_name, "%s", name);
    sk_find_mod(name);
    sk_dir(d, sizeof d);
    snprintf(p, sizeof p, "%s/skin.json", d);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) return;
    sk_base = (int)json_int(json_get(doc, "base"), 8);
    sk_class = (int)json_int(json_get(doc, "class"), eng_ws_body_class(sk_base));   /* pre-V597 skins: the base's class */
    snprintf(sk_char, sizeof sk_char, "%s", json_str(json_get(doc, "character"), ""));
    snprintf(sk_outl, sizeof sk_outl, "%s", json_str(json_get(doc, "outlines"), ""));
    sk_height = (int)json_int(json_get(doc, "height_pct"), 100);
    sk_width  = (int)json_int(json_get(doc, "width_pct"), 100);
    sk_build_anchor = !strcmp(json_str(json_get(doc, "build"), "mannequin"), "anchor");
    {   const char *pv = json_str(json_get(doc, "provider"), "codex");
        sk_provider = 0;
        for (int k = 0; wf_art_provider_names[k]; k++) if (!strcmp(pv, wf_art_provider_names[k])) sk_provider = k; }
    for (int e = 0; e < ENG_SND_N; e++) snprintf(sk_snd[e], sizeof sk_snd[e], "%s", json_str(json_get(json_get(doc, "sounds"), snd_ev_key[e]), ""));
    json_free(doc);
    sk_pt_load(d);
    sk_mt_ref = sk_mt_anchor = sk_mt_portrait = sk_mt_frame = 0;   /* re-read previews */
    sk_rev_page = 0;
    for (int k = 0; k < SK_REV_N; k++) sk_rev_mt[k] = 0;
}
/* PNG -> streaming texture, refreshed when the file's mtime moves */
/* PROMPT TEMPLATES (user 2026-08-25: each recipe editable): defaults from
 * `--art-prompt-defaults`, a skin's prompts.json overrides; the edit boxes
 * show a soft-WRAPPED copy (Nuklear does not wrap) and the canonical
 * single-line text is rebuilt when a box loses focus. */
#define SK_PK_N 9
static const char *sk_pk_name[SK_PK_N] = { "anchor", "select", "sheet", "frag_sheet", "single", "frag_single", "cards", "cont", "title" };
static char sk_pt[SK_PK_N][4096], sk_pdef[SK_PK_N][4096], sk_ptw[SK_PK_N][4800], sk_ptitle[SK_PK_N][200];
static int sk_pt_loaded, sk_pt_active[SK_PK_N], sk_pt_cols;

static void sk_pt_wrap(int k, int cols)
{
    const char *t = sk_pt[k]; size_t o = 0; int col = 0;
    while (*t && o < sizeof sk_ptw[k] - 2) {
        const char *e = t; int wl;
        while (*e && *e != ' ') e++;
        wl = (int)(e - t);
        if (col && col + 1 + wl > cols) { sk_ptw[k][o++] = '\n'; col = 0; }
        else if (col) { sk_ptw[k][o++] = ' '; col++; }
        memcpy(sk_ptw[k] + o, t, (size_t)wl); o += (size_t)wl; col += wl;
        t = *e ? e + 1 : e;
    }
    sk_ptw[k][o] = 0;
}
static void sk_pt_unwrap(int k)         /* box text -> canonical: newlines become spaces */
{
    const char *t = sk_ptw[k]; size_t o = 0; int sp = 0;
    while (*t && o < sizeof sk_pt[k] - 1) {
        if (*t == '\n' || *t == ' ') { if (!sp && o) { sk_pt[k][o++] = ' '; sp = 1; } }
        else { sk_pt[k][o++] = *t; sp = 0; }
        t++;
    }
    while (o && sk_pt[k][o-1] == ' ') o--;
    sk_pt[k][o] = 0;
}
static void sk_pt_defaults(void)
{
    char err[96]; json_val *doc;
    if (sk_pt_loaded) return;
    if (system("./wfengine --art-prompt-defaults jobs/.prompt_defaults.json >/dev/null 2>&1") != 0) return;
    doc = json_parse_file("jobs/.prompt_defaults.json", err, sizeof err);
    if (!doc) return;
    for (int k = 0; k < SK_PK_N; k++) {
        char tk[48];
        snprintf(sk_pdef[k], sizeof sk_pdef[k], "%s", json_str(json_get(doc, sk_pk_name[k]), ""));
        snprintf(tk, sizeof tk, "%s_title", sk_pk_name[k]);
        snprintf(sk_ptitle[k], sizeof sk_ptitle[k], "%s", json_str(json_get(doc, tk), sk_pk_name[k]));
        snprintf(sk_pt[k], sizeof sk_pt[k], "%s", sk_pdef[k]);
    }
    json_free(doc);
    sk_pt_loaded = 1; sk_pt_cols = 0;
}
static void sk_pt_load(const char *dir)   /* defaults, then the skin's prompts.json */
{
    char jp[420], err[96]; json_val *doc;
    sk_pt_defaults();
    for (int k = 0; k < SK_PK_N; k++) snprintf(sk_pt[k], sizeof sk_pt[k], "%s", sk_pdef[k]);
    snprintf(jp, sizeof jp, "%s/prompts.json", dir);
    doc = json_parse_file(jp, err, sizeof err);
    if (doc) {
        for (int k = 0; k < SK_PK_N; k++) {
            const char *t = json_str(json_get(doc, sk_pk_name[k]), NULL);
            if (t && t[0]) snprintf(sk_pt[k], sizeof sk_pt[k], "%s", t);
        }
        json_free(doc);
    }
    sk_pt_cols = 0;                    /* re-wrap */
}
static void sk_pt_save(const char *dir)   /* only the recipes that differ from the default */
{
    char jp[420]; FILE *f; int n = 0;
    for (int k = 0; k < SK_PK_N; k++) if (strcmp(sk_pt[k], sk_pdef[k])) n++;
    snprintf(jp, sizeof jp, "%s/prompts.json", dir);
    if (!n) { unlink(jp); return; }
    f = fopen(jp, "w");
    if (!f) return;
    fprintf(f, "{\n");
    for (int k = 0, w = 0; k < SK_PK_N; k++) if (strcmp(sk_pt[k], sk_pdef[k])) {
        fprintf(f, "%s  \"%s\": ", w++ ? ",\n" : "", sk_pk_name[k]); json_write_string(f, sk_pt[k]);
    }
    fprintf(f, "\n}\n");
    fclose(f);
}
static int sk_png_gray;               /* sk_png_tex: convert to luma (the ref as sent) */
static int sk_png_flip;               /* sk_png_tex: mirror horizontally (alias of a flipped source) */
static int sk_png_plain;              /* sk_png_tex: no square letterbox (texture = image size) */
static void sk_png_tex(const char *path, SDL_Texture **tex, time_t *mt)
{
    struct stat st; uint8_t *rgba; int w, h;
    if (stat(path, &st)) {             /* gone (new skin, deleted out): DROP the
                                          stale preview instead of keeping the
                                          previous skin's (user 2026-08-25) */
        if (*tex) { SDL_DestroyTexture(*tex); *tex = NULL; }
        *mt = 0;
        return;
    }
    if (*tex && st.st_mtime == *mt) return;
    if (wf_video_load_rgba_png(path, &rgba, &w, &h)) return;
    if (sk_png_flip)                   /* an alias drawn from its mirrored source (Poses tab) */
        for (int y = 0; y < h; y++) {
            uint8_t *row = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w / 2; x++) { uint8_t t[4]; memcpy(t, row + x * 4, 4); memcpy(row + x * 4, row + (w - 1 - x) * 4, 4); memcpy(row + (w - 1 - x) * 4, t, 4); }
        }
    if (sk_png_gray) {                 /* show the GREY mannequin the model gets, not
                                          the colour ref (user 2026-08-25): the same
                                          RANKED grey art_run.c sends (2026-08-28) */
        extern void wf_art_gray_ranked(uint8_t *rgba, int w, int h);
        wf_art_gray_ranked(rgba, w, h);
    }
    if (sk_png_under) {                /* pair preview: the victim canvas under the frame */
        uint8_t *u; int uw, uh;
        if (wf_video_load_rgba_png(sk_png_under, &u, &uw, &uh) == 0) {
            if (uw == w && uh == h)
                for (int i = 0; i < w * h; i++) {
                    uint8_t *q = rgba + (size_t)i * 4; const uint8_t *s = u + (size_t)i * 4;
                    if (q[3] < 128 && s[3] >= 128) { q[0] = s[0]; q[1] = s[1]; q[2] = s[2]; q[3] = 255; }
                }
            free(u);
        }
    }
    if (sk_png_ghost) {                /* ghost the ref OVER the result at ~35% */
        uint8_t *u; int uw, uh;
        if (wf_video_load_rgba_png(sk_png_ghost, &u, &uw, &uh) == 0) {
            if (uw == w && uh == h)
                for (int i = 0; i < w * h; i++) {
                    uint8_t *q = rgba + (size_t)i * 4; const uint8_t *s = u + (size_t)i * 4;
                    if (s[3] >= 128) {
                        uint8_t l = (uint8_t)((299 * s[0] + 587 * s[1] + 114 * s[2]) / 1000);
                        if (q[3] < 128) {          /* outside the result: the ghost alone,
                                                      a clear red silhouette */
                            q[0] = (uint8_t)(96 + l / 2); q[1] = (uint8_t)(l / 3);
                            q[2] = (uint8_t)(l / 3); q[3] = 185;
                        } else {                   /* overlap: strong red-tinted mix */
                            q[0] = (uint8_t)((q[0] + 255) / 2);
                            q[1] = (uint8_t)((q[1] * 5 + l * 3) / 8);
                            q[2] = (uint8_t)((q[2] * 5 + l * 3) / 8);
                        }
                    }
                }
            free(u);
        }
    }
    if (*tex) { SDL_DestroyTexture(*tex); *tex = NULL; }
    if (sk_png_canvas == 1 && w == 256 && h == 320) {
        /* grid tiles: a FIXED shared window of the canvas (x 40..216,
           y 40..260 - the band figures live in) - every frame gets the
           SAME crop so relative scale stays truthful, but ~1.6x bigger
           than the whole canvas ("very small in the boxes"). 176x220 =
           the 88x110 cell aspect. The modal (mode 2) keeps everything. */
        uint8_t *cr = malloc((size_t)176 * 220 * 4);
        if (cr) {
            for (int y = 0; y < 220; y++)
                memcpy(cr + (size_t)y * 176 * 4,
                       rgba + (((size_t)(40 + y) * 256) + 40) * 4, (size_t)176 * 4);
            free(rgba); rgba = cr; w = 176; h = 220;
        }
    }
    if (!sk_png_canvas) {   /* crop to the figure's alpha bbox first: the ref canvas and the
           hi-res anchor then preview at comparable figure scale */
        int x0 = w, y0 = h, x1 = 0, y1 = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (rgba[((size_t)y * w + x) * 4 + 3]) {
                    if (x < x0) x0 = x;
                    if (x + 1 > x1) x1 = x + 1;
                    if (y < y0) y0 = y;
                    if (y + 1 > y1) y1 = y + 1;
                }
        if (x1 > x0 && y1 > y0 && (x1 - x0 < w || y1 - y0 < h)) {
            int nw2 = x1 - x0, nh2 = y1 - y0;
            uint8_t *cr = malloc((size_t)nw2 * nh2 * 4);
            if (cr) {
                for (int y = 0; y < nh2; y++)
                    memcpy(cr + (size_t)y * nw2 * 4,
                           rgba + (((size_t)(y0 + y) * w) + x0) * 4, (size_t)nw2 * 4);
                free(rgba); rgba = cr; w = nw2; h = nh2;
            }
        }
    }
    {   /* letterbox into a SQUARE texture so nk_image cannot stretch it
           (sk_png_plain = 1: the texture keeps the image's own size - the
           caller lays the widget out at that aspect; Weapons/Classes tabs) */
        int side = (sk_png_plain || sk_png_canvas) ? 0 : (w > h ? w : h), sw = side ? side : w, sh = side ? side : h;
        uint8_t *sq = calloc((size_t)sw * sh, 4);
        if (sq) {
            int ox = (sw - w) / 2, oy = (sh - h) / 2;
            for (int y = 0; y < h; y++)
                memcpy(sq + (((size_t)(oy + y) * sw) + ox) * 4,
                       rgba + (size_t)y * w * 4, (size_t)w * 4);
            *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                     SDL_TEXTUREACCESS_STREAMING, sw, sh);
            if (*tex) {
                SDL_UpdateTexture(*tex, NULL, sq, sw * 4);
                SDL_SetTextureBlendMode(*tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(*tex, SDL_ScaleModeNearest);
                *mt = st.st_mtime;
            }
            free(sq);
        }
    }
    free(rgba);
}
/* gated pipeline button: dimmed until its prerequisites exist, but still
 * CLICKABLE — a press on a gated button logs WHY instead of doing nothing
 * (nuklear's disabled widgets swallow clicks silently; user 2026-08-25:
 * "i pressed 1 refs - nothing happened") */
/* the button that launched the running tool shows a spinner (user
 * 2026-08-26); matched by its label with digits stripped, since the
 * "(N missing)" count changes while the run progresses */
static char sk_running_key[64];
static void sk_label_key(const char *label, char *key, size_t n)
{
    size_t o = 0;
    for (; *label && o < n - 1; label++) if (*label < '0' || *label > '9') key[o++] = *label;
    key[o] = 0;
}
static int sk_button_gated(const char *label, int enabled, const char *why)
{
    char key[64];
    sk_label_key(label, key, sizeof key);
    if (tool_busy && sk_running_key[0] && !strcmp(key, sk_running_key)) {
        static const char spin[4] = { '|', '/', '-', '\\' };
        struct nk_style_button run = ctx->style.button; char lb[96];
        run.normal = run.hover = run.active = nk_style_item_color(nk_rgb(40, 70, 110));
        run.text_normal = run.text_hover = run.text_active = nk_rgb(200, 230, 255);
        snprintf(lb, sizeof lb, "%c  %s  %c", spin[(SDL_GetTicks() / 150) & 3], label, spin[(SDL_GetTicks() / 150) & 3]);
        nk_button_label_styled(ctx, &run, lb);
        return 0;
    }
    if (!tool_busy) sk_running_key[0] = 0;
    if (!enabled) {
        struct nk_style_button dim = ctx->style.button;
        dim.text_normal = dim.text_hover = dim.text_active = nk_rgb(110, 110, 120);
        dim.normal = dim.hover = dim.active = nk_style_item_color(nk_rgb(38, 38, 44));
        if (nk_button_label_styled(ctx, &dim, label))
            logf_("%s: not yet — %s", label, why);
        return 0;
    }
    if (nk_button_label(ctx, label)) { snprintf(sk_running_key, sizeof sk_running_key, "%s", key); return 1; }
    return 0;
}
static int sk_have(const char *fmt, ...)
{
    char p[560]; struct stat st; va_list ap;
    va_start(ap, fmt); vsnprintf(p, sizeof p, fmt, ap); va_end(ap);
    return stat(p, &st) == 0;
}
static int sk_run_alive(void)      /* an --art-run that survived an editor
                                      close (jobs/<skin>/run.lock, live pid) */
{
    char p[360]; FILE *f; long pid = 0;
    snprintf(p, sizeof p, "jobs/%s/run.lock", sk_name);
    f = fopen(p, "r");
    if (!f) return 0;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);
    return pid > 0 && kill((pid_t)pid, 0) == 0;
}
static void sk_newest_frame(char *out, size_t n)
{
    char p[360]; DIR *d; struct dirent *e; time_t best = 0;
    out[0] = 0;
    snprintf(p, sizeof p, "jobs/%s/out", sk_name);
    d = opendir(p);
    if (!d) return;
    while ((e = readdir(d))) {
        char q[520]; struct stat st;
        if (strncmp(e->d_name, "pose_", 5)) continue;
        snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        if (stat(q, &st) == 0 && st.st_mtime >= best) {
            best = st.st_mtime;
            snprintf(out, n, "%s", q);
        }
    }
    closedir(d);
}
static const char *sk_next = "";
static int sk_sub = 1;                 /* Skins sub-tab: 0 Info, 1 Artwork, 2 Poses, 3 AI Recipe (user 2026-08-30: the prompt variables moved off the info page) */
static int sk_miss_frames, sk_miss_anim, sk_miss_title;   /* per art category, from the review scan */      /* the pipeline's next step, shown on the run-status line */
static int sk_open;                    /* a skin (or '+ new skin') was clicked: the right pane shows it (user 2026-08-27: blank until then) */
static void skin_delete(const char *nm)
{
    char sd[300], cmd[800];
    if (sk_resolve(nm, sd, sizeof sd) && nm[0] && !strchr(nm, '/') && !strstr(nm, "..")) {
        /* the skin dir + its generation job; slots wearing it keep their ingested art */
        snprintf(cmd, sizeof cmd, "rm -rf \"%s\" \"jobs/%s\"", sd, nm);
        if (system(cmd) == 0) logf_("deleted skin '%s' (%s + jobs/%s) - slots already wearing it keep their art", nm, sd, nm);
        else logf_("delete failed (%s)", sd);
        if (sk_sel >= 0 && sk_sel < sk_nlist && !strcmp(sk_list[sk_sel], nm)) { sk_sel = -1; sk_name[0] = 0; sk_open = 0; }
    } else logf_("cannot find skin '%s'", nm);
}
/* ---- POSES sub-tab (user 2026-08-28): the skin's frames split into the
 * GENERIC set (every skin needs it) and the MOVE poses (needed only when a
 * move is mapped on a slot wearing this skin), from data/moveposes.json
 * (--move-poses). Counts are UNIQUE DRAWINGS: an id counts only if the job
 * has a ref for it (the Refs step unlinks aliases), so double-ups are never
 * generated; "have" = out/ frame exists. Generate = queue the missing ids
 * into the re-roll queue (singles, codex) and run it. */
typedef struct { char kind[10]; int id; char name[40]; int generic; int users[12], nusers; int *pose[ENG_BODY_CLASSES]; int np[ENG_BODY_CLASSES]; } mp_anim_t;
static mp_anim_t *mp_anim; static int mp_n = -1;
static void move_names_apply(void);
static void mp_load(void)
{
    char err[128]; json_val *doc = json_parse_file("data/moveposes.json", err, sizeof err);
    mp_n = 0;
    if (!doc) { logf_("data/moveposes.json: %s (run ./wfengine --move-poses data/moveposes.json)", err); return; }
    for (const json_val *a = json_get(doc, "animations") ? json_get(doc, "animations")->child : NULL; a; a = a->next) {
        mp_anim_t *m;
        mp_anim = realloc(mp_anim, sizeof(mp_anim_t) * (size_t)(mp_n + 1));
        m = &mp_anim[mp_n++]; memset(m, 0, sizeof *m);
        snprintf(m->kind, sizeof m->kind, "%s", json_str(json_get(a, "kind"), ""));
        m->id = (int)json_int(json_get(a, "id"), -1);
        snprintf(m->name, sizeof m->name, "%s", json_str(json_get(a, "name"), ""));
        m->generic = json_get(a, "generic") && json_get(a, "generic")->type == JSON_BOOL && json_int(json_get(a, "generic"), 0);
        for (const json_val *u = json_get(a, "users") ? json_get(a, "users")->child : NULL; u && m->nusers < 12; u = u->next) m->users[m->nusers++] = (int)json_int(u, -1);
        for (int c = 0; c < ENG_BODY_CLASSES; c++) {
            char key[4]; int n = 0; snprintf(key, sizeof key, "%d", c);
            const json_val *pl = json_get(json_get(a, "poses"), key), *vl = json_get(json_get(a, "victims"), key);
            for (const json_val *q = pl ? pl->child : NULL; q; q = q->next) n++;
            for (const json_val *q = vl ? vl->child : NULL; q; q = q->next) n++;
            m->pose[c] = malloc(sizeof(int) * (size_t)(n ? n : 1)); m->np[c] = 0;
            for (const json_val *q = pl ? pl->child : NULL; q; q = q->next) m->pose[c][m->np[c]++] = (int)json_int(q, 0);
            for (const json_val *q = vl ? vl->child : NULL; q; q = q->next) m->pose[c][m->np[c]++] = (int)json_int(q, 0);
        }
    }
    json_free(doc);
    if (pi_n < 0) pi_load();
    move_names_apply();
}
/* MOVE NAMES are global (data/move-names.json, the overlay --move-catalog
 * merges): the editor applies it over every loaded table and a right-click
 * Rename writes it back (user 2026-08-28) */
static void move_names_apply(void)
{
    char err[128]; json_val *doc = json_parse_file("data/move-names.json", err, sizeof err);
    if (!doc) return;
    for (const json_val *e = doc->child; e; e = e->next) {
        int id; const char *nm;
        if (!e->key || e->type != JSON_STRING || !strcmp(e->key, "note")) continue;
        id = atoi(e->key); nm = json_str(e, "");
        if (!nm[0]) continue;
        for (int a = 0; a < mp_n; a++) if (!strcmp(mp_anim[a].kind, "move") && mp_anim[a].id == id) snprintf(mp_anim[a].name, sizeof mp_anim[a].name, "%s", nm);
        for (int a = 0; a < pi_n; a++) if (!strcmp(pi_anim[a].kind, "move") && pi_anim[a].id == id) snprintf(pi_anim[a].name, sizeof pi_anim[a].name, "%s", nm);
    }
    json_free(doc);
}
static void move_name_save(int id, const char *name)
{
    char err[128]; json_val *doc = json_parse_file("data/move-names.json", err, sizeof err); FILE *f; char key[16]; int wrote = 0;
    snprintf(key, sizeof key, "%d", id);
    f = fopen("data/move-names.json", "w");
    if (!f) { logline("cannot write data/move-names.json"); if (doc) json_free(doc); return; }
    fprintf(f, "{\n \"note\": ");
    json_write_string(f, doc && json_get(doc, "note") ? json_str(json_get(doc, "note"), "") : "move names (global overlay; merged into data/movecatalog.json by --move-catalog)");
    for (const json_val *e = doc ? doc->child : NULL; e; e = e->next) {
        if (!e->key || !strcmp(e->key, "note") || e->type != JSON_STRING) continue;
        fprintf(f, ",\n \"%s\": ", e->key);
        if (!strcmp(e->key, key)) { json_write_string(f, name); wrote = 1; } else json_write_string(f, json_str(e, ""));
    }
    if (!wrote) { fprintf(f, ",\n \"%s\": ", key); json_write_string(f, name); }
    fprintf(f, "\n}\n");
    fclose(f);
    if (doc) json_free(doc);
    logf_("move 0x%02X named '%s' (data/move-names.json - global)", id, name);
    move_names_apply();
}
/* right-click on a move row: rename (global) */
static void move_rename_popup(struct nk_rect b, int anim)
{
    if (anim < 0 || anim >= mp_n || strcmp(mp_anim[anim].kind, "move")) return;
    if (nk_contextual_begin(ctx, 0, nk_vec2(340, 60), b)) {
        static int rn_for = -1; static char rn_buf[40];
        if (rn_for != anim) { rn_for = anim; snprintf(rn_buf, sizeof rn_buf, "%s", mp_anim[anim].name); }
        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        nk_layout_row_push(ctx, 230); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, rn_buf, sizeof rn_buf, nk_filter_default);
        nk_layout_row_push(ctx, 80);
        if (nk_button_label(ctx, "Rename") && rn_buf[0]) { move_name_save(mp_anim[anim].id, rn_buf); rn_for = -1; nk_contextual_close(ctx); }
        nk_layout_row_end(ctx);
        nk_contextual_end(ctx);
    }
}
/* CLASS ALIAS OVERRIDE from a tile's right-click (user 2026-08-28: "1037 and
 * 1370 look the same"): 'alias of N' appends {id: {of, flip 0, dx, dy}} to
 * data/generics/C/aliases.override.json "add" (dx/dy = the bbox offset, the
 * shape the builder uses); 'un-alias' appends the id to "reject". Verify
 * re-reads. */
static int png_bbox(const char *path, int *x0, int *y0)
{
    uint8_t *px; int w, h, mx = 1 << 30, my = 1 << 30;
    if (wf_video_load_rgba_png(path, &px, &w, &h)) return -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (px[((size_t)y*w + x)*4 + 3]) { if (x < mx) mx = x; if (y < my) my = y; }
    free(px); *x0 = mx; *y0 = my; return mx == 1 << 30 ? -1 : 0;
}
static void cs_override_write2(int cls, int add_id, int add_of, int add_flip, int reject_id, int keep_a, int keep_b)
{
    char p[300], err[128]; json_val *doc; FILE *f; int first;
    snprintf(p, sizeof p, "data/generics/%d/aliases.override.json", cls);
    doc = json_parse_file(p, err, sizeof err);
    f = fopen(p, "w");
    if (!f) { logf_("cannot write %s", p); if (doc) json_free(doc); return; }
    fprintf(f, "{\n \"reject\": ["); first = 1;
    for (const json_val *r = doc && json_get(doc, "reject") ? json_get(doc, "reject")->child : NULL; r; r = r->next) { fprintf(f, "%s%lld", first ? "" : ", ", (long long)json_int(r, -1)); first = 0; }
    if (reject_id >= 0) { fprintf(f, "%s%d", first ? "" : ", ", reject_id); first = 0; }
    fprintf(f, "],\n \"keep\": ["); first = 1;   /* pairs reviewed as 'keep both' (the dedupe pass skips them) */
    for (const json_val *k = doc && json_get(doc, "keep") ? json_get(doc, "keep")->child : NULL; k; k = k->next) { fprintf(f, "%s[%lld, %lld]", first ? "" : ", ", (long long)json_int(json_at(k, 0), -1), (long long)json_int(json_at(k, 1), -1)); first = 0; }
    if (keep_a >= 0) { fprintf(f, "%s[%d, %d]", first ? "" : ", ", keep_a, keep_b); first = 0; }
    fprintf(f, "],\n \"add\": {"); first = 1;
    for (const json_val *e = doc && json_get(doc, "add") ? json_get(doc, "add")->child : NULL; e; e = e->next) {
        if (add_id >= 0 && atoi(e->key) == add_id) continue;
        if (reject_id >= 0 && atoi(e->key) == reject_id) continue;
        fprintf(f, "%s\n  \"%s\": {\"of\": %lld, \"flip\": %lld, \"dx\": %lld, \"dy\": %lld}", first ? "" : ",", e->key,
                (long long)json_int(json_get(e, "of"), -1), (long long)json_int(json_get(e, "flip"), 0), (long long)json_int(json_get(e, "dx"), 0), (long long)json_int(json_get(e, "dy"), 0)); first = 0;
    }
    if (add_id >= 0) {
        char a[300], b[300]; int ax = 0, ay = 0, bx = 0, by = 0;
        snprintf(a, sizeof a, "data/generics/%d/frames/pose_%04d.png", cls, add_id);
        snprintf(b, sizeof b, "data/generics/%d/frames/pose_%04d.png", cls, add_of);
        if (png_bbox(a, &ax, &ay) || png_bbox(b, &bx, &by)) { ax = ay = bx = by = 0; }
        fprintf(f, "%s\n  \"%d\": {\"of\": %d, \"flip\": %d, \"dx\": %d, \"dy\": %d}", first ? "" : ",", add_id, add_of, add_flip ? 1 : 0, ax - bx, ay - by);
    }
    fprintf(f, "\n }\n}\n");
    fclose(f);
    if (doc) json_free(doc);
    logf_("%s updated - press Verify (status) to apply", p);
}
static void cs_override_write(int cls, int add_id, int add_of, int add_flip, int reject_id) { cs_override_write2(cls, add_id, add_of, add_flip, reject_id, -1, -1); }
/* the dedupe pass's NEAR pairs (data/generics/C/dupes.json) for the side-by-side review */
static int dd_a[400], dd_b[400], dd_flip[400]; static float dd_iou[400], dd_pct[400]; static int dd_n = -1, dd_cls = -1;
static void dd_load_pairs(int cls)
{
    char p[300], err[128]; json_val *doc;
    dd_n = 0; dd_cls = cls;
    snprintf(p, sizeof p, "data/generics/%d/dupes.json", cls);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) return;
    for (const json_val *e = json_get(doc, "pairs") ? json_get(doc, "pairs")->child : NULL; e && dd_n < 400; e = e->next) {
        dd_a[dd_n] = (int)json_int(json_get(e, "a"), -1); dd_b[dd_n] = (int)json_int(json_get(e, "b"), -1); dd_flip[dd_n] = (int)json_int(json_get(e, "flip"), 0);
        dd_iou[dd_n] = json_get(e, "iou") ? (float)json_get(e, "iou")->num : 0; dd_pct[dd_n] = json_get(e, "diff_pct") ? (float)json_get(e, "diff_pct")->num : 0;
        dd_n++;
    }
    json_free(doc);
}
/* per-skin frame presence, rescanned when out/ or ref/ change (like the review cache) */
static unsigned char sp_ref[2048], sp_out[2048]; static char sp_skin[40]; static long long sp_mt; static Uint32 sp_t;
static void sp_scan(void)
{
    char rp[400]; struct stat so, sr; long long mt = 0; DIR *rd; struct dirent *re; int pn;
    snprintf(rp, sizeof rp, "jobs/%s/out", sk_name); if (stat(rp, &so) == 0) mt += (long long)so.st_mtim.tv_sec * 1000000000LL + so.st_mtim.tv_nsec;
    snprintf(rp, sizeof rp, "jobs/%s/ref", sk_name); if (stat(rp, &sr) == 0) mt += (long long)sr.st_mtim.tv_sec * 1000000000LL + sr.st_mtim.tv_nsec;
    if (!strcmp(sp_skin, sk_name) && mt == sp_mt && SDL_GetTicks() - sp_t < 1000) return;
    if (!strcmp(sp_skin, sk_name) && mt == sp_mt) { sp_t = SDL_GetTicks(); return; }
    memset(sp_ref, 0, sizeof sp_ref); memset(sp_out, 0, sizeof sp_out);
    snprintf(rp, sizeof rp, "jobs/%s/ref", sk_name);
    if ((rd = opendir(rp))) { while ((re = readdir(rd))) if (sscanf(re->d_name, "pose_%d.png", &pn) == 1 && pn >= 0 && pn < 2048) sp_ref[pn] = 1; closedir(rd); }
    snprintf(rp, sizeof rp, "jobs/%s/out", sk_name);
    if ((rd = opendir(rp))) { while ((re = readdir(rd))) if (sscanf(re->d_name, "pose_%d.png", &pn) == 1 && pn >= 0 && pn < 2048) sp_out[pn] = 1; closedir(rd); }
    snprintf(sp_skin, sizeof sp_skin, "%s", sk_name); sp_mt = mt; sp_t = SDL_GetTicks();
}
/* the moves mapped on every slot wearing this skin (mods/<m>/wrestlers/NN):
 * move id -> bitmask of wearers; wearer names for the "used by" label */
static unsigned char sp_mapped[256]; static char sp_wear[8][24]; static int sp_nwear; static Uint32 sp_wear_t; static char sp_wear_skin[40];
static void sp_wearers(void)
{
    if (!strcmp(sp_wear_skin, sk_name) && SDL_GetTicks() - sp_wear_t < 3000) return;
    memset(sp_mapped, 0, sizeof sp_mapped); sp_nwear = 0;
    for (int m2 = 0; m2 < wf_profile_nmods(); m2++)
        for (int sl = ENG_WS_MAX; sl < ENG_WS_EXT_MAX && sp_nwear < 8; sl++) {
            char wj[560], err[96]; json_val *doc, *sd;
            snprintf(wj, sizeof wj, "mods/%s/wrestlers/%02d/wrestler.json", wf_profile_mod(m2), sl);
            doc = json_parse_file(wj, err, sizeof err);
            if (!doc) continue;
            if (!strcmp(json_str(json_get(doc, "skin"), ""), sk_name)) {
                snprintf(sp_wear[sp_nwear], sizeof sp_wear[0], "%s (%d)", json_str(json_get(doc, "name"), "?"), sl);
                snprintf(wj, sizeof wj, "mods/%s/wrestlers/%02d/stats.json", wf_profile_mod(m2), sl);
                sd = json_parse_file(wj, err, sizeof err);
                if (sd) {
                    for (const json_val *r = json_get(json_get(sd, "move_map"), "rows") ? json_get(json_get(sd, "move_map"), "rows")->child : NULL; r; r = r->next)
                        for (const json_val *c = r->child; c; c = c->next) { int mv = (int)json_int(c, 255); if (mv >= 0 && mv < 255) sp_mapped[mv] |= (unsigned char)(1 << sp_nwear); }
                    json_free(sd);
                }
                sp_nwear++;
            }
            json_free(doc);
        }
    snprintf(sp_wear_skin, sizeof sp_wear_skin, "%s", sk_name); sp_wear_t = SDL_GetTicks();
}
/* counts for a set of ids: unique = has a ref (not an alias), have = out exists */
static void sp_count(const int *ids, int n, unsigned char *seen, int *unique, int *have, int *alias)
{
    for (int k = 0; k < n; k++) {
        int id = ids[k];
        if (id < 0 || id >= 2048 || seen[id]) continue;
        seen[id] = 1;
        if (sp_ref[id]) { (*unique)++; if (sp_out[id]) (*have)++; }
        else if (sp_out[id]) { (*unique)++; (*have)++; }   /* drawn even though no ref remains */
        else (*alias)++;
    }
}
static void sp_queue_missing(const int *ids, int n, unsigned char *seen)
{
    int q = 0;
    for (int k = 0; k < n; k++) {
        int id = ids[k];
        if (id < 0 || id >= 2048 || seen[id]) continue;
        seen[id] = 1;
        if (sp_ref[id] && !sp_out[id] && sk_rq_find(id) < 0 && sk_rq_n < SK_RQ_MAX) { sk_rq_pose[sk_rq_n] = id; sk_rq_extra[sk_rq_n][0] = 0; sk_rq_n++; q++; }
    }
    sk_rq_save();
    logf_("poses: %d frame(s) queued", q);
}
/* frame STRIP per animation (user 2026-08-28: "horizontal scrolling frames
 * per move"): the pose-index flipbook order (data/poseindex.json) when it
 * names the animation, else the move table's order; victims (1024+) after
 * the singles. Tiles look like the Artwork grid (colour = drawn, grey
 * mannequin in a red frame = missing); click = zoom, right-click = queue. */
static int sp_show_alias;          /* grid: SHOW the aliased frames, drawn from their source (off by default, user 2026-08-28) */
static int sp_al_of[2048], sp_al_flip[2048]; static char sp_al_skin[40]; static int sp_al_loaded;
static void sp_alias_load(void)    /* the job's manifest (copied from the class template): id -> {of, flip} */
{
    char p[300], err[128]; json_val *doc;
    if (sp_al_loaded && !strcmp(sp_al_skin, sk_name)) return;
    for (int i = 0; i < 2048; i++) sp_al_of[i] = -1;
    snprintf(p, sizeof p, "jobs/%s/manifest.json", sk_name);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) { snprintf(p, sizeof p, "data/generics/%d/manifest.json", sk_class); doc = json_parse_file(p, err, sizeof err); }
    if (doc) {
        for (const json_val *e = json_get(doc, "aliases") ? json_get(doc, "aliases")->child : NULL; e; e = e->next) {
            int id = atoi(e->key);
            if (id >= 0 && id < 2048) { sp_al_of[id] = (int)json_int(json_get(e, "of"), -1); sp_al_flip[id] = (int)json_int(json_get(e, "flip"), 0); }
        }
        json_free(doc);
    }
    snprintf(sp_al_skin, sizeof sp_al_skin, "%s", sk_name); sp_al_loaded = 1;
}
#define SP_TEX_N 768               /* > the biggest pose set: an LRU smaller than a set reloads every frame (flashing, user 2026-08-28) */
static SDL_Texture *sp_tex[SP_TEX_N]; static char sp_tex_key[SP_TEX_N][200]; static time_t sp_tex_mt[SP_TEX_N]; static Uint32 sp_tex_use[SP_TEX_N];
static SDL_Texture *sp_tile_tex(const char *path, const char *under, int gray, int flip)
{
    int k, lru = 0; char key[200];
    snprintf(key, sizeof key, "%s%s", flip ? "F:" : "", path);   /* a flipped copy is its own texture */
    for (k = 0; k < SP_TEX_N; k++) if (!strcmp(sp_tex_key[k], key)) break;
    if (k == SP_TEX_N) {
        for (k = 1; k < SP_TEX_N; k++) if (sp_tex_use[k] < sp_tex_use[lru]) lru = k;
        k = lru; snprintf(sp_tex_key[k], sizeof sp_tex_key[0], "%s", key); sp_tex_mt[k] = 0;
        if (sp_tex[k]) { SDL_DestroyTexture(sp_tex[k]); sp_tex[k] = NULL; }
    }
    sp_tex_use[k] = SDL_GetTicks();
    sk_png_gray = gray; sk_png_under = under; sk_png_canvas = 0; sk_png_flip = flip;   /* bbox-cropped like the grid */
    sk_png_tex(path, &sp_tex[k], &sp_tex_mt[k]);
    sk_png_canvas = 0; sk_png_under = NULL; sk_png_gray = 0; sk_png_flip = 0;
    return sp_tex[k];
}
static void sp_grid(const char *kind, int id, const int *ids, int n, int cols)
{
    static int order[700]; int no = 0;
    if (pi_n < 0) pi_load();
    for (int a = 0; a < pi_n; a++)         /* flipbook order first */
        if (!strcmp(pi_anim[a].kind, kind) && pi_anim[a].id == id)
            for (int f = 0; f < pi_anim[a].n && no < 700; f++) { int v = pi_anim[a].pose[f]; int dup = 0; for (int q = 0; q < no; q++) if (order[q] == v) dup = 1; if (!dup) order[no++] = v; }
    for (int k = 0; k < n && no < 700; k++) { int dup = 0; for (int q = 0; q < no; q++) if (order[q] == ids[k]) dup = 1; if (!dup) order[no++] = ids[k]; }
    if (!sp_show_alias) {              /* drop the alias placeholders */
        int w = 0;
        for (int k = 0; k < no; k++) if (sp_out[order[k]] || sp_ref[order[k]]) order[w++] = order[k];
        no = w;
    }
    if (!no) { nk_layout_row_dynamic(ctx, 22, 1); nk_label_colored(ctx, "(every frame of this animation is an alias of another - nothing to draw)", NK_TEXT_LEFT, nk_rgb(150, 150, 160)); return; }
    if (cols < 1) cols = 1;
    for (int row = 0; row < no; row += cols) {
        int nrow = no - row < cols ? no - row : cols;
        nk_layout_row_static(ctx, 110, 110, nrow);
        for (int k = row; k < row + nrow; k++) {
            int pose = order[k], miss = sp_ref[pose] && !sp_out[pose], have = sp_out[pose], src = pose, flip = 0;
            char rp[400]; static char under[560]; const char *u = NULL; SDL_Texture *t;
            if (!have && !sp_ref[pose]) {  /* an ALIAS: draw it from its source (mirrored when the alias is) */
                sp_alias_load();
                src = sp_al_of[pose]; flip = src >= 0 ? sp_al_flip[pose] : 0;
                for (int hop = 0; hop < 4 && src >= 0 && !sp_out[src] && !sp_ref[src] && sp_al_of[src] >= 0; hop++) { flip ^= sp_al_flip[src]; src = sp_al_of[src]; }
                if (src < 0 || src >= 2048 || (!sp_out[src] && !sp_ref[src])) { nk_label_colored(ctx, "alias ?", NK_TEXT_CENTERED, nk_rgb(90, 95, 105)); continue; }
                have = sp_out[src]; miss = !have;
            }
            snprintf(rp, sizeof rp, "jobs/%s/%s/pose_%04d.png", sk_name, have ? "out" : "ref", src);
            if (pose < 800) { int vid = pi_pair_victim(sk_base, pose);
                if (vid >= 0) { struct stat vs;
                    snprintf(under, sizeof under, "jobs/stock-class%d/out/pose_%04d.png", sk_class, vid);
                    if (stat(under, &vs)) snprintf(under, sizeof under, "jobs/stock-class%d/ref/pose_%04d.png", sk_class, vid);
                    if (!stat(under, &vs)) u = under; } }
            t = sp_tile_tex(rp, u, miss, flip);
            if (!t) { nk_label(ctx, "?", NK_TEXT_CENTERED); continue; }
            if (miss) { nk_style_push_color(ctx, &ctx->style.button.border_color, nk_rgb(230, 70, 60)); nk_style_push_float(ctx, &ctx->style.button.border, 3.0f); }
            { struct nk_rect tb = WB(); int hit = nk_button_image(ctx, nk_image_ptr(t));
              if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_RIGHT, tb) && sk_rq_find(src) < 0) { sk_rq_toggle(src, ""); hit = 0; }
              hint_at(tb, src != pose ? "ALIAS: this id is a copy of the source frame shown (never generated). Click = zoom the source, right-click = queue the source for a re-roll."
                        : miss ? "MISSING: the grey mannequin as sent. Click = zoom, right-click = queue a re-roll." : "drawn. Click = zoom, right-click = queue a re-roll.");
              if (hit) { sk_zoom_on = 2; sk_zoom_pose = src; sk_zoom_mt = 0; sk_zoom_out_fmt[0] = 0; snprintf(sk_zoom_title, sizeof sk_zoom_title, "pose %04d", src); nk_window_show(ctx, "skin zoom", NK_SHOWN); nk_window_set_focus(ctx, "skin zoom"); } }
            if (miss) { nk_style_pop_float(ctx); nk_style_pop_color(ctx); }
        }
        nk_layout_row_static(ctx, 14, 110, nrow);
        for (int k = row; k < row + nrow; k++) {
            int pose = order[k], al = !sp_out[pose] && !sp_ref[pose];
            if (al) { sp_alias_load(); nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(110, 150, 200), "%04d = %s%04d", pose, sp_al_of[pose] >= 0 && sp_al_flip[pose] ? "flip " : "", sp_al_of[pose] >= 0 ? sp_al_of[pose] : 0); }
            else nk_labelf_colored(ctx, NK_TEXT_CENTERED, sp_ref[pose] && !sp_out[pose] ? nk_rgb(255, 120, 90) : nk_rgb(150, 150, 160), "%04d", pose);
        }
    }
}
static int sp_sel = -2;            /* the selected list row: -2 nothing, -1 = GENERIC (all), else an animation index */
static int list_nav;               /* +1 / -1: an arrow key pressed this frame (SDL event) - the visible Poses list moves its selection */
static int rowkeys[900], nrowkeys; /* the rows drawn this frame, in order */
static void rows_begin(void) { nrowkeys = 0; }
static void rows_add(int key) { if (nrowkeys < 900) rowkeys[nrowkeys++] = key; }
static void rows_nav(int *sel, const char *group)   /* apply the arrow key to the selection + scroll the group to it */
{
    int cur = -1;
    if (!list_nav) return;
    for (int i = 0; i < nrowkeys; i++) if (rowkeys[i] == *sel) { cur = i; break; }
    if (cur < 0) cur = list_nav > 0 ? -1 : nrowkeys;
    cur += list_nav;
    if (cur >= 0 && cur < nrowkeys) { *sel = rowkeys[cur]; nk_group_set_scroll(ctx, group, 0, (nk_uint)(cur > 6 ? (cur - 6) * 26 : 0)); }
    list_nav = 0;
}

/* one selectable list row: name | counts; returns 1 when clicked */
static int sp_row(int key, const char *label, int have, int unique, int mapped)
{
    int sel = sp_sel == key; char t[120]; struct nk_rect b;
    snprintf(t, sizeof t, "%s   %d/%d", label, have, unique);
    b = WB(); rows_add(key);
    if (mapped) nk_style_push_color(ctx, &ctx->style.selectable.text_normal, nk_rgb(150, 210, 255));
    if (nk_selectable_label(ctx, t, NK_TEXT_LEFT, &sel) && sel) sp_sel = key;
    if (mapped) nk_style_pop_color(ctx);
    if (have == unique && unique) tick_icon(b);
    move_rename_popup(b, key);         /* right-click: rename the move (global) */
    return sel;
}

static void draw_skin_poses(void)
{
    static unsigned char seen[2048];
    int cls = sk_class, busy = tool_busy || sk_run_alive();
    int refs_ok = sk_have("jobs/%s/ref/pose_0000.png", sk_name), anch_ok = sk_have("skins/%s/anchor.png", sk_name) || sk_have("skins/%s/anchor_candidate.png", sk_name);
    float lr[2] = { 0.216f, 0.784f };   /* list narrowed twice (user 2026-08-28) */
    if (mp_n < 0) mp_load();
    sp_scan(); sp_wearers();
    if (strcmp(sk_rq_skin, sk_name)) sk_rq_load();
    heading("Poses", "UNIQUE drawings only (aliases are never generated). Left: GENERIC (every skin) and the moves - blue = mapped on a slot wearing this skin. Right: the selected pose set.");
    if (!refs_ok) { note("run 1 Refs (Artwork tab) first - the refs decide which ids are unique drawings."); return; }
    nk_layout_row(ctx, NK_DYNAMIC, avail_h() - 4, 2, lr);
    rows_begin();
    if (nk_group_begin(ctx, "sp_list", NK_WINDOW_BORDER)) {   /* ---- the vertical list ---- */
        int unique = 0, have = 0, alias = 0;
        memset(seen, 0, sizeof seen);
        for (int a = 0; a < mp_n; a++) {
            if (mp_anim[a].generic) sp_count(mp_anim[a].pose[cls], mp_anim[a].np[cls], seen, &unique, &have, &alias);
            else for (int k = 0; k < mp_anim[a].np[cls]; k++) if (mp_anim[a].pose[cls][k] >= 1024) sp_count(&mp_anim[a].pose[cls][k], 1, seen, &unique, &have, &alias);   /* victims of anyone's move */
        }
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label_colored(ctx, "GENERIC  (every skin)", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
        sp_row(-1, "all generic", have, unique, 0);
        {   /* the held bodies of every NON-generic move: anyone can do it to him */
            int u = 0, h = 0, al = 0;
            memset(seen, 0, sizeof seen);
            for (int a = 0; a < mp_n; a++) if (!mp_anim[a].generic) for (int k = 0; k < mp_anim[a].np[cls]; k++) if (mp_anim[a].pose[cls][k] >= 1024) sp_count(&mp_anim[a].pose[cls][k], 1, seen, &u, &h, &al);
            if (u || al) sp_row(-3, "  victim of any move", h, u, 0);
        }
        memset(seen, 0, sizeof seen);
        for (int a = 0; a < mp_n; a++) if (mp_anim[a].generic) {
            int u = 0, h = 0, al = 0; char lb[80];
            sp_count(mp_anim[a].pose[cls], mp_anim[a].np[cls], seen, &u, &h, &al);
            if (!u && !al) continue;
            if (!strncmp(mp_anim[a].name, mp_anim[a].kind, strlen(mp_anim[a].kind))) snprintf(lb, sizeof lb, "  %s", mp_anim[a].name);   /* "reaction 0x00" already says it */
            else snprintf(lb, sizeof lb, "  %s %s", mp_anim[a].kind, mp_anim[a].name);
            sp_row(a, lb, h, u, 0);
        }
        for (int pass = 0; pass < 2; pass++) {
            nk_layout_row_dynamic(ctx, 22, 1);
            if (pass == 0) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "MOVES on: %s%s", sp_nwear ? sp_wear[0] : "(no slot wears this skin)", sp_nwear > 1 ? " + more" : "");
            else nk_label_colored(ctx, "OTHER MOVES  (not mapped)", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            for (int a = 0; a < mp_n; a++) {
                mp_anim_t *m = &mp_anim[a]; int u = 0, h = 0, al = 0, mapped; char lb[80];
                if (m->generic || strcmp(m->kind, "move")) continue;
                mapped = m->id >= 0 && m->id < 256 && sp_mapped[m->id];
                if ((pass == 0) != (mapped != 0)) continue;
                memset(seen, 0, sizeof seen);
                sp_count(m->pose[cls], m->np[cls], seen, &u, &h, &al);
                if (!u && !al) continue;
                snprintf(lb, sizeof lb, "  0x%02X %s", m->id, m->name);
                sp_row(a, lb, h, u, mapped);
            }
        }
        rows_nav(&sp_sel, "sp_list");
        nk_group_end(ctx);
    }
    if (nk_group_begin(ctx, "sp_detail", NK_WINDOW_BORDER)) {   /* ---- the pose set ---- */
        int cols = (int)((0.85f * 0.784f * (float)nk_window_get_width(ctx)) / 118.0f);
        if (sp_sel == -2) { nk_layout_row_dynamic(ctx, 24, 1); nk_label_colored(ctx, "pick a pose set on the left", NK_TEXT_LEFT, nk_rgb(150, 150, 160)); }
        else {
            int u = 0, h = 0, al = 0; const char *title; char who[220] = "";
            static int all_ids[2048]; int nall = 0;
            memset(seen, 0, sizeof seen);
            if (sp_sel == -1 || sp_sel == -3) {
                title = sp_sel == -1 ? "GENERIC - every skin needs these" : "VICTIM of any move - the held bodies anyone can put him in";
                for (int a = 0; a < mp_n; a++) {
                    if (mp_anim[a].generic && sp_sel == -1) { sp_count(mp_anim[a].pose[cls], mp_anim[a].np[cls], seen, &u, &h, &al);
                        for (int k = 0; k < mp_anim[a].np[cls] && nall < 2048; k++) all_ids[nall++] = mp_anim[a].pose[cls][k]; }
                    else if (!mp_anim[a].generic) for (int k = 0; k < mp_anim[a].np[cls] && nall < 2048; k++) if (mp_anim[a].pose[cls][k] >= 1024) { sp_count(&mp_anim[a].pose[cls][k], 1, seen, &u, &h, &al); all_ids[nall++] = mp_anim[a].pose[cls][k]; }
                }
            } else if (sp_sel >= 0 && sp_sel < mp_n) {
                mp_anim_t *m = &mp_anim[sp_sel];
                static char tt[120]; snprintf(tt, sizeof tt, "%s %s%02X  %s", m->kind, strcmp(m->kind, "move") ? "" : "0x", m->id, m->name); title = tt;
                sp_count(m->pose[cls], m->np[cls], seen, &u, &h, &al);
                if (m->id >= 0 && m->id < 256 && sp_mapped[m->id]) for (int w = 0; w < sp_nwear; w++) if (sp_mapped[m->id] & (1 << w)) { if (who[0]) strcat(who, ", "); strcat(who, sp_wear[w]); }
            } else title = "";
            nk_layout_row_begin(ctx, NK_STATIC, 24, 4);
            nk_layout_row_push(ctx, 320); nk_label_colored(ctx, title, NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            nk_layout_row_push(ctx, 300); nk_labelf_colored(ctx, NK_TEXT_LEFT, h == u ? nk_rgb(120, 190, 120) : nk_rgb(200, 200, 210), "%d / %d drawn   (%d reused as aliases)", h, u, al);
            nk_layout_row_push(ctx, 110);
            if (h < u) { if (sk_button_gated("Generate", anch_ok && !busy, anch_ok ? "a tool is running" : "needs an anchor (Artwork > 2 Anchor)")) {
                memset(seen, 0, sizeof seen);
                if (sp_sel == -1 || sp_sel == -3) sp_queue_missing(all_ids, nall, seen);
                else sp_queue_missing(mp_anim[sp_sel].pose[cls], mp_anim[sp_sel].np[cls], seen);
                sk_rq_run(); } }
            else { struct nk_rect tb = WB(); nk_label(ctx, "", NK_TEXT_LEFT); tick_icon(tb); }
            nk_layout_row_push(ctx, 140); nk_checkbox_label(ctx, "show aliases", &sp_show_alias);
            nk_layout_row_end(ctx);
            if (who[0]) { nk_layout_row_dynamic(ctx, 20, 1); nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 210, 255), "mapped on %s", who); }
            if (sp_sel == -1 || sp_sel == -3) sp_grid("generic", sp_sel, all_ids, nall, cols);
            else sp_grid(mp_anim[sp_sel].kind, mp_anim[sp_sel].id, mp_anim[sp_sel].pose[cls], mp_anim[sp_sel].np[cls], cols);
        }
        nk_group_end(ctx);
    }
}

/* Save + pack a skin: sk_save, pull the run's frames in, re-dress every slot
 * wearing it + repack (the Artwork tab's button, shared by the button row on
 * every skins sub-tab - user 2026-08-30) */
static void sk_save_pack(const char *d, int out_ok)
{
    char cmd[4608];

            sk_save();
            if (out_ok) {              /* one button: saving also pulls the run's
                                          frames into the skin (user 2026-08-25) */
                snprintf(cmd, sizeof cmd, "mkdir -p \"%s/frames\" && cp \"jobs/%s/out/\"pose_*.png \"%s/frames/\" 2>/dev/null; { [ -d \"jobs/%s/out_hi\" ] && mkdir -p \"%s/frames_hi\" && cp \"jobs/%s/out_hi/\"pose_*.png \"%s/frames_hi/\"; } 2>/dev/null; cp \"jobs/%s/victmap.json\" \"%s/\" 2>/dev/null; cp \"jobs/%s/manifest.json\" \"%s/\" 2>/dev/null",
                         d, sk_name, d, sk_name, d, sk_name, d, sk_name, d, sk_name, d);
                if (system(cmd) == 0) logline("skin saved + frames pulled in - the Forge can wear it now");
                else logline("skin saved (frame copy failed)");
            } else logline("skin saved");
            {   /* re-dress every slot wearing THIS skin (recorded at Forge
                   dress time): re-ingest its frames + one repack — saving
                   the skin is all you do (user 2026-08-25) */
                char rcmd[4096]; int rn = 0; size_t off = 0;
                rcmd[0] = 0;
                for (int m2 = 0; m2 < wf_profile_nmods(); m2++) {
                    for (int sl = ENG_WS_MAX; sl < ENG_WS_EXT_MAX; sl++) {
                        char wj[560], err2[96]; json_val *doc2;
                        snprintf(wj, sizeof wj, "mods/%s/wrestlers/%02d/wrestler.json",
                                 wf_profile_mod(m2), sl);
                        doc2 = json_parse_file(wj, err2, sizeof err2);
                        if (!doc2) continue;
                        if (!strcmp(json_str(json_get(doc2, "skin"), ""), sk_name)) {
                            off += (size_t)snprintf(rcmd + off, sizeof rcmd - off,
                                "%srm -rf jobs/_dress && mkdir -p jobs/_dress && "
                                "ln -s \"$(pwd)/%s/frames\" jobs/_dress/out && "
                                "{ [ -d \"%s/frames_hi\" ] && ln -s \"$(pwd)/%s/frames_hi\" jobs/_dress/out_hi; true; } && "
                                "cp \"$(pwd)/%s/victmap.json\" jobs/_dress/ 2>/dev/null; cp \"$(pwd)/%s/palette.json\" jobs/_dress/ 2>/dev/null; cp \"$(pwd)/%s/manifest.json\" jobs/_dress/ 2>/dev/null; true && "
                                "./wfengine --art-ingest jobs/_dress %d \"mods/%s/wrestlers/%02d\" && "
                                "cp \"%s/select.png\" \"mods/%s/select/%02d.png\"",
                                rn ? " && " : "", d, d, d, d, d, d, sl, wf_profile_mod(m2), sl,
                                d, wf_profile_mod(m2), sl);
                            rn++;
                        }
                        json_free(doc2);
                        if (off > sizeof rcmd - 600) break;
                    }
                }
                if (rn > 0 && off < sizeof rcmd - 200) {
                    off += (size_t)snprintf(rcmd + off, sizeof rcmd - off,
                        " && %s", pack_cmd());
                    logf_("re-dressing %d slot(s) wearing '%s' + repack...", rn, sk_name);
                    run_tool_modal(rcmd);
                } else {               /* nobody wears it yet: still pack (the
                                          button says so; user 2026-08-26) */
                    snprintf(cmd, sizeof cmd, "%s", pack_cmd());
                    run_tool_modal(cmd);
                }
            }
            sk_scan();
        
}
static void draw_skins(void)
{
    char d[300], p[560], cmd[900];
    if (!wf_profile()[0]) { note("STOCK is read-only - pick a profile in the header first."); return; }
    if (!sk_open) {                    /* nothing clicked yet: open the FIRST skin
                                          in the list instead of a blank pane
                                          (user 2026-08-28) */
        if (sk_nlist <= 0) sk_scan();
        if (sk_nlist <= 0) return;
        sk_open = 1; sk_sel = 0; sk_load(sk_list[0]);
    }
    sk_dir(d, sizeof d);

    {   /* two sub-tabs (user 2026-08-25): PROMPT (identity + prompt
         * variables) and ART (the category rows, the run, the review grid).
         * primary pass + AI provider ride THIS row (user 2026-08-28:
         * reclaim vertical space; the skin:/body-class info line is gone -
         * the list on the left already says which skin is open). */
        nk_layout_row_begin(ctx, NK_STATIC, 26, 4);   /* tabs ALONE up here - the
                                          dropdowns ride the action row (user 2026-08-28) */
        nk_layout_row_push(ctx, ed_tab_w("Info"));
        if (ed_tab("Info", sk_sub == 0)) sk_sub = 0;
        nk_layout_row_push(ctx, ed_tab_w("Artwork"));
        if (ed_tab("Artwork", sk_sub == 1)) sk_sub = 1;
        nk_layout_row_push(ctx, ed_tab_w("Poses"));
        if (ed_tab("Poses", sk_sub == 2)) sk_sub = 2;   /* user 2026-08-28: generic vs move poses, as-needed */
        nk_layout_row_push(ctx, ed_tab_w("AI Recipe"));
        if (ed_tab("AI Recipe", sk_sub == 3)) sk_sub = 3;
        nk_layout_row_end(ctx);
        {   /* the Arenas-tab button row on EVERY skins sub-tab (user 2026-08-30): Browse opens THIS tab's folder */
            char cmd2[600], folder[400]; int named = sk_name[0] != 0, busy = tool_busy || (named && sk_run_alive());
            sk_dir(folder, sizeof folder);
            if (sk_sub == 1) snprintf(folder, sizeof folder, "jobs/%s", sk_name);            /* the run: ref/ out/ anchors/ */
            else if (sk_sub == 2) { size_t l = strlen(folder); snprintf(folder + l, sizeof folder - l, "/frames"); }
            nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
            nk_layout_row_push(ctx, 150);
            { struct nk_rect b = WB();
              if (sk_button_gated("Browse skin files", named, "name the skin first")) { snprintf(cmd2, sizeof cmd2, "xdg-open \"%s\" >/dev/null 2>&1 &", folder); if (system(cmd2)) {} }
              hint_at(b, sk_sub == 0 ? "the skin folder: skin.json, character.txt, anchor.png, select.png, frames/" : sk_sub == 1 ? "jobs/<skin>/: ref/ (the mannequins sent), out/ (accepted frames), anchors/, run.log" : sk_sub == 2 ? "the skin's frames/ (pose_NNNN.png)" : "the skin folder: prompts.json holds the recipes, character.txt the variables"); }
            nk_layout_row_push(ctx, 150);
            if (({ struct nk_rect sb = WB(); int h = sk_button_gated("  Save + pack skin", named && !busy, named ? "a request is still running (Cancel it first)" : "name the skin first"); save_icon(sb, !(named && !busy)); h; })) sk_save_pack(d, named && sk_have("jobs/%s/out", sk_name));
            nk_layout_row_end(ctx);
        }
        if (sk_sub == 2) { draw_skin_poses(); return; }
        if (sk_sub == 1) goto art;
        if (sk_sub == 3) goto recipe;
    }
    heading("Identity", NULL);
    {
        nk_layout_row_begin(ctx, NK_STATIC, 24, 4);   /* text-sized (user 2026-08-27) */
        nk_layout_row_push(ctx, 80); nk_label(ctx, "name", NK_TEXT_RIGHT);
        nk_layout_row_push(ctx, 200);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, sk_name, sizeof sk_name, nk_filter_default);
        sk_slug();                     /* live: spaces -> '-', lowercase (dir + shell arg) */
        nk_layout_row_push(ctx, 90); nk_label(ctx, "body class", NK_TEXT_RIGHT);
        nk_layout_row_push(ctx, 220);
        { struct nk_rect cb = WB();
          sk_class = nk_combo(ctx, sk_class_names, ENG_BODY_CLASSES, sk_class, 22, nk_vec2(220, 200));
          hint_at(cb, "The body template the skin is drawn on (docs/ai-art-pipeline.md 'Body templates'). Refs come from the class template: all 552 body poses (borrowed ones from the nearest class) + the class's victim bodies. A finished skin dresses any slot whose base is in this class."); }
        nk_layout_row_end(ctx);
    }
    nk_layout_row_dynamic(ctx, 24, 1);
    { struct nk_rect sb = WB();
      nk_checkbox_label(ctx, "class STOCK template: only the borrowed poses are generated (anchor = the stock member)", &sk_stock);
      hint_at(sb, "Builds the class's complete stock set: 1 Refs pre-fills out/ with the class's own frames and writes anchor.png from the stock member, so GO converts only the poses this class never had (76-184) onto its body. Describe the stock member in the prompt box. Save it as e.g. stock-class2."); }
    heading("Body", "the template fixes the POSE; these fix the BODY");
    {
        /* stacked, short (user 2026-08-27) */
        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        nk_layout_row_push(ctx, 80);
        { struct nk_rect b = WB(); nk_label(ctx, "height %", NK_TEXT_RIGHT);
          hint_at(b, "Scale of the accepted figure at placement, feet anchored (he grows upward). 100 = the template's height. Keep it within ~110: hitboxes and holds stay the template's, the Calibrate tab hides a few points of mismatch."); }
        nk_layout_row_push(ctx, 140); nk_property_int(ctx, "#h", 80, &sk_height, 120, 1, 1);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        nk_layout_row_push(ctx, 80); nk_label(ctx, "width %", NK_TEXT_RIGHT);
        nk_layout_row_push(ctx, 140); nk_property_int(ctx, "#w", 80, &sk_width, 120, 1, 1);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        nk_layout_row_push(ctx, 80);
        { struct nk_rect b = WB(); nk_label(ctx, "build", NK_TEXT_RIGHT);
          hint_at(b, "The {BUILD} slot: 'mannequin' asks codex for the template's proportions; 'anchor' asks for the character's OWN build (frame, limb thickness) as drawn in the anchor - for a body that is not the template's, e.g. Andre on the giant template."); }
        nk_layout_row_push(ctx, 300);
        { static const char *bn[2] = { "mannequin (template proportions)", "anchor (the character's own build)" };
          sk_build_anchor = nk_combo(ctx, bn, 2, sk_build_anchor, 22, nk_vec2(300, 100)); }
        nk_layout_row_end(ctx);
    }
    heading("Sounds", "the wrestler's own announcer clips: a stock clip or a WAV from the sounds/ library (Sounds > Library). '(base)' = whatever his base wrestler gets. Saved with the skin; Save + pack (Artwork) puts it in the paks.");
    for (int e = 0; e < ENG_SND_N; e++) {
        nk_layout_row_begin(ctx, NK_STATIC, 24, 3);
        nk_layout_row_push(ctx, 80); nk_label(ctx, snd_ev_lbl[e], NK_TEXT_RIGHT);
        nk_layout_row_push(ctx, 300);
        if (snd_ref_combo(sk_snd[e], sizeof sk_snd[e], "(base)", 300)) sk_save();
        nk_layout_row_push(ctx, 50);
        if (sk_snd[e][0]) { if (nk_button_label(ctx, "Play")) snd_ref_play(sk_snd[e]); }
        else nk_label(ctx, "", NK_TEXT_LEFT);
        nk_layout_row_end(ctx);
    }
    nk_layout_row_static(ctx, 24, 200, 1);
    if (sk_button_gated(sk_name[0] ? "Save skin" : "Save skin (name it first)", sk_name[0] != 0, "type a name above")) { sk_save(); logf_("skin '%s' saved to mods/%s/skins", sk_name, sk_mod); }
    return;

recipe:
    heading("AI Recipe", "the prompt variables (character + outline) and the per-call recipes codex gets for this skin");
    nk_layout_row_dynamic(ctx, 54, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, sk_char, sizeof sk_char, nk_filter_default);
    nk_layout_row_dynamic(ctx, 54, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, sk_outl, sizeof sk_outl, nk_filter_default);
    {   /* defaults live with the boxes they fill (user 2026-08-25) */
        float dr[3] = { 0.30f, 0.30f, 0.40f };
        nk_layout_row_static(ctx, 24, 200, 3);   /* text-sized (user 2026-08-27) */
        (void)dr;
        if (({ struct nk_rect sb = WB(); int h = nk_button_label(ctx, "Save as default prompts"); save_icon(sb, 0); h; })) {
            sk_defaults_save();
            logline("these prompt variables are now the defaults for new skins");
        }
        if (nk_button_label(ctx, "Load default prompts")) sk_defaults_load();
        if (sk_button_gated(sk_name[0] ? "Save skin -> art" : "Save skin (name it first)", sk_name[0] != 0, "type a name above")) {
            sk_save();
            logf_("skin '%s' saved to mods/%s/skins - now the art sub-tab: 1 Refs", sk_name, sk_mod);
            sk_sub = 1;
        }
    }
    {   /* the recipes, one editable box each (user 2026-08-25). Slots:
         * {CHAR} {OUTL} {BUILD} {N} {GRID} {EXTRA}. Saved with the skin
         * (prompts.json, only what differs from the default). */
        int cols = (int)(nk_window_get_content_region(ctx).w / 7.6f) - 4;
        sk_pt_defaults();
        if (cols < 40) cols = 40;
        if (cols != sk_pt_cols) { for (int k = 0; k < SK_PK_N; k++) if (!sk_pt_active[k]) sk_pt_wrap(k, cols); sk_pt_cols = cols; }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Recipes  (one box per codex call - {CHAR} {OUTL} {BUILD} {N} {GRID} {EXTRA}; saved with the skin)", NK_MAXIMIZED)) {   /* open: this tab is the recipe (user 2026-08-30) */
        for (int k = 0; k < SK_PK_N; k++) {
            int lines = 1, changed = strcmp(sk_pt[k], sk_pdef[k]) != 0; nk_flags fl;
            float rr[2] = { 0.86f, 0.14f };
            for (const char *t = sk_ptw[k]; *t; t++) if (*t == '\n') lines++;
            nk_layout_row(ctx, NK_DYNAMIC, 20, 2, rr);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, changed ? nk_rgb(255, 190, 90) : nk_rgb(180, 180, 200), "%s%s", sk_ptitle[k], changed ? "  (edited)" : "");
            if (nk_button_label(ctx, "reset")) { snprintf(sk_pt[k], sizeof sk_pt[k], "%s", sk_pdef[k]); sk_pt_wrap(k, cols); }
            nk_layout_row_dynamic(ctx, (float)(lines * 17 + 12), 1);
            fl = nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_MULTILINE, sk_ptw[k], sizeof sk_ptw[k], nk_filter_default);
            if (fl & NK_EDIT_ACTIVE) { sk_pt_active[k] = 1; sk_pt_unwrap(k); }
            else if (sk_pt_active[k]) { sk_pt_active[k] = 0; sk_pt_unwrap(k); sk_pt_wrap(k, cols); }
        }
        nk_tree_pop(ctx);
        }
    }
    return;

art:
    setenv("WF_ART_PROVIDER", wf_art_provider_names[sk_provider], 1);   /* every tool launched below inherits it */
    {
        int named   = sk_name[0] != 0;
        int orphan  = named && !tool_busy && sk_run_alive();
        int busy    = tool_busy || orphan;
        int refs_ok = named && sk_have("jobs/%s/ref/pose_0000.png", sk_name);
        int cand_ok = named && sk_have("%s/anchor_candidate.png", d);
        int anch_ok = named && sk_have("%s/anchor.png", d);
        int out_ok  = named && sk_have("jobs/%s/out", sk_name);
        sk_next = !named ? "name the skin" :
                          !refs_ok ? "1 Refs" : !(anch_ok || cand_ok) ? "2 Anchor" :
                          !sk_have("%s/select.png", d) ? "3 Portrait, 4 Continue faces, 5 Title card, 6 GO poses" :
                          !out_ok ? "4 GO" : "5 Animation / 6 Title card, then Save skin (pulls the frames in)";
        heading("Artwork", NULL);      /* title above the row (user 2026-08-28) */
        /* ONE row: the primary pass / Provider dropdowns FIRST, then the 6
         * numbered steps + Cancel + Save (user 2026-08-28: dropdowns before
         * the buttons; buttons trimmed to make room) */
        nk_layout_row_begin(ctx, NK_STATIC, 24, 12);
        char l_go[48], l_anim[40], l_title[40];
        snprintf(l_go, sizeof l_go, out_ok && sk_miss_frames ? "6 Poses (%d)" : "6 Poses", sk_miss_frames);   /* the tick says done (user 2026-08-28) */
        snprintf(l_anim, sizeof l_anim, out_ok && sk_miss_anim ? "4 Faces (%d)" : "4 Faces", sk_miss_anim);
        snprintf(l_title, sizeof l_title, "%s", "5 Title");
        {
            int npv = 0;
            while (wf_art_provider_names[npv]) npv++;
            nk_layout_row_push(ctx, 90);
            nk_label(ctx, "primary pass", NK_TEXT_RIGHT);
            nk_layout_row_push(ctx, 60);
            {   /* the sheet grid of the GO runs' MAIN pass (standing figures;
                 * lying/low figures always go 2x2): art_run.c WF_ART_GRID */
                static const char *grids[4] = { "4x4", "3x3", "2x2", "1x1" };
                struct nk_rect cb = WB(); int gi = 4 - sk_grid;
                gi = nk_combo(ctx, grids, 4, gi < 0 || gi > 3 ? 1 : gi, 22, nk_vec2(120, 140));
                sk_grid = 4 - gi;
                hint_at(cb, "Poses per codex call on the main pass: 4x4 = 256 px per figure (pixelated conversions), 3x3 = 341 px (~70 calls for a 600-frame skin), 2x2 = 512 px (~150 calls), 1x1 = one pose per call at 1024 px (best adherence, ~600 calls). Lying/low figures always use 2x2."); }
            nk_layout_row_push(ctx, 62);
            nk_label(ctx, "Provider", NK_TEXT_RIGHT);
            nk_layout_row_push(ctx, 110);
            { struct nk_rect cb = WB();
              sk_provider = nk_combo(ctx, wf_art_provider_names, npv, sk_provider, 22, nk_vec2(200, 120));
              hint_at(cb, "The image-generation backend every button on this tab calls. Saved with the skin. codex = the codex CLI (two input images; tools/codex_setup.sh checks the install). New backends register in tools/art_run.c art_providers[]."); }
        }
        nk_layout_row_push(ctx, 115);
        if (({ struct nk_rect tb = WB(); int h_ = (sk_button_gated("1 Refs", named && !busy, named ? "a tool is still running" : "select or name a skin first")); if (refs_ok) tick_icon(tb); h_; })) {
            logf_("Refs: class %d template -> jobs/%s ...", sk_class, sk_name);
            sk_save();
            if (sk_stock)
                snprintf(cmd, sizeof cmd, "./wfengine --class-template %d \"jobs/%s\" stock && cp \"jobs/%s/anchor.png\" \"%s/anchor.png\"", sk_class, sk_name, sk_name, d);
            else
                snprintf(cmd, sizeof cmd, "./wfengine --class-template %d \"jobs/%s\"", sk_class, sk_name);
            run_tool(cmd);
        }
        nk_layout_row_push(ctx, 125);
        if (({ struct nk_rect tb = WB(); int h_ = (sk_button_gated("2 Anchor", refs_ok && !busy, refs_ok ? "a tool is still running" : "run 1 Refs first")); if ((anch_ok || cand_ok)) tick_icon(tb); h_; })) {
            logline("Anchor: asking codex (a few minutes; press again to re-roll; the NEXT step locks it in)");
            sk_save();
            snprintf(cmd, sizeof cmd, "./wfengine --art-anchor \"jobs/%s\" \"%s/character.txt\" \"%s/anchor_candidate.png\"",
                     sk_name, d, d);
            run_tool(cmd);
        }
        /* no Accept buttons (user 2026-08-25): pressing the NEXT step
         * promotes the newest anchor candidate; the portrait installs
         * itself right after generation. Re-press a step to re-roll. */
        nk_layout_row_push(ctx, 130);
        if (({ struct nk_rect tb = WB(); int h_ = (sk_button_gated("3 Portrait", (anch_ok || cand_ok) && !busy, "generate an anchor first")); if (sk_have("%s/select.png", d)) tick_icon(tb); h_; })) {
            sk_save();
            if (cand_ok) {
                snprintf(cmd, sizeof cmd, "cp \"%s/anchor_candidate.png\" \"%s/anchor.png\" && (cp \"%s/anchor_candidate.png.meta\" \"%s/anchor.png.meta\" 2>/dev/null; true)", d, d, d, d);
                if (system(cmd) == 0) logline("anchor locked in");
                sk_mt_anchor = 0;
            }
            snprintf(cmd, sizeof cmd,
                     "./wfengine --art-portrait data/select/04.png \"%s/anchor.png\" \"%s/character.txt\" \"%s/portrait_raw.png\""
                     " && ./wfengine --art-portrait-install \"%s/portrait_raw.png\" \"%s/select.png\"",
                     d, d, d, d, d);
            run_tool(cmd);
        }
        nk_layout_row_push(ctx, 130);
        if (({ struct nk_rect tb = WB(); int h_ = (sk_button_gated(l_anim, (anch_ok || cand_ok) && refs_ok && !busy, "needs refs and an anchor")); if ((out_ok && !sk_miss_anim)) tick_icon(tb); h_; })) {
            sk_save();
            if (cand_ok) {
                snprintf(cmd, sizeof cmd, "cp \"%s/anchor_candidate.png\" \"%s/anchor.png\" && (cp \"%s/anchor_candidate.png.meta\" \"%s/anchor.png.meta\" 2>/dev/null; true)", d, d, d, d);
                if (system(cmd) == 0) logline("anchor locked in");
                sk_mt_anchor = 0;
            }
            logline("Animation: the 6 continue faces as ONE sheet (1 request; delete jobs/<skin>/out/pose_080x.png first to redo)");
            snprintf(cmd, sizeof cmd, "WF_ART_CARDS=faces ./wfengine --art-run \"jobs/%s\" \"%s/anchor.png\" \"%s/character.txt\"",
                     sk_name, d, d);
            run_tool(cmd);
        }
        nk_layout_row_push(ctx, 120);
        if (({ struct nk_rect tb = WB(); int h_ = (sk_button_gated(l_title, (anch_ok || cand_ok) && refs_ok && !busy, "needs refs and an anchor")); if ((out_ok && !sk_miss_title)) tick_icon(tb); h_; })) {
            sk_save();
            if (cand_ok) {
                snprintf(cmd, sizeof cmd, "cp \"%s/anchor_candidate.png\" \"%s/anchor.png\" && (cp \"%s/anchor_candidate.png.meta\" \"%s/anchor.png.meta\" 2>/dev/null; true)", d, d, d, d);
                if (system(cmd) == 0) logline("anchor locked in");
                sk_mt_anchor = 0;
            }
            snprintf(cmd, sizeof cmd, "WF_ART_CARDS=title ./wfengine --art-run \"jobs/%s\" \"%s/anchor.png\" \"%s/character.txt\"",
                     sk_name, d, d);
            run_tool(cmd);
        }
        nk_layout_row_push(ctx, 145);
        if (({ struct nk_rect tb = WB(); int h_ = (sk_button_gated(l_go, (anch_ok || cand_ok) && refs_ok && !busy, "needs refs and an anchor")); if ((out_ok && !sk_miss_frames)) tick_icon(tb); h_; })) {
            sk_save();
            if (cand_ok) {
                snprintf(cmd, sizeof cmd, "cp \"%s/anchor_candidate.png\" \"%s/anchor.png\" && (cp \"%s/anchor_candidate.png.meta\" \"%s/anchor.png.meta\" 2>/dev/null; true)", d, d, d, d);
                if (system(cmd) == 0) logline("anchor locked in");
                sk_mt_anchor = 0;
            }
            snprintf(cmd, sizeof cmd, "WF_ART_GRID=%d ./wfengine --art-run \"jobs/%s\" \"%s/anchor.png\" \"%s/character.txt\"",
                     sk_grid, sk_name, d, d);
            run_tool(cmd);
        }
        if (strcmp(sk_rq_skin, sk_name)) sk_rq_load();   /* the queue follows the loaded skin */
        if (sk_rq_n && !busy && !orphan) sk_rq_run();    /* queued re-rolls start by themselves */
        nk_layout_row_push(ctx, 115);
        if (sk_button_gated("Cancel request", busy, "nothing is running")) {
            wf_editor_kill_tools();    /* our own tool group (codex rides in it) */
            if (orphan) {              /* a run surviving from a previous editor */
                char lp[360]; FILE *lf; long opid = 0;
                snprintf(lp, sizeof lp, "jobs/%s/run.lock", sk_name);
                lf = fopen(lp, "r");
                if (lf) { if (fscanf(lf, "%ld", &opid) != 1) opid = 0; fclose(lf); }
                if (opid > 0) { kill(-(pid_t)opid, SIGKILL); kill((pid_t)opid, SIGKILL); }
                unlink(lp);
            }
            logline("cancelled (GO resumes: finished frames are skipped)");
        }
        nk_layout_row_push(ctx, 125);
        if (({ struct nk_rect sb = WB(); int h = sk_button_gated("  Save + pack", named && !busy, named ? "a request is still running (Cancel it first)" : "name the skin first"); save_icon(sb, !(named && !busy)); h; })) { sk_save_pack(d, out_ok); }
        nk_layout_row_end(ctx);
    }

    {   /* the tab's own log: a SCROLLABLE box (user 2026-08-27), newest at
           the bottom, scrolled to the end whenever a line arrives */
        static int seen_n = -1;
        /* the same width as the button row above: its 12 pushes + 11 gaps
         * (user 2026-08-28) */
        float roww = 90 + 60 + 62 + 110 + 115 + 125 + 130 + 130 + 120 + 145 + 115 + 125
                     + 11 * ctx->style.window.spacing.x;
        nk_layout_row_begin(ctx, NK_STATIC, 90, 1);
        nk_layout_row_push(ctx, roww);
        if (nk_group_begin(ctx, "sk_log", NK_WINDOW_BORDER)) {
            SDL_LockMutex(log_mx);
            nk_layout_row_dynamic(ctx, 15, 1);
            for (int i = 0; i < log_n; i++) {
                int k = (log_head - log_n + i + 64) % 64;
                nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(170, 170, 190), "%s", logbuf[k]);
            }
            if (seen_n != log_head) { seen_n = log_head; nk_group_set_scroll(ctx, "sk_log", 0, 100000); }
            SDL_UnlockMutex(log_mx);
            nk_group_end(ctx);
        }
        nk_layout_row_end(ctx);
    }
    {
        int anchor_is_cand = 0;
        /* 1: the CLASS reference - what '2 Anchor' actually sends as the identity
           input (data/generics/C/base_ref.png, the generic man; user 2026-08-28:
           "shouldn't my base ref be the class image?"). The job's stance ref only
           when the class has none. */
        snprintf(p, sizeof p, "data/generics/%d/base_ref.png", sk_class);
        if (!sk_have("%s", p)) { snprintf(p, sizeof p, "jobs/%s/ref/pose_0000.png", sk_name); sk_png_gray = 1; }
        snprintf(sk_zsrc[0], sizeof sk_zsrc[0], "%s", p);
        sk_png_tex(p, &sk_tex_ref, &sk_mt_ref); sk_png_gray = 0;
        /* 2: newest anchor (candidate wins until the next step locks it).
         * ANCHOR HISTORY: every candidate the provider returns is kept in
         * <skin>/anchors/NN.png; the zoom modal's candidate arrows copy one
         * back over anchor_candidate.png. */
        static time_t an_seen; static char an_skin[40];
        {
            char cand[420], hist[440]; struct stat ca, aa;
            snprintf(cand, sizeof cand, "%s/anchor_candidate.png", d);
            if (strcmp(an_skin, sk_name)) {   /* count the skin's history */
                snprintf(an_skin, sizeof an_skin, "%s", sk_name);
                sk_an_n = 0; sk_an_cur = -1; an_seen = 0;
                while (sk_have("%s/anchors/%02d.png", d, sk_an_n + 1)) sk_an_n++;
                sk_an_cur = sk_an_n - 1;
                if (stat(cand, &ca) == 0) an_seen = ca.st_mtime;
            }
            if (!tool_busy && stat(cand, &ca) == 0 && ca.st_mtime != an_seen) {
                int dup = 0;           /* a candidate restored by the arrows is
                                          already in the history */
                if (sk_an_cur >= 0 && sk_an_cur < sk_an_n) {
                    struct stat hs;
                    snprintf(hist, sizeof hist, "%s/anchors/%02d.png", d, sk_an_cur + 1);
                    if (stat(hist, &hs) == 0 && hs.st_size == ca.st_size) dup = 1;
                }
                if (!dup && sk_an_n < 99) {
                    char cc[1200];
                    snprintf(cc, sizeof cc, "mkdir -p \"%s/anchors\" && cp \"%s\" \"%s/anchors/%02d.png\" && cp \"%s.meta\" \"%s/anchors/%02d.png.meta\" 2>/dev/null; true",
                             d, cand, d, sk_an_n + 1, cand, d, sk_an_n + 1);
                    if (system(cc) == 0) { sk_an_n++; sk_an_cur = sk_an_n - 1; }
                }
                an_seen = ca.st_mtime;
            }
            snprintf(p, sizeof p, "%s/anchor.png", d);
            if (stat(cand, &ca) == 0 && (stat(p, &aa) != 0 || ca.st_mtime > aa.st_mtime)) {
                snprintf(p, sizeof p, "%s", cand);
                anchor_is_cand = 1;
            }
        }
        snprintf(sk_zsrc[1], sizeof sk_zsrc[1], "%s", p);
        sk_png_tex(p, &sk_tex_anchor, &sk_mt_anchor);
        /* 3: the portrait — the installed 80x80 cell once made, else the raw */
        snprintf(p, sizeof p, "%s/select.png", d);
        if (!sk_have("%s/select.png", d)) snprintf(p, sizeof p, "%s/portrait_raw.png", d);
        snprintf(sk_zsrc[2], sizeof sk_zsrc[2], "%s", p);
        sk_png_tex(p, &sk_tex_portrait, &sk_mt_portrait);
        /* 4: the newest generated frame of the run */
        sk_newest_frame(p, sizeof p);
        if (p[0]) { snprintf(sk_frame_name, sizeof sk_frame_name, "%s", strrchr(p, '/') + 1); snprintf(sk_zsrc[3], sizeof sk_zsrc[3], "%s", p); sk_png_tex(p, &sk_tex_frame, &sk_mt_frame); }
        else { sk_zsrc[3][0] = 0; if (sk_tex_frame) { SDL_DestroyTexture(sk_tex_frame); sk_tex_frame = NULL; sk_mt_frame = 0; } }   /* no run: no stale frame */
        {
            SDL_Texture *tiles[4] = { sk_tex_ref, sk_tex_anchor, sk_tex_portrait, sk_tex_frame };
            const char *empty[4] = { "(no refs yet)", "(no anchor yet)", "(no portrait yet)", "(no frames yet)" };
            nk_layout_row_static(ctx, 180, 180, 4);   /* square cells: click = zoom (prev/next live in the modal) */
            for (int k = 0; k < 4; k++) {
                if (tiles[k] && nk_button_image(ctx, nk_image_ptr(tiles[k]))) {
                    sk_zoom_on = 2; sk_zoom_pose = -1; sk_zoom_tile = k; sk_zoom_mt = 0; sk_zoom_out_fmt[0] = 0;
                    snprintf(sk_zoom_path, sizeof sk_zoom_path, "%s", sk_zsrc[k]);
                    snprintf(sk_zoom_title, sizeof sk_zoom_title, "%s", k == 3 ? sk_frame_name : sk_zsrc_name[k]);
                    /* (no log line for a zoom) */
                    nk_window_show(ctx, "skin zoom", NK_SHOWN); nk_window_set_focus(ctx, "skin zoom");
                } else if (!tiles[k]) nk_label(ctx, empty[k], NK_TEXT_CENTERED);
            }
            nk_layout_row_static(ctx, 16, 180, 4);
            nk_label(ctx, "class ref (anchor input)", NK_TEXT_CENTERED);
            nk_label(ctx, anchor_is_cand ? "anchor CANDIDATE" : "anchor (locked)", NK_TEXT_CENTERED);
            nk_label(ctx, sk_have("%s/select.png", d) ? "portrait (installed)" : "portrait (raw)", NK_TEXT_CENTERED);
            nk_label(ctx, sk_frame_name[0] ? sk_frame_name : "newest frame", NK_TEXT_CENTERED);
            /* (timestamp/model line removed - user 2026-08-27; the zoom modal still shows it) */
        }
    }
    {   /* ---- review browser: flip through every generated frame before
           saving (user 2026-08-25). Sorted by pose id, SK_REV_N per page,
           click any frame for the zoom modal. ---- */
        static int revc_poses[2048]; static unsigned char revc_miss[2048];
        static int rev_poses[2048]; static unsigned char rev_miss[2048], rev_more[2048];   /* rev_more: other animations sharing the pose */
        static int rev_cache_n = -1; static Uint32 rev_scan_t;
        static long long rev_out_mt, rev_ref_mt; static char rev_skin[40]; static int rev_busy = -1;
        int rev_n, pages;
        char rp[560];
        /* the scan is CACHED: 1162 refs re-scanned (and worse, re-decoded)
           57x/s made the whole tab crawl. Rescan at most 1x/s, and only
           when a dir mtime moved or the skin/busy state changed. The mtime
           is taken in NANOSECONDS: a Refs run writes every ref and unlinks
           the aliased ones inside the same second, and a whole-second key
           froze the count mid-run ("150 missing" with 1 on disk, 2026-08-27). */
        {
            struct stat so, sr; long long om = 0, rm = 0;
            snprintf(rp, sizeof rp, "jobs/%s/out", sk_name);
            if (stat(rp, &so) == 0) om = (long long)so.st_mtim.tv_sec * 1000000000LL + so.st_mtim.tv_nsec;
            snprintf(rp, sizeof rp, "jobs/%s/ref", sk_name);
            if (stat(rp, &sr) == 0) rm = (long long)sr.st_mtim.tv_sec * 1000000000LL + sr.st_mtim.tv_nsec;
            if (rev_cache_n < 0 || SDL_GetTicks() - rev_scan_t > 1000) {
                if (rev_cache_n < 0 || om != rev_out_mt || rm != rev_ref_mt
                    || rev_busy != tool_busy || strcmp(rev_skin, sk_name)) {
                    rev_busy = tool_busy;
                    DIR *rd; struct dirent *re; int n2 = 0;
                    snprintf(rp, sizeof rp, "jobs/%s/out", sk_name);
                    rd = opendir(rp);
                    if (rd) {
                        while ((re = readdir(rd)) && n2 < 2048) {
                            int pn;
                            if (sscanf(re->d_name, "pose_%d.png", &pn) == 1) {
                                revc_miss[n2] = 0; revc_poses[n2++] = pn;
                            }
                        }
                        closedir(rd);
                    }
                    {   /* refs without an out frame = MISSING (no decode:
                           art-job never writes an empty ref). Listed even
                           with an EMPTY out/: a fresh job must show its
                           refs, not a blank grid (user 2026-08-25) */
                        snprintf(rp, sizeof rp, "jobs/%s/ref", sk_name);
                        rd = opendir(rp);
                        if (rd) {
                            while ((re = readdir(rd)) && n2 < 2048) {
                                int pn; struct stat st2; char q2[600];
                                if (sscanf(re->d_name, "pose_%d.png", &pn) != 1) continue;
                                snprintf(q2, sizeof q2, "jobs/%s/out/pose_%04d.png", sk_name, pn);
                                if (stat(q2, &st2) != 0) {
                                    revc_miss[n2] = 1; revc_poses[n2++] = pn;
                                }
                            }
                            closedir(rd);
                        }
                    }
                    rev_cache_n = n2; rev_out_mt = om; rev_ref_mt = rm;
                    sk_miss_frames = sk_miss_anim = sk_miss_title = 0;
                    for (int i = 0; i < n2; i++) if (revc_miss[i]) {
                        int pn = revc_poses[i];
                        if (pn >= ENG_CONT_POSE0 && pn < ENG_CONT_POSE0 + 8) sk_miss_anim++;
                        else if (pn == ENG_TITLE_POSE) sk_miss_title++;
                        else if (pn < 800 || pn >= 1024) sk_miss_frames++;
                    }
                    snprintf(rev_skin, sizeof rev_skin, "%s", sk_name);
                    for (int i = 1; i < n2; i++) {   /* MISSING first, then pose id */
                        int v = revc_poses[i], j = i - 1; unsigned char mv = revc_miss[i];
                        while (j >= 0 && (revc_miss[j] < mv
                                          || (revc_miss[j] == mv && revc_poses[j] > v))) {
                            revc_poses[j+1] = revc_poses[j]; revc_miss[j+1] = revc_miss[j]; j--;
                        }
                        revc_poses[j+1] = v; revc_miss[j+1] = mv;
                    }
                }
                rev_scan_t = SDL_GetTicks();
            }
        }
        rev_n = rev_cache_n > 0 ? rev_cache_n : 0;
        if (rev_n > 0) {   /* per-frame view: filtering never mutates the cache.
                              The CARDS (800-1023) live on their own row below,
                              not in the paged in-game grid (user 2026-08-25) */
            int n3 = 0;
#define REV_KEEP(p) ((p) < 800 || (p) >= 1024)
#define REV_FILT(p) (!sk_rev_find[0] || ({ char idb_[8]; snprintf(idb_, sizeof idb_, "%04d", (p)); !strncmp(idb_, sk_rev_find, strlen(sk_rev_find)); }))
            if (sk_rev_group == 0) {
                for (int i = 0; i < rev_n; i++) {
                    if (!REV_KEEP(revc_poses[i])) continue;
                    if (!REV_FILT(revc_poses[i])) continue;   /* POSE FILTER: prefix of the 4-digit id */
                    rev_more[n3] = 0; rev_poses[n3] = revc_poses[i]; rev_miss[n3++] = revc_miss[i];
                }
            } else {                   /* BY MOVE: ONE animation, picked in the pager's
                                          move dropdown (user 2026-08-27), its frames in
                                          playing order; the dropdown's last two entries
                                          are "other" and "victim bodies" */
                int nanim;
                if (pi_n < 0) pi_load();
                nanim = pi_n;
                if (sk_rev_move > nanim + 1) sk_rev_move = 0;
                if (sk_rev_move < nanim) {
                    int a = sk_rev_move;
                    for (int f = 0; f < pi_anim[a].n && n3 < 2000; f++) {
                        int p = pi_anim[a].pose[f], ci = -1;
                        for (int i = 0; i < rev_n; i++) if (revc_poses[i] == p) { ci = i; break; }
                        if (ci < 0 || !REV_FILT(p)) continue;
                        { int m = 0; for (int b = 0; b < nanim; b++) if (b != a) for (int g = 0; g < pi_anim[b].n; g++) if (pi_anim[b].pose[g] == p) { m++; break; }
                          rev_more[n3] = (unsigned char)(m > 255 ? 255 : m); }
                        rev_poses[n3] = p; rev_miss[n3++] = revc_miss[ci];
                    }
                } else {               /* other (no animation names it) / victim bodies (1024+) */
                    int sec = sk_rev_move - nanim;
                    static unsigned char used[2048];
                    memset(used, 0, sizeof used);
                    for (int a = 0; a < nanim; a++) for (int f = 0; f < pi_anim[a].n; f++) used[pi_anim[a].pose[f] & 2047] = 1;
                    for (int i = 0; i < rev_n && n3 < 2000; i++) {
                        int p = revc_poses[i];
                        if (!REV_KEEP(p) || (sec ? p < 1024 : (p >= 1024 || used[p & 2047]))) continue;
                        if (!REV_FILT(p)) continue;
                        rev_more[n3] = 0; rev_poses[n3] = p; rev_miss[n3++] = revc_miss[i];
                    }
                }
            }
            rev_n = n3;
        }
        if (rev_n > 0 || sk_rev_find[0]) {
            int nframes = 0;
            sk_all_n = 0;              /* the modal's prev/next walk the FRAMES (headers skipped) */
            for (int i = 0; i < rev_n; i++) if (rev_poses[i] >= 0) { sk_all_poses[sk_all_n++] = rev_poses[i]; nframes++; }
            pages = (rev_n + SK_REV_N - 1) / SK_REV_N;
            if (pages < 1) pages = 1;
            if (sk_rev_page >= pages) sk_rev_page = pages - 1;
            {   /* pager row (user 2026-08-27): < > on the left, group + move
                   dropdowns beside them, then page text / pose filter /
                   home / end packed on the right */
                static const char *grp[2] = { "by id", "by move" };
                static const char **mv_items; static char (*mv_buf)[64]; static int mv_n = -1;
                struct nk_rect reg = nk_window_get_content_region(ctx);
                int nw = sk_rev_group == 1 ? 9 : 8;
                float gap;
                if (sk_rev_group == 1 && pi_n < 0) pi_load();
                if (sk_rev_group == 1 && mv_n != pi_n + 2 && pi_n >= 0) {   /* (re)build the move list */
                    free((void *)mv_items); free(mv_buf);
                    mv_buf = malloc(sizeof *mv_buf * (size_t)(pi_n + 2));
                    mv_items = malloc(sizeof *mv_items * (size_t)(pi_n + 2));
                    for (int a = 0; a < pi_n; a++) {
                        snprintf(mv_buf[a], sizeof mv_buf[a], "%s %s%02X  %s", pi_anim[a].kind,
                                 !strcmp(pi_anim[a].kind, "move") ? "0x" : "", pi_anim[a].id, pi_anim[a].name);
                        mv_items[a] = mv_buf[a];
                    }
                    snprintf(mv_buf[pi_n], sizeof mv_buf[0], "other (unnamed)"); mv_items[pi_n] = mv_buf[pi_n];
                    snprintf(mv_buf[pi_n + 1], sizeof mv_buf[0], "victim bodies"); mv_items[pi_n + 1] = mv_buf[pi_n + 1];
                    mv_n = pi_n + 2;
                }
                (void)reg; gap = 0; (void)gap;
                nk_layout_row_begin(ctx, NK_STATIC, 24, nw - 1);
                nk_layout_row_push(ctx, 30);
                if (nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_LEFT) && sk_rev_page > 0) sk_rev_page--;
                nk_layout_row_push(ctx, 30);
                if (nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_RIGHT) && sk_rev_page < pages - 1) sk_rev_page++;
                nk_layout_row_push(ctx, 100);
                {   struct nk_rect gb = WB(); int g = nk_combo(ctx, grp, 2, sk_rev_group, 22, nk_vec2(160, 80));
                    if (g != sk_rev_group) { sk_rev_group = g; sk_rev_page = 0; for (int k = 0; k < SK_REV_N; k++) sk_rev_mt[k] = 0; }
                    hint_at(gb, "by move: pick ONE ROM animation in the dropdown beside this - the grid shows its frames in playing order; two-man frames are drawn OVER the class template's victim body so contact can be judged. 'other' = frames no animation names, then 'victim bodies'."); }
                if (sk_rev_group == 1 && mv_n > 0) {   /* the MOVE picker (user 2026-08-27) */
                    int m2;
                    nk_layout_row_push(ctx, 260);
                    m2 = nk_combo(ctx, mv_items, mv_n, sk_rev_move < mv_n ? sk_rev_move : 0, 22, nk_vec2(320, 420));
                    if (m2 != sk_rev_move) { sk_rev_move = m2; sk_rev_page = 0; for (int k = 0; k < SK_REV_N; k++) sk_rev_mt[k] = 0; }
                }
                nk_layout_row_push(ctx, 200);
                nk_labelf(ctx, NK_TEXT_RIGHT, sk_rev_find[0] ? "page %d / %d  (%d match%s)" : "page %d / %d  (%d frames)",
                          sk_rev_page + 1, pages, nframes, nframes == 1 ? "" : "es");
                nk_layout_row_push(ctx, 60);
                {   /* pose filter: up to 4 digits; the grid narrows as you type, page resets */
                    struct nk_rect fb = WB(); char before[8];
                    snprintf(before, sizeof before, "%s", sk_rev_find);
                    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, sk_rev_find, 5, nk_filter_decimal);
                    if (strcmp(before, sk_rev_find)) sk_rev_page = 0;
                    hint_at(fb, "Filter the grid by pose id: a prefix of the 4-digit id (1086 = that frame, 108 = 1080-1089). Clear to see everything.");
                }
                nk_layout_row_push(ctx, 52);
                if (nk_button_label(ctx, "home")) sk_rev_page = 0;
                nk_layout_row_push(ctx, 44);
                if (nk_button_label(ctx, "end")) sk_rev_page = pages - 1;
                nk_layout_row_end(ctx);
            }
            for (int half = 0; half < 2; half++) {
            nk_layout_row_static(ctx, 150, 150, 8);   /* readable figure tiles (bbox-cropped,
                                          square-letterboxed = undistorted); TRUE-scale
                                          comparison lives in the modal (user 2026-08-28:
                                          "too small to be meaningful") */
            for (int k = half * 8; k < half * 8 + 8; k++) {
                int idx = sk_rev_page * SK_REV_N + k;
                if (idx >= rev_n) { nk_label(ctx, "", NK_TEXT_LEFT); sk_rev_pose[k] = -1; continue; }
                if (rev_poses[idx] < 0) {  /* SECTION HEADER cell */
                    int a = -rev_poses[idx] - 2; char hd[140];
                    if (rev_poses[idx] == -1) snprintf(hd, sizeof hd, "OTHER\n(no animation\nnames these)");
                    else if (rev_poses[idx] == -2000) snprintf(hd, sizeof hd, "VICTIM\nBODIES\n(1024+)");
                    else if (a >= 0 && a < pi_n) snprintf(hd, sizeof hd, "%s %s%02X\n%s", pi_anim[a].kind, !strcmp(pi_anim[a].kind, "move") ? "0x" : "", pi_anim[a].id, pi_anim[a].name);
                    else hd[0] = 0;
                    if (nk_group_begin(ctx, hd, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
                        char *line = hd, *nl;
                        nk_layout_row_dynamic(ctx, 18, 1);
                        while (line && *line) { nl = strchr(line, '\n'); if (nl) *nl = 0; nk_label_colored(ctx, line, NK_TEXT_LEFT, nk_rgb(240, 200, 120)); line = nl ? nl + 1 : NULL; }
                        nk_group_end(ctx);
                    }
                    sk_rev_pose[k] = -1;
                    continue;
                }
                if (sk_rev_pose[k] != rev_poses[idx]) { sk_rev_pose[k] = rev_poses[idx]; sk_rev_mt[k] = 0; }
                snprintf(rp, sizeof rp, "jobs/%s/%s/pose_%04d.png", sk_name,
                         rev_miss[idx] ? "ref" : "out", rev_poses[idx]);
                sk_png_gray = rev_miss[idx];   /* a missing tile = the grey mannequin as sent */
                {   /* EVERY two-man frame previews over the template victim
                       body (was by-move only; the modal gets the same
                       underlay - user 2026-08-28) */
                    static char under[560]; int vid = -1;
                    if (rev_poses[idx] < 800) vid = pi_pair_victim(sk_base, rev_poses[idx]);
                    if (vid >= 0) {
                        struct stat vs;
                        snprintf(under, sizeof under, "jobs/stock-class%d/out/pose_%04d.png", sk_class, vid);
                        if (stat(under, &vs)) snprintf(under, sizeof under, "jobs/stock-class%d/ref/pose_%04d.png", sk_class, vid);
                        if (stat(under, &vs)) snprintf(under, sizeof under, "jobs/stock-class0/out/pose_%04d.png", vid);
                        sk_png_under = under;
                    }
                    sk_png_tex(rp, &sk_rev_tex[k], &sk_rev_mt[k]);
                    sk_png_under = NULL;
                }
                sk_png_gray = 0;
                if (!sk_rev_tex[k]) {          /* ALWAYS emit a widget: a hole
                                                  here shifted the row and sent
                                                  clicks to the wrong cell */
                    nk_label(ctx, "?", NK_TEXT_CENTERED);
                } else {
                    int miss_tile = idx < rev_n && rev_miss[idx], hit;
                    if (miss_tile) {       /* PLACEHOLDER look: the stock ref behind a thick red frame */
                        nk_style_push_color(ctx, &ctx->style.button.border_color, nk_rgb(230, 70, 60));
                        nk_style_push_float(ctx, &ctx->style.button.border, 4.0f);
                    }
                    { struct nk_rect tb = WB();
                      hit = nk_button_image(ctx, nk_image_ptr(sk_rev_tex[k]));
                      if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_RIGHT, tb) && sk_rq_find(rev_poses[idx]) < 0) { sk_rq_toggle(rev_poses[idx], ""); hit = 0; } }
                    if (miss_tile) { nk_style_pop_float(ctx); nk_style_pop_color(ctx); }
                    if (hit) {
                    sk_zoom_on = 2; sk_zoom_pose = rev_poses[idx]; sk_zoom_mt = 0; sk_zoom_out_fmt[0] = 0;
                    snprintf(sk_zoom_title, sizeof sk_zoom_title, "pose %04d", rev_poses[idx]);
                    /* (no log line for a zoom - user 2026-08-27) */
                    nk_window_show(ctx, "skin zoom", NK_SHOWN); nk_window_set_focus(ctx, "skin zoom");
                    }
                }
            }
            nk_layout_row_static(ctx, 14, 150, 8);
            for (int k = half * 8; k < half * 8 + 8; k++) {
                int idx = sk_rev_page * SK_REV_N + k;
                if (sk_rev_pose[k] < 0) { nk_label(ctx, "", NK_TEXT_LEFT); continue; }
                if (idx < rev_n && rev_miss[idx])
                    nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(255, 120, 90),
                                      "%04d MISSING", sk_rev_pose[k]);
                else if (sk_rq_find(sk_rev_pose[k]) >= 0) nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(255, 205, 100), "%04d  QUEUED", sk_rev_pose[k]);
                else if (idx < rev_n && rev_more[idx]) nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(180, 200, 240), "%04d  x%d", sk_rev_pose[k], rev_more[idx] + 1);
                else nk_labelf(ctx, NK_TEXT_CENTERED, "%04d", sk_rev_pose[k]);
            }
            }
        }
        if (rev_cache_n > 0) {   /* CARDS row: the 6 continue faces + the title card, always visible */
            static const int cards[7] = { ENG_CONT_POSE0, ENG_CONT_POSE0 + 1, ENG_CONT_POSE0 + 2,   /* ROM cells 0,1,2,4,5,6 - 3 and 7 are empty */
                                          ENG_CONT_POSE0 + 4, ENG_CONT_POSE0 + 5, ENG_CONT_POSE0 + 6, ENG_TITLE_POSE };
            static SDL_Texture *ctex[7]; static time_t cmt[7]; static char cskin[40];
            static int card_src[7];
            if (strcmp(cskin, sk_name)) { snprintf(cskin, sizeof cskin, "%s", sk_name); for (int k = 0; k < 7; k++) cmt[k] = 0; }
            static unsigned char card_hide[7];   /* an aliased face (804-806 with no ref of its own) is not shown at all (user 2026-08-29: "why not just draw 3?") */
            for (int k = 0; k < 7; k++) { struct stat cs2; char q[560]; card_hide[k] = 0;
                if (cards[k] >= 804 && cards[k] <= 806) { snprintf(q, sizeof q, "jobs/%s/ref/pose_%04d.png", sk_name, cards[k]); if (stat(q, &cs2) != 0) { snprintf(q, sizeof q, "jobs/%s/out/pose_%04d.png", sk_name, cards[k]); if (stat(q, &cs2) != 0) card_hide[k] = 1; } } }
            nk_layout_row_static(ctx, 110, 110, 8);   /* cards: square letterboxed textures in square cells */
            for (int k = 0; k < 7; k++) {
                int missk = 1, hit;
                if (card_hide[k]) continue;
                for (int i = 0; i < rev_cache_n; i++) if (revc_poses[i] == cards[k]) { missk = revc_miss[i]; break; }
                {   /* faces 4-6 are ALIASES of 1-3 in the template (identical cells): show the source */
                    int src = cards[k]; struct stat cs2; char q[560];
                    snprintf(q, sizeof q, "jobs/%s/ref/pose_%04d.png", sk_name, src);
                    if (cards[k] >= 804 && cards[k] <= 806 && stat(q, &cs2) != 0) { snprintf(q, sizeof q, "jobs/%s/out/pose_%04d.png", sk_name, src); if (stat(q, &cs2) != 0) src -= 4; }
                    if (src != cards[k]) { missk = 0; for (int i = 0; i < rev_cache_n; i++) if (revc_poses[i] == src) { missk = revc_miss[i]; break; } }
                    snprintf(rp, sizeof rp, "jobs/%s/%s/pose_%04d.png", sk_name, missk ? "ref" : "out", src);
                    card_src[k] = src;
                }
                sk_png_tex(rp, &ctex[k], &cmt[k]);   /* cards: bbox crop + square letterbox
                                          (square cells = undistorted); the POSE grid keeps
                                          the whole canvas for true scale */
                if (!ctex[k]) { nk_label(ctx, "-", NK_TEXT_CENTERED); continue; }
                if (missk) {
                    nk_style_push_color(ctx, &ctx->style.button.border_color, nk_rgb(230, 70, 60));
                    nk_style_push_float(ctx, &ctx->style.button.border, 4.0f);
                }
                hit = nk_button_image(ctx, nk_image_ptr(ctex[k]));
                if (missk) { nk_style_pop_float(ctx); nk_style_pop_color(ctx); }
                if (hit) {
                    sk_zoom_on = 2; sk_zoom_pose = cards[k]; sk_zoom_mt = 0; sk_zoom_out_fmt[0] = 0;
                    snprintf(sk_zoom_title, sizeof sk_zoom_title, "pose %04d", cards[k]);
                    nk_window_show(ctx, "skin zoom", NK_SHOWN); nk_window_set_focus(ctx, "skin zoom");
                }
            }
            nk_label(ctx, "", NK_TEXT_LEFT);
            nk_layout_row_static(ctx, 14, 110, 8);
            for (int k = 0; k < 7; k++) {
                int missk = 1;
                if (card_hide[k]) continue;
                for (int i = 0; i < rev_cache_n; i++) if (revc_poses[i] == cards[k]) { missk = revc_miss[i]; break; }
                if (missk) nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(255, 120, 90), "%s MISSING", k < 6 ? (const char *[]){"face 1","face 2","face 3","face 4","face 5","face 6"}[k] : "title");
                else if (k < 6) nk_labelf(ctx, NK_TEXT_CENTERED, "face %d  (%04d)", k + 1, cards[k]);
                else nk_labelf(ctx, NK_TEXT_CENTERED, "title  (%04d)", cards[k]);
            }
        }
        /* (the 'show ungenerated/missing' filter is gone - missing frames
           always show, user 2026-08-27) */
    }
    {   /* the run progress (bar + cyan text) lives in the STATUS BAR now
         * (user 2026-08-28): sk_run_status() */
        {   /* YOUR re-roll queue: "k / N" of the batch running (the script
             * writes reroll_queue.progress before each pose) + poses waiting */
            char pp[300]; FILE *pf; int k = 0, n = 0;
            snprintf(pp, sizeof pp, "jobs/%s/reroll_queue.progress", sk_name);
            pf = sk_name[0] ? fopen(pp, "r") : NULL;
            if (pf) { if (fscanf(pf, "%d %d", &k, &n) != 2) k = n = 0; fclose(pf); }
            if (n || sk_rq_n) {
                nk_layout_row_dynamic(ctx, 18, 1);
                if (n) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(255, 205, 100), "re-rolls: %d / %d running%s", k, n, sk_rq_n ? "" : "");
                if (n && sk_rq_n) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(255, 205, 100), "   + %d queued for the next batch", sk_rq_n);
                if (!n && sk_rq_n) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(255, 205, 100), "re-rolls queued: %d (start when the running job ends)", sk_rq_n);
            }
        }
    }
}

/* the skin run's progress out of run_state.json, drawn INTO THE BOTTOM
 * STATUS BAR (user 2026-08-28: "put the yellow progress bar and the cyan
 * text in the bottom task bar"). The caller has a static row open; this
 * pushes a 210 px bar + `textw` of text and returns 1, or pushes nothing
 * and returns 0 when the skin has no run. */
static int cl_sel_for_status(void);   /* the class viewer's selected class (defined with the Classes tab) */
static const char *ar_sel_name(void); /* the Arenas tab's selected arena */
static int sk_run_status(float textw)
{
    char p[300], phase[32] = ""; int done = 0, todo = 0, ok = 0, miss = 0; FILE *f;
    if (nav == NAV_CLASSES) snprintf(p, sizeof p, "data/generics/%d/_build/run_state.json", cl_sel_for_status());   /* a class build's progress */
    else if (nav == NAV_ARENAS) snprintf(p, sizeof p, "arenas/%s/_gen/run_state.json", ar_sel_name());              /* an arena recipe run */
    else { if (!sk_name[0]) return 0; snprintf(p, sizeof p, "jobs/%s/run_state.json", sk_name); }
    f = fopen(p, "r");
    if (!f) return 0;
    if (fscanf(f, "{\"phase\":\"%31[^\"]\",\"done\":%d,\"todo\":%d,\"ok\":%d,\"miss\":%d",
               phase, &done, &todo, &ok, &miss) < 3) { fclose(f); return 0; }
    fclose(f);
    {
        static const char spin[4] = { '|', '/', '-', '\\' };
        char runtag[24] = "";
        if (tool_busy || sk_run_alive())
            snprintf(runtag, sizeof runtag, "   (running %c)", spin[(SDL_GetTicks() / 250) & 3]);
        nk_layout_row_push(ctx, 210);
        nk_prog(ctx, (nk_size)(todo && ok < todo ? ok * 100 / todo : 100), 100, nk_false);   /* CLAMPED:
                                  ok can exceed todo (re-rolls) and nuklear paints a
                                  242%% cursor straight over the text */
        nk_layout_row_push(ctx, textw - 210 > 100 ? textw - 210 : 100);
        if (nav == NAV_CLASSES)
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 210, 255), "  class build: %s   %d / %d   failed %d%s", phase, ok, todo, miss, runtag);
        else if (nav == NAV_ARENAS)
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 210, 255), "  arena %s: %s   %d / %d pictures   missed %d%s", ar_sel_name(), phase, ok, todo, miss, runtag);
        else
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 210, 255),
                          "  run: %s   ok %d / %d   re-roll queue %d%s",
                          phase, ok, todo, miss, runtag);
    }
    return 1;
}

/* ------------------------------------------------------- CALIBRATE tab
 * (user 2026-08-25): the live paused match is the reference. Pick the man
 * in a move (state 5), see the composed pair from eng_compose with his
 * calibration applied, drag his body (or nudge), watch the game window
 * follow, Save -> data/classes/C/stock/calib.json (+ pack). Reused later
 * by move design: the same view will scrub authored frames. */
static SDL_Texture *cal_tex; static int cal_valid;
static int cal_obj = -1, cal_drag, cal_drag_x, cal_drag_y, cal_zoom = 2;
static int cal_held = -1, cal_held_obj = -1;   /* HELD-side editing: the held man's id (his victim cells move) */
static int cal_layer;              /* 0 = the CLASS template layer (base, shared by every skin of the class),
                                      1 = THIS SKIN's delta on top (user 2026-08-26); the canvas shows the sum */
static int cal_pkg(int held) { return held >= ENG_WS_MAX ? held : eng_ws_class_slot(eng_ws_body_class(held)); }   /* whose package holds his art (victim side) */
/* the package the chosen LAYER edits, given the package whose own art draws:
 * layer 0 -> its class template slot; layer 1 -> the skin itself (only when
 * it is a real skin, not the class slot) */
static int cal_target(int pkg)
{
    if (pkg < 0) return -1;
    if (cal_layer == 0) { int c = eng_calib_layer(pkg); return c < 0 ? pkg : c; }
    return eng_calib_layer(pkg) < 0 ? -1 : pkg;
}
static int cal_hpkg(int holder, unsigned pose)
{   /* whose OWN art draws the holder's body: the clone when he has the pose, else the class template (a borrowed move) — stock ROM art is never shifted */
    const eng_pkg_rec *pr;
    if (holder >= ENG_WS_MAX && eng_pkg_pose((unsigned)holder, pose, 0, -1, &pr) > 0 && eng_pkg_pose_was_own()) return holder;
    { int b = eng_ws_base(holder); if (eng_pkg_pose((unsigned)b, pose, 0, -1, &pr) > 0) return -1; }   /* stock frame: exact */
    return eng_ws_class_slot(eng_ws_body_class(holder));
}
static unsigned cal_krow(const eng_state *st, int holder_obj, int held)
{   /* the held man's victim-offset key row: 12 = "own base holds him" */
    int bh = eng_ws_base(st->obj[holder_obj].wrestler);
    return bh == eng_ws_base(held) ? 12u : (unsigned)bh;
}
static void cal_render(const eng_state *st)
{
    static uint32_t pix[256 * 256];
    const eng_pkg_rec *pr; const uint8_t *src; int n;
    const eng_obj *o; int victim = -1, flip;
    if (!cal_tex) cal_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 256);
    for (int i = 0; i < 256 * 256; i++) pix[i] = 0xFF303040u;
    if (cal_obj < 0 || cal_obj >= ENG_MAX_OBJS) { SDL_UpdateTexture(cal_tex, NULL, pix, 256 * 4); cal_valid = 1; return; }
    o = &st->obj[cal_obj];
    if (o->partner >= 0 && o->partner < ENG_MAX_OBJS) victim = st->obj[o->partner].wrestler;
    flip = (o->spr & 0x8000u) ? 1 : 0;
    { int k = eng_calib_key(o); if (k >= 0) eng_compose_ctx(eng_ws_body_class(o->wrestler), k, (int)(o->frame & 0xFFu)); }
    n = eng_compose(o->wrestler, o->spr & 0x7FFFu, flip, victim, &pr, &src);
    eng_compose_ctx(-1, -1, -1);
    for (int i = 0; i < n; i++) {   /* same raster as pose_render; the holder's cells get a
                                       faint warm tint so you can tell the two bodies apart */
        const uint16_t *pens = eng_pkg_palette((unsigned)(src[i] == ENG_SRC_VICTIM || src[i] == ENG_SRC_VICTIM_OWN
                                                          ? (victim >= 0 ? victim : o->wrestler) : o->wrestler));
        int x = pr[i].x + 128, y = pr[i].y + 0x80, xpos = x & 0x1FF, ypos;
        unsigned chain = pr[i].chain & 7u;
        int holder = src[i] == ENG_SRC_HOLDER || src[i] == ENG_SRC_HOLDER_OWN || src[i] == ENG_SRC_OVERLAY;
        if (xpos > 512 - 16) xpos -= 512;
        ypos = ((256 - y) & 0x1FF) - 16;
        for (unsigned c = 0; c <= chain; c++) {
            const uint8_t *t = wf_video_tile_pens((unsigned)(pr[i].tile + c));
            int dy = pr[i].flipy ? ypos - (int)(16 * chain) + (int)(16 * c) : ypos - (int)(16 * c);
            if (!t) continue;
            for (int py = 0; py < 16; py++) for (int px = 0; px < 16; px++) {
                int qx = pr[i].flipx ? 15 - px : px, qy = pr[i].flipy ? 15 - py : py;
                uint8_t pen = t[qy * 16 + qx];
                int ox = xpos + px, oy = dy + py + 64;
                unsigned w;
                if (!pen || ox < 0 || ox >= 256 || oy < 0 || oy >= 256) continue;
                w = pens ? pens[pen] : (unsigned)(pen * 0x111);
                pix[oy * 256 + ox] = 0xFF000000u | ((w & 0xF) * 17u) << 16 | (((w >> 4) & 0xF) * 17u) << 8 | (((w >> 8) & 0xF) * 17u);
                if (holder && ((ox + oy) & 7) == 0) pix[oy * 256 + ox] |= 0x00404000u;   /* dotted tint */
            }
        }
    }
    SDL_UpdateTexture(cal_tex, NULL, pix, 256 * 4);
    cal_valid = 1;
}
static void draw_calib(eng_state *st)
{
    struct nk_rect b; int cls, mv, fr, dx, dy, hpkg = -1;
    const eng_obj *o;
    heading("Calibrate contact", "Pause the game on a hold or a strike, pick a man below, drag on the canvas until the bodies meet; the game window follows live. Pick the HOLDER = his body moves (saved in his skin's package per move/stance + frame); pick the HELD man = his victim cells move (saved in his skin per holder pose). Stock ROM art is exact and never shifted. A left-facing correction mirrors for right-facing.");
    if (fg_cat_state == -1) fg_catalog_load();
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_labelf_colored(ctx, NK_TEXT_LEFT, paused ? nk_rgb(150, 220, 150) : nk_rgb(240, 180, 60), paused ? "game PAUSED - step with the Match tab or ." : "game RUNNING - press P (or pause in Match) to freeze a frame");
    /* attackers: objects in state 5 — auto-pick the one holding/hitting someone
       (the victim of a hold is state 0xFF, hidden inside the holder's frame) */
    /* cal_sel = the man the user CLICKED; cal_obj = the man whose frame is
       drawn (the holder). Clicking the HELD man (sprite hidden, 0x7FFF)
       edits the held side. Kept apart so the auto-pick and the held->holder
       redirect never undo a click (user 2026-08-26: "won't let me click my
       man" - the auto-pick re-selected the attacker every frame). */
    {
        static int cal_sel = -1;
        int held_ok = cal_sel >= 0 && cal_sel < ENG_MAX_OBJS && st->obj[cal_sel].active &&
                      (st->obj[cal_sel].spr & 0x7FFFu) == 0x7FFFu && st->obj[cal_sel].partner >= 0 &&
                      st->obj[cal_sel].partner < ENG_MAX_OBJS && st->obj[st->obj[cal_sel].partner].active;
        int att_ok = cal_sel >= 0 && cal_sel < ENG_MAX_OBJS && st->obj[cal_sel].active && eng_calib_key(&st->obj[cal_sel]) >= 0;
        if (!held_ok && !att_ok)       /* nothing useful selected: the man doing a move */
            for (int i = 0; i < ENG_MAX_OBJS; i++)
                if (st->obj[i].active && eng_calib_key(&st->obj[i]) >= 0 && st->obj[i].partner >= 0 && (st->obj[i].spr & 0x7FFFu) != 0x7FFFu) { cal_sel = i; att_ok = 1; cal_valid = 0; break; }
        nk_layout_row_dynamic(ctx, 24, 6);
        for (int i = 0; i < ENG_MAX_OBJS; i++) {
            char lab[64];
            o = &st->obj[i];
            if (!o->active) continue;
            snprintf(lab, sizeof lab, "o%d %s%s", i, ws_name(o->wrestler),
                     eng_calib_key(o) >= 0 && (o->spr & 0x7FFFu) != 0x7FFFu ? " [move]" : (o->spr & 0x7FFFu) == 0x7FFFu && o->partner >= 0 ? " [held]" : "");
            if (nk_selectable_label(ctx, lab, NK_TEXT_LEFT, &(int){ cal_sel == i })) { cal_sel = i; cal_valid = 0; }
        }
        if (cal_sel < 0 || cal_sel >= ENG_MAX_OBJS || !st->obj[cal_sel].active) { nk_layout_row_dynamic(ctx, 24, 1); nk_label(ctx, "pick the attacker, or the held man", NK_TEXT_LEFT); return; }
        cal_held = -1;
        if (held_ok) {
            /* the HELD man: his body is the victim cells inside the holder's
               frame. Editing the HELD SIDE: his own victim cells move (per his
               package, keyed by the holder's pose); the holder draws the pair,
               so the preview/context is the holder's frame. */
            cal_held = st->obj[cal_sel].wrestler; cal_held_obj = cal_sel;
            if (cal_obj != st->obj[cal_sel].partner) cal_valid = 0;
            cal_obj = st->obj[cal_sel].partner;
        } else { if (cal_obj != cal_sel) cal_valid = 0; cal_obj = cal_sel; }
        o = &st->obj[cal_obj];
    }
    cls = eng_ws_body_class(o->wrestler); mv = eng_calib_key(o); fr = (int)(o->frame & 0xFFu);
    hpkg = cal_hpkg(o->wrestler, o->spr & 0x7FFFu);
    /* LAYER switch: the class template is the base every skin of the class
       shares; the skin's own table is a delta on top. dx,dy below = the
       layer being edited; the sum is what the canvas and the game draw. */
    {
        int pkg = cal_held >= 0 ? cal_pkg(cal_held) : hpkg, has_skin = pkg >= 0 && eng_calib_layer(pkg) >= 0;
        static const char *lay[2] = { "class template (all skins of the class)", "this skin (delta on top)" };
        nk_layout_row_dynamic(ctx, 24, 3);
        nk_label(ctx, "edit layer:", NK_TEXT_RIGHT);
        if (nk_option_label(ctx, lay[0], cal_layer == 0)) cal_layer = 0;
        if (nk_option_label(ctx, lay[1], cal_layer == 1) && has_skin) cal_layer = 1;
        if (cal_layer == 1 && !has_skin) cal_layer = 0;   /* a class slot / stock man has no skin layer */
    }
    if (cal_held >= 0) {               /* HELD side: the offset lives in the held man's package */
        unsigned krow = cal_krow(st, cal_obj, cal_held), hpose = o->spr & 0x7FFFu;
        int sx, sy, tgt = cal_target(cal_pkg(cal_held));
        eng_voff_sum(cal_pkg(cal_held), krow, hpose, &sx, &sy);
        eng_voff_get(tgt, krow, hpose, &dx, &dy);
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 220, 150), "HELD SIDE: o%d %s's own cells move   holder o%d %s pose 0x%03X %s   %s layer %+d,%+d   drawn sum %+d,%+d   (click the holder above to edit his side instead)",
                          cal_held_obj, ws_name(cal_held), cal_obj, ws_name(o->wrestler), hpose, (o->spr & 0x8000u) ? "(facing right)" : "(facing left)",
                          cal_layer ? "skin" : "class", dx, dy, sx, sy);
        mv = 0x9F;                      /* any valid key: enables the controls */
    } else {
        int sx = 0, sy = 0, tgt = cal_target(hpkg);
        if (hpkg >= 0) { eng_calib_sum(hpkg, mv, fr, &sx, &sy); eng_calib_get(tgt, mv, fr, &dx, &dy); }
        nk_layout_row_dynamic(ctx, 22, 1);
        if (mv >= 0 && hpkg < 0)
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "o%d %s draws this pose from STOCK ROM art (exact by construction) - nothing to calibrate on the holder side; pick the held man to move his cells", cal_obj, ws_name(o->wrestler));
        else if (mv >= 0)
            nk_labelf(ctx, NK_TEXT_LEFT, "%s (package %d, editing %s %d)   %s 0x%02X %s   frame %d   pose 0x%03X %s   opponent %s   %s layer %+d,%+d   drawn sum %+d,%+d",
                      ws_name(hpkg), hpkg, cal_layer ? "skin" : "class slot", tgt, mv < 0x90 ? "move" : "stance", mv < 0x90 ? mv : mv - 0x90,
                      mv < 0x90 && fg_mvname[mv & 0xFF][0] ? fg_mvname[mv & 0xFF] : (mv == 0x9C ? "tie-up" : mv == 0x9B ? "lockup" : ""), fr, o->spr & 0x7FFFu,
                      (o->spr & 0x8000u) ? "(facing right)" : "(facing left)", o->partner >= 0 ? ws_name(st->obj[o->partner].wrestler) : "-",
                      cal_layer ? "skin" : "class", dx, dy, sx, sy);
        else
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "o%d is not drawing a pair (state %02X, no partner) - pick the man doing the move/hold; nudges are disabled", cal_obj, o->state & 0xFFu);
    }
    if (o->partner >= 0 && o->partner < ENG_MAX_OBJS && st->obj[o->partner].active) {
        /* DEPTH override (user 2026-08-26): which body draws on top for this
           holder pose vs the held man's body class; stored in the holder's
           package on the chosen layer (skin wins over class) */
        int hp = o->wrestler >= ENG_WS_MAX ? o->wrestler : eng_ws_class_slot(eng_ws_body_class(o->wrestler));
        int tgt = cal_target(hp); unsigned hpose = o->spr & 0x7FFFu, vcls = (unsigned)eng_ws_body_class(st->obj[o->partner].wrestler);
        int cur = tgt >= 0 ? eng_depth_get(tgt, hpose, vcls) : 0, eff = hp >= 0 ? eng_depth_sum(hp, hpose, vcls) : 0, pick = cur;
        static const char *dm[3] = { "template interleave", "holder over held", "held over holder" };
        nk_layout_row_dynamic(ctx, 24, 5);
        nk_labelf(ctx, NK_TEXT_RIGHT, "draw order (%s layer):", cal_layer ? "skin" : "class");
        for (int m = 0; m < 3; m++) if (nk_option_label(ctx, dm[m], cur == m) && tgt >= 0) pick = m;
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "in force: %s", dm[eff]);
        if (pick != cur && tgt >= 0) { eng_depth_set(tgt, hpose, vcls, pick); cal_valid = 0; }
    }
#define CAL_NUDGE(ddx, ddy) do { \
        if (cal_held >= 0) eng_voff_set(cal_target(cal_pkg(cal_held)), cal_krow(st, cal_obj, cal_held), o->spr & 0x7FFFu, dx + (ddx), dy + (ddy)); \
        else if (hpkg >= 0) eng_calib_set(cal_target(hpkg), mv, fr, dx + (ddx), dy + (ddy)); \
        cal_valid = 0; } while (0)
    if (!cal_valid || !paused) cal_render(st);
    {
        int Z = cal_zoom < 1 ? 1 : cal_zoom;
        nk_layout_row_static(ctx, 256 * Z, 256 * Z, 1);
        b = WB();
        if (cal_tex) nk_image(ctx, nk_image_ptr(cal_tex));
        if (nk_input_is_mouse_hovering_rect(&ctx->input, b) && mv >= 0 && (cal_held >= 0 || hpkg >= 0)) {
            if (nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
                int mx = (int)ctx->input.mouse.pos.x, my = (int)ctx->input.mouse.pos.y;
                if (!cal_drag) { cal_drag = 1; cal_drag_x = mx; cal_drag_y = my; }
                else if (mx != cal_drag_x || my != cal_drag_y) {
                    int ddx = (mx - cal_drag_x) / Z, ddy = (my - cal_drag_y) / Z;
                    if (ddx || ddy) {
                        if (o->spr & 0x8000u) ddx = -ddx;   /* authored in left-facing terms */
                        CAL_NUDGE(ddx, -ddy);               /* record y grows UP the screen */
                        cal_drag_x += ddx * Z * ((o->spr & 0x8000u) ? -1 : 1); cal_drag_y += ddy * Z;
                        cal_valid = 0;
                    }
                }
            } else cal_drag = 0;
        } else cal_drag = 0;
    }
    nk_layout_row_dynamic(ctx, 26, 8);
    {
        int inmove = mv >= 0 && (cal_held >= 0 || hpkg >= 0);
        if (!inmove) nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(90, 94, 104));
        if (nk_button_label(ctx, "< 1px") && inmove) CAL_NUDGE(-1, 0);
        if (nk_button_label(ctx, "1px >") && inmove) CAL_NUDGE(1, 0);
        if (nk_button_label(ctx, "up 1px") && inmove) CAL_NUDGE(0, 1);
        if (nk_button_label(ctx, "down 1px") && inmove) CAL_NUDGE(0, -1);
        if (nk_button_label(ctx, "clear frame") && inmove) CAL_NUDGE(-dx, -dy);
        if (!inmove) nk_style_pop_color(ctx);
    }
    { int z = cal_zoom; nk_property_int(ctx, "zoom", 1, &z, 3, 1, 1); if (z != cal_zoom) { cal_zoom = z; } }
    if (nk_button_label(ctx, cal_held >= 0 ? (cal_layer ? "Save skin victoffs.json" : "Save class victoffs.json")
                                           : (cal_layer ? "Save skin calib.json" : "Save class calib.json"))) {   /* not layer-gated: class data / the man's own package */
        /* the edited LAYER's package: a class slot -> data/classes/C/stock,
           a skin -> its mod dir (wrestlers/NN) */
        char dir[300], path[360];
        int tgt = cal_target(cal_held >= 0 ? cal_pkg(cal_held) : hpkg);
        if (tgt >= 0) {
            if (!eng_ws_hidden(tgt)) {
                char rel[64], probe[300];
                snprintf(rel, sizeof rel, "wrestlers/%02d/wrestler.json", tgt);
                if (wf_mod_resolve(rel, probe, sizeof probe)) dirname_of(probe, dir, sizeof dir);
                else snprintf(dir, sizeof dir, "mods/%s/wrestlers/%02d", mod_layer[0] ? mod_layer : "unknown", tgt);
            } else snprintf(dir, sizeof dir, "data/classes/%d/stock", eng_ws_body_class(eng_ws_base(tgt)));
            mkdir_p(dir);
            if (cal_held >= 0) {
                snprintf(path, sizeof path, "%s/victoffs.json", dir);
                if (eng_voff_save(tgt, path) == 0) logf_("saved %s (%d entries) - Pack to bake it in", path, eng_voff_count(tgt));
                else logf_("cannot write %s", path);
            } else {
                snprintf(path, sizeof path, "%s/calib.json", dir);
                if (eng_calib_save(tgt, path) == 0) logf_("saved %s (%d entries) - Pack to bake it in", path, eng_calib_count(tgt));
                else logf_("cannot write %s", path);
            }
        }
        {   /* the draw-order overrides live in the HOLDER's package on the same layer */
            int hp = o->wrestler >= ENG_WS_MAX ? o->wrestler : eng_ws_class_slot(eng_ws_body_class(o->wrestler));
            int dt = cal_target(hp);
            if (dt >= 0 && dt != tgt) {   /* a different package than the offsets (held-side edit): resolve its dir */
                if (!eng_ws_hidden(dt)) {
                    char rel[64], probe[300];
                    snprintf(rel, sizeof rel, "wrestlers/%02d/wrestler.json", dt);
                    if (wf_mod_resolve(rel, probe, sizeof probe)) dirname_of(probe, dir, sizeof dir);
                    else snprintf(dir, sizeof dir, "mods/%s/wrestlers/%02d", mod_layer[0] ? mod_layer : "unknown", dt);
                } else snprintf(dir, sizeof dir, "data/classes/%d/stock", eng_ws_body_class(eng_ws_base(dt)));
                mkdir_p(dir);
            }
            if (dt >= 0 && (eng_depth_count(dt) || sk_have("%s/depth.json", dir))) {
                snprintf(path, sizeof path, "%s/depth.json", dir);
                if (eng_depth_save(dt, path) == 0) logf_("saved %s (%d draw-order overrides)", path, eng_depth_count(dt));
            }
        }
    }
    if (nk_button_label(ctx, "Pack profile")) {
        char cmd[200];
        logf_("packing profile %s (repacks its paks with the saved calib.json; the live game keeps the in-memory table)", wf_profile()[0] ? wf_profile() : "stock");
        snprintf(cmd, sizeof cmd, "%s", pack_cmd());
        run_tool_modal(cmd);
    }
    nk_layout_row_dynamic(ctx, 20, 1);
    {
        int tgt = cal_target(cal_held >= 0 ? cal_pkg(cal_held) : hpkg);
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "%d holder-side frames + %d held-side offsets in the %s layer (package %d), in memory (live). Drag moves the side being edited; the other stays put. './wfengine --calib-report' lists both layers per package.",
                          tgt >= 0 ? eng_calib_count(tgt) : 0, tgt >= 0 ? eng_voff_count(tgt) : 0, cal_layer ? "skin" : "class", tgt);
    }
    {   /* the tool log's tail: Pack progress / result right here */
        nk_layout_row_dynamic(ctx, 18, 1);
        for (int i = log_n > 3 ? log_n - 3 : 0; i < log_n; i++) {
            int k = (log_head - log_n + i + 64) % 64;
            nk_label_colored(ctx, logbuf[k], NK_TEXT_LEFT, tool_busy ? nk_rgb(240, 180, 60) : nk_rgb(120, 190, 120));
        }
        if (tool_busy) nk_label_colored(ctx, "packing...", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
    }
}

static void draw_forge(void)
{
    struct nk_rect b;
    if (fg_cat_state < 0) fg_catalog_load();
    if (!fg_inited) { fg_inited = 1; fg_default_mod(); }
    if (fg_base_loaded != fg_base) fg_load_base();

    if (fg_reload_slot >= 0 && !tool_busy) {   /* the pack finished: the slot
                                                  registry shows the new name */
        fg_repoint_pakdir();
        eng_pkg_reload((unsigned)fg_reload_slot);
        logf_("Forge: slot %d now reads %s", fg_reload_slot,
              eng_ws_clone_name(fg_reload_slot) ? eng_ws_clone_name(fg_reload_slot) : "(free)");
        fg_reload_slot = -1;
        fg_seen = -1;                       /* the Wrestlers page re-reads the slot (user 2026-08-27) */
        ws_loaded = -1; pose_tex_valid = 0;
    }
    /* ---- identity ---- */
    if (fg_sub == 0) {
    heading("Identity", NULL);
    {   /* one aligned grid (user 2026-08-27): label 90 | field 220 | note.
           Order: name, SKIN (picking one fixes the base's class), plays-like
           (the base: free with base art, same-class only under a skin),
           slot (the roster drives it), body template. */
        static char names[16][40]; static const char *items[17];
        int nsk = fg_skins_for_base(names), cur = 0, nw, skin_cls = -1;
        static const char *const members[ENG_BODY_CLASSES] = {
            "Hogan, Warrior, Hawk, Animal", "Jake, DiBiase, Smash, Crush",
            "Perfect", "Boss Man, Slaughter", "Earthquake" };
        items[0] = "(base art)";
        for (int k = 0; k < nsk; k++) { items[k + 1] = names[k]; if (!strcmp(fg_skin, names[k])) { cur = k + 1; skin_cls = fg_skin_cls[k]; } }
#define ID_ROW(lbl) do { nk_layout_row_begin(ctx, NK_STATIC, 24, 3); nk_layout_row_push(ctx, 90); nk_label(ctx, lbl, NK_TEXT_RIGHT); nk_layout_row_push(ctx, 220); } while (0)
#define ID_END() do { nk_layout_row_end(ctx); } while (0)
        ID_ROW("name");
        b = WB();
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, fg_name, 11, nk_filter_default);
        for (char *p = fg_name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        fg_name[10] = 0;
        hint_at(b, "Ring name, uppercase, max 10 characters = the width of a select cell; the aisle card and result screens use it too.");
        ID_END();

        ID_ROW("skin");
        b = WB();
        nw = nk_combo(ctx, items, nsk + 1, cur, 22, nk_vec2(220, 260));
        if (nw != cur) {
            snprintf(fg_skin, sizeof fg_skin, "%s", nw ? items[nw] : "");
            if (nw && fg_skin_base[nw - 1] >= 0 && fg_skin_base[nw - 1] < ENG_WS_MAX) fg_base = fg_skin_base[nw - 1];   /* the skin's own base */
            else if (nw && fg_skin_cls[nw - 1] >= 0 && eng_ws_body_class(fg_base) != fg_skin_cls[nw - 1])
                for (int i = 0; i < ENG_WS_MAX; i++) if (eng_ws_body_class(i) == fg_skin_cls[nw - 1]) { fg_base = i; break; }
            skin_cls = nw ? fg_skin_cls[nw - 1] : -1;
        }
        hint_at(b, "A FINISHED skin from the Skins tab: its frames ingest into this slot and its portrait becomes the select cell. Picking one sets the base to the skin's own; '(base art)' = the stock look.");
        nk_layout_row_push(ctx, 400);
        nk_label_colored(ctx, nw || cur ? "the skin fixes the body class; 'plays like' offers that class only" : "", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        ID_END();

        ID_ROW("plays like");
        b = WB();
        {   /* the base: every stock man with base art, the skin's class under a skin */
            const char *bi[ENG_WS_MAX]; int bmap[ENG_WS_MAX], nb = 0, bsel = 0;
            for (int i = 0; i < ENG_WS_MAX; i++)
                if (skin_cls < 0 || eng_ws_body_class(i) == skin_cls) { if (i == fg_base) bsel = nb; bmap[nb] = i; bi[nb++] = ws_names[i]; }
            if (nb) { int bw = nk_combo(ctx, bi, nb, bsel, 22, nk_vec2(220, 320)); if (bmap[bw] != fg_base) fg_base = bmap[bw]; }
        }
        hint_at(b, "The stock wrestler he plays like: moveset, AI, announcer (and art until a skin dresses him). Changing it reloads the stats, move map and palette with that base's values.");
        nk_layout_row_push(ctx, 400);
        {   int bc = eng_ws_body_class(fg_base);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "body class %d %s  (%s)", bc, eng_body_class_name(bc), members[bc]); }
        ID_END();

#undef ID_ROW
#undef ID_END
    }

    if (eng_ws_clone_base(fg_slot) >= 0) {   /* stats / palette only once the record exists (user 2026-08-27) */
    /* ---- stats ---- */
    heading("Stats", "Defaults are the base's package values; saved into stats.json. HP takes effect at match init.");
    nk_layout_row_static(ctx, 24, 220, 1);   /* stacked, short (user 2026-08-27) */
    b = WB(); nk_property_int(ctx, "energy (hp)", 1, &fg_hp, 255, 1, 1);
    if (nk_input_is_mouse_hovering_rect(&ctx->input, b))
        nk_tooltipf(ctx, "Starting/maximum energy. Base %s: %d", ws_name(fg_base), eng_pkg_stat((unsigned)fg_base, "hp", 100));
    b = WB(); nk_property_int(ctx, "walk speed", 1, &fg_walk, 255, 1, 1);
    if (nk_input_is_mouse_hovering_rect(&ctx->input, b))
        nk_tooltipf(ctx, "Polar walk speed, 4.4 fixed (16 = one pixel per frame). Base %s: %d", ws_name(fg_base), eng_pkg_stat((unsigned)fg_base, "walk", 20));
    b = WB(); nk_property_int(ctx, "run speed", 1, &fg_run, 255, 1, 1);
    if (nk_input_is_mouse_hovering_rect(&ctx->input, b))
        nk_tooltipf(ctx, "Run/whip speed, same 4.4 scale. Base %s: %d", ws_name(fg_base), eng_pkg_stat((unsigned)fg_base, "run", 40));

    /* ---- move map ---- */
    }   /* registered (stats) */
    }
    if (fg_sub == 1) {
    heading("Move map", "All 21 categories x 3 button columns of his move map. Pick from the moves ANY stock wrestler routes there (the safe list) or none (FF = unrouted). Amber = differs from the base; hover an amber cell for the base move.");
    {
        float hd[4] = { 0.27f, 0.2433f, 0.2433f, 0.2434f };
        nk_layout_row(ctx, NK_DYNAMIC, 18, 4, hd);
        nk_label_colored(ctx, "category", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "B1", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "B2", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "B1+B2", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
    }
    {   /* flat rows, no scroll box (user 2026-08-27) */
        float mr[4] = { 0.26f, 0.2433f, 0.2433f, 0.2434f };
        for (int c = 0; c < FG_NCAT; c++) {
            nk_layout_row(ctx, NK_DYNAMIC, 24, 4, mr);
            { char lab[48];
              snprintf(lab, sizeof lab, "%2d %s", c, fg_cat_lab[c][0] ? fg_cat_lab[c] : "?");
              nk_label(ctx, lab, NK_TEXT_LEFT); }
            for (int col = 0; col < 3; col++) fg_cell(c, col);
        }
    }
    if (fg_cat_state == 0) note("data/movecatalog.json missing - run ./wfengine --move-catalog for the pick lists.");
    }
    if (fg_sub == 2 && eng_ws_clone_base(fg_slot) >= 0) {
        /* ---- SOUND MAP sub-tab (user 2026-08-28): what this slot resolves
           to, with the source, and a slot-level override ("(inherit)" = the
           skin's, else the base's stock clip) ---- */
        static char sk_cache_name[40], sk_cache_snd[ENG_SND_N][48];
        if (strcmp(sk_cache_name, fg_skin)) {
            char sp[400], err[128]; json_val *sd;
            snprintf(sk_cache_name, sizeof sk_cache_name, "%s", fg_skin);
            for (int e = 0; e < ENG_SND_N; e++) sk_cache_snd[e][0] = 0;
            if (fg_skin[0] && sk_resolve(fg_skin, sp, sizeof sp)) {
                strcat(sp, "/skin.json");
                sd = json_parse_file(sp, err, sizeof err);
                if (sd) { for (int e = 0; e < ENG_SND_N; e++) snprintf(sk_cache_snd[e], 48, "%s", json_str(json_get(json_get(sd, "sounds"), snd_ev_key[e]), "")); json_free(sd); }
            }
        }
        heading("Sound map", "the wrestler's own sounds: each event resolves slot override -> skin -> base. Pick a stock clip or a WAV from the sounds/ library (Sounds > Library); Save wrestler writes a slot override into wrestler.json.");
        for (int e = 0; e < ENG_SND_N; e++) {
            nk_layout_row_begin(ctx, NK_STATIC, 24, 4);
            nk_layout_row_push(ctx, 90); nk_label(ctx, snd_ev_lbl[e], NK_TEXT_RIGHT);
            nk_layout_row_push(ctx, 300);
            snd_ref_combo(fg_snd[e], sizeof fg_snd[e], "(inherit)", 300);
            nk_layout_row_push(ctx, 50);
            { const char *eff = fg_snd[e][0] ? fg_snd[e] : sk_cache_snd[e];
              if (eff[0]) { if (nk_button_label(ctx, "Play")) snd_ref_play(eff); } else nk_label(ctx, "", NK_TEXT_LEFT); }
            nk_layout_row_push(ctx, 400);
            if (fg_snd[e][0]) nk_label_colored(ctx, "slot override (Save wrestler writes it)", NK_TEXT_LEFT, nk_rgb(240, 210, 140));
            else if (sk_cache_snd[e][0]) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 210, 255), "from skin '%s': %s", fg_skin, sk_cache_snd[e]);
            else nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(120, 190, 120), "base %s (stock clip)", ws_name(fg_base));
            nk_layout_row_end(ctx);
        }
    }
    if (fg_sub == 0 || fg_sub == 2) {
    /* ---- actions (text-sized, user 2026-08-27; also under Sound map) ---- */
    nk_layout_row_dynamic(ctx, 12, 1); nk_label(ctx, "", NK_TEXT_LEFT);   /* breathing space above the buttons */
    nk_layout_row_static(ctx, 24, 150, 3);
    b = WB();
    if (({ int h = nk_button_label(ctx, eng_ws_clone_base(fg_slot) >= 0 ? "Save wrestler" : "Add"); save_icon(b, 0); h; }))
        { pend_fg_on = 1; pend_fg_frames = 0; }   /* sheet first, work next frame (2026-08-27) */
    hint_at(b, "Write wrestler.json + stats.json + palette.json into the target mod (OVERWRITES the slot's existing files when he already exists), repack the profile. data/ and stock are never touched.");
    b = WB();
    if (nk_button_label(ctx, "Reset to base")) fg_load_base();
    hint_at(b, "Throw the form away: reload the base's stats, move map and palette.");
    {   /* delete: two-step arm/confirm (destructive). Removes the slot's
           package + select portrait + outfits from its OWNING mod; the
           repack drops the stale pak (V508) so he leaves the select grid. */
        static int arm_slot = -1; static Uint32 arm_t;
        int registered = eng_ws_clone_base(fg_slot) >= 0;
        if (arm_slot == fg_slot && SDL_GetTicks() - arm_t > 4000) arm_slot = -1;
        b = WB();
        if (!registered || tool_busy) {
            if (sk_button_gated("Delete wrestler", 0, registered ? "a tool is running" : "slot is free")) {}
        } else if (arm_slot != fg_slot) {
            if (nk_button_label(ctx, "Delete wrestler")) { arm_slot = fg_slot; arm_t = SDL_GetTicks(); }
        } else {
            struct nk_style_button warn = ctx->style.button;
            warn.normal = warn.hover = warn.active = nk_style_item_color(nk_rgb(120, 30, 30));
            if (nk_button_label_styled(ctx, &warn, "REALLY delete?")) {
                if (slot_delete(fg_slot)) fg_reload_slot = fg_slot;
                arm_slot = -1;
            }
        }
        hint_at(b, "Remove this forged wrestler from his mod (package, select portrait, outfits) and repack - the slot frees up. His SKIN (if any) stays in the skins library. Click twice to confirm.");
    }
    }   /* fg_sub == 0 || 2 */
}

static void profile_delete(int i)
{
    char pth[320], cmd[400]; int editing;
    if (i < 0 || i >= n_profs) return;
    editing = !strcmp(wf_profile(), prof_list[i]);
    snprintf(pth, sizeof pth, "profiles/%s.json", prof_list[i]);
    remove(pth);
    snprintf(cmd, sizeof cmd, "rm -rf \"build/profiles/%s\"", prof_list[i]);
    if (system(cmd) != 0) logline("pak cache removal failed");
    logf_("profile %s deleted (its mod layers in mods/ are KEPT)", prof_list[i]);
    if (editing) switch_edit_profile("");
    read_profiles(); cur_prof = -1;
}
static void draw_mods(void)
{
    if (pf_loaded == -2) { read_profiles(); read_mods(); load_profile_edit(-1); }
    heading("Add new profile", NULL);

    /* ---- create ---- */
    nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
    nk_layout_row_push(ctx, 240);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, new_prof, sizeof new_prof, nk_filter_default);
    nk_layout_row_push(ctx, 120);
    if (nk_button_label(ctx, "Create profile") && new_prof[0] && n_profs < 16) {
        char pth[300]; FILE *f;
        mkdir_p("profiles");
        snprintf(pth, sizeof pth, "profiles/%s.json", new_prof);
        f = fopen(pth, "w");
        if (f) {
            fprintf(f, "{\n  \"name\": "); json_write_string(f, new_prof);
            /* the profile's own save layer (user 2026-08-27: no layer
               dialog any more - every profile owns mods/<name>) */
            fprintf(f, ",\n  \"description\": \"\",\n  \"mode\": \"tag\",\n  \"mods\": ["); json_write_string(f, new_prof); fprintf(f, "],\n  \"disabled\": []\n}\n"); fclose(f);
            snprintf(pth, sizeof pth, "mods/%s", new_prof); mkdir_p(pth);
            read_mods(); read_profiles();
            for (int i = 0; i < n_profs; i++) if (!strcmp(prof_list[i], new_prof)) { cur_prof = i; load_profile_edit(i); }
            logf_("profile %s created - tick its mods below, Save, Launch", new_prof);
            new_prof[0] = 0;
        } else logf_("cannot write %s", pth);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 16, 1); nk_label(ctx, "", NK_TEXT_LEFT);   /* breathing space above the table */

    /* ---- table: name | description | edit | launch (buttons text-sized,
       user 2026-08-27) ---- */
    {
#define PROF_ROW(h) do { nk_layout_row_template_begin(ctx, h); nk_layout_row_template_push_static(ctx, 240); \
        nk_layout_row_template_push_static(ctx, 110); \
        nk_layout_row_template_push_static(ctx, 70); nk_layout_row_template_push_dynamic(ctx); nk_layout_row_template_end(ctx); } while (0)
        PROF_ROW(20);
        nk_label_colored(ctx, "PROFILE", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "MODE", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        nk_label_colored(ctx, "DESCRIPTION", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        PROF_ROW(24);
        nk_label_colored(ctx, "stock", NK_TEXT_LEFT, nk_rgb(120, 190, 120));
        nk_label(ctx, "", NK_TEXT_LEFT);
        if (nk_button_label(ctx, "Launch")) { wf_editor_kill_tools(); unsetenv("WF_PAKDIR"); unsetenv("WF_GFXPAK"); unsetenv("WF_PAK"); execl("./wfengine", "wfengine", "--editor", "--play", (char *)NULL); }
        nk_label_colored(ctx, "the original game - zero mods, locked", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        {
            /* flat list, silently CLUSTERED by the profiles' "category"
             * field (labels removed on user request 2026-08-24) */
            char done[16] = {0};
            for (int pass = 0; pass < n_profs; pass++) {
                const char *cat = NULL;
                for (int i = 0; i < n_profs; i++)
                    if (!done[i]) { cat = prof_cat[i]; break; }
                if (!cat) break;
                for (int i = 0; i < n_profs; i++) {
                    int sel, editing;
                    if (done[i] || strcmp(prof_cat[i], cat)) continue;
                    done[i] = 1;
                    editing = !strcmp(wf_profile(), prof_list[i]);
                    sel = editing;                        /* the row highlight = the profile being edited (user 2026-08-27) */
                    PROF_ROW(24);
                    if (editing) nk_style_push_color(ctx, &ctx->style.selectable.text_normal, nk_rgb(255, 205, 100));
                    { struct nk_rect nb = WB();
                    if (nk_selectable_label(ctx, prof_list[i], NK_TEXT_LEFT, &sel) && sel && !editing)
                        { cur_prof = i; request_edit_profile(prof_list[i]); }   /* click = edit this profile */
                    if (editing) nk_style_pop_color(ctx);
                    /* right-click (user 2026-08-27): rename / delete */
                    if (nk_contextual_begin(ctx, 0, nk_vec2(300, 110), nb)) {
                        static int rn_for = -1; static char rn_buf[64];
                        int list_changed = 0;
                        if (rn_for != i) { rn_for = i; snprintf(rn_buf, sizeof rn_buf, "%s", prof_list[i]); }
                        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
                        nk_layout_row_push(ctx, 200); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, rn_buf, sizeof rn_buf, nk_filter_default);
                        nk_layout_row_push(ctx, 80);
                        if (nk_button_label(ctx, "Rename") && rn_buf[0] && strcmp(rn_buf, prof_list[i]) && !strpbrk(rn_buf, "/\\ .")) {
                            char cmd[900]; int was_editing = editing;
                            /* profiles/<old>.json -> new (name + own layer entry), mods/<old> -> mods/<new>
                               when the profile owns that layer, build/profiles/<old> dropped (pak cache) */
                            snprintf(cmd, sizeof cmd,
                                "mv \"profiles/%s.json\" \"profiles/%s.json\" && sed -i 's/\"%s\"/\"%s\"/g' \"profiles/%s.json\" && "
                                "{ [ -d \"mods/%s\" ] && mv \"mods/%s\" \"mods/%s\"; true; } && rm -rf \"build/profiles/%s\"",
                                prof_list[i], rn_buf, prof_list[i], rn_buf, rn_buf, prof_list[i], prof_list[i], rn_buf, prof_list[i]);
                            if (system(cmd) == 0) {
                                logf_("profile %s renamed to %s (repack before launching)", prof_list[i], rn_buf);
                                read_mods(); read_profiles(); cur_prof = -1; list_changed = 1;
                                if (was_editing) switch_edit_profile(rn_buf);
                            } else logf_("rename failed (%s)", prof_list[i]);
                            nk_contextual_close(ctx);
                        }
                        nk_layout_row_end(ctx);
                        nk_layout_row_dynamic(ctx, 24, 1);
                        if (nk_contextual_item_label(ctx, "Delete profile", NK_TEXT_LEFT))
                            { cf_kind = 3; cf_id = i; snprintf(cf_name, sizeof cf_name, "%s", prof_list[i]); }
                        nk_contextual_end(ctx);
                        if (list_changed) break;   /* the list changed under us: redraw next frame */
                    } }
                    {   /* game mode + description INLINE, saved on change
                           (user 2026-08-27: the edit dialogs are gone) */
                        static const char *modes[2] = { "tag", "rumble" };
                        static char dbuf[16][160]; static int dloaded[16];
                        int m0 = prof_mode[i], m1;
                        if (!dloaded[i]) { snprintf(dbuf[i], 160, "%s", prof_desc[i]); dloaded[i] = 1; }
                        if (!editing) {   /* only the edited profile's row is live (user 2026-08-27) */
                            nk_label_colored(ctx, modes[m0], NK_TEXT_LEFT, nk_rgb(110, 110, 125));
                            sk_button_gated("Launch", 0, "pick this profile in the header dropdown first");
                            nk_label_colored(ctx, dbuf[i], NK_TEXT_LEFT, nk_rgb(110, 110, 125));
                            continue;
                        }
                        { extern nk_flags nk_combo_label_align;
                          nk_combo_label_align = NK_TEXT_CENTERED;   /* user 2026-08-29: centred mode text */
                          m1 = nk_combo(ctx, modes, 2, m0, 22, nk_vec2(110, 80));
                          nk_combo_label_align = NK_TEXT_LEFT; }
                        if (m1 != m0) { cur_prof = i; load_profile_edit(i); pf_mode = m1; save_profile_edit(); prof_mode[i] = m1; }
                        {
                            struct nk_rect lb = WB();
                            if (nk_button_label(ctx, "Launch")) {
                                wf_editor_kill_tools();
                                unsetenv("WF_PAKDIR"); unsetenv("WF_GFXPAK"); unsetenv("WF_PAK");
                                execl("./wfengine", "wfengine", "--editor", "--play", "--profile", prof_list[i], (char *)NULL);
                                logline("Launch: exec failed");
                            }
                            hint_at(lb, "Play this profile with the game INSIDE the editor (restarts the editor with --play). Unsaved editor changes are lost - Save first.");
                        }
                        {
                            nk_flags fl = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, dbuf[i], 160, nk_filter_default);
                            if ((fl & NK_EDIT_DEACTIVATED) && strcmp(dbuf[i], prof_desc[i]))
                                { cur_prof = i; load_profile_edit(i); snprintf(pf_descbuf, sizeof pf_descbuf, "%s", dbuf[i]); save_profile_edit(); }
                        }
                        continue;
                    }
                    {
                        struct nk_rect lb = WB();
                        if (nk_button_label(ctx, "Launch")) {
                            /* restart THIS editor with the game inside it (--play):
                               PAUSE, step, Match and Calibrate all work on it
                               (user 2026-08-25: Launch = play here). The editing
                               context's WF_PAKDIR must not leak into the new process. */
                            wf_editor_kill_tools();
                            unsetenv("WF_PAKDIR"); unsetenv("WF_GFXPAK"); unsetenv("WF_PAK");
                            execl("./wfengine", "wfengine", "--editor", "--play", "--profile", prof_list[i], (char *)NULL);
                            logline("Launch: exec failed");
                        }
                        hint_at(lb, "Play this profile with the game INSIDE the editor (restarts the editor with --play): PAUSE, frame step, the Match tab and CALIBRATE act on it. Unsaved editor changes are lost - Save first.");
                    }
                    nk_label_colored(ctx, prof_desc[i], NK_TEXT_LEFT, nk_rgb(150, 150, 160));
                }
            }
        }
    }
    /* ---- LAYER popup: the profile's mod-layer stack (user 2026-08-24:
     * its own dialog from the "layer edit" button) ---- */
    if (cur_prof >= 0 && layer_popup) {
        struct nk_rect prL;
        {   /* fill most of the panel (user 2026-08-24: taller + wider) */
            struct nk_rect reg = nk_window_get_content_region(ctx);
            prL = nk_rect(20, 8, reg.w - 60, reg.h - 40);
            if (prL.w < 900) prL.w = 900;
            if (prL.h < 560) prL.h = 560;
        }
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, prof_list[cur_prof],
                           NK_WINDOW_BORDER | NK_WINDOW_TITLE, prL)) {
            {   /* description + game mode (from the retired 'mod edit'
                   popup, user 2026-08-27: its rule grid = the Rules tab) */
                nk_layout_row_begin(ctx, NK_STATIC, 24, 5);
                nk_layout_row_push(ctx, 90); nk_label(ctx, "description", NK_TEXT_RIGHT);
                nk_layout_row_push(ctx, 420); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, pf_descbuf, sizeof pf_descbuf, nk_filter_default);
                nk_layout_row_push(ctx, 90); nk_label(ctx, "game mode", NK_TEXT_RIGHT);
                nk_layout_row_push(ctx, 150); if (nk_option_label(ctx, "tag (tournament)", pf_mode == 0)) pf_mode = 0;
                nk_layout_row_push(ctx, 150); if (nk_option_label(ctx, "rumble (ring-out)", pf_mode == 1)) pf_mode = 1;
                nk_layout_row_end(ctx);
            }
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, "mod layers: tick the layers this profile stacks; LATER entries override earlier ones.", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            nk_layout_row_dynamic(ctx, prL.h - 156.f, 1);
            if (nk_group_begin(ctx, "lp_rows", NK_WINDOW_BORDER)) {
                for (int m = 0; m < n_mods; m++) {
                    int at = -1;
                    for (int k = 0; k < pf_nmods; k++) if (!strcmp(pf_mods[k], mod_list[m])) at = k;
                    {
                        int on = at >= 0, was = on;
                        float mr[5] = { 0.24f, 0.40f, 0.14f, 0.08f, 0.08f };
                        nk_layout_row(ctx, NK_DYNAMIC, 22, 5, mr);
                        nk_checkbox_label(ctx, mod_list[m], &on);
                        nk_label_colored(ctx, mod_desc[m], NK_TEXT_LEFT, nk_rgb(150, 150, 160));
                        if (on && !was && pf_nmods < 16) snprintf(pf_mods[pf_nmods++], 64, "%s", mod_list[m]);
                        if (!on && was) { for (int k = at; k < pf_nmods - 1; k++) snprintf(pf_mods[k], 64, "%s", pf_mods[k + 1]); pf_nmods--; at = -1; }
                        if (at >= 0) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "order %d", at + 1);
                        else nk_label(ctx, "", NK_TEXT_LEFT);
                        if (at > 0 && nk_button_label(ctx, "^")) { char t[64]; snprintf(t, 64, "%s", pf_mods[at - 1]); snprintf(pf_mods[at - 1], 64, "%s", pf_mods[at]); snprintf(pf_mods[at], 64, "%s", t); }
                        else if (at <= 0) nk_label(ctx, "", NK_TEXT_LEFT);
                        if (at >= 0 && at < pf_nmods - 1 && nk_button_label(ctx, "v")) { char t[64]; snprintf(t, 64, "%s", pf_mods[at + 1]); snprintf(pf_mods[at + 1], 64, "%s", pf_mods[at]); snprintf(pf_mods[at], 64, "%s", t); }
                        else nk_label(ctx, "", NK_TEXT_LEFT);
                    }
                }
                nk_layout_row_dynamic(ctx, 24, 2);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, new_mod, sizeof new_mod, nk_filter_default);
                if (nk_button_label(ctx, "Create mod layer") && new_mod[0]) {
                    char d2[256];
                    snprintf(d2, sizeof d2, "mods/%s", new_mod); mkdir_p(d2);
                    read_mods(); new_mod[0] = 0;
                }
                nk_group_end(ctx);
            }
            nk_layout_row_dynamic(ctx, 26, 2);
            if (nk_button_label(ctx, "Save")) {
                char cmd[256];
                save_profile_edit();
                snprintf(cmd, sizeof cmd, "./wfengine --pack-profile %s", prof_list[cur_prof]);
                run_tool(cmd);
                snprintf(save_msg, sizeof save_msg, "saved layers of %s", prof_list[cur_prof]);
                save_msg_t = SDL_GetTicks();
                layer_popup = 0; nk_popup_close(ctx);
            }
            if (nk_button_label(ctx, "Cancel")) { load_profile_edit(cur_prof); layer_popup = 0; nk_popup_close(ctx); }
            nk_popup_end(ctx);
        } else layer_popup = 0;
    }

    /* ---- selected profile: POPUP with description + mod list (user
     * 2026-08-24: not on the same page) ---- */
    if (cur_prof >= 0 && prof_popup) {
        struct nk_rect pr3;
        {   /* fill most of the panel (user 2026-08-24: "plenty of screen
             * real estate") */
            struct nk_rect reg = nk_window_get_content_region(ctx);
            pr3 = nk_rect(20, 8, reg.w - 60, reg.h - 40);
            if (pr3.w < 700) pr3.w = 700;
            if (pr3.h < 520) pr3.h = 520;
        }
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, prof_list[cur_prof],
                           NK_WINDOW_BORDER | NK_WINDOW_TITLE, pr3)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, "Every game rule for this profile. Edit a value (amber = differs from stock); Save writes them into the profile's own mod and repacks. Hover a clipped description for the full text.", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, "description:", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, pf_descbuf, sizeof pf_descbuf, nk_filter_default);
            {   /* ---- ONE game mode per profile (user 2026-08-27): it boots
                 * straight into that flow; the roster is the shared roster/
                 * library minus this profile's hidden slots ---- */
                /* (radio options, NOT a combo: a combo is a popup and Nuklear
                   asserts on a popup inside this popup - crash 2026-08-27) */
                float r[6] = { 0.10f, 0.15f, 0.15f, 0.20f, 0.20f, 0.20f }; int nro = 0;
                nk_layout_row(ctx, NK_DYNAMIC, 26, 6, r);
                nk_label(ctx, "game mode:", NK_TEXT_RIGHT);
                if (nk_option_label(ctx, "tag (tournament)", pf_mode == 0)) pf_mode = 0;
                if (nk_option_label(ctx, "rumble (ring-out)", pf_mode == 1)) pf_mode = 1;
                /* the roster hide list is gone (user 2026-08-27: slots are
                   deleted on the Wrestlers tab); "disabled" in old profile
                   JSON is still honoured by the engine and round-tripped */
                (void)nro;
            }
            /* ---- the SETTINGS PAGE: every game rule, merged for this
             * profile; edits land in the profile's own mod on Save ---- */
            {
                float gh = avail_h() - 92.f;
                if (gh < 120) gh = 120;
                nk_layout_row_dynamic(ctx, gh, 1);
            }
            if (nk_group_begin(ctx, "ps_rules", NK_WINDOW_BORDER)) {
                ps_rows(0, "MATCH MODE");
                ps_rows(1, "GENERAL MODS");
                nk_group_end(ctx);
            }
            if (0) {
            if (ps_adv) {
            nk_layout_row_dynamic(ctx, 170, 1);
            if (nk_group_begin(ctx, "ps_layers", NK_WINDOW_BORDER)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, "mods (later entries override earlier ones):", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
            for (int m = 0; m < n_mods; m++) {
                int at = -1;
                for (int k = 0; k < pf_nmods; k++) if (!strcmp(pf_mods[k], mod_list[m])) at = k;
                {
                    int on = at >= 0, was = on;
                    float mr[5] = { 0.24f, 0.40f, 0.14f, 0.08f, 0.08f };
                    nk_layout_row(ctx, NK_DYNAMIC, 22, 5, mr);
                    nk_checkbox_label(ctx, mod_list[m], &on);
                    nk_label_colored(ctx, mod_desc[m], NK_TEXT_LEFT, nk_rgb(150, 150, 160));
                    if (on && !was && pf_nmods < 16) snprintf(pf_mods[pf_nmods++], 64, "%s", mod_list[m]);
                    if (!on && was) { for (int k = at; k < pf_nmods - 1; k++) snprintf(pf_mods[k], 64, "%s", pf_mods[k + 1]); pf_nmods--; at = -1; }
                    if (at >= 0) nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "order %d", at + 1);
                    else nk_label(ctx, "", NK_TEXT_LEFT);
                    if (at > 0 && nk_button_label(ctx, "^")) { char t[64]; snprintf(t, 64, "%s", pf_mods[at - 1]); snprintf(pf_mods[at - 1], 64, "%s", pf_mods[at]); snprintf(pf_mods[at], 64, "%s", t); }
                    else if (at <= 0) nk_label(ctx, "", NK_TEXT_LEFT);
                    if (at >= 0 && at < pf_nmods - 1 && nk_button_label(ctx, "v")) { char t[64]; snprintf(t, 64, "%s", pf_mods[at + 1]); snprintf(pf_mods[at + 1], 64, "%s", pf_mods[at]); snprintf(pf_mods[at], 64, "%s", t); }
                    else nk_label(ctx, "", NK_TEXT_LEFT);
                }
            }
            nk_layout_row_dynamic(ctx, 24, 2);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, new_mod, sizeof new_mod, nk_filter_default);
            if (nk_button_label(ctx, "Create mod layer") && new_mod[0]) {
                char d2[256];
                snprintf(d2, sizeof d2, "mods/%s", new_mod); mkdir_p(d2);
                read_mods(); new_mod[0] = 0;
            }
            nk_group_end(ctx);
            }                                        /* ps_layers group */
            }                                        /* ps_adv */
            }                                        /* moved to the layer popup */
            nk_layout_row_dynamic(ctx, 26, 3);
            if (nk_button_label(ctx, "Save")) {
                ps_save(); prof_popup = 0; nk_popup_close(ctx);
                snprintf(save_msg, sizeof save_msg, "saved profile %s", prof_list[cur_prof]);
                save_msg_t = SDL_GetTicks();
            }
            if (nk_button_label(ctx, "Cancel")) { load_profile_edit(cur_prof); prof_popup = 0; del_arm = 0; nk_popup_close(ctx); }
            if (!del_arm) {
                if (nk_button_label(ctx, "Delete profile...")) del_arm = 60;
            } else {
                nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 120, 100));
                if (nk_button_label(ctx, "REALLY DELETE?")) {
                    char pth[320], cmd[400];
                    snprintf(pth, sizeof pth, "profiles/%s.json", prof_list[cur_prof]);
                    remove(pth);
                    snprintf(cmd, sizeof cmd, "rm -rf build/profiles/%s", prof_list[cur_prof]);
                    run_tool(cmd);                       /* the pak cache only */
                    logf_("profile %s deleted (its mod layers in mods/ are KEPT)", prof_list[cur_prof]);
                    if (!strcmp(wf_profile(), prof_list[cur_prof])) switch_edit_profile("");
                    read_profiles(); cur_prof = -1; del_arm = 0;
                    prof_popup = 0;
                    nk_style_pop_color(ctx);
                    nk_popup_close(ctx);
                    nk_popup_end(ctx);
                    goto popup_done;
                }
                nk_style_pop_color(ctx);
                if (--del_arm == 0) ;                    /* disarm after ~1s */
            }
            nk_popup_end(ctx);
popup_done:;
        } else prof_popup = 0;
    }

}

/* ------------------------------------------------------- Weapons tab
 * (2026-08-27, replaces the Python builder's Weapons screen): the weapon
 * table as the game reads it - data/weapons/weapons.json through the
 * editing profile's mod layers; spawn slots, per-type palette file and
 * rules row; Save writes the WHOLE table into mods/<layer>/weapons/. */
#define WP_MAXT 32
static json_val *wp_doc; static char wp_src[300]; static int wp_loaded = -1;   /* -1 = never; else the profile generation */
static int wp_ntypes, wp_sel, wp_spawn[ENG_WEAPONS];
static char wp_tname[WP_MAXT][16], wp_palfile[WP_MAXT][64]; static int wp_rules[WP_MAXT][4]; static int wp_npose[WP_MAXT];
static int wp_slot_in[ENG_WEAPONS], wp_nslots;   /* per slot: inside flag (weapons.json "slots") */
static char wp_img[WP_MAXT][64];       /* "image" file of a PNG-ingested type, "" = ROM-cell type */
static uint16_t wp_pens[16];
static SDL_Texture *wp_tex[WP_MAXT][24]; static time_t wp_mt[WP_MAXT][24];
static const char *wp_rule_key[4] = { "carry_dz", "carry_dx", "tumble_steps", "damage" };
static void wp_resolve(const char *file, char *out, size_t n)
{
    char rel[128];
    snprintf(rel, sizeof rel, "weapons/%s", file);
    if (!wf_mod_resolve(rel, out, n)) snprintf(out, n, "data/weapons/%s", file);
}
static void wp_load(void)
{
    char err[128], path[300]; const json_val *types, *spawn, *pal;
    if (wp_doc) { json_free(wp_doc); wp_doc = NULL; }
    wp_loaded = wp_gen; wp_ntypes = 0;
    wp_resolve("weapons.json", wp_src, sizeof wp_src);
    wp_doc = json_parse_file(wp_src, err, sizeof err);
    if (!wp_doc) { logf_("weapons: %s: %s", wp_src, err); return; }
    types = json_get(wp_doc, "types");
    for (const json_val *t = types ? types->child : NULL; t && wp_ntypes < WP_MAXT; t = t->next) {
        int k = wp_ntypes++; const json_val *r = json_get(t, "rules"), *p = json_get(t, "poses");
        snprintf(wp_tname[k], 16, "%s", t->key ? t->key : "?");
        snprintf(wp_palfile[k], 64, "%s", json_str(json_get(t, "palette"), ""));
        snprintf(wp_img[k], 64, "%s", json_str(json_get(t, "image"), ""));
        for (int f = 0; f < 4; f++) wp_rules[k][f] = (int)json_int(json_get(r, wp_rule_key[f]), 0);
        wp_npose[k] = p ? p->n : 0;
    }
    for (int k = 0; k < ENG_WEAPONS; k++) wp_spawn[k] = (k < 2 && k < wp_ntypes) ? k : wp_ntypes;   /* default: ROM pair, rest EMPTY */
    spawn = json_get(wp_doc, "spawn");
    if (spawn) { int k = 0; for (const json_val *s = spawn->child; s && k < ENG_WEAPONS; s = s->next, k++) {
        const char *nm2 = json_str(s, NULL);
        if (nm2 && !nm2[0]) { wp_spawn[k] = wp_ntypes; continue; }   /* "" = empty */
        for (int t = 0; t < wp_ntypes; t++) if (nm2 && !strcmp(nm2, wp_tname[t])) wp_spawn[k] = t; } }
    {   /* slot placements: the in/out flags label the spawn rows */
        const json_val *sl = json_get(wp_doc, "slots"); int k = 0;
        wp_nslots = 0;
        for (const json_val *s = sl && sl->type == JSON_ARRAY ? sl->child : NULL; s && k < ENG_WEAPONS; s = s->next, k++)
            { wp_slot_in[k] = (int)json_int(json_get(s, "in"), 0); wp_nslots = k + 1; }
        if (!wp_nslots) { wp_nslots = 2; wp_slot_in[0] = wp_slot_in[1] = 0; }   /* ROM pair */
    }
    wp_resolve("palette.json", path, sizeof path);
    { json_val *pd = json_parse_file(path, err, sizeof err);
      pal = pd ? json_get(pd, "pens") : NULL;
      for (int i = 0; i < 16; i++) wp_pens[i] = (uint16_t)json_int(json_at(pal, i), 0);
      if (pd) json_free(pd); }
    if (wp_sel >= wp_ntypes) wp_sel = 0;
}
/* small json_val builders (json.h exposes the node) */
static json_val *jv_new(json_type t, const char *key)
{
    json_val *v = calloc(1, sizeof *v); v->type = t; if (key) v->key = strdup(key); return v;
}
static void jv_remove(json_val *obj, const char *key)
{
    json_val **pp = &obj->child;
    while (*pp) { if ((*pp)->key && !strcmp((*pp)->key, key)) { json_val *d = *pp; *pp = d->next; d->next = NULL; json_free(d); obj->n--; return; } pp = &(*pp)->next; }
}
static json_val *jv_append(json_val *obj, json_val *v)
{
    json_val **pp = &obj->child; while (*pp) pp = &(*pp)->next; *pp = v; obj->n++; return v;
}
static void jv_set_string(json_val *obj, const char *key, const char *s)
{
    json_val *v; jv_remove(obj, key); v = jv_append(obj, jv_new(JSON_STRING, key)); v->str = strdup(s);
}
static void wp_save(void)
{
    char path[400], dir[400]; json_val *types, *sp; const char *layer = mod_layer;
    if (!layer[0] && wf_profile()[0] && wf_profile_nmods() > 0) layer = wf_profile_mod(wf_profile_nmods() - 1);   /* the profile's top layer */
    if (!wp_doc || !layer[0]) { logline("weapons: pick a profile (stock is read-only)"); return; }
    types = (json_val *)json_get(wp_doc, "types");
    for (json_val *t = types ? types->child : NULL; t; t = t->next) {
        int k = -1; for (int i = 0; i < wp_ntypes; i++) if (t->key && !strcmp(t->key, wp_tname[i])) k = i;
        if (k < 0) continue;
        if (wp_palfile[k][0]) {
            char pf[400];
            jv_set_string(t, "palette", wp_palfile[k]);
            snprintf(pf, sizeof pf, "mods/%s/weapons/%s", layer, wp_palfile[k]);
            if (access(pf, R_OK)) { char src[400], cmd[900]; wp_resolve("palette.json", src, sizeof src);
                dirname_of(pf, dir, sizeof dir); mkdir_p(dir);
                snprintf(cmd, sizeof cmd, "cp \"%s\" \"%s\"", src, pf); if (system(cmd) == 0) logf_("weapons: created %s as a copy of the shared palette - edit its pens", pf); }
        } else jv_remove(t, "palette");
        jv_remove(t, "rules");
        if (wp_rules[k][0] || wp_rules[k][1] || wp_rules[k][2] || wp_rules[k][3]) {
            json_val *r = jv_append(t, jv_new(JSON_OBJECT, "rules"));
            for (int f = 0; f < 4; f++) if (wp_rules[k][f]) json_set_number(r, wp_rule_key[f], wp_rules[k][f]);
        }
    }
    jv_remove(wp_doc, "spawn");
    sp = jv_append(wp_doc, jv_new(JSON_ARRAY, "spawn"));
    for (int k = 0; k < ENG_WEAPONS; k++) { json_val *s = jv_append(sp, jv_new(JSON_STRING, NULL));
        s->str = strdup(wp_spawn[k] >= wp_ntypes ? "" : wp_tname[wp_spawn[k]]); }
    snprintf(path, sizeof path, "mods/%s/weapons/weapons.json", layer);
    dirname_of(path, dir, sizeof dir); mkdir_p(dir);
    if (json_write_file(path, wp_doc) == 0) {
        char cmd[256];
        logf_("saved %s (packing)", path);
        snprintf(save_msg, sizeof save_msg, "saved weapons"); save_msg_t = SDL_GetTicks();
        snprintf(cmd, sizeof cmd, "%s", pack_cmd()); run_tool_modal(cmd);
        wp_loaded = -1;
    } else logf_("cannot write %s", path);
}
/* ---------------------------------------------------------- arenas
 * The ARENA LIBRARY (user 2026-08-29: "arenas load from their own pak
 * files"): arenas/<name>/{in,out}/ = the two views of one arena as PNG art
 * (crowd_N.png frames + ring.png / floor.png + ropes.png / backcrowd_N.png, arena.json), packed by --pack-arena into
 * build/arenas/<name>.pak; a profile names the arenas it uses per in-ring
 * slot ("arenas": {"0": "wwf", "5": "challenge", "1": "cage"}) and src/
 * arena.c draws them instead of the ROM composer.  Stock names none. */
#define AR_MAX 32
static char ar_names[AR_MAX][64]; static int ar_n = -1, ar_sel, ar_view;
static SDL_Texture *ar_tex[2]; static time_t ar_mt[2];
static char ar_newname[64]; static int ar_newsrc;
static const struct { const char *label; int scene; } ar_slots[] = { { "WWF ring (scene 0)", 0 }, { "Wrestling Challenge (scene 5)", 5 }, { "The cage (scene 1)", 1 }, { "Extra ring (scene 7)", 7 } };
#define AR_SLOTS 4
static int ar_is_stock(const char *name);
static void ar_scan(void)
{
    DIR *d = opendir("arenas"); struct dirent *e;
    ar_n = 0;
    if (!d) return;
    while ((e = readdir(d)) && ar_n < AR_MAX) {
        char p[300]; struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "arenas/%s/ring/arena.json", e->d_name);
        if (stat(p, &st) == 0) snprintf(ar_names[ar_n++], 64, "%s", e->d_name);
    }
    closedir(d);
    {   /* order: wwf, challenge, cage (the stock head), then the rest by name */
        static const char *head[3] = { "wwf", "challenge", "cage" };
        char sorted[AR_MAX][64]; int n = 0;
        for (int h = 0; h < 3; h++) for (int i = 0; i < ar_n; i++) if (!strcmp(ar_names[i], head[h])) memcpy(sorted[n++], ar_names[i], 64);
        for (int i = 0; i < ar_n; i++) if (!ar_is_stock(ar_names[i])) memcpy(sorted[n++], ar_names[i], 64);
        for (int i = 4; i < n; i++) for (int j = i; j > 3 && strcmp(sorted[j - 1], sorted[j]) > 0; j--) { char t[64]; memcpy(t, sorted[j], 64); memcpy(sorted[j], sorted[j - 1], 64); memcpy(sorted[j - 1], t, 64); }
        memcpy(ar_names, sorted, sizeof ar_names);
    }
    if (ar_sel >= ar_n) ar_sel = 0;
}
static int ar_pak_state(const char *name)             /* 0 none, 1 packed, 2 stale */
{
    char p[300]; struct stat sp, ss; time_t src = 0;
    static const char *files[] = { "ring/arena.json", "ring/ring.png", "ring/ropes.png", "ring/crowd_0.png", "ringside/arena.json", "ringside/floor.png", "ringside/crowd_0.png", "ringside/backcrowd_0.png", "arena.json" };
    snprintf(p, sizeof p, "build/arenas/%s.pak", name);
    if (stat(p, &sp) != 0) return 0;
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) { snprintf(p, sizeof p, "arenas/%s/%s", name, files[i]); if (stat(p, &ss) == 0 && ss.st_mtime > src) src = ss.st_mtime; }
    return sp.st_mtime >= src ? 1 : 2;
}
/* the active profile's "arenas" map, read/written in profiles/<p>.json */
static void ar_profile_get(const char *names[AR_SLOTS])
{
    static char buf[AR_SLOTS][64]; char pth[300], err[128]; json_val *doc;
    for (int i = 0; i < AR_SLOTS; i++) { buf[i][0] = 0; names[i] = buf[i]; }
    if (!wf_profile()[0]) return;
    snprintf(pth, sizeof pth, "profiles/%s.json", wf_profile());
    doc = json_parse_file(pth, err, sizeof err);
    if (!doc) return;
    for (int i = 0; i < AR_SLOTS; i++) { char key[8]; snprintf(key, sizeof key, "%d", ar_slots[i].scene); snprintf(buf[i], 64, "%s", json_str(json_get(json_get(doc, "arenas"), key), "")); }
    json_free(doc);
}
static void ar_profile_set(int slot, const char *name)
{
    char pth[300], err[128], key[8]; json_val *doc, *map;
    if (!wf_profile()[0]) return;
    snprintf(pth, sizeof pth, "profiles/%s.json", wf_profile());
    doc = json_parse_file(pth, err, sizeof err);
    if (!doc) { logf_("cannot read %s", pth); return; }
    snprintf(key, sizeof key, "%d", ar_slots[slot].scene);
    map = json_set_object(doc, "arenas");
    if (name && name[0]) json_set_string(map, key, name); else json_remove(map, key);
    if (map->n == 0) json_remove(doc, "arenas");
    if (json_write_file(pth, doc) == 0) logf_("%s: %s -> %s (Pack profile, then Launch)", pth, ar_slots[slot].label, name && name[0] ? name : "stock ROM");
    else logf_("cannot write %s", pth);
    json_free(doc);
}
/* write one table's bytes as the profile save layer's JSON override (the
 * Rules > Tables Save path) and apply them live */
static int write_table_override(const char *name, const uint8_t *bytes, uint32_t len)
{
    int id = tbl_id(name); const tbl_def *d = id >= 0 ? tbl_def_at(id) : NULL;
    char path[1024], dir[1024]; FILE *f;
    if (!d) return 0;
    tbl_set_bytes(id, bytes, len);
    table_path(d, path, sizeof path); dirname_of(path, dir, sizeof dir); mkdir_p(dir);
    f = fopen(path, "w"); if (!f) { logf_("cannot write %s", path); return 0; }
    tbl_write_json(f, d, bytes, len); fclose(f); stock_rescan();
    return 1;
}
static int ar_pending;                     /* a Create is running: rescan + select when it lands */
static char ar_pending_name[64];
/* Rules > Arenas: which library arena each of the profile's ring slots plays in
 * (user 2026-08-29: the assignment lives under Rules, the Arenas tab is the
 * library) */
static int rules_arenas;
static void draw_rules_arenas(void)
{
    const char *cur[AR_SLOTS];
    if (ar_n < 0) ar_scan();
    heading("Arenas", "Which arena each ring slot uses in this profile: stock ROM = the game's own tables, or a library arena from build/arenas/<name>.pak (the Arenas tab makes and packs them). The EXTRA ring (scene 7) is a fourth slot the ROM never used - give it an arena and send stages to it below. Saved straight into profiles/<name>.json + the layer's scene tables; Pack profile + Launch to play.");
    ar_profile_get(cur);
    for (int i = 0; i < AR_SLOTS; i++) {
        const char *items[AR_MAX + 1]; int n = 1, idx = 0, pick;
        items[0] = "stock ROM";
        for (int k = 0; k < ar_n; k++) { items[n] = ar_names[k]; if (!strcmp(ar_names[k], cur[i])) idx = n; n++; }
        nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
        nk_layout_row_push(ctx, 260); nk_label(ctx, ar_slots[i].label, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 240);
        pick = nk_combo(ctx, items, n, idx, 22, nk_vec2(240, 200));
        nk_layout_row_push(ctx, 400);
        nk_label_colored(ctx, idx ? (i == 3 ? "library arena (in-ring view; its ring-out view borrows the base ring's)" : "library arena (in-ring + ring-out views from its pak)")
                              : (i == 3 ? "unused (no stage sends anyone here)" : "the ROM composer"), NK_TEXT_LEFT, nk_rgb(165, 165, 175));
        nk_layout_row_end(ctx);
        if (pick != idx) ar_profile_set(i, pick ? items[pick] : "");
    }
    /* the campaign's ring per stage: match_scene_by_stage (+ the ring-out /
     * return scene tables kept consistent) as a layer table override */
    {
        static const char *rings[4] = { "WWF ring", "Wrestling Challenge", "The cage", "Extra ring" };
        static const int ring_scene[4] = { 0, 5, 1, 7 };
        uint32_t ln = 0; const uint8_t *ms = tbl_bytes(tbl_id("match_scene_by_stage"), &ln);
        int changed = -1, newring = 0;
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, "ring per stage (match_scene_by_stage, saved into the layer with the ring-out / return scenes):", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
        for (int st = 0; st < 10 && ms && st < (int)ln; st++) {
            int cur_r = ms[st] == 5 ? 1 : ms[st] == 1 ? 2 : ms[st] == 7 ? 3 : 0, pick;
            char lab[32]; snprintf(lab, sizeof lab, "stage %d", st + 1);
            nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
            nk_layout_row_push(ctx, 90); nk_label(ctx, lab, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 200); pick = nk_combo(ctx, rings, 4, cur_r, 22, nk_vec2(200, 120));
            nk_layout_row_end(ctx);
            if (pick != cur_r) { changed = st; newring = pick; }
        }
        if (changed >= 0 && ms) {
            uint8_t m[10], ro[10], rt[10]; uint32_t l2 = 0, l3 = 0;
            const uint8_t *r1 = tbl_bytes(tbl_id("ringout_scene_by_stage"), &l2), *r2 = tbl_bytes(tbl_id("return_scene_by_stage"), &l3);
            memcpy(m, ms, 10); if (r1 && l2 >= 10) memcpy(ro, r1, 10); else memset(ro, 0, 10); if (r2 && l3 >= 10) memcpy(rt, r2, 10); else memset(rt, 0, 10);
            m[changed] = (uint8_t)ring_scene[newring];
            /* ring-out view: the ring's own (0 -> 2, 5 -> 6), the cage none; the extra ring
               borrows its base ring's (arena.json stock_scene) */
            { int base = ring_scene[newring];
              if (base == 7) { char pth[300], err[128]; json_val *doc; base = 0;
                  snprintf(pth, sizeof pth, "arenas/%s/arena.json", cur[3]);
                  if (cur[3][0] && (doc = json_parse_file(pth, err, sizeof err))) { base = (int)json_int(json_get(doc, "stock_scene"), 0); json_free(doc); } }
              ro[changed] = (uint8_t)(base == 0 ? 2 : base == 5 ? 6 : 0);
              rt[changed] = (uint8_t)ring_scene[newring]; }
            if (write_table_override("match_scene_by_stage", m, 10) && write_table_override("ringout_scene_by_stage", ro, 10) && write_table_override("return_scene_by_stage", rt, 10))
                logf_("stage %d -> %s (three scene tables written to the layer; Pack profile)", changed + 1, rings[newring]);
        }
    }
    nk_layout_row_static(ctx, 26, 140, 1);
    { struct nk_rect b = WB(); if (nk_button_label(ctx, "Pack profile")) run_tool(pack_cmd()); hint_at(b, "Repack the profile so the game picks up the assignment (packs any stale arena pak too)."); }
}
/* the three stock copies are the library's fixed head: uneditable, always first */
static int ar_is_stock(const char *name) { return !strcmp(name, "wwf") || !strcmp(name, "challenge") || !strcmp(name, "cage"); }
/* a PLAIN TEMPLATE (--arena-plain: arena.json "template": 1) - a base for
 * new arenas, listed under "based on" (user 2026-08-30) */
static int ar_is_template(const char *name)
{
    char p[300], err[128]; json_val *doc; int t;
    snprintf(p, sizeof p, "arenas/%s/arena.json", name);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) return 0;
    t = json_int(json_get(doc, "template"), 0) != 0;
    json_free(doc);
    return t;
}
static int ar_open, ar_new;                            /* right pane: blank / an arena / the new-arena form */
static void arena_delete(const char *name)
{
    char cmd[400];
    if (!name[0] || strpbrk(name, "/\\ .") || ar_is_stock(name)) return;
    snprintf(cmd, sizeof cmd, "rm -rf \"arenas/%s\" \"build/arenas/%s.pak\"", name, name);
    if (system(cmd) == 0) logf_("deleted arena %s", name); else logf_("delete failed: %s", name);
    ar_n = -1; ar_open = 0;
}
static void draw_arenas_left(void)
{
    char cmd[400];
    if (ar_pending && !tool_busy) {           /* the Create landed: refresh + select it */
        char pp[300], err[128]; json_val *doc;
        ar_pending = 0; ar_scan();
        snprintf(pp, sizeof pp, "arenas/%s/arena.json", ar_pending_name);   /* a template copy: its own name, no longer a template */
        if ((doc = json_parse_file(pp, err, sizeof err))) { if (json_get(doc, "template")) { char d[200]; json_remove(doc, "template"); json_set_string(doc, "name", ar_pending_name); snprintf(d, sizeof d, "from the plain template (give it a theme under AI recipe)"); json_set_string(doc, "description", d); json_write_file(pp, doc); } json_free(doc); }
        for (int i = 0; i < ar_n; i++) if (!strcmp(ar_names[i], ar_pending_name)) { ar_sel = i; ar_open = 1; ar_new = 0; ar_view = 0; }
    }
    if (ar_n < 0) ar_scan();
    if (!ar_open && !ar_new && ar_n > 0 && ar_sel >= 0 && ar_sel < ar_n) ar_open = 1;   /* the highlighted arena IS shown (user 2026-08-29) */
    heading("Arenas", NULL);
    nk_layout_row_static(ctx, 24, 120, 1);
    { struct nk_rect b = WB();
      if (nk_button_label(ctx, "+ new arena")) { ar_new = 1; ar_open = 1; ar_sel = -1; ar_newname[0] = 0; }
      hint_at(b, "A new library arena, copied from one of the stock rings: name it, pick the base, Create - then edit its PNGs."); }
    nk_layout_row_dynamic(ctx, 22, 1);
    for (int i = 0; i < ar_n; i++) {
        char lab[96]; int st = ar_pak_state(ar_names[i]), sel = ar_sel == i;
        struct nk_rect rb = WB();
        snprintf(lab, sizeof lab, "%s   %s%s", ar_names[i], ar_is_stock(ar_names[i]) ? "(stock)" : ar_is_template(ar_names[i]) ? "(template)" : "", st == 1 ? "" : st == 2 ? " (pak stale)" : " (not packed)");
        if (nk_selectable_label(ctx, lab, NK_TEXT_LEFT, &sel) && sel) { ar_sel = i; ar_open = 1; ar_new = 0; }
        if (!ar_is_stock(ar_names[i]) && nk_contextual_begin(ctx, 0, nk_vec2(300, 90), rb)) {
            static int rn_for = -1; static char rn_buf[40];
            if (rn_for != i) { rn_for = i; snprintf(rn_buf, sizeof rn_buf, "%s", ar_names[i]); }
            nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
            nk_layout_row_push(ctx, 200); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, rn_buf, sizeof rn_buf, nk_filter_default);
            nk_layout_row_push(ctx, 80);
            if (nk_button_label(ctx, "Rename") && rn_buf[0] && strcmp(rn_buf, ar_names[i]) && !strpbrk(rn_buf, "/\\ .") && !tool_busy) {
                snprintf(cmd, sizeof cmd, "mv \"arenas/%s\" \"arenas/%s\" && { [ ! -f \"build/arenas/%s.pak\" ] || mv \"build/arenas/%s.pak\" \"build/arenas/%s.pak\"; }", ar_names[i], rn_buf, ar_names[i], ar_names[i], rn_buf);
                if (system(cmd) == 0) { logf_("renamed arena %s -> %s (profiles naming the old name need re-assigning)", ar_names[i], rn_buf); ar_n = -1; }
                else logf_("rename failed (%s)", ar_names[i]);
                nk_contextual_close(ctx);
            }
            nk_layout_row_end(ctx);
            nk_layout_row_dynamic(ctx, 24, 1);
            if (tool_busy) nk_label_colored(ctx, "a tool is running", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            else if (nk_contextual_item_label(ctx, "Delete arena", NK_TEXT_LEFT))
                { cf_kind = 4; cf_id = i; snprintf(cf_name, sizeof cf_name, "%s", ar_names[i]); }
            nk_contextual_end(ctx);
        }
    }
    if (ar_n == 0) nk_label_colored(ctx, "no arenas yet", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
    note("right-click a name to rename or delete (the stock three stay)");
    note("side ropes are sprites, not arena art");
}
/* a FRAME TILE the skins way: grey box, the image fitted inside (never
 * squashed - scaled down to fit, up to 4x), the caption underneath;
 * returns 1 on a click */
static int frame_tile(SDL_Texture *tex, const char *cap)
{
    struct nk_rect b; enum nk_widget_layout_states st = nk_widget(&b, ctx);
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    float capH = cap && cap[0] ? 15 : 0, iw = b.w - 6, ih = b.h - 6 - capH;
    if (!st) return 0;
    nk_fill_rect(cv, b, 3, nk_rgb(52, 52, 56));
    if (tex) {
        int w = 1, h = 1; float sc, dw, dh; struct nk_image im = nk_image_ptr(tex); struct nk_rect r;
        SDL_QueryTexture(tex, NULL, NULL, &w, &h);
        sc = iw / (float)w; if ((float)h * sc > ih) sc = ih / (float)h; if (sc > 4) sc = 4;
        dw = (float)w * sc; dh = (float)h * sc;
        r = nk_rect(b.x + 3 + (iw - dw) / 2, b.y + 3 + (ih - dh) / 2, dw, dh);
        nk_draw_image(cv, r, &im, nk_rgb(255, 255, 255));
    } else nk_draw_text(cv, nk_rect(b.x + 3, b.y + 3, iw, ih), "?", 1, ctx->style.font, nk_rgba(0, 0, 0, 0), nk_rgb(120, 120, 130));
    if (capH > 0) nk_draw_text(cv, nk_rect(b.x + 4, b.y + b.h - capH - 1, b.w - 8, capH), cap, (int)strlen(cap), ctx->style.font, nk_rgba(0, 0, 0, 0), nk_rgb(170, 170, 180));
    return st != NK_WIDGET_INVALID && nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, b);   /* NK_WIDGET_ROM = a tile
                                          partly scrolled out of the panel: nuklear gives it no input, but the
                                          click is real - the big arena tiles near the panel edge "did nothing"
                                          (user 2026-08-30) */
}
/* a row of frame tiles shaped like the PICTURE (user 2026-08-30: arena
 * pictures must keep their aspect in tiles and modal): the cell width is
 * picked from the aspect (wide strips such as the 960x256 ringside get half
 * the pane each, tall or square ones the 236-px box), the cell height is the
 * width divided by the aspect + the caption; n = tiles in the row */
static void tile_row(SDL_Texture *tex, int n)
{
    int tw = 0, th = 0; float cw = 236, ch = 150, asp;
    if (tex) SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    if (tw > 0 && th > 0) {
        asp = (float)tw / (float)th;
        if (asp >= 2.5f) cw = 476;
        else if (asp >= 1.1f) cw = 320;             /* the 640x512 ring planes: four to a row */
        ch = (cw - 6) / asp;
        if (ch > 300) { ch = 300; cw = ch * asp + 6; }
        if (ch < 40) ch = 40;
    }
    { float pw = nk_window_get_content_region(ctx).w - 24;
      int cols = (int)(pw / (cw + 8)); if (cols < 1) cols = 1; if (n < cols) cols = n; if (cols < 1) cols = 1;
      nk_layout_row_static(ctx, ch + 6 + 21, (int)cw, cols); }
}
/* loads the texture of one picture into the slot (the shared plain-PNG cache) */
static SDL_Texture *tile_tex(const char *path, SDL_Texture **slot, time_t *mt, char *keep)
{
    if (strcmp(keep, path)) { snprintf(keep, 300, "%s", path); if (*slot) { SDL_DestroyTexture(*slot); *slot = NULL; } *mt = 0; }
    sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1; sk_png_canvas = 2;   /* the WHOLE picture, no bbox crop:
                                          a crowd frame's place on the plane and the rope frames' shared canvas are the point */
    sk_png_tex(path, slot, mt); sk_png_plain = 0; sk_png_canvas = 0;
    return *slot;
}
/* the file name of a frame, aliases resolved ("same_as" -> the source step) */
static const json_val *frame_src(const json_val *frames, const json_val *fr)
{
    for (int g = 0; g < 16 && fr && json_get(fr, "same_as"); g++) fr = json_at(frames, (int)json_int(json_get(fr, "same_as"), 0));
    return fr;
}

/* one tilemap VIEW directory (the layers format): the two
 * planes at 3/4 and one tile row per crowd animation, tiles open the frame
 * modal - shared by the arena views and the scenes' tilemap tab */
/* one arena VIEW directory in the layers format (arena.json "layers":
 * under = crowd frames + the ring/floor overlay, over = the plane drawn
 * over the sprites): frames as tiles (the modal steps through them), the
 * overlay and the over plane as pictures (click = modal).  tbase: texture
 * slot range so two views can share a frame. */
static void draw_view_layers(const char *vd, int tbase, int mode)
{
    char p[500], err[128]; json_val *doc;
    static SDL_Texture *at[512]; static time_t amt[512]; static char apath[512][300]; int ti = tbase;
    snprintf(p, sizeof p, "%s/arena.json", vd);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) { note("no arena.json in this view"); return; }
    { const json_val *layers = json_get(doc, "layers");
      if (!layers) { nk_layout_row_dynamic(ctx, 18, 1); nk_label_colored(ctx, "old bg/fg format - re-export this view (the one format is layers: crowd frames + ring + ropes)", NK_TEXT_LEFT, nk_rgb(240, 120, 120)); json_free(doc); return; }
      for (int pl = 0; pl < 2; pl++) {
          const json_val *L = json_get(layers, pl ? "over" : "under"), *frames = L ? json_get(L, "frames") : NULL, *steps = L ? json_get(L, "steps") : NULL;
          const char *ov = L ? json_str(json_get(L, "overlay"), NULL) : NULL;
          const json_val *ovs = L ? json_get(L, "overlays") : NULL;
          const char *ov2 = ovs && ovs->child && ovs->child->next ? json_str(ovs->child->next, NULL) : NULL;   /* the rear ropes */
          int nf = 0, ns = 0, k = 0; char lab[200], seq[160] = "";
          int want_frames = pl ? (mode & 4) : (mode & 1), want_ov = pl == 0 && (mode & 2), want_ov2 = pl == 0 && (mode & 4) && ov2;
          if (!L || (!want_frames && !want_ov && !want_ov2)) continue;
          if (!want_frames) nf = -1;
          if (nf >= 0) for (const json_val *q = frames ? frames->child : NULL; q; q = q->next) nf++;
          for (const json_val *q = steps ? steps->child : NULL; q; q = q->next) { char t[16]; snprintf(t, sizeof t, "%s%lld", ns ? "," : "", (long long)json_int(json_get(q, "frame"), 0)); if (strlen(seq) + strlen(t) < sizeof seq - 1) strcat(seq, t); ns++; }
          if (nf >= 0) {
          nk_layout_row_dynamic(ctx, 22, 1);
          if (pl == 0) snprintf(lab, sizeof lab, "CROWD (under the wrestlers): %d frame%s%s%s - the ring is NOT in these, it is the overlay", nf, nf == 1 ? "" : "s", ns ? "  -  steps " : "", ns ? seq : "");
          else snprintf(lab, sizeof lab, "OVER the wrestlers: %d picture%s%s%s (%s)", nf, nf == 1 ? "" : "s", ns ? "  -  steps " : "", ns ? seq : "", strstr(vd, "ringside") ? "the far crowd" : "the top and bottom rope lines - they never move");
          nk_label_colored(ctx, lab, NK_TEXT_LEFT, nk_rgb(240, 180, 60));
          }
          if (nf > 0) {
              snprintf(p, sizeof p, "%s/%s", vd, json_str(frames->child, ""));
              tile_row(tile_tex(p, &at[ti], &amt[ti], apath[ti]), nf);
              for (const json_val *q = frames->child; q && ti < tbase + 200 && k < 16; q = q->next, k++, ti++) {
                  snprintf(p, sizeof p, "%s/%s", vd, json_str(q, ""));
                  tile_tex(p, &at[ti], &amt[ti], apath[ti]);
                  if (frame_tile(at[ti], json_str(q, "")) && !fr_on) {
                      int m = 0;
                      for (const json_val *z = frames->child; z && m < FR_MAX; z = z->next, m++) snprintf(fr_paths[m], sizeof fr_paths[m], "%s/%s", vd, json_str(z, ""));
                      fr_open(pl ? "over plane" : "crowd frames", m, k);
                  }
              }
          }
          if (ov && want_ov) {
              const json_val *r = json_get(L, "overlay_rect");
              nk_layout_row_dynamic(ctx, 18, 1);
              nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "%s  -  the whole static %s (every cell the crowd never touches, bbox %lld,%lld %lldx%lld cells); composited over the crowd at pack time", ov, strstr(vd, "ringside") ? "floor, rails and ring" : "ring, posts, apron and floor",
                                (long long)json_int(json_at(r, 0), 0), (long long)json_int(json_at(r, 1), 0), (long long)json_int(json_at(r, 2), 0), (long long)json_int(json_at(r, 3), 0));
              snprintf(p, sizeof p, "%s/%s", vd, ov);
              tile_row(tile_tex(p, &at[ti], &amt[ti], apath[ti]), 1);
              if (frame_tile(at[ti], ov) && !fr_on) { snprintf(fr_paths[0], sizeof fr_paths[0], "%s", p); fr_open("overlay", 1, 0); }
              ti++;
          }
          if (want_ov2) {                                   /* the REAR ropes: under the wrestlers, composited after the ring */
              nk_layout_row_dynamic(ctx, 18, 1);
              nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "%s  -  the REAR rope strands (drawn under the wrestlers, over the ring); static, their own layer", ov2);
              snprintf(p, sizeof p, "%s/%s", vd, ov2);
              tile_row(tile_tex(p, &at[ti], &amt[ti], apath[ti]), 1);
              if (frame_tile(at[ti], ov2) && !fr_on) { snprintf(fr_paths[0], sizeof fr_paths[0], "%s", p); fr_open("rear ropes", 1, 0); }
              ti++;
          }
      } }
    json_free(doc);
}
static void draw_view_dir(const char *vd) { draw_view_layers(vd, 0, 7); }

/* the packer's composite previews (build/arena-preview/<arena>/<view>_<step>.png) */
static void draw_previews(const char *arena, const char *view, int tbase)
{
    static SDL_Texture *pt[64]; static time_t pmt[64]; static char ppath[64][300];
    char p[400]; int n = 0;
    for (int k = 0; k < 16; k++) { snprintf(p, sizeof p, "build/arena-preview/%s/%s_%d.png", arena, view, k); if (access(p, R_OK) != 0) break; n++; }
    nk_layout_row_dynamic(ctx, 22, 1);
    if (!n) { nk_label_colored(ctx, "no composite preview yet - Save + Pack arena writes one per crowd step", NK_TEXT_LEFT, nk_rgb(165, 165, 175)); return; }
    nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "AS THE GAME DRAWS IT: crowd step + %s + over plane, %d step%s (click a tile to step through)", strcmp(view, "ring") ? "floor" : "ring", n, n == 1 ? "" : "s");
    snprintf(p, sizeof p, "build/arena-preview/%s/%s_0.png", arena, view);
    tile_row(tile_tex(p, &pt[tbase], &pmt[tbase], ppath[tbase]), n);
    for (int k = 0; k < n && k < 16; k++) {
        int ti = tbase + k; char lab[32];
        snprintf(p, sizeof p, "build/arena-preview/%s/%s_%d.png", arena, view, k);
        tile_tex(p, &pt[ti], &pmt[ti], ppath[ti]);
        snprintf(lab, sizeof lab, "step %d", k);
        if (frame_tile(pt[ti], lab) && !fr_on) {
            for (int m = 0; m < n && m < FR_MAX; m++) snprintf(fr_paths[m], sizeof fr_paths[m], "build/arena-preview/%s/%s_%d.png", arena, view, m);
            fr_open("composite preview", n < FR_MAX ? n : FR_MAX, k);
        }
    }
}

/* the side-rope sprite frames (arenas/<name>/ring/ropes/ropes.json) */
static void draw_rope_frames(const char *ringdir, int tbase)
{
    static SDL_Texture *rt[16]; static time_t rmt[16]; static char rpath[16][300];
    char p[500], err[128]; json_val *doc; const json_val *fr; int k = 0, n = 0;
    static const char *what[11] = { "0", "1", "back idle", "3 (back shake)", "4 (back shake)", "5 (back shake)", "6 (back shake)", "7 (front shake)", "8 (front shake)", "9 (front shake)", "front idle" };
    snprintf(p, sizeof p, "%s/ropes/ropes.json", ringdir);
    doc = json_parse_file(p, err, sizeof err);
    nk_layout_row_dynamic(ctx, 22, 1);
    if (!doc) { nk_label_colored(ctx, "no side-rope art in this arena: the stock ROM sprites draw (re-export a stock ring with --export-arena to get ropes/)", NK_TEXT_LEFT, nk_rgb(165, 165, 175)); return; }
    fr = json_get(doc, "frames");
    for (const json_val *q = fr ? fr->child : NULL; q; q = q->next) n++;
    nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "SIDE ROPES (sprites, %d frames on one %lldx%lld canvas, origin %lld,%lld): the only ropes that move - the ROM's shake tables pick the frames (light / heavy / lean); the right side is the same art mirrored", n,
                      (long long)json_int(json_at(json_get(doc, "canvas"), 0), 0), (long long)json_int(json_at(json_get(doc, "canvas"), 1), 0), (long long)json_int(json_at(json_get(doc, "origin"), 0), 0), (long long)json_int(json_at(json_get(doc, "origin"), 1), 0));
    if (fr && fr->child) { snprintf(p, sizeof p, "%s/ropes/%s", ringdir, json_str(fr->child, "")); tile_row(tile_tex(p, &rt[0], &rmt[0], rpath[0]), n); }
    else tile_row(NULL, 1);
    (void)tbase;
    for (const json_val *q = fr ? fr->child : NULL; q && k < 16; q = q->next, k++) {
        char lab[64];
        snprintf(p, sizeof p, "%s/ropes/%s", ringdir, json_str(q, ""));
        tile_tex(p, &rt[k], &rmt[k], rpath[k]);
        snprintf(lab, sizeof lab, "%s - %s", json_str(q, ""), k < 11 ? what[k] : "");
        if (frame_tile(rt[k], lab) && !fr_on) {
            int m = 0;
            for (const json_val *z = fr->child; z && m < FR_MAX; z = z->next, m++) snprintf(fr_paths[m], sizeof fr_paths[m], "%s/ropes/%s", ringdir, json_str(z, ""));
            fr_open("side ropes", m, k);
        }
    }
    json_free(doc);
}

/* ARENAS > AI RECIPE (user 2026-08-30): the theme + prompt template for
 * `--art-arena NAME` (tools/art_run.c: every picture of both views repainted
 * by codex, style-anchored on the first result), the run buttons on the
 * skins pattern (progress in the status bar via sk_run_status), the newest
 * result as a live tile, the results grid (modal with the original /
 * generated toggle), Apply (results over the views, originals kept in
 * _orig/) and Restore. */
static char rc_theme[2048], rc_prompt[4096], rc_loaded[64]; static int rc_dirty;
static SDL_Texture *rc_last_tex; static time_t rc_last_mt; static char rc_last_path[300];
static SDL_Texture *rc_tex[48]; static time_t rc_mt[48]; static char rc_path[48][300];
static const char *ar_sel_name(void) { return ar_n > 0 && ar_sel >= 0 && ar_sel < ar_n ? ar_names[ar_sel] : ""; }
static void rc_load(const char *name)
{
    char p[300], err[128]; json_val *doc;
    snprintf(rc_loaded, sizeof rc_loaded, "%s", name);
    rc_theme[0] = rc_prompt[0] = 0; rc_dirty = 0;
    snprintf(p, sizeof p, "arenas/%s/recipe.json", name);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) return;
    snprintf(rc_theme, sizeof rc_theme, "%s", json_str(json_get(doc, "theme"), ""));
    snprintf(rc_prompt, sizeof rc_prompt, "%s", json_str(json_get(doc, "prompt"), ""));
    json_free(doc);
}
static int rc_save(const char *name)
{
    char p[300], err[128]; json_val *doc; int rc;
    snprintf(p, sizeof p, "arenas/%s/recipe.json", name);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) doc = json_parse("{}", 2, err, sizeof err);
    if (!doc) return 0;
    json_set_string(doc, "theme", rc_theme);
    json_set_string(doc, "prompt", rc_prompt);
    rc = json_write_file(p, doc) == 0;
    json_free(doc);
    if (rc) rc_dirty = 0; else logf_("cannot write %s", p);
    return rc;
}
static int rc_run_alive(const char *name)      /* an --art-arena from a previous editor still writing */
{
    char p[300]; struct stat st;
    snprintf(p, sizeof p, "arenas/%s/_gen/run_state.json", name);
    return stat(p, &st) == 0 && time(NULL) - st.st_mtime < 15 && !tool_busy && access("jobs/.codex.lock", R_OK) == 0;
}
static int rc_gen_count(const char *name, char list[48][300])
{
    static const char *views[2] = { "ring", "ringside" }; int n = 0;
    for (int v = 0; v < 2; v++) {
        char d[300]; DIR *dd; struct dirent *e; int first = n;
        snprintf(d, sizeof d, "arenas/%s/_gen/%s", name, views[v]);
        dd = opendir(d);
        if (!dd) continue;
        while ((e = readdir(dd)) && n < 48) { size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".png") && e->d_name[0] != '_') snprintf(list[n++], 300, "%s/%s", d, e->d_name); }
        closedir(dd);
        for (int i = first + 1; i < n; i++) for (int j = i; j > first && strcmp(list[j - 1], list[j]) > 0; j--) { char t[300]; memcpy(t, list[j], 300); memcpy(list[j], list[j - 1], 300); memcpy(list[j - 1], t, 300); }
    }
    return n;
}
static void draw_arena_recipe(const char *name, int stock)
{
    char cmd[1200], p[300]; int busy = tool_busy || rc_run_alive(name), ngen; char gl[48][300];
    if (strcmp(rc_loaded, name)) rc_load(name);
    if (stock) { note("the stock arenas are never repainted - make a copy with + new arena, then give the copy a theme here"); return; }
    ngen = rc_gen_count(name, gl);
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label_colored(ctx, "THEME - what the whole arena should look like (codex repaints every crowd frame, the ring, the floor and the far crowd in this look; the geometry never moves)", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
    nk_layout_row_dynamic(ctx, 70, 1);
    if (nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_MULTILINE, rc_theme, sizeof rc_theme, nk_filter_default) & NK_EDIT_COMMITED) rc_dirty = 1;
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label_colored(ctx, "PROMPT TEMPLATE - empty = the built-in one (printf order: width, height, what the picture is, the theme, width, height); the style-anchor sentence is appended by the tool", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
    nk_layout_row_dynamic(ctx, 90, 1);
    if (nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_MULTILINE, rc_prompt, sizeof rc_prompt, nk_filter_default) & NK_EDIT_COMMITED) rc_dirty = 1;
    nk_layout_row_dynamic(ctx, 34, 1);
    nk_label_colored_wrap(ctx, "reference frames sent per picture: image 1 = the picture as it is now (transparent = magenta), image 2 = the first result of the run (crowd steps 1+ anchor on the theme's step 0); results are scaled back to size and masked with the original's transparency; rope art stays stock", nk_rgb(150, 150, 160));
    nk_layout_row_begin(ctx, NK_STATIC, 26, 6);
    nk_layout_row_push(ctx, 110);
    { struct nk_rect b = WB(); if (nk_button_label(ctx, "Save recipe")) { if (rc_save(name)) logf_("arenas/%s/recipe.json saved", name); } hint_at(b, "Writes theme + prompt template to arenas/<name>/recipe.json (Generate saves too)."); }
    nk_layout_row_push(ctx, 120);
    if (sk_button_gated("Generate arena", !busy && rc_theme[0], busy ? "a request is still running" : "write a theme first")) {
        rc_save(name);
        snprintf(cmd, sizeof cmd, "./wfengine --art-arena \"%s\"", name);
        run_tool(cmd);
        logf_("arena %s: generating (resumes: pictures already in _gen/ are kept - Discard to start over)", name);
    }
    nk_layout_row_push(ctx, 115);
    if (sk_button_gated("Cancel request", busy, "nothing is running")) {
        wf_editor_kill_tools();
        { FILE *lf = fopen("jobs/.codex.lock", "r"); long opid = 0; if (lf) { if (fscanf(lf, "%ld", &opid) != 1) opid = 0; fclose(lf); }
          if (opid > 0 && !tool_busy) { kill(-(pid_t)opid, SIGKILL); kill((pid_t)opid, SIGKILL); unlink("jobs/.codex.lock"); } }
        logf_("cancelled (Generate resumes: finished pictures are kept)");
    }
    nk_layout_row_push(ctx, 190);
    if (sk_button_gated("Apply generated -> views", ngen > 0 && !busy, busy ? "a request is still running" : "nothing generated yet")) {
        snprintf(cmd, sizeof cmd, "for v in ring ringside; do for f in \"arenas/%s/_gen/$v\"/*.png; do [ -f \"$f\" ] || continue; b=$(basename \"$f\"); mkdir -p \"arenas/%s/_orig/$v\"; [ -f \"arenas/%s/_orig/$v/$b\" ] || cp \"arenas/%s/$v/$b\" \"arenas/%s/_orig/$v/$b\"; cp \"$f\" \"arenas/%s/$v/$b\"; done; done", name, name, name, name, name, name);
        if (system(cmd) == 0) logf_("arena %s: %d generated picture%s copied over the views (originals in _orig/) - Save + Pack arena to play it", name, ngen, ngen == 1 ? "" : "s"); else logf_("apply failed");
        ar_mt[0] = ar_mt[1] = 0;
    }
    nk_layout_row_push(ctx, 130);
    { char od[300]; snprintf(od, sizeof od, "arenas/%s/_orig", name);
      if (sk_button_gated("Restore originals", access(od, R_OK) == 0 && !busy, "nothing applied yet")) {
          snprintf(cmd, sizeof cmd, "for v in ring ringside; do for f in \"arenas/%s/_orig/$v\"/*.png; do [ -f \"$f\" ] || continue; cp \"$f\" \"arenas/%s/$v/$(basename \"$f\")\"; done; done", name, name);
          if (system(cmd) == 0) logf_("arena %s: originals back in the views (the generated pictures stay in _gen/) - Save + Pack arena", name); else logf_("restore failed");
      } }
    nk_layout_row_push(ctx, 130);
    if (sk_button_gated("Discard generated", ngen > 0 && !busy, "nothing generated")) {
        snprintf(cmd, sizeof cmd, "rm -rf \"arenas/%s/_gen\"", name);
        if (system(cmd) == 0) logf_("arena %s: _gen/ removed", name);
    }
    nk_layout_row_end(ctx);
    if (rc_dirty) { nk_layout_row_dynamic(ctx, 16, 1); nk_label_colored(ctx, "unsaved recipe edits", NK_TEXT_LEFT, nk_rgb(240, 180, 60)); }
    /* the newest result, live */
    snprintf(p, sizeof p, "arenas/%s/_gen/last.txt", name);
    { FILE *lf = fopen(p, "r"); char lp[300] = "";
      if (lf) { if (fgets(lp, sizeof lp, lf)) lp[strcspn(lp, "\n")] = 0; fclose(lf); }
      nk_layout_row_dynamic(ctx, 20, 1);
      if (lp[0]) {
          nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "LAST GENERATED: %s%s", lp, busy ? "   (watch it - Cancel if the theme goes the wrong way)" : "");
          tile_row(tile_tex(lp, &rc_last_tex, &rc_last_mt, rc_last_path), 1);
          if (frame_tile(rc_last_tex, strrchr(lp, '/') ? strrchr(lp, '/') + 1 : lp) && !fr_on) { snprintf(fr_paths[0], sizeof fr_paths[0], "%s", lp); fr_open("last generated", 1, 0); }
      } else nk_label_colored(ctx, busy ? "generating - the first picture is on its way" : "no results yet", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
    }
    if (ngen > 0) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "GENERATED SO FAR: %d picture%s (click one: the modal toggles original / generated)", ngen, ngen == 1 ? "" : "s");
        tile_row(tile_tex(gl[0], &rc_tex[0], &rc_mt[0], rc_path[0]), ngen);
        for (int i = 0; i < ngen; i++) {
            const char *cap = strstr(gl[i], "/_gen/") ? strstr(gl[i], "/_gen/") + 6 : gl[i];
            tile_tex(gl[i], &rc_tex[i], &rc_mt[i], rc_path[i]);
            if (frame_tile(rc_tex[i], cap) && !fr_on) {
                for (int m = 0; m < ngen && m < FR_MAX; m++) snprintf(fr_paths[m], sizeof fr_paths[m], "%s", gl[m]);
                fr_open("generated", ngen, i);
            }
        }
    }
}

/* the view's ROLE MAP (--arena-plain: roles.png - grey geometry, red crowd
 * cells, green stands, blue mat, magenta skirt, cyan floor) */
static void draw_roles(const char *vd, int slot)
{
    static SDL_Texture *rt[2]; static time_t rmt[2]; static char rp[2][300];
    char p[400]; int k = slot & 1;
    snprintf(p, sizeof p, "%s/roles.png", vd);
    if (access(p, R_OK) != 0) return;
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, "ROLES (roles.png - what each pixel is for: grey GEOMETRY never repainted, red CROWD animated cells, green STANDS, blue MAT, magenta SKIRT, cyan FLOOR, dark cyan floor lines, orange POSTS; hand-fix in a pixel editor, then --arena-plain again)", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
    tile_row(tile_tex(p, &rt[k], &rmt[k], rp[k]), 1);
    if (frame_tile(rt[k], "roles.png") && !fr_on) { snprintf(fr_paths[0], sizeof fr_paths[0], "%s", p); fr_open("roles", 1, 0); }
}

static void draw_arenas(void)
{
    char dir[300], p[400], err[128], cmd[400];
    static const char *srcs[3] = { "WWF ring", "Wrestling Challenge ring", "The cage" };
    if (ar_n < 0) ar_scan();
    if (!ar_open) { note("pick an arena on the left, or + new arena"); return; }
    if (ar_new) {                                       /* the create form (the skins tab's shape) */
        const char *items[AR_MAX + 3]; int n = 3;
        for (int k = 0; k < 3; k++) items[k] = srcs[k];
        for (int k = 0; k < ar_n; k++) if (!ar_is_stock(ar_names[k]) && ar_is_template(ar_names[k])) items[n++] = ar_names[k];   /* the plain templates */
        heading("New arena", "Copies one of the stock rings, or a PLAIN TEMPLATE (flat surfaces + geometry, made by --arena-plain), into arenas/<name>/ and packs it. Edit the pictures, or give it a theme under AI recipe; Save + Pack arena, then assign it to a slot under Rules > Arenas.");
        nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
        nk_layout_row_push(ctx, 120); nk_label(ctx, "name", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 240); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ar_newname, sizeof ar_newname, nk_filter_default);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
        nk_layout_row_push(ctx, 120); nk_label(ctx, "based on", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 240); if (ar_newsrc >= n) ar_newsrc = 0; ar_newsrc = nk_combo(ctx, items, n, ar_newsrc, 22, nk_vec2(240, 160));
        nk_layout_row_end(ctx);
        nk_layout_row_static(ctx, 26, 120, 1);
        { struct nk_rect b = WB();
          if (nk_button_label(ctx, "Create")) {
              if (!ar_newname[0] || strpbrk(ar_newname, "/\\ .")) logline("give the arena a plain name (no spaces, dots or slashes)");
              else if (tool_busy) logline("a tool is still running");
              else { if (ar_newsrc < 3) snprintf(cmd, sizeof cmd, "./wfengine --new-arena \"%s\" %d && ./wfengine --pack-arena \"%s\"", ar_newname, ar_slots[ar_newsrc].scene, ar_newname);
                     else snprintf(cmd, sizeof cmd, "cp -r \"arenas/%s\" \"arenas/%s\" && rm -rf \"arenas/%s/_gen\" \"arenas/%s/_orig\" \"arenas/%s/recipe.json\" && ./wfengine --pack-arena \"%s\"", items[ar_newsrc], ar_newname, ar_newname, ar_newname, ar_newname, ar_newname);
                     run_tool(cmd); ar_pending = 1; snprintf(ar_pending_name, sizeof ar_pending_name, "%s", ar_newname); ar_new = 0; ar_open = 0; }
          }
          hint_at(b, "Runs --new-arena (two headless exports) and --pack-arena; the arena then appears in the list."); }
        return;
    }
    if (ar_sel < 0 || ar_sel >= ar_n) { ar_open = 0; return; }
    /* the four tabs (user 2026-08-30): In-ring / Ringside / Crowd / Ropes - title +
     * description BELOW the tabs, the button row under that */
    { int stock = ar_is_stock(ar_names[ar_sel]);
    static const char *tabs[5] = { "In-ring", "Ringside", "Crowd", "Ropes", "AI recipe" };
    nk_layout_row_begin(ctx, NK_STATIC, 26, 5);
    for (int t = 0; t < 5; t++) { nk_layout_row_push(ctx, ed_tab_w(tabs[t])); if (ed_tab(tabs[t], ar_view == t)) ar_view = t; }
    nk_layout_row_end(ctx);
    if (ar_view < 0 || ar_view > 4) ar_view = 0;
    { json_val *doc; char sub[300];
      snprintf(dir, sizeof dir, "arenas/%s", ar_names[ar_sel]);
      snprintf(p, sizeof p, "%s/arena.json", dir); doc = json_parse_file(p, err, sizeof err);
      if (doc) { snprintf(sub, sizeof sub, "%s/  -  %s (stock scene %lld)  -  %s", dir, json_str(json_get(doc, "description"), ""), (long long)json_int(json_get(doc, "stock_scene"), -1),
                          ar_pak_state(ar_names[ar_sel]) == 1 ? "packed" : ar_pak_state(ar_names[ar_sel]) == 2 ? "PAK STALE - Pack arena" : "NOT PACKED - Pack arena"); json_free(doc); }
      else snprintf(sub, sizeof sub, "%s/", dir);
      heading(ar_names[ar_sel], sub); }
    nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
    nk_layout_row_push(ctx, 150);
    { struct nk_rect b = WB(); const char *sub = ar_view == 1 ? "/ringside" : ar_view == 3 ? "/ring/ropes" : ar_view == 4 ? "" : "/ring";
      if (nk_button_label(ctx, "Browse arena files")) { snprintf(cmd, sizeof cmd, "xdg-open \"arenas/%s%s\" >/dev/null 2>&1 &", ar_names[ar_sel], sub); if (system(cmd)) {} }
      hint_at(b, ar_view == 0 ? "ring/: ring.png (the whole static ring), crowd_N.png, ropes.png, arena.json" : ar_view == 1 ? "ringside/: floor.png, crowd_N.png, backcrowd_N.png, arena.json" : ar_view == 2 ? "the crowd frames live in ring/ and ringside/ (crowd_N.png, backcrowd_N.png)" : ar_view == 4 ? "recipe.json + _gen/ (results) + _orig/ (originals after Apply)" : "ring/ropes/: side_NN.png on one canvas + ropes.json (origin); ring/ropes.png = the rope lines"); }
    nk_layout_row_push(ctx, 150);
    if (sk_button_gated("Save + Pack arena", !stock && !tool_busy, stock ? "the stock arenas are packed by the build and never edited - copy one with + new arena" : "a tool is running")
        && !stock) { snprintf(cmd, sizeof cmd, "./wfengine --pack-arena \"%s\"", ar_names[ar_sel]); run_tool(cmd); ar_mt[0] = ar_mt[1] = 0; }
    nk_layout_row_end(ctx);
    }
    snprintf(dir, sizeof dir, "arenas/%s", ar_names[ar_sel]);
    { char ring[400], out[400], pr[420], po[420]; int has_ring, has_out;
      snprintf(ring, sizeof ring, "%s/ring", dir); snprintf(out, sizeof out, "%s/ringside", dir);
      snprintf(pr, sizeof pr, "%s/arena.json", ring); snprintf(po, sizeof po, "%s/arena.json", out);
      has_ring = access(pr, R_OK) == 0; has_out = access(po, R_OK) == 0;
      switch (ar_view) {
      case 0:                                             /* In-ring: the ring picture + the composite */
          if (!has_ring) { note("no in-ring view"); break; }
          draw_view_layers(ring, 0, 2);
          draw_previews(ar_names[ar_sel], "ring", 0);
          draw_roles(ring, 600);
          break;
      case 1:                                             /* Ringside: the floor picture + the composite */
          if (!has_out) { note("this arena has no ring-out view (the cage has no count-outs)"); break; }
          draw_view_layers(out, 100, 2);
          draw_previews(ar_names[ar_sel], "ringside", 16);
          draw_roles(out, 601);
          break;
      case 2:                                             /* Crowd: both views' animation frames */
          if (has_ring) { nk_layout_row_dynamic(ctx, 20, 1); nk_label_colored(ctx, "IN-RING VIEW", NK_TEXT_LEFT, nk_rgb(200, 200, 210)); draw_view_layers(ring, 200, 1); }
          if (has_out)  { nk_layout_row_dynamic(ctx, 20, 1); nk_label_colored(ctx, "RINGSIDE VIEW (the near crowd under, the far crowd over the wrestlers)", NK_TEXT_LEFT, nk_rgb(200, 200, 210)); draw_view_layers(out, 300, 1 | 4); }
          break;
      case 3:                                             /* Ropes: the rope lines + the side-rope sprites */
          if (!has_ring) { note("no in-ring view"); break; }
          draw_view_layers(ring, 400, 4);
          draw_rope_frames(ring, 0);
          break;
      default:                                            /* AI recipe */
          draw_arena_recipe(ar_names[ar_sel], ar_is_stock(ar_names[ar_sel]));
          break;
      }
    }
}

/* ---------------------------------------------------------- scenes
 * SCENE TYPES = the six stock screens (rendered headless by `--export-scene`
 * into build/scene-preview/<type>/, uneditable); LIBRARY SCENES =
 * scenes/<name>/ created from a type with '+ new scene' (the skins pattern):
 * a copy of the type's render (preview.png, tilemap/ in the arena format,
 * sprites/ frames, scene.json with "type"), editable - the structure (its
 * tilemap + sprite rows) is the type's and never changes.  Tabs per scene:
 * Preview | Tilemap | one per sprite row.  The profile picks a library scene
 * per type under Rules > Scenes ("scenes": {type: name}).  The ENGINE does
 * not yet draw library scenes (import = tilemap via the arena path, sprite
 * art via the clone-art arena) - the assignment is stored, not consumed. */
/* picks = how the Preview plays it (user 2026-08-30): 0 = the same whoever
 * plays (no dropdowns), 1 = one wrestler, 2 = two (walkout: the second may be
 * "(none)" = a one-man entrance) */
static const struct { const char *type, *label, *desc; int locked, picks; } sc_types[] = {
    { "walkout", "Walkout (aisle)",  "the entrance tunnel walk-in - scene word 4 tilemap + the VS banner", 0, 2 },
    { "talk",    "Talk screen",      "the between-round taunt: three row-0x2C panels + the row-0x4E announcer", 0, 0 },
    { "title",   "Title-win card",   "the championship card: portraits (row 0x4D) + the 17-letter marquee (row 0x4E)", 0, 1 },
    { "belt",    "Belt count",       "'N more matches to get to the title match': herald + count art (row 0x4F)", 0, 0 },
    { "ending",  "Ending + credits", "the championship ending: the champion's walk-off poses (row 0x4D) with the credit pages (row 0x50) rolling over the same screen - the pages are kept as they are, a homage to the developers", 0, 2 } };
#define SC_TYPES ((int)(sizeof sc_types / sizeof sc_types[0]))
#define SC_LIB_MAX 32
static char sc_lib[SC_LIB_MAX][64]; static int sc_libtype[SC_LIB_MAX]; static int sc_lib_n = -1;
static int sc_sel, sc_open, sc_new, sc_sub;           /* sel < SC_TYPES = a stock type, else library index + SC_TYPES */
static char sc_newname[64]; static int sc_newtype;
static int sc_started[SC_TYPES], sc_pending, sc_pending_type; static char sc_pending_name[64];
static SDL_Texture *sc_prev_tex; static time_t sc_prev_mt; static char sc_prev_path[300];
#define SC_FRAMES 128
static SDL_Texture *sc_ftex[SC_FRAMES]; static time_t sc_fmt[SC_FRAMES]; static char sc_fpath[SC_FRAMES][300];
static json_val *sc_doc; static char sc_doc_dir[300];
static int sc_grid;                                    /* a grid of single-pose sets is open */
static const char *sc_row_name(int row)
{
    switch (row) { case 0x2C: return "Panels"; case 0x4D: return "Portraits"; case 0x4E: return "Announcer & marquee"; case 0x4F: return "Belt & count art"; case 0x50: return "Credit pages (kept)"; default: return "Sprites"; }
}
static int sc_type_index(const char *t) { for (int i = 0; i < SC_TYPES; i++) if (!strcmp(sc_types[i].type, t)) return i; return -1; }
static void sc_scan(void)
{
    DIR *d = opendir("scenes"); struct dirent *e;
    sc_lib_n = 0;
    if (d) {
        while ((e = readdir(d)) && sc_lib_n < SC_LIB_MAX) {
            char p[300], err[128]; json_val *doc; int t;
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "scenes/%s/scene.json", e->d_name);
            doc = json_parse_file(p, err, sizeof err);
            if (!doc) continue;
            t = sc_type_index(json_str(json_get(doc, "type"), json_str(json_get(doc, "scene"), "")));
            json_free(doc);
            if (t < 0 || sc_type_index(e->d_name) >= 0) continue;      /* scenes/<type>/ = the converted stock, listed above */
            snprintf(sc_lib[sc_lib_n], 64, "%s", e->d_name); sc_libtype[sc_lib_n] = t; sc_lib_n++;
        }
        closedir(d);
    }
    for (int i = 1; i < sc_lib_n; i++) for (int j = i; j > 0 && strcmp(sc_lib[j - 1], sc_lib[j]) > 0; j--) { char t[64]; int tt = sc_libtype[j]; memcpy(t, sc_lib[j], 64); memcpy(sc_lib[j], sc_lib[j - 1], 64); sc_libtype[j] = sc_libtype[j - 1]; memcpy(sc_lib[j - 1], t, 64); sc_libtype[j - 1] = tt; }
    if (sc_sel >= SC_TYPES + sc_lib_n) sc_sel = 0;
}
static int sc_is_stock(void) { return sc_sel < SC_TYPES; }
static int sc_type(void) { return sc_is_stock() ? sc_sel : sc_libtype[sc_sel - SC_TYPES]; }
static int sc_render_dir(int type, char *dir, size_t n)   /* the converted stock scene scenes/<type>/, converted on demand */
{
    char p[400], cmd[600]; struct stat st;
    snprintf(dir, n, "scenes/%s", sc_types[type].type);
    snprintf(p, sizeof p, "%s/scene.json", dir);
    if (stat(p, &st) == 0) return 1;
    if (!sc_started[type] && !tool_busy) { sc_started[type] = 1; snprintf(cmd, sizeof cmd, "mkdir -p \"%s\" && ./wfengine --export-scene %s \"%s\" && ./wfengine --pack-scene %s", dir, sc_types[type].type, dir, sc_types[type].type); run_tool(cmd); }
    return 0;
}
static int sc_dir(char *dir, size_t n)                     /* the selected scene's folder, 0 = not there yet */
{
    if (sc_is_stock()) return sc_render_dir(sc_sel, dir, n);
    snprintf(dir, n, "scenes/%s", sc_lib[sc_sel - SC_TYPES]);
    return 1;
}
static void scene_create(const char *name, int type)     /* a library scene = the type's render, tagged */
{
    char src[300], dst[300], cmd[800], p[400], err[128]; json_val *doc;
    if (!sc_render_dir(type, src, sizeof src)) { sc_pending = 1; sc_pending_type = type; snprintf(sc_pending_name, sizeof sc_pending_name, "%s", name); logf_("rendering the %s first...", sc_types[type].label); return; }
    snprintf(dst, sizeof dst, "scenes/%s", name);
    snprintf(cmd, sizeof cmd, "mkdir -p \"%s\" && cp -r \"%s/.\" \"%s/\"", dst, src, dst);
    if (system(cmd)) { logf_("create failed"); return; }
    snprintf(p, sizeof p, "%s/scene.json", dst);
    doc = json_parse_file(p, err, sizeof err);
    if (doc) { json_set_string(doc, "name", name); json_set_string(doc, "type", sc_types[type].type); json_write_file(p, doc); json_free(doc); }
    snprintf(cmd, sizeof cmd, "./wfengine --pack-scene %s", name); run_tool(cmd);
    logf_("scene %s created from the %s (%s/) - packing", name, sc_types[type].label, dst);
    sc_lib_n = -1; sc_scan();
    for (int i = 0; i < sc_lib_n; i++) if (!strcmp(sc_lib[i], name)) { sc_sel = SC_TYPES + i; sc_open = 1; sc_new = 0; sc_sub = 0; }
}
static void scene_delete(const char *name)
{
    char cmd[400];
    if (!name[0] || strpbrk(name, "/\\ .")) return;
    snprintf(cmd, sizeof cmd, "rm -rf \"scenes/%s\"", name);
    if (system(cmd) == 0) logf_("deleted scene %s", name); else logf_("delete failed: %s", name);
    sc_lib_n = -1; sc_open = 0;
}
static void draw_scenes_left(void)
{
    char cmd[400];
    if (sc_lib_n < 0) sc_scan();
    if (sc_pending && !tool_busy) { sc_pending = 0; scene_create(sc_pending_name, sc_pending_type); }
    if (!sc_open && !sc_new) sc_open = 1;                   /* the highlighted scene IS shown */
    heading("Scenes", NULL);
    nk_layout_row_static(ctx, 24, 120, 1);
    { struct nk_rect b = WB();
      if (nk_button_label(ctx, "+ new scene")) { sc_new = 1; sc_open = 1; sc_newname[0] = 0; }
      hint_at(b, "A new library scene, copied from one of the stock screens: name it, pick the type, Create. Its structure (tilemap + sprite rows) is the type's and stays."); }
    nk_layout_row_dynamic(ctx, 22, 1);
    for (int i = 0; i < SC_TYPES; i++) {
        int sel = sc_sel == i; char lab[80];
        snprintf(lab, sizeof lab, "%s   (stock)", sc_types[i].label);
        if (nk_selectable_label(ctx, lab, NK_TEXT_LEFT, &sel) && sel) { sc_sel = i; sc_open = 1; sc_new = 0; sc_sub = 0; }
    }
    for (int i = 0; i < sc_lib_n; i++) {
        int sel = sc_sel == SC_TYPES + i; char lab[96]; struct nk_rect rb = WB();
        snprintf(lab, sizeof lab, "%s   (%s)", sc_lib[i], sc_types[sc_libtype[i]].type);
        if (nk_selectable_label(ctx, lab, NK_TEXT_LEFT, &sel) && sel) { sc_sel = SC_TYPES + i; sc_open = 1; sc_new = 0; sc_sub = 0; }
        if (nk_contextual_begin(ctx, 0, nk_vec2(300, 90), rb)) {
            static int rn_for = -1; static char rn_buf[40];
            if (rn_for != i) { rn_for = i; snprintf(rn_buf, sizeof rn_buf, "%s", sc_lib[i]); }
            nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
            nk_layout_row_push(ctx, 200); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, rn_buf, sizeof rn_buf, nk_filter_default);
            nk_layout_row_push(ctx, 80);
            if (nk_button_label(ctx, "Rename") && rn_buf[0] && strcmp(rn_buf, sc_lib[i]) && !strpbrk(rn_buf, "/\\ .")) {
                snprintf(cmd, sizeof cmd, "mv \"scenes/%s\" \"scenes/%s\"", sc_lib[i], rn_buf);
                if (system(cmd) == 0) { logf_("renamed scene %s -> %s", sc_lib[i], rn_buf); sc_lib_n = -1; } else logf_("rename failed");
                nk_contextual_close(ctx);
            }
            nk_layout_row_end(ctx);
            nk_layout_row_dynamic(ctx, 24, 1);
            if (nk_contextual_item_label(ctx, "Delete scene", NK_TEXT_LEFT)) { cf_kind = 5; cf_id = i; snprintf(cf_name, sizeof cf_name, "%s", sc_lib[i]); }
            nk_contextual_end(ctx);
        }
    }
    note("stock screens render into build/scene-preview/; right-click a library scene to rename or delete");
}
static void draw_scenes(void)
{
    char dir[300], p[400], cmd[700], err[128];
    int type, ready, nrows = 0;
    static const char *type_labels[SC_TYPES];
    if (sc_lib_n < 0) sc_scan();
    if (!sc_open) { note("pick a scene on the left, or + new scene"); return; }
    if (sc_new) {
        for (int i = 0; i < SC_TYPES; i++) type_labels[i] = sc_types[i].label;
        heading("New scene", "Copies one of the stock screens (its tilemap, sprite frames and scene.json) into scenes/<name>/. The scene TYPE decides its structure and cannot change afterwards; edit the PNGs in place.");
        nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
        nk_layout_row_push(ctx, 120); nk_label(ctx, "name", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 240); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, sc_newname, sizeof sc_newname, nk_filter_default);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
        nk_layout_row_push(ctx, 120); nk_label(ctx, "type", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 240); sc_newtype = nk_combo(ctx, type_labels, SC_TYPES, sc_newtype, 22, nk_vec2(240, 160));
        nk_layout_row_end(ctx);
        nk_layout_row_static(ctx, 26, 120, 1);
        { struct nk_rect b = WB();
          if (nk_button_label(ctx, "Create")) {
              if (!sc_newname[0] || strpbrk(sc_newname, "/\\ .")) logline("give the scene a plain name (no spaces, dots or slashes)");
              else scene_create(sc_newname, sc_newtype);
          }
          hint_at(b, "Copies the type's render into scenes/<name>/ (renders it first if it never was)."); }
        return;
    }
    type = sc_type();
    ready = sc_dir(dir, sizeof dir);
    /* scene.json of this folder (cached per dir) */
    if (ready && strcmp(sc_doc_dir, dir)) { snprintf(sc_doc_dir, sizeof sc_doc_dir, "%s", dir); if (sc_doc) json_free(sc_doc); snprintf(p, sizeof p, "%s/scene.json", dir); sc_doc = json_parse_file(p, err, sizeof err); }
    if (ready && sc_doc) { const json_val *rows = json_get(sc_doc, "rows"); for (const json_val *r = rows ? rows->child : NULL; r; r = r->next) nrows++; }
    /* TABS on their own line: Preview | Tilemap | one per sprite row */
    nk_layout_row_begin(ctx, NK_STATIC, 26, 3 + nrows);
    nk_layout_row_push(ctx, ed_tab_w("Preview")); if (ed_tab("Preview", sc_sub == 0)) sc_sub = 0;
    nk_layout_row_push(ctx, ed_tab_w("Tilemap")); if (ed_tab("Tilemap", sc_sub == 1)) sc_sub = 1;
    if (ready && sc_doc) { int k = 2; for (const json_val *r = json_get(sc_doc, "rows")->child; r; r = r->next, k++) { const char *nm = sc_row_name((int)json_int(json_get(r, "row"), 0)); nk_layout_row_push(ctx, ed_tab_w(nm)); if (ed_tab(nm, sc_sub == k)) sc_sub = k; } }
    nk_layout_row_push(ctx, ed_tab_w("Sounds")); if (ed_tab("Sounds", sc_sub == 2 + nrows)) sc_sub = 2 + nrows;
    nk_layout_row_end(ctx);
    heading(sc_is_stock() ? sc_types[type].label : sc_lib[sc_sel - SC_TYPES], sc_types[type].desc);
    if (!ready) { nk_layout_row_dynamic(ctx, 18, 1); nk_label_colored(ctx, "rendering the screen headless...", NK_TEXT_LEFT, nk_rgb(165, 165, 175)); return; }
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(165, 165, 175), "%s/  -  type %s%s  -  scene word %lld  -  tables: %s", dir, sc_types[type].type, sc_is_stock() ? " (stock)" : "", sc_doc ? (long long)json_int(json_get(sc_doc, "scene_word"), -1) : -1LL, sc_doc ? json_str(json_get(sc_doc, "tables"), "") : "");
    /* the button row BELOW the title (user 2026-08-30: tabs, then title + description, then buttons - every tab from now on) */
    nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
    nk_layout_row_push(ctx, 150);
    { struct nk_rect b = WB();
      if (sk_button_gated("Browse scene files", ready, "rendering first")) {
          char sub[64] = "";
          if (sc_sub == 1) snprintf(sub, sizeof sub, "/tilemap");
          else if (sc_sub >= 2 && sc_doc) { int k = 2; const json_val *r = json_get(sc_doc, "rows")->child; while (r && k < sc_sub) { r = r->next; k++; } if (r) snprintf(sub, sizeof sub, "/sprites/row_%02llX", (long long)json_int(json_get(r, "row"), 0)); }
          snprintf(cmd, sizeof cmd, "xdg-open \"%s%s\" >/dev/null 2>&1 &", dir, sub); if (system(cmd)) {} }
      hint_at(b, "Open THIS tab's files: the scene folder (preview.png, scene.json), tilemap/ (the arena layers format), or sprites/row_XX/pose_NNN.png for a sprite row."); }
    nk_layout_row_push(ctx, 150);
    if (sc_is_stock()) { struct nk_rect b = WB();
      if (sk_button_gated("Re-convert from ROM", !tool_busy, "a tool is running")) { snprintf(cmd, sizeof cmd, "rm -rf \"%s\" && mkdir -p \"%s\" && ./wfengine --export-scene %s \"%s\" && ./wfengine --pack-scene %s", dir, dir, sc_types[type].type, dir, sc_types[type].type); run_tool(cmd); sc_prev_mt = 0; sc_doc_dir[0] = 0; }
      hint_at(b, "Export this stock screen from the ROM again (page, sprites, script) and pack it - the converted stock scene every non-stock profile plays by default."); }
    else { struct nk_rect b = WB();
      if (sk_button_gated("Save + Pack scene", !tool_busy, "a tool is running")) { snprintf(cmd, sizeof cmd, "./wfengine --pack-scene %s", sc_lib[sc_sel - SC_TYPES]); run_tool(cmd); sc_doc_dir[0] = 0; }
      hint_at(b, "Pack scenes/<name>/ (tilemap/, scene.json) into build/scenes/<name>.pak - what the game plays."); }
    nk_layout_row_end(ctx);
    if (sc_sub == 0) {                                     /* the frame */
        static int pk[2] = { 0, 2 }, fr_at = 0; static const char *wsn[13]; static char scn[64]; const char *sname = sc_is_stock() ? sc_types[type].type : sc_lib[sc_sel - SC_TYPES];
        int picks = sc_types[type].picks, ncol;
        for (int i = 0; i < 12; i++) wsn[i] = ws_name(i);
        wsn[12] = "(none)";
        if (!fr_at) fr_at = !strcmp(sc_types[type].type, "talk") ? 220 : 160;
        if (strcmp(scn, sname)) { snprintf(scn, sizeof scn, "%s", sname); }
        ncol = 5 + (picks ? 1 + picks : 0);
        nk_layout_row_begin(ctx, NK_STATIC, 26, ncol);
        if (picks) {
            nk_layout_row_push(ctx, 60); nk_label(ctx, "play with", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 150); if (pk[0] > 11) pk[0] = 0; pk[0] = nk_combo(ctx, wsn, 12, pk[0], 22, nk_vec2(150, 300));
            if (picks > 1) { nk_layout_row_push(ctx, 150); pk[1] = nk_combo(ctx, wsn, !strcmp(sc_types[type].type, "walkout") ? 13 : 12, pk[1], 22, nk_vec2(150, 300)); }
        }
        nk_layout_row_push(ctx, 50); nk_label(ctx, "frame", NK_TEXT_RIGHT);
        nk_layout_row_push(ctx, 90); nk_property_int(ctx, "#", 1, &fr_at, 3000, 10, 1);
        nk_layout_row_push(ctx, 90);
        { struct nk_rect b = WB(); int p1 = picks > 1 && pk[1] < 12 ? pk[1] : (picks > 1 ? -1 : (pk[0] + 2) % 12);
          if (sk_button_gated("Render", !tool_busy, "a tool is running")) {
              snprintf(cmd, sizeof cmd, "mkdir -p build/scene-render && WF_PICKS=%d,%d,%d,%d ./wfengine %s%s --play-scene %s --headless --no-front --frames %d --shot build/scene-render/%s.png", picks ? pk[0] : 0, p1, (pk[0] + 2) % 12, p1 >= 0 ? (p1 + 2) % 12 : -1, wf_profile()[0] ? "--profile " : "", wf_profile(), sname, fr_at, sname);
              run_tool(cmd); }
          hint_at(b, picks ? "Render this scene with the engine's own scene player (sceneplay.c) at that frame, headless, with these wrestlers - the picture replaces the frame below." : "Render this scene with the engine's own scene player at that frame, headless - this screen is the same whoever plays; the picture replaces the frame below."); }
        nk_layout_row_push(ctx, 90);
        { struct nk_rect b = WB(); int p1 = picks > 1 && pk[1] < 12 ? pk[1] : (picks > 1 ? -1 : (pk[0] + 2) % 12);
          if (nk_button_label(ctx, "Play")) {
              snprintf(cmd, sizeof cmd, "WF_PICKS=%d,%d,%d,%d WF_SCENE_EXIT=1 ./wfengine %s%s --play-scene %s --no-front >/dev/null 2>&1 &", picks ? pk[0] : 0, p1, (pk[0] + 2) % 12, p1 >= 0 ? (p1 + 2) % 12 : -1, wf_profile()[0] ? "--profile " : "", wf_profile(), sname);
              if (system(cmd)) {} }
          hint_at(b, "Play this scene in a window with the engine's own scene player; the window closes when the scene ends."); }
        nk_layout_row_push(ctx, 40); nk_label(ctx, "", NK_TEXT_LEFT);
        nk_layout_row_end(ctx);
        /* ONE frame: the scene player's render when there is one (Render overwrites it), else the export's preview.png */
        snprintf(p, sizeof p, "build/scene-render/%s.png", sname);
        { int rendered = access(p, R_OK) == 0;
          if (!rendered) snprintf(p, sizeof p, "%s/preview.png", dir);
          nk_layout_row_dynamic(ctx, 16, 1);
          nk_label_colored(ctx, rendered ? "rendered by the scene player (build/scene-render/ - Render overwrites it)" : (sc_is_stock() ? "the ROM's own render at export (preview.png) - Render replaces it with the scene player's" : "preview.png (copied at creation) - Render replaces it with the scene player's"), NK_TEXT_LEFT, nk_rgb(165, 165, 175));
          if (strcmp(sc_prev_path, p)) { snprintf(sc_prev_path, sizeof sc_prev_path, "%s", p); if (sc_prev_tex) { SDL_DestroyTexture(sc_prev_tex); sc_prev_tex = NULL; } sc_prev_mt = 0; }
          sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1; sk_png_canvas = 2; sk_png_tex(p, &sc_prev_tex, &sc_prev_mt); sk_png_plain = 0; sk_png_canvas = 0;
          if (sc_prev_tex) { int w, h; SDL_QueryTexture(sc_prev_tex, NULL, NULL, &w, &h); nk_layout_row_static(ctx, (float)h * 2, w * 2, 1); nk_image(ctx, nk_image_ptr(sc_prev_tex)); } }
    } else if (sc_sub == 1) {                              /* the tilemap: the arena view machinery */
        char vd[400]; snprintf(vd, sizeof vd, "%s/tilemap", dir);
        snprintf(p, sizeof p, "%s/arena.json", vd);
        if (access(p, R_OK) == 0) draw_view_dir(vd); else note("no tilemap in this scene");
    } else if (sc_sub == 2 + nrows && sc_doc) {            /* SOUNDS: the script's own (read-only) + the editable "sounds" list */
        const json_val *music = json_get(sc_doc, "music"), *voices = json_get(sc_doc, "voices"), *acts = json_get(sc_doc, "actors"); int n = 0, vi = 0;
        static char ss_dir[300]; static int ss_n, ss_frame[32]; static char ss_ref[32][48]; static int ss_dirty;
        if (strcmp(ss_dir, dir)) {                          /* load the list from scene.json */
            const json_val *L = json_get(sc_doc, "sounds"); snprintf(ss_dir, sizeof ss_dir, "%s", dir); ss_n = 0; ss_dirty = 0;
            for (const json_val *q = L ? L->child : NULL; q && ss_n < 32; q = q->next) { ss_frame[ss_n] = (int)json_int(json_get(q, "frame"), 0); snprintf(ss_ref[ss_n], sizeof ss_ref[ss_n], "%s", json_str(json_get(q, "ref"), "")); ss_n++; }
        }
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, "YOUR SOUNDS - fired when the scene reaches the frame: a stock command or a WAV from the sounds/ library (Sounds > Library); Save writes scene.json, Save + Pack scene applies", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
        for (int i = 0; i < ss_n; i++) {
            nk_layout_row_begin(ctx, NK_STATIC, 24, 5);
            nk_layout_row_push(ctx, 50); nk_label(ctx, "frame", NK_TEXT_RIGHT);
            nk_layout_row_push(ctx, 110); { int v = ss_frame[i]; nk_property_int(ctx, "#f", 0, &v, 6000, 1, 1); if (v != ss_frame[i]) { ss_frame[i] = v; ss_dirty = 1; } }
            nk_layout_row_push(ctx, 300); if (snd_ref_combo(ss_ref[i], sizeof ss_ref[i], "(none)", 300)) ss_dirty = 1;
            nk_layout_row_push(ctx, 50); if (ss_ref[i][0]) { if (nk_button_label(ctx, "Play")) snd_ref_play(ss_ref[i]); } else nk_label(ctx, "", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 60); if (nk_button_label(ctx, "remove")) { for (int j = i; j + 1 < ss_n; j++) { ss_frame[j] = ss_frame[j + 1]; memcpy(ss_ref[j], ss_ref[j + 1], sizeof ss_ref[j]); } ss_n--; ss_dirty = 1; }
            nk_layout_row_end(ctx);
        }
        nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
        nk_layout_row_push(ctx, 110); if (ss_n < 32 && nk_button_label(ctx, "+ add sound")) { ss_frame[ss_n] = 0; ss_ref[ss_n][0] = 0; ss_n++; ss_dirty = 1; }
        nk_layout_row_push(ctx, 110);
        if (sk_button_gated("Save sounds", ss_dirty, "nothing changed")) {
            json_val *L = json_set_array(sc_doc, "sounds");
            for (int i = 0; i < ss_n; i++) { json_val *q = json_array_push_object(L); json_set_number(q, "frame", ss_frame[i]); json_set_string(q, "ref", ss_ref[i]); }
            snprintf(p, sizeof p, "%s/scene.json", dir);
            if (json_write_file(p, sc_doc) == 0) { ss_dirty = 0; logf_("%s: %d sound%s saved - Save + Pack scene to apply", p, ss_n, ss_n == 1 ? "" : "s"); } else logf_("cannot write %s", p);
        }
        nk_layout_row_push(ctx, 200); if (ss_dirty) nk_label_colored(ctx, "unsaved", NK_TEXT_LEFT, nk_rgb(240, 180, 60)); else nk_label(ctx, "", NK_TEXT_LEFT);
        nk_layout_row_end(ctx);
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, "THE SCRIPT'S OWN sounds (scene.json: music / voices / actors' sound_on_start / script steps with voice) - a YM command word", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
        for (const json_val *m = music ? music->child : NULL; m; m = m->next, n++) { nk_layout_row_dynamic(ctx, 18, 1); nk_labelf(ctx, NK_TEXT_LEFT, "at begin        music   0x%04llX", (long long)json_int(m, 0)); }
        for (const json_val *a = acts ? acts->child : NULL; a; a = a->next) {
            const json_val *sc = json_get(a, "script"); int k = 0;
            if (json_get(a, "sound_on_start")) { nk_layout_row_dynamic(ctx, 18, 1); nk_labelf(ctx, NK_TEXT_LEFT, "%-14s starts   0x%04llX", json_str(json_get(a, "name"), "?"), (long long)json_int(json_get(a, "sound_on_start"), 0)); n++; }
            for (const json_val *q = sc ? sc->child : NULL; q; q = q->next, k++)
                if (json_int(json_get(q, "voice"), 0)) { nk_layout_row_dynamic(ctx, 18, 1); nk_labelf(ctx, NK_TEXT_LEFT, "%-14s step %-3d cell %-3lld  voice #%d = 0x%04llX", json_str(json_get(a, "name"), "?"), k, (long long)json_int(json_get(q, "cell"), 0), vi, (long long)json_int(json_at(voices, vi), 0)); vi++; n++; }
        }
        if (!n) note("the script fires no sounds of its own");
    } else if (sc_doc) {                                   /* one sprite row: tiles + the frame modal */
        int k = 2; const json_val *r = json_get(sc_doc, "rows")->child;
        while (r && k < sc_sub) { r = r->next; k++; }
        if (r) {
            const json_val *frames = json_get(r, "frames"), *sets = json_get(r, "sets"); int n = 0, fi = 0, nsets = 0;
            for (const json_val *fr = frames ? frames->child : NULL; fr; fr = fr->next) n++;
            for (const json_val *q = sets ? sets->child : NULL; q; q = q->next) nsets++;
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "%s - sprite row 0x%02llX, stream %s, meta %s - %d frames in %d trigger set%s (click one to step through)", sc_row_name((int)json_int(json_get(r, "row"), 0)), (long long)json_int(json_get(r, "row"), 0), json_str(json_get(r, "stream"), "?"), json_str(json_get(r, "meta"), "?"), n, nsets ? nsets : 1, nsets == 1 ? "" : "s");
            /* one row of tiles per trigger set (else all frames as one set);
             * single-pose sets share one grid, the set label as the caption */
            for (int si = 0; si < (nsets ? nsets : 1); si++) {
                const json_val *set = nsets ? json_at(sets, si) : NULL, *poses = set ? json_get(set, "poses") : NULL; int per = 0, any = 0, single = 0;
                int npose = 0; for (const json_val *q = poses ? poses->child : NULL; q; q = q->next) npose++;
                single = set && npose == 1;
                for (const json_val *fr = frames ? frames->child : NULL; fr; fr = fr->next) { int pose = (int)json_int(json_get(fr, "pose"), -1), in = !set;
                    for (const json_val *q = poses ? poses->child : NULL; q && !in; q = q->next) if ((int)json_int(q, -2) == pose) in = 1;
                    if (in) { any = 1; break; } }
                if (!any) continue;
                if (set && !single) { nk_layout_row_dynamic(ctx, 16, 1); nk_label_colored(ctx, json_str(json_get(set, "label"), ""), NK_TEXT_LEFT, nk_rgb(165, 165, 175)); }
                if (single) {                                   /* the grid of singles: opened once, closed by the first non-single set */
                    if (!sc_grid) { nk_layout_row_static(ctx, 120 + 21 + 6, 126, 10); sc_grid = 1; }
                } else if (sc_grid) sc_grid = 0;
                for (const json_val *fr = frames ? frames->child : NULL; fr && fi < SC_FRAMES; fr = fr->next) {
                    int pose = (int)json_int(json_get(fr, "pose"), -1), in = !set; char cap[64];
                    for (const json_val *q = poses ? poses->child : NULL; q && !in; q = q->next) if ((int)json_int(q, -2) == pose) in = 1;
                    if (!in) continue;
                    if (!single && per == 0) nk_layout_row_static(ctx, 120 + 21 + 6, 126, 10);
                    snprintf(p, sizeof p, "%s/%s", dir, json_str(json_get(fr, "png"), ""));
                    if (strcmp(sc_fpath[fi], p)) { snprintf(sc_fpath[fi], sizeof sc_fpath[fi], "%s", p); if (sc_ftex[fi]) { SDL_DestroyTexture(sc_ftex[fi]); sc_ftex[fi] = NULL; } sc_fmt[fi] = 0; }
                    sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1; sk_png_tex(p, &sc_ftex[fi], &sc_fmt[fi]); sk_png_plain = 0;
                    if (single) snprintf(cap, sizeof cap, "%s  (pose %d)", json_str(json_get(set, "label"), ""), pose); else snprintf(cap, sizeof cap, "pose_%03d.png", pose);
                    if (frame_tile(sc_ftex[fi], cap) && !fr_on) {
                        int m = 0, mine = 0; char lab[96];
                        for (const json_val *q = frames->child; q && m < FR_MAX; q = q->next, m++) { if (q == fr) mine = m; snprintf(fr_paths[m], sizeof fr_paths[m], "%s/%s", dir, json_str(json_get(q, "png"), "")); }
                        snprintf(lab, sizeof lab, "%s - row 0x%02llX", sc_row_name((int)json_int(json_get(r, "row"), 0)), (long long)json_int(json_get(r, "row"), 0));
                        fr_open(lab, m, mine);
                    }
                    fi++; if (!single && ++per == 10) per = 0;
                }
            }
            sc_grid = 0;
        }
    }
}
/* Rules > Scenes: which library scene the profile uses per TYPE (stored in
 * profiles/<p>.json "scenes"; the engine does not consume it yet) */
static void sc_profile_get(const char *names[SC_TYPES])
{
    static char buf[SC_TYPES][64]; char pth[300], err[128]; json_val *doc;
    for (int i = 0; i < SC_TYPES; i++) { buf[i][0] = 0; names[i] = buf[i]; }
    if (!wf_profile()[0]) return;
    snprintf(pth, sizeof pth, "profiles/%s.json", wf_profile());
    doc = json_parse_file(pth, err, sizeof err); if (!doc) return;
    for (int i = 0; i < SC_TYPES; i++) snprintf(buf[i], 64, "%s", json_str(json_get(json_get(doc, "scenes"), sc_types[i].type), ""));
    json_free(doc);
}
static void sc_profile_set(int type, const char *name)
{
    char pth[300], err[128]; json_val *doc, *map;
    if (!wf_profile()[0]) return;
    snprintf(pth, sizeof pth, "profiles/%s.json", wf_profile());
    doc = json_parse_file(pth, err, sizeof err); if (!doc) return;
    map = json_set_object(doc, "scenes");
    if (name && name[0]) json_set_string(map, sc_types[type].type, name); else json_remove(map, sc_types[type].type);
    if (map->n == 0) json_remove(doc, "scenes");
    if (json_write_file(pth, doc) == 0) logf_("%s: %s -> %s", pth, sc_types[type].label, name && name[0] ? name : "stock"); else logf_("cannot write %s", pth);
    json_free(doc);
}
static void draw_rules_scenes(void)
{
    const char *cur[SC_TYPES];
    if (sc_lib_n < 0) sc_scan();
    heading("Scenes", "Which scene each screen uses in this profile: stock = the game's own, or a library scene of that type from scenes/<name>/ (the Scenes tab makes them). Saved straight into profiles/<name>.json. NOTE: the engine does not draw library scenes yet - this records the choice for the import step.");
    sc_profile_get(cur);
    for (int i = 0; i < SC_TYPES; i++) {
        const char *items[SC_LIB_MAX + 1]; int n = 1, idx = 0, pick;
        items[0] = "stock";
        for (int k = 0; k < sc_lib_n; k++) if (sc_libtype[k] == i) { items[n] = sc_lib[k]; if (!strcmp(sc_lib[k], cur[i])) idx = n; n++; }
        nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
        nk_layout_row_push(ctx, 260); nk_label(ctx, sc_types[i].label, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 240);
        pick = nk_combo(ctx, items, n, idx, 22, nk_vec2(240, 200));
        nk_layout_row_push(ctx, 400);
        nk_label_colored(ctx, n > 1 ? "library scenes of this type" : "no library scene of this type yet (Scenes tab: + new scene)", NK_TEXT_LEFT, nk_rgb(165, 165, 175));
        nk_layout_row_end(ctx);
        if (pick != idx) sc_profile_set(i, pick ? items[pick] : "");
    }
}

static void draw_weapons_left(void)
{
    heading("Weapons", NULL);
    if (wp_loaded != wp_gen) wp_load();
    nk_layout_row_static(ctx, 24, 170, 1);   /* buttons ABOVE the list (user 2026-08-27), one per
                                                row: two across clipped the second (2026-08-29) */
    { struct nk_rect b = WB();
      if (({ int h = nk_button_label(ctx, "Save to layer + pack"); save_icon(b, 0); h; })) wp_save();
      hint_at(b, "Writes the whole table into mods/<layer>/weapons/weapons.json and repacks the profile (its own weapons.pak)."); }
    if (nk_button_label(ctx, "Check vs ROM")) run_tool("./wfengine --weapon-check");
    if (nk_button_label(ctx, "Regenerate from ROM")) { run_tool("./wfengine --weapon-table data/weapons"); wp_loaded = -1; }
    nk_layout_row_dynamic(ctx, 22, 1);
    for (int t = 0; t < wp_ntypes; t++) {
        int sel = wp_sel == t; char nm[80];
        if (wp_img[t][0]) snprintf(nm, sizeof nm, "%s   image %s", wp_tname[t], wp_img[t]);
        else snprintf(nm, sizeof nm, "%s   %d poses", wp_tname[t], wp_npose[t]);
        if (nk_selectable_label(ctx, nm, NK_TEXT_LEFT, &sel) && sel) wp_sel = t;
    }
    {   /* NEW TYPE from a PNG dropped into the save layer's weapons/ dir
         * (2026-08-27): every *.png there that no type references yet gets
         * an Add button; Add appends { "image": file } to the table (Save +
         * pack quantises it, cuts arena tiles and generates the carry +
         * ringside cells). */
        const char *layer = mod_layer;
        if (!layer[0] && wf_profile()[0] && wf_profile_nmods() > 0) layer = wf_profile_mod(wf_profile_nmods() - 1);
        if (layer[0] && wp_doc) {
            char p[300]; DIR *d; struct dirent *e; int shown = 0;
            snprintf(p, sizeof p, "mods/%s/weapons", layer);
            d = opendir(p);
            while (d && (e = readdir(d))) {
                size_t l = strlen(e->d_name); int used = 0;
                const json_val *types = json_get(wp_doc, "types");
                if (l < 5 || l > 60 || strcmp(e->d_name + l - 4, ".png")) continue;
                for (const json_val *t = types ? types->child : NULL; t && !used; t = t->next)
                    used = !strcmp(json_str(json_get(t, "image"), ""), e->d_name)
                        || !strcmp(json_str(json_get(t, "image_swing"), ""), e->d_name);
                if (used) continue;
                if (!shown++) note("new art in the layer (weapons/*.png):");
                {
                    char bl[80], stem[16];
                    snprintf(stem, sizeof stem, "%.*s", (int)(l - 4 < 15 ? l - 4 : 15), e->d_name);
                    snprintf(bl, sizeof bl, "Add type '%s'", stem);
                    nk_layout_row_static(ctx, 24, 170, 1);
                    { struct nk_rect b = WB(); int hit = nk_button_label(ctx, bl);
                      hint_at(b, "Appends { \"image\": the PNG } to the table. Save to layer + pack then cuts its tiles and generates the carry / ringside sprites.");
                      if (hit && wp_ntypes < WP_MAXT) {
                          json_val *types_w = (json_val *)json_get(wp_doc, "types");
                          if (types_w) {
                              json_val *t = jv_append(types_w, jv_new(JSON_OBJECT, stem));
                              jv_set_string(t, "image", e->d_name);
                              { int k = wp_ntypes++;
                                snprintf(wp_tname[k], 16, "%s", stem);
                                wp_palfile[k][0] = 0; wp_npose[k] = 0;
                                wp_rules[k][0] = wp_rules[k][1] = wp_rules[k][2] = 0;
                                snprintf(wp_img[k], 64, "%s", e->d_name);
                                wp_sel = k; }
                              logf_("weapons: added type '%s' from %s - Save to layer + pack to build it", stem, e->d_name);
                          }
                      } }
                }
            }
            if (d) closedir(d);
        }
    }
    note("weapon slots (out = ringside, in = ring):");
    { const char *items[WP_MAXT + 1]; for (int t = 0; t < wp_ntypes; t++) items[t] = wp_tname[t];
      items[wp_ntypes] = "(empty)";
      for (int k = 0; k < ENG_WEAPONS && k < wp_nslots && wp_ntypes; k++) {
          float r[2] = { 0.3f, 0.7f };
          nk_layout_row(ctx, NK_DYNAMIC, 24, 2, r);
          nk_labelf(ctx, NK_TEXT_RIGHT, "%s %d", wp_slot_in[k] ? "ring" : "ringside", k);
          if (wp_spawn[k] > wp_ntypes) wp_spawn[k] = wp_ntypes;
          wp_spawn[k] = nk_combo(ctx, items, wp_ntypes + 1, wp_spawn[k], 22, nk_vec2(200, 220));
      } }
}
static void draw_weapons(void)
{
    const json_val *types, *t = NULL, *poses;
    if (wp_loaded != wp_gen) wp_load();
    if (!wp_doc) { note("no weapon table - run Regenerate table"); return; }
    heading("Weapon palette", "palette.json = body-palette row 15, the bank every stock weapon draws in. A type with its own palette file borrows a free bank at match time.");
    nk_layout_row_dynamic(ctx, 22, 16);
    for (int i = 0; i < 16; i++) {
        unsigned v = wp_pens[i];
        nk_button_color(ctx, nk_rgb((v & 0xF) * 17, ((v >> 4) & 0xF) * 17, ((v >> 8) & 0xF) * 17));
    }
    if (wp_sel >= wp_ntypes) return;
    types = json_get(wp_doc, "types");
    for (const json_val *x = types ? types->child : NULL; x; x = x->next) if (x->key && !strcmp(x->key, wp_tname[wp_sel])) t = x;
    if (!t) return;
    {
        char title[64]; snprintf(title, sizeof title, "type: %s", wp_tname[wp_sel]);
        heading(title, "palette file (blank = the shared row; a name = its own bank, created in the layer on Save), rules row (0 = the weapon_rules scalar).");
    }
    {
        float r[10] = { 0.08f, 0.16f, 0.09f, 0.09f, 0.09f, 0.09f, 0.11f, 0.09f, 0.09f, 0.09f };
        nk_layout_row(ctx, NK_DYNAMIC, 26, 10, r);
        nk_label(ctx, "palette", NK_TEXT_RIGHT);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, wp_palfile[wp_sel], sizeof wp_palfile[wp_sel], nk_filter_default);
        for (int f = 0; f < 4; f++) {
            nk_label(ctx, wp_rule_key[f], NK_TEXT_RIGHT);
            nk_property_int(ctx, "#", 0, &wp_rules[wp_sel][f], 4095, 1, 1);
        }
    }
    poses = json_get(t, "poses");
    if (wp_img[wp_sel][0]) {           /* PNG-ingested type: show the source art; its carry /
                                          swing / ringside cells are generated at pack time */
        char path[300]; int w = 60, h = 60;
        heading("source image", "the pack quantises it to an own 15-pen palette, cuts arena tiles and places it on the shared box carry poses (120-123/128-131) + the ringside lying/tumble sprites.");
        wp_resolve(wp_img[wp_sel], path, sizeof path);
        sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1;
        sk_png_tex(path, &wp_tex[wp_sel][0], &wp_mt[wp_sel][0]);
        if (wp_tex[wp_sel][0]) { SDL_QueryTexture(wp_tex[wp_sel][0], NULL, NULL, &w, &h); w *= 3; h *= 3; }
        nk_layout_row_static(ctx, (float)h, w, 1);
        if (wp_tex[wp_sel][0]) nk_image(ctx, nk_image_ptr(wp_tex[wp_sel][0])); else nk_labelf(ctx, NK_TEXT_LEFT, "missing: %s", path);
        sk_png_plain = 0;
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_labelf_wrap(ctx, "%s (max 96x96; optional \"image_swing\" PNG for the swing frames, \"nudge\": [dx,dy] to shift the carry anchor)", path);
        return;
    }
    heading("carry / pickup / swing poses", "the overlay cells alone (data/weapons/<type>/overlay_NNNN.png); the body is drawn under them at the wrestler's attach point.");
    {
        int k = 0, col = 0;
        sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1;
        for (const json_val *p = poses ? poses->child : NULL; p && k < 24; p = p->next, k++) {
            char path[300]; int w = 60, h = 110;
            snprintf(path, sizeof path, "data/weapons/%s/overlay_%04d.png", wp_tname[wp_sel], atoi(p->key ? p->key : "0"));
            sk_png_tex(path, &wp_tex[wp_sel][k], &wp_mt[wp_sel][k]);
            if (col == 0) nk_layout_row_begin(ctx, NK_STATIC, 130, 12);
            if (wp_tex[wp_sel][k]) { SDL_QueryTexture(wp_tex[wp_sel][k], NULL, NULL, &w, &h); w = w * 110 / (h ? h : 1); h = 110; }
            nk_layout_row_push(ctx, (float)(w + 8));
            if (nk_group_begin(ctx, path, NK_WINDOW_NO_SCROLLBAR)) {
                nk_layout_row_dynamic(ctx, (float)h, 1);
                if (wp_tex[wp_sel][k]) nk_image(ctx, nk_image_ptr(wp_tex[wp_sel][k])); else nk_label(ctx, "-", NK_TEXT_CENTERED);
                nk_layout_row_dynamic(ctx, 14, 1);
                nk_label_colored(ctx, p->key ? p->key : "?", NK_TEXT_CENTERED, nk_rgb(150, 150, 160));
                nk_group_end(ctx);
            }
            if (++col == 6) { nk_layout_row_end(ctx); col = 0; }
        }
        if (col) nk_layout_row_end(ctx);
    }
    {
        const json_val *att = json_get(wp_doc, "attach"); int n = 0;
        for (const json_val *a = att ? att->child : NULL; a; a = a->next) n += a->n;
        nk_layout_row_dynamic(ctx, 40, 1);
        nk_labelf_wrap(ctx, "attach offsets: %d (wrestler, pose) hand positions vs wrestler 00; a clone or generic takes his class base's. New weapon ART: drop a PNG into the layer's weapons/ dir and Add it as a type on the left.", n);
    }
    sk_png_plain = 0;
}

/* ------------------------------------------------------- Classes tab
 * (2026-08-27): the five body-class GENERICS (data/generics/N): summary,
 * base ref, needs, and the alias-group review - reject members of a
 * group that are not the same drawing, accept suggested near-merges ->
 * aliases.override.json, then Rebuild generic. */
#define CL_MAXG 1024
static int cl_sel, cl_loaded = -1, cl_page, cl_ng, cl_nrej, cl_nadd;
static int cl_sel_for_status(void) { return cl_sel; }
static int cs_loaded = -1;         /* class poses viewer: which class's status.json is loaded (-1 = reload) */
static int cl_g_of[CL_MAXG]; static unsigned char cl_g_kind[CL_MAXG];   /* 0 group, 1 suggested */
static unsigned char cl_rej[2048], cl_add[2048];
static json_val *cl_al;
static SDL_Texture *cl_tex_ref, *cl_tex_sheet[3]; static time_t cl_mt_ref, cl_mt_sheet[3];
static char cl_rejbuf[3][160]; static int cl_rejbuf_page = -1;
static char cl_summary[400], cl_needs[600];
static const char *cl_names[5] = { "0 medium", "1 lean", "2 small", "3 heavy", "4 giant" };
static void cl_load(int c)
{
    char path[300], err[128]; json_val *d; const json_val *al, *sg;
    cl_loaded = c; cl_page = 0; cl_ng = 0; cl_nrej = cl_nadd = 0; cl_rejbuf_page = -1;
    memset(cl_rej, 0, sizeof cl_rej); memset(cl_add, 0, sizeof cl_add);
    if (cl_al) { json_free(cl_al); cl_al = NULL; }
    snprintf(path, sizeof path, "data/generics/%d/summary.json", c);
    d = json_parse_file(path, err, sizeof err);
    if (d) {
        snprintf(cl_summary, sizeof cl_summary, "universe %lld ids - stock frames %lld body + %lld victim - stand-ins %lld - aliases %lld in %lld groups - needs %lld borrowed + %lld victim - UNIQUE TO DRAW %lld",
                 (long long)json_int(json_get(d, "universe"), 0), (long long)json_int(json_get(d, "frames_body"), 0), (long long)json_int(json_get(d, "frames_victim"), 0),
                 (long long)json_int(json_get(d, "stand_in_frames"), 0), (long long)json_int(json_get(d, "aliases"), 0), (long long)json_int(json_get(d, "alias_groups"), 0),
                 (long long)json_int(json_get(d, "needs_borrowed"), 0), (long long)json_int(json_get(d, "needs_self_victim"), 0), (long long)json_int(json_get(d, "unique_to_draw"), 0));
        json_free(d);
    } else snprintf(cl_summary, sizeof cl_summary, "(not built - Rebuild generic)");
    snprintf(path, sizeof path, "data/generics/%d/needs.json", c);
    d = json_parse_file(path, err, sizeof err);
    cl_needs[0] = 0;
    if (d) {
        const json_val *es = json_get(d, "entries"); int nb = 0, nv = 0, no = 0;
        for (const json_val *e = es ? es->child : NULL; e; e = e->next) {
            const char *r = json_str(json_get(e, "reason"), "");
            if (strstr(r, "victim")) nv++; else if (strstr(r, "borrow")) nb++; else no++;
        }
        snprintf(cl_needs, sizeof cl_needs, "needs %d: %d borrowed poses, %d self-victim frames, %d other - the frames this class cannot supply from stock (the generation list; needs.json)", es ? es->n : 0, nb, nv, no);
        json_free(d);
    }
    snprintf(path, sizeof path, "data/generics/%d/aliases.json", c);
    cl_al = json_parse_file(path, err, sizeof err);
    al = cl_al ? json_get(cl_al, "aliases") : NULL; sg = cl_al ? json_get(cl_al, "suggested") : NULL;
    for (int kind = 0; kind < 2; kind++) {
        const json_val *m = kind ? sg : al;
        for (const json_val *e = m ? m->child : NULL; e && cl_ng < CL_MAXG; e = e->next) {
            int of = (int)json_int(json_get(e, "of"), -1), seen = 0;
            for (int g = 0; g < cl_ng; g++) if (cl_g_kind[g] == kind && cl_g_of[g] == of) seen = 1;
            if (!seen && of >= 0) { cl_g_of[cl_ng] = of; cl_g_kind[cl_ng] = (unsigned char)kind; cl_ng++; }
        }
    }
    snprintf(path, sizeof path, "data/generics/%d/aliases.override.json", c);
    d = json_parse_file(path, err, sizeof err);
    if (d) {
        const json_val *r = json_get(d, "reject"), *a = json_get(d, "add");
        for (const json_val *e = r ? r->child : NULL; e; e = e->next) { int id = (int)json_int(e, -1); if (id >= 0 && id < 2048) { cl_rej[id] = 1; cl_nrej++; } }
        for (const json_val *e = a ? a->child : NULL; e; e = e->next) { int id = e->key ? atoi(e->key) : -1; if (id >= 0 && id < 2048) { cl_add[id] = 1; cl_nadd++; } }
        json_free(d);
    }
}
static int cl_members(int g, int *out, int max)   /* member ids of review group g */
{
    const json_val *m = cl_al ? json_get(cl_al, cl_g_kind[g] ? "suggested" : "aliases") : NULL; int n = 0;
    for (const json_val *e = m ? m->child : NULL; e && n < max; e = e->next)
        if ((int)json_int(json_get(e, "of"), -1) == cl_g_of[g] && e->key) out[n++] = atoi(e->key);
    return n;
}
static void cl_rejbuf_sync(void)           /* page -> the reject text fields */
{
    if (cl_rejbuf_page == cl_page) return;
    cl_rejbuf_page = cl_page;
    for (int i = 0; i < 3; i++) {
        int g = cl_page * 3 + i, mem[256], n; size_t o = 0;
        cl_rejbuf[i][0] = 0;
        if (g >= cl_ng || cl_g_kind[g]) continue;
        n = cl_members(g, mem, 256);
        for (int k = 0; k < n; k++) if (mem[k] < 2048 && cl_rej[mem[k]] && o < sizeof cl_rejbuf[i] - 8) o += (size_t)snprintf(cl_rejbuf[i] + o, sizeof cl_rejbuf[i] - o, "%s%d", o ? " " : "", mem[k]);
    }
}
static void cl_rejbuf_apply(void)          /* the reject text fields -> cl_rej for this page's groups */
{
    if (cl_rejbuf_page < 0) return;
    for (int i = 0; i < 3; i++) {
        int g = cl_rejbuf_page * 3 + i, mem[256], n; const char *p = cl_rejbuf[i];
        if (g >= cl_ng || cl_g_kind[g]) continue;
        n = cl_members(g, mem, 256);
        for (int k = 0; k < n; k++) if (mem[k] < 2048) cl_rej[mem[k]] = 0;
        while (*p) { int id; if (sscanf(p, "%d", &id) == 1 && id >= 0 && id < 2048) cl_rej[id] = 1; while (*p && *p != ' ') p++; while (*p == ' ') p++; }
    }
}
static void cl_save_override(void)
{
    char path[300]; FILE *f; int first = 1; const json_val *sg;
    cl_rejbuf_apply();
    snprintf(path, sizeof path, "data/generics/%d/aliases.override.json", cl_sel);
    f = fopen(path, "w");
    if (!f) { logf_("cannot write %s", path); return; }
    fprintf(f, "{ \"reject\": [");
    for (int id = 0; id < 2048; id++) if (cl_rej[id]) { fprintf(f, "%s%d", first ? "" : ", ", id); first = 0; }
    fprintf(f, "],\n  \"add\": {");
    first = 1; sg = cl_al ? json_get(cl_al, "suggested") : NULL;
    for (const json_val *e = sg ? sg->child : NULL; e; e = e->next) {
        int id = e->key ? atoi(e->key) : -1;
        if (id < 0 || id >= 2048 || !cl_add[id]) continue;
        fprintf(f, "%s\n    \"%d\": {\"of\": %lld, \"flip\": %lld, \"dx\": %lld, \"dy\": %lld}", first ? "" : ",", id,
                (long long)json_int(json_get(e, "of"), 0), (long long)json_int(json_get(e, "flip"), 0), (long long)json_int(json_get(e, "dx"), 0), (long long)json_int(json_get(e, "dy"), 0));
        first = 0;
    }
    fprintf(f, "\n  } }\n"); fclose(f);
    cl_nrej = cl_nadd = 0;
    for (int id = 0; id < 2048; id++) { cl_nrej += cl_rej[id]; cl_nadd += cl_add[id]; }
    logf_("saved %s: %d rejected, %d added - Rebuild generic to apply", path, cl_nrej, cl_nadd);
    snprintf(save_msg, sizeof save_msg, "saved alias override"); save_msg_t = SDL_GetTicks();
}
static void draw_classes_left(void)
{
    heading("Body classes", NULL);
    if (cl_loaded != cl_sel) cl_load(cl_sel);
    nk_layout_row_dynamic(ctx, 24, 1);     /* one per row in the narrow frame; the class is READ-ONLY here -
                                              the template is built by the standalone tool (user 2026-08-28) */
    { char cmd[200]; struct nk_rect b;
      b = WB(); if (nk_button_label(ctx, "Verify (status)")) { snprintf(cmd, sizeof cmd, "./wfengine --class-verify %d", cl_sel); run_tool(cmd); cs_loaded = -1; }
      hint_at(b, "Re-read the class template: --class-verify writes data/generics/C/status.json (own / alias / generated / stand-in / needs / missing) and the verdict.");
      b = WB(); if (nk_button_label(ctx, "Dedupe (pairs)")) { snprintf(cmd, sizeof cmd, "./wfengine --class-dedupe %d", cl_sel); run_tool(cmd); cs_loaded = -1; dd_n = -1; }
      hint_at(b, "--class-dedupe: every frame against every other (mirrored too, singles and victims). Certain duplicates are aliased; near pairs go to the 'duplicates to review' row.");
      b = WB(); if (nk_button_label(ctx, "Rebuild generic")) { snprintf(cmd, sizeof cmd, "./wfengine --generic-class %d data/generics/%d", cl_sel, cl_sel); run_tool(cmd); cl_loaded = -1; cs_loaded = -1; }
      hint_at(b, "Re-derive the class manifest from stock (aliases, needs, review sheets). Generated frames in gen/ are kept.");
      b = WB(); if (nk_button_label(ctx, "Pack as fallback")) { snprintf(cmd, sizeof cmd, "./wfengine --class-pack %d && %s", cl_sel, pack_cmd()); run_tool(cmd); }
      hint_at(b, "--class-pack: the class generic becomes the hidden class slot = the fallback art for an unfinished skin (skin -> generic -> base), then the profile is packed.");
      b = WB(); if (nk_button_label(ctx, "Open review folder")) { snprintf(cmd, sizeof cmd, "xdg-open data/generics/%d/review", cl_sel); run_tool(cmd); }
      hint_at(b, "The alias review sheets (Aliases sub-tab) as PNG files.");
      if (({ struct nk_rect sb = WB(); int h = nk_button_label(ctx, "Save override"); save_icon(sb, 0); h; })) cl_save_override(); }
    nk_layout_row_dynamic(ctx, 24, 1);
    for (int c = 0; c < 5; c++) {
        int sel = cl_sel == c;
        if (nk_selectable_label(ctx, cl_names[c], NK_TEXT_LEFT, &sel) && sel) cl_sel = c;
    }
}
/* ---- CLASS POSES viewer (user 2026-08-28): the Skins > Poses layout over
 * the class TEMPLATE - data/generics/C/status.json (--class-verify) says
 * per id: own / alias / generated / stand-in / needs / missing. Read-only:
 * the class is built by the standalone tool (--class-build), not here. */
enum { CS_OWN, CS_ALIAS, CS_GEN, CS_STANDIN, CS_NEEDS, CS_MISSING, CS_NONE };
static unsigned char cs_st[2048]; static int cs_of[2048], cs_owner[2048]; static char cs_verdict[200];
static int cs_counts[6];
static void cs_load(int cls)
{
    char p[300], err[128]; json_val *doc;
    memset(cs_st, CS_NONE, sizeof cs_st); for (int i = 0; i < 2048; i++) { cs_of[i] = -1; cs_owner[i] = -1; }
    memset(cs_counts, 0, sizeof cs_counts); cs_verdict[0] = 0;
    snprintf(p, sizeof p, "data/generics/%d/status.json", cls);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) { snprintf(cs_verdict, sizeof cs_verdict, "no status.json - run: ./wfengine --class-verify %d", cls); cs_loaded = cls; return; }
    for (const json_val *e = json_get(doc, "ids") ? json_get(doc, "ids")->child : NULL; e; e = e->next) {
        int id = atoi(e->key); const char *st = json_str(json_get(e, "status"), "");
        if (id < 0 || id >= 2048) continue;
        cs_st[id] = !strcmp(st, "own") ? CS_OWN : !strcmp(st, "alias") ? CS_ALIAS : !strcmp(st, "generated") ? CS_GEN : !strcmp(st, "stand-in") ? CS_STANDIN : !strcmp(st, "needs") ? CS_NEEDS : CS_MISSING;
        cs_of[id] = (int)json_int(json_get(e, "of"), -1); cs_owner[id] = (int)json_int(json_get(e, "owner"), -1);
        if (cs_st[id] < 6) cs_counts[cs_st[id]]++;
    }
    snprintf(cs_verdict, sizeof cs_verdict, "%s - own %d  alias %d  generated %d | stand-in %d  needs %d  missing %d",
             json_get(doc, "complete") && json_int(json_get(doc, "complete"), 0) ? "COMPLETE" : "INCOMPLETE",
             cs_counts[0], cs_counts[1], cs_counts[2], cs_counts[3], cs_counts[4], cs_counts[5]);
    json_free(doc);
    {   /* frames already in gen/ count as generated even before --class-build's
           final verify rewrites status.json (live refresh, user 2026-08-28) */
        DIR *d; struct dirent *e; int pn;
        snprintf(p, sizeof p, "data/generics/%d/gen", cls);
        if ((d = opendir(p))) {
            while ((e = readdir(d))) if (sscanf(e->d_name, "pose_%d.png", &pn) == 1 && pn >= 0 && pn < 2048 && cs_st[pn] != CS_NONE && cs_st[pn] != CS_GEN && cs_st[pn] != CS_ALIAS) {
                if (cs_st[pn] < 6) cs_counts[cs_st[pn]]--;
                cs_st[pn] = CS_GEN; cs_counts[CS_GEN]++;
            }
            closedir(d);
        }
    }
    cs_loaded = cls;
}
/* counts for a set of ids at class level: unique = not an alias; done = own/generated */
static void cs_count(const int *ids, int n, unsigned char *seen, int *unique, int *done, int *alias)
{
    for (int k = 0; k < n; k++) {
        int id = ids[k];
        if (id < 0 || id >= 2048 || seen[id]) continue;
        seen[id] = 1;
        if (cs_st[id] == CS_ALIAS) (*alias)++;
        else if (cs_st[id] == CS_NONE) continue;
        else { (*unique)++; if (cs_st[id] == CS_GEN) (*done)++; }   /* done = drawn from the base ref; 'own' is stock identity, still to convert */
    }
}
static int cs_sel = -2; static int cs_log_keep;   /* the log box stays after a build ran */
static int cs_grid_sel = 1;        /* build sheet size dropdown: 0 = 4x4, 1 = 3x3, 2 = 2x2, 3 = 1x1 */
static int cs_row(int key, const char *label, int done, int unique)
{
    int sel = cs_sel == key; char t[120]; struct nk_rect b;
    snprintf(t, sizeof t, "%s   %d/%d", label, done, unique);
    b = WB(); rows_add(key);
    if (nk_selectable_label(ctx, t, NK_TEXT_LEFT, &sel) && sel) cs_sel = key;
    if (done == unique && unique) tick_icon(b);
    move_rename_popup(b, key);         /* right-click: rename the move (global) */
    return sel;
}
static void cs_grid(int cls, const char *kind, int id, const int *ids, int n, int cols)
{
    static int order[700]; int no = 0;
    if (pi_n < 0) pi_load();
    for (int a = 0; a < pi_n; a++)
        if (!strcmp(pi_anim[a].kind, kind) && pi_anim[a].id == id)
            for (int f = 0; f < pi_anim[a].n && no < 700; f++) { int v = pi_anim[a].pose[f]; int dup = 0; for (int q = 0; q < no; q++) if (order[q] == v) dup = 1; if (!dup) order[no++] = v; }
    for (int k = 0; k < n && no < 700; k++) { int dup = 0; for (int q = 0; q < no; q++) if (order[q] == ids[k]) dup = 1; if (!dup) order[no++] = ids[k]; }
    if (!sp_show_alias) { int w = 0; for (int k = 0; k < no; k++) if (cs_st[order[k]] != CS_ALIAS && cs_st[order[k]] != CS_NONE) order[w++] = order[k]; no = w; }
    if (!no) { nk_layout_row_dynamic(ctx, 22, 1); nk_label_colored(ctx, "(nothing to draw for this animation on this class)", NK_TEXT_LEFT, nk_rgb(150, 150, 160)); return; }
    if (!strcmp(kind, "class")) {      /* the everything / other views: NEWLY GENERATED first (user 2026-08-28) */
        static long long mt[700]; char gp[300]; struct stat gs;
        for (int k = 0; k < no; k++) {
            mt[k] = 0;
            if (cs_st[order[k]] == CS_GEN) { snprintf(gp, sizeof gp, "data/generics/%d/gen/pose_%04d.png", cls, order[k]); if (stat(gp, &gs) == 0) mt[k] = (long long)gs.st_mtime + 1; }
        }
        for (int i = 1; i < no; i++) {   /* stable insertion: generated (newest first), then the rest in order */
            int v = order[i]; long long m = mt[i]; int j = i - 1;
            while (j >= 0 && mt[j] < m) { order[j + 1] = order[j]; mt[j + 1] = mt[j]; j--; }
            order[j + 1] = v; mt[j + 1] = m;
        }
    }
    if (cols < 1) cols = 1;
    for (int row = 0; row < no; row += cols) {
        int nrow = no - row < cols ? no - row : cols;
        nk_layout_row_static(ctx, 110, 110, nrow);
        for (int k = row; k < row + nrow; k++) {
            int pose = order[k], src = pose, flip = 0, st = cs_st[pose]; char rp[400]; SDL_Texture *t; struct nk_color edge = nk_rgb(0, 0, 0); int framed = 0;
            if (st == CS_ALIAS) { src = cs_of[pose]; for (int hop = 0; hop < 4 && src >= 0 && cs_st[src] == CS_ALIAS; hop++) src = cs_of[src]; if (src < 0) { nk_label_colored(ctx, "alias ?", NK_TEXT_CENTERED, nk_rgb(90, 95, 105)); continue; } st = cs_st[src]; }
            if (st == CS_GEN) snprintf(rp, sizeof rp, "data/generics/%d/gen/pose_%04d.png", cls, src);
            else if (st == CS_NEEDS || st == CS_MISSING) { snprintf(rp, sizeof rp, "data/generics/%d/needs/pose_%04d.png", cls, src); edge = nk_rgb(230, 70, 60); framed = 1; }
            else { snprintf(rp, sizeof rp, "data/generics/%d/frames/pose_%04d.png", cls, src); if (st == CS_STANDIN) { edge = nk_rgb(240, 160, 40); framed = 1; } else if (st == CS_OWN) { edge = nk_rgb(70, 120, 200); framed = 1; } }
            t = sp_tile_tex(rp, NULL, st == CS_MISSING, flip);   /* a 'needs' frame shows in COLOUR: its fault (a mangled palette) must be visible (user 2026-08-28) */
            if (!t) { nk_label_colored(ctx, st == CS_STANDIN ? "stand-in ?" : "?", NK_TEXT_CENTERED, nk_rgb(150, 150, 160)); continue; }
            if (framed) { nk_style_push_color(ctx, &ctx->style.button.border_color, edge); nk_style_push_float(ctx, &ctx->style.button.border, 3.0f); }
            { struct nk_rect tb = WB(); int hit = nk_button_image(ctx, nk_image_ptr(t));
              if (hit) {               /* the zoom modal over the CLASS: gen/ = the result, the stock / stand-in frame = the original (toggle) */
                  static int nav[700]; for (int q = 0; q < no; q++) nav[q] = order[q];
                  memcpy(sk_all_poses, nav, sizeof(int) * (size_t)no); sk_all_n = no;
                  snprintf(sk_zoom_out_fmt, sizeof sk_zoom_out_fmt, "data/generics/%d/gen/pose_%%04d.png", cls);
                  snprintf(sk_zoom_ref_fmt, sizeof sk_zoom_ref_fmt, "data/generics/%d/%s/pose_%%04d.png", cls, st == CS_NEEDS || st == CS_MISSING ? "needs" : "frames");
                  sk_zoom_cls = cls; sk_zoom_on = 2; sk_zoom_pose = src; sk_zoom_mt = 0;
                  snprintf(sk_zoom_title, sizeof sk_zoom_title, "class %d pose %04d", cls, src);
                  nk_window_show(ctx, "skin zoom", NK_SHOWN); nk_window_set_focus(ctx, "skin zoom");
              }
              if (nk_contextual_begin(ctx, 0, nk_vec2(320, 64), tb)) {   /* right-click: alias override */
                  static int al_for = -1; static char al_buf[8]; static int al_flip;
                  if (al_for != pose) { al_for = pose; al_buf[0] = 0; al_flip = 0; }
                  nk_layout_row_begin(ctx, NK_STATIC, 24, 4);
                  nk_layout_row_push(ctx, 60); nk_labelf(ctx, NK_TEXT_LEFT, "%04d =", pose);
                  nk_layout_row_push(ctx, 70); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, al_buf, sizeof al_buf, nk_filter_decimal);
                  nk_layout_row_push(ctx, 70); nk_checkbox_label(ctx, "flipped", &al_flip);   /* 0267 = 1057 mirrored (user 2026-08-28) */
                  nk_layout_row_push(ctx, 80);
                  if (nk_button_label(ctx, "alias of") && al_buf[0] && atoi(al_buf) != pose) { cs_override_write(cls, pose, atoi(al_buf), al_flip, -1); nk_contextual_close(ctx); }
                  nk_layout_row_end(ctx);
                  nk_layout_row_dynamic(ctx, 24, 1);
                  if (cs_st[pose] == CS_ALIAS) { if (nk_contextual_item_label(ctx, "un-alias (draw it as its own frame)", NK_TEXT_LEFT)) cs_override_write(cls, -1, -1, 0, pose); }
                  else nk_label_colored(ctx, "type the id this frame duplicates, then 'alias of'", NK_TEXT_LEFT, nk_rgb(150, 150, 160));
                  nk_contextual_end(ctx);
              }
              hint_at(tb, st == CS_STANDIN ? "STAND-IN: another body's frame (orange) - to be generated on this class by --class-build. Click = zoom (toggle 'ref' for the original)" :
                          st == CS_NEEDS ? "NEEDS: a mangled/absent stock frame (red) - to be generated" : st == CS_GEN ? "generated from the class base ref" : "OWN: a stock member's frame (blue) - the pose reference, still to convert to the generic"); }
            if (framed) { nk_style_pop_float(ctx); nk_style_pop_color(ctx); }
        }
        nk_layout_row_static(ctx, 14, 110, nrow);
        for (int k = row; k < row + nrow; k++) {
            int pose = order[k], st = cs_st[pose];
            if (st == CS_ALIAS) nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(110, 150, 200), "%04d = %04d", pose, cs_of[pose]);
            else if (st == CS_STANDIN) nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(240, 160, 40), "%04d (w%d)", pose, cs_owner[pose]);
            else nk_labelf_colored(ctx, NK_TEXT_CENTERED, st == CS_NEEDS || st == CS_MISSING ? nk_rgb(255, 120, 90) : nk_rgb(150, 150, 160), "%04d", pose);
        }
    }
}
static void draw_class_poses(int cls)
{
    static unsigned char seen[2048];
    float lr[2] = { 0.216f, 0.784f };
    static Uint32 cs_poll; static int cs_was_busy;
    if (mp_n < 0) mp_load();
    if (cs_loaded != cls) cs_load(cls);
    if (tool_busy && SDL_GetTicks() - cs_poll > 2000) { cs_poll = SDL_GetTicks(); cs_load(cls); }   /* live: gen/ fills up during a build */
    if (cs_was_busy && !tool_busy) cs_load(cls);       /* the run ended: status.json is final */
    cs_was_busy = tool_busy;
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, cs_verdict, NK_TEXT_LEFT, strncmp(cs_verdict, "COMPLETE", 8) ? nk_rgb(240, 180, 60) : nk_rgb(120, 190, 120));
    nk_layout_row(ctx, NK_DYNAMIC, avail_h() - 4, 2, lr);
    rows_begin();
    if (nk_group_begin(ctx, "cs_list", NK_WINDOW_BORDER)) {
        int unique = 0, done = 0, alias = 0, ou = 0, od = 0, oa = 0;
        static int uni[2048]; int nuni = 0;
        for (int id = 0; id < 2048; id++) if (cs_st[id] != CS_NONE) uni[nuni++] = id;   /* the FULL status universe */
        memset(seen, 0, sizeof seen);
        cs_count(uni, nuni, seen, &unique, &done, &alias);
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label_colored(ctx, "CLASS UNIVERSE", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
        cs_row(-1, "everything", done, unique);
        memset(seen, 0, sizeof seen);      /* ids NO animation names (handler-set frames, aisle cells, two-man variants) */
        for (int a = 0; a < mp_n; a++) for (int k = 0; k < mp_anim[a].np[cls]; k++) { int id = mp_anim[a].pose[cls][k]; if (id >= 0 && id < 2048) seen[id] = 1; }
        { static int oth[2048]; int no = 0; for (int id = 0; id < 2048; id++) if (cs_st[id] != CS_NONE && !seen[id]) oth[no++] = id;
          memset(seen, 0, sizeof seen); cs_count(oth, no, seen, &ou, &od, &oa);
          if (ou || oa) cs_row(-3, "other (no animation)", od, ou); }
        if (dd_cls != cls || dd_n < 0) dd_load_pairs(cls);
        { char lb[64]; snprintf(lb, sizeof lb, "duplicates to review (%d)", dd_n > 0 ? dd_n : 0); int sel = cs_sel == -4; struct nk_rect b = WB(); rows_add(-4);
          if (nk_selectable_label(ctx, lb, NK_TEXT_LEFT, &sel) && sel) cs_sel = -4; (void)b; }
        for (int pass = 0; pass < 2; pass++) {
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label_colored(ctx, pass ? "MOVES" : "GENERIC", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            for (int a = 0; a < mp_n; a++) {
                int u = 0, d = 0, al = 0; char lb[80]; mp_anim_t *m = &mp_anim[a];
                if ((pass == 0) != (m->generic != 0)) continue;
                if (!strcmp(m->kind, "other")) continue;   /* the class list has its own 'other (no animation)' row */
                memset(seen, 0, sizeof seen);
                cs_count(m->pose[cls], m->np[cls], seen, &u, &d, &al);
                if (!u && !al) continue;
                if (!strcmp(m->kind, "move")) snprintf(lb, sizeof lb, "  0x%02X %s", m->id, m->name);
                else if (!strncmp(m->name, m->kind, strlen(m->kind))) snprintf(lb, sizeof lb, "  %s", m->name);
                else snprintf(lb, sizeof lb, "  %s %s", m->kind, m->name);
                cs_row(a, lb, d, u);
            }
        }
        rows_nav(&cs_sel, "cs_list");
        nk_group_end(ctx);
    }
    if (nk_group_begin(ctx, "cs_detail", NK_WINDOW_BORDER)) {
        int cols = (int)((0.85f * 0.784f * (float)nk_window_get_width(ctx)) / 118.0f);
        if (cs_sel == -2) { nk_layout_row_dynamic(ctx, 24, 1); nk_label_colored(ctx, "pick a pose set on the left", NK_TEXT_LEFT, nk_rgb(150, 150, 160)); }
        else if (cs_sel == -4) {         /* DUPLICATES review: each near pair side by side, alias or keep both */
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "DUPLICATES - %d near pairs from --class-dedupe (silhouette IoU >= 0.90 or <= 10%% pixels differ). 'alias' = draw once, 'keep both' = they are different poses.", dd_n > 0 ? dd_n : 0);
            if (dd_n <= 0) { nk_layout_row_dynamic(ctx, 24, 1); nk_label_colored(ctx, "none - run Dedupe (left) after a build or a Verify", NK_TEXT_LEFT, nk_rgb(150, 150, 160)); }
            for (int k = 0; k < dd_n; k++) {
                char rp[300]; SDL_Texture *ta, *tb; int a = dd_a[k], b = dd_b[k];
                nk_layout_row_begin(ctx, NK_STATIC, 110, 5);
                snprintf(rp, sizeof rp, "data/generics/%d/%s/pose_%04d.png", cls, cs_st[a] == CS_GEN ? "gen" : "frames", a);
                ta = sp_tile_tex(rp, NULL, 0, 0);
                snprintf(rp, sizeof rp, "data/generics/%d/%s/pose_%04d.png", cls, cs_st[b] == CS_GEN ? "gen" : "frames", b);
                tb = sp_tile_tex(rp, NULL, 0, dd_flip[k]);   /* b shown mirrored when the match is a flip */
                for (int side = 0; side < 2; side++) {   /* click a tile: the zoom modal, prev/next = the other one (inspect at size) */
                    SDL_Texture *t = side ? tb : ta; int id = side ? b : a;
                    nk_layout_row_push(ctx, 110);
                    if (!t) { nk_label(ctx, "?", NK_TEXT_CENTERED); continue; }
                    { struct nk_rect tb2 = WB(); int hit = nk_button_image(ctx, nk_image_ptr(t));
                      hint_at(tb2, "click = zoom; prev / next in the modal flips between the two; mouse wheel zooms");
                      if (hit) {
                          sk_all_poses[0] = a; sk_all_poses[1] = b; sk_all_n = 2;
                          snprintf(sk_zoom_out_fmt, sizeof sk_zoom_out_fmt, "data/generics/%d/gen/pose_%%04d.png", cls);
                          snprintf(sk_zoom_ref_fmt, sizeof sk_zoom_ref_fmt, "data/generics/%d/frames/pose_%%04d.png", cls);
                          sk_zoom_cls = cls; sk_zoom_on = 2; sk_zoom_pose = id; sk_zoom_mt = 0;
                          snprintf(sk_zoom_title, sizeof sk_zoom_title, "class %d pose %04d (pair %04d / %04d)", cls, id, a, b);
                          nk_window_show(ctx, "skin zoom", NK_SHOWN); nk_window_set_focus(ctx, "skin zoom");
                      } }
                }
                nk_layout_row_push(ctx, 300);
                if (nk_group_begin(ctx, rp, NK_WINDOW_NO_SCROLLBAR)) {
                    nk_layout_row_dynamic(ctx, 20, 1);
                    nk_labelf(ctx, NK_TEXT_LEFT, "%04d  vs  %04d%s", a, b, dd_flip[k] ? "  (mirrored)" : "");
                    nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "silhouette IoU %.3f   %.1f%% pixels differ", dd_iou[k], dd_pct[k]);
                    nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "%s / %s", cs_st[a] == CS_ALIAS ? "already alias" : "own", cs_st[b] == CS_ALIAS ? "already alias" : "own");
                    nk_group_end(ctx);
                }
                nk_layout_row_push(ctx, 150);
                if (nk_button_label(ctx, "alias (draw once)")) { int tgt = b, fl = dd_flip[k]; for (int hop = 0; hop < 6 && cs_st[tgt] == CS_ALIAS && cs_of[tgt] >= 0; hop++) tgt = cs_of[tgt];   /* a group of 3: alias to the ROOT */
                    cs_override_write(cls, a, tgt, fl, -1); for (int q = k; q < dd_n - 1; q++) { dd_a[q] = dd_a[q+1]; dd_b[q] = dd_b[q+1]; dd_flip[q] = dd_flip[q+1]; dd_iou[q] = dd_iou[q+1]; dd_pct[q] = dd_pct[q+1]; } dd_n--; }
                nk_layout_row_push(ctx, 120);
                if (nk_button_label(ctx, "keep both")) { cs_override_write2(cls, -1, -1, 0, -1, a, b); for (int q = k; q < dd_n - 1; q++) { dd_a[q] = dd_a[q+1]; dd_b[q] = dd_b[q+1]; dd_flip[q] = dd_flip[q+1]; dd_iou[q] = dd_iou[q+1]; dd_pct[q] = dd_pct[q+1]; } dd_n--; }
                nk_layout_row_end(ctx);
            }
        }
        else {
            int u = 0, d = 0, al = 0; const char *title; static int all_ids[2048]; int nall = 0; static char tt[120];
            memset(seen, 0, sizeof seen);
            if (cs_sel == -1) { title = "CLASS UNIVERSE - every id of the class";
                for (int id = 0; id < 2048; id++) if (cs_st[id] != CS_NONE) all_ids[nall++] = id;
                cs_count(all_ids, nall, seen, &u, &d, &al); }
            else if (cs_sel == -3) { title = "OTHER - ids no animation names (handler-set, aisle, two-man variants)";
                static unsigned char named[2048]; memset(named, 0, sizeof named);
                for (int a = 0; a < mp_n; a++) for (int k = 0; k < mp_anim[a].np[cls]; k++) { int id = mp_anim[a].pose[cls][k]; if (id >= 0 && id < 2048) named[id] = 1; }
                for (int id = 0; id < 2048; id++) if (cs_st[id] != CS_NONE && !named[id]) all_ids[nall++] = id;
                cs_count(all_ids, nall, seen, &u, &d, &al); }
            else if (cs_sel >= 0 && cs_sel < mp_n) { mp_anim_t *m = &mp_anim[cs_sel]; snprintf(tt, sizeof tt, "%s %s%02X  %s", m->kind, strcmp(m->kind, "move") ? "" : "0x", m->id, m->name); title = tt; cs_count(m->pose[cls], m->np[cls], seen, &u, &d, &al); }
            else title = "";
            nk_layout_row_begin(ctx, NK_STATIC, 24, 6);
            nk_layout_row_push(ctx, 330); nk_label_colored(ctx, title, NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            nk_layout_row_push(ctx, 420); nk_labelf_colored(ctx, NK_TEXT_LEFT, d == u ? nk_rgb(120, 190, 120) : nk_rgb(200, 200, 210), "%d / %d generic   (%d aliases; blue = stock, orange = stand-in, red = needs)", d, u, al);
            nk_layout_row_push(ctx, 70);
            {   /* sheet size for the build (user 2026-08-28): 4x4 / 3x3 / 2x2 / 1x1 -> WF_ART_GRID */
                static const char *grids[4] = { "4x4", "3x3", "2x2", "1x1" }; struct nk_rect gb = WB();
                cs_grid_sel = nk_combo(ctx, grids, 4, cs_grid_sel, 22, nk_vec2(90, 130));
                hint_at(gb, "Poses per codex call: 4x4 = 256 px per figure, 3x3 = 341 px (~50 calls for a class), 2x2 = 512 px, 1x1 = one pose per call at 1024 px (best adherence, ~470 calls). Lying/low figures always go 2x2 on a sheet run.");
            }
            nk_layout_row_push(ctx, 110);
            if (d < u) {               /* TEMPORARY (user 2026-08-28): runs the standalone --class-build
                                          on this set's stand-in/needs ids (codex) as a tool subprocess */
                if (sk_button_gated("Generate", !tool_busy, "a tool is running")) {
                    char ids[3000] = ""; size_t o = 0; const int *src = cs_sel < 0 ? all_ids : mp_anim[cs_sel].pose[cls]; int ns = cs_sel < 0 ? nall : mp_anim[cs_sel].np[cls];
                    for (int k = 0; k < ns && o < sizeof ids - 8; k++) { int id = src[k]; if (id >= 0 && id < 2048 && (cs_st[id] == CS_OWN || cs_st[id] == CS_STANDIN || cs_st[id] == CS_NEEDS || cs_st[id] == CS_MISSING)) o += (size_t)snprintf(ids + o, sizeof ids - o, "%s%d", o ? "," : "", id); }
                    char cbcmd[3200];      /* NOT tool_cmd: run_tool rewrites that buffer from its
                                              argument (aliasing gave an empty command = instant exit 0) */
                    snprintf(cbcmd, sizeof cbcmd, "WF_ART_GRID=%d ./wfengine --class-build %d %s", 4 - cs_grid_sel, cls, cs_sel == -1 ? "all" : ids);
                    logf_("class-build %d: %s", cls, cs_sel == -1 ? "every stand-in / needs id" : ids);
                    run_tool(cbcmd); cs_loaded = -1;
                }
            } else { struct nk_rect tb = WB(); nk_label(ctx, "", NK_TEXT_LEFT); tick_icon(tb); }
            nk_layout_row_push(ctx, 90);
            {   /* CANCEL next to Generate (user 2026-08-28): kills the tool group - codex rides in it */
                if (tool_busy) {
                    struct nk_style_button warn = ctx->style.button;
                    warn.normal = warn.hover = warn.active = nk_style_item_color(nk_rgb(120, 30, 30));
                    if (nk_button_label_styled(ctx, &warn, "Cancel")) { wf_editor_kill_tools(); logline("class build cancelled (frames already in gen/ stay; Generate resumes)"); cs_loaded = -1; }
                } else if (sk_button_gated("Cancel", 0, "nothing is running")) { }
            }
            nk_layout_row_push(ctx, 140); nk_checkbox_label(ctx, "show aliases", &sp_show_alias);
            nk_layout_row_end(ctx);
            if (tool_busy || cs_log_keep) {   /* the tool's output while a build runs (user 2026-08-28: only '[exit 0]' was visible) */
                static int seen_n = -1;
                nk_layout_row_dynamic(ctx, 70, 1);
                if (nk_group_begin(ctx, "cs_log", NK_WINDOW_BORDER)) {
                    SDL_LockMutex(log_mx);
                    nk_layout_row_dynamic(ctx, 15, 1);
                    for (int i = 0; i < log_n; i++) { int k = (log_head - log_n + i + 64) % 64; nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(170, 170, 190), "%s", logbuf[k]); }
                    if (seen_n != log_head) { seen_n = log_head; nk_group_set_scroll(ctx, "cs_log", 0, 100000); }
                    SDL_UnlockMutex(log_mx);
                    nk_group_end(ctx);
                }
                if (tool_busy) cs_log_keep = 1;
            }
            if (cs_sel == -1 || cs_sel == -3) cs_grid(cls, "class", cs_sel, all_ids, nall, cols);
            else cs_grid(cls, mp_anim[cs_sel].kind, mp_anim[cs_sel].id, mp_anim[cs_sel].pose[cls], mp_anim[cs_sel].np[cls], cols);
        }
        nk_group_end(ctx);
    }
}
static int cl_sub = 2;              /* Classes sub-tab: 2 Info (first, user 2026-08-30), 0 Poses (viewer), 1 Aliases (the review) */

static void draw_classes(void)
{
    char path[300];
    if (cl_loaded != cl_sel) cl_load(cl_sel);
    nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
    nk_layout_row_push(ctx, ed_tab_w("Info")); if (ed_tab("Info", cl_sub == 2)) cl_sub = 2;
    nk_layout_row_push(ctx, ed_tab_w("Poses")); if (ed_tab("Poses", cl_sub == 0)) cl_sub = 0;
    if (getenv("WF_EDITOR_ALIASES")) { nk_layout_row_push(ctx, ed_tab_w("Aliases")); if (ed_tab("Aliases", cl_sub == 1)) cl_sub = 1; }   /* the review sheets: hidden (user 2026-08-28: "a bit of a mess") */
    nk_layout_row_end(ctx);
    if (cl_sub == 0) { heading(cl_names[cl_sel], NULL); draw_class_poses(cl_sel); return; }
    if (cl_sub == 2) {                 /* INFO: the class reference + its data */
        static const char *const members[ENG_BODY_CLASSES] = { "Hulk Hogan, Ultimate Warrior, L.O.D. Hawk, L.O.D. Animal", "Jake Roberts, Ted DiBiase, Demolition Smash, Demolition Crush", "Mr. Perfect", "Big Boss Man, Sgt. Slaughter", "Earthquake" };
        static const char *const bbox[ENG_BODY_CLASSES] = { "54x106", "57x104", "48x99", "53x104", "56x106" };
        char path[300], txt[1200] = ""; FILE *tf; int w = 100, h = 300;
        if (cs_loaded != cl_sel) cs_load(cl_sel);
        heading(cl_names[cl_sel], NULL);
        snprintf(path, sizeof path, "data/generics/%d/base_ref.png", cl_sel);
        sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1;
        sk_png_tex(path, &cl_tex_ref, &cl_mt_ref);
        if (cl_tex_ref) { SDL_QueryTexture(cl_tex_ref, NULL, NULL, &w, &h); w = w * 300 / (h ? h : 1); }
        snprintf(path, sizeof path, "data/generics/%d/character.txt", cl_sel);
        if (access(path, R_OK) != 0) snprintf(path, sizeof path, "data/generics/class%d.txt", cl_sel);
        if (access(path, R_OK) != 0) snprintf(path, sizeof path, "data/generics/generic.txt");
        tf = fopen(path, "r"); if (tf) { size_t n = fread(txt, 1, sizeof txt - 1, tf); txt[n] = 0; fclose(tf); while (n && (txt[n-1] == '\n' || txt[n-1] == '\r')) txt[--n] = 0; }
        nk_layout_row_begin(ctx, NK_STATIC, 300, 2);
        nk_layout_row_push(ctx, (float)w + 8);
        if (cl_tex_ref) nk_image(ctx, nk_image_ptr(cl_tex_ref)); else nk_label(ctx, "(no base ref)", NK_TEXT_CENTERED);
        nk_layout_row_push(ctx, 900);
        if (nk_group_begin(ctx, "cl_info", NK_WINDOW_NO_SCROLLBAR)) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(240, 180, 60), "class %d  %s", cl_sel, cl_names[cl_sel]);
            nk_labelf(ctx, NK_TEXT_LEFT, "members: %s", members[cl_sel]);
            nk_labelf(ctx, NK_TEXT_LEFT, "representative: %s   standing box: %s px", ws_name(cls_rep[cl_sel]), bbox[cl_sel]);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, strncmp(cs_verdict, "COMPLETE", 8) ? nk_rgb(240, 180, 60) : nk_rgb(120, 190, 120), "template: %s", cs_verdict);
            nk_labelf(ctx, NK_TEXT_LEFT, "reference: data/generics/%d/base_ref.png   (the identity anchor every frame of the template is drawn from)", cl_sel);
            nk_labelf(ctx, NK_TEXT_LEFT, "description: %s", path);
            nk_layout_row_dynamic(ctx, 120, 1);
            nk_label_colored_wrap(ctx, txt, nk_rgb(200, 200, 210));
            nk_group_end(ctx);
        }
        nk_layout_row_end(ctx);
        nk_layout_row_dynamic(ctx, 60, 1);
        nk_label_colored_wrap(ctx, cl_summary, nk_rgb(150, 150, 160));
        return;
    }
    heading(cl_names[cl_sel], cl_summary);
    {
        int w = 100, h = 150;
        snprintf(path, sizeof path, "data/generics/%d/base_ref.png", cl_sel);
        sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1;
        sk_png_tex(path, &cl_tex_ref, &cl_mt_ref);
        if (cl_tex_ref) { SDL_QueryTexture(cl_tex_ref, NULL, NULL, &w, &h); w = w * 150 / (h ? h : 1); }
        nk_layout_row_begin(ctx, NK_STATIC, 150, 2);
        nk_layout_row_push(ctx, (float)w + 8);
        if (cl_tex_ref) nk_image(ctx, nk_image_ptr(cl_tex_ref)); else nk_label(ctx, "(no base ref)", NK_TEXT_CENTERED);
        nk_layout_row_push(ctx, nk_window_get_width(ctx) * 0.76f - 80 - (float)w);
        nk_label_colored_wrap(ctx, cl_needs[0] ? cl_needs : "needs.json missing", nk_rgb(200, 200, 210));
        nk_layout_row_end(ctx);
    }
    /* alias review: 3 sheets per page */
    {
        int pages = (cl_ng + 2) / 3, ngroups = 0;
        for (int g = 0; g < cl_ng; g++) ngroups += !cl_g_kind[g];
        if (cl_page >= pages) cl_page = pages ? pages - 1 : 0;
        cl_rejbuf_sync();
        {
            float r[5] = { 0.08f, 0.08f, 0.44f, 0.20f, 0.20f };
            nk_layout_row(ctx, NK_DYNAMIC, 26, 5, r);
            if (nk_button_label(ctx, "< prev") && cl_page > 0) { cl_rejbuf_apply(); cl_page--; }
            if (nk_button_label(ctx, "next >") && cl_page + 1 < pages) { cl_rejbuf_apply(); cl_page++; }
            nk_labelf(ctx, NK_TEXT_LEFT, "alias review page %d/%d  (%d groups, %d suggested near-merges)", pages ? cl_page + 1 : 0, pages, ngroups, cl_ng - ngroups);
            if (nk_button_label(ctx, "jump to suggestions")) { cl_rejbuf_apply(); for (int g = 0; g < cl_ng; g++) if (cl_g_kind[g]) { cl_page = g / 3; break; } }
            if (nk_button_label(ctx, "Save override")) cl_save_override();
        }
        for (int i = 0; i < 3; i++) {
            int g = cl_page * 3 + i, mem[256], n, w = 1000, h = 200; char line[400]; size_t o;
            if (g >= cl_ng) break;
            n = cl_members(g, mem, 256);
            o = (size_t)snprintf(line, sizeof line, "%s - source %d: %d member(s) ", cl_g_kind[g] ? "SUGGESTED near merge" : "alias group", cl_g_of[g], n);
            for (int k = 0; k < n && k < 12 && o < sizeof line - 8; k++) o += (size_t)snprintf(line + o, sizeof line - o, "%d ", mem[k]);
            if (n > 12) snprintf(line + o, sizeof line - o, "...");
            {
                float r[3] = { 0.55f, 0.15f, 0.30f };
                nk_layout_row(ctx, NK_DYNAMIC, 24, 3, r);
                nk_label(ctx, line, NK_TEXT_LEFT);
                if (cl_g_kind[g]) {
                    int on = 1; for (int k = 0; k < n; k++) if (mem[k] < 2048 && !cl_add[mem[k]]) on = 0;
                    { int was = on; nk_checkbox_label(ctx, "accept: one drawing", &on);
                      if (on != was) for (int k = 0; k < n; k++) if (mem[k] < 2048) cl_add[mem[k]] = (unsigned char)on; }
                    nk_label(ctx, "", NK_TEXT_LEFT);
                } else {
                    nk_label(ctx, "not the same drawing:", NK_TEXT_RIGHT);
                    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, cl_rejbuf[i], sizeof cl_rejbuf[i], nk_filter_default);
                }
            }
            snprintf(path, sizeof path, "data/generics/%d/review/%s_%04d.png", cl_sel, cl_g_kind[g] ? "suggest" : "group", cl_g_of[g]);
            sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1;
            sk_png_tex(path, &cl_tex_sheet[i], &cl_mt_sheet[i]);
            if (cl_tex_sheet[i]) {
                float avail = nk_window_get_width(ctx) * 0.76f - 48, fw, fh;   /* the sheets are 2x already: native size, shrunk only when too wide */
                SDL_QueryTexture(cl_tex_sheet[i], NULL, NULL, &w, &h);
                fw = (float)w; fh = (float)h;
                if (fw > avail) { fh = fh * avail / fw; fw = avail; }
                nk_layout_row_static(ctx, fh, (int)fw, 1);
                nk_image(ctx, nk_image_ptr(cl_tex_sheet[i]));
            } else note("(no review sheet - Rebuild generic writes them)");
        }
    }
    sk_png_plain = 0;
}

/* ------------------------------------------------------- tools panel */
static void tool_row(const char *label, const char *cmd, const char *what)
{
    float tr[2] = { 0.24f, 0.76f };
    nk_layout_row(ctx, NK_DYNAMIC, 28, 2, tr);
    if (nk_button_label(ctx, label)) run_tool(cmd);
    nk_label_colored(ctx, what, NK_TEXT_LEFT, nk_rgb(165, 165, 175));
}

static void draw_tools(void)
{
    heading("Data pipeline tools", "Each button runs the corresponding C tool as a subprocess of this binary; its output streams into the log below.");
    /* modder's tools only (user 2026-08-27): the dev rows (export-all,
       export-stock, verify-gfx, list tables, regress, cov drives, git)
       stay CLI verbs - see docs/data-pipeline.md */
    tool_row("Pack", pack_cmd(),
             "Rebuild this profile's paks from its JSON/PNG trees (stock: the base paks). The header Save does this too.");
    {   char vp[200];
        snprintf(vp, sizeof vp, "./wfengine --verify-profile %s", wf_profile()[0] ? wf_profile() : "stock");
        tool_row("Verify profile", vp,
                 "Render the 12 stock men in this profile through grapple / drag / pin / throw / weapon scenarios and compare with the ROM render (PASS <= 64 px).");
    }
    tool_row("Frame coverage", "./wfengine --frame-coverage data/framecoverage.json",
             "Which wrestler owns art for which move (the matrix behind the move map's green/amber cells).");
    {
        float tr[2] = { 0.24f, 0.76f };
        nk_layout_row(ctx, NK_DYNAMIC, 28, 2, tr);
        if (nk_button_label(ctx, "Clear log")) { log_n = 0; log_head = 0; }
        nk_labelf_colored(ctx, NK_TEXT_LEFT, tool_busy ? nk_rgb(240, 180, 60) : nk_rgb(120, 190, 120),
                          tool_busy ? "running %c" : "idle%.0s",
                          tool_busy ? "|/-\\"[(SDL_GetTicks() / 250) & 3] : 0);
    }
    nk_layout_row_dynamic(ctx, avail_h(), 1);
    if (nk_group_begin(ctx, "log", NK_WINDOW_BORDER)) {
        SDL_LockMutex(log_mx);
        for (int i = 0; i < log_n; i++) {
            int k = (log_head - log_n + i + 64) % 64;
            nk_layout_row_dynamic(ctx, 16, 1);
            nk_label(ctx, logbuf[k], NK_TEXT_LEFT);
        }
        SDL_UnlockMutex(log_mx);
        nk_group_end(ctx);
    }
}

/* --------------------------------------------------------- style */
/* Dark slate + gold accent so the editor stops looking like raw
 * default-grey Nuklear. */
static void apply_style(void)
{
    struct nk_color tbl[NK_COLOR_COUNT];
    tbl[NK_COLOR_TEXT]                    = nk_rgb(214, 214, 220);
    tbl[NK_COLOR_WINDOW]                  = nk_rgb(28, 30, 36);
    tbl[NK_COLOR_HEADER]                  = nk_rgb(38, 40, 48);
    tbl[NK_COLOR_BORDER]                  = nk_rgb(62, 66, 78);
    tbl[NK_COLOR_BUTTON]                  = nk_rgb(52, 56, 68);
    tbl[NK_COLOR_BUTTON_HOVER]            = nk_rgb(72, 78, 94);
    tbl[NK_COLOR_BUTTON_ACTIVE]           = nk_rgb(200, 140, 40);
    tbl[NK_COLOR_TOGGLE]                  = nk_rgb(48, 52, 62);
    tbl[NK_COLOR_TOGGLE_HOVER]            = nk_rgb(70, 76, 90);
    tbl[NK_COLOR_TOGGLE_CURSOR]           = nk_rgb(240, 180, 60);
    tbl[NK_COLOR_SELECT]                  = nk_rgb(40, 43, 52);
    tbl[NK_COLOR_SELECT_ACTIVE]           = nk_rgb(120, 84, 24);
    tbl[NK_COLOR_SLIDER]                  = nk_rgb(44, 47, 56);
    tbl[NK_COLOR_SLIDER_CURSOR]           = nk_rgb(240, 180, 60);
    tbl[NK_COLOR_SLIDER_CURSOR_HOVER]     = nk_rgb(255, 200, 90);
    tbl[NK_COLOR_SLIDER_CURSOR_ACTIVE]    = nk_rgb(255, 210, 110);
    tbl[NK_COLOR_PROPERTY]                = nk_rgb(44, 47, 56);
    tbl[NK_COLOR_EDIT]                    = nk_rgb(40, 43, 52);
    tbl[NK_COLOR_EDIT_CURSOR]             = nk_rgb(240, 180, 60);
    tbl[NK_COLOR_COMBO]                   = nk_rgb(44, 47, 56);
    tbl[NK_COLOR_CHART]                   = nk_rgb(44, 47, 56);
    tbl[NK_COLOR_CHART_COLOR]             = nk_rgb(240, 180, 60);
    tbl[NK_COLOR_CHART_COLOR_HIGHLIGHT]   = nk_rgb(255, 0, 0);
    tbl[NK_COLOR_SCROLLBAR]               = nk_rgb(34, 36, 44);
    tbl[NK_COLOR_SCROLLBAR_CURSOR]        = nk_rgb(72, 78, 94);
    tbl[NK_COLOR_SCROLLBAR_CURSOR_HOVER]  = nk_rgb(96, 104, 124);
    tbl[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(240, 180, 60);
    tbl[NK_COLOR_TAB_HEADER]              = nk_rgb(38, 40, 48);
    tbl[NK_COLOR_KNOB]                    = nk_rgb(44, 47, 56);
    tbl[NK_COLOR_KNOB_CURSOR]             = nk_rgb(240, 180, 60);
    tbl[NK_COLOR_KNOB_CURSOR_HOVER]       = nk_rgb(255, 200, 90);
    tbl[NK_COLOR_KNOB_CURSOR_ACTIVE]      = nk_rgb(255, 210, 110);
    nk_style_from_table(ctx, tbl);
    ctx->style.button.rounding = 4;
    ctx->style.property.rounding = 3;
    ctx->style.window.spacing = nk_vec2(6, 5);
    ctx->style.window.padding = nk_vec2(8, 6);
    ctx->style.window.group_padding = nk_vec2(8, 6);
    /* the SELECTED nav tab / list row reads gold, not just a darker box */
    ctx->style.selectable.text_normal_active = nk_rgb(255, 205, 100);
    ctx->style.selectable.text_hover_active  = nk_rgb(255, 215, 120);
    ctx->style.selectable.text_pressed_active = nk_rgb(255, 220, 130);
    ctx->style.selectable.rounding = 3;
    ctx->style.edit.rounding = 3;
    ctx->style.scrollv.rounding = 3;
}

/* ------------------------------------------------------------ public */
int ed_open(void)
{
    struct nk_font_atlas *atlas;
    int ww = 1400, wh = 900;
    SDL_Rect ub;
    if (opened) return 0;
    /* fill the desktop work area ("full-screen" editor, still a window
     * so the game window stays visible on top / another monitor) */
    if (SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.w > 400 && ub.h > 300) { ww = ub.w; wh = ub.h; }
    win = SDL_CreateWindow("wfeditor", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, wh,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
    if (!win) { fprintf(stderr, "editor: %s\n", SDL_GetError()); return -1; }
    wf_window_dress((void *)win);      /* taskbar icon + raise to the FOREGROUND
                                          (the WM parked a fresh editor behind
                                          everything - user 2026-08-28) */
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);          /* software (dummy driver, VMs) */
    if (!ren) { fprintf(stderr, "editor: %s\n", SDL_GetError()); return -1; }
    ctx = nk_sdl_init(win, ren);
    nk_sdl_font_stash_begin(&atlas);
    {
        struct nk_font *f = nk_font_atlas_add_from_file(atlas, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 15, 0);
        if (!f) f = nk_font_atlas_add_from_file(atlas, "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 14, 0);
        if (!f) f = nk_font_atlas_add_default(atlas, 14, 0);
        nk_sdl_font_stash_end();
        if (f) nk_style_set_font(ctx, &f->handle);
    }
    apply_style();
    log_mx = SDL_CreateMutex();
    read_mods();
    opened = 1;
    nk_input_begin(ctx);
    logf_("wfeditor V%s - %d tables, backend %s", WF_VERSION_STRING, tbl_count(), tbl_backend());
    return 0;
}
void ed_close(void)
{
    if (!opened) return;
    if (pose_tex) SDL_DestroyTexture(pose_tex);
    nk_sdl_shutdown();
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    opened = 0;
}
int ed_is_open(void) { return opened; }
int ed_game_paused(void) { return paused; }
int ed_consume_step(void) { int s = step_once; step_once = 0; return s; }
int ed_consume_back(void) { int s = step_back; step_back = 0; return s; }
/* the game's keys are blocked only while TYPING in the editor (a text
 * field active) — not merely because the editor window has focus, so the
 * pad works straight after Launch without clicking the game window
 * (user 2026-08-25: "lost my controls") */
int ed_wants_keyboard(void) { return opened && focused && ctx && nk_item_is_any_active(ctx); }
static int game_live;
void ed_set_game_live(int on) { game_live = on; }

void ed_handle_event(const SDL_Event *ev)
{
    if (!opened) return;
    if (ev->type == SDL_WINDOWEVENT && ev->window.windowID == SDL_GetWindowID(win)) {
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = 1;
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = 0;
        if (ev->window.event == SDL_WINDOWEVENT_CLOSE) ed_close();
    }
    if (ev->type == SDL_KEYDOWN && focused && ev->key.keysym.sym == SDLK_ESCAPE && pose_zoom_on) { pose_zoom_on = 0; return; }
    if (ev->type == SDL_KEYDOWN && focused && rebind_p >= 0) {   /* Input panel: take the next key */
        if (ev->key.keysym.sym != SDLK_ESCAPE) {
            keymap_set(rebind_p, rebind_b, ev->key.keysym.scancode);
            keymap_dirty = 1;
            logf_("keymap: %s = %s", keymap_entry_name(rebind_p, rebind_b),
                  keymap_keyname(rebind_p, rebind_b));
        }
        rebind_p = rebind_b = -1;
        return;
    }
    if (ev->type == SDL_KEYDOWN && focused && ev->key.keysym.sym == SDLK_p && !(ev->key.keysym.mod & KMOD_CTRL)) {
        /* P toggles pause only when no text field is active; nk tells us via nk_item_is_any_active */
        if (!nk_item_is_any_active(ctx)) { paused = !paused; return; }
    }
    if (ev->type == SDL_KEYDOWN && focused && (ev->key.keysym.sym == SDLK_UP || ev->key.keysym.sym == SDLK_DOWN) && !(ev->key.keysym.mod & KMOD_CTRL))
        list_nav = ev->key.keysym.sym == SDLK_UP ? -1 : 1;   /* browse the Poses lists (user 2026-08-28) */
    if (ev->type == SDL_KEYDOWN && focused && ev->key.keysym.sym == SDLK_F11) {
        /* F11: true borderless fullscreen on/off */
        Uint32 fl = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(win, fl ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    nk_sdl_handle_event((SDL_Event *)ev);
}

void ed_frame(eng_state *st)
{
    {   /* the WM maps the window a few frames in - raise THEN, or the editor
           opens behind everything (user 2026-08-28; same trick as the game
           window's frame-10 raise) */
        static int raise_t;
        if (raise_t <= 22 && ++raise_t == 22 && win) {
            SDL_RaiseWindow(win);
            SDL_SetWindowInputFocus(win);
        }
    }
    int w, h;
    if (!opened) return;
    if (pend_fg_on && pend_fg_frames++ >= 1) {       /* the sheet was drawn last frame: save now */
        fg_create(0); pend_fg_on = 0;
    }
    if (pend_prof_on && pend_prof_frames++ >= 1) {   /* the sheet was drawn last frame: switch now */
        switch_edit_profile(pend_prof); pend_prof_on = 0;
    }
    if (!nav_env_applied) {               /* WF_EDITOR_NAV=n: start on a panel (screenshots) */
        nav_env_applied = 1;
        if (wf_profile()[0] && !mod_layer[0] && !pend_prof_on)   /* --profile P: that profile is the EDIT target from the
                                                                   first frame (it used to show "save: (read-only)" until
                                                                   the header dropdown was touched - "save is greyed out") */
            request_edit_profile(wf_profile());
        if (getenv("WF_EDITOR_NAV")) { const char *e = getenv("WF_EDITOR_NAV"); nav = atoi(e);   /* a number, or a tab NAME */
            for (int i = 0; i < NAV_N; i++) if (!strcasecmp(e, nav_names[i])) nav = i;
            if (nav < 0 || nav >= NAV_N) nav = NAV_ENGINE; if (nav == NAV_TABLES) select_table(tbl_id("hud_slot_base")); if (nav == NAV_RULES) select_table(tbl_id("ringout_rules")); }
        if (getenv("WF_EDITOR_TBL")) select_table(tbl_id(getenv("WF_EDITOR_TBL")));   /* screenshots */
        if (getenv("WF_EDITOR_SKIN")) {        /* screenshots: open a skin on the skins tab */
            sk_scan();
            for (int i = 0; i < sk_nlist; i++)
                if (!strcmp(sk_list[i], getenv("WF_EDITOR_SKIN")))
                    { sk_open = 1; sk_sel = i; sk_load(sk_list[i]); }
        }
        if (getenv("WF_EDITOR_POPUP")) {           /* screenshots: open a profile's settings */
            read_profiles(); read_mods();
            for (int i = 0; i < n_profs; i++)
                if (!strcmp(prof_list[i], getenv("WF_EDITOR_POPUP")))
                    { cur_prof = i; load_profile_edit(i); ps_load(); prof_popup = 1; pf_loaded = i; }
            if (getenv("WF_EDITOR_LAYER") && cur_prof >= 0)   /* layer dialog instead */
                { prof_popup = 0; layer_popup = 1; }
        }
        if (getenv("WF_FORGE")) {   /* headless forge: "slot,base,NAME[,cat.col=move]..."
                                       drives fg_create() (the Create button's code) with a
                                       synchronous pack; run with --editor --profile <p> */
            char spec[256], *tok, *sp = NULL;
            snprintf(spec, sizeof spec, "%s", getenv("WF_FORGE"));
            if ((tok = strtok_r(spec, ",", &sp))) fg_slot = atoi(tok);
            if ((tok = strtok_r(NULL, ",", &sp))) fg_base = atoi(tok);
            if ((tok = strtok_r(NULL, ",", &sp))) snprintf(fg_name, sizeof fg_name, "%s", tok);
            if (fg_slot < ENG_WS_MAX || fg_slot >= ENG_WS_EXT_MAX) fg_slot = ENG_WS_MAX;
            if ((unsigned)fg_base >= (unsigned)ENG_WS_MAX) fg_base = 0;
            for (char *p = fg_name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
            fg_load_base();
            while ((tok = strtok_r(NULL, ",", &sp))) {
                int c, col, v;
                if (sscanf(tok, "%d.%d=%d", &c, &col, &v) == 3 && c >= 0 && c < FG_NCAT && col >= 0 && col < 3)
                    fg_map[c][col] = v & 0xFF;
            }
            fg_default_mod();
            fg_create(1);
        }
    }
    nk_input_end(ctx);
    SDL_GetWindowSize(win, &w, &h);
    {   /* window title = "wfeditor Vnnn - <profile>" (user 2026-08-28) */
        static char last_title[160]; char title[160];
        snprintf(title, sizeof title, "wfeditor V%s - %s", WF_VERSION_STRING, wf_profile()[0] ? wf_profile() : "stock");
        if (strcmp(title, last_title)) { snprintf(last_title, sizeof last_title, "%s", title); SDL_SetWindowTitle(win, title); }
    }
    if (nk_begin(ctx, "wfeditor", nk_rect(0, 0, (float)w, (float)h), NK_WINDOW_NO_SCROLLBAR)) {
        float ratio[2] = { 0.15f, 0.85f };   /* half the old split (user 2026-08-28: the list frame was too wide) */
        /* ---- header bar: title / nav tabs / game controls ---- */
        {
            /* STATIC row: 'Profile:' | dropdown | the visible tabs at TEXT
               width | PAUSE < > (or 'no live game' + 2 blanks) | rest */
            struct nk_rect b;
            /* the tabs start over the RIGHT frame (user 2026-08-28): a
               spacer after the dropdown pads out to the 0.20 split */
            float sp = ctx->style.window.spacing.x;
            float ddw = 0.15f * (float)w - 60 - 3 * sp;   /* the dropdown fills the left frame's width */
            if (ddw < 110) ddw = 110;
            float gap = 4;
            float used = 60 + ddw + gap + sp + 90 + 30 + 30;
            nk_layout_row_begin(ctx, NK_STATIC, 30, 3 + (NAV_N - 3) + 3 + 1);
            nk_layout_row_push(ctx, 60);
            nk_label(ctx, "Profile:", NK_TEXT_RIGHT);
            nk_layout_row_push(ctx, ddw);
            {
                const char *items[18]; int n_items = 1, sel = 0, was;
                if (pf_loaded == -2) { read_profiles(); read_mods(); load_profile_edit(-1); }
                items[0] = "stock (read-only)";
                for (int i = 0; i < n_profs && n_items < 17; i++) {
                    items[n_items] = prof_list[i];
                    if (!strcmp(wf_profile(), prof_list[i])) sel = n_items;
                    n_items++;
                }
                was = sel;
                b = WB();
                sel = nk_combo(ctx, items, n_items, sel, 22, nk_vec2(240, 300));
                hint_at(b, "Which profile this editor shows and edits. Stock is read-only.");
                if (sel != was) request_edit_profile(sel == 0 ? "" : items[sel]);
            }
            nk_layout_row_push(ctx, gap); nk_label(ctx, "", NK_TEXT_LEFT);   /* spacer to the right frame */
            /* display order (user 2026-08-27): Tables = the deep-dive editor, last before Tools;
               Forge merged into Wrestlers; Match = engine debugging only (WF_EDITOR_NAV=Match) */
            /* Tables is a SUB-TAB of Rules now (user 2026-08-28: Rules ->
               Ruleset | Tables), so it hides from the row and lights Rules */
            static const int nav_order[NAV_N] = { NAV_MODS, NAV_RULES, NAV_WRESTLERS, NAV_SKINS, NAV_CLASSES,
                                                  NAV_WEAPONS, NAV_ARENAS, NAV_SCENES, NAV_CALIB, NAV_SOUNDS, NAV_INPUT, NAV_TOOLS, NAV_TABLES, NAV_FORGE, NAV_ENGINE };
            for (int oi = 0; oi < NAV_N; oi++) {
                int i = nav_order[oi];
                if (i == NAV_FORGE || i == NAV_ENGINE || i == NAV_TABLES) continue;
                nk_layout_row_push(ctx, ed_tab_w(nav_names[i]));
                used += ed_tab_w(nav_names[i]) + ctx->style.window.spacing.x;
                b = WB();
                if (!wf_profile()[0] && i != NAV_MODS) {   /* STOCK: only Profiles is live */
                    ed_tab_disabled(nav_names[i]);
                    hint_at(b, "Pick a profile in the header dropdown (or add one on the Profiles tab) to edit here.");
                    continue;
                }
                if (ed_tab(nav_names[i], nav == i || (i == NAV_RULES && nav == NAV_TABLES))) nav = i;
                hint_at(b, nav_help[i]);
            }
            nk_layout_row_push(ctx, 90);
            b = WB();
            if (game_live) {                       /* the in-process game only
                                                      (--editor --play) */
                int pushed = paused;   /* the click flips `paused`: pair the pop with the PUSH, not the new state */
                if (pushed) nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(150, 220, 150));
                if (nk_button_label(ctx, paused ? "RESUME" : "PAUSE")) paused = !paused;
                if (pushed) nk_style_pop_color(ctx);
                hint_at(b, "Pause / resume the running match (also the P key while this window has focus). < > step one frame back / forward while paused (back = the last 120 frames).");
                {   /* < > step: a click = one frame; HOLDING the button repeats
                       (after 0.35 s, ~15 frames/s) — user 2026-08-25 */
                    static Uint32 hold_since; static int hold_dir;
                    Uint32 now = SDL_GetTicks();
                    int dir = 0;
                    nk_layout_row_push(ctx, 30);
                    b = WB();
                    if (nk_button_label(ctx, "<")) { paused = 1; step_back = 1; }
                    if (nk_input_is_mouse_hovering_rect(&ctx->input, b) && nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) dir = -1;
                    hint_at(b, "Step ONE frame back (pauses; rewinds from the snapshot ring, up to 120 frames). Hold to keep stepping.");
                    nk_layout_row_push(ctx, 30);
                    b = WB();
                    if (nk_button_label(ctx, ">")) { paused = 1; step_once = 1; }
                    if (nk_input_is_mouse_hovering_rect(&ctx->input, b) && nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) dir = 1;
                    hint_at(b, "Step ONE frame forward (pauses). Hold to keep stepping.");
                    if (dir && dir == hold_dir) {
                        if (now - hold_since > 350 && ((now - hold_since - 350) / 66) != ((now - hold_since - 350 - 16) / 66)) {
                            paused = 1; if (dir > 0) step_once = 1; else step_back = 1;
                        }
                    } else { hold_dir = dir; hold_since = now; }
                }
            } else {
                nk_label_colored(ctx, "no live game", NK_TEXT_CENTERED, nk_rgb(110, 115, 125));
                hint_at(b, "Pause needs the in-process game: Launch a profile from the Profiles tab (it plays inside the editor).");
                nk_layout_row_push(ctx, 30); nk_label(ctx, "", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 30); nk_label(ctx, "", NK_TEXT_LEFT);
            }
            nk_layout_row_push(ctx, (float)w - used - 40 > 10 ? (float)w - used - 40 : 10);
            nk_label(ctx, "", NK_TEXT_LEFT);   /* (status text removed - gui space) */
            nk_layout_row_end(ctx);
        }
        /* ---- body ---- */
        nk_layout_row(ctx, NK_DYNAMIC, (float)h - 30 - 26 - 30, 2, ratio);
        /* STOCK context (user 2026-08-27): nothing to edit, so every panel
           but Profiles / Tools stays empty until a profile is picked in the
           header dropdown */
        if (!wf_profile()[0] && nav != NAV_MODS && nav != NAV_ENGINE) nav = NAV_MODS;   /* STOCK lands on Profiles (Match stays reachable for debugging) */
        int stock_ctx = !wf_profile()[0] && nav != NAV_MODS && nav != NAV_ENGINE;
        if (nk_group_begin(ctx, "left", NK_WINDOW_BORDER)) {
            if (stock_ctx) { }
            else if (nav == NAV_TABLES) draw_table_list(0);
            else if (nav == NAV_RULES) draw_table_list(1);
            else if (nav == NAV_WRESTLERS) {
                heading("Roster", NULL);
                nk_layout_row_dynamic(ctx, 22, 1);
                for (int i = 0; i < 12; i++) {
                    int sel = (cur_ws == i); char nm[40];
                    snprintf(nm, sizeof nm, "%02d  %s", i, ws_name(i));
                    if (nk_selectable_label(ctx, nm, NK_TEXT_LEFT, &sel) && sel) cur_ws = i;
                }
                for (int i = 12; i < ENG_WS_EXT_MAX; i++) {   /* every clone slot, free or used */
                    int b = eng_ws_clone_base(i), sel = (cur_ws == i); char nm[48];
                    struct nk_rect rb = WB();
                    if (b < 0) {
                        snprintf(nm, sizeof nm, "%02d  (free)", i);
                        nk_style_push_color(ctx, &ctx->style.selectable.text_normal, nk_rgb(110, 110, 125));
                        if (nk_selectable_label(ctx, nm, NK_TEXT_LEFT, &sel) && sel) cur_ws = i;
                        nk_style_pop_color(ctx);
                        continue;
                    }
                    snprintf(nm, sizeof nm, "%02d  %s (of %s)", i, ws_name(i), ws_name(b));
                    /* right-click: Delete ARMS the row (drawn red); a second
                       right-click -> Delete removes it; any other click disarms
                       (user 2026-08-27) */
                    if (nk_selectable_label(ctx, nm, NK_TEXT_LEFT, &sel) && sel) cur_ws = i;
                    if (nk_contextual_begin(ctx, 0, nk_vec2(240, 60), rb)) {
                        nk_layout_row_dynamic(ctx, 24, 1);
                        if (tool_busy) nk_label_colored(ctx, "a tool is running", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
                        else if (nk_contextual_item_label(ctx, "Delete wrestler", NK_TEXT_LEFT))
                            { cf_kind = 1; cf_id = i; snprintf(cf_name, sizeof cf_name, "%02d  %s", i, ws_name(i)); }
                        nk_contextual_end(ctx);
                    }
                }
            } else if (nav == NAV_FORGE) {
                heading("Forge", "Build a wrestler into a clone slot with his own select cell.");
                nk_layout_row_dynamic(ctx, 22, 1);
                for (int s = ENG_WS_MAX; s < ENG_WS_EXT_MAX; s++) {
                    int cb = eng_ws_clone_base(s); char nm[48]; int sel = (fg_slot == s);
                    snprintf(nm, sizeof nm, "%02d  %s", s,
                             cb >= 0 ? (eng_ws_clone_name(s) && eng_ws_clone_name(s)[0] ? eng_ws_clone_name(s) : "(clone)") : "(free)");
                    if (nk_selectable_label(ctx, nm, NK_TEXT_LEFT, &sel) && sel && fg_slot != s) {
                        fg_slot = s;
                        if (eng_ws_clone_base(s) >= 0)
                            fg_load_slot();   /* registered: populate the form (user 2026-08-25) */
                    }
                }
                note("slot registry as loaded from the editing profile's paks");
                nk_layout_row_dynamic(ctx, 130, 1);
                nk_label_wrap(ctx, "A forged wrestler PLAYS like his base (AI, art, announcer) but carries his own name, stats, palette and move map. Give him his own art later: docs/modding.md, 'New-art wrestlers'.");
            } else if (nav == NAV_SKINS) {
                heading("Skins", NULL);
                nk_layout_row_static(ctx, 24, 120, 1);
                if (nk_button_label(ctx, "+ new skin")) {
                    sk_open = 1; sk_sel = -1;
                    sk_name[0] = 0;
                    sk_defaults_load();     /* start from the saved defaults */
                    sk_sub = 0;             /* the name lives on the prompt sub-tab */
                    sk_pt_defaults();
                    for (int k = 0; k < SK_PK_N; k++) snprintf(sk_pt[k], sizeof sk_pt[k], "%s", sk_pdef[k]);
                    sk_pt_cols = 0;
                    sk_height = sk_width = 100; sk_build_anchor = 0;
                    sk_mod[0] = 0;
                    sk_mt_ref = sk_mt_anchor = sk_mt_portrait = sk_mt_frame = 0;
                    sk_rev_page = 0;
                    for (int k = 0; k < SK_REV_N; k++) sk_rev_mt[k] = 0;
                }
                nk_layout_row_dynamic(ctx, 22, 1);
                sk_scan();
                for (int i2 = 0; i2 < sk_nlist; i2++) {
                    int sel = (sk_sel == i2);
                    struct nk_rect rb = WB();
                    if (nk_selectable_label(ctx, sk_list[i2], NK_TEXT_LEFT, &sel) && sel) {
                        sk_open = 1; sk_sel = i2;
                        sk_load(sk_list[i2]);
                    }
                    if (nk_contextual_begin(ctx, 0, nk_vec2(300, 90), rb)) {
                        static int rn_for = -1; static char rn_buf[40];
                        if (rn_for != i2) { rn_for = i2; snprintf(rn_buf, sizeof rn_buf, "%s", sk_list[i2]); }
                        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
                        nk_layout_row_push(ctx, 200); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, rn_buf, sizeof rn_buf, nk_filter_default);
                        nk_layout_row_push(ctx, 80);
                        if (nk_button_label(ctx, "Rename") && rn_buf[0] && strcmp(rn_buf, sk_list[i2]) && !strpbrk(rn_buf, "/\\ .") && !tool_busy) {
                            /* skins/<old> -> <new>, jobs/<old> -> <new>, every wrestler.json
                               wearing it re-pointed (mods/ + roster/) */
                            char sd[300], cmd[1200]; const char *old = sk_list[i2];
                            sk_resolve(old, sd, sizeof sd);
                            snprintf(cmd, sizeof cmd,
                                "mv \"%s\" \"skins/%s\" && { [ -d \"jobs/%s\" ] && mv \"jobs/%s\" \"jobs/%s\"; true; } && "
                                "grep -ls '\"skin\": *\"%s\"' mods/*/wrestlers/*/wrestler.json roster/wrestlers/*/wrestler.json 2>/dev/null | xargs -r sed -i 's/\"skin\": *\"%s\"/\"skin\": \"%s\"/'",
                                sd, rn_buf, old, old, rn_buf, old, old, rn_buf);
                            if (system(cmd) == 0) {
                                logf_("skin '%s' renamed to '%s' (slots wearing it re-pointed)", old, rn_buf);
                                if (sk_sel == i2) sk_load(rn_buf);
                            } else logf_("rename failed (%s)", old);
                            nk_contextual_close(ctx);
                        }
                        nk_layout_row_end(ctx);
                        nk_layout_row_dynamic(ctx, 24, 1);
                        if (tool_busy) nk_label_colored(ctx, "a tool is running", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
                        else if (nk_contextual_item_label(ctx, "Delete skin", NK_TEXT_LEFT))
                            { cf_kind = 2; cf_id = i2; snprintf(cf_name, sizeof cf_name, "%s", sk_list[i2]); }
                        nk_contextual_end(ctx);
                    }
                }
            } else if (nav == NAV_CLASSES) {
                draw_classes_left();
            } else if (nav == NAV_WEAPONS) {
                draw_weapons_left();
            } else if (nav == NAV_ARENAS) {
                draw_arenas_left();
            } else if (nav == NAV_SCENES) {
                draw_scenes_left();
            } else if (nav == NAV_ENGINE) {
                heading("Match controls", "Everything here edits the LIVE match in the other window. Nothing is saved to disk.");
                nk_layout_row_dynamic(ctx, 28, 1);
                { struct nk_rect b = WB();
                  if (nk_button_label(ctx, "Step one frame")) step_once = 1;
                  hint_at(b, "Run exactly one engine frame while paused - single-step the action."); }
                { struct nk_rect b = WB();
                  if (nk_button_label(ctx, "+1 credit")) eng_credit_set(eng_credits() + 1);
                  hint_at(b, "Insert a coin (the front-end CREDIT counter)."); }
                nk_layout_row_dynamic(ctx, 60, 1);
                nk_label_colored_wrap(ctx, "The panel on the right lists every live object (wrestler) with its decoded state and editable values.", nk_rgb(165, 165, 175));
            } else {
                heading("wfeditor", "The in-engine deep-dive editor. Everything works on the same in-memory data the running match uses.");
                nk_layout_row_dynamic(ctx, 150, 1);
                nk_label_wrap(ctx, "Tables/Rules: edit cells, Save (to the profile's save layer). Wrestlers: stats, palette, standing sprite. Engine: pause/step + live objects. Mods: pick where saves go. Tools: pack/export/verify/regress with output below.");
                nk_layout_row_dynamic(ctx, 70, 1);
                nk_label_wrap(ctx, "Keys in this window: P pause/resume, F11 fullscreen. Game keys are ignored while this window has focus. Hover any control for an explanation.");
            }
            nk_group_end(ctx);
        }
        if (nk_group_begin(ctx, "right", NK_WINDOW_BORDER)) {
            if (stock_ctx) {
                nk_layout_row_dynamic(ctx, 26, 1);
                nk_label_colored(ctx, "STOCK is locked - pick a profile in the header dropdown (or add one on the Profiles tab) to edit.", NK_TEXT_LEFT, nk_rgb(255, 205, 100));
                nk_group_end(ctx);
            } else {
            switch (nav) {
            case NAV_TABLES: case NAV_RULES:
                {   /* Rules sub-tabs (user 2026-08-28): Ruleset = the rule
                       scalars, Tables = the deep-dive table editor */
                    nk_layout_row_begin(ctx, NK_STATIC, 26, 4);
                    nk_layout_row_push(ctx, ed_tab_w("Ruleset"));
                    if (ed_tab("Ruleset", nav == NAV_RULES && !rules_arenas) && (nav != NAV_RULES || rules_arenas)) {
                        nav = NAV_RULES; rules_arenas = 0;
                        { const tbl_def *d = tbl_def_at(cur_tbl);
                          if (!d || strcmp(d->group, "rules")) select_table(tbl_id("ringout_rules")); }
                    }
                    nk_layout_row_push(ctx, ed_tab_w("Tables"));
                    if (ed_tab("Tables", nav == NAV_TABLES)) { nav = NAV_TABLES; rules_arenas = 0; }
                    nk_layout_row_push(ctx, ed_tab_w("Arenas"));
                    if (ed_tab("Arenas", nav == NAV_RULES && rules_arenas == 1)) { nav = NAV_RULES; rules_arenas = 1; }
                    nk_layout_row_push(ctx, ed_tab_w("Scenes"));
                    if (ed_tab("Scenes", nav == NAV_RULES && rules_arenas == 2)) { nav = NAV_RULES; rules_arenas = 2; }
                    nk_layout_row_end(ctx);
                }
                if (nav == NAV_TABLES) draw_table_editor(); else if (rules_arenas == 1) draw_rules_arenas(); else if (rules_arenas == 2) draw_rules_scenes(); else draw_rules();
                break;
            case NAV_INPUT: draw_input(); break;
            case NAV_WRESTLERS: draw_wrestlers(); break;
            case NAV_FORGE: draw_forge(); break;
            case NAV_SKINS: draw_skins(); break;
            case NAV_CLASSES: draw_classes(); break;   /* its own main tab again (user 2026-08-28, reversed) */
            case NAV_WEAPONS: draw_weapons(); break;
            case NAV_ARENAS: draw_arenas(); break;
            case NAV_SCENES: draw_scenes(); break;
            case NAV_CALIB: draw_calib(st); break;
            case NAV_ENGINE: draw_engine(st); break;
            case NAV_MODS: draw_mods(); break;
            case NAV_SOUNDS: draw_sounds(); break;
            default: draw_tools(); break;
            }
            nk_group_end(ctx);
            }
        }
        /* ---- status bar ---- */
        {
            /* profile | table state | tool | [skin run: 210 px bar + cyan
             * text (user 2026-08-28)] or the log tail */
            int skrun = (nav == NAV_SKINS && !stock_ctx && sk_name[0]) || (nav == NAV_CLASSES && !stock_ctx) || (nav == NAV_ARENAS && !stock_ctx && ar_sel_name()[0]);
            float fixed = 0.16f * (float)w + 0.14f * (float)w + 0.10f * (float)w;
            nk_layout_row_begin(ctx, NK_STATIC, 20, skrun ? 5 : 4);
            nk_layout_row_push(ctx, 0.16f * (float)w);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "%s%s | save: %s",
                              wf_profile()[0] ? "profile: " : "", wf_profile()[0] ? wf_profile() : "STOCK",
                              mod_layer[0] ? mod_layer : "(read-only)");
            nk_layout_row_push(ctx, 0.14f * (float)w);
            if (edit_dirty)
                nk_label_colored(ctx, "unsaved table edits", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            else if (tbl_changed_total > 0)
                nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(255, 210, 120), "%d table%s off stock", tbl_changed_total, tbl_changed_total == 1 ? "" : "s");
            else
                nk_label_colored(ctx, "", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            nk_layout_row_push(ctx, 0.10f * (float)w);
            nk_label_colored(ctx, tool_busy ? "tool running..." : "", NK_TEXT_LEFT, nk_rgb(240, 180, 60));
            if (!skrun || !sk_run_status((float)w - fixed - 40)) {
                nk_layout_row_push(ctx, (float)w - fixed - 40);
                SDL_LockMutex(log_mx);
                nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "%s",
                                  log_n ? logbuf[(log_head - 1 + 64) % 64] : "");
                SDL_UnlockMutex(log_mx);
            }
            nk_layout_row_end(ctx);
        }
    }
    nk_end(ctx);
    if (pose_zoom_on) {                /* STANDING SPRITE zoom: the 256 px render at 3x, click anywhere / Esc to close */
        int zs = h - 120 < 768 ? h - 120 : 768;
        nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 100)));
        if (nk_begin(ctx, "pose zoom", nk_rect(0, 0, (float)w, (float)h), NK_WINDOW_NO_SCROLLBAR)) {
            sheet_box((float)w / 2, (float)h / 2, (float)zs + 24, (float)zs + 24);
            nk_layout_row_dynamic(ctx, ((float)h - (float)zs) / 2 - 8, 1); nk_label(ctx, "", NK_TEXT_LEFT);
            nk_layout_row_begin(ctx, NK_STATIC, (float)zs, 2);
            nk_layout_row_push(ctx, ((float)w - (float)zs) / 2 - 12); nk_label(ctx, "", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, (float)zs);
            if (pose_tex) nk_image(ctx, nk_image_ptr(pose_tex));
            nk_layout_row_end(ctx);
            if (pose_zoom_on == 2) pose_zoom_on = 1;                    /* the opening release is not a close */
            else if (nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT)) pose_zoom_on = 0;
        }
        nk_end(ctx);
        nk_style_pop_style_item(ctx);
    }
    if (cf_kind) {                     /* DELETE CONFIRM sheet */
        static const char *what[6] = { "", "wrestler", "skin", "profile", "arena", "scene" };
        nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 100)));
        if (nk_begin(ctx, "confirm", nk_rect(0, 0, (float)w, (float)h), NK_WINDOW_NO_SCROLLBAR)) {
            sheet_box((float)w / 2, (float)h / 2 + 5, 820, 140);
            nk_layout_row_dynamic(ctx, (float)h / 2 - 60, 1); nk_label(ctx, "", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(255, 120, 100), "Delete %s  %s ?", what[cf_kind], cf_name);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, cf_kind == 1 ? "removes his package, select portrait and outfits from his layer and repacks; a skin he wears stays in the library"
                                 : cf_kind == 2 ? "removes skins/<name> and its generation job; slots already dressed in it keep their art"
                                 : cf_kind == 4 ? "removes arenas/<name> and build/arenas/<name>.pak; a profile still naming it falls back to the ROM arena"
                                 : cf_kind == 5 ? "removes scenes/<name>; a profile still naming it falls back to the stock screen"
                                 : "removes profiles/<name>.json and its pak cache; its mod layers in mods/ are kept", NK_TEXT_CENTERED, nk_rgb(190, 190, 200));
            nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
            nk_layout_row_push(ctx, (float)w / 2 - 130); nk_label(ctx, "", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 120);
            {
                struct nk_style_button warn = ctx->style.button;
                warn.normal = warn.hover = warn.active = nk_style_item_color(nk_rgb(120, 30, 30));
                if (nk_button_label_styled(ctx, &warn, "Delete")) {
                    if (cf_kind == 1) { if (slot_delete(cf_id)) fg_reload_slot = cf_id; }
                    else if (cf_kind == 2) skin_delete(cf_name);
                    else if (cf_kind == 4) arena_delete(cf_name);
                    else if (cf_kind == 5) scene_delete(cf_name);
                    else profile_delete(cf_id);
                    cf_kind = 0;
                }
            }
            nk_layout_row_push(ctx, 120);
            if (nk_button_label(ctx, "Cancel")) cf_kind = 0;
            nk_layout_row_end(ctx);
        }
        nk_end(ctx);
        nk_style_pop_style_item(ctx);
    }
    if ((tool_busy && tool_modal) || pend_prof_on || pend_fg_on) {   /* LOCKING spinner: a centred window over everything, the
                                          editor's own window gets no input while it shows */
        /* a full-window translucent sheet (topmost window = it takes every
           click) with the spinner in the middle */
        nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 100)));
        if (nk_begin(ctx, "working", nk_rect(0, 0, (float)w, (float)h), NK_WINDOW_NO_SCROLLBAR)) {
            const char sp[4] = { '|', '/', '-', '\\' };
            /* text block: h/2-40 .. h/2+12 */
            sheet_box((float)w / 2, (float)h / 2 - 14, 640, 100);
            nk_layout_row_dynamic(ctx, (float)h / 2 - 40, 1); nk_label(ctx, "", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 30, 1);
            nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(240, 180, 60), "%c   %s   %c",
                              sp[(SDL_GetTicks() / 120) & 3],
                              pend_prof_on ? "loading profile..." : pend_fg_on ? "saving wrestler..." : "working...",
                              sp[(SDL_GetTicks() / 120) & 3]);
            nk_layout_row_dynamic(ctx, 22, 1);
            if (pend_prof_on) nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(200, 200, 210), "%s", pend_prof[0] ? pend_prof : "stock");
            else {
                SDL_LockMutex(log_mx);
                nk_labelf_colored(ctx, NK_TEXT_CENTERED, nk_rgb(200, 200, 210), "%.70s", log_n ? logbuf[(log_head - 1 + 64) % 64] : "");
                SDL_UnlockMutex(log_mx);
            }
        }
        nk_end(ctx);
        nk_style_pop_style_item(ctx);
    }
    if (sk_zoom_on == 2) {             /* the OPENING click's press/release must not land
                                          on a modal button under the cursor ("clicking a
                                          tile ALSO clicked a button", user 2026-08-28) */
        if (!ctx->input.mouse.buttons[NK_BUTTON_LEFT].down) sk_zoom_on = 1;
        ctx->input.mouse.buttons[NK_BUTTON_LEFT].down = nk_false;
        ctx->input.mouse.buttons[NK_BUTTON_LEFT].clicked = nk_false;
    }
    if (fr_on == 2) { if (!ctx->input.mouse.buttons[NK_BUTTON_LEFT].down) fr_on = 1; ctx->input.mouse.buttons[NK_BUTTON_LEFT].down = nk_false; }
    if (fr_on) {                       /* FRAME modal: prev/next through a frame list */
        /* the window is sized from the PICTURE (a 960x256 ringside strip gets a
         * wide, short window; a 64x64 crowd patch a small one) - a fixed 640x520
         * box shrank the wide views into a strip over dead space (user 2026-08-30) */
        static float last_zw, last_zh;
        float zw = (float)(w > 760 ? 640 : w - 40), zh = zw - 120, sc = 1;
        int tw = 0, th = 0, kind = 0; char alt[300] = ""; const char *show = fr_n > 0 ? fr_paths[fr_idx] : "";
        if (fr_n > 0) { kind = fr_counterpart(fr_paths[fr_idx], alt, sizeof alt); if (fr_alt && kind) show = alt; }
        if (fr_n > 0 && strcmp(fr_cur, show)) { snprintf(fr_cur, sizeof fr_cur, "%s", show); if (fr_tex) { SDL_DestroyTexture(fr_tex); fr_tex = NULL; } fr_mt = 0; }
        if (fr_n > 0) { sk_png_gray = 0; sk_png_under = NULL; sk_png_plain = 1; sk_png_canvas = 2; sk_png_tex(fr_cur, &fr_tex, &fr_mt); sk_png_plain = 0; sk_png_canvas = 0; }
        if (fr_tex) {
            SDL_QueryTexture(fr_tex, NULL, NULL, &tw, &th);
            sc = ((float)w - 80) / (float)tw; if ((float)th * sc > (float)h - 160) sc = ((float)h - 160) / (float)th;
            if (sc > 6) sc = 6;
            zw = (float)tw * sc + 30; if (zw < 420) zw = 420;
            zh = (float)th * sc + 110 + (kind ? 30 : 0);
        }
        if (zh > (float)h - 20) zh = (float)h - 20;
        if (zw != last_zw || zh != last_zh) { nk_window_set_bounds(ctx, "frames", nk_rect(((float)w - zw) / 2, ((float)h - zh) / 2, zw, zh)); last_zw = zw; last_zh = zh; }
        if (nk_begin(ctx, "frames", nk_rect(((float)w - zw) / 2, ((float)h - zh) / 2, zw, zh),
                     NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_NO_SCROLLBAR)) {
            int step = 0; float imh = zh - 110 - (kind ? 30 : 0), drawn;
            nk_layout_row_dynamic(ctx, 26, 3);
            if (nk_button_label(ctx, "< prev")) step = -1;
            nk_labelf(ctx, NK_TEXT_CENTERED, "%s   %d / %d", fr_title, fr_idx + 1, fr_n);
            if (nk_button_label(ctx, "next >")) step = 1;
            if (step && fr_n > 0) fr_idx = (fr_idx + step + fr_n) % fr_n;
            if (kind) {                /* original <-> generated (the skins modal's ref toggle) */
                int showing_gen = (kind == 2) != (fr_alt != 0);
                nk_layout_row_dynamic(ctx, 26, 3);
                nk_label_colored(ctx, showing_gen ? "showing: GENERATED" : "showing: ORIGINAL", NK_TEXT_LEFT, showing_gen ? nk_rgb(150, 210, 255) : nk_rgb(240, 180, 60));
                if (nk_button_label(ctx, showing_gen ? "show original" : "show generated")) fr_alt = !fr_alt;
                nk_label(ctx, "", NK_TEXT_LEFT);
            }
            if (fr_tex) {
                drawn = (float)th * sc; if (drawn > imh) drawn = imh;
                nk_layout_row_dynamic(ctx, imh, 1);
                if (nk_group_begin(ctx, "frimg", NK_WINDOW_NO_SCROLLBAR)) {
                    nk_layout_row_static(ctx, drawn, (int)((float)tw * sc), 1);
                    nk_image(ctx, nk_image_ptr(fr_tex));
                    nk_group_end(ctx);
                }
            }
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_labelf_colored(ctx, NK_TEXT_LEFT, nk_rgb(150, 150, 160), "%s  (%dx%d)", show, tw, th);
        }
        nk_end(ctx);
        if (nk_window_is_hidden(ctx, "frames")) fr_on = 0;
    }
    if (sk_zoom_on) {                  /* preview zoom modal (click to open, X to close) */
        float zw = (float)(w > 760 ? 720 : w - 40), zh = zw + 34;
        if (sk_zoom_pose >= 0) {       /* track the pose: out-frame if it exists,
                                          the mannequin otherwise — a re-rolled
                                          image (or a revert) lands LIVE; the
                                          ref toggle forces the original */
            char zp[600], zr[600]; struct stat zst;
            if (sk_zoom_out_fmt[0]) snprintf(zr, sizeof zr, sk_zoom_ref_fmt, sk_zoom_pose);
            else snprintf(zr, sizeof zr, "jobs/%s/ref/pose_%04d.png", sk_name, sk_zoom_pose);
            if (sk_zoom_showref) snprintf(zp, sizeof zp, "%s", zr);
            else {
                if (sk_zoom_out_fmt[0]) snprintf(zp, sizeof zp, sk_zoom_out_fmt, sk_zoom_pose);
                else snprintf(zp, sizeof zp, "jobs/%s/out/pose_%04d.png", sk_name, sk_zoom_pose);
                if (stat(zp, &zst)) snprintf(zp, sizeof zp, "%s", zr);
            }
            if (strcmp(zp, sk_zoom_path)) { snprintf(sk_zoom_path, sizeof sk_zoom_path, "%s", zp); sk_zoom_mt = 0; }
        }
        {   /* the zoomed frame composites the SAME victim underlay as the
               grid tile, so hand positions are judgeable at size (user
               2026-08-28: "i dont get the opponents sprite in the modal");
               'ghost' overlays the pose's own GREY REF under the result so
               a scale/position shift (height_pct!) is directly visible */
            static char zunder[560]; int zvid = -1;
            int zcls = sk_zoom_out_fmt[0] ? sk_zoom_cls : sk_class;
            if (sk_zoom_ghost && sk_zoom_pose >= 0 && !sk_zoom_showref) {
                if (sk_zoom_out_fmt[0]) snprintf(zunder, sizeof zunder, sk_zoom_ref_fmt, sk_zoom_pose);
                else snprintf(zunder, sizeof zunder, "jobs/%s/ref/pose_%04d.png", sk_name, sk_zoom_pose);
                sk_png_ghost = zunder;
            } else if (sk_zoom_pose >= 0 && sk_zoom_pose < 800) {
                zvid = pi_pair_victim(sk_zoom_out_fmt[0] ? cls_rep[zcls] : sk_base, sk_zoom_pose);
                if (zvid >= 0) {
                    struct stat vs2;
                    snprintf(zunder, sizeof zunder, "jobs/stock-class%d/out/pose_%04d.png", zcls, zvid);
                    if (stat(zunder, &vs2)) snprintf(zunder, sizeof zunder, "jobs/stock-class%d/ref/pose_%04d.png", zcls, zvid);
                    if (stat(zunder, &vs2)) snprintf(zunder, sizeof zunder, "jobs/stock-class0/out/pose_%04d.png", zvid);
                    sk_png_under = zunder;
                }
            }
            sk_png_canvas = (sk_zoom_pose >= 0 || sk_zoom_tile == 0 || sk_zoom_tile == 1) ? 2 : 0;
                                       /* poses AND the base-ref/anchor tiles keep the
                                          whole canvas: flipping ref <-> anchor (prev/
                                          next) shows the true scale difference */
            sk_png_tex(sk_zoom_path, &sk_zoom_tex, &sk_zoom_mt);
            sk_png_canvas = 0;
            sk_png_under = NULL; sk_png_ghost = NULL;
        }
        if (zh > (float)h - 20) { zh = (float)h - 20; zw = zh - 34; }
        if (nk_begin(ctx, "skin zoom", nk_rect(((float)w - zw) / 2, ((float)h - zh) / 2, zw, zh),
                     NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_NO_SCROLLBAR)) {
            float imh = zw - 20 - (sk_zoom_pose >= 0 ? 96 : (sk_zoom_tile == 1 && sk_an_n > 1) ? 30 : 0);
            if (sk_zoom_pose < 0 && sk_zoom_tile >= 0) {   /* prev/next through the preview tiles
                                                              (ref <-> anchor <-> portrait <-> frame) */
                int step = 0;
                nk_layout_row_dynamic(ctx, 26, 3);
                if (nk_button_label(ctx, "< prev")) step = -1;
                nk_labelf(ctx, NK_TEXT_CENTERED, "%s", sk_zoom_title);
                if (nk_button_label(ctx, "next >")) step = 1;
                if (step) {
                    int k = sk_zoom_tile;
                    for (int t = 0; t < 4; t++) {   /* skip tiles that have no file */
                        struct stat zs;
                        k = (k + step + 4) % 4;
                        if (sk_zsrc[k][0] && stat(sk_zsrc[k], &zs) == 0) break;
                    }
                    sk_zoom_tile = k; sk_zoom_mt = 0;
                    snprintf(sk_zoom_path, sizeof sk_zoom_path, "%s", sk_zsrc[k]);
                    snprintf(sk_zoom_title, sizeof sk_zoom_title, "%s", k == 3 ? sk_frame_name : sk_zsrc_name[k]);
                }
                if (sk_zoom_tile == 1 && sk_an_n > 1) {   /* anchor: walk the candidate history */
                    char zd[300]; int go = 0;
                    sk_dir(zd, sizeof zd);
                    nk_layout_row_dynamic(ctx, 26, 3);
                    if (nk_button_label(ctx, "< earlier candidate") && sk_an_cur > 0 && !tool_busy) { sk_an_cur--; go = 1; }
                    nk_labelf(ctx, NK_TEXT_CENTERED, "candidate %d / %d", sk_an_cur + 1, sk_an_n);
                    if (nk_button_label(ctx, "later candidate >") && sk_an_cur < sk_an_n - 1 && !tool_busy) { sk_an_cur++; go = 1; }
                    if (go) {
                        char cc[1400];
                        snprintf(cc, sizeof cc, "cp \"%s/anchors/%02d.png\" \"%s/anchor_candidate.png\" && (cp \"%s/anchors/%02d.png.meta\" \"%s/anchor_candidate.png.meta\" 2>/dev/null; true)",
                                 zd, sk_an_cur + 1, zd, zd, sk_an_cur + 1, zd);
                        if (system(cc) == 0) { sk_mt_anchor = 0; sk_zoom_mt = 0; logf_("anchor candidate %d/%d (the next step locks it in)", sk_an_cur + 1, sk_an_n); }
                    }
                }
            }
            if (sk_zoom_pose >= 0) {   /* prev/next: the review list, or the CARD ROW
                                          when a face/title frame is open (they are not
                                          in the review list - user 2026-08-28: "i want
                                          to scroll across the face frames") */
                static const int cardnav[7] = { 800, 801, 802, 804, 805, 806, 810 };
                const int *nav = sk_all_poses; int navn = sk_all_n;
                int cur = -1;
                if (sk_zoom_pose >= 800 && sk_zoom_pose < 1024) { nav = cardnav; navn = 7; }
                for (int k = 0; k < navn; k++) if (nav[k] == sk_zoom_pose) { cur = k; break; }
                if (navn > 0) {
                    nk_layout_row_dynamic(ctx, 26, 3);
                    if (nk_button_label(ctx, "< prev") && cur > 0) {
                        sk_zoom_pose = nav[cur - 1]; sk_zoom_mt = 0;
                        snprintf(sk_zoom_title, sizeof sk_zoom_title, "pose %04d", sk_zoom_pose);
                    }
                    nk_labelf(ctx, NK_TEXT_CENTERED, "%d / %d", cur + 1, navn);
                    if (nk_button_label(ctx, "next >") && cur >= 0 && cur < navn - 1) {
                        sk_zoom_pose = nav[cur + 1]; sk_zoom_mt = 0;
                        snprintf(sk_zoom_title, sizeof sk_zoom_title, "pose %04d", sk_zoom_pose);
                    }
                }
            }
            {   /* the image keeps ITS OWN aspect, centred; the MOUSE WHEEL
                   zooms INTO THE CENTRE by showing a centred sub-rect of the
                   texture at the same widget size - no layout change, no
                   scrollbars - and the factor persists across prev/next and
                   the ref toggle (user 2026-08-28) */
                static float zsc = 1.0f;
                int tw = 0, th = 0; float iw = imh, pad;
                if (ctx->input.mouse.scroll_delta.y != 0 && sk_zoom_tex) {
                    zsc *= ctx->input.mouse.scroll_delta.y > 0 ? 1.15f : 1.0f / 1.15f;
                    if (zsc < 1.0f) zsc = 1.0f;
                    if (zsc > 6.0f) zsc = 6.0f;
                    ctx->input.mouse.scroll_delta.y = 0;
                }
                if (sk_zoom_tex && SDL_QueryTexture(sk_zoom_tex, NULL, NULL, &tw, &th) == 0 && th > 0)
                    iw = imh * (float)tw / (float)th;
                if (iw > zw - 24) { imh *= (zw - 24) / iw; iw = zw - 24; }
                pad = (zw - 24 - iw) / 2; if (pad < 0) pad = 0;
                nk_layout_row_begin(ctx, NK_STATIC, imh, 2);
                nk_layout_row_push(ctx, pad); nk_label(ctx, "", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, iw);
                if (sk_zoom_tex) {
                    float sw2 = (float)tw / zsc, sh2 = (float)th / zsc;
                    struct nk_rect sub = nk_rect(((float)tw - sw2) / 2, ((float)th - sh2) / 2, sw2, sh2);
                    nk_image(ctx, nk_subimage_ptr(sk_zoom_tex, (nk_ushort)tw, (nk_ushort)th, sub));
                } else nk_label(ctx, "(loading...)", NK_TEXT_CENTERED);
                nk_layout_row_end(ctx);
            }
            if (sk_zoom_pose >= 0) {   /* tweak field + ref toggle + re-roll + revert */
                {
                    float tr[2] = { 0.16f, 0.84f };
                    nk_layout_row(ctx, NK_DYNAMIC, 26, 2, tr);
                    nk_checkbox_label(ctx, "append", &sk_zoom_append);
                    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, sk_zoom_extra,
                                                   sizeof sk_zoom_extra, nk_filter_default);
                }
                static const float br[4] = { 0.26f, 0.10f, 0.36f, 0.28f };
                nk_layout_row(ctx, NK_DYNAMIC, 28, 4, br);
                /* ^ nuklear KEEPS the ratio POINTER for the row's widgets —
                   a block-scoped array dangled and every widget got garbage
                   widths ("buttons missing, elements mangled", user
                   2026-08-28); static storage outlives the row */
                {   /* viewing the REF = red button, viewing the RESULT = green */
                    struct nk_style_button tb = ctx->style.button;
                    struct nk_color tc = sk_zoom_showref ? nk_rgb(140, 40, 40)
                                                         : nk_rgb(40, 110, 60);
                    tb.normal = tb.hover = tb.active = nk_style_item_color(tc);
                    if (nk_button_label_styled(ctx, &tb,
                            sk_zoom_showref ? "Show result" : "Show original ref"))
                        { sk_zoom_showref = !sk_zoom_showref; sk_zoom_mt = 0; }
                }
                {   int g2 = sk_zoom_ghost;
                    struct nk_rect gb2 = WB();
                    nk_checkbox_label(ctx, "ref", &g2);
                    hint_at(gb2, "Ghost the pose's reference OVER the result at ~35%: a size or position shift (height_pct, drifted feet, moved hands) reads directly.");
                    if (g2 != sk_zoom_ghost) { sk_zoom_ghost = g2; sk_zoom_mt = 0; }
                }
                {   /* ONE button (user 2026-08-27): idle -> re-roll now; busy ->
                     * this pose joins the queue and starts by itself when the
                     * running job ends (sk_rq_run from the art tab's frame) */
                    int queued = sk_rq_find(sk_zoom_pose) >= 0; char lab[64];
                    if (queued) {                  /* queued = committed (user 2026-08-27): no un-queue */
                        snprintf(lab, sizeof lab, "Queued for re-roll (%d ahead)", sk_rq_find(sk_zoom_pose));
                        nk_label_colored(ctx, lab, NK_TEXT_CENTERED, nk_rgb(255, 205, 100));
                    } else {
                    snprintf(lab, sizeof lab, tool_busy ? "Re-roll this pose (queue: %d)" : "Re-roll this pose (codex single)", sk_rq_n);
                    if (nk_button_label(ctx, lab)) {
                        char zd[300], zcmd[900], extra[222]; int xn = 0;
                        sk_dir(zd, sizeof zd);
                        /* the tweak rides the command as the optional 5th arg
                           (single quotes; embedded quotes stripped) */
                        if (!sk_zoom_append && sk_zoom_extra[0])
                            extra[xn++] = '=';         /* replace-mode marker */
                        for (int ci = 0; sk_zoom_extra[ci] && xn < (int)sizeof extra - 1; ci++)
                            if (sk_zoom_extra[ci] != '\'' && sk_zoom_extra[ci] != '"')
                                extra[xn++] = sk_zoom_extra[ci];
                        extra[xn] = 0;
                        if (!sk_zoom_append && sk_zoom_extra[0])
                            logline("REPLACE mode: your text is the whole prompt - remember to ask for the magenta background");
                        if (tool_busy) sk_rq_toggle(sk_zoom_pose, extra);
                        else if (sk_zoom_out_fmt[0]) {   /* a CLASS frame: redraw it on the class body */
                            char gp[300]; snprintf(gp, sizeof gp, sk_zoom_out_fmt, sk_zoom_pose); unlink(gp);
                            snprintf(zcmd, sizeof zcmd, "./wfengine --class-build %d %d%s%s%s", sk_zoom_cls, sk_zoom_pose, xn ? " '" : "", xn ? extra : "", xn ? "'" : "");
                            run_tool(zcmd); cs_loaded = -1;
                            logf_("class %d: re-drawing pose %04d on the class body", sk_zoom_cls, sk_zoom_pose);
                        } else {
                            snprintf(zcmd, sizeof zcmd,
                                     "./wfengine --art-pose \"jobs/%s\" \"%s/anchor.png\" \"%s/character.txt\" %d%s%s%s",
                                     sk_name, zd, zd, sk_zoom_pose,
                                     xn ? " '" : "", xn ? extra : "", xn ? "'" : "");
                            run_tool(zcmd);
                            logf_("re-rolling pose %04d - the frame refreshes when it lands", sk_zoom_pose);
                        }
                    }
                    }
                }
                {   /* revert is REVERSIBLE (user 2026-08-26): the generated
                     * frame (and its hi-res archive) moves to jobs/<skin>/trash/
                     * and 'Restore generated' moves it back - no new download */
                    char op[560], tp[560], oh[560], th[560]; struct stat ost, tst;
                    snprintf(op, sizeof op, "jobs/%s/out/pose_%04d.png", sk_name, sk_zoom_pose);
                    snprintf(tp, sizeof tp, "jobs/%s/trash/pose_%04d.png", sk_name, sk_zoom_pose);
                    snprintf(oh, sizeof oh, "jobs/%s/out_hi/pose_%04d.png", sk_name, sk_zoom_pose);
                    snprintf(th, sizeof th, "jobs/%s/trash/hi_%04d.png", sk_name, sk_zoom_pose);
                    if (stat(op, &ost) == 0) {
                        if (nk_button_label(ctx, "Revert to stock (base art)")) {
                            char sp2[560], td[560];
                            snprintf(td, sizeof td, "jobs/%s/trash", sk_name); mkdir_p(td);
                            if (rename(op, tp) == 0) {
                                char zd2[300];
                                rename(oh, th);
                                sk_dir(zd2, sizeof zd2);
                                snprintf(sp2, sizeof sp2, "%s/frames/pose_%04d.png", zd2, sk_zoom_pose);
                                unlink(sp2);           /* also drop a saved copy in the skin */
                                snprintf(sp2, sizeof sp2, "%s/frames_hi/pose_%04d.png", zd2, sk_zoom_pose);
                                unlink(sp2);
                                logf_("pose %04d reverted to base art (kept in trash/ - 'Restore generated' brings it back)", sk_zoom_pose);
                            } else logf_("cannot move pose %04d", sk_zoom_pose);
                        }
                    } else if (stat(tp, &tst) == 0) {
                        if (nk_button_label(ctx, "Restore generated (from trash)")) {
                            if (rename(tp, op) == 0) { rename(th, oh); logf_("pose %04d restored (Save skin to pull it in)", sk_zoom_pose); }
                            else logf_("cannot restore pose %04d", sk_zoom_pose);
                        }
                    } else nk_label(ctx, "(already base art)", NK_TEXT_CENTERED);
                }
            }
        }
        nk_end(ctx);
        if (nk_window_is_hidden(ctx, "skin zoom")) { sk_zoom_on = 0; sk_zoom_showref = 0; }   /* closing resets the ref toggle (user 2026-08-26) */
    }
    SDL_SetRenderDrawColor(ren, 30, 30, 34, 255);
    SDL_RenderClear(ren);
    nk_sdl_render(NK_ANTI_ALIASING_ON);
    if (pend_prof_on || pend_fg_on) {      /* keep this frame: the progress frames paint it underneath */
        static uint8_t *cap; static int cw, ch;
        if (!cap || cw != w || ch != h) { free(cap); cap = malloc((size_t)w * (size_t)h * 4); cw = w; ch = h;
            if (prog_bg) SDL_DestroyTexture(prog_bg); prog_bg = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h); }
        prog_bg_ok = cap && prog_bg && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, cap, w * 4) == 0
                     && SDL_UpdateTexture(prog_bg, NULL, cap, w * 4) == 0;
    }
    if (getenv("WF_EDITOR_SHOT")) {        /* debug: dump the editor window as PPM every frame */
        static uint8_t *buf; FILE *f;
        if (!buf) buf = malloc((size_t)w * (size_t)h * 4);
        if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, buf, w * 4) == 0 && (f = fopen(getenv("WF_EDITOR_SHOT"), "wb"))) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) { fputc(buf[i*4+2], f); fputc(buf[i*4+1], f); fputc(buf[i*4], f); }
            fclose(f);
        }
    }
    SDL_RenderPresent(ren);
    nk_input_begin(ctx);
}
