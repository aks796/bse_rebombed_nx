/* Fatal errors, the trace log, and start-up progress.
 *
 * Based on the MIT-licensed so-loader boilerplate by fgsfds and Andy Nguyen.
 */

#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "error.h"

static int screen_taken;
static int console_open;
static Mutex trace_lock;
static int trace_lock_ready;
/* Held open so the guest's stdout and stderr can be handed the same file the
 * port's own trace lines go to. */
static FILE *trace_file;
static u64 last_flush;

/* Often enough that the tail of the log is never far behind a crash, rare
 * enough that no thread is waiting on the card. */
#define TRACE_FLUSHES_PER_SECOND 4

static void wait_for_a(void) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_A) break;
    consoleUpdate(NULL);
  }
}

/* Ordinary start-up progress is written to the log and nothing is drawn:
 * a line of terminal output is not what anyone wants to see when they launch
 * a game.
 *
 * Unpacking an APK is the exception. It runs for minutes on a first launch,
 * and minutes of black screen is indistinguishable from a hang, so that one
 * job opens the console for as long as it lasts. */
void startup_screen_open(void) {
  if (!console_open) {
    consoleInit(NULL);
    console_open = 1;
  }
}

void startup_screen_close(void) {
  if (!console_open) return;
  consoleExit(NULL);
  console_open = 0;
}

void startup_status_begin(const char *message) { startup_status_update(message); }

void startup_status_update(const char *message) {
  trace("startup: %s", message);
  if (!console_open) return;
  printf("\x1b[2J\x1b[H\n  Explodinary for Nintendo Switch\n\n  %s\n\n  Please wait...\n",
         message);
  consoleUpdate(NULL);
}

void startup_status_printf(const char *fmt, ...) {
  char message[512];
  va_list list;
  va_start(list, fmt);
  vsnprintf(message, sizeof message, fmt, list);
  va_end(list);
  startup_status_update(message);
}

void startup_status_end(void) {}

void error_screen_taken(void) { screen_taken = 1; }

static void trace_open(void) {
  if (!trace_lock_ready) { mutexInit(&trace_lock); trace_lock_ready = 1; }
  if (!trace_file) trace_file = fopen(TRACE_LOG_PATH, "a");
}

void trace_reset(void) {
  if (!trace_lock_ready) { mutexInit(&trace_lock); trace_lock_ready = 1; }
  if (trace_file) { fclose(trace_file); trace_file = NULL; }
  FILE *log = fopen(TRACE_LOG_PATH, "w");
  if (log) fclose(log);
  trace_open();
}

FILE *trace_stream(void) {
  trace_open();
  return trace_file;
}

void trace(const char *fmt, ...) {
  char line[512];
  va_list list;
  va_start(list, fmt);
  vsnprintf(line, sizeof line, fmt, list);
  va_end(list);

  mutexLock(&trace_lock);
  trace_open();
  if (trace_file) {
    const u64 now = armGetSystemTick();
    fprintf(trace_file, "[%10llu] %s\n", (unsigned long long)now, line);
    /* Not flushed per line. A flush is an SD-card write that stalls whichever
     * thread is holding this lock, and these lines are written from the frame
     * loop, the audio thread and the engine's own logging -- a single one
     * landing badly is a dropped frame. Committing a few times a second keeps
     * the log current enough to diagnose from while keeping it off the
     * critical path. */
    if (now - last_flush >= armGetSystemTickFreq() / TRACE_FLUSHES_PER_SECOND) {
      fflush(trace_file);
      last_flush = now;
    }
  }
  mutexUnlock(&trace_lock);
}

void trace_flush(void) {
  mutexLock(&trace_lock);
  if (trace_file) {
    fflush(trace_file);
    last_flush = armGetSystemTick();
  }
  mutexUnlock(&trace_lock);
}

/* The guest decides how much it logs, and it can decide badly: one broken
 * socket inside CPython's event loop produced over a thousand tracebacks a
 * second, and writing them to the SD card cost more frame time than the
 * fault that caused them. Past this much in a second the rest is dropped and
 * counted, so a storm still shows up in the log without the log being what
 * makes the game unplayable. */
#define TRACE_BYTES_PER_SECOND (64 * 1024)

static u64 window_start;
static size_t window_bytes;
static size_t window_dropped;

void trace_raw(const char *text, size_t length) {
  if (!text || !length) return;
  mutexLock(&trace_lock);
  trace_open();
  if (trace_file) {
    const u64 now = armGetSystemTick();
    const u64 frequency = armGetSystemTickFreq();

    if (now - window_start >= frequency) {
      if (window_dropped)
        fprintf(trace_file,
                "\n[trace] dropped %zu bytes of game output in the last "
                "second\n",
                window_dropped);
      window_start = now;
      window_bytes = 0;
      window_dropped = 0;
    }

    if (window_bytes >= TRACE_BYTES_PER_SECOND) {
      window_dropped += length;
    } else {
      window_bytes += length;
      fwrite(text, 1, length, trace_file);
      /* Same reasoning as trace(): the card is slow enough that flushing
       * per line is a source of dropped frames all by itself. */
      if (now - last_flush >= frequency / TRACE_FLUSHES_PER_SECOND) {
        fflush(trace_file);
        last_flush = now;
      }
    }
  }
  mutexUnlock(&trace_lock);
}

void fatal_error(const char *fmt, ...) {
  char message[1024];
  va_list list;
  va_start(list, fmt);
  vsnprintf(message, sizeof message, fmt, list);
  va_end(list);

  FILE *log = fopen(FATAL_LOG_PATH, "w");
  if (log) { fprintf(log, "%s\n", message); fclose(log); }
  trace("FATAL %s", message);
  mutexLock(&trace_lock);
  if (trace_file) { fclose(trace_file); trace_file = NULL; }
  mutexUnlock(&trace_lock);

  if (screen_taken) {
    /* The renderer holds the framebuffer, so ask the system to show this. */
    ErrorApplicationConfig config;
    if (R_SUCCEEDED(errorApplicationCreate(&config, "Explodinary stopped", message)))
      errorApplicationShow(&config);
  } else {
    startup_screen_open();
    printf("\x1b[2J\x1b[H\n  Explodinary could not start\n\n  %s\n\n  Press A to exit.\n",
           message);
    consoleUpdate(NULL);
    wait_for_a();
    startup_screen_close();
  }

  /* exit() would run libnx's teardown while the engine's threads are still
   * live, which turns one clear message into a second, unrelated fatal
   * report. The message and the logs are already written, so stop here. */
  svcExitProcess();
  __builtin_unreachable();
}
