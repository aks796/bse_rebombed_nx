/* Unpacking the player's own APK into the layout the engine reads. */

#ifndef BSNX_APK_H
#define BSNX_APK_H

#include <stdbool.h>

/* Looks for a .apk in the game folder and pulls out the pieces this port
 * needs: the arm64 library and the asset tree. Only what is missing gets
 * written, so a half-finished install repairs itself on the next launch.
 *
 * Returns true when something was extracted. A malformed or wrong-flavoured
 * APK stops the launch with an explanation rather than being skipped
 * quietly -- the player put it there on purpose. */
bool apk_install(void);

/* True when something is missing that an APK could supply, so the caller can
 * put a screen up before the long part starts. */
bool apk_needed(void);

/* The archive unpacked during this launch, or NULL if none was. Meaningful
 * only after apk_install() has returned true. */
const char *apk_installed_path(void);

#endif
