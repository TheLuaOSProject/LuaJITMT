/*
** Focused test for the arena lua_Alloc shim.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

static void fill_seq(uint8_t *p, size_t n, uint8_t base)
{
  size_t i;
  for (i = 0; i < n; i++)
    p[i] = (uint8_t)(base + i);
}

static void check_seq(const uint8_t *p, size_t n, uint8_t base)
{
  size_t i;
  for (i = 0; i < n; i++)
    assert(p[i] == (uint8_t)(base + i));
}

int main(void)
{
  PRNGState rs;
  uint32_t round;

  lj_prng_seed_fixed(&rs);

  for (round = 0; round < 8; round++) {
    TGAlloc alloc;
    LJArenaAllocD ad;
    HugeTab ht = { NULL };
    LJHugeInfo hi;
    void *p, *q, *typed, *huge, *huge_same, *huge2, *small;
    size_t hsize = LJ_HUGE_THRESHOLD + 100u + round;
    size_t hsize_same = hsize + 512u;
    size_t hsize2 = LJ_ARENA_SIZE + 700u + round;

    lj_arena_alloc_init(&alloc);
    alloc.owner_tid = 0xabc00000u + round;
    lj_arena_allocd_init(&ad, &alloc, &rs, 0);
    assert(lj_arena_hugetab_init(&ht, 4) == 1);
    lj_arena_allocd_sethugetab(&ad, &ht);

    assert(lj_arena_allocf(&ad, NULL, 0, 0) == NULL);
    typed = lj_arena_allocd_alloc(&ad, 48, LJ_AF_TRAVERSABLE);
    assert(typed != NULL);
    assert(lj_arena_of(typed)->hdr.owner_tid == alloc.owner_tid);
    assert((lj_arena_of(typed)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
    assert(lj_arena_cdata_get(lj_arena_of(typed),
			      lj_arena_cellof(typed)) == 0);
    assert(lj_arena_ready_get(lj_arena_of(typed),
			      lj_arena_cellof(typed)) == 0);
    assert(lj_arena_allocd_publish_interior_cdata(&ad, typed, 48) == 1);
    assert(lj_arena_cdata_get(lj_arena_of(typed),
			      lj_arena_cellof(typed)) == 1);
    assert(lj_arena_ready_get(lj_arena_of(typed),
			      lj_arena_cellof(typed)) == 1);
    lj_arena_free(&alloc, typed, 48);
    assert(lj_arena_cdata_get(lj_arena_of(typed),
			      lj_arena_cellof(typed)) == 0);
    assert(lj_arena_ready_get(lj_arena_of(typed),
			      lj_arena_cellof(typed)) == 0);

    p = lj_arena_allocf(&ad, NULL, 0, 64);
    assert(p != NULL);
    assert(!lj_arena_ishuge(lj_arena_of(p)));
    assert(lj_arena_of(p)->hdr.owner_tid == alloc.owner_tid);
    fill_seq((uint8_t *)p, 64, (uint8_t)(0x20u + round));

    q = lj_arena_allocf(&ad, p, 64, 256);
    assert(q != NULL);
    assert(lj_arena_of(q)->hdr.owner_tid == alloc.owner_tid);
    check_seq((uint8_t *)q, 64, (uint8_t)(0x20u + round));
    p = q;

    assert(lj_arena_allocf(&ad, p, 256, ~(size_t)0) == NULL);
    check_seq((uint8_t *)p, 64, (uint8_t)(0x20u + round));

    huge = lj_arena_allocf(&ad, p, 256, hsize);
    assert(huge != NULL);
    assert(lj_arena_ishuge(lj_arena_of(huge)));
    assert(lj_arena_of(huge)->hdr.owner_tid == alloc.owner_tid);
    assert(lj_arena_hugetab_lookup(&ht, huge, &hi) == 1);
    assert(hi.size == hsize);
    assert(hi.flags == 0);
    check_seq((uint8_t *)huge, 64, (uint8_t)(0x20u + round));
    fill_seq((uint8_t *)huge, 128, (uint8_t)(0x50u + round));

    huge_same = lj_arena_allocf(&ad, huge, hsize, hsize_same);
    assert(huge_same == huge);  /* Equal mapping extent stays O(1), but pinned. */
    assert(lj_arena_hugetab_lookup(&ht, huge_same, &hi) == 1);
    assert(hi.size == hsize_same);
    check_seq((uint8_t *)huge_same, 128, (uint8_t)(0x50u + round));
    huge = huge_same;

    alloc.alloc_black = 1;
    huge2 = lj_arena_allocf(&ad, huge, hsize_same, hsize2);
    assert(huge2 != NULL);
    assert(lj_arena_ishuge(lj_arena_of(huge2)));
    assert(lj_arena_of(huge2)->hdr.owner_tid == alloc.owner_tid);
    assert(lj_arena_hugetab_lookup(&ht, huge, NULL) == 0);
    assert(lj_arena_hugetab_lookup(&ht, huge2, &hi) == 1);
    assert(hi.size == hsize2);
    assert((hi.flags & LJ_HUGEF_MARK) != 0);
    check_seq((uint8_t *)huge2, 128, (uint8_t)(0x50u + round));

    small = lj_arena_allocf(&ad, huge2, hsize2, 128);
    assert(small != NULL);
    assert(!lj_arena_ishuge(lj_arena_of(small)));
    assert(lj_arena_of(small)->hdr.owner_tid == alloc.owner_tid);
    assert(lj_arena_hugetab_lookup(&ht, huge2, NULL) == 0);
    check_seq((uint8_t *)small, 128, (uint8_t)(0x50u + round));
    /* The original size class makes all of these safe after huge2 is
    ** unmapped. A duplicate/stale operation observes only the terminal table
    ** state and never probes its former mapping header or payload. */
    assert(lj_arena_allocf(&ad, huge2, hsize2, 0) == NULL);
    assert(lj_arena_free_deferred(&alloc, huge2, hsize2) == 0);
    assert(lj_arena_allocf(&ad, huge2, hsize2, hsize) == NULL);
    assert(lj_arena_allocf(&ad, small, 128, 0) == NULL);

    lj_arena_hugetab_fini(&ht);
    lj_arena_alloc_fini(&alloc);
    assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert(alloc.owned[LJ_ARENAK_PLAIN] == NULL);
  }

  printf("t-arena-allocf OK: lua_Alloc shim transitions verified\n");
  return 0;
}
