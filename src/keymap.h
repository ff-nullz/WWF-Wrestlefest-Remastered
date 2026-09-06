/* SDL keyboard mapping for the two players — engine-native config (NOT a
 * ROM table): data/keymap.json over compiled defaults, then any
 * mods/<name>/keymap.json in mods/order.txt order (later wins). Values
 * are SDL key names (SDL_GetKeyFromName). See docs/wfeditor.md.
 *
 * Bit layout of eng_state.inputs[p] (main.c input scan): bit 0 RIGHT,
 * 1 LEFT, 2 UP, 3 DOWN, 4 BUTTON1, 5 BUTTON2, 6 START, 7 COIN chute. */
#ifndef WF_KEYMAP_H
#define WF_KEYMAP_H
#include <SDL.h>

enum { KM_RIGHT = 0, KM_LEFT, KM_UP, KM_DOWN, KM_B1, KM_B2, KM_START, KM_COIN,
       KM_RUN,          /* extra: a DEDICATED RUN key = both buttons (0x30).
                           No engine input bit of its own; default unbound.
                           (user 2026-08-24: "instead of button combinations
                           for run, have a dedicated run button") */
       KM_N };
#define KM_PLAYERS 4        /* four cabinet ports (mid-game buy-in seats 3/4) */

const char  *keymap_entry_name(int player, int bit);   /* "p1_right" .. "p4_coin" */
SDL_Scancode keymap_sc(int player, int bit);           /* scancode for GetKeyboardState */
void         keymap_set(int player, int bit, SDL_Scancode sc);
const char  *keymap_keyname(int player, int bit);      /* SDL key name for display/save */
void         keymap_load(void);                        /* defaults <- data/keymap.json <- mods */
int          keymap_save(const char *path);            /* NULL = data/keymap.json; 0 = ok */
void         keymap_reset_defaults(void);
#endif
