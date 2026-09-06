/* Coins, credits and the START seat — ROM IRQ2 0x434 / 0x48C / 0x4F2,
 * IRQ3 0x978 / 0x55A, CREDIT line gate 0x1E92 (credit.c).
 *
 * Work-RAM cells the ROM keeps (all engine-private here):
 *   $1C004B coins per credit, $1C004D credits per coin   (0x400 / table 0x42C)
 *   $1C004E coins banked toward the next credit (byte)
 *   $1C004F credits (byte, capped 0x63)
 *   $1C0050 / $1C0052 lifetime credit / coin counters (bookkeeping only)
 *   $1C0054 / $1C0078 chute 1 / 2 held-frame counters, $1C0010 / $1C0011
 *           frames since the last pulse (saturate at 10)
 *   $1C0072 last credit word the CREDIT line drew (bit 7 = force redraw)
 *
 * Input bits (eng_state.inputs[p]): bit 6 = START, bit 7 = COIN chute p.
 */
#ifndef ENG_CREDIT_H
#define ENG_CREDIT_H

#include <stdint.h>

/* IRQ2 0x434: run once per frame on every screen (the interrupt is not
 * gated by the scene). Coins land in the credit counter. */
void     eng_coin_tick(const uint32_t inputs[4]);

unsigned eng_credits(void);        /* $1C004F */
int      eng_can_afford(void);     /* 0xC404: a coin or a credit is in (default dips) */
int      eng_take_buyin(void);     /* 0x55A D0=2 (regain power, dip SW1:4 at 0x592) */
int      eng_take_continue(void);  /* 0x55A D0=1 (continue, dip SW1:5 at 0x592) */
int      eng_take_seat(void);      /* 0x60A2 select-screen join: one plain credit */
int      eng_take_join(void);      /* 0x55A D0=5 (mid-game BUY-IN join, dip SW1:3 at 0x592 —
                                      0x578 maps D0=5 to price bit 2 at 0x588) */
unsigned eng_credit_word(void);    /* word at $1C004E: coins<<8 | credits */

/* IRQ3 0x978 (4-slot cabinet path): scan the START buttons; the first
 * pressed one with a credit available consumes it (0x55A mode 0 ->
 * 0x5CE) and seats that player. Returns player index + 1, or 0. With
 * zero credits START does nothing (0x5CE bcc). */
int      eng_start_scan(const uint32_t inputs[4]);

/* 0x1F9E: bset 7,$1C0072 — force the next 0x1E92 to redraw. */
void     eng_credit_force(void);
/* 0x1E92: redraw the CREDIT line when the credit word changed or a
 * redraw is forced. Callers: the attract hold (IRQ3 0x900) and the game
 * select loop (0x546A). */
void     eng_credit_line(void);

/* Harness poke: preload credits (WF_CREDITS). */
void     eng_credit_set(unsigned n);

#endif
