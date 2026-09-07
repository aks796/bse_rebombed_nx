/* Filesystem, descriptor and memory-mapping surface for the guest.
 *
 * Two things force a translation layer rather than straight pass-through:
 * bionic's struct stat / dirent / O_* differ from newlib's, and the guest
 * expects a handful of Linux pseudo-files that the Switch has no equivalent
 * for.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <switch.h>
#include <unistd.h>

#include "bionic.h"
#include "config.h"
#include "epoll_shim.h"
#include "error.h"
#include "io_shim.h"
#include "pipe_shim.h"
#include "net_shim.h"
#include "so_util.h"
#include "util.h"

/* ------------------------------------------------------------ path mapping */

static __thread char g_path_buf[1088];

const char *host_path(const char *path) {
  if (!path) return path;
  /* Already device-qualified, or relative: hand it straight to newlib. */
  if (path[0] != '/') return path;
  if (strchr(path, ':')) return path;
  snprintf(g_path_buf, sizeof g_path_buf, "sdmc:%s", path);
  return g_path_buf;
}

/* ------------------------------------------------------------ virtual files */

/* The guest reads a few Linux pseudo-files during startup. Serve them from
 * memory on descriptors well above anything newlib hands out. */
#define VFD_BASE 0x7000
#define VFD_MAX 16

typedef struct {
  int used;
  int random;      /* refill from the CSRNG on every read */
  char *data;
  size_t size;
  size_t pos;
} VirtualFile;

static VirtualFile g_vfiles[VFD_MAX];
static Mutex g_vfile_lock;

/* On Android fds 0, 1 and 2 are always open, and CPython decides whether to
 * build sys.stdout/sys.stderr by probing them. Nothing on the console backs
 * them, so they are served here and their output goes to the trace log --
 * which is what makes a Python traceback visible at all. */
static int is_std_fd(int fd) { return fd >= 0 && fd <= 2; }

static int is_virtual(int fd) { return fd >= VFD_BASE && fd < VFD_BASE + VFD_MAX; }
static VirtualFile *vfile(int fd) {
  return is_virtual(fd) ? &g_vfiles[fd - VFD_BASE] : NULL;
}

static int vfile_open(const char *data, size_t size, int random) {
  mutexLock(&g_vfile_lock);
  for (int i = 0; i < VFD_MAX; i++) {
    if (g_vfiles[i].used) continue;
    g_vfiles[i].used = 1;
    g_vfiles[i].random = random;
    g_vfiles[i].pos = 0;
    g_vfiles[i].size = size;
    g_vfiles[i].data = NULL;
    if (size) {
      g_vfiles[i].data = malloc(size);
      if (!g_vfiles[i].data) { g_vfiles[i].used = 0; break; }
      memcpy(g_vfiles[i].data, data, size);
    }
    mutexUnlock(&g_vfile_lock);
    return VFD_BASE + i;
  }
  mutexUnlock(&g_vfile_lock);
  errno = EMFILE;
  return -1;
}

/* Returns -2 when the path is not one we synthesize. */
static int try_open_virtual(const char *path) {
  if (!path) return -2;

  if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random"))
    return vfile_open(NULL, 0, 1);
  if (!strcmp(path, "/dev/null")) return vfile_open(NULL, 0, 0);
  if (!strcmp(path, "/dev/stdout")) return 1;
  if (!strcmp(path, "/dev/stderr")) return 2;
  if (!strcmp(path, "/dev/stdin")) return 0;

  if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/self/smaps")) {
    static char maps[2048];
    so_dump_maps(maps, sizeof maps);
    return vfile_open(maps, strlen(maps), 0);
  }
  if (!strcmp(path, "/proc/self/cmdline")) {
    static const char cmdline[] = "net.froemling.bombsquad\0";
    return vfile_open(cmdline, sizeof cmdline, 0);
  }
  if (!strcmp(path, "/proc/cpuinfo")) {
    static const char cpuinfo[] =
        "processor\t: 0\nmodel name\t: ARMv8 Cortex-A57\nFeatures\t: fp asimd aes "
        "pmull sha1 sha2 crc32\nCPU implementer\t: 0x41\nCPU architecture: 8\n"
        "processor\t: 1\nprocessor\t: 2\n";
    return vfile_open(cpuinfo, sizeof cpuinfo - 1, 0);
  }
  return -2;
}

/* --------------------------------------------------------- failure tracing
 *
 * Python's import machinery probes many paths that legitimately do not
 * exist, so a missing file is not worth reporting. Anything else is, and a
 * directory that will not open at all is the failure most likely to leave
 * the interpreter unable to find its own standard library. */

static unsigned g_open_failures;
static unsigned g_opendir_failures;
#define TRACE_FAILURE_LIMIT 24

static void trace_open_failure(const char *path) {
  if (errno == ENOENT) return;
  if (__atomic_fetch_add(&g_open_failures, 1, __ATOMIC_RELAXED) >= TRACE_FAILURE_LIMIT)
    return;
  trace("open failed: %s (%s)", path ? path : "?", strerror(errno));
}

static void trace_opendir_failure(const char *path) {
  if (__atomic_fetch_add(&g_opendir_failures, 1, __ATOMIC_RELAXED) >=
      TRACE_FAILURE_LIMIT)
    return;
  trace("opendir failed: %s (%s)", path ? path : "?", strerror(errno));
}

/* ------------------------------------------------------------ flag mapping */

static int flags_to_host(int flags) {
  int out = 0;
  switch (flags & 3) {
    case BIONIC_O_WRONLY: out |= O_WRONLY; break;
    case BIONIC_O_RDWR: out |= O_RDWR; break;
    default: out |= O_RDONLY; break;
  }
  if (flags & BIONIC_O_CREAT) out |= O_CREAT;
  if (flags & BIONIC_O_EXCL) out |= O_EXCL;
  if (flags & BIONIC_O_TRUNC) out |= O_TRUNC;
  if (flags & BIONIC_O_APPEND) out |= O_APPEND;
  if (flags & BIONIC_O_NONBLOCK) out |= O_NONBLOCK;
  if (flags & BIONIC_O_NOCTTY) out |= O_NOCTTY;
  if (flags & BIONIC_O_CLOEXEC) out |= O_CLOEXEC;
  if (flags & BIONIC_O_NOFOLLOW) out |= O_NOFOLLOW;
  if (flags & BIONIC_O_DIRECTORY) out |= O_DIRECTORY;
  return out;
}

static void stat_to_bionic(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof *out);
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize ? in->st_blksize : 4096;
  out->st_blocks = in->st_blocks ? in->st_blocks : (in->st_size + 511) / 512;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

/* ------------------------------------------------------------- descriptors */

void io_shim_init(void) {
  mutexInit(&g_vfile_lock);
  epoll_shim_init();
}

int bionic_open(const char *path, int flags, ...) {
  const int virt = try_open_virtual(path);
  if (virt != -2) return virt;

  mode_t mode = 0666;
  if (flags & BIONIC_O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = (mode_t)va_arg(args, int);
    va_end(args);
  }
  const int fd = open(host_path(path), flags_to_host(flags), mode);
  if (fd < 0) trace_open_failure(path);
  return fd;
}

int bionic_openat(int dirfd, const char *path, int flags, ...) {
  mode_t mode = 0666;
  if (flags & BIONIC_O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = (mode_t)va_arg(args, int);
    va_end(args);
  }
  /* Only AT_FDCWD is ever used here; anything else would need a real *at. */
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') {
    errno = ENOSYS;
    return -1;
  }
  return bionic_open(path, flags, (int)mode);
}

int bionic_close(int fd) {
  if (is_std_fd(fd)) return 0;
  if (epoll_shim_owns(fd)) return epoll_shim_close(fd);
  if (pipe_shim_owns(fd)) return pipe_shim_close(fd);
  VirtualFile *vf = vfile(fd);
  if (vf) {
    mutexLock(&g_vfile_lock);
    free(vf->data);
    vf->data = NULL;
    vf->used = 0;
    mutexUnlock(&g_vfile_lock);
    return 0;
  }
  return close(fd);
}

long bionic_read(int fd, void *buf, size_t count) {
  if (pipe_shim_owns(fd)) return pipe_shim_read(fd, buf, count);
  if (is_std_fd(fd)) {
    (void)buf;
    (void)count;
    return 0; /* stdin is always at end of file */
  }
  VirtualFile *vf = vfile(fd);
  if (vf) {
    if (vf->random) {
      csrngGetRandomBytes(buf, count);
      return (long)count;
    }
    if (vf->pos >= vf->size) return 0;
    size_t available = vf->size - vf->pos;
    if (count > available) count = available;
    memcpy(buf, vf->data + vf->pos, count);
    vf->pos += count;
    return (long)count;
  }
  return read(fd, buf, count);
}

long bionic_write(int fd, const void *buf, size_t count) {
  if (pipe_shim_owns(fd)) return pipe_shim_write(fd, buf, count);
  if (fd == 1 || fd == 2) {
    trace_raw((const char *)buf, count);
    return (long)count;
  }
  if (is_std_fd(fd)) return (long)count;
  if (vfile(fd)) return (long)count; /* /dev/null and friends swallow it */
  return write(fd, buf, count);
}

long bionic_pread(int fd, void *buf, size_t count, int64_t offset) {
  if (is_std_fd(fd)) return 0;
  if (vfile(fd)) return 0;
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  if (saved < 0) return -1;
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) return -1;
  const long got = read(fd, buf, count);
  lseek(fd, saved, SEEK_SET);
  return got;
}

long bionic_pwrite(int fd, const void *buf, size_t count, int64_t offset) {
  if (is_std_fd(fd)) return bionic_write(fd, buf, count);
  if (vfile(fd)) return (long)count;
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  if (saved < 0) return -1;
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) return -1;
  const long put = write(fd, buf, count);
  lseek(fd, saved, SEEK_SET);
  return put;
}

int64_t bionic_lseek(int fd, int64_t offset, int whence) {
  /* Reported as pipes, so nothing tries to seek or size them. */
  if (is_std_fd(fd)) { errno = ESPIPE; return -1; }
  VirtualFile *vf = vfile(fd);
  if (vf) {
    int64_t target = offset;
    if (whence == SEEK_CUR) target += (int64_t)vf->pos;
    else if (whence == SEEK_END) target += (int64_t)vf->size;
    if (target < 0) { errno = EINVAL; return -1; }
    vf->pos = (size_t)target;
    return target;
  }
  return lseek(fd, (off_t)offset, whence);
}

int bionic_fcntl(int fd, int cmd, ...) {
  va_list args;
  va_start(args, cmd);
  const long arg = va_arg(args, long);
  va_end(args);
  /* F_GETFD on the standard descriptors is how CPython decides whether they
   * exist, so they have to answer as open. */
  if (is_std_fd(fd)) return 0;
  if (vfile(fd)) return 0;
  /* The status-flag bits differ between the two libcs, so this goes through
   * the same translation sockets use -- which also knows about the
   * in-process pipe pairs. */
  return net_fcntl(fd, cmd, arg);
}

static unsigned g_ioctl_failures;

static void trace_ioctl_failure(int fd, unsigned long request) {
  if (__atomic_fetch_add(&g_ioctl_failures, 1, __ATOMIC_RELAXED) >=
      TRACE_FAILURE_LIMIT)
    return;
  trace("ioctl 0x%lx failed on fd %d (%s)", request, fd, strerror(errno));
}

/* Linux request numbers, which is what the guest was compiled against. */
#define BIONIC_FIONBIO 0x5421u
#define BIONIC_FIONREAD 0x541Bu
#define BIONIC_FIOCLEX 0x5451u
#define BIONIC_FIONCLEX 0x5450u

int bionic_ioctl(int fd, unsigned long request, ...) {
  va_list args;
  va_start(args, request);
  void *argument = va_arg(args, void *);
  va_end(args);

  int result;
  switch (request) {
    case BIONIC_FIONBIO:
      /* Both CPython's socket.setblocking() and the socket constructor come
       * through here rather than fcntl, so asyncio depends on it. */
      result = net_set_nonblocking(fd, argument && *(int *)argument);
      break;
    case BIONIC_FIONREAD:
      if (pipe_shim_owns(fd)) {
        const long readable = pipe_shim_bytes_readable(fd);
        if (readable < 0) return -1;
        if (argument) *(int *)argument = (int)readable;
        return 0;
      }
      result = ioctl(fd, FIONREAD, argument);
      break;
    case BIONIC_FIOCLEX:
    case BIONIC_FIONCLEX:
      return 0; /* nothing here ever execs */
    default:
      /* CPython treats ENOTTY as "this descriptor has no ioctls" and moves
       * on; any other errno becomes an exception. */
      errno = ENOTTY;
      return -1;
  }

  if (result < 0) {
    trace_ioctl_failure(fd, request);
    return -1;
  }
  /* libnx can hand back a positive value on success; callers only test for
   * -1, so normalise it. */
  return 0;
}

int bionic_fstat(int fd, struct bionic_stat *out) {
  if (pipe_shim_owns(fd)) {
    memset(out, 0, sizeof *out);
    out->st_mode = S_IFIFO | 0600;
    out->st_nlink = 1;
    out->st_blksize = 4096;
    return 0;
  }
  if (is_std_fd(fd)) {
    memset(out, 0, sizeof *out);
    out->st_mode = S_IFCHR | 0620;
    out->st_nlink = 1;
    out->st_blksize = 1024;
    out->st_rdev = 0x0103; /* the usual major/minor for a null device */
    return 0;
  }
  VirtualFile *vf = vfile(fd);
  if (vf) {
    memset(out, 0, sizeof *out);
    out->st_mode = S_IFCHR | 0666;
    out->st_size = (int64_t)vf->size;
    out->st_blksize = 4096;
    return 0;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) return -1;
  stat_to_bionic(&st, out);
  return 0;
}

int bionic_fsync(int fd) {
  return (is_std_fd(fd) || vfile(fd) || pipe_shim_owns(fd)) ? 0 : fsync(fd);
}

int bionic_ftruncate(int fd, int64_t length) {
  if (is_std_fd(fd)) { errno = EINVAL; return -1; }
  return vfile(fd) ? 0 : ftruncate(fd, (off_t)length);
}

int bionic_dup(int fd) {
  if (is_std_fd(fd) || vfile(fd) || pipe_shim_owns(fd)) return fd;
  return dup(fd);
}
int bionic_dup2(int oldfd, int newfd) {
  if (is_std_fd(oldfd) || vfile(oldfd) || pipe_shim_owns(oldfd)) return newfd;
  return dup2(oldfd, newfd);
}
int bionic_isatty(int fd) { (void)fd; return 0; }

/* -------------------------------------------------------------- path layer */

int bionic_stat(const char *path, struct bionic_stat *out) {
  struct stat st;
  if (stat(host_path(path), &st) != 0) return -1;
  stat_to_bionic(&st, out);
  return 0;
}

int bionic_lstat(const char *path, struct bionic_stat *out) {
  /* The SD filesystem has no symlinks, so lstat and stat agree. */
  return bionic_stat(path, out);
}

int bionic_fstatat(int dirfd, const char *path, struct bionic_stat *out, int flags) {
  (void)flags;
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  return bionic_stat(path, out);
}

int bionic_access(const char *path, int mode) {
  struct stat st;
  if (stat(host_path(path), &st) != 0) return -1;
  /* Everything readable is also writable here; X_OK only holds for dirs. */
  if ((mode & 1) && !S_ISDIR(st.st_mode)) { errno = EACCES; return -1; }
  return 0;
}

int bionic_mkdir(const char *path, unsigned mode) {
  return mkdir(host_path(path), mode);
}
int bionic_mkdirat(int dirfd, const char *path, unsigned mode) {
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  return bionic_mkdir(path, mode);
}
int bionic_rmdir(const char *path) { return rmdir(host_path(path)); }
int bionic_unlink(const char *path) { return unlink(host_path(path)); }
int bionic_unlinkat(int dirfd, const char *path, int flags) {
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  return (flags & 0x200 /* AT_REMOVEDIR */) ? bionic_rmdir(path) : bionic_unlink(path);
}
int bionic_remove(const char *path) {
  struct stat st;
  const char *hp = host_path(path);
  if (stat(hp, &st) == 0 && S_ISDIR(st.st_mode)) return rmdir(hp);
  return unlink(hp);
}

int bionic_rename(const char *from, const char *to) {
  /* host_path uses one per-thread buffer, so both paths need copies. */
  char from_host[1088];
  char to_host[1088];
  snprintf(from_host, sizeof from_host, "%s", host_path(from));
  snprintf(to_host, sizeof to_host, "%s", host_path(to));

  if (rename(from_host, to_host) == 0) return 0;

  /* POSIX rename replaces an existing destination atomically. The SD card's
   * filesystem refuses instead, which breaks every save-to-temp-then-replace
   * pattern -- config writes and Python's own .pyc caching among them. */
  if (errno != EEXIST) return -1;
  if (unlink(to_host) != 0) return -1;
  return rename(from_host, to_host);
}

int bionic_renameat(int fromfd, const char *from, int tofd, const char *to) {
  if ((fromfd != BIONIC_AT_FDCWD && from && from[0] != '/') ||
      (tofd != BIONIC_AT_FDCWD && to && to[0] != '/')) {
    errno = ENOSYS;
    return -1;
  }
  return bionic_rename(from, to);
}

int bionic_chdir(const char *path) { return chdir(host_path(path)); }

char *bionic_getcwd(char *buf, size_t size) {
  char host[768];
  if (!getcwd(host, sizeof host)) return NULL;
  const char *unix_path = strip_device(host);
  if (strlen(unix_path) + 1 > size) { errno = ERANGE; return NULL; }
  strcpy(buf, unix_path);
  return buf;
}

char *bionic_realpath(const char *path, char *resolved) {
  /* No symlinks and no ".." games on the SD card, so the input is already
   * canonical once it is absolute. */
  char out[1024];
  if (path && path[0] == '/') {
    snprintf(out, sizeof out, "%s", path);
  } else {
    char cwd[512];
    if (!bionic_getcwd(cwd, sizeof cwd)) return NULL;
    snprintf(out, sizeof out, "%s/%s", cwd, path ? path : "");
  }
  if (!path_exists(host_path(out))) { errno = ENOENT; return NULL; }
  char *dest = resolved ? resolved : malloc(strlen(out) + 1);
  if (!dest) return NULL;
  strcpy(dest, out);
  return dest;
}

long bionic_readlink(const char *path, char *buf, size_t size) {
  (void)path;
  (void)buf;
  (void)size;
  errno = EINVAL; /* nothing on the SD card is a symlink */
  return -1;
}
long bionic_readlinkat(int dirfd, const char *path, char *buf, size_t size) {
  (void)dirfd;
  return bionic_readlink(path, buf, size);
}

int bionic_truncate(const char *path, int64_t length) {
  return truncate(host_path(path), (off_t)length);
}
int bionic_chmod(const char *path, unsigned mode) {
  (void)path;
  (void)mode;
  return 0; /* FAT has no permission bits to change */
}

static void fill_statvfs(struct bionic_statvfs *out) {
  memset(out, 0, sizeof *out);
  out->f_bsize = 32768;
  out->f_frsize = 32768;
  /* Reported as a large fixed volume; the engine only uses this to decide
   * whether a cache write is worth attempting. */
  out->f_blocks = 1024ull * 1024ull;
  out->f_bfree = 512ull * 1024ull;
  out->f_bavail = 512ull * 1024ull;
  out->f_namemax = 255;
}

int bionic_statvfs(const char *path, struct bionic_statvfs *out) {
  (void)path;
  fill_statvfs(out);
  return 0;
}
int bionic_fstatvfs(int fd, struct bionic_statvfs *out) {
  (void)fd;
  fill_statvfs(out);
  return 0;
}
int bionic_statfs(const char *path, struct bionic_statfs *out) {
  (void)path;
  memset(out, 0, sizeof *out);
  out->f_type = 0x4d44; /* MSDOS_SUPER_MAGIC */
  out->f_bsize = 32768;
  out->f_frsize = 32768;
  out->f_blocks = 1024ull * 1024ull;
  out->f_bfree = 512ull * 1024ull;
  out->f_bavail = 512ull * 1024ull;
  out->f_namelen = 255;
  return 0;
}

/* ------------------------------------------------------------- directories */

typedef struct {
  DIR *host;
  struct bionic_dirent entry;
} ShimDir;

void *bionic_opendir(const char *path) {
  DIR *host = opendir(host_path(path));
  if (!host) {
    trace_opendir_failure(path);
    return NULL;
  }
  ShimDir *dir = calloc(1, sizeof *dir);
  if (!dir) { closedir(host); errno = ENOMEM; return NULL; }
  dir->host = host;
  return dir;
}

void *bionic_fdopendir(int fd) {
  (void)fd;
  errno = ENOSYS;
  return NULL;
}

void *bionic_readdir(void *handle) {
  ShimDir *dir = handle;
  if (!dir) return NULL;
  struct dirent *src = readdir(dir->host);
  if (!src) return NULL;

  memset(&dir->entry, 0, sizeof dir->entry);
  snprintf(dir->entry.d_name, sizeof dir->entry.d_name, "%s", src->d_name);
  dir->entry.d_ino = 1;
  dir->entry.d_reclen = sizeof dir->entry;
#ifdef DT_DIR
  if (src->d_type == DT_DIR) dir->entry.d_type = BIONIC_DT_DIR;
  else if (src->d_type == DT_REG) dir->entry.d_type = BIONIC_DT_REG;
  else dir->entry.d_type = BIONIC_DT_UNKNOWN;
#else
  dir->entry.d_type = BIONIC_DT_UNKNOWN;
#endif
  return &dir->entry;
}

int bionic_closedir(void *handle) {
  ShimDir *dir = handle;
  if (!dir) return -1;
  const int rc = closedir(dir->host);
  free(dir);
  return rc;
}

void bionic_rewinddir(void *handle) {
  ShimDir *dir = handle;
  if (dir) rewinddir(dir->host);
}

/* ------------------------------------------------------------------ stdio */

/* The guest resolves stdin/stdout/stderr as &__sF[n]; give it three opaque
 * blocks we can recognise and swap for the host streams. */
uint8_t bionic_sF[3][BIONIC_FILE_SIZE];

FILE *host_stream(FILE *guest) {
  const uintptr_t address = (uintptr_t)guest;
  const uintptr_t base = (uintptr_t)bionic_sF;
  if (address < base || address >= base + sizeof bionic_sF) return guest;

  /* Tolerate a pointer landing anywhere inside an entry rather than exactly
   * on its start. */
  /* stdin has nothing behind it; stdout and stderr go to the trace log. */
  if ((address - base) / BIONIC_FILE_SIZE == 0) return stdin;
  FILE *log = trace_stream();
  return log ? log : stderr;
}

FILE *bionic_fopen(const char *path, const char *mode) {
  const int virt = try_open_virtual(path);
  if (virt != -2) {
    if (virt < 0) return NULL;
    /* Only the /dev entries reach here in practice, and nothing reads one
     * as a stream; drop the descriptor and hand back a null sink. */
    bionic_close(virt);
    return fopen("sdmc:/dev/null", mode);
  }
  return fopen(host_path(path), mode);
}

FILE *bionic_fdopen(int fd, const char *mode) {
  if (vfile(fd)) return NULL;
  return fdopen(fd, mode);
}

/* ---------------------------------------------------------- memory mapping */

/* CPython's arena allocator and the engine's asset loader both mmap. Back
 * anonymous maps with aligned heap blocks and file maps with a read-through
 * copy, tracking each so munmap can free it. */
#define MMAP_TRACK_MAX 512

typedef struct {
  void *addr;
  size_t length;
} MmapEntry;

static MmapEntry g_mmaps[MMAP_TRACK_MAX];
static Mutex g_mmap_lock;
static int g_mmap_lock_ready;

static void mmap_lock(void) {
  if (!g_mmap_lock_ready) { mutexInit(&g_mmap_lock); g_mmap_lock_ready = 1; }
  mutexLock(&g_mmap_lock);
}

static void mmap_track(void *addr, size_t length) {
  mmap_lock();
  for (int i = 0; i < MMAP_TRACK_MAX; i++) {
    if (g_mmaps[i].addr) continue;
    g_mmaps[i].addr = addr;
    g_mmaps[i].length = length;
    break;
  }
  mutexUnlock(&g_mmap_lock);
}

static int mmap_untrack(void *addr) {
  mmap_lock();
  for (int i = 0; i < MMAP_TRACK_MAX; i++) {
    if (g_mmaps[i].addr != addr) continue;
    g_mmaps[i].addr = NULL;
    g_mmaps[i].length = 0;
    mutexUnlock(&g_mmap_lock);
    return 1;
  }
  mutexUnlock(&g_mmap_lock);
  return 0;
}

#define BIONIC_MAP_ANONYMOUS 0x20
#define BIONIC_MAP_FIXED 0x10
#define BIONIC_MAP_FAILED ((void *)-1)

void *bionic_mmap(void *addr, size_t length, int prot, int flags, int fd,
                  int64_t offset) {
  (void)prot;
  if (flags & BIONIC_MAP_FIXED) { errno = ENOMEM; return BIONIC_MAP_FAILED; }
  if (length == 0) { errno = EINVAL; return BIONIC_MAP_FAILED; }
  (void)addr;

  void *block = memalign(0x1000, (length + 0xFFF) & ~(size_t)0xFFF);
  if (!block) { errno = ENOMEM; return BIONIC_MAP_FAILED; }

  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(block, 0, length);
  } else {
    if (bionic_pread(fd, block, length, offset) < 0) {
      free(block);
      errno = EACCES;
      return BIONIC_MAP_FAILED;
    }
  }
  mmap_track(block, length);
  return block;
}

int bionic_munmap(void *addr, size_t length) {
  (void)length;
  if (!mmap_untrack(addr)) { errno = EINVAL; return -1; }
  free(addr);
  return 0;
}

void *bionic_mremap(void *addr, size_t old_size, size_t new_size, int flags, ...) {
  (void)flags;
  if (!mmap_untrack(addr)) { errno = EINVAL; return BIONIC_MAP_FAILED; }
  void *block = memalign(0x1000, (new_size + 0xFFF) & ~(size_t)0xFFF);
  if (!block) {
    mmap_track(addr, old_size);
    errno = ENOMEM;
    return BIONIC_MAP_FAILED;
  }
  const size_t copy = old_size < new_size ? old_size : new_size;
  memcpy(block, addr, copy);
  if (new_size > old_size) memset((char *)block + old_size, 0, new_size - old_size);
  free(addr);
  mmap_track(block, new_size);
  return block;
}

int bionic_mprotect(void *addr, size_t length, int prot) {
  (void)addr;
  (void)length;
  (void)prot;
  return 0; /* heap pages are already RW; guard pages are not enforced */
}
int bionic_msync(void *addr, size_t length, int flags) {
  (void)addr;
  (void)length;
  (void)flags;
  return 0;
}
int bionic_madvise(void *addr, size_t length, int advice) {
  (void)addr;
  (void)length;
  (void)advice;
  return 0;
}
