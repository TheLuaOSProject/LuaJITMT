/*
** Focused test for the huge-object side table scaffold.
*/

#include <assert.h>
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

  printf("t-arena-hugetab OK: insert lookup mark live delete tombstone full\n");
  return 0;
}
