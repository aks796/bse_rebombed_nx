/* In-process pipe pairs.
 *
 * These used to be a pair of connected loopback TCP sockets, which behaves
 * like a socketpair right up until the console sleeps. Waking up tears the
 * network stack down and every socket in the process -- loopback included --
 * starts answering ENETDOWN for good.
 *
 * That is fatal rather than merely inconvenient, because CPython's asyncio
 * builds its event loop around a socketpair: one end is written to wake the
 * loop, the other is read in _read_from_self, and that function only expects
 * EAGAIN. A permanent error there is re-raised, logged with a full traceback,
 * and then hit again immediately because the descriptor still reads as ready.
 * A single sleep turned into thousands of tracebacks a second for the rest of
 * the session, which is what took the frame rate down and kept it there.
 *
 * On Android the same pair is AF_UNIX and never touches the network, so
 * moving these into the process restores what the guest was written against
 * and takes the console's network state out of the picture entirely.
 */

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <switch.h>

#include "error.h"
#include "pipe_shim.h"

/* Well clear of both real descriptors and the virtual files in io_shim. */
#define PIPE_FD_BASE 0x7100
#define PIPE_PAIRS 8
#define PIPE_ENDPOINTS (PIPE_PAIRS * 2)

/* Wake-up bytes are all these normally carry, so the buffer only has to be
 * big enough that a burst of them cannot fill it before the loop drains. */
#define PIPE_BUFFER_BYTES 16384

/* Long enough that a blocking reader is not spinning, short enough that it
 * still notices a peer closing. Nothing in the game blocks on these in
 * practice -- asyncio sets both ends non-blocking -- so this is a backstop. */
#define PIPE_WAIT_NS 20000000ull

typedef struct {
  uint8_t bytes[PIPE_BUFFER_BYTES];
  size_t start;
  size_t count;
} Channel;

typedef struct {
  int used;
  int open[2];
  int nonblocking[2];
  /* readable[e] holds what endpoint e can read; writing to e appends here. */
  Channel readable[2];
  Mutex lock;
  CondVar changed;
} PipePair;

static PipePair g_pairs[PIPE_PAIRS];
static Mutex g_table_lock;

void pipe_shim_init(void) {
  mutexInit(&g_table_lock);
  for (int i = 0; i < PIPE_PAIRS; i++) {
    mutexInit(&g_pairs[i].lock);
    condvarInit(&g_pairs[i].changed);
  }
}

int pipe_shim_owns(int fd) {
  return fd >= PIPE_FD_BASE && fd < PIPE_FD_BASE + PIPE_ENDPOINTS &&
         g_pairs[(fd - PIPE_FD_BASE) / 2].used;
}

static PipePair *pair_for(int fd, int *endpoint) {
  if (!pipe_shim_owns(fd)) return NULL;
  const int index = fd - PIPE_FD_BASE;
  *endpoint = index % 2;
  return &g_pairs[index / 2];
}

int pipe_shim_pair(int fds[2]) {
  if (!fds) {
    errno = EINVAL;
    return -1;
  }
  mutexLock(&g_table_lock);
  for (int i = 0; i < PIPE_PAIRS; i++) {
    if (g_pairs[i].used) continue;
    PipePair *pair = &g_pairs[i];
    pair->open[0] = pair->open[1] = 1;
    pair->nonblocking[0] = pair->nonblocking[1] = 0;
    memset(&pair->readable, 0, sizeof pair->readable);
    pair->used = 1;
    mutexUnlock(&g_table_lock);
    fds[0] = PIPE_FD_BASE + i * 2;
    fds[1] = PIPE_FD_BASE + i * 2 + 1;
    return 0;
  }
  mutexUnlock(&g_table_lock);
  errno = EMFILE;
  return -1;
}

/* --------------------------------------------------------------- transfer */

static size_t channel_read(Channel *channel, void *out, size_t length) {
  if (length > channel->count) length = channel->count;
  uint8_t *destination = out;
  for (size_t i = 0; i < length; i++)
    destination[i] = channel->bytes[(channel->start + i) % PIPE_BUFFER_BYTES];
  channel->start = (channel->start + length) % PIPE_BUFFER_BYTES;
  channel->count -= length;
  return length;
}

static size_t channel_write(Channel *channel, const void *in, size_t length) {
  const size_t room = PIPE_BUFFER_BYTES - channel->count;
  if (length > room) length = room;
  const uint8_t *source = in;
  for (size_t i = 0; i < length; i++)
    channel->bytes[(channel->start + channel->count + i) % PIPE_BUFFER_BYTES] =
        source[i];
  channel->count += length;
  return length;
}

long pipe_shim_read(int fd, void *buffer, size_t length) {
  int endpoint;
  PipePair *pair = pair_for(fd, &endpoint);
  if (!pair) {
    errno = EBADF;
    return -1;
  }
  if (length == 0) return 0;

  mutexLock(&pair->lock);
  for (;;) {
    if (!pair->open[endpoint]) {
      mutexUnlock(&pair->lock);
      errno = EBADF;
      return -1;
    }
    if (pair->readable[endpoint].count > 0) {
      const size_t got = channel_read(&pair->readable[endpoint], buffer, length);
      condvarWakeAll(&pair->changed);
      mutexUnlock(&pair->lock);
      return (long)got;
    }
    /* Nothing buffered and no writer left is end of file, which is what a
     * closed pipe means. */
    if (!pair->open[1 - endpoint]) {
      mutexUnlock(&pair->lock);
      return 0;
    }
    if (pair->nonblocking[endpoint]) {
      mutexUnlock(&pair->lock);
      errno = EAGAIN;
      return -1;
    }
    condvarWaitTimeout(&pair->changed, &pair->lock, PIPE_WAIT_NS);
  }
}

long pipe_shim_write(int fd, const void *buffer, size_t length) {
  int endpoint;
  PipePair *pair = pair_for(fd, &endpoint);
  if (!pair) {
    errno = EBADF;
    return -1;
  }
  if (length == 0) return 0;

  mutexLock(&pair->lock);
  for (;;) {
    if (!pair->open[endpoint]) {
      mutexUnlock(&pair->lock);
      errno = EBADF;
      return -1;
    }
    if (!pair->open[1 - endpoint]) {
      mutexUnlock(&pair->lock);
      errno = EPIPE;
      return -1;
    }
    const size_t written =
        channel_write(&pair->readable[1 - endpoint], buffer, length);
    if (written > 0) {
      condvarWakeAll(&pair->changed);
      mutexUnlock(&pair->lock);
      return (long)written;
    }
    if (pair->nonblocking[endpoint]) {
      mutexUnlock(&pair->lock);
      errno = EAGAIN;
      return -1;
    }
    condvarWaitTimeout(&pair->changed, &pair->lock, PIPE_WAIT_NS);
  }
}

int pipe_shim_close(int fd) {
  int endpoint;
  PipePair *pair = pair_for(fd, &endpoint);
  if (!pair) {
    errno = EBADF;
    return -1;
  }
  mutexLock(&pair->lock);
  pair->open[endpoint] = 0;
  const int both_closed = !pair->open[0] && !pair->open[1];
  condvarWakeAll(&pair->changed);
  mutexUnlock(&pair->lock);

  if (both_closed) {
    mutexLock(&g_table_lock);
    pair->used = 0;
    mutexUnlock(&g_table_lock);
  }
  return 0;
}

/* ---------------------------------------------------------------- polling */

short pipe_shim_poll(int fd, short events) {
  int endpoint;
  PipePair *pair = pair_for(fd, &endpoint);
  if (!pair) return POLLNVAL;

  short revents = 0;
  mutexLock(&pair->lock);
  if (!pair->open[endpoint]) {
    revents = POLLNVAL;
  } else {
    const int peer_open = pair->open[1 - endpoint];
    /* A closed peer still reads as readable so the reader gets its end of
     * file rather than waiting for something that will never arrive. */
    if (pair->readable[endpoint].count > 0 || !peer_open) revents |= POLLIN;
    if (peer_open && pair->readable[1 - endpoint].count < PIPE_BUFFER_BYTES)
      revents |= POLLOUT;
    if (!peer_open) revents |= POLLHUP;
  }
  mutexUnlock(&pair->lock);

  /* POLLERR, POLLHUP and POLLNVAL come back whether or not they were asked
   * for; everything else is masked to the request. */
  return revents & (events | POLLERR | POLLHUP | POLLNVAL);
}

/* ------------------------------------------------------------- attributes */

int pipe_shim_set_nonblocking(int fd, int enable) {
  int endpoint;
  PipePair *pair = pair_for(fd, &endpoint);
  if (!pair) {
    errno = EBADF;
    return -1;
  }
  mutexLock(&pair->lock);
  pair->nonblocking[endpoint] = enable ? 1 : 0;
  mutexUnlock(&pair->lock);
  return 0;
}

int pipe_shim_is_nonblocking(int fd) {
  int endpoint;
  PipePair *pair = pair_for(fd, &endpoint);
  return pair ? pair->nonblocking[endpoint] : 0;
}

long pipe_shim_bytes_readable(int fd) {
  int endpoint;
  PipePair *pair = pair_for(fd, &endpoint);
  if (!pair) {
    errno = EBADF;
    return -1;
  }
  mutexLock(&pair->lock);
  const long count = (long)pair->readable[endpoint].count;
  mutexUnlock(&pair->lock);
  return count;
}

#define GUEST_AF_UNIX 1
#define GUEST_SOL_SOCKET 1
#define GUEST_SO_TYPE 3
#define GUEST_SO_ERROR 4
#define GUEST_SOCK_STREAM 1

int pipe_shim_getsockname(int fd, void *address, uint32_t *length) {
  if (!pipe_shim_owns(fd)) {
    errno = EBADF;
    return -1;
  }
  if (!address || !length || *length < 2) {
    errno = EINVAL;
    return -1;
  }
  /* An unnamed pair: the family and nothing else, which is what Linux
   * reports for a socketpair. */
  uint16_t family = GUEST_AF_UNIX;
  memcpy(address, &family, sizeof family);
  *length = sizeof family;
  return 0;
}

int pipe_shim_getsockopt(int fd, int level, int option, void *value,
                         uint32_t *length) {
  if (!pipe_shim_owns(fd)) {
    errno = EBADF;
    return -1;
  }
  if (!value || !length || *length < sizeof(int)) {
    errno = EINVAL;
    return -1;
  }
  if (level != GUEST_SOL_SOCKET) {
    errno = ENOPROTOOPT;
    return -1;
  }
  int answer;
  switch (option) {
    case GUEST_SO_TYPE: answer = GUEST_SOCK_STREAM; break;
    case GUEST_SO_ERROR: answer = 0; break;
    default: errno = ENOPROTOOPT; return -1;
  }
  memcpy(value, &answer, sizeof answer);
  *length = sizeof answer;
  return 0;
}
