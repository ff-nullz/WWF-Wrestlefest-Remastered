/* Native WrestleFest engine — from-scratch rebuild, no 68k anywhere.
 *
 * Reuses the decomp's verified rendering as a library: video.c rasterises
 * the arcade layer model from the `wf` state struct, scene_map.c composes
 * the scene tilemaps (C form of ROM 0x26E66), pal_load.c loads palettes
 * (C form of 0x2A06). The engine OWNS the `wf` instance those files render
 * from — there is no bus, no Musashi, no ROM code running.
 *
 * Design contract (docs/native-goals.md):
 *   - physics/mechanics transcribed from the ROM, same fixed-point math,
 *     fixed 57.44 Hz step (7 MHz pixel clock / 448 / 272);
 *   - every gameplay constant from data/romdata/ (see docs/rom-tables.md)
 *     or from eng_const.h with the ROM PC cited;
 *   - eng_update() is deterministic: same inputs -> same state, no SDL,
 *     no wall clock, no floats in gameplay.
 */
#ifndef ENG_ENGINE_H
#define ENG_ENGINE_H

#include <stdint.h>

#define ENG_REFRESH_HZ   (7000000.0 / 448.0 / 272.0)   /* 57.4449... */
#define ENG_FRAME_NS     ((int64_t)(1e9 * 448.0 * 272.0 / 7000000.0))

/* One in-world object. Fields mirror the ROM object layout
 * (docs/memory-catalog.csv) so oracle comparisons stay per-field. */
typedef struct { uint16_t on, t, b2, b3, base, pose; } eng_chip;   /* 0x8710 overlay */

typedef struct {
    int active;            /* +0x00 bit7 */
    int wrestler;          /* +0x02: directory id; stock 0..11 */
    uint8_t mover;         /* +0x01: 0 none, 1 polar, 2 velocity */
    uint16_t spr;          /* +0x04: sprite word, bit15 hflip, 0xFFFF hidden */
    int32_t x, y, z;       /* +0x06/+0x0A/+0x0E: 16.16 world position */
    int16_t sx, sy;        /* +0x14/+0x16: screen pos from 0x247C */
    uint16_t anim_sel;     /* +0x1C: bit15 init latch, low byte source id */
    uint16_t prev_sel;     /* +0x1E */
    uint16_t state;        /* +0x20: bit15 entered latch, low byte state id */
    uint16_t count;        /* +0x22: anim countdown, 0xFF00 hold */
    uint16_t frame;        /* +0x24: anim frame, 0xFE finished */
    uint16_t speed;        /* +0x2A: polar speed (low byte) */
    uint16_t angle;        /* +0x2C: polar angle (low byte), 0 = +y away */
    uint16_t facing;       /* +0x2E: 0x0000 face-left / 0x8000 face-right */
    uint16_t st_flags;          /* +0x32 flags (bit3 run, bit7 freeze) */
    int16_t vx, vy, vz;    /* +0x58/+0x5A/+0x5C: 8.8 px/frame */
    uint16_t grav;         /* +0x5E */
    uint8_t joy;           /* +0xA9: held joystick nibble (R,L,U,D = bits 0..3) */
    uint8_t joy_prev;      /* last frame's nibble (for +0xA9-style edges) */
    uint8_t joy_new;       /* new-press direction bits this frame */
    uint16_t btn_acc;      /* +0xA4: odd-frame OR accumulator (0x514E) */
    uint16_t btn_held;     /* +0xA6: button level */
    uint16_t btn_new;      /* +0xA2: new-press buttons */
    int opp;               /* +0x7A: current opponent (facing target), obj index */
    int partner;           /* +0x26: engaged grapple partner, -1 free */
    uint16_t grap44;       /* +0x44: the per-STATE context word - one ROM word whose value is
                              meant to survive state changes (a whipped runner's 1 rides into
                              the hold drag), so it is documented, not split (rename pass
                              2026-09-05). Roles: climb/perch = corner index (0-3 in-ring,
                              8/9 the ringside posts; & 0xF); run = whip counter (1 = whipped
                              runner, 0x1EDC4 rope stop); tie-up/hold = b15 hidden-half,
                              b14 lost-tick, b13 impact, 0x80/0xAA/0xBB the hold clock;
                              throws = phase / entry (0x48/0x50, 1|8 the mod climb-out);
                              84 sites clear it on a state change. */
    uint16_t exch;         /* +0x48: tie-up exchange counter, mod 4 */
    uint8_t  lock_n;       /* engine: consecutive CPU-vs-CPU tie-up lockups (tieup_rules cpu_lockup_max) */
    uint16_t hold_t;       /* +0x46: hold timer */
    uint16_t whiff_t;      /* engine-only: frames an UNROUTED record has sat
                              on a handler-driven hold (soft-lock guard) */
    uint8_t hold_ph;       /* +0x45 */
    uint8_t mash;          /* +0xBD: mash countdown */
    uint16_t atk;          /* +0x4C: attack/stance record word (low byte = id) */
    uint16_t move_id;      /* +0x60: move id for state 5 */
    uint16_t react_id;     /* +0x64: reaction id for state 4 */
    uint16_t hp, hp_max;   /* +0x66 / +0x72 */
    uint16_t tries[11];    /* +0xE8..+0xFC: per-throw attempt counters this match
                              (0x1129E entry 1 index; cleared 0x8CBE-0x8CE6):
                              E8 0x28, EA 0x30, EC 0x2B, EE 0x43, F0 0x2F,
                              F2 0x44, F4 0x19, F6 0x33, F8 0x26, FA 0x34, FC 0x1A */
    int16_t hp_delta;      /* +0x6A: pending gauge delta (HUD 0x7548) */
    int16_t hp_acc;        /* +0x6C: gauge accumulator */
    int16_t hp_disp;       /* +0x6E: displayed hp */
    uint16_t dmg;          /* +0x68: pending damage */
    uint16_t hit_mask;     /* +0x8E: per-victim already-hit bits */
    uint16_t spr_cache;    /* +0x90: anim word cache for 0x24090 */
    uint16_t alt62;        /* +0x62: attack alternation latch (bchg bit7) */
    uint16_t band;         /* +0x70: energy band from 0x24EC2 */
    uint16_t role;          /* +0x33 (b0 legal, b6 engaged, b7 side) */
    uint16_t cue_flags;          /* +0x35 (b0 = legal pin in progress: referee cue) */
    uint16_t result;       /* +0xFE match-result word ($4000/$4001/... b7=final) */
    uint16_t a0flags;      /* +0xA0 byte: 0x40 = this man's side won, 0x80 = lost.
                              Written by the win pose 0x1AD50 and the lose pose
                              0x1ADAE/0x1ADDE, and by time-up 0x26410; read by the
                              frame loop 0x1178 and the banners 0x11FA/0x1240. */
    uint16_t mash_aa;      /* +0xAA pin/hold mash counter; 0x4000 = mashed out */
    uint8_t  foe1, foe_lp1, foe_pn1;   /* engine (parasitic mod): the LAST man paired with me, +1 (0 = none) -
                                          the newest change of +0x92 (last_pair) or the partner wins; a throw's
                                          damage drains after both pairings are gone (hit.c eng_damage_drain) */
    uint8_t  throw_pend;   /* engine: throw_out mod - airborne over the ring, the ring law holds him until
                              he crosses the rope line, then he is OUTSIDE (motion.c) */
    uint8_t  backup;       /* engine: a mod backup run-in (modrules.c) - 1 = brawling, 2 = LEAVING (an AI goal:
                              when free he climbs out / walks to the aisle and up it, then the slot frees); the
                              ring-out switch keeps him a ring brawler instead of an apron partner */
    uint16_t hitctr_d2[7]; /* +0xD2..+0xDE: downed-attack hits taken, per move {0x10,0x0B,0x13,0x14,0x0E,0x35,0x23}
                              (0x14338/0x13AEA/0x14AAC/0x14C58/0x13E50/0x1786A/0x15F06): the CPU's escape roll
                              0x1DD2A indexes its table by min(ctr,5), the leg drop's mash need too (0x17624) */
    uint8_t  hud_c2, hud_c3;   /* +0xC2/+0xC3: last banner id drawn in the HUD block (0xC710) */
    uint8_t  flash_ce, flash_cf; /* +0xCE b7 POWER-UP flash live / +0xCF its frame (0x88A2) */
    uint8_t  cmb_c6, hits_c7;    /* +0xC6 b6 = REGAIN POWER armed (0xFD64); +0xC7 = big hits taken (the 28 move handlers +1/+5) */
    uint8_t  ai_run_own;         /* engine: the current run was launched by the AI (a human-started run is stopped on hand-over) */
    uint16_t cmb_c8;             /* +0xC8: apron frames (humans, 0x11834/0x1197A) / the ON-FIRE countdown (0xFD08) */
    uint16_t flash_cc;     /* +0xCC flash frame divider */
    uint16_t halfct;       /* +0x109 per-victim half-count tally */
    uint8_t pinning;       /* state-0x0C context: 1 = lying pin (from a
                              cover), 0 = standing hold (from the tie-up) */
    uint16_t combo;        /* +0x52: combo hit counter */
    uint16_t combo_t;      /* +0x54: combo decay timer (even frames) */
    uint16_t down_t;       /* +0x9A: forced-down frames */
    int16_t floor42;       /* +0x42: flight floor bias for the z clip */
    int16_t lookahead;     /* +0x3E: facing-relative X probe look-ahead */
    int16_t clip_h;        /* +0x40 body-size hint: extra Y margin the
                              bounds keep from the rope lines (downed
                              states; lying uses 0x18) */
    uint8_t landed;        /* +0x37 bit4 analog: floor contact this frame */
    uint16_t off_x, off_y; /* +0x18/+0x1A: sprite offsets, signed low byte,
                              off_x bit15 = clear both after the anim pass */
    int last_pair;         /* +0x92: last hit pairing (catch bookkeeping) */
    int16_t run_tgt;       /* +0xBE: rope-run return target x */
    uint16_t slowwalk_t;   /* +0xD0: tired (f32 b11) walk timer, 0xD0 */
    uint8_t cpu;           /* +0x56 b7: CPU-driven (engine/ai.c) */
    uint16_t spr_force;    /* handler sprite override applied after the tick;
                              ENG_SPR_STANCE = sprite 0 (the stance) in the
                              current facing, which a plain value cannot
                              express when the man faces left (0|0 = none) */
#define ENG_SPR_STANCE 0x7FFFu
    uint8_t cam_mode;      /* +0x4B: 0 group, 1 solo, 2 FROZEN, 3 ignored, 4 scripted */
    uint16_t driver;          /* +0x56: b7 CPU, b6 autopilot (apron partner) */
    int input;             /* player index feeding this object, -1 = none */
    int teammate;          /* +0x86: team partner object index */
    int rescue;            /* +0x7E: rescue / double-cover link (rumble helper: the pinned man) */
    uint8_t divorce;       /* +0x26 b7: drop the partner link at the END of this frame (0x2930) — set by
                              move ends (bset #7,(+0x26) in ~120 handlers) and by the strike results */
    uint8_t apron;         /* on the apron (outside, z 0x140, no ring probe) */
    uint8_t sub;           /* +0xAE state-1 sub: 1 follow, 2 post (|0x80 ready), 4 walk-out */
    uint16_t regen_t;      /* +0x44 as the apron regen clock (0x94) */
    uint8_t worry_t;       /* +0x46 while +0x34 b5 (0x11870): the apron man's
                              "holds his head" pose 0x1D3 clock (0x20 frames,
                              armed by the slam impacts' 0x110E0) */
    uint8_t worry_seen;    /* engine edge memory: teammate's last (state,react)
                              token for the 0x110E0 stand-in detector */
    uint8_t hold4_t;       /* 0xEF6A: frames a climb direction has been held */
    uint8_t list;          /* +0x12 sprite list: 0 front, 2 rope-lean, 4 back */
    uint16_t ai_t;         /* AI scratch timer (+0xB6/+0xBD family) */
    uint8_t ai_idle;       /* +0xB9 b1: policy said idle */
    /* ai.c — the ROM's AI scratch block (ai-core.md §0), kept per bit so
     * the transcription reads like the asm */
    uint8_t ai_b5, ai_b6, ai_b7;   /* +0xB5 / +0xB6 / +0xB7 flag bytes */
    uint8_t ai_pinroll;    /* engine: 0x1108C pin-intent rolled for this knockdown (the ROM jsr's it from the knockdown enders; +0xB6 b6 is the rumble no-target flag) */
    uint8_t  ai_dec_t;     /* mod ai_commit: frames the current policy decision stays committed (0 = free to re-roll) */
    uint8_t  ai_dec_mv;    /* ... the committed move id (what the policy answered) */
    uint16_t ai_dec_key;   /* ... the situation it was taken in: policy table | ctx | opponent state class | range bucket */
    uint8_t ai_hdir, ai_hage;  /* engine smoothing (user 2026-08-23): last horizontal
                                  steer + frames held — a flip within 6 frames is
                                  suppressed so the approach doesn't jitter L/R */
    uint8_t badge_t;       /* RETIRED (user 2026-08-28: the nP pad badge corrupted the
                              HUD rows and was removed) — field kept so the struct layout
                              and digest stand */
    uint8_t ai_hcool, ai_vdir, ai_vage, ai_vcool;   /* engine smoothing (user 2026-08-27
                                  "cpu players jitter"): vertical commit + a 3-frame
                                  restart cooldown after either axis releases (the
                                  1-frame step/stop stutter read as vibration) */
    uint8_t ai_bd;         /* +0xBD: perch wait / lockup seed (own copy) */
    uint16_t ai_bc;        /* +0xBC: distance gate / corner index / countdown */
    uint16_t ai_ba;        /* +0xBA: CPU held-victim escape timer (0x1E3F2) */
    int16_t ai_tx, ai_ty;  /* +0xBE/+0xC0: walk-to target */
    uint8_t ai_sub;        /* +0xAE AI sub: 0 approach, 1 run-in, 9 corner,
                              0xA walk-to, 0x3C/0x3D/0x3E pseudo-moves */
    uint8_t ai_mv;         /* +0x61: the move the AI has decided on */
    uint8_t ai_press_t;    /* virtual-controller button latch (frames left) */
    uint32_t ai_press;     /* ... and the bits held */
    uint16_t ai_e6;        /* +0xE6: rescue run-in countdown (tag-mode.md §4) */
    uint16_t ai_runin_t;   /* engine: frames until the usher recall ($1C1682 stand-in) */
    uint16_t ai_sub_t;     /* engine: frames spent in the current AI sub (bail guard) */
    uint16_t tag_flags;          /* +0x34: b6 "I was whipped", b7 "I whipped him" */
    uint8_t zone;          /* +0x36: 1 rope contact, 2 x-edge while high */
    uint8_t clip;          /* +0x37: b0 xmax, b1 xmin, b2 ymax, b3 ymin, b4 z */
    uint8_t intro_1d;      /* +0x1D during 0xA654: intro pose cycle running */
    uint16_t intro_22, intro_24;   /* 0xACB6 cell tick / cell index */
    int16_t tgt_y;         /* +0xC0: walk target y (pairs with run_tgt +0xBE) */
    uint16_t weapon_w;          /* +0x74/+0x75: b15 (byte +0x74 b7) = holding a
                              ringside weapon, low byte (+0x75) = its type
                              (0x19EB0 copies the weapon's +0x74 word, then
                              bset #7,(+0x74)) */
    int wobj;              /* +0x76 as the weapon link (0xF168/0x1C784):
                              0 none, 1+k = weapon slot k */
} eng_obj;

/* ---- names for the ROM-shaped words (rename pass, 2026-09-05) ----------
 * The object words above keep their ROM offsets in the comments; the code
 * reads them through these names. Bits whose meaning is not yet pinned
 * down keep a numbered name (SF_LAW_EXEMPT ...) with the ROM site that uses them:
 * rename when you learn what they are. */

/* st_flags (+0x32): the man's STATUS */
#define SF_APRON       0x0001u   /* on the apron / apron man (0x2105C "holder outside", 0x1B416 back to walk) */
#define SF_TOPROPE     0x0002u   /* up on the buckle (0x1208A bset #1; the AI leaves him alone, 0x17AA0) */
#define SF_RUNIN_MARK  0x0004u   /* forced tag run-in mark (0x214A4), spent by ai.c */
#define SF_SLAMMED     0x0008u   /* ROM +0x32 b3: the big slams' victim tails set it (0x1736A / 0x167B0
                                    bset #3 next to the 0x2000 react flag); the engine never reads it */
#define SF_ELIMINATED  0x0010u   /* rumble: eliminated (0x20C10 / 0x201F6) */
#define SF_LAW_EXEMPT  0x0020u   /* ROM +0x32 b5: on the apron line, the ring law skips him (0x280EC);
                                    the engine models that with o->apron and only mirrors the bit
                                    (ringout.c FABA placement sets, the climb-in end 0x19C1A clears) */
#define SF_FROZEN      0x0080u   /* held / frozen out of the frame pass (0xF4CE, 0x8C0C, 0x20C28) */
#define SF_TIRED       0x0800u   /* tired: stomach-hold walk, 0x116BA cell swap, 0x24EA0 clock (engine bit) */
#define SF_QUEUED      0x8000u   /* queued rumble entrant, not in the arena (0x1C17E) */
#define SF_OUT_OF_PLAY (SF_ELIMINATED | SF_QUEUED)

/* role (+0x33): who he is in the MATCH */
#define RF_LEGAL       0x0001u   /* the legal man of his team */
#define RF_PAD         0x0002u   /* driven by a human pad (attract clears it; autopilot partners lack it) */
#define RF_OUTSIDE     0x0004u   /* outside the ring: the 0x28288 ringside law */
#define RF_BIT3        0x0008u   /* ROM +0x33 b3: the engine only ever CLEARS it (the hop / climb-out landings);
                                    whatever sets it lives in untranscribed ROM code - meaning unconfirmed */
#define RF_RUNNING     0x0010u   /* running: +1 damage on his hits (0x24DDC), reaction 4 not 3 */
#define RF_ONFIRE      0x0020u   /* ON FIRE (0x88B2): CPU walks +6, holds pre-set */
#define RF_ENGAGED     0x0040u   /* engaged with an opponent (grab / hold / pair) */
#define RF_SIDE        0x0080u   /* team side: 0 = left team, 1 = right team */

/* tag_flags (+0x34): tag / whip bookkeeping */
#define TF_USHER_A     0x0001u   /* usher bit 0: 0x21688 bset #0 (with b1 cleared) once the usher has
                                    sent him; the apron rescue gate (ai.c) - exact meaning unconfirmed */
#define TF_RECALL      0x0002u   /* usher bit 1: recall pending (0xF3C6 / tag.c) */
#define TF_USHER_C     0x0004u   /* usher bit 2: travels with b0/b1 (0x21978 hands b0-2 to the holder) */
#define TF_USHER_BITS  (TF_USHER_A | TF_RECALL | TF_USHER_C)   /* 0x21978 andi.b #7 */
#define TF_BIT3        0x0008u   /* ROM +0x34 b3: the top-rope dive landing clears it (0x154B6 bclr #3);
                                    meaning unconfirmed */
#define TF_PIN_INTENT  0x0010u   /* CPU pin intent (0x20C18 / 0xEBFA) */
#define TF_WORRY       0x0020u   /* apron man "holds his head" (0x11870 / 0xE2E0) */
#define TF_WHIPPED     0x0040u   /* "I was whipped" (0x1153E) */
#define TF_WHIPPER     0x0080u   /* "I whipped him" */

/* cue_flags (+0x35): referee cues and HUD chips */
#define CF_PIN_CUE     0x0001u   /* legal pin in progress: the referee's cue (0x24C0C) */
#define CF_SUB_HOLDER  0x0002u   /* submission hold cue on the holder (0x20D44) */
#define CF_SUB_VICTIM  0x0004u   /* ... and on the held man */
#define CF_CHIP_CHANGE 0x0008u   /* CHANGE OVER chip (0x8732) */
#define CF_CHIP_P      0x0010u   /* nP chip (0x8778) */
#define CF_CHIP_HIDE   0x0020u   /* hide the chip (0x8766) */

/* driver (+0x56): who drives the man */
#define DRV_AUTOPILOT  0x0040u   /* human's partner on autopilot (apron) */
#define DRV_CPU        0x0080u   /* CPU-driven */

/* weapon_w (+0x74/+0x75): b15 holding a ringside weapon, low byte its type */
#define WPN_HELD       0x8000u

/* state (+0x20 low byte; bit15 set = the cell is latched) */
enum { ST_STAND = 0, ST_WALK = 1, ST_RUN = 2, ST_SKID = 3, ST_REACT = 4, ST_MOVE = 5,
       ST_TURN = 6, ST_GETUP = 7, ST_CLIMB = 8, ST_PERCH = 9, ST_CLIMBDOWN = 0x0A,
       ST_LOCKUP = 0x0B, ST_HOLD = 0x0C, ST_HELD = 0xFF };
/* react_id (+0x64) low byte while state == ST_REACT (bit15 = face-down) */
enum { RC_FLINCH = 0, RC_DIZZY = 1, RC_FALL_HIGH = 2, RC_FALL = 3, RC_FALL_RUN = 4,
       RC_BOUNCE = 5, RC_LYING = 8, RC_LYING_B = 9 /* the second lying cell 0x1B30C */, RC_GRABBED = 0x0E };
#define DRV_ANY_CPU    (DRV_CPU | DRV_AUTOPILOT)
#define TF_WHIP_MARKS  (TF_WHIPPED | TF_WHIPPER)
#define CF_HOLD_CUES   (CF_PIN_CUE | CF_SUB_HOLDER)   /* a scripted hold / cover in progress */

#define ENG_MAX_OBJS 12   /* the ROM has 9 object slots; 12 = the 12-man battle royale (user 2026-09-05): every stock wrestler in the ring at the bell */

/* The referee — object $1C11F4, machine 0x1F914 (docs/engine-specs/
 * pins-referee.md). Sprite stream row 12. */
typedef struct {
    int active;
    int32_t x, y, z;       /* 16.16 like the wrestlers */
    uint16_t spr;
    int16_t sx, sy;
    int sm;                /* +0x21 subset: 5 idle, 1 approach, 6 count, 9 win */
    int cell;              /* +0x25 half-count */
    int t23;               /* +0x23 count timer */
    int pose_flip;
    int target;            /* +0x56: pinner slot */
    unsigned ym;           /* +0x54 ONE/TWO/THREE cmd */
    int win_t;
    uint16_t cue_flags;          /* +0x35: b1 left-rope / b2 top-rope memory,
                              b3 action x < 0x270, b4 action y < 0x160 */
    uint16_t facing;       /* +0x2E: b15 face left */
    uint8_t angle, speed;  /* +0x2D heading, +0x2B speed (4.4) */
    int pose, p23, p25;    /* +0x05 walk pose, +0x23 tick, +0x25 pose ctr */
    int vis;               /* +0x1D visual index while sm == 5: 5 = the $8005
                              walk-back-to-the-ropes (0x1FF52), 0 = the $8000
                              idle (0x1FB46 / rumble 0x1FC06) */
    int vis_init;          /* visual first-entry latch (+0x1C b7) */
    int t22, t24;          /* +0x22 pose timer word / +0x24 count-out tick (SM3, 0x1FD5E) */
} eng_ref;

/* Ring announcer object $1C14CE during the 0xA654 intro (walkin.c). */
typedef struct {
    int active;
    int32_t x, y, z;
    uint16_t spr;          /* +0x04 cell (b15 flip) */
    int16_t sx, sy;
    int entered;           /* +0x1C b7 */
    int step;              /* +0x1D */
    int t22, t24, t1E;     /* +0x22 / +0x24 / +0x1E counters */
    int mouth;             /* +0x25 */
    uint8_t angle, speed;  /* +0x2D / +0x2B for the 0xA8EC walk-out */
    int log_step;          /* WF_INTROLOG debug latch */
    uint8_t skip_arm;      /* buttons seen released since the intro began:
                              the game-select confirm press must not skip */
} eng_ann;

/* Companion-sprite slots $1C1258 (11 x 0x2A): 0x10D3A(D0) fills the first
 * free one from table 0x10DDA[D0] = {dy, pose, list} — pose drawn from the
 * OWNER's row at (x + hotspot x, y + dy, z - dy) — and enqueues it for this
 * frame; 0x10E6A (frame list 0x1006/0x10AA) frees them all every frame, so
 * a handler re-spawns per tick (leg drop frame 4: 0xE9 beside 0xE8). */
#define ENG_FX_SLOTS 11
typedef struct {
    uint8_t active;        /* +0x00 b7 */
    uint16_t row;          /* +0x02: owner's stream row */
    uint16_t spr;          /* +0x04: pose | owner facing */
    uint16_t list;         /* +0x12: draw list */
    int32_t x, y, z;       /* +0x06/+0x0A/+0x0E */
    int16_t sx, sy;        /* +0x14/+0x16 from 0x247C (no hotspot of its own) */
} eng_fx;

/* Ringside weapons — the object slots $1C0F1C / $1C1028 (stride 0x10C
 * slots 9/10 of the work-RAM object array; sprite sheet byte +0x03 =
 * 0x0F). Spawned at match init (0xCF4 -> 0xFFD2), machine 0xFDEE runs
 * per frame while the ringside scene shows ($1C0161 b1). +0x1C states:
 * 0 hidden, 1 carried (tracks the holder, sprite hidden), 2 tossed
 * (launcher class 0x10 flight), 3 resting on the floor, 4 tumbling. */
#define ENG_WEAPONS 10     /* 10 SLOTS (user 2026-08-28): 0/1 = the ROM's ringside
                              spots; a profile's weapons.json "slots" list places up
                              to 10 (outside AND inside the ring; "" spawn = empty) */
typedef struct {
    uint8_t active;        /* +0x00 b7 */
    uint8_t inside;        /* ENGINE: slot is INSIDE the ring - live in the normal
                              scene (tick/draw/pickup), floor = the mat (z 0x140) */
    uint8_t type;          /* +0x74: 0 = $1C0F1C, 1 = $1C1028; pose base type*3 */
    uint16_t state;        /* +0x1C: b15 init latch, low byte 0..4 */
    uint16_t spr;          /* +0x04: pose | facing b15, 0xFFFF hidden */
    uint16_t facing;       /* +0x2E as the engine facing word */
    int32_t x, y, z;       /* +0x06/+0x0A/+0x0E, 16.16 */
    int16_t sx, sy;        /* +0x14/+0x16 from 0x247C */
    int holder;            /* +0x76: 0 none, 1+i = obj i holds/reserved it */
    uint8_t list;          /* +0x12: 0 flying (depth sort), 2 on the floor */
    int16_t vx, vz;        /* +0x58/+0x5C style 8.8 flight (0x258E class 0x10) */
    uint16_t grav;         /* +0x5E */
    uint16_t t22, t24;     /* +0x22 tumble tick / +0x24 tumble count (0xFF72) */
} eng_weapon;

typedef struct {
    int64_t frame;
    uint32_t inputs[4];    /* per player: bit0 R, 1 L, 2 U, 3 D, 4 b1, 5 b2, 6 start */
    int scene;             /* $1C007E scene word: 0 ring, 1 cage, ... */
    int32_t cam_x, cam_y;  /* scroll origin */
    eng_obj obj[ENG_MAX_OBJS];
    eng_ref ref;
    /* match clock + HUD globals (hud-rules.md §2): BCD min/sec, frame
     * divider $1C16A4, signal bits $1C169E (b4 time-up, b5 <=5:00, b6
     * inited, b7 enabled), lock $1C16C5 */
    uint8_t clk_min, clk_sec, clk_div, sig169e, lock16c5, hud_inited;
    int over_t;            /* frames since the result went final */
    uint16_t corner_bits;  /* $1C1670: occupied turnbuckles (b0..3) */
    /* ring announcer (announce.c): request latch $1C15D2/3, current
     * name/phrase $1C15D4/5, step $1C15D6, countdown $1C15D8 */
    uint8_t ann_req_name, ann_req_phrase, ann_name, ann_phrase, ann_active;
    uint16_t ann_step, ann_t;
    /* ring intro 0xA654 (walkin.c): owns the frame while `intro` != 0 */
    int intro;
    int team_name;         /* $1C0168: one name covered both partners */
    eng_ann ann;
    eng_fx fx[ENG_FX_SLOTS];   /* $1C1258 companion sprites, one frame long */
    /* ring-out camera scene (ringout.c, docs/engine-specs/ringout-scene.md):
     * $1C1678 trigger (b15 fire, b14 return), $1C1679 faller facing
     * (engine facing word), $1C0161 (b1 = ringside scene showing),
     * $1C169A count-out counter, $1C0162 stage */
    uint16_t ringout_trig;
    uint16_t ringout_face;
    uint8_t g161;
    /* Royal Rumble (rumble.c, docs/engine-specs/rumble.md): picked ids
     * $1C16A8/$1C16B4, eliminations pending an entrant $1C16A7, entrant
     * timer $1C1694, controller bits $1C16C5 */
    uint8_t rumble_picked[12];
    uint8_t rumble_pending;
    uint16_t rumble_t;
    uint8_t rumble_phase;
    /* 0x899C get-up mash overlay: $1C167A b7 (a human body is down this frame,
     * 0x10D04), last frame's copy $1C167B, the 2-frame toggle $1C00AA/$1C00B2 */
    uint8_t body_down, body_down_prev, mash_t, mash_f;
    uint8_t hud_slot;      /* $1C16CE: the HUD block slot 0xC710 serves this frame */
    uint16_t count_out;
    uint16_t usher_t;      /* $1C1682: referee usher grace. 0xFA when a
                              rescue is armed with the partner already
                              inside (0x215FA/0x2176E), 0x177 the frame a
                              run-in fires (0x1D582) — the "allowed in
                              ring during a pin" time (tag_rules.json) */
    eng_chip chip[4];      /* $1C14CE+n*0x2A: the 1P-4P / CHANGE OVER
                              overlay objects (chips.c, 0x8710) */
    uint16_t stage;        /* $1C0162: campaign stage = AI difficulty index
                              (0..9; byte $1C0163 is bumped by the ladder
                              0x1A76/0x1AA4). Default 0; WF_STAGE poke. */
    eng_weapon wpn[ENG_WEAPONS];   /* ringside weapons $1C0F1C/$1C1028 */
    /* mid-game buy-in (hud.c eng_join_tick, ROM 0x18C4 from the frame
     * list 0x103E) and the rumble continue words (rumble.c): */
    uint8_t port_prev;     /* last frame's START level per port (edge detect;
                              stock keeps per-port edges at $1C15DA/$1C15DC) */
    uint8_t joinq;         /* bit p: rumble join queued for port p ($1C1586
                              seat word 0x8000, 0x194A) */
    uint16_t cont[4];      /* $1C0156[0..3]: per-seat rumble continue word —
                              0x3001 armed at the elimination (0x2021C), hi
                              byte counts down on odd frames (0xBD28); any
                              nonzero defers the b1 game-over (0x2019A) */
} eng_state;

/* weapon.c — ringside weapons (0xFFD2 spawn, 0xFDEE machine) */
void eng_weapons_spawn(eng_state *st);              /* 0xFFD2 (match init 0xCF4) */
void eng_weapons_tick(eng_state *st);               /* 0xFDEE (frame list 0xFFA/0x109E) */
void eng_weapons_scene_enter(eng_state *st);        /* 0xFA14/0xFA26 */
void eng_weapon_drop(eng_state *st, eng_obj *o);    /* the 0x24E0E shape: state 2 in place */
int  eng_weapon_pickup_try(eng_state *st, eng_obj *o);   /* 0xF0BA press consumer */

/* ringout.c — 0xF98C scene switch + ringside helpers */
void eng_ringout_switch(eng_state *st);
int  eng_legal_pair(eng_state *st, eng_obj **a, eng_obj **b);   /* 0x204FA */
void eng_ringside_ai(eng_state *st, eng_obj *o);                /* 0x1C6DC / 0x1D74A */
void eng_ref_digit_wipe(void);                                  /* 0x206FE */
void eng_count_digit(unsigned d0);   /* 0x2067C: count/countdown digit blit
                                        (bit15 = the $C1188 continue window) */
extern int eng_floor_scene;    /* $1C007E low byte for the floor dispatch 0x28124 */

/* Ring-out / count-out timing constants — data/romdata/ringout_rules.json
 * (flat rows in THIS order; the ROM value is the fallback when the table
 * is missing or short). See docs/engine-specs/ringout-scene.md §2. */
enum {
    RO_FRAMES_PER_COUNT,   /* 0x1FDF8  cmpi.w #$50,(+0x24)   ticks per count */
    RO_WARN_COUNT,         /* 0x1FE38  cmpi.b #$11,D0        first YM warning */
    RO_RESOLVE_COUNT,      /* 0x1FE0C/0x1FE50 cmpi.b #$14,D0 the 20-count */
    RO_REF_X_MIN,          /* 0x1FD92  cmpi.w #$298          referee x clamp lo */
    RO_REF_X_MAX,          /* 0x1FD9E  cmpi.w #$390          referee x clamp hi */
    RO_REF_ENTRY_X,        /* 0xFB94   move.w #$340          SM3 teleport x */
    RO_REF_ENTRY_Y,        /* 0xFB9C   move.w #$160          SM3 teleport y */
    RO_POSE_FIRST,         /* 0x1FD80  move.b #$9,(+0x05)    count-arm pose lo */
    RO_POSE_LAST,          /* 0x1FDB6  cmpi.b #$C,(+0x05)    count-arm pose hi */
    RO_POSE_FRAMES,        /* 0x1FD7A/0x1FDB4 move.b #$8,(+0x23) */
    RO_POSE_LAST_EXTRA,    /* 0x1FDD8  addi.b #$4,(+0x23)    extra on the last pose */
    RO_WARN_YM,            /* 0x1FD84  move.w #$3165,(+0x54) first warning id */
    RO_FREEZE_T22,         /* 0x1FE5A  move.w #$1000,(+0x22) pose timer frozen */
    RO_FALLER_DAMAGE,      /* 0xFAC0   move.w #$8,(+0x68)    damage for the fall */
    RO_DIGIT_WIN_TENS,     /* 0x2069E  $C0744 (offset in FG0 RAM) */
    RO_DIGIT_WIN_ONES,     /* 0x20692  $C0748 */
    RO_RESULT_OUT,         /* 0x1FEC2/0x1FF12 $8003 — counted-out pair (loser) */
    RO_RESULT_IN,          /* 0x1FEB2/0x1FF02 $8002 — the pair still inside */
    RO_RESULT_DOUBLE,      /* 0x1FE76  $8005 — both out, double count-out */
    RO_ILLEGAL_THROW_OUT,  /* engine (no ROM site): 1 = a NON-legal run-in man's throw can fire
                              the ring-out scene on a legal victim (stock); 0 = the victim lands
                              outside and walks back in (user 2026-08-27: an illegal man's ring-out
                              scrambled the legal pair / control switch) */
    RO_RULE_COUNT
};
int ringout_rule(int idx);

/* walkin.c — 0xA654 ring intro */
void eng_intro_begin(eng_state *st);
void eng_intro_end(eng_state *st);
int  eng_intro_tick(eng_state *st);
uint32_t eng_rng(void);                /* ai.c: 0x21B4 */

/* campaign.c — the tag ladder: 0x11B6 match-over, 0x1256 continue screen,
 * 0x19BA..0x1AF0 next stage, 0x1B4E ceremony, 0x1034A opponent pick. */
void eng_camp_new_game(const int *picks, const unsigned *roster);  /* charselect 0x5DAC */
const int *eng_camp_picks(void);       /* roster for eng_init_picks */
unsigned eng_camp_stage(void);         /* $1C0163 */
unsigned eng_camp_played(void);        /* $1C0165 (0x1A7E): campaign wins, the tag ranking score (0x67B8) */

/* rank.c — the BEST ranking table (0x6F6 defaults, 0x69C8 page, 0x65C0
 * game-over insert + name entry, 0xA4D4/0x7874 renderers) */
void eng_rank_reset(void);
void eng_rank_page_begin(eng_state *st, int rumble);
void eng_rank_page_frame(int rumble);
int  eng_rank_gameover_arm(eng_state *st, int rumble);   /* 1 = name entry owed */
int  eng_rank_entry_pending(void);
int  eng_rank_entry_rumble(void);
int  eng_rank_entry_frame(eng_state *st);      /* one 0x66FE frame; 0 = done */
void eng_rank_rumble_elim(const eng_state *st, int victim);   /* +0xC4 score (0x11284) */
void eng_rank_elims_clear(void);

/* attract.c — the demo-match side of the attract cycle (0xAC0/0x1102) */
int  eng_demo_active(void);
int  eng_demo_frame(eng_state *st);            /* 1 = frame consumed (wipe / scene switch) */
void eng_demo_seed(eng_state *st);             /* 0xE02 tail: all-CPU, energy, no intro */
const int *eng_demo_picks(void);               /* 0xF3E[seg] (engine slot order) */
const uint8_t *eng_demo_rumble_six(void);      /* 0xF76 when demo segment 1, else NULL */
void eng_demo_overlay_emit(const eng_state *st);   /* 0x8CF8 match card sprite (render.c) */

extern int eng_seated;                         /* core.c: bit n = player n joined */
int  eng_camp_armed(void);
int  eng_camp_end_test(eng_state *st); /* 0x1178: 1 = the match is over */
void eng_camp_tick(eng_state *st);     /* one frame of 0x11B6..0x1396 */
void eng_camp_hp(eng_state *st);       /* 0x10782 energy at match init */
void eng_camp_pick_cpu(unsigned stage, unsigned *roster, unsigned *w0, unsigned *w1);
void eng_campaign_register(void);

/* announce.c — 0xA0E8 ring announcer driver */
void eng_lazy_divorce(eng_obj *o);     /* anim.c: 0x115D2 non-mutual link drop */
eng_obj *eng_team_legal(eng_state *st, const eng_obj *o);   /* tag.c: my side's in-ring man */
int eng_pkg_register_alt(int base);    /* package.c: duplicate pick -> alt-palette clone */
void eng_fg0_bigtext(unsigned row, unsigned col, const char *txt, unsigned pal); /* the aisle name face */
int  eng_blit_text(unsigned id, char *out, int cap);   /* decode a mode-1 plate to ASCII */

/* arena overrides (src/arena.c): imported arena art per scene word */
int  wf_arena_has(unsigned scene);
void wf_arena_tiles_for_scene(unsigned scene);
int  wf_arena_compose(uint8_t *out, unsigned outsize, int plane, unsigned scene, uint16_t ox, uint16_t oy);
const uint8_t *wf_arena_palette(unsigned scene, int plane);   /* 512-byte blob, plane 0 = FG, 1 = BG; NULL = stock */
int  wf_arena_scenery_tick(const eng_state *st);   /* 1 = the scene's patches were handled */
int  wf_arena_out_scene(int in_scene);              /* 0 -> 2, 5 -> 6, else -1 */
int  wf_arena_base_scene(unsigned scene);           /* the stock scene an override derives from */
/* crowd.c: the engine's own crowd animation (regions of an imported arena) */
void wf_crowd_bind(unsigned scene, const uint8_t *blk, uint32_t len, int x0, int r0, int W, int H);
void wf_crowd_unbind(unsigned scene);
int  wf_crowd_cell(unsigned scene, int plane, int gcx, int gcy, uint8_t out[4]);
void wf_crowd_tick(const eng_state *st, unsigned scene);
/* general-mod rules (modrules.c, phase 2 of the mod plan) */
/* the game_rules rows: ONE list, src/rules.def (enum + default + label + help) */
#define GAME_RULE(e, l, d, h) e,
#define MODE_RULE(e, l, d, h)
enum {
#include "rules.def"
       MODR_COUNT };
#undef GAME_RULE
#undef MODE_RULE
const char *eng_rule_help(const char *tbl, const char *label);   /* modrules.c: rules.def help text, "" if unknown */
/* ---- modhooks.c: where the mod rules reach into the transcription ---- */
int  eng_pin_allowed(const eng_obj *o, const eng_obj *v);   /* the legal-pin gate (0x18C68 shape) + pin_anyone / pin_outside */
void eng_throw_out(eng_obj *v);                            /* mod throw_out - the thrown man clears the ropes */
int  mod_fall_launch(eng_obj *o);                          /* handler_fall init: 1 = a mod arc already flies him */
int  mod_fall_landed(eng_obj *o);                          /* handler_fall landing: 1 = landed outside (taken over) */
void mod_hit_scan_extras(eng_state *st, eng_obj *a);       /* per swing: the perched man, the referee */
void mod_weapon_landed(eng_state *st, eng_obj *a);         /* a landed weapon swing: weapon_dq */
void mod_track_pairs(eng_state *st);                       /* every frame: the parasitic pairing memory */
void mod_damage_scale(eng_state *st, eng_obj *o);          /* pending damage: human_hit_mult */
void mod_damage_taken(eng_state *st, eng_obj *o, int i, uint16_t nhp);   /* hp about to drop: parasitic_pct, ko_dq */
int  mod_exit_ring_gate(eng_state *st, eng_obj *o);        /* pad gate: exit_ring* / cage_escape, 1 = took the frame */
#define MOD_MOVE_CAGE_CLIMB 0x90                            /* a mod move id past the ROM's move table (0x8F entries) */
uint32_t mod_cage_climb(eng_obj *o, uint32_t cell);        /* the cage escape climb handler (state 5, move 0x8C) */
/* what the hooks need from anim.c */
void eng_knockback(eng_obj *o, unsigned i);                /* the 0x258E launcher rows */
int  eng_land_outside(eng_obj *o);                         /* the outside landing (0x68 lying outside / a backup leaves) */
void eng_force_cover(eng_state *st, int pinner, int victim);   /* harness WF_PINAT: a cover imposed on a pair (anim.c) */
eng_state *eng_anim_state(void);                           /* the frame-scoped state anim.c is ticking (NULL outside a tick) */
/* mode descriptor (modrules.c, phase 3) */
/* the mode_rules rows: the same list, src/rules.def */
#define GAME_RULE(e, l, d, h)
#define MODE_RULE(e, l, d, h) e,
enum {
#include "rules.def"
       MODE_COUNT };
#undef GAME_RULE
#undef MODE_RULE
void eng_topple(eng_obj *v, int from_right);    /* anim.c: knocked off the buckle */
void eng_match_escape_win(eng_state *st, eng_obj *winner);   /* referee.c */
int eng_mode_rule(int slot);
int  wf_view_w(void); int wf_view_h(void);      /* video.c: the compose window */
void wf_video_set_view(int w, int h);
void eng_rope_exit(eng_obj *o);                 /* anim.c: the over-the-rope hop */
void eng_backup_tick(eng_state *st);            /* modrules.c: random run-ins */
void eng_grapple_gauge_tick(eng_state *st);     /* hud.c: mod floating bar */
void eng_ref_knockdown(eng_state *st);          /* referee.c: mod SM10 down */
const char *eng_ref_down_badge(const eng_state *st);
void eng_sprite_reclaim_bank(unsigned id);       /* sprite.c: a late stock spawn takes its palette bank back */   /* referee.c: the ref_down_N badge while he is down, NULL up */
void eng_match_dq(eng_state *st, eng_obj *offender);   /* referee.c: mod DQ */
int eng_mod_rule(int slot);
void eng_comeback_tick(eng_state *st);        /* anim.c: 0xFD00 on fire / REGAIN POWER loop */
uint16_t eng_mod_speed(const eng_obj *o, unsigned base);
unsigned eng_mod_turbo_pct(void);                 /* mod turbo: 100 = off, 120..200 */
uint16_t eng_mod_turbo_speed(unsigned base);      /* mod turbo: scale a final walk/run speed */
void eng_mod_energy_tick(eng_state *st);
void eng_announce(eng_state *st, unsigned name, unsigned phrase);
void eng_announce_tick(eng_state *st);

/* package.c — data/wrestlers/NN packages (poses, palette, stats).
 * EXTENDED IDS (mod roster, ADR-001 rule 8): ids 12..15 are CLONE slots a
 * mod layer registers via wrestlers/NN/wrestler.json {"clone_of":B,"name":..}
 * (packed into the profile's NN.pak by tools/pack_wrestler.c). A clone
 * resolves EVERY ROM table (moves, speeds, AI rolls, portraits, throw
 * matrices — anything indexed by wrestler id) through its BASE id; its
 * package data (stats, palette, optionally sheet/poses) comes from its own
 * pak with per-file fallback to the base's package. */
#define ENG_WS_MAX      12             /* stock roster / ROM table rows */
#define ENG_WS_EXT_MAX  44             /* ids 12..43 = registered clone slots (the knob; 32 = the 18-bit tile-space ceiling) */
int  eng_ws_base(int id);              /* ROM-table index: identity for id<12, else
                                          the clone's registered base (unregistered:
                                          0 with a one-shot warning) */
int  eng_ws_clone_base(int id);        /* base id if `id` is a REGISTERED clone, else -1 */
const char *eng_ws_clone_name(int id); /* registered clone's name, else NULL */
/* WRESTLER SOUND MAP (2026-08-28): pointers packed from skin.json /
 * wrestler.json "sounds" {event: "cmd:0xNN" | "wav:name"}; the WAV lives
 * in the global sounds/ library. 0 = nothing mapped (stock behaviour). */
enum { ENG_SND_NAME_CALL = 0, ENG_SND_INTRO = 1, ENG_SND_N = 2 };
int  eng_ws_sound(int id, int event, unsigned *cmd, const char **wav);   /* 1 = cmd, 2 = wav */
double eng_sound_wav(const char *name);   /* main.c: play sounds/<name>.wav, seconds (0 = silent/headless) */
#define ENG_BODY_CLASSES 5             /* ROM behind_grab_class 0x18DFA: 0 medium (Hogan/Warrior/LOD),
                                          1 lean (Jake/DiBiase/Demolition), 2 small (Perfect),
                                          3 heavy (Boss Man/Slaughter), 4 giant (Earthquake) */
int  eng_ws_body_class(int id);        /* body template 0..4 of id's base (art + grab offsets) */
const char *eng_body_class_name(int cls);
typedef struct { int16_t x, y; uint32_t tile; uint8_t flipx, flipy, chain, pal; } eng_pkg_rec;   /* tile > 0xFFFF = clone-art arena (video.c marker bytes) */
int  eng_pkg_has_poses(unsigned id);
const uint16_t *eng_pkg_palette(unsigned id);
int  eng_pkg_stat(unsigned id, const char *name, int fallback);
uint16_t *eng_pkg_pens_mut(unsigned id);                       /* editor: live palette */
int  eng_pkg_pose(unsigned id, unsigned pose, int flip, int prow, const eng_pkg_rec **out);
int  eng_pkg_own_frame(unsigned id, unsigned pose);   /* own-art record count of a single pose, 0 = base art */
int  eng_pkg_pose_was_own(void);   /* last call served the id's OWN pak art (not base-delegated) */
int  eng_pkg_has_part(unsigned id, unsigned pose, int prow);   /* real two-man variant? */
int  eng_pkg_vict(unsigned id, unsigned row, unsigned pose, int flip, const eng_pkg_rec **out);
#define ENG_OVL_WEAPON 1   /* overlay tags: cells that are NOT the wrestler's body (pal nibble 15 = weapon bank) */
#define ENG_OVL_ROPE   2   /* pal nibble 14 = rope bank (climb poses 0x20-0x29) */
int  eng_pkg_own_n(unsigned id, unsigned pose);
int  eng_ws_grid_count(unsigned id);            /* alternate move grids (pak "mgrids") */
int  eng_ws_tmatrix(unsigned id, unsigned band, unsigned bank);   /* per-grid throw matrix row, -1 = none */
int  eng_ws_grid_get(unsigned id);
void eng_ws_grid_set(unsigned id, int k);         /* 0 = the move_map, 1..count = grid k */                          /* own-list record count */
int  eng_pkg_template(unsigned id, unsigned pose, int flip, const eng_pkg_rec **out);
unsigned eng_escape_of(unsigned victim_move);   /* core.c 0xEBC4 ladder: the escape move of a held victim's move, 0 = none */   /* the ROM single-pose list, never composed (tools) */
int  eng_wpn_spawn_type(int slot);                /* slot k -> weapon type (0xFF = empty) */
int  eng_wpn_slot(int slot, int *x, int *y, int *inside);   /* pak "wslots" placement; 0 = undefined */
int  eng_wpn_rule(int type, int field);           /* 0 carry_dz 1 carry_dx 2 tumble_steps 3 damage */
#define ENG_WPN_TILE0 0xE8000u          /* the weapon tile arena: pak-ingested weapon art (wtiles) */
#define ENG_BADGE_TILE0 0xEA000u        /* the badge tile arena: build/badges.pak (badges.c) */
#define ENG_ROPE_TILE0  0xEB000u        /* the side-rope art arena: arena paks' "rtiles" (ropeart.c) */
struct pak; void eng_ropeart_bind(unsigned scene, struct pak *pk); void eng_ropeart_unbind(unsigned scene);
int  eng_ropeart_has(unsigned scene); int eng_ropeart_emit(unsigned scene, unsigned frame, int flip, int sx, int sy, unsigned *slot);
void eng_ropeart_reset_banks(void);
int  eng_badge_emit(const char *name, int sx, int sy, unsigned *slot);   /* badges.c: draw a pak badge, origin = centre/bottom */
void eng_badges_reset_banks(void);                   /* badges.c: after a body-palette install */
int  eng_sprite_borrow_bank(const uint16_t *pens);   /* sprite.c: load a 16-pen palette into a free stock bank (-1 = none) */
int  eng_sprite_obj_top(int i);                      /* sprite.c: this frame's top sprite y of object i (hardware y, up = larger) */
int  eng_sprite_obj_drawn(int i);
int  eng_sprite_obj_cx(int i);                       /* sprite.c: the drawn sprite's horizontal centre (screen x) */                    /* sprite.c: 1 = his own sprite this frame, 2 = carried inside his partner's composite, 0 = not on screen */
void eng_wpn_carry_ctx(int type);       /* the weapon type the object being emitted HOLDS (-1 none): an image
                                           type's cells replace the box's on the shared carry poses */
int  eng_wpn_loose(int type, int variant, const eng_pkg_rec **out, int *pal_idx);   /* ringside lying/tumble cells (pak wloose), 0 = ROM art */           /* 0 carry_dz, 1 carry_dx, 2 tumble_steps; 0 = use weapon_rules */
const uint16_t *eng_wpn_palette(int idx);         /* 16 pens, NULL = none (0 = the shared bank 15) */
int  eng_wpn_cells(unsigned pose, int wrestler, int flip, eng_pkg_rec *out, int max);
const uint8_t *eng_pkg_overlay(unsigned id, unsigned pose);              /* per own record tag (automatic from the bank; overlay.json overrides), NULL = none */
enum { ENG_SRC_HOLDER = 0, ENG_SRC_HOLDER_OWN, ENG_SRC_VICTIM, ENG_SRC_VICTIM_OWN, ENG_SRC_OVERLAY, ENG_SRC_ROPE };
int  eng_pkg_alt_victim(unsigned row, unsigned pose, unsigned victim);
int  eng_pkg_mirror_ok(unsigned row, unsigned pose);   /* the ROM's self variant is a usable hold (else a mirror hole) */
#define ENG_CLASS_SLOT0 (ENG_WS_EXT_MAX - ENG_BODY_CLASSES)   /* slots 39..43 = class STOCK templates (hidden packages) */
int  eng_ws_class_slot(int cls);       /* the class template's package id, or -1 when not packed */
int  eng_ws_hidden(int id);            /* a registered slot that never appears on select (name starts with '@') */
int  eng_compose_slot(int side);
void eng_compose_ctx(int cls, int move, int frame);   /* attacker context for calibration (-1 = none) */
int  eng_calib_key(const eng_obj *o);   /* move id (state 5) or 0x90+state for a paired stance, -1 none */
int  eng_calib_get(int cls, int move, int frame, int *dx, int *dy);
void eng_calib_set(int cls, int move, int frame, int dx, int dy);
int  eng_calib_count(int cls);
int  eng_voff_get(int id, unsigned row, unsigned pose, int *dx, int *dy);   /* held man's own victim cells: per package, (holder row, pose) */
void eng_voff_set(int id, unsigned row, unsigned pose, int dx, int dy);
int  eng_voff_count(int id);
int  eng_calib_layer(int id);          /* the CLASS template slot under a skin's package (-1 for a class slot itself) */
int  eng_calib_sum(int id, int key, int frame, int *dx, int *dy);          /* class layer + the package's own delta */
int  eng_voff_sum(int id, unsigned row, unsigned pose, int *dx, int *dy);
void eng_calib_report(void);           /* --calib-report */
/* DEPTH override, per HOLDER package (holder pose, victim body class or 0xFF): 0 template, 1 holder over, 2 victim over */
int  eng_depth_get(int id, unsigned pose, unsigned vcls);
void eng_depth_set(int id, unsigned pose, unsigned vcls, int mode);
int  eng_depth_sum(int id, unsigned pose, unsigned vcls);   /* skin layer, else class layer */
int  eng_depth_count(int id);
int  eng_depth_save(int id, const char *path);
int  eng_voff_save(int id, const char *path);
int  eng_calib_save(int cls, const char *path);       /* package id whose OWN art the last eng_compose used for side 0 holder / 1 victim, -1 none */   /* nearest held body when the exact variant is missing, -1 none */
int  eng_compose(int holder, unsigned pose, int flip, int victim, const eng_pkg_rec **out, const uint8_t **src);   /* THE composition path (package.c) */
int  eng_pkg_victim_mask(unsigned row, unsigned pose, int flip, unsigned victim, uint8_t *mask, int n);   /* victim cells of a composed frame (nibble / holder subtraction) */
#define ENG_VICT_POSE0 1024u   /* synthetic ref ids: victim-body art (docs/ai-art-pipeline.md) */
#define ENG_AISLE_POSE0 768u
#define ENG_CONT_POSE0  800u   /* pak pose ids 800+cell = continue-screen face cards (row 0x30+base cells) */
#define ENG_TITLE_POSE  810u   /* pak pose id 810 = title-win card portrait (row 0x4D cell) */
const uint16_t *eng_pkg_cell_pens(unsigned id, unsigned pose);   /* a portrait surface's own palette */
unsigned eng_sprite_cont_row(int wrestler);    /* continue screen row for a wrestler (clone: virtual row when he has the art) */
unsigned eng_sprite_obj_row(int wrestler);     /* match emit row for a wrestler (clone: virtual row -> his own pak art) */
int      eng_sprite_title_row(int wrestler);   /* title-card row for a clone with card art, -1 = none */   /* pak pose ids 768+cell = aisle walkout cells (docs/ai-art-pipeline.md) */
unsigned eng_sprite_aisle_row(int wrestler);   /* walker id (raw, clones ok) -> emit row */

/* package.c: the wrestler's own move map ([cat 0..0x14][col]*3 bytes) —
 * package "movemap" override, clone-delegated, ROM row fallback */
unsigned eng_ws_move8(unsigned id, unsigned off);
void eng_pkg_reload(unsigned id);   /* editor: re-read one slot's pak */

/* palsel.c — pluggable palette-select mod (palettes/NN.json in a mod) */
int  eng_palsel_count(int id);
const char *eng_palsel_name(int id, int k);   /* k = 1..count */
const uint16_t *eng_palsel_pens(int id);      /* active alt, NULL = stock */
int  eng_palsel_get(int id);
void eng_palsel_set(int id, int k);
void eng_palsel_reset(void);

/* tag.c — apron partners and the tag exchange (tag-mode.md) */
void eng_apron_tick(eng_state *st, eng_obj *o);
void eng_tag_swap(eng_state *st, eng_obj *out, eng_obj *in);
void eng_dt_land(eng_state *st, eng_obj *diver);    /* 0x21424 double-team landing */
void eng_ringside_retarget(eng_state *st);          /* ringside brawl targeting (user-verified) */
int  eng_tag_trigger(eng_state *st, eng_obj *o);
int  eng_side(const eng_obj *o);

/* tag.c — the pin run-in (docs/engine-specs/pin-partner.md).
 * Slot order matches data/romdata/tag_rules.json row order. */
enum {
    TAG_USHER_ARM, TAG_USHER_RUNIN, TAG_PIN_ARM_DELAY,
    TAG_ENTER_TICKS, TAG_ENTER_CELLS, TAG_ENTER_STEP_X,
    TAG_BREAK_RC_PINNER, TAG_BREAK_RC_VICTIM, TAG_BREAK_DOWN_T,
    TAG_BREAK_PINNER_DOWN_T, TAG_N_RULES
};
int  eng_tag_rule(int slot);
int  eng_pin_is_pinner(const eng_obj *o);
void eng_pin_break(eng_state *st, eng_obj *p, eng_obj *v);
void eng_tag_pin_end_count(eng_state *st, eng_obj *v);   /* 0x21282: 'two-count!' if ended at 4-5, half-count cleared */       /* 0x24BC2/0x24C0C */
void eng_hold_break_by(eng_state *st, eng_obj *a, eng_obj *h);   /* 0x24AA8 family (hit.c) */
void eng_tag_arm_pin(eng_state *st, eng_obj *p, eng_obj *v);     /* 0x215B6 + 0x21732 */
void eng_tag_arm_hold(eng_state *st, eng_obj *p, eng_obj *v);    /* 0x215B6 alone (holds) */
void eng_chips_tick(eng_state *st);            /* 0x8710 $1C14CE overlay machine (chips.c) */
void eng_chips_emit(const eng_state *st, unsigned *slot);
void eng_tag_restore_control(eng_state *st, eng_obj *o);         /* 0x212D4 */
void eng_tag_pin_end(eng_state *st, eng_obj *p, eng_obj *v);     /* 0x212A0 + 0x213A6 */
void eng_tag_arm_holder(eng_state *st, eng_obj *h, eng_obj *v);   /* 0x215B6 from a non-pin hold (0x1C @0x15772) */
void eng_tag_arm_behind_holder(eng_state *st, eng_obj *h, eng_obj *v);           /* 0x20F04: the 0x1D holder's partner */
void eng_tag_arm_behind_victim(eng_state *st, eng_obj *v, eng_obj *h, unsigned mv); /* 0x2103E (0x52) / 0x21114 (0x53) */
int  eng_tag_rescue_live(eng_state *st, const eng_obj *o);
int  eng_tag_dt_window(const eng_state *st, const eng_obj *o);   /* 0x1F760 */
int  eng_tag_dt_start(eng_state *st, eng_obj *o);                /* 0x214C0 -> move 0x37 */
void eng_tag_hand_pad(eng_obj *out, eng_obj *in);
int  eng_ai_apron_attack(eng_state *st, eng_obj *o);   /* ai.c: 0x1D428 apron punch / grab */   /* tag.c: the 0x21618 pad move */
void eng_tag_arm_victim61(eng_state *st, eng_obj *v, eng_obj *h); /* 0x211EC */
void eng_tag_usher_tick(eng_state *st);                          /* $1C1682 + 0x202BA */

/* ai.c — CPU opponent as a virtual controller over the ROM policy tables */
uint32_t eng_ai_inputs(eng_state *st, eng_obj *o);
void eng_ai_frame(eng_state *st);           /* per-frame: $1C0080 feed for 0x21B4 */
int  eng_ai_rescue_tick(eng_state *st, eng_obj *o);   /* 0x215B6 arm + 0x1D526 countdown */
uint32_t eng_rng_fold(uint32_t known);      /* 0x21B4 with the known register sum */

#include "bus.h"

/* core.c — deterministic, SDL-free */
void eng_init(eng_state *st);
void eng_init_picks(eng_state *st, const int *picks);   /* picks[4] from charselect.c, NULL = default */
void eng_update(eng_state *st);

/* render.c — owns `struct wf wf` + the reused rasteriser */
int  eng_render_init(const char *rom_dir, int need_rom);
void eng_render_frame(const eng_state *st, uint32_t *pixels, int pitch);
int  eng_render_shot(const eng_state *st, const char *path);
extern int eng_bg_cam_x;               /* BG plane camera x when the FG scrolls alone; -1 = follow cam_x */

/* sprite.c — pose -> spriteram via the ported 0xD1FC decoder */
int  eng_sprite_init(const char *rom_dir);
void eng_sprite_emit(const eng_state *st);
unsigned eng_sprite_extra_bank(unsigned id);
int  eng_sprite_emit_pose(unsigned row, unsigned pose_word, int sx, int sy,
                          int partner_row, unsigned *slot);

/* ringhw.c — ROM 0x1004A/0x10120 sprite side ropes + shake machines */
void eng_ropes_arm(int right, int state, int busy_gated);
void eng_ringhw_emit(const eng_state *st, int list, unsigned *slot);
void eng_sprite_install_body_palettes(void);   /* sprite.c: 0x2AEA per-match body palettes */
void eng_sprite_scene_pals_begin(void);        /* sprite.c: interlude 0x2AEA on-demand bank map */
void eng_sprite_scene_pals_end(void);
void eng_sprite_scene_pals_rearm(void);        /* keep the map across a body-palette flush */
unsigned eng_sprite_portrait_bank(void);       /* sprite.c: 0x2AEA #$1C portrait palette bank */

/* rumble.c — Royal Rumble (docs/engine-specs/rumble.md) */
int  eng_rumble_armed(void);                    /* game select armed it (or WF_RUMBLE) */
void eng_rumble_init(eng_state *st, const int *picks, int seated);   /* 0x10902 */
void eng_rumble_tick(eng_state *st);            /* 0xBB14 spawner, 0x20AA6 re-target, 0x2017A controller */
void eng_rumble_arm_helper(eng_state *st, eng_obj *p, eng_obj *v);   /* 0x216E8: a CPU cover calls a second man onto the pile */
uint32_t eng_rumble_helper_ai(eng_state *st, eng_obj *o);            /* the helper's walk + second cover (+0xB5 b7) */
void eng_rumble_slot_free(eng_state *st, eng_obj *o);   /* 0x1A504: the eliminated man has left */

/* motion.c — ROM 0x2208/0x22C0/0x2818E/0x247C transcriptions */
void eng_sincos_step(uint32_t angle, uint32_t speed, int32_t *dx, int32_t *dy);
void eng_apply_motion(eng_obj *o);
void eng_ring_bounds(eng_obj *o);
void eng_slide_clip(eng_obj *o, int16_t d0);
void eng_screen_pos(eng_obj *o, const eng_state *st);

/* anim.c — ROM 0x1C03E/0x1C0C8/0x1C12C + cell handlers */
int  eng_prox_box(const eng_obj *o, const eng_obj *v, unsigned box);
void eng_anim_latch(eng_obj *o);
void eng_anim_tick(eng_state *st, eng_obj *o);
int  eng_hold_rule(int idx, int def);  /* data/romdata/hold_rules.json row */

/* hit.c — ROM 0x24062 hit pipeline + 0x24090 swing bookkeeping */
void eng_hit_bookkeep(eng_state *st);
void eng_hit_scan(eng_state *st);
void eng_damage_drain(eng_state *st);

/* tieup.c — ROM 0x10BE8 face_opponent + 0xF574 tie-up scan */
void eng_face_opponent(eng_state *st, eng_obj *o);
void eng_tieup_scan(eng_state *st);
void eng_retarget_tick(eng_state *st);        /* 0x2095A/0x20A3A target selector (tag/singles) */

/* referee.c — ROM 0x1F914 machine subset + pin count */
void eng_referee_init(eng_state *st);
void eng_referee_frame(eng_state *st);

/* hud.c — DEV placeholder HP bars (replaced by the ROM HUD later) */
void eng_hud_tick(eng_state *st);
void eng_mash_overlay(eng_state *st);   /* hud.c: 0x899C FG0 'mash to get up' overlay */
void eng_powerup_flash(eng_state *st);  /* hud.c: 0x88A2 POWER-UP flash (even frames) */
void eng_buyin_prompt(eng_state *st);   /* hud.c: 0xC710 coin / buy-in prompt, one slot per frame */
void eng_start_buyin(eng_state *st);    /* hud.c: 0x8B3A/0x8BA2 START buy-in */      /* 0x7506 init + 0x7548 odd frames */
void eng_join_tick(eng_state *st);      /* hud.c: 0x18C4 (frame list 0x103E) — mid-game buy-in
                                           dispatcher: tag 0x6E34 4-seat loop, rumble $1C1586 queue */
int  eng_join_maxports(void);           /* hud.c: SW2:3-4 Players dip -> 2/3/4 joinable ports */
void eng_rumble_join(eng_state *st, int port);   /* rumble.c: 0x196A staging -> queued human entrant */
void eng_rumble_continue_tick(eng_state *st);    /* rumble.c: 0xBCFE (odd frames) $1C0156 countdown +
                                                    the PLAYER-n IS DISQUALIFIED plate 0x4C+seat */
void eng_apron_press(eng_state *st, eng_obj *o);        /* tag.c: 0xE496 cat 0x13 — the human apron
                                                           man's punch 0x39 / grab 0x1C presses */
void eng_clock_tick(eng_state *st);    /* 0x262D2 even frames */
void eng_blit(unsigned id);            /* 0x2503C big FG0 blit */

/* banner.c — 0x98BA match-card "VS" banner over the ring (intro step 0) */
void eng_banner_draw(const eng_state *st);
void eng_banner_clear(void);                                  /* 0x9A42 */
int  eng_banner_active(void);
void eng_banner_refresh(void);                                /* replay the 0x98BA writes */
void eng_banner_runs(uint32_t a2, unsigned idx, unsigned d7); /* 0x9ACA glyph run */
/* aisle.c — 0x7B70 entrance walk (scene word 4) */
void eng_aisle_register(void);

/* scenery.c — ROM 0x285DA tile-animation overlay */
void eng_scenery_tick(const eng_state *st);

/* camera.c — ROM 0x26936 + 0x2983C */
void eng_camera_update(eng_state *st);

/* sound: forwards to the reused audio layer when enabled (main.c) */
void eng_sound(unsigned cmd);

/* dips.c — the cabinet DIP banks ("dips" rules table; docs/dip-switches.md).
 * Row order of the table; values are the RAW switch fields (MAME numbers). */
enum {
    DIP_COIN_A = 0,       /* SW1:1-2  3=1C/1C 2=1C/2C 1=2C/1C 0=3C/1C ($1C0067&3, 0x408) */
    DIP_BUYIN_PRICE,      /* SW1:3    1=1 coin, 0=as start (0x592 bit 2, 0x55A D0=5)     */
    DIP_REGAIN_PRICE,     /* SW1:4    1=1 coin, 0=as start (0x592 bit 3, 0x55A D0=2)     */
    DIP_CONTINUE_PRICE,   /* SW1:5    1=1 coin, 0=as start (0x592 bit 4, 0x55A D0=1/3)   */
    DIP_DEMO_SOUNDS,      /* SW1:6    1=on (0x205E gates 0x2052 while $1C007C == 0)      */
    DIP_FLIP_SCREEN,      /* SW1:7    1=off (0x203E -> $10000B bit 0)                    */
    DIP_FBI_LOGO,         /* SW1:8    1=on (0x724 skips the splash when off)             */
    DIP_DIFFICULTY,       /* SW2:1-2  3=normal 2=easy 1=hard 0=hardest (0x10806 index)   */
    DIP_PLAYERS,          /* SW2:3-4  3=4p 2=3p 1=2p ($1C0066 & 0xC00 sites)             */
    DIP_SW2_5,            /* SW2:5    unused (1=off)                                     */
    DIP_STAGE_POWERUP,    /* SW2:6-7  3=24 2=32 1=12 0=none (0x90D6 via table 0x912A)    */
    DIP_CHAMPIONSHIP,     /* SW2:8    1=5th, 0=4th defence (0x1A86)                      */
    DIP_N_ROWS
};
int      eng_dip(int row);        /* raw switch value from the table */
unsigned eng_dip_word(void);      /* $1C0066 after ROM 0x1E1E: ~(SW2<<8|SW1); WF_DIPS poke */

/* data.c — data/romdata loader */

#endif
