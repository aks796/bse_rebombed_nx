/* Switch HID translated into the Android input events the engine expects. */

#ifndef BSNX_INPUT_H
#define BSNX_INPUT_H

#include <stdbool.h>

/* Android event ids accepted by PlatformAndroid::HandleAndroidInputDeviceEvent
 * in BombSquad 1.7.62. */
enum {
  ANDROID_JOY_ADDED = 0,
  ANDROID_JOY_REMOVED = 1,
  ANDROID_JOY_AXIS = 2,
  ANDROID_JOY_HAT = 3,
  ANDROID_JOY_BUTTON = 4,
  ANDROID_KEYBOARD_ADDED = 5,
  ANDROID_KEYBOARD_REMOVED = 6,
  ANDROID_TEXT_INPUT = 7,
};

/* Android MotionEvent axis ids. */
enum {
  ANDROID_AXIS_X = 0,
  ANDROID_AXIS_Y = 1,
  ANDROID_AXIS_Z = 11,
  ANDROID_AXIS_RZ = 14,
};

/* Android MotionEvent actions used for touch. */
enum {
  ANDROID_TOUCH_DOWN = 0,
  ANDROID_TOUCH_UP = 1,
  ANDROID_TOUCH_MOVE = 2,
  ANDROID_TOUCH_POINTER_DOWN = 5,
  ANDROID_TOUCH_POINTER_UP = 6,
};

void input_init(void);

/* Announce every controller that is already attached. Called once, just
 * before the engine goes live, so its first frame sees them. */
void input_register_pads(void);

/* Poll HID and emit whatever changed. Called once per rendered frame. */
void input_poll(void);

/* Deliver a string through the text-input route, used by the software
 * keyboard bridge. */
void input_send_text(const char *utf8);

/* True while the user is holding the exit chord, so the frame loop can quit
 * cleanly instead of being killed by the applet. */
bool input_exit_requested(void);

#endif
