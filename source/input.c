/* Switch HID translated into the Android input events the engine expects.
 *
 * BombSquad is a local-multiplayer game, so every attached controller is
 * registered as its own Android joystick rather than folding them all into
 * one device.
 */

#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "error.h"
#include "guest.h"
#include "input.h"
#include "port_config.h"
#include "ui_state.h"
#include "video.h"

/* Android keycodes for gamepad buttons. */
enum {
  AKEYCODE_BACK = 4,
  AKEYCODE_DPAD_UP = 19,
  AKEYCODE_DPAD_DOWN = 20,
  AKEYCODE_DPAD_LEFT = 21,
  AKEYCODE_DPAD_RIGHT = 22,
  AKEYCODE_BUTTON_A = 96,
  AKEYCODE_BUTTON_B = 97,
  AKEYCODE_BUTTON_X = 99,
  AKEYCODE_BUTTON_Y = 100,
  AKEYCODE_BUTTON_L1 = 102,
  AKEYCODE_BUTTON_R1 = 103,
  AKEYCODE_BUTTON_L2 = 104,
  AKEYCODE_BUTTON_R2 = 105,
  AKEYCODE_BUTTON_THUMBL = 106,
  AKEYCODE_BUTTON_THUMBR = 107,
  AKEYCODE_BUTTON_START = 108,
  AKEYCODE_BUTTON_SELECT = 109,
};

typedef struct {
  u64 mask;
  int keycode[3]; /* indexed by FaceButtonScheme */
} ButtonMapping;

/* The console names the four face buttons by their position on a sideways
 * single Joy-Con as well as by their Nintendo label -- HidNpadButton_A is
 * documented as "A button / Right face button" -- so one table covers every
 * controller style.
 *
 * Columns, in FaceButtonScheme order:
 *   default  A confirms as a Switch player expects, while X and Y sit where
 *            the game's own layout puts them.
 *   labels   every button acts as the letter printed on it.
 *   xbox     fully positional: the bottom button becomes A.
 */
#define SAME3(code) {code, code, code}

static const ButtonMapping kButtons[] = {
    /*                        default              labels             xbox */
    {HidNpadButton_A, {AKEYCODE_BUTTON_A, AKEYCODE_BUTTON_A, AKEYCODE_BUTTON_B}},
    {HidNpadButton_B, {AKEYCODE_BUTTON_B, AKEYCODE_BUTTON_B, AKEYCODE_BUTTON_A}},
    {HidNpadButton_X, {AKEYCODE_BUTTON_Y, AKEYCODE_BUTTON_X, AKEYCODE_BUTTON_Y}},
    {HidNpadButton_Y, {AKEYCODE_BUTTON_X, AKEYCODE_BUTTON_Y, AKEYCODE_BUTTON_X}},
    /* Everything under an index finger runs. The game binds running to
     * BUTTON_L1 and BUTTON_R1 and leaves L2 and R2 unassigned -- and it
     * explicitly does not treat unassigned buttons as run buttons -- so ZL
     * and ZR did nothing at all. They report as the shoulder above them.
     *
     * SL and SR join the same groups: a sideways Joy-Con has no shoulder row
     * of its own, and those two sit where the index fingers land.
     *
     * The masks are grouped rather than listed one per row so that a press
     * and its release are worked out across the whole group. Listed
     * separately, holding L and tapping ZL would send a release the game
     * would act on while L was still held down. */
    {HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_AnySL,
     SAME3(AKEYCODE_BUTTON_L1)},
    {HidNpadButton_R | HidNpadButton_ZR | HidNpadButton_AnySR,
     SAME3(AKEYCODE_BUTTON_R1)},
    /* A single Joy-Con carries only one of these, so both open the menu. */
    {HidNpadButton_Plus, SAME3(AKEYCODE_BUTTON_START)},
    {HidNpadButton_Minus, SAME3(AKEYCODE_BUTTON_START)},
    {HidNpadButton_StickL, SAME3(AKEYCODE_BUTTON_THUMBL)},
    {HidNpadButton_StickR, SAME3(AKEYCODE_BUTTON_THUMBR)},
};

#undef SAME3

#define BUTTON_COUNT ((int)(sizeof kButtons / sizeof kButtons[0]))

typedef struct {
  PadState pad;
  bool present;
  bool registered;
  u64 buttons;
  float left_x, left_y;
  float right_x, right_y;
  u32 logged_style;
  u64 last_stick_log;
  /* What each of the four face buttons sent when it was pressed. A release
   * has to repeat it: on auto the mapping changes the moment a menu opens,
   * and telling the engine that one button went down and a different one
   * came up would leave the first one held for good. */
  int face_keycode[4];
} PadSlot;

static PadSlot g_pads[MAX_PADS];
/* The lifecycle thread announces the already-attached pads once, while the
 * frame loop polls them every frame. */
static Mutex g_pad_lock;
static HidSixAxisSensorHandle g_sixaxis[4];
static int g_sixaxis_count;
static bool g_touch_active[8];
static float g_touch_x[8];
static float g_touch_y[8];
static int g_touch_count;
static bool g_exit_requested;

static const HidNpadIdType kNpadIds[MAX_PADS] = {
    HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
    HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
};

void input_init(void) {
  mutexInit(&g_pad_lock);
  padConfigureInput(MAX_PADS, HidNpadStyleSet_NpadStandard);

  /* Split every Joy-Con pair so each half counts as its own player. For a
   * party game that doubles the number of people who can join from the
   * controllers already in the room. */
  if (port_config()->split_joycons) {
    for (int i = 0; i < MAX_PADS; i++)
      hidSetNpadJoyAssignmentModeSingleByDefault(kNpadIds[i]);
    trace("Joy-Con pairs split into single controllers");
  }
  for (int i = 0; i < MAX_PADS; i++) {
    /* Player 1 also owns the handheld pad so undocked play works. */
    if (i == 0) padInitializeDefault(&g_pads[i].pad);
    else padInitialize(&g_pads[i].pad, kNpadIds[i]);
    padUpdate(&g_pads[i].pad);
  }
  hidInitializeTouchScreen();

  if (port_config()->gyro) {
    /* Handheld and a docked Pro Controller expose the sensor differently, so
     * both handles are opened and whichever reports is used. */
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_sixaxis[g_sixaxis_count], 1,
                                               HidNpadIdType_Handheld,
                                               HidNpadStyleTag_NpadHandheld)))
      g_sixaxis_count++;
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_sixaxis[g_sixaxis_count], 1,
                                               HidNpadIdType_No1,
                                               HidNpadStyleTag_NpadFullKey)))
      g_sixaxis_count++;
    for (int i = 0; i < g_sixaxis_count; i++)
      hidStartSixAxisSensor(g_sixaxis[i]);
    trace("gyro enabled with %d sensor handles", g_sixaxis_count);
  }
}

/* BombSquad's Android activity forwards angular velocity, already rotated
 * into screen space. The console reports it that way for a landscape screen
 * already, so the axes pass through. */
static void poll_gyro(void) {
  static float last_x, last_y, last_z;
  for (int i = 0; i < g_sixaxis_count; i++) {
    HidSixAxisSensorState state;
    if (hidGetSixAxisSensorStates(g_sixaxis[i], &state, 1) != 1) continue;

    const float x = state.angular_velocity.x;
    const float y = state.angular_velocity.y;
    const float z = state.angular_velocity.z;
    if (fabsf(x - last_x) < 0.002f && fabsf(y - last_y) < 0.002f &&
        fabsf(z - last_z) < 0.002f) {
      return;
    }
    last_x = x;
    last_y = y;
    last_z = z;
    guest_gyro(x, y, z);
    return;
  }
}

static int device_id(int slot) { return JOY_DEVICE_ID_BASE + slot; }

/* Which of the three columns is in force right now.
 *
 * On auto, the four face buttons mean one thing while a menu, the pause
 * screen or the join screen has the controller and another once a round is
 * being played: A confirms and readies up where a Switch player reaches for
 * it, and jump stays under the thumb an Xbox player would use. The engine's
 * own idea of what is on screen is what decides, so the two always agree. */
static FaceButtonScheme resolved_scheme(void) {
  const FaceButtonScheme configured = port_config()->face_buttons;
  if (configured != FACE_BUTTONS_AUTO) return configured;
  return ui_state_menu_context() ? FACE_BUTTONS_LABELS : FACE_BUTTONS_XBOX;
}

static int keycode_for(const ButtonMapping *mapping, FaceButtonScheme scheme) {
  return mapping->keycode[(int)scheme];
}

static bool is_single_joycon(u32 style) {
  return (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) != 0;
}

/* The four buttons a controller has in a diamond, named by where they sit
 * under the player's thumb rather than by the letter printed on them. */
typedef enum { POS_TOP, POS_RIGHT, POS_BOTTOM, POS_LEFT } FacePosition;

static const char *position_name(FacePosition position) {
  switch (position) {
    case POS_TOP: return "top";
    case POS_RIGHT: return "right";
    case POS_BOTTOM: return "bottom";
    default: return "left";
  }
}

/* A Pro Controller's letters already line up with these positions, so going
 * through position is what makes every controller put the same action under
 * the same thumb. */
static int keycode_for_position(FacePosition position, FaceButtonScheme scheme) {
  /* Positional, so the bottom button is the one that jumps. */
  if (scheme == FACE_BUTTONS_XBOX) {
    switch (position) {
      case POS_TOP: return AKEYCODE_BUTTON_Y;
      case POS_RIGHT: return AKEYCODE_BUTTON_B;
      case POS_BOTTOM: return AKEYCODE_BUTTON_A;
      default: return AKEYCODE_BUTTON_X;
    }
  }
  /* Lettered, matching where a Pro Controller prints them. */
  switch (position) {
    case POS_TOP: return AKEYCODE_BUTTON_X;
    case POS_RIGHT: return AKEYCODE_BUTTON_A;
    case POS_BOTTOM: return AKEYCODE_BUTTON_B;
    default: return AKEYCODE_BUTTON_Y;
  }
}

/* Held sideways a Joy-Con is turned a quarter turn -- the left one
 * anticlockwise, the right one clockwise, each bringing SL and SR up under
 * the index fingers. The console keeps reporting the buttons as printed, so
 * the turn is undone here.
 *
 * The two halves report through different bits: a left Joy-Con's four
 * buttons arrive on the D-pad bits, a right one's on the lettered face
 * bits. Both are resolved to a position, which is what stops the two from
 * behaving like mirror images of each other. */
typedef struct {
  u64 mask;
  const char *printed;
  FacePosition upright;
  FacePosition sideways;
} JoyconFace;

static const JoyconFace kJoyconLeftFaces[] = {
    {HidNpadButton_Up, "up", POS_TOP, POS_LEFT},
    {HidNpadButton_Right, "right", POS_RIGHT, POS_TOP},
    {HidNpadButton_Down, "down", POS_BOTTOM, POS_RIGHT},
    {HidNpadButton_Left, "left", POS_LEFT, POS_BOTTOM},
};

static const JoyconFace kJoyconRightFaces[] = {
    {HidNpadButton_X, "X", POS_TOP, POS_RIGHT},
    {HidNpadButton_A, "A", POS_RIGHT, POS_BOTTOM},
    {HidNpadButton_B, "B", POS_BOTTOM, POS_LEFT},
    {HidNpadButton_Y, "Y", POS_LEFT, POS_TOP},
};

/* A full controller's D-pad, which a single Joy-Con does not have. */
typedef struct {
  u64 mask;
  int keycode;
} DpadMapping;

static const DpadMapping kDpad[] = {
    {HidNpadButton_Up, AKEYCODE_DPAD_UP},
    {HidNpadButton_Down, AKEYCODE_DPAD_DOWN},
    {HidNpadButton_Left, AKEYCODE_DPAD_LEFT},
    {HidNpadButton_Right, AKEYCODE_DPAD_RIGHT},
};

/* The console names a sideways Joy-Con's buttons by position rather than by
 * the letter printed on it, so the log spells out both. */
static const char *hid_button_name(u64 mask) {
  switch (mask) {
    case HidNpadButton_A: return "A / right-face";
    case HidNpadButton_B: return "B / down-face";
    case HidNpadButton_X: return "X / up-face";
    case HidNpadButton_Y: return "Y / left-face";
    case HidNpadButton_L: return "L";
    case HidNpadButton_R: return "R";
    case HidNpadButton_ZL: return "ZL";
    case HidNpadButton_ZR: return "ZR";
    case HidNpadButton_Plus: return "Plus";
    case HidNpadButton_Minus: return "Minus";
    case HidNpadButton_StickL: return "StickL-click";
    case HidNpadButton_StickR: return "StickR-click";
    case HidNpadButton_AnySL: return "SL";
    case HidNpadButton_AnySR: return "SR";
    case HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_AnySL:
      return "L/ZL/SL";
    case HidNpadButton_R | HidNpadButton_ZR | HidNpadButton_AnySR:
      return "R/ZR/SR";
    default: return "dpad/stick";
  }
}

static const char *style_name(u32 style) {
  if (style & HidNpadStyleTag_NpadJoyLeft) return "JoyLeft";
  if (style & HidNpadStyleTag_NpadJoyRight) return "JoyRight";
  if (style & HidNpadStyleTag_NpadJoyDual) return "JoyDual";
  if (style & HidNpadStyleTag_NpadHandheld) return "Handheld";
  if (style & HidNpadStyleTag_NpadFullKey) return "ProController";
  return "unknown";
}

/* Generous enough for a full hands-on mapping test, bounded so a stuck
 * button cannot fill the card. */
static unsigned g_button_traces;
#define BUTTON_TRACE_LIMIT 400

static void trace_button(int slot, u64 mask, int keycode, bool down) {
  if (!port_config()->log_input) return;
  if (__atomic_fetch_add(&g_button_traces, 1, __ATOMIC_RELAXED) >=
      BUTTON_TRACE_LIMIT)
    return;
  trace("pad %d %-4s %-14s hid=0x%08llx -> android keycode %d", slot + 1,
        down ? "DOWN" : "UP", hid_button_name(mask),
        (unsigned long long)mask, keycode);
}

static void trace_joycon_face(int slot, const char *printed,
                              FacePosition position, int keycode, bool down) {
  if (!port_config()->log_input) return;
  if (__atomic_fetch_add(&g_button_traces, 1, __ATOMIC_RELAXED) >=
      BUTTON_TRACE_LIMIT)
    return;
  trace("pad %d %-4s joycon '%s' -> %s position -> android keycode %d",
        slot + 1, down ? "DOWN" : "UP", printed, position_name(position),
        keycode);
}

static void register_pad(int slot) {
  char name[64];
  snprintf(name, sizeof name, "Nintendo Switch Controller %d", slot + 1);
  /* Only record the controller as known if the engine actually received the
   * event. Marking it registered after a dropped one would leave the pad
   * invisible for the rest of the session, and every later button would be
   * rejected as belonging to a nonexistent device. */
  if (!guest_input_device_event(ANDROID_JOY_ADDED, device_id(slot), 1, 0.0f, name))
    return;
  g_pads[slot].registered = true;
  trace("controller %d attached (style 0x%x)", slot + 1,
        (unsigned)padGetStyleSet(&g_pads[slot].pad));
}

static void unregister_pad(int slot) {
  guest_input_device_event(ANDROID_JOY_REMOVED, device_id(slot), 0, 0.0f, NULL);
  g_pads[slot].registered = false;
  g_pads[slot].buttons = 0;
  g_pads[slot].left_x = g_pads[slot].left_y = 0.0f;
  g_pads[slot].right_x = g_pads[slot].right_y = 0.0f;
  trace("controller %d detached", slot + 1);
}

static float apply_deadzone(float value, float other, float deadzone) {
  const float magnitude = sqrtf(value * value + other * other);
  if (magnitude < deadzone) return 0.0f;
  if (magnitude <= 0.0f) return 0.0f;
  const float scaled = (magnitude - deadzone) / (1.0f - deadzone);
  float result = (value / magnitude) * scaled;
  if (result > 1.0f) result = 1.0f;
  else if (result < -1.0f) result = -1.0f;
  return result;
}

static void send_axis_if_changed(int slot, int axis, float value, float *previous) {
  if (fabsf(value - *previous) < 0.002f) return;
  *previous = value;
  guest_input_device_event(ANDROID_JOY_AXIS, device_id(slot), axis, value, NULL);
}

void input_register_pads(void) {
  mutexLock(&g_pad_lock);
  for (int i = 0; i < MAX_PADS; i++) {
    padUpdate(&g_pads[i].pad);
    if (!padIsConnected(&g_pads[i].pad)) continue;
    if (!g_pads[i].registered) register_pad(i);
    g_pads[i].present = g_pads[i].registered;
  }
  mutexUnlock(&g_pad_lock);
}

static void poll_pad(int slot) {
  PadSlot *state = &g_pads[slot];
  padUpdate(&state->pad);

  /* Pads are announced only once the engine is live; input_register_pads
   * sweeps up whatever was already attached at that moment. */
  if (!guest_input_enabled()) return;

  const bool connected = padIsConnected(&state->pad);
  if (connected != state->present) {
    if (connected) {
      register_pad(slot);
      /* Left unset if the announcement did not land, so the next frame
       * tries again rather than assuming the engine knows about it. */
      state->present = state->registered;
    } else {
      state->present = false;
      if (state->registered) unregister_pad(slot);
    }
  }
  if (!connected || !state->registered) return;

  /* A Joy-Con changes style when it is detached from the console or from
   * its partner, so this is watched rather than read once. */
  const u32 style = padGetStyleSet(&state->pad);
  if (style != state->logged_style) {
    state->logged_style = style;
    memset(state->face_keycode, 0, sizeof state->face_keycode);
    if (port_config()->log_input)
      trace("pad %d style is now %s (0x%x)", slot + 1, style_name(style),
            (unsigned)style);
  }

  const u64 buttons = padGetButtons(&state->pad);
  const u64 changed = buttons ^ state->buttons;
  const FaceButtonScheme scheme = resolved_scheme();
  if (changed) {
    /* On a single Joy-Con the lettered bits are its face buttons and are
     * handled by position below, so the shared row skips them. */
    const u64 face_bits = is_single_joycon(style)
                              ? (HidNpadButton_A | HidNpadButton_B |
                                 HidNpadButton_X | HidNpadButton_Y)
                              : 0;
    for (int i = 0; i < BUTTON_COUNT; i++) {
      const u64 mask = kButtons[i].mask & ~face_bits;
      if (!mask) continue;
      if (!(changed & mask)) continue;
      /* Several sources can drive one keycode, so only edges of the
       * combined mask are reported. */
      const bool was_down = (state->buttons & mask) != 0;
      const bool is_down = (buttons & mask) != 0;
      if (was_down == is_down) continue;
      /* The first four entries are the diamond, and only those move. */
      int keycode;
      if (i < 4) {
        if (is_down || state->face_keycode[i] == 0) {
          keycode = keycode_for(&kButtons[i], scheme);
          state->face_keycode[i] = keycode;
        } else {
          keycode = state->face_keycode[i];
        }
      } else {
        keycode = keycode_for(&kButtons[i], scheme);
      }
      trace_button(slot, mask, keycode, is_down);
      guest_input_device_event(ANDROID_JOY_BUTTON, device_id(slot), keycode,
                               is_down ? 1.0f : 0.0f, NULL);
    }

    /* Note what is deliberately absent: the stick's pseudo-button bits.
     * The console raises those (and, on a single Joy-Con, the D-pad bits
     * with them) whenever the stick leaves centre. Forwarding them as D-pad
     * presses is what reduced movement to four directions -- the engine
     * takes the digital input over the analog axes. The stick is reported
     * purely through the axes below. */
    if (is_single_joycon(style)) {
      const bool left_half = (style & HidNpadStyleTag_NpadJoyLeft) != 0;
      const JoyconFace *faces = left_half ? kJoyconLeftFaces : kJoyconRightFaces;
      const bool sideways = port_config()->joycon_sideways;

      for (int i = 0; i < 4; i++) {
        const u64 mask = faces[i].mask;
        if (!(changed & mask)) continue;
        const FacePosition position =
            sideways ? faces[i].sideways : faces[i].upright;
        const bool is_down = (buttons & mask) != 0;
        int keycode;
        if (is_down || state->face_keycode[i] == 0) {
          keycode = keycode_for_position(position, scheme);
          state->face_keycode[i] = keycode;
        } else {
          keycode = state->face_keycode[i];
        }
        trace_joycon_face(slot, faces[i].printed, position, keycode, is_down);
        guest_input_device_event(ANDROID_JOY_BUTTON, device_id(slot), keycode,
                                 is_down ? 1.0f : 0.0f, NULL);
      }
    } else {
      for (int i = 0; i < 4; i++) {
        const u64 mask = kDpad[i].mask;
        if (!(changed & mask)) continue;
        trace_button(slot, mask, kDpad[i].keycode, (buttons & mask) != 0);
        guest_input_device_event(ANDROID_JOY_BUTTON, device_id(slot),
                                 kDpad[i].keycode,
                                 (buttons & mask) ? 1.0f : 0.0f, NULL);
      }
    }
    state->buttons = buttons;
  }

  /* A lone right Joy-Con reports its stick where a full controller reports
   * the right one. */
  const bool right_joycon = (style & HidNpadStyleTag_NpadJoyRight) != 0;
  const HidAnalogStickState left = padGetStickPos(&state->pad, right_joycon ? 1 : 0);
  const HidAnalogStickState right = padGetStickPos(&state->pad, right_joycon ? 0 : 1);
  const float deadzone = port_config()->stick_deadzone;

  /* Both raw sticks, a few times a second while one is pushed. Tells us
   * whether the console rotates a sideways Joy-Con's stick for us, and
   * which slot it arrives in. */
  if (port_config()->log_input) {
    const HidAnalogStickState raw0 = padGetStickPos(&state->pad, 0);
    const HidAnalogStickState raw1 = padGetStickPos(&state->pad, 1);
    const int pushed = abs(raw0.x) > 12000 || abs(raw0.y) > 12000 ||
                       abs(raw1.x) > 12000 || abs(raw1.y) > 12000;
    const u64 now = armGetSystemTick();
    if (pushed && now - state->last_stick_log > armGetSystemTickFreq() / 4) {
      state->last_stick_log = now;
      trace("pad %d sticks: [0] x=%6d y=%6d  [1] x=%6d y=%6d  (movement from %s)",
            slot + 1, raw0.x, raw0.y, raw1.x, raw1.y,
            right_joycon ? "[1]" : "[0]");
    }
  }

  float lx = (float)left.x / 32767.0f;
  float ly = (float)left.y / 32767.0f;
  float rx = (float)right.x / 32767.0f;
  float ry = (float)right.y / 32767.0f;

  /* The stick is turned with the controller, so it gets the same quarter
   * turn the buttons do -- otherwise pushing towards the top of the screen
   * would send the player sideways. */
  if (is_single_joycon(style) && port_config()->joycon_sideways) {
    const float x = lx, y = ly;
    if (style & HidNpadStyleTag_NpadJoyLeft) {
      lx = -y; /* turned anticlockwise */
      ly = x;
    } else {
      lx = y; /* turned clockwise */
      ly = -x;
    }
  }

  const float lx_clean = apply_deadzone(lx, ly, deadzone);
  const float ly_clean = apply_deadzone(ly, lx, deadzone);
  const float rx_clean = apply_deadzone(rx, ry, deadzone);
  const float ry_clean = apply_deadzone(ry, rx, deadzone);

  /* Android reports the Y axes growing downward. */
  send_axis_if_changed(slot, ANDROID_AXIS_X, lx_clean, &state->left_x);
  send_axis_if_changed(slot, ANDROID_AXIS_Y, -ly_clean, &state->left_y);
  send_axis_if_changed(slot, ANDROID_AXIS_Z, rx_clean, &state->right_x);
  send_axis_if_changed(slot, ANDROID_AXIS_RZ, -ry_clean, &state->right_y);
}

static void poll_touch(void) {
  HidTouchScreenState touch;
  memset(&touch, 0, sizeof touch);
  const int count = hidGetTouchScreenStates(&touch, 1) ? (int)touch.count : 0;

  const float width = (float)video_width();
  const float height = (float)video_height();

  for (int i = 0; i < count && i < (int)(sizeof g_touch_active / sizeof g_touch_active[0]);
       i++) {
    /* The touch panel reports in its own 1280x720 space regardless of the
     * rendered surface size, so normalise before scaling. */
    const float nx = (float)touch.touches[i].x / 1280.0f;
    const float ny = (float)touch.touches[i].y / 720.0f;

    const int action = g_touch_active[i]
                           ? ANDROID_TOUCH_MOVE
                           : (i == 0 ? ANDROID_TOUCH_DOWN : ANDROID_TOUCH_POINTER_DOWN);
    g_touch_active[i] = true;
    /* The engine wants a bottom-left origin in 0..1. */
    g_touch_x[i] = nx;
    g_touch_y[i] = 1.0f - ny;
    guest_touch_event(TOUCH_DEVICE_ID, i, action, g_touch_x[i], g_touch_y[i]);
  }

  for (int i = count; i < g_touch_count; i++) {
    if (!g_touch_active[i]) continue;
    g_touch_active[i] = false;
    /* The release carries the last position the finger held. Sending the
     * origin instead reads as a release in the corner of the screen, so a
     * widget highlights under the finger and then never activates. */
    guest_touch_event(TOUCH_DEVICE_ID, i,
                      i == 0 ? ANDROID_TOUCH_UP : ANDROID_TOUCH_POINTER_UP,
                      g_touch_x[i], g_touch_y[i]);
  }
  g_touch_count = count;
  (void)width;
  (void)height;
}

void input_poll(void) {
  mutexLock(&g_pad_lock);
  for (int i = 0; i < MAX_PADS; i++) poll_pad(i);
  const u64 held = padGetButtons(&g_pads[0].pad);
  mutexUnlock(&g_pad_lock);

  if (appletGetOperationMode() == AppletOperationMode_Handheld) poll_touch();
  if (g_sixaxis_count) poll_gyro();

  /* L + R + Minus + Plus quits, matching the usual homebrew exit chord. */
  const u64 chord = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Minus |
                    HidNpadButton_Plus;
  if ((held & chord) == chord) g_exit_requested = true;
}

void input_send_text(const char *utf8) {
  if (!utf8 || !*utf8) return;
  guest_input_device_event(ANDROID_TEXT_INPUT, 0, 0, 0.0f, utf8);
}

bool input_exit_requested(void) { return g_exit_requested; }
