#!/usr/bin/env python3
"""Generate source/imports.c from the guest library's undefined symbols.

Run with the path to an arm64-v8a libmain.so; every imported symbol must be
classified below or generation fails, so a new game build cannot silently
lose a binding.
"""

import subprocess
import sys
import os

# ---------------------------------------------------------------- categories

# Passed straight through to the host libc/libm/zlib: same name, same ABI.
DIRECT = """
acos acosf acosh asin asinf asinh atan atan2 atan2f atanf atanh cbrt cbrtf
copysign cos cosf cosh erf erfc exp exp2 exp2f expf expm1 fabs fmod fmodf
frexp frexpf hypot hypotf ldexp ldexpf log log10 log10f log1p log2 log2f logf
modf nextafter pow powf sin sincos sincosf sinf sinh sinhf sqrt tan tanf tanh
trunc
memchr memcmp memcpy memmove memrchr memset strcat strchr strcmp strcpy
strcspn strdup strlen strncat strncmp strncpy strnlen strpbrk strrchr strspn
strstr strtok_r strsignal
malloc free calloc realloc qsort atoi rand srand strtod strtof strtol strtold
strtoll strtoul strtoull exit _exit
snprintf sprintf vsnprintf sscanf vsscanf vasprintf
btowc mbrlen mbrtowc mbsnrtowcs mbsrtowcs mbtowc swprintf wcrtomb wcschr wcscmp
wcscoll wcscpy wcsftime wcslen wcsncmp wcsncpy wcsnrtombs wcsrchr wcstod wcstof
wcstok wcstol wcstold wcstoll wcstombs wcstoul wcstoull wcsxfrm wctob wmemchr
wmemcmp
clock gettimeofday nanosleep sleep usleep time times
setlocale towupper
adler32 crc32 deflate deflateCopy deflateEnd deflateInit2_ deflateSetDictionary
inflate inflateCopy inflateEnd inflateInit2_ inflateSetDictionary zlibVersion
gai_strerror gethostname getprotobyname getservbyname getservbyport
hstrerror inet_aton inet_ntoa listen shutdown
kill raise signal mkstemp
"""

# GL entry points forwarded to Mesa unchanged.
GL_DIRECT = """
glActiveTexture glAttachShader glBindAttribLocation glBindBuffer
glBindFramebuffer glBindRenderbuffer glBindTexture glBindVertexArray
glBlendFunc glBlendFuncSeparate glBlitFramebuffer glBufferData
glCheckFramebufferStatus glClear glClearColor glCompileShader glCreateProgram
glCreateShader glCullFace glDeleteBuffers glDeleteFramebuffers glDeleteProgram
glDeleteRenderbuffers glDeleteShader glDeleteTextures glDeleteVertexArrays
glDepthFunc glDepthMask glDepthRangef glDetachShader glDisable glDrawArrays
glDrawElements glEnable glEnableVertexAttribArray glFramebufferRenderbuffer
glFramebufferTexture2D glGenBuffers glGenFramebuffers glGenRenderbuffers
glGenTextures glGenVertexArrays glGenerateMipmap glGetError glGetFloatv
glGetIntegerv glGetInternalformativ glGetProgramInfoLog glGetProgramiv
glGetShaderInfoLog glGetShaderiv glGetString glGetStringi glGetUniformLocation
glInvalidateFramebuffer glLinkProgram glReadPixels glRenderbufferStorage
glRenderbufferStorageMultisample glScissor glShaderSource glTexImage2D
glTexParameterf glTexParameteri glUniform1f glUniform1fv glUniform1i
glUniform2f glUniform4f glUniformMatrix4fv glUseProgram glVertexAttribPointer
glViewport
"""

# Symbols answered by one of the port's shims.
SHIM = {
    # errno
    "__errno": "bionic_errno_location",
    "__get_h_errno": "bionic_h_errno_location",
    # descriptors and files
    "open": "bionic_open", "openat": "bionic_openat", "close": "bionic_close",
    "read": "bionic_read", "write": "bionic_write",
    "pread": "bionic_pread", "pwrite": "bionic_pwrite",
    "lseek": "bionic_lseek", "fcntl": "bionic_fcntl", "ioctl": "bionic_ioctl",
    "fstat": "bionic_fstat", "fsync": "bionic_fsync",
    "ftruncate": "bionic_ftruncate", "dup": "bionic_dup", "dup2": "bionic_dup2",
    "isatty": "bionic_isatty",
    "stat": "bionic_stat", "lstat": "bionic_lstat", "fstatat": "bionic_fstatat",
    "access": "bionic_access", "mkdir": "bionic_mkdir",
    "mkdirat": "bionic_mkdirat", "rmdir": "bionic_rmdir",
    "unlink": "bionic_unlink", "unlinkat": "bionic_unlinkat",
    "remove": "bionic_remove", "rename": "bionic_rename",
    "renameat": "bionic_renameat", "chdir": "bionic_chdir",
    "getcwd": "bionic_getcwd", "realpath": "bionic_realpath",
    "readlink": "bionic_readlink", "readlinkat": "bionic_readlinkat",
    "truncate": "bionic_truncate", "chmod": "bionic_chmod",
    "statvfs": "bionic_statvfs", "fstatvfs": "bionic_fstatvfs",
    "statfs": "bionic_statfs",
    "opendir": "bionic_opendir", "fdopendir": "bionic_fdopendir",
    "readdir": "bionic_readdir", "closedir": "bionic_closedir",
    "rewinddir": "bionic_rewinddir",
    "fopen": "bionic_fopen", "fdopen": "bionic_fdopen",
    "mmap": "bionic_mmap", "munmap": "bionic_munmap",
    "mremap": "bionic_mremap", "mprotect": "bionic_mprotect",
    "msync": "bionic_msync", "madvise": "bionic_madvise",
    # stdio over the fake __sF
    "fclose": "shim_fclose", "fread": "shim_fread", "fwrite": "shim_fwrite",
    "fflush": "shim_fflush", "fseek": "shim_fseek", "fseeko": "shim_fseeko",
    "ftell": "shim_ftell", "ftello": "shim_ftello", "feof": "shim_feof",
    "ferror": "shim_ferror", "clearerr": "shim_clearerr",
    "fileno": "shim_fileno", "fgetc": "shim_fgetc", "fgets": "shim_fgets",
    "fputc": "shim_fputc", "fputs": "shim_fputs", "fputwc": "shim_fputwc",
    "getc": "shim_getc", "getc_unlocked": "shim_getc", "getwc": "shim_getwc",
    "ungetc": "shim_ungetc", "ungetwc": "shim_ungetwc",
    "rewind": "shim_rewind", "setbuf": "shim_setbuf",
    "setvbuf": "shim_setvbuf", "fprintf": "shim_fprintf",
    "vfprintf": "shim_vfprintf", "fscanf": "shim_fscanf",
    "perror": "shim_perror", "printf": "shim_printf", "puts": "shim_puts",
    "putchar": "shim_putchar", "dprintf": "shim_dprintf",
    "flockfile": "shim_flockfile", "funlockfile": "shim_funlockfile",
    # environment and identity
    "getenv": "bionic_getenv", "setenv": "bionic_setenv",
    "unsetenv": "bionic_unsetenv", "sysconf": "bionic_sysconf",
    "getauxval": "bionic_getauxval",
    "__system_property_get": "bionic_system_property_get",
    "getentropy": "bionic_getentropy", "arc4random_buf": "bionic_arc4random_buf",
    "syscall": "bionic_syscall", "gettid": "bionic_gettid",
    # sockets: Linux vs BSD numbering and sockaddr layout
    "socket": "net_socket", "bind": "net_bind", "connect": "net_connect",
    "accept": "net_accept", "getsockname": "net_getsockname",
    "getpeername": "net_getpeername", "send": "net_send", "recv": "net_recv",
    "sendto": "net_sendto", "recvfrom": "net_recvfrom",
    "sendmsg": "net_sendmsg", "recvmsg": "net_recvmsg",
    "setsockopt": "net_setsockopt", "getsockopt": "net_getsockopt",
    "getaddrinfo": "net_getaddrinfo", "freeaddrinfo": "net_freeaddrinfo",
    "getnameinfo": "net_getnameinfo", "gethostbyname": "net_gethostbyname",
    "inet_ntop": "net_inet_ntop", "inet_pton": "net_inet_pton",
    # no AF_UNIX and no epoll on the console; both are emulated
    "socketpair": "net_socketpair", "pipe": "net_pipe", "pipe2": "net_pipe2",
    "accept4": "net_accept4", "dup3": "net_dup3",
    "poll": "net_poll", "select": "net_select",
    "epoll_create1": "shim_epoll_create1", "epoll_ctl": "shim_epoll_ctl",
    "epoll_wait": "shim_epoll_wait",
    "clock_gettime": "shim_clock_gettime",
    "clock_getres": "shim_clock_getres",
    # struct tm and struct lconv are longer or differently ordered on bionic
    "localtime": "shim_localtime", "localtime_r": "shim_localtime_r",
    "gmtime_r": "shim_gmtime_r", "mktime": "shim_mktime",
    "localeconv": "shim_localeconv",
    "uname": "shim_uname",
    # aborts and logging
    "abort": "bionic_abort", "__assert2": "bionic_assert2",
    "android_set_abort_message": "bionic_set_abort_message",
    "__stack_chk_fail": "bionic_stack_chk_fail",
    "__android_log_print": "bionic_android_log_print",
    "__android_log_write": "bionic_android_log_write",
    # locale
    "newlocale": "bionic_newlocale", "freelocale": "bionic_freelocale",
    "uselocale": "bionic_uselocale",
    "__ctype_get_mb_cur_max": "shim_ctype_get_mb_cur_max",
    # The locale-aware variants drop the locale argument: only the C locale is
    # ever installed, and the host's locale_t is not what the guest passes.
    "iswalpha_l": "shim_iswalpha_l", "iswblank_l": "shim_iswblank_l",
    "iswcntrl_l": "shim_iswcntrl_l", "iswdigit_l": "shim_iswdigit_l",
    "iswlower_l": "shim_iswlower_l", "iswprint_l": "shim_iswprint_l",
    "iswpunct_l": "shim_iswpunct_l", "iswspace_l": "shim_iswspace_l",
    "iswupper_l": "shim_iswupper_l", "iswxdigit_l": "shim_iswxdigit_l",
    "towlower_l": "shim_towlower_l", "towupper_l": "shim_towupper_l",
    "strcoll_l": "shim_strcoll_l", "strxfrm_l": "shim_strxfrm_l",
    "strftime_l": "shim_strftime_l", "strtold_l": "shim_strtold_l",
    "strtoll_l": "shim_strtoll_l", "strtoull_l": "shim_strtoull_l",
    "wcscoll_l": "shim_wcscoll_l", "wcsxfrm_l": "shim_wcsxfrm_l",
    # threading
    "pthread_mutex_init": "bionic_pthread_mutex_init",
    "pthread_mutex_destroy": "bionic_pthread_mutex_destroy",
    "pthread_mutex_lock": "bionic_pthread_mutex_lock",
    "pthread_mutex_trylock": "bionic_pthread_mutex_trylock",
    "pthread_mutex_unlock": "bionic_pthread_mutex_unlock",
    "pthread_mutexattr_init": "bionic_pthread_mutexattr_init",
    "pthread_mutexattr_destroy": "bionic_pthread_mutexattr_destroy",
    "pthread_mutexattr_settype": "bionic_pthread_mutexattr_settype",
    "pthread_cond_init": "bionic_pthread_cond_init",
    "pthread_cond_destroy": "bionic_pthread_cond_destroy",
    "pthread_cond_signal": "bionic_pthread_cond_signal",
    "pthread_cond_broadcast": "bionic_pthread_cond_broadcast",
    "pthread_cond_wait": "bionic_pthread_cond_wait",
    "pthread_cond_timedwait": "bionic_pthread_cond_timedwait",
    "pthread_condattr_init": "bionic_pthread_condattr_init",
    "pthread_condattr_setclock": "bionic_pthread_condattr_setclock",
    "pthread_rwlock_init": "bionic_pthread_rwlock_init",
    "pthread_rwlock_destroy": "bionic_pthread_rwlock_destroy",
    "pthread_rwlock_rdlock": "bionic_pthread_rwlock_rdlock",
    "pthread_rwlock_wrlock": "bionic_pthread_rwlock_wrlock",
    "pthread_rwlock_unlock": "bionic_pthread_rwlock_unlock",
    "pthread_attr_init": "bionic_pthread_attr_init",
    "pthread_attr_destroy": "bionic_pthread_attr_destroy",
    "pthread_attr_setstacksize": "bionic_pthread_attr_setstacksize",
    "pthread_create": "bionic_pthread_create",
    "pthread_setname_np": "bionic_pthread_setname_np",
    "pthread_setschedparam": "bionic_pthread_setschedparam",
    "pthread_kill": "bionic_pthread_kill",
    "pthread_sigmask": "bionic_pthread_sigmask",
    "pthread_getcpuclockid": "bionic_pthread_getcpuclockid",
    "pthread_key_create": "bionic_pthread_key_create",
    "pthread_key_delete": "bionic_pthread_key_delete",
    "pthread_setspecific": "bionic_pthread_setspecific",
    "pthread_getspecific": "bionic_pthread_getspecific",
    "pthread_once": "bionic_pthread_once",
    "sem_init": "bionic_sem_init", "sem_destroy": "bionic_sem_destroy",
    "sem_post": "bionic_sem_post", "sem_wait": "bionic_sem_wait",
    "sem_trywait": "bionic_sem_trywait",
    "sem_timedwait": "bionic_sem_timedwait",
    # host pthread entry points whose bionic types happen to match
    "pthread_self": "pthread_self", "pthread_equal": "pthread_equal",
    "pthread_exit": "pthread_exit",
    "pthread_join": "bionic_pthread_join",
    "pthread_detach": "bionic_pthread_detach",
    # dynamic linking
    "dlopen": "shim_dlopen", "dlsym": "shim_dlsym", "dlclose": "shim_dlclose",
    "dlerror": "shim_dlerror", "dladdr": "shim_dladdr",
    "dl_iterate_phdr": "so_dl_iterate_phdr",
    # graphics
    "eglGetProcAddress": "gl_shim_eglGetProcAddress",
    "glCompressedTexImage2D": "gl_shim_CompressedTexImage2D",
    "AndroidBitmap_getInfo": "AndroidBitmap_getInfo",
    "AndroidBitmap_lockPixels": "AndroidBitmap_lockPixels",
    "AndroidBitmap_unlockPixels": "AndroidBitmap_unlockPixels",
    # fortified variants
    "__memcpy_chk": "shim_memcpy_chk", "__strcpy_chk": "shim_strcpy_chk",
    "__strncpy_chk": "shim_strncpy_chk", "__strlen_chk": "shim_strlen_chk",
    "__strchr_chk": "shim_strchr_chk", "__vsnprintf_chk": "shim_vsnprintf_chk",
    "__vsprintf_chk": "shim_vsprintf_chk", "__read_chk": "shim_read_chk",
    "__write_chk": "shim_write_chk", "__poll_chk": "shim_poll_chk",
    "__FD_SET_chk": "shim_fd_set_chk", "__FD_ISSET_chk": "shim_fd_isset_chk",
    # odds and ends
    "basename": "shim_basename",
    "clock_nanosleep": "shim_clock_nanosleep",
    "posix_memalign": "shim_posix_memalign",
    "strerror": "shim_strerror", "strerror_r": "shim_strerror_r",
    "__gnu_strerror_r": "shim_gnu_strerror_r",
    "siglongjmp": "shim_siglongjmp",
    "__cmsg_nxthdr": "shim_cmsg_nxthdr",
    "__sched_cpualloc": "shim_sched_cpualloc",
    "__sched_cpufree": "shim_sched_cpufree",
    "__sched_cpucount": "shim_sched_cpucount",
    "__libc_current_sigrtmin": "shim_sigrtmin",
    "__libc_current_sigrtmax": "shim_sigrtmax",
    "__cxa_atexit": "shim_cxa_atexit",
    "__cxa_finalize": "shim_cxa_finalize",
    "__cxa_thread_atexit_impl": "shim_cxa_thread_atexit",
    "__register_atfork": "shim_register_atfork",
    "readv": "shim_readv", "writev": "shim_writev",
    "preadv": "shim_preadv", "pwritev": "shim_pwritev",
    "sched_yield": "shim_sched_yield",
    "sched_getaffinity": "shim_sched_getaffinity",
    "sched_setaffinity": "shim_sched_setaffinity",
    "getrlimit": "shim_getrlimit", "setrlimit": "shim_setrlimit",
    "getrusage": "shim_getrusage",
    "getpid": "shim_getpid",
    "fpathconf": "shim_pathconf_fd", "pathconf": "shim_pathconf",
}

# Data symbols the guest links against.
DATA = {
    "__sF": "bionic_sF",
    "stdin": "bionic_stdin",
    "stdout": "bionic_stdout",
    "stderr": "bionic_stderr",
    "environ": "bionic_environ",
    "tzname": "bionic_tzname",
    "in6addr_any": "in6addr_any",
    "in6addr_loopback": "in6addr_loopback",
}

# Present but deliberately inert. The value is the stub family to use.
#   "zero"  -> returns 0 / success
#   "fail"  -> returns -1 with ENOSYS
#   "null"  -> returns NULL
STUBS = {
    # no console equivalent for process management
    "fork": "fail", "vfork": "fail", "execv": "fail", "execve": "fail",
    "system": "fail", "wait": "fail", "waitpid": "fail", "wait4": "fail",
    "waitid": "fail", "killpg": "zero", "setns": "fail", "unshare": "fail",
    "nice": "zero", "pause": "fail", "alarm": "zero",
    # credentials: a single-user console
    "getuid": "zero", "geteuid": "zero", "getgid": "zero", "getegid": "zero",
    "getppid": "zero", "getpgid": "zero", "getpgrp": "zero", "getsid": "zero",
    "setpgid": "zero", "setpgrp": "zero", "setsid": "zero",
    "getresuid": "zero", "getresgid": "zero", "getgroups": "zero",
    "setgroups": "zero", "getgrouplist": "fail", "getpwuid_r": "fail",
    "getpwnam_r": "fail", "getlogin": "null", "umask": "zero",
    "chown": "zero", "lchown": "zero", "fchown": "zero", "fchownat": "zero",
    "fchmod": "zero", "fchmodat": "zero", "fchdir": "fail",
    # scheduling and limits
    "sched_get_priority_max": "zero", "sched_get_priority_min": "zero",
    "sched_getparam": "fail", "sched_setparam": "fail",
    "sched_getscheduler": "fail", "sched_setscheduler": "fail",
    "sched_rr_get_interval": "fail",
    "getpriority": "zero", "setpriority": "zero",
    "getitimer": "fail", "setitimer": "fail",
    "mlock": "zero",
    # signals: nothing delivers them here
    "sigaction": "zero", "sigaddset": "zero", "sigdelset": "zero",
    "sigemptyset": "zero", "sigfillset": "zero", "sigismember": "zero",
    "sigprocmask": "zero", "sigpending": "zero", "sigaltstack": "zero",
    "sigwait": "fail", "sigwaitinfo": "fail", "sigtimedwait": "fail",
    # descriptors the SD filesystem cannot provide
    "eventfd": "fail", "eventfd_read": "fail", "eventfd_write": "fail",
    "timerfd_create": "fail", "timerfd_gettime": "fail",
    "timerfd_settime": "fail",
    "flock": "zero", "lockf": "zero", "fdatasync": "zero", "sync": "zero",
    "posix_fadvise": "zero", "posix_fallocate": "zero",
    "futimens": "zero", "utimensat": "zero", "utimes": "zero",
    "mkfifo": "fail", "mkfifoat": "fail", "mknod": "fail", "mknodat": "fail",
    "link": "fail", "linkat": "fail", "symlink": "fail", "symlinkat": "fail",
    "sendfile": "fail", "splice": "fail",
    # extended attributes
    "getxattr": "fail", "lgetxattr": "fail", "fgetxattr": "fail",
    "setxattr": "fail", "lsetxattr": "fail", "fsetxattr": "fail",
    "listxattr": "fail", "llistxattr": "fail", "flistxattr": "fail",
    "removexattr": "fail", "lremovexattr": "fail", "fremovexattr": "fail",
    # terminals
    "openpty": "fail", "forkpty": "fail", "login_tty": "fail",
    "posix_openpt": "fail", "ptsname_r": "fail", "grantpt": "fail",
    "unlockpt": "fail", "ttyname_r": "fail",
    "tcgetpgrp": "fail", "tcsetpgrp": "fail",
    # networking corners libnx does not implement
    "gethostbyname_r": "fail", "gethostbyaddr_r": "fail",
    "if_indextoname": "null", "if_nametoindex": "zero",
    "if_nameindex": "null", "if_freenameindex": "zero",
    # logging to a syslog daemon that does not exist
    "openlog": "zero", "syslog": "zero", "closelog": "zero",
}

HEADER = """/* Bindings for every symbol the Android library imports.
 *
 * GENERATED by tools/gen_imports.py -- do not edit by hand. Re-run the
 * generator against a game library to refresh it; any symbol it cannot
 * classify is reported instead of silently dropped.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>
#include <wchar.h>
#include <zlib.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "epoll_shim.h"
#include "errno_shim.h"
#include "gl_shim.h"
#include "imports.h"
#include "io_shim.h"
#include "jni_env.h"
#include "net_shim.h"
#include "opensles.h"
#include "pthread_shim.h"
#include "shims.h"
#include "so_util.h"
#include "sys_shim.h"

/* Bitmap entry points live with the rest of the Java bridge. */
int AndroidBitmap_getInfo(void *env, jobject bitmap, void *info);
int AndroidBitmap_lockPixels(void *env, jobject bitmap, void **pixels);
int AndroidBitmap_unlockPixels(void *env, jobject bitmap);

"""


def parse_words(text):
    return [w for w in text.split() if w]


def main():
    if len(sys.argv) < 2:
        print("usage: gen_imports.py <libmain.so> [output.c]", file=sys.stderr)
        return 2
    lib = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "source/imports.c"

    readelf = os.environ.get("READELF", "aarch64-none-elf-readelf")
    raw = subprocess.check_output([readelf, "-W", "--dyn-syms", lib], text=True)
    undefined = set()
    for line in raw.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[6] == "UND":
            undefined.add(parts[7].split("@")[0])
    undefined.discard("")

    direct = set(parse_words(DIRECT))
    gl_direct = set(parse_words(GL_DIRECT))

    # A name in two categories would be resolved silently by the emit order
    # below, which is exactly the kind of mistake this generator exists to
    # prevent.
    categories = {"direct": direct, "gl": gl_direct, "shim": set(SHIM),
                  "data": set(DATA), "stub": set(STUBS)}
    names = sorted(categories)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            clash = sorted(categories[a] & categories[b] & undefined)
            if clash:
                print("Imports classified as both %s and %s: %s"
                      % (a, b, ", ".join(clash)), file=sys.stderr)
                return 1

    known = direct | gl_direct | set(SHIM) | set(DATA) | set(STUBS)
    missing = sorted(undefined - known)
    if missing:
        print("Unclassified imports (%d):" % len(missing), file=sys.stderr)
        for name in missing:
            print("  " + name, file=sys.stderr)
        return 1

    unused = sorted(known - undefined)

    lines = [HEADER]

    # Stub bodies.
    lines.append("/* ------------------------------------------------ inert entry points */\n")
    lines.append("/* Facilities the console has no equivalent for. Failing loudly here would\n"
                 " * abort startup, so each one answers the way an unprivileged Linux process\n"
                 " * would when the feature is unavailable. */\n")
    for name in sorted(n for n in STUBS if n in undefined):
        kind = STUBS[name]
        if kind == "zero":
            lines.append("static long stub_%s(void) { return 0; }\n" % name)
        elif kind == "null":
            lines.append("static void *stub_%s(void) { return NULL; }\n" % name)
        else:
            lines.append("static long stub_%s(void) { errno = ENOSYS; return -1; }\n" % name)
    lines.append("\n")

    lines.append("/* --------------------------------------------------------- the table */\n\n")
    lines.append("DynLibFunction dynlib_functions[] = {\n")

    def emit(name, target, cast="(uintptr_t)&"):
        lines.append('    {"%s", %s%s},\n' % (name, cast, target))

    for name in sorted(undefined):
        if name in SHIM:
            emit(name, SHIM[name])
        elif name in DATA:
            emit(name, DATA[name])
        elif name in STUBS:
            emit(name, "stub_%s" % name)
        else:
            emit(name, name)

    lines.append("};\n\n")
    lines.append("const size_t dynlib_function_count =\n"
                 "    sizeof dynlib_functions / sizeof dynlib_functions[0];\n\n")
    lines.append("""uintptr_t dynlib_find_export(const char *name) {
  if (!name) return 0;
  for (size_t i = 0; i < dynlib_function_count; i++)
    if (!strcmp(dynlib_functions[i].symbol, name)) return dynlib_functions[i].func;
  return 0;
}
""")

    with open(out_path, "w") as handle:
        handle.write("".join(lines))

    print("wrote %s: %d imports (%d direct, %d GL, %d shims, %d data, %d inert)"
          % (out_path, len(undefined),
             len(direct & undefined), len(gl_direct & undefined),
             len(set(SHIM) & undefined), len(set(DATA) & undefined),
             len(set(STUBS) & undefined)))
    if unused:
        print("note: %d classified names are not imported by this build" % len(unused))
    return 0


if __name__ == "__main__":
    sys.exit(main())
