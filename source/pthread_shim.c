/* Bionic threading primitives mapped onto devkitPro's pthreads. */

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <time.h>

#include "errno_shim.h"
#include "error.h"

/* bionic clock ids, which are Linux's rather than newlib's. */
#define BIONIC_CLOCK_REALTIME 0
#define BIONIC_CLOCK_MONOTONIC 1
#include "pthread_shim.h"
#include "util.h"

/* Bionic's static initializers store a small type tag rather than a pointer,
 * so anything below this is "not yet created". */
#define SLOT_IS_POINTER(v) ((uintptr_t)(v) >= 0x10000u)

/* Bits 14-15 of a bionic mutex's first word carry its type. */
#define BIONIC_MUTEX_TYPE(v) (((uintptr_t)(v) >> 14) & 3u)
#define BIONIC_MUTEX_RECURSIVE 1
#define BIONIC_MUTEX_ERRORCHECK 2

/* pthread reports failures through its return value rather than errno, so
 * the errno translation that covers the rest of the port does not apply
 * here and every code has to be converted on the way out. Getting this
 * wrong is not subtle: CPython's GIL compares the result of
 * pthread_cond_timedwait against ETIMEDOUT, which is 116 in newlib and 110
 * in bionic, and calls Py_FatalError when it does not match. */
static int to_guest(int host_code) {
  return host_code ? errno_host_to_bionic(host_code) : 0;
}

static Mutex g_create_lock;
static int g_create_lock_ready;

static void create_lock(void) {
  if (!g_create_lock_ready) { mutexInit(&g_create_lock); g_create_lock_ready = 1; }
  mutexLock(&g_create_lock);
}
static void create_unlock(void) { mutexUnlock(&g_create_lock); }

/* ---------------------------------------------------------------- mutexes */

static int mutex_create(void **slot, int type) {
  pthread_mutex_t *m = calloc(1, sizeof *m);
  if (!m) return ENOMEM;

  int rc;
  if (type == BIONIC_MUTEX_RECURSIVE || type == BIONIC_MUTEX_ERRORCHECK) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, type == BIONIC_MUTEX_RECURSIVE
                                         ? PTHREAD_MUTEX_RECURSIVE
                                         : PTHREAD_MUTEX_ERRORCHECK);
    rc = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    rc = pthread_mutex_init(m, NULL);
  }
  if (rc != 0) { free(m); return to_guest(rc); }

  *slot = m;
  return 0;
}

static pthread_mutex_t *mutex_of(void **slot) {
  if (!slot) return NULL;
  if (SLOT_IS_POINTER(*slot)) return (pthread_mutex_t *)*slot;

  create_lock();
  if (!SLOT_IS_POINTER(*slot)) {
    const int type = BIONIC_MUTEX_TYPE(*slot);
    if (mutex_create(slot, type) != 0) { create_unlock(); return NULL; }
  }
  create_unlock();
  return (pthread_mutex_t *)*slot;
}

int bionic_pthread_mutex_init(void **slot, const int *attr) {
  if (!slot) return EINVAL;
  const int type = attr ? *attr : 0;
  *slot = NULL;
  return mutex_create(slot, type);
}

int bionic_pthread_mutex_destroy(void **slot) {
  if (!slot || !SLOT_IS_POINTER(*slot)) return 0;
  pthread_mutex_destroy((pthread_mutex_t *)*slot);
  free(*slot);
  *slot = NULL;
  return 0;
}

int bionic_pthread_mutex_lock(void **slot) {
  pthread_mutex_t *m = mutex_of(slot);
  return m ? to_guest(pthread_mutex_lock(m)) : EINVAL;
}

int bionic_pthread_mutex_trylock(void **slot) {
  pthread_mutex_t *m = mutex_of(slot);
  return m ? to_guest(pthread_mutex_trylock(m)) : EINVAL;
}

int bionic_pthread_mutex_unlock(void **slot) {
  pthread_mutex_t *m = mutex_of(slot);
  return m ? to_guest(pthread_mutex_unlock(m)) : EINVAL;
}

/* Bionic's pthread_mutexattr_t is a bare int holding the type. */
int bionic_pthread_mutexattr_init(int *attr) { if (attr) *attr = 0; return 0; }
int bionic_pthread_mutexattr_destroy(int *attr) { (void)attr; return 0; }
int bionic_pthread_mutexattr_settype(int *attr, int type) {
  if (attr) *attr = type;
  return 0;
}

/* ------------------------------------------------------------- condvars */

typedef struct {
  pthread_cond_t cond;
  clockid_t clock;
} ShimCond;

static ShimCond *cond_of(void **slot) {
  if (!slot) return NULL;
  if (SLOT_IS_POINTER(*slot)) return (ShimCond *)*slot;

  create_lock();
  if (!SLOT_IS_POINTER(*slot)) {
    ShimCond *c = calloc(1, sizeof *c);
    if (!c) { create_unlock(); return NULL; }
    c->clock = CLOCK_REALTIME;
    if (pthread_cond_init(&c->cond, NULL) != 0) {
      free(c);
      create_unlock();
      return NULL;
    }
    *slot = c;
  }
  create_unlock();
  return (ShimCond *)*slot;
}

int bionic_pthread_cond_init(void **slot, const int *attr) {
  if (!slot) return EINVAL;
  ShimCond *c = calloc(1, sizeof *c);
  if (!c) return ENOMEM;

  /* The attribute carries a bionic clock id, not a host one. */
  c->clock = (attr && *attr == BIONIC_CLOCK_MONOTONIC) ? CLOCK_MONOTONIC
                                                       : CLOCK_REALTIME;

  pthread_condattr_t host_attr;
  pthread_condattr_init(&host_attr);
  pthread_condattr_setclock(&host_attr, c->clock);
  const int rc = pthread_cond_init(&c->cond, &host_attr);
  pthread_condattr_destroy(&host_attr);
  if (rc != 0) { free(c); return to_guest(rc); }

  *slot = c;
  return 0;
}

int bionic_pthread_cond_destroy(void **slot) {
  if (!slot || !SLOT_IS_POINTER(*slot)) return 0;
  ShimCond *c = (ShimCond *)*slot;
  pthread_cond_destroy(&c->cond);
  free(c);
  *slot = NULL;
  return 0;
}

int bionic_pthread_cond_signal(void **slot) {
  ShimCond *c = cond_of(slot);
  return c ? to_guest(pthread_cond_signal(&c->cond)) : EINVAL;
}

int bionic_pthread_cond_broadcast(void **slot) {
  ShimCond *c = cond_of(slot);
  return c ? to_guest(pthread_cond_broadcast(&c->cond)) : EINVAL;
}

int bionic_pthread_cond_wait(void **slot, void **mutex) {
  ShimCond *c = cond_of(slot);
  pthread_mutex_t *m = mutex_of(mutex);
  if (!c || !m) return EINVAL;
  return to_guest(pthread_cond_wait(&c->cond, m));
}

int bionic_pthread_cond_timedwait(void **slot, void **mutex,
                                  const struct timespec *abstime) {
  ShimCond *c = cond_of(slot);
  pthread_mutex_t *m = mutex_of(mutex);
  if (!c || !m) return EINVAL;
  if (!abstime) return to_guest(pthread_cond_wait(&c->cond, m));
  return to_guest(pthread_cond_timedwait(&c->cond, m, abstime));
}

/* Bionic's pthread_condattr_t is a bare int holding a bionic clock id. */
int bionic_pthread_condattr_init(int *attr) {
  if (attr) *attr = BIONIC_CLOCK_REALTIME;
  return 0;
}
int bionic_pthread_condattr_setclock(int *attr, int clock) {
  if (attr) *attr = clock;
  return 0;
}

/* -------------------------------------------------------------- rwlocks */

static pthread_rwlock_t *rwlock_of(void **slot) {
  if (!slot) return NULL;
  if (SLOT_IS_POINTER(*slot)) return (pthread_rwlock_t *)*slot;

  create_lock();
  if (!SLOT_IS_POINTER(*slot)) {
    pthread_rwlock_t *l = calloc(1, sizeof *l);
    if (!l) { create_unlock(); return NULL; }
    if (pthread_rwlock_init(l, NULL) != 0) { free(l); create_unlock(); return NULL; }
    *slot = l;
  }
  create_unlock();
  return (pthread_rwlock_t *)*slot;
}

int bionic_pthread_rwlock_init(void **slot, const void *attr) {
  (void)attr;
  if (!slot) return EINVAL;
  *slot = NULL;
  return rwlock_of(slot) ? 0 : ENOMEM;
}

int bionic_pthread_rwlock_destroy(void **slot) {
  if (!slot || !SLOT_IS_POINTER(*slot)) return 0;
  pthread_rwlock_destroy((pthread_rwlock_t *)*slot);
  free(*slot);
  *slot = NULL;
  return 0;
}

int bionic_pthread_rwlock_rdlock(void **slot) {
  pthread_rwlock_t *l = rwlock_of(slot);
  return l ? to_guest(pthread_rwlock_rdlock(l)) : EINVAL;
}
int bionic_pthread_rwlock_wrlock(void **slot) {
  pthread_rwlock_t *l = rwlock_of(slot);
  return l ? to_guest(pthread_rwlock_wrlock(l)) : EINVAL;
}
int bionic_pthread_rwlock_unlock(void **slot) {
  pthread_rwlock_t *l = rwlock_of(slot);
  return l ? to_guest(pthread_rwlock_unlock(l)) : EINVAL;
}

/* --------------------------------------------------------------- threads */

#define ATTR_MAGIC 0x42534E58u /* "BSNX" */

typedef struct {
  uint32_t magic;
  uint32_t detach;
  size_t stacksize;
} ShimAttr;

int bionic_pthread_attr_init(void *attr) {
  if (!attr) return EINVAL;
  ShimAttr *a = attr;
  a->magic = ATTR_MAGIC;
  a->detach = 0;
  a->stacksize = 0;
  return 0;
}
int bionic_pthread_attr_destroy(void *attr) { (void)attr; return 0; }

int bionic_pthread_attr_setstacksize(void *attr, size_t size) {
  ShimAttr *a = attr;
  if (a && a->magic == ATTR_MAGIC) a->stacksize = size;
  return 0;
}
int bionic_pthread_attr_setdetachstate(void *attr, int state) {
  ShimAttr *a = attr;
  if (a && a->magic == ATTR_MAGIC) a->detach = (uint32_t)state;
  return 0;
}

/* devkitPro's pthread_detach forwards to __syscall_thread_detach, which
 * libnx does not implement -- it always fails with ENOSYS. Every finished
 * thread therefore stays joinable forever, holding a kernel handle and its
 * whole stack, until thread creation starts failing. CPython leans on detach
 * heavily, so detached threads are tracked here and reaped once their body
 * returns. */
#define THREAD_RECORDS 512

typedef struct {
  int used;
  pthread_t handle;
  volatile int finished;
  volatile int detached;
  void *start_block;
} ThreadRecord;

static ThreadRecord g_threads[THREAD_RECORDS];
static Mutex g_thread_lock;
static int g_thread_lock_ready;
static Thread g_reaper;
static int g_reaper_started;

typedef struct {
  void *(*entry)(void *);
  void *arg;
  ThreadRecord *record;
  uint8_t tls[BIONIC_TLS_SIZE];
} ThreadStart;

static void thread_lock(void) {
  if (!g_thread_lock_ready) { mutexInit(&g_thread_lock); g_thread_lock_ready = 1; }
  mutexLock(&g_thread_lock);
}

/* Join anything that asked to be detached and has since finished. Joining an
 * already-exited thread returns immediately, so this never blocks. */
static void reap_finished(void) {
  for (int i = 0; i < THREAD_RECORDS; i++) {
    thread_lock();
    ThreadRecord *record = &g_threads[i];
    const int ready = record->used && record->detached && record->finished;
    const pthread_t handle = record->handle;
    void *block = record->start_block;
    if (ready) {
      record->used = 0;
      record->start_block = NULL;
    }
    mutexUnlock(&g_thread_lock);

    if (!ready) continue;
    pthread_join(handle, NULL);
    free(block);
  }
}

static void reaper_entry(void *arg) {
  (void)arg;
  while (1) {
    svcSleepThread(250000000ull); /* 250 ms */
    reap_finished();
  }
}

static void start_reaper(void) {
  if (g_reaper_started) return;
  g_reaper_started = 1;
  if (R_SUCCEEDED(threadCreate(&g_reaper, reaper_entry, NULL, NULL, 16 * 1024,
                               0x3B, -2))) {
    threadStart(&g_reaper);
  } else {
    trace("could not start the thread reaper");
  }
}

static ThreadRecord *record_reserve(void) {
  thread_lock();
  for (int i = 0; i < THREAD_RECORDS; i++) {
    if (g_threads[i].used) continue;
    ThreadRecord *record = &g_threads[i];
    record->used = 1;
    record->finished = 0;
    record->detached = 0;
    record->start_block = NULL;
    mutexUnlock(&g_thread_lock);
    return record;
  }
  mutexUnlock(&g_thread_lock);
  return NULL;
}

static ThreadRecord *record_for(pthread_t handle) {
  for (int i = 0; i < THREAD_RECORDS; i++)
    if (g_threads[i].used && pthread_equal(g_threads[i].handle, handle))
      return &g_threads[i];
  return NULL;
}

static __thread int g_tls_installed;

void bionic_thread_adopt(void) {
  if (g_tls_installed) return;
  /* Leaked deliberately: TPIDR_EL0 keeps pointing into this block for the
   * lifetime of the thread. */
  void *block = memalign(16, BIONIC_TLS_SIZE);
  if (!block) fatal_error("Out of memory allocating guest thread TLS.");
  install_bionic_tls(block);
  g_tls_installed = 1;
}

static void *thread_trampoline(void *p) {
  ThreadStart *ts = p;
  install_bionic_tls(ts->tls);
  g_tls_installed = 1;
  void *result = ts->entry(ts->arg);
  /* The block stays allocated until the thread is joined: TLS destructors
   * still run after this returns and read the canary from it. */
  if (ts->record) __atomic_store_n(&ts->record->finished, 1, __ATOMIC_RELEASE);
  return result;
}

int bionic_pthread_create(pthread_t *thread, const void *attr, void *entry, void *arg) {
  start_reaper();

  size_t stack = 0;
  int detached = 0;
  if (attr) {
    const ShimAttr *a = attr;
    if (a->magic == ATTR_MAGIC) {
      stack = a->stacksize;
      detached = a->detach != 0;
    }
  }
  /* Deeper than the 128 KB default for CPython's recursion, but not the 1 MB
   * bionic would use -- the engine keeps dozens of threads alive at once. */
  if (stack < (512u << 10)) stack = 512u << 10;
  stack = (stack + 0xFFFFu) & ~(size_t)0xFFFFu;

  ThreadRecord *record = record_reserve();
  if (!record) {
    /* The table is full, which means threads are outliving their reaping.
     * Sweep synchronously and try once more before giving up. */
    reap_finished();
    record = record_reserve();
    if (!record) return EAGAIN;
  }

  ThreadStart *ts = malloc(sizeof *ts);
  if (!ts) {
    thread_lock();
    record->used = 0;
    mutexUnlock(&g_thread_lock);
    return EAGAIN;
  }
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  ts->record = record;
  record->start_block = ts;
  record->detached = detached;

  pthread_attr_t host_attr;
  pthread_attr_init(&host_attr);
  pthread_attr_setstacksize(&host_attr, stack);
  /* Always joinable on the host: the reaper is what reclaims these, and the
   * host cannot detach them anyway. */

  const int rc = pthread_create(thread, &host_attr, thread_trampoline, ts);
  pthread_attr_destroy(&host_attr);

  if (rc != 0) {
    thread_lock();
    record->used = 0;
    record->start_block = NULL;
    mutexUnlock(&g_thread_lock);
    free(ts);
    return to_guest(rc);
  }

  thread_lock();
  record->handle = *thread;
  mutexUnlock(&g_thread_lock);
  return 0;
}

int bionic_pthread_join(pthread_t thread, void **result) {
  thread_lock();
  ThreadRecord *record = record_for(thread);
  void *block = NULL;
  if (record) {
    /* Claim it so the reaper cannot join the same handle underneath us. */
    record->used = 0;
    block = record->start_block;
    record->start_block = NULL;
  }
  mutexUnlock(&g_thread_lock);

  /* An unknown handle has already been reaped; joining it again would be a
   * use-after-free, so report the success the caller expects. */
  if (!record) {
    if (result) *result = NULL;
    return 0;
  }

  const int rc = pthread_join(thread, result);
  free(block);
  return to_guest(rc);
}

int bionic_pthread_detach(pthread_t thread) {
  thread_lock();
  ThreadRecord *record = record_for(thread);
  if (record) record->detached = 1;
  mutexUnlock(&g_thread_lock);
  /* Reported as successful whether or not the thread is still tracked: the
   * host cannot detach, so the reaper handles it instead. */
  return 0;
}

int bionic_pthread_setname_np(pthread_t thread, const char *name) {
  (void)thread;
  (void)name;
  return 0;
}
int bionic_pthread_setschedparam(pthread_t thread, int policy, const void *param) {
  (void)thread;
  (void)policy;
  (void)param;
  return 0;
}
int bionic_pthread_kill(pthread_t thread, int sig) {
  (void)thread;
  (void)sig;
  return 0;
}
int bionic_pthread_sigmask(int how, const void *set, void *old) {
  (void)how;
  (void)set;
  (void)old;
  return 0;
}
int bionic_pthread_getcpuclockid(pthread_t thread, clockid_t *clock) {
  (void)thread;
  /* Handed back to the guest, which will pass it to clock_gettime, so it has
   * to be a bionic id. */
  if (clock) *clock = BIONIC_CLOCK_MONOTONIC;
  return 0;
}

/* ------------------------------------------------------------- TLS keys */

/* Bionic keys are small ints the guest stores in its own structures, so we
 * hand out dense indices and multiplex them over one host key. */
#define SHIM_KEYS_MAX 128

typedef struct {
  void *values[SHIM_KEYS_MAX];
} KeyBlock;

static struct {
  int used;
  void (*dtor)(void *);
} g_keys[SHIM_KEYS_MAX];
static pthread_key_t g_master_key;
static int g_master_ready;
static Mutex g_key_lock;
static int g_key_lock_ready;

static void key_lock(void) {
  if (!g_key_lock_ready) { mutexInit(&g_key_lock); g_key_lock_ready = 1; }
  mutexLock(&g_key_lock);
}

static void master_dtor(void *block) {
  KeyBlock *kb = block;
  if (!kb) return;
  /* One destructor pass matches what bionic guarantees in practice. */
  for (int i = 0; i < SHIM_KEYS_MAX; i++) {
    void *value = kb->values[i];
    if (!value || !g_keys[i].used || !g_keys[i].dtor) continue;
    kb->values[i] = NULL;
    g_keys[i].dtor(value);
  }
  free(kb);
}

static KeyBlock *key_block(int create) {
  if (!g_master_ready) return NULL;
  KeyBlock *kb = pthread_getspecific(g_master_key);
  if (!kb && create) {
    kb = calloc(1, sizeof *kb);
    if (kb) pthread_setspecific(g_master_key, kb);
  }
  return kb;
}

int bionic_pthread_key_create(unsigned *key, void (*dtor)(void *)) {
  if (!key) return EINVAL;
  key_lock();
  if (!g_master_ready) {
    if (pthread_key_create(&g_master_key, master_dtor) != 0) {
      mutexUnlock(&g_key_lock);
      return EAGAIN;
    }
    g_master_ready = 1;
  }
  for (int i = 0; i < SHIM_KEYS_MAX; i++) {
    if (g_keys[i].used) continue;
    g_keys[i].used = 1;
    g_keys[i].dtor = dtor;
    *key = (unsigned)i;
    mutexUnlock(&g_key_lock);
    return 0;
  }
  mutexUnlock(&g_key_lock);
  return EAGAIN;
}

int bionic_pthread_key_delete(unsigned key) {
  if (key >= SHIM_KEYS_MAX) return EINVAL;
  key_lock();
  g_keys[key].used = 0;
  g_keys[key].dtor = NULL;
  mutexUnlock(&g_key_lock);
  return 0;
}

int bionic_pthread_setspecific(unsigned key, const void *value) {
  if (key >= SHIM_KEYS_MAX) return EINVAL;
  KeyBlock *kb = key_block(1);
  if (!kb) return ENOMEM;
  kb->values[key] = (void *)value;
  return 0;
}

void *bionic_pthread_getspecific(unsigned key) {
  if (key >= SHIM_KEYS_MAX) return NULL;
  KeyBlock *kb = key_block(0);
  return kb ? kb->values[key] : NULL;
}

/* pthread_once_t is a bare int in bionic, zero before the first run.
 *
 * The state lives in that int rather than a shared mutex: initializers here
 * routinely call pthread_key_create, and borrowing the key lock would
 * deadlock against it. */
int bionic_pthread_once(volatile int *once, void (*init)(void)) {
  if (!once || !init) return EINVAL;
  if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2) return 0;

  int expected = 0;
  if (__atomic_compare_exchange_n(once, &expected, 1, false, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    init();
    __atomic_store_n(once, 2, __ATOMIC_RELEASE);
    return 0;
  }
  while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) svcSleepThread(100000ull);
  return 0;
}

/* ----------------------------------------------------------- semaphores */

typedef struct {
  Mutex lock;
  CondVar cond;
  int count;
} ShimSem;

static ShimSem *sem_of(void **slot) {
  return (slot && SLOT_IS_POINTER(*slot)) ? (ShimSem *)*slot : NULL;
}

int bionic_sem_init(void **slot, int pshared, unsigned value) {
  (void)pshared;
  if (!slot) return EINVAL;
  ShimSem *s = calloc(1, sizeof *s);
  if (!s) return ENOMEM;
  mutexInit(&s->lock);
  condvarInit(&s->cond);
  s->count = (int)value;
  *slot = s;
  return 0;
}

int bionic_sem_destroy(void **slot) {
  ShimSem *s = sem_of(slot);
  if (s) { free(s); *slot = NULL; }
  return 0;
}

int bionic_sem_post(void **slot) {
  ShimSem *s = sem_of(slot);
  if (!s) return EINVAL;
  mutexLock(&s->lock);
  s->count++;
  condvarWakeOne(&s->cond);
  mutexUnlock(&s->lock);
  return 0;
}

int bionic_sem_wait(void **slot) {
  ShimSem *s = sem_of(slot);
  if (!s) return EINVAL;
  mutexLock(&s->lock);
  while (s->count <= 0) condvarWait(&s->cond, &s->lock);
  s->count--;
  mutexUnlock(&s->lock);
  return 0;
}

int bionic_sem_trywait(void **slot) {
  ShimSem *s = sem_of(slot);
  if (!s) { errno = EINVAL; return -1; }
  mutexLock(&s->lock);
  const int ok = s->count > 0;
  if (ok) s->count--;
  mutexUnlock(&s->lock);
  if (ok) return 0;
  errno = EAGAIN;
  return -1;
}

int bionic_sem_timedwait(void **slot, const struct timespec *abstime) {
  ShimSem *s = sem_of(slot);
  if (!s) { errno = EINVAL; return -1; }
  if (!abstime) return bionic_sem_wait(slot) == 0 ? 0 : -1;

  mutexLock(&s->lock);
  while (s->count <= 0) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t remaining_ns = ((int64_t)abstime->tv_sec - now.tv_sec) * 1000000000ll +
                           ((int64_t)abstime->tv_nsec - now.tv_nsec);
    if (remaining_ns <= 0) {
      mutexUnlock(&s->lock);
      errno = ETIMEDOUT;
      return -1;
    }
    condvarWaitTimeout(&s->cond, &s->lock, (u64)remaining_ns);
  }
  s->count--;
  mutexUnlock(&s->lock);
  return 0;
}
