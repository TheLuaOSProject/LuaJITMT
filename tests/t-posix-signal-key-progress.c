/* Bind a second profiler while the first caller holds a private pthread key. */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lj_tg.h"
#include "lj_thr.h"

enum Role { NONE, FIRST, PEER };
static _Thread_local enum Role role;
static uint32_t entered, resume, peer_waited;
static uint32_t peer_created, loser_deleted;
static pthread_key_t first_key;
static int fail_first;

extern int __real_pthread_key_create(pthread_key_t *key, void (*dtor)(void *));
extern int __real_pthread_key_delete(pthread_key_t key);
extern int __real_sched_yield(void);

int __wrap_pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
  int rc = __real_pthread_key_create(key, dtor);
  if (rc == 0 && role == FIRST) {
    first_key = *key;
    la_store32_rel(&entered, 1);
    while (!la_load32_acq(&resume)) __real_sched_yield();
    if (fail_first) {
      assert(__real_pthread_key_delete(*key) == 0);
      return EAGAIN;
    }
  } else if (rc == 0 && role == PEER) {
    assert(*key != first_key);
    la_store32_rel(&peer_created, 1);
  }
  return rc;
}

int __wrap_pthread_key_delete(pthread_key_t key)
{
  int rc = __real_pthread_key_delete(key);
  if (role == FIRST) {
    assert(rc == 0 && key == first_key);
    la_store32_rel(&loser_deleted, 1);
  }
  return rc;
}

int __wrap_sched_yield(void)
{
  if (role == PEER && !la_load32_acq(&resume)) {
    /* Let the old single-initializer path finish and report its dependence. */
    la_store32_rel(&peer_waited, 1);
    la_store32_rel(&resume, 1);
  }
  return __real_sched_yield();
}

static void *bind_thread(void *arg)
{
  TGState tg = {0};
  role = *(enum Role *)arg;
  errno = ERANGE;
  lj_thr_set_tg(&tg);
  assert(errno == ERANGE && lj_thr_get_tg() == &tg);
  assert(lj_thr_get_tg_profile_signal() == &tg);
  if (role == PEER)
    la_store32_rel(&resume, 1);
  role = NONE;
  return NULL;
}

static void check_admission(int inject_failure)
{
  pthread_t first, peer;
  enum Role first_role = FIRST, peer_role = PEER;
  fail_first = inject_failure;
  assert(pthread_create(&first, NULL, bind_thread, &first_role) == 0);
  while (!la_load32_acq(&entered)) __real_sched_yield();
  assert(pthread_create(&peer, NULL, bind_thread, &peer_role) == 0);
  assert(pthread_join(peer, NULL) == 0 && pthread_join(first, NULL) == 0);
  assert(!peer_waited && peer_created);
  assert(loser_deleted == (uint32_t)!inject_failure);
}

int main(void)
{
  pid_t child = fork();
  int status;
  assert(child >= 0);
  if (child == 0) {
    check_admission(0);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  check_admission(1);  /* A concurrent winner also covers local EAGAIN. */
  puts("t-posix-signal-key-progress OK");
  return 0;
}
