/*
** Deterministic remote-free pending-hint publication/drain races.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

#define RACE_PUBLISHERS 8u

typedef struct RemoteWork {
  TGAlloc *alloc;
  void *p;
  uint32_t n;
  int result;
} RemoteWork;

static void wait_seen(uint32_t (*seen)(void))
{
  uint32_t n = 0;
  while (!seen()) {
    assert(++n != 100000000u);
    la_cpu_pause();
  }
}

static void *publish_one(void *ud)
{
  RemoteWork *w = (RemoteWork *)ud;
  w->result = lj_arena_remote_free_publish(w->alloc, w->p, 64u);
  return NULL;
}

static void *drain_one(void *ud)
{
  RemoteWork *w = (RemoteWork *)ud;
  w->n = lj_arena_remote_free_drain(w->alloc);
  return NULL;
}

static void test_empty_elision(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  uint32_t i, arenas = 0;

  lj_arena_alloc_init(&alloc);
  for (i = 0; i < 32u; i++)
    assert(lj_arena_alloc(&alloc, rs, 16000u, 0) != NULL);
  for (a = alloc.owned[LJ_ARENAK_PLAIN]; a; a = lj_arena_next_acq(a))
    arenas++;
  assert(arenas > 4u);

  lj_arena_test_remote_stats_reset();
  assert(lj_arena_remote_free_drain(&alloc) == 0u);
  assert(lj_arena_test_remote_fast_skips() == 1u);
  assert(lj_arena_test_remote_arena_probes() == 0u);
  lj_arena_alloc_fini(&alloc);
}

static void test_queue_before_wake(PRNGState *rs)
{
  TGAlloc alloc;
  RemoteWork w;
  pthread_t thread;
  GCArena *a;

  lj_arena_alloc_init(&alloc);
  w.alloc = &alloc;
  w.p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  w.result = 0;
  assert(w.p != NULL);
  a = lj_arena_of(w.p);

  lj_arena_test_remote_publish_pause(1);
  assert(pthread_create(&thread, NULL, publish_one, &w) == 0);
  wait_seen(lj_arena_test_remote_publish_paused);
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == w.p);
  assert(lj_arena_remote_pending_acq(&alloc) == 0u);
  lj_arena_test_remote_stats_reset();
  assert(lj_arena_remote_free_drain(&alloc) == 0u);
  assert(lj_arena_test_remote_fast_skips() == 1u);
  assert(lj_arena_test_remote_arena_probes() == 0u);

  lj_arena_test_remote_publish_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(w.result == 1);
  assert(lj_arena_remote_pending_acq(&alloc) == 1u);
  assert(lj_arena_remote_free_drain(&alloc) == 1u);
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
  /* This fixture verifies exact remote-free reuse, not allocation order.
  ** Exhaust its independent owner-private bump before selecting the bin. */
  alloc.bump[LJ_ARENAK_TRAVERSABLE].cell =
    alloc.bump[LJ_ARENAK_TRAVERSABLE].end;
  assert(lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE) == w.p);
  lj_arena_alloc_fini(&alloc);
}

static void test_publish_after_clear(PRNGState *rs)
{
  TGAlloc alloc;
  RemoteWork drain;
  pthread_t thread;
  void *p1, *p2;
  GCArena *a;

  lj_arena_alloc_init(&alloc);
  p1 = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  p2 = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p1 != NULL && p2 != NULL);
  a = lj_arena_of(p1);
  assert(lj_arena_of(p2) == a);
  assert(lj_arena_remote_free_publish(&alloc, p1, 64u));

  drain.alloc = &alloc;
  drain.n = 0;
  lj_arena_test_remote_drain_pause(1);
  assert(pthread_create(&thread, NULL, drain_one, &drain) == 0);
  wait_seen(lj_arena_test_remote_drain_paused);
  assert(lj_arena_remote_pending_acq(&alloc) == 0u);
  assert(lj_arena_remote_free_publish(&alloc, p2, 64u));
  assert(lj_arena_remote_pending_acq(&alloc) == 1u);
  lj_arena_test_remote_drain_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(drain.n == 2u);
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);

  /* The post-clear publisher deliberately leaves a conservative wake even
  ** when this in-flight scan happened to consume its record. */
  assert(lj_arena_remote_pending_acq(&alloc) == 1u);
  assert(lj_arena_remote_free_drain(&alloc) == 0u);
  assert(lj_arena_remote_pending_acq(&alloc) == 0u);
  lj_arena_alloc_fini(&alloc);
}

static void test_blocked_scan_rearms(PRNGState *rs)
{
  TGAlloc alloc;
  void *p;
  GCArena *a;

  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  assert(lj_arena_remote_free_publish(&alloc, p, 64u));

  /* A non-queue rescue admission can make the arena temporarily unavailable.
  ** Consuming the allocator wake in that interval must rearm it, because the
  ** rescue leave has no remote-free record of its own to announce. */
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  assert(lj_arena_remote_free_drain(&alloc) == 0u);
  assert(lj_arena_remote_pending_acq(&alloc) == 1u);
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == p);
  lj_arena_rescue_leave(a);
  assert(lj_arena_remote_free_drain(&alloc) == 1u);
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
  lj_arena_alloc_fini(&alloc);
}

static void test_multi_publisher_no_drop(PRNGState *rs)
{
  TGAlloc alloc;
  RemoteWork work[RACE_PUBLISHERS];
  pthread_t threads[RACE_PUBLISHERS];
  uint32_t i;

  lj_arena_alloc_init(&alloc);
  for (i = 0; i < RACE_PUBLISHERS; i++) {
    work[i].alloc = &alloc;
    work[i].p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
    work[i].result = 0;
    assert(work[i].p != NULL);
  }
  for (i = 0; i < RACE_PUBLISHERS; i++)
    assert(pthread_create(&threads[i], NULL, publish_one, &work[i]) == 0);
  for (i = 0; i < RACE_PUBLISHERS; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
    assert(work[i].result == 1);
  }
  assert(lj_arena_remote_free_drain(&alloc) == RACE_PUBLISHERS);
  for (i = 0; i < RACE_PUBLISHERS; i++)
    assert(lj_arena_lifetime_state_acq(lj_arena_of(work[i].p),
	lj_arena_cellof(work[i].p)) == LJ_ARENA_LIFETIME_FREE);
  lj_arena_alloc_fini(&alloc);
}

static void test_force_transfer_sweep_terminal(PRNGState *rs)
{
  TGAlloc src, dst, sweep, terminal;
  void *p, *live;
  GCArena *a;
  uint32_t cell;

  /* Transfer must scan even if an advisory wake was conservatively consumed. */
  lj_arena_alloc_init(&src);
  lj_arena_alloc_init(&dst);
  src.owner_tid = 0x1001u;
  dst.owner_tid = 0x2002u;
  p = lj_arena_alloc(&src, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p != NULL && lj_arena_remote_free_publish(&src, p, 64u));
  lj_arena_remote_pending_rel(&src, 0);
  assert(lj_arena_alloc_transfer(&dst, &src) == 1u);
  assert(la_loadptr_acq((void *const *)&lj_arena_of(p)->hdr.remote_free) ==
	 NULL);
  assert(lj_arena_alloc(&dst, rs, 64u, LJ_AF_TRAVERSABLE) == p);
  lj_arena_alloc_fini(&src);
  lj_arena_alloc_fini(&dst);

  /* Sweep has an exact arena and never consults the allocator hint. */
  lj_arena_alloc_init(&sweep);
  p = lj_arena_alloc(&sweep, rs, 64u, LJ_AF_TRAVERSABLE);
  live = lj_arena_alloc(&sweep, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p != NULL && live != NULL && lj_arena_of(p) == lj_arena_of(live));
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  assert(lj_arena_remote_free_publish(&sweep, p, 64u));
  lj_arena_remote_pending_rel(&sweep, 0);
  assert(lj_arena_alloc_prepare_sweep_kind(&sweep,
	LJ_ARENAK_TRAVERSABLE));
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING);
  lj_arena_alloc_sweep_kind(&sweep, LJ_ARENAK_TRAVERSABLE, 7u, 0);
  lj_arena_alloc_fini(&sweep);

  /* Terminal prefree uses the same force primitive after publishers join. */
  lj_arena_alloc_init(&terminal);
  p = lj_arena_alloc(&terminal, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p != NULL && lj_arena_remote_free_publish(&terminal, p, 64u));
  lj_arena_remote_pending_rel(&terminal, 0);
  assert(lj_arena_remote_free_drain_force(&terminal) == 1u);
  assert(la_loadptr_acq(
	(void *const *)&lj_arena_of(p)->hdr.remote_free) == NULL);
  lj_arena_alloc_fini(&terminal);
}

int main(void)
{
  PRNGState rs;

  lj_prng_seed_fixed(&rs);
  test_empty_elision(&rs);
  test_queue_before_wake(&rs);
  test_publish_after_clear(&rs);
  test_blocked_scan_rearms(&rs);
  test_multi_publisher_no_drop(&rs);
  test_force_transfer_sweep_terminal(&rs);
  printf("t-arena-remote-pending OK: empty scans elided, races and force drains preserve frees\n");
  return 0;
}
