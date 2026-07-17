/*
** Focused test for arena OS mapping.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

#define NARENA 128

int main(void)
{
  PRNGState rs;
  GCArena *arenas[NARENA];
  uint32_t i, j;

  lj_prng_seed_fixed(&rs);
  lj_arena_unmap(NULL);

  for (i = 0; i < NARENA; i++) {
    uint32_t flags = (i & 1u) ? LJ_AF_TRAVERSABLE : 0;
    GCArena *a = lj_arena_map(&rs, flags);
    uintptr_t addr = (uintptr_t)a;
    assert(a != NULL);
    assert((addr & LJ_ARENA_MASK) == 0);
    assert(checkptrGC(a));
    assert(checkptrGC((char *)a + LJ_ARENA_SIZE - 1u));
    assert(lj_arena_of(lj_arena_cellptr(a, LJ_AFIRST_CELL)) == a);
    assert(lj_arena_cellof(lj_arena_cellptr(a, LJ_AFIRST_CELL)) ==
	   LJ_AFIRST_CELL);
    assert(a->hdr.flags == flags);
    assert(a->hdr.owner_tid == 0);
    assert(lj_arena_next_acq(a) == NULL);
    assert(a->hdr.live_cells == 0);
    if (flags & LJ_AF_TRAVERSABLE) {
      LJGC2TabStampArena *side = lj_arena_gc2_tabstamp_acq(a);
      assert(side != NULL);
      assert(sizeof(*side) == LJ_ARENA_SIZE);
      for (j = 0; j < LJ_ARENA_CELLS; j++) {
	uint64_t control = la_load64_acq(&side->cell[j].token.control);
	assert(la_load64_acq(&side->cell[j].state) == 0);
	assert(lj_gc2_table_token_state(control) == LJ_GC2_TABLE_TOKEN_NONE);
	assert(lj_gc2_table_token_generation(control) == 0);
      }
      assert(lj_arena_gc2_stamp_acq(
	lj_arena_cellptr(a, LJ_AFIRST_CELL)) ==
	&side->cell[LJ_AFIRST_CELL]);
    } else {
      assert(lj_arena_gc2_tabstamp_acq(a) == NULL);
      assert(lj_arena_gc2_stamp_acq(
	lj_arena_cellptr(a, LJ_AFIRST_CELL)) == NULL);
    }
    assert(lj_arena_gc2_tokens_empty_acq(a));
    for (j = 0; j < LJ_ARENA_WORDS; j++) {
      assert(a->block[j] == 0);
      assert(a->mark[j] == 0);
    }
    memset(lj_arena_cellptr(a, LJ_AFIRST_CELL), 0xa5, LJ_CELL_SIZE);
    memset(lj_arena_cellptr(a, LJ_ARENA_CELLS - 1u), 0x5a, LJ_CELL_SIZE);
    arenas[i] = a;
  }

  for (i = 0; i < NARENA; i++)
    lj_arena_unmap(arenas[i]);

  /* Direct terminal unmap must retain a mapping while any exact token is
  ** pending, then accept the same NONE token with its advanced generation. */
  {
    GCArena *a = lj_arena_map(&rs, LJ_AF_TRAVERSABLE);
    LJGC2TabStamp *stamp;
    LJGC2TableTokenTicket ticket;
    assert(a != NULL);
    stamp = lj_arena_gc2_stamp_acq(
      lj_arena_cellptr(a, LJ_AFIRST_CELL));
    assert(stamp != NULL);
    assert(lj_gc2_table_token_refresh(&stamp->token, &ticket) ==
	   LJ_GC2_TABLE_TOKEN_RESULT_OK);
    assert(!lj_arena_gc2_tokens_empty_acq(a));
    lj_arena_unmap(a);
    assert(lj_arena_gc2_stamp_acq(
	     lj_arena_cellptr(a, LJ_AFIRST_CELL)) == stamp);
    assert(lj_gc2_table_token_complete(&stamp->token, &ticket) ==
	   LJ_GC2_TABLE_TOKEN_RESULT_OK);
    assert(lj_arena_gc2_tokens_empty_acq(a));
    assert(lj_gc2_table_token_generation(
	     la_load64_acq(&stamp->token.control)) == 2u);
    lj_arena_unmap(a);
  }

  printf("t-arena-map OK: %u arenas mapped below 47 bits\n", (uint32_t)NARENA);
  return 0;
}
