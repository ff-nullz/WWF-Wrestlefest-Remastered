/* General-mod rule scalars (phase 2 of the mod plan, user 2026-08-24) —
 * a synthetic `rules` table whose STOCK values are all neutral, so the
 * stock pak plays byte-for-byte stock and a mod layer flips them per
 * profile (docs/adr-001). Every consumer cites its hook site.
 *
 *   unlimited_energy   0/1/2: 1 = pad humans never lose energy, 2 = nobody
 *                      does (hook: eng_mod_energy_tick, core frame loop)
 *   unlimited_time     0/1: the match clock never ticks (hud.c 0x26370)
 *   difficulty_offset  0x80 + n (u16, 0x80 = stock): shifts the human-vs-
 *                      CPU tie-up bias row ($1C0162 stage row, tieup.c
 *                      0xF6D6). +n = CPU wins exchanges more often.
 *   speed_pct_human    walk/run speed scale, 100 = stock (anim.c
 *                      0x116AE/0x11D4A readers)
 *   speed_pct_cpu      same for CPU-driven men
 *   cpu_kickout_delay  extra half-counts before a pinned CPU kicks out
 *                      (ai.c 0x1E45C) — negative energy for the player;
 *                      0x80 + n encoded like difficulty_offset
 *   human_hit_mult     1..9: damage dealt BY a human player is multiplied
 *                      (hook: eng_damage_drain, hit.c 0x24E58 — the victim's
 *                      +0x92 pairing names the attacker) (user 2026-08-25)
 *   parasitic_pct      0 = off; n = that percent of every hit's damage is
 *                      GIVEN to the hitter's energy (100 = the full amount)
 *                      (hook: eng_damage_drain) (user 2026-09-05)
 *   turbo              0 = off; 1..5 = the WRESTLERS run faster - walk/run
 *                      speed and every animation frame x (100 + 20n)%, the
 *                      clocks / counts / referee at real time (hooks:
 *                      eng_mod_turbo_speed at the walk/run speed writes,
 *                      anim.c anim_load) (user 2026-09-05)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
extern int eng_dbgsel;

/* game_rules: rows, defaults, labels, help - all from src/rules.def */
#define GAME_RULE(e, l, d, h) TBL_BE16(d),
#define MODE_RULE(e, l, d, h)
static const uint8_t mod_rules_be[] = {
#include "rules.def"
};
#undef GAME_RULE
#undef MODE_RULE
#define GAME_RULE(e, l, d, h) l,
#define MODE_RULE(e, l, d, h)
static const char *const mod_rule_labels[] = {
#include "rules.def"
    NULL };
#undef GAME_RULE
#undef MODE_RULE
#define GAME_RULE(e, l, d, h) d,
#define MODE_RULE(e, l, d, h)
static const int mod_rule_defaults[] = {
#include "rules.def"
};
#undef GAME_RULE
#undef MODE_RULE
#define GAME_RULE(e, l, d, h) { l, h },
#define MODE_RULE(e, l, d, h)
static const struct { const char *label, *help; } mod_rule_help[] = {
#include "rules.def"
    { NULL, NULL } };
#undef GAME_RULE
#undef MODE_RULE
static const tbl_def mod_rule_tables[] = {
    { "game_rules", "rules", TBL_SYNTH, sizeof mod_rules_be, TK_U16, 1,
      "general-mod switches (the editor's Rules > game_rules has a line per row), all NEUTRAL in stock: unlimited energy (1 humans / 2 all), unlimited time, tie-up difficulty offset (0x80 = stock), walk/run speed %% human/CPU (100 = stock), CPU pin kick-out delay (0x80 = stock), REF KNOCKDOWN on/off + recovery frames (strikes floor the referee: no counts while he is down), weapon DQ on/off (a landed weapon swing loses the match), CPU energy meters on/off (CPU rows show gauge+portrait instead of the idle BUY-IN plate), grapple gauge on/off (hold-clock bar, blinks when the throw window opens), exit_ring (hold into a rope to hop out, tag only), backup run-ins: avg seconds between them (0 = off) / per-match cap / brawl frames before leaving, camera zoom %% (100 = stock; smaller = zoomed OUT, min 63), deterministic grapple pick (direction chooses the throw row), camera_fixed (locked 512x384 centred view, NO panning - the whole ring always on screen), human_hit_mult (1 = stock; n = damage dealt by a human player x n), exit_ring_hold (frames held into the rope, 32 = ~0.5s), exit_ring_ropes (bitmask right 1 / left 2 / top 4 / bottom 8, 0 = any), exit_ring_climb (1 = climb out through the ropes instead of hopping), video_smooth (1 = bilinear-filtered scaling like MAME's default, 0 = crisp pixels), badge_human (1 = the nP chip stays over every pad-driven man all match, so you always see who you control), badge_cpu (1 = a CPU tag over every CPU / autopilot man)",
      mod_rules_be, mod_rule_labels },
};
TBL_REGISTER(mod_rule_tables)

int eng_mod_rule(int slot)
{
    const int *def = mod_rule_defaults;   /* generated from rules.def with the table - cannot drift */
    uint32_t len = 0;
    const uint8_t *b = tbl_bytes(TBL(game_rules), &len);
    if (slot < 0 || slot >= (int)(sizeof mod_rule_defaults / sizeof mod_rule_defaults[0])) return 0;
    /* a mod layer packed before a rule existed is SHORT: its missing tail
       reads as the stock default, not 0 (human_hit_mult 0 = no damage) */
    {   /* harness: WF_MODRULES="label=value,label=value" overrides (headless tests) */
        static int parsed, ovr[64];
        if (!parsed) {
            const char *e = getenv("WF_MODRULES");
            parsed = 1;
            for (int k = 0; k < 64; k++) ovr[k] = -1;
            while (e && *e) {
                char name[40]; int v, n = 0;
                if (sscanf(e, "%39[^=]=%i%n", name, &v, &n) == 2 && n > 0) {
                    for (int k = 0; mod_rule_labels[k]; k++) if (!strcmp(mod_rule_labels[k], name)) ovr[k] = v;
                    e += n;
                }
                while (*e && *e != ',') e++;
                if (*e == ',') e++;
            }
        }
        if (slot < 64 && ovr[slot] >= 0) return ovr[slot];
    }
    if (!b || (uint32_t)slot * 2u + 2u > len) return def[slot];
    return (int)tbl16(TBL(game_rules), (uint32_t)slot * 2u);
}

/* scale a speed by the mod percentage for this man's driver */
uint16_t eng_mod_speed(const eng_obj *o, unsigned base)
{
    int pct = eng_mod_rule((o->cpu || (o->driver & DRV_AUTOPILOT)) ? MODR_SPEED_CPU
                                                        : MODR_SPEED_HUMAN);
    if (pct == 100 || pct <= 0) return (uint16_t)base;
    base = base * (unsigned)pct / 100u;
    return (uint16_t)(base ? base : 1);
}

/* mod turbo: the level as a percentage (100 = off, 120..200) */
unsigned eng_mod_turbo_pct(void)
{
    int t = eng_mod_rule(MODR_TURBO);
    if (t <= 0) return 100u;
    if (t > 5) t = 5;
    return 100u + 20u * (unsigned)t;
}

/* mod turbo: scale a FINAL walk/run speed (after the package stat, so a
 * skin's own "walk"/"run" numbers get it too) */
uint16_t eng_mod_turbo_speed(unsigned base)
{
    unsigned pct = eng_mod_turbo_pct();
    if (pct == 100u) return (uint16_t)base;
    base = base * pct / 100u;
    return (uint16_t)(base ? base : 1);
}

/* per-frame energy refill (core frame loop, after the object pass) */
void eng_mod_energy_tick(eng_state *st)
{
    int mode = eng_mod_rule(MODR_UNL_ENERGY);
    if (!mode) return;
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active || !o->hp_max) continue;
        if (mode == 1 && (o->cpu || o->input < 0)) continue;   /* humans only */
        if (o->hp < o->hp_max) { o->hp_delta += (int16_t)(o->hp_max - o->hp); o->hp = o->hp_max; }
    }
}

/* ---- MOD random backup run-ins (backup_avg_secs / _max / _stay) ----
 * "ability to have random 'backup' called in-game (just use the rumble
 * ring entry, brawl, and exit rumble, but make it random)" — user
 * 2026-08-24. One backup at a time: a random wrestler not in the match
 * runs down the aisle and climbs in (the rumble entry, move 0x69 with
 * the 0x7430 walk), brawls as a CPU intruder against a random man for
 * backup_stay frames, then takes the eliminated ringside walk-off
 * (move 0x7A: the side-rope climb-out, then up the aisle and gone) and
 * his slot frees. He is never legal: no pin (unless pin_anyone) and no
 * count-out apply. Tag only. The in-ring view's only outside walkway is
 * the aisle at the TOP (the ring law has no floor below the ring), so he
 * enters and leaves the way rumble entrants do. */
void eng_backup_tick(eng_state *st)
{
    static int spawned, stay, slot = -1;
    static int64_t prev = -1;
    int avg = eng_mod_rule(MODR_BACKUP_AVG);
    if (st->frame < prev) { spawned = 0; slot = -1; }     /* new match */
    prev = st->frame;
    { const char *ko = getenv("WF_REF_KO"); if (ko && st->frame == atoi(ko)) eng_ref_knockdown(st); }   /* harness: drop the referee at this frame */
    if (!avg || (st->g161 & 1u)) return;                  /* off / rumble */
    if (slot >= 0) {                                      /* one live backup */
        eng_obj *b = &st->obj[slot];
        if (!b->active) { slot = -1; return; }
        if (st->g161 & 2u) return;                        /* the ringside view: the clock waits, no walk-off from there */
        if (b->backup == 2 && ((b->state & 0xFFu) == ST_STAND || (b->state & 0xFFu) == ST_WALK)) {
            /* LEAVING, and free: inside -> the rope climb-out a player would do
             * (0x7A phases 1-3, grap44 b3, lands standing); outside -> walk to the
             * aisle mouth and up the walkway (0x7A phase 5, backup targets), the
             * slot frees on arrival. Never while he is mid-move or being hit. */
            b->partner = -1; b->mover = 0; b->list = 0;
            if (!(b->role & RF_OUTSIDE)) { b->state = ST_MOVE; b->move_id = 0x7A; b->grap44 = 1u | 8u; }
            else                   { b->state = ST_MOVE; b->move_id = 0x7A; b->grap44 = 5;       b->st_flags |= SF_ELIMINATED; }
            if (eng_dbgsel) fprintf(stderr, "mod: backup w%d heads home (%s)\n", b->wrestler, (b->role & RF_OUTSIDE) ? "up the aisle" : "climbs out first");
            return;
        }
        if ((b->role & RF_OUTSIDE) && ((b->state & 0xFFu) == ST_STAND || (b->state & 0xFFu) == ST_WALK)) {
            /* FREE and OUTSIDE while the RING view shows: stock never has such a
             * man (partners are apron men or 0x68 walkers), so the outside law
             * has no ring wall and his AI walked him under the mat plane
             * ("floats around beyond the ring area", user 2026-08-30) -> the
             * 0x68 walk back in (corner -> aisle -> 0x69 climb) */
            b->state = ST_MOVE; b->move_id = 0x68; b->grap44 = 1; b->partner = -1; b->mover = 0;
            if (eng_dbgsel) fprintf(stderr, "mod: backup w%d outside in the ring view - walks back in\n", b->wrestler);
        }
        if (stay > 0 && --stay == 0) {                    /* the optional timer (rule > 0): just the GOAL */
            b->backup = 2;
            if (eng_dbgsel) fprintf(stderr, "mod: backup w%d will leave (timer)\n", b->wrestler);
        }
        return;
    }
    if (spawned >= eng_mod_rule(MODR_BACKUP_MAX)) return;
    if (st->g161 & 2u) return;                            /* not during the ringside view (the aisle climb froze there) */
    {   const char *at = getenv("WF_BACKUP_AT");           /* harness: spawn at this frame, already at the climb spot */
        if (at) { if (st->frame != atoi(at)) return; }
        else {
            if (st->frame < 300) return;                  /* let the match settle */
            if ((eng_rng() % (uint32_t)(avg * 60)) != 0) return;
        }
    }
    {
        int s = 4, id = -1, tgt = -1, used[12] = {0};
        while (s < ENG_MAX_OBJS && st->obj[s].active) s++;
        if (s >= ENG_MAX_OBJS) return;
        for (int i = 0; i < ENG_MAX_OBJS; i++)
            if (st->obj[i].active && st->obj[i].wrestler < ENG_WS_EXT_MAX)
                used[eng_ws_base(st->obj[i].wrestler)] = 1;   /* a clone occupies its base's art */
        for (int tries = 0; tries < 24 && id < 0; tries++) {
            int c = (int)(eng_rng() % 12u);
            if (!used[c]) id = c;
        }
        for (int tries = 0; tries < 24 && tgt < 0; tries++) {
            int c = (int)(eng_rng() % 4u);
            if (st->obj[c].active && !st->obj[c].apron && !(st->obj[c].role & RF_OUTSIDE)) tgt = c;
        }
        if (id < 0 || tgt < 0) return;
        {
            eng_obj *o = &st->obj[s];
            memset(o, 0, sizeof *o);
            o->active = 1; o->wrestler = (int16_t)id;
            o->partner = -1; o->last_pair = -1; o->teammate = -1; o->rescue = -1;
            o->input = -1; o->cpu = 1; o->driver = 0x80u;
            o->hp = o->hp_max = 0x87;                     /* 0x1081A CPU energy */
            o->ai_b6 |= 0x80u;
            o->role = (uint16_t)(((st->obj[tgt].role & RF_SIDE) ^ 0x80u) | 0x04u);   /* enemy side, NOT legal, OUTSIDE (0xBCB0's
                                          +0x32 b2: the walkway law, not the ring's - without it the ring law
                                          clamped him mid-aisle = "a weird sprite walking around", user 2026-08-30) */
            o->spr = 0; o->cam_mode = 0; o->hold_t = 0; o->ai_bc = 0; o->backup = 1;
            eng_sprite_reclaim_bank((unsigned)id);        /* his palette bank may have been lent out since the bell */
            o->opp = tgt;
            /* the rumble entry (0x7430 arrive): down the aisle, climb at
             * the 0x279 corner (the V401 snap) */
            o->x = 0x350 << 16; o->y = 0x20 << 16; o->z = 0x100 << 16;
            o->state = ST_MOVE; o->move_id = 0x69; o->grap44 = 0x80u;
            o->run_tgt = 0x279; o->tgt_y = 0x100;
            if (getenv("WF_BACKUP_AT")) { o->x = 0x279 << 16; o->y = 0x100 << 16; o->grap44 = 0; }
            slot = s; spawned++;
            stay = eng_mod_rule(MODR_BACKUP_STAY);        /* 0 = no timer: only a throw over the top rope sends him home */
            if (eng_dbgsel) fprintf(stderr, "mod: BACKUP w%d runs in (slot %d, target o%d)\n", id, s, tgt);
        }
    }
}

/* ---- MODE DESCRIPTOR (phase 3 of the mod plan, user 2026-08-24) ----
 * What KIND of match this is, as data — all stock values by default, so
 * the stock pak plays byte-for-byte stock. A mod flips rows per profile:
 * singles (team_size 1), battle royale (rumble_bell 6 + live cap 8 +
 * fast spawns), hardcore (weapons_mode 2), no-countout, no-pin... */
/* mode_rules: from src/rules.def */
#define GAME_RULE(e, l, d, h)
#define MODE_RULE(e, l, d, h) TBL_BE16(d),
static const uint8_t mode_rules_be[] = {
#include "rules.def"
};
#undef GAME_RULE
#undef MODE_RULE
#define GAME_RULE(e, l, d, h)
#define MODE_RULE(e, l, d, h) l,
static const char *const mode_rule_labels[] = {
#include "rules.def"
    NULL };
#undef GAME_RULE
#undef MODE_RULE
#define GAME_RULE(e, l, d, h)
#define MODE_RULE(e, l, d, h) d,
static const int mode_rule_defaults[] = {
#include "rules.def"
};
#undef GAME_RULE
#undef MODE_RULE
#define GAME_RULE(e, l, d, h)
#define MODE_RULE(e, l, d, h) { l, h },
static const struct { const char *label, *help; } mode_rule_help[] = {
#include "rules.def"
    { NULL, NULL } };
#undef GAME_RULE
#undef MODE_RULE
static const tbl_def mode_rule_tables[] = {
    { "mode_rules", "rules", TBL_SYNTH, sizeof mode_rules_be, TK_U16, 1,
      "the MATCH MODE as data, stock values by default: team size (1 = singles), pin/referee/count-out on-off, weapons mode (0 none / 1 stock / 2 hardcore carry-in), time limit (BCD minutes, 0 = none), rumble bell count / live cap / spawn interval, tag run-ins on-off, side-B team size (0 = same; teams of up to 3, tags rotate), ELIMINATION rules (a fall eliminates the pinned man, the match runs until a side is empty), mirror picks (duplicates allowed on the select screen - the second copy gets an alternate palette), select_extended (SELECT PLAYERS becomes a 2x6 grid: the LEGION OF DOOM join as positions 10/11 and B2 on a cell cycles a registered clone of that wrestler)",
      mode_rules_be, mode_rule_labels },
};
TBL_REGISTER(mode_rule_tables)

int eng_mode_rule(int slot)
{
    if (!tbl_bytes(TBL(mode_rules), NULL)) {
        return (slot >= 0 && slot < (int)(sizeof mode_rule_defaults / sizeof mode_rule_defaults[0])) ? mode_rule_defaults[slot] : 0;
    }
    return (int)tbl16(TBL(mode_rules), (uint32_t)slot * 2u);
}

/* the editor's help text for a rule row, from rules.def ("" if unknown) */
const char *eng_rule_help(const char *tbl, const char *label)
{
    if (!strcmp(tbl, "game_rules")) { for (int i = 0; mod_rule_help[i].label; i++) if (!strcmp(mod_rule_help[i].label, label)) return mod_rule_help[i].help; }
    if (!strcmp(tbl, "mode_rules")) { for (int i = 0; mode_rule_help[i].label; i++) if (!strcmp(mode_rule_help[i].label, label)) return mode_rule_help[i].help; }
    return "";
}

/* wfengine --rules-doc: the two rule tables as markdown (docs/modding.md) */
int eng_rules_doc(void)
{
    printf("| mode_rules row | stock | meaning |\n|---|---|---|\n");
    for (int i = 0; mode_rule_help[i].label; i++) printf("| %s | %d | %s |\n", mode_rule_help[i].label, mode_rule_defaults[i], mode_rule_help[i].help);
    printf("\n| game_rules row | stock | meaning |\n|---|---|---|\n");
    for (int i = 0; mod_rule_help[i].label; i++) printf("| %s | %d | %s |\n", mod_rule_help[i].label, mod_rule_defaults[i], mod_rule_help[i].help);
    return 0;
}
