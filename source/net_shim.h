/* Sockets, translated between Linux and BSD conventions.
 *
 * The guest speaks Linux/bionic: sockaddr with no length byte, AF_INET6 10,
 * SOL_SOCKET 1, Linux MSG_* and IP_* numbering. libnx's BSD sockets use the
 * FreeBSD conventions for all of those, so nothing may pass through
 * untouched -- BombSquad's LAN discovery relies on SO_BROADCAST and its
 * online play on IPv6 addresses resolving correctly.
 */

#ifndef BSNX_NET_SHIM_H
#define BSNX_NET_SHIM_H

#include <stdbool.h>

#include <stddef.h>
#include <stdint.h>

int net_socket(int domain, int type, int protocol);
int net_bind(int fd, const void *address, uint32_t length);
int net_connect(int fd, const void *address, uint32_t length);
int net_accept(int fd, void *address, uint32_t *length);
int net_accept4(int fd, void *address, uint32_t *length, int flags);
int net_dup3(int oldfd, int newfd, int flags);
int net_getsockname(int fd, void *address, uint32_t *length);
int net_getpeername(int fd, void *address, uint32_t *length);

long net_send(int fd, const void *buffer, size_t length, int flags);
long net_recv(int fd, void *buffer, size_t length, int flags);
long net_sendto(int fd, const void *buffer, size_t length, int flags,
                const void *address, uint32_t address_length);
long net_recvfrom(int fd, void *buffer, size_t length, int flags, void *address,
                  uint32_t *address_length);
long net_sendmsg(int fd, const void *message, int flags);
long net_recvmsg(int fd, void *message, int flags);

int net_setsockopt(int fd, int level, int option, const void *value, uint32_t length);
int net_getsockopt(int fd, int level, int option, void *value, uint32_t *length);

int net_getaddrinfo(const char *node, const char *service, const void *hints,
                    void **result);
void net_freeaddrinfo(void *list);
int net_getnameinfo(const void *address, uint32_t address_length, char *host,
                    uint32_t host_length, char *service, uint32_t service_length,
                    int flags);
void *net_gethostbyname(const char *name);

const char *net_inet_ntop(int family, const void *source, char *destination,
                          uint32_t size);
int net_inet_pton(int family, const char *source, void *destination);

int net_fcntl(int fd, int command, ...);

/* The console's BSD service has no AF_UNIX, so a connected pair is built
 * over the loopback interface instead. asyncio's event loop needs one for
 * its self-pipe, and CPython reaches for pipes in the same places. */
int net_socketpair(int domain, int type, int protocol, int fds[2]);
int net_pipe(int fds[2]);
int net_pipe2(int fds[2], int flags);

/* Set or clear non-blocking mode, whichever way the guest asked. */
int net_set_nonblocking(int fd, int enable);

/* poll and select. Neither ever blocks inside the console's socket service,
 * and neither can busy-spin: the game's network reader waits on
 * poll(fds, n, -1) in a loop with no sleep of its own. */
int net_poll(void *fds, unsigned nfds, int timeout_ms);
int net_select(int nfds, void *readfds, void *writefds, void *exceptfds,
               void *timeout);

/* True once the console has an address on a network again. Cheap enough to
 * ask every frame: the answer behind it is cached for a second. */
bool net_have_address(void);

/* The console's own address and mask, shaped like the interface list the
 * game's LAN scan expects. These replace the copies inside the game, which
 * ask netlink and so always come up empty here. */
int net_getifaddrs(void **out);
void net_freeifaddrs(void *list);

#endif
