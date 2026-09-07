/* Corrections applied to the game's own Python before the engine reads it.
 *
 * These are bugs in Explodinary's own scripts rather than gaps in this port,
 * but they are the difference between a working menu and a stuck one, and the
 * scripts sit on the SD card where they can be corrected. Each fix below
 * names what it changes and why.
 *
 * They run on every launch rather than once at install, so unpacking the APK
 * again does not bring the bug back. Rewriting a file changes its timestamp,
 * which is what makes CPython discard the stale bytecode it cached for it.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "error.h"
#include "mod_fixes.h"

#define MAX_SCRIPT_BYTES (512u * 1024u)

/* Replaces every occurrence of `find` with `replace`, in place. The
 * replacement is never longer than what it replaces, so the text only ever
 * shrinks and no reallocation is needed. Returns how many it changed. */
static unsigned replace_all(char *text, const char *find, const char *replace) {
  const size_t find_length = strlen(find);
  const size_t replace_length = strlen(replace);
  if (replace_length > find_length) return 0;

  unsigned changed = 0;
  char *at = text;
  while ((at = strstr(at, find)) != NULL) {
    memcpy(at, replace, replace_length);
    memmove(at + replace_length, at + find_length,
            strlen(at + find_length) + 1);
    at += replace_length;
    changed++;
  }
  return changed;
}

/* Reads a script, hands it to `edit`, and writes it back only if the edit
 * changed something. */
static void edit_script(const char *path, const char *what,
                        unsigned (*edit)(char *)) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    /* Said out loud rather than passed over: "the script was not where this
     * expected it" and "the script was already correct" look identical from
     * the outside otherwise, and they call for very different answers. */
    trace("no script at %s; %s not applied", path, what);
    return;
  }

  if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return; }
  const long size = ftell(file);
  if (size <= 0 || (unsigned long)size >= MAX_SCRIPT_BYTES) {
    fclose(file);
    return;
  }
  rewind(file);

  char *text = malloc((size_t)size + 1);
  if (!text) { fclose(file); return; }
  const size_t got = fread(text, 1, (size_t)size, file);
  fclose(file);
  text[got] = '\0';

  const unsigned changed = edit(text);
  if (changed == 0) {
    trace("%s: already correct", what);
    free(text);
    return;
  }

  FILE *out = fopen(path, "wb");
  if (!out) {
    trace("could not rewrite %s; %s stays as shipped", path, what);
    free(text);
    return;
  }
  const size_t length = strlen(text);
  const bool written = fwrite(text, 1, length, out) == length;
  const bool closed = fclose(out) == 0;
  free(text);

  if (written && closed)
    trace("fixed %s (%u %s)", what, changed,
          changed == 1 ? "occurrence" : "occurrences");
  else
    trace("failed part-way through fixing %s; reinstall from the APK", what);
}

/* Both of the shop selector's buttons close the selector's own window and
 * then, a tenth of a second later, ask the UI system to navigate to a shop.
 * Two things go wrong with that, and they surface in whichever order the
 * race happens to land:
 *
 *  - Navigating needs a current main window to work out where the new one
 *    should go back to. The selector was the main window and it has just
 *    been closed, so there is none, and the navigation raises
 *    "Not currently handling no-top-level-window case".
 *
 *  - The new window is told to zoom out of a button belonging to the window
 *    that just closed. That button is gone, so building the window raises
 *    WidgetNotFoundError.
 *
 * Either way the menu is left with no window in it at all: every widget
 * still lights up under the cursor and none of them can do anything, which
 * is the soft lock.
 *
 * Nothing in this port is involved. The engine-side files this goes through
 * are identical to stock BombSquad's.
 *
 * The closing is what has to go. auxiliary_window_activate closes the
 * outgoing window itself, at the point where it has already worked out the
 * back-state, so doing it early is both redundant and destructive. Dropping
 * the origin widget on top of that costs the zoom-from-button animation and
 * nothing else; the button is still gone by the time the new window is
 * built, because closing it is the step immediately before. */
static unsigned fix_shop_selector(char *text) {
  return replace_all(
             text,
             "bui.containerwidget(edit=self._root_widget, transition=\"out_left\")",
             "pass  # window is closed by auxiliary_window_activate") +
         replace_all(text, "origin_widget=self.store_button",
                     "origin_widget=None") +
         replace_all(text, "origin_widget=self.ettoga_button",
                     "origin_widget=None");
}

void mod_fixes_apply(void) {
  edit_script(ASSETS_PATH "/ba_data/python/explodinary/ui/shop_select.py",
              "the shop selector closing its own window", fix_shop_selector);
}
