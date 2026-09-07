/* Filesystem, descriptor and memory-mapping surface for the guest. */

#ifndef BSNX_IO_SHIM_H
#define BSNX_IO_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct bionic_stat;
struct bionic_statvfs;
struct bionic_statfs;

/* Map a guest path onto a host devoptab path. The result lives in a
 * per-thread buffer and stays valid until the next call on that thread. */
const char *host_path(const char *path);

void io_shim_init(void);

/* Descriptor layer. */
int bionic_open(const char *path, int flags, ...);
int bionic_openat(int dirfd, const char *path, int flags, ...);
int bionic_close(int fd);
long bionic_read(int fd, void *buf, size_t count);
long bionic_write(int fd, const void *buf, size_t count);
long bionic_pread(int fd, void *buf, size_t count, int64_t offset);
long bionic_pwrite(int fd, const void *buf, size_t count, int64_t offset);
int64_t bionic_lseek(int fd, int64_t offset, int whence);
int bionic_fcntl(int fd, int cmd, ...);
int bionic_ioctl(int fd, unsigned long request, ...);
int bionic_fstat(int fd, struct bionic_stat *out);
int bionic_fsync(int fd);
int bionic_ftruncate(int fd, int64_t length);
int bionic_dup(int fd);
int bionic_dup2(int oldfd, int newfd);
int bionic_isatty(int fd);

/* Path layer. */
int bionic_stat(const char *path, struct bionic_stat *out);
int bionic_lstat(const char *path, struct bionic_stat *out);
int bionic_fstatat(int dirfd, const char *path, struct bionic_stat *out, int flags);
int bionic_access(const char *path, int mode);
int bionic_mkdir(const char *path, unsigned mode);
int bionic_mkdirat(int dirfd, const char *path, unsigned mode);
int bionic_rmdir(const char *path);
int bionic_unlink(const char *path);
int bionic_unlinkat(int dirfd, const char *path, int flags);
int bionic_remove(const char *path);
int bionic_rename(const char *from, const char *to);
int bionic_renameat(int fromfd, const char *from, int tofd, const char *to);
int bionic_chdir(const char *path);
char *bionic_getcwd(char *buf, size_t size);
char *bionic_realpath(const char *path, char *resolved);
long bionic_readlink(const char *path, char *buf, size_t size);
long bionic_readlinkat(int dirfd, const char *path, char *buf, size_t size);
int bionic_truncate(const char *path, int64_t length);
int bionic_chmod(const char *path, unsigned mode);
int bionic_statvfs(const char *path, struct bionic_statvfs *out);
int bionic_fstatvfs(int fd, struct bionic_statvfs *out);
int bionic_statfs(const char *path, struct bionic_statfs *out);

/* Directory iteration, with bionic's dirent layout. */
void *bionic_opendir(const char *path);
void *bionic_fdopendir(int fd);
void *bionic_readdir(void *dir);
int bionic_closedir(void *dir);
void bionic_rewinddir(void *dir);

/* stdio. Some of the guest's translation units still reach the standard
 * streams as &__sF[n], so the array's stride has to be bionic's sizeof(FILE)
 * on LP64. Nothing reads the contents; host_stream swaps each entry for the
 * real host stream. */
#define BIONIC_FILE_SIZE 152
extern uint8_t bionic_sF[3][BIONIC_FILE_SIZE];
FILE *bionic_fopen(const char *path, const char *mode);
FILE *bionic_fdopen(int fd, const char *mode);
FILE *host_stream(FILE *guest);

/* Memory mapping, backed by the heap. */
void *bionic_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int bionic_munmap(void *addr, size_t length);
void *bionic_mremap(void *addr, size_t old_size, size_t new_size, int flags, ...);
int bionic_mprotect(void *addr, size_t length, int prot);
int bionic_msync(void *addr, size_t length, int flags);
int bionic_madvise(void *addr, size_t length, int advice);

#endif
