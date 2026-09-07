/* Bionic threading primitives mapped onto devkitPro's pthreads.
 *
 * Every bionic lock type is at least 16 bytes and zero-initialized by its
 * static initializer, so we store a pointer to a real host object in the
 * first word and create it lazily on first use.
 */

#ifndef BSNX_PTHREAD_SHIM_H
#define BSNX_PTHREAD_SHIM_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

int bionic_pthread_mutex_init(void **slot, const int *attr);
int bionic_pthread_mutex_destroy(void **slot);
int bionic_pthread_mutex_lock(void **slot);
int bionic_pthread_mutex_trylock(void **slot);
int bionic_pthread_mutex_unlock(void **slot);
int bionic_pthread_mutexattr_init(int *attr);
int bionic_pthread_mutexattr_destroy(int *attr);
int bionic_pthread_mutexattr_settype(int *attr, int type);

int bionic_pthread_cond_init(void **slot, const int *attr);
int bionic_pthread_cond_destroy(void **slot);
int bionic_pthread_cond_signal(void **slot);
int bionic_pthread_cond_broadcast(void **slot);
int bionic_pthread_cond_wait(void **slot, void **mutex);
int bionic_pthread_cond_timedwait(void **slot, void **mutex,
                                  const struct timespec *abstime);
int bionic_pthread_condattr_init(int *attr);
int bionic_pthread_condattr_setclock(int *attr, int clock);

int bionic_pthread_rwlock_init(void **slot, const void *attr);
int bionic_pthread_rwlock_destroy(void **slot);
int bionic_pthread_rwlock_rdlock(void **slot);
int bionic_pthread_rwlock_wrlock(void **slot);
int bionic_pthread_rwlock_unlock(void **slot);

int bionic_pthread_attr_init(void *attr);
int bionic_pthread_attr_destroy(void *attr);
int bionic_pthread_attr_setstacksize(void *attr, size_t size);
int bionic_pthread_attr_setdetachstate(void *attr, int state);

int bionic_pthread_create(pthread_t *thread, const void *attr, void *entry, void *arg);
int bionic_pthread_join(pthread_t thread, void **result);
int bionic_pthread_detach(pthread_t thread);
int bionic_pthread_setname_np(pthread_t thread, const char *name);
int bionic_pthread_setschedparam(pthread_t thread, int policy, const void *param);
int bionic_pthread_kill(pthread_t thread, int sig);
int bionic_pthread_sigmask(int how, const void *set, void *old);
int bionic_pthread_getcpuclockid(pthread_t thread, clockid_t *clock);

int bionic_pthread_key_create(unsigned *key, void (*dtor)(void *));
int bionic_pthread_key_delete(unsigned key);
int bionic_pthread_setspecific(unsigned key, const void *value);
void *bionic_pthread_getspecific(unsigned key);

int bionic_pthread_once(volatile int *once, void (*init)(void));

int bionic_sem_init(void **slot, int pshared, unsigned value);
int bionic_sem_destroy(void **slot);
int bionic_sem_post(void **slot);
int bionic_sem_wait(void **slot);
int bionic_sem_trywait(void **slot);
int bionic_sem_timedwait(void **slot, const struct timespec *abstime);

/* Give the calling thread a bionic TLS block so guest code can read its
 * stack canary. Idempotent per thread. */
void bionic_thread_adopt(void);

#endif
