/* User-editable port settings, read from bombsquad_nx.cfg on the SD card. */

#ifndef BSNX_PORT_CONFIG_H
#define BSNX_PORT_CONFIG_H

#include <stdbool.h>

typedef enum {
  UI_SCALE_AUTO = 0,
  UI_SCALE_SMALL,
  UI_SCALE_MEDIUM,
  UI_SCALE_LARGE,
} UiScaleSetting;

typedef enum {
  /* A and B keep their Nintendo meaning, so A confirms; X and Y follow the
   * physical layout games were designed around. */
  FACE_BUTTONS_DEFAULT = 0,
  /* Every label matches: Switch X acts as X, Switch Y acts as Y. */
  FACE_BUTTONS_LABELS,
  /* Fully positional: the bottom button is A, as on an Xbox pad. */
  FACE_BUTTONS_XBOX,
  /* Last on purpose: the three above index the keycode columns in input.c,
   * and this one is resolved into one of them before any lookup happens. */
  FACE_BUTTONS_AUTO,
} FaceButtonScheme;

typedef struct {
  UiScaleSetting ui_scale;
  FaceButtonScheme face_buttons;
  bool split_joycons;
  bool udp_listener;
  bool keep_apk;
  bool log_input;
  /* True when a lone Joy-Con is held turned a quarter turn, SL and SR up. */
  bool joycon_sideways;
  float stick_deadzone;
  bool gyro;
  char device_name[48];
} PortConfig;

const PortConfig *port_config(void);

/* Load the file if present, then write it back so a first run leaves a
 * documented template behind. */
void port_config_load(void);

/* Resolve UI_SCALE_AUTO against the current display mode. */
const char *port_config_ui_scale_name(void);

#endif
