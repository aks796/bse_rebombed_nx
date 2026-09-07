/* newlib <-> bionic errno translation.
 *
 * The two agree up to ERANGE (34) and diverge after it, which matters:
 * EINPROGRESS gates non-blocking connect and EADDRINUSE gates BombSquad's
 * UDP listener, and both differ between the two libcs.
 */

#include <errno.h>
#include <stddef.h>

#include "errno_shim.h"

/* Linux/bionic asm-generic values, paired with newlib's macro of the same
 * name so only one side of each row is hard-coded. */
static const struct {
  int host;
  int bionic;
} kErrnoMap[] = {
    {EDEADLK, 35},        {ENAMETOOLONG, 36},   {ENOLCK, 37},
    {ENOSYS, 38},         {ENOTEMPTY, 39},      {ELOOP, 40},
    {ENOMSG, 42},         {EIDRM, 43},          {ENOSTR, 60},
    {ENODATA, 61},        {ETIME, 62},          {ENOSR, 63},
    {ENOLINK, 67},        {EPROTO, 71},         {EMULTIHOP, 72},
    {EBADMSG, 74},        {EOVERFLOW, 75},      {EILSEQ, 84},
    {ENOTSOCK, 88},       {EDESTADDRREQ, 89},   {EMSGSIZE, 90},
    {EPROTOTYPE, 91},     {ENOPROTOOPT, 92},    {EPROTONOSUPPORT, 93},
    {EOPNOTSUPP, 95},     {EPFNOSUPPORT, 96},
    {EAFNOSUPPORT, 97},   {EADDRINUSE, 98},     {EADDRNOTAVAIL, 99},
    {ENETDOWN, 100},      {ENETUNREACH, 101},   {ENETRESET, 102},
    {ECONNABORTED, 103},  {ECONNRESET, 104},    {ENOBUFS, 105},
    {EISCONN, 106},       {ENOTCONN, 107},
    {ETOOMANYREFS, 109},  {ETIMEDOUT, 110},     {ECONNREFUSED, 111},
    {EHOSTDOWN, 112},     {EHOSTUNREACH, 113},  {EALREADY, 114},
    {EINPROGRESS, 115},   {ESTALE, 116},        {EDQUOT, 122},
    {ECANCELED, 125},     {ENOTRECOVERABLE, 131}, {EOWNERDEAD, 130},
};

#define ERRNO_MAP_COUNT ((int)(sizeof kErrnoMap / sizeof kErrnoMap[0]))

/* Values at or below ERANGE are identical in both libcs. */
#define ERRNO_SHARED_MAX 34

int errno_host_to_bionic(int value) {
  if (value <= ERRNO_SHARED_MAX) return value;
  for (int i = 0; i < ERRNO_MAP_COUNT; i++)
    if (kErrnoMap[i].host == value) return kErrnoMap[i].bionic;
  return value;
}

int errno_bionic_to_host(int value) {
  if (value <= ERRNO_SHARED_MAX) return value;
  for (int i = 0; i < ERRNO_MAP_COUNT; i++)
    if (kErrnoMap[i].bionic == value) return kErrnoMap[i].host;
  return value;
}

typedef struct {
  int bionic;
  int host_seen;
} ErrnoState;

static __thread ErrnoState g_errno_state;

int *bionic_errno_location(void) {
  ErrnoState *state = &g_errno_state;
  const int host = errno;

  if (host != state->host_seen) {
    /* The host libc moved errno since the guest last looked. */
    state->host_seen = host;
    state->bionic = errno_host_to_bionic(host);
  } else {
    /* Nothing changed underneath, so any difference is a guest write
     * (typically "errno = 0" before a call). Push it back down. */
    const int want = errno_bionic_to_host(state->bionic);
    if (want != host) {
      errno = want;
      state->host_seen = want;
    }
  }
  return &state->bionic;
}
