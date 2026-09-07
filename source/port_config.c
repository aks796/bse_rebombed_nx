/* User-editable port settings, read from bombsquad_nx.cfg on the SD card. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "error.h"
#include "port_config.h"

static PortConfig g_config = {
    .ui_scale = UI_SCALE_AUTO,
    .face_buttons = FACE_BUTTONS_AUTO,
    .split_joycons = true,
    .udp_listener = true,
    .keep_apk = false,
    .log_input = false,
    .joycon_sideways = true,
    .stick_deadzone = 0.15f,
    .gyro = false,
    .device_name = "Nintendo Switch",
};

const PortConfig *port_config(void) { return &g_config; }

/* Bumped whenever a default changes in a way that would surprise someone
 * whose config file was written by an older build. A file from before the
 * bump is re-created rather than obeyed, so a default the player never chose
 * cannot silently override the new one. */
#define CONFIG_VERSION 8

static int g_file_version;

static void trim(char *text) {
  char *end = text + strlen(text);
  while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                        end[-1] == '\t'))
    *--end = '\0';
  char *start = text;
  while (*start == ' ' || *start == '\t') start++;
  if (start != text) memmove(text, start, strlen(start) + 1);
}

static bool parse_bool(const char *value) {
  return !strcmp(value, "1") || !strcasecmp(value, "true") ||
         !strcasecmp(value, "yes") || !strcasecmp(value, "on");
}

static void apply(const char *key, const char *value) {
  if (!strcmp(key, "version")) {
    g_file_version = (int)strtol(value, NULL, 10);
  } else if (!strcmp(key, "ui_scale")) {
    if (!strcasecmp(value, "small")) g_config.ui_scale = UI_SCALE_SMALL;
    else if (!strcasecmp(value, "medium")) g_config.ui_scale = UI_SCALE_MEDIUM;
    else if (!strcasecmp(value, "large")) g_config.ui_scale = UI_SCALE_LARGE;
    else g_config.ui_scale = UI_SCALE_AUTO;
  } else if (!strcmp(key, "face_buttons")) {
    if (!strcasecmp(value, "auto")) g_config.face_buttons = FACE_BUTTONS_AUTO;
    else if (!strcasecmp(value, "labels")) g_config.face_buttons = FACE_BUTTONS_LABELS;
    else if (!strcasecmp(value, "xbox")) g_config.face_buttons = FACE_BUTTONS_XBOX;
    else g_config.face_buttons = FACE_BUTTONS_DEFAULT;
  } else if (!strcmp(key, "split_joycons")) {
    g_config.split_joycons = parse_bool(value);
  } else if (!strcmp(key, "udp_listener")) {
    g_config.udp_listener = parse_bool(value);
  } else if (!strcmp(key, "keep_apk")) {
    g_config.keep_apk = parse_bool(value);
  } else if (!strcmp(key, "log_input")) {
    g_config.log_input = parse_bool(value);
  } else if (!strcmp(key, "joycon_sideways")) {
    g_config.joycon_sideways = parse_bool(value);
  } else if (!strcmp(key, "stick_deadzone")) {
    const float parsed = strtof(value, NULL);
    if (parsed >= 0.0f && parsed < 0.9f) g_config.stick_deadzone = parsed;
  } else if (!strcmp(key, "gyro")) {
    g_config.gyro = parse_bool(value);
  } else if (!strcmp(key, "device_name")) {
    snprintf(g_config.device_name, sizeof g_config.device_name, "%s", value);
  }
}

static void write_template(void) {
  FILE *file = fopen(PORT_CONFIG_PATH, "w");
  if (!file) return;
  fprintf(file,
          "# Explodinary for Nintendo Switch -- port settings\n"
          "#\n"
          "# Leave 'version' alone; it lets the port notice a file written\n"
          "# before a default changed.\n"
          "version = %d\n"
          "#\n"
          "# ui_scale: auto | small | medium | large\n"
          "#   auto picks medium docked and small handheld.\n"
          "ui_scale = %s\n"
          "\n"
          "# face_buttons: auto | labels | default | xbox\n"
          "#   auto    -- labels in menus and on the pause screen, xbox once\n"
          "#              play resumes. A confirms where you expect it to and\n"
          "#              jump still sits under your thumb.\n"
          "#   labels  -- every button acts as the letter printed on it.\n"
          "#   default -- A and B keep their labels; X and Y swap to match\n"
          "#              the physical layout other pads use.\n"
          "#   xbox    -- fully positional; the bottom button becomes A.\n"
          "face_buttons = %s\n"
          "\n"
          "# split_joycons: treat each half of a Joy-Con pair as its own\n"
          "# player. Set to 0 to hold a pair as a single controller.\n"
          "split_joycons = %d\n"
          "\n"
          "# udp_listener: open the port LAN play and BombSquad Remote need.\n"
          "# Turning this off gives up both, along with hosting for players on\n"
          "# the same network; online play through the server list does not\n"
          "# depend on it.\n"
          "udp_listener = %d\n"
          "\n"
          "# keep_apk: leave your .apk in this folder after it has been\n"
          "# unpacked. Off by default -- once the game is installed the file\n"
          "# is just over a hundred megabytes doing nothing, and it is only\n"
          "# deleted after everything it supplied has been checked.\n"
          "keep_apk = %d\n"
          "\n"
          "# log_input: record controller styles, button presses and stick\n"
          "# positions in trace.txt. Useful for reporting a bad mapping, but\n"
          "# it writes to the SD card from the frame loop, so leave it off\n"
          "# unless you are chasing one.\n"
          "log_input = %d\n"
          "\n"
          "# joycon_sideways: a lone Joy-Con is held turned a quarter turn,\n"
          "# SL and SR up. Set to 0 if the four buttons and the stick come\n"
          "# out rotated -- it undoes the quarter turn for both together.\n"
          "joycon_sideways = %d\n"
          "\n"
          "# stick_deadzone: 0.0 - 0.9\n"
          "stick_deadzone = %.2f\n"
          "\n"
          "# gyro: forward the handheld motion sensor to the engine.\n"
          "gyro = %d\n"
          "\n"
          "# device_name: shown to other players on the network.\n"
          "device_name = %s\n",
          CONFIG_VERSION,
          g_config.ui_scale == UI_SCALE_SMALL    ? "small"
          : g_config.ui_scale == UI_SCALE_MEDIUM ? "medium"
          : g_config.ui_scale == UI_SCALE_LARGE  ? "large"
                                                 : "auto",
          g_config.face_buttons == FACE_BUTTONS_AUTO     ? "auto"
          : g_config.face_buttons == FACE_BUTTONS_LABELS ? "labels"
          : g_config.face_buttons == FACE_BUTTONS_XBOX   ? "xbox"
                                                         : "default",
          g_config.split_joycons ? 1 : 0, g_config.udp_listener ? 1 : 0,
          g_config.keep_apk ? 1 : 0,
          g_config.log_input ? 1 : 0, g_config.joycon_sideways ? 1 : 0,
          g_config.stick_deadzone,
          g_config.gyro ? 1 : 0, g_config.device_name);
  fclose(file);
}

void port_config_load(void) {
  const PortConfig defaults = g_config;

  FILE *file = fopen(PORT_CONFIG_PATH, "r");
  if (file) {
    char line[256];
    while (fgets(line, sizeof line, file)) {
      char *hash = strchr(line, '#');
      if (hash) *hash = '\0';
      char *equals = strchr(line, '=');
      if (!equals) continue;
      *equals = '\0';
      char *key = line;
      char *value = equals + 1;
      trim(key);
      trim(value);
      if (*key) apply(key, value);
    }
    fclose(file);

    if (g_file_version < CONFIG_VERSION) {
      trace("port config is from an older build (v%d); restoring defaults",
            g_file_version);
      g_config = defaults;
    }
  }
  write_template();
  trace("port config: ui_scale=%s deadzone=%.2f face_buttons=%d split_joycons=%d",
        port_config_ui_scale_name(), g_config.stick_deadzone,
        (int)g_config.face_buttons, g_config.split_joycons ? 1 : 0);
}

const char *port_config_ui_scale_name(void) {
  switch (g_config.ui_scale) {
    case UI_SCALE_SMALL: return "small";
    case UI_SCALE_MEDIUM: return "medium";
    case UI_SCALE_LARGE: return "large";
    case UI_SCALE_AUTO:
    default:
      /* Docked play sits far from a big screen, so the roomier medium scale
       * reads better there; handheld keeps the compact phone layout. */
      return appletGetOperationMode() == AppletOperationMode_Console ? "medium"
                                                                     : "small";
  }
}
