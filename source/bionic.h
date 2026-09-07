/* Structure layouts the guest expects, which differ from newlib's.
 *
 * Anything the Android library allocates itself or reads field-by-field has
 * to match bionic/arm64 exactly; anything it only ever passes back to us
 * opaquely (DIR, FILE, locale_t) can stay a host pointer.
 */

#ifndef BSNX_BIONIC_H
#define BSNX_BIONIC_H

#include <stdint.h>
#include <time.h>

/* Linux arm64 struct stat, which bionic exposes verbatim. 128 bytes. */
struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

#define BIONIC_DT_UNKNOWN 0
#define BIONIC_DT_DIR 4
#define BIONIC_DT_REG 8
#define BIONIC_DT_LNK 10

/* Linux arm64 statvfs/statfs, both 112 bytes with an f_type-first layout for
 * statfs. Only f_bsize/f_blocks/f_bavail are ever read in practice. */
struct bionic_statvfs {
  uint64_t f_bsize;
  uint64_t f_frsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  uint64_t f_favail;
  uint64_t f_fsid;
  uint64_t f_flag;
  uint64_t f_namemax;
  uint32_t __reserved[6];
};

struct bionic_statfs {
  uint64_t f_type;
  uint64_t f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  uint64_t f_fsid;
  uint64_t f_namelen;
  uint64_t f_frsize;
  uint64_t f_flags;
  uint64_t f_spare[4];
};

/* Bionic O_* differ from newlib's in the high bits. */
#define BIONIC_O_RDONLY 0x00000
#define BIONIC_O_WRONLY 0x00001
#define BIONIC_O_RDWR 0x00002
#define BIONIC_O_CREAT 0x00040
#define BIONIC_O_EXCL 0x00080
#define BIONIC_O_NOCTTY 0x00100
#define BIONIC_O_TRUNC 0x00200
#define BIONIC_O_APPEND 0x00400
#define BIONIC_O_NONBLOCK 0x00800
#define BIONIC_O_DIRECTORY 0x04000
#define BIONIC_O_NOFOLLOW 0x08000
#define BIONIC_O_CLOEXEC 0x80000

/* Bionic seeks and whences match newlib, as do mode_t bits. */

#define BIONIC_AT_FDCWD (-100)

#endif
