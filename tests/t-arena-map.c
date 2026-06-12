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
    assert(a->hdr.next == NULL);
    assert(a->hdr.live_cells == 0);
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

  printf("t-arena-map OK: %u arenas mapped below 47 bits\n", (uint32_t)NARENA);
  return 0;
}
