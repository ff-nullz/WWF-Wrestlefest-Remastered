/* Hit pipeline — transcription of ROM 0x24062 (scan), 0x24126 (legality),
 * 0x241E8/0x2425A (boxes + AABB), 0x24408/0x24852 (generic strike pair),
 * 0x24090 (swing bookkeeping), 0x24E58 (damage drain).
 * Spec: docs/engine-specs/hit-pipeline.md (2026-08-22). Tables (hit_tables[]
 * below, read straight from the ROM image through tbl.h): hit_record
 * (7 bytes: flags, vbox1, vbox2, abox, damage, result, reaction),
 * hit_attacker_boxes / hit_victim_boxes (4 signed bytes each).
 */
#include <stdlib.h>
#include "engine.h"
#include "tbl.h"
#include <stdio.h>
extern int eng_dbgsel;

/* Tables this file owns (docs/adr-001-data-formats.md). Bounds from
 * reference/maincpu.asm: the records run 0x24EF6..0x24FE3 (34 x 7, A6 =
 * 0x24EF6 + id*7 at 0x240BC), the attacker boxes 0x24FE4..0x2500F (lea at
 * 0x241F6, box*4), the victim boxes 0x25010..0x2503B (lea at 0x24212/
 * 0x24232, box*4), then the 0x2503C routine. */
static const tbl_def hit_tables[] = {
    { "hit_record",         "base/hit", 0x24EF6u, 34 * 7, TK_U8, 7,
      "0x240BC: attack id*7 -> {flags (b7 has hitbox, b6 vbox1 valid, b5 vbox2 valid), vbox1, vbox2, abox, damage, result handler, reaction handler}; read at 0x24080/0x241FC/0x2420A/0x24218/0x2422A/0x24364/0x2437E" },
    { "hit_attacker_boxes", "base/hit", 0x24FE4u, 11 * 4, TK_S8, 4,
      "0x241F6: attacker hitbox*4 -> {x0, y0, x1, y1} signed bytes, x mirrored by the render flip (0x24264)" },
    { "hit_victim_boxes",   "base/hit", 0x25010u, 11 * 4, TK_S8, 4,
      "0x24212/0x24232: victim hurtbox*4 -> {x0, y0, x1, y1} signed bytes (0x24264/0x242DE)" },
};
TBL_REGISTER(hit_tables)

/* One hit record, the 7 bytes as ints (same shape the old JSON rows had). */
static const int *hit_rec(unsigned id, int out[7])
{
    for (unsigned k = 0; k < 7; k++)
        out[k] = (int)tbl8(TBL(hit_record), id * 7u + k);
    return out;
}
/* One box: 4 signed bytes as ints. */
static const int *hit_box(int tbl, unsigned idx, int out[4])
{
    for (unsigned k = 0; k < 4; k++)
        out[k] = tbl_s8(tbl, idx * 4u + k);
    return out;
}

/* 0x24090 (from the 0xF510 epilogue): a changed anim word starts a new
 * swing frame — clear the per-victim mask and the stance record; move
 * handlers re-arm +0x4C every frame. */
void eng_hit_bookkeep(eng_state *st)
{
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active)
            continue;
        if (o->spr != o->spr_cache) {
            o->hit_mask = 0;
            o->atk = 0;
            o->spr_cache = o->spr;
        }
    }
}

/* 0x2425A: one axis pair, unsigned 16-bit overlap after position add. */
static int overlap(uint16_t a0, uint16_t a1, uint16_t b0, uint16_t b1)
{
    return (b0 >= a0 && a1 >= b0) || (b1 >= a0 && a1 >= b1)
        || (a0 >= b0 && b1 >= a0) || (a1 >= b0 && b1 >= a1);
}

/* Boxes are signed bytes x0,y0,x1,y1; X mirrored by the RENDER flip
 * (+0x04 bit15), Y axis uses the height word +0x0E. */
static int box_test(const eng_obj *a, const int *ab,
                    const eng_obj *v, const int *vb)
{
    int ax0 = (int8_t)ab[0], ay0 = (int8_t)ab[1];
    int ax1 = (int8_t)ab[2], ay1 = (int8_t)ab[3];
    int vx0 = (int8_t)vb[0], vy0 = (int8_t)vb[1];
    int vx1 = (int8_t)vb[2], vy1 = (int8_t)vb[3];
    int t;

    if (a->spr & 0x8000u) { ax0 = -ax0; ax1 = -ax1; t = ax0; ax0 = ax1; ax1 = t; }
    if (v->spr & 0x8000u) { vx0 = -vx0; vx1 = -vx1; t = vx0; vx0 = vx1; vx1 = t; }
    {
        uint16_t axp = (uint16_t)(a->x >> 16), vxp = (uint16_t)(v->x >> 16);
        if (!overlap((uint16_t)(ax0 + axp), (uint16_t)(ax1 + axp),
                     (uint16_t)(vx0 + vxp), (uint16_t)(vx1 + vxp)))
            return 0;
    }
    {
        uint16_t az = (uint16_t)(a->z >> 16), vz = (uint16_t)(v->z >> 16);
        return overlap((uint16_t)(ay0 + az), (uint16_t)(ay1 + az),
                       (uint16_t)(vy0 + vz), (uint16_t)(vy1 + vz));
    }
}

static unsigned strike_react_raw;      /* the reaction the result handler wrote,
                                          before the 0x24D7A behind remap */

/* Result handler 1 (0x24408, generic strike) + reaction 0 (0x24852). */
static int strike(eng_state *st, eng_obj *a, eng_obj *v, const int *arec)
{   /* returns 1 = the hit took effect. Stock: a veto SETS CARRY at 0x2435A
     * and the scan skips the 0x24DEC/0x24E02 epilogue (bcs $240de); running
     * the epilogue on a shrugged hit cleared the victim's hold/get-up
     * clock (+0x46) and record (+0x4C) mid-hold — "characters decided not
     * to get up, and to grapple forever" (rumble playtest 2026-08-24). */
    unsigned res = arec[5] & 0xFFu;

    /* vetoes first (result handlers may refuse the hit, carry set) */
    if (v->atk & 0x4000u)              /* 0x24408: strike-immune form */
        return 0;
    if ((res == 2 || res == 3) && (v->atk & 0xFF00u))
        return 0;                      /* modifier forms veto the trade/catch */
    if (res == 3 && ((v->anim_sel & 0xFFu) != 2
                     || ((a->facing ^ v->facing) & 0x8000u) == 0))
        return 0;                      /* catch needs an opposed runner */

    switch (res) {
    case 2:                            /* 0x24468 clothesline trade */
        eng_sound(0x2B);
        if ((v->anim_sel & 0xFFu) == 2) {   /* both running: both fall */
            a->state = ST_REACT; a->react_id = RC_FALL_HIGH; a->down_t = 0x50;
            v->state = ST_REACT; v->react_id = RC_FALL_HIGH; v->down_t = 0x50;
        } else {                       /* standing victim falls; the
                                          runner staggers past (0x244AC) */
            a->state = ST_REACT; a->react_id = 0x18;
            a->mover = 0;              /* kill the run (react-0x18 cell
                                          handler's job — untranscribed) */
            v->state = ST_REACT; v->react_id = RC_FALL_HIGH; v->down_t = 0x50;
        }
        break;                         /* record 3 carries no damage */
    case 3:                            /* 0x244E0 anti-run catch */
        v->state = ST_REACT;
        v->react_id = RC_GRABBED;            /* grabbed: flies over the catcher */
        v->dmg = (uint16_t)((v->dmg & 0xFF00u) | (arec[4] & 0xFFu));
        a->last_pair = (int)(v - st->obj);   /* +0x92 pairing: the
                                                catcher's 0x13034 converts
                                                on its next tick */
        v->last_pair = (int)(a - st->obj);
        v->role &= (uint16_t)~0x10u;
        break;
    case 4:                            /* 0x2452A running strike catch */
        eng_sound(0x2B); eng_sound(0x32);
        v->dmg = (uint16_t)((v->dmg & 0xFF00u) | (arec[4] & 0xFFu));
        a->atk = 0;
        v->state = ST_REACT;
        if (((a->angle & 0x80u) != 0) == ((v->facing & 0x8000u) != 0)) {
            /* 0x24556: attacker angle +0x2D b7 == victim facing byte b7
             * (word b15) -> they face each other: CAUGHT */
            v->react_id = 0x10;        /* caught: 32-tick freeze */
            v->facing = a->facing ^ 0x8000u;
            if (v->role & RF_RUNNING) v->dmg++;              /* 0x24DDC */
            if (a->wrestler == 0 || a->wrestler == 5) v->dmg += 3;
        } else
            v->react_id = 0x0B;
        a->last_pair = (int)(v - st->obj);
        v->last_pair = (int)(a - st->obj);
        break;
    case 6:                            /* 0x24640: the WEAPON shot (record 0xF) */
        eng_announce(st, 0x0F, 0x28);  /* $1C15D2 = $0F28 */
        eng_sound(0x2E); eng_sound(0x32);   /* 0x24648/0x24652 */
        v->dmg = (uint16_t)arec[4];    /* 0x2465C: +0x69 = record damage (0x14) */
        v->state = ST_REACT;                  /* 0x24662 */
        v->react_id = (uint16_t)((v->role & RF_RUNNING) ? 4 : 3);   /* 0x24668/0x2466E-0x24676 */
        if (v->role & RF_RUNNING) v->dmg++;  /* 0x24DDC running bonus */
        a->last_pair = (int)(v - st->obj);   /* 0x24DEC pairing: the swing
                                                handler's +0x92 connect test */
        v->last_pair = (int)(a - st->obj);   /* 0x24E40 */
        break;
    case 7:                            /* 0x2468A */
        eng_sound(0x2A); eng_sound(0x32);
        v->dmg = (uint16_t)arec[4];
        v->state = ST_REACT;
        v->react_id = ((a->move_id & 0xFFu) == 0x41u) ? 0x1D : 0x14;
        if (v->role & RF_RUNNING) v->dmg++;
        a->last_pair = (int)(v - st->obj);
        v->last_pair = (int)(a - st->obj);
        break;
    case 10: case 14:                  /* 0x24784 runner caught by the
                                          flying tackle 0x2E (rec 5) /
                                          catch-and-slam 0x31 (rec 0x1B) */
        if ((v->atk & 0xFF00u) || !(v->role & RF_RUNNING)
            || ((a->facing ^ v->facing) & 0x8000u) == 0)
            return 0;                  /* vetoes: modifier form, not running,
                                          running away */
        eng_sound(0x29); eng_sound(0x32);
        v->state = ST_REACT;
        v->dmg = (uint16_t)((v->dmg & 0xFF00u) | (arec[4] & 0xFFu));
        if ((a->atk & 0xFFu) == 0x1Bu) {                  /* 0x247BE */
            v->react_id = 0x28;
            eng_announce(st, a->wrestler, 0x2D);           /* $1C15D2 = 0x032D:
                                                              id 3 is the only
                                                              0x31 owner */
        } else
            v->react_id = 0x1F;
        a->atk = 0;
        a->last_pair = (int)(v - st->obj);                 /* 0x24DEC/0x24E02 */
        v->last_pair = (int)(a - st->obj);
        v->role &= (uint16_t)~0x10u;
        break;
    case 11:                           /* 0x247EC dropkick */
        if (v->atk & 0x8000u) return 0;
        eng_sound(0x2A); eng_sound(0x32);
        a->atk = 0;
        v->dmg = (uint16_t)arec[4];
        v->state = ST_REACT;
        v->react_id = 0x12;
        if (v->role & RF_RUNNING) v->dmg++;
        a->last_pair = (int)(v - st->obj);
        v->last_pair = (int)(a - st->obj);
        break;
    case 12:                           /* 0x245BC: announce, result-5 body */
    case 5:                            /* 0x245F0 heavy: knockdown */
        eng_sound(0x2A); eng_sound(0x32);
        v->dmg = (uint16_t)arec[4];
        v->state = ST_REACT;
        v->react_id = (uint16_t)(((v->anim_sel & 0xFFu) == 2) ? 3 : 2);
        break;
    default:                           /* result 0/1 shape */
        eng_sound(0x2A);
        eng_sound(0x32);
        v->dmg = (uint16_t)arec[4];
        v->state = ST_REACT;
        v->react_id = 0;
        if ((v->role & RF_RUNNING) || v->combo >= 3)
            v->react_id = RC_FALL_HIGH;           /* 0x2443E: a RUNNING victim (f33 b4)
                                          or stagger count >= 3 -> knockdown */
        if (v->role & RF_RUNNING) v->dmg++;  /* 0x24DDC */
        break;
    }
    { extern void eng_ai_stat_landed(const eng_state *, const eng_obj *); eng_ai_stat_landed(st, a); }   /* WF_AISTATS */
    if (eng_dbgsel)
        fprintf(stderr, "hit: P%d rec %02X res %u -> P%d react %02X (vf33=%04X vst=%02X)\n",
                (int)(a - st->obj) + 1, a->atk & 0xFFu, res, (int)(v - st->obj) + 1,
                v->react_id, v->role, v->state & 0xFFu);
    strike_react_raw = v->react_id & 0xFFu;   /* before the remap: what the
                                                 ROM's reaction handlers see */
    if (((a->facing ^ v->facing) & 0x8000u) == 0 && v->react_id < 5
        && res != 2 && res != 3 && res != 4 && res != 7 && res != 11)
        v->react_id = (uint16_t)(0x0Bu + (v->react_id ? v->react_id - 2u : 0u));
                                       /* 0x24D7A behind-hit remap */
    a->hit_mask |= (uint16_t)(0x8000u >> (v - st->obj));   /* 0x24334 */
    return 1;
}

/* Reaction handlers 6/7/0x0A/0x0B (0x24AA8 / 0x24B20 / 0x24C42 /
 * 0x24CCA): the man hit was HOLDING someone (records 0x14 mount, 0x15
 * ground hold + MDD, 0x1E bearhug, 0x1F Slaughter). Only a light hit —
 * reaction 0 (or 0x0A) from the result handler — breaks the hold: the
 * holder goes down (react 2, behind remap), the held man is freed per
 * hold (stand / get up / a short 8-tick knockdown behind), the referee
 * bits and the pads are released, and unless the striker is the holder's
 * own partner the announcer calls it (0x24900, phrase 0x19). A heavier
 * hit is shrugged off: the holder is put back into his move (0x8005).
 * TODO EXACT: the attacker's +0x20 restore from +0x1C on the shrug path. */
static void hold_hit(eng_state *st, eng_obj *a, eng_obj *v, unsigned vr)
{
    eng_obj *h = v->partner >= 0 ? &st->obj[v->partner] : 0;

    if (strike_react_raw != 0 && strike_react_raw != 0x0A) {   /* 0x24B02 */
        v->state = 0x8005;
        if (vr == 6 || vr == 7) v->grap44 = 0;
        if (eng_dbgsel)
            fprintf(stderr, "hold: P%d hit react %02X shrugged off\n",
                    (int)(v - st->obj) + 1, strike_react_raw);
        return;
    }
    if (!(st->g161 & 1u) && v->teammate != (int)(a - st->obj))   /* 0x24900 */
        eng_announce(st, 0x0F, 0x19);
    v->state = ST_REACT; v->react_id = RC_FALL_HIGH;
    if (((a->facing ^ v->facing) & 0x8000u) == 0) v->react_id = 0x0B;   /* 0x24D7A */
    if (h) {
        switch (vr) {
        case 6:  h->state = ST_GETUP; break;                          /* 0x24AD2 */
        case 7:  h->state = ST_STAND; break;                          /* 0x24B56 */
        case 10: h->state = ST_REACT; h->react_id = 0x0B;             /* 0x24C6E */
                 h->facing = v->facing; h->down_t = 8; break;
        default: h->state = ST_GETUP; h->facing = v->facing;          /* 0x24CFC */
                 h->x += ((h->facing & 0x8000u) ? -0x30 : 0x30) << 16;   /* 0x10BD0(0x30) */
                 break;
        }
        h->role &= (uint16_t)~0x40u; h->cue_flags &= (uint16_t)~0x04u;
        h->pinning = 0;
        h->off_x = 0; h->off_y = 0;    /* the hold's sprite offset dies with the hold (see eng_pin_break) */
    }
    v->role &= (uint16_t)~0x40u; v->cue_flags &= (uint16_t)~0x02u;
    v->off_x = 0; v->off_y = 0;
    eng_tag_pin_end(st, v, h);                                 /* 0x212A0 */
    v->partner = -1;
    if (eng_dbgsel)
        fprintf(stderr, "hold: BREAK by P%d -> holder P%d down, held man freed (react %u)\n",
                (int)(a - st->obj) + 1, (int)(v - st->obj) + 1, vr);
}

/* Reaction handlers 1..4 (0x24868 / 0x24924 / 0x24984 / 0x24A38): the
 * BEHIND GRAB pair was struck (records 9 = the 0x52 held man, 0xA = his
 * 0x1C holder, 0xC = the 0x53 held man, 0xD = his 0x1D holder —
 * docs/engine-specs/behind-grab.md §5). The held man only counts when hit
 * from the FRONT (striker facing the other way, 0x24874/0x2498C), the
 * holder only from BEHIND (same facing, 0x24930/0x24A40); otherwise the
 * man is put straight back into his move (0x8005). A light hit (reaction
 * 0 / 0xA) on a held man with fewer than 3 hits taken (+0x52) is the
 * DOUBLE-TEAM punch: he flinches (0xC052/0xC053 phase +0x44 = 1, or 2 for
 * 0xA on 0x53) and the holder braces (0x65 / 0x73 / 0x74, back to 0xC01C/
 * 0xC01D after one cell); the 3rd hit or a heavier one drops him (react
 * 2) and the holder lets go with the 0x6F pose. A hit on the holder
 * frees the held man (0x1C: stand; 0x1D: back to dizzy react 1) and the
 * holder falls (react 0xB, the behind remap). 0x24900: announcer 0x0F19
 * unless the striker is the hit man's own partner (or rumble). */
static void behind_hit(eng_state *st, eng_obj *a, eng_obj *v, unsigned vr)
{
    eng_obj *h = v->partner >= 0 ? &st->obj[v->partner] : 0;
    int same = ((a->facing ^ v->facing) & 0x8000u) == 0;
    int held = (vr == 1 || vr == 3);
    unsigned raw = strike_react_raw;

    if (held ? same : !same) {                             /* 0x24874 / 0x24930 / 0x2498C / 0x24A40 */
        v->state = 0x8005;
        if (eng_dbgsel)
            fprintf(stderr, "behind: P%d hit react %02X shrugged off (wrong side)\n",
                    (int)(v - st->obj) + 1, raw);
        return;
    }
    if (!h) { v->state = ST_REACT; v->react_id = RC_FALL_HIGH; return; }
    v->role &= (uint16_t)~0x40u; h->role &= (uint16_t)~0x40u;
    if (vr == 1) {                                         /* 0x24884 */
        v->cue_flags &= (uint16_t)~0x04u; h->cue_flags &= (uint16_t)~0x02u;
    } else if (vr == 2) {                                  /* 0x2493E */
        h->cue_flags &= (uint16_t)~0x04u; v->cue_flags &= (uint16_t)~0x02u;
        h->state = ST_STAND;                                      /* 0x2494A: the held man is freed */
    }
    if (!(st->g161 & 1u) && v->teammate != (int)(a - st->obj))   /* 0x24900 */
        eng_announce(st, 0x0F, 0x19);
    if (held) {
        if ((raw == 0 || raw == 0x0A) && v->combo < 3) {   /* 0x248A6 / 0x249B6 / 0x249DE */
            v->state = ST_MOVE; v->grap44 = (uint16_t)((vr == 3 && raw == 0x0A) ? 2 : 1);
            v->move_id = (uint16_t)(vr == 1 ? 0xC052u : 0xC053u);
            h->state = ST_MOVE;
            h->move_id = (uint16_t)(vr == 1 ? 0xC065u : raw == 0x0A ? 0xC074u : 0xC073u);
            if (eng_dbgsel)
                fprintf(stderr, "behind: P%d punched the held P%d (hit %u) -> flinch, holder P%d braces %02X\n",
                        (int)(a - st->obj) + 1, (int)(v - st->obj) + 1, v->combo + 1,
                        (int)(h - st->obj) + 1, h->move_id & 0xFFu);
            return;
        }
        if (vr == 3 || raw == 0 || raw == 0x0A) { v->state = ST_REACT; v->react_id = RC_FALL_HIGH; }   /* 0x248CE / 0x24A06 */
        h->state = ST_MOVE; h->move_id = 0x6F;                   /* 0x248DA / 0x24A12 */
        eng_tag_pin_end(st, v, h);                         /* 0x212A0 */
        v->partner = -1;
        if (eng_dbgsel)
            fprintf(stderr, "behind: held P%d dropped by P%d's hit (react %02X), holder P%d lets go\n",
                    (int)(v - st->obj) + 1, (int)(a - st->obj) + 1, raw, (int)(h - st->obj) + 1);
        return;
    }
    if (vr == 4) { h->state = ST_REACT; h->react_id = RC_DIZZY; v->grap44 = 0; }   /* 0x24A56: back to dizzy */
    v->state = ST_REACT; v->react_id = 0x0B;                      /* 0x2495E / 0x24A62 + 0x24D7A */
    eng_tag_pin_end(st, h, v);                             /* 0x212A0 (A0 = the held man) */
    v->partner = -1;
    if (eng_dbgsel)
        fprintf(stderr, "behind: holder P%d hit from behind by P%d -> down, held P%d freed\n",
                (int)(v - st->obj) + 1, (int)(a - st->obj) + 1, (int)(h - st->obj) + 1);
}

/* The AI run-in's stand-in for the untranscribed strike move 0x39: apply
 * the light-hit break to holder `h` as if `a` had jabbed him. */
void eng_hold_break_by(eng_state *st, eng_obj *a, eng_obj *h)
{
    unsigned vr;
    if (!(h->atk & 0xFFu)) return;
    vr = tbl8(TBL(hit_record), (h->atk & 0xFFu) * 7u + 6u) & 0xFFu;
    if (vr != 6 && vr != 7 && vr != 10 && vr != 11) return;
    strike_react_raw = 0;
    h->state = ST_REACT; h->react_id = 0; h->dmg = 2;
    hold_hit(st, a, h, vr);
}

/* 0x24062/0x24106/0x24126: attacker scan, victim legality, prefilter. */
void eng_hit_scan(eng_state *st)
{
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *a = &st->obj[i];
        int arec[7], abx[4];
        if (!a->active || a->atk == 0 || !(a->state & 0x8000u)
            || (a->apron && (a->atk & 0xFFu) != 0x19u)   /* engine guard vs stale apron
                                          records; stock's prefilter (0x2406C-0x24086)
                                          has no apron test — the APRON PUNCH's record
                                          0x19 (0x17B8A) must land over the ropes */
            || (a->st_flags & SF_ELIMINATED))       /* +0x32 b4: eliminated (rumble) */
            continue;
        hit_rec(a->atk & 0xFFu, arec);
        if (!(arec[0] & 0x80))         /* record has no hitbox */
            continue;
        hit_box(TBL(hit_attacker_boxes), (unsigned)(arec[3] & 0xFF), abx);
        mod_hit_scan_extras(st, a);    /* modhooks.c: the perched man, the referee */
        for (int j = 0; j < ENG_MAX_OBJS; j++) {
            eng_obj *v = &st->obj[j];
            int vrec[7], vbx[4];
            uint16_t d;
            if (v != a && v->active && (v->state & 0xFFu) == ST_MOVE && (v->move_id & 0xFFu) == 0x37
                && v->partner >= 0 && ((a->role ^ v->role) & 0x80u)
                && !(a->hit_mask & (0x8000u >> j)) && (arec[5] & 0xFFu) <= 1u) {
                /* ENGINE: the buckle double team's HOLDER (0x37) carries no
                 * hit record; the ROM frees the pair through the AI's
                 * rescue strike 0x39 (untranscribed). A light strike from
                 * an enemy standing in front of him is that rescue: the
                 * held man takes his escape 0x59 (the holder is thrown
                 * off) - "if he punches the holder he kicks them both out
                 * of the hold" (user 2026-08-30). */
                int32_t dx = (v->x >> 16) - (a->x >> 16), dy = (v->y >> 16) - (a->y >> 16);
                if (labs(dy) < 0x0C && labs(dx) < 0x30 && ((a->facing & 0x8000u) ? dx > 0 : dx < 0)) {
                    eng_obj *hm = &st->obj[v->partner];
                    a->hit_mask |= (uint16_t)(0x8000u >> j);
                    eng_sound(0x2A); eng_sound(0x32);
                    if (hm->active && (hm->state & 0xFFu) == ST_MOVE && (hm->move_id & 0xFFu) == 0x61) {
                        hm->mash_aa = 0; hm->state = ST_MOVE; hm->move_id = 0x59; hm->grap44 = 0; hm->frame = 0; hm->anim_sel = 0;
                    }
                    if (eng_dbgsel) fprintf(stderr, "hit: P%d frees P%d from the buckle hold (P%d thrown off)\n",
                                            (int)(a - st->obj) + 1, (int)(hm - st->obj) + 1, (int)(v - st->obj) + 1);
                }
                continue;
            }
            if (v == a || !v->active || v->atk == 0 || !(v->state & 0x8000u)
                || (v->st_flags & SF_ELIMINATED))   /* eliminated men are out of the match */
                continue;
            if (v->result)             /* 0x24146: latched */
                continue;
            if ((v->apron || (v->role & RF_OUTSIDE)) && !(st->g161 & 2u)
                && !v->pinning)        /* 0x2414C: an OUTSIDE/apron victim is
                                          only hittable while the ringside view
                                          shows ($1C0161 b1) — an in-ring swing
                                          reaching the apron launched partners
                                          "to the abyss". A PINNING man is
                                          necessarily in-ring: a stale outside
                                          bit must not shield his cover from
                                          the break ("the cpu just punches the
                                          air", playtest 2026-08-24). */
                continue;
            if ((v->spr & 0x7FFFu) == 0x7FFFu && (v->atk & 0xFFu) != 0x1Du)
                continue;              /* 0x24160 — except the covering man:
                                          the ROM's pin-hold (0x15F22) keeps
                                          him visible with record 0x1D, our
                                          0x48 cover hides him behind the
                                          composite (0x133DC). TODO EXACT. */
            if (a->hit_mask & (0x8000u >> j))
                continue;              /* already hit this swing */
            d = (uint16_t)abs((int)(v->y >> 16) - (int)(a->y >> 16));
            if ((v->atk & 0xFFu) == 0x14u && d >= 8)
                d -= 8;
            if (d >= 0x0C)
                continue;              /* 0x2418A */
            d = (uint16_t)abs((int)(v->x >> 16) - (int)(a->x >> 16));
            if (d >= 0x50)
                continue;              /* 0x2419E */
            hit_rec(v->atk & 0xFFu, vrec);
            if (!(vrec[0] & 0x40))     /* victim box1 mandatory */
                continue;
            if (!box_test(a, abx,
                          v, hit_box(TBL(hit_victim_boxes), (unsigned)(vrec[1] & 0xFF), vbx))
                && !((vrec[0] & 0x20) &&
                     box_test(a, abx,
                              v, hit_box(TBL(hit_victim_boxes), (unsigned)(vrec[2] & 0xFF), vbx))))
                continue;
            {
                /* 0x2435A runs the VICTIM record's reaction handler after
                 * the result handler. Handler 9 = 0x24BC2/0x24C0C: the man
                 * hit was covering someone (record 0x1D, written at 0x15F22
                 * and by the engine's cover in anim.c handler_hold) — the
                 * pair is freed, the referee's +0x35 b0 cue dies and the
                 * count aborts with the digit wipe. */
                int pinned = (eng_pin_is_pinner(v)
                              || ((v->state & 0xFFu) == ST_MOVE && v->pinning && (v->cue_flags & 1u)))
                             ? v->partner : -1;   /* 0x0C cover, or the scripted
                                                     covers 0x1A/0x23/0x0F (state 5) */
                unsigned vr = vrec[6] & 0xFFu;
                unsigned vst_before = v->state & 0xFFu;
                int ground_link = (vst_before == 5 && v->partner >= 0 && v->partner != (int)(a - st->obj)
                                   && (st->obj[v->partner].state & 0xFFu) == ST_REACT
                                   && st->obj[v->partner].partner == (int)(v - st->obj)) ? v->partner : -1;
                /* snapshot for the 0x24C24 "cover stands" path below */
                uint16_t pv_react = v->react_id, pv_down = v->down_t, pv_atk = v->atk, pv_f33 = v->role;
                int pv_last = v->last_pair;
                int landed = strike(st, a, v, arec);   /* result 1 + reaction 0 shape */
                if (landed && ((a->atk & 0xFFu) == 0x1Eu || (a->atk & 0xFFu) == 0x1Fu)) {
                    if (a->wobj >= 1 && a->wobj <= ENG_WEAPONS) {
                        int wd = eng_wpn_rule((int)st->wpn[a->wobj - 1].type, 3);
                        if (wd > 0) v->dmg = (int16_t)wd;   /* weapons.json per-type
                                                               "damage" (user 2026-08-28) */
                    }
                    mod_weapon_landed(st, a);   /* modhooks.c: weapon_dq */
                }
                if (landed) {
                    /* 0x24DEC/0x24E02-0x24E40: carry CLEAR only — a vetoed
                     * (shrugged) hit skips ALL of this (bcs $240de). */
                    if ((v->weapon_w & WPN_HELD) && (v->state & 0xFFu) == ST_REACT) {
                        eng_weapon_drop(st, v);   /* a struck man drops his weapon */
                        v->grap44 = 0;            /* 0x24E20 clr.w (+0x44) */
                    }
                    /* 0x24E24-0x24E40 struck epilogue: the victim's ATTACK
                     * RECORD dies with the hit — a stale strike box on a
                     * flying/lying man floored anyone his sprite touched. */
                    v->atk = 0;                   /* 0x24E2A clr.w (+0x4C) */
                    v->hold_t = 0;                /* 0x24E2E clr.w (+0x46) */
                    if (vst_before == 5 && (v->state & 0xFFu) == ST_REACT)
                        v->off_x = 0, v->off_y = 0;   /* struck OUT of a move (tackle +0x50, dive, pickup...)
                                                         by a third man: the move's sprite offset never reaches
                                                         its own release; reactions set none (TODO EXACT, see
                                                         eng_pin_break) */
                    v->role &= (uint16_t)~0x10u;   /* 0x24E3A bclr #4: not running */
                    v->last_pair = (int)(a - st->obj);   /* 0x24E40 (+0x92) */
                }
                if ((v->state & 0xFFu) == ST_REACT && v->partner >= 0 && v->partner != (int)(a - st->obj)
                    && !eng_pin_is_pinner(v) && (st->obj[v->partner].state & 0xFFu) != 0xFFu)
                    v->divorce = 1;        /* 0x24852/0x248F0/...: the strike results flag the struck
                                              man's link (bset #7,+0x26) unless he is holding someone */
                if (ground_link >= 0 && (v->state & 0xFFu) == ST_REACT) {
                    /* A man struck out of his own GROUND attack (stomp 0x0A,
                     * drops, leap) drops the mutual 0xF178 / 0x1E5E6 link
                     * with the lying man he was working on: neither side
                     * holds the other, and a mutual link that outlives the
                     * move is never broken by the lazy divorce (0x115D2
                     * only drops one-sided links) — it left both men
                     * unlinkable for the rest of the match (tie-up gate
                     * 0xF866, rumble target filter 0x20C0A): "grappling
                     * stops working". TODO EXACT: stock's equivalent site. */
                    st->obj[ground_link].partner = -1;
                    v->partner = -1;
                    if (eng_dbgsel) fprintf(stderr, "hit: P%d struck out of his ground move, link to P%d dropped\n", (int)(v - st->obj) + 1, ground_link + 1);
                }
                if (vr == 9 && pinned >= 0) {
                    /* 0x24BC2 light-hit break, NARROWED (user 2026-08-28
                     * "it should ONLY be a stomp"): the stomp breaks the
                     * cover through its own handler (anim.c handler_stomp,
                     * handler-applied), so a SCAN hit reaching a pinner is
                     * a punch/kick/running brush whose marginal box overlap
                     * made pin breaks feel random — those shrug off now.
                     * A landed WEAPON swing (records 0x1E/0x1F) still
                     * breaks the pile. */
                    unsigned ar = a->atk & 0xFFu;
                    if ((ar == 0x1Eu || ar == 0x1Fu)
                        && (strike_react_raw == 0 || strike_react_raw == 0x0A))
                        eng_pin_break(st, v, &st->obj[pinned]);
                    else {
                        /* 0x24C24: the cover STANDS - state back (8005 in the
                         * ROM = the cover move), +0x44 cleared; the attacker's
                         * state := +0x1C (his move is cut).  The engine also puts
                         * back the reaction, down clock, record and pairing the
                         * result handler wrote: the running-attack handlers
                         * (runjump/dropkick 0x19 follow-up, runstrike catch) read
                         * v->react_id + v->last_pair == self and floored the
                         * pinner AFTER the shrug - "a running clothesline knocked
                         * my own pinner off and the ref kept counting" (user
                         * 2026-08-30). The damage stays (the ROM's result ran). */
                        v->state = (uint16_t)(0x8000u | vst_before);
                        v->react_id = pv_react; v->down_t = pv_down; v->atk = pv_atk; v->last_pair = pv_last;
                        v->role = (uint16_t)((v->role & ~0x10u) | (pv_f33 & 0x10u));
                        v->grap44 = 0;                                /* 0x24C3C clr.w +0x44 */
                        v->off_x = 0; v->off_y = 0;
                        if (a->last_pair == (int)(v - st->obj)) a->last_pair = -1;   /* no follow-up on him */
                        if (eng_dbgsel)
                            fprintf(stderr, "pin: P%d hit (rec %02X react %02X) shrugged off\n",
                                    (int)(v - st->obj) + 1, ar, strike_react_raw);
                    }
                }
                else if ((vr == 6 || vr == 7 || vr == 10 || vr == 11)
                         && (v->state & 0xFFu) == ST_REACT)
                    hold_hit(st, a, v, vr);   /* 0x24AA8 family */
                else if (vr >= 1 && vr <= 4 && vst_before == 5
                         && (v->state & 0xFFu) == ST_REACT)
                    behind_hit(st, a, v, vr); /* 0x24868 family (behind grab) */
            }
        }
    }
}

/* 0x24E58: pending damage into HP, floor 0. */
void eng_damage_drain(eng_state *st)
{
    mod_track_pairs(st);               /* modhooks.c: the parasitic pairing memory */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active || o->dmg == 0 || o->result)   /* 0x24E6C gate */
            continue;
        mod_damage_scale(st, o);       /* modhooks.c: human_hit_mult */
        {
            uint16_t nhp = o->hp > o->dmg ? (uint16_t)(o->hp - o->dmg) : 0;
            o->hp_delta = (int16_t)(o->hp_delta - (int16_t)(o->hp - nhp)); /* +0x6A */
            mod_damage_taken(st, o, i, nhp);   /* modhooks.c: parasitic_pct, ko_dq */
            o->hp = nhp;
        }
        o->dmg = 0;
        /* band 0x24EC2: <=0x18 -> 2, <= 2*max/3 -> 1, else 0 */
        o->band = o->hp <= 0x18 ? 2
                : o->hp <= (uint16_t)(2 * o->hp_max / 3) ? 1 : 0;
    }
    if (st->frame & 1)
        return;                        /* combo decay ticks even frames, 0x24E92 */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *o = &st->obj[i];
        if (!o->active) continue;
        if (o->combo_t && --o->combo_t == 0)
            o->combo = 0;
        if ((o->st_flags & SF_TIRED) && o->slowwalk_t && --o->slowwalk_t == 0)
            o->st_flags &= (uint16_t)~0x0800u;   /* 0x24EA0 tired clock */
    }
}
