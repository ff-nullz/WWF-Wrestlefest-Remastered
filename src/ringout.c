/* Ring-out camera scene — transcription of ROM 0xF98C (scene switch in
 * and out of the ringside view), the 0x204FA legal-pair scan and the
 * CPU ringside walker (0x1C6DC / 0x1D74A). Spec: docs/engine-specs/
 * ringout-scene.md; implementation notes: docs/engine-specs/ringout-impl.md.
 *
 * Trigger word $1C1678: 0x8000 (+ $1C1679 = faller facing) written by
 * move 0x68 when a LEGAL man lands outside (anim.c handler_outside,
 * 0x198A0); 0xC000 by the end of climb-in 0x69 in the ringside scene
 * when both legal men are back in (0x19B2E). Called once per frame from
 * the pass-2 wrapper (0xF564), after the tie-up scan.
 */
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include <stdio.h>
extern int eng_dbgsel;

#define TAB_RINGOUT_SCENE 0xFA00u      /* byte per stage: 02 06 00 06 02 06 02 02 06 02 */
#define TAB_RETURN_SCENE  0xFA0Au      /* byte per stage: 00 05 00 05 00 05 00 00 05 00 */

/* docs/adr-001, group base/scene: the two stage -> scene byte tables 0xF9C2
 * (enter) and 0xF9E0 (leave) index with the stage word $1C0162; ten stages,
 * the second table starts at 0xFA0A and the code resumes at 0xFA14. */
static const tbl_def ringout_tables[] = {
    { "ringout_scene_by_stage", "base/scene", 0xFA00, 10, TK_U8, 1,
      "0xF9C2: ringside scene word low byte ($1C007F) per stage ($1C0162) when a legal man lands outside" },
    { "return_scene_by_stage",  "base/scene", 0xFA0A, 10, TK_U8, 1,
      "0xF9E0: scene to return to per stage when both legal men are back in the ring" },
};
TBL_REGISTER(ringout_tables)

/* Ring-out / count-out constants — the ROM immediates as a synthetic table
 * "ringout_rules" (group rules, order = the RO_* enum in engine.h, which
 * carries the PC of each): exported to data/tables/rules/, packed, moddable
 * (ADR-001). The array below is the source of truth and the fallback. */
static const uint8_t ringout_rules_be[RO_RULE_COUNT * 2] = {
    TBL_BE16(0x50),     /* RO_FRAMES_PER_COUNT   0x1FDF8 */
    TBL_BE16(0x11),     /* RO_WARN_COUNT         0x1FE38 */
    TBL_BE16(0x14),     /* RO_RESOLVE_COUNT      0x1FE0C */
    TBL_BE16(0x298),    /* RO_REF_X_MIN          0x1FD92 */
    TBL_BE16(0x390),    /* RO_REF_X_MAX          0x1FD9E */
    TBL_BE16(0x340),    /* RO_REF_ENTRY_X        0xFB94  */
    TBL_BE16(0x160),    /* RO_REF_ENTRY_Y        0xFB9C  */
    TBL_BE16(0x09),     /* RO_POSE_FIRST         0x1FD80 */
    TBL_BE16(0x0C),     /* RO_POSE_LAST          0x1FDB6 */
    TBL_BE16(0x08),     /* RO_POSE_FRAMES        0x1FD7A */
    TBL_BE16(0x04),     /* RO_POSE_LAST_EXTRA    0x1FDD8 */
    TBL_BE16(0x65),     /* RO_WARN_YM            0x1FD84 ($3165, low byte) */
    TBL_BE16(0x1000),   /* RO_FREEZE_T22         0x1FE5A */
    TBL_BE16(0x08),     /* RO_FALLER_DAMAGE      0xFAC0  */
    TBL_BE16(0x744),    /* RO_DIGIT_WIN_TENS     0x2069E */
    TBL_BE16(0x748),    /* RO_DIGIT_WIN_ONES     0x20692 */
    TBL_BE16(0x8003),   /* RO_RESULT_OUT         0x1FEC2 */
    TBL_BE16(0x8002),   /* RO_RESULT_IN          0x1FEB2 */
    TBL_BE16(0x8005),   /* RO_RESULT_DOUBLE      0x1FE76 */
    TBL_BE16(0),        /* RO_ILLEGAL_THROW_OUT  engine  */
};
static const char *const ringout_rule_labels[] = {
    "frames_per_count", "warn_count", "resolve_count", "ref_x_min", "ref_x_max", "ref_entry_x", "ref_entry_y",
    "pose_first", "pose_last", "pose_frames", "pose_last_extra", "warn_ym", "freeze_t22", "faller_damage",
    "digit_win_tens", "digit_win_ones", "result_out", "result_in", "result_double", "illegal_throw_out", NULL };
static const tbl_def ringout_rule_tables[] = {
    { "ringout_rules", "rules", TBL_SYNTH, sizeof ringout_rules_be, TK_U16, 1,
      "engine scalars (ROM immediates) of the count-out: order = RO_* enum in engine.h (PC per row there)", ringout_rules_be, ringout_rule_labels },
};
TBL_REGISTER(ringout_rule_tables)

int ringout_rule(int idx)
{
    if (idx < 0 || idx >= RO_RULE_COUNT) return 0;
    {   /* a pak packed before a row was added is SHORT: compiled default for the tail */
        uint32_t n = 0;
        if (tbl_bytes(TBL(ringout_rules), &n) && n >= ((uint32_t)idx + 1u) * 2u)
            return (int)tbl16(TBL(ringout_rules), (uint32_t)idx * 2u);
    }
    return (ringout_rules_be[idx * 2] << 8) | ringout_rules_be[idx * 2 + 1];
}

/* 0x204FA: first two live slots with +0x33 b0 (the legal men). */
int eng_legal_pair(eng_state *st, eng_obj **a, eng_obj **b)
{
    *a = *b = 0;
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active || !(o->role & RF_LEGAL)) continue;
        if (!*a) *a = o; else if (!*b) { *b = o; break; }
    }
    if (!*a) return 0;
    if (!*b) *b = *a;                  /* singles: the ROM scan leaves A3 at
                                          the last slot probed; pairing the
                                          lone man with himself keeps every
                                          b2 test sane (TODO EXACT singles) */
    return 1;
}

/* 0xFA44 / 0xFBEA per-object clears, shared by both directions. */
static void clear_links(eng_obj *o)
{
    o->off_x = 0; o->off_y = 0;        /* +0x18 / +0x1A */
    o->list = 0;                       /* +0x12 */
    o->sub = 0;                        /* +0xAE */
    o->tag_flags = 0;                        /* +0x34 */
    o->partner = -1;                   /* +0x26.l */
    o->atk = 0;                        /* +0x4C */
    o->grap44 = 0;
    o->weapon_w = 0; o->wobj = 0;           /* 0xFA58/0xFC0A clr.w +0x74 (weapon) */
    if (o->cpu) o->ai_t = 0;           /* +0xB4 (CPU only) */
}

void eng_ringout_switch(eng_state *st)
{
    unsigned stage = (unsigned)(st->stage & 0xF);

    if (!(st->ringout_trig & 0x8000u)) return;                  /* F98C btst #7 */
    /* F998-F9B2: sprite/HUD latches, tilemap work RAM + VRAM clears —
     * the renderer recomposes on the scene-word change (render.c). */
    st->corner_bits = 0;                                        /* clr.l $1C1670 */
    if (!(st->ringout_trig & 0x4000u)) {
        /* ---- ENTER 0xF9C2 ---- */
        int face_right = (st->ringout_face & 0x8000u) != 0;     /* $1C1679 b7 = faces right (engine 0x8000):
                                                                   oracle fuzz2 f2524: faller lying left, fc=00 -> cam 0x190 */
        st->scene = tbl_ra8(TAB_RINGOUT_SCENE + (stage < 10 ? stage : 0));
        st->g161 |= 2u;                                         /* F9D6 ringside scene showing */
        eng_weapons_scene_enter(st);                            /* FA14/FA26: the ringside
                                                                   weapons $1C0F1C/$1C1028
                                                                   z=0x100, +0x1C=3 (weapon.c) */
        for (int i = 0; i < ENG_MAX_OBJS; i++) {                /* FA38 loop 0x250E */
            eng_obj *o = &st->obj[i];
            uint16_t f74; int wobj, keep;
            if (!o->active) continue;
            f74 = o->weapon_w; wobj = o->wobj;
            keep = eng_mode_rule(MODE_WEAPONS) == 2 && (f74 & 0x8000u) && wobj >= 1 && wobj <= ENG_WEAPONS;
            clear_links(o);                                     /* 0xFA58 clears +0x74 too */
            if (keep) {                                         /* HARDCORE: a weapon carried OUT (the exit_ring_climb
                                          mod: he climbed out holding it) survives the switch as a
                                          ringside slot ("it disappears in the transition", user 2026-08-30) */
                eng_weapon *w = &st->wpn[wobj - 1];
                o->weapon_w = f74; o->wobj = wobj;
                w->inside = 0; w->state = ST_WALK; w->holder = 1 + i;
                if (eng_dbgsel) fprintf(stderr, "wpn: P%d carries weapon %d out to ringside (hardcore)\n", i + 1, wobj - 1);
            }
            if (o->backup) {                                    /* a mod BACKUP is never teleported (user
                                          2026-08-30): outside he stays where he stands; inside he
                                          climbs out over the side rope to the brawl (0x7A phases
                                          1-3 with grap44 b3, stopping on the floor - anim.c) */
                o->st_flags &= 0x00C3u; o->ai_b5 = 0; o->apron = 0;
                if (!(o->role & RF_OUTSIDE)) {
                    o->state = ST_MOVE; o->move_id = 0x7A; o->grap44 = 1u | 8u; o->partner = -1;
                    if (eng_dbgsel) fprintf(stderr, "mod: backup P%d climbs out to the ringside brawl\n", i + 1);
                }
                continue;
            }
            if ((o->role & RF_OUTSIDE) && (o->role & RF_LEGAL) && (o->move_id & 0xFFu) == 0x6B) {
                /* MOD exit_ring_climb: the legal man CLIMBED out and stands
                 * at ringside - the camera treats him as the faller, he
                 * re-inits 0x6B (walk to the corner spot) instead of lying */
                st->cam_y = 0x200;
                st->cam_x = face_right ? 0x350 : 0x190;
                o->st_flags |= SF_LAW_EXEMPT;
                o->y = 0x164 << 16; o->z = 0x100 << 16;
                o->x = (face_right ? 0x428 : 0x200) << 16;
                o->state = ST_MOVE; o->move_id = 0x6B;
            } else if ((o->role & RF_OUTSIDE) && (o->move_id & 0xFFu) == 0x6A) {   /* FA78: THE FALLER */
                st->cam_y = 0x200;                              /* FA8A $1C1806 */
                st->cam_x = face_right ? 0x350 : 0x190;         /* $1C1804 */
                o->state = ST_MOVE; o->move_id = 0x6A;                /* FAAE re-init (the +0x1C latch was cleared) */
                o->st_flags |= SF_LAW_EXEMPT;                                /* FABA probe-exempt */
                o->dmg = (uint16_t)ringout_rule(RO_FALLER_DAMAGE);   /* FAC0 */
            } else if (!(o->role & RF_LEGAL)) {                     /* FACA: NON-LEGAL man -> ringside 0x6B */
                o->st_flags &= 0x00C3u;
                o->role |= RF_OUTSIDE; o->st_flags |= SF_LAW_EXEMPT;
                o->state = ST_MOVE; o->move_id = 0x6B;
                o->y = 0x164 << 16; o->z = 0x100 << 16;
                if (o->role & RF_SIDE) { o->x = 0x428 << 16; o->facing = 0; }         /* FB02: side 1 at 0x428, bclr #7 = faces left */
                else                { o->x = 0x200 << 16; o->facing = 0x8000u; }   /* side 0 at 0x200, bset #7 = faces right */
                o->apron = 0;                                   /* engine: off the apron line while the view lasts */
                o->ai_b5 = 0;
            } else {                                            /* FB2A: the OTHER LEGAL man */
                o->st_flags &= 0x00C3u;
                o->state = ST_STAND;
                o->y = 0x160 << 16;
                o->x = (face_right ? 0x3A0 : 0x2A0) << 16;      /* FB3C: on the faller's side */
                o->ai_b5 = 0;
            }
        }
        /* FB5A-FB8A: vblank wait, 0x26E66 compose, palettes — render.c */
        st->ref.x = ringout_rule(RO_REF_ENTRY_X) << 16;         /* FB94 referee teleport */
        st->ref.y = ringout_rule(RO_REF_ENTRY_Y) << 16;         /* FB9C */
        st->ref.sm = 3; st->ref.vis_init = 0; st->ref.target = -1;   /* +0x20 = $8003 */
        st->ref.speed = 0;
        st->ringout_trig = 0;                                   /* FBAC */
        eng_sound(0x80);                                        /* FBBA YM 0x80 (music change) */
        return;
    }
    /* ---- LEAVE 0xFBC8 ---- */
    st->ringout_trig = 0;
    st->scene = tbl_ra8(TAB_RETURN_SCENE + (stage < 10 ? stage : 0));   /* F9E0 */
    st->g161 &= (uint8_t)~2u;                                   /* F9F4 */
    st->cam_x = 0x1E0; st->cam_y = 0x230;                       /* FBC8 scroll target */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {                    /* FBDE loop */
        eng_obj *o = &st->obj[i];
        uint16_t f74; int wobj, keep;
        if (!o->active) continue;
        f74 = o->weapon_w; wobj = o->wobj;
        keep = eng_mode_rule(MODE_WEAPONS) == 2 && (f74 & 0x8000u) && wobj >= 1 && wobj <= ENG_WEAPONS;
        clear_links(o);                                         /* 0xFC0A clears +0x74 too */
        if (keep) {                                             /* HARDCORE (weapons_mode 2): the carry survives
                                          the return - the man climbed in holding it and the
                                          ROM's clear "teleported him in without it" (user
                                          2026-08-30); the slot becomes an IN-RING one so it
                                          ticks, draws and lands on the mat from here */
            eng_weapon *w = &st->wpn[wobj - 1];
            o->weapon_w = f74; o->wobj = wobj;
            w->inside = 1; w->state = ST_WALK; w->holder = 1 + i;
            if (eng_dbgsel) fprintf(stderr, "wpn: P%d carries weapon %d back into the ring (hardcore)\n", i + 1, wobj - 1);
        }
        o->st_flags &= 0x00C3u;
        o->ai_b5 = 0;
        if (o->role & RF_LEGAL) {                                   /* FC2A legal */
            o->state = ST_STAND; o->sub = 0;
            o->y = 0x150 << 16; o->z = 0x140 << 16;
            o->x = ((o->role & RF_SIDE) ? 0x2C0 : 0x240) << 16;
            o->role &= (uint16_t)~0x04u;
        } else if (o->backup) {                                 /* a mod BACKUP is never teleported (user
                                          2026-08-30): inside he keeps brawling where he is; outside
                                          he walks to the aisle spot and climbs back in (the 0x69
                                          pre-walk) - never the apron follow sub, which left him
                                          floating (he has no legal man to follow) */
            o->apron = 0; o->partner = -1;
            if ((o->role & RF_OUTSIDE) && o->backup != 2) {   /* a LEAVING backup stays outside (modrules.c walks him home) */
                o->state = ST_MOVE; o->move_id = 0x69; o->grap44 = 0x80u; o->sub = 0;
                o->run_tgt = 0x279; o->tgt_y = 0x100; o->z = 0x100 << 16;
                if (eng_dbgsel) fprintf(stderr, "mod: backup P%d walks round to climb back in\n", i + 1);
            }
        } else {                                                /* FC5A non-legal: state 1 sub 1 (follow) */
            o->state = ST_WALK; o->sub = 1;
            o->y = 0x160 << 16; o->z = 0x140 << 16;
            o->role &= (uint16_t)~0x04u;   /* oracle fuzz2 f2880: b2 clear on the partners after the return */
            o->apron = 1;                  /* engine: back on the apron line (x re-pinned by tag.c) */
        }
    }
    eng_ref_digit_wipe();                                       /* FCBA 0x206FE */
    /* FCC0 0x1004A ring hardware re-create: the rope objects are static
     * in ringhw.c and simply resume drawing once the scene is back. */
    st->ref.x = 0x280 << 16; st->ref.y = 0x198 << 16;           /* FCC6 */
    st->ref.sm = 5; st->ref.vis_init = 0; st->ref.cue_flags = 0; st->ref.target = -1;   /* +0x20 = $8000 -> SM0 falls into 5 */
    eng_sound(0x20);                                            /* FCE6 YM 0x20 (music back) */
    st->ann_name = 0; st->ann_active = 0;                       /* FCF0 clr.w $1C15D4 */
}

/* Ringside brawl targeting (user-verified stock behaviour, 2026-08-23;
 * TODO EXACT the ROM site — likely the 0x1C99E arms' +0x7A re-points
 * generalised): while the ring-out view shows, an OUTSIDE man targets
 * the nearest ENEMY who is ALSO outside — the tagged-in man inside the
 * ring never counts. face_opponent (0x10BE8) then faces him; running
 * (run + direction) is free, as in the ring. */
void eng_ringside_retarget(eng_state *st)
{
    if (!(st->g161 & 2u) || (st->g161 & 1u)) return;
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        int best = -1; int32_t bd = 0x7FFFFFFF;
        if (!o->active || !(o->role & RF_OUTSIDE)) continue;
        for (int j = 0; j < ENG_MAX_OBJS; j++) {
            const eng_obj *p = &st->obj[j];
            int32_t dx, dy;
            if (j == i || !p->active || !(p->role & RF_OUTSIDE)) continue;
            if (!((p->role ^ o->role) & 0x80u)) continue;    /* enemies only */
            dx = (p->x >> 16) - (o->x >> 16); if (dx < 0) dx = -dx;
            dy = (p->y >> 16) - (o->y >> 16); if (dy < 0) dy = -dy;
            if (dx + dy < bd) { bd = dx + dy; best = j; }
        }
        if (best >= 0) o->opp = best;
    }
}

/* CPU man at ringside (ringside scene showing). 0x1C6DC / 0x1EBF4: once
 * the count reaches 10 a legal CPU man with +0xB5 b5 clear is sent home —
 * state 1 sub 0x0A walks to (0x310,0x138) then runs move 0x69. The
 * oracle (fuzz2 f2664-2668) shows the CPU taking the same walk as soon
 * as his 0x6B arrival leaves him standing, so the standing CPU legal man
 * takes it at any count (TODO EXACT: the 0x1D74A ringside walker that
 * issues it before 10). The walk itself is carried by move 0x69's
 * pre-walk phase (anim.c handler_climbin, grap44 b7). */
void eng_ringside_ai(eng_state *st, eng_obj *o)
{
    unsigned state = o->state & 0xFFu;
    if (!(st->g161 & 2u) || !(o->role & RF_OUTSIDE) || !(o->role & RF_LEGAL)) return;
    if (!(o->cpu || (o->driver & DRV_AUTOPILOT) || o->input < 0)) return;   /* nobody's pad = autopilot,
                                                    whichever hand-off dropped the bit */
    if (o->ai_b5 & 0x20u) return;
    if (state != ST_STAND && state != ST_WALK) return;
    if (!(o->state & 0x8000u)) return;
    o->ai_b5 |= 0x20u;                                          /* 1C6E6 bset #5,+0xB5 */
    o->run_tgt = 0x310; o->tgt_y = 0x138;                       /* 1C6F2 (+0xBE,+0xC0) */
    o->state = ST_MOVE; o->move_id = 0x69; o->grap44 = 0x80;          /* 1C6EC move 0x69 via the state-1 sub 0x0A walk */
}
