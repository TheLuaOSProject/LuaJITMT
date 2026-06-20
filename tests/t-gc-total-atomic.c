/*
** Focused guard for atomic GC total accounting helpers.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"

#include "lj_obj.h"
#include "lj_gc.h"

#define GC_TOTAL_THREADS 8
#define GC_TOTAL_ITERS 100000
#define GC_TOTAL_BASE ((GCSize)GC_TOTAL_THREADS * (GCSize)GC_TOTAL_ITERS * 8u)

static global_State Gs;

static void *worker_main(void *arg)
{
  intptr_t id = (intptr_t)arg;
  int i;
  for (i = 0; i < GC_TOTAL_ITERS; i++) {
    lj_gc_total_add(&Gs, (GCSize)(7 + (id & 1)));
    lj_gc_total_sub(&Gs, (GCSize)(3 + (id & 1)));
    lj_gc_total_adjust(&Gs, 11, 15);
    lj_gc_total_adjust(&Gs, 19, 13);
  }
  return NULL;
}

int main(void)
{
  pthread_t threads[GC_TOTAL_THREADS];
  GCSize expected = GC_TOTAL_BASE;
  int i;

  memset(&Gs, 0, sizeof(Gs));
  lj_gc_total_store(&Gs, GC_TOTAL_BASE);

  for (i = 0; i < GC_TOTAL_THREADS; i++) {
    expected += (GCSize)GC_TOTAL_ITERS * (GCSize)2;
    assert(pthread_create(&threads[i], NULL, worker_main,
			  (void *)(intptr_t)i) == 0);
  }
  for (i = 0; i < GC_TOTAL_THREADS; i++)
    assert(pthread_join(threads[i], NULL) == 0);

  assert(lj_gc_total_load(&Gs) == expected);
  printf("t-gc-total-atomic OK: %d threads x %d helper updates\n",
	 GC_TOTAL_THREADS, GC_TOTAL_ITERS);
  return 0;
}
