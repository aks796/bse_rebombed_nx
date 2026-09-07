/* Small helpers shared across the loader. */

#ifndef BSNX_UTIL_H
#define BSNX_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Bionic arm64 keeps the stack-protector canary at TPIDR_EL0 + 0x28, and
 * guest code reads it directly. libnx keeps its own thread state in
 * TPIDRRO_EL0 instead, so claiming TPIDR_EL0 for the guest is safe -- but
 * every thread that runs guest code needs its own block. */
#define BIONIC_TLS_SIZE 0x400
void install_bionic_tls(void *buf);

/* Recursive mkdir; returns 1 on success or if the directory already exists. */
int mkpath(const char *path);
int path_exists(const char *path);
int is_directory(const char *path);
int copy_file(const char *from, const char *to);
/* Recursive move that falls back to copy+unlink across devices. */
int move_tree(const char *from, const char *to);
void remove_tree(const char *path);
int count_tree_entries(const char *path);

/* Strip a devoptab prefix ("sdmc:") so guest code only ever sees Unix paths. */
const char *strip_device(const char *path);

#endif
