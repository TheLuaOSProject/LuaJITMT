/*
** Shared pthread helpers for C fixtures.
*/

#ifndef TESTS_LIB_THREAD_FIXTURE_HELPERS_H
#define TESTS_LIB_THREAD_FIXTURE_HELPERS_H

#include <pthread.h>

#define LJT_BARRIER_SERIAL_THREAD 1

typedef struct ljt_barrier {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  unsigned count;
  unsigned waiting;
  unsigned generation;
} ljt_barrier_t;

static inline int ljt_barrier_init(ljt_barrier_t *barrier, unsigned count)
{
  int rc;
  barrier->count = count;
  barrier->waiting = 0;
  barrier->generation = 0;
  rc = pthread_mutex_init(&barrier->mutex, NULL);
  if (rc != 0)
    return rc;
  rc = pthread_cond_init(&barrier->cond, NULL);
  if (rc != 0) {
    (void)pthread_mutex_destroy(&barrier->mutex);
    return rc;
  }
  return 0;
}

static inline int ljt_barrier_wait(ljt_barrier_t *barrier)
{
  unsigned generation;
  int rc = pthread_mutex_lock(&barrier->mutex);
  if (rc != 0)
    return rc;
  generation = barrier->generation;
  if (++barrier->waiting == barrier->count) {
    barrier->generation++;
    barrier->waiting = 0;
    rc = pthread_cond_broadcast(&barrier->cond);
    (void)pthread_mutex_unlock(&barrier->mutex);
    return rc == 0 ? LJT_BARRIER_SERIAL_THREAD : rc;
  }
  while (generation == barrier->generation) {
    rc = pthread_cond_wait(&barrier->cond, &barrier->mutex);
    if (rc != 0) {
      (void)pthread_mutex_unlock(&barrier->mutex);
      return rc;
    }
  }
  rc = pthread_mutex_unlock(&barrier->mutex);
  return rc;
}

static inline int ljt_barrier_destroy(ljt_barrier_t *barrier)
{
  int rc = pthread_cond_destroy(&barrier->cond);
  int rc2 = pthread_mutex_destroy(&barrier->mutex);
  return rc != 0 ? rc : rc2;
}

#endif
