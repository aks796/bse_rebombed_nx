/* Process, time, locale and platform-identity surface for the guest. */

#ifndef BSNX_SYS_SHIM_H
#define BSNX_SYS_SHIM_H

#include <stdarg.h>
#include <stddef.h>
#include <time.h>

void sys_shim_init(void);

/* The guest gets its own environment block: it is how the port sets
 * BA_UI_SCALE and friends before the engine reads them. */
char *bionic_getenv(const char *name);
int bionic_setenv(const char *name, const char *value, int overwrite);
int bionic_unsetenv(const char *name);
extern char **bionic_environ;

/* bionic's clock ids are Linux's; newlib numbers them differently, so every
 * clock id crossing the boundary has to be translated. */
int clock_id_to_host(int bionic_clock);
int shim_clock_gettime(int clock, struct timespec *out);
int shim_clock_getres(int clock, struct timespec *out);

/* bionic's struct tm carries tm_gmtoff and tm_zone after the nine standard
 * fields; newlib's stops at 36 bytes. Anything filling one of these for the
 * guest has to write the longer layout, or CPython reads stack garbage as
 * the GMT offset. */
typedef struct {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
  long tm_gmtoff;
  const char *tm_zone;
} BionicTm;

/* Seconds east of UTC, measured from the console's own clocks. */
long console_utc_offset(void);

BionicTm *shim_localtime(const time_t *when);
BionicTm *shim_localtime_r(const time_t *when, BionicTm *out);
BionicTm *shim_gmtime_r(const time_t *when, BionicTm *out);
time_t shim_mktime(BionicTm *broken_down);
void *shim_localeconv(void);

int bionic_sysconf(int name);
unsigned long bionic_getauxval(unsigned long type);
int bionic_system_property_get(const char *name, char *value);

int bionic_getentropy(void *buffer, size_t length);
void bionic_arc4random_buf(void *buffer, size_t length);

long bionic_syscall(long number, ...);
int bionic_gettid(void);

void bionic_abort(void) __attribute__((noreturn));
void bionic_assert2(const char *file, int line, const char *function,
                    const char *message) __attribute__((noreturn));
void bionic_set_abort_message(const char *message);
void bionic_stack_chk_fail(void) __attribute__((noreturn));

int bionic_android_log_print(int priority, const char *tag, const char *fmt, ...);
int bionic_android_log_write(int priority, const char *tag, const char *text);
int bionic_android_log_vprint(int priority, const char *tag, const char *fmt, va_list args);

/* Locale. Only the C locale is ever really used, so the *_l entry points
 * forward to their plain counterparts. */
void *bionic_newlocale(int mask, const char *name, void *base);
void bionic_freelocale(void *locale);
void *bionic_uselocale(void *locale);

#endif
