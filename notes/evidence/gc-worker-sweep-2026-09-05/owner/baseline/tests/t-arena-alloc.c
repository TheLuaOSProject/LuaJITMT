/*
** Focused test for the owner-local arena bump allocator scaffold.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

#define NALLOC 2400

static uint32_t ncells_for(size_t size)
{
  return (uint32_t)((size + LJ_CELL_SIZE-1u) >> LJ_CELL_SHIFT);
}

static uint32_t count_owned(TGAlloc *alloc, uint32_t kind)
{
  GCArena *a = alloc->owned[kind];
  uint32_t n = 0;
  while (a) {
    assert(((uintptr_t)a & LJ_ARENA_MASK) == 0);
    assert(checkptrGC(a));
    n++;
    a = lj_arena_next_acq(a);
  }
  return n;
}

static void test_table_token_generation_survives_reuse(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  LJGC2TabStamp *stamp, *bump_stamp;
  LJGC2TableTokenTicket ticket;
  uint64_t completed, bump_control;
  void *p, *reuse, *bump;

  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, rs, LJ_CELL_SIZE, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  assert(lj_arena_gc2_tabstamp_acq(a) != NULL);
  stamp = lj_arena_gc2_stamp_acq(p);
  assert(stamp != NULL);
  assert(lj_gc2_table_token_refresh(&stamp->token, &ticket) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_complete(&stamp->token, &ticket) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  completed = la_load64_acq(&stamp->token.control);
  assert(lj_gc2_table_token_state(completed) == LJ_GC2_TABLE_TOKEN_NONE);
  assert(lj_gc2_table_token_generation(completed) == 2u);
  la_store64_rel(&stamp->state, (UINT64_C(0x12345678) << 32) | 91u);

  /* The owner-private bump path has the same reincarnation contract as bin
  ** reuse: clear the old scan proof, never the persistent token generation. */
  bump = lj_arena_cellptr(a, alloc.bump[LJ_ARENAK_TRAVERSABLE].cell);
  bump_stamp = lj_arena_gc2_stamp_acq(bump);
  assert(bump_stamp != NULL);
  assert(lj_gc2_table_token_refresh(&bump_stamp->token, &ticket) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  assert(lj_gc2_table_token_complete(&bump_stamp->token, &ticket) ==
	 LJ_GC2_TABLE_TOKEN_RESULT_OK);
  bump_control = la_load64_acq(&bump_stamp->token.control);
  la_store64_rel(&bump_stamp->state, (UINT64_C(0x87654321) << 32) | 37u);
  assert(lj_arena_alloc(&alloc, rs, LJ_CELL_SIZE, LJ_AF_TRAVERSABLE) == bump);
  assert(la_load64_acq(&bump_stamp->state) == 0);
  assert(la_load64_acq(&bump_stamp->token.control) == bump_control);
  lj_arena_free(&alloc, bump, LJ_CELL_SIZE);

  /* Make the exact freed run the next allocation candidate. No allocator
  ** path may zero the persistent side entry when this cell is reused. */
  alloc.bump[LJ_ARENAK_TRAVERSABLE].cell =
    alloc.bump[LJ_ARENAK_TRAVERSABLE].end;
  lj_arena_free(&alloc, p, LJ_CELL_SIZE);
  reuse = lj_arena_alloc(&alloc, rs, LJ_CELL_SIZE, LJ_AF_TRAVERSABLE);
  assert(reuse == p);
  assert(lj_arena_gc2_stamp_acq(reuse) == stamp);
  assert(la_load64_acq(&stamp->state) == 0);
  assert(la_load64_acq(&stamp->token.control) == completed);

  lj_arena_free(&alloc, reuse, LJ_CELL_SIZE);
  lj_arena_alloc_fini(&alloc);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
}

int main(void)
{
  PRNGState rs;
  TGAlloc alloc;
  GCArena *seen[NALLOC];
  uint32_t expect[NALLOC];
  uint32_t i, nseen = 0;

  lj_prng_seed_fixed(&rs);
  test_table_token_generation_survives_reuse(&rs);
  lj_arena_alloc_init(&alloc);

  for (i = 0; i < NALLOC; i++) {
    uint32_t flags = (i & 1u) ? LJ_AF_TRAVERSABLE : 0;
    size_t size = (size_t)(1u + ((i * 37u) % 2048u));
    uint32_t ncells = ncells_for(size);
    void *p;
    GCArena *a;
    uint32_t cell, j, slot;
    alloc.alloc_black = (uint8_t)(i >= (NALLOC / 2));
    p = lj_arena_alloc(&alloc, &rs, size, flags);
    assert(p != NULL);
    a = lj_arena_of(p);
    cell = lj_arena_cellof(p);
    for (slot = 0; slot < nseen && seen[slot] != a; slot++)
      ;
    if (slot == nseen) {
      assert(nseen < NALLOC);
      seen[nseen] = a;
      expect[nseen] = LJ_AFIRST_CELL;
      nseen++;
    }
    assert(a->hdr.flags == flags);
    assert(cell == expect[slot]);
    assert(lj_arena_state(a, cell) == (alloc.alloc_black ? 3u : 2u));
    for (j = 1; j < ncells; j++)
      assert(lj_arena_state(a, cell + j) == 0);
    memset(p, (int)(0x40u + (i & 63u)), (size_t)ncells << LJ_CELL_SHIFT);
    expect[slot] += ncells;
  }

  assert(count_owned(&alloc, LJ_ARENAK_TRAVERSABLE) > 1);
  assert(count_owned(&alloc, LJ_ARENAK_PLAIN) > 1);
  assert(lj_arena_alloc(&alloc, &rs, 0, 0) == NULL);
  {
    void *huge = lj_arena_alloc(&alloc, &rs, LJ_HUGE_THRESHOLD + 1u, 0);
    assert(huge != NULL);
    assert(lj_arena_ishuge(lj_arena_of(huge)));
    lj_arena_free(&alloc, huge, LJ_HUGE_THRESHOLD + 1u);
  }

  lj_arena_alloc_fini(&alloc);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(alloc.owned[LJ_ARENAK_PLAIN] == NULL);
  assert(alloc.bump[LJ_ARENAK_TRAVERSABLE].a == NULL);
  assert(alloc.bump[LJ_ARENAK_PLAIN].a == NULL);

  printf("t-arena-alloc OK: %u allocations across owned arenas\n",
	 (uint32_t)NALLOC);
  return 0;
}
