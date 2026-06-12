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
    a = a->hdr.next;
  }
  return n;
}

int main(void)
{
  PRNGState rs;
  TGAlloc alloc;
  GCArena *current[LJ_ARENA_NKINDS] = { NULL, NULL };
  uint32_t expect[LJ_ARENA_NKINDS] = { LJ_AFIRST_CELL, LJ_AFIRST_CELL };
  uint32_t i;

  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);

  for (i = 0; i < NALLOC; i++) {
    uint32_t flags = (i & 1u) ? LJ_AF_TRAVERSABLE : 0;
    uint32_t kind = (flags & LJ_AF_TRAVERSABLE) ? LJ_ARENAK_TRAVERSABLE :
						  LJ_ARENAK_PLAIN;
    size_t size = (size_t)(1u + ((i * 37u) % 2048u));
    uint32_t ncells = ncells_for(size);
    void *p;
    GCArena *a;
    uint32_t cell, j;
    alloc.alloc_black = (uint8_t)(i >= (NALLOC / 2));
    p = lj_arena_alloc(&alloc, &rs, size, flags);
    assert(p != NULL);
    a = lj_arena_of(p);
    cell = lj_arena_cellof(p);
    if (a != current[kind]) {
      current[kind] = a;
      expect[kind] = LJ_AFIRST_CELL;
    }
    assert(a->hdr.flags == flags);
    assert(cell == expect[kind]);
    assert(lj_arena_state(a, cell) == (alloc.alloc_black ? 3u : 2u));
    for (j = 1; j < ncells; j++)
      assert(lj_arena_state(a, cell + j) == 0);
    memset(p, (int)(0x40u + (i & 63u)), (size_t)ncells << LJ_CELL_SHIFT);
    expect[kind] += ncells;
  }

  assert(count_owned(&alloc, LJ_ARENAK_TRAVERSABLE) > 1);
  assert(count_owned(&alloc, LJ_ARENAK_PLAIN) > 1);
  assert(lj_arena_alloc(&alloc, &rs, 0, 0) == NULL);
  assert(lj_arena_alloc(&alloc, &rs, LJ_HUGE_THRESHOLD + 1u, 0) == NULL);

  lj_arena_alloc_fini(&alloc);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(alloc.owned[LJ_ARENAK_PLAIN] == NULL);
  assert(alloc.bump[LJ_ARENAK_TRAVERSABLE].a == NULL);
  assert(alloc.bump[LJ_ARENAK_PLAIN].a == NULL);

  printf("t-arena-alloc OK: %u allocations across owned arenas\n",
	 (uint32_t)NALLOC);
  return 0;
}
