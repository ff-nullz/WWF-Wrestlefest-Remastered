/* Ringside weapons — the ring STAIRS and the crate/BOX that can be
 * picked up and swung while outside the ring during the ring-out camera
 * scene. Transcription of:
 *   0xFFD2  spawn at match init (called from 0xCF4, next to the ring
 *           hardware create 0x1004A): object slots $1C0F1C / $1C1028
 *           (work-RAM slots 9/10, stride 0x10C), sprite sheet byte
 *           +0x03 = 0x0F, +0x74 = type 0/1, spots (0x40D,0x140) and
 *           (0x330,0x119), z 0x100, +0x1C = 0 (hidden).
 *   0xFA14/0xFA26  ring-out scene enter: both weapons z = 0x100,
 *           +0x1C = 3 (resting on the ringside floor).
 *   0xFDEE  per-frame machine, gated on the ringside scene ($1C0161
 *           b1); jump table 0xFE22 on +0x1C & 0xF:
 *     0 0xFE36  hidden: sprite word = 0xFFFF.
 *     1 0xFE3E  CARRIED: sprite hidden (the carry is baked into the
 *           holder's own poses 0x72-0x83); tracks the holder — facing
 *           copied, x/y from him, z + 0x50, x +/- 0x20 toward facing.
 *     2 0xFE82  TOSSED: init bset b7, launcher 0x258E class 0x10
 *           ({vx -64, vz 256, grav 72} row of knockback_launch 0x25CA),
 *           pose = type*3, +0x12 = 0, +0x76 cleared; per tick the
 *           motion step (0x2208) + the ringside bounds probe with
 *           +/-0x20 lookahead (0xFEBC/0xFECE 0x280DC) whose banked
 *           push-back lands it (+0x37 b4 -> state 4). Simplified here
 *           to the same velocity/gravity integration with the ringside
 *           floor plane z = 0x100 (TODO EXACT: the +/-0x20 wall probes).
 *     3 0xFF1E  ON THE FLOOR: pose = type*3 | facing, +0x12 = 2,
 *           +0x76 cleared every tick (the pickup reservation is
 *           frame-transient).
 *     4 0xFF54  TUMBLE: +0x12 = 2, every 0x10 ticks +0x24 += 1 and the
 *           pose alternates type*3+1 / type*3+2 (+0x25 b0); at
 *           +0x24 == 4 -> state 3 (0xFFBE).
 *   0xF0BA  the human PICKUP press consumer (jsr'd from the press
 *           dispatcher 0xDEBC, before the category chain 0xDF3A):
 *           ringside scene, not holding (+0x74 b7), OUTSIDE (+0x33 b2),
 *           state 0/1; 0xF106 probes both weapons (+0x76 == 0, +0x1D ==
 *           3, |dy| < 0x20, |dx| < 0x20) and links both sides (0xF164/
 *           0xF168); hit -> state 5 move 0x70 (0xF0F4).
 * The moves themselves live in anim.c (0x19EA0 pickup 0x70, 0x15A20
 * swing 0x1E/0x1F); the CPU arm is ai.c (0x1C70C scan / 0x1CD2C swing);
 * the drops are the climb-in 0x19A66, the victim hit drop 0x24E02 and
 * the swing's own release 0x15A72/0x15ABE. */
#include "engine.h"
#include "tbl.h"
#include <stdio.h>
extern int eng_dbgsel;

/* The 0xFFD2 spawn immediates as a synthetic rules table (ADR-001) so
 * the spots stay moddable like the other engine scalars. */
static const uint8_t weapon_rules_be[] = {
    TBL_BE16(0x40D),    /* 0: 0xFFEE  weapon 0 x */
    TBL_BE16(0x140),    /* 1: 0xFFF4  weapon 0 y */
    TBL_BE16(0x330),    /* 2: 0x10022 weapon 1 x */
    TBL_BE16(0x119),    /* 3: 0x10028 weapon 1 y */
    TBL_BE16(0x100),    /* 4: 0xFFFA/0x1002E/0xFA1A z (spawn + scene enter) */
    TBL_BE16(0x20),     /* 5: 0xF14A/0xF15E pickup |dx|,|dy| window */
    TBL_BE16(0x50),     /* 6: 0xFE68 carried z offset above the holder */
    TBL_BE16(0x20),     /* 7: 0xFE6E carried x offset toward the facing */
    TBL_BE16(0x10),     /* 8: 0xFF72 tumble tick */
    TBL_BE16(0x04),     /* 9: 0xFF86 tumble steps before resting */
};
static const char *const weapon_rule_labels[] = {
    "w0_x", "w0_y", "w1_x", "w1_y", "floor_z", "pickup_range",
    "carry_dz", "carry_dx", "tumble_tick", "tumble_steps", NULL };
static const tbl_def weapon_tables[] = {
    { "weapon_rules", "rules", TBL_SYNTH, sizeof weapon_rules_be, TK_U16, 1,
      "engine scalars (ROM immediates) of the ringside weapons: spawn spots 0xFFD2, pickup window 0xF12E, carry offsets 0xFE3E, tumble 0xFF54", weapon_rules_be, weapon_rule_labels },
};
TBL_REGISTER(weapon_tables)

static int wrule(int idx)
{
    if (tbl_bytes(TBL(weapon_rules), NULL))
        return (int)tbl16(TBL(weapon_rules), (uint32_t)idx * 2u);
    return (weapon_rules_be[idx * 2] << 8) | weapon_rules_be[idx * 2 + 1];
}
/* per-TYPE rules row from the weapon pak (carry_dz 6, carry_dx 7,
 * tumble_steps 9), the shared scalar when the type has none */
static int wrule_t(const eng_weapon *w, int idx)
{
    int f = idx == 6 ? 0 : idx == 7 ? 1 : idx == 9 ? 2 : -1, v = f >= 0 ? eng_wpn_rule(w->type, f) : 0;
    return v ? v : wrule(idx);
}

/* 0xFFD2 — create both weapon objects at match init (jsr from 0xCF4). */
void eng_weapons_spawn(eng_state *st)
{
    for (int k = 0; k < ENG_WEAPONS; k++) {
        eng_weapon *w = &st->wpn[k];
        int t = eng_wpn_spawn_type(k), sx2 = 0, sy2 = 0, ins = 0;
        int placed = eng_wpn_slot(k, &sx2, &sy2, &ins);
        if (!placed && k < 2) { sx2 = wrule(k ? 2 : 0); sy2 = wrule(k ? 3 : 1); placed = 1; }   /* ROM spots 0x40D/0x330 */
        if (!placed || t == 0xFF) { w->active = 0; continue; }   /* empty slot (user 2026-08-28) */
        w->active = 1;                             /* +0x00 = 0x8000 */
        w->inside = (uint8_t)ins;
        w->type = (uint8_t)t;                      /* +0x74 (the profile's spawn list; stock: slot k) */
        w->facing = 0;                             /* +0x2E = 0 */
        w->x = sx2 << 16;
        w->y = sy2 << 16;
        w->z = (ins ? 0x140 : wrule(4)) << 16;     /* mat / ringside floor */
        w->state = (uint16_t)(ins ? 3 : 0);        /* inside: on the mat from the bell;
                                                      outside: hidden until the ring-out scene */
        w->spr = 0xFFFFu;
        w->holder = 0;
        w->list = 2;
        w->vx = w->vz = 0; w->grav = 0;
        w->t22 = w->t24 = 0;
    }
}

/* 0xFA14/0xFA26 — the ring-out scene enter places both on the floor. */
void eng_weapons_scene_enter(eng_state *st)
{
    for (int k = 0; k < ENG_WEAPONS; k++) {
        if (!st->wpn[k].active || st->wpn[k].inside) continue;   /* inside slots live already */
        st->wpn[k].z = wrule(4) << 16;             /* +0x0E = 0x100 */
        st->wpn[k].state = ST_SKID;                      /* +0x1C = 3 (b7 clear: re-init) */
    }
}

/* The 0x24E0E drop shape shared by the climb-in 0x19A70, the victim hit
 * drop 0x24E02 and the turnbuckle drop: the man lets go, the weapon
 * falls where it is (state 2 re-launches it from the carry spot). */
void eng_weapon_drop(eng_state *st, eng_obj *o)
{
    if (!(o->weapon_w & WPN_HELD)) return;               /* btst #7,(+0x74) */
    if (o->wobj >= 1 && o->wobj <= ENG_WEAPONS) {
        eng_weapon *w = &st->wpn[o->wobj - 1];
        w->state = ST_RUN;                              /* move.w #2,(+0x1C,A3) */
        w->holder = 0;
        if (eng_dbgsel)
            fprintf(stderr, "wpn: P%d drops weapon %d at (%x,%x)\n",
                    (int)(o - st->obj) + 1, o->wobj - 1,
                    (unsigned)(w->x >> 16), (unsigned)(w->y >> 16));
    }
    o->weapon_w = 0;                                    /* clr.w (+0x74) */
    o->wobj = 0;                                   /* clr.l (+0x76) */
}

/* 0xF106/0xF12E — pickup probe (both weapons), links both sides on a
 * hit. Returns 1 and starts move 0x70 (0xF0EE/0xF0F4). Gates 0xF0BA:
 * ringside scene, not holding, outside, state 0/1 — checked by the
 * caller (core.c walk_logic) exactly as the press dispatcher does. */
int eng_weapon_pickup_try(eng_state *st, eng_obj *o)
{
    int rng = wrule(5);
    for (int k = 0; k < ENG_WEAPONS; k++) {        /* $1C0F1C first (0xF10A) */
        eng_weapon *w = &st->wpn[k];
        int dx, dy;
        if (!w->active || w->holder) continue;     /* tst.l (+0x76,A1) */
        if ((w->state & 0xFFu) != ST_SKID) continue;     /* cmpi.b #3,(+0x1D,A1) */
        if (w->inside ? (o->role & RF_OUTSIDE) != 0      /* an INSIDE slot needs the man in the
                                                      ring; an outside one keeps the stock
                                                      gates (ringside scene + outside man) */
                      : (!(st->g161 & 2u) || !(o->role & RF_OUTSIDE)))
            continue;
        dy = (int)(w->y >> 16) - (int)(o->y >> 16);
        if (dy < 0) dy = -dy;
        if (dy >= rng) continue;                   /* 0xF14A (cmpi.b in ROM) */
        dx = (int)(w->x >> 16) - (int)(o->x >> 16);
        if (dx < 0) dx = -dx;
        if (dx >= rng) continue;                   /* 0xF15E */
        w->holder = 1 + (int)(o - st->obj);        /* 0xF164 reservation */
        o->wobj = 1 + k;                           /* 0xF168 */
        o->state = ST_MOVE; o->move_id = 0x70;           /* 0xF0EE/0xF0F4 */
        o->grap44 = 0;
        if (eng_dbgsel)
            fprintf(stderr, "wpn: P%d picks up weapon %d (type %u)\n",
                    (int)(o - st->obj) + 1, k, w->type);
        return 1;
    }
    return 0;
}

/* 0xFDEE — per-frame machine, ringside scene only. */
void eng_weapons_tick(eng_state *st)
{
    for (int k = 0; k < ENG_WEAPONS; k++) {
        eng_weapon *w = &st->wpn[k];
        if (!w->active) continue;
        if (!(st->g161 & 2u) && !w->inside         /* 0xFDEE ran in the ringside scene only;
                                                      INSIDE slots (and anything airborne or
                                                      carried) tick in the normal scene too */
            && (w->state & 0xFu) != 1 && (w->state & 0xFu) != 2)
            continue;
        switch (w->state & 0xFu) {                 /* 0xFE10 andi #$F, table 0xFE22 */
        case 0:                                    /* 0xFE36 hidden */
        default:
            w->spr = 0xFFFFu;
            break;
        case 1: {                                  /* 0xFE3E carried */
            eng_obj *h;
            w->spr = 0xFFFFu;                      /* 0xFE46 (b7 never set here:
                                                      the carry is drawn by the
                                                      holder's own poses) */
            if (w->holder < 1 || w->holder > ENG_MAX_OBJS) break;
            h = &st->obj[w->holder - 1];
            w->facing = h->facing;                 /* 0xFE50 */
            w->x = h->x; w->y = h->y;              /* 0xFE56/0xFE5C */
            w->z = h->z + (wrule_t(w, 6) << 16);        /* 0xFE62/0xFE68 +0x50 */
            w->x += (w->facing & 0x8000u) ? (wrule_t(w, 7) << 16)
                                          : -(wrule_t(w, 7) << 16);   /* 0xFE6E-0xFE7C */
            break; }
        case 2:                                    /* 0xFE82 tossed */
            if (!(w->state & 0x8000u)) {
                w->state |= 0x8000u;               /* 0xFE8A bset #7 */
                /* 0xFE90 jsr 0x258E class 0x10: knockback_launch row
                 * {vx, vz, grav}, vx negated when facing left. */
                w->vx = (int16_t)tbl16(TBL(knockback_launch), 0x10u * 6u);
                w->vz = (int16_t)tbl16(TBL(knockback_launch), 0x10u * 6u + 2u);
                w->grav = (uint16_t)tbl16(TBL(knockback_launch), 0x10u * 6u + 4u);
                if (w->facing & 0x8000u) w->vx = (int16_t)-w->vx;   /* same rule as anim.c knockback (0x25BA) */
                w->spr = (uint16_t)((w->type * 3u) | (w->facing & 0x8000u));   /* 0xFE9A-0xFEA6 */
                w->list = 0;                       /* 0xFEAC +0x12 = 0 */
                w->holder = 0;                     /* 0xFEB2 clr.l +0x76 */
            }
            /* 0xFEB6 jsr 0x2208 (mode-2 velocity) + the 0x280DC probes
             * whose banked push lands it. TODO EXACT: the +/-0x20 wall
             * probes 0xFEBC/0xFECE — only the floor plane is applied. */
            w->x += ((int32_t)w->vx) << 8;
            w->z += ((int32_t)w->vz) << 8;
            w->vz = (int16_t)((uint16_t)w->vz - w->grav);
            { int fz = w->inside ? 0x140 : wrule(4);
            if ((w->z >> 16) <= fz) {              /* landed (+0x37 b4) */
                w->z = fz << 16;                   /* the push snaps to the plane */
                w->state = ST_REACT;                      /* 0xFF06 */
                w->vx = w->vz = 0;
                if (eng_dbgsel)
                    fprintf(stderr, "wpn: weapon %d lands at (%x,%x), tumbles\n",
                            k, (unsigned)(w->x >> 16), (unsigned)(w->y >> 16));
            } }
            break;
        case 3:                                    /* 0xFF1E resting */
            if (!(w->state & 0x8000u)) {
                w->state |= 0x8000u;
                w->spr = (uint16_t)((w->type * 3u) | (w->facing & 0x8000u));   /* 0xFF2A-0xFF36 */
                w->list = 2;                       /* 0xFF3C +0x12 = 2 */
            }
            w->holder = 0;                         /* 0xFF42 clr.l +0x76 every tick */
            break;
        case 4:                                    /* 0xFF54 tumble */
            if (!(w->state & 0x8000u)) {
                w->state |= 0x8000u;               /* 0xFF5C */
                w->t24 = 0;                        /* 0xFF66 clr +0x24 */
                w->list = 2;                       /* 0xFF6A +0x12 = 2 */
                w->t22 = 1;                        /* init path falls into 0xFF7A */
            }
            if (--w->t22 == 0) {                   /* 0xFF72 */
                w->t22 = (uint16_t)wrule(8);       /* 0xFF7A +0x22 = 0x10 */
                w->t24++;                          /* 0xFF80 */
                if (w->t24 == (uint16_t)wrule_t(w, 9)) {   /* 0xFF86 cmpi #4 */
                    w->state = ST_SKID;                  /* 0xFFBE */
                } else {
                    unsigned pose = w->type * 3u + ((w->t24 & 1u) ? 1u : 2u);   /* 0xFF9A-0xFFB6 */
                    w->spr = (uint16_t)(pose | (w->facing & 0x8000u));
                }
            }
            break;
        }
        w->sx = (int16_t)((w->x >> 16) - st->cam_x);           /* 0xFF10/0xFF46 jsr 0x247C */
        w->sy = (int16_t)((w->y >> 16) + (w->z >> 16) - st->cam_y);
    }
}
