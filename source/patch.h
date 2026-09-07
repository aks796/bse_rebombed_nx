/* Targeted edits to the loaded game code. */

#ifndef BSNX_PATCH_H
#define BSNX_PATCH_H

/* Checked while the image is still staged, before any of it is mapped
 * executable, so a wrong APK fails immediately and cheaply. */
void patch_verify_build_early(void);

/* Confirms the version string once the module is live. */
void patch_verify_build(void);

void patch_apply(void);

/* Points the game's interface lookup at the console's own network settings.
 * Called by patch_apply; separate so the reason it exists stays readable. */
void patch_local_interfaces(void);

#endif
