/* First-run data preparation and validation. */

#ifndef BSNX_SETUP_H
#define BSNX_SETUP_H

/* Creates the writable directories, migrates a freshly extracted APK into
 * the layout the engine expects, and refuses to continue with a clear
 * message if anything required is missing. */
void setup_prepare_data(void);

/* Deletes the .apk that was unpacked this launch, unless the player asked to
 * keep it. Call once the settings have been read and the data has checked
 * out; it does nothing if no archive was unpacked. */
void setup_remove_installed_apk(void);

#endif
