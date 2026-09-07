/* Whether the engine's menu UI is currently up.
 *
 * The engine never announces this, and asking it from another thread is not
 * safe -- the answer lives behind a Python-backed UI delegate that only the
 * logic thread may touch. But the engine asks itself constantly, including
 * once a frame while it draws, so the answer is taken as it goes past:
 * base::UI::IsMainUIVisible is replaced with the same three lines it already
 * ran, plus a note of what came back.
 *
 * That makes this a read-only observer. It runs on the logic thread, in the
 * engine's own call, and returns exactly what the original would have; the
 * only thing it adds is a value other threads can look at afterwards.
 *
 * The two constants below were read out of the shipped 1.7.62 arm64 library:
 *
 *   ldr x19, [x0, #64]   ; the delegate
 *   cbz x19, ...         ; no delegate means no UI
 *   ldr x8, [x19]        ; its vtable
 *   ldr x8, [x8, #48]    ; IsMainUIVisible
 *   blr x8
 *
 * patch.c refuses to run against any other build, which is what keeps them
 * honest.
 */

#include <stdint.h>
#include <switch.h>
#include <string.h>

#include "error.h"
#include "guest.h"
#include "so_util.h"
#include "ui_state.h"

#define UI_DELEGATE_OFFSET 64
#define UI_DELEGATE_VTABLE_SLOT (48 / 8)

/* Starts out saying "menu", so anything reading this before the engine has
 * drawn a frame -- or if the hook never lands -- gets the behaviour the port
 * had before any of this existed. */
static volatile int g_menu_visible = 1;
static volatile int g_observed;

static bool observe(void *ui) {
  bool visible = false;

  void *delegate = NULL;
  memcpy(&delegate, (const char *)ui + UI_DELEGATE_OFFSET, sizeof delegate);
  if (delegate) {
    void **vtable = NULL;
    memcpy(&vtable, delegate, sizeof vtable);
    bool (*is_visible)(void *) =
        (bool (*)(void *))vtable[UI_DELEGATE_VTABLE_SLOT];
    visible = is_visible(delegate);
  }

  __atomic_store_n(&g_menu_visible, visible ? 1 : 0, __ATOMIC_RELEASE);
  if (!__atomic_exchange_n(&g_observed, 1, __ATOMIC_ACQ_REL))
    trace("watching the engine's menu state (first answer: %s)",
          visible ? "menu" : "playing");
  return visible;
}

void ui_state_install(void) {
  const uintptr_t target =
      so_symbol(&guest_module, "_ZNK10ballistica4base2UI15IsMainUIVisibleEv");
  if (!target) {
    trace("no menu-state symbol to watch; face buttons stay on one mapping");
    return;
  }
  so_hook_addr(target, (uintptr_t)&observe);
}

/* ------------------------------------------------------------ join screen
 *
 * The lobby -- where a controller picks a character and readies up -- lives
 * entirely in the game's Python and is drawn into the scene, so the engine's
 * UI state calls it play. To a player it is plainly a menu: the button that
 * readies up should be the one that confirms everywhere else.
 *
 * The engine does name it, though, in the screen it reports for its own
 * analytics. That name arrives through the Java bridge, so it is taken from
 * there. */

#define JOIN_SCREEN_NAME "Joining Screen"

/* A lobby is a thing you pass through, and every screen that follows one
 * clears it. This only bounds how wrong a missed clear could get. */
#define JOIN_SCREEN_MAX_MS (5u * 60u * 1000u)

static volatile int g_joining;
static volatile u64 g_joining_since;

void ui_state_note_screen(const char *screen) {
  if (!screen) return;
  const bool joining = !strcmp(screen, JOIN_SCREEN_NAME);
  if (joining) __atomic_store_n(&g_joining_since, armGetSystemTick(), __ATOMIC_RELAXED);
  __atomic_store_n(&g_joining, joining ? 1 : 0, __ATOMIC_RELEASE);
  trace("screen is now \"%s\"; face buttons follow the %s layout", screen,
        joining ? "menu" : "screen's own");
}

static bool still_joining(void) {
  if (!__atomic_load_n(&g_joining, __ATOMIC_ACQUIRE)) return false;
  const u64 since = __atomic_load_n(&g_joining_since, __ATOMIC_RELAXED);
  const u64 elapsed_ms =
      (armGetSystemTick() - since) * 1000ull / armGetSystemTickFreq();
  return elapsed_ms < JOIN_SCREEN_MAX_MS;
}

bool ui_state_menu_context(void) {
  return __atomic_load_n(&g_menu_visible, __ATOMIC_ACQUIRE) != 0 ||
         still_joining();
}
