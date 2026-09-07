/* Corrections applied to the game's own Python before the engine reads it. */

#ifndef BSNX_MOD_FIXES_H
#define BSNX_MOD_FIXES_H

/* Rewrites the handful of lines in Explodinary's scripts that are known to
 * break the menus, if they are still as shipped. Safe to run every launch:
 * an already-corrected file is left alone, and a missing one is ignored. */
void mod_fixes_apply(void);

#endif
