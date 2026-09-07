/* Targeted edits to the loaded game code.
 *
 * Kept deliberately small: each patch names the exact symbol it rewrites and
 * refuses to guess if the symbol is missing, so an unsupported build fails
 * with a clear message instead of misbehaving later.
 */

#include <stdint.h>
#include <string.h>

#include "error.h"
#include "guest.h"
#include "patch.h"
#include "net_shim.h"
#include "so_util.h"
#include "ui_state.h"

/* The one build this port targets.
 *
 * Explodinary carries its own engine build rather than the stock one. It
 * reports the same 1.7.62 version string but a lower build number, 22824
 * against stock's 22837, so the number is what tells the two apart. Loading
 * stock BombSquad here would get as far as this check and stop. */
#define SUPPORTED_VERSION "1.7.62"
#define SUPPORTED_BUILD 22824
#define SUPPORTED_NAME "BombSquad Explodinary"

void patch_verify_build_early(void) {
  /* The build number is a plain int, so it can be read from the staging copy
   * before anything is mapped executable. The version string next to it is a
   * relocated pointer into the live mapping, which does not exist yet. */
  const int *build = so_symbol_staging(&guest_module,
                                       "_ZN10ballistica18kEngineBuildNumberE");
  if (!build) fatal_error("This libmain.so is not a Ballistica engine build.");

  if (*build != SUPPORTED_BUILD)
    fatal_error("Unsupported game build %d.\n\n"
                "  This port needs %s (engine build %d),\n"
                "  arm64-v8a. Stock BombSquad reports build 22837 and will\n"
                "  not run here.",
                *build, SUPPORTED_NAME, SUPPORTED_BUILD);
}

void patch_verify_build(void) {
  const char *const *version =
      (const char *const *)so_symbol(&guest_module, "_ZN10ballistica14kEngineVersionE");
  if (!version || !*version || strcmp(*version, SUPPORTED_VERSION))
    fatal_error("Unsupported game version %s.\n\n"
                "  This port needs %s, arm64-v8a.",
                (version && *version) ? *version : "unknown", SUPPORTED_NAME);
  trace("verified %s (engine %s build %d)", SUPPORTED_NAME, *version,
        SUPPORTED_BUILD);
}

/* mov w0, #0 ; ret */
static const uint32_t kReturnZero[2] = {0x52800000u, 0xD65F03C0u};

void patch_apply(void) {
  /* The Android platform layer says it owns text entry, which on a phone
   * means a Java dialog. There is no such dialog here, so report no platform
   * editor and let the engine use its own controller-driven on-screen
   * keyboard instead. */
  const uintptr_t have_string_editor = so_symbol(
      &guest_module, "_ZN10ballistica4base18AppPlatformAndroid16HaveStringEditorEv");
  if (!have_string_editor)
    fatal_error("Could not find AppPlatformAndroid::HaveStringEditor.");
  if (so_patch_code((void *)have_string_editor, kReturnZero, sizeof kReturnZero) != 0)
    fatal_error("Could not patch AppPlatformAndroid::HaveStringEditor.");
  trace("patched HaveStringEditor to false");

  patch_local_interfaces();
  ui_state_install();
}

/* The game bundles the NDK's compatibility getifaddrs, which asks a Linux
 * netlink socket for the interface list -- something this console does not
 * have, so it fails and the engine's LAN scan is left with no addresses to
 * broadcast to. Without this the console can host a LAN game and answer
 * BombSquad Remote, but can never see anybody else's game.
 *
 * A missing symbol is not fatal: everything except finding other people's
 * LAN games still works, and saying so in the log beats refusing to start. */
void patch_local_interfaces(void) {
  const uintptr_t get = so_symbol(&guest_module, "getifaddrs");
  const uintptr_t release = so_symbol(&guest_module, "freeifaddrs");
  if (!get || !release) {
    trace("no bundled getifaddrs to replace; LAN game discovery unavailable");
    return;
  }
  so_hook_addr(get, (uintptr_t)&net_getifaddrs);
  so_hook_addr(release, (uintptr_t)&net_freeifaddrs);
  trace("replaced the game's netlink getifaddrs with the console's own");
}
