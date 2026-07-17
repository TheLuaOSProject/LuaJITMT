/*
** Focused test for the huge-object side table scaffold.
*/

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"
#include "lj_tg.h"

typedef struct FreeRunProbe {
  uint32_t cell;
  uint32_t start;
  uint32_t len;
  int found;
} FreeRunProbe;

static void find_free_run_covering(uint32_t start, uint32_t len, void *ud)
{
  FreeRunProbe *probe = (FreeRunProbe *)ud;
  if (!probe->found && probe->cell >= start && probe->cell - start < len) {
    probe->start = start;
    probe->len = len;
    probe->found = 1;
  }
}

static void check_info(const LJHugeInfo *hi, size_t size, uint32_t flags)
{
  assert(hi->size == size);
  assert(hi->flags == flags);
  assert(hi->readers == 0);
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

static void test_root_state(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 2039u;
  const uint32_t baseflags = LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY|
    LJ_HUGEF_CDATA|LJ_HUGEF_MARK;
  HugeTab src = { NULL }, dst = { NULL };
  LJHugeInfo hi;
  void *p;

  assert(lj_arena_hugetab_init(&src, 2) == 1);
  assert(lj_arena_hugetab_init(&dst, 2) == 1);
  p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  /* Fresh insertion owns initialization and must never import a live root
  ** state. Dead-owner transfer below is the sole state-preserving insertion. */
  assert(lj_arena_hugetab_insert(&src, p, size,
	baseflags | ((uint32_t)LJ_ARENA_ROOT_LINKING <<
		     LJ_HUGEF_ROOT_SHIFT)) == -1);
  assert(lj_arena_hugetab_insert(&src, p, size, baseflags) == 1);
  assert(lj_arena_hugetab_root_state_acq(&src, p, &hi) ==
	 LJ_ARENA_ROOT_NONE);
  check_info(&hi, size, baseflags);

  assert(lj_arena_hugetab_root_state_cas(&src, p, LJ_ARENA_ROOT_NONE,
					  LJ_ARENA_ROOT_LINKING, &hi));
  check_info(&hi, size, baseflags |
	((uint32_t)LJ_ARENA_ROOT_LINKING << LJ_HUGEF_ROOT_SHIFT));
  assert(!lj_arena_hugetab_root_state_cas(&src, p, LJ_ARENA_ROOT_NONE,
					   LJ_ARENA_ROOT_MEMBER, NULL));
  assert(lj_arena_hugetab_root_state_cas(&src, p, LJ_ARENA_ROOT_LINKING,
					  LJ_ARENA_ROOT_MEMBER, &hi));
  assert(lj_arena_huge_root_state(hi.flags) == LJ_ARENA_ROOT_MEMBER);
  assert((hi.flags & ~LJ_HUGEF_ROOT_MASK) == baseflags);
  assert(lj_arena_hugetab_root_state_cas(&src, p, LJ_ARENA_ROOT_MEMBER,
					  LJ_ARENA_ROOT_UNLINKING, &hi));
  assert(lj_arena_hugetab_root_state_cas(&src, p,
					  LJ_ARENA_ROOT_UNLINKING,
					  LJ_ARENA_ROOT_MEMBER, &hi));
  assert(!lj_arena_hugetab_root_state_cas(&src, p,
					   LJ_ARENA_ROOT_MEMBER, 4u, NULL));

  /* Collector metadata transforms and every terminal shortcut preserve or
  ** veto the explicit membership bits. */
  lj_arena_hugetab_prepare_sweep(&src);
  assert(lj_arena_hugetab_root_state_acq(&src, p, &hi) ==
	 LJ_ARENA_ROOT_MEMBER);
  assert((hi.flags & LJ_HUGEF_SWEEP_OLD) != 0);
  lj_arena_hugetab_abort_sweep(&src);
  assert(lj_arena_hugetab_root_state_acq(&src, p, &hi) ==
	 LJ_ARENA_ROOT_MEMBER);
  assert((hi.flags & LJ_HUGEF_SWEEP_OLD) == 0);
  assert(!lj_arena_hugetab_delete(&src, p, NULL));
  assert(!lj_arena_hugetab_forget_terminal(&src, p, NULL));
  assert(lj_arena_hugetab_fini_all(&src) == 0u);
  assert(src.h != NULL && lj_arena_hugetab_lookup(&src, p, NULL));

  /* Dead-owner transfer copies the exact packed state and tombstones only the
  ** source identity. Ordinary fini must retain the destination locator. */
  assert(lj_arena_hugetab_transfer(&dst, &src, 0x7654u));
  assert(!lj_arena_hugetab_lookup(&src, p, NULL));
  assert(lj_arena_hugetab_root_state_acq(&dst, p, &hi) ==
	 LJ_ARENA_ROOT_MEMBER);
  assert((hi.flags & ~LJ_HUGEF_ROOT_MASK) == baseflags);
  assert(lj_arena_owner_acq(lj_arena_of(p)) == 0x7654u);
  lj_arena_hugetab_fini(&dst);
  assert(dst.h != NULL);

  assert(lj_arena_hugetab_root_complete(&dst, p, LJ_ARENA_ROOT_MEMBER,
	LJ_ARENA_ROOT_UNLINKING, 0, &hi) ==
	LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  assert(lj_arena_hugetab_root_complete(&dst, p, LJ_ARENA_ROOT_UNLINKING,
	LJ_ARENA_ROOT_NONE, 0, &hi) == LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  assert(lj_arena_huge_root_state(hi.flags) == LJ_ARENA_ROOT_NONE);
  delete_unmap(&dst, p);

  /* Constructor rollback has the same no-drop obligation as ordinary unlink:
  ** a free which observes LINKING is recorded and the rollback completion
  ** atomically converts it into sweep ownership. */
  p = lj_arena_huge_map(rs, size + 1u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&dst, p, size + 1u,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY|LJ_AF_ROOT_CONSTRUCT) == 1);
  assert(lj_arena_hugetab_lookup(&dst, p, &hi));
  assert((hi.flags & LJ_AF_ROOT_CONSTRUCT) == 0);
  assert(lj_arena_huge_root_state(hi.flags) == LJ_ARENA_ROOT_LINKING);
  assert(!lj_arena_hugetab_claim_external_free(&dst, p, &hi));
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0);
  assert(lj_arena_huge_root_state(hi.flags) == LJ_ARENA_ROOT_LINKING);
  assert(!lj_arena_hugetab_root_state_cas(&dst, p,
	LJ_ARENA_ROOT_LINKING, LJ_ARENA_ROOT_NONE, NULL));
	assert(lj_arena_hugetab_root_construct_abandon(&dst, p, 1234u, &hi) ==
	LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP);
  assert((hi.flags & (LJ_HUGEF_ROOT_MASK|LJ_HUGEF_DEFER_FREE|
		      LJ_HUGEF_BUSY)) == 0);
  assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
  assert(la_load64_acq(&lj_arena_of(p)->hdr.retire_epoch) == 1234u);
  delete_unmap(&dst, p);

  /* Commit never consumes a racing deferred free: LINKING->MEMBER remains a
  ** live mapping, and the eventual unlink is the unique sweep handoff. */
  p = lj_arena_huge_map(rs, size + 3u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&dst, p, size + 3u,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY|LJ_AF_ROOT_CONSTRUCT) == 1);
  assert(!lj_arena_hugetab_claim_external_free(&dst, p, &hi));
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0);
  assert(lj_arena_hugetab_root_construct_commit(&dst, p, &hi) ==
	 LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  assert(lj_arena_huge_root_state(hi.flags) == LJ_ARENA_ROOT_MEMBER);
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0);
  assert(lj_arena_hugetab_root_complete(&dst, p, LJ_ARENA_ROOT_MEMBER,
	LJ_ARENA_ROOT_UNLINKING, 0, &hi) ==
	LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  assert(lj_arena_hugetab_root_complete(&dst, p, LJ_ARENA_ROOT_UNLINKING,
	LJ_ARENA_ROOT_NONE, 1236u, &hi) ==
	LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP);
  assert((hi.flags & (LJ_HUGEF_ROOT_MASK|LJ_HUGEF_DEFER_FREE|
		      LJ_HUGEF_BUSY)) == 0);
  delete_unmap(&dst, p);

  /* If fallback recovery also owns the mapping, root completion relinquishes
  ** only its own bits. Recovery remains the unique consumer of DEFER_FREE. */
  p = lj_arena_huge_map(rs, size + 2u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&dst, p, size + 2u,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) == 1);
  assert(lj_arena_hugetab_root_state_cas(&dst, p, LJ_ARENA_ROOT_NONE,
	LJ_ARENA_ROOT_UNLINKING, NULL));
  assert(lj_arena_hugetab_recovery_state_cas(&dst, p,
	LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));
  assert(!lj_arena_hugetab_claim_external_free(&dst, p, &hi));
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0);
  assert(lj_arena_hugetab_root_complete(&dst, p,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_NONE, 1235u, &hi) ==
	LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
  assert(lj_arena_huge_root_state(hi.flags) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_huge_recovery_state(hi.flags) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0);
  assert(lj_arena_hugetab_recovery_state_cas(&dst, p,
	LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED, NULL));
  assert(lj_arena_hugetab_recovery_complete(&dst, p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP);
  assert((hi.flags & (LJ_HUGEF_ROOT_MASK|LJ_HUGEF_RECOVERY_MASK|
		      LJ_HUGEF_DEFER_FREE)) == 0);
  delete_unmap(&dst, p);
  lj_arena_hugetab_fini(&src);
  lj_arena_hugetab_fini(&dst);
}

typedef struct RootFreeRace {
  HugeTab *ht;
  void *p;
  uint64_t epoch;
  uint32_t go;
  int claim;
  int finish;
  int complete;
} RootFreeRace;

static void root_free_race_wait(RootFreeRace *race)
{
  while (!la_load32_acq(&race->go))
    la_cpu_pause();
}

static void *root_free_race_free(void *ud)
{
  RootFreeRace *race = (RootFreeRace *)ud;
  LJHugeInfo hi;
  root_free_race_wait(race);
  race->claim = lj_arena_hugetab_claim_external_free(race->ht, race->p,
						       &hi);
  if (race->claim)
    race->finish = lj_arena_hugetab_finish_external_free(race->ht,
							  race->p, &hi);
  return NULL;
}

static void *root_free_race_complete(void *ud)
{
  RootFreeRace *race = (RootFreeRace *)ud;
  root_free_race_wait(race);
  race->complete = lj_arena_hugetab_root_complete(race->ht, race->p,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_NONE, race->epoch, NULL);
  return NULL;
}

static void test_root_free_race(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 2081u;
  uint32_t i;
  for (i = 0; i < 32u; i++) {
    HugeTab ht = { NULL };
    RootFreeRace race;
    pthread_t free_thread, complete_thread;
    LJHugeInfo hi;

    assert(lj_arena_hugetab_init(&ht, 2));
    race.ht = &ht;
    race.p = lj_arena_huge_map(rs, size + i, LJ_AF_TRAVERSABLE);
    assert(race.p != NULL);
    race.epoch = 900u + i;
    race.go = 0;
    race.claim = 0;
    race.finish = LJ_ARENA_HUGE_FINISH_LOST;
    race.complete = LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
    assert(lj_arena_hugetab_insert(&ht, race.p, size + i,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
    assert(lj_arena_hugetab_root_state_cas(&ht, race.p,
	LJ_ARENA_ROOT_NONE, LJ_ARENA_ROOT_UNLINKING, NULL));
    lj_arena_hugetab_prepare_sweep(&ht);
    assert(pthread_create(&free_thread, NULL, root_free_race_free, &race) == 0);
    assert(pthread_create(&complete_thread, NULL, root_free_race_complete,
			  &race) == 0);
    la_store32_rel(&race.go, 1);
    assert(pthread_join(free_thread, NULL) == 0);
    assert(pthread_join(complete_thread, NULL) == 0);

    assert(race.complete == LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE ||
	   race.complete == LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP);
    if (race.claim)
      assert(race.finish == LJ_ARENA_HUGE_FINISH_DEFERRED);
    assert(lj_arena_hugetab_lookup(&ht, race.p, &hi));
    assert(lj_arena_huge_root_state(hi.flags) == LJ_ARENA_ROOT_NONE);
    assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	   (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
    assert((hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE)) == 0);
    assert(la_load64_acq(&lj_arena_of(race.p)->hdr.retire_epoch) != 0);
    delete_unmap(&ht, race.p);
    lj_arena_hugetab_fini(&ht);
  }
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

typedef struct HugeReallocRace {
  LJArenaAllocD *ad;
  void *p;
  size_t osize;
  size_t nsize;
  void *result;
} HugeReallocRace;

static void *huge_realloc_race_thread(void *ud)
{
  HugeReallocRace *race = (HugeReallocRace *)ud;
  race->result = lj_arena_allocf(race->ad, race->p,
				 race->osize, race->nsize);
  return NULL;
}

static void huge_realloc_race_wait_busy(void)
{
  while (!lj_arena_hugetab_test_realloc_paused())
    la_cpu_pause();
}

static void test_huge_realloc_busy_preemption(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 2601u;
  HugeTab ht = { NULL };
  TGAlloc primary_alloc, follower_alloc;
  LJArenaAllocD primary_ad, follower_ad;
  PRNGState follower_rs;
  HugeReallocRace race;
  LJHugeReader reader = { NULL, NULL, 0 };
  LJHugeInfo hi;
  pthread_t thread;
  void *p;

  lj_prng_seed_fixed(&follower_rs);
  assert(lj_arena_hugetab_init(&ht, 3));
  lj_arena_alloc_init(&primary_alloc);
  lj_arena_alloc_init(&follower_alloc);
  lj_arena_allocd_init(&primary_ad, &primary_alloc, rs, 0);
  lj_arena_allocd_init(&follower_ad, &follower_alloc, &follower_rs, 0);
  lj_arena_allocd_sethugetab(&primary_ad, &ht);
  lj_arena_allocd_sethugetab(&follower_ad, &ht);

  p = lj_arena_allocd_alloc(&primary_ad, size, 0);
  assert(p != NULL);
  memset(p, 0x5a, 256u);
  race.ad = &primary_ad;
  race.p = p;
  race.osize = size;
  race.nsize = SIZE_MAX;  /* Deterministic primary allocation failure. */
  race.result = (void *)(uintptr_t)1u;
  lj_arena_hugetab_test_realloc_pause(1);
  assert(pthread_create(&thread, NULL, huge_realloc_race_thread, &race) == 0);
  huge_realloc_race_wait_busy();

  /* Two exact-buffer resizes need an incarnation/forwarding descriptor to
  ** share success without address ABA or double accounting. The follower is
  ** a bounded rejection: it allocates nothing and changes no metadata. */
  assert(lj_arena_allocf(&follower_ad, p, size, size + 64u) == NULL);
  assert(lj_arena_hugetab_lookup(&ht, p, &hi));
  assert(hi.size == size && hi.readers == 0u &&
	 (hi.flags & LJ_HUGEF_BUSY) != 0 &&
	 (hi.flags & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_FREEING)) == 0);
  assert(memcmp(p, "\x5a\x5a\x5a\x5a", 4u) == 0);
  lj_arena_hugetab_test_realloc_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(race.result == NULL);
  assert(lj_arena_hugetab_lookup(&ht, p, &hi));
  assert(hi.size == size && hi.readers == 0u &&
	 (hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE|
		     LJ_HUGEF_FREEING)) == 0);
  assert(memcmp(p, "\x5a\x5a\x5a\x5a", 4u) == 0);
  assert(lj_arena_allocf(&primary_ad, p, size, 0) == NULL);
  assert(!lj_arena_hugetab_lookup(&ht, p, NULL));

  /* The opposite reader/owner completion order is equally nonblocking. BUSY
  ** keeps the source mapped after the last old reader leaves; the claim
  ** snapshot still forces a move and the owner then deletes it directly. */
  p = lj_arena_allocd_alloc(&primary_ad, size + 4u, 0);
  assert(p != NULL);
  memset(p, 0x3c, 256u);
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, NULL) ==
	 LJ_ARENA_HUGE_READER_ACQUIRED);
  race.p = p;
  race.osize = size + 4u;
  race.nsize = size + 64u;
  race.result = NULL;
  lj_arena_hugetab_test_realloc_pause(1);
  assert(pthread_create(&thread, NULL, huge_realloc_race_thread, &race) == 0);
  huge_realloc_race_wait_busy();
  assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  assert(hi.readers == 0u && (hi.flags & LJ_HUGEF_BUSY) != 0 &&
	 (hi.flags & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_FREEING)) == 0);
  lj_arena_hugetab_test_realloc_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(race.result != NULL && race.result != p);
  assert(memcmp(race.result, "\x3c\x3c\x3c\x3c", 4u) == 0);
  assert(!lj_arena_hugetab_lookup(&ht, p, NULL));
  assert(lj_arena_allocf(&primary_ad, race.result, size + 64u, 0) == NULL);

  /* An external free publishes the same irrevocable preemption while BUSY.
  ** The primary must not consume that intent or manufacture a replacement. */
  p = lj_arena_allocd_alloc(&primary_ad, size + 8u, 0);
  assert(p != NULL);
  race.p = p;
  race.osize = size + 8u;
  race.nsize = size + 32u;
  race.result = (void *)(uintptr_t)1u;
  lj_arena_hugetab_test_realloc_pause(1);
  assert(pthread_create(&thread, NULL, huge_realloc_race_thread, &race) == 0);
  huge_realloc_race_wait_busy();
  assert(lj_arena_allocf(&follower_ad, p, size + 8u, 0) == NULL);
  assert(lj_arena_hugetab_lookup(&ht, p, &hi));
  assert((hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE)) ==
	 (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE));
  lj_arena_hugetab_test_realloc_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(race.result == NULL);
  assert(lj_arena_hugetab_lookup(&ht, p, &hi));
  assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
  delete_unmap(&ht, p);

  lj_arena_alloc_fini(&primary_alloc);
  lj_arena_alloc_fini(&follower_alloc);
  lj_arena_hugetab_fini(&ht);
}

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

static void test_retire_busy_deferred_terminal(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 1877u;
  HugeTab raceht = { NULL };
  RetireRace race;
  pthread_t thread;
  LJHugeInfo hi;

  assert(lj_arena_hugetab_init(&raceht, 2));
  race.p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(race.p != NULL);
  race.obj = race.p;
  race.ht = &raceht;
  race.epoch = 104u;
  race.result = 0;
  assert(lj_arena_hugetab_insert(&raceht, race.p, size,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
  lj_arena_hugetab_prepare_sweep(&raceht);

  lj_arena_hugetab_test_retire_pause(1);
  assert(pthread_create(&thread, NULL, retire_race_thread, &race) == 0);
  retire_race_wait_busy();
  assert(!lj_arena_hugetab_claim_external_free(&raceht, race.p, &hi));
  assert((hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE)) ==
	 (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE));
  lj_arena_hugetab_test_retire_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(race.result == 1);
  assert(lj_arena_hugetab_lookup(&raceht, race.p, &hi));
  assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD|
		      LJ_HUGEF_TICKET)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TICKET));
  assert((hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE)) == 0);
  delete_unmap(&raceht, race.p);
  lj_arena_hugetab_fini(&raceht);
}
#endif

#if defined(LJ_ARENA_TEST_HELPERS)
typedef struct SmallLifetimeRace {
  TGAlloc *alloc;
  GCArena *a;
  void *p;
  size_t size;
} SmallLifetimeRace;

static void *small_lifetime_free_thread(void *ud)
{
  SmallLifetimeRace *race = (SmallLifetimeRace *)ud;
  lj_arena_free(race->alloc, race->p, race->size);
  return NULL;
}

static void *small_lifetime_sweep_thread(void *ud)
{
  SmallLifetimeRace *race = (SmallLifetimeRace *)ud;
  lj_arena_sweep_words(race->a, 0);
  return NULL;
}

static void small_lifetime_wait_pause(void)
{
  while (!lj_arena_test_lifetime_paused())
    la_cpu_pause();
}

static void test_small_lifetime_descriptor(PRNGState *rs)
{
  const size_t size = 64u;
  TGAlloc alloc;
  SmallLifetimeRace race;
  pthread_t thread;
  GCArena *a;
  void *p, *reuse;
  uint32_t cell, i;
  unsigned char before[64];

  lj_arena_alloc_init(&alloc);

  /* Root construction is a request-only allocation mode. It publishes both
  ** descriptors before block visibility, leaves every interior FREE and
  ** prevents a remote free from overwriting the pending header/body. */
  p = lj_arena_alloc(&alloc, rs, size,
	LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  assert((lj_arena_flags_acq(a) & LJ_AF_ROOT_CONSTRUCT) == 0);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_LINKING);
  for (i = 1; i < lj_arena_ncells(size); i++)
    assert(lj_arena_lifetime_state_acq(a, cell + i) ==
	   LJ_ARENA_LIFETIME_FREE);
  memset(p, 0xa7, size);
  memcpy(before, p, size);
  assert(lj_arena_remote_free_publish(&alloc, p, size));
  assert(memcmp(before, p, size) == 0);
  assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(lj_arena_root_construct_abandon(a, cell));
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_lifetime_clear_terminal(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);

  /* Recovery may own the constructor lane while list publication commits or
  ** abandons. The root helper finishes without waiting; recovery restores
  ** LIVE because LINKING no longer denotes an unfinished constructor. */
  p = lj_arena_alloc(&alloc, rs, size,
	LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_RECOVERY));
  assert(lj_arena_root_construct_commit(a, cell));
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RECOVERY);
  /* Model recovery's stale LINKING sample restoring CONSTRUCT after commit;
  ** an idempotent constructor retry repairs the crossover to LIVE. */
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_RECOVERY, LJ_ARENA_LIFETIME_CONSTRUCT));
  assert(lj_arena_root_construct_commit(a, cell));
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_MEMBER,
				 LJ_ARENA_ROOT_UNLINKING));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_UNLINKING,
				 LJ_ARENA_ROOT_NONE));
  assert(lj_arena_lifetime_clear_terminal(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);

  p = lj_arena_alloc(&alloc, rs, size,
	LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_RECOVERY));
  assert(lj_arena_root_construct_abandon(a, cell));
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RECOVERY);
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_RECOVERY, LJ_ARENA_LIFETIME_CONSTRUCT));
  assert(lj_arena_root_construct_abandon(a, cell));
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_lifetime_clear_terminal(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);

  /* Free wins the lifetime lane. A root linker cannot acquire LIVE while the
  ** free owner is paused, and no byte changes precede its admission proof. */
  p = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  memset(p, 0x5c, size);
  memcpy(before, p, size);
  race.alloc = &alloc;
  race.a = a;
  race.p = p;
  race.size = size;
  lj_arena_test_lifetime_pause(1);
  assert(pthread_create(&thread, NULL, small_lifetime_free_thread, &race) == 0);
  small_lifetime_wait_pause();
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_DESTRUCT);
  assert(!lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				      LJ_ARENA_LIFETIME_MUTATING));
  assert(memcmp(before, p, size) == 0);
  lj_arena_test_lifetime_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  assert(!lj_arena_bm_get(a->block, cell));
  /* The lifetime race proved p reusable; select its bin after the independent
  ** private bump is exhausted. */
  alloc.bump[LJ_ARENAK_TRAVERSABLE].cell =
    alloc.bump[LJ_ARENAK_TRAVERSABLE].end;
  reuse = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(reuse == p);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);

  /* Root wins the opposite ordering by holding MUTATING through LINKING. A
  ** concurrent free records a late intent but cannot touch its bytes. */
  memset(reuse, 0x39, size);
  memcpy(before, reuse, size);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_MUTATING));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_NONE,
				 LJ_ARENA_ROOT_LINKING));
  lj_arena_free(&alloc, reuse, size);
  assert(memcmp(before, reuse, size) == 0);
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_LIVE));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_LINKING,
				 LJ_ARENA_ROOT_MEMBER));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_MEMBER,
				 LJ_ARENA_ROOT_UNLINKING));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_UNLINKING,
				 LJ_ARENA_ROOT_NONE));
  assert(lj_arena_lifetime_clear_terminal(a, cell) ==
	 LJ_ARENA_LIFETIME_LIVE);

  /* Sweep's writer-side half of the no-both-miss handshake sees a reader
  ** admitted after DESTRUCT. It restores LIVE without clearing block/READY;
  ** a later reader-free sample performs LIVE->DESTRUCT->FREE exactly once. */
  p = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  race.a = a;
  race.p = p;
  lj_arena_test_lifetime_pause(1);
  assert(pthread_create(&thread, NULL, small_lifetime_sweep_thread, &race) == 0);
  small_lifetime_wait_pause();
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  la_fence_seq();
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_DESTRUCT);
  lj_arena_test_lifetime_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_bm_get(a->block, cell));
  lj_arena_rescue_leave(a);
  lj_arena_bm_clear(a->mark, cell);
  lj_arena_sweep_words(a, 0);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  assert(!lj_arena_bm_get(a->block, cell));

  /* A semantic publisher may cancel a tentative sweep in the exact lifetime
  ** lane. RESCUE is readable and pins the untouched body; the sweep's terminal
  ** DESTRUCT->FREE CAS loses without clearing block or overwriting bytes. */
  p = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  memset(p, 0xd3, size);
  memcpy(before, p, size);
  race.a = a;
  race.p = p;
  lj_arena_test_lifetime_pause(1);
  assert(pthread_create(&thread, NULL, small_lifetime_sweep_thread, &race) == 0);
  small_lifetime_wait_pause();
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_DESTRUCT);
  assert(!lj_arena_late_get(a, cell));
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_RESCUE));
  assert(memcmp(before, p, size) == 0);
  assert(lj_arena_bm_get(a->block, cell));
  lj_arena_test_lifetime_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RESCUE);
  assert(memcmp(before, p, size) == 0);
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_lifetime_state_cas(a, cell,
	LJ_ARENA_LIFETIME_RESCUE, LJ_ARENA_LIFETIME_LIVE));
  /* The losing sweep may have conservatively carried the rescued start's mark.
  ** A later unmarked pass owns and terminalizes it normally. */
  lj_arena_bm_clear(a->mark, cell);
  lj_arena_sweep_words(a, 0);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  assert(!lj_arena_bm_get(a->block, cell));

  /* Only the exact terminal pair is duplicate destructor ownership. A late
  ** logical-free intent leaves the body LIVE/readable and makes a tentative
  ** semantic destructor lose rather than masquerading as already completed. */
  p = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  memset(p, 0x6e, size);
  memcpy(before, p, size);
  assert(lj_arena_free_deferred(&alloc, p, size));
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_LOST);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(memcmp(before, p, size) == 0);

  p = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  memset(p, 0x84, size);
  memcpy(before, p, size);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_ACQUIRED);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_OWNED);
  assert(lj_arena_quarantine_owns_body(p, size));
  assert(!lj_arena_late_get(a, cell));
  assert(memcmp(before, p, size) == 0);
  lj_arena_free(&alloc, p, size);
  assert(lj_arena_bm_get(a->block, cell));
  assert(memcmp(before, p, size) == 0);

  lj_arena_alloc_fini(&alloc);
}
#endif

static void test_huge_reader_lifetime(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 2111u;
  HugeTab ht = { NULL }, dst = { NULL };
  LJHugeReader reader = { NULL, NULL, 0 }, rejected = { NULL, NULL, 0 };
  LJHugeInfo hi;
  void *p;

  assert(lj_arena_hugetab_init(&ht, 2));
  assert(lj_arena_hugetab_init(&dst, 2));
  p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, &hi) ==
	 LJ_ARENA_HUGE_READER_ACQUIRED);
  assert(reader.h == ht.h && reader.base == p && hi.readers == 1u);

  /* Every table/mapping terminal route observes the same slot-local count. */
  lj_arena_hugetab_fini(&ht);
  assert(ht.h != NULL);
  assert(lj_arena_hugetab_fini_all(&ht) == 0u && ht.h != NULL);
  assert(!lj_arena_hugetab_forget_terminal(&ht, p, NULL));
  assert(!lj_arena_hugetab_delete(&ht, p, NULL));
  assert(!lj_arena_hugetab_transfer(&dst, &ht, 0x9911u));

  /* External free is irrevocable but byte-free while a reader exists. New
  ** admissions reject DEFER_FREE, and the last release performs the handoff. */
  assert(!lj_arena_hugetab_claim_external_free(&ht, p, &hi));
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0 && hi.readers == 1u);
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &rejected, NULL) ==
	 LJ_ARENA_HUGE_READER_MISSING);
  assert(rejected.h == NULL && rejected.base == NULL);
  assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	 LJ_ARENA_HUGE_READER_HANDOFF);
  assert(reader.h == NULL && reader.base == NULL && hi.readers == 0u);
  assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) == 0);
  assert(lj_arena_hugetab_reader_release(&reader, NULL) ==
	 LJ_ARENA_HUGE_READER_RELEASE_LOST);
  delete_unmap(&ht, p);

  /* Reanchor BUSY is body-stable. A reader admitted after that claim must not
  ** strand its non-destructive completion; finish preserves the count and the
  ** last reader consumes the racing external-free intent. */
  p = lj_arena_huge_map(rs, size + 3u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size + 3u,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY|LJ_HUGEF_SWEEP_OLD|
	LJ_HUGEF_TICKET|LJ_HUGEF_MARK));
  assert(lj_arena_hugetab_claim_live_ticket(&ht, p, &hi));
  assert(lj_arena_hugetab_mark_reader_acquire(&ht, p, &reader, &hi) == 0);
  assert(hi.readers == 1u && (hi.flags & LJ_HUGEF_BUSY));
  assert(!lj_arena_hugetab_claim_external_free(&ht, p, &hi));
  assert((hi.flags & LJ_HUGEF_DEFER_FREE) != 0 && hi.readers == 1u);
  assert(lj_arena_hugetab_finish_live_ticket(&ht, p, &hi));
  assert(hi.readers == 1u &&
	 (hi.flags & (LJ_HUGEF_TICKET|LJ_HUGEF_BUSY)) == 0 &&
	 (hi.flags & LJ_HUGEF_DEFER_FREE));
  assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	 LJ_ARENA_HUGE_READER_HANDOFF);
  delete_unmap(&ht, p);

  /* A sweep-terminal claimant and a dead-owner transfer are both mechanical
  ** count checks, not caller-discipline assumptions. */
  p = lj_arena_huge_map(rs, size + 1u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size + 1u,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY|LJ_HUGEF_SWEEP_OLD|
	LJ_HUGEF_RETIRED|LJ_HUGEF_TICKET));
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, &hi) == 1);
  assert(!lj_arena_hugetab_claim_freeing(&ht, p, NULL));
  assert(!lj_arena_hugetab_transfer(&dst, &ht, 0x9912u));
  assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  assert(lj_arena_hugetab_claim_freeing(&ht, p, &hi));
  delete_unmap(&ht, p);

  p = lj_arena_huge_map(rs, size + 2u, 0);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size + 2u, 0));
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, NULL) == 1);
  assert(!lj_arena_hugetab_transfer(&dst, &ht, 0x9913u));
  assert(lj_arena_hugetab_reader_release(&reader, NULL) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  assert(lj_arena_hugetab_transfer(&dst, &ht, 0x9913u));
  assert(!lj_arena_hugetab_lookup(&ht, p, NULL));
  assert(lj_arena_hugetab_lookup(&dst, p, &hi) && hi.readers == 0u);
  delete_unmap(&dst, p);
  lj_arena_hugetab_fini(&ht);
  lj_arena_hugetab_fini(&dst);
}

static void test_huge_reader_root_recovery_orders(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 2237u;
  HugeTab ht = { NULL };
  LJHugeReader reader = { NULL, NULL, 0 };
  LJHugeInfo hi;
  void *p;
  uint32_t order;

  assert(lj_arena_hugetab_init(&ht, 3));
  for (order = 0; order < 2u; order++) {
    p = lj_arena_huge_map(rs, size + order, LJ_AF_TRAVERSABLE);
    assert(p != NULL);
    assert(lj_arena_hugetab_insert(&ht, p, size + order,
	  LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
    assert(lj_arena_hugetab_root_state_cas(&ht, p, LJ_ARENA_ROOT_NONE,
	  LJ_ARENA_ROOT_UNLINKING, NULL));
    assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, NULL) == 1);
    assert(!lj_arena_hugetab_claim_external_free(&ht, p, &hi));
    if (order == 0) {
      assert(lj_arena_hugetab_root_complete(&ht, p,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_NONE, 3001u, &hi) ==
	LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE);
      assert(hi.readers == 1u && (hi.flags & LJ_HUGEF_DEFER_FREE));
      assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	LJ_ARENA_HUGE_READER_HANDOFF);
    } else {
      assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	LJ_ARENA_HUGE_READER_RELEASED);
      assert(hi.readers == 0u && (hi.flags & LJ_HUGEF_DEFER_FREE));
      assert(lj_arena_hugetab_root_complete(&ht, p,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_NONE, 3002u, &hi) ==
	LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP);
    }
    assert((hi.flags & (LJ_HUGEF_ROOT_MASK|LJ_HUGEF_DEFER_FREE)) == 0);
    assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	   (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
    delete_unmap(&ht, p);
  }

  for (order = 0; order < 2u; order++) {
    p = lj_arena_huge_map(rs, size + 8u + order, LJ_AF_TRAVERSABLE);
    assert(p != NULL);
    assert(lj_arena_hugetab_insert(&ht, p, size + 8u + order,
	  LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
    assert(lj_arena_hugetab_recovery_state_cas(&ht, p,
	  LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));
    assert(lj_arena_hugetab_recovery_state_cas(&ht, p,
	  LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED, NULL));
    assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, NULL) == 1);
    assert(!lj_arena_hugetab_claim_external_free(&ht, p, &hi));
    if (order == 0) {
      assert(lj_arena_hugetab_recovery_complete(&ht, p, &hi) ==
	LJ_ARENA_HUGE_RECOVERY_COMPLETE_LIVE);
      assert(hi.readers == 1u &&
	     lj_arena_huge_recovery_state(hi.flags) == LJ_ARENA_RECOVERY_IDLE);
      assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	LJ_ARENA_HUGE_READER_HANDOFF);
    } else {
      assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	LJ_ARENA_HUGE_READER_RELEASED);
      assert(lj_arena_hugetab_recovery_complete(&ht, p, &hi) ==
	LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP);
    }
    assert((hi.flags & (LJ_HUGEF_RECOVERY_MASK|LJ_HUGEF_DEFER_FREE)) == 0);
    assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	   (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
    delete_unmap(&ht, p);
  }

  p = lj_arena_huge_map(rs, size + 16u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size + 16u,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
  assert(lj_arena_hugetab_recovery_state_cas(&ht, p,
	LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING, NULL));
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, NULL) == 1);
  assert(lj_arena_hugetab_recovery_discard_terminal(&ht, p, NULL) ==
	 LJ_ARENA_HUGE_RECOVERY_TERMINAL_LOST);
  assert(lj_arena_hugetab_reader_release(&reader, NULL) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  assert(lj_arena_hugetab_recovery_discard_terminal(&ht, p, &hi) ==
	 LJ_ARENA_HUGE_RECOVERY_TERMINAL_CLEARED);
  delete_unmap(&ht, p);
  lj_arena_hugetab_fini(&ht);
}

static void test_huge_reader_shapes_and_realloc(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 2309u;
  HugeTab ht = { NULL };
  LJHugeReader reader = { NULL, NULL, 0 };
  LJHugeInfo hi;
  TGAlloc alloc;
  LJArenaAllocD ad;
  TGState *owner;
  global_State *progress = (global_State *)(uintptr_t)0x12340000u;
  void *p, *q, *sweepp, *base = NULL;
  uint32_t cursor;

  assert(lj_arena_hugetab_init(&ht, 2));
  p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size, LJ_HUGEF_TRAVERSABLE));
  assert(lj_arena_hugetab_publish_interior_cdata(&ht, p));
  assert(lj_arena_hugetab_mark_cdata_range_reader_acquire(
	&ht, (char *)p + 1u, &base, &reader, &hi) == 1);
  assert(base == p && reader.base == p && hi.readers == 1u);
  assert(lj_arena_hugetab_reader_covers(&reader, p));
  assert(lj_arena_hugetab_reader_covers(&reader, (char *)p + size - 1u));
  assert(!lj_arena_hugetab_reader_covers(&reader, (char *)p + size));
  assert(lj_arena_hugetab_reader_covers_range(&reader,
	(char *)p + size, 0));
  assert(!lj_arena_hugetab_reader_covers_range(&reader,
	(char *)p + size - 1u, 2u));
  assert((hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_CDATA|
		      LJ_HUGEF_INTERIOR_CDATA|LJ_HUGEF_READY)) ==
	 (LJ_HUGEF_MARK|LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA|
	  LJ_HUGEF_READY));
  assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  assert(lj_arena_hugetab_reader_release(&reader, NULL) ==
	 LJ_ARENA_HUGE_READER_RELEASE_LOST);
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, NULL) == 1);
  {
    LJHugeTabHdr *stable = reader.h;
    ht.h = NULL;  /* Model retirement/overwrite of the TG-embedded wrapper. */
    assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	   LJ_ARENA_HUGE_READER_RELEASED);
    ht.h = stable;
  }
  delete_unmap(&ht, p);

  /* Generic semantic range admission covers non-cdata representation bytes
  ** such as a BCIns inside a huge GCproto allocation. */
  p = lj_arena_huge_map(rs, size + 8u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size + 8u,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
  base = NULL;
  assert(lj_arena_hugetab_mark_range_reader_acquire(
	&ht, (char *)p + 8u, &base, &reader, &hi) == 1);
  assert(base == p && lj_arena_hugetab_reader_covers_range(
	&reader, (char *)p + 8u, sizeof(uint32_t)));
  assert(lj_arena_hugetab_reader_release(&reader, NULL) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  delete_unmap(&ht, p);

  /* A raw mark/reader is transient GC observation, not allocation failure.
  ** Preserve its old geometry, move even within one mapping-size class, and
  ** let the last reader expose the old raw mapping to fresh-grace sweep. */
  lj_arena_alloc_init(&alloc);
  owner = (TGState *)calloc(1, sizeof(*owner));
  assert(owner != NULL);
  owner->gl = progress;
  lj_arena_alloc_owner_tg_rel(&alloc, owner);
  lj_arena_allocd_init(&ad, &alloc, rs, 0);
  lj_arena_allocd_sethugetab(&ad, &ht);
  q = lj_arena_allocd_alloc(&ad, size + 12u, LJ_AF_TRAVERSABLE);
  assert(q != NULL && lj_arena_allocd_publish_gco(&ad, q));
  assert(lj_arena_allocf(&ad, q, size + 12u, size + 24u) == NULL);
  assert(lj_arena_hugetab_lookup(&ht, q, &hi));
  assert(hi.size == size + 12u &&
	 (hi.flags & LJ_HUGEF_TRAVERSABLE) != 0 &&
	 (hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE|
		     LJ_HUGEF_FREEING)) == 0);
  assert(lj_arena_allocf(&ad, q, size + 12u, 0) == NULL);
  p = lj_arena_allocd_alloc(&ad, size + 16u, 0);
  assert(p != NULL);
  assert(lj_arena_progress_g_acq(lj_arena_of(p)) == progress);
  memset(p, 0xa5, 256u);
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, &hi) == 1);
  assert(lj_arena_allocf(&ad, p, size + 16u, SIZE_MAX) == NULL);
  assert(lj_arena_hugetab_lookup(&ht, p, &hi));
  assert(hi.size == size + 16u && hi.readers == 1u &&
	 (hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_DEFER_FREE|
		     LJ_HUGEF_FREEING)) == 0);
  assert(memcmp(p, "\xa5\xa5\xa5\xa5", 4u) == 0);
  q = lj_arena_allocf(&ad, p, size + 16u, size + 32u);
  assert(q != NULL && q != p);
  assert(memcmp(q, p, 256u) == 0);
  assert(lj_arena_hugetab_lookup(&ht, p, &hi));
  assert(hi.size == size + 16u && hi.readers == 1u &&
	 (hi.flags & LJ_HUGEF_DEFER_FREE) != 0 &&
	 (hi.flags & (LJ_HUGEF_BUSY|LJ_HUGEF_FREEING)) == 0);
  assert(lj_arena_hugetab_lookup(&ht, q, &hi));
  assert(hi.size == size + 32u && hi.readers == 0u);
  assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	 LJ_ARENA_HUGE_READER_HANDOFF);
  assert((hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
  assert((hi.flags & LJ_HUGEF_TRAVERSABLE) == 0);
  cursor = 0;
  sweepp = NULL;
  assert(lj_arena_hugetab_sweep_next(&ht, &cursor, &sweepp, &hi));
  assert(sweepp == p && lj_arena_hugetab_has_sweep_old(&ht));
  delete_unmap(&ht, p);
  assert(lj_arena_allocf(&ad, q, size + 32u, 0) == NULL);
  assert(!lj_arena_hugetab_lookup(&ht, q, NULL));
  lj_arena_alloc_owner_tg_rel(&alloc, NULL);
  free(owner);
  lj_arena_alloc_fini(&alloc);
  lj_arena_hugetab_fini(&ht);
}

static void test_huge_reader_overflow_and_size(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 2417u;
  HugeTab ht = { NULL };
  LJHugeReader *readers, extra = { NULL, NULL, 0 };
  LJHugeInfo hi;
  void *p;
  uint32_t i;

  assert(lj_arena_hugetab_init(&ht, 1));
  /* Size is exactly 32-bit authoritative even though the reader counter uses
  ** the remaining high metadata bits. No physical 4 GiB mapping is needed. */
  p = (void *)(uintptr_t)UINT64_C(0x100000040);
  assert(lj_arena_hugetab_insert(&ht, p, (size_t)UINT32_MAX,
	LJ_HUGEF_FINALIZER) == 1);
  assert(lj_arena_hugetab_lookup(&ht, p, &hi));
  check_info(&hi, (size_t)UINT32_MAX, LJ_HUGEF_FINALIZER);
  assert(lj_arena_hugetab_delete(&ht, p, &hi));
#if SIZE_MAX > UINT32_MAX
  assert(lj_arena_hugetab_insert(&ht, p, (size_t)UINT32_MAX + 1u, 0) == -1);
#endif

  p = lj_arena_huge_map(rs, size, 0);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size, 0));
  readers = (LJHugeReader *)calloc(0xffffu, sizeof(*readers));
  assert(readers != NULL);
  for (i = 0; i < 0xffffu; i++) {
    assert(lj_arena_hugetab_reader_acquire(&ht, p, &readers[i], &hi) == 1);
    assert(hi.readers == i + 1u);
  }
  assert(lj_arena_hugetab_mark_reader_acquire(&ht, p, &extra, &hi) ==
	 LJ_ARENA_HUGE_MARK_SATURATED);
  assert(extra.h == NULL && extra.base == NULL && hi.readers == 0xffffu);
  assert((hi.flags & LJ_HUGEF_MARK) != 0);  /* Saturation never drops liveness. */
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &extra, &hi) ==
	 LJ_ARENA_HUGE_READER_OVERFLOW);
  assert(extra.h == NULL && extra.base == NULL);
  assert(!lj_arena_hugetab_claim_external_free(&ht, p, &hi));
  assert(hi.readers == 0xffffu && (hi.flags & LJ_HUGEF_DEFER_FREE));
  for (i = 0; i < 0xfffeu; i++)
    assert(lj_arena_hugetab_reader_release(&readers[i], NULL) ==
	   LJ_ARENA_HUGE_READER_RELEASED);
  assert(lj_arena_hugetab_reader_release(&readers[0xfffeu], &hi) ==
	 LJ_ARENA_HUGE_READER_HANDOFF);
  assert(hi.readers == 0u &&
	 (hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD)) ==
	 (LJ_HUGEF_FREEING|LJ_HUGEF_SWEEP_OLD));
  free(readers);
  delete_unmap(&ht, p);
  lj_arena_hugetab_fini(&ht);
}

#if defined(LJ_ARENA_TEST_HELPERS)
typedef struct PlainLateRace {
  TGAlloc *alloc;
  void *p;
  size_t size;
  int result;
} PlainLateRace;

typedef struct RegistryRescueRace {
  HugeTab *registry;
  GCArena *a;
  const unsigned char *p;
  uint32_t entered;
  uint32_t leave;
  int admission;
  unsigned char observed;
} RegistryRescueRace;

static void *plain_late_race_thread(void *ud)
{
  PlainLateRace *race = (PlainLateRace *)ud;
  race->result = lj_arena_free_deferred(race->alloc, race->p, race->size);
  return NULL;
}

static void plain_late_wait_paused(void)
{
  while (!lj_arena_test_plain_late_paused())
    la_cpu_pause();
}

static void *registry_rescue_race_thread(void *ud)
{
  RegistryRescueRace *race = (RegistryRescueRace *)ud;
  race->admission = lj_arena_hugetab_rescue_enter(
    race->registry, race->a, NULL);
  if (race->admission != LJ_ARENA_RESCUE_RETRY) {
    race->observed = *race->p;
    la_store32_rel(&race->entered, 1);
    while (!la_load32_acq(&race->leave))
      la_cpu_pause();
    lj_arena_rescue_leave(race->a);
  }
  return NULL;
}
#endif

static void test_registry_rescue_unmap_handoff(PRNGState *rs)
{
#if defined(LJ_ARENA_TEST_HELPERS)
  HugeTab registry = { NULL };
  TGAlloc alloc;
  RegistryRescueRace race;
  pthread_t thread;
  void *p;

  assert(lj_arena_hugetab_init(&registry, 2));
  lj_arena_alloc_init(&alloc);
  lj_arena_alloc_set_registry(&alloc, &registry);
  p = lj_arena_alloc(&alloc, rs, 64u, 0);
  assert(p != NULL);
  memset(p, 0xa6, 64u);
  assert(lj_arena_alloc_registry_lookup(&alloc, lj_arena_of(p), NULL));

  race.registry = &registry;
  race.a = lj_arena_of(p);
  race.p = (const unsigned char *)p;
  race.entered = 0;
  race.leave = 0;
  race.admission = LJ_ARENA_RESCUE_RETRY;
  race.observed = 0;
  lj_arena_test_registry_pause(1);
  assert(pthread_create(&thread, NULL, registry_rescue_race_thread, &race) ==
	 0);
  while (!lj_arena_test_registry_paused())
    la_cpu_pause();

  /* The registry reader defeats its exact delete. REGISTERED and the allocator
  ** list remain intact, so terminal fini cannot proceed to unmap. */
  lj_arena_alloc_fini(&alloc);
  assert(lj_arena_alloc_registry_lookup(&alloc, race.a, NULL));
  assert((lj_arena_flags_acq(race.a) & LJ_AF_REGISTERED) != 0);

  lj_arena_test_registry_pause(0);
  while (!la_load32_acq(&race.entered))
    la_cpu_pause();
  assert(race.admission == LJ_ARENA_RESCUE_FULL && race.observed == 0xa6);
  /* After the bridge drops its registry count, the arena admission itself
  ** defeats terminal close. A second fini still retains the locator. */
  lj_arena_alloc_fini(&alloc);
  assert(lj_arena_alloc_registry_lookup(&alloc, race.a, NULL));
  la_store32_rel(&race.leave, 1);
  assert(pthread_join(thread, NULL) == 0);

  /* With both admissions gone, close -> registry delete -> unmap succeeds in
  ** that order. The stale address is used only as a HugeTab key afterward. */
  lj_arena_alloc_fini(&alloc);
  assert(!lj_arena_hugetab_lookup(&registry, race.a, NULL));
  lj_arena_hugetab_fini(&registry);
#else
  UNUSED(rs);
#endif
}

static void test_alloc_committed_prefix_tail_veto(PRNGState *rs)
{
  const size_t span_size = 128u, take_size = 32u, prefix_size = 16u;
  TGAlloc alloc;
  GCArena *a;
  void *span, *guard, *p, *prefix, *tail;
  uint32_t start, conflict;

  lj_arena_alloc_init(&alloc);
  span = lj_arena_alloc(&alloc, rs, span_size, LJ_AF_TRAVERSABLE);
  guard = lj_arena_alloc(&alloc, rs, 16u, LJ_AF_TRAVERSABLE);
  assert(span != NULL && guard != NULL);
  a = lj_arena_of(span);
  start = lj_arena_cellof(span);
  lj_arena_free(&alloc, span, span_size);

  /* The selected run remains valid for the requested prefix, but a root
  ** claim appearing in its unused suffix vetoes private-tail preparation.
  ** Allocation must return the already committed prefix, not false OOM. */
  conflict = start + lj_arena_ncells(take_size) + 1u;
  assert(lj_arena_root_state_cas(a, conflict, LJ_ARENA_ROOT_NONE,
				 LJ_ARENA_ROOT_LINKING));
  alloc.bump[LJ_ARENAK_TRAVERSABLE].cell =
    alloc.bump[LJ_ARENAK_TRAVERSABLE].end;
  p = lj_arena_alloc(&alloc, rs, take_size, LJ_AF_TRAVERSABLE);
  assert(p == span);
  assert(lj_arena_bm_get(a->block, start));
  assert(lj_arena_lifetime_state_acq(a, start) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(a, conflict) == LJ_ARENA_ROOT_LINKING);

  /* Rebuild and consume the free prefix while the interior descriptor still
  ** splits the suffix. Its own retained mark must survive as the next
  ** discovery point; relying only on the suffix-start sentinel would strand
  ** everything after conflict once this prefix allocation consumes it. */
  lj_arena_alloc_rebuild_free_kind(&alloc, LJ_ARENAK_TRAVERSABLE);
  prefix = lj_arena_alloc(&alloc, rs, prefix_size, LJ_AF_TRAVERSABLE);
  assert(prefix == (unsigned char *)span + take_size);
  assert(lj_arena_root_state_acq(a, conflict) == LJ_ARENA_ROOT_LINKING);

  assert(lj_arena_root_state_cas(a, conflict, LJ_ARENA_ROOT_LINKING,
				 LJ_ARENA_ROOT_NONE));
  /* Once the descriptor clears, its bitmap-only boundary recovers the complete
  ** remaining tail even though the earlier prefix is still allocated. */
  lj_arena_alloc_rebuild_free_kind(&alloc, LJ_ARENAK_TRAVERSABLE);
  tail = lj_arena_alloc(&alloc, rs,
			span_size - take_size - prefix_size,
			LJ_AF_TRAVERSABLE);
  assert(tail == (unsigned char *)span + take_size + prefix_size);
  lj_arena_free(&alloc, tail, span_size - take_size - prefix_size);
  lj_arena_free(&alloc, prefix, prefix_size);
  lj_arena_free(&alloc, p, take_size);
  lj_arena_free(&alloc, guard, 16u);
  lj_arena_alloc_fini(&alloc);
}

static void test_managed_shrink_suffix_veto(PRNGState *rs)
{
  const size_t osize = 64u, nsize = 32u;
  TGAlloc alloc;
  GCArena *a;
  void *p;
  uint32_t start, conflict;
  unsigned char before[64];

  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, osize, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  start = lj_arena_cellof(p);
  memset(p, 0x6d, osize);
  memcpy(before, p, osize);
  conflict = start + lj_arena_ncells(nsize);
  assert(lj_arena_root_state_cas(a, conflict, LJ_ARENA_ROOT_NONE,
				 LJ_ARENA_ROOT_LINKING));

  /* The direct arena API permits a retryable managed shrink attempt even
  ** though Lua's GC wrapper rejects resizing traversable object bodies. An
  ** interior owner must preserve the complete old extent; moving and freeing
  ** it would encounter the same durable veto after terminal ownership. */
  assert(lj_arena_realloc(&alloc, rs, p, osize, nsize,
			  LJ_AF_TRAVERSABLE) == NULL);
  assert(lj_arena_bm_get(a->block, start));
  assert(lj_arena_lifetime_state_acq(a, start) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(a, conflict) == LJ_ARENA_ROOT_LINKING);
  assert(memcmp(before, p, osize) == 0);

  assert(lj_arena_root_state_cas(a, conflict, LJ_ARENA_ROOT_LINKING,
				 LJ_ARENA_ROOT_NONE));
  assert(lj_arena_realloc(&alloc, rs, p, osize, nsize,
			  LJ_AF_TRAVERSABLE) == p);
  assert(memcmp(before, p, nsize) == 0);
  lj_arena_free(&alloc, p, nsize);
  lj_arena_alloc_fini(&alloc);
}

static void test_plain_reader_mutation_gate(PRNGState *rs)
{
  const size_t size = 64u;
  TGAlloc alloc;
  GCArena *a;
  void *p;
  uint32_t cell;
  unsigned char before[64];
  int admission;

  lj_arena_alloc_init(&alloc);

  p = lj_arena_alloc(&alloc, rs, size, 0);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  memset(p, 0x51, size);
  memcpy(before, p, size);
  admission = lj_arena_rescue_enter(a);
  assert(admission == LJ_ARENA_RESCUE_FULL);
  lj_arena_free(&alloc, p, size);
  assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
  assert(memcmp(before, p, size) == 0);
  lj_arena_rescue_leave(a);
  lj_arena_free(&alloc, p, size);
  assert(!lj_arena_bm_get(a->block, cell));

  p = lj_arena_alloc(&alloc, rs, size, 0);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  memset(p, 0x62, size);
  memcpy(before, p, size);
  admission = lj_arena_rescue_enter(a);
  assert(admission == LJ_ARENA_RESCUE_FULL);
  {
    void *moved = lj_arena_realloc(&alloc, rs, p, size, size / 2u, 0);
    assert(moved != NULL && moved != p);
    assert(memcmp(before, moved, size / 2u) == 0);
    assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
    /* The admitted reader still owns the old bytes; realloc has transferred
    ** the caller to an independent allocation without waiting for it. */
    assert(memcmp(before, p, size) == 0);
    lj_arena_rescue_leave(a);
    p = moved;
  }
  assert(lj_arena_realloc(&alloc, rs, p, size / 2u, size / 4u, 0) == p);
  assert(memcmp(before, p, size / 4u) == 0);
  lj_arena_free(&alloc, p, size / 4u);

  p = lj_arena_alloc(&alloc, rs, size, 0);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  memset(p, 0x73, size);
  memcpy(before, p, size);
  admission = lj_arena_rescue_enter(a);
  assert(admission == LJ_ARENA_RESCUE_FULL);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_LOST);
  assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
  assert(memcmp(before, p, size) == 0);
  lj_arena_rescue_leave(a);
  lj_arena_free(&alloc, p, size);

  p = lj_arena_alloc(&alloc, rs, size, 0);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_ACQUIRED);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_RETRY);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_OWNED);
  memset(p, 0x84, size);  /* The unique semantic destructor owns these bytes. */
  lj_arena_free(&alloc, p, size);
  assert(!lj_arena_bm_get(a->block, cell));
  assert(lj_arena_remote_active_acq(a) == 0);

  {
    TGAlloc move_alloc;
    void *gatep, *oldp, *np;
    size_t nsize = size / 2u;
    lj_arena_alloc_init(&move_alloc);
    oldp = lj_arena_alloc(&move_alloc, rs, size, 0);
    gatep = lj_arena_alloc(&move_alloc, rs, size, 0);
    assert(oldp != NULL && gatep != NULL);
    a = lj_arena_of(oldp);
    assert(lj_arena_of(gatep) == a);
    memset(oldp, 0xa5, size);
    assert(lj_arena_destruct_acquire(gatep, size) ==
	   LJ_ARENA_DESTRUCT_ACQUIRED);

    /* Losing the unrelated arena-wide plain writer gate is not an OOM. A
    ** shrink which cannot take its in-place token moves immediately, keeps
    ** the requested prefix and leaves the old body durably late-freed. */
    np = lj_arena_realloc(&move_alloc, rs, oldp, size, nsize, 0);
    assert(np != NULL && np != oldp);
    memset(before, 0xa5, nsize);
    assert(memcmp(np, before, nsize) == 0);
    assert(lj_arena_late_get(a, lj_arena_cellof(oldp)));
    lj_arena_free(&move_alloc, gatep, size);
    lj_arena_free(&move_alloc, np, nsize);
    lj_arena_alloc_fini(&move_alloc);
  }

#if defined(LJ_ARENA_TEST_HELPERS)
  {
    SmallLifetimeRace free_race;
    pthread_t thread;
    p = lj_arena_alloc(&alloc, rs, size, 0);
    assert(p != NULL);
    a = lj_arena_of(p);
    cell = lj_arena_cellof(p);
    memset(p, 0x91, size);
    memcpy(before, p, size);
    free_race.alloc = &alloc;
    free_race.a = a;
    free_race.p = p;
    free_race.size = size;

    /* Initial writer-CAS loss acquires its own fallback admission before the
    ** original reader may be the last leave/open. */
    admission = lj_arena_rescue_enter(a);
    assert(admission == LJ_ARENA_RESCUE_FULL);
    lj_arena_test_plain_admit_pause(1);
    assert(pthread_create(&thread, NULL, small_lifetime_free_thread,
			  &free_race) == 0);
    while (!lj_arena_test_plain_admit_paused())
      la_cpu_pause();
    lj_arena_rescue_leave(a);
    assert(lj_arena_remote_active_acq(a) != 0);
    assert(lj_arena_bm_get(a->block, cell) && memcmp(before, p, size) == 0);
    lj_arena_test_plain_admit_pause(0);
    assert(pthread_join(thread, NULL) == 0);
    assert(lj_arena_late_get(a, cell) && lj_arena_bm_get(a->block, cell));
    lj_arena_free(&alloc, p, size);

    p = lj_arena_alloc(&alloc, rs, size, 0);
    assert(p != NULL);
    a = lj_arena_of(p);
    cell = lj_arena_cellof(p);
    memset(p, 0x92, size);
    memcpy(before, p, size);
    free_race.a = a;
    free_race.p = p;
    /* A late publisher wins between C|S and S|P. The losing writer replaces
    ** its consumed count, publishes its own intent, and only then leaves. */
    lj_arena_test_plain_claim_pause(1);
    assert(pthread_create(&thread, NULL, small_lifetime_free_thread,
			  &free_race) == 0);
    while (!lj_arena_test_plain_claim_paused())
      la_cpu_pause();
    assert(lj_arena_free_deferred(&alloc, p, size));
    lj_arena_test_plain_claim_pause(0);
    assert(pthread_join(thread, NULL) == 0);
    assert(lj_arena_remote_active_acq(a) == 0);
    assert(lj_arena_late_get(a, cell) && lj_arena_bm_get(a->block, cell));
    assert(memcmp(before, p, size) == 0);
    lj_arena_free(&alloc, p, size);
  }

  {
    PlainLateRace race;
    pthread_t thread;
    void *other, *reuse;
    uint32_t tail_cell;
    FreeRunProbe probe;
    p = lj_arena_alloc(&alloc, rs, size, 0);
    assert(p != NULL);
    a = lj_arena_of(p);
    cell = lj_arena_cellof(p);
    race.alloc = &alloc;
    race.p = p;
    race.size = size;
    race.result = 0;
    assert(lj_arena_destruct_acquire(p, size) ==
	   LJ_ARENA_DESTRUCT_ACQUIRED);
    lj_arena_test_plain_late_pause(1);
    assert(pthread_create(&thread, NULL, plain_late_race_thread, &race) == 0);
    plain_late_wait_paused();

    /* The producer has incremented the writer generation but has not sampled
    ** block[] or published late[]. Free may finish without waiting, while the
    ** CLOSED generation makes its bin node temporarily non-reusable. */
    lj_arena_free(&alloc, p, size);
    assert(!lj_arena_bm_get(a->block, cell));
    assert(lj_arena_remote_active_acq(a) != 0);
    assert(alloc.bump[LJ_ARENAK_PLAIN].a == a);
    tail_cell = alloc.bump[LJ_ARENAK_PLAIN].cell;
    other = lj_arena_alloc(&alloc, rs, size, 0);
    assert(other != NULL && other != p);
    assert(lj_arena_of(other) != a);  /* Fresh fallback while a is vetoed. */
    assert(!lj_arena_bm_get(a->block, tail_cell));
    assert(lj_arena_bm_get(a->mark, tail_cell));

    lj_arena_test_plain_late_pause(0);
    assert(pthread_join(thread, NULL) == 0);
    assert(race.result == 1);
    assert(lj_arena_remote_active_acq(a) == 0);
    memset(&probe, 0, sizeof(probe));
    probe.cell = tail_cell;
    lj_arena_scan_free_runs(a, find_free_run_covering, &probe);
    assert(probe.found && probe.start <= tail_cell &&
	   probe.start + probe.len > tail_cell);
    /* The race proves p is reusable; explicitly exhaust this fixture's
    ** independent bump window before asserting exact bin-address reuse. */
    alloc.bump[LJ_ARENAK_PLAIN].cell = alloc.bump[LJ_ARENAK_PLAIN].end;
    reuse = lj_arena_alloc(&alloc, rs, size, 0);
    assert(reuse == p);
    assert(!lj_arena_late_get(a, cell));
    if (other)
      lj_arena_free(&alloc, other, size);
    lj_arena_free(&alloc, reuse, size);
  }
#endif

  lj_arena_alloc_fini(&alloc);
}

static void terminal_make_closed_pending(GCArena *a)
{
  assert(lj_arena_remote_active_acq(a) == 0 ||
	 lj_arena_remote_active_acq(a) == LJ_ARENA_REMOTE_CLOSED);
  assert(lj_arena_reclaim_seal(a));
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_BIT_ONLY);
  lj_arena_rescue_leave(a);
  assert(lj_arena_remote_active_acq(a) ==
	 (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED|
	  LJ_ARENA_REMOTE_PENDING));
  lj_arena_reclaim_unseal(a, 1);
  assert(lj_arena_remote_active_acq(a) ==
	 (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING));
}

static void test_terminal_reconcile(PRNGState *rs)
{
  const size_t size = 64u;
  const uint64_t stale = LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_PENDING;
  TGAlloc lists, alloc;
  GCArena *arena[5];
  GCArena *a;
  void *p;
  uint32_t cell, i;

  /* Every allocator list is visited without detachment. A failure in an early
  ** entry does not prevent safe later entries from being reconciled, and each
  ** ambiguous word remains byte-for-byte unchanged for diagnosis/retry. */
  lj_arena_alloc_init(&lists);
  for (i = 0; i < 5u; i++) {
    arena[i] = lj_arena_map(rs, 0);
    assert(arena[i] != NULL);
    lj_arena_next_rel(arena[i], NULL);
    la_store64_rel(&arena[i]->hdr.remote_active, stale);
  }
  lists.owned[0] = arena[0];
  lj_arena_next_rel(arena[0], arena[1]);
  lists.needsweep[0] = arena[2];
  lists.quarantine[0] = arena[3];
  la_storeptr_rel((void **)&lists.reclaimed[0], arena[4]);

  la_store64_rel(&arena[0]->hdr.remote_active,
		 LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING);
  assert(!lj_arena_alloc_terminal_reconcile(&lists));
  assert(lists.owned[0] == arena[0] && lj_arena_next_acq(arena[0]) == arena[1]);
  assert(lists.needsweep[0] == arena[2] &&
	 lists.quarantine[0] == arena[3] &&
	 la_loadptr_acq((void *const *)&lists.reclaimed[0]) == arena[4]);
  assert(lj_arena_remote_active_acq(arena[0]) ==
	 (LJ_ARENA_REMOTE_SEALED|LJ_ARENA_REMOTE_PENDING));
  for (i = 1; i < 5u; i++)
    assert(lj_arena_remote_active_acq(arena[i]) == LJ_ARENA_REMOTE_CLOSED);

  la_store64_rel(&arena[0]->hdr.remote_active, stale);
  la_store64_rel(&arena[2]->hdr.remote_active, stale | 1u);
  la_store64_rel(&arena[4]->hdr.remote_active, stale);
  assert(!lj_arena_alloc_terminal_reconcile(&lists));
  assert(lj_arena_remote_active_acq(arena[2]) == (stale | 1u));
  assert(lj_arena_remote_active_acq(arena[4]) == LJ_ARENA_REMOTE_CLOSED);

  la_store64_rel(&arena[2]->hdr.remote_active, stale);
  la_store64_rel(&arena[3]->hdr.remote_active, stale);
  la_store32_rel(&arena[3]->hdr.terminal_closed, 1);
  la_store64_rel(&arena[4]->hdr.remote_active, stale);
  assert(!lj_arena_alloc_terminal_reconcile(&lists));
  assert(lj_arena_remote_active_acq(arena[3]) == stale);
  assert(lj_arena_remote_active_acq(arena[4]) == LJ_ARENA_REMOTE_CLOSED);
  la_store32_rel(&arena[3]->hdr.terminal_closed, 0);
  assert(lj_arena_alloc_terminal_reconcile(&lists));
  for (i = 0; i < 5u; i++)
    assert(lj_arena_remote_active_acq(arena[i]) == LJ_ARENA_REMOTE_CLOSED);

  /* Corrupt multi-node topology must fail closed rather than hanging the
  ** joined-world terminal pass. The read-only walker leaves the cycle intact
  ** for diagnosis, and the fixture repairs it before unmapping. */
  lj_arena_next_rel(arena[1], arena[0]);
  assert(!lj_arena_alloc_terminal_reconcile(&lists));
  assert(lists.owned[0] == arena[0] &&
	 lj_arena_next_acq(arena[0]) == arena[1] &&
	 lj_arena_next_acq(arena[1]) == arena[0]);
  lj_arena_next_rel(arena[1], NULL);

  lists.owned[0] = NULL;
  lists.needsweep[0] = NULL;
  lists.quarantine[0] = NULL;
  la_storeptr_rel((void **)&lists.reclaimed[0], NULL);
  for (i = 0; i < 5u; i++) {
    lj_arena_next_rel(arena[i], NULL);
    lj_arena_unmap(arena[i]);
  }
  lj_arena_alloc_fini(&lists);

  /* A stale gate intent alone used to make terminal managed destruction lose
  ** forever. Reconciliation preserves CLOSED and the lifetime descriptor, but
  ** removes that sole non-semantic veto. */
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  terminal_make_closed_pending(a);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_LOST);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_alloc_terminal_reconcile(&alloc));
  assert(lj_arena_remote_active_acq(a) == LJ_ARENA_REMOTE_CLOSED);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  /* An earlier terminal destructor may recreate the conservative gate intent
  ** after global PRE. The exact-arena check repairs it immediately before the
  ** later object's acquisition without changing its lifetime lane. */
  terminal_make_closed_pending(a);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_LOST);
  assert(lj_arena_terminal_reconcile(a));
  assert(lj_arena_remote_active_acq(a) == LJ_ARENA_REMOTE_CLOSED);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_ACQUIRED);
  lj_arena_free(&alloc, p, size);
  lj_arena_alloc_fini(&alloc);

  /* Exact late ownership survives gate repair and remains a destructor veto. */
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  assert(lj_arena_free_deferred(&alloc, p, size));
  assert(lj_arena_late_get(a, cell));
  terminal_make_closed_pending(a);
  assert(lj_arena_alloc_terminal_reconcile(&alloc));
  assert(lj_arena_late_get(a, cell) &&
	 lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_LOST);
  (void)la_and64_rlx(&a->late[cell >> 6],
		     ~((uint64_t)1 << (cell & 63)));
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_ACQUIRED);
  lj_arena_free(&alloc, p, size);
  lj_arena_alloc_fini(&alloc);

  /* Root membership is an independent exact owner and is neither inferred
  ** from nor erased with the stale gate intent. */
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, size,
	LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  assert(lj_arena_root_construct_commit(a, cell));
  terminal_make_closed_pending(a);
  assert(lj_arena_alloc_terminal_reconcile(&alloc));
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER &&
	 lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_LOST);
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_MEMBER,
				 LJ_ARENA_ROOT_UNLINKING));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_UNLINKING,
				 LJ_ARENA_ROOT_NONE));
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_ACQUIRED);
  lj_arena_free(&alloc, p, size);
  lj_arena_alloc_fini(&alloc);

  /* Recovery identity has the same independent veto and remains exact. */
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  assert(lj_arena_recovery_state_cas(a, cell, LJ_ARENA_RECOVERY_IDLE,
				     LJ_ARENA_RECOVERY_PENDING));
  lj_arena_rescue_leave(a);
  terminal_make_closed_pending(a);
  assert(lj_arena_alloc_terminal_reconcile(&alloc));
  assert(lj_arena_recovery_state_acq(a, cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_LOST);
  assert(lj_arena_recovery_state_cas(a, cell, LJ_ARENA_RECOVERY_PENDING,
				     LJ_ARENA_RECOVERY_IDLE));
  assert(lj_arena_destruct_acquire(p, size) == LJ_ARENA_DESTRUCT_ACQUIRED);
  lj_arena_free(&alloc, p, size);
  lj_arena_alloc_fini(&alloc);
}

static void test_huge_reader_destructor_retry(PRNGState *rs)
{
  const size_t size = LJ_HUGE_THRESHOLD + 2711u;
  HugeTab ht = { NULL };
  LJHugeReader reader = { NULL, NULL, 0 };
  LJHugeInfo hi;
  void *p;

  assert(lj_arena_hugetab_init(&ht, 2));
  p = lj_arena_huge_map(rs, size, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  assert(lj_arena_hugetab_insert(&ht, p, size,
	LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY|LJ_HUGEF_MARK));
  lj_arena_hugetab_prepare_sweep(&ht);
  assert(lj_arena_hugetab_reader_acquire(&ht, p, &reader, &hi) ==
	 LJ_ARENA_HUGE_READER_ACQUIRED);

  /* A semantic destructor has not run, so reader contention must remain a
  ** plain retry. Publishing DEFER_FREE here lets the last reader terminalize
  ** the body without its exittab/type/accounting teardown. */
  assert(lj_arena_hugetab_destruct_acquire(&ht, p, &hi) ==
	 LJ_ARENA_DESTRUCT_LOST);
  assert(hi.readers == 1u);
  assert((hi.flags & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_FREEING|
		      LJ_HUGEF_BUSY)) == 0);
  assert((hi.flags & LJ_HUGEF_MARK) != 0);
  assert(lj_arena_hugetab_reader_release(&reader, &hi) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  assert((hi.flags & (LJ_HUGEF_DEFER_FREE|LJ_HUGEF_FREEING)) == 0);

  assert(lj_arena_hugetab_destruct_acquire(&ht, p, &hi) ==
	 LJ_ARENA_DESTRUCT_ACQUIRED);
  assert((hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING|
		      LJ_HUGEF_BUSY)) ==
	 (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_FREEING|LJ_HUGEF_BUSY));
  assert((hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_DEFER_FREE)) == 0);
  assert(lj_arena_hugetab_destruct_acquire(&ht, p, &hi) ==
	 LJ_ARENA_DESTRUCT_OWNED);
  assert(lj_arena_hugetab_finish_external_free(&ht, p, &hi) ==
	 LJ_ARENA_HUGE_FINISH_DEFERRED);
  delete_unmap(&ht, p);
  lj_arena_hugetab_fini(&ht);
}

#if defined(LJ_ARENA_TEST_HELPERS)
static void test_gc2_sidecar_prealloc_and_fini_veto(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a, *plain;
  LJGC2TabStamp *stamp;
  LJGC2TableTokenTicket ticket;
  void *p;

  /* A traversable small arena is not publishable without its complete
  ** 64-KiB sidecar. Plain mappings do not depend on that allocation. */
  errno = EDOM;
  lj_arena_test_gc2_sidecar_fail_alloc(1);
  assert(lj_arena_map(rs, LJ_AF_TRAVERSABLE) == NULL);
  assert(errno == EDOM);
  plain = lj_arena_map(rs, 0);
  assert(plain != NULL);
  assert(errno == EDOM);
  assert(lj_arena_gc2_tabstamp_acq(plain) == NULL);
  lj_arena_unmap(plain);
  assert(errno == EDOM);
  lj_arena_test_gc2_sidecar_fail_alloc(0);
  a = lj_arena_map(rs, LJ_AF_TRAVERSABLE);
  assert(a != NULL && lj_arena_gc2_tabstamp_acq(a) != NULL);
  lj_arena_unmap(a);

  /* Allocator-list teardown has the same exact-token veto as direct unmap.
  ** Completing to NONE preserves the advanced generation and lets a retry
  ** release both the arena and its sidecar. */
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, 64u, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  stamp = lj_arena_gc2_stamp_acq(p);
  assert(stamp != NULL);
  assert(lj_gc2_table_token_refresh(&stamp->token, &ticket) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  lj_arena_alloc_fini(&alloc);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == a);
  assert(lj_arena_remote_active_acq(a) == 0);
  assert(!lj_arena_gc2_tokens_empty_acq(a));
  assert(lj_gc2_table_token_complete(&stamp->token, &ticket) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_generation(
	   la_load64_acq(&stamp->token.control)) == 2u);
  assert(lj_arena_gc2_tokens_empty_acq(a));
  lj_arena_alloc_fini(&alloc);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
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
  test_huge_reader_lifetime(&rs);
  test_huge_reader_root_recovery_orders(&rs);
  test_huge_reader_shapes_and_realloc(&rs);
  test_huge_reader_overflow_and_size(&rs);
  test_huge_reader_destructor_retry(&rs);
  test_alloc_committed_prefix_tail_veto(&rs);
  test_managed_shrink_suffix_veto(&rs);
  test_plain_reader_mutation_gate(&rs);
  test_terminal_reconcile(&rs);
  test_registry_rescue_unmap_handoff(&rs);
  test_recovery_state(&rs);
  test_root_state(&rs);
  test_root_free_race(&rs);
#if defined(LJ_ARENA_TEST_HELPERS)
  test_gc2_sidecar_prealloc_and_fini_veto(&rs);
  test_huge_realloc_busy_preemption(&rs);
  test_small_lifetime_descriptor(&rs);
  test_retire_busy_mark_intent(&rs);
  test_retire_busy_exact_mark(&rs);
  test_recovery_deferred_busy_requeue(&rs);
  test_retire_busy_deferred_terminal(&rs);
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
