/* Coins / credits / START — transcription of ROM IRQ2 0x434 (coin
 * chutes), 0x4F2 (coin -> credit), IRQ3 0x978 + 0x55A (START seat) and
 * the 0x1E92 CREDIT-line gate. See credit.h for the RAM cells.
 *
 * IRQ2 0x434 (every frame, any screen):
 *   clr $140016 ; bsr 0x4BC (service coin)
 *   chute 1: D0=$1C0054 D1=0 D2=$1C0010 ; bsr 0x48C ; store back
 *   chute 2: D0=$1C0078 D1=1 D2=$1C0011 ; bsr 0x48C ; store back
 *
 * 0x48C (one chute; the port bit is active-low, "pressed" = bit clear):
 *   pressed                 -> D0++ (0x4EE)                      held frames
 *   released, D0 < 1        -> D0 = 0 ; if D2 < 10: D2++ (0x4B0)  idle gap
 *   released, D2 < 4        -> D0 = 0 (0x4AC)                    too soon after the last pulse
 *   released, D0 >= 0x360   -> D0 = 0 (0x556)                    stuck coin, ignored
 *   else                    -> bsr 0x4F2 ; D2 = 0 ; D0 = 0       a coin
 *
 * 0x4F2: $1C004E++ ; $1C0052++ ; sound 0x312A ; if $1C004B > $1C004E rts
 *        D1 = $1C004D ; if $1C004F + D1 >= 0x63: $1C004F = 0x63, $1C004E = 0
 *        else $1C004F += D1 ; $1C0050 += D1 ; $1C004E -= $1C004B
 *
 * 0x978 (IRQ3, $1C007C bit 7 clear, dip $1C0066 & 0x800 != 0x800):
 *   for slot 0..3: if START pressed (btst 7,(1,A0) clear):
 *       D0 = 0 ; jsr 0x55A -> 0x5CE: $1C004F == 0 ? carry clear (skip)
 *                                     : $1C004F-- ; carry set
 *       carry: clr $1C0076 ; bset 7,$1C007C ; jsr 0x1F38 ; slot+0 = 0x8000 ;
 *              slot+0x8A = port ; return carry  -> 0x8AE restart -> 0x52BE
 *
 * The dip word $1C0066 is modelled by dips.c (eng_dip_word, ROM 0x1E1E):
 * coinage takes the $1C0067 & 3 row of table 0x42C (0x408), the priced
 * takes 0x55A test their price bit at 0x592. TODO EXACT: the 2-slot
 * START path of a 2-player cabinet dip ($1C0066 & 0x800 at 0x97E — the
 * engine always walks the 4-slot loop), the D0=3/4 two-unit continue
 * (0x5DE/0x638), and the service coin 0x4BC ($140020 bit 2, $1C0056
 * debounce), which has no input mapped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "engine.h"
#include "tbl.h"
#include "scene.h"
#include "credit.h"

/* Tables this file owns (docs/adr-001-data-formats.md). */
static const tbl_def credit_tables[] = {
    { "coinage", "base/front", 0x42C, 4 * 2, TK_U8, 2,
      "0x400 coinage by dip ($1C0067 & 3): {coins per credit, credits per coin}; IRQ2 code 0x434 follows" },
};
TBL_REGISTER(credit_tables)
#define CREDIT_CAP    0x63      /* 0x526 cmpi.w #$63 */
#define HELD_MAX      0x360     /* 0x4A0 */
#define GAP_MIN       4         /* 0x49A */
#define GAP_MAX       10        /* 0x4B2 */
#define SND_COIN      0x312A    /* 0x500 */

static struct {
    unsigned cpc, cpcred;       /* $1C004B, $1C004D */
    unsigned coins;             /* $1C004E (byte) */
    unsigned credits;           /* $1C004F */
    unsigned total_cred, total_coin;   /* $1C0050, $1C0052 */
    unsigned held[2];           /* $1C0054, $1C0078 */
    unsigned gap[2];            /* $1C0010, $1C0011 */
    unsigned drawn;             /* $1C0072 */
    int      force;             /* $1C0072 bit 7 */
    int      inited;
} cr;

static void coinage_init(void)              /* 0x400 */
{
    unsigned dip = eng_dip_word() & 3u;     /* 0x408: $1C0067 & 3 (dips.c) */
    cr.cpc    = tbl8(TBL(coinage), dip * 2);
    cr.cpcred = tbl8(TBL(coinage), dip * 2 + 1);
    if (cr.cpc == 0) cr.cpc = 1;            /* ROM not loaded yet (selftest) */
    if (cr.cpcred == 0) cr.cpcred = 1;
    cr.coins = 0;
    cr.force = 1;                           /* 0x1F9E on the reset path */
    cr.inited = 1;
    if (getenv("WF_CREDITS"))
        cr.credits = (unsigned)strtoul(getenv("WF_CREDITS"), 0, 0) & 0xFFu;
}

static void coin_in(void)                   /* 0x4F2 */
{
    cr.coins = (cr.coins + 1) & 0xFFu;
    cr.total_coin = (cr.total_coin + 1) & 0xFFFFu;
    eng_sound(SND_COIN);
    if (getenv("WF_DBGSEL"))
        fprintf(stderr, "coin: coins=%u credits=%u\n", cr.coins, cr.credits);
    if (cr.cpc > cr.coins)                  /* 0x514 bhi */
        return;
    if (cr.credits + cr.cpcred >= CREDIT_CAP) {          /* 0x526 */
        cr.credits = CREDIT_CAP;
        cr.coins = 0;
        return;
    }
    cr.credits = (cr.credits + cr.cpcred) & 0xFFu;       /* 0x53C */
    cr.total_cred = (cr.total_cred + cr.cpcred) & 0xFFFFu;
    cr.coins = (cr.coins - cr.cpc) & 0xFFu;              /* 0x54E */
}

static void chute(int n, int pressed)       /* 0x48C */
{
    if (pressed) {                          /* 0x4EE */
        cr.held[n] = (cr.held[n] + 1) & 0xFFFFu;
        return;
    }
    if (cr.held[n] < 1) {                   /* 0x4B0 */
        cr.held[n] = 0;
        if (cr.gap[n] < GAP_MAX) cr.gap[n]++;
        return;
    }
    if (cr.gap[n] < GAP_MIN) { cr.held[n] = 0; return; }      /* 0x4AC */
    if (cr.held[n] >= HELD_MAX) { cr.held[n] = 0; return; }   /* 0x556 */
    coin_in();                                                /* 0x4A8 */
    cr.gap[n] = 0;
    cr.held[n] = 0;
}

void eng_coin_tick(const uint32_t inputs[4])
{
    if (!cr.inited) coinage_init();
    chute(0, (inputs[0] >> 7) & 1u);        /* $140020 bit 8, D1 = 0 */
    chute(1, (inputs[1] >> 7) & 1u);        /* $140020 bit 9, D1 = 1 */
}

unsigned eng_credits(void)     { return cr.credits; }
unsigned eng_credit_word(void) { return ((cr.coins & 0xFFu) << 8) | (cr.credits & 0xFFu); }
void     eng_credit_set(unsigned n) { if (!cr.inited) coinage_init(); cr.credits = n & 0xFFu; }

int eng_start_scan(const uint32_t inputs[4])
{
    if (!cr.inited) coinage_init();
    for (int n = 0; n < 4; n++) {           /* 0x98A moveq #3 / dbra */
        if (!((inputs[n] >> 6) & 1u))       /* 0x99A: released -> next slot */
            continue;
        if (cr.credits == 0)                /* 0x5CE: bcc, nothing happens */
            continue;
        cr.credits--;                       /* 0x5D6 */
        if (getenv("WF_DBGSEL"))
            fprintf(stderr, "start: P%d seated, credits=%u\n", n + 1, cr.credits);
        { extern int eng_seated; eng_seated |= 1 << n; }
        return n + 1;                       /* 0x9AC.. seat, carry set */
    }
    return 0;
}

void eng_credit_force(void) { cr.force = 1; }

/* 0xC404 (D0 = 2 or 5; dip bits 2/3 of $1C0067 clear, the default):
 * C=1 when a part-coin ($1C004E) or a credit ($1C004F) is in. */
int eng_can_afford(void)
{
    if (!cr.inited) coinage_init();
    return cr.coins != 0 || cr.credits != 0;               /* 0xC43A / 0xC444 */
}

/* 0x55A one price unit, D0 = 5/2/1 (buy-in / regain power / continue):
 * 0x592 btst <price bit>, $1C0067 — bit SET (switch = "as start price")
 * -> 0x5CE one credit; bit CLEAR ("1 coin") -> 0x59A: a part-coin pays
 * first ($1C004E--), else one credit is broken ($1C004F--, $1C004E =
 * coins-per-credit - 1). C=1 taken, C=0 nothing to take. */
static int take_priced(unsigned bit)
{
    if (!cr.inited) coinage_init();
    if (eng_dip_word() & (1u << bit)) {                    /* 0x598 bne -> 0x5CE */
        if (!cr.credits) return 0;                         /* 0x5CE -> 0x64E */
        cr.credits--;                                      /* 0x5D6 */
        cr.force = 1;
        return 1;
    }
    if (cr.coins) { cr.coins--; cr.force = 1; return 1; }  /* 0x5A2 */
    if (!cr.credits) return 0;                             /* 0x5AC -> 0x64E */
    cr.credits--;                                          /* 0x5B6 */
    cr.coins = (cr.cpc ? cr.cpc - 1 : 0) & 0xFFu;          /* 0x5BC/0x5C6 */
    cr.force = 1;
    return 1;
}
int eng_take_buyin(void)    { return take_priced(3); }     /* D0=2 (0x582): regain power SW1:4 */
int eng_take_continue(void) { return take_priced(4); }     /* D0=1 (0x58E): continue SW1:5 */
int eng_take_join(void)     { return take_priced(2); }     /* D0=5 (0x578->0x588): BUY-IN SW1:3 */
int eng_take_seat(void)                                    /* 0x60A2 select-screen join: one
                                                              plain START credit (0x5CE shape) */
{
    if (!cr.inited) coinage_init();
    if (!cr.credits) return 0;
    cr.credits--;
    cr.force = 1;
    return 1;
}

void eng_credit_line(void)                  /* 0x1E92 */
{
    unsigned w = eng_credit_word();
    if (!cr.force && cr.drawn == w)         /* 0x1E9C bmi / 0x1E9E cmp */
        return;
    cr.force = 0;
    cr.drawn = w;                           /* 0x1EA6 */
    eng_credit_draw(w);                     /* 0x1EB0.. (gameselect.c) */
}
