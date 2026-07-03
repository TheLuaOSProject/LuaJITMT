/*
** Focused regression test for atomic GC2 pacing helpers.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"

#include "lj_obj.h"
#include "lj_gc.h"

#define GC2_PACING_THREADS 8
#define GC2_PACING_ITERS 100000

static global_State Gs;

static void check_legacy_controls(void)
{
  memset(&Gs, 0, sizeof(Gs));
  lj_gc_pause_store(&Gs, 200);
  lj_gc_stepmul_store(&Gs, 300);
  assert(lj_gc_pause_load(&Gs) == 200);
  assert(lj_gc_stepmul_load(&Gs) == 300);
  assert(lj_gc_pause_xchg(&Gs, 150) == 200);
  assert(lj_gc_stepmul_xchg(&Gs, 400) == 300);
  assert(lj_gc_pause_load(&Gs) == 150);
  assert(lj_gc_stepmul_load(&Gs) == 400);
}

static void *worker_main(void *arg)
{
  intptr_t id = (intptr_t)arg;
  uint64_t bytes = (uint64_t)(id + 1);
  int i;
  for (i = 0; i < GC2_PACING_ITERS; i++)
    (void)lj_gc2_alloc_since_add(&Gs, bytes);
  return NULL;
}

int main(void)
{
  pthread_t threads[GC2_PACING_THREADS];
  uint64_t expected = 0;
  int i;

  check_legacy_controls();

  memset(&Gs, 0, sizeof(Gs));
  lj_gc2_alloc_since_store(&Gs, 0);
  lj_gc2_cycle_alloc_store(&Gs, 0);
  lj_gc2_trigger_store(&Gs, 0);
  lj_gc2_hard_store(&Gs, ~(uint64_t)0);

  for (i = 0; i < GC2_PACING_THREADS; i++) {
    expected += (uint64_t)GC2_PACING_ITERS * (uint64_t)(i + 1);
    assert(pthread_create(&threads[i], NULL, worker_main,
			  (void *)(intptr_t)i) == 0);
  }
  for (i = 0; i < GC2_PACING_THREADS; i++)
    assert(pthread_join(threads[i], NULL) == 0);

  assert(lj_gc2_alloc_since_load(&Gs) == expected);
  assert(lj_gc2_alloc_since_xchg(&Gs, 17) == expected);
  assert(lj_gc2_alloc_since_load(&Gs) == 17);
  lj_gc2_alloc_since_store(&Gs, expected);
  lj_gc2_cycle_alloc_store(&Gs, expected);
  lj_gc2_trigger_store(&Gs, expected - 1u);
  lj_gc2_hard_store(&Gs, expected + 1u);
  assert(lj_gc2_cycle_alloc_load(&Gs) == expected);
  assert(lj_gc2_trigger_load(&Gs) == expected - 1u);
  assert(lj_gc2_hard_load(&Gs) == expected + 1u);
  assert(!lj_gc2_hard_limit_reached(&Gs));
  lj_gc2_hard_store(&Gs, expected - 1u);
  assert(lj_gc2_hard_limit_reached(&Gs));

  printf("t-gc2-pacing-atomic OK: %d threads x %d helper updates\n",
	 GC2_PACING_THREADS, GC2_PACING_ITERS);
  return 0;
}
