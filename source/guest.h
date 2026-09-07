/* Calls into the loaded Android library.
 *
 * Every entry point here is one of the nativeX JNI exports the Java activity
 * would normally invoke, resolved once at startup.
 */

#ifndef BSNX_GUEST_H
#define BSNX_GUEST_H

#include <stdbool.h>

#include "so_util.h"

extern so_module guest_module;

void guest_resolve_entrypoints(void);

void guest_init(void);
void guest_init_cycle(void);
void guest_set_running(bool running);
void guest_set_active(bool active);
void guest_net_avail_changed(bool available);

void guest_surface_created(void);
void guest_surface_changed(int width, int height);
void guest_draw_frame(void);

/* Returns false when the engine is not yet accepting input, in which case
 * the event was dropped and the caller must try again later. */
bool guest_input_device_event(int type, int device, int control, float value,
                              const char *name);
void guest_touch_event(int device, int pointer, int action, float x, float y);
/* Replays a command Java would have delivered on the UI thread. */
void guest_handle_command2(const char *command, const char *value);

void guest_key_down(int keycode);
void guest_key_up(int keycode);
void guest_gyro(float x, float y, float z);

/* Guarded by the init-complete handshake so events are not delivered before
 * the engine has an input subsystem to receive them. */
void guest_enable_input(void);
bool guest_input_enabled(void);

#endif
