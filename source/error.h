/* Fatal errors and the text-console startup progress screen. */

#ifndef BSNX_ERROR_H
#define BSNX_ERROR_H

#include <stddef.h>
#include <stdio.h>

/* Opens and closes the text console the start-up messages are painted on.
 * Only worth doing around a job long enough that silence would look like a
 * crash; everything else just goes to the log. */
void startup_screen_open(void);
void startup_screen_close(void);

void startup_status_begin(const char *message);
void startup_status_update(const char *message);
void startup_status_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void startup_status_end(void);

void fatal_error(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Once the renderer owns the default window, the text console can no longer
 * be brought up; errors go through the system error applet instead. */
void error_screen_taken(void);

/* Append one line to the on-SD trace log. Cheap enough for lifecycle
 * milestones; not for per-frame use. */
/* Commits whatever is buffered. Only worth calling before something that
 * might not come back. */
void trace_flush(void);

void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void trace_reset(void);

/* Append bytes to the log exactly as given. The guest's stdout and stderr
 * land here, which is the only way to see a Python traceback on a console
 * with no terminal. */
void trace_raw(const char *text, size_t length);

/* The log as a stream, for the guest's stdout/stderr FILE objects. */
FILE *trace_stream(void);

#endif
