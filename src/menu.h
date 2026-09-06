#ifndef MENU_H
#define MENU_H

/* C game-mode select + per-mode settings.
 *
 * Stock GAME SELECT is painted by jsr $26e66 at 0x5318, after a credit
 * and a player start. C takes over at 0x5324 (or the 0x534A input loop
 * if start lands after the load) so the banner, brick, and two cards
 * stay. Extra modes scroll in from the right. Settings keeps the same
 * chrome and only covers the card region. Arcade is --68k
 * and exact scenarios leave the 68k menu
 * alone. Stock C is --port; default live is mods. */

extern int wf_menu_enabled;
int  wf_menu_active(void);
int  wf_menu_offer(unsigned pc);
void wf_menu_begin(void);
void wf_menu_run_frame(long target);

#endif
