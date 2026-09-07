/* Sockets, translated between Linux and BSD conventions. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <switch.h>
#include <unistd.h>

#include "error.h"
#include "net_shim.h"
#include "pipe_shim.h"

/* Bounded so a failing call in a hot loop cannot flood the log. */
static unsigned g_net_traces;
#define NET_TRACE_LIMIT 32

static void trace_net(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace_net(const char *fmt, ...) {
  if (__atomic_fetch_add(&g_net_traces, 1, __ATOMIC_RELAXED) >= NET_TRACE_LIMIT)
    return;
  char line[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  trace("%s", line);
}

/* ------------------------------------------------------- address families */

#define GUEST_AF_UNSPEC 0
#define GUEST_AF_UNIX 1
#define GUEST_AF_INET 2
#define GUEST_AF_INET6 10

static int family_to_host(int family) {
  switch (family) {
    case GUEST_AF_UNSPEC: return AF_UNSPEC;
    case GUEST_AF_UNIX: return AF_LOCAL;
    case GUEST_AF_INET: return AF_INET;
    case GUEST_AF_INET6: return AF_INET6;
    default: return family;
  }
}

static int family_to_guest(int family) {
  switch (family) {
    case AF_UNSPEC: return GUEST_AF_UNSPEC;
    case AF_LOCAL: return GUEST_AF_UNIX;
    case AF_INET: return GUEST_AF_INET;
    case AF_INET6: return GUEST_AF_INET6;
    default: return family;
  }
}

/* ---------------------------------------------------------- socket addresses
 *
 * Both layouts are otherwise identical: only the first two bytes differ,
 * where Linux keeps a 16-bit family and BSD keeps a length byte followed by
 * an 8-bit family. */

#define SOCKADDR_MAX 128

typedef struct {
  uint8_t bytes[SOCKADDR_MAX];
} HostAddress;

static int address_to_host(const void *guest, uint32_t length, HostAddress *out) {
  if (!guest || length < 2 || length > SOCKADDR_MAX) return 0;
  memcpy(out->bytes, guest, length);
  uint16_t guest_family;
  memcpy(&guest_family, guest, sizeof guest_family);
  out->bytes[0] = (uint8_t)length;
  out->bytes[1] = (uint8_t)family_to_host(guest_family);
  return 1;
}

static void address_to_guest(const HostAddress *host, void *guest, uint32_t length) {
  if (!guest || length < 2) return;
  memcpy(guest, host->bytes, length);
  const uint16_t guest_family = (uint16_t)family_to_guest(host->bytes[1]);
  memcpy(guest, &guest_family, sizeof guest_family);
}

/* --------------------------------------------------------- message flags */

typedef struct {
  int guest;
  int host;
} FlagPair;

static const FlagPair kMsgFlags[] = {
    {0x0001, MSG_OOB},      {0x0002, MSG_PEEK},     {0x0004, MSG_DONTROUTE},
    {0x0008, MSG_CTRUNC},   {0x0020, MSG_TRUNC},    {0x0040, MSG_DONTWAIT},
    {0x0080, MSG_EOR},      {0x0100, MSG_WAITALL},  {0x4000, MSG_NOSIGNAL},
};

static int translate_flags(int value, const FlagPair *pairs, size_t count,
                           int to_host) {
  int result = 0;
  for (size_t i = 0; i < count; i++) {
    const int from = to_host ? pairs[i].guest : pairs[i].host;
    const int to = to_host ? pairs[i].host : pairs[i].guest;
    if (value & from) result |= to;
  }
  return result;
}

static int msg_flags_to_host(int flags) {
  return translate_flags(flags, kMsgFlags,
                         sizeof kMsgFlags / sizeof kMsgFlags[0], 1);
}
static int msg_flags_to_guest(int flags) {
  return translate_flags(flags, kMsgFlags,
                         sizeof kMsgFlags / sizeof kMsgFlags[0], 0);
}

/* --------------------------------------------------------- socket options */

#define GUEST_SOL_SOCKET 1
#define GUEST_IPPROTO_IP 0
#define GUEST_IPPROTO_TCP 6
#define GUEST_IPPROTO_IPV6 41

static const FlagPair kSocketOptions[] = {
    {1, SO_DEBUG},       {2, SO_REUSEADDR},  {3, SO_TYPE},
    {4, SO_ERROR},       {5, SO_DONTROUTE},  {6, SO_BROADCAST},
    {7, SO_SNDBUF},      {8, SO_RCVBUF},     {9, SO_KEEPALIVE},
    {10, SO_OOBINLINE},  {13, SO_LINGER},    {15, SO_REUSEPORT},
    {18, SO_RCVLOWAT},   {19, SO_SNDLOWAT},  {20, SO_RCVTIMEO},
    {21, SO_SNDTIMEO},   {30, SO_ACCEPTCONN},
};

static const FlagPair kIpOptions[] = {
    {1, IP_TOS},          {2, IP_TTL},           {3, IP_HDRINCL},
    {4, IP_OPTIONS},      {32, IP_MULTICAST_IF}, {33, IP_MULTICAST_TTL},
    {34, IP_MULTICAST_LOOP}, {35, IP_ADD_MEMBERSHIP},
    {36, IP_DROP_MEMBERSHIP},
};

static const FlagPair kIpv6Options[] = {
    {16, IPV6_UNICAST_HOPS},  {17, IPV6_MULTICAST_IF},
    {18, IPV6_MULTICAST_HOPS}, {19, IPV6_MULTICAST_LOOP},
    {20, IPV6_JOIN_GROUP},    {21, IPV6_LEAVE_GROUP},
    {26, IPV6_V6ONLY},
};

static int lookup_option(const FlagPair *pairs, size_t count, int guest) {
  for (size_t i = 0; i < count; i++)
    if (pairs[i].guest == guest) return pairs[i].host;
  return -1;
}

/* Returns 0 when the option has no counterpart here. */
static int option_to_host(int guest_level, int guest_option, int *host_level,
                          int *host_option) {
  switch (guest_level) {
    case GUEST_SOL_SOCKET:
      *host_level = SOL_SOCKET;
      *host_option = lookup_option(kSocketOptions,
                                   sizeof kSocketOptions / sizeof kSocketOptions[0],
                                   guest_option);
      break;
    case GUEST_IPPROTO_IP:
      *host_level = IPPROTO_IP;
      *host_option =
          lookup_option(kIpOptions, sizeof kIpOptions / sizeof kIpOptions[0],
                        guest_option);
      break;
    case GUEST_IPPROTO_IPV6:
      *host_level = IPPROTO_IPV6;
      *host_option = lookup_option(kIpv6Options,
                                   sizeof kIpv6Options / sizeof kIpv6Options[0],
                                   guest_option);
      break;
    case GUEST_IPPROTO_TCP:
      /* TCP_NODELAY and friends already agree. */
      *host_level = IPPROTO_TCP;
      *host_option = guest_option;
      break;
    default:
      *host_level = guest_level;
      *host_option = guest_option;
      break;
  }
  return *host_option >= 0;
}

/* ------------------------------------------------------------------- calls */

/* Linux folds these into the socket type; the kernel strips them before the
 * protocol layer sees them. The console's BSD service does not, so a type of
 * SOCK_DGRAM|SOCK_CLOEXEC reaches it as a nonsense number and is rejected.
 *
 * CPython always tries SOCK_CLOEXEC first and only retries without it when
 * the failure is EINVAL, so an unrecognised type here fails socket creation
 * outright -- which takes out the account login and the server browser. */
#define GUEST_SOCK_CLOEXEC 0x80000
#define GUEST_SOCK_NONBLOCK 0x800
#define GUEST_SOCK_FLAGS (GUEST_SOCK_CLOEXEC | GUEST_SOCK_NONBLOCK)

int net_socket(int domain, int type, int protocol) {
  const int fd = socket(family_to_host(domain), type & ~GUEST_SOCK_FLAGS, protocol);
  /* The engine opens an IPv6 listener alongside its IPv4 one and polls both
   * together, so how this console answers decides whether that poll has one
   * usable descriptor or two. */
  if (domain == GUEST_AF_INET6)
    trace_net("ipv6 socket -> %s", fd < 0 ? strerror(errno) : "ok");
  if (fd < 0) return fd;
  if (type & GUEST_SOCK_NONBLOCK) net_set_nonblocking(fd, 1);
  return fd;
}

int net_accept4(int fd, void *address, uint32_t *length, int flags) {
  const int accepted = net_accept(fd, address, length);
  if (accepted < 0) return accepted;
  if (flags & GUEST_SOCK_NONBLOCK) net_set_nonblocking(accepted, 1);
  return accepted;
}

int net_dup3(int oldfd, int newfd, int flags) {
  const int result = dup2(oldfd, newfd);
  if (result < 0) return result;
  if (flags & GUEST_SOCK_NONBLOCK) net_set_nonblocking(result, 1);
  return result;
}

int net_bind(int fd, const void *address, uint32_t length) {
  HostAddress host;
  if (!address_to_host(address, length, &host)) { errno = EINVAL; return -1; }
  const int result = bind(fd, (const struct sockaddr *)host.bytes, (socklen_t)length);

  /* BombSquad Remote finds a console by broadcasting to the game's UDP
   * listener, so whether that bind happens at all is worth recording. */
  if (host.bytes[1] == AF_INET) {
    uint16_t port;
    memcpy(&port, host.bytes + 2, sizeof port);
    trace_net("bind fd %d to port %u -> %s", fd, ntohs(port),
              result == 0 ? "ok" : strerror(errno));
  }
  return result;
}

int net_connect(int fd, const void *address, uint32_t length) {
  HostAddress host;
  if (!address_to_host(address, length, &host)) { errno = EINVAL; return -1; }
  return connect(fd, (const struct sockaddr *)host.bytes, (socklen_t)length);
}

int net_accept(int fd, void *address, uint32_t *length) {
  if (!address || !length) return accept(fd, NULL, NULL);
  HostAddress host;
  socklen_t host_length = (socklen_t)(*length > SOCKADDR_MAX ? SOCKADDR_MAX : *length);
  const int result = accept(fd, (struct sockaddr *)host.bytes, &host_length);
  if (result >= 0) {
    address_to_guest(&host, address, host_length);
    *length = host_length;
  }
  return result;
}

static int name_call(int fd, void *address, uint32_t *length,
                     int (*call)(int, struct sockaddr *, socklen_t *)) {
  if (!address || !length) { errno = EINVAL; return -1; }
  HostAddress host;
  socklen_t host_length = (socklen_t)(*length > SOCKADDR_MAX ? SOCKADDR_MAX : *length);
  const int result = call(fd, (struct sockaddr *)host.bytes, &host_length);
  if (result == 0) {
    address_to_guest(&host, address, host_length);
    *length = host_length;
  }
  return result;
}

int net_getsockname(int fd, void *address, uint32_t *length) {
  /* CPython proves a descriptor really is a socket by calling this on it, so
   * an in-process pair has to answer rather than report ENOTSOCK. */
  if (pipe_shim_owns(fd)) return pipe_shim_getsockname(fd, address, length);
  return name_call(fd, address, length, getsockname);
}
int net_getpeername(int fd, void *address, uint32_t *length) {
  if (pipe_shim_owns(fd)) return pipe_shim_getsockname(fd, address, length);
  return name_call(fd, address, length, getpeername);
}

long net_send(int fd, const void *buffer, size_t length, int flags) {
  if (pipe_shim_owns(fd)) return pipe_shim_write(fd, buffer, length);
  return send(fd, buffer, length, msg_flags_to_host(flags));
}
long net_recv(int fd, void *buffer, size_t length, int flags) {
  if (pipe_shim_owns(fd)) return pipe_shim_read(fd, buffer, length);
  return recv(fd, buffer, length, msg_flags_to_host(flags));
}

long net_sendto(int fd, const void *buffer, size_t length, int flags,
                const void *address, uint32_t address_length) {
  if (pipe_shim_owns(fd)) return pipe_shim_write(fd, buffer, length);
  if (!address)
    return sendto(fd, buffer, length, msg_flags_to_host(flags), NULL, 0);
  HostAddress host;
  if (!address_to_host(address, address_length, &host)) { errno = EINVAL; return -1; }
  const long result = sendto(fd, buffer, length, msg_flags_to_host(flags),
                             (const struct sockaddr *)host.bytes,
                             (socklen_t)address_length);

  /* The LAN scan finds games by broadcasting, so a console that refuses to
   * send to a broadcast address would come up empty with nothing to show for
   * it. Worth one line if it ever happens. */
  if (result < 0 && host.bytes[1] == AF_INET) {
    uint32_t destination;
    memcpy(&destination, host.bytes + 4, sizeof destination);
    if ((destination & 0xff000000u) == 0xff000000u ||
        destination == 0xffffffffu)
      trace_net("broadcast sendto failed: %s", strerror(errno));
  }
  return result;
}

/* Whether anything on the network is actually reaching the game is the first
 * question when LAN play or BombSquad Remote does not show up, and it is not
 * answerable from the outside. The first byte of every Ballistica datagram is
 * its packet type, so recording the sender and that byte says who found the
 * console and what they asked. Only the opening handful are logged; after
 * that a running count is enough. */
static unsigned g_datagrams;

static void note_datagram(const HostAddress *from, socklen_t from_length,
                          const void *payload, long payload_length) {
  if (from_length < 8 || from->bytes[1] != AF_INET) return;

  const unsigned seen = __atomic_add_fetch(&g_datagrams, 1, __ATOMIC_RELAXED);
  if (seen > 8 && seen % 512 != 0) return;

  uint16_t port;
  memcpy(&port, from->bytes + 2, sizeof port);
  const unsigned char *octets = (const unsigned char *)from->bytes + 4;
  trace("datagram %u: %u bytes from %u.%u.%u.%u:%u, type %d", seen,
        (unsigned)payload_length, octets[0], octets[1], octets[2], octets[3],
        ntohs(port), payload_length > 0 ? *(const unsigned char *)payload : -1);
}

long net_recvfrom(int fd, void *buffer, size_t length, int flags, void *address,
                  uint32_t *address_length) {
  if (pipe_shim_owns(fd)) return pipe_shim_read(fd, buffer, length);
  if (!address || !address_length)
    return recvfrom(fd, buffer, length, msg_flags_to_host(flags), NULL, NULL);

  HostAddress host;
  socklen_t host_length =
      (socklen_t)(*address_length > SOCKADDR_MAX ? SOCKADDR_MAX : *address_length);
  const long result = recvfrom(fd, buffer, length, msg_flags_to_host(flags),
                               (struct sockaddr *)host.bytes, &host_length);
  if (result >= 0) {
    note_datagram(&host, host_length, buffer, result);
    address_to_guest(&host, address, host_length);
    *address_length = host_length;
  }
  return result;
}

/* Linux msghdr counts iovecs in a size_t where BSD uses an int, so the two
 * structures differ past msg_iov. */
typedef struct {
  void *msg_name;
  uint32_t msg_namelen;
  void *msg_iov;
  size_t msg_iovlen;
  void *msg_control;
  size_t msg_controllen;
  int msg_flags;
} GuestMsghdr;

long net_sendmsg(int fd, const void *message, int flags) {
  const GuestMsghdr *guest = message;
  if (!guest) { errno = EINVAL; return -1; }

  HostAddress host_address;
  struct msghdr host;
  memset(&host, 0, sizeof host);
  if (guest->msg_name && address_to_host(guest->msg_name, guest->msg_namelen,
                                         &host_address)) {
    host.msg_name = host_address.bytes;
    host.msg_namelen = guest->msg_namelen;
  }
  host.msg_iov = guest->msg_iov;
  host.msg_iovlen = (int)guest->msg_iovlen;
  host.msg_control = guest->msg_control;
  host.msg_controllen = (socklen_t)guest->msg_controllen;
  host.msg_flags = msg_flags_to_host(guest->msg_flags);

  return sendmsg(fd, &host, msg_flags_to_host(flags));
}

long net_recvmsg(int fd, void *message, int flags) {
  GuestMsghdr *guest = message;
  if (!guest) { errno = EINVAL; return -1; }

  HostAddress host_address;
  struct msghdr host;
  memset(&host, 0, sizeof host);
  if (guest->msg_name) {
    host.msg_name = host_address.bytes;
    host.msg_namelen = guest->msg_namelen > SOCKADDR_MAX ? SOCKADDR_MAX
                                                         : guest->msg_namelen;
  }
  host.msg_iov = guest->msg_iov;
  host.msg_iovlen = (int)guest->msg_iovlen;
  host.msg_control = guest->msg_control;
  host.msg_controllen = (socklen_t)guest->msg_controllen;

  const long result = recvmsg(fd, &host, msg_flags_to_host(flags));
  if (result >= 0) {
    if (guest->msg_name) {
      address_to_guest(&host_address, guest->msg_name, host.msg_namelen);
      guest->msg_namelen = host.msg_namelen;
    }
    guest->msg_controllen = host.msg_controllen;
    guest->msg_flags = msg_flags_to_guest(host.msg_flags);
  }
  return result;
}

int net_setsockopt(int fd, int level, int option, const void *value,
                   uint32_t length) {
  /* Nothing an in-process pair carries is configurable; accepting the call
   * is closer to the truth than failing it. */
  if (pipe_shim_owns(fd)) return 0;
  int host_level, host_option;
  if (!option_to_host(level, option, &host_level, &host_option)) {
    /* Silently accepting an unknown option is safer than failing a call the
     * engine does not check. */
    trace("ignoring unsupported setsockopt level=%d option=%d", level, option);
    return 0;
  }
  return setsockopt(fd, host_level, host_option, value, (socklen_t)length);
}

int net_getsockopt(int fd, int level, int option, void *value, uint32_t *length) {
  if (pipe_shim_owns(fd))
    return pipe_shim_getsockopt(fd, level, option, value, length);
  int host_level, host_option;
  if (!option_to_host(level, option, &host_level, &host_option)) {
    errno = ENOPROTOOPT;
    return -1;
  }
  socklen_t host_length = length ? (socklen_t)*length : 0;
  const int result = getsockopt(fd, host_level, host_option, value, &host_length);
  if (length) *length = host_length;
  return result;
}

/* ---------------------------------------------------------- name lookups */

/* bionic's addrinfo puts ai_canonname before ai_addr, which matches the BSD
 * layout here; only the family and flag numbering has to change. */
typedef struct GuestAddrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  uint32_t ai_addrlen;
  char *ai_canonname;
  void *ai_addr;
  struct GuestAddrinfo *ai_next;
} GuestAddrinfo;

static const FlagPair kAiFlags[] = {
    {0x0001, AI_PASSIVE},  {0x0002, AI_CANONNAME}, {0x0004, AI_NUMERICHOST},
    {0x0008, AI_V4MAPPED}, {0x0010, AI_ALL},       {0x0020, AI_ADDRCONFIG},
    {0x0400, AI_NUMERICSERV},
};

static const FlagPair kNiFlags[] = {
    {0x0001, NI_NUMERICHOST}, {0x0002, NI_NUMERICSERV}, {0x0004, NI_NOFQDN},
    {0x0008, NI_NAMEREQD},    {0x0010, NI_DGRAM},
};

int net_getaddrinfo(const char *node, const char *service, const void *hints_in,
                    void **result_out) {
  const GuestAddrinfo *guest_hints = hints_in;
  struct addrinfo hints;
  struct addrinfo *host_result = NULL;

  if (guest_hints) {
    memset(&hints, 0, sizeof hints);
    hints.ai_flags = translate_flags(guest_hints->ai_flags, kAiFlags,
                                     sizeof kAiFlags / sizeof kAiFlags[0], 1);
    hints.ai_family = family_to_host(guest_hints->ai_family);
    hints.ai_socktype = guest_hints->ai_socktype;
    hints.ai_protocol = guest_hints->ai_protocol;
  }

  const int rc = getaddrinfo(node, service, guest_hints ? &hints : NULL,
                             &host_result);
  if (rc != 0) return rc;

  GuestAddrinfo *head = NULL, *tail = NULL;
  for (struct addrinfo *entry = host_result; entry; entry = entry->ai_next) {
    GuestAddrinfo *copy = calloc(1, sizeof *copy);
    if (!copy) break;
    copy->ai_flags = 0;
    copy->ai_family = family_to_guest(entry->ai_family);
    copy->ai_socktype = entry->ai_socktype;
    copy->ai_protocol = entry->ai_protocol;
    copy->ai_addrlen = entry->ai_addrlen;
    if (entry->ai_canonname) copy->ai_canonname = strdup(entry->ai_canonname);
    if (entry->ai_addr && entry->ai_addrlen) {
      copy->ai_addr = calloc(1, entry->ai_addrlen);
      if (copy->ai_addr) {
        HostAddress host;
        memset(&host, 0, sizeof host);
        memcpy(host.bytes, entry->ai_addr,
               entry->ai_addrlen > SOCKADDR_MAX ? SOCKADDR_MAX : entry->ai_addrlen);
        address_to_guest(&host, copy->ai_addr, entry->ai_addrlen);
      }
    }
    if (tail) tail->ai_next = copy;
    else head = copy;
    tail = copy;
  }
  freeaddrinfo(host_result);

  if (!head) return EAI_MEMORY;
  *result_out = head;
  return 0;
}

void net_freeaddrinfo(void *list) {
  GuestAddrinfo *entry = list;
  while (entry) {
    GuestAddrinfo *next = entry->ai_next;
    free(entry->ai_canonname);
    free(entry->ai_addr);
    free(entry);
    entry = next;
  }
}

int net_getnameinfo(const void *address, uint32_t address_length, char *host,
                    uint32_t host_length, char *service, uint32_t service_length,
                    int flags) {
  HostAddress host_address;
  if (!address_to_host(address, address_length, &host_address)) return EAI_FAIL;
  return getnameinfo((const struct sockaddr *)host_address.bytes,
                     (socklen_t)address_length, host, host_length, service,
                     service_length,
                     translate_flags(flags, kNiFlags,
                                     sizeof kNiFlags / sizeof kNiFlags[0], 1));
}

/* struct hostent agrees between the two, apart from h_addrtype. */
void *net_gethostbyname(const char *name) {
  struct hostent *entry = gethostbyname(name);
  if (entry) entry->h_addrtype = family_to_guest(entry->h_addrtype);
  return entry;
}

const char *net_inet_ntop(int family, const void *source, char *destination,
                          uint32_t size) {
  return inet_ntop(family_to_host(family), source, destination, size);
}

int net_inet_pton(int family, const char *source, void *destination) {
  return inet_pton(family_to_host(family), source, destination);
}

/* ------------------------------------------------------------ socket pairs
 *
 * The BSD service behind libnx implements no AF_UNIX, so socketpair() fails
 * outright. A loopback TCP connection gives the same thing where it counts:
 * two connected descriptors that read, write, poll and close normally. */

/* A socketpair used to be two connected loopback TCP sockets. It is an
 * in-process pipe now; see pipe_shim.c for why the network stack turned out
 * to be the wrong thing to build this on. */
int net_socketpair(int domain, int type, int protocol, int fds[2]) {
  (void)domain;
  (void)protocol;
  /* Only a stream pair is emulated; asyncio and CPython never ask for
   * anything else. */
  (void)type;
  if (!fds) { errno = EINVAL; return -1; }
  const int result = pipe_shim_pair(fds);
  if (result == 0) trace_net("socketpair: fds %d and %d", fds[0], fds[1]);
  else trace("socketpair failed: %s", strerror(errno));
  return result;
}

int net_pipe(int fds[2]) { return pipe_shim_pair(fds); }

int net_pipe2(int fds[2], int flags) {
  if (pipe_shim_pair(fds) < 0) return -1;
  /* Only O_NONBLOCK is meaningful here; O_CLOEXEC has nothing to exec into. */
  if (flags & 0x800) {
    net_set_nonblocking(fds[0], 1);
    net_set_nonblocking(fds[1], 1);
  }
  return 0;
}

/* libnx's fcntl only understands F_GETFL and F_SETFL, returns a bare 95 for
 * anything else, and reports -1 without touching errno when the descriptor
 * is not a socket. Its BSD ioctl is the reliable path, so try that first. */
int net_set_nonblocking(int fd, int enable) {
  if (pipe_shim_owns(fd)) return pipe_shim_set_nonblocking(fd, enable);
  int value = enable ? 1 : 0;
  if (ioctl(fd, FIONBIO, &value) >= 0) return 0;

  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    trace_net("FIONBIO and F_GETFL both failed on fd %d", fd);
    errno = ENOTTY;
    return -1;
  }
  const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return fcntl(fd, F_SETFL, updated) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------ waiting
 *
 * Socket calls travel over a small pool of service sessions, and a call that
 * blocks holds its session for as long as it waits. The engine's UDP listener
 * waits forever between packets, so letting that one call block outright
 * would check a session out permanently and leave every other thread queueing
 * behind it.
 *
 * So a wait is served in slices. While it is fresh the console's poll is
 * allowed to block for the length of a slice, which costs nothing in latency
 * -- it returns the moment a packet lands -- and holds a session only for
 * that slice. Once a wait has been quiet long enough to look idle, the poll
 * stops blocking altogether and the slice is spent asleep outside the
 * session, so a listener nobody is talking to is not sitting on one. */

#define POLL_ACTIVE_SLICE_MS 5
#define POLL_IDLE_SLICE_MS 20
#define POLL_GOES_IDLE_AFTER_MS 250

static u64 elapsed_ms_since(u64 started) {
  return (armGetSystemTick() - started) * 1000ull / armGetSystemTickFreq();
}

/* True when something the caller actually asked about is ready. A revents of
 * POLLERR/POLLHUP/POLLNVAL alone is not: callers typically ignore it and poll
 * again, so returning it without pause is what turns a wait loop into a
 * spin. */
static int wanted_event_ready(const struct pollfd *fds, unsigned nfds) {
  for (unsigned i = 0; i < nfds; i++)
    if (fds[i].revents & fds[i].events) return 1;
  return 0;
}

/* A wait can mix descriptors the console owns with in-process pipe ends, so
 * each kind is answered from the right place and the results merged.
 *
 * The host descriptors go out in one call, because each call costs a round
 * trip. That call fails as a whole if any single descriptor is unacceptable,
 * which would hide every other socket in the set -- not hypothetical here,
 * since the engine polls its IPv4 and IPv6 listeners together and this
 * console has no IPv6. So a failure is retried a descriptor at a time and the
 * offending one is reported as POLLNVAL, which is what a caller expects for a
 * descriptor it cannot use. */

#define POLL_HOST_MAX 64

static short poll_one_host(int fd, short events, int timeout_ms) {
  struct pollfd one = {fd, events, 0};
  if (poll(&one, 1, timeout_ms) < 0) return POLLNVAL;
  return one.revents;
}

/* A descriptor the console has given up on reports POLLERR or POLLNVAL and
 * nothing else -- and it gives up on all of them at once when the console
 * sleeps, since the network stack does not survive that. A caller waiting to
 * read from such a descriptor would then wait forever: it watches for POLLIN,
 * and POLLIN is never coming.
 *
 * So the bits it asked about are reported alongside the error. It goes ahead
 * with the read, the read fails, and it can act on that. This is what lets
 * the engine's UDP listener notice its socket is gone -- a failed read is the
 * only thing that makes it close the pair and open a fresh one -- instead of
 * sitting on a dead descriptor for the rest of the session. */
static short widen_error(short revents, short events, int *widened) {
  if ((revents & (POLLERR | POLLHUP | POLLNVAL)) && !(revents & events)) {
    *widened = 1;
    return (short)(revents | events);
  }
  return revents;
}

/* host_timeout_ms is how long the console's poll may wait. It is honoured
 * only when every descriptor belongs to the console: a blocking host call
 * would not notice an in-process pipe going ready underneath it. *waited says
 * whether the call did in fact wait, so the caller knows whether it still
 * owes this slice a sleep. */
static int poll_now(struct pollfd *fds, unsigned nfds, int host_timeout_ms,
                    int *waited, int *only_errors) {
  struct pollfd host[POLL_HOST_MAX];
  unsigned source[POLL_HOST_MAX];
  unsigned host_count = 0;
  unsigned pipe_count = 0;
  int ready = 0;
  int genuine = 0;
  int widened = 0;

  *waited = 0;
  *only_errors = 0;

  for (unsigned i = 0; i < nfds; i++) {
    if (pipe_shim_owns(fds[i].fd)) {
      fds[i].revents = pipe_shim_poll(fds[i].fd, fds[i].events);
      if (fds[i].revents & fds[i].events) genuine++;
      if (fds[i].revents) ready++;
      pipe_count++;
    } else if (host_count < POLL_HOST_MAX) {
      source[host_count] = i;
      host[host_count] = fds[i];
      host[host_count].revents = 0;
      host_count++;
    } else {
      /* More host descriptors than fit in one batch. Rare enough that asking
       * about the overflow individually beats growing the batch. */
      const short revents = poll_one_host(fds[i].fd, fds[i].events, 0);
      if (revents & fds[i].events) genuine++;
      fds[i].revents = widen_error(revents, fds[i].events, &widened);
      if (fds[i].revents) ready++;
    }
  }

  if (pipe_count > 0) host_timeout_ms = 0;

  int host_failed = 0;
  int host_error = 0;
  if (host_count > 0) {
    if (poll(host, host_count, host_timeout_ms) < 0) {
      host_failed = 1;
      host_error = errno;
      for (unsigned i = 0; i < host_count; i++)
        host[i].revents = poll_one_host(host[i].fd, host[i].events, 0);
    } else if (host_timeout_ms > 0) {
      *waited = 1;
    }
    for (unsigned i = 0; i < host_count; i++) {
      if (host[i].revents & host[i].events) genuine++;
      fds[source[i]].revents =
          widen_error(host[i].revents, host[i].events, &widened);
      if (fds[source[i]].revents) ready++;
    }
  }

  /* Says the only thing this call has to report is a descriptor in an error
   * state, so the caller can still pace itself. */
  *only_errors = (genuine == 0 && widened);

  /* Only report the failure when it left nothing at all to say, and give the
   * reason the whole-set call gave rather than whatever the last
   * single-descriptor retry happened to leave behind. */
  if (ready == 0 && host_failed) {
    errno = host_error;
    return -1;
  }
  return ready;
}

static unsigned g_poll_traces;

int net_poll(void *fds, unsigned nfds, int timeout_ms) {
  struct pollfd *entries = fds;
  int waited, only_errors;
  if (timeout_ms == 0) return poll_now(entries, nfds, 0, &waited, &only_errors);

  const u64 started = armGetSystemTick();
  int remaining = timeout_ms; /* negative means wait indefinitely */
  int first_pass = 1;
  for (;;) {
    const int idle = elapsed_ms_since(started) >= POLL_GOES_IDLE_AFTER_MS;
    int slice = idle ? POLL_IDLE_SLICE_MS : POLL_ACTIVE_SLICE_MS;
    if (remaining >= 0 && remaining < slice) slice = remaining;

    const int result =
        poll_now(entries, nfds, idle ? 0 : slice, &waited, &only_errors);

    /* Once per call rather than once per slice: a wait that lasts seconds
     * would otherwise spend the whole trace budget on itself. */
    if (first_pass) {
      first_pass = 0;
      if (__atomic_fetch_add(&g_poll_traces, 1, __ATOMIC_RELAXED) < 8) {
        trace("poll(n=%u, timeout=%d) -> %d, fd %d events=0x%x revents=0x%x",
              nfds, timeout_ms, result, nfds ? entries[0].fd : -1,
              nfds ? entries[0].events : 0, nfds ? entries[0].revents : 0);
      }
    }

    if (result > 0 && wanted_event_ready(entries, nfds)) {
      /* Readiness that came only from a broken descriptor still gets its
       * slice: a caller that keeps polling instead of acting on the failure
       * must not be able to spin. */
      if (only_errors && !waited) svcSleepThread((u64)slice * 1000000ull);
      return result;
    }

    /* Nothing useful happened. Make sure the slice is spent one way or the
     * other before handing control back -- whether the call timed out,
     * failed, or reported only an error condition -- so a caller that loops
     * on any of those cannot turn into a spin. */
    if (!waited) svcSleepThread((u64)slice * 1000000ull);
    if (result != 0) return result;
    if (remaining >= 0) {
      remaining -= slice;
      if (remaining <= 0) return 0;
    }
  }
}

int net_select(int nfds, void *readfds, void *writefds, void *exceptfds,
               void *timeout) {
  struct timeval *deadline = timeout;
  int remaining_ms = -1;
  if (deadline)
    remaining_ms = (int)(deadline->tv_sec * 1000 + deadline->tv_usec / 1000);

  struct timeval slice_timeout = {0, 0};
  if (remaining_ms == 0)
    return select(nfds, readfds, writefds, exceptfds, &slice_timeout);

  /* select consumes its descriptor sets, so they are restored each slice. */
  fd_set read_copy, write_copy, except_copy;
  if (readfds) memcpy(&read_copy, readfds, sizeof read_copy);
  if (writefds) memcpy(&write_copy, writefds, sizeof write_copy);
  if (exceptfds) memcpy(&except_copy, exceptfds, sizeof except_copy);

  const u64 started = armGetSystemTick();
  for (;;) {
    if (readfds) memcpy(readfds, &read_copy, sizeof read_copy);
    if (writefds) memcpy(writefds, &write_copy, sizeof write_copy);
    if (exceptfds) memcpy(exceptfds, &except_copy, sizeof except_copy);

    const int idle = elapsed_ms_since(started) >= POLL_GOES_IDLE_AFTER_MS;
    int slice = idle ? POLL_IDLE_SLICE_MS : POLL_ACTIVE_SLICE_MS;
    if (remaining_ms >= 0 && remaining_ms < slice) slice = remaining_ms;

    slice_timeout.tv_sec = 0;
    slice_timeout.tv_usec = idle ? 0 : slice * 1000;
    const int result =
        select(nfds, readfds, writefds, exceptfds, &slice_timeout);
    if (result != 0) return result;

    if (idle) svcSleepThread((u64)slice * 1000000ull);
    if (remaining_ms >= 0) {
      remaining_ms -= slice;
      if (remaining_ms <= 0) return 0;
    }
  }
}

/* --------------------------------------------------------------- fcntl */

#define GUEST_O_NONBLOCK 0x00800
#define GUEST_O_APPEND 0x00400
#define GUEST_F_GETFL 3
#define GUEST_F_SETFL 4

int net_fcntl(int fd, int command, ...) {
  va_list args;
  va_start(args, command);
  const long argument = va_arg(args, long);
  va_end(args);

  if (pipe_shim_owns(fd)) {
    switch (command) {
      case GUEST_F_SETFL:
        return pipe_shim_set_nonblocking(fd, (argument & GUEST_O_NONBLOCK) != 0);
      case GUEST_F_GETFL:
        return pipe_shim_is_nonblocking(fd) ? GUEST_O_NONBLOCK : 0;
      default: return 0; /* descriptor flags; nothing here execs */
    }
  }

  /* Only the status flags need remapping; the command numbers agree. */
  if (command == GUEST_F_SETFL) {
    int host_flags = 0;
    if (argument & GUEST_O_NONBLOCK) host_flags |= O_NONBLOCK;
    if (argument & GUEST_O_APPEND) host_flags |= O_APPEND;
    return fcntl(fd, F_SETFL, host_flags);
  }
  if (command == GUEST_F_GETFL) {
    const int host_flags = fcntl(fd, F_GETFL, 0);
    if (host_flags < 0) return host_flags;
    int guest_flags = host_flags & 3; /* access mode is shared */
    if (host_flags & O_NONBLOCK) guest_flags |= GUEST_O_NONBLOCK;
    if (host_flags & O_APPEND) guest_flags |= GUEST_O_APPEND;
    return guest_flags;
  }

  /* libnx answers every other command with a bare 95, which callers would
   * read as a flag word. Nothing here execs, so the descriptor flags are
   * answered directly instead. */
  switch (command) {
    case 1: return 0;  /* F_GETFD */
    case 2: return 0;  /* F_SETFD */
    case 0: return dup(fd); /* F_DUPFD */
    default:
      errno = EINVAL;
      return -1;
  }
}

/* ------------------------------------------------------- local interfaces
 *
 * The game carries its own getifaddrs -- the NDK's compatibility version,
 * which asks a Linux netlink socket for the interface list. There is no
 * netlink here, so that copy fails every time, and the engine's LAN scan is
 * built on top of it: it turns each interface's address and mask into a
 * broadcast address and pings those to find games on the network. With an
 * empty list it broadcasts nowhere and never sees anybody.
 *
 * nifm knows the console's address and mask, which is all the scan needs, so
 * these replace the guest's copies (see patch.c). */

typedef struct {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  uint8_t sin_zero[8];
} GuestSockaddrIn;

/* bionic's struct ifaddrs; the tail beyond ifa_netmask is unused here but has
 * to be present so callers walking the list read the right offsets. */
typedef struct GuestIfaddrs {
  struct GuestIfaddrs *ifa_next;
  char *ifa_name;
  unsigned int ifa_flags;
  GuestSockaddrIn *ifa_addr;
  GuestSockaddrIn *ifa_netmask;
  GuestSockaddrIn *ifa_broadaddr;
  void *ifa_data;
} GuestIfaddrs;

/* One allocation per interface, so freeing is a walk and a free per node. */
typedef struct {
  GuestIfaddrs entry;
  GuestSockaddrIn address;
  GuestSockaddrIn netmask;
  GuestSockaddrIn broadcast;
  char name[8];
} InterfaceNode;

#define GUEST_IFF_UP 0x1
#define GUEST_IFF_BROADCAST 0x2
#define GUEST_IFF_RUNNING 0x40

static Mutex g_nifm_lock;
static bool g_nifm_lock_ready;
static bool g_nifm_ready;

static bool nifm_available(void) {
  if (!g_nifm_lock_ready) {
    mutexInit(&g_nifm_lock);
    g_nifm_lock_ready = true;
  }
  mutexLock(&g_nifm_lock);
  if (!g_nifm_ready) {
    const Result rc = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(rc)) g_nifm_ready = true;
    else trace_net("nifm unavailable (0x%x); no LAN broadcast addresses", rc);
  }
  mutexUnlock(&g_nifm_lock);
  return g_nifm_ready;
}

static void fill_address(GuestSockaddrIn *out, uint32_t address) {
  memset(out, 0, sizeof *out);
  out->sin_family = AF_INET;
  /* nifm hands back the four octets in address order, which is already what
   * sin_addr wants. */
  out->sin_addr = address;
}

/* The LAN scan asks for this on every sweep, several times a second, and
 * each ask is a round trip to the network service. The console's address is
 * not going to change between two sweeps. */
static Mutex g_ip_lock;
static bool g_ip_lock_ready;
static u64 g_ip_checked;
static uint32_t g_ip_address;
static uint32_t g_ip_netmask;

static bool current_ip_config(uint32_t *address, uint32_t *netmask) {
  if (!g_ip_lock_ready) {
    mutexInit(&g_ip_lock);
    g_ip_lock_ready = true;
  }
  mutexLock(&g_ip_lock);
  const u64 now = armGetSystemTick();
  if (g_ip_address == 0 || now - g_ip_checked >= armGetSystemTickFreq()) {
    uint32_t got = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    if (nifm_available() &&
        R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&got, &mask, &gateway, &dns1,
                                               &dns2))) {
      g_ip_address = got;
      g_ip_netmask = mask;
    } else {
      g_ip_address = 0;
    }
    g_ip_checked = now;
  }
  *address = g_ip_address;
  *netmask = g_ip_netmask;
  mutexUnlock(&g_ip_lock);
  return *address != 0;
}

static unsigned g_ifaddrs_traces;

bool net_have_address(void) {
  uint32_t address = 0, netmask = 0;
  return current_ip_config(&address, &netmask);
}

int net_getifaddrs(void **out) {
  if (!out) {
    errno = EINVAL;
    return -1;
  }
  *out = NULL;

  uint32_t address = 0, netmask = 0;
  if (!current_ip_config(&address, &netmask)) {
    errno = ENODEV;
    return -1;
  }

  InterfaceNode *node = calloc(1, sizeof *node);
  if (!node) {
    errno = ENOMEM;
    return -1;
  }

  snprintf(node->name, sizeof node->name, "eth0");
  fill_address(&node->address, address);
  fill_address(&node->netmask, netmask);
  fill_address(&node->broadcast, address | ~netmask);
  node->entry.ifa_name = node->name;
  node->entry.ifa_flags = GUEST_IFF_UP | GUEST_IFF_BROADCAST | GUEST_IFF_RUNNING;
  node->entry.ifa_addr = &node->address;
  node->entry.ifa_netmask = &node->netmask;
  node->entry.ifa_broadaddr = &node->broadcast;

  if (__atomic_fetch_add(&g_ifaddrs_traces, 1, __ATOMIC_RELAXED) == 0) {
    const uint32_t bcast = address | ~netmask;
    trace("console address %u.%u.%u.%u mask %u.%u.%u.%u -> LAN broadcast "
          "%u.%u.%u.%u",
          address & 0xff, (address >> 8) & 0xff, (address >> 16) & 0xff,
          (address >> 24) & 0xff, netmask & 0xff, (netmask >> 8) & 0xff,
          (netmask >> 16) & 0xff, (netmask >> 24) & 0xff, bcast & 0xff,
          (bcast >> 8) & 0xff, (bcast >> 16) & 0xff, (bcast >> 24) & 0xff);
  }

  *out = &node->entry;
  return 0;
}

void net_freeifaddrs(void *list) {
  GuestIfaddrs *entry = list;
  while (entry) {
    GuestIfaddrs *next = entry->ifa_next;
    /* Every node is the head of its own allocation. */
    free((InterfaceNode *)entry);
    entry = next;
  }
}
