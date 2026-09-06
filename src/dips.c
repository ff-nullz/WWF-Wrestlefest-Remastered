/* DIP switches — the two 8-switch banks of the wwfwfest board as a
 * synthetic "rules" table (docs/dip-switches.md holds the full inventory).
 *
 * Stock plumbing: MAME (technos/ddragon3.cpp) scatters the DSW bits over
 * the four input ports ($140020 P1 bits 12-13 = SW2:7-8, $140022 P2 bits
 * 8-13 = SW2:1-6, $140024 P3 bits 8-13 = SW1:1-6, $140026 P4 bits 8-9 =
 * SW1:7-8). ROM 0x1E1E (called from the 0x654 reset path at 0x6D6)
 * gathers them back:
 *      move.w $140020,D0 ; andi #$3000 ; asl #2  -> bits 14-15
 *   or move.w $140022,D0 ; andi #$3F00           -> bits  8-13
 *   or move.w $140024,D0 ; andi #$3F00 ; lsr #8  -> bits  0-5
 *   or move.w $140026,D0 ; andi #$300  ; lsr #2  -> bits  6-7
 *   not.w -> $1C0066
 * which round-trips to plainly  $1C0066 = ~(SW2 << 8 | SW1):  byte
 * $1C0066 = ~SW2, byte $1C0067 = ~SW1, a set bit = switch OFF (0).
 * Consumers therefore test INVERTED switch bits; eng_dip_word() serves
 * that word so every wired site can keep the stock mask.
 *
 * The table stores the RAW switch values (what the bank physically reads,
 * MAME's PORT_DIPSETTING numbers); defaults = factory settings, so a
 * missing/unedited table changes nothing. */
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"
#include "tbl.h"

/* Factory defaults (MAME wwfwfest PORT_DIPNAME defaults):
 * SW1 = 0xEF (1C/1C, buy-in 1 coin, regain 1 coin, continue "as start
 * price", demo sounds on, flip off, FBI logo on); SW2 = 0xFF (normal,
 * 4 players, SW2:5 off, power-up 24, championship 5th). */
static const uint8_t dips_be[DIP_N_ROWS * 2] = {
    TBL_BE16(3),   /* DIP_COIN_A            SW1:1-2  3=1C/1C 2=1C/2C 1=2C/1C 0=3C/1C */
    TBL_BE16(1),   /* DIP_BUYIN_PRICE       SW1:3    1=1 coin, 0=as start price      */
    TBL_BE16(1),   /* DIP_REGAIN_PRICE      SW1:4    1=1 coin, 0=as start price      */
    TBL_BE16(0),   /* DIP_CONTINUE_PRICE    SW1:5    1=1 coin, 0=as start price      */
    TBL_BE16(1),   /* DIP_DEMO_SOUNDS       SW1:6    1=on, 0=off                     */
    TBL_BE16(1),   /* DIP_FLIP_SCREEN       SW1:7    1=off, 0=on                     */
    TBL_BE16(1),   /* DIP_FBI_LOGO          SW1:8    1=on, 0=off                     */
    TBL_BE16(3),   /* DIP_DIFFICULTY        SW2:1-2  3=normal 2=easy 1=hard 0=hardest */
    TBL_BE16(3),   /* DIP_PLAYERS           SW2:3-4  3=4 players 2=3 1=2             */
    TBL_BE16(1),   /* DIP_SW2_5             SW2:5    unused, 1=off                   */
    TBL_BE16(3),   /* DIP_STAGE_POWERUP     SW2:6-7  3=24 2=32 1=12 0=none (0x90D6)  */
    TBL_BE16(1),   /* DIP_CHAMPIONSHIP      SW2:8    1=5th, 0=4th (0x1A86)           */
};
static const char *const dip_labels[] = {
    "coin_a", "buyin_price", "regain_power_price", "continue_price",
    "demo_sounds", "flip_screen", "fbi_logo",
    "difficulty", "players", "sw2_5_unused", "clear_stage_powerup",
    "championship_game", NULL };
static const tbl_def dip_tables[] = {
    { "dips", "rules", TBL_SYNTH, sizeof dips_be, TK_U16, 1,
      "cabinet DIP switches, raw MAME values (docs/dip-switches.md): coin_a 3=1C/1C 2=1C/2C 1=2C/1C 0=3C/1C; prices 1=1 coin 0=as start; demo/fbi 1=on; flip 1=off; difficulty 3=normal 2=easy 1=hard 0=hardest; players 3/2/1=4/3/2; powerup 3=24 2=32 1=12 0=none; championship 1=5th 0=4th", dips_be, dip_labels },
};
TBL_REGISTER(dip_tables)

int eng_dip(int row)
{
    if (row < 0 || row >= DIP_N_ROWS) return 0;
    if (tbl_bytes(TBL(dips), NULL)) return (int)tbl16(TBL(dips), (uint32_t)row * 2u);
    return (dips_be[row * 2] << 8) | dips_be[row * 2 + 1];
}

/* $1C0066 as ROM 0x1E1E leaves it: ~(SW2 << 8 | SW1). WF_DIPS=0xNNNN
 * (harness poke, raw SW2<<8|SW1) overrides the table. */
unsigned eng_dip_word(void)
{
    unsigned sw1, sw2;
    const char *ov = getenv("WF_DIPS");
    if (ov) return ~(unsigned)strtoul(ov, 0, 0) & 0xFFFFu;
    sw1 = ((unsigned)eng_dip(DIP_COIN_A) & 3u)
        | (((unsigned)eng_dip(DIP_BUYIN_PRICE)    & 1u) << 2)
        | (((unsigned)eng_dip(DIP_REGAIN_PRICE)   & 1u) << 3)
        | (((unsigned)eng_dip(DIP_CONTINUE_PRICE) & 1u) << 4)
        | (((unsigned)eng_dip(DIP_DEMO_SOUNDS)    & 1u) << 5)
        | (((unsigned)eng_dip(DIP_FLIP_SCREEN)    & 1u) << 6)
        | (((unsigned)eng_dip(DIP_FBI_LOGO)       & 1u) << 7);
    sw2 = ((unsigned)eng_dip(DIP_DIFFICULTY) & 3u)
        | (((unsigned)eng_dip(DIP_PLAYERS)        & 3u) << 2)
        | (((unsigned)eng_dip(DIP_SW2_5)          & 1u) << 4)
        | (((unsigned)eng_dip(DIP_STAGE_POWERUP)  & 3u) << 5)
        | (((unsigned)eng_dip(DIP_CHAMPIONSHIP)   & 1u) << 7);
    return ~((sw2 << 8) | sw1) & 0xFFFFu;      /* 0x1E64 not.w */
}
