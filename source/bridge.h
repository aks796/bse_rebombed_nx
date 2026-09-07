/* Startup handshake and lifecycle signals raised by the Java bridge. */

#ifndef BSNX_BRIDGE_H
#define BSNX_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

/* The engine drives its own startup: it asks for init cycles until it
 * reports completion. */
void bridge_request_init_cycle(void);
bool bridge_take_init_cycle_request(void);
bool bridge_init_complete(void);

/* The engine schedules work for its main thread by handing Java a call id
 * and expecting it back on the UI thread. Nothing here is Java, so the ids
 * are queued and replayed from the lifecycle thread. */
void bridge_queue_main_thread_call(const char *id);
/* Blocks until a call is queued or the timeout expires, so the pump never
 * spins against the render thread for the core they share. */
bool bridge_wait_main_thread_call(char *out, size_t size, unsigned timeout_ms);

/* The engine asks for a render resolution and expects the surface size back;
 * true once when that acknowledgement is due. */
void bridge_request_surface_refresh(void);
bool bridge_take_surface_refresh(void);

bool bridge_quit_requested(void);
bool bridge_network_monitor_started(void);

#endif
