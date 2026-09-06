/* wfeditor — the in-engine deep-dive editor (`wfengine --editor`): a second
 * SDL window (Nuklear UI) beside the running game. Edits the source data
 * tree (data/tables, data/wrestlers JSON) and applies table /
 * package edits LIVE to the running match; drives the C tools (export,
 * pack, verify, regress) and shows their output. See docs/wfeditor.md. */
#ifndef WF_EDITOR_H
#define WF_EDITOR_H
#include <SDL.h>
#include "engine.h"

int  ed_open(void);                        /* create the editor window; 0 = ok */
void ed_close(void);
int  ed_is_open(void);
void ed_handle_event(const SDL_Event *ev); /* route events for the editor window */
void ed_frame(eng_state *st);              /* build + draw the UI (once per game frame) */
int  ed_game_paused(void);                 /* 1 = the editor holds the game (pause / step) */
int  ed_consume_step(void);                /* 1 = run exactly one frame now */
int  ed_consume_back(void);                /* 1 = rewind one frame (snapshot ring) */
int  ed_wants_keyboard(void);              /* editor window focused: game ignores the keys */
#endif
