/* Whether the engine's menu UI is currently up. */

#ifndef BSNX_UI_STATE_H
#define BSNX_UI_STATE_H

#include <stdbool.h>

/* Starts watching the engine's own answer to that question. Safe to skip:
 * without it the state simply reads as "menu", which is what the port did
 * before it could tell the difference. */
void ui_state_install(void);

/* True while the controller is driving something other than a character:
 * a menu, the pause screen, or the join screen where players pick a
 * character and ready up. False once a round is actually being played. */
bool ui_state_menu_context(void);

/* The engine names the screen it is on for its own analytics. That is the
 * only way to tell the join screen apart from play, because the join screen
 * is drawn in the scene rather than as a UI window. */
void ui_state_note_screen(const char *screen);

#endif
