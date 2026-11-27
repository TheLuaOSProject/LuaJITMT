/*
** Deterministic remote-free pending-hint publication/drain races.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"
#include "lj_tg.h"

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

static void *publish_routed_plain(void *ud)
{
  RemoteWork *w = (RemoteWork *)ud;
  w->result = lj_arena_remote_plain_free(w->p, 64u);
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
  GCArena *a;
  void *guard;
  uint32_t i, drained, reason = LJ_ARENA_FINISH_NONE, fillers = 0;
  const uint32_t kind = LJ_ARENAK_TRAVERSABLE;

  lj_arena_alloc_init(&alloc);
  alloc.alloc_black = 1;  /* Reclaim explicit free intent, not unmarked bytes. */
  for (i = 0; i < RACE_PUBLISHERS; i++) {
    work[i].alloc = &alloc;
    work[i].p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
    work[i].result = 0;
    assert(work[i].p != NULL);
    memset(work[i].p, 0x30 + (int)i, 64u);
    if (i)
      assert(lj_arena_of(work[i].p) == lj_arena_of(work[0].p));
  }
  a = lj_arena_of(work[0].p);
  guard = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(guard && lj_arena_of(guard) == a);
  memset(guard, 0xa7, 64u);
  /* Consume the unused bump with live opaque fillers. Reuse below must then
  ** come from the eight target bodies, not an unrelated fresh tail. */
  while (alloc.bump[kind].cell < alloc.bump[kind].end) {
    uint32_t cells = alloc.bump[kind].end - alloc.bump[kind].cell;
    void *p;
    if (cells > LJ_HUGE_THRESHOLD / LJ_CELL_SIZE)
      cells = LJ_HUGE_THRESHOLD / LJ_CELL_SIZE;
    p = lj_arena_alloc(&alloc, rs, (size_t)cells * LJ_CELL_SIZE,
                       LJ_AF_TRAVERSABLE);
    assert(p && lj_arena_of(p) == a && ++fillers < 32u);
  }
  /* Publisher 0 retains its admission while paused after the queue CAS.
  ** Each later publisher must defer: own_count=1 cannot exclude that reader.
  ** Its late bit retains the free without overwriting the still-live body. */
  lj_arena_test_remote_publish_pause(1);
  assert(pthread_create(&threads[0], NULL, publish_one, &work[0]) == 0);
  wait_seen(lj_arena_test_remote_publish_paused);
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == work[0].p);
  for (i = 1; i < RACE_PUBLISHERS; i++)
    assert(pthread_create(&threads[i], NULL, publish_one, &work[i]) == 0);
  for (i = 1; i < RACE_PUBLISHERS; i++) {
    unsigned j;
    assert(pthread_join(threads[i], NULL) == 0 && work[i].result == 1);
    assert(lj_arena_bm_get(a->block, lj_arena_cellof(work[i].p)));
    assert(lj_arena_late_get(a, lj_arena_cellof(work[i].p)));
    assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(work[i].p)) ==
           LJ_ARENA_LIFETIME_LIVE);
    for (j = 0; j < 64u; j++)
      assert(((unsigned char *)work[i].p)[j] == 0x30u + i);
  }
  lj_arena_test_remote_publish_pause(0);
  assert(pthread_join(threads[0], NULL) == 0 && work[0].result == 1);
  drained = lj_arena_remote_free_drain(&alloc);
  assert(drained == 1u);
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(work[0].p)));
  assert(!lj_arena_late_get(a, lj_arena_cellof(work[0].p)));
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
  for (i = 1; i < RACE_PUBLISHERS; i++) {
    uint32_t j, c = lj_arena_cellof(work[i].p);
    assert(lj_arena_bm_get(a->block, c) && lj_arena_late_get(a, c));
    assert(lj_arena_lifetime_state_acq(a, c) == LJ_ARENA_LIFETIME_LIVE);
    for (j = 0; j < 64u; j++)
      assert(((const uint8_t *)work[i].p)[j] == 0x30u + i);
  }
  /* Real allocator preparation and quarantine consume every durable late
  ** intent only after all publishers/readers joined. No repeated raw free. */
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, kind));
  for (i = 1; i < RACE_PUBLISHERS; i++) {
    uint32_t c = lj_arena_cellof(work[i].p);
    assert(lj_arena_bm_get(a->block, c) && lj_arena_late_get(a, c));
    assert(lj_arena_lifetime_state_acq(a, c) == LJ_ARENA_LIFETIME_FREE);
    assert(lj_arena_sweep_state_acq(a, c) == LJ_ARENA_SWEEP_FREEING);
  }
  assert(lj_arena_alloc_quarantine_one(&alloc, kind, 11u) == a);
  assert(lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(&alloc, kind, a, 31u, 0, &reason));
  assert(lj_arena_bm_get(a->block, lj_arena_cellof(guard)));
  assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(guard)) ==
         LJ_ARENA_LIFETIME_LIVE);
  for (i = 0; i < 64u; i++)
    assert(((const uint8_t *)guard)[i] == 0xa7);
  for (i = 0; i < RACE_PUBLISHERS; i++) {
    uint32_t c = lj_arena_cellof(work[i].p);
    assert(!lj_arena_bm_get(a->block, c) && !lj_arena_late_get(a, c));
    assert(lj_arena_lifetime_state_acq(a, c) == LJ_ARENA_LIFETIME_FREE);
  }
  for (i = 0; i < RACE_PUBLISHERS; i++) {
    void *p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
    assert(p == work[i].p);
  }
  assert(alloc.owned[kind] == a && lj_arena_next_acq(a) == NULL);
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

static void test_routed_plain_wake(PRNGState *rs)
{
  TGState src = {0}, dst = {0};
  LJTGRegistrySlot src_slot, dst_slot;
  RemoteWork work = {0};
  pthread_t publisher;
  GCArena *arena;
  void *retained, *independent;
  uint32_t cell;

  assert(lj_tgregistry_slot_init_unpublished(&src_slot, 0, NULL));
  assert(lj_tgregistry_slot_init_unpublished(&dst_slot, 0, NULL));
  src.registry_key.slot = &src_slot;
  dst.registry_key.slot = &dst_slot;
  lj_arena_alloc_init(&src.alloc);
  lj_arena_alloc_init(&dst.alloc);
  lj_arena_alloc_owner_rel(&src.alloc, 0x1001u);
  lj_arena_alloc_owner_rel(&dst.alloc, 0x2002u);
  lj_arena_alloc_owner_tg_rel(&src.alloc, &src);
  lj_arena_alloc_owner_tg_rel(&dst.alloc, &dst);
  work.p = lj_arena_alloc(&src.alloc, rs, 64u, 0);
  retained = lj_arena_alloc(&src.alloc, rs, 64u, 0);
  independent = lj_arena_alloc(&dst.alloc, rs, 64u, 0);
  assert(work.p && retained && independent);
  arena = lj_arena_of(work.p);
  cell = lj_arena_cellof(work.p);
  assert(lj_arena_of(retained) == arena);

  lj_arena_test_plain_admit_pause(1);
  assert(pthread_create(&publisher, NULL, publish_routed_plain, &work) == 0);
  wait_seen(lj_arena_test_plain_admit_paused);
  assert(la_load32_acq(&src_slot.plain_free_pending) == 0);
  assert(lj_arena_remote_free_drain_force(&src.alloc) == 0);
  assert(lj_arena_bm_get(arena->block, cell));
  assert(lj_arena_remote_free_publish(&dst.alloc, independent, 64u));
  assert(lj_arena_remote_free_drain(&dst.alloc) == 1);
  lj_arena_test_plain_admit_pause(0);
  assert(pthread_join(publisher, NULL) == 0 && work.result == 1);

  assert(lj_arena_late_get(arena, cell));
  assert(la_load32_acq(&src_slot.plain_free_pending) == 1);
  assert(lj_arena_rescue_enter(arena) == LJ_ARENA_RESCUE_FULL);
  assert(lj_arena_remote_free_drain(&src.alloc) == 0);
  lj_arena_rescue_leave(arena);
  assert(lj_arena_remote_free_drain(&src.alloc) == 1);
  assert(!lj_arena_bm_get(arena->block, cell));

  assert(lj_arena_alloc_transfer(&dst.alloc, &src.alloc) == 1);
  assert(lj_arena_owner_acq(arena) == lj_arena_alloc_owner_acq(&dst.alloc));
  assert(lj_arena_remote_plain_free(retained, 64u));
  assert(la_load32_acq(&src_slot.plain_free_pending) == 0);
  assert(la_load32_acq(&dst_slot.plain_free_pending) == 1);
  assert(lj_arena_remote_free_drain(&dst.alloc) == 1);
  lj_arena_alloc_fini(&src.alloc);
  lj_arena_alloc_fini(&dst.alloc);
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
  test_routed_plain_wake(&rs);
  printf("t-arena-remote-pending OK: empty scans elided, races and force drains preserve frees\n");
  return 0;
}
