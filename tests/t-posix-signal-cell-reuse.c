/* Exercise Linux profiler mirror reuse with distinct caller-supplied stacks. */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>

#include "lj_tg.h"
#include "lj_thr.h"

enum { STACK_COUNT = 257, STACK_BYTES = 1024 * 1024 };

typedef struct Probe {
  const void *cell;
  uint32_t hold, ready;
  int fail_first;
} Probe;

static _Thread_local Probe *binding;
static _Thread_local uintptr_t sampled;

extern int __real_pthread_setspecific(pthread_key_t key, const void *value);

int __wrap_pthread_setspecific(pthread_key_t key, const void *value)
{
  if (binding) {
    binding->cell = value;
    if (binding->fail_first) {
      binding->fail_first = 0;
      return ENOMEM;
    }
  }
  return __real_pthread_setspecific(key, value);
}

static void sample_handler(int signo)
{
  (void)signo;
  la_storeuptr_rel(&sampled, (uintptr_t)lj_thr_get_tg_profile_signal());
}

static void sample(TGState *expected)
{
  la_storeuptr_rlx(&sampled, ~(uintptr_t)0);
  assert(raise(SIGUSR1) == 0);
  assert(la_loaduptr_acq(&sampled) == (uintptr_t)expected);
}

static void *bind_and_sample(void *arg)
{
  Probe *p = (Probe *)arg;
  TGState tg = {0};
  sample(NULL);
  binding = p;
  if (p->fail_first) {
    const void *first;
    errno = EDOM;
    lj_thr_set_tg(&tg);
    assert(errno == EDOM && lj_thr_get_tg() == &tg);
    assert(!p->fail_first && p->cell);
    first = p->cell;
    sample(NULL);
    lj_thr_set_tg(&tg);
    assert(p->cell == first);
  } else {
    lj_thr_set_tg(&tg);
  }
  binding = NULL;
  assert(p->cell && lj_thr_get_tg_signal() == NULL);
  sample(&tg);
  la_store32_rel(&p->ready, 1);
  while (la_load32_acq(&p->hold)) {
    sample(&tg);
    sched_yield();
  }
  sample(&tg);
  return NULL;  /* The destructor must withdraw this live mirror. */
}

static pthread_t start(char *stack, Probe *probe)
{
  pthread_t tid;
  pthread_attr_t attr;
  assert(pthread_attr_init(&attr) == 0);
  assert(pthread_attr_setstack(&attr, stack, STACK_BYTES) == 0);
  assert(pthread_create(&tid, &attr, bind_and_sample, probe) == 0);
  assert(pthread_attr_destroy(&attr) == 0);
  return tid;
}

int main(void)
{
  const size_t bytes = (size_t)STACK_COUNT * STACK_BYTES;
  char *stacks = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  struct sigaction action = {0}, previous;
  Probe probes[STACK_COUNT] = {{0}}, retry = {0}, peer[2] = {{0}};
  pthread_t workers[2];
  unsigned i, j, left = 0, right = 0;
  assert(stacks != MAP_FAILED);
  action.sa_handler = sample_handler;
  assert(sigemptyset(&action.sa_mask) == 0);
  assert(sigaction(SIGUSR1, &action, &previous) == 0);

  /* Even with an independently placed pthread stack every time, 257
  ** sequential registrations must share a cell in the 256-bucket cache. */
  for (i = 0; i < STACK_COUNT; i++) {
    assert(pthread_join(start(stacks + i * STACK_BYTES, &probes[i]), NULL) == 0);
    for (j = 0; j < i && !right; j++) {
      if (probes[j].cell == probes[i].cell) {
        left = j;
        right = i;
      }
    }
  }
  assert(right != 0);
  retry.fail_first = 1;
  assert(pthread_join(start(stacks + left * STACK_BYTES, &retry), NULL) == 0);
  assert(retry.cell == probes[left].cell);

  /* Both colliding owners now stay live and sample in actual handlers. */
  peer[0].hold = peer[1].hold = 1;
  workers[0] = start(stacks + left * STACK_BYTES, &peer[0]);
  while (!la_load32_acq(&peer[0].ready)) sched_yield();
  workers[1] = start(stacks + right * STACK_BYTES, &peer[1]);
  while (!la_load32_acq(&peer[1].ready)) sched_yield();
  assert(peer[0].cell == retry.cell && peer[1].cell != peer[0].cell);
  for (i = 0; i < 2; i++) {
    la_store32_rel(&peer[i].hold, 0);
    assert(pthread_join(workers[i], NULL) == 0);
  }
  assert(sigaction(SIGUSR1, &previous, NULL) == 0);
  assert(munmap(stacks, bytes) == 0);
  puts("t-posix-signal-cell-reuse OK");
  return 0;
}
