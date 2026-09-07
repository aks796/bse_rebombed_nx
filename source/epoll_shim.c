/* epoll, emulated over poll(). */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "epoll_shim.h"
#include "net_shim.h"
#include "error.h"

/* Linux epoll bits. The basic poll bits below them are numbered the same in
 * both worlds, so only the epoll-only flags need care. */
#define EPOLLIN 0x001u
#define EPOLLPRI 0x002u
#define EPOLLOUT 0x004u
#define EPOLLERR 0x008u
#define EPOLLHUP 0x010u
#define EPOLLRDHUP 0x2000u
#define EPOLLONESHOT (1u << 30)
#define EPOLLET (1u << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* Not packed on aarch64, unlike x86_64: 16 bytes with the union 8-aligned. */
typedef struct {
  uint32_t events;
  uint32_t padding;
  uint64_t data;
} EpollEvent;

#define EPOLL_INSTANCES 8
#define EPOLL_WATCHES 64
#define EPFD_BASE 0x7100

typedef struct {
  int fd;
  uint32_t events;
  uint64_t data;
} Watch;

typedef struct {
  int used;
  int count;
  Watch watches[EPOLL_WATCHES];
} Instance;

static Instance g_instances[EPOLL_INSTANCES];
static Mutex g_lock;

void epoll_shim_init(void) { mutexInit(&g_lock); }

int epoll_shim_owns(int fd) {
  return fd >= EPFD_BASE && fd < EPFD_BASE + EPOLL_INSTANCES;
}

static Instance *instance_of(int epfd) {
  if (!epoll_shim_owns(epfd)) return NULL;
  Instance *instance = &g_instances[epfd - EPFD_BASE];
  return instance->used ? instance : NULL;
}

int shim_epoll_create1(int flags) {
  (void)flags; /* EPOLL_CLOEXEC has nothing to exec into */
  mutexLock(&g_lock);
  for (int i = 0; i < EPOLL_INSTANCES; i++) {
    if (g_instances[i].used) continue;
    memset(&g_instances[i], 0, sizeof g_instances[i]);
    g_instances[i].used = 1;
    mutexUnlock(&g_lock);
    return EPFD_BASE + i;
  }
  mutexUnlock(&g_lock);
  errno = EMFILE;
  return -1;
}

int epoll_shim_close(int fd) {
  mutexLock(&g_lock);
  Instance *instance = instance_of(fd);
  if (instance) instance->used = 0;
  mutexUnlock(&g_lock);
  return instance ? 0 : -1;
}

static Watch *find_watch(Instance *instance, int fd) {
  for (int i = 0; i < instance->count; i++)
    if (instance->watches[i].fd == fd) return &instance->watches[i];
  return NULL;
}

int shim_epoll_ctl(int epfd, int op, int fd, void *event_in) {
  const EpollEvent *event = event_in;

  mutexLock(&g_lock);
  Instance *instance = instance_of(epfd);
  if (!instance) {
    mutexUnlock(&g_lock);
    errno = EBADF;
    return -1;
  }

  Watch *watch = find_watch(instance, fd);
  int result = 0;

  switch (op) {
    case EPOLL_CTL_ADD:
      if (watch) {
        errno = EEXIST;
        result = -1;
        break;
      }
      if (!event || instance->count >= EPOLL_WATCHES) {
        errno = event ? ENOSPC : EFAULT;
        result = -1;
        break;
      }
      instance->watches[instance->count].fd = fd;
      instance->watches[instance->count].events = event->events;
      instance->watches[instance->count].data = event->data;
      instance->count++;
      break;

    case EPOLL_CTL_MOD:
      if (!watch || !event) {
        errno = watch ? EFAULT : ENOENT;
        result = -1;
        break;
      }
      watch->events = event->events;
      watch->data = event->data;
      break;

    case EPOLL_CTL_DEL:
      if (!watch) {
        errno = ENOENT;
        result = -1;
        break;
      }
      *watch = instance->watches[--instance->count];
      break;

    default:
      errno = EINVAL;
      result = -1;
      break;
  }

  mutexUnlock(&g_lock);
  return result;
}

int shim_epoll_wait(int epfd, void *events_out, int max_events, int timeout) {
  EpollEvent *events = events_out;
  if (!events || max_events <= 0) { errno = EINVAL; return -1; }

  struct pollfd fds[EPOLL_WATCHES];
  Watch snapshot[EPOLL_WATCHES];
  int count;

  mutexLock(&g_lock);
  Instance *instance = instance_of(epfd);
  if (!instance) {
    mutexUnlock(&g_lock);
    errno = EBADF;
    return -1;
  }
  count = instance->count;
  memcpy(snapshot, instance->watches, (size_t)count * sizeof(Watch));
  mutexUnlock(&g_lock);

  if (count == 0) {
    /* Nothing registered: honour the timeout so callers still yield. */
    if (timeout > 0) svcSleepThread((u64)timeout * 1000000ull);
    return 0;
  }

  for (int i = 0; i < count; i++) {
    fds[i].fd = snapshot[i].fd;
    /* POLLIN/PRI/OUT share their values with the matching EPOLL bits; the
     * error and hangup bits are output-only and must not be requested. */
    fds[i].events = (short)(snapshot[i].events & (EPOLLIN | EPOLLPRI | EPOLLOUT));
    fds[i].revents = 0;
  }

  /* Through the port's own poll rather than the host's: an epoll set can
   * hold in-process pipe ends alongside real sockets, and only that one
   * knows about both. */
  const int ready = net_poll(fds, (unsigned)count, timeout);
  if (ready <= 0) return ready;

  int produced = 0;
  for (int i = 0; i < count && produced < max_events; i++) {
    if (!fds[i].revents) continue;
    uint32_t reported = (uint32_t)fds[i].revents & (EPOLLIN | EPOLLPRI | EPOLLOUT |
                                                    EPOLLERR | EPOLLHUP);
    /* A closed peer reads as a hangup; report it the way epoll callers
     * expect so asyncio notices a dropped connection. */
    if (fds[i].revents & POLLNVAL) reported |= EPOLLERR;
    if (!reported) continue;

    events[produced].events = reported;
    events[produced].padding = 0;
    events[produced].data = snapshot[i].data;
    produced++;
  }
  return produced;
}
