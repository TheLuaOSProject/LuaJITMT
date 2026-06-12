/*
** Focused test for huge arena mappings.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

static const size_t sizes[] = {
  LJ_HUGE_THRESHOLD + 1u,
  LJ_ARENA_SIZE + 333u,
  (size_t)LJ_ARENA_SIZE * 3u + 17u
};

int main(void)
{
  PRNGState rs;
  uint32_t i;

  lj_prng_seed_fixed(&rs);
  assert(lj_arena_huge_map(&rs, LJ_HUGE_THRESHOLD, 0) == NULL);
  assert(lj_arena_huge_mapsize(LJ_HUGE_THRESHOLD) == 0);

  for (i = 0; i < (uint32_t)(sizeof(sizes)/sizeof(sizes[0])); i++) {
    size_t size = sizes[i];
    size_t mapsize = lj_arena_huge_mapsize(size);
    uint32_t flags = (i & 1u) ? LJ_AF_TRAVERSABLE : 0;
    void *p = lj_arena_huge_map(&rs, size, flags);
    GCArena *a = lj_arena_of(p);
    assert(p != NULL);
    assert(mapsize >= size + sizeof(GCAhdr));
    assert((mapsize & LJ_ARENA_MASK) == 0);
    assert((uintptr_t)a % LJ_ARENA_SIZE == 0);
    assert((char *)p == (char *)a + sizeof(GCAhdr));
    assert(checkptrGC(p));
    assert(checkptrGC((char *)a + mapsize - 1u));
    assert(lj_arena_ishuge(a));
    assert((a->hdr.flags & flags) == flags);
    assert(a->hdr.live_cells == (uint32_t)(mapsize >> LJ_CELL_SHIFT));
    memset(p, 0x31 + (int)i, size);
    assert(((uint8_t *)p)[0] == (uint8_t)(0x31 + i));
    assert(((uint8_t *)p)[size - 1u] == (uint8_t)(0x31 + i));
    lj_arena_huge_unmap(p, size);
  }

  lj_arena_huge_unmap(NULL, sizes[0]);
  printf("t-arena-huge OK: %u huge mappings verified\n",
	 (uint32_t)(sizeof(sizes)/sizeof(sizes[0])));
  return 0;
}
