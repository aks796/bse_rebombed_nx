/* In-process pipe pairs, standing in for AF_UNIX socketpairs and pipes. */

#ifndef BSNX_PIPE_SHIM_H
#define BSNX_PIPE_SHIM_H

#include <stddef.h>
#include <stdint.h>

void pipe_shim_init(void);

/* True for a descriptor this file handed out. Callers that dispatch on a
 * descriptor -- read, write, close, poll, fcntl, ioctl -- ask this first. */
int pipe_shim_owns(int fd);

/* Two connected endpoints, readable and writable in both directions. */
int pipe_shim_pair(int fds[2]);

long pipe_shim_read(int fd, void *buffer, size_t length);
long pipe_shim_write(int fd, const void *buffer, size_t length);
int pipe_shim_close(int fd);

/* The poll bits currently true for this endpoint, masked to what was asked
 * for plus the error bits poll always reports. */
short pipe_shim_poll(int fd, short events);

int pipe_shim_set_nonblocking(int fd, int enable);
int pipe_shim_is_nonblocking(int fd);
long pipe_shim_bytes_readable(int fd);

/* CPython checks that a descriptor it was handed is really a socket by
 * calling getsockname on it, so these answer as an unnamed AF_UNIX pair. */
int pipe_shim_getsockname(int fd, void *address, uint32_t *length);
int pipe_shim_getsockopt(int fd, int level, int option, void *value,
                         uint32_t *length);

#endif
