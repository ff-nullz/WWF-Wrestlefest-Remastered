/* The 1P-4P player chips and the CHANGE OVER flasher — transcription of
 * the $1C14CE overlay machine at 0x8710 (four 0x2A-stride companion
 * objects beside the player slots $1C05B0+n*0x10C, run every live frame):
 *
 *  +0x35 b3 (0x21624 hand_pad, 0x20F86 the behind-grab pad hand, the
 *    0x212D4-family restores) -> the CHANGE OVER flasher: pose base 0
 *    (0x8750 clr +0x1E), the 3-step 0..2 cycle.
 *  +0x35 b4 (0x6EF4/0x6FFC/0x70A0 buy-in joins, 0x9862 go-live,
 *    0x2054E) -> the P-NUMBER chip: pose = 3 + the man's pad port
 *    (0x87A2 +0x1E = 3; 0x8884 walks $140026 down to find the port;
 *    0x87AC adds it).
 *  +0x35 b5 -> hide (0x8766).
 *
 *  Tick 0x87B6: +0x1C counts to 0x80 then auto-hides; position follows
 *  the owner: x ±8 by his facing (0x87DE), y = his, z = his + 0x68
 *  (0x87FA) — the chip floats above the head. Blink 0x8806: every 2
 *  frames a step, 3 steps wrap -> pose := base; base 0 -> pose++ (the
 *  animated change-over). Sprite row 0x1E (poses 0-6, palette id 0x35 —
 *  0x2AEA on show, 0x2B58 freed when the last chip hides; the engine's
 *  extra-bank cache stands in).
 *
 *  GATE: stock runs the machine only under the 2P-vs flag $1C007C
 *  (0x870E) — the engine runs it whenever the owner is pad-driven so
 *  the 1P change-over shows too (user request 2026-08-24, TODO EXACT
 *  the $1C007C semantics once 2P lands). */
#include <stdio.h>
#include <stdlib.h>
#include "wf.h"
#include "engine.h"

extern int eng_dbgsel; static int dbg_once;
void eng_chips_tick(eng_state *st)
{
    for (int i = 0; i < 4 && i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        eng_chip *c = &st->chip[i];

        if (!o->active) { c->on = 0; continue; }
        if (o->cue_flags & CF_CHIP_CHANGE) {                          /* 0x8732 b3: CHANGE OVER */
            o->cue_flags &= (uint16_t)~0x08u;
            c->on = 1; c->t = 0; c->b2 = 0; c->b3 = 0; /* 0x8750-0x8760 */
            c->base = 0; c->pose = 0;
            if (eng_dbgsel) fprintf(stderr, "chip: CHANGE OVER on o%d\n", i);
        } else if (o->cue_flags & CF_CHIP_HIDE) {                   /* 0x8766 b5: hide */
            o->cue_flags &= (uint16_t)~0x20u;
            c->on = 0;
        } else if (o->cue_flags & CF_CHIP_P) {                   /* 0x8778 b4: P chip */
            o->cue_flags &= (uint16_t)~0x10u;
            c->on = 1; c->t = 0; c->b2 = 0; c->b3 = 0; /* 0x8796-0x879E */
            c->base = (uint16_t)(3 + (o->input >= 0 ? (o->input & 3) : i));   /* 0x87A2/0x8884 */
            c->pose = c->base;                         /* 0x87B0 */
        }
        if (eng_dbgsel && i == 0 && !dbg_once) { dbg_once = 1; fprintf(stderr, "chips: badge_human=%d badge_cpu=%d\n", eng_mod_rule(MODR_BADGE_HUMAN), eng_mod_rule(MODR_BADGE_CPU)); }
        if (eng_mod_rule(MODR_BADGE_HUMAN) && o->input >= 0) {   /* MOD: the chip lives all match, on the pad-driven man */
            if (!c->on || c->base < 3) { c->on = 1; c->t = 0; c->b2 = 0; c->b3 = 0; }
            c->base = (uint16_t)(3 + (o->input & 3)); c->pose = c->base;
            continue;
        }
        if (!c->on) continue;
        {   /* user 2026-08-24: the P chip vanishes the moment its man
             * ENGAGES (grapple/move/held) — floating a "1P" over a
             * two-man composite was confusing. Engine addition (stock
             * lets the 0x80-frame life run; TODO EXACT if a ROM site
             * shows an early kill). */
            unsigned s5 = o->state & 0xFFu;
            if (c->base >= 3 && (o->partner >= 0 || s5 == 5 || s5 == 0x0B
                                 || s5 == 0x0C || s5 == 0xFF)) {
                c->on = 0; continue;
            }
        }
        if (++c->t >= 0x80u) { c->on = 0; continue; }  /* 0x87C4 auto-hide */
        if (++c->b2 >= 2) {                            /* 0x880A blink */
            c->b2 = 0;
            if (++c->b3 >= 3) { c->b3 = 0; c->pose = c->base; }   /* 0x881A wrap */
            else if (c->base == 0) c->pose++;          /* 0x882E the 0..2 cycle */
        }
    }
}

/* Emit the active chips (row 0x1E) — called at the tail of the match
 * sprite compile so they draw on top; stock emits via 0x27B8 in the
 * same loop. */
void eng_chips_emit(const eng_state *st, unsigned *slot)
{
    int mod_h = eng_mod_rule(MODR_BADGE_HUMAN), mod_c = eng_mod_rule(MODR_BADGE_CPU);
    for (int i = 0; i < 4 && i < ENG_MAX_OBJS; i++) {
        const eng_obj *o = &st->obj[i];
        const eng_chip *c = &st->chip[i];
        int sx, sy;
        if (!c->on || !o->active) continue;
        if (mod_h && c->base >= 3) continue;                 /* the mod draws its own P badge below */
        sx = (int)(o->x >> 16) + ((o->facing & 0x8000u) ? 8 : -8) - st->cam_x;   /* 0x87DE */
        sy = (int)(o->y >> 16) + (int)(o->z >> 16) + 0x68 - st->cam_y;           /* 0x87FA */
        eng_sprite_emit_pose(0x1Eu, c->pose & 0x7FFFu, sx, sy, -1, slot);
    }
    if (!mod_h && !mod_c) return;
    /* MOD badges (build/badges.pak): over the head of every man DRAWN this
     * frame - a hidden half of a pair rides his partner's composite, a man
     * not on this scene's screen (ring-out view) gets none */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        const eng_obj *o = &st->obj[i];
        int is_cpu, drawn = eng_sprite_obj_drawn(i), sx, sy;
        if (!o->active || !drawn) continue;
        is_cpu = o->cpu || (o->driver & DRV_AUTOPILOT) || o->input < 0;
        sx = drawn == 1 ? eng_sprite_obj_cx(i) : (int)(o->x >> 16) - st->cam_x;   /* over the DRAWN sprite (a lying man's head is not at his x) */
        if (o->partner >= 0 && o->partner < ENG_MAX_OBJS && eng_sprite_obj_drawn(o->partner))   /* a pair shares one x: part them by facing */
            sx += (o->facing & 0x8000u) ? -10 : 10;   /* bit set = faces right = stands on the left */
        sy = eng_sprite_obj_top(i) - 5;                      /* into the top cell's empty rows: close to the head */
        {   /* the walk cycle's arm frames move the sprite top a few px per frame: a chip
             * that follows it bobs.  Ease: a small change moves 1 px a frame, a big one
             * (a fall, a jump) snaps. */
            static int last_y[ENG_MAX_OBJS], last_on[ENG_MAX_OBJS];
            if (last_on[i] && abs(sy - last_y[i]) <= 10) sy = last_y[i] + (sy > last_y[i] ? 1 : sy < last_y[i] ? -1 : 0);
            last_y[i] = sy; last_on[i] = 1;
        }
        if (is_cpu) { if (mod_c) eng_badge_emit("cpu", sx, sy, slot); }
        else if (mod_h) { char nm[8]; snprintf(nm, sizeof nm, "p%d", (o->input & 3) + 1); eng_badge_emit(nm, sx, sy, slot); }
    }
}
