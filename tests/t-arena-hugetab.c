/*
** Focused test for the huge-object side table scaffold.
*/

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

static void check_info(const LJHugeInfo *hi, size_t size, uint32_t flags)
{
  assert(hi->size == size);
  assert(hi->flags == flags);
}

static void delete_unmap(HugeTab *ht, void *p)
{
  LJHugeInfo hi;
  assert(lj_arena_hugetab_delete(ht, p, &hi) == 1);
  assert(lj_arena_hugetab_lookup(ht, p, NULL) == 0);
  assert(lj_arena_hugetab_delete(ht, p, NULL) == 0);
  lj_arena_huge_unmap(p, hi.size);
}

static void test_recovery_state(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 1901u;
  HugeTab src = { NULL }, dst = { NULL };
  LJHugeInfo hi;
  void *p, *found = NULL;
  uint32_t cursor, seen;

  assert(lj_arena_hugetab_init(&src, 2) == 1);
  assert(lj_arena_hugetab_init(&dst, 2) == 1);
  p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&src, p, size,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_recovery_state_acq(&src, p, &hi) ==
	 LJ_ARENA_RECOVERY_IDLE);
  /* A recovery identity cannot expose a READY0 header. */
  assert(!lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));
  assert(lj_arena_hugetab_publish_gco(&src, p) == 1);
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, &hi));
  assert(lj_arena_huge_recovery_state(hi.flags) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert((hi.flags & LJ_HUGEF_MARK) != 0);

  cursor = 0;
  assert(lj_arena_hugetab_recovery_next(&src, &cursor, &found, &hi));
  assert(found == p && hi.size == size);
  assert(!lj_arena_hugetab_recovery_next(&src, &cursor, &found, NULL));
  cursor = seen = 0;
  while (lj_arena_hugetab_next(&src, &cursor, &found, &hi)) {
    assert(found == p);
    seen++;
  }
  assert(seen == 1u);

  /* Every physical ownership route must fail closed while recovery is live. */
  assert(!lj_arena_hugetab_delete(&src, p, NULL));
  assert(!lj_arena_hugetab_claim_external_free(&src, p, NULL));
  assert(lj_arena_hugetab_lookup(&src, p, &hi));
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0);
  assert(!lj_arena_hugetab_forget_terminal(&src, p, NULL));
  assert(lj_arena_hugetab_fini_all(&src) == 0u);
  assert(src.h != NULL && lj_arena_hugetab_lookup(&src, p, NULL));
  lj_arena_hugetab_clear_marks(&src);
  assert(lj_arena_hugetab_lookup(&src, p, &hi));
  assert((hi.flags & LJ_HUGEF_MARK) != 0);
  assert(!lj_arena_hugetab_transfer(&dst, &src, 77u));
  assert(lj_arena_hugetab_lookup(&src, p, NULL));
  assert(!lj_arena_hugetab_lookup(&dst, p, NULL));

  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED, NULL));
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_CLAIMED, LJ_ARENA_RECOVERY_REDIRTY, NULL));
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_REDIRTY, LJ_ARENA_RECOVERY_PENDING, NULL));

  /* Retirement preserves state, forces liveness and cannot publish RETIRED. */
  lj_arena_hugetab_prepare_sweep(&src);
  assert(lj_arena_hugetab_retire(&src, p, p, 91u, &hi) == 2);
  assert((hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_MARK|LJ_HUGEF_TICKET));
  assert((hi.flags & LJ_HUGEF_RETIRED) == 0);
  assert(lj_arena_huge_recovery_state(hi.flags) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(lj_arena_hugetab_claim_live_ticket(&src, p, NULL));
  la_storeptr_rel(&lj_arena_of(p)->hdr.retire_obj, NULL);
  assert(lj_arena_hugetab_finish_live_ticket(&src, p, NULL));
  lj_arena_hugetab_finish_sweep(&src, 0);
  assert(lj_arena_hugetab_lookup(&src, p, &hi));
  assert((hi.flags & LJ_HUGEF_SWEEP_OLD) != 0);

  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED, NULL));
  /* Generic clearing cannot forget the logical free. Completion atomically
  ** converts it into a fresh-grace, sweep-owned terminal mapping. */
  assert(!lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_CLAIMED, LJ_ARENA_RECOVERY_IDLE, NULL));
  assert(lj_arena_hugetab_recovery_complete(&src, p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP);
  assert(lj_arena_hugetab_lookup(&src, p, &hi));
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING));
  assert((hi.flags & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_RECOVERY_MASK|
		      LJ_HUGEF_MARK)) == 0);
  assert(la_load64_acq(&lj_arena_of(p)->hdr.retire_epoch) ==
	 ~(uint64_t)0);
  delete_unmap(&src, p);

  /* Without a logical free, completion retains the live mapping. */
  p = lj_arena_huge_map(rs, size + 1u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&src, p, size + 1u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_publish_gco(&src, p));
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED, NULL));
  assert(lj_arena_hugetab_recovery_complete(&src, p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_COMPLETE_LIVE);
  assert(lj_arena_hugetab_lookup(&src, p, &hi));
  assert(lj_arena_huge_recovery_state(hi.flags) ==
	 LJ_ARENA_RECOVERY_IDLE);
  delete_unmap(&src, p);

  /* Even before sweep ownership exists, completion publishes a fresh-grace
  ** sweep handoff rather than unmapping inside an SMR read-side drain. */
  p = lj_arena_huge_map(rs, size + 2u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&src, p, size + 2u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_publish_gco(&src, p));
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));
  assert(!lj_arena_hugetab_claim_external_free(&src, p, &hi));
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0);
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED, NULL));
  assert(lj_arena_hugetab_recovery_complete(&src, p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP);
  assert(hi.size == size + 2u);
  assert(lj_arena_hugetab_lookup(&src, p, &hi));
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING));
  assert(la_load64_acq(&lj_arena_of(p)->hdr.retire_epoch) ==
	 ~(uint64_t)0);
  delete_unmap(&src, p);

  /* Terminal reconciliation runs only after all publishers have joined. It
  ** clears ordinary recovery for later table teardown, but a logical free is
  ** consumed by an exact tombstone and explicit unmap handoff. */
  p = lj_arena_huge_map(rs, size + 3u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&src, p, size + 3u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_publish_gco(&src, p));
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));
  assert(lj_arena_hugetab_recovery_discard_terminal(&src, p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_TERMINAL_CLEARED);
  assert(lj_arena_huge_recovery_state(hi.flags) ==
	 LJ_ARENA_RECOVERY_IDLE);
  assert(lj_arena_hugetab_recovery_discard_terminal(&src, p, NULL) ==
	 LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST);
  delete_unmap(&src, p);

  p = lj_arena_huge_map(rs, size + 4u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&src, p, size + 4u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_publish_gco(&src, p));
  assert(lj_arena_hugetab_recovery_state_cas(&src, p,
	 LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));
  assert(!lj_arena_hugetab_claim_external_free(&src, p, NULL));
  assert(lj_arena_hugetab_recovery_discard_terminal(&src, p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_TERMINAL_UNMAP);
  assert(hi.size == size + 4u);
  assert(!lj_arena_hugetab_lookup(&src, p, NULL));
  lj_arena_huge_unmap(p, hi.size);
  lj_arena_hugetab_fini(&src);
  lj_arena_hugetab_fini(&dst);
}

#if defined(LJ_ARENA_TEST_HELPERS)
typedef struct RetireRace {
  HugeTab *ht;
  void *p;
  void *obj;
  uint64_t epoch;
  LJHugeInfo hi;
  int result;
} RetireRace;

static void *retire_race_thread(void *ud)
{
  RetireRace *race = (RetireRace *)ud;
  race->result = lj_arena_hugetab_retire(race->ht, race->p, race->obj,
					 race->epoch, &race->hi);
  return NULL;
}

static void retire_race_wait_busy(void)
{
  while (!lj_arena_hugetab_test_retire_paused())
    la_cpu_pause();
}

static void retire_race_cleanup(HugeTab *ht, void *p, void *obj)
{
  LJHugeInfo hi;
  assert(lj_arena_hugetab_claim_live_ticket(ht, p, &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET|LJ_HUGEF_BUSY)) ==
	 (LJ_HUGEF_MARK|LJ_HUGEF_TICKET|LJ_HUGEF_BUSY));
  assert(la_loadptr_acq((void *const *)&lj_arena_of(p)->hdr.retire_obj) == obj);
  la_storeptr_rel(&lj_arena_of(p)->hdr.retire_obj, NULL);
  assert(lj_arena_hugetab_finish_live_ticket(ht, p, &hi) == 1);
  lj_arena_hugetab_finish_sweep(ht, 0);
  delete_unmap(ht, p);
  lj_arena_hugetab_fini(ht);
}

static void test_retire_busy_mark_intent(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 1537u;
  HugeTab raceht = { NULL };
  RetireRace race;
  pthread_t thread;
  LJHugeInfo hi;
  void *base = (void *)(uintptr_t)1u;

  assert(lj_arena_hugetab_init(&raceht, 2) == 1);
  race.p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(race.p != NULL);
  race.obj = (char *)race.p + 16u;
  race.ht = &raceht;
  race.epoch = 101u;
  race.result = 0;
  assert(lj_arena_hugetab_insert(&raceht, race.p, size,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_publish_interior_cdata(&raceht, race.p) == 1);
  lj_arena_hugetab_prepare_sweep(&raceht);

  lj_arena_hugetab_test_retire_pause(1);
  assert(pthread_create(&thread, NULL, retire_race_thread, &race) == 0);
  retire_race_wait_busy();
  assert(lj_arena_hugetab_lookup(&raceht, race.p, &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_BUSY)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_BUSY));
  assert((hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET|LJ_HUGEF_FREEING)) == 0);
  assert(la_loadptr_acq((void *const *)&lj_arena_of(race.p)->hdr.retire_obj) ==
	 NULL);

  /* The marker must neither wait for the paused owner nor expose a base whose
  ** retire_obj header has not been published. Duplicate markers retain the
  ** same opaque intent; the unique retire owner discharges one traversal. */
  assert(lj_arena_hugetab_mark_cdata_range(&raceht, race.obj, &base, &hi) ==
	 LJ_ARENA_HUGE_MARK_INTENT);
  assert(base == NULL);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_BUSY|LJ_HUGEF_MARK)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_BUSY|LJ_HUGEF_MARK));
  assert((hi.flags & (LJ_HUGEF_TICKET|LJ_HUGEF_FREEING)) == 0);
  assert(lj_arena_hugetab_mark_cdata_range(&raceht, race.obj, &base, &hi) ==
	 LJ_ARENA_HUGE_MARK_INTENT);
  assert(base == NULL);
  assert(lj_arena_hugetab_mark(&raceht, race.p, &hi) == 0);

  lj_arena_hugetab_test_retire_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(race.result == 2);  /* Unique retire owner discharges the intent. */
  assert((race.hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK|
			   LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK|LJ_HUGEF_TICKET));
  assert((race.hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_RETIRED|
			   LJ_HUGEF_FREEING)) == 0);
  retire_race_cleanup(&raceht, race.p, race.obj);
}

static void test_realloc_shaped_busy_mark_only(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 1777u;
  HugeTab ht = { NULL };
  LJHugeInfo hi;
  void *p, *base = (void *)(uintptr_t)1u;

  assert(lj_arena_hugetab_init(&ht, 2) == 1);
  p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  /* Model a raw allocation whose realloc claim won after SWEEP_OLD. It is
  ** conservatively candidate-shaped but has no GC header graph and no retire
  ** owner that could discharge traversal. Mapping liveness alone is exact. */
  assert(lj_arena_hugetab_insert(&ht, p, size,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY|LJ_HUGEF_SWEEP_OLD|
	LJ_HUGEF_BUSY) == 1);
  assert(lj_arena_hugetab_mark_range(&ht, p, &base, &hi) ==
	 LJ_ARENA_HUGE_MARK_INTENT);
  assert(base == NULL);
  assert((hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_BUSY)) ==
	 (LJ_HUGEF_MARK|LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_BUSY));
  assert((hi.flags & LJ_HUGEF_TICKET) == 0);
  assert(la_loadptr_acq((void *const *)&lj_arena_of(p)->hdr.retire_obj) ==
	 NULL);
  assert(lj_arena_hugetab_fini_all(&ht) == 1u);
  lj_arena_hugetab_fini(&ht);
}

static void test_retire_busy_exact_mark(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 1663u;
  HugeTab raceht = { NULL };
  RetireRace race;
  pthread_t thread;
  LJHugeInfo hi;

  assert(lj_arena_hugetab_init(&raceht, 2) == 1);
  race.p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(race.p != NULL);
  race.obj = race.p;
  race.ht = &raceht;
  race.epoch = 102u;
  race.result = 0;
  assert(lj_arena_hugetab_insert(&raceht, race.p, size,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_publish_gco(&raceht, race.p) == 1);
  lj_arena_hugetab_prepare_sweep(&raceht);

  lj_arena_hugetab_test_retire_pause(1);
  assert(pthread_create(&thread, NULL, retire_race_thread, &race) == 0);
  retire_race_wait_busy();
  /* Exact/raw roots need no payload admission. They publish MARK and return
  ** immediately while the retire owner is deliberately paused. */
  assert(lj_arena_hugetab_mark(&raceht, race.p, &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_BUSY|LJ_HUGEF_MARK)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_BUSY|LJ_HUGEF_MARK));
  lj_arena_hugetab_test_retire_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(race.result == 2);  /* BUSY-window exact MARK has the same owner. */
  assert((race.hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK|
			   LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_MARK|LJ_HUGEF_TICKET));
  assert((race.hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_RETIRED|
			   LJ_HUGEF_FREEING)) == 0);
  retire_race_cleanup(&raceht, race.p, race.obj);
}

static void test_recovery_deferred_busy_requeue(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 1811u;
  HugeTab raceht = { NULL };
  RetireRace race;
  pthread_t thread;
  LJHugeInfo hi;

  assert(lj_arena_hugetab_init(&raceht, 2) == 1);
  race.p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(race.p != NULL);
  race.obj = race.p;
  race.ht = &raceht;
  race.epoch = 103u;
  race.result = 0;
  assert(lj_arena_hugetab_insert(&raceht, race.p, size,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_publish_gco(&raceht, race.p));
  lj_arena_hugetab_prepare_sweep(&raceht);
  assert(lj_arena_hugetab_recovery_state_cas(&raceht, race.p,
	 LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));

  lj_arena_hugetab_test_retire_pause(1);
  assert(pthread_create(&thread, NULL, retire_race_thread, &race) == 0);
  retire_race_wait_busy();
  assert(!lj_arena_hugetab_claim_external_free(&raceht, race.p, &hi));
  assert((hi.flags & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_BUSY)) ==
	 (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_BUSY));
  assert(lj_arena_hugetab_recovery_state_cas(&raceht, race.p,
	 LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED, NULL));
  assert(lj_arena_hugetab_recovery_complete(&raceht, race.p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_COMPLETE_REQUEUED);
  assert(lj_arena_huge_recovery_state(hi.flags) ==
	 LJ_ARENA_RECOVERY_PENDING);

  lj_arena_hugetab_test_retire_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(race.result == 2);
  assert(lj_arena_hugetab_recovery_state_cas(&raceht, race.p,
	 LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED, NULL));
  assert(lj_arena_hugetab_recovery_complete(&raceht, race.p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP);
  assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD|
		      LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TICKET));
  assert((hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE|
		      LJ_HUGEF_RECOVERY_MASK|LJ_HUGEF_MARK)) == 0);
  assert(la_load64_acq(&lj_arena_of(race.p)->hdr.retire_epoch) ==
	 ~(uint64_t)0);
  delete_unmap(&raceht, race.p);
  lj_arena_hugetab_fini(&raceht);
}
#endif

int main(void)
{
  static const size_t sizes[] = {
    LJ_HUGE_THRESHOLD + 1u,
    LJ_HUGE_THRESHOLD + 257u,
    LJ_ARENA_SIZE + 17u,
    (size_t)LJ_ARENA_SIZE * 2u + 333u
  };
  PRNGState rs;
  HugeTab ht = { NULL };
  HugeTab tiny = { NULL };
  HugeTab src = { NULL };
  HugeTab dst = { NULL };
  void *ptrs[sizeof(sizes)/sizeof(sizes[0])];
  void *racep;
  void *rangep, *rangebase;
  LJHugeInfo hi;
  uint32_t i;

  lj_prng_seed_fixed(&rs);
  test_recovery_state(&rs);
#if defined(LJ_ARENA_TEST_HELPERS)
  test_retire_busy_mark_intent(&rs);
  test_retire_busy_exact_mark(&rs);
  test_recovery_deferred_busy_requeue(&rs);
#endif
  test_realloc_shaped_busy_mark_only(&rs);
  assert(lj_arena_hugetab_init(&ht, 4) == 1);
  assert(lj_arena_hugetab_lookup(&ht, (void *)0x12340, &hi) == 0);
  assert(lj_arena_hugetab_mark(&ht, (void *)0x12340, &hi) == -1);

  for (i = 0; i < (uint32_t)(sizeof(ptrs)/sizeof(ptrs[0])); i++) {
    uint32_t flags = (i & 1u) ? LJ_HUGEF_TRAVERSABLE : LJ_HUGEF_FINALIZER;
    ptrs[i] = lj_arena_huge_map(&rs, sizes[i],
				(flags & LJ_HUGEF_TRAVERSABLE) ?
				LJ_AF_TRAVERSABLE : 0);
    assert(ptrs[i] != NULL);
    assert(lj_arena_hugetab_insert(&ht, ptrs[i], sizes[i], flags) == 1);
    assert(lj_arena_hugetab_lookup(&ht, ptrs[i], &hi) == 1);
    check_info(&hi, sizes[i], flags);
  }

  /* Containing marks use one full-slot CAS as lookup+lifetime LP and retain
  ** the post-initialization interior-cdata identity bit. */
  rangep = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 1234u,
			     LJ_AF_TRAVERSABLE);
  assert(rangep != NULL);
  assert(lj_arena_hugetab_insert(&ht, rangep, LJ_HUGE_THRESHOLD + 1234u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  rangebase = NULL;
  assert(lj_arena_hugetab_mark_range(&ht, (char *)rangep + 1,
				     &rangebase, &hi) == -1);
  assert(rangebase == NULL);
  assert(lj_arena_hugetab_lookup(&ht, rangep, &hi) == 1);
  check_info(&hi, LJ_HUGE_THRESHOLD + 1234u, LJ_HUGEF_TRAVERSABLE);
  assert(lj_arena_hugetab_publish_interior_cdata(&ht, rangep) == 1);
  assert(lj_arena_hugetab_lookup(&ht, rangep, &hi) == 1);
  check_info(&hi, LJ_HUGE_THRESHOLD + 1234u,
	     LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_CDATA|
	     LJ_HUGEF_INTERIOR_CDATA|LJ_HUGEF_READY);
  /* A stale allocation-base edge cannot mark a tagged interior-header
  ** allocation. The strict cdata candidate shape rejects without mutation. */
  assert(lj_arena_hugetab_mark_cdata_range(&ht, rangep,
					   &rangebase, &hi) == -1);
  assert(lj_arena_hugetab_lookup(&ht, rangep, &hi) == 1);
  assert((hi.flags & LJ_HUGEF_MARK) == 0);
  assert(lj_arena_hugetab_mark_cdata_range(&ht, (char *)rangep + 1,
					   &rangebase, &hi) == 1);
  assert(rangebase == rangep);
  check_info(&hi, LJ_HUGE_THRESHOLD + 1234u,
	     LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_CDATA|
	     LJ_HUGEF_INTERIOR_CDATA|LJ_HUGEF_READY|LJ_HUGEF_MARK);
  assert(lj_arena_hugetab_mark_range(
    &ht, (char *)rangep + LJ_HUGE_THRESHOLD + 1233u,
    &rangebase, &hi) == 0);
  assert(lj_arena_hugetab_mark_range(
    &ht, (char *)rangep + LJ_HUGE_THRESHOLD + 1234u,
    &rangebase, &hi) == -1);
  delete_unmap(&ht, rangep);

  assert(lj_arena_hugetab_insert(&ht, ptrs[0], sizes[0] + 16u,
				 LJ_HUGEF_MARK) == 0);
  assert(lj_arena_hugetab_lookup(&ht, ptrs[0], &hi) == 1);
  check_info(&hi, sizes[0], LJ_HUGEF_FINALIZER);

  assert(lj_arena_hugetab_mark(&ht, ptrs[1], &hi) == 1);
  check_info(&hi, sizes[1], LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_MARK);
  assert(lj_arena_huge_mapsize(sizes[1]) != sizes[1]);
  assert(lj_arena_hugetab_live_bytes(&ht, LJ_HUGEF_TRAVERSABLE) ==
	 sizes[1] + sizes[3]);
  assert(lj_arena_hugetab_live_bytes(&ht,
    LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_MARK) == sizes[1]);
  assert(lj_arena_hugetab_mark(&ht, ptrs[1], &hi) == 0);
  check_info(&hi, sizes[1], LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_MARK);
  assert(lj_arena_hugetab_lookup(&ht, ptrs[1], &hi) == 1);
  check_info(&hi, sizes[1], LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_MARK);
  lj_arena_hugetab_clear_marks(&ht);
  assert(lj_arena_hugetab_lookup(&ht, ptrs[1], &hi) == 1);
  check_info(&hi, sizes[1], LJ_HUGEF_TRAVERSABLE);
  assert(lj_arena_hugetab_live_bytes(&ht,
    LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_MARK) == 0);

  /* Deterministic prepare-vs-free ordering, free first. The one pair-CAS
  ** claim pins both slot halves, so prepare cannot add SWEEP_OLD and neither a
  ** duplicate free nor the generic deleter can acquire the mapping. */
  racep = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 777u,
			    LJ_AF_TRAVERSABLE);
  assert(racep != NULL);
  assert(lj_arena_hugetab_insert(&ht, racep, LJ_HUGE_THRESHOLD + 777u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_claim_external_free(&ht, racep, &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_BUSY));
  assert((hi.flags & LJ_HUGEF_SWEEP_OLD) == 0);
  assert(lj_arena_hugetab_mark_range(&ht, (char *)racep + 1,
				     &rangebase, &hi) == -1);
  lj_arena_hugetab_prepare_sweep(&ht);
  assert(lj_arena_hugetab_lookup(&ht, racep, &hi) == 1);
  assert((hi.flags & LJ_HUGEF_SWEEP_OLD) == 0);
  assert(lj_arena_hugetab_claim_external_free(&ht, racep, NULL) == 0);
  assert(lj_arena_hugetab_delete(&ht, racep, NULL) == 0);
  assert(lj_arena_hugetab_finish_external_free(&ht, racep, &hi) ==
	 LJ_ARENA_HUGE_FINISH_UNMAP);
  assert(hi.size == LJ_HUGE_THRESHOLD + 777u);
  assert(lj_arena_hugetab_lookup(&ht, racep, NULL) == 0);
  assert(lj_arena_hugetab_finish_external_free(&ht, racep, NULL) ==
	 LJ_ARENA_HUGE_FINISH_LOST);
  lj_arena_huge_unmap(racep, hi.size);
  /* These stale operations consult only the tombstoned table slot. */
  assert(lj_arena_hugetab_claim_external_free(&ht, racep, NULL) == 0);
  assert(lj_arena_hugetab_defer_external_free(&ht, racep, NULL) == 0);
  lj_arena_hugetab_abort_sweep(&ht);
  lj_arena_hugetab_clear_marks(&ht);

  /* Prepare first. The same claim now retains SWEEP_OLD and BUSY through the
  ** fresh-grace header publication, then finish can only defer to sweep. */
  racep = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 888u,
			    LJ_AF_TRAVERSABLE);
  assert(racep != NULL);
  assert(lj_arena_hugetab_insert(&ht, racep, LJ_HUGE_THRESHOLD + 888u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  lj_arena_hugetab_prepare_sweep(&ht);
  assert(lj_arena_hugetab_lookup(&ht, racep, &hi) == 1);
  assert((hi.flags & LJ_HUGEF_SWEEP_OLD) != 0);
  assert(lj_arena_hugetab_claim_external_free(&ht, racep, &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING|
		      LJ_HUGEF_BUSY)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING|LJ_HUGEF_BUSY));
  assert(la_load64_acq(&lj_arena_of(racep)->hdr.retire_epoch) ==
	 ~(uint64_t)0);
  assert(lj_arena_hugetab_retire(&ht, racep, racep, 23u, NULL) == 0);
  assert(la_load64_acq(&lj_arena_of(racep)->hdr.retire_epoch) ==
	 ~(uint64_t)0);  /* A losing retire never overwrites the sentinel. */
  assert(lj_arena_hugetab_delete(&ht, racep, NULL) == 0);
  assert(lj_arena_hugetab_finish_external_free(&ht, racep, &hi) ==
	 LJ_ARENA_HUGE_FINISH_DEFERRED);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING));
  assert((hi.flags & LJ_HUGEF_BUSY) == 0);
  /* Root detachment which arrives after the publisher may still ticket the
  ** exact object, but it must preserve the external fresh-grace sentinel. */
  assert(lj_arena_hugetab_retire(&ht, racep, racep, 24u, &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_TICKET));
  assert(la_load64_acq(&lj_arena_of(racep)->hdr.retire_epoch) ==
	 ~(uint64_t)0);
  assert(lj_arena_hugetab_claim_external_free(&ht, racep, NULL) == 0);
  assert(lj_arena_hugetab_defer_external_free(&ht, racep, NULL) == 1);
  assert(lj_arena_hugetab_delete(&ht, racep, &hi) == 1);
  lj_arena_huge_unmap(racep, hi.size);
  lj_arena_hugetab_abort_sweep(&ht);
  lj_arena_hugetab_clear_marks(&ht);

  /* A concurrent mark wins retirement by clearing RETIRED, but the explicit
  ** detached-root TICKET must continue to block finish until reanchor. */
  lj_arena_hugetab_prepare_sweep(&ht);
  assert(lj_arena_hugetab_retire(&ht, ptrs[3], ptrs[3], 17u, &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_RETIRED|
		      LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_RETIRED|LJ_HUGEF_TICKET));
  assert(lj_arena_hugetab_mark(&ht, ptrs[3], &hi) == 2);
  assert((hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_MARK|LJ_HUGEF_TICKET));
  assert((hi.flags & LJ_HUGEF_RETIRED) == 0);
  lj_arena_hugetab_finish_sweep(&ht, 0);
  assert(lj_arena_hugetab_lookup(&ht, ptrs[3], &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TICKET));
  assert(lj_arena_hugetab_claim_live_ticket(&ht, ptrs[3], &hi) == 1);
  assert((hi.flags & LJ_HUGEF_BUSY) != 0);
  la_storeptr_rel(&lj_arena_of(ptrs[3])->hdr.retire_obj, NULL);
  assert(lj_arena_hugetab_finish_live_ticket(&ht, ptrs[3], &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_TICKET|LJ_HUGEF_BUSY)) == 0);
  lj_arena_hugetab_finish_sweep(&ht, 0);
  assert(lj_arena_hugetab_lookup(&ht, ptrs[3], &hi) == 1);
  assert((hi.flags & LJ_HUGEF_SWEEP_OLD) == 0);

  /* Model the other CAS ordering explicitly: MARK is already visible before
  ** retire publishes its metadata ticket. Provenance is not encoded, so the
  ** unique retire owner must request a conservative duplicate traversal. */
  lj_arena_hugetab_prepare_sweep(&ht);
  assert(lj_arena_hugetab_mark(&ht, ptrs[3], &hi) == 1);
  assert(lj_arena_hugetab_retire(&ht, ptrs[3], ptrs[3], 19u, &hi) == 2);
  assert((hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_MARK|LJ_HUGEF_TICKET));
  assert((hi.flags & LJ_HUGEF_RETIRED) == 0);
  assert(lj_arena_hugetab_claim_live_ticket(&ht, ptrs[3], NULL) == 1);
  la_storeptr_rel(&lj_arena_of(ptrs[3])->hdr.retire_obj, NULL);
  assert(lj_arena_hugetab_finish_live_ticket(&ht, ptrs[3], NULL) == 1);
  lj_arena_hugetab_finish_sweep(&ht, 0);

  /* External FREEING publication owns the mapping through BUSY while it
  ** stores the fresh-grace sentinel, then exposes a ready terminal entry. */
  assert(lj_arena_hugetab_defer_external_free(&ht, ptrs[1], &hi) == 1);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING));
  assert((hi.flags & LJ_HUGEF_BUSY) == 0);
  assert(la_load64_acq(&lj_arena_of(ptrs[1])->hdr.retire_epoch) ==
	 ~(uint64_t)0);

  for (i = 0; i < (uint32_t)(sizeof(ptrs)/sizeof(ptrs[0])); i++)
    delete_unmap(&ht, ptrs[i]);
  lj_arena_hugetab_fini(&ht);
  assert(ht.h == NULL);

  assert(lj_arena_hugetab_init(&tiny, 0) == 1);
  ptrs[0] = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 11u, 0);
  ptrs[1] = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 22u, 0);
  ptrs[2] = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 33u, 0);
  assert(ptrs[0] != NULL && ptrs[1] != NULL && ptrs[2] != NULL);

  assert(lj_arena_hugetab_insert(&tiny, ptrs[0],
				 LJ_HUGE_THRESHOLD + 11u, 0) == 1);
  assert(lj_arena_hugetab_delete(&tiny, ptrs[0], &hi) == 1);
  check_info(&hi, LJ_HUGE_THRESHOLD + 11u, 0);
  lj_arena_huge_unmap(ptrs[0], hi.size);

  assert(lj_arena_hugetab_insert(&tiny, ptrs[1],
				 LJ_HUGE_THRESHOLD + 22u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_insert(&tiny, ptrs[2],
				 LJ_HUGE_THRESHOLD + 33u, 0) == -1);
  assert(lj_arena_hugetab_delete(&tiny, ptrs[1], &hi) == 1);
  check_info(&hi, LJ_HUGE_THRESHOLD + 22u, LJ_HUGEF_TRAVERSABLE);
  lj_arena_huge_unmap(ptrs[1], hi.size);
  lj_arena_huge_unmap(ptrs[2], LJ_HUGE_THRESHOLD + 33u);
  lj_arena_hugetab_fini(&tiny);
  assert(tiny.h == NULL);

  assert(lj_arena_hugetab_init(&src, 4) == 1);
  assert(lj_arena_hugetab_init(&dst, 4) == 1);
  ptrs[0] = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 4096u,
			      LJ_AF_TRAVERSABLE);
  assert(ptrs[0] != NULL);
  lj_arena_of(ptrs[0])->hdr.owner_tid = 0x1234u;
  assert(lj_arena_hugetab_insert(&src, ptrs[0],
				 LJ_HUGE_THRESHOLD + 4096u,
				 LJ_HUGEF_TRAVERSABLE) == 1);
  assert(lj_arena_hugetab_publish_interior_cdata(&src, ptrs[0]) == 1);
  assert(lj_arena_hugetab_transfer(&dst, &src, 0x5678u) == 1);
  assert(lj_arena_of(ptrs[0])->hdr.owner_tid == 0x5678u);
  assert(lj_arena_hugetab_lookup(&src, ptrs[0], NULL) == 0);
  assert(lj_arena_hugetab_lookup(&dst, ptrs[0], &hi) == 1);
  check_info(&hi, LJ_HUGE_THRESHOLD + 4096u,
	     LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_CDATA|
	     LJ_HUGEF_INTERIOR_CDATA|LJ_HUGEF_READY);
  delete_unmap(&dst, ptrs[0]);
  lj_arena_hugetab_fini(&src);
  lj_arena_hugetab_fini(&dst);
  assert(src.h == NULL);
  assert(dst.h == NULL);

  /* A dead-owner transfer is transactional per entry even when the source
  ** carries abandoned BUSY/FREEING state and dst fills partway through. The
  ** moved prefix must be destination-only and the unmoved suffix source-only;
  ** physically freeing the moved mapping before terminal source destruction
  ** must neither dereference it nor attempt a second unmap. */
  assert(lj_arena_hugetab_init(&src, 2) == 1);
  assert(lj_arena_hugetab_init(&dst, 0) == 1);
  ptrs[0] = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 501u, 0);
  ptrs[1] = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 502u, 0);
  assert(ptrs[0] != NULL && ptrs[1] != NULL);
  lj_arena_owner_rel(lj_arena_of(ptrs[0]), 0x1111u);
  lj_arena_owner_rel(lj_arena_of(ptrs[1]), 0x1111u);
  assert(lj_arena_hugetab_insert(&src, ptrs[0],
	LJ_HUGE_THRESHOLD + 501u, LJ_HUGEF_BUSY) == 1);
  assert(lj_arena_hugetab_insert(&src, ptrs[1],
	LJ_HUGE_THRESHOLD + 502u, LJ_HUGEF_FREEING|LJ_HUGEF_BUSY) == 1);
  assert(lj_arena_hugetab_transfer(&dst, &src, 0x2222u) == 0);
  {
    int s0 = lj_arena_hugetab_lookup(&src, ptrs[0], NULL);
    int d0 = lj_arena_hugetab_lookup(&dst, ptrs[0], NULL);
    int s1 = lj_arena_hugetab_lookup(&src, ptrs[1], NULL);
    int d1 = lj_arena_hugetab_lookup(&dst, ptrs[1], NULL);
    void *moved = d0 ? ptrs[0] : ptrs[1];
    assert((s0 != 0) != (d0 != 0));
    assert((s1 != 0) != (d1 != 0));
    assert((d0 != 0) + (d1 != 0) == 1);
    assert(lj_arena_owner_acq(lj_arena_of(moved)) == 0x2222u);
    assert(lj_arena_hugetab_forget_terminal(&dst, moved, &hi) == 1);
    lj_arena_huge_unmap(moved, hi.size);
  }
  errno = EDOM;
  assert(lj_arena_hugetab_fini_all(&src) == 1u);
  assert(errno == EDOM);
  assert(src.h == NULL);
  errno = ERANGE;
  assert(lj_arena_hugetab_fini_all(&dst) == 0u);
  assert(errno == ERANGE);
  assert(dst.h == NULL);

  /* Functional terminal forget excludes a still-live mapping even when its
  ** slot has a state which ordinary delete must refuse (the GG close path uses
  ** this before retaining GG's final manual unmap). */
  assert(lj_arena_hugetab_init(&src, 1) == 1);
  ptrs[2] = lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD + 503u, 0);
  assert(ptrs[2] != NULL);
  assert(lj_arena_hugetab_insert(&src, ptrs[2],
	LJ_HUGE_THRESHOLD + 503u, LJ_HUGEF_BUSY|LJ_HUGEF_FREEING) == 1);
  assert(lj_arena_hugetab_forget_terminal(&src, ptrs[2], &hi) == 1);
  check_info(&hi, LJ_HUGE_THRESHOLD + 503u,
	     LJ_HUGEF_BUSY|LJ_HUGEF_FREEING);
  assert(lj_arena_hugetab_lookup(&src, ptrs[2], NULL) == 0);
  lj_arena_huge_unmap(ptrs[2], hi.size);
  assert(lj_arena_hugetab_fini_all(&src) == 0u);

  printf("t-arena-hugetab OK: insert lookup mark live delete tombstone full "
	 "terminal\n");
  return 0;
}
