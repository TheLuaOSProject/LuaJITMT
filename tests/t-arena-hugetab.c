/*
** Focused test for the huge-object side table scaffold.
*/

#include <assert.h>
#include <errno.h>
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
  LJHugeInfo hi;
  uint32_t i;

  lj_prng_seed_fixed(&rs);
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
  ** retire publishes its metadata ticket. The ticket is live-only and cannot
  ** be mistaken for stale destructor payload. */
  lj_arena_hugetab_prepare_sweep(&ht);
  assert(lj_arena_hugetab_mark(&ht, ptrs[3], &hi) == 1);
  assert(lj_arena_hugetab_retire(&ht, ptrs[3], ptrs[3], 19u, &hi) == 1);
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
  assert(lj_arena_hugetab_transfer(&dst, &src, 0x5678u) == 1);
  assert(lj_arena_of(ptrs[0])->hdr.owner_tid == 0x5678u);
  assert(lj_arena_hugetab_lookup(&src, ptrs[0], NULL) == 0);
  assert(lj_arena_hugetab_lookup(&dst, ptrs[0], &hi) == 1);
  check_info(&hi, LJ_HUGE_THRESHOLD + 4096u, LJ_HUGEF_TRAVERSABLE);
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
