/* Leftover libc surface. */

#define _GNU_SOURCE

#include <errno.h>
#include <malloc.h>
#include <poll.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "errno_shim.h"
#include "error.h"
#include "gl_shim.h"
#include "imports.h"
#include "io_shim.h"
#include "opensles.h"
#include "shims.h"
#include "sys_shim.h"
#include "so_util.h"

/* ------------------------------------------------------------------ stdio */

FILE *bionic_stdin;
FILE *bionic_stdout;
FILE *bionic_stderr;
char *bionic_tzname[2];

static int g_h_errno;
int *bionic_h_errno_location(void) { return &g_h_errno; }

void shims_init(void) {
  bionic_stdin = (FILE *)bionic_sF[0];
  bionic_stdout = (FILE *)bionic_sF[1];
  bionic_stderr = (FILE *)bionic_sF[2];
  /* Filled in properly once sys_shim_init has read the console's clocks. */
  bionic_tzname[0] = tzname[0] ? tzname[0] : "UTC";
  bionic_tzname[1] = tzname[1] ? tzname[1] : "UTC";
}

void shims_sync_timezone(void) {
  bionic_tzname[0] = tzname[0] ? tzname[0] : "UTC";
  bionic_tzname[1] = tzname[1] ? tzname[1] : "UTC";
}

#define HOST(stream) host_stream(stream)

/* The guest's stdout and stderr share the trace file with the port's own
 * log lines, but they were reaching it through newlib stdio directly while
 * trace() wrote under a mutex. Two writers on one FILE lose and interleave
 * output -- which is why log lines have been turning up truncated. Routing
 * them through trace_raw puts every writer behind the same lock. */
static int to_log(FILE *stream) {
  FILE *host = host_stream(stream);
  return host != NULL && host == trace_stream();
}

int shim_fclose(FILE *stream) {
  FILE *host = HOST(stream);
  /* Never close the process's own standard streams on the guest's behalf. */
  if (host == stdin || host == stdout || host == stderr) return 0;
  return fclose(host);
}
size_t shim_fread(void *ptr, size_t size, size_t count, FILE *stream) {
  return fread(ptr, size, count, HOST(stream));
}
size_t shim_fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
  if (to_log(stream)) {
    trace_raw((const char *)ptr, size * count);
    return count;
  }
  return fwrite(ptr, size, count, HOST(stream));
}
int shim_fflush(FILE *stream) {
  if (stream && to_log(stream)) return 0; /* trace_raw flushes on its own */
  return fflush(stream ? HOST(stream) : NULL);
}
int shim_fseek(FILE *stream, long offset, int whence) {
  return fseek(HOST(stream), offset, whence);
}
int shim_fseeko(FILE *stream, off_t offset, int whence) {
  return fseeko(HOST(stream), offset, whence);
}
long shim_ftell(FILE *stream) { return ftell(HOST(stream)); }
off_t shim_ftello(FILE *stream) { return ftello(HOST(stream)); }
int shim_feof(FILE *stream) { return feof(HOST(stream)); }
int shim_ferror(FILE *stream) { return ferror(HOST(stream)); }
void shim_clearerr(FILE *stream) { clearerr(HOST(stream)); }
int shim_fileno(FILE *stream) { return fileno(HOST(stream)); }
int shim_fgetc(FILE *stream) { return fgetc(HOST(stream)); }
char *shim_fgets(char *buf, int size, FILE *stream) {
  return fgets(buf, size, HOST(stream));
}
int shim_fputc(int c, FILE *stream) {
  if (to_log(stream)) {
    const char byte = (char)c;
    trace_raw(&byte, 1);
    return c;
  }
  return fputc(c, HOST(stream));
}
int shim_fputs(const char *text, FILE *stream) {
  if (to_log(stream)) {
    if (text) trace_raw(text, strlen(text));
    return 0;
  }
  return fputs(text, HOST(stream));
}
wint_t shim_fputwc(wchar_t c, FILE *stream) { return fputwc(c, HOST(stream)); }
int shim_getc(FILE *stream) { return getc(HOST(stream)); }
wint_t shim_getwc(FILE *stream) { return getwc(HOST(stream)); }
int shim_ungetc(int c, FILE *stream) { return ungetc(c, HOST(stream)); }
wint_t shim_ungetwc(wint_t c, FILE *stream) { return ungetwc(c, HOST(stream)); }
void shim_rewind(FILE *stream) { rewind(HOST(stream)); }
void shim_setbuf(FILE *stream, char *buf) { setbuf(HOST(stream), buf); }
int shim_setvbuf(FILE *stream, char *buf, int mode, size_t size) {
  return setvbuf(HOST(stream), buf, mode, size);
}
int shim_vfprintf(FILE *stream, const char *fmt, va_list args) {
  if (to_log(stream)) {
    char line[1024];
    const int length = vsnprintf(line, sizeof line, fmt, args);
    if (length > 0)
      trace_raw(line, (size_t)length < sizeof line ? (size_t)length
                                                   : sizeof line - 1);
    return length;
  }
  return vfprintf(HOST(stream), fmt, args);
}
int shim_fprintf(FILE *stream, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int result = shim_vfprintf(stream, fmt, args);
  va_end(args);
  return result;
}
int shim_fscanf(FILE *stream, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int result = vfscanf(HOST(stream), fmt, args);
  va_end(args);
  return result;
}
void shim_perror(const char *message) {
  trace("%s: %s", message ? message : "error", strerror(errno));
}
/* The guest's unqualified stdio must not reach the host's stdout. libnx
 * points that at the text console, which is torn down once the renderer
 * takes the framebuffer -- writing to it afterwards faults inside
 * consolePrintChar. Sending it to the log is both safe and more useful. */
int shim_printf(const char *fmt, ...) {
  char line[1024];
  va_list args;
  va_start(args, fmt);
  const int length = vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  if (length > 0) trace_raw(line, (size_t)length < sizeof line ? (size_t)length
                                                              : sizeof line - 1);
  return length;
}

int shim_puts(const char *text) {
  if (text) trace_raw(text, strlen(text));
  trace_raw("\n", 1);
  return 0;
}

int shim_putchar(int c) {
  const char byte = (char)c;
  trace_raw(&byte, 1);
  return c;
}

int shim_dprintf(int fd, const char *fmt, ...) {
  char line[1024];
  va_list args;
  va_start(args, fmt);
  const int length = vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  if (length <= 0) return length;
  const size_t count =
      (size_t)length < sizeof line ? (size_t)length : sizeof line - 1;
  return (int)bionic_write(fd, line, count);
}

void shim_flockfile(FILE *stream) { (void)stream; }
void shim_funlockfile(FILE *stream) { (void)stream; }

/* -------------------------------------------------------- dynamic linking */

/* Handles are opaque tokens; dlsym searches the loaded module, the port's own
 * import table and the OpenSL ES shim in turn. */
#define DL_HANDLE_GENERIC ((void *)0x1)
#define DL_HANDLE_OPENSLES ((void *)0x2)

void *shim_dlopen(const char *name, int flags) {
  (void)flags;
  if (!name) return DL_HANDLE_GENERIC;
  if (strstr(name, "libOpenSLES")) return DL_HANDLE_OPENSLES;
  /* Oboe prefers AAudio when it can load it; reporting the library as absent
   * routes it to the OpenSL ES backend this port implements. */
  if (strstr(name, "libaaudio")) return NULL;
  return DL_HANDLE_GENERIC;
}

int shim_dlclose(void *handle) {
  (void)handle;
  return 0;
}
const char *shim_dlerror(void) { return NULL; }

void *shim_dlsym(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) return NULL;

  void *found = opensles_lookup(symbol);
  if (found) return found;

  found = so_resolve_external(symbol);
  if (found) return found;

  const uintptr_t shim = dynlib_find_export(symbol);
  if (shim) return (void *)shim;

  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3))
    return gl_shim_eglGetProcAddress(symbol);

  return NULL;
}

typedef struct {
  const char *dli_fname;
  void *dli_fbase;
  const char *dli_sname;
  void *dli_saddr;
} ShimDlInfo;

int shim_dladdr(const void *address, void *info) {
  ShimDlInfo *out = info;
  if (!out) return 0;
  out->dli_fname = "libmain.so";
  out->dli_fbase = NULL;
  out->dli_sname = NULL;
  out->dli_saddr = (void *)address;
  return 1;
}

/* ----------------------------------------------------- fortified variants */

/* The guest was compiled with _FORTIFY_SOURCE; these carry an extra
 * destination-size argument that only matters when it would have trapped. */
void *shim_memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  if (n > dst_len) fatal_error("Detected a buffer overflow in memcpy.");
  return memcpy(dst, src, n);
}
char *shim_strcpy_chk(char *dst, const char *src, size_t dst_len) {
  if (strlen(src) + 1 > dst_len) fatal_error("Detected a buffer overflow in strcpy.");
  return strcpy(dst, src);
}
char *shim_strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len) {
  if (n > dst_len) fatal_error("Detected a buffer overflow in strncpy.");
  return strncpy(dst, src, n);
}
size_t shim_strlen_chk(const char *s, size_t s_len) {
  (void)s_len;
  return strlen(s);
}
char *shim_strchr_chk(const char *s, int c, size_t s_len) {
  (void)s_len;
  return strchr(s, c);
}
int shim_vsnprintf_chk(char *s, size_t n, int flag, size_t s_len, const char *fmt,
                       va_list args) {
  (void)flag;
  if (n > s_len) n = s_len;
  return vsnprintf(s, n, fmt, args);
}
int shim_vsprintf_chk(char *s, int flag, size_t s_len, const char *fmt,
                      va_list args) {
  (void)flag;
  return vsnprintf(s, s_len, fmt, args);
}
long shim_read_chk(int fd, void *buf, size_t count, size_t buf_len) {
  if (count > buf_len) count = buf_len;
  return bionic_read(fd, buf, count);
}
long shim_write_chk(int fd, const void *buf, size_t count, size_t buf_len) {
  if (count > buf_len) count = buf_len;
  return bionic_write(fd, buf, count);
}
int shim_poll_chk(void *fds, unsigned n, int timeout, size_t fds_len) {
  (void)fds_len;
  return poll((struct pollfd *)fds, n, timeout);
}
void shim_fd_set_chk(int fd, void *set, size_t set_len) {
  (void)set_len;
  if (fd >= 0 && fd < FD_SETSIZE) FD_SET(fd, (fd_set *)set);
}
int shim_fd_isset_chk(int fd, const void *set, size_t set_len) {
  (void)set_len;
  if (fd < 0 || fd >= FD_SETSIZE) return 0;
  return FD_ISSET(fd, (fd_set *)set);
}

/* ------------------------------------------------------------------- misc */

char *shim_basename(char *path) {
  if (!path || !*path) return (char *)".";
  char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

int shim_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                         struct timespec *remain) {
  if (!request) return EINVAL;
  const int host_clock = clock_id_to_host((int)clock);
  struct timespec relative = *request;
  /* TIMER_ABSTIME */
  if (flags & 1) {
    struct timespec now;
    if (clock_gettime(host_clock, &now) != 0) clock_gettime(CLOCK_MONOTONIC, &now);
    relative.tv_sec = request->tv_sec - now.tv_sec;
    relative.tv_nsec = request->tv_nsec - now.tv_nsec;
    if (relative.tv_nsec < 0) {
      relative.tv_nsec += 1000000000L;
      relative.tv_sec--;
    }
    if (relative.tv_sec < 0) return 0;
  }
  return nanosleep(&relative, remain) == 0 ? 0 : errno;
}

int shim_posix_memalign(void **out, size_t alignment, size_t size) {
  if (!out) return EINVAL;
  if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
  void *block = memalign(alignment, size);
  if (!block) return ENOMEM;
  *out = block;
  return 0;
}

/* strerror is fed guest (bionic) error numbers. */
char *shim_strerror(int code) { return strerror(errno_bionic_to_host(code)); }

int shim_strerror_r(int code, char *buf, size_t len) {
  const char *text = strerror(errno_bionic_to_host(code));
  if (!buf || len == 0) return ERANGE;
  snprintf(buf, len, "%s", text);
  return 0;
}

char *shim_gnu_strerror_r(int code, char *buf, size_t len) {
  shim_strerror_r(code, buf, len);
  return buf;
}

void shim_siglongjmp(void *env, int value) {
  /* The matching sigsetjmp is not among the guest's imports, so the buffer
   * was never filled by this libc. Jumping into it would land anywhere. */
  (void)env;
  fatal_error("The game used siglongjmp, which this port cannot honour "
              "(value %d).", value);
}

/* Control-message walking, over bionic's own msghdr/cmsghdr layout rather
 * than whatever the host headers happen to expose. */
typedef struct {
  void *msg_name;
  unsigned int msg_namelen;
  void *msg_iov;
  size_t msg_iovlen;
  void *msg_control;
  size_t msg_controllen;
  int msg_flags;
} BionicMsghdr;

typedef struct {
  size_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
} BionicCmsghdr;

#define BIONIC_CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))

void *shim_cmsg_nxthdr(void *message, void *control) {
  BionicMsghdr *msg = message;
  BionicCmsghdr *cmsg = control;
  if (!msg || !cmsg || !msg->msg_control) return NULL;
  if (cmsg->cmsg_len < sizeof *cmsg) return NULL;

  BionicCmsghdr *next =
      (BionicCmsghdr *)((unsigned char *)cmsg + BIONIC_CMSG_ALIGN(cmsg->cmsg_len));
  unsigned char *end = (unsigned char *)msg->msg_control + msg->msg_controllen;
  if ((unsigned char *)(next + 1) > end ||
      (unsigned char *)next + BIONIC_CMSG_ALIGN(next->cmsg_len) > end) {
    return NULL;
  }
  return next;
}

/* The console reserves the fourth core for the system, so applications see
 * three. os.process_cpu_count() reaches this through sched_getaffinity and
 * Ballistica sizes its thread pool from the answer. */
#define USABLE_CORES 3

int shim_sched_getaffinity(int pid, size_t set_size, void *mask) {
  (void)pid;
  if (!mask || set_size == 0) { errno = EINVAL; return -1; }
  memset(mask, 0, set_size);

  unsigned char *bytes = mask;
  for (int cpu = 0; cpu < USABLE_CORES; cpu++) {
    const size_t index = (size_t)cpu / 8;
    if (index >= set_size) break;
    bytes[index] |= (unsigned char)(1u << (cpu % 8));
  }
  return 0;
}

int shim_sched_setaffinity(int pid, size_t set_size, const void *mask) {
  (void)pid;
  (void)set_size;
  (void)mask;
  /* Accepted and ignored: the console schedules threads for us. */
  return 0;
}

/* CPU sets are addressed in 64-bit words, so the allocation has to be
 * rounded the same way CPU_ALLOC_SIZE rounds it. */
void *shim_sched_cpualloc(size_t count) {
  return calloc(((count + 63) / 64) * 8, 1);
}
void shim_sched_cpufree(void *set) { free(set); }
int shim_sched_cpucount(size_t size, const void *set) {
  const unsigned char *bytes = set;
  int total = 0;
  for (size_t i = 0; i < size; i++)
    for (int bit = 0; bit < 8; bit++)
      if (bytes[i] & (1u << bit)) total++;
  return total;
}

/* Android has working resource limits and usage counters, so answering
 * plausibly is closer to the truth than failing. */
#define BIONIC_RLIM_INFINITY 0xFFFFFFFFFFFFFFFFull
#define BIONIC_RLIMIT_NOFILE 7

typedef struct {
  uint64_t rlim_cur;
  uint64_t rlim_max;
} BionicRlimit;

int shim_getrlimit(int resource, void *limit) {
  BionicRlimit *out = limit;
  if (!out) { errno = EFAULT; return -1; }
  if (resource == BIONIC_RLIMIT_NOFILE) {
    out->rlim_cur = 256;
    out->rlim_max = 256;
  } else {
    out->rlim_cur = BIONIC_RLIM_INFINITY;
    out->rlim_max = BIONIC_RLIM_INFINITY;
  }
  return 0;
}

int shim_setrlimit(int resource, const void *limit) {
  (void)resource;
  (void)limit;
  return 0;
}

/* Linux arm64 struct rusage: two timevals then fourteen longs. */
typedef struct {
  struct timeval ru_utime;
  struct timeval ru_stime;
  long ru_fields[14];
} BionicRusage;

int shim_getrusage(int who, void *usage) {
  (void)who;
  BionicRusage *out = usage;
  if (!out) { errno = EFAULT; return -1; }
  memset(out, 0, sizeof *out);

  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
    out->ru_utime.tv_sec = now.tv_sec;
    out->ru_utime.tv_usec = now.tv_nsec / 1000;
  }
  return 0;
}

int shim_sigrtmin(void) { return 32; }
int shim_sigrtmax(void) { return 64; }

/* Static destructors never run: the process ends with the console session. */
int shim_cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
  (void)fn;
  (void)arg;
  (void)dso;
  return 0;
}
void shim_cxa_finalize(void *dso) { (void)dso; }
int shim_cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso) {
  (void)fn;
  (void)arg;
  (void)dso;
  return 0;
}
int shim_register_atfork(void (*prepare)(void), void (*parent)(void),
                         void (*child)(void), void *dso) {
  (void)prepare;
  (void)parent;
  (void)child;
  (void)dso;
  return 0;
}

typedef struct {
  void *iov_base;
  size_t iov_len;
} ShimIovec;

long shim_readv(int fd, const void *iov_in, int count) {
  const ShimIovec *iov = iov_in;
  long total = 0;
  for (int i = 0; i < count; i++) {
    const long got = bionic_read(fd, iov[i].iov_base, iov[i].iov_len);
    if (got < 0) return total ? total : -1;
    total += got;
    if ((size_t)got < iov[i].iov_len) break;
  }
  return total;
}

long shim_writev(int fd, const void *iov_in, int count) {
  const ShimIovec *iov = iov_in;
  long total = 0;
  for (int i = 0; i < count; i++) {
    const long put = bionic_write(fd, iov[i].iov_base, iov[i].iov_len);
    if (put < 0) return total ? total : -1;
    total += put;
    if ((size_t)put < iov[i].iov_len) break;
  }
  return total;
}

long shim_preadv(int fd, const void *iov_in, int count, off_t offset) {
  const ShimIovec *iov = iov_in;
  long total = 0;
  for (int i = 0; i < count; i++) {
    const long got = bionic_pread(fd, iov[i].iov_base, iov[i].iov_len, offset + total);
    if (got < 0) return total ? total : -1;
    total += got;
    if ((size_t)got < iov[i].iov_len) break;
  }
  return total;
}

long shim_pwritev(int fd, const void *iov_in, int count, off_t offset) {
  const ShimIovec *iov = iov_in;
  long total = 0;
  for (int i = 0; i < count; i++) {
    const long put =
        bionic_pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset + total);
    if (put < 0) return total ? total : -1;
    total += put;
    if ((size_t)put < iov[i].iov_len) break;
  }
  return total;
}

int shim_sched_yield(void) {
  struct timespec zero = {0, 0};
  nanosleep(&zero, NULL);
  return 0;
}

int shim_getpid(void) { return 1; }

typedef struct {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
} BionicUtsname;

int shim_uname(void *out) {
  BionicUtsname *name = out;
  if (!name) return -1;
  memset(name, 0, sizeof *name);
  snprintf(name->sysname, sizeof name->sysname, "Horizon");
  snprintf(name->nodename, sizeof name->nodename, "switch");
  snprintf(name->release, sizeof name->release, "1.0");
  snprintf(name->version, sizeof name->version, "bombsquad_nx");
  snprintf(name->machine, sizeof name->machine, "aarch64");
  return 0;
}

size_t shim_ctype_get_mb_cur_max(void) { return 4; /* UTF-8 */ }

long shim_pathconf(const char *path, int name) {
  (void)path;
  /* _PC_NAME_MAX on bionic */
  if (name == 3) return 255;
  /* _PC_PATH_MAX */
  if (name == 4) return 1024;
  return -1;
}
long shim_pathconf_fd(int fd, int name) {
  (void)fd;
  return shim_pathconf(NULL, name);
}

/* ------------------------------------------- locale-suffixed duplicates */

#define LOCALE_FORWARD_1(name, call, type, ret)          \
  ret shim_##name(type c, void *locale) {                \
    (void)locale;                                        \
    return call(c);                                      \
  }

LOCALE_FORWARD_1(iswalpha_l, iswalpha, wint_t, int)
LOCALE_FORWARD_1(iswblank_l, iswblank, wint_t, int)
LOCALE_FORWARD_1(iswcntrl_l, iswcntrl, wint_t, int)
LOCALE_FORWARD_1(iswdigit_l, iswdigit, wint_t, int)
LOCALE_FORWARD_1(iswlower_l, iswlower, wint_t, int)
LOCALE_FORWARD_1(iswprint_l, iswprint, wint_t, int)
LOCALE_FORWARD_1(iswpunct_l, iswpunct, wint_t, int)
LOCALE_FORWARD_1(iswspace_l, iswspace, wint_t, int)
LOCALE_FORWARD_1(iswupper_l, iswupper, wint_t, int)
LOCALE_FORWARD_1(iswxdigit_l, iswxdigit, wint_t, int)
LOCALE_FORWARD_1(towlower_l, towlower, wint_t, wint_t)
LOCALE_FORWARD_1(towupper_l, towupper, wint_t, wint_t)

#undef LOCALE_FORWARD_1

int shim_strcoll_l(const char *a, const char *b, void *locale) {
  (void)locale;
  return strcoll(a, b);
}
size_t shim_strxfrm_l(char *dst, const char *src, size_t n, void *locale) {
  (void)locale;
  return strxfrm(dst, src, n);
}
size_t shim_strftime_l(char *out, size_t max, const char *fmt,
                       const struct tm *tm, void *locale) {
  (void)locale;
  return strftime(out, max, fmt, tm);
}
long double shim_strtold_l(const char *s, char **end, void *locale) {
  (void)locale;
  return strtold(s, end);
}
long long shim_strtoll_l(const char *s, char **end, int base, void *locale) {
  (void)locale;
  return strtoll(s, end, base);
}
unsigned long long shim_strtoull_l(const char *s, char **end, int base,
                                   void *locale) {
  (void)locale;
  return strtoull(s, end, base);
}
int shim_wcscoll_l(const wchar_t *a, const wchar_t *b, void *locale) {
  (void)locale;
  return wcscoll(a, b);
}
size_t shim_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, void *locale) {
  (void)locale;
  return wcsxfrm(dst, src, n);
}
