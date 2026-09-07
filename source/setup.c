/* First-run data preparation and validation.
 *
 * The player supplies their own copy of the game. Dropping the APK into the
 * game folder is all that is asked of them: it gets unpacked here on the
 * first launch. An APK the player extracted themselves is still accepted --
 * that leaves an Android tree, which this turns into the layout the engine
 * reads and then clears of the parts nothing will ever open.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "apk.h"
#include "config.h"
#include "port_config.h"
#include "error.h"
#include "mod_fixes.h"
#include "setup.h"
#include "util.h"

static void require_directories(void) {
  static const char *const directories[] = {
      GAME_ROOT, FILES_PATH, NO_BACKUP_PATH, EXTERNAL_PATH, CACHE_PATH,
  };
  for (size_t i = 0; i < sizeof directories / sizeof directories[0]; i++)
    if (!mkpath(directories[i]))
      fatal_error("Could not create %s on the SD card.", directories[i]);
}

static void migrate_library(void) {
  if (path_exists(SO_PATH)) return;
  if (!path_exists(APK_LIB_PATH)) return;

  startup_status_update("Moving the game library into place");
  if (!move_tree(APK_LIB_PATH, SO_PATH))
    fatal_error("Could not move %s to %s.", APK_LIB_PATH, SO_PATH);
  trace("migrated libmain.so out of the extracted APK");
}

static void migrate_assets(void) {
  if (is_directory(ASSETS_PATH)) return;
  if (!is_directory(APK_ASSETS_PATH)) return;

  startup_status_update("Moving game assets into place (this takes a minute)");
  if (!move_tree(APK_ASSETS_PATH, ASSETS_PATH))
    fatal_error("Could not move %s to %s.", APK_ASSETS_PATH, ASSETS_PATH);
  trace("migrated ballistica_files out of the extracted APK");
}

/* Everything an extracted APK leaves behind that this port never opens:
 * the other ABIs' libraries, Android resources, and the Java side. */
static void remove_apk_leftovers(void) {
  static const char *const leftovers[] = {
      GAME_ROOT "/lib",         GAME_ROOT "/assets",
      GAME_ROOT "/META-INF",    GAME_ROOT "/res",
      GAME_ROOT "/kotlin",      GAME_ROOT "/classes.dex",
      GAME_ROOT "/resources.arsc", GAME_ROOT "/AndroidManifest.xml",
      GAME_ROOT "/DebugProbesKt.bin",
  };
  int removed = 0;
  for (size_t i = 0; i < sizeof leftovers / sizeof leftovers[0]; i++) {
    if (!path_exists(leftovers[i])) continue;
    if (!removed) startup_status_update("Clearing unused Android files");
    remove_tree(leftovers[i]);
    trace("removed APK leftover %s", leftovers[i]);
    removed = 1;
  }
}

static void require_assets(void) {
  static const char *const required[] = {
      ASSETS_PATH "/ba_data",
      ASSETS_PATH "/pylib",
      ASSETS_PATH "/payload_info",
  };
  for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
    if (path_exists(required[i])) continue;
    fatal_error(
        "Game assets are missing.\n\n"
        "  Expected: %s\n\n"
        "  Copy your own BombSquad 1.7.62 APK into\n  %s\n"
        "  and launch again -- it will be unpacked for you.",
        required[i], GAME_ROOT_UNIX);
  }
}

/* Clears the archive away once the game it supplied has been checked over.
 * Deliberately last: until require_assets has passed, the copy on the card
 * is the only one the player has. */
void setup_remove_installed_apk(void) {
  const char *path = apk_installed_path();
  if (!path) return;
  if (port_config()->keep_apk) {
    trace("leaving %s in place; keep_apk is set", path);
    return;
  }
  if (remove(path) == 0)
    trace("removed %s now that the game is installed", path);
  else
    trace("could not remove %s (%s)", path, strerror(errno));
}

void setup_prepare_data(void) {
  require_directories();

  /* Unpacking runs for minutes on a first launch, so it is the one job that
   * says so on screen. */
  if (apk_needed()) {
    startup_screen_open();
    apk_install();
    startup_screen_close();
  }

  migrate_library();
  migrate_assets();

  if (!path_exists(SO_PATH)) {
    fatal_error(
        "The game is not installed yet.\n\n"
        "  Copy your own BombSquad 1.7.62 APK into\n  %s\n"
        "  and launch again -- it will be unpacked for you.\n\n"
        "  You must own the game; no files come with this port.",
        GAME_ROOT_UNIX);
  }
  require_assets();
  remove_apk_leftovers();
  /* After the assets are known to be there and before the engine reads any
   * of them. */
  mod_fixes_apply();

  trace("data layout verified");
}
