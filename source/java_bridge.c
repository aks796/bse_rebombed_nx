/* The Java side of BombSquad's Android activity, answered natively.
 *
 * Every fromNativeX call the engine makes on its BallisticaContext is
 * declared here and handled inline. The android.graphics.Bitmap surface the
 * text-texture path needs lives here too, since the bitmaps it returns are
 * created by this bridge.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bridge.h"
#include "config.h"
#include "error.h"
#include "guest.h"
#include "jni_env.h"
#include "port_config.h"
#include "ui_state.h"
#include "text.h"
#include "util.h"

enum {
  METHOD_NONE = 0,
  METHOD_CLIPBOARD_GET,
  METHOD_CLIPBOARD_HAS,
  METHOD_CLIPBOARD_SET,
  METHOD_CREATE_TEXT_TEXTURE,
  METHOD_INVOKE_STRING_EDITOR,
  METHOD_GET_BA_LOCALE,
  METHOD_GET_DEVICE_NAME,
  METHOD_GET_DEVICE_SIZE,
  METHOD_GET_DEVICE_UUID,
  METHOD_GET_EXEC_ARG,
  METHOD_GET_EXTERNAL_FILES_DIR,
  METHOD_GET_FILES_DIR,
  METHOD_GET_HAS_TOUCH_SCREEN,
  METHOD_GET_LOCALE_TAG,
  METHOD_GET_NO_BACKUP_FILES_DIR,
  METHOD_GET_SYSTEM_VERSION,
  METHOD_GET_TEXT_BOUNDS,
  METHOD_HAVE_PERMISSION,
  METHOD_INIT_COMPLETE,
  METHOD_IS_DAYDREAM,
  METHOD_IS_DESKTOP,
  METHOD_IS_ON_TV,
  METHOD_IS_FIRE_TV,
  METHOD_MISC_COMMAND,
  METHOD_MISC_COMMAND_2,
  METHOD_MISC_COMMAND_3,
  METHOD_MISC_COMMAND_ARRAY,
  METHOD_MISC_COMMAND_BUFFER,
  METHOD_OPEN_URL,
  METHOD_QUIT,
  METHOD_REQUEST_INIT_CYCLE,
  METHOD_START_NETWORK_MONITOR,
  METHOD_GET_PACKAGE_NAME,
};

enum {
  FIELD_NONE = 0,
  FIELD_WINDOW_SERVICE,
  FIELD_SDK_INT,
};

const JMemberDesc java_methods[] = {
    {METHOD_CLIPBOARD_GET, "fromNativeClipboardGetText", JK_OBJECT},
    {METHOD_CLIPBOARD_HAS, "fromNativeClipboardHasText", JK_BOOLEAN},
    {METHOD_CLIPBOARD_SET, "fromNativeClipboardSetText", JK_VOID},
    {METHOD_CREATE_TEXT_TEXTURE, "fromNativeCreateTextTexture", JK_OBJECT},
    {METHOD_INVOKE_STRING_EDITOR, "fromNativeDoInvokeStringEditor", JK_VOID},
    {METHOD_GET_BA_LOCALE, "fromNativeGetBaLocale", JK_OBJECT},
    {METHOD_GET_DEVICE_NAME, "fromNativeGetDeviceName", JK_OBJECT},
    {METHOD_GET_DEVICE_SIZE, "fromNativeGetDeviceSize", JK_OBJECT},
    {METHOD_GET_DEVICE_UUID, "fromNativeGetDeviceUUID", JK_OBJECT},
    {METHOD_GET_EXEC_ARG, "fromNativeGetExecArg", JK_OBJECT},
    {METHOD_GET_EXTERNAL_FILES_DIR, "fromNativeGetExternalFilesDirString", JK_OBJECT},
    {METHOD_GET_FILES_DIR, "fromNativeGetFilesDirString", JK_OBJECT},
    {METHOD_GET_HAS_TOUCH_SCREEN, "fromNativeGetHasTouchScreen", JK_BOOLEAN},
    {METHOD_GET_LOCALE_TAG, "fromNativeGetLocaleTag", JK_OBJECT},
    {METHOD_GET_NO_BACKUP_FILES_DIR, "fromNativeGetNoBackupFilesDirString", JK_OBJECT},
    {METHOD_GET_SYSTEM_VERSION, "fromNativeGetSystemVersionString", JK_OBJECT},
    {METHOD_GET_TEXT_BOUNDS, "fromNativeGetTextBounds", JK_OBJECT},
    {METHOD_HAVE_PERMISSION, "fromNativeHavePermission", JK_BOOLEAN},
    {METHOD_INIT_COMPLETE, "fromNativeInitComplete", JK_VOID},
    {METHOD_IS_DAYDREAM, "fromNativeIsDaydream", JK_BOOLEAN},
    {METHOD_IS_DESKTOP, "fromNativeIsDesktop", JK_BOOLEAN},
    {METHOD_IS_ON_TV, "fromNativeIsOnTV", JK_BOOLEAN},
    {METHOD_IS_FIRE_TV, "fromNativeIsRunningOnFireTV", JK_BOOLEAN},
    {METHOD_MISC_COMMAND, "fromNativeMiscAndroidCommand", JK_VOID},
    {METHOD_MISC_COMMAND_2, "fromNativeMiscAndroidCommand2", JK_VOID},
    {METHOD_MISC_COMMAND_3, "fromNativeMiscAndroidCommand3", JK_VOID},
    {METHOD_MISC_COMMAND_ARRAY, "fromNativeMiscAndroidCommandArray", JK_VOID},
    {METHOD_MISC_COMMAND_BUFFER, "fromNativeMiscAndroidCommandBuffer", JK_VOID},
    {METHOD_OPEN_URL, "fromNativeOpenURL", JK_VOID},
    {METHOD_QUIT, "fromNativeQuit", JK_VOID},
    {METHOD_REQUEST_INIT_CYCLE, "fromNativeRequestNativeInitCycle", JK_VOID},
    {METHOD_START_NETWORK_MONITOR, "fromNativeStartNetworkAvailabilityMonitoring", JK_VOID},
    {METHOD_GET_PACKAGE_NAME, "getPackageName", JK_OBJECT},
};
const int java_method_count = (int)(sizeof java_methods / sizeof java_methods[0]);

const JMemberDesc java_fields[] = {
    {FIELD_WINDOW_SERVICE, "WINDOW_SERVICE", JK_OBJECT},
    {FIELD_SDK_INT, "SDK_INT", JK_INT},
};
const int java_field_count = (int)(sizeof java_fields / sizeof java_fields[0]);

/* ------------------------------------------------ engine startup handshake */

static volatile int g_init_cycle_requests;
static volatile int g_init_complete;
static volatile int g_quit_requested;
static volatile int g_network_monitor_started;

void bridge_request_init_cycle(void) {
  const int pending = __atomic_add_fetch(&g_init_cycle_requests, 1, __ATOMIC_RELEASE);
  trace("engine requested an init cycle (%d pending)", pending);
}

bool bridge_take_init_cycle_request(void) {
  int pending;
  do {
    pending = __atomic_load_n(&g_init_cycle_requests, __ATOMIC_ACQUIRE);
    if (pending <= 0) return false;
  } while (!__atomic_compare_exchange_n(&g_init_cycle_requests, &pending,
                                        pending - 1, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE));
  return true;
}

bool bridge_init_complete(void) {
  return __atomic_load_n(&g_init_complete, __ATOMIC_ACQUIRE) != 0;
}

bool bridge_quit_requested(void) {
  return __atomic_load_n(&g_quit_requested, __ATOMIC_ACQUIRE) != 0;
}

bool bridge_network_monitor_started(void) {
  return __atomic_load_n(&g_network_monitor_started, __ATOMIC_ACQUIRE) != 0;
}

/* ------------------------------------------------- main-thread call queue */

#define MAIN_CALL_SLOTS 128
#define MAIN_CALL_ID_LENGTH 24

static char g_main_calls[MAIN_CALL_SLOTS][MAIN_CALL_ID_LENGTH];
static int g_main_call_head;
static int g_main_call_tail;
static Mutex g_main_call_lock;
static CondVar g_main_call_signal;
static int g_main_call_lock_ready;
static volatile int g_surface_refresh;

static void main_call_lock(void) {
  if (!g_main_call_lock_ready) {
    mutexInit(&g_main_call_lock);
    condvarInit(&g_main_call_signal);
    g_main_call_lock_ready = 1;
  }
  mutexLock(&g_main_call_lock);
}

void bridge_queue_main_thread_call(const char *id) {
  if (!id) return;
  main_call_lock();
  const int next = (g_main_call_tail + 1) % MAIN_CALL_SLOTS;
  if (next == g_main_call_head) {
    mutexUnlock(&g_main_call_lock);
    trace("main-thread call queue overflowed; dropping id %s", id);
    return;
  }
  snprintf(g_main_calls[g_main_call_tail], MAIN_CALL_ID_LENGTH, "%s", id);
  g_main_call_tail = next;
  condvarWakeOne(&g_main_call_signal);
  mutexUnlock(&g_main_call_lock);
}

bool bridge_wait_main_thread_call(char *out, size_t size, unsigned timeout_ms) {
  main_call_lock();
  if (g_main_call_head == g_main_call_tail) {
    condvarWaitTimeout(&g_main_call_signal, &g_main_call_lock,
                       (u64)timeout_ms * 1000000ull);
  }
  if (g_main_call_head == g_main_call_tail) {
    mutexUnlock(&g_main_call_lock);
    return false;
  }
  snprintf(out, size, "%s", g_main_calls[g_main_call_head]);
  g_main_call_head = (g_main_call_head + 1) % MAIN_CALL_SLOTS;
  mutexUnlock(&g_main_call_lock);
  return true;
}

void bridge_request_surface_refresh(void) {
  __atomic_store_n(&g_surface_refresh, 1, __ATOMIC_RELEASE);
}

bool bridge_take_surface_refresh(void) {
  return __atomic_exchange_n(&g_surface_refresh, 0, __ATOMIC_ACQ_REL) != 0;
}

/* ------------------------------------------------------------- clipboard */

static char g_clipboard[1024];

/* ------------------------------------------------------- object handlers */

static jobject directory_string(const char *host_path, const char *guest_path) {
  if (!mkpath(host_path))
    trace("could not create %s", host_path);
  return jni_new_string(guest_path);
}

static jobject create_text_texture(JArgs *args) {
  const jint width = jargs_int(args);
  const jint height = jargs_int(args);
  jobject strings_array = jargs_object(args);
  jobject positions_array = jargs_object(args);
  jobject widths_array = jargs_object(args);
  const jfloat scale = jargs_float(args);
  (void)widths_array;

  jobject bitmap = jni_new_bitmap(width, height);
  JBitmap *data = jni_bitmap(bitmap);
  if (!data) return bitmap;

  const int count = jni_array_length(strings_array);
  if (count <= 0) return bitmap;

  /* The positions array holds an (x, baseline_y) pair per string. */
  const float *positions = jni_float_array_data(positions_array);
  if (!positions) return bitmap;

  /* Kept on the stack: text textures can be built from more than one thread. */
  const char *strings[128];
  const int usable = count < (int)(sizeof strings / sizeof strings[0])
                         ? count
                         : (int)(sizeof strings / sizeof strings[0]);
  for (int i = 0; i < usable; i++) strings[i] = jni_object_array_utf8(strings_array, i);

  text_render(data->pixels, data->width, data->height, strings, positions,
              usable, scale);
  return bitmap;
}

static jobject get_text_bounds(JArgs *args) {
  jobject text_object = jargs_object(args);
  const char *text = jni_string_utf8((jstring)text_object);
  float bounds[5];
  text_measure(text, bounds);
  return jni_new_float_array(bounds, 5);
}

jobject java_call_object(int id, JArgs *args) {
  switch (id) {
    case METHOD_CLIPBOARD_GET:
      return jni_new_string(g_clipboard);
    case METHOD_GET_EXEC_ARG:
      return jni_new_string("");
    case METHOD_GET_BA_LOCALE:
      return jni_new_string("English");
    case METHOD_GET_DEVICE_NAME:
      return jni_new_string(port_config()->device_name);
    case METHOD_GET_DEVICE_SIZE:
      /* Drives the engine's default UI scale; the BA_UI_SCALE environment
       * variable set at startup takes precedence over it. */
      return jni_new_string(port_config_ui_scale_name());
    case METHOD_GET_DEVICE_UUID: {
      /* Stable per console, derived from the device's own serial. */
      static char uuid[64];
      if (!uuid[0]) {
        SetSysSerialNumber serial;
        memset(&serial, 0, sizeof serial);
        setsysGetSerialNumber(&serial);
        snprintf(uuid, sizeof uuid, "BSNX-%s",
                 serial.number[0] ? serial.number : "UNKNOWN");
      }
      return jni_new_string(uuid);
    }
    case METHOD_GET_EXTERNAL_FILES_DIR:
      return directory_string(EXTERNAL_PATH, EXTERNAL_PATH_UNIX);
    case METHOD_GET_FILES_DIR:
      return directory_string(FILES_PATH, FILES_PATH_UNIX);
    case METHOD_GET_NO_BACKUP_FILES_DIR:
      return directory_string(NO_BACKUP_PATH, NO_BACKUP_PATH_UNIX);
    case METHOD_GET_LOCALE_TAG:
      return jni_new_string("en_US");
    case METHOD_GET_SYSTEM_VERSION:
      return jni_new_string("Nintendo Switch");
    case METHOD_GET_PACKAGE_NAME:
      return jni_new_string("net.froemling.bombsquad");
    case METHOD_CREATE_TEXT_TEXTURE:
      return create_text_texture(args);
    case METHOD_GET_TEXT_BOUNDS:
      return get_text_bounds(args);
    default:
      return NULL;
  }
}

jboolean java_call_boolean(int id, JArgs *args) {
  (void)args;
  switch (id) {
    case METHOD_GET_HAS_TOUCH_SCREEN:
      /* Reported so the engine keeps its touch paths alive for handheld;
       * controllers remain the primary input. */
      return JNI_TRUE;
    case METHOD_HAVE_PERMISSION:
      return JNI_TRUE;
    case METHOD_CLIPBOARD_HAS:
      return g_clipboard[0] ? JNI_TRUE : JNI_FALSE;
    case METHOD_IS_ON_TV:
      return appletGetOperationMode() == AppletOperationMode_Console ? JNI_TRUE
                                                                    : JNI_FALSE;
    case METHOD_IS_DAYDREAM:
    case METHOD_IS_DESKTOP:
    case METHOD_IS_FIRE_TV:
    default:
      return JNI_FALSE;
  }
}

jint java_call_int(int id, JArgs *args) {
  (void)id;
  (void)args;
  return 0;
}
jlong java_call_long(int id, JArgs *args) {
  (void)id;
  (void)args;
  return 0;
}
jfloat java_call_float(int id, JArgs *args) {
  (void)id;
  (void)args;
  return 0.0f;
}
jdouble java_call_double(int id, JArgs *args) {
  (void)id;
  (void)args;
  return 0.0;
}

void java_call_void(int id, JArgs *args) {
  switch (id) {
    case METHOD_INIT_COMPLETE:
      __atomic_store_n(&g_init_complete, 1, __ATOMIC_RELEASE);
      trace("engine reported init complete");
      break;
    case METHOD_REQUEST_INIT_CYCLE:
      bridge_request_init_cycle();
      break;
    case METHOD_START_NETWORK_MONITOR:
      __atomic_store_n(&g_network_monitor_started, 1, __ATOMIC_RELEASE);
      trace("engine asked for network availability monitoring");
      /* Android answers this synchronously with the current state before
       * registering for later changes. */
      guest_net_avail_changed(true);
      break;
    case METHOD_QUIT:
      __atomic_store_n(&g_quit_requested, 1, __ATOMIC_RELEASE);
      trace("engine requested quit");
      break;
    case METHOD_CLIPBOARD_SET: {
      const char *text = jni_string_utf8((jstring)jargs_object(args));
      snprintf(g_clipboard, sizeof g_clipboard, "%s", text ? text : "");
      break;
    }
    case METHOD_OPEN_URL: {
      const char *url = jni_string_utf8((jstring)jargs_object(args));
      /* Nothing on the console can open a browser, so surface it in the log
       * where the player can find it. */
      trace("engine wanted to open %s", url ? url : "");
      break;
    }
    case METHOD_INVOKE_STRING_EDITOR:
      /* HaveStringEditor is patched to false so the engine uses its own
       * controller-driven on-screen keyboard; this should never fire. */
      trace("unexpected platform string editor request");
      break;
    case METHOD_MISC_COMMAND_2: {
      const char *command = jni_string_utf8((jstring)jargs_object(args));
      const char *value = jni_string_utf8((jstring)jargs_object(args));
      /* The engine's own network setup rides on this: it applies the app
       * config from its logic thread by scheduling a main-thread call, and
       * that call is what opens the UDP listener. Dropping these leaves the
       * game unable to receive anything. */
      if (command && !strcmp(command, "MAIN_THREAD_CALL")) {
        bridge_queue_main_thread_call(value);
      } else if (command && !strcmp(command, "SET_RES")) {
        /* Android would resize the surface and report back. The surface here
         * is fixed by the display mode, so the engine is told the size it
         * actually has rather than being left waiting. */
        trace("engine asked for %s; re-reporting the current surface",
              value ? value : "?");
        bridge_request_surface_refresh();
      } else {
        if (command && !strcmp(command, "SET_ANALYTICS_SCREEN"))
          ui_state_note_screen(value);
        trace("android command2: %s %s", command ? command : "",
              value ? value : "");
      }
      break;
    }
    case METHOD_MISC_COMMAND:
    case METHOD_MISC_COMMAND_3:
    case METHOD_MISC_COMMAND_ARRAY:
    case METHOD_MISC_COMMAND_BUFFER: {
      const char *command = jni_string_utf8((jstring)jargs_object(args));
      trace("android command: %s", command ? command : "");
      break;
    }
    default:
      break;
  }
}

/* Static Java fields the engine reads through JNI. */
static char g_window_service[] = "window";

jobject java_get_object_field(int id) {
  if (id == FIELD_WINDOW_SERVICE) return jni_new_string(g_window_service);
  return NULL;
}

jint java_get_int_field(int id) {
  /* Reported as Android 12; high enough that no legacy code path engages. */
  if (id == FIELD_SDK_INT) return 31;
  return 0;
}

/* ------------------------------------------------------ android bitmaps */

#define ANDROID_BITMAP_RESULT_SUCCESS 0
#define ANDROID_BITMAP_RESULT_BAD_PARAMETER (-1)
#define ANDROID_BITMAP_FORMAT_RGBA_8888 1

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  int32_t format;
  uint32_t flags;
} AndroidBitmapInfo;

int AndroidBitmap_getInfo(void *env, jobject bitmap, AndroidBitmapInfo *info) {
  (void)env;
  JBitmap *data = jni_bitmap(bitmap);
  if (!data || !info) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  info->width = (uint32_t)data->width;
  info->height = (uint32_t)data->height;
  info->stride = (uint32_t)data->stride;
  info->format = ANDROID_BITMAP_FORMAT_RGBA_8888;
  info->flags = 0;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

int AndroidBitmap_lockPixels(void *env, jobject bitmap, void **pixels) {
  (void)env;
  JBitmap *data = jni_bitmap(bitmap);
  if (!data || !pixels) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  *pixels = data->pixels;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

int AndroidBitmap_unlockPixels(void *env, jobject bitmap) {
  (void)env;
  return jni_bitmap(bitmap) ? ANDROID_BITMAP_RESULT_SUCCESS
                            : ANDROID_BITMAP_RESULT_BAD_PARAMETER;
}
