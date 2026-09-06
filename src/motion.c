/* Object motion — transcription of ROM 0x2208 (apply_motion), 0x22C0
 * (polar trig core) and the in-ring bounds of 0x280DC/0x2818E.
 * Spec: scratchpad mv/apply-motion.md + mv/input-walk.md §6 (2026-08-22),
 * derived from reference/maincpu.asm. Integer-only; positions are 16.16.
 */
#include "engine.h"

/* ROM 0x2378: round(5120*sin(i*pi/128)), i=0..64. X component — the 1.25
 * aspect boost (5120 = 1.25*4096) is deliberate. Verbatim ROM data. */
static const uint16_t SIN5120[65] = {
0x0000,0x007E,0x00FB,0x0179,0x01F6,0x0273,0x02EF,0x036B,0x03E7,0x0462,
0x04DC,0x0556,0x05CE,0x0646,0x06BD,0x0733,0x07A7,0x081B,0x088D,0x08FE,
0x096E,0x09DC,0x0A48,0x0AB3,0x0B1D,0x0B84,0x0BEA,0x0C4E,0x0CB0,0x0D10,
0x0D6E,0x0DCA,0x0E24,0x0E7C,0x0ED2,0x0F25,0x0F76,0x0FC4,0x1010,0x105A,
0x10A1,0x10E6,0x1128,0x1167,0x11A3,0x11DD,0x1214,0x1249,0x127A,0x12A9,
0x12D5,0x12FE,0x1324,0x1347,0x1367,0x1384,0x139E,0x13B5,0x13C9,0x13D9,
0x13E7,0x13F2,0x13FA,0x13FE,0x1400 };

/* ROM 0x23FA: round(4096*cos(i*pi/128)). Y component. */
static const uint16_t COS4096[65] = {
0x1000,0x0FFF,0x0FFB,0x0FF5,0x0FEC,0x0FE1,0x0FD4,0x0FC4,0x0FB1,0x0F9C,
0x0F85,0x0F6C,0x0F50,0x0F31,0x0F11,0x0EEE,0x0EC8,0x0EA1,0x0E77,0x0E4B,
0x0E1C,0x0DEC,0x0DB9,0x0D85,0x0D4E,0x0D15,0x0CDA,0x0C9B,0x0C5E,0x0C1E,
0x0BDB,0x0B97,0x0B50,0x0B08,0x0ABF,0x0A73,0x0A26,0x09D8,0x0988,0x0937,
0x08E4,0x088F,0x083A,0x07E3,0x078B,0x0732,0x06D7,0x067C,0x061F,0x05C2,
0x0564,0x0505,0x04A5,0x0444,0x03E3,0x0381,0x031F,0x02BC,0x0259,0x01F5,
0x0191,0x012D,0x00C9,0x0065,0x0000 };

/* ROM 0x22C0-0x2376. Angle: 0 = +y (away/up-screen), 0x40 = +x (right),
 * clockwise 256-step circle. mulu.w semantics: u16*u16 -> u32, sign by
 * neg.l per quadrant; mirror quadrants index 0x40-n (1..64). */
void eng_sincos_step(uint32_t angle, uint32_t speed, int32_t *dx, int32_t *dy)
{
    switch ((angle >> 6) & 3) {
    case 0: {                                   /* 0x22E0 */
        uint32_t i = angle & 0x3F;
        *dx =  (int32_t)(SIN5120[i] * speed);
        *dy =  (int32_t)(COS4096[i] * speed);
        break; }
    case 1: {                                   /* 0x22FE */
        uint32_t i = 0x40u - (angle - 0x40u);
        *dx =  (int32_t)(SIN5120[i] * speed);
        *dy = -(int32_t)(COS4096[i] * speed);   /* neg.l 0x2324 */
        break; }
    case 2: {                                   /* 0x2328 */
        uint32_t i = angle - 0x80u;
        *dx = -(int32_t)(SIN5120[i] * speed);   /* neg.l 0x2348 */
        *dy = -(int32_t)(COS4096[i] * speed);   /* neg.l 0x234A */
        break; }
    default: {                                  /* 0x234E */
        uint32_t i = 0x40u - (angle - 0xC0u);
        *dx = -(int32_t)(SIN5120[i] * speed);   /* neg.l 0x2374 */
        *dy =  (int32_t)(COS4096[i] * speed);
        break; }
    }
}

/* ROM 0x2208: 3-way dispatcher on the mode byte (+0x01). The only gate
 * here is the mode; freeze (+0x32 bit7) and slot-active live in the
 * caller loop (0xF4C2/0x250E). */
void eng_apply_motion(eng_obj *o)
{
    switch (o->mover) {
    case 0:
    default:
        return;

    case 1: {                                   /* 0x222C polar walk */
        uint32_t angle = o->angle & 0xFFu;
        uint32_t speed = o->speed & 0xFFu;
        int32_t dx, dy;
        eng_sincos_step(angle, speed, &dx, &dy);
        o->x += dx;                             /* 16.16 add.l 0x2248 */
        o->y += dy;                             /* 0x224C */
        return; }

    case 2:                                     /* 0x2252 velocity+gravity */
        o->x += ((int32_t)o->vx) << 8;          /* 8.8 px/frame */
        o->y += ((int32_t)o->vy) << 8;
        o->z += ((int32_t)o->vz) << 8;
        o->vz = (int16_t)((uint16_t)o->vz - o->grav);   /* wraps, 0x22A6 */
        if (o->vz < 0 && (uint16_t)o->vz < 0x2000)
            o->vz = 0x2000;                     /* 0x22B8: provably dead,
                                                   kept verbatim */
        return;
    }
}

/* In-ring bounds, the 0x2818E law (scene 0 floor handler), probing the
 * hinted point (x + facing*lookahead, y + clip_h, z + floor42):
 *   Y: test the biased y against [0x118, 0x198]; the pushback moves the
 *      BODY so the probe sits on the line (window [0x118-h, 0x198-h]).
 *      Applies at ANY height.
 *   X: perspective trapezoid xmin = (y<<8 + 0x40000)/0x2E0 (guard
 *      px < 0x220), xmax = -((y<<8 - 0xA3000)/0x2E0) (guard px > 0x2E0),
 *      rows from the BIASED y. A crossing always sets the clip bit; the
 *      position pushback and zone=1 only when the biased z < 0x180 —
 *      high flight crosses the ropes (zone=2, no clamp).
 *   Z: landed when (z + floor42) < 0x140; the snap target is ALWAYS
 *      exactly 0x140 (0x281DE: push = 0x140 - z). vz is NOT zeroed —
 *      the next state re-arms it; one visible dip-and-resnap frame is
 *      part of the stock look.
 * SIMPLIFIED vs ROM: the ROM banks the push-back into +0x38/+0x3A/+0x3C
 * and applies it at the top of the NEXT frame's pass (0xF52E); this
 * clamps in place, a one-frame difference. (rope-fall.md §3) */
int eng_floor_scene;                   /* $1C007E low byte, set by core.c */

/* One law pass = the 0x28124 dispatch: scene 2/6 rows (ring-out camera
 * scene, ringout-scene.md §3), else the 0x28288 ringside law when the man
 * is OUTSIDE (f33 b2), else the 0x2818E in-ring trapezoid. It reports the
 * push-backs the ROM banks into +0x38/+0x3A/+0x3C plus zone/clip/landed,
 * so the frame pass (0x280DC via 0xF518) and the X-only slide 0x10B62
 * share ONE law — the slide used to hard-code the in-ring trapezoid, which
 * teleported a far-right ringside tie-up back to the rope line. Same
 * banked-pushback simplification as before (applied in place). */
typedef struct {
    int32_t dx, dy;        /* +0x38 / +0x3A */
    int32_t snapz;         /* -1, or the absolute z the floor snaps to (0x281DE: push = 0x140 - z) */
    uint8_t zone, clip, landed;
} law_out;

static void law_run(const eng_obj *o, int32_t px, int32_t py, int32_t pz, law_out *r)
{
    r->dx = 0; r->dy = 0; r->snapz = -1; r->zone = 0; r->clip = 0; r->landed = 0;
    if (eng_floor_scene == 1) {        /* 0x2831C scene-1 (cage) law: rows 1 of 0x28154,
                                          both inside AND outside slots point here /rts —
                                          nobody ever leaves this arena */
        if (py < 0x118) { r->dy = 0x118 - py; py = 0x118; r->clip |= 0x08u; r->zone = 1; }   /* 2832A */
        if (py > 0x198) { r->dy = 0x198 - py; py = 0x198; r->clip |= 0x04u; r->zone = 1; }   /* 28348 */
        if (pz < 0x140) { r->clip |= 0x10u; r->snapz = 0x140; r->landed = 1; }               /* 28366 (+0x42 bias in caller) */
        if (pz < 0x180) {                                              /* 28384 low: the ropes */
            int32_t xmin = ((py << 8) + 0x40000) / 0x2E0;              /* 28396 */
            int32_t xmax = -(((py << 8) - 0xA3000) / 0x2E0);           /* 283CC */
            if (px < 0x220 && px < xmin) { r->dx = xmin - px; r->clip |= 0x02u; r->zone = 5; }   /* 283AC */
            else if (px > 0x2E0 && px > xmax) { r->dx = xmax - px; r->clip |= 0x01u; r->zone = 5; }   /* 283E4 */
        } else {                                                       /* 283FA high: the cage wall,
                                                                          a shade outside the ropes */
            int32_t xmin = ((py << 8) + 0x38000) / 0x2E0;              /* 2840C */
            int32_t xmax = -(((py << 8) - 0xAC000) / 0x2E0);           /* 2844C */
            if (px < 0x220 && px < xmin) { r->dx = xmin - px; r->clip |= 0x02u; r->zone = 6; }   /* 28422 */
            else if (px > 0x2E0 && px > xmax) { r->dx = xmax - px; r->clip |= 0x01u; r->zone = 6; }   /* 28464 */
        }
        return;
    }
    if (eng_floor_scene == 2 || eng_floor_scene == 6) {   /* 0x28124 table rows 2/6 */
        if (o->role & RF_OUTSIDE) {                                             /* 0x2851E outside */
            if (py < 0x110) { r->dy = 0x110 - py; r->clip |= 0x08u; r->zone = 1; }         /* 28522: top of the walkway */
            else if (py > 0x140) {                                                          /* 28540: ring-skirt line */
                r->dy = 0x140 - py; r->clip |= 0x04u; r->zone = 1;
                if (px >= 0x294 && px < 0x38C) r->zone = 4;   /* 2855E: only on the pushed-up-from-below path (28544 bls skips it) */
            }
            if (pz < 0x100) { r->snapz = 0x100; r->landed = 1; }                            /* 28570: floor z 0x100 */
            if (px < 0x1C8) { r->dx = 0x1C8 - px; r->clip |= 0x02u; r->zone = 3; return; }   /* 2858E: barrier wall */
            if (px > 0x470) { r->dx = 0x470 - px; r->clip |= 0x01u; r->zone = 3; }          /* 285B2 */
            return;
        }
        /* 0x28480 inside */
        if (py != 0x160) { r->dy = 0x160 - py; r->zone = 1; r->clip |= 0x04u; }   /* 28484: y pinned to the mat line */
        if (pz < 0x140) { r->clip |= 0x02u; r->snapz = 0x140; r->landed = 1; } /* 284A2: clip b1 (sic), z snaps to 0x140 */
        if (px < 0x270) { r->dx = 0x270 - px; r->clip |= 0x02u; r->zone = 1; return; }   /* 284C0 */
        if (px > 0x3D0) { r->dx = 0x3D0 - px; r->clip |= 0x01u; r->zone = 1; return; }   /* 284E4 */
        if (px >= 0x2B0 && px < 0x390) r->zone = 4;                                        /* 28506: in front of the ring */
        return;
    }
    if (o->role & RF_OUTSIDE) {              /* OUTSIDE: 0x28288 ringside law */
        if (pz < 0x100) { r->snapz = 0x100; r->landed = 1; }
        if (px < 0x1C7) {
            int32_t xmin = ((py << 8) + 0x28000) / 0x2E0;
            if (px < xmin) { r->dx = xmin - px; r->clip |= 0x02; r->zone = 3; return; }
        }
        if (px > 0x360) {
            int32_t xmax = -(((py << 8) - 0xBA000) / 0x2E0);
            if (px > xmax) { r->dx = xmax - px; r->clip |= 0x01; r->zone = 3; }
        }
        return;
    }
    /* 0x2818E in-ring law */
    if (py < 0x118) { r->dy = 0x118 - py; py = 0x118; r->clip |= 0x08; r->zone = 1; }
    if (py > 0x198) { r->dy = 0x198 - py; py = 0x198; r->clip |= 0x04; r->zone = 1; }
    {
        int32_t xmin = ((py << 8) + 0x40000) / 0x2E0;
        int32_t xmax = -(((py << 8) - 0xA3000) / 0x2E0);
        if (px < 0x220 && px < xmin) {
            r->clip |= 0x02;
            if (pz < 0x180) { r->dx = xmin - px; r->zone = 1; }
            else            r->zone = 2;     /* high crossing: no pushback */
        } else if (px > 0x2E0 && px > xmax) {
            r->clip |= 0x01;
            if (pz < 0x180) { r->dx = xmax - px; r->zone = 1; }
            else            r->zone = 2;
        }
    }
    /* Floor: larger z = higher on screen (the rasteriser inverts y), so
     * the mat is a MINIMUM. The clip records contact like +0x37 bit4. */
    if (pz < 0x140) { r->snapz = 0x140; r->landed = 1; }   /* snap unbiased, 0x281DE */
}

void eng_ring_bounds(eng_obj *o)
{
    int32_t la = (o->facing & 0x8000u) ? -o->lookahead : o->lookahead;
    law_out r;
    if (o->apron) return;              /* 0x280EC +0x32 b5: apron men live on the
                                          apron line (tag.c) — the ring trapezoid
                                          must not clamp them back inside */
    if (o->throw_pend && !(o->role & RF_OUTSIDE)) {   /* throw_out mod (anim.c): thrown over the ropes -
                                          the ring law holds him until he crosses the rope line */
        int32_t px = o->x >> 16, py = (o->y >> 16) + o->clip_h;
        int32_t xmin = ((py << 8) + 0x40000) / 0x2E0, xmax = -(((py << 8) - 0xA3000) / 0x2E0);
        if (px < xmin - 4 || px > xmax + 4) o->role |= RF_OUTSIDE;   /* outside from here; the flag stays until the landing (anim.c) */
    }
    law_run(o, (o->x >> 16) + la, (o->y >> 16) + o->clip_h, (o->z >> 16) + o->floor42, &r);
    if (o->throw_pend == 1) r.dx = 0;   /* the ropes never push a thrown man back (anim.c handler_fall clears it at the landing) */
    o->x += r.dx << 16;
    o->y += r.dy << 16;
    if (r.snapz >= 0) o->z = r.snapz << 16;

    o->zone = r.zone;
    o->clip = r.clip;
    o->landed = r.landed;
}

/* 0x10B62 — clipped facing-relative X slide: +0x3E = D0, one full law
 * pass 0x280DC on the point d0 ahead of facing (the law facing-mirrors
 * +0x3E), vx killed when the pass's zone & 3 is set (0x10B74-0x10B80:
 * any low/high X crossing AND a Y crossing, which also write zone 1),
 * then ONLY the X push-back +0x38 is applied and +0x36 is restored — the
 * frame pass's zone/clip are left as they were. (rope-fall.md §2) */
void eng_slide_clip(eng_obj *o, int16_t d0)
{
    int32_t px = (o->x >> 16) + ((o->facing & 0x8000u) ? -d0 : d0);
    int32_t py = (o->y >> 16) + o->clip_h;
    int32_t pz = (o->z >> 16) + o->floor42;
    law_out r;
    if (o->apron) return;              /* 0x280EC skips the law for +0x32 b5 */
    law_run(o, px, py, pz, &r);
    if (r.zone & 3u) o->vx = 0;        /* 0x10B80: keyed on the new zone */
    o->x += r.dx << 16;                /* 0x10B8C: X only, Y/Z untouched */
}

/* ROM 0x247C: world -> screen. +0x14 = x - camX (+ facing-mirrored sprite
 * offset, not yet carried); +0x16 = y + z - camY. No clamp, no wrap;
 * the y inversion happens in the rasteriser (256 - y). */
void eng_screen_pos(eng_obj *o, const eng_state *st)
{
    int16_t hx = (int8_t)(o->off_x & 0xFFu);   /* byte +0x19 */
    int16_t hy = (int8_t)(o->off_y & 0xFFu);   /* byte +0x1B */

    if (o->facing & 0x8000u)                   /* facing mirrors the offset */
        hx = (int16_t)-hx;
    o->sx = (int16_t)((o->x >> 16) - st->cam_x + hx);
    o->sy = (int16_t)((o->y >> 16) + (o->z >> 16) - st->cam_y + hy);
}
