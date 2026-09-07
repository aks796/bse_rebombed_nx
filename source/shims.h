/* Leftover libc surface: stdio over the fake __sF, fortified variants,
 * locale-suffixed duplicates, dynamic linking and a few odds and ends. */

#ifndef BSNX_SHIMS_H
#define BSNX_SHIMS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <wchar.h>

/* Data symbols the guest links against. */
extern FILE *bionic_stdin;
extern FILE *bionic_stdout;
extern FILE *bionic_stderr;
extern char *bionic_tzname[2];

int *bionic_h_errno_location(void);

void shims_init(void);

/* Re-read tzname after the console's zone has been installed. */
void shims_sync_timezone(void);

/* stdio */
int shim_fclose(FILE *stream);
size_t shim_fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t shim_fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
int shim_fflush(FILE *stream);
int shim_fseek(FILE *stream, long offset, int whence);
int shim_fseeko(FILE *stream, off_t offset, int whence);
long shim_ftell(FILE *stream);
off_t shim_ftello(FILE *stream);
int shim_feof(FILE *stream);
int shim_ferror(FILE *stream);
void shim_clearerr(FILE *stream);
int shim_fileno(FILE *stream);
int shim_fgetc(FILE *stream);
char *shim_fgets(char *buf, int size, FILE *stream);
int shim_fputc(int c, FILE *stream);
int shim_fputs(const char *text, FILE *stream);
wint_t shim_fputwc(wchar_t c, FILE *stream);
int shim_getc(FILE *stream);
wint_t shim_getwc(FILE *stream);
int shim_ungetc(int c, FILE *stream);
wint_t shim_ungetwc(wint_t c, FILE *stream);
void shim_rewind(FILE *stream);
void shim_setbuf(FILE *stream, char *buf);
int shim_setvbuf(FILE *stream, char *buf, int mode, size_t size);
int shim_fprintf(FILE *stream, const char *fmt, ...);
int shim_vfprintf(FILE *stream, const char *fmt, va_list args);
int shim_fscanf(FILE *stream, const char *fmt, ...);
void shim_perror(const char *message);
int shim_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int shim_puts(const char *text);
int shim_putchar(int c);
int shim_dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void shim_flockfile(FILE *stream);
void shim_funlockfile(FILE *stream);

/* dynamic linking */
void *shim_dlopen(const char *name, int flags);
void *shim_dlsym(void *handle, const char *symbol);
int shim_dlclose(void *handle);
const char *shim_dlerror(void);
int shim_dladdr(const void *address, void *info);

/* fortified variants */
void *shim_memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len);
char *shim_strcpy_chk(char *dst, const char *src, size_t dst_len);
char *shim_strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len);
size_t shim_strlen_chk(const char *s, size_t s_len);
char *shim_strchr_chk(const char *s, int c, size_t s_len);
int shim_vsnprintf_chk(char *s, size_t n, int flag, size_t s_len,
                       const char *fmt, va_list args);
int shim_vsprintf_chk(char *s, int flag, size_t s_len, const char *fmt,
                      va_list args);
long shim_read_chk(int fd, void *buf, size_t count, size_t buf_len);
long shim_write_chk(int fd, const void *buf, size_t count, size_t buf_len);
int shim_poll_chk(void *fds, unsigned n, int timeout, size_t fds_len);
void shim_fd_set_chk(int fd, void *set, size_t set_len);
int shim_fd_isset_chk(int fd, const void *set, size_t set_len);

/* misc */
char *shim_basename(char *path);
int shim_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                         struct timespec *remain);
int shim_posix_memalign(void **out, size_t alignment, size_t size);
char *shim_strerror(int code);
int shim_strerror_r(int code, char *buf, size_t len);
char *shim_gnu_strerror_r(int code, char *buf, size_t len);
void shim_siglongjmp(void *env, int value);
void *shim_cmsg_nxthdr(void *msg, void *cmsg);
int shim_sched_getaffinity(int pid, size_t set_size, void *mask);
int shim_sched_setaffinity(int pid, size_t set_size, const void *mask);
int shim_getrlimit(int resource, void *limit);
int shim_setrlimit(int resource, const void *limit);
int shim_getrusage(int who, void *usage);
void *shim_sched_cpualloc(size_t count);
void shim_sched_cpufree(void *set);
int shim_sched_cpucount(size_t size, const void *set);
int shim_sigrtmin(void);
int shim_sigrtmax(void);
int shim_cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void shim_cxa_finalize(void *dso);
int shim_cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso);
int shim_register_atfork(void (*prepare)(void), void (*parent)(void),
                         void (*child)(void), void *dso);
long shim_readv(int fd, const void *iov, int count);
long shim_writev(int fd, const void *iov, int count);
long shim_preadv(int fd, const void *iov, int count, off_t offset);
long shim_pwritev(int fd, const void *iov, int count, off_t offset);
int shim_sched_yield(void);
int shim_getpid(void);
int shim_uname(void *buf);
size_t shim_ctype_get_mb_cur_max(void);
long shim_pathconf(const char *path, int name);
long shim_pathconf_fd(int fd, int name);

/* locale-suffixed duplicates */
int shim_iswalpha_l(wint_t c, void *locale);
int shim_iswblank_l(wint_t c, void *locale);
int shim_iswcntrl_l(wint_t c, void *locale);
int shim_iswdigit_l(wint_t c, void *locale);
int shim_iswlower_l(wint_t c, void *locale);
int shim_iswprint_l(wint_t c, void *locale);
int shim_iswpunct_l(wint_t c, void *locale);
int shim_iswspace_l(wint_t c, void *locale);
int shim_iswupper_l(wint_t c, void *locale);
int shim_iswxdigit_l(wint_t c, void *locale);
wint_t shim_towlower_l(wint_t c, void *locale);
wint_t shim_towupper_l(wint_t c, void *locale);
int shim_strcoll_l(const char *a, const char *b, void *locale);
size_t shim_strxfrm_l(char *dst, const char *src, size_t n, void *locale);
size_t shim_strftime_l(char *out, size_t max, const char *fmt,
                       const struct tm *tm, void *locale);
long double shim_strtold_l(const char *s, char **end, void *locale);
long long shim_strtoll_l(const char *s, char **end, int base, void *locale);
unsigned long long shim_strtoull_l(const char *s, char **end, int base,
                                   void *locale);
int shim_wcscoll_l(const wchar_t *a, const wchar_t *b, void *locale);
size_t shim_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, void *locale);

#endif
