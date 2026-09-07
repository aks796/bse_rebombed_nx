/* epoll, emulated over poll().
 *
 * CPython's select module always exposes select.epoll on Android, and
 * selectors.DefaultSelector picks it ahead of poll. asyncio therefore needs
 * epoll to exist even though the console's BSD stack has no such thing.
 */

#ifndef BSNX_EPOLL_SHIM_H
#define BSNX_EPOLL_SHIM_H

#include <stdint.h>

void epoll_shim_init(void);

/* True when the descriptor belongs to this layer rather than the BSD stack. */
int epoll_shim_owns(int fd);
int epoll_shim_close(int fd);

int shim_epoll_create1(int flags);
int shim_epoll_ctl(int epfd, int op, int fd, void *event);
int shim_epoll_wait(int epfd, void *events, int max_events, int timeout);

#endif
