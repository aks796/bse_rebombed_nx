/* Process, time, locale and platform-identity surface for the guest. */

#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <time.h>

#include "config.h"
#include "error.h"
#include "sys_shim.h"

/* ------------------------------------------------------------ environment */

#define ENV_MAX 32

static char *g_env_slots[ENV_MAX + 1];
char **bionic_environ = g_env_slots;

static int env_index(const char *name) {
  const size_t len = strlen(name);
  for (int i = 0; g_env_slots[i]; i++) {
    if (!strncmp(g_env_slots[i], name, len) && g_env_slots[i][len] == '=') return i;
  }
  return -1;
}

char *bionic_getenv(const char *name) {
  if (!name) return NULL;
  const int index = env_index(name);
  if (index < 0) return NULL;
  return g_env_slots[index] + strlen(name) + 1;
}

int bionic_setenv(const char *name, const char *value, int overwrite) {
  if (!name || !value) { errno = EINVAL; return -1; }
  const int index = env_index(name);
  if (index >= 0 && !overwrite) return 0;

  char *entry = malloc(strlen(name) + strlen(value) + 2);
  if (!entry) { errno = ENOMEM; return -1; }
  sprintf(entry, "%s=%s", name, value);

  if (index >= 0) {
    free(g_env_slots[index]);
    g_env_slots[index] = entry;
    return 0;
  }
  for (int i = 0; i < ENV_MAX; i++) {
    if (g_env_slots[i]) continue;
    g_env_slots[i] = entry;
    g_env_slots[i + 1] = NULL;
    return 0;
  }
  free(entry);
  errno = ENOMEM;
  return -1;
}

int bionic_unsetenv(const char *name) {
  if (!name) { errno = EINVAL; return -1; }
  const int index = env_index(name);
  if (index < 0) return 0;
  free(g_env_slots[index]);
  int i = index;
  for (; g_env_slots[i + 1]; i++) g_env_slots[i] = g_env_slots[i + 1];
  g_env_slots[i] = NULL;
  return 0;
}

/* -------------------------------------------------------------- time zone */

/* The console reports local time as a second clock rather than a zone name,
 * so the offset is measured by differencing the two. */
static long g_utc_offset;
static char g_zone_name[8] = "UTC";

long console_utc_offset(void) { return g_utc_offset; }

static void apply_console_timezone(void) {
  u64 utc = 0, local = 0;
  if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &utc)) &&
      R_FAILED(timeGetCurrentTime(TimeType_NetworkSystemClock, &utc))) {
    trace("no system clock; running on UTC");
    return;
  }
  if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &local))) {
    trace("no local clock; running on UTC");
    return;
  }

  long offset = (long)((int64_t)local - (int64_t)utc);
  offset = (offset / 60) * 60; /* the service works in whole minutes */
  if (offset < -14 * 3600 || offset > 14 * 3600) {
    trace("implausible local clock offset %ld; running on UTC", offset);
    return;
  }
  g_utc_offset = offset;

  if (offset == 0) {
    setenv("TZ", "UTC0", 1);
  } else {
    /* A POSIX TZ zone name needs at least three characters, and its offset
     * is written west-positive -- the opposite sign to the one measured. */
    const long inverted = -offset;
    const long magnitude = inverted < 0 ? -inverted : inverted;
    char tz[32];
    snprintf(tz, sizeof tz, "NXT%c%ld:%02ld", inverted < 0 ? '-' : '+',
             magnitude / 3600, (magnitude % 3600) / 60);
    setenv("TZ", tz, 1);
    snprintf(g_zone_name, sizeof g_zone_name, "NXT");
  }
  tzset();
  trace("local time is UTC%+ld:%02ld", offset / 3600,
        (offset < 0 ? -offset : offset) % 3600 / 60);
}

/* ------------------------------------------------------- broken-down time */

static void tm_to_bionic(const struct tm *host, BionicTm *out, long gmtoff,
                         const char *zone) {
  out->tm_sec = host->tm_sec;
  out->tm_min = host->tm_min;
  out->tm_hour = host->tm_hour;
  out->tm_mday = host->tm_mday;
  out->tm_mon = host->tm_mon;
  out->tm_year = host->tm_year;
  out->tm_wday = host->tm_wday;
  out->tm_yday = host->tm_yday;
  out->tm_isdst = host->tm_isdst;
  out->tm_gmtoff = gmtoff;
  out->tm_zone = zone;
}

static void tm_from_bionic(const BionicTm *in, struct tm *host) {
  memset(host, 0, sizeof *host);
  host->tm_sec = in->tm_sec;
  host->tm_min = in->tm_min;
  host->tm_hour = in->tm_hour;
  host->tm_mday = in->tm_mday;
  host->tm_mon = in->tm_mon;
  host->tm_year = in->tm_year;
  host->tm_wday = in->tm_wday;
  host->tm_yday = in->tm_yday;
  host->tm_isdst = in->tm_isdst;
}

BionicTm *shim_localtime_r(const time_t *when, BionicTm *out) {
  struct tm host;
  if (!when || !out || !localtime_r(when, &host)) return NULL;
  tm_to_bionic(&host, out, g_utc_offset, g_zone_name);
  return out;
}

BionicTm *shim_gmtime_r(const time_t *when, BionicTm *out) {
  struct tm host;
  if (!when || !out || !gmtime_r(when, &host)) return NULL;
  tm_to_bionic(&host, out, 0, "UTC");
  return out;
}

BionicTm *shim_localtime(const time_t *when) {
  static BionicTm shared;
  return shim_localtime_r(when, &shared);
}

time_t shim_mktime(BionicTm *broken_down) {
  if (!broken_down) return (time_t)-1;
  struct tm host;
  tm_from_bionic(broken_down, &host);
  const time_t result = mktime(&host);
  /* mktime normalises its argument, so the guest's copy is refreshed. */
  tm_to_bionic(&host, broken_down, g_utc_offset, g_zone_name);
  return result;
}

/* ----------------------------------------------------------------- locale */

/* Same fields as newlib's, but the last six are ordered differently. */
typedef struct {
  char *decimal_point;
  char *thousands_sep;
  char *grouping;
  char *int_curr_symbol;
  char *currency_symbol;
  char *mon_decimal_point;
  char *mon_thousands_sep;
  char *mon_grouping;
  char *positive_sign;
  char *negative_sign;
  char int_frac_digits;
  char frac_digits;
  char p_cs_precedes;
  char p_sep_by_space;
  char n_cs_precedes;
  char n_sep_by_space;
  char p_sign_posn;
  char n_sign_posn;
  char int_p_cs_precedes;
  char int_p_sep_by_space;
  char int_n_cs_precedes;
  char int_n_sep_by_space;
  char int_p_sign_posn;
  char int_n_sign_posn;
} BionicLconv;

void *shim_localeconv(void) {
  static BionicLconv converted;
  const struct lconv *host = localeconv();
  if (!host) return NULL;

  converted.decimal_point = host->decimal_point;
  converted.thousands_sep = host->thousands_sep;
  converted.grouping = host->grouping;
  converted.int_curr_symbol = host->int_curr_symbol;
  converted.currency_symbol = host->currency_symbol;
  converted.mon_decimal_point = host->mon_decimal_point;
  converted.mon_thousands_sep = host->mon_thousands_sep;
  converted.mon_grouping = host->mon_grouping;
  converted.positive_sign = host->positive_sign;
  converted.negative_sign = host->negative_sign;
  converted.int_frac_digits = host->int_frac_digits;
  converted.frac_digits = host->frac_digits;
  converted.p_cs_precedes = host->p_cs_precedes;
  converted.p_sep_by_space = host->p_sep_by_space;
  converted.n_cs_precedes = host->n_cs_precedes;
  converted.n_sep_by_space = host->n_sep_by_space;
  converted.p_sign_posn = host->p_sign_posn;
  converted.n_sign_posn = host->n_sign_posn;
  converted.int_p_cs_precedes = host->int_p_cs_precedes;
  converted.int_p_sep_by_space = host->int_p_sep_by_space;
  converted.int_n_cs_precedes = host->int_n_cs_precedes;
  converted.int_n_sep_by_space = host->int_n_sep_by_space;
  converted.int_p_sign_posn = host->int_p_sign_posn;
  converted.int_n_sign_posn = host->int_n_sign_posn;
  return &converted;
}

void sys_shim_init(void) {
  g_env_slots[0] = NULL;
  bionic_setenv("HOME", FILES_PATH_UNIX, 1);
  bionic_setenv("TMPDIR", CACHE_PATH_UNIX, 1);
  bionic_setenv("LANG", "en_US.UTF-8", 1);
  bionic_setenv("ANDROID_DATA", GAME_ROOT_UNIX, 1);
  apply_console_timezone();
}

/* ----------------------------------------------------------------- clocks */

/* Linux/bionic: REALTIME 0, MONOTONIC 1, PROCESS_CPUTIME 2, THREAD_CPUTIME 3,
 * MONOTONIC_RAW 4, REALTIME_COARSE 5, MONOTONIC_COARSE 6, BOOTTIME 7.
 * newlib numbers the same clocks differently, so nothing may pass through. */
int clock_id_to_host(int bionic_clock) {
  switch (bionic_clock) {
    case 0: return CLOCK_REALTIME;
    case 1: return CLOCK_MONOTONIC;
    /* CPU-time clocks are not available here; monotonic is the honest
     * substitute and keeps profiling code running. */
    case 2:
    case 3: return CLOCK_MONOTONIC;
    case 4: return CLOCK_MONOTONIC;  /* _RAW: same source here */
    case 5: return CLOCK_REALTIME;   /* _COARSE */
    case 6: return CLOCK_MONOTONIC;  /* _COARSE */
    case 7: return CLOCK_MONOTONIC;  /* BOOTTIME */
    default: return CLOCK_MONOTONIC;
  }
}

int shim_clock_gettime(int clock, struct timespec *out) {
  return clock_gettime(clock_id_to_host(clock), out);
}

int shim_clock_getres(int clock, struct timespec *out) {
  return clock_getres(clock_id_to_host(clock), out);
}

/* -------------------------------------------------------- system identity */

int bionic_sysconf(int name) {
  switch (name) {
    case 0x0000: return 131072;  /* _SC_ARG_MAX */
    case 0x0005: return 1;       /* _SC_CHILD_MAX */
    case 0x0006: return 100;     /* _SC_CLK_TCK */
    case 0x000a: return 32;      /* _SC_NGROUPS_MAX */
    case 0x000b: return 256;     /* _SC_OPEN_MAX */
    case 0x0027: return 4096;    /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    case 0x0060:                 /* _SC_NPROCESSORS_CONF */
    case 0x0061: return 3;       /* _SC_NPROCESSORS_ONLN -- core 3 is reserved */
    case 0x0062: {               /* _SC_PHYS_PAGES */
      u64 total = 0;
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      return total ? (int)(total / 4096) : (256 * 1024);
    }
    case 0x0063: {               /* _SC_AVPHYS_PAGES */
      u64 total = 0, used = 0;
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      return (total > used) ? (int)((total - used) / 4096) : (64 * 1024);
    }
    default: return -1;
  }
}

/* Cortex-A57 features the guest's crypto and memcpy paths probe for. */
#define AT_HWCAP_VALUE 0xFBull /* FP ASIMD AES PMULL SHA1 SHA2 CRC32 */

unsigned long bionic_getauxval(unsigned long type) {
  switch (type) {
    case 6: return 4096;            /* AT_PAGESZ */
    case 16: return AT_HWCAP_VALUE; /* AT_HWCAP */
    case 23: return 0;              /* AT_SECURE */
    case 26: return 0;              /* AT_HWCAP2 */
    default: return 0;
  }
}

int bionic_system_property_get(const char *name, char *value) {
  const char *result = "";
  if (!name) { if (value) value[0] = '\0'; return 0; }

  if (!strcmp(name, "ro.build.version.sdk")) result = "31";
  else if (!strcmp(name, "ro.build.version.release")) result = "12";
  else if (!strcmp(name, "ro.product.model")) result = "Nintendo Switch";
  else if (!strcmp(name, "ro.product.manufacturer")) result = "Nintendo";
  else if (!strcmp(name, "ro.product.cpu.abi")) result = "arm64-v8a";
  else if (!strcmp(name, "ro.hardware")) result = "nx";

  const int length = (int)strlen(result);
  if (value) memcpy(value, result, length + 1);
  return length;
}

/* ---------------------------------------------------------------- entropy */

int bionic_getentropy(void *buffer, size_t length) {
  if (length > 256) { errno = EIO; return -1; }
  csrngGetRandomBytes(buffer, length);
  return 0;
}

void bionic_arc4random_buf(void *buffer, size_t length) {
  csrngGetRandomBytes(buffer, length);
}

/* --------------------------------------------------------------- syscalls */

#define SYS_gettid_arm64 178
#define SYS_getrandom_arm64 278

int bionic_gettid(void) { return (int)(uintptr_t)threadGetCurHandle(); }

long bionic_syscall(long number, ...) {
  va_list args;
  va_start(args, number);
  long result;
  switch (number) {
    case SYS_gettid_arm64:
      result = bionic_gettid();
      break;
    case SYS_getrandom_arm64: {
      void *buffer = va_arg(args, void *);
      size_t length = va_arg(args, size_t);
      csrngGetRandomBytes(buffer, length);
      result = (long)length;
      break;
    }
    default:
      errno = ENOSYS;
      result = -1;
      break;
  }
  va_end(args);
  return result;
}

/* ------------------------------------------------------- aborts and logs */

static char g_abort_message[512];

void bionic_set_abort_message(const char *message) {
  if (!message) return;
  snprintf(g_abort_message, sizeof g_abort_message, "%s", message);
  trace("abort message: %s", message);
}

void bionic_abort(void) {
  fatal_error("The game aborted.%s%s", g_abort_message[0] ? "\n\n  " : "",
              g_abort_message);
}

void bionic_assert2(const char *file, int line, const char *function,
                    const char *message) {
  fatal_error("Assertion failed in %s\n  %s:%d\n  %s",
              function ? function : "?", file ? file : "?", line,
              message ? message : "");
}

void bionic_stack_chk_fail(void) { fatal_error("Stack corruption detected."); }

int bionic_android_log_vprint(int priority, const char *tag, const char *fmt,
                              va_list args) {
  char line[768];
  vsnprintf(line, sizeof line, fmt, args);
  trace("[%d %s] %s", priority, tag ? tag : "", line);
  return 0;
}

int bionic_android_log_print(int priority, const char *tag, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int rc = bionic_android_log_vprint(priority, tag, fmt, args);
  va_end(args);
  return rc;
}

int bionic_android_log_write(int priority, const char *tag, const char *text) {
  trace("[%d %s] %s", priority, tag ? tag : "", text ? text : "");
  return 0;
}

/* ----------------------------------------------------------------- locale */

/* newlib has no per-thread locale objects. The engine and CPython only ever
 * ask for "C"/"C.UTF-8" here, so hand back a sentinel and let the *_l
 * wrappers in imports.c drop the locale argument. */
static int g_locale_sentinel;

void *bionic_newlocale(int mask, const char *name, void *base) {
  (void)mask;
  (void)name;
  (void)base;
  return &g_locale_sentinel;
}
void bionic_freelocale(void *locale) { (void)locale; }
void *bionic_uselocale(void *locale) {
  (void)locale;
  return &g_locale_sentinel;
}
