/* Calls into the loaded Android library. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "error.h"
#include "guest.h"
#include "jni_env.h"
#include "so_util.h"

so_module guest_module;

#define NATIVE_PREFIX "Java_com_ericfroemling_ballistica_BallisticaContext_"

typedef void (*fn_void)(void *env, void *clazz);
typedef void (*fn_bool)(void *env, void *clazz, jboolean value);
typedef void (*fn_size)(void *env, void *clazz, jint width, jint height);
typedef void (*fn_key)(void *env, void *clazz, jint keycode);
typedef void (*fn_input_device)(void *env, void *clazz, jint type, jint device,
                                jint control, jfloat value, jstring name);
typedef void (*fn_touch)(void *env, void *clazz, jint device, jint pointer,
                         jint action, jfloat x, jfloat y, jfloat pressure,
                         jboolean is_mouse);
typedef void (*fn_vec3)(void *env, void *clazz, jfloat x, jfloat y, jfloat z);
typedef void (*fn_command2)(void *env, void *clazz, jstring a, jstring b);

static fn_void g_init;
static fn_void g_init_cycle;
static fn_bool g_set_running;
static fn_bool g_set_active;
static fn_bool g_net_avail_changed;
static fn_void g_surface_created;
static fn_size g_surface_changed;
static fn_void g_draw_frame;
static fn_key g_key_down;
static fn_key g_key_up;
static fn_input_device g_input_device;
static fn_touch g_touch;
static fn_vec3 g_gyro;
static fn_command2 g_handle_command2;

static volatile int g_input_enabled;

static void *entry_point(const char *suffix, char *full_name, size_t size) {
  const size_t prefix_length = sizeof NATIVE_PREFIX - 1;
  memcpy(full_name, NATIVE_PREFIX, prefix_length);
  size_t i = 0;
  while (suffix[i] && prefix_length + i + 1 < size) {
    full_name[prefix_length + i] = suffix[i];
    i++;
  }
  full_name[prefix_length + i] = '\0';
  return (void *)so_symbol(&guest_module, full_name);
}

static void *required(const char *suffix) {
  char name[160];
  void *address = entry_point(suffix, name, sizeof name);
  if (!address)
    fatal_error("This libmain.so is missing %s.\n\n  Only BombSquad "
                "Explodinary (arm64-v8a) is supported.",
                name);
  return address;
}

/* Explodinary is built from a slightly older engine than stock BombSquad and
 * does not export every entry point stock does. One that is only ever told
 * about a change it can already see for itself is not worth refusing to
 * start over, so it is looked up and left null if it is not there. */
static void *optional(const char *suffix) {
  char name[160];
  void *address = entry_point(suffix, name, sizeof name);
  if (!address) trace("%s is absent from this build; skipping it", name);
  return address;
}

void guest_resolve_entrypoints(void) {
  g_init = required("nativeInit");
  g_init_cycle = required("nativeInitCycle");
  g_set_running = required("nativeSetRunning");
  g_set_active = required("nativeSetActive");
  g_net_avail_changed = optional("nativeOnNetAvailChanged");
  g_surface_created = required("nativeOnSurfaceCreated");
  g_surface_changed = required("nativeOnSurfaceChanged");
  g_draw_frame = required("nativeOnDrawFrame");
  g_key_down = required("nativeKeyDownEvent");
  g_key_up = required("nativeKeyUpEvent");
  g_input_device = required("nativeInputDeviceEvent");
  g_touch = required("nativeTouchEvent");
  g_gyro = required("nativeGyro");
  g_handle_command2 = required("nativeHandleCommand2");
  trace("guest entry points resolved");
}

void guest_init(void) { g_init(&jni_env, jni_context_object()); }
void guest_init_cycle(void) { g_init_cycle(&jni_env, jni_context_object()); }
void guest_set_running(bool running) {
  g_set_running(&jni_env, jni_context_object(), running ? JNI_TRUE : JNI_FALSE);
}
void guest_set_active(bool active) {
  g_set_active(&jni_env, jni_context_object(), active ? JNI_TRUE : JNI_FALSE);
}
void guest_net_avail_changed(bool available) {
  if (!g_net_avail_changed) return;
  g_net_avail_changed(&jni_env, jni_context_object(), available ? JNI_TRUE : JNI_FALSE);
}
void guest_surface_created(void) { g_surface_created(&jni_env, jni_context_object()); }
void guest_surface_changed(int width, int height) {
  g_surface_changed(&jni_env, jni_context_object(), width, height);
}
void guest_draw_frame(void) { g_draw_frame(&jni_env, jni_context_object()); }

bool guest_input_device_event(int type, int device, int control, float value,
                              const char *name) {
  if (!g_input_enabled) return false;
  jstring name_object = name ? jni_new_string(name) : NULL;
  g_input_device(&jni_env, jni_context_object(), type, device, control, value,
                 name_object);
  return true;
}

void guest_touch_event(int device, int pointer, int action, float x, float y) {
  if (!g_input_enabled) return;
  g_touch(&jni_env, jni_context_object(), device, pointer, action, x, y, 1.0f,
          JNI_FALSE);
}

void guest_key_down(int keycode) {
  if (!g_input_enabled) return;
  g_key_down(&jni_env, jni_context_object(), keycode);
}
void guest_key_up(int keycode) {
  if (!g_input_enabled) return;
  g_key_up(&jni_env, jni_context_object(), keycode);
}
void guest_gyro(float x, float y, float z) {
  if (!g_input_enabled) return;
  g_gyro(&jni_env, jni_context_object(), x, y, z);
}

void guest_handle_command2(const char *command, const char *value) {
  jstring a = jni_new_string(command);
  jstring b = jni_new_string(value);
  g_handle_command2(&jni_env, jni_context_object(), a, b);
  /* These are ours, and the engine has no local frame to drop them from. */
  jni_env->DeleteLocalRef(&jni_env, a);
  jni_env->DeleteLocalRef(&jni_env, b);
}

void guest_enable_input(void) {
  __atomic_store_n(&g_input_enabled, 1, __ATOMIC_RELEASE);
  trace("guest input delivery enabled");
}
bool guest_input_enabled(void) {
  return __atomic_load_n(&g_input_enabled, __ATOMIC_ACQUIRE) != 0;
}
