/* The referee — transcription subset of ROM 0x1F914/0x1F952/0x1FB1A for
 * a singles match. Spec: docs/engine-specs/pins-referee.md (2026-08-22).
 *
 * Implemented: idle midpoint walk (SM5 shape), pin hunt (+0x35 bit0 on
 * any slot -> attend), approach (visual 1 -> $8006), the 1-2-3 count
 * (visual 6 EXACT: +0x23 seed 0x14 then 0x20/0x38 alternation, digit
 * blits via table 0x2073A into $C0744/$C0748 with attr 0x00EA, result
 * stamped at half-count 6, point of no return at 6), abort-on-kickout
 * with digit wipe, and a minimal win pose. Escort/count-out/cage arrive
 * with their scenes. Referee sprites: stream row 12.
 */
#include <string.h>
#include <stdlib.h>
#include "wf.h"
#include "engine.h"
extern int eng_dbgsel;
#include "tbl.h"

#define REF_ROW 12u

/* Tables this file owns (docs/adr-001-data-formats.md). */
static const tbl_def referee_tables[] = {
    { "ref_approach_angles",   "base/referee", 0x2060C, 32, TK_U8, 16,
      "0x205F8 pin approach: walk angle by (+0x35 rope bits 1..4 >> 1) | 0x10 when the pin is to the left (0x205DC); 0x2062C follows" },
    { "ref_count_digit_ptrs",  "base/referee", 0x2073A, 21 * 4, TK_U32, 1,
      "0x206B4 count blit: long -> glyph for count value 0..0x14 (1..9 = 8-byte glyph, 0xA..0x14 = 16-byte two-digit glyph)" },
    { "ref_count_digit_glyphs","base/referee", 0x2078E, 0x2088E - 0x2078E, TK_U8, 0,
      "0x206BA count glyphs: 4 rows x 2 tile bytes (attr 0x00EA) per value, two-digit values 4 rows x 2 + 4 rows x 2 (0x206E6); code 0x2088E follows" },
    { "ref_return_masks",      "base/referee", 0x2001A, 4, TK_U8, 4,
      "0x1FF52 visual 5 ($8005 walk back to the ropes): clip stop mask by rope memory (+0x35 b1-2 >> 1), read 0x1FFD6; arrival when (clip & mask) == mask (0x1FFE2) -> state $8000" },
    { "ref_return_angles",     "base/referee", 0x2001E, 4, TK_U8, 4,
      "0x1FF52 visual 5 walk-back heading (+0x2D) by rope memory (+0x35 b1-2 >> 1), read 0x1FF82; row 0 (no memory) = 0x00, the back line y 0x198" },
};
TBL_REGISTER(referee_tables)

/* Every ROM return-to-idle writes state $8005 (0x1F9F0 abort, 0x2014A
 * rumble count end, 0x20482 win-pose end): SM5 + visual 5, the 0x1FF52
 * walk back to the rope line. Arrival there (0x1FFEC) moves to $8000. */
static void ref_to_idle(eng_ref *r)
{
    r->sm = 5;
    r->vis = 5;
    r->vis_init = 0;
}

void eng_referee_init(eng_state *st)
{
    eng_ref *r = &st->ref;

    memset(r, 0, sizeof *r);
    r->active = 1;
    ref_to_idle(r);                    /* $8005 like every idle entry */
    r->x = 0x280 << 16;                /* 0x10718 spawn */
    r->y = 0x198 << 16;
    r->z = 0x140 << 16;
    r->spr = 0;
    r->facing = 0;
}

/* 0x2067C: blit the count value n (1..20). The glyph pointer table
 * 0x2073A holds one long per value; entries 0..9 point at an 8-byte
 * glyph (4 rows x 2 FG0 tiles), entries 0x0A..0x14 at a SIXTEEN-byte
 * glyph — the two-digit number drawn as 4 rows x 4 tiles, low 8 bytes =
 * the left pair of cells, high 8 bytes = the right pair. Each cell is a
 * zero-extended tile word + attr 0x00EA, row stride 0x100 VRAM bytes.
 * Windows: $C0748 (ones) for n < 10, $C0744 (tens) for n >= 10.
 *
 * 0x206E6-0x206F6 is the second half: `moveq #1,D0 ; bra $206BA` jumps
 * back INTO the row loop, past the 0x206B4 table lookup — so A4 still
 * points at glyph[n] and D3 is still 8 from the first pass, i.e. it
 * blits bytes 8..15 of the SAME glyph two cells to the right. The
 * `#1` is only there to make the 0x206E6 `cmpi.b #$a,D0` fail and end
 * the routine; it is NOT the digit "1". */
static void digit_glyph(unsigned dest, uint32_t g)  /* 0x206BA loop body */
{
    for (unsigned row = 0; row < 4; row++) {
        unsigned off = dest + row * 0x100u;
        if (off + 8 <= WF_FG0RAM_SIZE) {
            /* 0x206BC: two tiles per row (glyph bytes 2*row, 2*row+1),
             * each a zero-extended tile word + attr 0x00EA */
            wf.fg0_videoram[off]     = 0;
            wf.fg0_videoram[off + 1] = tbl_ra8(g + row * 2u);
            wf.fg0_videoram[off + 2] = 0;
            wf.fg0_videoram[off + 3] = 0xEA;
            wf.fg0_videoram[off + 4] = 0;
            wf.fg0_videoram[off + 5] = tbl_ra8(g + row * 2u + 1u);
            wf.fg0_videoram[off + 6] = 0;
            wf.fg0_videoram[off + 7] = 0xEA;
        }
    }
}
/* 0x2067C entry: bit15 of D0 selects the CONTINUE-screen window $C1188
 * (0x2068A) instead of the referee's count windows (0x20692/0x2069E). */
#define DIGIT_WIN_CONTINUE  (0xC1188u - 0xC0000u)
void eng_count_digit(unsigned d0)
{
    unsigned n = d0 & 0x7FFFu;
    unsigned dest;
    if (n > 99) n = 99;
    if (d0 & 0x8000u) dest = DIGIT_WIN_CONTINUE;             /* 0x2068A */
    else {
        dest = ringout_rule(RO_DIGIT_WIN_ONES);              /* 0x20692 $C0748 */
        if (n >= 10) dest = ringout_rule(RO_DIGIT_WIN_TENS); /* 0x2069E $C0744 */
    }
    if (n > 20) {
        /* the ROM table stops at 20 (a modded resolve_count may run to 99):
         * compose the number from the single-digit glyphs 0..9, tens in the
         * left pair of cells, ones in the right - the same cells the baked
         * two-digit glyphs occupy */
        digit_glyph(dest,      tbl32(TBL(ref_count_digit_ptrs), (n / 10u) * 4u));
        digit_glyph(dest + 8u, tbl32(TBL(ref_count_digit_ptrs), (n % 10u) * 4u));
        return;
    }
    uint32_t g = tbl32(TBL(ref_count_digit_ptrs), n * 4u);   /* 0x206B4 */
    digit_glyph(dest, g);
    if (n >= 10) digit_glyph(dest + 8u, g + 8u);      /* 0x206E6: bytes 8..15 of glyph[n] */
}
static void digit_blit(unsigned n) { eng_count_digit(n); }

/* 0x280DC through a scratch object: the ring trapezoid clamps the ref
 * every frame he walks (idle 0x1FB90, approach 0x2062C). Applies the
 * push-back and returns the clip bits (nonzero = he hit a rope). */
static unsigned ref_probe(eng_ref *r)
{
    eng_obj tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.x = r->x; tmp.y = r->y; tmp.z = r->z; tmp.mover = 1;
    eng_ring_bounds(&tmp);
    if (tmp.clip) { r->x = tmp.x; r->y = tmp.y; }
    return tmp.clip;
}

static void digit_wipe(void)                        /* 0x206FE: 4 rows x 4 longs at $C0744 */
{
    unsigned base = ringout_rule(RO_DIGIT_WIN_TENS);
    for (unsigned row = 0; row < 4; row++)
        for (unsigned off = base; off < base + 0x10u; off += 4)
            if (off + row * 0x100u + 4 <= WF_FG0RAM_SIZE)
                memset(wf.fg0_videoram + off + row * 0x100u, 0, 4);
}
void eng_ref_digit_wipe(void) { digit_wipe(); }

/* SM3 0x1FA82 + visual 3 0x1FD5E — the ring-out 20-count
 * (docs/engine-specs/ringout-scene.md §2). Entered by the 0xF98C scene
 * switch (+0x20 = $8003, referee teleported to (0x340,0x160)). */
static void ref_sm3_countout(eng_state *st, eng_ref *r)
{
    eng_obj *a, *b;
    int32_t rx = r->x >> 16;

    /* --- SM3 0x1FA82: walk toward the X midpoint of the two legal men ---
     * (1FA8C clr.w $1C1682, the usher timer: no engine counterpart yet) */
    if (eng_legal_pair(st, &a, &b)) {
        int32_t ax = a->x >> 16, bx = b->x >> 16;
        int32_t mid = (ax >= bx) ? ((ax - bx) >> 1) + bx : ((bx - ax) >> 1) + ax;   /* 1FA96-1FAB2 */
        int32_t d0 = mid - rx;
        if (d0 < 0) { d0 = -d0; r->angle = 0xC0; } else r->angle = 0x40;     /* 1FAB4-1FAC4 */
        if (d0 < 0x20)      { r->angle ^= 0x80; r->speed = 0x14; }           /* 1FACA: bchg #7 (jitter) */
        else if (d0 < 0x28) r->speed = 0;                                    /* 1FAD8: stand */
        else                r->speed = 0x14;                                 /* 1FAE4 */
    }
    /* --- visual 3 0x1FD5E --- */
    if (!r->vis_init) {                              /* 1FD5E first entry */
        r->vis_init = 1;
        st->count_out = 0;                           /* clr $1C169A */
        r->t24 = 0;
        r->ym = (uint16_t)ringout_rule(RO_WARN_YM);  /* +0x54 = $3165 (warning YM ids) */
        r->t23 = ringout_rule(RO_POSE_FRAMES);
        r->pose = ringout_rule(RO_POSE_FIRST);
        r->facing = (uint16_t)(r->angle == 0xC0 ? 0x8000u : 0);   /* +0x04 = +0x2E */
        if (r->t22 <= 0) r->t22 = 8;                 /* +0x22 as left by the previous visual (oracle: 5
                                                        at entry) — TODO EXACT: engine idle keeps it in p23 */
    }
    {                                                /* 1FD88 jsr 0x2208 (mover 1 as left by the servo) */
        int32_t dx, dy;
        if (r->speed) { eng_sincos_step(r->angle, r->speed, &dx, &dy); r->x += dx; r->y += dy; }
    }
    {
        int lo = ringout_rule(RO_REF_X_MIN), hi = ringout_rule(RO_REF_X_MAX);
        rx = r->x >> 16;
        if (rx < lo) r->x = lo << 16;                /* 1FD8E clamp [0x298, 0x390] */
        else if (rx >= hi) r->x = hi << 16;
    }
    if (--r->t22 == 0) {                             /* 1FDAC: +0x22 word, reloaded via its low byte +0x23 */
        int last = ringout_rule(RO_POSE_LAST);
        r->t23 = ringout_rule(RO_POSE_FRAMES);
        if (r->pose == last) r->pose = ringout_rule(RO_POSE_FIRST);      /* 1FDBA */
        else if (++r->pose == last) r->t23 += ringout_rule(RO_POSE_LAST_EXTRA);  /* 1FDD0 */
        r->t22 = r->t23;
        r->facing = (uint16_t)(r->angle == 0xC0 ? 0x8000u : 0);
    }
    r->spr = (uint16_t)((unsigned)r->pose | (r->facing & 0x8000u));
    {
        unsigned n = st->count_out & 0xFFu;
        unsigned resolve = (unsigned)ringout_rule(RO_RESOLVE_COUNT);
        /* the count is drawn with two digits: a modded resolve_count caps at 99;
         * the four warning calls ($3165..$3168) stay on the LAST four counts,
         * wherever the resolve is (stock: warn_count 17 = resolve 20 - 3) */
        unsigned warn = (unsigned)ringout_rule(RO_WARN_COUNT);
        if (resolve > 99) resolve = 99;
        if (resolve != 20) warn = resolve > 3 ? resolve - 3 : 1;
        if (n == resolve) return;                    /* 1FDE6: parked at 20 */
        /* --- tick 0x1FDF4 --- */
        if (!eng_mode_rule(MODE_COUNTOUT)) return;   /* mode: the 20-count never runs */
        if (++r->t24 < ringout_rule(RO_FRAMES_PER_COUNT)) return;   /* 0x50 frames per count */
        r->t24 = 0;
        st->count_out++;                             /* 1FE04 */
        n = st->count_out & 0xFFu;
        if (!eng_legal_pair(st, &a, &b)) return;
        if (n == resolve && !(a->role & RF_OUTSIDE) && !(b->role & RF_OUTSIDE))
            return;                                  /* 1FE18: nobody outside at 20 — park, no result */
        digit_blit(n);                               /* 1FE2A 0x2067C */
        if (n >= warn && r->ym < (unsigned)ringout_rule(RO_WARN_YM) + 4u)
            eng_sound(r->ym++);                      /* 1FE36: $3165..$3168, never past the 4th */
        if (n != resolve) return;
    }
    /* --- resolve 0x1FE5A --- */
    r->t22 = ringout_rule(RO_FREEZE_T22);            /* pose timer frozen */
    st->sig169e = 0;                                 /* clr $1C169E (decided; clock off) */
    if ((a->role & RF_OUTSIDE) && (b->role & RF_OUTSIDE)) {      /* 1FE76: both out — double count-out */
        eng_obj *q[4] = { a, a->teammate >= 0 ? &st->obj[a->teammate] : 0,
                          b, b->teammate >= 0 ? &st->obj[b->teammate] : 0 };
        for (int i = 0; i < 4; i++)
            if (q[i]) q[i]->result = (uint16_t)ringout_rule(RO_RESULT_DOUBLE);
        eng_sound(0x08);                             /* YM $3108 */
        eng_blit(0x52);                              /* 0x2503C(0x52) */
        return;
    }
    {
        /* 1FEB2 (A2 inside) / 1FF02 (A3 inside): the OUTSIDE man's pair gets
         * $8003 (+0xFF b0 = loser pose 0x8C per 0x11598), the inside pair
         * $8002. hud-rules.md has it right; ringout-scene.md §2a swapped A2/A3. */
        eng_obj *in  = (a->role & RF_OUTSIDE) ? b : a;
        eng_obj *out = (a->role & RF_OUTSIDE) ? a : b;
        uint16_t r_in  = (uint16_t)ringout_rule(RO_RESULT_IN);
        uint16_t r_out = (uint16_t)ringout_rule(RO_RESULT_OUT);
        in->result = r_in;   if (in->teammate  >= 0) st->obj[in->teammate].result  = r_in;
        out->result = r_out; if (out->teammate >= 0) st->obj[out->teammate].result = r_out;
        /* TODO EXACT 0x90D6 (leftover-energy scoring) */
        if (out->cpu) eng_sound(0x05);               /* 0x20156 jingle ($3105; $3109 on stages 4/9) */
        else { eng_sound(0x08); eng_blit(0x52); }    /* YM $3108 + blit 0x52 */
    }
}

static int hunt(eng_state *st)                      /* 0x20556 */
{
    if (!eng_mode_rule(MODE_PIN)) return -1;   /* mode: pins never counted */
    for (int i = 0; i < ENG_MAX_OBJS; i++)
        if (st->obj[i].active && (st->obj[i].cue_flags & 1u)
            && !(st->obj[i].result & 0x8000u))  /* the match is DECIDED: a cover made
                                          while the win/lose flow plays gets no count
                                          (user 2026-08-28, like a cover outside) */
            return i;
    for (int i = 0; i < ENG_MAX_OBJS; i++)          /* holder: $8004 watch */
        if (st->obj[i].active && (st->obj[i].cue_flags & 2u))
            return -2 - i;
    return -1;
}

void eng_referee_frame(eng_state *st)
{
    eng_ref *r = &st->ref;
    eng_obj *t = r->target >= 0 && r->target < ENG_MAX_OBJS
               ? &st->obj[r->target] : 0;

    if (!r->active)
        return;

    switch (r->sm) {
    case 3:
        ref_sm3_countout(st, r);
        break;
    case 5: {
        /* SM5 0x1F99E + visual 0 0x1FB46 (referee-walk.md §1): the action
         * point is the midpoint of the two legal men; f35 b3/b4 classify
         * it; the heading comes from 0x2060C indexed by rope memory and
         * those bits; the one stand condition is 0x205C0. */
        int32_t ax, ay, rx = r->x >> 16, ry = r->y >> 16;
        const eng_obj *a = &st->obj[0], *b = &st->obj[2];
        for (int i = 0; i < ENG_MAX_OBJS; i++)           /* 0x204FA: the two legal men */
            if (st->obj[i].active && (st->obj[i].role & RF_LEGAL) && !(st->obj[i].st_flags & SF_QUEUED)) {
                if (!(a->role & RF_LEGAL) || a == b) a = &st->obj[i];
                else if (&st->obj[i] != a) b = &st->obj[i];
            }
        int32_t lx = (a->x >> 16) < (b->x >> 16) ? (a->x >> 16) : (b->x >> 16);
        int32_t ly = (a->y >> 16) < (b->y >> 16) ? (a->y >> 16) : (b->y >> 16);
        ax = lx + labs((a->x >> 16) - (b->x >> 16)) / 2;        /* 0x204B2 */
        ay = ly + labs((a->y >> 16) - (b->y >> 16)) / 2;
        if (st->g161 & 1u) { ax = 0x268; ay = 0x158; }          /* 0x1F9B0 rumble: fixed action point */
        r->cue_flags &= (uint16_t)~0x18u;                              /* 0x2052E */
        if (ax < 0x270) r->cue_flags |= CF_CHIP_CHANGE;
        if (ay < 0x160) r->cue_flags |= CF_CHIP_P;
        if (r->vis == 5) {
            /* ---- visual 5, 0x1FF52: the $8005 walk BACK TO THE ROPES.
             * Every ROM return-to-idle enters here; he walks the 0x2001E
             * heading for his rope memory (no memory -> 0x00, the back
             * line y 0x198 behind the wrestlers) until the 0x2001A clip
             * mask fires, then parks in $8000. */
            unsigned idx = (r->cue_flags & 0x06u) >> 1;
            if (!r->vis_init) {                                  /* 0x1FF52 first entry */
                r->vis_init = 1;
                r->speed = 0x16;                                 /* 0x1FF5A */
                r->pose = 0; r->p25 = 4; r->p23 = 1;             /* 0x1FF60-0x1FF6C */
                /* 0x1FF72 visual 0x0A (cage escort) -> fixed 0xC0: no cage yet */
                r->angle = tbl8(TBL(ref_return_angles), idx);    /* 0x1FF82-0x1FF92 */
            }
            {
                int32_t dx, dy;
                unsigned clip;
                eng_sincos_step(r->angle, r->speed, &dx, &dy);   /* 0x1FF98 jsr 0x2208 */
                r->x += dx; r->y += dy;
                clip = ref_probe(r);                             /* 0x1FFA2 jsr 0x280DC (pushback 0x1FFBC) */
                if (clip) {
                    unsigned m = tbl8(TBL(ref_return_masks), idx);   /* 0x1FFD6 */
                    if ((clip & m) == m) {                       /* 0x1FFE2 arrived */
                        r->sm = 5; r->vis = 0; r->vis_init = 0;  /* 0x1FFEC state $8000 */
                        r->spr = (uint16_t)((unsigned)r->pose | (r->facing & 0x8000u));
                        break;                                   /* 0x1FFF2 rts */
                    }
                }
            }
            if (--r->p23 == 0) {                                 /* 0x1FFF4 pose clock */
                r->p23 = 6;                                      /* 0x1FFFA */
                if (--r->p25 == 0) { r->pose = 0; r->p25 = 4; }  /* 0x20006 */
                else r->pose++;                                  /* 0x20014 */
            }
        } else if (st->g161 & 1u) {
            /* ---- visual 0, RUMBLE (0x1FC06): NO idle walk — first entry
             * only re-seeds the pose clock and facing, then he stands
             * where the walk-back left him (the rope line). */
            if (!r->vis_init) {                                  /* 0x1FC06-0x1FC1A */
                r->vis_init = 1; r->p23 = 8; r->pose = 0;
            }
        } else {
            /* ---- visual 0, singles/tag (0x1FB46): follow the action point */
            if (!r->vis_init) {                                  /* 0x1FB52 */
                r->vis_init = 1; r->p23 = 8; r->p25 = 4; r->pose = 0;
            }
            {                                                    /* 0x205AC */
                unsigned idx = (r->cue_flags & 0x1Eu) >> 1;
                uint16_t d1 = (uint16_t)(ax - rx);
                int stand = 0;
                if (d1 < 8) {                                    /* 0x205C0 */
                    unsigned m = idx & 0x0Au;
                    if (m != 0 && m != 0x0Au) stand = 1;
                }
                if (stand) {
                    r->speed = 0;                                /* 0x205D6 */
                } else {
                    if (ax < rx) { idx |= 0x10u; r->facing = 0x8000u; } /* 0x205DC */
                    r->angle = tbl8(TBL(ref_approach_angles), idx);    /* 0x205F8 */
                    r->speed = 0x16;                             /* 0x20604 */
                }
            }
            if (r->speed) {
                int32_t dx, dy;
                unsigned clip;
                eng_sincos_step(r->angle, r->speed, &dx, &dy);   /* 0x2208 */
                r->x += dx; r->y += dy;
                clip = ref_probe(r);                             /* 0x280DC probe */
                if (clip) {                                      /* 0x1FB96 */
                    r->cue_flags &= (uint16_t)~0x06u;                  /* 0x1FBAC */
                    if (clip & 0x01u)      r->cue_flags |= 0x06u;      /* right rope */
                    else if (clip & 0x02u) r->cue_flags |= CF_SUB_HOLDER;      /* left rope */
                    else if (clip & 0x08u) r->cue_flags |= CF_SUB_VICTIM;      /* top rope */
                }
                if (--r->p23 == 0) {                             /* 0x1FBE0 */
                    r->p23 = 8;
                    if (--r->p25 == 0) { r->pose = 0; r->p25 = 4; }
                    else r->pose++;
                }
            }
        }
        r->spr = (uint16_t)((unsigned)r->pose | (r->facing & 0x8000u));
        (void)ry;
        int p = hunt(st);
        if (p >= 0) { r->target = p; r->sm = 1; }
        else if (p <= -2) { r->target = -2 - p; r->sm = 4; r->vis_init = 0; }
        break; }
    case 4: {                                       /* $8004: walk to the hold */
        int32_t dx, dy, d1 = 0x30;
        if (!t || !(t->cue_flags & 2u) || t->result) { ref_to_idle(r); break; }
        if ((t->move_id & 0xFFu) == 0x09) d1 = 0x50;
        dx = (t->x >> 16) + (((t->x >> 16) > 0x280) ? -d1 : d1) - (r->x >> 16);
        dy = (t->y >> 16) - 0x10 - (((t->move_id & 0xFFu) == 0x22) ? 0x10 : 0) - (r->y >> 16);
        if (dx > 2) r->x += 0x1C000; else if (dx < -2) r->x -= 0x1C000;
        if (dy > 2) r->y += 0x1C000; else if (dy < -2) r->y -= 0x1C000;
        r->facing = (uint16_t)(dx < 0 ? 0x8000u : 0);
        r->spr = (uint16_t)(((st->frame >> 3) & 3) | (r->facing & 0x8000u));
        if (ref_probe(r)) break;                     /* 0x2062C: clipped = not arrived */
        if (dx <= 6 && dx >= -6 && dy <= 6 && dy >= -6) {   /* 0x2062C */
            r->y = t->y + (1 << 16);
            r->sm = 8; r->t23 = 0x0A; r->cell = 0x0D;
            r->facing = (uint16_t)(((t->x >> 16) >= (r->x >> 16)) ? 0x8000u : 0);
        }
        break; }
    case 8: {                                       /* $8008: watch (0x20354) */
        if (!t || !(t->cue_flags & 2u) || t->result) { ref_to_idle(r); break; }
        if (hunt(st) >= 0) { r->target = hunt(st); r->sm = 1; break; }
        if (--r->t23 == 0) {
            if (r->cell == 0x0D) { r->cell = 0x0E; r->t23 = 8; }
            else                 { r->cell = 0x0D; r->t23 = 0x0A; }
        }
        r->spr = (uint16_t)((unsigned)r->cell | (r->facing & 0x8000u));
        break; }
    case 2: {                                       /* SM2 0x1F9F8: the usher escort — walk to the
                                                       run-in man, point him out (SM7 visual 0x202BA),
                                                       recall every intruder (+0x34 b1) */
        eng_obj *u = (r->target >= 0 && r->target < ENG_MAX_OBJS) ? &st->obj[r->target] : 0;
        {   /* 0x1F9F8 head: bsr 0x20556 EVERY frame — a fresh pin cue
             * yanks the escort to the count (SM1) / hold watch (SM4), so a
             * SECOND cover during the escort is always counted. */
            int p = hunt(st);
            if (p >= 0) { r->target = p; r->sm = 1; r->pose = 0; r->p23 = 0; break; }
            if (p <= -2) { r->target = -2 - p; r->sm = 4; r->vis_init = 0; r->pose = 0; r->p23 = 0; break; }
        }
        if (!u || !u->active || u->apron || (u->role & RF_LEGAL) || (u->tag_flags & TF_RECALL)) {
            int nxt = -1;                           /* 0x1FA20: the second man $1C1688 */
            for (int i = 0; i < ENG_MAX_OBJS; i++) {
                const eng_obj *q = &st->obj[i];
                if (q->active && q->ai_runin_t && !q->apron && !(q->role & RF_LEGAL) && !(q->tag_flags & TF_RECALL)) { nxt = i; break; }
            }
            if (nxt < 0) { ref_to_idle(r); break; }
            r->target = nxt; r->pose = 0; r->p23 = 0;
            break;
        }
        if (!r->p23) {                              /* walk over (visual 0x1FC22, shared with SM1) */
            /* 0x1FC92: dest = target.x -/+ 0x50 toward ring CENTRE (0x280)
             * — always a reachable in-ring spot. (Was +/-0x30 AWAY from
             * centre: with the intruder near a rope the dest lay outside
             * the ropes, ref_probe clipped forever and the referee stuck
             * in SM2 for the rest of the match.) */
            int32_t dx = (u->x >> 16) + (((u->x >> 16) > 0x280) ? -0x50 : 0x50) - (r->x >> 16);
            int32_t dy = (u->y >> 16) + 1 - (r->y >> 16);
            if (dx > 2) r->x += 0x18000; else if (dx < -2) r->x -= 0x18000;
            if (dy > 2) r->y += 0x18000; else if (dy < -2) r->y -= 0x18000;
            r->spr = (uint16_t)((((st->frame >> 3) & 1) ? 1 : 0) | (dx > 0 ? 0x8000 : 0));
            ref_probe(r);
            if (dx <= 8 && dx >= -8 && dy <= 8 && dy >= -8) {
                r->p23 = 0x28;                      /* 0x202CA pose clock */
                r->facing = (uint16_t)(((u->x >> 16) >= (r->x >> 16)) ? 0x8000 : 0);   /* 0x202DE */
                for (int i = 0; i < ENG_MAX_OBJS; i++) {   /* 0x202F4-0x20322 recall all intruders */
                    eng_obj *q = &st->obj[i];
                    if (q->active && q->ai_runin_t && !q->apron && !(q->role & RF_LEGAL)) {
                        q->tag_flags |= TF_RECALL; q->tag_flags &= (uint16_t)~0x01u; q->ai_sub = 0; q->ai_runin_t = 0;
                        if (q->teammate >= 0) eng_tag_restore_control(st, &st->obj[q->teammate]);
                        if (eng_dbgsel) fprintf(stderr, "ref: usher points o%d out (0x202BA)\n", i);
                    }
                }
            }
            break;
        }
        r->spr = (uint16_t)(7u | (r->facing & 0x8000u));   /* visual 7: the pointing pose */
        if (--r->p23 == 0) { ref_to_idle(r); }
        break; }
    case 1: {                                       /* approach the pin */
        int32_t dx, dy;
        if (!t || !(t->cue_flags & 1u)) { digit_wipe(); ref_to_idle(r); break; }   /* 0x1F9F0 $8005 */
        /* 0x1FC92: dest = target.x -/+ 0x50 toward ring centre (0x280) */
        dx = (t->x >> 16) + (((t->x >> 16) > 0x280) ? -0x50 : 0x50) - (r->x >> 16);
        dy = (t->y >> 16) + 1 - (r->y >> 16);
        if ((t->role & RF_OUTSIDE) && eng_mod_rule(MODR_PIN_OUTSIDE)) {   /* mod pin_outside: the ring law would never
                                                                       let him walk out there - he joins the pile */
            r->x = t->x + ((((t->x >> 16) > 0x280) ? -0x50 : 0x50) << 16); r->y = t->y + (1 << 16);
            r->sm = 6; r->cell = 0; r->t23 = 0x14; r->spr = 4; r->ym = 0x62;
            break;
        }
        if (dx > 2) r->x += 0x18000; else if (dx < -2) r->x -= 0x18000;
        if (dy > 2) r->y += 0x18000; else if (dy < -2) r->y -= 0x18000;
        r->spr = (uint16_t)(((st->frame >> 3) & 1) ? 1 : 0);
        if (ref_probe(r)) break;                     /* 0x2062C: clipped = not arrived */
        if (dx <= 6 && dx >= -6 && dy <= 6 && dy >= -6) {   /* 0x2062C */
            r->sm = 6;                               /* $8006 */
            r->cell = 0;
            r->t23 = 0x14;                           /* 0x20022 seed */
            r->spr = 4;
            r->ym = 0x62;                            /* $3162 ONE */
        }
        break; }
    case 6: {                                       /* the 1-2-3 */
        if (r->cell < 6 && (!t || !(t->cue_flags & 1u)
                            || (t->state & 0xFFu) == ST_REACT)) {   /* abort (SM6->SM1); engine safety
                                          net: a pinner LYING DOWN (any path that floored him
                                          without eng_pin_break's cue kill) ends the count
                                          too - "the ref kept counting" (user 2026-08-30) */
            if (t && (t->state & 0xFFu) == ST_REACT) { t->cue_flags &= (uint16_t)~1u; t->pinning = 0; t->partner = -1; }
            digit_wipe();
            ref_to_idle(r);              /* 0x1F9EC/0x1F9F0 $8005 */
            break;
        }
        if (--r->t23 > 0)
            break;
        r->cell++;
        if (t && t->partner >= 0)
            st->obj[t->partner].halfct++;            /* +0x109 */
        /* (the "It's a two-count!" call is NOT here: 0x21282 says it when the
           pin ENDS with the half-count at 4 or 5 - eng_tag_pin_end_count) */
        if (r->cell == 7) {                          /* fall complete */
            digit_wipe();
            /* (SURVIVOR gate for the stamp below lives at cell 6) */
            if (st->g161 & 1u) {                     /* 0x200D4 RUMBLE: the pinned man is
                                                        ELIMINATED, no result, referee back
                                                        to idle (0x2014A -> $8005) */
                if (t) {
                    eng_obj *v = t->partner >= 0 ? &st->obj[t->partner] : 0;
                    if (v) {
                        v->st_flags |= SF_ELIMINATED;             /* 0x200F0 bset #4,+0x32 */
                        eng_announce(st, (unsigned)v->wrestler, 0x29);   /* 0x18130 "eliminated" */
                        v->state = ST_REACT; v->react_id = (uint16_t)((v->react_id & 0xFF00u) | 8);
                        v->down_t = 0x20; v->partner = -1;   /* 0x18184 */
                        v->mash_aa = 0;
                    }
                    t->state = ST_GETUP; t->pinning = 0; t->partner = -1;   /* 0x18172 release */
                    t->cue_flags &= (uint16_t)~1u;
                    /* +0xC4++ score and 0x21358 re-decide: TODO */
                }
                ref_to_idle(r); r->target = -1;   /* 0x2014A $8005 */
                break;
            }
            if (eng_mode_rule(MODE_ELIM) && t && t->partner >= 0) {
                /* MODE: SURVIVOR elimination (mirrors the rumble branch
                 * 0x200D4 shape). The pinned man is ELIMINATED and walks
                 * off (0x7A); his next teammate is promoted by the stock
                 * swap; the match runs on unless he was his side's LAST. */
                eng_obj *v = &st->obj[t->partner];
                int alive = 0, nx = v->teammate;
                for (int i = 0; i < ENG_MAX_OBJS; i++) {
                    eng_obj *q = &st->obj[i];
                    if (q != v && q->active && !(q->st_flags & SF_ELIMINATED)
                        && !((q->role ^ v->role) & 0x80u)) alive++;
                }
                if (alive > 0) {
                    eng_announce(st, (unsigned)v->wrestler, 0x29);   /* "eliminated" */
                    t->state = ST_GETUP; t->pinning = 0; t->partner = -1;   /* 0x18172 release */
                    t->cue_flags &= (uint16_t)~1u;
                    if (nx >= 0 && st->obj[nx].active)
                        eng_tag_swap(st, v, &st->obj[nx]);       /* promote the next man */
                    for (int i = 0; i < ENG_MAX_OBJS; i++)   /* bypass v in the circle */
                        if (st->obj[i].active && st->obj[i].teammate == (int)(v - st->obj))
                            st->obj[i].teammate = (v->teammate == (int)(st->obj[i].teammate)) ? -1 : v->teammate;
                    v->teammate = -1;
                    v->st_flags |= SF_ELIMINATED;                 /* eliminated (+0x32 b4) */
                    v->apron = 0; v->partner = -1; v->mash_aa = 0;
                    v->driver &= (uint16_t)~0x40u; v->tag_flags &= (uint16_t)~0x03u;
                    v->state = ST_MOVE; v->move_id = 0x7A; v->grap44 = 0;   /* the walk-off */
                    if (eng_dbgsel)
                        fprintf(stderr, "ref: SURVIVOR — o%d eliminated, o%d promoted\n",
                                (int)(v - st->obj), nx);
                    ref_to_idle(r); r->target = -1;
                    break;
                }
                /* his side's last man: the normal fall ends the match */
            }
            if (t) {
                t->cue_flags &= (uint16_t)~1u;             /* the cue dies with the fall —
                                                        a live cue re-hunts after the
                                                        win pose and the count LOOPED
                                                        on the scripted pins (splash
                                                        0x51: no result-reaction in
                                                        its mover). 0x18172-family. */
                t->pinning = 0;
            }
            r->sm = 9;
            r->win_t = 44;                           /* 0x203EE: 8 + 18 + 18 */
            r->spr = 0x0F;
            break;
        }
        if (!(r->cell & 1)) {                        /* full count lands */
            digit_blit((unsigned)r->cell >> 1);
            eng_sound(r->ym++);                      /* 3162/63/64 */
        }
        r->spr = (uint16_t)(r->spr == 5 ? 6 : 5);
        r->t23 = r->spr == 5 ? 0x38 : 0x20;
        if (r->cell == 6) {                          /* stamp the result */
            int elim_more = 0;
            if (eng_mode_rule(MODE_ELIM) && t && t->partner >= 0) {
                eng_obj *v = &st->obj[t->partner];   /* SURVIVOR: no result words
                                                        unless this is his side's
                                                        LAST man */
                for (int i = 0; i < ENG_MAX_OBJS; i++) {
                    eng_obj *q = &st->obj[i];
                    if (q != v && q->active && !(q->st_flags & SF_ELIMINATED)
                        && !((q->role ^ v->role) & 0x80u)) elim_more = 1;
                }
            }
            r->t23 += 0x10;
            if (t && !elim_more && !(st->g161 & 1u)) {   /* rumble: no result words (0x200D4) */
                /* the TEAMS share the fall — stamped by SIDE so 3-man
                 * teams (mode team_size 3) catch every member: winners
                 * take the 0x8B high-five, losers the 0x8C pose (user
                 * spec 2026-08-24; TODO EXACT the ROM stamp) */
                for (int i = 0; i < ENG_MAX_OBJS; i++) {
                    eng_obj *q = &st->obj[i];
                    if (!q->active || q->result) continue;
                    q->result = (uint16_t)(((q->role ^ t->role) & 0x80u) ? 0x4001 : 0x4000);
                }
            }
        }
        break; }
    case 10: {                                       /* MOD: KNOCKED DOWN (no stock
                                                        analog — user 2026-08-24).
                                                        Kneeling count poses shaken
                                                        slowly; NO hunts, NO counts,
                                                        the ring-out count is paused
                                                        (ringout.c gate) until the
                                                        recovery timer runs out. */
        r->spr = (uint16_t)(6u | (r->facing & 0x8000u));   /* STILL — one kneel
                                          pose, no shake (user 2026-08-24) */
        if (r->win_t && --r->win_t == 0)
            ref_to_idle(r);                          /* back up: SM5 re-hunts live cues */
        break; }
    case 9:                                          /* win pose F/10/11 (0x203EE) */
        r->spr = (uint16_t)((r->win_t > 36 ? 0x0F : r->win_t > 18 ? 0x10 : 0x11)
                            | (r->facing & 0x8000u));
        if (r->win_t == 18) {                        /* 0x2040A: cell 0x10 expired */
            int human = (t && !t->cpu);
            eng_sound(0x05);                         /* 0x20156: stage jingle (stage 0 ->
                                                        0x3105; 4/9 -> 0x3109 TODO) */
            eng_announce(st, 0x0F, human ? 0x2A : 0x2C);   /* bells + roar / bells */
        }
        if (--r->win_t == 0) {
            if (t && t->cpu) eng_sound(0x08);        /* 0x20472: CPU winner fanfare */
            for (int i = 0; i < ENG_MAX_OBJS; i++)
                if (st->obj[i].active && st->obj[i].result)
                    st->obj[i].result |= 0x8000;     /* $C000/$C001 final */
            /* End-of-match sweep (user spec 2026-08-24, TODO EXACT the
             * ROM's bell sweep): every decided man is freed from whatever
             * he is doing so the 0x11598 stand row can route him into the
             * 0x8B walk-to-centre high-five / 0x8C loser pose; a WINNING
             * apron partner climbs in (0x4E) to join the celebration. */
            if (!(st->g161 & 1u)) {
                for (int i = 0; i < ENG_MAX_OBJS; i++) {
                    eng_obj *o = &st->obj[i];
                    unsigned s = o->state & 0xFFu;
                    if (!o->active || !o->result) continue;
                    if (s == 0x0B || s == 0x0C || s == 0xFF) {
                        o->state = ST_STAND; o->grap44 = 0; o->count = 0;
                        o->role &= (uint16_t)~0x40u; o->pinning = 0;
                    }
                    o->partner = -1;
                    if (o->result & 1u) continue;    /* losers keep their lie */
                    o->tag_flags &= (uint16_t)~0x03u;      /* no recall fights the pose */
                    if ((o->state & 0xFFu) == ST_REACT)
                        o->down_t = 1;               /* up at once, whatever he was */
                    if ((o->state & 0xFFu) == ST_MOVE && o->move_id != 0x8B && o->move_id != 0x4E) {
                        /* a winner still inside his own move record — a pin
                         * HOLD (plex 0xFF00-hold: Perfect stayed in the
                         * bridge, playtest 2026-08-26) — drops to the stand
                         * row so it can route him into the walk */
                        o->state = ST_STAND; o->grap44 = 0; o->count = 0;
                        o->pinning = 0; o->spr_force = 0;
                        o->off_x = 0; o->off_y = 0;
                    }
                    if (o->apron) {                  /* the partner joins in */
                        o->apron = 0; o->sub = 0;
                        o->state = ST_MOVE; o->move_id = 0x4E; o->grap44 = 0;
                    }
                }
            }
            ref_to_idle(r);                      /* 0x20482 -> $8005 */
        }
        break;
    default:
        ref_to_idle(r);
        break;
    }

    r->sx = (int16_t)((r->x >> 16) - st->cam_x);     /* 0x247C */
    r->sy = (int16_t)((r->y >> 16) + (r->z >> 16) - st->cam_y);
}

/* the external KNOCKED-OUT art frame while the referee is down (SM10):
 * badges ref_down_0 (falling) / _1 (mid-fall) / _2 (flat, held) - 3 frames
 * (user 2026-08-30), 8 ticks each on the way down. NULL = not down. */
const char *eng_ref_down_badge(const eng_state *st)
{
    static const char *const names[3] = { "ref_down_0", "ref_down_1", "ref_down_2" };
    unsigned total, elapsed;
    if (st->ref.sm != 10) return NULL;
    total = (unsigned)eng_mod_rule(MODR_REF_DOWN_FRAMES); if (!total) total = 0x708;
    elapsed = total > st->ref.win_t ? total - st->ref.win_t : 0;
    return names[elapsed < 8 ? 0 : elapsed < 16 ? 1 : 2];
}

/* MOD ref knockdown entry (modrules ref_knockdown): any live SM except the
 * win pose drops him; a running 1-2-3 wipes its digits (the pin CUE stays —
 * a cover with the referee down simply is not counted until he recovers). */
void eng_ref_knockdown(eng_state *st)
{
    eng_ref *r = &st->ref;
    if (r->sm == 9 || r->sm == 10 || (st->g161 & 2u)) return;
    if (r->sm == 6) eng_ref_digit_wipe();
    r->sm = 10;
    r->win_t = (uint16_t)eng_mod_rule(MODR_REF_DOWN_FRAMES);
    if (!r->win_t) r->win_t = 0x708;
    eng_sound(0x2E); eng_sound(0x32);  /* the weapon-shot pair (0x24648/0x24652) */
    eng_announce(st, 0x0F, 0x28);      /* the "OH NO" call — the same $0F28 the
                                          weapon shot makes (user 2026-08-24:
                                          "same as when you hit someone with
                                          the stairs") */
    if (eng_dbgsel) fprintf(stderr, "ref: KNOCKED DOWN for %u frames (mod)\n", r->win_t);
}

/* MODE cage escape (mode cage_escape_win): the ESCAPER'S team wins. */
void eng_match_escape_win(eng_state *st, eng_obj *winner)
{
    eng_ref *r = &st->ref;
    if (r->sm == 9 || (st->g161 & 1u)) return;
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *q = &st->obj[i];
        if (!q->active) continue;
        q->result = (uint16_t)(((q->role ^ winner->role) & 0x80u) ? 0x4001 : 0x4000);
    }
    r->sm = 9; r->win_t = 44; r->spr = 0x0F;
    r->target = (int)(winner - st->obj);
    eng_ref_digit_wipe();
    if (eng_dbgsel) fprintf(stderr, "match: o%d ESCAPES THE CAGE — his team wins (mode)\n",
                            (int)(winner - st->obj));
}

/* MOD weapon DQ (modrules weapon_dq): the offender's team loses on the
 * spot — results stamped like a fall, the referee takes the win pose and
 * the end-of-match sweep/celebration runs as normal. */
void eng_match_dq(eng_state *st, eng_obj *offender)
{
    eng_ref *r = &st->ref;
    eng_obj *winner = 0;
    if (r->sm == 9 || (st->g161 & 1u)) return;       /* decided / rumble: no DQ */
    for (int i = 0; i < ENG_MAX_OBJS; i++) {
        eng_obj *q = &st->obj[i];
        if (!q->active) continue;
        if (((q->role ^ offender->role) & 0x80u)) {    /* enemy team wins */
            q->result = 0x4000;
            if (!winner && (q->role & RF_LEGAL)) winner = q;
        } else
            q->result = 0x4001;
    }
    r->sm = 9; r->win_t = 44; r->spr = 0x0F;
    r->target = winner ? (int)(winner - st->obj) : -1;
    eng_ref_digit_wipe();
    if (eng_dbgsel) fprintf(stderr, "match: DQ on o%d (weapon, mod)\n", (int)(offender - st->obj));
}
